# #28 no-SEH cold-read hardening plan (from no-seh-coldread-audit workflow)
Auto-generated from the audit (40 cold regions, 16 confirmed, 21 contested).
All fixes are CROSS-PLATFORM os::safe_read / existing fault-safe helpers (NOT _WIN32-gated):
these are Method/ConstantPool **metadata** reads, not interpreter frame-slot reads, so they do
NOT regress return_value::stack_trace (the 958877f stk_ frame-walk regression does not apply).

## Summary

Verified every confirmed and contested site by reading the source. The residual #28 cold-read surface that keeps windows-clang/windows-mingw quarantined reduces to FOUR distinct un-hardened reads/writes (one of them appearing at many call sites): (A) constant_pool::get_length() raw int32 deref of ConstantPool::_length at line 2330 — this single accessor is the highest-leverage fix because it is the one un-hardened node on the otherwise-fully-hardened get_name/get_signature metadata chain, and it is reached from the watchdog (Method->get_name), the revive loop, frame::get_arguments on the detour thread, AND the call_jni/resolve_compatible_method static paths; (B) the two terminal base[index] Symbol* slot reads in const_method::get_name (2439) and get_signature (2508), guarded only by a TOCTOU is_readable_pointer VirtualQuery probe; (C) the raw RMW of Method::_access_flags (*flags &= ~NO_COMPILE) at the two teardown sites 10851 (shutdown_hooks) and 10945 (hook_handle::stop) — a raw write, strictly worse than a stray read, whose fault-safe sibling safe_access_flags_or already exists; (D) the raw i2i stub byte read this->target[0]==JMP_OPCODE in ~midi2i_hook at 6684 (the destructor twin of the already-fixed verify_and_repair); (E) the detect_adapter_offset_from_method memcpy scan at 7669; (F) the two static-path _pool_holder Klass* raw derefs at 16098 (call_jni) and 17322 (resolve_compatible_method); and (G) is_static()/get_access_flags raw flags reads on the dispatch path (16066/16716 via is_static() at 16949). All of these have a fault-safe helper already in the codebase (os::safe_read, cold_read_metadata_pointer, safe_access_flags_or/test, or a new safe_access_flags_and to add). The CONTESTED items are overwhelmingly already-guarded (frame::get_method, extract_frame_arg, get_argument, get_i2i_entry, set_code, set_from_compiled_entry, symbol::to_string, safe_access_flags_test/or, get_const_method in the freed-method guard) — no action. The set_dont_inline atomic RMW (7419/7436) is a non-issue for safe_read/safe_write rewriting (it is already read-probed and the atomic RMW must stay atomic). The method_proxy::call call-stub invocation (16799) is an SEH-only concern, not a safe_read target. Recommended order below maximizes severity×confidence×reachability; the single biggest win is hardening get_length() (fix #1) because it closes the last gap on the most-traversed metadata chain.

## Ordered fixes

### #1  vmhook.hpp:2330

**Change:**

```
Replace the raw int32 load `return *reinterpret_cast<const std::int32_t*>(reinterpret_cast<const std::uint8_t*>(this) + entry->offset);` with a cross-platform (NOT _WIN32-gated) fault-safe read returning the existing -1 sentinel:

    std::int32_t length{ -1 };
    if (!vmhook::os::safe_read(&length,
                              reinterpret_cast<const std::uint8_t*>(this) + entry->offset,
                              sizeof(length)))
    {
        return -1;
    }
    return length;

Keep the existing `if (!entry || !is_valid_pointer(this)) return -1;` pre-guard at 2326 untouched. Add a one-line comment mirroring get_name/get_constants noting this is a ConstantPool METADATA read (not a frame walk), so cross-platform safe_read carries no stk_ POSIX regression.
```

**Rationale:** Highest severity×confidence×reachability. This is the SINGLE un-hardened node on the otherwise-fully-hardened Method->ConstMethod->ConstantPool->Symbol metadata chain: get_name (2439-area), get_signature (2478), get_constants (cold_read_metadata_pointer), symbol::to_string (2225/2255) are all already os::safe_read; get_length() at 2330 is the lone raw deref. It is called at 2430 and 2499 by get_name/get_signature, which are themselves reached on FOUR no-SEH paths I confirmed: the auto-repair watchdog (Method->get_name per tick), the verify_hooks revive match loop, frame::get_arguments on the detour thread, and the call_jni/resolve_compatible_method static dispatch. is_valid_pointer (the only current guard) is a pure range/alignment heuristic that never probes the page, so a cold ConstantPool header faults uncontained. Both verifiers (reachability conf 0.9, posix-safety 0.9) and three additional confirmed entries independently flagged this exact line. Returning -1 is the documented 'unknown length, skip bound check' contract (2313-2320); callers at 2431/2500 already test cp_length>=0 and degrade to is_readable_pointer(&base[index]). Lowest-risk, highest-coverage single edit.

**POSIX caveat:** Use cross-platform os::safe_read, do NOT Windows-gate. This is Method/ConstantPool metadata, not an interpreter frame slot, so it cannot reproduce the stk_/return_value::stack_trace POSIX regression (commit 958877f, which was confined to saved-rbp/locals/_pool_holder frame-walk reads in cold_read_frame_pointer). For a live mapped ConstantPool, process_vm_readv returns byte-identical bytes to the raw load, so the warm path and POSIX behavior are unchanged. Cross-platform also covers the detached POSIX watchdog thread, which is itself uncontained.

### #2  vmhook.hpp:10851

**Change:**

```
Add a fault-safe RMW sibling to class method (next to safe_access_flags_or at ~2745):

    auto safe_access_flags_and(const std::uint32_t mask) const noexcept -> bool
    {
        static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("Method", "_access_flags") };
        if (!entry || !vmhook::hotspot::is_valid_pointer(this)) { return false; }
        std::uint8_t* const slot{ reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::method*>(this)) + entry->offset };
        std::uint32_t flags_value{ 0 };
        if (!vmhook::os::safe_read(&flags_value, slot, sizeof(flags_value))) { return false; }
        const std::uint32_t cleared{ flags_value & mask };
        if (cleared == flags_value) { return true; }
        flags_value = cleared;
        return vmhook::os::safe_write(slot, &flags_value, sizeof(flags_value));
    }

Then replace lines 10848-10852 (the get_access_flags() + `*flags &= ~NO_COMPILE` block) with:

    (void)hooked_method_entry.method->safe_access_flags_and(static_cast<std::uint32_t>(~vmhook::hotspot::NO_COMPILE));

Apply the SAME one-line substitution at the twin site 10943-10946 (hook_handle::stop). Leave the existing freed-Method precheck at 10839-10841 / set_dont_inline at 10846/10942 untouched.
```

**Rationale:** High severity: this is a raw WRITE (read-modify-write) into Method::_access_flags on a STORED Method* during teardown, which the codebase itself documents (2727-2740) as 'strictly more dangerous than a stray read because it also corrupts whatever it lands on.' Confirmed reachable on the no-SEH teardown path: shutdown_hooks (10830 delete loop) and hook_handle::stop run on the user control thread outside the msvc-only harness __try. The freed-Method guard at 10839 uses get_const_method() (already fault-safe) but a RedefineClasses/class-unload between that check and the store leaves a TOCTOU window where the slot is cold; is_valid_pointer cannot detect it. The exact-shape fix already exists one accessor over (safe_access_flags_or, 2745) — adding the symmetric AND restores install/teardown symmetry. Fixes BOTH 10851 and 10945 with one helper. posix-safety verifier conf 0.9 on both teardown entries.

**POSIX caveat:** Cross-platform (NOT _WIN32-gated): safe_read/safe_write operate on Method metadata, not a frame walk, so no stk_ regression, and they cover the detached POSIX watchdog/teardown thread too. safe_access_flags_or is already unconditional cross-platform with no reported regression; the AND sibling matches it. The get_access_flags() raw accessor at 2659 stays for live-frame/test-module callers.

### #3  vmhook.hpp:6684

**Change:**

```
Replace the raw opcode probe in ~midi2i_hook with a fault-safe read of byte 0, mirroring the already-fixed verify_and_repair (6748). Change lines 6684-6692 to:

    std::uint8_t first_byte{};
    if (vmhook::os::safe_read(&first_byte, this->target, 1)
        && first_byte == JMP_OPCODE
        && vmhook::os::protect(this->target, 5, vmhook::os::memory_protection::execute_rw, &old_protect))
    {
        std::memcpy(this->target, this->allocated, 5);
        vmhook::os::protect(this->target, 5, vmhook::os::memory_protection::execute_read, &old_protect);
        vmhook::os::flush_instruction_cache(this->target, 5);
    }

Leave the std::memcpy WRITE at 6688 as-is (it is already gated by the os::protect && short-circuit — protect success proves the page is mapped+writable) and os::release at 6694 unchanged.
```

**Rationale:** High severity, clean fix. this->target points into the shared HotSpot i2i interpreter stub page, which a code-cache sweep (GC/deopt on JDK11+) can transiently unmap — the exact #28 fault class. The destructor's raw read at 6684 executes BEFORE the os::protect that guards the write, so it is the lone uncontained operation. Reached via `delete hook_data_entry.hook` in shutdown_hooks (10830) on a thread outside the msvc-only __try. The sibling verify_and_repair was already hardened for the IDENTICAL stub bytes via unconditional os::safe_read (6748, commit 54473de); this destructor copy was simply missed. The only debate (contested entry: reachability said the template interpreter is permanent and not GC-swept) does not lower the fix's value — it is a 3-line, byte-identical-on-success change matching an already-landed sibling, and even the dissenting verifier rated posix_safe_to_guard=true.

**POSIX caveat:** Cross-platform unconditional os::safe_read is correct (matches verify_and_repair, which has no _WIN32 gate). This is a 5-byte machine-code stub read at a fixed address — not a frame field, stack-local, or frame walk — so it cannot reintroduce the stack_trace POSIX regression. On the happy path (stub mapped, byte0==0xE9) ReadProcessMemory/process_vm_readv returns the identical byte, so no test behavior changes; only an unmapped stub diverges (raw->crash, safe_read->skip restore, which is the documented-correct outcome).

### #4  vmhook.hpp:2439

**Change:**

```
Replace the raw terminal slot load `void* const entry_pointer{ base[index] };` at line 2439 (get_name) and the identical site at 2508 (get_signature) with the all-platform metadata helper:

    void* const entry_pointer{ vmhook::hotspot::cold_read_metadata_pointer(&base[index]) };

Keep the downstream `if (!entry_pointer || !is_valid_pointer(entry_pointer)) return nullptr;` check unchanged. The preceding is_readable_pointer(&base[index]) probe at 2435/2504 may be kept as a cheap pre-filter or removed (now redundant).
```

**Rationale:** Medium severity. The terminal Symbol* slot read is currently guarded only by an is_readable_pointer (VirtualQuery) probe at 2435/2504, which is a TOCTOU guard: a concurrent class-unload can unmap the page between the query and the deref at 2439/2508. Reached on the same detour/watchdog/revive metadata paths as fix #1. cold_read_metadata_pointer (2172) routes through os::safe_read on all platforms and returns nullptr on an unmapped page, closing the TOCTOU window. This matches the sibling get_constants() (already uses cold_read_metadata_pointer) and the index reads already in these same functions (os::safe_read at 2478). Two confirmed entries flagged 2439/2508; verifier confidence 0.83-0.9. Ranked below #1-#3 because fix #1 (get_length) removes the more probable fault on the same chain and the is_readable_pointer probe gives partial existing protection here.

**POSIX caveat:** Use cold_read_metadata_pointer (cross-platform safe_read), NOT a _WIN32 gate — it is a ConstantPool metadata read, not a frame walk, immune to the stk_ regression, and the sibling get_constants already uses it. For a mapped slot the bytes are byte-identical to the raw load; POSIX warm path unchanged. Covers the POSIX watchdog thread too.

### #5  vmhook.hpp:16098

**Change:**

```
Replace the raw _pool_holder deref at 16098-16099 with the cross-platform metadata helper:

    auto* const holder_klass{ reinterpret_cast<vmhook::hotspot::klass*>(
        vmhook::hotspot::cold_read_metadata_pointer(
            reinterpret_cast<const std::uint8_t*>(cp) + pool_holder_entry->offset)) };

Keep the existing `if (holder_klass && is_valid_pointer(holder_klass))` gate at 16100. Apply the IDENTICAL change to the twin raw read in resolve_compatible_method at 17321-17322 (gate with the same downstream is_valid_pointer(holder) at 17323).
```

**Rationale:** Medium severity. Both sites raw-deref ConstantPool::_pool_holder to get the owning Klass* on the JNI static-dispatch path, reached from the detour thread (call_jni at 16098 via this->object==nullptr static call; resolve_compatible_method at 17322 reached at 16007/16639). is_valid_pointer on cp is heuristic-only; a cold ConstantPool faults the deref uncontained. Note the FRAME-WALK counterparts at 9430/9570 already use cold_read_frame_pointer (Windows-gated) — but these two are NOT frame walks (they chase a cp reached from a Method*), so the cross-platform cold_read_metadata_pointer is the correct, consistent helper. Both 16098 and 17322 must be fixed together (the call_jni-2 verifier explicitly notes fixing only 16098 leaves 17322 still faulting). Verifier conf 0.82-0.9.

**POSIX caveat:** Use cold_read_metadata_pointer (safe_read on all platforms), NOT cold_read_frame_pointer and NOT a _WIN32 gate. Although the field is _pool_holder (same field the frame-walk uses cold_read_frame_pointer for), THIS read is not part of a stack-frame walk — it is on the detour-thread static-dispatch metadata path — so the watchdog-class rationale (no frame walk -> safe cross-platform) applies. A cold slot now yields nullptr and falls through to the existing FindClass-null diagnostic at 16110.

### #6  vmhook.hpp:16949

**Change:**

```
Harden method_proxy::is_static() once, fixing every caller (the call_jni gate at 16066 and the call() receiver-slot gate at 16716) with no signature change. Replace the body that does `if (auto* flags = this->method->get_access_flags()) return (*flags & 0x0008u) != 0u;` with the existing fault-safe helper:

    auto is_static() const noexcept -> bool
    {
        if (this->method)
        {
            bool found{ false };
            const bool s{ this->method->safe_access_flags_test(0x0008u /*JVM_ACC_STATIC*/, found) };
            return found ? s : false;
        }
        return false;
    }
```

**Rationale:** Medium severity. is_static() raw-derefs Method::_access_flags via get_access_flags() and is on the detour dispatch path (call_jni 16066, call 16716). safe_access_flags_test (2702) already exists, is_valid_pointer-gates `this`, reads the u4 via os::safe_read on all platforms, and is documented as a metadata read with no stk_ regression. Fixing the single accessor fixes both call sites. On a cold/relocated/freed Method it yields found=false -> is_static()==false (non-static -> receiver included), byte-identical to today's fallback contract. JVM_ACC_STATIC (0x0008) lives in the low byte and is width-independent across JDK 8-26. Ranked lower than #1-#5 because the dispatch path is exercised heavily in the green suite without crashing (the warm Method is rarely cold here), but it is a genuine residual raw deref on a no-SEH path. Verifier conf 0.82-0.9.

**POSIX caveat:** Reuse safe_access_flags_test (cross-platform safe_read), NOT a _WIN32 gate. It is a Method-metadata u4 read, not a frame walk — no stk_ regression — and is already used unconditionally on the watchdog path (10326/10486/10511). Mapped Method reads byte-identical; POSIX behavior unchanged.

### #7  vmhook.hpp:7669

**Change:**

```
In detect_adapter_offset_from_method's try_offset lambda (7666-7671), replace the raw `std::memcpy(&candidate, probe_bytes + offset, sizeof(candidate));` with the all-platform fault-safe value reader:

    auto try_offset = [&](const std::size_t offset) noexcept -> bool
    {
        void* const candidate{ vmhook::hotspot::cold_read_metadata_pointer(probe_bytes + offset) };
        return validate_adapter_handler_entry(candidate, c2i_offset);
    };

Note: the two memcpys inside validate_adapter_handler_entry (7619, 7625) read from `candidate`, which is already is_readable_pointer-gated at 7612, so they need no change.
```

**Rationale:** High severity but lower reachability than #1-#6. detect_adapter_offset_from_method scans every 8-byte slot of a cold Method (JDK 9+ uncached _adapter), reached via get_adapter on Mode-3 try_reinstall — the watchdog path. The raw memcpy at 7669 reads slots of a possibly-cold/relocated Method without any per-slot fault guard (only is_valid_pointer(probe) at 7652, which is heuristic). On a cold page the memcpy faults uncontained. get_adapter's own exported-path read (3219) already routes through os::safe_read; this heuristic scan is the remaining raw path on the same Method. cold_read_metadata_pointer returns nullptr on a cold slot -> validate_adapter_handler_entry rejects it -> scan continues. Ranked 7th because it only fires on JDK 9+ when _adapter is not VMStruct-exported AND only on the Mode-3 watchdog repair path, narrowing live reachability; but severity is high (full-Method byte scan). Verifier conf 0.84-0.88.

**POSIX caveat:** Cross-platform cold_read_metadata_pointer is correct — it is a flat Method metadata value-copy, not the stack_trace interpreter frame walk, so no POSIX regression. Covers the detached POSIX watchdog thread. For a mapped slot the value is byte-identical to the memcpy.

## Non-issues
- frame::get_method() (7043 / accessor 5916-5933): ALREADY Windows-guarded via is_valid_pointer + os::safe_read with a deliberately-retained POSIX raw #else. This is the canonical stk_-regression case; must NOT be extended to POSIX. No action — both verifiers agree (reachability already_guarded=true, posix-safety real=false).
- frame::get_arguments signature walk (6098): the Method->ConstMethod->Symbol chain is fully os::safe_read-hardened EXCEPT the get_length() node, which is fixed separately as ordered_fix #1. The walk itself needs no other change.
- frame::get_argument<T> read_slot locals[-slot_index] (6263) and extract_frame_arg read_slot (9158) and the long/double high slot (9228): ALREADY correctly Windows-gated os::safe_read with POSIX raw #else. POSIX MUST stay raw (stack_trace frame-walk regression). No action.
- return_value::caller saved-rbp walk: the finding's quoted line 7627 is stale (that line is now validate_adapter_handler_entry). The real caller()/stack_trace() reads (9353/9491) already use cold_read_frame_pointer (Windows-gated, POSIX raw) — the correct shape for a frame walk. No action; do NOT use cold_read_metadata_pointer here (would reintroduce the POSIX regression).
- method::get_i2i_entry (2596): ALREADY guarded by is_valid_pointer + unconditional cross-platform os::safe_read. Downgrading to a _WIN32 gate (raw POSIX) would REGRESS the detached POSIX watchdog. No action.
- method::safe_access_flags_test (2713) and safe_access_flags_or read (2757) and write (2766): ALREADY routed through os::safe_read/os::safe_write cross-platform. These are the helpers other fixes reuse. No action; a _WIN32 gate would be a net regression.
- method::set_code store (3060) and set_from_compiled_entry store (3172): ALREADY routed through os::safe_write cross-platform. The proposed os::safe_read 'fix' is the wrong primitive (these are STORES) and a _WIN32 gate would un-protect the POSIX watchdog. No action.
- symbol::to_string _body/_length copy (2255/2225) and `this` probe (2215): ALREADY os::safe_read on all platforms. Flat metaspace byte copy, not a frame walk; the proposed POSIX-raw split would delete existing POSIX fault-safety with no upside. No action.
- get_const_method() in the shutdown_hooks freed-Method guard (10841): ALREADY hardened — get_const_method reads _constMethod via cold_read_metadata_pointer (cross-platform safe_read). The finding's note that it is 'Windows-guarded with raw POSIX' is inaccurate; it is all-platform safe. No action.
- verify_hooks NO_COMPILE probe at 10486: this IS safe_access_flags_test, already os::safe_read-routed. No raw deref present. No action.
- set_dont_inline u2 atomic RMW (7419) and u4 atomic RMW (7436): the cold READ is ALREADY guarded by a width-exact os::safe_read pre-probe at 7400-7405. The remaining operation is a std::atomic_ref fetch_or/fetch_and that MUST stay an atomic lock-RMW (composes with HotSpot's own Atomic::cmpxchg from JIT threads); replacing it with os::safe_write would be a correctness regression (non-atomic, torn-write). The narrow probe->RMW TOCTOU window is the accepted #28 limitation, not a safe_read-fixable read bug. No safe_read/safe_write rewrite.
- method_proxy::call call-stub invocation (16799): this is an indirect CALL into the interpreter, not a pointer load — os::safe_read has no applicable shape to wrap an execution transfer. It is an SEH-only concern. The feeding metadata reads (entry via get_from_interpreted_entry safe_read at 2638, re-checked is_valid_pointer at 16661) are already hardened. The real correctness sub-hazard (unrooted GC-relocatable oops packed into params[]) is a separate GC-rooting bug, not a cold-read hardening item. Not a safe_read finding.
- jni_decode_object raw handle deref (16564 -> 11243): CONTESTED (reachability real=false, posix-safety real=true). The handle is a freshly-returned JNI local ref produced microseconds earlier on the same thread (warmest possible pointer); a throwing callee returns NULL (caught by the existing null guard), not a garbage non-null handle. No green CI cell exercises a stale-handle fault here. Optional belt-and-suspenders hardening via cold_read_metadata_pointer is cheap and POSIX-safe but prevents no observed crash — deprioritized below the seven ordered fixes; not required to un-quarantine.

## Confirmed findings (reachability-lens recommended fixes)

### vmhook.hpp:2330 [high] constant_pool::get_length() RAW int32 deref of ConstantPool::_length is not routed through os::safe_read on Windows

- why_cold: `this` is a ConstantPool* obtained off a stored Method*'s ConstMethod* (the get_constants() chain). get_length() is called at line 2430 and line 2499 during symbol resolution (get_name/get_signature), which is exactly the auto-repair watchdog verify_hooks() path. A DEOPT or class-unload between watchdog polls can leave the ConstMethod._constants / ConstantPool pointer non-null but pointing at a freed/unmapped Metaspace page, so `this + entry->offset` (the _length field) can be on an unmapped page when this raw int32 load runs. The immediately-following sibling get_constants() (lines 2357-2373)
- fix: Replace the raw int32 load at line 2330 with a fault-safe read on ALL platforms, mirroring the sibling accessors. Concretely:

    std::int32_t length{ 0 };
    if (!vmhook::os::safe_read(&length,
                               reinterpret_cast<const std::uint8_t*>(this) + entry->offset,
                               sizeof(length)))
    {
        return -1;
    }
    return length;

Returning -1 on an unreadable slot is exactly the existing "length unknown -> skip bound check" contract (see the get_length doc, lines 2312-2320), and the two callers (lines 2431 and 2500) already treat cp_length < 0 as "skip the >= bound check" and fall back to is_readable_pointer(&base[index]) before the element deref, so degraded behavior is already correct. Cross-platform safe_read is justified here (NOT a Windows-only gate): this is a Method-metadata read, not a frame-slot read, so the stk_ POSIX regr

### vmhook.hpp:2330 [high] constant_pool::get_length() raw-derefs _length (int32) — only is_valid_pointer-guarded, not os::safe_read

- why_cold: get_length() is called from inside both get_name() (line 2430) and get_signature() (line 2499), which run on the auto-repair watchdog thread (verify_hooks/Mode-2) and on stack walks. The ConstantPool* comes from ConstMethod::get_constants() off a stored Method* whose ConstMethod can be left non-null but pointing at an unmapped page after a DEOPT / class-unload. The only guard before the load is is_valid_pointer(this) (line 2326), which is a pure range + poison-pattern + alignment filter (VirtualQuery is NOT consulted here) — it does not detect an unmapped/decommitted page, so a stale-but-in-ra
- fix: Replace the raw load at vmhook.hpp:2330 with a fault-safe read on ALL platforms, mirroring the sibling reads in the same struct. Use the cross-platform metadata helper, e.g.:

    std::int32_t value{ -1 };
    if (!vmhook::os::safe_read(&value, reinterpret_cast<const std::uint8_t*>(this) + entry->offset, sizeof(value)))
    {
        return -1; // unreadable/cold ConstantPool -> "unknown length", degrade to is_readable_pointer slot guard
    }
    return value;

Returning -1 on a faulting read is exactly the existing "unknown, skip the bound check" contract (lines 2313-2320, 2431/2500 already test `cp_length >= 0`), so behaviour degrades gracefully to the downstream is_readable_pointer(&base[index]) guard rather than crashing. This is the same discipline get_constants() (cold_read_metadata_pointer) and get_name/get_signature's own _name_index/_signature_index reads (os::safe_read) alread

### vmhook.hpp:2439 [med] get_name(): base[index] Symbol* read is a raw load (VirtualQuery TOCTOU guard, not os::safe_read)

- why_cold: base is ConstantPool::get_base() (header end of the same cold ConstantPool the watchdog reached via a stale Method*->ConstMethod*->_constants). The slot read at base[index] is the constant-pool entry array, which a DEOPT/class-unload can leave on an unmapped page. It IS preceded by is_readable_pointer(&base[index]) (line 2435, a VirtualQuery probe) but that is a TOCTOU check, not a fault-safe load: the page can be decommitted/unmapped between the VirtualQuery and this raw load (the watchdog races live JVM GC/unload), and the load itself does not go through os::safe_read. A faulting load here i
- fix: Replace the raw load at vmhook.hpp:2439 (and the identical site in get_signature at :2508) with the existing fault-safe passthrough already used by the sibling reads in this same file: void* entry_pointer{ nullptr }; if (!vmhook::hotspot::cold_read_metadata_pointer-style safe_read of &base[index] into entry_pointer) return nullptr; e.g. `void* const entry_pointer{ vmhook::hotspot::cold_read_metadata_pointer(&base[index]) };`. cold_read_metadata_pointer (vmhook.hpp:2172) wraps os::safe_read (ReadProcessMemory on Windows, process_vm_readv/sigsetjmp on Linux, mach_vm_read on macOS) on ALL platforms and returns nullptr on an unmapped page instead of faulting. This is the exact idiom already applied to the _name_index read (:2408-2414, safe_read), get_constants() (:2371-2373, cold_read_metadata_pointer), and method::get_const_method() (:2897). The subsequent null + is_valid_pointer check at :

### vmhook.hpp:2508 [med] get_signature(): base[index] Symbol* read is a raw load (VirtualQuery TOCTOU guard, not os::safe_read)

- why_cold: Identical pattern to the get_name() case (finding-2) but in get_signature(). base = ConstantPool::get_base() of a cold ConstantPool the watchdog reached through a stale ConstMethod*; index = _signature_index (safe_read at line 2478). The entry-array slot can be on an unmapped page after DEOPT/class-unload. Preceded by is_readable_pointer(&base[index]) (line 2504, VirtualQuery), but that is a TOCTOU probe and the actual base[index] load is raw, not os::safe_read — a fault here is uncontained on the no-SEH Windows legs and the detached POSIX watchdog thread.
- fix: Replace the raw terminal slot load at line 2508 with os::safe_read on ALL platforms, mirroring the _signature_index read already done at line 2478 in this same function. Concretely, replace `void* const entry_pointer{ base[index] };` with a fault-safe load: `void* entry_pointer{ nullptr }; if (!vmhook::os::safe_read(&entry_pointer, &base[index], sizeof(entry_pointer))) { return nullptr; }`. Keep the existing is_readable_pointer(&base[index]) probe (cheap pre-filter) but no longer rely on it as the sole guard. Apply the identical fix to the sibling get_name() at line 2439 (finding-2) for consistency. safe_read returns false (not a fault) on an unmapped page, so the function degrades to nullptr exactly as its own comment promises ("Read fault-safe on ALL platforms"). No #if _WIN32 split is needed because this is a metadata read, not a frame walk, so the POSIX safe_read path is byte-identic

### vmhook.hpp:2330 [med] ConstantPool::get_length() raw-derefs _length off a cold ConstantPool* (reached via m->get_name()/get_signature() in the revive match loop)

- why_cold: The revive loop at 10302/10304 calls m->get_name()/m->get_signature() on each candidate Method*. Those go method::get_name -> const_method::get_name -> cp = this->get_constants() (safe_read) -> cp->get_length() to bound-check the name/signature index. get_length() RAW-dereferences *(cp + _length offset) guarded ONLY by is_valid_pointer(this) (line 2326), which is a range+alignment+poison HEURISTIC (see 2007-2022) and query_region is NOT consulted here — it does NOT prove the page is mapped. try_reinstall runs on the detached auto-repair watchdog; the ConstantPool reached off a Method that a co
- fix: Replace the raw load at vmhook.hpp:2330 with the same fault-tolerant probe its siblings already use, cross-platform (NOT Windows-gated), because the watchdog is detached on POSIX too. Concretely: std::int32_t length{ 0 }; if (!vmhook::os::safe_read(&length, reinterpret_cast<const std::uint8_t*>(this) + entry->offset, sizeof(length))) return -1; return length;  -- keeping the existing entry/is_valid_pointer(this) precheck and the -1 ("unknown, skip bound check") contract intact. This is a metadata read (not a frame/stack walk), so it cannot trigger the documented stk_ POSIX regression, and -1 already degrades gracefully to the surviving is_readable_pointer(&base[index]) query_region guard in get_name/get_signature.

### vmhook.hpp:2330 [high] constant_pool::get_length() raw-derefs ConstantPool::_length off a cold/relocated CP (reached every watchdog tick via Method::get_name)

- why_cold: The watchdog tick calls verify_hooks() -> for each hm: hm.method->get_name() (line 10435, Mode-2 alias check) and try_reinstall->m->get_name()/get_signature(). const_method::get_name/get_signature (lines 2415/2484) obtain the ConstantPool via this->get_constants() (a stored-Method-derived ConstMethod) and then call cp->get_length() at lines 2430/2499. get_length() does a RAW *(this + _length_offset) load. The cp came from a STORED Method* whose ConstMethod/ConstantPool can be relocated or freed by a class-unload / JVMTI RedefineClasses / deopt between install and this tick. is_valid_pointer(cp
- fix: Convert the raw load in constant_pool::get_length() (vmhook.hpp:2330) to a cross-platform fault-safe read, identical in spirit to the already-applied cold_read_metadata_pointer / os::safe_read conversions on its siblings. Replace:

    return *reinterpret_cast<const std::int32_t*>(reinterpret_cast<const std::uint8_t*>(this) + entry->offset);

with:

    std::int32_t length{ 0 };
    if (!vmhook::os::safe_read(&length, reinterpret_cast<const std::uint8_t*>(this) + entry->offset, sizeof(length)))
    {
        return -1;   // unreadable CP page -> "unknown length", callers already degrade to is_readable_pointer guard
    }
    return length;

Returning -1 on an unreadable page is already the documented "skip the bound check" sentinel (lines 2313-2320), so callers at 2430-2434 and 2499-2503 transparently fall through to the existing is_readable_pointer(&base[index]) per-slot guard at 2435/2

### vmhook.hpp:7669 [high] detect_adapter_offset_from_method() raw memcpy-scans every 8-byte slot of a cold Method (reached via get_adapter on Mode-3 / try_reinstall, JDK 9+ uncached)

- why_cold: verify_hooks() Mode-3 (hm.method->get_adapter(), line 10515, taken when code_now != nullptr) and try_reinstall (new_method->get_adapter(), line 10336) call get_adapter(). On JDK 9+ the VMStructs _adapter export is absent, so get_adapter() falls into the heuristic branch and, whenever cached_offset == 0 (first time, or until some Method validates), calls detect_adapter_offset_from_method(this). That helper memcpy-reads probe_bytes+offset for offset 0..method_size(~512) in 8-byte steps. The probe is the STORED (Mode-3) or freshly-re-resolved Method* and can be cold/relocated/freed; get_adapter()
- fix: In detect_adapter_offset_from_method's try_offset lambda (vmhook.hpp:7666-7671), replace the raw `std::memcpy(&candidate, probe_bytes + offset, sizeof(candidate));` at line 7669 with the existing all-platform fault-safe value reader: `void* const candidate{ vmhook::hotspot::cold_read_metadata_pointer(probe_bytes + offset) };` (defined at 2172, already documented as the get_adapter watchdog metadata reader and cleared for the POSIX no-frame-walk concern). On a cold/unmapped probe slot it returns nullptr, validate_adapter_handler_entry rejects it, and the scan simply continues/returns 0 instead of faulting. This mirrors how get_adapter() (3219/3265) already routes its OWN _adapter slot read through os::safe_read — the helper is the one remaining raw path on the same Method. Equivalently, replace memcpy with an os::safe_read whose failure makes try_offset return false. No POSIX regression: 

### vmhook.hpp:2439 [low] const_method::get_name() raw-derefs base[index] (ConstantPool entry slot) after a VirtualQuery probe, not os::safe_read

- why_cold: Reached per tick via hm.method->get_name() (Mode-2 alias check, line 10435). base = cp->get_base() points into the (cold/relocated) ConstantPool entry array of a stored Method. The element read base[index] is a RAW load. It is preceded by is_readable_pointer(&base[index]) (line 2435), which is a VirtualQuery-based committed/readable/non-guard page check on Windows -- a real mitigation, but a TOCTOU probe rather than os::safe_read, and it is NOT the os::safe_read idiom the residual-hunt targets. A class-unload between the probe and the load (or a non-Windows path where query_region semantics di
- fix: Replace the raw load at vmhook.hpp line 2439 `void* const entry_pointer{ base[index] };` with the library's own cross-platform fault-safe metadata idiom, e.g. `void* const entry_pointer{ vmhook::hotspot::cold_read_metadata_pointer(&base[index]) };` (or `safe_read_pointer(&base[index])`). This routes the cold ConstantPool entry-slot read through os::safe_read (ReadProcessMemory on Windows / process_vm_readv+sigsetjmp on POSIX), which is kernel-validated and atomic — it closes the TOCTOU window that the preceding `is_readable_pointer(&base[index])` probe leaves open (page readable at the 2435 probe, unmapped by a concurrent class-unload at the 2439 load) and returns nullptr instead of faulting the no-SEH legs / the detached POSIX watchdog thread. Keep the is_readable_pointer + bounds + is_valid_pointer checks as cheap pre-filters. Apply the identical change to the same pattern in get_signa

### vmhook.hpp:2508 [low] const_method::get_signature() raw-derefs base[index] (ConstantPool entry slot) after a VirtualQuery probe, not os::safe_read

- why_cold: Reached on the watchdog tick via try_reinstall -> m->get_signature() (line 10304) when an expected_signature is set, and m is a Method resolved off the re-found klass. Identical pattern to finding -3: base = cp->get_base() into a possibly-cold ConstantPool entry array; base[index] is a RAW load guarded only by the preceding is_readable_pointer(&base[index]) VirtualQuery probe (line 2504), not os::safe_read. Lower risk for the same reason, and the Method here was just resolved off a live find_class() klass (warmer than the stored-Method paths).
- fix: Replace the VirtualQuery-probe-then-raw-load pair at vmhook.hpp:2504-2508 (and the identical get_name pair at 2435-2439) with the codebase's own canonical fault-safe helper: `void* const entry_pointer{ vmhook::hotspot::cold_read_metadata_pointer(&base[index]) };` then keep the existing `if (!entry_pointer || !is_valid_pointer(entry_pointer)) return nullptr;` check. Keep the preceding is_valid_pointer(cp)/is_valid_pointer(base) and the cp_length bound check. cold_read_metadata_pointer (line 2172) routes through os::safe_read -> ReadProcessMemory on Windows (kernel-atomic, never faults) and is documented (line 2146) as the intended idiom for exactly get_name/get_signature; it is fault-safe on all platforms and cannot trigger the stk_ POSIX regression because the watchdog never walks a stack frame. This eliminates the TOCTOU window that the current VirtualQuery-then-raw-load leaves open. Th

### vmhook.hpp:10851 [high] RAW read-modify-write of Method::_access_flags (*flags &= ~NO_COMPILE) — un-hardened cold write on a stored Method*

- why_cold: get_access_flags() (line 2659) does NOT read — it only computes `this + _access_flags.offset` and returns the raw pointer; the deref happens here at 10851 as a read-modify-WRITE. The Method* was captured at install time and stored in g_hooked_methods. Between install and shutdown a JVMTI RedefineClasses, class-unload, or GC/metaspace relocation can free or relocate the Method, leaving this slot on a page that still passes the upstream is_valid_pointer() range/alignment heuristic (10840) yet is no longer mapped. The is_valid_pointer + get_const_method() guard at 10839-10841 only proves _constMe
- fix: Replace the raw clear at vmhook.hpp:10848-10852 with a fault-safe RMW sibling, mirroring safe_access_flags_or(). Add a method::safe_access_flags_and(mask) that does is_valid_pointer(this) -> os::safe_read(_access_flags) -> (if bit set) os::safe_write(cleared) and returns bool (false == deferred/cold, never faults). Then shutdown_hooks becomes: (void)hooked_method_entry.method->safe_access_flags_and(~vmhook::hotspot::NO_COMPILE); dropping the get_access_flags()+raw-deref entirely. This restores symmetry with the install (10326) / re-arm (10511) NO_COMPILE-set path and the already-hardened set_dont_inline (10846) on the same loop iteration. Cross-platform (os::safe_read/safe_write are fault-safe on Windows via RPM/WPM and on POSIX via process_vm_readv/writev + signal fallback); it is a Method-metadata RMW, NOT a frame-slot walk, so it does not hit the documented stk_ POSIX regression (9588

### vmhook.hpp:6684 [high] RAW read of i2i interpreter entry stub byte (this->target[0] == JMP_OPCODE) inside ~midi2i_hook, reached via the `delete hook_data_entry.hook` teardown loop

- why_cold: Reached from this region at line 10830 (`delete hook_data_entry.hook` -> ~midi2i_hook). `this->target` is the injection point inside the SHARED HotSpot i2i interpreter entry stub (ctor doc: 'Pointer to the injection point within the i2i stub') — a VM-owned code-cache page, NOT library heap. The very next member, verify_and_repair (6738-6751), reads these SAME stub bytes through os::safe_read with the explicit rationale that 'the shared HotSpot i2i stub page can be transiently UNMAPPED by a code-cache sweep (a GC/deopt on JDK 11+)' and a raw read 'faults UNCONTAINED and kills the JVM — the #28 
- fix: Replace the RAW read `this->target[0] == JMP_OPCODE` at line 6684 with a fault-safe probe of the first stub byte. Read one byte through `vmhook::os::safe_read` and gate the whole restore block on it, e.g.:

    std::uint8_t first_byte{};
    if (vmhook::os::safe_read(&first_byte, this->target, 1)
        && first_byte == JMP_OPCODE
        && vmhook::os::protect(this->target, 5, execute_rw, &old_protect))
    {
        std::memcpy(this->target, this->allocated, 5);
        ... // re-tighten + flush unchanged
    }

This mirrors exactly what the sibling verify_and_repair (6738-6751) already does for the identical stub bytes. safe_read is the right cross-platform call here: it is a passthrough-quality kernel probe (ReadProcessMemory) on Windows and process_vm_readv+sigsetjmp on POSIX, and — like verify_and_repair — this code path never walks an interpreter stack frame, so it CANNOT reintro

### vmhook.hpp:6688 [med] RAW memcpy write into the i2i interpreter stub (restore original 5 bytes) inside ~midi2i_hook — kernel-guarded by os::protect short-circuit but preceded by the raw read

- why_cold: Reached from line 10830 via ~midi2i_hook. `this->target` is the shared HotSpot i2i stub (VM code-cache page) that a code-cache sweep/deopt can unmap between install and shutdown. This WRITE is partially guarded: it only runs if os::protect(this->target,...) (VirtualProtect, kernel-validated) succeeds via the && short-circuit, so an unmapped page makes protect() fail and skips the memcpy. But (a) the guard is the os::protect success, not os::safe_write, so it relies on VirtualProtect rejecting the unmapped page rather than a fault-tolerant probe, and (b) the condition is only reached AFTER the 
- fix: Harden the destructor's pre-check read of the shared i2i stub the SAME way the sibling verify_and_repair() already was (commit 54473de). In ~midi2i_hook (vmhook.hpp:6668-6695), replace the raw `if (this->target[0] == JMP_OPCODE && os::protect(...))` at line 6684 with a fault-tolerant probe of the 5 stub bytes via os::safe_read (which is kernel-validated ReadProcessMemory on Windows / process_vm_readv on POSIX and never faults). Concretely: read `std::uint8_t current[5]` through `vmhook::os::safe_read(current, this->target, 5)`; if it returns false (stub page transiently unmapped by a code-cache sweep/deopt during teardown) skip the restore entirely and just fall through to `os::release(this->allocated, ...)` — leaving the stub bytes as-is is harmless because the page is gone/being reclaimed. Only when the read succeeds AND current[0]==0xE9 should it proceed to os::protect + std::memcpy(t

### vmhook.hpp:10945 [high] Raw read-modify-write of Method::_access_flags (*flags &= ~NO_COMPILE) is not routed through os::safe_read on Windows

- why_cold: get_access_flags() (line 2659) only does pointer arithmetic (this + _access_flags offset) and returns a RAW std::uint32_t* into the Method; the dereference is THIS raw RMW. stop() runs when a user drops a hook_handle: the stored Method* (entry_it->method) may have been GC-relocated / class-unloaded / freed by another DLL's JVMTI RedefineClasses between install and teardown (exactly the drift documented at hooked_method lines 6873-6884). A cold/relocated/freed Method page that still passes is_valid_pointer's range/alignment heuristic FAULTS on this load+store. stop()'s try/catch does NOT catch 
- fix: Mirror the shutdown_hooks() guard + add a fault-safe clear path. In hook_handle::stop(), before touching access flags, gate on the same drift-detector shutdown_hooks() already uses (lines 10839-10844): skip the entry when !is_valid_pointer(entry_it->method) || !entry_it->method->get_const_method() (get_const_method() is already cross-platform fault-safe via cold_read_metadata_pointer/safe_read and returning null is the established "Method freed" signal). Then replace the raw RMW at line 10945 (`*flags &= ~NO_COMPILE`) with a fault-safe clear counterpart of safe_access_flags_or (line 2745): add method::safe_access_flags_and(mask) that reads via os::safe_read and writes via os::safe_write the (~NO_COMPILE) clear, deferring on a cold page. Both halves use the same safe_read/safe_write primitives that safe_access_flags_or already uses cross-platform — no #if _WIN32 split needed, no stk_ POSI

### vmhook.hpp:16716 [med] this->is_static() -> get_access_flags() RAW *flags read (receiver-slot decision)

- why_cold: is_static() (impl 16949-16960) calls get_access_flags() which returns a RAW pointer ((u8*)this + _access_flags.offset, impl 2671, NOT safe_read) and then RAW-reads *flags & 0x0008u at 16956. The Method (this->method) is the proxy's stored Method*; in the detour context it can be freed/relocated by a class-unload/deopt, so _access_flags can land on a cold page and the *flags load faults uncontained on the no-SEH legs. Note method::safe_access_flags_test() (2702) is the os::safe_read-guarded variant that exists precisely for cold Method flag reads, but is_static() does not use it. The deref inst
- fix: Route the receiver-slot static decision through the existing fault-safe path instead of the raw is_static(). Replace the `!this->is_static()` clause at line 16716 with a safe_access_flags_test()-based check, e.g.:

    bool flags_found{ false };
    const bool method_is_static{ this->method
        ? this->method->safe_access_flags_test(0x0008u /* JVM_ACC_STATIC */, flags_found)
        : false };
    if (this->object && !method_is_static)
    {
        params[param_idx++] = reinterpret_cast<std::intptr_t>(this->object);
    }

safe_access_flags_test() (impl 2702) already does is_valid_pointer(this) + os::safe_read of the u4 flags word and is noexcept, so a cold/relocated/freed Method yields found=false (treated as non-static -> receiver included, byte-identical to today's noexcept-false fallback) and NEVER faults. It routes through os::safe_read on every platform (ReadProcessMemory on W

### vmhook.hpp:16066 [med] is_static() raw-derefs Method::_access_flags (no safe_read; safe_access_flags_test exists but is not used)

- why_cold: this->method is an arbitrary proxy's stored Method*. call_jni runs on the detour thread; a concurrent GC / JIT deopt / class-unload / RedefineClasses can relocate or free that Method between when the proxy captured it and this dispatch. The Method* passes is_valid_pointer's range/alignment heuristic yet its _access_flags page can be unmapped, so *flags faults uncontained on the no-SEH legs (mingw/clang-on-windows). The fault-safe sibling safe_access_flags_test() (os::safe_read, line 2702) exists for exactly this but is_static() does NOT call it; get_access_flags() (2671) just returns a raw int
- fix: Make method_proxy::is_static() (vmhook.hpp:16949-16960) read JVM_ACC_STATIC fault-safely instead of raw-dereferencing the interior pointer from get_access_flags(). The exact fault-safe sibling already exists: vmhook::hotspot::method::safe_access_flags_test(mask, found) at line 2702 (is_valid_pointer precheck + os::safe_read of the _access_flags u4). Change is_static() to:
  if (this->method) { bool found{false}; const bool is_st{ this->method->safe_access_flags_test(0x0008u, found) }; if (found) return is_st; }
  return false;
This is POSIX-safe because it reads Method metadata (a u4), not a stack frame — the header's own comment at lines 2693-2696 confirms "no stk_ POSIX regression" — so it can be applied unconditionally (no #ifdef _WIN32 needed). Apply to BOTH callers: call_jni (the is_static_call gate at line 16066) and call() (the receiver-slot gate at line 16716) inherit the fix aut

### vmhook.hpp:16098 [med] Static path raw-derefs ConstantPool::_pool_holder slot to get the owning Klass*

- why_cold: Static-call path (first uncached dispatch). cp comes from const_method->get_constants() (which IS safe-read, returning a non-null but possibly stale ConstantPool*) and only passes is_valid_pointer() at 16092 — a range/alignment heuristic, NOT proof the _pool_holder field page is mapped. A concurrent class-unload / metaspace relocation of the declaring class between get_constants() and this load leaves cp pointing at a freed/cold metaspace page; the raw *(cp + offset) load then faults uncontained on the no-SEH legs and on the detached POSIX watchdog-equivalent. Unlike the surrounding get_const_
- fix: Replace the raw deref at vmhook.hpp:16098-16099 with the already-proven sibling idiom used by return_value::caller (9429-9438): first gate the SLOT address, then read through the Windows-gated / POSIX-raw helper, then re-validate the result. Concretely:

  const auto* const slot{ reinterpret_cast<const std::uint8_t*>(cp) + pool_holder_entry->offset };
  if (vmhook::hotspot::is_valid_pointer(slot)) {
      auto* const holder_klass{ reinterpret_cast<vmhook::hotspot::klass*>(
          vmhook::hotspot::cold_read_frame_pointer(slot)) };
      if (holder_klass && vmhook::hotspot::is_valid_pointer(holder_klass)) { ... }
  }

cold_read_frame_pointer is os::safe_read on _WIN32 and a raw load on POSIX (2124-2137), so it fixes the no-SEH crash without touching POSIX behavior; cold_read_metadata_pointer (safe_read everywhere) is equally acceptable here since this is a metadata read, not a frame wal

## Completeness critic

`safe_read_pointer` is clean — it range-checks then reads into a local via safe_read, no deref-before-probe. 

I have completed the analysis. Here are my findings.

---

## Critic findings — residual un-guarded cold reads NOT covered by the mapped regions

The mapped regions and CONFIRMED FIXES focus on the **watchdog**, **frame-walk**, **teardown**, and the `method_proxy::call`/`call_jni` bodies themselves. The gap is a whole accessor family — `klass::*` instance-metadata getters and the **detour-side overload-resolution loop** — that raw-derefs cold Klass/Method/Array slots. The watchdog twin of this exact slot read was hardened (line 10300 via `safe_read_pointer`); the **detour twin was not**.

### Highest severity (detour path, GC/JIT-relocatable target, raw deref)

1. **`method_proxy::resolve_compatible_method` — raw `_methods` array-slot deref**
   `C:\repos\cpp\vmhook\vmhook\ext\vmhook\vmhook.hpp:17348` — `methods_array[method_index]` is a raw load off the `Array<Method*>` data region while resolving an overload FROM a detour (called by both `call()` @16639 and `call_jni()` @16007). Its watchdog twin at line 10300 reads the identical slot via `safe_read_pointer`; this detour copy does not. A torn/corrupt `_methods` count vs. real array extent walks off an unmapped page → uncontained AV on no-SEH.

2. **`object_base::get_method` resolution loop — same raw `_methods` slot deref**
   `C:\repos\cpp\vmhook\vmhook\ext\vmhook\vmhook.hpp:17687` (and the parallel interface/static walks at `17743`, `17813`, `17876` per the earlier grep) — same `methods_array[method_index]` raw load, reachable from a detour via the wrapper API.

3. **`klass::get_methods_count` / `klass::get_methods_ptr` — raw `_methods` pointer + `_length` deref**
   `vmhook.hpp:3391` and `3404` (count), `3430` (ptr) — `*reinterpret_cast<void**>(this+offset)` and `*reinterpret_cast<int32*>(array)`, gated only by `is_valid_pointer`. These feed every loop above on the detour path; the slot read is raw even though the result is validated.

4. **`klass::get_super` — raw `_super` slot deref**
   `vmhook.hpp:3620-3622` — raw `*reinterpret_cast<klass**>(this+_super_offset)`, gated only by `is_valid_pointer(this)`. Drives the superclass walk in #1/#2 on the detour. (Distinct hazard from the array-klass JDK-variant issue already noted in memory — this is the cold-fault angle.)

### Medium (detour path, but target is a permanently-loaded bootstrap klass — low practical, real in principle)

5. **`klass::get_prototype_header` — raw `_prototype_header` deref**
   `vmhook.hpp:3685` — raw deref used by `make_java_object` @13875 on the detour (make_java_string/make_java_array). Klass is usually `java/lang/String` (never unloaded), so practically warm, but it is an un-hardened cold read by the stated rule.

6. **`klass::get_instance_size` — raw `_layout_helper` deref**
   `vmhook.hpp:3651` — raw deref, called from `make_java_string` @14094 on the detour. Same warm-klass caveat as #5.

7. **`klass::find_field` — raw `_constants` and `_fields` slot derefs**
   `vmhook.hpp:3887` (`_constants`) and `3912` (`_fields`) — gated only by `is_valid_pointer(this)`, reached on the detour via make_java_string's `find_field("coder"/"value"/...)`. Same warm-klass caveat.

8. **`method::get_access_flags` raw accessor consumed on the detour via `method_proxy::is_static`**
   The accessor (`vmhook.hpp:2671`) returns a raw pointer by design; the **detour-path consumer** `is_static()` then does a raw `*flags & 0x0008u` at `vmhook.hpp:16956` (reached from `call()` @16716 and the static-arg decision). The watchdog already has `safe_access_flags_test` (2702) for exactly this; the detour `is_static` does not use it. Target Method is the proxy's own (usually warm), but it is the same un-contained raw read pattern.

### Cleared (checked, already safe — no action)
- `return_value::caller` (9332) and `return_value::stack_trace` (9455): fully routed through `cold_read_frame_pointer` (Windows-gated, POSIX-raw) with stack-growth invariant. No residual raw deref.
- `cold_read_frame_pointer` (2124) / `cold_read_metadata_pointer` (2172) / `safe_read_pointer` (2066): all read into a local, no deref-before-probe.
- `read_java_string` (19819) and `make_java_string` backing-array writes (14114-14155): writes target freshly-allocated/memset TLAB pages; reads are safe_read. Safe.
- `os::safe_read` (926): no deref-before-probe on Windows/Linux/macOS (iOS raw-memcpy is documented and out of no-SEH scope).
- `klass::get_name` (3333) / `get_next_link` (3369): already `safe_read_pointer`.

### Bottom line
The biggest missed class is the **detour-side klass-metadata accessors** (`get_methods_count`/`get_methods_ptr`/`get_super`, and the `methods_array[i]` slot reads in `resolve_compatible_method` @17348 and `get_method` @17687/17743/17813/17876). These are genuine GC/JIT-relocatable targets read raw on the detour thread, and their watchdog twin (10300) is already hardened — so the fix pattern is proven and the asymmetry is the tell. Findings #5–#8 are real un-hardened cold reads too, but their targets are permanently-loaded bootstrap klasses / the proxy's own warm Method, so they are lower practical risk.
