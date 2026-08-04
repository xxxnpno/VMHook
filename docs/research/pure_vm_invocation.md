# Pure-VM invocation: calling Java methods from native code with zero JNI, HotSpot 8 → 26

Research note, 2026-08-04. Read-only investigation; no tracked source was modified.

Primary evidence in this document is **empirical**: three real HotSpot JVMs on this machine were
loaded in-process, their `gHotSpotVMStructs` tables dumped, and a complete zero-JNI invocation
path was built and exercised end-to-end against each. Secondary evidence is OpenJDK source and
prior art. Where a claim is inferred rather than measured it is marked **[INFERRED]**.

> **Relation to `docs/ROADMAP_ZERO_JNI.md`.** That roadmap's §3.3 lists the live-JVM VMStructs
> probe as *"under measurement … in flight"*, and phase item **1.5** states that this single
> measurement *"decides between a one-day fix and a multi-week one"* and gates whether phase 4.3
> is a lookup fallback or a full interpreter-entry implementation. **This document is that
> measurement, and the answer is: lookup fallback.** No interpreter-entry reimplementation is
> needed. See §2.3 and §10.
> (Note: this file was written to `audit/research/` as instructed, but `audit/` was emptied by a
> concurrent restructure while this ran; it may belong under `docs/research/` in the new layout.)

JVMs measured (Windows x64, all product builds):

| Label | Build | `jvm.dll` |
|---|---|---|
| JDK 8  | Temurin 8.0.492+9   | `C:\Program Files\Eclipse Adoptium\jdk-8.0.492.9-hotspot\jre\bin\server\jvm.dll` |
| JDK 21 | Temurin 21.0.11+10  | `C:\Program Files\Eclipse Adoptium\jdk-21.0.11.10-hotspot\bin\server\jvm.dll` |
| JDK 26 | Oracle/OpenJDK 26.0.1 | `C:\Program Files\Java\jdk-26.0.1\bin\server\jvm.dll` |

---

## 0. Executive verdict

**Zero-JNI invocation is possible on every HotSpot version from 8 to 26, on the same code path,
and it has been proven working here on 8, 21 and 26.** It does *not* depend on any VMStructs
entry that comes and goes across versions.

The blocker recorded by the owner ("`StubRoutines::_call_stub_entry` is missing from VMStructs on
JDK 21+, so fall back to JNI") is **half right and half wrong, in a way that matters**:

* `_call_stub_entry` is **not in VMStructs on JDK 8 either**. It has never been in VMStructs on
  any version measured. So `vmhook::detail::find_call_stub_entry()` returns `nullptr` on *every*
  JDK, and `method_proxy::call()` has been dead on *every* JDK — not just 21+.
* But `StubRoutines::_call_stub_return_address` **is** in VMStructs on 8, 21 and 26, and the call
  stub entry is trivially and deterministically recoverable from it. So the capability was never
  actually lost; it was only ever looked up under the wrong name.
* Separately, the existing `call()` implementation would corrupt the VM even if it *did* find the
  stub, because it omits the `JavaCallWrapper` that HotSpot's frame walker dereferences. This was
  reproduced as a hard JVM crash (see §5.4).

---

## 1. What the header does today, and what is wrong with it

### 1.1 The lookup that can never succeed

`vmhook/ext/vmhook/vmhook.hpp:14771-14783`

```cpp
inline auto find_call_stub_entry() noexcept -> void*
{
    static const vmhook::hotspot::vm_struct_entry_t* const entry{
        vmhook::hotspot::iterate_struct_entries("StubRoutines", "_call_stub_entry") };
    if (!entry || !entry->address) { return nullptr; }
    ...
}
```

Measured result of that exact lookup:

| JDK | `StubRoutines::_call_stub_entry` in `gHotSpotVMStructs` | `StubRoutines::_call_stub_return_address` |
|---|---|---|
| 8  | **absent** | present (`static=1`) |
| 21 | **absent** | present (`static=1`) |
| 26 | **absent** | present (`static=1`) |

On JDK 26 the *entire* `StubRoutines` VMStructs surface is a single entry — `_call_stub_return_address`
— because of the stub-generator macro refactor (`STUBGEN_BLOBS_DO` / `STUBGEN_ENTRIES_DO`,
`DECLARE_ENTRY_FIELD`). On JDK 21 there are 62 `StubRoutines` entries (crypto + arraycopy +
`_verify_oop_count` + `_call_stub_return_address`); on JDK 8, 12. **In no version is the call-stub
entry among them.** This is deliberate: VMStructs exists to serve the Serviceability Agent, which
is documented as read-only and out-of-process and therefore has no use for a call gate, but *does*
need `_call_stub_return_address` to terminate stack walks at entry frames.

Consequence: `vmhook.hpp:15068-15079`

```cpp
void* const call_stub{ vmhook::detail::find_call_stub_entry() };
if (!call_stub)
{
    // Pure-VM: no JNI CallXxxMethod fallback. ...
    return value_t{ std::monostate{} };
}
```

…takes the early-return branch **unconditionally, on every JDK**. `method_proxy::call()` is
currently a no-op that logs an error. The comment at `15062-15064` ("On modern JDKs (21+) …
is often missing") understates it.

### 1.2 The four other defects in `method_proxy::call()`

Even with a working stub pointer, the current call site is unsafe or incorrect:

1. **No `JavaCallWrapper`.** `vmhook.hpp:15258-15259` passes `-1` as the first argument (`link`).
   That argument is not a sentinel — HotSpot's frame walker reads it as a `JavaCallWrapper*`.
   Proven to crash the VM under GC (§5.4).
2. **No frame-anchor save/clear.** `JavaThread::_anchor` is left pointing at the caller's last Java
   frame while a *new* Java entry is created above it. Corrupts stack walks when invoking from a
   context that already has Java frames (i.e. from inside a detour — the exact intended use).
3. **Long/double argument packing is wrong.** `vmhook.hpp:15143` declares `std::intptr_t params[8]`
   and `15214-15217` packs any `sizeof(T) <= 8` value into **one** slot. HotSpot's interpreter takes
   **two** slots for `J`/`D`, with the value in the **higher-indexed** slot
   (`JNITypes::put_long(l, _value, pos)` → `*(jlong*)(to + pos + 1) = from; pos += 2;`).
   Measured directly (§5.3, T4): value in slot 0 → wrong result; value in slot 1 → correct.
   The `static_assert(sizeof...(args_t) <= 8)` at `15230` is also counting arguments, not slots.
4. **The "cannot clear a pending exception without JNI" claim is false.** `vmhook.hpp:15296-15300`
   says clearing would need JNI. `ThreadShadow::_pending_exception` is a VMStructs entry at
   **offset 8 on JDK 8, 21 and 26** (`ThreadShadow` is the root of `Thread`, so the absolute offset
   in `JavaThread` is 8). Reading it and writing `nullptr` is a two-line pure-VM operation and was
   verified working on all three JDKs (§5.3, T6).

### 1.3 A minor cosmetic finding

`vmhook.hpp:3256-3265` and `8319-8322` try `Method::_from_compiled_code_entry_point` first and fall
back to `_from_compiled_entry`. Measured: the field is named **`_from_compiled_entry` on 8, 21 and
26**. The first lookup never hits on any supported version; the fallback carries every call. Not a
bug (the fallback is correct), just dead work and a misleading comment.

---

## 2. Avenue 1 — `StubRoutines::_call_stub_entry`

### 2.1 Is it in VMStructs? No, and it never was.

Measured above. On master the field is not even written literally in `stubRoutines.hpp` any more —
it is generated by `DECLARE_ENTRY_FIELD` from `STUBGEN_ENTRIES_DO`, which is why searching the JDK
head sources for `static address _call_stub_entry` comes up empty. The accessor still exists:

```cpp
static CallStub call_stub() { assert(_call_stub_entry != nullptr, "");
  return CAST_TO_FN_PTR(CallStub, _call_stub_entry); }
```

So the field is alive and well as a C++ static — it is simply not published to VMStructs, on any
version.

### 2.2 Is it reachable as an exported symbol?

**Windows: no.** `jvm.dll` was dumped on all three JDKs:

| JDK | total exports | plain C exports | `JVM_*` | mangled C++ exports |
|---|---|---|---|---|
| 8  | 5747 | ~252 | 219 | 2746 — **all `??_7…@@6B@` vftables** |
| 21 | 8498 | ~275 | 209 | 4110 — vftables + a few JVMCI `c2v_*` |
| 26 | 8328 | ~265 | 204 | 4030 — vftables + `topLevelExceptionFilter` |

Grepping every export list for `call_stub`, `JavaCalls@`, `JNIHandles@`, `StubRoutines@` returns
**nothing but vftable names**. This is by construction: `make/hotspot/lib/CompileJvm.gmk` generates
the Windows `.def` by filtering `dumpbin /symbols` for `??_7.*@@6B@` only; everything else exported
is what carries `JNIEXPORT` (`extern "C"`). So on Windows there is no `GetProcAddress` route to any
HotSpot-internal invocation primitive.

**Linux/macOS: not via `dlsym`, but yes via `.symtab`.** `libjvm.so` is built with
`-fvisibility=hidden` (`make/autoconf/flags-cflags.m4`), so `.dynsym` contains only ~260 symbols —
the `JVM_*` / `JNI_*` / `jio_*` / `AsyncGetCallTrace` / `gHotSpotVM*` C API and nothing else. But on
unstripped builds (Temurin ships unstripped) the static `.symtab` retains ~66–70k local symbols
including:

```
_ZN12StubRoutines16_call_stub_entryE
_ZN12StubRoutines25_call_stub_return_addressE
_ZN9JavaCalls12call_virtualEP9JavaValue6HandleP5KlassP6SymbolS6_P10JavaThread
_ZN9JavaCalls11call_staticEP9JavaValueP5KlassP6SymbolS5_6HandleP10JavaThread
_ZN9JavaCalls22construct_new_instanceEP13InstanceKlassP6Symbol6HandleP10JavaThread
_ZN10JNIHandles10make_localEP10JavaThreadP7oopDescN17AllocFailStrategy13AllocFailEnumE
```

Reaching them requires parsing ELF yourself, as async-profiler does
(`src/symbols_linux.cpp`, `findSection(SHT_SYMTAB, ".symtab")`, plus build-id / `.gnu_debuglink` /
debuginfod fallbacks). Distro packages that strip `libjvm.so` lose this.

### 2.3 The recovery that actually works everywhere: scan back from `_call_stub_return_address`

`StubRoutines::_call_stub_return_address` is the address *inside* the call stub immediately after
the `call` to the method entry point (`generate_call_stub(address& return_address)` writes
`return_address = __ pc();` right after the call). The stub is one contiguous generated function,
so its entry is a short backward scan away, and the prologue is unmistakable.

Measured, all three JDKs, **exactly one match** in the whole backward window:

| JDK | `_call_stub_return_address` − entry | prologue bytes at the recovered entry |
|---|---|---|
| 8  | 179 bytes | `55 48 8B EC 48 81 EC …  4C 89 4D 28 …` |
| 21 | 404 bytes | `55 48 8B EC 48 81 EC D8 01 00 00 4C 89 4D 28 44 89 45 20 48 89 55 18 48 89 4D 10` |
| 26 | 175 bytes | `55 48 8B EC 48 81 EC D8 00 00 00 4C 89 4D 28 44 89 45 20 48 89 55 18 48 89 4D 10` |

Decoded (JDK 21/26, identical):

```
55                push rbp                       ; MacroAssembler::enter()
48 8B EC          mov  rbp, rsp
48 81 EC ........ sub  rsp, imm32
4C 89 4D 28       mov  [rbp+0x28], r9            ; Method*            (Win64 arg 4)
44 89 45 20       mov  [rbp+0x20], r8d           ; result_type (int32)(Win64 arg 3)
48 89 55 18       mov  [rbp+0x18], rdx           ; result*            (Win64 arg 2)
48 89 4D 10       mov  [rbp+0x10], rcx           ; JavaCallWrapper*   (Win64 arg 1)
```

The tail of the stub, immediately before and after the return address (JDK 21, verbatim dump):

```
4C 8B 7D 48       mov  r15, [rbp+0x48]           ; r15 = JavaThread*
44 8B 4D 40       mov  r9d, [rbp+0x40]           ; size_of_parameters
45 85 C9          test r9d, r9d
0F 84 13 00 00 00 je   done
4C 8B 45 38       mov  r8,  [rbp+0x38]           ; parameters*
41 8B D1          mov  edx, r9d
49 8B 00          mov  rax, [r8]                 ; loop: push parameters[i]
49 83 C0 08       add  r8, 8
FF CA             dec  edx
50                push rax
75 F4             jne  loop
48 8B 5D 28       mov  rbx, [rbp+0x28]           ; rbx = Method*
48 8B 55 30       mov  rdx, [rbp+0x30]           ; entry_point
4C 8B EC          mov  r13, rsp                  ; r13 = sender_sp
FF D2             call rdx
<<< _call_stub_return_address >>>
48 8B 4D 18       mov  rcx, [rbp+0x18]           ; result*
8B 55 20          mov  edx, [rbp+0x20]           ; result_type
83 FA 0C          cmp  edx, 12                   ; T_OBJECT
...
```

This confirms the entire ABI empirically, including that `result_type` is read as a **32-bit** value
and that `parameters[0]` ends up at the **highest** address (pushed first) — i.e. `parameters[0]` is
the receiver for an instance call, matching the interpreter's `locals[0]`.

**Scan robustness.** The scan must be bounded. `CodeCache::_low_bound` is a VMStructs static on 21
and 26 but **absent on JDK 8** (JDK 8 has `CodeCache::_heap` → `CodeHeap::_memory`); fall back to a
`VirtualQuery`/`/proc/self/maps` region base. In all three measurements the scan terminated within
a few hundred bytes and produced a *unique* candidate, so the "first match wins" rule is safe here,
but the byte-level validation of the four argument spills should be treated as mandatory, not
optional.

**Non-Windows x86-64 [INFERRED].** The stub is emitted by HotSpot's own `MacroAssembler`, not by the
platform C compiler, so `enter()` encodes identically as `55 48 8B EC` on Linux and macOS. Only the
*source registers* of the spills change (SysV `c_rarg0..3` = rdi, rsi, rdx, rcx), so the validation
bytes become `48 89 4D 28 / 89 55 20 / 48 89 75 18 / 48 89 7D 10`, and SysV additionally spills
`entry_point` and `parameters` (`r8`, `r9`) into `[rbp+0x30]` / `[rbp+0x38]`. Destination slot
offsets are ABI-independent. This has **not** been measured.

**aarch64 [INFERRED].** Completely different prologue (`stp x29, x30, [sp, #-N]!` …) and
`frame::entry_frame_call_wrapper_offset` differs; a separate pattern is required. Not investigated.

---

## 3. Avenue 2 — the call-stub calling convention

The typedef has been **stable since JDK 8** — this is one of the least volatile interfaces in
HotSpot. From `src/hotspot/share/runtime/stubRoutines.hpp` on master (JDK 26/27-dev):

```cpp
typedef void (*CallStub)(
  address   link,
  intptr_t* result,
  int       result_type, /* BasicType on 4 bytes */
  Method* method,
  address   entry_point,
  intptr_t* parameters,
  int       size_of_parameters,
  TRAPS
);
```

Drift notes:

* `result_type` was `BasicType` (an enum) in JDK 8; it became `int` with the explicit comment
  `/* BasicType on 4 bytes */`. **This is a no-op at the ABI level** — it was always passed in a
  32-bit register slot; the change only made that explicit. The header's `int` is correct.
* `TRAPS` is `JavaThread* THREAD` on JDK 21+ and `Thread* THREAD` on JDK ≤ 17. Same pointer at the
  ABI level for a JavaThread.
* No parameter was added or removed in 8 → 26. Confirmed by the identical machine code above.

`vmhook.hpp:15243-15252` already declares this correctly.

### Argument block layout (measured)

* One `intptr_t` slot per JVM stack slot, `parameters[0]` first.
* Instance call: `parameters[0]` = **raw, uncompressed oop** of the receiver. Not a handle.
  (`JavaCallArguments::parameters()` in HotSpot converts `Handle`s to raw oops in place immediately
  before handing the array to the stub.)
* Static call: no receiver slot.
* `Z B C S I F` → 1 slot each, value zero/sign-extended into the slot.
* **`J` and `D` → 2 slots, value in the SECOND (higher-index) slot**, first slot ignored.
  Measured: `Long.toString(1234567L)` with the value in `params[0]` returned a 1-character string;
  with the value in `params[1]` it returned the correct 7-character string. Same result on 8, 21, 26.
* `size_of_parameters` counts **slots**, not arguments.
* Return: written to `*result` by the stub, decoded per `result_type`. `T_OBJECT`/`T_ARRAY` yield a
  raw oop.

`BasicType` values (`T_BOOLEAN=4 … T_VOID=14`) are unchanged across 8→26; the header's
`sig_char_to_basic_type()` at `vmhook.hpp:14789-14806` is correct.

---

## 4. Avenue 3 — entering `_i2i_entry` / `_from_interpreted_entry` directly

Technically possible and **strongly not recommended**. The interpreter entry expects
`rbx = Method*`, `r13 = sender_sp`, arguments already on the stack, `r15 = JavaThread*` — this is
exactly what the call stub sets up, and it is ~15 instructions. But the call stub does one thing a
hand-rolled jump cannot easily do: it establishes an **entry frame** whose return address is
`StubRoutines::_call_stub_return_address`, which is the *only* marker HotSpot's frame walker uses to
recognise the native→Java boundary (`frame::is_entry_frame()`). Without it, any GC, safepoint,
stack trace, or exception unwind that reaches your synthetic frame walks straight into native code
and dies.

You could construct a fake entry frame yourself (push a fake return address equal to
`_call_stub_return_address`, lay out `rbp` so that `[rbp + 2*8]` holds a `JavaCallWrapper*`) — that
is precisely what the real stub does. Since the real stub is recoverable (§2.3), reimplementing it
buys nothing and costs a per-architecture assembly blob.

**What the header does today with these fields is fine and unrelated**: `get_i2i_entry()`
(`vmhook.hpp:2667`), `get_from_interpreted_entry()` (`2716`), `set_from_interpreted_entry()` (`3223`)
are used for *hook installation and deoptimisation*, not for calling. Keep them as they are.

One relevant caveat already known to this project: `get_i2i_entry()` throws for a method that has
never been dispatched (lazy link stub). For invocation, use `_from_interpreted_entry` (which the
header already does at `vmhook.hpp:15102`) — HotSpot's own `JavaCalls::call_helper` uses
`method->from_interpreted_entry()` too.

---

## 5. Avenue 4 — the thread-state / safepoint protocol, and what "only works inside a detour" really means

### 5.1 What is VMStructs-exposed (measured, all three JDKs)

| Field | 8 | 21 | 26 | Notes |
|---|---|---|---|---|
| `JavaThread::_thread_state` | ✓ (off 728) | ✓ (1100) | ✓ (1196) | `JavaThreadState` int8 |
| `JavaThread::_anchor` | ✓ (576) | ✓ (928) | ✓ (1040) | |
| `JavaFrameAnchor::_last_Java_sp / _pc / _fp` | ✓ 0/8/16 | ✓ 0/8/16 | ✓ 0/8/16 | size 24, stable |
| `JavaThread::_active_handles` | ✓ (as `Thread::_active_handles`, 56) | ✓ (1072) | ✓ (1168) | moved `Thread`→`JavaThread` |
| `JNIHandleBlock::_top` | ✓ (256) | ✓ (256) | ✓ (256) | |
| `ThreadShadow::_pending_exception` | ✓ (8) | ✓ (8) | ✓ (8) | absolute offset in JavaThread |
| `JavaCallWrapper::_anchor` | ✓ (32) | ✓ (32) | ✓ (32) | type size 64, stable |
| `frame::entry_frame_call_wrapper_offset` | ✓ = 2 | ✓ = 2 | ✓ = 2 | int constant |
| `frame::interpreter_frame_sender_sp_offset` | ✗ | ✓ = −1 | ✓ = −1 | |
| `StubRoutines::_call_stub_return_address` | ✓ | ✓ | ✓ | **the key enabler** |
| `_thread_in_Java` = 8 etc. | ✓ | ✓ | ✓ | int constants |
| `JavaThread::_jni_environment` | ✗ | ✗ | ✗ | see §6 |
| `JavaThread::_stack_overflow_state` | ✗ | ✗ | ✗ | |
| `JavaThread::_poll_data` / handshake state | ✗ | ✗ | ✗ | **no safepoint signal** |
| `SafepointSynchronize::_state` | ✗ | ✗ | ✗ | |
| `os::_polling_page` | ✓ | ✗ | ✗ | JDK ≤ 12 global poll only |
| `Threads::_thread_list` | ✓ | ✗ | ✗ | 21+ use `ThreadsList::_threads/_length` |
| `CodeCache::_low_bound` | ✗ | ✓ | ✓ | JDK 8 has `CodeCache::_heap` |

The critical negative: **there is no VMStructs-exposed way to ask "is a safepoint/handshake armed
right now?" on JDK 13+.** `_poll_data` and `SafepointSynchronize::_state` are both absent. That
constrains the design (see §5.5).

### 5.2 The synthetic `JavaCallWrapper` — the thing that was missing

`JavaCallWrapper` (verbatim from `src/hotspot/share/runtime/javaCalls.hpp`, master):

```cpp
JavaThread*      _thread;         // offset  0
JNIHandleBlock*  _handles;        // offset  8
Method*          _callee_method;  // offset 16
oop              _receiver;       // offset 24
JavaFrameAnchor  _anchor;         // offset 32  (24 bytes)
JavaValue*       _result;         // offset 56
```
Total 64 bytes — exactly what `gHotSpotVMTypes` reports (`TYPE JavaCallWrapper size=64`) with
`_anchor` at 32, on 8, 21 and 26 alike. This struct is **stack-allocated by the caller**; nothing
in the VM allocates it. So native code can build one on its own stack.

The frame walker's use of it, on the entry-frame path:

```
frame::sender_for_entry_frame(map):
    JavaFrameAnchor* jfa = entry_frame_call_wrapper()->anchor();   // *(rbp + 2*wordSize) then +32
    ...
    frame fr(jfa->last_Java_sp(), jfa->last_Java_fp(), jfa->last_Java_pc());
```

`entry_frame_is_first()` is `entry_frame_call_wrapper()->anchor()->last_Java_sp() == nullptr`.

### 5.3 The harness and the results

A test harness was built in the scratchpad (`probe3.cpp`) that:

1. loads `jvm.dll` via `LoadLibraryEx` and boots a VM with `JNI_CreateJavaVM` (scaffolding only);
2. reads `gHotSpotVMStructs` / `gHotSpotVMTypes` for every offset it needs;
3. locates the current `JavaThread*` by probing backwards from `JNIEnv*` and validating
   `_osthread->_thread_id == GetCurrentThreadId()`;
4. recovers the call stub by the backward prologue scan of §2.3;
5. resolves `Method*` **purely through VMStructs** — mirror oop → `java_lang_Class::_klass_offset`
   → `InstanceKlass::_methods` (`Array<Method*>`) → `Method::_constMethod` →
   `ConstMethod::_name_index/_signature_index` → `ConstantPool` base (at `sizeof(ConstantPool)` from
   `gHotSpotVMTypes`) → `Symbol::_length/_body` — i.e. zero JNI in the resolution path;
6. invokes with a synthetic `JavaCallWrapper`, anchor save/clear, and `JNIHandleBlock::_top`
   save/restore.

Results — **identical on JDK 8, 21 and 26, all PASS**:

| # | Test | Exercises | 8 | 21 | 26 |
|---|---|---|---|---|---|
| T1 | `Math.max(7,42)` → `42` | static, 2 int args, int return | ✓ | ✓ | ✓ |
| T2 | `"hello".length()` → `5` | instance, receiver in `params[0]`, int return | ✓ | ✓ | ✓ |
| T3 | `"hello".concat(" world!")` → oop, then `.length()` = 12 on the returned oop | object arg + object return + chaining | ✓ | ✓ | ✓ |
| T4 | `Long.toString(1234567L)` | 2-slot long; **only value-in-high-slot works** | ✓ | ✓ | ✓ |
| T5 | `System.currentTimeMillis()` (a **native** method) | interpreter native entry; JNI local refs still valid afterwards | ✓ | ✓ | ✓ |
| T6 | `Integer.parseInt("zz")` | `ThreadShadow::_pending_exception` set; cleared by a pure-VM write | ✓ | ✓ | ✓ |
| T7 | `System.gc()` through the stub | **full GC walks the synthetic entry frame** | ✓ | ✓ | ✓ |
| T8 | nested invoke from inside a native method reached **from Java** (`RegisterNatives` on `System.nanoTime`), with a live `_last_Java_sp`, plus a GC across **two stacked** synthetic entry frames | the detour scenario | ✓ | ✓ | ✓ |

T5 additionally proves a subtlety: HotSpot's interpreter **native** entry zeroes
`thread->_active_handles->_top`. Calling a `native` Java method through the call stub therefore
invalidates the caller's JNI local references unless `_top` is saved and restored. Without the
save/restore this reliably crashed the harness on the next JNI call.

### 5.4 The negative control — proof that the `JavaCallWrapper` is mandatory

The same harness rerun with `link = -1` (exactly what `vmhook.hpp:15259` passes today), calling
`System.gc()`:

```
#  EXCEPTION_ACCESS_VIOLATION (0xc0000005) at pc=0x00007ffd92cbc998
#  Problematic frame:  V  [jvm.dll+0x31c998]
   siginfo: EXCEPTION_ACCESS_VIOLATION, reading address 0x000000000000001f
   JavaThread 0x000000000011d2a0 (nid = 16484) was being processed
   Java frames:  j java.lang.Runtime.gc()V+0
                 j java.lang.System.gc()V+3
```

`0x1f` = `(uintptr_t)(-1) + 0x20`, i.e. `((JavaCallWrapper*)-1)->_anchor._last_Java_sp`. The GC
thread walking the target's stack hit the entry frame, read the call-wrapper pointer at
`rbp + 2*wordSize`, and dereferenced `+0x20`. This is the exact fatal error the design note at
`vmhook.hpp:14758-14762` was worried about, and it is caused by the header's own `-1` argument.

### 5.5 So why do calls "only work inside a detour"?

Two distinct requirements are being conflated:

**(a) You must be running on a real `JavaThread`.** Non-negotiable. `r15`/`Thread::current()` TLS,
`Threads_lock` + `ThreadsList` SMR registration, TLAB, stack guard pages, and a `java.lang.Thread`
mirror must all exist. A detour by definition runs on one.

**(b) The thread must be in `_thread_in_Java`, or you must put it there.** Inside a detour it
already is — so **no state manipulation is needed at all**, which is the safest possible case.
From a JNI-native context the thread is `_thread_in_native` and you must flip it.

Flipping it is where pure-VM hits its only genuine wall. The correct HotSpot sequence is
`_thread_in_native` → `_thread_in_native_trans` → fence → *poll safepoint/handshake* →
`_thread_in_vm` → `_thread_in_Java`, and the poll step requires `SafepointMechanism::process_if_requested`,
which is neither exported nor expressible through VMStructs (no `_poll_data`, no
`SafepointSynchronize::_state`). The harness used the blunt `_thread_state = _thread_in_Java` write
(same as `vmhook.hpp:15256`) and survived thousands of calls plus forced GCs — because once the
thread claims `_thread_in_Java`, the *interpreter's own* safepoint polls at method entry/return do
the right thing. But there is a **narrow, real race**: if a safepoint is armed in the instant
between the store and the first interpreter poll, or on the way back out
(`_thread_in_Java` → `_thread_in_native` without the trans state and fence), the VM can observe an
inconsistent thread. On JDK ≤ 12 you can *partially* detect this by `VirtualQuery`-ing
`os::_polling_page` for `PAGE_NOACCESS`; on JDK 13+ there is no signal.

**This is the honest answer to "why only in a detour": in a detour the transition is unnecessary,
so the one unsound step disappears.** Everything else — the wrapper, the anchor, the handle block —
is needed in both cases and was simply missing.

---

## 6. Avenue 5 — a `JNIEnv` obtained without JNI

### Is `JavaThread::_jni_environment` a VMStructs entry?

**No.** Measured absent on 8, 21 and 26 (the string `_jni_environment` does not occur in any of the
three tables). The neko-obfuscator project reached the same conclusion independently.

### But it is trivially derivable at runtime

`JNIEnv*` **is** `&thread->_jni_environment` — HotSpot's own
`JavaThread::thread_from_jni_environment(env)` does the pointer arithmetic. The offset was recovered
here by probing and validating against `_osthread->_thread_id`:

| JDK | `JNIEnv*` − `JavaThread*` | `sizeof(JavaThread)` |
|---|---|---|
| 8  | 608  | 1072 |
| 21 | 960  | 1632 |
| 26 | 1072 | 1952 |

Robust discovery in either direction (both used in the wild — async-profiler caches
`_env_offset = (intptr_t)env - (intptr_t)vm_thread`; neko-obfuscator scans `[0x100, 0x4000)` and
validates):

* **JavaThread → JNIEnv (pure VM)**: scan the `JavaThread` for a slot holding a pointer `P` such
  that `*(void**)P` lands in `jvm.dll`/`libjvm`'s data section and `((void**)*P)[4]` (i.e.
  `JNINativeInterface_::GetVersion`, after the four reserved NULL slots) lands in its text section.
  Cache the offset once per process.
* **JNIEnv → JavaThread**: probe backwards and validate `_osthread->_thread_id`.

### What a `JNIEnv` buys, and what it costs

With a `JNIEnv*` you can call the exported `JVM_*` C API — notably (verbatim from
`src/hotspot/share/include/jvm.h`, master):

```c
JNIEXPORT jobject JNICALL JVM_InvokeMethod(JNIEnv *env, jobject method, jobject obj, jobjectArray args0);
JNIEXPORT jobject JNICALL JVM_NewInstanceFromConstructor(JNIEnv *env, jobject c, jobjectArray args0);
JNIEXPORT jobject JNICALL JVM_Clone(JNIEnv *env, jobject obj);
JNIEXPORT jobject JNICALL JVM_NewArray(JNIEnv *env, jclass eltClass, jint length);
JNIEXPORT jclass  JNICALL JVM_DefineClass(JNIEnv *env, const char *name, jobject loader,
                                          const jbyte *buf, jsize len, jobject pd);
```

204–219 `JVM_*` names are exported on every measured `jvm.dll`, plus `JNI_CreateJavaVM`,
`JNI_GetCreatedJavaVMs`, `JNI_GetDefaultJavaVMInitArgs`.

**Assessment for this project: reject it as the primary mechanism, keep it as a documented escape
hatch.** Reasons:

1. It *is* JNI, semantically — `JVM_InvokeMethod`'s first parameter is a `JNIEnv*` and it takes
   `jobject` **handles**, not oops. To make a handle you need `JNIHandles::make_local`, which is not
   exported on Windows at all. So in practice you use `env->NewLocalRef`/`CallStaticObjectMethod`
   and you are back to plain JNI.
2. It costs a local-ref frame per call and drags in JNI exception state.
3. It is *strictly less capable* than the call stub: `JVM_InvokeMethod` goes through
   `java.lang.reflect.Method`, so it needs reflection objects, boxes every primitive, and cannot
   invoke non-public members without setAccessible plumbing.
4. The call stub route needs none of it and is proven working.

Where the derived `JNIEnv` *is* genuinely useful: as a **degradation tier** when the call-stub scan
fails on an exotic build, and for operations HotSpot really only exposes through JNI
(`DefineClass`, weak/global reference management).

### Related finding: `jmethodID` is no longer a `Method**` on JDK 25+

Measured on JDK 26: `GetStaticMethodID(...)` returned the literal value `3`. `jmethodID` is now an
index into a global `JmethodIDTable` (JDK-8268406 "Deallocate jmethodID native memory" family of
changes, JDK 25). Any code that does `Method* m = *(Method**)mid` — a very common trick — **crashes
on JDK 25+**. vmhook is immune because it resolves `Method*` from VMStructs directly; this is worth
recording so nobody "optimises" a jmethodID shortcut into the library later.

---

## 7. Avenue 6 — threads, and the "run this on the next JavaThread tick" executor

### Can an OS thread become a JavaThread by memory manipulation alone?

**No. Plainly, no.** `JNI attach_current_thread` (`src/hotspot/share/prims/jni.cpp`) does:

```c
JavaThread* thread = JavaThread::create_attaching_thread();
thread->set_thread_state(_thread_in_vm);
thread->record_stack_base_and_size();
thread->initialize_thread_current();          // sets the TLS Thread::current() slot
os::create_attached_thread(thread);
thread->stack_overflow_state()->create_stack_guard_pages();
thread->initialize_tlab();
thread->cache_global_variables();
{ MutexLocker ml(Threads_lock);
  thread->set_active_handles(JNIHandleBlock::allocate_block());
  Threads::add(thread, daemon); }              // ThreadsList / SMR registration
{ EXCEPTION_MARK; HandleMark hm(THREAD);
  thread->allocate_threadObj(...); }           // RUNS java.lang.Thread.<init>
```

Four independent blockers: the `THREAD_LOCAL Thread* _thr_current` slot and the `TlsAlloc`
key are internal and cannot be set for another thread; `Threads::add` must run under `Threads_lock`
with the hazard-pointer SMR protocol or the thread is invisible to safepoints (silent heap
corruption); `JNIHandleBlock::allocate_block` / `initialize_tlab` / `create_stack_guard_pages` are
internal functions, not data layouts; and `allocate_threadObj` *runs Java*, which requires the
thread to already be valid. Neither async-profiler nor neko-obfuscator attempts it — both operate
on threads the JVM already owns.

### The practical pattern: a detour-hosted executor

This is what the library should build.

```
             injected thread                          a real JavaThread
             ---------------                          -----------------
  submit(Method*, args, result_slot)  ──►  [ lock-free MPSC queue ]
      │                                              │
      │ wait on per-job event                        │  detour on a hot Java method fires
      │                                              │  (already _thread_in_Java, valid frames)
      │                                              ▼
      │                                        drain queue, for each job:
      │                                          pure-VM invoke() per §8
      │                                          store result / pending exception
      │                                          signal the job event
      ◄──────────────────────────────────────────────┘
```

Design notes:

* **Pump method choice.** It must be (i) frequently called, (ii) not on a latency-critical path,
  (iii) safe to add work to. The viewer already hooks `Runtime.getRuntime`. A better pump is a
  method called on a timer-ish cadence; failing that, install a dedicated pump hook on a method the
  target app calls in its main loop, and fall back to a low-frequency one otherwise.
* **Re-entrancy.** T8 proves nested invocation from a detour is safe (two stacked synthetic entry
  frames survived a full GC). Still, guard the drain with a per-thread "already draining" flag so a
  job that itself triggers the pump method does not recurse unboundedly.
* **Argument lifetime.** Arguments are **raw oops**. Between `submit()` and the drain, a GC can move
  them. Either (a) only submit primitives and oops obtained *inside* the same detour, or (b) park
  oops in a GC-visible root. The library's `jni::global_ref` is documented as not being a real GC
  root in the pure-VM build — that is the blocking correctness issue for cross-thread object
  arguments, and it is separate from invocation.
* **Timeout + cancellation.** If no JavaThread ticks (app idle, all threads parked), jobs must time
  out rather than hang the caller.
* **Never invoke from the injected thread directly**, even though the harness shows it "works" when
  that thread happens to be JNI-attached — an un-attached thread has no `JavaThread` and there is no
  legitimate pure-VM way to get one.

---

## 8. The safety protocol — implementer's checklist

For a call issued **from inside a detour** (the recommended path), steps marked ⚠ are the ones the
current header omits.

```
PRE
 0. Assert we are on a JavaThread:  thread != null && is_valid_pointer(thread).
    Obtain it from r15 / the detour context, NOT by attaching.
 1. Read thread->_thread_state.
      == _thread_in_Java (8)  -> no transition needed          [detour: always this]
      == _thread_in_native(4) -> see "NATIVE-CONTEXT" below
      anything else           -> refuse the call.
 2. Resolve the target's Method* and read Method::_from_interpreted_entry.
    Reject null / !is_valid_pointer.
 3. Build the parameter slot array:
      - instance: params[0] = raw uncompressed receiver oop
      - one slot per Z B C S I F, value zero/sign-extended
      - TWO slots per J D, value in the SECOND slot, first slot 0     ⚠ (currently one slot)
      - object args: raw uncompressed oop
      - size_of_parameters = SLOT COUNT, not argument count           ⚠
 4. ⚠ Build a JavaCallWrapper on the C++ stack (64 bytes; verify against
      gHotSpotVMTypes["JavaCallWrapper"].size and VMStructs
      JavaCallWrapper::_anchor offset == 32):
        [ 0] _thread        = thread
        [ 8] _handles       = thread->_active_handles          (must be non-null)
        [16] _callee_method = Method*
        [24] _receiver      = receiver oop, or null for static
        [32] _anchor        = COPY of thread->_anchor  (sp, pc, fp)
        [56] _result        = null
 5. ⚠ Clear the thread's anchor:
        thread->_anchor._last_Java_sp = _last_Java_pc = _last_Java_fp = 0
      (This is what JavaCallWrapper's constructor does. Skipping it makes the walker
       believe the new Java frames belong to the OLD anchor.)
 6. ⚠ Save thread->_active_handles->_top.
      (HotSpot's interpreter native-method entry ZEROES it; without this, calling a
       native Java method silently invalidates every JNI local ref held by the caller,
       and any handle the caller later reads is garbage.)

CALL
 7. call_stub(&wrapper, &result, result_type_int, method,
              from_interpreted_entry, params, slot_count, thread)
    - result_type is the BasicType int, read by the stub as 32 bits.
    - The stub is recovered per §2.3, validated by prologue bytes, cached process-wide.

POST
 8. Restore thread->_anchor from wrapper._anchor.
 9. Restore thread->_active_handles->_top.
10. ⚠ Check ThreadShadow::_pending_exception (absolute offset 8 in JavaThread).
      - non-null: surface it (read the exception oop's class name via the normal
        VMStructs klass path) and then WRITE NULL to clear it.                ⚠
      - Never return to the interpreter with a foreign pending exception set.
11. Decode *result per the return descriptor. T_OBJECT/T_ARRAY -> raw oop
    (already uncompressed; do NOT decode again).
12. Do NOT hold the returned oop across a possible GC without a real root.

NATIVE-CONTEXT (only if step 1 saw _thread_in_native; degraded, document the risk)
 N1. Write _thread_state = _thread_in_Java.
 N2. ... perform steps 4-9 ...
 N3. Write _thread_state back to the previous value.
     RISK: the _thread_in_native_trans / fence / safepoint-poll steps cannot be
     performed (no VMStructs access to _poll_data or SafepointSynchronize::_state on
     JDK 13+). A safepoint armed inside the window is a real, if narrow, race.
     On JDK <= 12 os::_polling_page IS exposed: VirtualQuery it and refuse the call
     while it reads PAGE_NOACCESS.

NEVER
 - Never pass -1 (or any non-JavaCallWrapper) as the stub's first argument.   ⚠ current bug
 - Never invoke on a thread that is not a JavaThread.
 - Never invoke while the thread is _thread_blocked / _thread_new / *_trans.
 - Never pass a compressed (narrow) oop in a parameter slot.
```

---

## 9. Version-dispatch table

| JDK | Mechanism | VMStructs entries required | Measured? | Fallback |
|---|---|---|---|---|
| **8** | Recover `_call_stub_entry` by backward prologue scan from `StubRoutines::_call_stub_return_address`; invoke with synthetic `JavaCallWrapper` | `StubRoutines::_call_stub_return_address`, `Method::_from_interpreted_entry`, `JavaThread::_thread_state`/`_anchor`, `JavaFrameAnchor::_last_Java_{sp,pc,fp}`, `Thread::_active_handles`, `JNIHandleBlock::_top`, `ThreadShadow::_pending_exception`, `JavaCallWrapper::_anchor`, int const `frame::entry_frame_call_wrapper_offset` | **YES — 8/8 tests PASS** | scan bound: `CodeCache::_heap`→`CodeHeap::_memory` (no `_low_bound`); then JNI |
| **11, 17** | identical | identical (`_active_handles` may be on `Thread` or `JavaThread` — probe both) | **[INFERRED]** — not installed here | as above |
| **21** | identical | + `CodeCache::_low_bound` available | **YES — 8/8 tests PASS** | Linux/macOS `.symtab` `_ZN12StubRoutines16_call_stub_entryE`; then JNI |
| **25** | identical | identical; ⚠ `jmethodID` is no longer `Method**` — resolve `Method*` via VMStructs only | **[INFERRED]** — 26 measured, 25 not installed | as above |
| **26** | identical | identical; `StubRoutines` VMStructs surface shrinks to the single `_call_stub_return_address` entry | **YES — 8/8 tests PASS** | as above |

Platform coverage:

| Platform | Status |
|---|---|
| Windows x64 | **measured on 8, 21, 26** |
| Linux x64 | **[INFERRED]** — same HotSpot-emitted `enter()` (`55 48 8B EC`); spill registers become SysV `rdi/rsi/rdx/rcx` so validation bytes differ; slot offsets identical. Additionally has the `.symtab` shortcut. |
| macOS x64 | **[INFERRED]** — as Linux |
| any aarch64 | **[NOT INVESTIGATED]** — different prologue and a different `entry_frame_call_wrapper_offset`; needs its own pattern before it can be claimed |

---

## 10. Recommended implementation plan for `vmhook.hpp`

### Step 1 — fix the resolver (small, high value)

Replace `detail::find_call_stub_entry()` with a tiered resolver, cached in a function-local static:

```
tier 0: VMStructs "StubRoutines::_call_stub_entry"        (free; never hits today, would win
                                                           if a vendor build ever adds it)
tier 1: VMStructs "StubRoutines::_call_stub_return_address"
        -> bound the scan below by CodeCache::_low_bound, else CodeHeap::_memory,
           else the OS region base
        -> walk back up to 64 KiB for  55 48 8B EC
        -> VALIDATE the four argument spills at +11..+26
             Win64 : 4C 89 4D 28 | 44 89 45 20 | 48 89 55 18 | 48 89 4D 10
             SysV  : 48 89 4D 28 | 89 55 20    | 48 89 75 18 | 48 89 7D 10
        -> require exactly ONE validated candidate; if 0 or >1, fail this tier
tier 2 (non-Windows only): ELF .symtab lookup of _ZN12StubRoutines16_call_stub_entryE
tier 3: unavailable -> method_proxy::call() returns monostate and logs, as today
```

All reads through `os::safe_read`, consistent with the project's no-SEH hardening policy.

### Step 2 — rewrite `method_proxy::call()`'s call site

Implement steps 3–11 of §8. Concretely, in `vmhook.hpp` around `15139-15270`:

* replace `std::intptr_t params[8]` with a slot-counting packer that emits two slots for `J`/`D`
  with the value in the high slot, and change the `static_assert` to bound **slots**;
* add the `java_call_wrapper` POD, sized/validated against `gHotSpotVMTypes` and
  `JavaCallWrapper::_anchor`, and pass `&wrapper` instead of `-1`;
* add anchor save/clear/restore around the stub call;
* add `_active_handles->_top` save/restore;
* replace the "cannot clear without JNI" comment block at `15296-15300` with a real
  `ThreadShadow::_pending_exception` read → surface → write-null.

### Step 3 — gate on thread state

Add a `detail::can_invoke_now(thread)` predicate. Return `true` only for `_thread_in_Java`. For
`_thread_in_native`, allow the flip **behind an explicit opt-in** (a `vmhook::config` flag or a
`call_unsafe_from_native()` overload), never by default, and document the race. This is the honest
encoding of "calls only work inside a detour".

### Step 4 — the executor

Build the detour-hosted MPSC queue of §7 as the public API for "call a method from my own thread".
Make `method_proxy::call()` on a non-JavaThread route through it rather than fail.

### Step 5 — runtime capability detection

Expose `vmhook::invocation_capability()` returning `{ unavailable, call_stub_scanned,
call_stub_vmstructs, call_stub_symtab }` plus the recovered address, so scripts and the viewer can
show *why* invocation is or is not available instead of a silent monostate.

### Degradation ladder

```
call_stub recovered + on a JavaThread + _thread_in_Java   -> full pure-VM invocation
call_stub recovered + on a JavaThread + _thread_in_native -> opt-in only, documented race
call_stub recovered + NOT on a JavaThread                 -> queue to the detour executor
call_stub NOT recovered                                   -> report unavailable (do not
                                                             silently re-introduce JNI)
```

---

## 11. What must be proven on a live JVM, and the cheapest test that proves it

Already proven here on 8 / 21 / 26 (Windows x64): stub recovery, the full ABI, receiver passing,
object arguments and returns, the 2-slot long layout, native-method entry + handle-block
clobbering, exception set/clear, GC across one and two stacked synthetic entry frames, and
invocation from a Java-reached native frame.

Still to prove:

| # | Claim | Cheapest test |
|---|---|---|
| 1 | Recovery works on JDK 11, 17, 25 | Run the same probe against those three JDKs. ~2 minutes each; the `.localci` JDK cache already has the download plumbing. **Do this first — it closes the whole version matrix.** |
| 2 | Recovery works on Linux x64 | Same probe, `dlopen`+`dlsym` instead of `LoadLibrary`, SysV validation bytes. One CI job. |
| 3 | Invocation is safe from a *real vmhook detour* (not a `RegisterNatives` stand-in) | Add one JVM test module: hook a fixture method, and inside the detour invoke a second fixture method that allocates enough to force a young GC, asserting the return value and that no `hs_err` is produced. This is the single highest-value test — it validates the intended production path. |
| 4 | Cross-thread argument oops survive the queue | Submit an object argument from the injected thread, force `System.gc()` before the drain, assert the callee sees the right object. Expected to **fail** until `global_ref` is a real GC root — which is exactly the point of running it. |
| 5 | No leak/corruption under load | 10⁵ invocations in a loop with `-Xcheck:jni` and `-XX:+UseSerialGC`/`+UseG1GC`/`+UseZGC`, asserting stable `JNIHandleBlock::_top` and no `hs_err`. |
| 6 | aarch64 | Out of scope until 1–5 are green; then port the prologue pattern. |

Cheapest single test that would have caught today's bug: assert at startup that
`find_call_stub_entry()` returns non-null on every JDK in the matrix. It currently returns null on
all of them, and nothing notices.

---

## 12. Prior art

| Project | Technique | Relevance |
|---|---|---|
| **async-profiler** | Reads `gHotSpotVMStructs` + parses ELF `.symtab` (not `dlsym`) for internals; caches `_env_offset = env - javathread`; brute-forces the pthread TLS key to find `Thread::current()`. Uses `_call_stub_return_address` **only** as a stack-walk boundary (`isEntryFrame`). Real calls go through ordinary JNI. | Confirms the VMStructs + symbol-table reading model; confirms `_call_stub_return_address` is the recognised entry-frame marker. |
| **AliceAPI** (`MsPantheum/AliceAPI`) | Resolves `_ZN12StubRoutines16_call_stub_entryE` via a hand-rolled symbol lookup **and** scans backwards from `_call_stub_return_address` for `55 48 89 e5` / `55 48 8b ec`. | Independent discovery of the exact recovery technique proven here. |
| **neko-obfuscator** (`Fadouse/neko-obfuscator`) | Zero JVMTI; VMStructs-driven i2i/c2i entry patching with manual anchor publication, thread-state transitions and safepoint polls; derives `JavaThread::_jni_environment` by offset scanning; reads `ThreadShadow::_pending_exception` instead of `ExceptionCheck`. Notes `_jni_environment` is *not* in stock `gHotSpotVMStructs` on JDK 21. | Independent corroboration of §5 and §6, including the "wrap the transition yourself" problem. |
| **HSDB / Serviceability Agent** | Same `gHotSpotVMStructs` metadata, but strictly out-of-process and read-only — "executes no code in the target process". | Explains *why* `_call_stub_entry` is absent from VMStructs but `_call_stub_return_address` is present. |
| **jattach** | Dynamic-attach socket protocol only; loads agents, never touches VM memory. | Not applicable to invocation. |
| **JOL, Byte Buddy agent** | In-process Java (`Unsafe`, `java.lang.instrument`). | Not applicable. |

No project was found that implements the *complete* protocol (recovered stub + synthetic
`JavaCallWrapper` + anchor + handle-block discipline). The harness described in §5.3 appears to be
the first end-to-end demonstration across JDK 8/21/26.

---

## 13. Reproduction

Scratchpad (not part of the repo):
`C:\Users\arno\AppData\Local\Temp\claude\C--repos-cpp-vmhook\2f3010a4-d978-4fe9-8ead-bff55b4d8b79\scratchpad\`

* `dump_vmstructs.cpp` — `LoadLibraryEx(jvm.dll)` + walk `gHotSpotVMStructs` (no VM created).
* `dump_all.cpp` — same for `gHotSpotVMTypes` / `gHotSpotVMIntConstants` / `gHotSpotVMLongConstants`.
* `probe3.cpp` — the full harness (T1–T8). Build:
  `g++ -O0 -g -std=c++17 -o p3.exe probe3.cpp -I"$JH/include" -I"$JH/include/win32"`,
  run with `PROBE_JVM=<path to jvm.dll>`; `PROBE_BAD_WRAPPER=1` reproduces the §5.4 crash;
  `PROBE_DUMP=1` prints the recovered stub bytes.
* `vms{8,21,26}.txt`, `all{8,21,26}.txt`, `exp{8,21,26}.txt` — the raw dumps quoted above.
