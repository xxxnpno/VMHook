# Changelog

All notable changes to this project are documented in this file.
The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and the project follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Fixed
- **Deoptimisation stopped working entirely on modern JVMs, because
  `AdapterHandlerEntry` is no longer exported.**  Pointing a method's
  `_from_compiled_entry` at the c2i adapter is how compiled callers get routed
  into the interpreter, and therefore into a hook.  vmhook found that address
  through `Method::_adapter` -> `AdapterHandlerEntry::_c2i_entry`.  Measured on a
  JDK 26-era VM: `Method::_adapter`, `AdapterHandlerEntry::_c2i_entry`,
  `_i2c_entry` and `_c2i_unverified_entry` are **all absent** from
  gHotSpotVMStructs.  With nothing to validate a candidate against, the
  heuristic scan returned 0, `get_adapter()` returned null, `hook()` fell back to
  a forced deopt that leaves `_from_compiled_entry` aimed at a stale nmethod, and
  `deoptimize_methods_if` skipped every method it saw (*"0 deoptimised, 5715
  skipped"*).  New `vmhook::find_shared_c2i_entry()` derives the same address
  from facts that ARE exported, using a property of HotSpot's own adapter
  library: adapters are **shared, keyed on the signature**
  (`AdapterHandlerLibrary::get_adapter` looks them up by an `AdapterFingerPrint`
  over argument BasicTypes plus the receiver), and a method that is *not*
  currently compiled has `_from_compiled_entry` pointing straight at its
  adapter's c2i entry.  So it borrows the entry from any interpreted method with
  the same descriptor and static-ness.  Matching on the exact descriptor is
  deliberately stricter than HotSpot's own fingerprint — identical descriptor and
  static-ness always implies the same fingerprint, so a match can never be wrong,
  only conservative.  Cached per signature; `resolve_c2i_entry()` tries the
  classic route first, so JDK 8 is unchanged.

  The donor must be **concrete and non-native**.  An ABSTRACT method also has
  `_code == nullptr`, so it passes the "is it interpreted" test and looks like an
  ideal donor — but HotSpot hands every abstract method the shared
  `_abstract_method_handler`, whose c2i entry *is* the AbstractMethodError stub.
  Borrowing it points the hooked method straight at a throw, and did: on
  Minecraft 26.2 the hook fired correctly and the game then died every time with
  `java.lang.AbstractMethodError: Receiver class net.minecraft.client.Minecraft
  does not define or inherit an implementation of the resolved method 'private
  void runTick(boolean)'` at the first compiled call site.  Abstract and native
  donors are now excluded, and because a wrong address here is a dead JVM rather
  than a failed lookup, up to four independent donors are sampled and a majority
  must agree before the address is used — a lone dissenter refuses the
  derivation and falls back to the previous (safe) forced-deopt path.

- **`NO_COMPILE` stopped working on JDK 24+, and the hook silently died with
  it.**  Until JDK 23 the compile-control bits lived in `Method::_access_flags`,
  so `*flags |= NO_COMPILE` inhibited the JIT.  JDK-8339113 shrank `AccessFlags`
  to `u2` and RELOCATED those bits into `MethodFlags::_status` — the same `u4`
  word that already holds `_dont_inline`.  The `u4` OR then landed its high byte
  in the alignment padding after the `u2` `_access_flags`: it corrupted nothing,
  and it also did nothing.  A hooked method was free to be recompiled, and the
  symptom in the field was the auto-repair watchdog reporting *"JIT-state
  drifted … NO_COMPILE=set"* — the flag really was set, in a word HotSpot had
  stopped reading.  This was known and documented in-source as an accepted
  no-op; it is now fixed.  New `hotspot::set_not_compilable()` writes
  `is_not_c2_compilable` (1<<8), `is_not_c1_compilable` (1<<9) and
  `is_not_c2_osr_compilable` (1<<10) into `MethodFlags::_status`, through the
  SAME resolver, alignment gate, cold-page probe and atomic RMW that
  `set_dont_inline()` already uses on that word.  The bit positions are read
  from `methodFlags.hpp` and verified identical on jdk-21 and master, and are
  corroborated by that table placing `_dont_inline` at bit 12 — the bit
  `derive_method_flags_layout()` derives *independently* from
  `_intrinsic_id_offset - 4`.  No-op on JDK 8..20, where the legacy
  `_access_flags` mask still owns those bits.
- **A hook installed on an already-inlined method silently never fired, and
  fixing it was the caller's problem.**  `hook()` deoptimises its target, but a
  caller compiled with that target *already inlined* has no call site left to
  intercept — the exact case when injecting into a running process.  Callers had
  to know about `deoptimize_all_jit_compiled_methods()` and remember to call it.
  They no longer do: the first successful `hook()` flushes JIT-compiled callers
  once per process.  Once-only by design — the sweep is the most expensive
  operation in the library, and the problem it solves is a property of code
  compiled *before* hooking began.  `set_auto_deoptimize_callers(false)` opts
  out for callers who would rather have no stutter at install time than a hook
  that fires on already-hot code.

  **Scoped to the hooked method's own class, not the whole VM.**  The first
  version swept every loaded method; on a live JDK 26 VM that deoptimised 5715
  nmethods in one pass and the JVM died about twenty seconds later — the profile
  of the code-cache sweeper reclaiming an nmethod a frame was still executing
  in.  Clearing `Method::_code` is not HotSpot's own `make_not_entrant()`: it
  consults no on-stack frames, no inline-cache references and no dependency
  lists, so one method is a small risk and thousands is a large one.  Inlining
  happens in the *caller*, and a caller that inlined a method is overwhelmingly
  likely to be a sibling in the same class (`Minecraft.run()` inlining
  `Minecraft.runTick()` is the case that motivated this), so a couple of hundred
  methods instead of several thousand is the same fix with a fraction of the
  exposure.  `deoptimize_all_jit_compiled_methods()` keeps the VM-wide behaviour
  and now documents that hazard.

- **CRITICAL**: `method_proxy::call()` never worked, on any JDK.  It resolved the
  call stub by looking up `StubRoutines::_call_stub_entry` in VMStructs — an entry
  HotSpot has never published, on any version.  The in-source claim that "JDK 21+
  dropped it" was wrong: it was equally absent on JDK 8.  `find_call_stub_entry()`
  therefore returned `nullptr` everywhere, and once the JNI fallback was removed,
  every Java method call became a silent no-op.  Measured on live JDK 8/21/26 by
  two independent probes; a before/after control resolves `0x0` on all three at
  the previous commit.  The entry is now derived from
  `StubRoutines::_call_stub_return_address` (which *is* published on all three)
  through four tiers — VMStructs, `.data` adjacency in **both** directions (`+8`
  on 8/21, `−8` on 26), a ranked data scan, and a prologue scan below the return
  address — each candidate positively validated by bounds, the `enter()` prologue
  bytes and the `FF D2` dispatch before the return address.  Neither the adjacency
  direction nor the return-address distance (179/404/175 bytes) is hardcoded.
  Non-x86-64 degrades to `nullptr`, and failure is deliberately not cached so a
  call before `StubRoutines::initialize()` cannot disable invocation for the
  process lifetime.
- **CRITICAL**: the call stub was passed `link = -1` where the VM expects a
  `JavaCallWrapper*`.  HotSpot's frame walker dereferences it, so a GC that walked
  the entry frame read `((JavaCallWrapper*)-1)->_anchor` — a negative control
  crashed reading address `0x1f`.  `call()` now builds a real synthetic
  `JavaCallWrapper`, trusting its sibling offsets only when the two
  VMStructs-published facts (type size 64, `_anchor` at 32) corroborate them, and
  reporting invocation unavailable rather than guessing when they do not.
- `long` and `double` arguments occupied one call-stub slot; they occupy **two**,
  with the value in the **high** slot.  Slot layout is now driven off the callee's
  descriptor rather than the C++ argument type, so an `int` passed to a `J`
  parameter (or a `float` to a `D`) is widened correctly.
- Invoking a `native` Java method zeroes `_active_handles->_top`, silently
  invalidating the caller's local references.  It is now saved and restored.
- A Java exception thrown by the callee was left pending on the `JavaThread` with
  no signal to the caller, which received decoded garbage.  `call()` now reads,
  classifies and clears `ThreadShadow::_pending_exception` — this does **not**
  require JNI, contrary to the comment that claimed it did — and reports the throw
  through `value_t::threw()` / `exception_class` while returning a
  value-initialised result.
- Removed three probes for `Method::_from_compiled_code_entry_point`; the field is
  `_from_compiled_entry` on every measured JDK, so the first lookup was dead work.

### Added
- **Inferred access: `get` / `set` / `call` never take a type argument, and a
  reference never has to be borrowed by hand.**  The JVM already stores every
  field's descriptor, so making the caller repeat it was only ever a way for a
  read to disagree with the field it reads.  What the caller *declares* now
  decides the shape a reference arrives in:

  ```cpp
  float health = self->get_field("health")->get();          // was get<float>()
  std::string n = self->get_field("name")->get();           // was .as_string()
  auto rider    = self->get_field("passenger")->get();      // was .to_borrowed<entity>()
  float x       = rider->get_field("posX")->get();          // no wrapper type at all

  entity                   w = self->get_field("passenger")->get();
  vmhook::borrowed<entity> b = self->get_field("passenger")->get();
  vmhook::ref<entity>      r = self->get_field("passenger")->get();
  ```

  Concretely, on both `field_proxy::value_t` and `method_proxy::value_t`:
  - conversion arms for `borrowed<W>`, `ref<W>` and a wrapper `W`.  Each of those
    targets **already compiled** — and silently produced a default-constructed
    empty handle / null wrapper, because the conversion fell through to
    `target_type{}`.  That is the failure mode this closes: code that reads
    correctly and is quietly null.  All three now share one decode-and-vet step
    (`reference_target`) with the existing `unique_ptr` arm, so a Java null, an
    array descriptor, an address failing the heap-range check, or a proven
    cross-klass mismatch yields an EMPTY result rather than a handle to
    reinterpreted bits.
  - `operator->` / `operator*`, binding a `vmhook::any_object` for one
    expression, so a reference can be navigated with no type named anywhere.
- `vmhook::any_object` — a Java object with no C++ type attached.  Fields and
  methods resolve through the klass read out of the **live object's header**, so
  the whole hierarchy of whatever the object actually is stays reachable with no
  `register_class<T>()` and no wrapper declaration.  A null instance degrades to
  `nullopt` / empty exactly as a typed wrapper on a null oop does, which is what
  lets `a->call("b")->call("c")` end quietly at the first missing link instead of
  needing a check between every hop.  Exposes `get_field`, `get_method` (by name
  or by name + descriptor), `call`, `class_name`, `instance_of`.
- `borrowed<>`, `ref<>` and `root<>` — the UNTYPED forms — now have `operator->`
  / `operator*`, binding `any_object`.  They were dead ends before: a caller who
  had one and wanted a single field had to go and declare a wrapper type first.
  Revalidation is unchanged, so an expired handle binds a null `any_object`
  rather than the address it used to have.
- `field_proxy::set()` now accepts anything that names a live Java object — a
  `borrowed<W>`, a `ref<W>`, a wrapper, an `any_object`, or the value another
  `get()` / `call()` produced — so storing a reference is `set(other)` rather
  than a hand-written `store_object` / raw oop.  Each source is resolved through
  ITS OWN validity rule first (a borrow revalidates its epoch, a ref re-walks
  its anchor, a value decodes and vets), so an expired or empty source stores
  nothing instead of writing a stale address into the heap — the one write that
  would outlive the mistake and corrupt a later collection.
- **`vmhook::make_unique<T>(args...)` now runs the real Java constructor.**  It
  allocated a zeroed instance and ran the *C++* wrapper's `construct()`, but
  never executed the Java `<init>` chain — so a class with any constructor logic
  came back half-built, and the function's own documentation called this "a
  minimal implementation".  It now selects the `<init>` overload from the C++
  argument types (the same way `get_method(name)->call(args...)` selects one),
  invokes it, and returns a never-null `std::unique_ptr<T>` whose
  `get_instance()` is null when the object could not be built.  Nothing has to
  be added to a wrapper: the oop constructor and `register_class<T>()` every
  wrapper already has are enough.  The optional `construct(args...)` hook is
  preserved and now runs *after* the Java constructor.  Every refusal —
  unregistered type, unloaded class, non-instantiable klass, no matching
  `<init>`, more than the 8 arguments the interpreter's locals[] array holds, no
  derivable call stub, not on a JavaThread — is detected BEFORE anything is
  allocated, so a failure cannot leave a raw constructor-less object on the Java
  heap, and a constructor that throws abandons the instance.
- `return_value::set_arg(0, "/hello")` — a STRING LITERAL now builds a
  `java.lang.String`.  `set_arg` classified its argument with `remove_cvref_t`,
  which leaves a literal as `char[N]`; that matched none of the string arms, so
  it fell through to the trivially-copyable arm and wrote the array's first
  bytes into the interpreter slot.  It now decays, so a literal takes the same
  path a `const char*` variable always did.
- ~~`object<T>::create(args...)`~~ — folded into `make_unique` above rather than
  shipped as a second spelling for the same thing.  Originally added as:
  selecting the `<init>` overload from the C++ argument types, and returning the
  same `std::unique_ptr<T>` every other object in the API is spelled with.  This
  is the gap `vmhook::make_unique` left: make_unique allocates a zeroed instance
  and runs the *C++* wrapper's `construct()`, but never executes the Java
  `<init>` chain, so any class with constructor logic came back half-built.
  Every refusal — unregistered type, unloaded class, non-instantiable klass, no
  matching `<init>`, no derivable call stub, not on a JavaThread — is detected
  BEFORE the allocation, so a failure cannot leave a raw constructor-less object
  on the Java heap; a constructor that throws abandons the instance.
- **`std::unique_ptr<T>` is now the only object type a user writes**, in all six
  positions: read from a field, returned from a call, passed as a call argument,
  stored by `set()`, produced by `create()`, and received as a hook parameter.
  Five of those already worked; `create()` was returning a `vmhook::borrowed`
  and was the one place the handle vocabulary leaked into user code.  Pinned by
  static_assert so a future change cannot reopen the gap.
- **A `std::unique_ptr` handed out by vmhook is never null.**  The POINTER is
  always valid and the OBJECT inside it is absent when the Java reference was
  null or could not be decoded, so `p->` is always safe to write and
  `p->get_instance() == nullptr` is the single question to ask.  Previously both
  cases collapsed into a null pointer, which made "there was no object" and "the
  read was refused" indistinguishable and put a null test in front of every
  access.  Applies to all four producing paths (field read, call result, detour
  argument, `create()`), each of which keeps its existing refusals — array
  descriptor, failed heap-range check, proven cross-klass mismatch — and now
  expresses them as an instance-less wrapper.  The detour path additionally
  stops routing through `g_type_factory_map`: the wrapper type is known
  statically there, and the factory route silently produced a null argument for
  a wrapper that was never `register_class<>`'d — a registration mistake that
  looked exactly like a Java null at the call site.
- `vmhook::vm_capabilities()` — a cached capability gate reporting the live
  collector, barrier shape, `UseCompressedOops` and `UseCompactObjectHeaders`.
  The collector is determined by walking the JVM flag table (`Flag` on JDK 8,
  `JVMFlag` on 11+), taking the record stride from `gHotSpotVMTypes` rather than
  `sizeof` (the `_doc` member exists only in non-product builds) and branching on
  the exported `typeString` of `_type` rather than on any JDK version.  The flag
  table is the only reliable source for `UseCompressedOops`: a zero narrow-oop
  base and shift is genuinely ambiguous between "off" and "on with an unscaled
  sub-4GB heap", and `heapOopSize` is exported nowhere.
- `vmhook::gc_epoch()` / `gc_epoch_changed()` — a relocation detector sampling
  `CollectedHeap::_total_collections` together with the STW-GC-active flag
  (`_is_gc_active` before JDK 21, `_is_stw_gc_active` from 21).  Measured at
  ~0.32 µs per call.

### Changed
- **The README documented an API that did not exist.**  `get_field("x")->get<T>()`
  and `self->call<T>("m")` were never spellings this library had — neither
  compiles, and never did.  The README is rewritten around the shape the library
  is actually for — you describe a Java class once as a C++ wrapper, `get_field`
  / `get_method` live INSIDE it, and callers see only your own accessors and
  `std::unique_ptr` — with one code box per operation and one per hook feature,
  modelled on the real consumer (npnoqol).  Every snippet in it is compiled under
  `-Werror` on GCC and `/W4 /WX /permissive-` on MSVC.  The same two fictions
  appeared in the header's own `java_thread_scope` examples and are fixed there
  too.
- **Two further README claims were wrong and are removed.**  "Deoptimise a hot
  method before hooking it, or the hook silently never fires" — `hook()` has
  always deoptimised its own target on install (it clears `_code`, repoints
  `_from_compiled_entry` at the c2i adapter and `_from_interpreted_entry` at the
  i2i stub) and holds `NO_COMPILE`; the only case it cannot reach is a caller
  that already inlined the target.  And `verify_hooks()` was documented as
  something a user calls — the auto-repair watchdog has always been spawned on
  the first successful install and re-verifies every second until shutdown, so
  it is not part of the user-facing surface.
- `get_field` / `get_method` are stated as the ONE access spelling: each resolves
  a static or an instance Java member indistinguishably, so `static_field` /
  `static_method` are only for the no-instance case.  The header's wrapper-class
  usage example is removed rather than corrected — the README is where examples
  belong.  Also documented plainly, because it is a language limit and not a
  choice: from a *static C++ method*, GCC and Clang >= 20 select the non-static
  candidate on argument match before checking object availability and hard-error,
  so those toolchains need the `static_*` spelling there.
- **`vmhook::jni::global_ref` no longer returns a stale address.**  It records the
  GC epoch at capture; `oop()`, `handle()` and `operator bool` now report empty
  once a relocating collection has occurred, and `is_stale()` exposes that
  directly.  Everything fails closed — an unreadable epoch, unresolved offsets, an
  unsupported collector or a pause in flight all yield "stale" rather than an
  address the library cannot vouch for.  `raw_unsafe()` returns the verbatim
  capture for diagnostics only.
  **This is still not a GC root**: it does not keep the object alive.  It only
  stops you dereferencing one that moved.
  Verified on live JVMs across **15 collector × JDK configurations** (Serial,
  Parallel, G1, ZGC, Shenandoah, Epsilon on JDK 8/21/26) — on every relocating run
  the object physically moved and the reference reported stale; ZGC and Shenandoah
  are refused outright, because their counters advance at cycle start, before
  relocation, which would make the detector itself unsound there.

> **Note:** the `Fixed` entries below this point describe `call_jni`, `jni_value`,
> `write_jni_arg_to_slot` and other JNI-fallback code that the de-JNI effort has
> since **removed entirely**.  They are retained as history; they do not describe
> any code that still exists.

- **CRITICAL**: `method_proxy::call_jni`'s argument diagnostic dump dereferenced
  `values[i].l` for EVERY argument, but `jni_value` is a union — for a primitive
  argument (e.g. `jint 1`) `.l` aliases the primitive bits (`0x1`), which is
  non-null but NOT a pointer.  `*reinterpret_cast<void**>(0x1)` took an access
  violation that crashed the whole JVM on the FIRST primitive-argument call
  through `call_jni` — i.e. every method call with a primitive arg on JDKs where
  the call stub is gone (21+).  The dump now dereferences only a synthetic object
  handle (`values[i].l == &handle_storage[i]`) and prints raw bytes otherwise.
  Same union-aliasing class as the DeleteLocalRef fix below; this one was masked
  because the integration suite's CI treated a crash (empty results, 0 FAIL) as a
  vacuous pass until the suite gained a required `TOTAL:` completion line.
- `write_jni_arg_to_slot` / `append_jni_arg` now clear the full 8-byte
  `jni_value` union cell (`value.j = 0`) before writing the active member,
  instead of relying on `value = jni_value{}`.  Value-initialising a union only
  guarantees the first member (`bool z`) plus padding are zeroed; the upper 7
  bytes were left unspecified and differed by compiler (MinGW zeroed them, Clang
  did not), so a narrow primitive (bool/int/float) could leave stale high bits in
  the slot.  Harmless for the call itself (the A-variant JNI calls read only the
  active member) but made the packing non-deterministic across platforms.
- **CRITICAL**: `method_proxy::call()` (the call-stub fast path) silently dropped
  every `Ljava/lang/String;` return.  On JDKs where `StubRoutines::_call_stub_entry`
  IS exposed (typically JDK 8/11/17), a String-returning `call()` fell into the
  reference-return `default:` arm, which did `static_cast<std::uint32_t>(result_holder)`
  — truncating the 64-bit oop AND mislabelling an *uncompressed* oop as the
  compressed-oop that `value_t`'s uint32 alternative is documented to hold.  The
  net effect: `get_method("toString")->call()` returned `""` on JDK 8/11/17 while
  the JNI fallback path (JDK 21+) returned the real text — the same wrapper line
  silently broke on one JDK and worked on another.  Now the reference-return arm
  decodes `java.lang.String` straight to UTF-8 (parity with `call_jni`) and
  re-encodes any other reference to a real compressed oop so `value_t` round-trips
  through `decode_oop_pointer` instead of truncating; a null oop returns monostate.
- `method_proxy::is_static()` always returned `false` — the constructor's
  `static_field` member is never wired to any caller, so the accessor reported
  garbage for every method.  It now reads `JVM_ACC_STATIC` (0x0008) from the live
  Method's `_access_flags` (the low byte is stable across every supported JDK),
  falling back to the stored member only when the flags slot can't be resolved.
- `on_class_loaded()` and `on_exception()` returned a watch_handle whose
  `running()` reported `true` even when the underlying hook never armed (e.g. no
  JVM in-process, or the method couldn't be resolved) — a caller had no way to
  tell a working watcher from a dead one.  They now return an inert
  `watch_handle{}` (`running() == false`) and drop the optimistically-registered
  callback when the install fails, matching `watch_static_field`'s contract.
- **CRITICAL**: `method_proxy::call_jni` and `detail::jni_make_unique` could
  call `DeleteLocalRef` on a garbage pointer for any non-zero primitive
  argument.  `vmhook::detail::jni_value` is a `union`, so a primitive store
  (`value.j = jlong`, `value.i = jint`, `value.z = true`, `value.f`, `value.d`)
  aliases the `.l` (jobject) member.  Both arg-cleanup paths decided "is this
  slot a JNI local ref that needs releasing?" by reading `.l` back — the
  stack path compared `.l != &handle_storage[i]`, the heap path range-checked
  `.l` against the `object_handles` vector.  For a primitive arg such as
  `int64_t{0x4242'4242'4242'4242}`, `.l` reads back as that bit pattern:
  non-null, outside every storage range, so it was handed to `DeleteLocalRef`
  as a wild jobject (→ `-Xcheck:jni` "Invalid local ref" warnings, internal
  assertions on fastdebug HotSpot, or an access violation).  Replaced the
  union read-back with an explicit per-slot release tag set by
  `write_jni_arg_to_slot` / `append_jni_arg` ONLY for jstrings produced by
  `NewStringUTF`.  `make_jni_args` now threads a `std::vector<char>` tag
  array; `call_jni` threads a `bool[]`.  Object handles and primitives are
  never released.
- `field_proxy::set` now also rejects writing a non-primitive C++ value
  (`std::string` / `std::string_view` / `const char*` / `std::vector<T>` /
  `std::unique_ptr<wrapper>`) into a *primitive* JVM field.  Previously the
  type-based dispatch fired the string / array / OOP branch regardless of the
  field's actual signature, so `set(std::string{"42"})` on an `"I"` field
  reinterpreted the int's 4 bytes as a compressed OOP and forwarded the
  decoded (wild) address into `write_java_string`.  The new guard mirrors the
  size guard below: if `jvm_primitive_byte_width(signature) != 0`, refuse with
  a diagnostic.  The diagnostic now also reports the field address and
  static/instance flag.
- `field_proxy::set` silently corrupted adjacent fields when the C++ value
  type was wider than the JVM field.  Writing `int64_t{...}` to an `"I"`
  field memcpy'd 8 bytes into a 4-byte slot, trampling the next 4 bytes
  of the object layout (whatever field came next in the class).  Added a
  `vmhook::detail::jvm_primitive_byte_width` helper that returns the
  JVM-spec width of each primitive signature, and `set()` now rejects
  the write with a diagnostic when the value size doesn't match.  The
  `'C' + 1-byte char` widening shortcut is preserved.
- `vmhook::for_each_thread` Path 1 had no cycle detection in the
  intrusive `Threads::_thread_list` walk.  A corrupted list (e.g. a JVMTI
  agent stitching `_next` during `RedefineClasses`) could form a cycle;
  with only the hard cap at 4096 entries, the visitor was invoked on the
  same JavaThread up to 4096 times.  Now tracks visited pointers in a
  small `unordered_set` and breaks out at the first repeat.
- Injector silently ignored the user-provided PID when more than one JVM
  was running.  The flow parsed `argv[1]` into `target_pid` and then
  unconditionally scanned for JVM processes, overriding the user's
  choice on a single-match scan and refusing to inject on multi-match.
  Now: if a PID is provided, skip the scan entirely.
- Injector's `wstr_to_str` truncated non-ASCII characters via
  `static_cast<char>(wchar_t)`.  Paths under user directories with
  accented characters (very common on non-English Windows installs)
  rendered as garbage in the diagnostic log.  Replaced with a proper
  `WideCharToMultiByte` UTF-8 conversion.
- Injector's `resolve_dll_path` didn't check `GetModuleFileNameW`'s
  return value.  On failure (returns 0) or truncation (returns MAX_PATH
  with no null terminator) the buffer's tail was indeterminate; the
  injector then constructed a bogus path and only failed at the
  `std::filesystem::exists` check rather than logging the underlying
  cause.  Now: return an empty path so the "vmhook.dll not found"
  diagnostic fires immediately and clearly.
- `watch_static_field` dr-slot use-after-free.  The VEH handler used to read
  the slot's `std::function` callback under the mutex, release the mutex,
  then call it - racing with `watch_handle::stop()` which clears the slot's
  callback under the same mutex.  In the lose-the-race window the VEH
  invoked an empty `std::function` (throws `bad_function_call`) or, worse,
  was mid-call when the destructor destructed the stored lambda's captures
  (use-after-free on any heap-held capture).  Fix: copy the `std::function`
  AND the address under the lock and call the local copy outside.
- VEH handler leak in the hardware-data-breakpoint path.  Previously
  `ensure_dr_handler_installed` called `AddVectoredExceptionHandler` once
  and never removed it - dropping the last `watch_handle` left the
  handler in the kernel's dispatch list for the rest of the process
  lifetime, costing one extra dispatch per future exception forever.
  Replaced with a refcounted `dr_arm_one` / `dr_unarm_one` pair so the
  handler is uninstalled on the 1 -> 0 transition (the watch_handle
  destructor's on_stop now invokes the unarm).
- `method_proxy::call_jni` leaked one JNI local reference per string-
  argument per call (jstring handles from `NewStringUTF` were never
  `DeleteLocalRef`'d) AND one local reference per `L/[`-returning call
  (the result handle).  Hot-path tight loops would eventually trip
  "JNI local reference table overflow" warnings and lose the receiver
  identity.  Added a RAII `string_handle_cleanup` struct that runs at
  scope exit and releases every `values[i].l` that isn't the synthetic
  `&handle_storage[i]` stack indirection; the result-handle release
  is inline in the `'L'/'['` case after the value has been extracted.
- `vmhook::detail::jni_make_unique` leaked 1 + N local refs per call
  (the `NewObjectA` result handle and every string-typed constructor
  arg).  Same fix pattern as `call_jni`: a scope-exit cleanup that
  walks `values[]`, distinguishes synthetic handles by checking the
  pointer against `object_handles.data()`'s range, and releases the
  rest plus the result handle.
- `vmhook::detail::jni_find_class_with_context_loader` leaked 4-8
  local refs per call (`thread_class`, `current_thread`,
  `context_loader`, `name_string`, `class_mirror`,
  `class_loader_class`, `system_loader`, `launch_class`,
  `launch_loader`).  The result was cached by upstream `find_class`,
  so the absolute leak was bounded by distinct class names - but
  detour threads that look up many classes (Minecraft + Forge + Lunar
  mods) eventually fill the local-ref table.  Centralised release
  through a small RAII `local_ref_bag` whose destructor walks every
  tracked handle.
- `return_value::set_arg(index, ...)`: enforce the JVM spec's `max_locals`
  upper bound (u2 = 65535).  Previously only `index < 0` was rejected, so a
  caller passing e.g. `index = 1'000'000` would write to `locals[-1000000]`,
  walking off the interpreter local-variable array into adjacent thread
  state (operand stack, saved registers, frame header) and producing a
  post-uninject crash cascade in the JVM.
- `return_value::set_arg(index, std::string)` and the `const char*` overload
  used to leak one JNI local reference per call: the jstring handle returned
  by `NewStringUTF` was never `DeleteLocalRef`'d after the underlying OOP
  was stored in the interpreter slot.  Long-lived attached threads (every
  HotSpot interpreter thread that runs our detour) would eventually trip
  JNI "local reference table overflow" warnings on hot-path string
  injection.  A new `vmhook::detail::jni_delete_local_ref(handle)` helper
  now releases the handle after the store, with cleanup on both the
  success and failure paths.
- `midi2i_hook` constructor now re-validates `chain_resume` with
  `is_valid_pointer` before baking it into the trampoline's resume JMP.
  `vmhook::hook<T>()` already filters at the call site, but direct
  consumers of `midi2i_hook` (anyone using the lower-level API) bypassed
  that guard; a bad pointer here would have caused the trampoline to
  resume at an arbitrary address.  Bad input now falls through to the
  default `target + HOOK_SIZE` resume.
- `iterate_struct_entries` / `iterate_type_entries` now guard against null
  arguments and null `field_name` mid-table.  The standard HotSpot terminator
  zeroes both `type_name` and `field_name`, but custom JVMs / JVMTI agents
  have been observed publishing partial entries where `type_name` is set and
  `field_name` is null; the previous code crashed on `strcmp(nullptr, x)`
  the first time iteration walked past such an entry.
- `vmhook::os::release(addr, 0)` is now a no-op instead of calling
  `munmap(addr, 0)` (which returns `EINVAL` on Linux).  Aligns POSIX
  behaviour with Windows, where `VirtualFree` already tolerates a zero
  size for `MEM_RELEASE`.

### Added
- Multi-classloader class resolution — `vmhook::find_class_via_oop(anchor_oop,
  name)`, `vmhook::override_class_lookup(name, klass)`,
  `vmhook::evict_class_lookup(name)`, and the
  `vmhook::reanchor_classes_via_oop(anchor_oop, {names...})` convenience.  The
  existing `find_class()` resolves a class by NAME across the whole
  ClassLoaderDataGraph and returns the first match; when a process loads two
  copies of a class under different loaders (a custom launcher loader, OSGi, app
  servers, modded games — the NPNOQOL deep-dive flagged Lunar/Forge shipping
  duplicate `net.kyori.*` / `com.mojang.*` classes), "first by name" is
  graph-iteration-order-dependent and routinely resolves the WRONG copy, with the
  only symptom a `ClassCastException` thrown deep in host code when the result is
  handed back.  `find_class_via_oop` walks `anchor -> getClass -> getClassLoader
  -> loadClass(name)` to force the copy visible from an object you already hold;
  `override_class_lookup` seeds that copy into the `find_class` cache so the whole
  SDK transparently follows it (the supported replacement for reaching into the
  internal cache); `reanchor_classes_via_oop` does both for a set of names and
  returns true only when all resolve (so callers can poll until their anchor is
  live).  Covered by a new standalone `classloader_reanchor` suite (null / no-JVM
  safety, and override/evict cache round-trips verified directly against
  `klass_lookup_cache`).
- Method enumeration / descriptor-based hooking — `vmhook::get_class_methods<T>()`
  / `get_class_methods("internal/Name")` return every declared method of a class
  as `(name, JVM-descriptor)` pairs by walking `InstanceKlass::_methods` directly
  (no JNI, any thread once loaded).  `vmhook::find_methods_by_signature<T>(desc)`
  returns the names of all methods whose descriptor matches, and
  `vmhook::hook_by_signature<T>(desc, detour)` hooks the single method selected by
  descriptor alone — refusing to guess if more than one matches.
  `vmhook::log_class_methods<T>()` is the debug-log convenience.  These close a
  real obfuscated-build gap surfaced by the NPNOQOL deep-dive: hook resolution was
  name-keyed, but obfuscated / mixin method names rotate per build while the
  descriptor stays stable — so "hook the method whose descriptor is S" is the
  operation you actually need, and it requires being able to list a class's
  methods (which the library previously only did privately inside `hook<T>`, and
  only via `VMHOOK_LOG`, which is compiled out in release).  Covered by a new
  standalone `method_enumeration` suite and a `test_method_enumeration`
  JVM-integration scenario that reads `vmhook/A`'s real methods and resolves
  `(I)I` back to `protectedAdd`.
- `vmhook::jni::global_ref` + `vmhook::pin()` — the missing GC-pin lifetime
  primitive.  Every other handle in the library (`oop_t`, `object_base`, wrapper
  `unique_ptr`, `method_proxy::call()` results) is valid only for the duration of
  the current hook invocation: HotSpot relocates objects on every collecting GC,
  so an address captured this tick dangles the moment a GC runs.  That makes the
  ubiquitous "compute Java objects on one thread/tick, consume them on another"
  pattern a use-after-relocation by construction — a gap the downstream NPNOQOL
  fork had to fill itself before it could cache method results across ticks or
  publish Java objects into cross-thread snapshots.  `global_ref` is a move-only
  RAII pin over `NewGlobalRef`/`DeleteGlobalRef` (JNI slots 21/22) whose `oop()`
  re-derives the object's CURRENT (post-relocation) address every call; `pin(oop)`
  and `pin(unique_ptr<wrapper>)` are one-liner factories.  Added
  `vmhook::detail::jni_new_global_ref` / `jni_delete_global_ref`.  Covered by a
  new standalone `global_ref` unit suite (move-only / null-safety / no-JVM-inert
  contract) and a `test_global_ref` JVM-integration scenario that pins a freshly
  allocated object, drops every other reference, forces `System.gc()`, and proves
  the field survives and the pin tracks relocation.
- `method_proxy::value_t` can now convert to `std::unique_ptr<wrapper>` and to
  `std::string`, mirroring `field_proxy::value_t`.  Previously an Object-returning
  Java method assigned into a `std::unique_ptr<my_wrapper>` silently yielded
  `nullptr`, and a String-returning method via the call-stub path yielded `""` —
  the exact method-vs-field parity gap the user flagged.  The conversion operator
  decodes the compressed-oop alternative into the wrapper (with klass validity
  check) and decodes String returns to UTF-8.  Added `value_t::is_void()`,
  `value_t::is_string()`, and `value_t::as_string()` — the last names the string
  extraction directly so it works on MSVC, where `std::string s = call()` /
  `static_cast<std::string>` are ambiguous (the templated conversion operator can
  also yield `const char*`, which std::string constructs from).
- 16 new standalone (no-JVM) unit-test executables, run by the full CI matrix on
  every OS/compiler: `field_proxy_value_conversions`, `field_proxy_set_guards`,
  `method_proxy_value_t`, `jni_arg_packing`, `signature_parsing`,
  `decode_oop_and_pointers`, `decode_u5`, `iterate_entries_safety`,
  `os_release_and_protect_edges`, `array_element_helpers`, `version_macros`,
  `platform_capability_macros`, `traits_extra`, `api_surface_extended`,
  `logging_format`, `collection_type_tags`.  They exercise the no-JVM-testable
  surface of each feature from every angle — value_t conversions, the union
  release-tag regression guard, signature/descriptor parsing, compressed-oop and
  is_valid_pointer boundaries, UNSIGNED5 decoding, never-throw collection
  conversions, compile-time platform/capability macro invariants, and null-safety
  of every public entry point when no JVM is loaded.
- `method_proxy::raw_method()` — returns the underlying HotSpot `method*`,
  mirroring `field_proxy::raw_address()`.  Closes a method-vs-field parity
  gap: advanced consumers driving low-level HotSpot APIs (custom trampolines,
  deopt sweeps) can now reach the `Method*` through the public API instead of
  only via `vmhook::hook<T>()`.
- `field_proxy::is_reference()` and `method_proxy::is_reference()` — `true`
  when the field's / method-return's JVM descriptor is a reference or array
  (`L` / `[`).  Lets callers gate `get_compressed_oop()` (which only makes
  sense for reference types) without hand-parsing `signature()[0]`.
- `vmhook::detail::jvm_primitive_byte_width(signature)` — JVM-spec byte
  width of a primitive type descriptor (Z/B=1, S/C=2, I/F=4, J/D=8;
  references / arrays / void return 0).  Used by `field_proxy::set`'s
  new size-mismatch guard.
- `vmhook::detail::dr_arm_one` / `dr_unarm_one` — refcounted VEH lifecycle
  helpers for the hardware-data-breakpoint path.  `ensure_dr_handler_installed`
  is now a thin alias that calls `dr_arm_one()`; consumers don't need to
  change.  See the watch_handle change above.
- `vmhook::detail::jni_delete_local_ref(handle)` — releases a JNI local
  reference via JNIEnv table slot 23.  Null-handle safe, no-JVM safe, used
  by the set_arg string fix described above.
- Unit-test coverage expanded from ~98 to ~177 checks in `test_helpers` and
  from ~20 to ~35 in `test_os_protect_interaction`.  New cases cover:
  iterate_*_entries no-JVM safety + null-arg guards, get_jvm_module /
  get_vm_types / get_vm_structs caching, return_value::set for float /
  double / pointer / unsigned / bool, return_value::caller / stack_trace /
  set_arg with a null frame, set_arg above the JVM max_locals limit,
  jni_delete_local_ref no-JVM safety, is_valid_pointer at the floor /
  ceiling boundaries, decode_u5 multi-byte boundary, format_log positive
  path, protect / allocate_rwx / release / safe_read / get_proc_address
  input guards, and protect walking every memory_protection enum value.
- `vmhook::for_each_thread(visitor)` + `struct thread_info` — walks HotSpot's
  live JavaThread list (classic `Threads::_thread_list` on JDK 8/9, falls
  back to `ThreadsSMRSupport::_java_thread_list` on JDK 10+) and reports each
  thread's state + OS thread ID + raw `java_thread*`.  Completes the
  introspection trio with `for_each_loaded_class` and `for_each_instance`.
- Documented `vmhook::read_java_string(oop)` in the README; the helper has
  existed since 0.2 but was not publicly surfaced.  Decodes a Java String
  (char[] in JDK 8, byte[] + coder in JDK 9+) to a UTF-8 `std::string`
  without needing to register `java/lang/String` as a wrapper.
- `vmhook::scoped_hook<T>(name, callback)` + `class hook_handle` — RAII variant
  of `vmhook::hook<T>`.  The returned `hook_handle` uninstalls just that hook
  when it goes out of scope, restoring the method's original entry points and
  clearing the no-inline / no-compile flags.  Other hooks are unaffected;
  `shutdown_hooks()` still works as a hard reset.
- `vmhook::for_each_instance<T>(visitor, max_visits)` — walks the live heap
  (`Universe::_collectedHeap::_reserved`) and invokes the visitor with a fresh
  `std::unique_ptr<T>` for every object whose narrow-klass header matches
  `T`'s registered class.  Best-effort on region-based GCs (G1); unsupported
  on colored-pointer GCs (ZGC / Shenandoah).
- `return_value::stack_trace(max_depth = 64)` — walks the saved-rbp chain from
  inside a hook callback and returns every interpreter frame as a
  `caller_info`.  Stops at the first compiled / native frame so the result
  reflects only the interpreted portion of the call stack.
- `vmhook::for_each_loaded_class(visitor)` — enumerates every Klass reachable
  through the `ClassLoaderDataGraph`.  Snapshot of the loaded set at call
  time; pair with `vmhook::on_class_loaded` for live notifications.
- `vmhook::on_exception(callback)` — event-driven hook on
  `java.lang.Throwable::fillInStackTrace()`.  Fires whenever any Throwable
  subclass is constructed through one of the public constructors and
  reports the dynamic class name (read from the oop's narrow-klass header).
- Optional vmhook-vs-pure-JNI microbench at `vmhook/src/speedtest.cpp`.
  Lives in its own translation unit so `<jni.h>` never leaks into
  `vmhook.hpp`; opt-in via CMake's `find_package(JNI)` and runs at the end
  of the JVM integration suite, printing `[BENCH]` lines with ns/call for
  both paths.

### Changed
- `method_proxy::call_jni` now handles **static methods** and every
  primitive return type (`Z B C S I J F D V` plus `Ljava/lang/String;`).
  Previously it only supported instance methods returning `void` or
  `String`, which broke on modern JDKs where
  `StubRoutines::_call_stub_entry` is no longer in VMStructs and the
  fallback path was the only way to dispatch.
- `method_proxy` caches `jmethodID` / `jclass` / return-type char on
  first call and reuses them across subsequent invocations.  Combined
  with packing JNI args on the stack instead of through a
  `std::vector`, this brings a tight `Math.abs`-style call loop from
  ~36× slower than pure JNI down to ~1.5× — most of the residual gap
  is the type-safe variant return and the thread-local attach probe.
- `call()` short-circuits straight into `call_jni` on JDKs where
  `_call_stub_entry` is unavailable, instead of doing the call-stub
  prep work (overload resolution, signature reload) on every call only
  to throw it away.

### Fixed
- `for_each_loaded_class` returned nothing on JDK 8.  The internal
  `ClassLoaderDataGraph::for_each_klass` only walked
  `ClassLoaderData::_klasses` (JDK 21+); the JDK 8 path walks
  per-CLD `_dictionary` hashtables plus `SystemDictionary::_dictionary`
  and `_shared_dictionary` for bootstrap classes, which is now wired up.

## [0.4.0] — 2026-05-14

### Added
- `vmhook::return_value::caller()` — from inside any hook callback, returns a
  `caller_info { method*, class_name, method_name, signature }` describing the
  method that invoked the currently-hooked one.  Walks the saved-rbp chain on
  the HotSpot interpreter stack with the same safe-pointer validation the rest
  of the header uses.  Returns `valid() == false` when the parent frame is
  compiled, native, or otherwise unreadable.
- `vmhook::on_class_loaded(callback)` — event-driven hook on
  `java.lang.ClassLoader::defineClass(String, byte[], int, int,
  ProtectionDomain)`.  Fires synchronously on the Java thread that triggered
  the load, with the internal class name (`/`-separated).  Zero polling, zero
  idle cost.  Returns an RAII `watch_handle` that removes the callback when
  destroyed.
- `vmhook::watch_static_field<T, V>(name, callback)` — installs a hardware
  data breakpoint (DR0–DR3 + DR7 write trap) on the field's address.  The
  trap fires instantly on every write with zero idle cost; the callback
  runs synchronously on the writing thread inside a vectored exception
  handler.  Up to four simultaneous watches per process.  Windows × x86_64
  only — on other platforms the function logs an error and returns an
  empty `watch_handle` (no polling fallback; `VMHOOK_HAS_HW_DATA_BREAKPOINTS`
  is the compile-time capability flag).
- New hook overload: `vmhook::hook<T>(name, signature, callback)` selects the
  target method by matching both name AND JVM descriptor.  Needed for classes
  with overloaded methods sharing a name (e.g. ClassLoader's five
  `defineClass` overloads).
- `field_proxy::raw_address()` — exposes the backing memory pointer so the
  hardware-breakpoint watcher can hand it to the kernel.
- `VMHOOK_VERSION_MAJOR/MINOR/PATCH` macros and a `VMHOOK_VERSION` packed
  integer for consumer feature-gating.
- New Java fixtures: `CallerProbe`, `TickerProbe`, `LateClass`.  178 unit
  tests covering everything above plus existing scenarios.

### Changed
- Java fixtures `A` and `B` reorganised for consistency: every class now has
  a Javadoc header documenting its role; constructors are `public` where
  the C++ side needs them; field visibility is uniform.
- `vmhook::object<T>` exposes both deducing-this and static-fallback
  overloads of `get_field`/`get_method` on MSVC and Clang 18+; GCC and
  Android NDK Clang fall back to inherited non-static + explicit
  `static_field`/`static_method` aliases (overload-resolution divergence
  is documented in the header).

### Fixed
- iOS / Android NDK builds now compile cleanly (mach_vm gating, sig*
  POSIX includes, deducing-this disabled where buggy).
- macOS arm64 `allocate_rwx` falls back to PROT_READ|PROT_WRITE when the
  kernel rejects W^X without a JIT entitlement.
- The hardware-breakpoint watcher needs `<mutex>` and `<tlhelp32.h>` —
  added to the master include list.

## [0.3.0] — 2026-05-14

### Added
- macOS, iOS, Android platform detection (`VMHOOK_OS_MACOS`, `_IOS`,
  `_ANDROID`).  Header now compiles cleanly with Apple Clang and the
  Android NDK Clang.
- `VMHOOK_ARCH_X86_64` / `VMHOOK_ARCH_ARM64` macros.
- `VMHOOK_RUNTIME_HOOKING_AVAILABLE` flag — `1` on x86_64 + non-iOS,
  `0` elsewhere.  Runtime hooking trampolines no-op on arm64 / iOS.
- System V AMD64 trampoline for Linux / macOS / Android x86_64.
- CI matrix now covers Windows × {MSVC, clang, MinGW-GCC}, Linux ×
  {GCC, clang}, macOS × clang, plus Android NDK and iOS Xcode cross-compile
  jobs (build-only).
- JVM integration runs against Java 8, 11, 17, 21, 24, 25 on every
  build that produces a usable artefact.

### Changed
- Massive expansion of the Java probe suite (101 → 178 tests).  Now
  covers every primitive type at boundary values, string edge cases,
  multi-dimensional arrays, enums, interface default methods,
  static + non-static nested classes, overloaded methods, throwing
  methods, every primitive return type plus void and null-returning.

## [0.2.0] — 2026-05-14

### Added
- CMake build system (`CMakeLists.txt`) with `vmhook::vmhook` INTERFACE
  target, optional example DLL, optional injector, opt-in
  warnings-as-errors.
- `vmhook::os` portable wrappers for module lookup, memory protection,
  region query, safe memory reads, and thread IDs.  Backed by Win32 on
  Windows and dlopen/mmap/mprotect/process_vm_readv + signal fallback
  on Linux.
- Standalone unit-test suite under `tests/` exercising the OS layer,
  type traits, ODR sanity, and the API surface without a JVM.

### Changed
- Header builds cleanly on MSVC, Clang, and GCC.  Replaced
  `std::print`/`std::println` with a portable formatter helper.
- Logging now goes through `vmhook::detail::format_log` + `emit_log_line`.

## [0.1.0] — 2026-04-29

### Added
- Original Windows-only release: single-header HotSpot hooking library
  with field access, method calling, interpreter-stub hooks (force-return,
  cancel, arg mutation), `make_unique` Java-object allocation, class
  lookup via VMStructs with JNI fallback.

[Unreleased]: https://github.com/xxxnpno/vmhook/compare/v0.4.0...HEAD
[0.4.0]: https://github.com/xxxnpno/vmhook/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/xxxnpno/vmhook/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/xxxnpno/vmhook/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/xxxnpno/vmhook/releases/tag/v0.1.0
