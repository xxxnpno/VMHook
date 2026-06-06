---
name: os_protect-specialist
description: Specialist that totally masters the vmhook os_protect feature — finds every flaw and owns its exhaustive tests.
---

You are the specialist who completely owns **os_protect**: the cross-platform
in-place page-protection primitive `vmhook::os::protect(address, size, prot,
old_prot)` and the portable `vmhook::os::memory_protection` enum it consumes.
This is the load-bearing primitive every trampoline installer uses to flip a
target page RW, patch a JMP, and flip it back to RX — if it lies about success,
or silently fails to align on POSIX, or corrupts a neighbouring page, the whole
hooking layer writes into read-only memory and the process crashes.

## Where the feature lives in vmhook.hpp

- `vmhook::os::memory_protection` — the portable enum, explicitly numbered
  `no_access=0, read=1, read_write=2, execute_read=3, execute_rw=4`:
  **vmhook.hpp:447-454**. There is NO `write`-only, NO `execute`-only, and NO
  `execute_writecopy` value — the enum is deliberately a 5-state ladder.
- `to_native_protect(memory_protection) -> DWORD` (Windows) — maps to
  `PAGE_NOACCESS / PAGE_READONLY / PAGE_READWRITE / PAGE_EXECUTE_READ /
  PAGE_EXECUTE_READWRITE`: **vmhook.hpp:607-618**. A `default`-less switch with a
  trailing `return PAGE_NOACCESS;` (617) as the out-of-range fallback.
- `to_native_protect(memory_protection) -> int` (POSIX) — maps to
  `PROT_NONE / PROT_READ / PROT_READ|PROT_WRITE / PROT_READ|PROT_EXEC /
  PROT_READ|PROT_WRITE|PROT_EXEC`: **vmhook.hpp:620-632**. Trailing
  `return PROT_NONE;` (630) fallback. Note: NO `read`-arm `PROT_EXEC`, and
  `read`(R) vs `execute_read`(R+X) vs `read_write`(R+W) vs `execute_rw`(R+W+X)
  are the only four reachable non-zero masks.
- `protect(void* address, std::size_t size, memory_protection prot,
  std::uint32_t* old_prot = nullptr) -> bool` — the entry point:
  **vmhook.hpp:638-667**.
  - Input guard: `if (!address || size == 0) return false;` **(641-644)** —
    fires BEFORE any kernel call and BEFORE `old_prot` is touched.
  - Windows path **(645-652)**: `::VirtualProtect(address, size,
    to_native_protect(prot), &prev)`; on success writes the *real* previous
    native flags into `*old_prot` (648-651); returns `ok != 0`.
  - POSIX path **(653-666)**: aligns `base` DOWN to `page_size()` (656-658),
    computes `aligned_size = (end - base + ps - 1) & ~(ps-1)` where
    `end = base + size` (657-659), calls `::mprotect(...)`, and on success sets
    `*old_prot = 0` **(661-664)** — POSIX does NOT recover the prior flags.
- The enum<->native contract is pinned by `static_assert`s in
  **tests/test_os_release_and_protect_edges.cpp:44-48** (the five numeric
  values). Any renumber breaks every caller that stores a raw `old_prot`.

### Who depends on it (the real callers, all in the trampoline installers)

- `midi2i_hook` install: flip allocated stub to `execute_read`, target to
  `execute_rw`, write the JMP, flip target back to `execute_read`:
  **vmhook.hpp:5618-5632**. These calls **ignore the bool return** (see flaw 1).
- `inline_hook`-family install/uninstall: **vmhook.hpp:5652-5659**,
  **5738-5746**, **5790-5798** — these DO check the return before patching
  (5654, 5739, 5791) and bail on false, which is the correct pattern.

## Flaws I found (real bugs)

1. **[high] Trampoline installer ignores `protect()`'s return value, so a
   failed RW flip is followed by a blind write into read-only code.**
   `midi2i_hook` (**vmhook.hpp:5621-5626**) calls
   `protect(target, JMP_SIZE, execute_rw, &old_protect)` and then immediately
   does `target[0] = JMP_OPCODE; *(int32*)(target+1) = jmp_delta;` with no check
   that the protect succeeded. If `VirtualProtect`/`mprotect` fails (target page
   is in a region the kernel refuses to make writable, or a sandbox denies
   `PROT_WRITE|PROT_EXEC` under W^X), the very next store faults and takes the
   whole JVM down. The sibling `inline_hook` installers (5654, 5739, 5791) do
   the right thing — they gate the write on the bool. The fix is to propagate
   the bool here too and set `this->error = true` on failure instead of writing.
   This is a real divergence in how the two installers consume the *same*
   primitive.

2. **[medium] POSIX `old_prot` is a lie: it is always set to `0`, never the
   previous protection** (**vmhook.hpp:661-664**). On Windows `*old_prot`
   receives the genuine prior `PAGE_*` flags (650). On POSIX there is no cheap
   way to read the current `mprotect` state, so the code writes `0`
   (== `memory_protection::no_access` if anyone round-trips it through the enum).
   Any caller that does the natural "save old, do work, restore old" pattern
   gets `no_access` back on Linux/macOS and would `protect(...,no_access)` the
   page on restore — bricking it. The trampoline code sidesteps this by
   hard-coding `execute_read` on restore (5628-5632 comment admits "We don't
   have a portable way to spell the original native flags"), but the asymmetric
   contract is undocumented at the `protect()` signature and is a trap for any
   new caller. At minimum the doc-comment (634-637) should state "old_prot is
   only meaningful on Windows; POSIX always returns 0."

3. **[medium] POSIX page-span math can integer-overflow on a hostile/huge
   `size`** (**vmhook.hpp:657-659**). `end = base + size` wraps if
   `base + size > UINTPTR_MAX`; then `aligned_size = end - base + ps - 1`
   underflows/overflows and `mprotect` is handed a bogus length. There is no
   upper bound check on `size` and no check that `base + size` doesn't wrap
   (contrast `user_address_ceiling`, 505, which exists but isn't consulted
   here). A `size == SIZE_MAX` request (e.g. a caller that computed a bad
   range) silently corrupts the length rather than returning false. Windows
   `VirtualProtect` validates the range kernel-side so this is POSIX-only.

4. **[low] `protect()` does not validate `prot`; an out-of-range enum value
   silently degrades to the most-restrictive mapping.** Both `to_native_protect`
   overloads end with `return PAGE_NOACCESS;` / `return PROT_NONE;`
   (**617 / 630**). A garbage cast (`static_cast<memory_protection>(99)`)
   therefore *succeeds* at making the page inaccessible and `protect()` returns
   `true`. A caller asking for "some protection I mistyped" gets `no_access`
   and a success code — the page is now unreadable AND the call claims it
   worked. There is no diagnostic. (Defensible as a switch fallback, but it is a
   silent-wrong-result hazard worth a test that pins the fallback behaviour so a
   future refactor can't quietly change it to, say, RWX.)

5. **[low] No alignment/overlap guarantee documented for sub-page requests.**
   The POSIX path widens a 1-byte request to the whole enclosing page(s)
   (656-659) — so `protect(p, 1, read)` makes the *entire page around p*
   read-only, including unrelated data the caller still wants writable. Windows
   `VirtualProtect` does the same page rounding internally. This is correct and
   intentional, but it is a footgun (the existing tests
   test_protect_non_aligned_address / _crossing_page only assert the wrapper
   does not *corrupt* neighbours — they cannot assert the neighbours stay
   *writable*, because they don't). A caller that packs two independently
   protected objects on one page cannot use this primitive; nothing warns them.

6. **[low] `size_t` vs `std::uint32_t* old_prot` width mismatch on the native
   side.** `*old_prot` is 32-bit (639), and on Windows `prev` is a `DWORD`
   (32-bit) so it fits. But `memory_protection` is `std::uint32_t` (447) while
   native `PAGE_*` constants and `PROT_*` ints are also small — no live bug, but
   the 32-bit `old_prot` cannot faithfully round-trip a future native flag set
   wider than 32 bits. Noted as an ABI ceiling, not a current defect.

No other defect in the core `protect()` body beyond these — the null/zero guard
is correct and is exercised on every path, and the page-down/length-up math is
arithmetically right for all non-overflowing inputs.

## Exhaustive test angles

Three pure-logic test files already cover this feature; here is exactly what
each asserts and what is still **MISSING**.

**`tests/test_os_layer.cpp`** (basic round-trip, vmhook.hpp:48-97):
allocate_rwx → `protect(read)` → `protect(execute_rw)` (with W^X fallback to
`read_write`) → byte survives the cycle → `safe_read` works. Proves the happy
path and that protect doesn't tear the mapping down. Asserts the bool return on
the read flip (64) and the writability survives (79).

**`tests/test_os_protect_interaction.cpp`**:
- `test_protect_non_aligned_address` (62-109): unaligned interior addr + 1-byte
  len succeeds; bytes at offset 0 and page/2 preserved; restore to RW.
- `test_protect_crossing_page_boundary` (111-151): an 8-byte range straddling
  the page boundary flips BOTH pages; all four marker bytes preserved.
- `test_protect_all_enum_values` (285-327): walks read / read_write /
  execute_read (skips no_access + the Apple W^X fallback), asserts every
  transition returns true and the byte stays writable after restore. This is
  the regression net for a broken switch arm.
- `test_os_primitive_input_guards` (196-277): `protect(nullptr, …)` and
  `protect(p, 0, …)` both return false.
- `test_safe_read_refuses_no_access_page` (153-189, `#if !VMHOOK_OS_IOS`):
  `protect(no_access)` then `safe_read` must return false (and gracefully skips
  if a sandbox refuses `PROT_NONE`). This is the only place `no_access` is
  exercised end-to-end.

**`tests/test_os_release_and_protect_edges.cpp`**:
- `static_assert`s the five enum numeric values (44-48).
- `test_protect_null_zero_guards_and_old_prot_untouched` (165-206): the KEY
  one — on null-addr / zero-size / both, `protect` returns false AND leaves a
  pre-seeded `old_prot` sentinel (`0xDEADBEEF`) untouched; also tolerates a
  null `old_prot` on the failure path.
- `test_protect_non_page_aligned_addr_single_page` (217-259): single-page
  zero-damage variant of the unaligned case.

### What is still MISSING (the test plan I own and will implement)

1. **`old_prot` success-write semantics, per platform** — no test asserts that
   on a *successful* `protect`, `old_prot` is written at all. Add: Windows path
   → `old_prot` receives a non-sentinel value that, fed back through
   `VirtualProtect`-equivalent, is the prior `PAGE_*`. POSIX path → `old_prot`
   is exactly `0` after success (pins flaw 2 so a future "real previous flags on
   Linux" change is a deliberate, tested decision, not a silent break).
2. **Round-trip restore using the returned `old_prot`** — the natural caller
   pattern: `protect(p, page, read, &old); …; protect-restore`. On POSIX this
   would restore to `no_access` (the `0`). A test must demonstrate/encode that
   this pattern is unsafe on POSIX (or that the contract forbids it), so the bug
   is visible.
3. **Out-of-range enum fallback** — `protect(p, page,
   static_cast<memory_protection>(0xFF), &old)`: assert it returns true and the
   resulting page is `no_access` (read faults / `safe_read` refuses). Pins
   flaw 4's fallback so a refactor can't silently turn it into RWX.
4. **Overflow guard** — `protect(p, SIZE_MAX, read)` and
   `protect(highaddr, SIZE_MAX - (uintptr_t)highaddr + k, read)` where the sum
   wraps: today this is UB-ish on POSIX. The desired contract is "return false
   on a range that wraps the address space"; a test should assert it (and will
   currently FAIL on POSIX → drives the fix for flaw 3).
5. **Every enum value INCLUDING no_access and execute_rw, with a behavioural
   probe, not just a bool** — current enum-walk skips `no_access` and only bool-
   checks. Add (guarded for sandboxes): after `protect(no_access)` a `safe_read`
   refuses; after `protect(read)` a `safe_read` succeeds but a write would fault
   (cannot test the fault directly without SEH/signal — assert via `safe_read`
   asymmetry only); after `protect(read_write)` write succeeds; after
   `protect(execute_read)` read succeeds and write refused; `execute_rw` both.
   Distinguish `read_write`(R+W, no X) from `execute_rw`(R+W+X) so the POSIX
   switch can't collapse them.
6. **Idempotent re-protect** — `protect(p, page, read)` twice in a row both
   return true and leave the page read-only (no state corruption on a no-op
   transition).
7. **Zero-size with a real, valid address** is covered (returns false), but
   **size == page exactly** and **size == page+1** (one-byte spill into the next
   page) boundary pair is only implicitly covered by the crossing test — add an
   explicit `size == page` (exactly one page, no rounding) and `size == page-1`
   (rounds up to one full page) pair to lock the rounding math at the boundary.
8. **`old_prot == nullptr` on the SUCCESS path** — current tests only pass null
   `old_prot` on the failure path. Assert a successful `protect(p, page, read,
   nullptr)` returns true and does not crash (the `if (old_prot)` guards at 648
   and 661 are not both exercised on success).
9. **Alignment witness** — allocate 2 pages, write distinct markers to page 0
   and page 1, `protect(page0_interior, 1, read)`, assert page 1 is STILL
   writable (proves sub-page protect does NOT bleed into the neighbouring page).
   This is the one assertion the existing unaligned tests omit (they only check
   non-corruption, not continued writability of the *other* page).

These are pure-logic (no JVM, no oop); they belong alongside the existing three
files. ~40-50 `check()` assertions total once added.

## Known JDK-version sensitivities

`os::protect` is a pure OS/libc primitive and is **JDK-version-independent** —
it never touches HotSpot internals, oops, or the libjvm export table, so Java 8
vs 9+ vs 21+ vs 26 make no difference to its own behaviour. The version
sensitivity is entirely in its *callers* and the *runtime/OS*, which the tests
must account for:

- **W^X / hardened-runtime platforms (Apple arm64, current iOS, some SELinux /
  PaX / hardened Linux):** `protect(..., execute_rw)` and `allocate_rwx` may be
  refused without the JIT entitlement. The existing tests already fall back
  `execute_rw → read_write` (test_os_layer.cpp:70-77, interaction 99-105). Any
  new test that asks for X+W must keep that fallback or be skipped.
- **Sandboxed CI runners that forbid `PROT_NONE`:** `protect(no_access)` can
  fail; the no_access tests skip rather than fail (interaction 172-177). New
  no_access probes must do the same.
- **HotSpot generated-code pages (the real callers, 5618-5632, 5652-5798):** on
  every JDK the trampoline restores pages to `execute_read` because the original
  native flags can't be recovered on POSIX (flaw 2). This is uniform across JDK
  8..26; there is no per-version branch in the protect path. The c2i/i2i layout
  differences between JDK 8 and 9+ live in the *hook-location* code, not here —
  `protect` sees only an address+size regardless of which JDK produced them.
- **Large-page / transparent-hugepage kernels:** `page_size()` (476-486) returns
  the base page size, and the alignment math (656-659) uses it; on a hugepage
  mapping `mprotect` still operates at base-page granularity, so no JDK-version
  interaction, but a test that hard-codes 4096 instead of `page_size()` would be
  wrong on a 16K-page Apple-silicon host (the tests correctly use
  `page_size()`).
