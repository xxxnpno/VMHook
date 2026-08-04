# VMStructs stub probe — is `StubRoutines::_call_stub_entry` really gone on JDK 21+?

Date: 2026-08-04
Scope: settle whether pure-VM Java method invocation via `StubRoutines::_call_stub_entry`
is recoverable, and what the correct lookup is.

Every claim below is tagged **MEASURED** (observed on a live JVM in this machine's
process, or read out of the shipped `jvm.dll`) or **INFERRED** (reasoned, not observed).

---

## 0. Headline verdict

**Verdict (b) — derivable, and cheaply, with runtime self-validation.**

Two findings, in order of importance:

1. **The premise on record is false.** `_call_stub_entry` is **not** "dropped by JDK 21+".
   It has **never** been in `gHotSpotVMStructs` on *any* JDK — not 8, not 21, not 26.
   OpenJDK's `vmStructs.cpp` has never contained a
   `static_field(StubRoutines, _call_stub_entry, address)` line, on jdk8u, jdk21u or
   jdk master. `vmhook::detail::find_call_stub_entry()` therefore returns `nullptr`
   on **every** supported JDK, and `method_proxy::call` has been dead on all of them —
   not just on 21+. **MEASURED** (live probe on 8 / 21 / 26) + **MEASURED**
   (OpenJDK source, three branches).

2. **What *is* published is `StubRoutines::_call_stub_return_address`, on all three
   JDKs, and `_call_stub_entry` lives immediately next to it in `jvm.dll`'s `.data`.**
   On a live JVM, the pointer stored adjacent to `&_call_stub_return_address` is the
   real call-stub entry — byte-verified: it starts with the exact
   `push rbp; mov rbp,rsp; sub rsp,N; mov [rbp+0x28],r9 …` call-stub prologue that
   vmhook's own comment documents, and the return address sits exactly two bytes past
   the `ff d2` (`call rdx`) that dispatches into Java. **MEASURED on all three JDKs.**

The adjacency *direction* is not stable (`+8` on JDK 8 and 21, `-8` on JDK 26), so the
offset must not be hardcoded. A version-independent rule that resolved correctly on all
three is given in §7.

---

## 1. Method

Two probes, both **live**, both reusing `vmhook.hpp` unmodified (read-only include, so
the table walking is byte-identical to what the library does at runtime).

Static PE parsing was considered and rejected as unnecessary: `gHotSpotVMStructs` is a
statically-initialised global, so simply `LoadLibraryA()`-ing a specific `jvm.dll` makes
the whole table readable — no injection, no target process. To also read the *values* of
static fields (which are only meaningful after `StubRoutines::initialize()` has run) the
probes additionally call `JNI_CreateJavaVM` through `GetProcAddress`, creating a real
in-process HotSpot VM. `JNI_CreateJavaVM` returned `rc=0` on all three JDKs.

This is strictly better evidence than DLL injection: same address space, same tables, no
injector variability, and the JVM is genuinely initialised.

| probe | what it does | source |
|---|---|---|
| `probe.cpp` | loads a given `jvm.dll`, optionally boots a JVM, dumps **every** entry of `gHotSpotVMStructs`, `gHotSpotVMTypes`, `gHotSpotVMIntConstants`, `gHotSpotVMLongConstants`, unfiltered | scratchpad |
| `probe2.cpp` | boots a JVM, reads `_call_stub_return_address`, dumps the `.data` neighbourhood, scans all writable sections of `jvm.dll` for code-cache pointers below it, hexdumps the candidate and the bytes before the return address | scratchpad |
| `probe3.cpp` | boots a JVM, walks the `JVMFlag`/`Flag` table, and walks the barrier-set → card-table chain end to end | scratchpad |

Compiler: MinGW g++ 15.2 (`-std=c++23`), x64. Export tables dumped with `objdump -p`.

### Artefact paths (raw evidence)

Scratchpad root:
`C:\Users\arno\AppData\Local\Temp\claude\C--repos-cpp-vmhook\2f3010a4-d978-4fe9-8ead-bff55b4d8b79\scratchpad\vmstructs_probe\`

| file | contents |
|---|---|
| `dump_jdk8_live.txt`, `dump_jdk21_live.txt`, `dump_jdk26_live.txt` | full 4-table dumps, live JVM |
| `dump_jdk8_static.txt`, `dump_jdk21_static.txt`, `dump_jdk26_static.txt` | same, JVM not booted (entry existence only) |
| `deriv_jdk8.txt`, `deriv_jdk21.txt`, `deriv_jdk26.txt` | derivation probe: neighbourhood, data scan, hexdumps |
| `sec_jdk8.txt`, `sec_jdk21.txt`, `sec_jdk26.txt` | flags / barrier-set / narrow-oop probe |
| `exports_jdk8.txt`, `exports_jdk21.txt`, `exports_jdk26.txt` | `jvm.dll` export tables |
| `probe.cpp`, `probe2.cpp`, `probe3.cpp` | probe sources |

JDKs used (paths verified):
* JDK 8 — `C:\Program Files\Eclipse Adoptium\jdk-8.0.492.9-hotspot\jre\bin\server\jvm.dll`
* JDK 21 — `C:\Program Files\Eclipse Adoptium\jdk-21.0.11.10-hotspot\bin\server\jvm.dll`
* JDK 26 — `C:\Program Files\Java\jdk-26.0.1\bin\server\jvm.dll`

Table sizes **MEASURED**: struct entries 771 / 813 / 572; type entries 678 / 801 / 325;
int constants 224 / 352 / 330; long constants 33 / 97 / 98 (JDK 8 / 21 / 26). Note the
sharp contraction in JDK 26 — VMStructs is being actively pruned, which is the real
version risk, not a 21-specific removal.

---

## 2. Raw evidence — every `Stub*` entry, verbatim

Format: `idx | typeName | fieldName | typeString | isStatic | offset | &field | *field`.
`*field` is only printed for statics; `-` for instance fields. Values are from the
**live** run.

### JDK 8 (8.0.492.9)

```
371	StubQueue	_stub_buffer	address	0	8	0000000000000000	-
372	StubQueue	_buffer_limit	int	0	20	0000000000000000	-
373	StubQueue	_queue_begin	int	0	24	0000000000000000	-
374	StubQueue	_queue_end	int	0	28	0000000000000000	-
375	StubQueue	_number_of_stubs	int	0	32	0000000000000000	-
379	StubRoutines	_verify_oop_count	jint	1	0	0000000075aa3e90	0x0000027f00000000
380	StubRoutines	_call_stub_return_address	address	1	0	0000000075aa3ea0	0x00000000028b061a
381	StubRoutines	_aescrypt_encryptBlock	address	1	0	0000000075aa3fd8	...
382	StubRoutines	_aescrypt_decryptBlock	address	1	0	0000000075aa3fe0	...
383	StubRoutines	_cipherBlockChaining_encryptAESCrypt	address	1	0	0000000075aa3fe8	...
384	StubRoutines	_cipherBlockChaining_decryptAESCrypt	address	1	0	0000000075aa3ff0	...
385	StubRoutines	_ghash_processBlocks	address	1	0	0000000075aa3ff8	...
386	StubRoutines	_updateBytesCRC32	address	1	0	0000000075aa4030	...
387	StubRoutines	_crc_table_adr	address	1	0	0000000075aa4038	...
388	StubRoutines	_multiplyToLen	address	1	0	0000000075aa4040	...
389	StubRoutines	_squareToLen	address	1	0	0000000075aa4048	...
390	StubRoutines	_mulAdd	address	1	0	0000000075aa4050	...
406	RuntimeStub	_caller_must_gc_arguments	bool	0	64	0000000000000000	-
--- gHotSpotVMTypes ---
177	StubQueue	(null)	0	0	0	48
178	StubRoutines	(null)	0	0	0	1
179	Stub	(null)	0	0	0	1
180	InterpreterCodelet	Stub	0	0	0	24
194	RuntimeStub	CodeBlob	0	0	0	72
620	StubQueue*	(null)	0	0	0	8
```

**12 `StubRoutines` fields. `_call_stub_entry` is not among them.**

### JDK 21 (21.0.11.10)

62 `StubRoutines` fields: `_verify_oop_count`, `_call_stub_return_address`, then the
crypto / CRC / BigInteger / transcendental / arraycopy families. Head of the list:

```
430	StubRoutines	_verify_oop_count	jint	1	0	00007ffd96a759b8	0x0000000000000000
431	StubRoutines	_call_stub_return_address	address	1	0	00007ffd96a759c8	0x0000000012c710e7
432	StubRoutines	_aescrypt_encryptBlock	address	1	0	00007ffd96a75ab8	...
...
491	StubRoutines	_generic_arraycopy	address	1	0	00007ffd96a75a80	...
--- gHotSpotVMTypes ---
115	StubQueue	(null)	0	0	0	48
116	StubRoutines	(null)	0	0	0	1
117	Stub	(null)	0	0	0	1
118	InterpreterCodelet	Stub	0	0	0	16
135	RuntimeStub	RuntimeBlob	0	0	0	96
678	StubQueue*	(null)	0	0	0	8
```

(full list in `dump_jdk21_live.txt`, indices 430-491)

**`_call_stub_entry` is not among them.**

### JDK 26 (26.0.1)

```
366	StubQueue	_stub_buffer	address	0	8	0000000000000000	-
367	StubQueue	_buffer_limit	int	0	20	0000000000000000	-
368	StubQueue	_queue_begin	int	0	24	0000000000000000	-
369	StubQueue	_queue_end	int	0	28	0000000000000000	-
370	StubQueue	_number_of_stubs	int	0	32	0000000000000000	-
374	StubRoutines	_call_stub_return_address	address	1	0	00007ffd96a79108	0x00000000132818e2
394	UpcallStub	_frame_data_offset	ByteSize	0	72	0000000000000000	-
560	UpcallStub::FrameData	jfa	JavaFrameAnchor	0	0	0000000000000000	-
--- gHotSpotVMTypes ---
118	StubQueue	(null)	0	0	0	48
119	StubRoutines	(null)	0	0	0	1
120	Stub	(null)	0	0	0	1
121	InterpreterCodelet	Stub	0	0	0	16
136	RuntimeStub	RuntimeBlob	0	0	0	64
138	UpcallStub	RuntimeBlob	0	0	0	80
232	StubQueue*	(null)	0	0	0	8
245	UpcallStub::FrameData	(null)	0	0	0	48
```

**JDK 26 publishes exactly ONE `StubRoutines` field: `_call_stub_return_address`.**
Even `_verify_oop_count` and the entire crypto/arraycopy family are gone. **MEASURED.**

---

## 3. OpenJDK source cross-check

`src/hotspot/share/runtime/vmStructs.cpp`, three branches, verbatim:

**jdk8u** (`hotspot/src/share/vm/runtime/vmStructs.cpp`) — all `StubRoutines` lines:
```cpp
static_field(StubRoutines,                _verify_oop_count,                             jint)
static_field(StubRoutines,                _call_stub_return_address,                     address)
static_field(StubRoutines,                _aescrypt_encryptBlock,                        address)
...
static_field(StubRoutines,                _mulAdd,                                       address)
```
`static_field(StubRoutines, _call_stub_entry, ...)`: **absent**.

**jdk21u** — 62 lines, beginning:
```cpp
static_field(StubRoutines,                _verify_oop_count,                             jint)
static_field(StubRoutines,                _call_stub_return_address,                     address)
```
`_call_stub_entry`: **absent**.

**jdk master** — only two lines survive:
```cpp
static_field(StubRoutines,                _call_stub_return_address,                     address)
static_field(StubRoutines,                _cont_returnBarrier,                           address)
```
`_call_stub_entry`: **absent**.
(`_cont_returnBarrier` is on master but **not** in the shipped 26.0.1 — **MEASURED**.)

**Conclusion: `_call_stub_entry` was never published. The "JDK 21 refactor dropped it"
story is a misdiagnosis of a lookup that never succeeded.**

`stubRoutines.hpp` (jdk21u) shows why the entry is nevertheless within reach — the two
statics are consecutive declarations:

```cpp
static jint    _verify_oop_count;
static address _verify_oop_subroutine_entry;

static address _call_stub_return_address;   // the return PC, when returning to a call stub
static address _call_stub_entry;
static address _forward_exception_entry;
static address _catch_exception_entry;
```
and the accessor is
```cpp
static CallStub call_stub() { return CAST_TO_FN_PTR(CallStub, _call_stub_entry); }
```

On jdk master these fields are no longer written out literally — they are generated by
the `STUBGEN_ENTRIES_DO()` / `DECLARE_ENTRY_FIELD` macro machinery introduced by the
JDK 24 stub-generation refactor, which is **INFERRED** to be why the declaration order
flipped on JDK 26 (see §7).

---

## 4. Answers to the specific questions (per JDK)

| question | JDK 8 | JDK 21 | JDK 26 |
|---|---|---|---|
| entry with fieldName `_call_stub_entry`? | **no** | **no** | **no** |
| `_call_stub_return_address` present? | **yes**, `StubRoutines` / `address` / static | **yes** | **yes** |
| `StubRoutines` in `gHotSpotVMTypes`? | **yes** (size 1, AllStatic) | **yes** | **yes** |
| # `StubRoutines` fields published | 12 | 62 | **1** |
| `AbstractInterpreter::_code` (`StubQueue*`, static) | yes, live `0x74f1f0` | yes, live `0x31ed3b0` | yes, live `0x324bd80` |
| `Method::_i2i_entry` | yes, off 48 | yes, off 56 | yes, off 56 |
| `Method::_from_interpreted_entry` | yes, off 80 | yes, off 80 | yes, off 80 |
| `Method::_from_compiled_entry` | yes, off 64 | yes, off 64 | yes, off 64 |

All **MEASURED**.

**Is there a sibling that plainly serves the same role?** No. There is no
`_initial_stubs`, no `_shared_runtime_stubs`, no stubs-blob type, no `StubCodeDesc`, and
nothing pointing at the initial-stubs `BufferBlob` in any of the three tables. Searching
all four tables case-insensitively for `call_stub` / `callstub` returns exactly one hit
per JDK: `_call_stub_return_address`. **MEASURED.**

**Could the entry be derived from `_call_stub_return_address`?** Yes — see §5-§7. This
is the whole answer.

**Are the exported symbols an alternative?** **No. MEASURED.**
`jvm.dll` exports 2998 / 4385 / 4295 symbols (JDK 8 / 21 / 26). Occurrences of
`StubRoutines` in the export table: **0, 0, 0**. Occurrences of `call_stub`: **0, 0, 0**.
`JavaCalls`: **0, 0, 0**. `SharedRuntime`: **0, 0, 0**. The exports are almost entirely
C++ vtables (`??_7…@@6B@`) plus the JNI/JVM C entry points; the only HotSpot-internal
data exported is the 24-symbol `gHotSpotVM*` family. There is no export-table route to
the call stub, and no export-table route to `JavaCalls::call_helper` either.

**Interpreter entry points:** confirmed still published on all three (row above), which
is expected since vmhook's hooking already depends on them. `AbstractInterpreter::_code`
resolves to a live non-null `StubQueue*` on all three, so the interpreter codelet queue
is walkable on 26 as well.

---

## 5. The derivation, measured

Live values of the published field (**MEASURED**):

| JDK | `&StubRoutines::_call_stub_return_address` | value (RA) |
|---|---|---|
| 8  | `0x0000000075aa3ea0` | `0x00000000028b061a` |
| 21 | `0x00007ffd96a759c8` | `0x0000000012c710e7` |
| 26 | `0x00007ffd96a79108` | `0x00000000132818e2` |

Neighbourhood of that slot (values that fall inside the code cache), and the distance
below RA:

**JDK 8** — code cache `[0x28b0000, 0x2b70000)`
```
slot    addr                value               in_cc   RA-value
 -1  0000000075aa3e98  0000000002901fb0        YES     -334230
 +0  0000000075aa3ea0  00000000028b061a        YES           0   <- _call_stub_return_address
 +1  0000000075aa3ea8  00000000028b0567        YES         179   <- _call_stub_entry
 +2  0000000075aa3eb0  00000000028b0520        YES         250   <- _forward_exception_entry
 +3  0000000075aa3eb8  00000000028b06cb        YES        -177   <- _catch_exception_entry
```

**JDK 21** — code cache `[0xb820000, 0x1a820000)`
```
 +0  00007ffd96a759c8  0000000012c710e7        YES           0   <- _call_stub_return_address
 +1  00007ffd96a759d0  0000000012c70f53        YES         404   <- _call_stub_entry
 +2  00007ffd96a759d8  0000000012c70f00        YES         487   <- _forward_exception_entry
 +3  00007ffd96a759e0  0000000012c7128f        YES        -424   <- _catch_exception_entry
```

**JDK 26** — code cache `[0xbe20000, 0x1ae30000)` — **note the flip to `-8`**
```
 -1  00007ffd96a79100  0000000013281833        YES         175   <- _call_stub_entry
 +0  00007ffd96a79108  00000000132818e2        YES           0   <- _call_stub_return_address
 +1  00007ffd96a79110  00000000132817e0        YES         258   <- _forward_exception_entry
 +2  00007ffd96a79118  00000000132819b0        YES        -206   <- _catch_exception_entry
```

### Byte-level proof that the identified pointer is the call stub

Bytes at the candidate — **JDK 8** `0x28b0567` and **JDK 21** `0x12c70f53`:

```
55 48 8b ec              push rbp ; mov rbp,rsp
48 81 ec d8 00 00 00     sub  rsp,0xd8            (0x1d8 on JDK 21)
4c 89 4d 28              mov  [rbp+0x28],r9       <- Method*
44 89 45 20              mov  [rbp+0x20],r8d      <- BasicType
48 89 55 18              mov  [rbp+0x18],rdx      <- result holder
48 89 4d 10              mov  [rbp+0x10],rcx      <- link
```

This is **exactly** the Windows-x64 call-stub prologue documented in the comment above
`vmhook::detail::find_call_stub_entry()` (`rcx`=link, `rdx`=result, `r8`=BasicType,
`r9`=Method*). **JDK 26** at `0x13281833` is byte-identical (visible in the hexdump at
`0x13281830`: `… 00 ff e3 | 55 48 8b ec 48 81 ec d8 00 00 00 4c 89 4d 28 …` — the
`ff e3` is the `jmp rbx` tail of the preceding stub).

The last 16 bytes before RA, identical on all three JDKs:
```
50 75 f4                 push rax ; jne …          (parameter copy loop)
48 8b 5d 28              mov  rbx,[rbp+0x28]       <- Method*
48 8b 55 30              mov  rdx,[rbp+0x30]       <- entry_point
4c 8b ec                 mov  r13,rsp
ff d2                    call rdx                  <- dispatch into Java
<RA>                     mov  rcx,[rbp+0x18] …     <- result handling
```

So RA is precisely the instruction after `call rdx` inside the call stub, and the
candidate is the stub's first instruction. **This is `StubRoutines::_call_stub_entry`.
MEASURED on all three JDKs.**

Also **MEASURED**: `0x132817e0` (JDK 26 `+1`) begins `48 8b 0c 24` = `mov rcx,[rsp]`,
which is the `movptr(c_rarg0, Address(rsp, 0))` opening of
`generate_forward_exception()` — corroborating the slot identification above.

### Whole-image corroboration

Independently of adjacency, `probe2` scanned **every** writable section of `jvm.dll` for
pointer-sized values inside the code cache and strictly below RA, then ranked them. The
**maximum** such value was the call-stub entry on all three JDKs:

| JDK | rank-0 value | RA − value | slot offset from `&_call_stub_return_address` | total candidates |
|---|---|---|---|---|
| 8  | `0x028b0567` | 179 | **+8** | 8 |
| 21 | `0x12c70f53` | 404 | **+8** | 9 |
| 26 | `0x13281833` | 175 | **−8** | 9 |

Two independent methods agree on all three JDKs. **MEASURED.**

---

## 6. Why the delta is not a constant

`RA − _call_stub_entry` = 179 (JDK 8), 404 (JDK 21), 175 (JDK 26). That is the size of
the call-stub prologue, which varies with register-save code (JDK 21's build uses
EVEX `62 …` AVX-512 saves; JDK 8/26 use `c5 fa 7f` AVX). **Never hardcode the delta.**
**MEASURED.**

---

## 7. Verdict (b) — derivable. Implementation sketch.

Replace the single failing lookup in `find_call_stub_entry()` with:

```
1. e := iterate_struct_entries("StubRoutines", "_call_stub_return_address")
   if !e || !e->address            -> give up (never observed to fail on 8/21/26)
   RA := *(uintptr_t*)e->address
   if !RA                          -> VM not fully initialised yet; retry later

2. code-cache bounds:
      lo := *CodeCache::_low_bound, hi := *CodeCache::_high_bound        (JDK 21/26)
   fallback JDK 8:
      heap := *CodeCache::_heap
      vs   := heap + CodeHeap::_memory.offset
      lo   := *(vs + VirtualSpace::_low.offset)
      hi   := *(vs + VirtualSpace::_high.offset)
   (all four lookups MEASURED present on the respective versions)

3. candidates, in order:
      c1 := *(uintptr_t*)((char*)e->address + 8)     // JDK 8, 21 layout
      c2 := *(uintptr_t*)((char*)e->address - 8)     // JDK 26 layout
   accept the first c that passes validate(); if neither does, fall back to
   scanning jvm.dll's writable sections for max{ p : lo <= p < RA } (a few
   hundred thousand 8-byte reads, one-off, cached in a function-local static).

4. validate(c):
      lo <= c < RA                                   // inside code cache, before RA
      RA - c < 4096                                  // same stub
      memcmp(c, "\x55\x48\x8B\xEC", 4) == 0          // push rbp; mov rbp,rsp
      memcmp((char*)RA - 2, "\xFF\xD2", 2) == 0      // call rdx (win64 c_rarg1)
   all reads through vmhook::os::safe_read.
```

Roughly 60-80 lines, all additive, inside the existing
`vmhook::detail::find_call_stub_entry()`; nothing else in `method_proxy::call` changes,
and the existing "give up" branch stays as the final fallback.

**Risk assessment.**

* Low, and *self-limiting*: the validation is a positive proof, not a guess. If the
  prologue signature ever stops matching, the function returns `nullptr` and behaviour
  is exactly what it is today. It cannot mis-dispatch into a wrong address.
* Architecture-bound: the `55 48 8B EC` / `FF D2` signatures are x86-64. On SysV
  (Linux/macOS) `c_rarg1` is `rsi`, so the call is `FF D6`; on aarch64 the whole
  signature differs. But `method_proxy::call` already hardcodes the Windows-x64 call-stub
  ABI (`rcx`/`rdx`/`r8`/`r9` + stack), so this adds no *new* portability debt — gate the
  signature check per architecture and let the max-below-RA scan carry the rest.
  **INFERRED** for non-Windows; only Windows x64 was measured.
* Version risk is on the **published** field, not the derivation:
  `_call_stub_return_address` is one of only **two** `StubRoutines` fields left on jdk
  master, and it survives precisely because `frame.cpp`'s
  `StubRoutines::returns_to_call_stub()` and SA's frame walker need it. It is the most
  durable thing in that struct. **INFERRED.**
* The ±8 adjacency is *not* relied on for correctness — it is only a fast path. The
  data-scan fallback is layout-independent.

**Why not (a):** there is no alternative published name to look up. This is not a rename.
**Why not (c):** hand-rolling an `_i2i_entry` call (building an interpreter frame,
setting `JavaThread::_anchor`, `_thread_state`, the last-Java-sp/fp/pc, and the
`_do_not_unlock_if_synchronized` bookkeeping, then arranging a safe return path) is
weeks of work and is exactly what the call stub exists to do correctly. It is the right
answer only if the derivation above ever stops validating — at which point the cheapest
safe path is *not* a hand-rolled interpreter entry either, but re-adding a narrowly
scoped JNI `CallXxxMethodA` shim used only inside a detour on a real `JavaThread`
(which the viewer payload already proves works on JDK 26).

**One correction to the code comments that must land with the fix:** the comment in
`method_proxy::call` ("On modern JDKs (21+) `StubRoutines::_call_stub_entry` is often
missing from VMStructs") is wrong on both counts — it is missing on *all* JDKs, and it
is not a modern-JDK regression.

---

# Secondary section — GC relocation, write barriers, oop width

Requested alongside the main question; same dumps, same live runs.

## S1. Relocation / GC-progress detection

| item | JDK 8 | JDK 21 | JDK 26 |
|---|---|---|---|
| `Universe::_collectedHeap` | **yes** — `CollectedHeap*`, static | **yes** | **yes** |
| `CollectedHeap::_total_collections` | **yes** — `unsigned int`, instance, **off 56** | **yes**, **off 64** | **yes**, **off 72** |
| `CollectedHeap::_total_full_collections` | **NO** | **NO** | **NO** |
| `SafepointSynchronize::_safepoint_counter` | **NO** — no `SafepointSynchronize` entries at all | **NO** | **NO** |
| gc-active flag | `CollectedHeap::_is_gc_active` (bool, off 48) | `_is_stw_gc_active` (bool, off 48) | `_is_stw_gc_active` (bool, off 56) |

All **MEASURED**. Live read-through confirmed working on all three
(`Universe::_collectedHeap` → `+_total_collections` → `0` on a freshly booted VM).

Other collection-ish fields found: none usable as an epoch. `G1CollectedHeap` publishes
`_summary_bytes_used`, `_hrm`, `_old_set`, `_humongous_set` (and on 26 the `G1Heap*`-
renamed types) but no GC counter. `ParallelScavengeHeap` / `SerialHeap` /
`GenCollectedHeap` publish only generation pointers.

**Verdict S1: reachable on all three, but only via one counter.** The relocation detector
must be built on `Universe::_collectedHeap` + `CollectedHeap::_total_collections`.
There is no full-GC counter and no safepoint counter — so the detector sees "a collection
happened", not "a safepoint happened", and cannot distinguish young from full.
Recommended lookup order (identical on all three; only the offset differs, which
VMStructs supplies):

```
find_first_of({ {"CollectedHeap","_total_collections"} })            // same on 8/21/26
gc-active: find_first_of({ {"CollectedHeap","_is_stw_gc_active"},    // 21, 26
                           {"CollectedHeap","_is_gc_active"} })      // 8
```

**INFERRED caveat:** `_total_collections` is incremented by the *VM thread* at a
safepoint. Reading it from a detour on a Java thread is a benign racy `unsigned` read;
the usual "read, work, re-read, retry if changed" pattern applies.

## S2. Write-barrier / card-table data

The spelling changed wholesale between 8 and 21 — on JDK 8 the barrier set **is** the
card table (`CardTableModRefBS`); on 21/26 they are separate objects.

| item | JDK 8 | JDK 21 | JDK 26 |
|---|---|---|---|
| reach the barrier set | `CollectedHeap::_barrier_set` (instance, **off 40**) | **static** `BarrierSet::_barrier_set` | **static** `BarrierSet::_barrier_set` |
| barrier-set kind tag | `BarrierSet::_kind` (`BarrierSet::Name`, off 12) | `BarrierSet::_fake_rtti` (off 8) → `FakeRtti::_concrete_tag` (off 8) | same as 21 |
| card table object | *is* the barrier set | `CardTableBarrierSet::_card_table` (off 72) | `CardTableBarrierSet::_card_table` (**off 64**) |
| `byte_map_base` | `CardTableModRefBS::byte_map_base` — **no leading underscore**, off 144 | `CardTable::_byte_map_base` off 48 | `CardTable::_byte_map_base` off 48 |
| `_byte_map` | `CardTableModRefBS::_byte_map` off 64 | `CardTable::_byte_map` off 40 | `CardTable::_byte_map` off 40 |
| `_whole_heap` | `CardTableModRefBS::_whole_heap` off 16 | `CardTable::_whole_heap` off 8 | `CardTable::_whole_heap` off 8 |
| `_guard_region` | `CardTableModRefBS::_guard_region` off 96 | `CardTable::_guard_region` off 88 | **ABSENT** |
| card shift | `CardTableModRefBS::card_shift` = **9** (int const) | **ABSENT** | **ABSENT** |
| card size | `CardTableModRefBS::card_size` = **512** | **ABSENT** | **ABSENT** |
| dirty card value | `CardTableModRefBS::dirty_card` = **0** | `CardTable::dirty_card` = **0** | `CardTable::dirty_card` = **0** |
| clean card value | `CardTableModRefBS::clean_card` = **-1** | `CardTable::clean_card` = **255** | `CardTable::clean_card` = **255** |
| G1 extras | `JavaThread::_dirty_card_queue`, `CardTableRS::youngergen_card`=17 | `G1CardTable::g1_young_gen`=2, `BarrierSet::CardTableBarrierSet`=1 | `BarrierSet::CardTableBarrierSet`=0 |

All **MEASURED**. Live end-to-end walk succeeded on all three:

```
JDK 8  : heap 0x711ec0 -> BarrierSet 0x726ae0 -> byte_map_base 0x0ee28000
JDK 21 : BarrierSet 0x3242100 -> CardTable 0x68c640 -> _byte_map_base 0x190e8000
JDK 26 : BarrierSet 0x326e480 -> CardTable 0x326d6f0 -> _byte_map_base 0x195a8000
```

**Verdict S2: reachable on all three, but the card *shift* is not.**
`CardTable::card_shift` / `card_size` exist as int constants **only on JDK 8**. On 21/26
you must hardcode `card_shift = 9` (`card_size = 512`) — **INFERRED** to be stable
(HotSpot has used a 512-byte card on all 64-bit platforms for the entire history of the
card table; JDK 26 does still expose `G1HeapRegion`/`HeapRegion` grain constants but not
the card shift). Sanity-check it at runtime instead of trusting it:
`(_byte_map_base + (heap_base >> 9)) == _byte_map` must hold, since
`_byte_map_base = _byte_map - (whole_heap.start >> card_shift)`. That equation
*measures* the shift — try 9, verify, and only then use it.

Exact lookup order a `find_first_of({...})` helper should use:

```
barrier set : { {"BarrierSet","_barrier_set"}            /* static, 21/26 */ }
              else CollectedHeap::_barrier_set off Universe::_collectedHeap   /* 8 */
kind tag    : { {"BarrierSet","_fake_rtti"} -> {"BarrierSet::FakeRtti","_concrete_tag"},   /* 21/26 */
                {"BarrierSet","_kind"} }                                                  /* 8 */
card table  : { {"CardTableBarrierSet","_card_table"} }  /* 21/26; on 8 the BS is it */
byte map bas: { {"CardTable","_byte_map_base"},          /* 21/26 */
                {"CardTableModRefBS","byte_map_base"} }  /* 8 — NO leading underscore */
byte map    : { {"CardTable","_byte_map"}, {"CardTableModRefBS","_byte_map"} }
whole heap  : { {"CardTable","_whole_heap"}, {"CardTableModRefBS","_whole_heap"} }
dirty value : int const { "CardTable::dirty_card", "CardTableModRefBS::dirty_card" }  == 0 everywhere
```

**Hard prerequisite (MEASURED):** ZGC and Shenandoah have **no card table** —
`ZCollectedHeap` is published on 21/26 and `XCollectedHeap` on 21. A hand-written card
dirty must therefore be gated on the barrier-set kind actually being
`CardTableBarrierSet`, read from the `_fake_rtti._concrete_tag` / `_kind` tag, **not**
assumed. Note the tag *value* for `CardTableBarrierSet` differs per version:
`BarrierSet::CardTableBarrierSet` = **1** on JDK 21 but **0** on JDK 26 (and JDK 8 uses
`BarrierSet::CardTableModRef` = 1) — read the constant from
`gHotSpotVMIntConstants`, never hardcode it.

## S3. Oop-width detection

**The header's unconditional "compressed oops" assumption is genuinely a bug, and it is
fixable — but not by the fields the request listed.**

| item | JDK 8 | JDK 21 | JDK 26 |
|---|---|---|---|
| `heapOopSize` (int const) | **ABSENT** | **ABSENT** | **ABSENT** |
| `oopSize` (int const) | **8** | **8** | **8** — always word size, tells you nothing |
| narrow-oop base spelling | `Universe::_narrow_oop._base` | `CompressedOops::_narrow_oop._base` | **`CompressedOops::_base`** |
| narrow-oop shift spelling | `Universe::_narrow_oop._shift` | `CompressedOops::_narrow_oop._shift` | **`CompressedOops::_shift`** |
| implicit-null-checks flag | `Universe::_narrow_oop._use_implicit_null_checks` | `CompressedOops::_narrow_oop._use_...` | `CompressedOops::_use_implicit_null_checks` |
| narrow-klass base/shift | `Universe::_narrow_klass._base/_shift` | `CompressedKlassPointers::_narrow_klass._base/_shift` | `CompressedKlassPointers::_base/_shift` |

**Three different spellings across three JDKs. MEASURED.** Live values on this machine:
base `0`, shift `3` on all three.

As the request anticipated, `_base`/`_shift` exist whether or not compression is on, so
they cannot answer the question (`shift==0 && base==0` is ambiguous between
"compressed, unscaled heap" and "not compressed").

**The answer is the JVM flag table, and it works on all three. MEASURED.**
`gHotSpotVMStructs` publishes the whole flag array:

| | JDK 8 | JDK 21 | JDK 26 |
|---|---|---|---|
| type | `Flag` | `JVMFlag` | `JVMFlag` |
| array / count | `Flag::flags`, `Flag::numFlags` (static) | `JVMFlag::flags`, `JVMFlag::numFlags` | same |
| record size (from `gHotSpotVMTypes`) | 32 | 24 | 24 |
| `_name` offset | 8 | 8 | 8 |
| `_addr` offset | 16 | 0 | 0 |
| `_type` offset / kind | 0, `const char*` (e.g. `"bool"`) | 20, `int` enum | 20, `int` enum |
| flags observed | 1442 | 1236 | 1175 |

Live readout (probe3):

```
JDK 8  : UseCompressedOops=1  UseCompressedClassPointers=1  ObjectAlignmentInBytes=8  UseParallelGC=1
JDK 21 : UseCompressedOops=1  UseCompressedClassPointers=1  ObjectAlignmentInBytes=8  UseG1GC=1
JDK 26 : UseCompressedOops=1  UseCompressedClassPointers=1  ObjectAlignmentInBytes=8  UseG1GC=1
         UseCompactObjectHeaders=0
```

**Verdict S3: reachable on all three.** Lookup order:

```
narrow oop base : find_first_of({ {"CompressedOops","_base"},              /* 26+   */
                                  {"CompressedOops","_narrow_oop._base"},  /* 21-25 */
                                  {"Universe","_narrow_oop._base"} })      /* 8-14  */
narrow oop shift: find_first_of({ {"CompressedOops","_shift"},
                                  {"CompressedOops","_narrow_oop._shift"},
                                  {"Universe","_narrow_oop._shift"} })
narrow klass    : same three-way, s/CompressedOops/CompressedKlassPointers/
                  and s/_narrow_oop/_narrow_klass/
is-compressed   : flag table -> "UseCompressedOops"
                  table type : find_first_of({ {"JVMFlag","flags"}, {"Flag","flags"} })
                  stride     : iterate_type_entries("JVMFlag"|"Flag")->size
                  offsets    : _name / _addr from VMStructs (they DIFFER: addr is at 16
                               on JDK 8 but 0 on 21/26 — never hardcode)
heapOopSize     : NOT published; derive = UseCompressedOops ? 4 : 8
```

Read bool flags as a single byte at `_addr`; **do not** read 8 bytes (probe3's
`as_int64` column shows adjacent flag bytes bleeding in). For `intx`/`int` flags like
`ObjectAlignmentInBytes` the width differs by version — dispatch on `_type`.

**Bonus finding (MEASURED), relevant to the same bug:** JDK 26 publishes
`markWord::klass_shift = 42` and `markWord::hash_shift = 11` in
`gHotSpotVMLongConstants` — the Lilliput compact-header layout. `UseCompactObjectHeaders`
was **0** on this JDK 26 run, but when it is on, the klass pointer lives **in the mark
word**, not at `oopDesc::_metadata._compressed_klass`. Any klass decode must check that
flag on 26+.

---

## 8. Summary table of what to change

| # | change | file | size |
|---|---|---|---|
| 1 | `find_call_stub_entry()` — derive from `_call_stub_return_address` with byte validation (§7) | `vmhook.hpp` ~14764 | ~70 lines |
| 2 | fix the false "JDK 21+ dropped it" comments | `vmhook.hpp` ~14764, ~15060 | comments |
| 3 | oop-width: read `UseCompressedOops` from the flag table; three-way narrow-oop spelling | `vmhook.hpp` | ~50 lines |
| 4 | GC-progress detector on `Universe::_collectedHeap` + `_total_collections` | new | ~30 lines |
| 5 | card-table walk with barrier-kind gate + shift self-verification | new | ~60 lines |
