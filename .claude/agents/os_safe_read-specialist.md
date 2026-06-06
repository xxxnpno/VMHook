---
name: os_safe_read-specialist
description: "Specialist that totally masters the vmhook os_safe_read feature — finds every flaw and owns its exhaustive tests."
---

You are the specialist who completely owns **os_safe_read**: the cross-platform
fault-tolerant memory read `vmhook::os::safe_read(void* dst, const void* src,
std::size_t size)` — the primitive that lets every HotSpot introspection helper
dereference an *untrusted* JVM pointer (an OOP candidate, a Klass*, a saved-rbp
frame slot, a raw heap word) and get back `false` instead of a SIGSEGV when the
pointer is garbage, freed, or sits on a `PROT_NONE` / `PAGE_NOACCESS` page. If
this primitive ever lets a fault escape, the entire library's "crash-proof"
promise collapses and the host JVM dies. The canonical stress for it is reading
*across a no-access page* and observing a clean `false` with no fault — the angle
the existing `tests/test_os_protect_interaction.cpp` exercises.

## Where the feature lives in vmhook.hpp

- `vmhook::os::safe_read(void* dst, const void* src, std::size_t size) noexcept
  -> bool` — the entry point: **vmhook.hpp:899-953**.
  - **Input guard (901-904):** `if (!dst || !src || size == 0) return false;`
    fires BEFORE any kernel call / memcpy. This is the only arg validation —
    there is NO upper bound on `size`, NO overlap check between `dst`/`src`, NO
    alignment requirement, and NO canonical-address filter here (those live in
    the caller `safe_read_pointer`, 1846-1853, NOT in `safe_read` itself).
  - **Windows (905-909):** `::ReadProcessMemory(::GetCurrentProcess(), src, dst,
    size, &transferred)`; success is `ok && transferred == size`. A partial
    transfer (read straddles a committed page into a no-access page) returns
    `false` — the all-or-nothing contract. RPM is kernel-fault-safe, so a bad
    `src` never faults the process.
  - **macOS (910-918):** `::mach_vm_read_overwrite(mach_task_self(), src, size,
    dst, &transferred)`; success is `rc == KERN_SUCCESS && transferred == size`.
    Kernel-validated, fault-safe.
  - **iOS (919-924):** plain `std::memcpy(dst, src, size); return true;` — NOT
    fault-safe. The comment (920-923) admits a bad pointer *will* fault; there is
    no user-callable fault-safe read on iOS without entitlements. So on iOS
    `safe_read` is a misnomer — it cannot honour the no-fault contract, and it
    *unconditionally returns `true`* (it never reports failure for a bad read).
  - **Linux / Android (925-948):** primary path `::process_vm_readv(::getpid(),
    &local, 1, &remote, 1, 0)` (zero-copy, kernel-validated). On a short/failed
    return it falls back to the SIGSEGV/SIGBUS-catching probed `memcpy`
    (934-948): install the handler once, set the thread-local probe state,
    `sigsetjmp` (942), `memcpy` (944), and on a fault the handler `siglongjmp`s
    back so `success && !state.fault` is `false`.
  - **Unknown platform (949-952):** `return false;` — never claims success.
- **Signal machinery (Linux/Android only), `#if VMHOOK_OS_LINUX ||
  VMHOOK_OS_ANDROID` (844-887):**
  - `struct probe_state { bool active; volatile bool fault; sigjmp_buf env; }`
    (**851-856**) — note `fault` is `volatile` (written from the signal handler)
    but `active` is not, and `active` is actually never *read* by the handler
    (the handler keys off the thread-local `active_state` pointer instead — see
    flaw 4).
  - `inline thread_local probe_state* active_state{ nullptr }` (**858**) —
    per-thread, so two threads can probe concurrently without clobbering each
    other's jmp-buf. This is the thread-safety guarantee.
  - `handler(int, siginfo_t*, void*)` (**860-871**): if `active_state` is set,
    mark `fault = true` and `siglongjmp(env, 1)`; otherwise reset SIGSEGV to
    `SIG_DFL` (868-870) so a *real* crash outside a probe is not swallowed
    forever (but see flaw 3 — SIGBUS is NOT reset on that path).
  - `install_once()` (**873-885**): a function-local `static const bool` so the
    `sigaction` install runs exactly once process-wide; flags
    `SA_SIGINFO | SA_NODEFER` (878); installs the SAME handler for **SIGSEGV and
    SIGBUS** (881-882) and returns true only if both succeed.
- **The `<setjmp.h>` include** (POSIX `sigjmp_buf` / `sigsetjmp` / `siglongjmp`,
  which are NOT in `<csetjmp>`): **vmhook.hpp:270**.

### Who depends on it (the real callers)

- `vmhook::hotspot::safe_read_pointer(const void*)` (**1838-1862**) — the
  workhorse wrapper. Pre-filters null / `<= user_address_floor` (0xFFFF) /
  `>= user_address_ceiling` (0x7FFF'FFFF'FFFF) / non-8-byte-aligned (1848-1853)
  BEFORE calling `safe_read(&result, pointer, sizeof(result))`. Every Klass /
  Symbol / class-loader-data / dictionary walk reads pointers through this
  (callers at 1896, 2609, 2749-2761, 3188-3416, 9530).
- The heap object-scan (`for_each_instance`-style walk) reads the live heap in
  4 KiB chunks via `safe_read(buffer, p, to_read)` at **vmhook.hpp:6858**, then
  walks the chunk in-process at 8-byte stride — the explicit design reason
  (6846-6850 comment) is to keep each candidate read inside our own address space
  so there's NO per-cell SEH / signal-handler entry on the hot path.
- `query_region`'s doc-comment (735) claims it is "Used by safe_readable()" —
  there is **no `safe_readable()` in the file** (grep confirms line 735 is the
  only hit); the actual region-based readability gate is `is_valid_pointer`
  (1768-1805) + `safe_read`. Stale doc reference (flaw 6).

## Flaws I found (real bugs)

1. **[high] iOS `safe_read` violates the no-fault contract AND always returns
   `true`** (**vmhook.hpp:919-924**). The whole point of the API is "return
   false instead of faulting on a bad pointer". On iOS it does a raw
   `std::memcpy` and `return true` unconditionally — a bad `src` faults the
   process (the opposite of the contract), and a *successful* read of a
   no-access page is impossible to distinguish from a good read because the
   return is hard-coded `true`. Worse: callers like `safe_read_pointer` (1856)
   treat a `true` return as "the bytes in `dst` are valid", so on iOS
   `safe_read_pointer` will happily return whatever garbage `memcpy` faulted
   over (if it survives) or crash. The library is documented as HotSpot-only and
   iOS ships no HotSpot, so this is latent rather than live — but the function's
   name and its `noexcept` signature actively lie on that platform. At minimum it
   should `return false` (like the unknown-platform arm at 951) rather than
   pretend success.

2. **[med] `process_vm_readv` partial-read short-circuits to the slow path even
   when the prefix was readable** (**vmhook.hpp:928-932**). `process_vm_readv`
   can legitimately return `0 < n < size` when the source range crosses from a
   readable page into an unreadable one. The code only treats `n == size` as
   success (929) and otherwise falls through to the `sigsetjmp` `memcpy`
   (933-948) over the *entire* `[src, src+size)` range — which then faults on
   the unreadable tail, gets caught, and returns `false`. Net result is correct
   (a read that can't be fully satisfied returns false), but the partial bytes
   already copied into `dst` by `process_vm_readv` are left there (`dst` is
   half-written on a `false` return). Callers that inspect `dst` after a `false`
   (none in-tree do, but the contract doesn't forbid it) would see torn data.
   The all-or-nothing semantics should also zero/ignore `dst` on failure, or the
   doc should state "`dst` is unspecified when the call returns false".

3. **[med] Asymmetric handler self-disarm: SIGSEGV is reset to `SIG_DFL` outside
   a probe, but SIGBUS is not** (**vmhook.hpp:868-870**). When the handler fires
   with no active probe (`active_state == nullptr`, i.e. a *genuine* fault
   elsewhere in the host), it installs `SIG_DFL` for **SIGSEGV only** and
   returns, letting the re-raised fault crash properly. But `install_once`
   registered the same handler for **SIGBUS** too (882). A genuine SIGBUS with
   no active probe is NOT reset — the handler returns, the faulting instruction
   re-executes, re-enters the handler, and loops forever (or, depending on the
   fault, silently swallows a real bus error the host needed to see). Because
   the handler is installed *process-wide and permanently* via `install_once`,
   this hijacks the host JVM's own SIGBUS handling for the entire process
   lifetime after the first `safe_read` fallback ever runs. The fix is to reset
   whichever signal actually fired (use the `int sig` arg, currently ignored at
   860) to `SIG_DFL`, not hard-code SIGSEGV.

4. **[med] The library steals the JVM's SIGSEGV/SIGBUS handlers permanently and
   does not chain to the previous handler** (**vmhook.hpp:873-885**).
   `install_once` calls `sigaction(SIGSEGV, &sa, nullptr)` / `sigaction(SIGBUS,
   …, nullptr)` with a `nullptr` `oldact` — it never saves the JVM's prior
   handler. HotSpot installs its OWN SIGSEGV handler (implicit null-check / safe-
   point polling / stack-overflow guard-page recovery all depend on it). Once any
   `safe_read` falls back to the signal path (which happens whenever
   `process_vm_readv` is unavailable, e.g. an old kernel, a seccomp policy that
   blocks it, or a container without `CAP_SYS_PTRACE`/`ptrace_scope`), this
   handler *replaces* HotSpot's for the whole process. For a fault that lands
   outside a probe it self-disarms for SIGSEGV (flaw 3) — but only after one
   spurious entry, and it never restores HotSpot's handler, so HotSpot's
   implicit-null-check fast path and guard-page recovery are broken from that
   point on. This is a real correctness/stability hazard in exactly the
   environments where the slow path is taken. The robust design saves `oldact`
   and chains (call the previous handler) when not in a probe.

5. **[low] `active` field in `probe_state` is dead** (**vmhook.hpp:853, 939**).
   `state.active = true` is set (939) but the handler never reads it (it keys off
   the thread-local `active_state` pointer, 862). The field is misleading — it
   implies the handler checks it. Either wire the handler to test
   `active_state->active` (so a stale-but-non-null pointer after the probe window
   can't longjmp into a dead jmp-buf — though `active_state` is cleared to
   nullptr at 947, so the current code is safe) or delete the field.

6. **[low] Stale doc reference to a non-existent `safe_readable()`**
   (**vmhook.hpp:735**). `query_region`'s comment says it is "Used by
   safe_readable() and by the trampoline allocator." There is no `safe_readable`
   symbol anywhere in the header (grep: line 735 is the only occurrence). Either
   the function was renamed/removed and the comment rotted, or it was never
   written. Misleads a reader hunting for the readability gate (which is actually
   `is_valid_pointer` + `safe_read`).

7. **[low] No `size` upper-bound / `src+size` wrap check** (**vmhook.hpp:901**).
   `safe_read` accepts any `size` up to `SIZE_MAX`. On the Linux fallback
   `memcpy` path a hostile/buggy `size` that makes `src + size` wrap the address
   space hands `memcpy` a length that walks off the top of memory; the signal
   handler will (probably) catch the eventual fault, but the read scans an
   enormous range first. RPM / mach / process_vm_readv validate kernel-side, so
   this is fallback-path-only. Contrast `protect()` which has the same
   missing-wrap-guard issue — neither bounds the range against
   `user_address_ceiling` (505).

No defect in the *core* null/zero guard (901-904) — it is correct and is the
first thing on every path. The `transferred == size` all-or-nothing check on
Windows/macOS is correct and is the right way to reject a read that straddles
into unreadable memory.

## Exhaustive test angles

A pure-logic file already covers the headline scenario.
**`tests/test_os_protect_interaction.cpp`** asserts:
- `test_safe_read_refuses_no_access_page` (**153-189**, `#if !VMHOOK_OS_IOS`):
  `allocate_rwx` one page, write a byte, `protect(no_access)`, then
  `safe_read(&dst, block, 1)` must return **false** with no fault. Gracefully
  *skips* (prints `[INFO]`, returns) if the sandbox refused `PROT_NONE`
  (172-177). This is THE across-a-no-access-page test for this feature — but it
  only reads **1 byte fully inside** the no-access page.
- `test_os_primitive_input_guards` (**237-249**): the null/zero guard matrix —
  `safe_read(nullptr, &src, 1)`, `safe_read(&dst, nullptr, 1)`,
  `safe_read(&dst, &src, 0)`, `safe_read(nullptr, nullptr, 0)` all return false.
- `test_protect_all_enum_values` / round-trip tests use `safe_read` only
  implicitly via the protect round-trip in `tests/test_os_layer.cpp`.

### What is still MISSING (the test plan I own and will implement)

All pure-logic (no JVM / no oop); they belong alongside the existing file. Every
no-access probe must keep the sandbox-skip pattern (172-177) and stay behind
`#if !VMHOOK_OS_IOS` (iOS cannot honour the contract — flaw 1).

1. **Positive round-trip across widths** — `safe_read` a readable RWX page into
   `dst` for size = 1, 2, 4, 8, 16, page-1, page, and assert the bytes match a
   reference `memcpy` exactly. Locks that a *good* read actually copies (the
   existing tests only assert the *negative* no-access case + the input guards).
2. **Straddle a page boundary from readable INTO no-access** — allocate 2 pages,
   make page 0 RWX and page 1 `no_access`, write a marker spanning
   `[page-4, page+4]`, then `safe_read(dst, base + page - 4, 8)`. Must return
   **false** (the read crosses into the unreadable page) and — per the
   all-or-nothing contract — should NOT report success on the readable prefix.
   This is the test that exposes flaw 2 (partial `process_vm_readv` leaving
   `dst` half-written): assert `dst` is either untouched or fully ignored on the
   `false` return. Today this would pass the bool check but `dst` semantics are
   unpinned — the test pins them.
3. **Read ending EXACTLY at the readable/no-access boundary** — `safe_read(dst,
   base + page - 8, 8)` (last 8 bytes of the readable page, none of the
   no-access page) must return **true** and copy correctly. The off-by-one
   sibling of #2; proves the wrapper doesn't over-read by even one byte.
4. **Read STARTING exactly on the no-access page** — `safe_read(dst, base +
   page, 1)` and `…, 8` must both return **false**. (The existing 153-189 test
   reads inside the no-access page but at offset 0 of a single no-access page;
   this variant verifies the readable→unreadable transition address.)
5. **`safe_read` from freed / unmapped memory** — `allocate_rwx`, `release`,
   then `safe_read(dst, block, 8)` must return **false** (not crash). Mirrors
   `test_query_region_reports_free_for_unallocated` for the read path.
6. **`safe_read` from a known-bad pointer** — the canonical sentinels
   (`reinterpret_cast<void*>(0x1)`, `0xFFFF` = `user_address_floor`,
   `0xDEADBEEF`, a non-canonical `0xFFFF'8000'0000'0000`) must each return
   **false** with no fault. (NB these bypass `safe_read_pointer`'s pre-filters
   and hit `safe_read` directly, so this tests the kernel/signal path, not the
   filter.)
7. **Self-read sanity (dst == valid, src == valid, overlapping)** — `safe_read`
   where `dst` and `src` alias / overlap: today there's no overlap guard
   (flaw-adjacent). Assert behaviour is defined (memcpy UB on overlap is the
   risk on the Linux/iOS fallback). At minimum pin "non-overlapping is the only
   supported usage" with a doc-comment test, or assert a small overlapping read
   doesn't corrupt. Drives a decision on whether to document the restriction.
8. **Concurrent probes on multiple threads** — spawn N threads each doing a
   no-access `safe_read` in a tight loop; assert every call returns false and no
   thread faults. This exercises the `thread_local active_state` (858) isolation
   — the property that two threads probing simultaneously don't clobber each
   other's `sigjmp_buf`. A regression that made `active_state` non-thread-local
   would crash here.
9. **Repeated fallback-path entry does not corrupt host signal handling** —
   after many no-access `safe_read`s (forcing the signal path), a *deliberate*
   readable `safe_read` still succeeds, AND a normal in-process operation still
   works (proves the handler self-disarm + re-arm via the `static` install
   didn't wedge SIGSEGV). This is the closest pure-logic proxy for flaws 3/4
   (can't fully test handler-chaining without a real JVM, but can assert the
   process stays healthy across hundreds of fallbacks).
10. **`size == 0` with otherwise-valid pointers** — already covered (245), keep
    it; add `size == 1` with both valid as the positive twin to prove the guard
    is `size == 0`, not `size < something`.
11. **Huge `size` does not hang/crash** — `safe_read(dst, valid_small_buffer,
    SIZE_MAX)` and a `src + size`-wrapping size: assert it returns **false**
    promptly (drives flaw 7's bound check). Currently UB-ish on the Linux
    fallback; the test encodes the desired "reject ranges that wrap" contract.

Roughly 30-40 `check()` assertions once added — the negative/no-access cases are
the load-bearing ones; the positive round-trip + boundary trio (#1-#4) are the
ones the current file is missing entirely.

## Known JDK-version sensitivities

`os::safe_read` is a pure OS/libc primitive — it touches NO HotSpot internals,
NO oops, NO `gHotSpotVMStructs`, NO libjvm export — so its own behaviour is
**completely JDK-version-independent** (Java 8 vs 9+ vs 21+ vs 26 make zero
difference to the read itself). The version/runtime sensitivities live in its
*environment* and its *callers*:

- **Kernel / sandbox, not JDK (Linux/Android):** whether the fast
  `process_vm_readv` path (928) is taken depends on the kernel (≥ 3.2) and on
  `ptrace_scope` / seccomp / container caps, NOT on the JDK. When it is blocked,
  the signal fallback (933-948) runs and flaws 3/4 (handler hijack) become live
  — and HotSpot's *own* SIGSEGV handler (which every modern JDK installs for
  implicit null-checks and guard-page stack-overflow recovery) is what gets
  clobbered. So the *interaction* is JDK-relevant even though the function isn't:
  newer JDKs lean harder on implicit-null-check SIGSEGV handling, so a clobbered
  handler hurts more on 9+ than on 8.
- **`PROT_NONE` availability:** some hardened/sandboxed CI runners forbid
  `PROT_NONE`; the across-no-access tests must skip (as 172-177 does) rather than
  fail. Independent of JDK.
- **Compressed-oops decode is the *consumer's* JDK concern, not safe_read's:**
  `safe_read_pointer` (1838-1862) reads a raw pointer-width word; whether that
  word is then decompressed depends on `UseCompressedOops` / `UseCompressedClassPointers`
  (default under ~32 GB heaps on all JDKs) — but that happens in `decode_oop_pointer`
  / `decode_klass_pointer`, downstream of `safe_read`, which only ever copies
  `sizeof(void*)` raw bytes regardless of JDK.
- **Apple W^X / iOS:** iOS has no fault-safe read API (flaw 1) on any toolchain;
  macOS uses `mach_vm_read_overwrite` (910-918) uniformly across all JDK
  versions. Page size differs (16K on Apple silicon) but the tests use
  `page_size()` (476-486), so no hard-coded-4096 hazard.
