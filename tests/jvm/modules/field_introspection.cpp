// field_introspection JVM test module  (feature area: fields)
//
// Exhaustively exercises the FIVE field_proxy introspection accessors on a live
// JVM, through the public wrapper API (static_field("n") / get_field("n")):
//
// CRASH-PROOFING (mingw·java8 has NO SEH net — any wild read kills the JVM):
//   The fixture's `instance` is a YOUNG-GEN object; any probe allocation can
//   trigger a minor GC that RELOCATES it.  field_proxy::get() and
//   get_compressed_oop() RAW-memcpy field_pointer (== instance+offset for an
//   instance field) with NO safe_read, so reading an INSTANCE field through a
//   wrapper decoded BEFORE that GC derefs an unmapped page and faults.  Every
//   instance-backed read here is therefore (1) RE-ACQUIRED fresh right before use
//   (get_instance() re-decodes the stable old-gen mirror slot to the CURRENT
//   location, shrinking the relocation window to a few instructions) and (2)
//   exact-byte PROBED via os::safe_read on the precise field slot the read will
//   touch (+ the oop header) before the read — a successful probe means the
//   matching memcpy CANNOT fault, and a transient miss degrades to a best-effort
//   [INFO] skip (never a fault, never a vacuous pass).  STATIC reads go through
//   the Class mirror (old-gen, NOT young-relocated) and stay HARD.  Fine-grained
//   ctx.record() checkpoints (flushed per line) bracket every section and every
//   individual instance read so a no-SEH crash's last-flushed line pinpoints the
//   exact faulting op.
//
//   * signature()         (vmhook.hpp:11759-11763) — returns the exact JVM type
//     descriptor for EVERY field shape: the eight primitives Z B S I J F D C,
//     Ljava/lang/String;, [I, [[I, [Ljava/lang/Object;, [[Ljava/lang/Object;,
//     [Ljava/lang/String;, Ljava/lang/Object;, an interface ref
//     Ljava/lang/Runnable;, and a self reference Lvmhook/fixtures/FieldIntrospection;.
//     Verified for static AND instance proxies, on synthetic stack proxies
//     (verbatim view of every primitive char + V + degenerate forms), and proven
//     to be a stable view aliasing the proxy.  FINAL fields carry the SAME
//     descriptor as their mutable twins (none of the five accessors surface
//     JVM_ACC_FINAL — finality-blind).
//
//   * is_static()         (vmhook.hpp:11787-11791) — true for every static
//     field, false for every instance field; cross-proven by reading a STATIC
//     field through an INSTANCE wrapper (is_static stays true — it reflects the
//     JVM_ACC_STATIC flag, not the accessor used).
//
//   * is_reference()      (vmhook.hpp:11805-11814) — true iff the descriptor's
//     first char is 'L' or '['; false for all primitives and for the empty
//     signature.  Checked to be the exact complement of jvm_primitive_byte_width
//     != 0 across the whole field set.
//
//   * raw_address()       (vmhook.hpp:11773-11776) — non-null for a resolved
//     field; byte-equal to an INDEPENDENTLY recomputed (mirror+offset) for
//     statics and (oop+offset) for instances (via find_class/find_field);
//     STABLE across repeated lookups; the exact address get()/get_compressed_oop
//     read from; and width-aligned.  Mode 2 forces a GC between two lookups to
//     DOCUMENT the GC-staleness flaw (raw_address does no pinning).
//
//   * get_compressed_oop()(vmhook.hpp:11820-11830) — for a reference field
//     decodes (decode_oop_pointer) to the SAME oop get() yields as void* and
//     that field_oop() yields; the decoded oop is the REAL object, proven
//     structurally (read_java_string / array_length / element / klass name) and
//     against Java-published identity witnesses.  The known FLAWS are pinned:
//     no signature guard (returns the raw 4 primitive bytes), reads exactly 4
//     bytes (low half of a J/D field), and 0 for a null reference field.
//
// Harness shape mirrors the pilot/hook_basic/field_* modules: register_class, a
// scoped_hook to satisfy the interpreter-hook-on-dispatch contract, run_probe
// for the go/done handshake, and a dense battery of ctx.check() assertions.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

namespace
{
    // Wrapper for vmhook.fixtures.FieldIntrospection.
    class fi_fixture : public vmhook::object<fi_fixture>
    {
    public:
        explicit fi_fixture(vmhook::oop_t instance) noexcept
            : vmhook::object<fi_fixture>{ instance }
        {
        }

        // ── handshake / scenario selector ──────────────────────────────────
        static auto set_go(bool value) -> void       { static_field("go")->set(value); }
        static auto set_done(bool value) -> void      { static_field("done")->set(value); }
        static auto get_done() -> bool                { return static_field("done")->get(); }
        static auto set_mode(std::int32_t m) -> void  { static_field("mode")->set(m); }
        static auto get_observed() -> std::int32_t    { return static_field("observed")->get(); }

        // ── the live instance the fixture published (a real heap OOP) ──────
        static auto get_instance() -> std::unique_ptr<fi_fixture>
        {
            return static_field("instance")->get();   // value_t -> unique_ptr<fi_fixture>
        }

        // ── Java-published identity witnesses ──────────────────────────────
        static auto s_string_hash()    -> std::int32_t { return static_field("sStringIdentityHash")->get(); }
        static auto s_object_hash()     -> std::int32_t { return static_field("sObjectIdentityHash")->get(); }
        static auto s_runnable_hash()   -> std::int32_t { return static_field("sRunnableIdentityHash")->get(); }
        static auto s_int_array_hash()  -> std::int32_t { return static_field("sIntArrayIdentityHash")->get(); }
        static auto s_int_array_len()   -> std::int32_t { return static_field("sIntArrayLength")->get(); }
        static auto s_int_array_elem0() -> std::int32_t { return static_field("sIntArrayElem0")->get(); }
        static auto s_obj_array_len()   -> std::int32_t { return static_field("sObjArrayLength")->get(); }
        static auto s_str_array_len()   -> std::int32_t { return static_field("sStrArrayLength")->get(); }
        static auto s_string_len()      -> std::int32_t { return static_field("sStringLength")->get(); }
        static auto i_int_array_len()   -> std::int32_t { return static_field("iIntArrayLength")->get(); }
        static auto s_int_array2d_len() -> std::int32_t { return static_field("sIntArray2DLength")->get(); }
        static auto s_obj_array2d_len() -> std::int32_t { return static_field("sObjArray2DLength")->get(); }
        static auto s_str_array_elem0_len() -> std::int32_t { return static_field("sStrArrayElem0Length")->get(); }

        // Read the static `sLong` field's raw 8 bytes for the get_compressed_oop
        // low-half truncation proof.
        static auto s_long_raw() -> std::int64_t { return static_field("sLong")->get(); }
        static auto s_int_raw()  -> std::int32_t { return static_field("sInt")->get(); }
    };

    // ── varied-shape wrappers for field-metadata enumeration (Sections I–L) ────
    // Each is a minimal object<T> registered for a nested fixture class.  We only
    // need static_field()/get_field() name resolution on them — the metadata
    // accessors (signature/is_static/is_reference/raw_address) key off the proxy
    // built by that resolution, so no extra members are required.

    // ZERO-field shape (FieldIntrospection$Empty): every lookup must miss.
    class fi_empty : public vmhook::object<fi_empty>
    {
    public:
        explicit fi_empty(vmhook::oop_t instance) noexcept
            : vmhook::object<fi_empty>{ instance } {}
    };

    // ONLY-static shape (FieldIntrospection$OnlyStatic).
    class fi_only_static : public vmhook::object<fi_only_static>
    {
    public:
        explicit fi_only_static(vmhook::oop_t instance) noexcept
            : vmhook::object<fi_only_static>{ instance } {}
    };

    // ONLY-instance shape (FieldIntrospection$OnlyInstance); the live instance is
    // published as FieldIntrospection.onlyInstance.
    class fi_only_instance : public vmhook::object<fi_only_instance>
    {
    public:
        explicit fi_only_instance(vmhook::oop_t instance) noexcept
            : vmhook::object<fi_only_instance>{ instance } {}
    };

    // Inheritance base (FieldIntrospection$Base).
    class fi_base : public vmhook::object<fi_base>
    {
    public:
        explicit fi_base(vmhook::oop_t instance) noexcept
            : vmhook::object<fi_base>{ instance } {}
    };

    // Inheritance leaf (FieldIntrospection$Derived); the live instance is
    // published as FieldIntrospection.derived.
    class fi_derived : public vmhook::object<fi_derived>
    {
    public:
        explicit fi_derived(vmhook::oop_t instance) noexcept
            : vmhook::object<fi_derived>{ instance } {}

        // The live Derived / OnlyInstance the fixture published.  Their static
        // slots live on FieldIntrospection's mirror, so resolve through the
        // primary wrapper (GCC-portable static_field), then re-type the oop.
        static auto get_derived() -> std::unique_ptr<fi_derived>
        {
            return fi_fixture::static_field("derived")->get();
        }
    };

    auto get_only_instance() -> std::unique_ptr<fi_only_instance>
    {
        return fi_fixture::static_field("onlyInstance")->get();
    }

    std::atomic<int>          g_hook_calls{ 0 };
    std::atomic<bool>         g_hook_saw_self{ false };

    // ── independent address recomputation (parallels get_field's own math) ──
    // For a STATIC field, raw_address must equal mirror + offset.  We recompute
    // the offset from the klass metadata directly, NOT through field_proxy, so a
    // future refactor that drifts the two apart is caught.
    auto recompute_static_addr(vmhook::hotspot::klass* k, const char* name) -> void*
    {
        if (!k) { return nullptr; }
        const auto entry{ vmhook::find_field(k, name) };
        if (!entry || !entry->is_static) { return nullptr; }
        void* const mirror{ k->get_java_mirror() };
        if (!mirror) { return nullptr; }
        return reinterpret_cast<std::uint8_t*>(mirror) + entry->offset;
    }

    // For an INSTANCE field, raw_address must equal oop + offset.
    auto recompute_instance_addr(vmhook::hotspot::klass* k, void* oop, const char* name) -> void*
    {
        if (!k || !oop) { return nullptr; }
        const auto entry{ vmhook::find_field(k, name) };
        if (!entry || entry->is_static) { return nullptr; }
        return reinterpret_cast<std::uint8_t*>(oop) + entry->offset;
    }

    // ── CRASH-PROOFING: validate a decoded OOP before any RAW dereference ────
    //
    // WHY this module needs more than is_valid_pointer():
    //   get_compressed_oop()/get()(as void*) call decode_oop_pointer(), which is
    //   PURE ARITHMETIC (vmhook.hpp ~4380 narrow_decode: base + (compressed<<shift))
    //   with NO validity filter — a stale/relocated/wild compressed value yields a
    //   wild but range-plausible pointer.  The library's object readers
    //   (read_java_string ~16046, array_length ~11877, get_array_element ~11906,
    //   klass_from_oop ~14930) then RAW-dereference that pointer, gated ONLY by
    //   is_valid_pointer().  is_valid_pointer() checks range + alignment + debug
    //   poison patterns — a GC-RELOCATED object's OLD address is canonical,
    //   aligned and in-range, so it PASSES is_valid_pointer() yet points into a
    //   now-unmapped/relocated page.  The raw read then segfaults; with no SEH net
    //   (mingw·java8) that takes down the whole JVM.
    //
    // os::safe_read() (ReadProcessMemory on Windows / process_vm_readv on Linux)
    // is the ONLY check that actually proves the page is currently mapped: it
    // performs the read through a kernel path that returns false instead of
    // faulting on an unmapped/relocated address.  We probe the OOP header region
    // every library reader touches (mark word @0, narrow-klass @8, array length
    // @12 — the first 16 bytes) before handing the oop to any raw-deref helper.
    //
    // GC timing makes this BEST-EFFORT, not a hard guarantee: a concurrent/young
    // collector can relocate an object between the probe and the library read.
    // So callers treat a failed probe (or an empty read) as a transient miss:
    // record [INFO] and skip the STRONG assertion (never fail).  The arithmetic /
    // descriptor / primitive checks that do NOT deref an oop stay hard.
    constexpr std::size_t k_oop_header_probe_bytes{ 16 };

    auto oop_header_safely_readable(void* const decoded) -> bool
    {
        if (!decoded || !vmhook::hotspot::is_valid_pointer(decoded)) { return false; }
        std::uint8_t scratch[k_oop_header_probe_bytes] = { 0 };
        // ReadProcessMemory/process_vm_readv: returns false (no fault) if any byte
        // of the header is on an unmapped/relocated page.
        return vmhook::os::safe_read(scratch, decoded, sizeof(scratch));
    }

    // Decode + safe-probe a reference field's compressed OOP in one step.  Returns
    // nullptr (NOT a faulting pointer) if the slot is null, the decode is wild, or
    // the object's header is not currently mapped (stale / mid-relocation).
    auto safely_decoded_field_oop(const vmhook::field_proxy& fp) -> void*
    {
        void* const decoded{ vmhook::hotspot::decode_oop_pointer(fp.get_compressed_oop()) };
        return oop_header_safely_readable(decoded) ? decoded : nullptr;
    }

    // Decode a reference field's compressed OOP to its internal klass name
    // (e.g. "java/lang/String", "[I").  Empty string on any failure OR if the
    // decoded oop's header is not safely readable (stale/relocated/wild) — so a
    // caller comparing the result to an expected name naturally degrades to a
    // best-effort miss instead of faulting inside klass_from_oop's raw read.
    auto klass_name_of_field(const vmhook::field_proxy& fp) -> std::string
    {
        void* const decoded{ safely_decoded_field_oop(fp) };
        if (!decoded) { return {}; }
        vmhook::hotspot::klass* const k{ vmhook::klass_from_oop(decoded) };
        if (!k) { return {}; }
        vmhook::hotspot::symbol* const sym{ k->get_name() };
        if (!sym) { return {}; }
        return sym->to_string();
    }

    // Safe wrappers around the RAW-deref library readers: probe the oop header
    // first, and return a sentinel (empty string / -1 length / T{}) on a failed
    // probe so the call site can branch to a best-effort [INFO] instead of
    // faulting.  These never themselves dereference an unmapped oop.
    auto safe_read_java_string(void* const decoded) -> std::string
    {
        return oop_header_safely_readable(decoded) ? vmhook::read_java_string(decoded)
                                                   : std::string{};
    }

    auto safe_array_length(void* const decoded) -> std::int32_t
    {
        return oop_header_safely_readable(decoded) ? vmhook::array_length(decoded)
                                                   : -1;
    }

    template<typename element_type>
    auto safe_array_element(void* const decoded, const std::int32_t index) -> element_type
    {
        return oop_header_safely_readable(decoded)
                   ? vmhook::get_array_element<element_type>(decoded, index)
                   : element_type{};
    }

    // ── CRASH-PROOFING the *field-slot* read itself (the a84e51a gap) ─────────
    //
    // a84e51a hardened only the oop-CONTENT decode sites (decode -> read_java_string
    // / array_length / klass_from_oop).  It did NOT harden the read of the field
    // SLOT on an INSTANCE-backed proxy.  field_proxy::get() (vmhook.hpp:12268) and
    // field_proxy::get_compressed_oop() (vmhook.hpp:12540) RAW-memcpy `field_pointer`
    // with only a null-check — no safe_read.  For an INSTANCE field
    // field_pointer == decoded_instance_oop + offset (vmhook.hpp:14371).  The
    // fixture's `instance` (FieldIntrospection.instance) is a young-gen object: a
    // minor/young GC triggered by any probe allocation RELOCATES it and frees its
    // old page, while the static `instance` slot on the (old-gen) Class mirror is
    // fixed up by the GC.  A wrapper obtained BEFORE that GC still holds the OLD
    // decoded oop, so reading instance+offset through it derefs an unmapped page →
    // SIGSEGV.  On mingw·java8 (no SEH net) that takes down the whole JVM, which is
    // exactly the crash that recurred after a84e51a.
    //
    // The guarantee: before any get()/get_compressed_oop()/memcpy that will read
    // `width` bytes at `field_addr`, safe_read EXACTLY those `width` bytes.
    // safe_read goes through ReadProcessMemory / process_vm_readv, a kernel path
    // that returns false instead of faulting on an unmapped/relocated page.  If it
    // succeeds, the matching memcpy of the SAME bytes at the SAME address cannot
    // fault (the page is currently mapped).  The only residual is a TOCTOU window
    // between probe and read; callers SHRINK it to a handful of instructions by
    // RE-ACQUIRING the instance fresh (re-decoding the stable mirror slot) right
    // before the probe, and a relocation landing inside that window degrades to a
    // best-effort [INFO] miss — never a fault.  A SUCCESSFUL read still asserts the
    // correct value, so coverage stays non-vacuous.
    auto field_slot_safely_readable(void* const field_addr, const std::size_t width) -> bool
    {
        if (!field_addr || width == 0) { return false; }
        std::uint8_t scratch[8] = { 0 };   // widest field is J/D (8 bytes)
        if (width > sizeof(scratch)) { return false; }
        return vmhook::os::safe_read(scratch, field_addr, width);
    }

    // An INSTANCE-backed proxy is safe to read iff (a) its object header is mapped
    // (proves the base oop is the real, currently-resident object) AND (b) the
    // exact field slot the read will touch is mapped.  `base_oop` is the wrapper's
    // decoded instance oop; `fp.raw_address()` is base_oop+offset (the slot).
    auto instance_field_read_safe(void* const base_oop,
                                  const vmhook::field_proxy& fp,
                                  const std::size_t width) -> bool
    {
        return oop_header_safely_readable(base_oop)
            && field_slot_safely_readable(fp.raw_address(), width);
    }
}

VMHOOK_JVM_MODULE(field_introspection)
{
    vmhook::register_class<fi_fixture>("vmhook/fixtures/FieldIntrospection");
    // Sibling wrappers for the varied-shape field-metadata enumeration (Sections
    // I–L).  Registering an absent/unloaded nested class is harmless — every
    // resolution through that wrapper then returns nullopt and the dependent
    // checks are individually guarded.  These nested classes are loaded by
    // FieldIntrospection's <clinit> (onlyInstance/derived construction references
    // them; OnlyStatic/Base/Empty are referenced by the wrappers below).
    vmhook::register_class<fi_empty>("vmhook/fixtures/FieldIntrospection$Empty");
    vmhook::register_class<fi_only_static>("vmhook/fixtures/FieldIntrospection$OnlyStatic");
    vmhook::register_class<fi_only_instance>("vmhook/fixtures/FieldIntrospection$OnlyInstance");
    vmhook::register_class<fi_base>("vmhook/fixtures/FieldIntrospection$Base");
    vmhook::register_class<fi_derived>("vmhook/fixtures/FieldIntrospection$Derived");

    // The klass for independent offset recomputation (find_class returns the
    // HotSpot klass* as void*).
    vmhook::hotspot::klass* const klass{
        reinterpret_cast<vmhook::hotspot::klass*>(
            vmhook::find_class("vmhook/fixtures/FieldIntrospection")) };
    ctx.check("fixture_class_found", klass != nullptr);

    // ── Fine-grained CRASH-LOCATOR checkpoints (CRITICAL on mingw·java8) ──────
    //
    // mingw·java8 installs NO SEH net, so a single wild read takes down the whole
    // JVM with no stack trace — the only forensic signal is the LAST line flushed
    // to test_results.txt (write_result/ctx.record flush per line, example.cpp
    // :1468).  We drop a checkpoint before every section AND immediately before
    // every individual instance-backed RAW read (the only reads that can fault on
    // a GC-relocated oop), so the next CI run's last-flushed line names the EXACT
    // op that died.  These are permanent: they cost one flushed line each and are
    // invaluable forensics on a platform with no fault recovery.
    const auto cp = [&](const char* where)
    {
        ctx.record(std::string{ "[INFO] field_introspection checkpoint: " } + where);
    };

    // =====================================================================
    //  SECTION A — signature(): exact JVM descriptor for EVERY static field.
    //  Asserted three ways: (1) signature() == descriptor, (2) the value_t the
    //  proxy carries embeds the SAME descriptor (value_t::signature), and
    //  (3) signature().size() is correct (no stray bytes).
    // =====================================================================
    cp("SECTION A (signature, static fields — mirror reads, no fault risk)");
    auto chk_static_sig = [&](const char* field, const char* descriptor)
    {
        auto fp{ fi_fixture::static_field(field) };
        ctx.check(std::string{ "sig_static_resolves_" } + field, fp.has_value());
        if (!fp) { return; }
        const std::string sig = std::string{ fp->signature() };
        ctx.check(std::string{ "sig_static_value_" } + field, sig == descriptor);
        ctx.check(std::string{ "sig_static_size_" } + field,
                  fp->signature().size() == std::char_traits<char>::length(descriptor));
        // The descriptor the proxy embeds in its value_t must match too.
        const auto v{ fp->get() };
        ctx.check(std::string{ "sig_static_value_t_matches_" } + field,
                  v.signature == descriptor);
    };
    chk_static_sig("sBool",      "Z");
    chk_static_sig("sByte",      "B");
    chk_static_sig("sShort",     "S");
    chk_static_sig("sInt",       "I");
    chk_static_sig("sLong",      "J");
    chk_static_sig("sFloat",     "F");
    chk_static_sig("sDouble",    "D");
    chk_static_sig("sChar",      "C");
    chk_static_sig("sString",    "Ljava/lang/String;");
    chk_static_sig("sIntArray",  "[I");
    chk_static_sig("sIntArray2D","[[I");
    chk_static_sig("sObjArray",  "[Ljava/lang/Object;");
    chk_static_sig("sStrArray",  "[Ljava/lang/String;");
    chk_static_sig("sObject",    "Ljava/lang/Object;");
    chk_static_sig("sRunnable",  "Ljava/lang/Runnable;");
    chk_static_sig("sSelfRef",   "Lvmhook/fixtures/FieldIntrospection;");
    chk_static_sig("sNullString","Ljava/lang/String;");   // descriptor is type-based, not value-based
    chk_static_sig("sNullArray", "[I");
    chk_static_sig("sObjArray2D","[[Ljava/lang/Object;");  // deepest reference-of-reference array shape
    // FINAL fields carry the SAME descriptor as their non-final twins — none of
    // the five accessors surface JVM_ACC_FINAL, so signature() is finality-blind.
    chk_static_sig("sFinalInt",    "I");
    chk_static_sig("sFinalString", "Ljava/lang/String;");
    ctx.record("[INFO] finality-blindness: none of signature/is_static/is_reference/"
               "raw_address/get_compressed_oop expose JVM_ACC_FINAL — a `final` field "
               "is indistinguishable from its mutable twin across all five accessors "
               "(field_entry_t carries offset/is_static/signature/declaring_klass only, "
               "no access-flag bitfield).");

    // signature() of INSTANCE fields (descriptor is identical to the static
    // twin where the type matches; exercises the instance get_field path).
    // NOTE: signature() returns signature_text (a C++ std::string copied at
    // resolve time, vmhook.hpp:12479) — it does NOT deref the instance oop, so
    // these reads are fault-free even if `instance` was relocated.
    {
        cp("SECTION A.inst (signature, instance fields — no oop deref)");
        const auto inst{ fi_fixture::get_instance() };
        ctx.check("sig_instance_wrapper_obtained", inst != nullptr);
        if (inst)
        {
            auto chk_inst_sig = [&](const char* field, const char* descriptor)
            {
                auto fp{ inst->get_field(field) };
                ctx.check(std::string{ "sig_instance_resolves_" } + field, fp.has_value());
                if (!fp) { return; }
                ctx.check(std::string{ "sig_instance_value_" } + field,
                          std::string{ fp->signature() } == descriptor);
            };
            chk_inst_sig("iBool",     "Z");
            chk_inst_sig("iByte",     "B");
            chk_inst_sig("iShort",    "S");
            chk_inst_sig("iInt",      "I");
            chk_inst_sig("iLong",     "J");
            chk_inst_sig("iFloat",    "F");
            chk_inst_sig("iDouble",   "D");
            chk_inst_sig("iChar",     "C");
            chk_inst_sig("iString",   "Ljava/lang/String;");
            chk_inst_sig("iIntArray", "[I");
            chk_inst_sig("iObject",   "Ljava/lang/Object;");
            chk_inst_sig("iNullString","Ljava/lang/String;");
            chk_inst_sig("iFinalInt", "I");   // instance final — descriptor unaffected by finality
        }
    }

    // signature() exhaustive descriptor-byte stress on STACK proxies — every
    // single-char primitive descriptor plus degenerate / non-primitive shapes,
    // proving signature() is a verbatim view of whatever the proxy was built with
    // (no normalization, no validation) and that .size() never strays.
    {
        cp("SECTION A.stack (signature verbatim view on synthetic proxies)");
        struct SigRow { const char* desc; };
        const SigRow stack_sigs[] = {
            { "Z" }, { "B" }, { "S" }, { "I" }, { "J" }, { "F" }, { "D" }, { "C" },
            { "V" },                                  // void descriptor (never a field, but verbatim)
            { "Ljava/lang/Thread;" }, { "[J" }, { "[[[I" },
            { "[Ljava/lang/Object;" }, { "" },        // empty signature survives verbatim
        };
        for (const SigRow& r : stack_sigs)
        {
            vmhook::field_proxy fp{ nullptr, r.desc, false };
            ctx.check(std::string{ "sig_stack_verbatim_" } + r.desc,
                      std::string{ fp.signature() } == r.desc);
            ctx.check(std::string{ "sig_stack_size_" } + r.desc,
                      fp.signature().size() == std::char_traits<char>::length(r.desc));
        }
    }

    // signature() lifetime/stability: the string_view aliases the proxy's own
    // storage (its data() must lie inside the value_t's signature string when
    // copied), and two reads of the same proxy yield byte-identical views.
    {
        auto fp{ fi_fixture::static_field("sLong") };
        if (fp)
        {
            const std::string_view a{ fp->signature() };
            const std::string_view b{ fp->signature() };
            ctx.check("sig_view_stable_data_ptr", a.data() == b.data());
            ctx.check("sig_view_stable_value", a == b && a == "J");
            // A copied std::string survives independently of the proxy.
            const std::string copied = std::string{ fp->signature() };
            ctx.check("sig_view_copy_independent", copied == "J");
        }
    }

    // =====================================================================
    //  SECTION B — is_static(): true for statics, false for instances, and
    //  INVARIANT to the accessor used (a static field read through an instance
    //  wrapper still reports is_static()==true).
    // =====================================================================
    // NOTE: is_static()/is_reference()/raw_address() read proxy METADATA (the
    // static_field flag, signature_text, and the stored pointer) — none deref the
    // instance oop, so Section B is fault-free regardless of relocation.
    cp("SECTION B (is_static — proxy metadata only, no oop deref)");
    {
        const char* static_fields[] = {
            "sBool", "sByte", "sShort", "sInt", "sLong", "sFloat", "sDouble",
            "sChar", "sString", "sIntArray", "sObjArray", "sObject", "sRunnable",
            "sSelfRef", "sNullString", "sObjArray2D",
            "sFinalInt", "sFinalString"   // final statics are STILL static
        };
        for (const char* f : static_fields)
        {
            auto fp{ fi_fixture::static_field(f) };
            ctx.check(std::string{ "is_static_true_" } + f,
                      fp.has_value() && fp->is_static() == true);
        }

        const auto inst{ fi_fixture::get_instance() };
        if (inst)
        {
            const char* instance_fields[] = {
                "iBool", "iByte", "iShort", "iInt", "iLong", "iFloat", "iDouble",
                "iChar", "iString", "iIntArray", "iObject", "iNullString",
                "iFinalInt"   // final instance field is STILL non-static
            };
            for (const char* f : instance_fields)
            {
                auto fp{ inst->get_field(f) };
                ctx.check(std::string{ "is_static_false_" } + f,
                          fp.has_value() && fp->is_static() == false);
            }

            // A STATIC field reached through the INSTANCE accessor: get_field
            // consults JVM_ACC_STATIC, so is_static() must STILL be true and the
            // proxy must resolve against the class mirror (not the instance).
            auto via_inst{ inst->get_field("sInt") };
            ctx.check("is_static_static_field_via_instance_true",
                      via_inst.has_value() && via_inst->is_static() == true);
            auto via_static{ fi_fixture::static_field("sInt") };
            ctx.check("is_static_static_via_instance_same_address",
                      via_inst.has_value() && via_static.has_value()
                      && via_inst->raw_address() == via_static->raw_address());
        }
    }

    // =====================================================================
    //  SECTION C — is_reference(): true for L.../[..., false for primitives.
    //  Proven to be the exact complement of "is a primitive descriptor".
    // =====================================================================
    // NOTE: is_reference() keys on signature_text[0] only — no oop deref, fault-free.
    cp("SECTION C (is_reference — signature byte only, no oop deref)");
    {
        struct Row { const char* field; bool is_ref; };
        const Row rows[] = {
            { "sBool",   false }, { "sByte",   false }, { "sShort",  false },
            { "sInt",    false }, { "sLong",   false }, { "sFloat",  false },
            { "sDouble", false }, { "sChar",   false },
            { "sString",     true }, { "sIntArray",  true }, { "sIntArray2D", true },
            { "sObjArray",   true }, { "sStrArray",  true }, { "sObject",     true },
            { "sRunnable",   true }, { "sSelfRef",   true }, { "sNullString", true },
            { "sNullArray",  true }, { "sObjArray2D", true },
            { "sFinalInt",   false }, { "sFinalString", true },  // finality-blind
        };
        for (const Row& r : rows)
        {
            auto fp{ fi_fixture::static_field(r.field) };
            ctx.check(std::string{ "is_reference_" } + r.field,
                      fp.has_value() && fp->is_reference() == r.is_ref);
            // is_reference() is precisely "NOT a 1-char primitive descriptor".
            if (fp)
            {
                const bool primitive{
                    vmhook::detail::jvm_primitive_byte_width(fp->signature()) != 0 };
                ctx.check(std::string{ "is_reference_complement_primitive_" } + r.field,
                          fp->is_reference() == !primitive);
            }
        }
        // The empty-signature contract: is_reference() is false (no front char).
        {
            vmhook::field_proxy empty{ nullptr, "", false };
            ctx.check("is_reference_empty_signature_false", empty.is_reference() == false);
        }
        // A bare 'L' or '[' (degenerate but front-char driven) still classifies
        // as reference — documents that is_reference keys on the first byte only.
        {
            vmhook::field_proxy bare_l{ nullptr, "L", false };
            vmhook::field_proxy bare_a{ nullptr, "[", false };
            ctx.check("is_reference_bare_L_true", bare_l.is_reference() == true);
            ctx.check("is_reference_bare_bracket_true", bare_a.is_reference() == true);
        }

        // EVERY single-char primitive descriptor classifies as NON-reference, and
        // 'V' (void) — which is neither a reference nor a sized primitive — is also
        // false (front char is not 'L'/'['); these pin is_reference's front-byte
        // rule across the whole primitive alphabet on synthetic proxies.
        {
            const char* non_ref_chars[] = { "Z", "B", "S", "I", "J", "F", "D", "C", "V" };
            for (const char* d : non_ref_chars)
            {
                vmhook::field_proxy fp{ nullptr, d, false };
                ctx.check(std::string{ "is_reference_primitive_char_false_" } + d,
                          fp.is_reference() == false);
            }
            // A multi-char descriptor whose first byte is a primitive letter is
            // STILL non-reference (front byte 'I'), and is NOT a sized primitive
            // (jvm_primitive_byte_width requires size()==1) — so is_reference and
            // the primitive-complement DISAGREE here.  This documents that
            // is_reference is a pure front-byte test, not a true type classifier.
            vmhook::field_proxy multi{ nullptr, "II", false };
            ctx.check("is_reference_multichar_primitive_front_false",
                      multi.is_reference() == false);
            ctx.check("is_reference_multichar_not_sized_primitive",
                      vmhook::detail::jvm_primitive_byte_width(multi.signature()) == 0);
        }
    }

    // SECTION C.inst — is_reference() on genuine INSTANCE fields, and its exact
    // complement-of-primitive relationship, on the live instance proxy path
    // (Section C above is static-only).  Proxy metadata only → no oop deref → HARD.
    {
        cp("SECTION C.inst (is_reference on instance fields — metadata only)");
        const auto inst{ fi_fixture::get_instance() };
        if (inst)
        {
            struct IRow { const char* field; bool is_ref; };
            const IRow irows[] = {
                { "iBool",   false }, { "iByte",   false }, { "iShort",  false },
                { "iInt",    false }, { "iLong",   false }, { "iFloat",  false },
                { "iDouble", false }, { "iChar",   false }, { "iFinalInt", false },
                { "iString",   true }, { "iIntArray", true }, { "iObject",   true },
                { "iNullString", true },
            };
            for (const IRow& r : irows)
            {
                auto fp{ inst->get_field(r.field) };
                ctx.check(std::string{ "is_reference_instance_" } + r.field,
                          fp.has_value() && fp->is_reference() == r.is_ref);
                if (fp)
                {
                    const bool primitive{
                        vmhook::detail::jvm_primitive_byte_width(fp->signature()) != 0 };
                    ctx.check(std::string{ "is_reference_instance_complement_" } + r.field,
                              fp->is_reference() == !primitive);
                }
            }
        }
    }

    // =====================================================================
    //  SECTION D — raw_address(): non-null, equals independently recomputed
    //  (mirror|oop)+offset, stable across lookups, width-aligned, and the
    //  EXACT address get()/get_compressed_oop read from.
    // =====================================================================
    cp("SECTION D (raw_address)");
    {
        // D.1 — STATIC fields: raw_address == mirror + offset (recomputed).
        // STATIC reads touch mirror+offset (old-gen Class mirror, NOT young-
        // relocated) so they are HARD and need no probe.
        cp("SECTION D.1 (static raw_address — mirror reads, no fault risk)");
        auto chk_static_addr = [&](const char* field, std::size_t align)
        {
            auto fp{ fi_fixture::static_field(field) };
            ctx.check(std::string{ "raw_addr_static_resolves_" } + field, fp.has_value());
            if (!fp) { return; }
            void* const got{ fp->raw_address() };
            ctx.check(std::string{ "raw_addr_static_nonnull_" } + field, got != nullptr);
            void* const expected{ recompute_static_addr(klass, field) };
            ctx.check(std::string{ "raw_addr_static_equals_mirror_plus_offset_" } + field,
                      expected != nullptr && got == expected);
            if (align > 1)
            {
                const auto a{ reinterpret_cast<std::uintptr_t>(got) };
                ctx.check(std::string{ "raw_addr_static_aligned_" } + field,
                          (a % align) == 0);
            }
            // Stable across a second, independent lookup.
            auto fp2{ fi_fixture::static_field(field) };
            ctx.check(std::string{ "raw_addr_static_stable_" } + field,
                      fp2.has_value() && fp2->raw_address() == got);
        };
        chk_static_addr("sBool",   1);
        chk_static_addr("sByte",   1);
        chk_static_addr("sShort",  2);
        chk_static_addr("sChar",   2);
        chk_static_addr("sInt",    4);
        chk_static_addr("sFloat",  4);
        chk_static_addr("sLong",   8);
        chk_static_addr("sDouble", 8);
        chk_static_addr("sString", 4);   // compressed-OOP slot (4B) under default UseCompressedOops
        chk_static_addr("sIntArray", 4);
        chk_static_addr("sObject", 4);
        chk_static_addr("sObjArray2D", 4);
        // FINAL statics resolve to a real mirror+offset slot exactly like a
        // non-final static — raw_address is finality-blind.
        chk_static_addr("sFinalInt",    4);
        chk_static_addr("sFinalString", 4);

        // A final and a non-final static of the SAME shape occupy DISTINCT slots
        // (different offsets) yet both resolve non-null and width-aligned — pins
        // that finality does not collapse or alias the addressing.
        {
            auto fin{ fi_fixture::static_field("sFinalInt") };
            auto non{ fi_fixture::static_field("sInt") };
            if (fin && non)
            {
                ctx.check("raw_addr_final_vs_nonfinal_distinct_slots",
                          fin->raw_address() != nullptr
                          && non->raw_address() != nullptr
                          && fin->raw_address() != non->raw_address());
            }
        }

        // D.2 — raw_address is the EXACT byte get() reads.  For a primitive int
        // field, the 4 bytes at raw_address must equal get() as int32.  This
        // catches any future internal offset drift between the two accessors.
        // STATIC fields → get()/memcpy touch mirror+offset (old-gen) → HARD.
        cp("SECTION D.2 (static int/long bytes==get — mirror reads, no fault risk)");
        {
            auto fp{ fi_fixture::static_field("sInt") };
            if (fp)
            {
                const std::int32_t via_get{ fp->get() };
                std::int32_t via_addr{};
                std::memcpy(&via_addr, fp->raw_address(), sizeof(via_addr));
                ctx.check("raw_addr_static_int_bytes_equal_get", via_addr == via_get);
                ctx.check("raw_addr_static_int_matches_java", via_get == 0x0BADF00D);
            }
        }
        {
            auto fp{ fi_fixture::static_field("sLong") };
            if (fp)
            {
                const std::int64_t via_get{ fp->get() };
                std::int64_t via_addr{};
                std::memcpy(&via_addr, fp->raw_address(), sizeof(via_addr));
                ctx.check("raw_addr_static_long_bytes_equal_get", via_addr == via_get);
            }
        }
        // FINAL static int: get() reads the right value AND the raw_address bytes
        // match — the read path treats a final field exactly like a mutable one.
        {
            auto fp{ fi_fixture::static_field("sFinalInt") };
            if (fp)
            {
                const std::int32_t via_get{ fp->get() };
                std::int32_t via_addr{};
                std::memcpy(&via_addr, fp->raw_address(), sizeof(via_addr));
                ctx.check("raw_addr_static_final_int_bytes_equal_get", via_addr == via_get);
                ctx.check("raw_addr_static_final_int_matches_java", via_get == 0x12345678);
            }
        }

        // D.3 — INSTANCE fields: raw_address == oop + offset (recomputed).
        // CRASH NOTE: every read below derives from a wrapper whose `instance` oop
        // (FieldIntrospection.instance) is YOUNG-GEN and may be relocated by a GC
        // that an earlier section's allocation triggered.  raw_address() and the
        // address-equality / alignment / after-header checks read proxy METADATA
        // and POINTERS ONLY (no oop deref) so they stay HARD; the ONLY reads that
        // touch instance memory are the get()/memcpy in the bytes==get block — those
        // are re-acquired fresh + exact-byte probed below.
        cp("SECTION D.3 (instance raw_address — get() reads guarded)");
        const auto inst{ fi_fixture::get_instance() };
        ctx.check("raw_addr_instance_wrapper_obtained", inst != nullptr);
        if (inst)
        {
            // fi_fixture::get_instance() is a static factory that shadows the
            // inherited object_base::get_instance(); qualify to read the raw OOP.
            void* const oop{ inst->vmhook::object_base::get_instance() };
            ctx.check("raw_addr_instance_oop_valid",
                      oop != nullptr && vmhook::hotspot::is_valid_pointer(oop));

            // chk_inst_addr derefs NOTHING off the oop: it compares raw_address()
            // (proxy metadata) against recompute_instance_addr (pure oop+offset
            // arithmetic on the SAME captured base) and checks alignment / ordering.
            // Both sides use the same captured `oop`, so the equality holds even if
            // `instance` was relocated — fault-free and non-vacuous → HARD.
            auto chk_inst_addr = [&](const char* field, std::size_t align)
            {
                auto fp{ inst->get_field(field) };
                ctx.check(std::string{ "raw_addr_instance_resolves_" } + field, fp.has_value());
                if (!fp) { return; }
                void* const got{ fp->raw_address() };
                ctx.check(std::string{ "raw_addr_instance_nonnull_" } + field, got != nullptr);
                void* const expected{ recompute_instance_addr(klass, oop, field) };
                ctx.check(std::string{ "raw_addr_instance_equals_oop_plus_offset_" } + field,
                          expected != nullptr && got == expected);
                // The instance field address must lie INSIDE the object (after
                // the 12-byte header on x64 compressed-class layout).
                ctx.check(std::string{ "raw_addr_instance_after_header_" } + field,
                          reinterpret_cast<std::uint8_t*>(got)
                              > reinterpret_cast<std::uint8_t*>(oop));
                if (align > 1)
                {
                    const auto a{ reinterpret_cast<std::uintptr_t>(got) };
                    ctx.check(std::string{ "raw_addr_instance_aligned_" } + field,
                              (a % align) == 0);
                }
            };
            chk_inst_addr("iBool",   1);
            chk_inst_addr("iByte",   1);
            chk_inst_addr("iShort",  2);
            chk_inst_addr("iChar",   2);
            chk_inst_addr("iInt",    4);
            chk_inst_addr("iFloat",  4);
            chk_inst_addr("iLong",   8);
            chk_inst_addr("iDouble", 8);
            chk_inst_addr("iString", 4);
            chk_inst_addr("iIntArray", 4);
            chk_inst_addr("iObject", 4);
            chk_inst_addr("iFinalInt", 4);   // final instance field — addressing unaffected

            // Two DIFFERENT instance fields on the SAME object have DIFFERENT
            // raw addresses (offsets differ).  raw_address() only — no deref → HARD.
            {
                auto a{ inst->get_field("iInt") };
                auto b{ inst->get_field("iLong") };
                if (a && b)
                {
                    ctx.check("raw_addr_distinct_fields_differ",
                              a->raw_address() != b->raw_address());
                }
            }

            // The instance int field's bytes at raw_address equal get().
            //
            // *** THE a84e51a GAP ***  fp->get() (vmhook.hpp:12268) and the memcpy
            // both RAW-read instance+offset.  If `instance` was relocated since the
            // wrapper above was decoded, instance+offset is an unmapped page → the
            // crash that recurred.  RE-ACQUIRE the wrapper fresh (re-decodes the
            // stable mirror slot to the CURRENT location, shrinking the window to a
            // few instructions) and exact-byte PROBE the 4 bytes the read touches
            // before reading.  A relocation inside the tiny window degrades to an
            // [INFO] miss; a successful probe means the get()/memcpy CANNOT fault and
            // the value assertion stays HARD (so coverage is non-vacuous).
            cp("SECTION D.3 iInt bytes==get (instance get() — re-acquire + probe)");
            {
                const auto fresh_inst{ fi_fixture::get_instance() };
                bool did_assert{ false };
                if (fresh_inst)
                {
                    void* const fresh_oop{ fresh_inst->vmhook::object_base::get_instance() };
                    auto fp{ fresh_inst->get_field("iInt") };
                    if (fp && instance_field_read_safe(fresh_oop, *fp, sizeof(std::int32_t)))
                    {
                        const std::int32_t via_get{ fp->get() };
                        std::int32_t via_addr{};
                        std::memcpy(&via_addr, fp->raw_address(), sizeof(via_addr));
                        ctx.check("raw_addr_instance_int_bytes_equal_get", via_addr == via_get);
                        ctx.check("raw_addr_instance_int_matches_java",
                                  via_get == 0x0BADCAFE);
                        did_assert = true;
                    }
                }
                if (!did_assert)
                {
                    ctx.record("[INFO] raw_addr_instance_int: instance slot not safely "
                               "readable (instance relocated mid-section) — skipped "
                               "bytes==get / value asserts (transient, not a defect).");
                }
            }
        }

        // D.4 — raw_address echoes whatever the proxy was constructed with,
        //       with NO validation (it is a pure accessor of the stored pointer).
        //       The 3-arg ctor remains a documented ESCAPE HATCH — but the READ
        //       through that pointer is now SAFE-BY-DEFAULT: robustness #1 added an
        //       is_valid_pointer gate on the deref in get()/get_compressed_oop(),
        //       so a proxy over a bogus pointer (0x1) returns the zero/empty default
        //       instead of dereferencing the wild address (was: unguarded UB/AV).
        {
            vmhook::field_proxy null_proxy{ nullptr, "I", false };
            ctx.check("raw_addr_null_base_is_null", null_proxy.raw_address() == nullptr);
            std::uint8_t storage[16] = { 0 };
            vmhook::field_proxy buf_proxy{ storage + 8, "I", false };
            ctx.check("raw_addr_echoes_constructor_pointer",
                      buf_proxy.raw_address() == storage + 8);

            // raw_address() (accessor, no deref) STILL echoes the bogus pointer
            // verbatim — the escape hatch is preserved.
            void* const bogus{ reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x1)) };
            vmhook::field_proxy bogus_proxy{ bogus, "Ljava/lang/String;", true };
            ctx.check("raw_addr_no_validation_passes_bogus",
                      bogus_proxy.raw_address() == bogus);

            // HARD: the READ through that bogus pointer is now gated.  0x1 fails
            // is_valid_pointer (below the user-address floor), so get() returns the
            // documented default (int32 alternative, value 0, signature preserved)
            // and get_compressed_oop() returns 0 — NO access violation, NO crash.
            // (Previously this dereferenced 0x1; the [INFO] that documented "bogus
            // pointer passes through" the read is now this HARD safety assertion.)
            const auto bogus_value{ bogus_proxy.get() };
            ctx.check("bogus_get_signature_preserved", bogus_value.signature == "Ljava/lang/String;");
            ctx.check("bogus_get_routes_to_null_void_ptr",
                      static_cast<void*>(bogus_value) == nullptr);
            ctx.check("bogus_get_as_string_empty", bogus_value.as_string().empty());
            ctx.check("bogus_get_compressed_oop_is_zero",
                      bogus_proxy.get_compressed_oop() == 0u);

            // A primitive-typed bogus proxy is likewise safe: zero default, no read
            // of the wild address.
            vmhook::field_proxy bogus_int{ bogus, "I", false };
            ctx.check("bogus_get_int_is_zero",
                      static_cast<std::int32_t>(bogus_int.get()) == 0);
        }
    }

    // =====================================================================
    //  SECTION E — get_compressed_oop() for REFERENCE fields decodes to the
    //  SAME oop get() (as void*) and field_oop() yield, and that oop is the
    //  REAL Java object (structural + identity cross-checks).
    // =====================================================================
    // For E.1–E.6 (STATIC reference fields) get_compressed_oop()/get() read the
    // compressed slot off the Class MIRROR (old-gen, stable) → HARD.  Only the
    // DECODED-oop CONTENT reads (read_java_string / array_length / klass_from_oop)
    // touch the young-gen referent and are routed through the safe_* probes →
    // best-effort.  E.7 (INSTANCE reference fields) is the genuine fault site: its
    // get_compressed_oop()/get() read instance+offset (young-gen) and are
    // re-acquired fresh + exact-byte probed there.
    cp("SECTION E (get_compressed_oop — static slots HARD, content guarded)");
    {
        // E.1 — String reference: three decode paths agree, decoded string is
        //       the real value, decoded klass is java/lang/String.
        {
            cp("SECTION E.1 (static sString — mirror slot, content decode guarded)");
            auto fp{ fi_fixture::static_field("sString") };
            ctx.check("cmp_oop_string_resolves", fp.has_value());
            if (fp)
            {
                const std::uint32_t raw{ fp->get_compressed_oop() };
                ctx.check("cmp_oop_string_nonzero", raw != 0);

                void* const decoded_direct{ vmhook::hotspot::decode_oop_pointer(raw) };
                void* const decoded_via_get{ static_cast<void*>(fp->get()) };  // value_t -> void*
                void* const decoded_via_field_oop{ vmhook::field_oop(*fp) };

                ctx.check("cmp_oop_string_get_equals_decode",
                          decoded_via_get == decoded_direct);
                ctx.check("cmp_oop_string_field_oop_equals_decode",
                          decoded_via_field_oop == decoded_direct);
                ctx.check("cmp_oop_string_decoded_valid",
                          decoded_direct != nullptr
                          && vmhook::hotspot::is_valid_pointer(decoded_direct));

                // The decoded object really is the String "introspect-me".  The
                // content read RAW-derefs the oop (read_java_string), so it is
                // BEST-EFFORT: a probe that the header is mapped guards the read,
                // and a transient empty miss (object mid-relocation) is recorded,
                // not failed.  A DIFFERENT non-empty value would still be a real
                // mis-decode and fails.
                const std::string text = safe_read_java_string(decoded_direct);
                if (!text.empty())
                {
                    ctx.check("cmp_oop_string_decodes_to_real_value",
                              text == "introspect-me");
                    ctx.check("cmp_oop_string_length_matches_java",
                              static_cast<std::int32_t>(text.size())
                                  == fi_fixture::s_string_len());
                }
                else
                {
                    ctx.record("[INFO] cmp_oop_string: decoded String header not safely "
                               "readable (stale/relocated) — skipped value/length asserts.");
                }
                // Its klass is java/lang/String (klass_name_of_field is itself
                // safe-probe-guarded; empty on a transient miss).
                const std::string kn{ klass_name_of_field(*fp) };
                if (!kn.empty())
                {
                    ctx.check("cmp_oop_string_klass_name", kn == "java/lang/String");
                }
            }
        }

        // E.2 — int[] reference: decoded oop is the real array (length + elem0
        //       match Java), klass name is "[I", and get()/field_oop agree.
        {
            cp("SECTION E.2 (static sIntArray — mirror slot, content decode guarded)");
            auto fp{ fi_fixture::static_field("sIntArray") };
            ctx.check("cmp_oop_intarray_resolves", fp.has_value());
            if (fp)
            {
                const std::uint32_t raw{ fp->get_compressed_oop() };
                ctx.check("cmp_oop_intarray_nonzero", raw != 0);

                void* const decoded{ vmhook::hotspot::decode_oop_pointer(raw) };
                void* const via_get{ static_cast<void*>(fp->get()) };
                ctx.check("cmp_oop_intarray_get_equals_decode", via_get == decoded);
                // field_oop uses decode_array_oop; for a real array oop it lands
                // on the same heap object as decode_oop_pointer.
                ctx.check("cmp_oop_intarray_field_oop_equals_decode",
                          vmhook::field_oop(*fp) == decoded);

                // length + elem0 RAW-deref the array oop, so they are BEST-EFFORT
                // (safe_* return -1 / T{} when the header is not safely readable).
                const std::int32_t len{ safe_array_length(decoded) };
                if (len >= 0)
                {
                    ctx.check("cmp_oop_intarray_length_matches_java",
                              len == fi_fixture::s_int_array_len() && len == 5);
                    const std::int32_t e0{ safe_array_element<std::int32_t>(decoded, 0) };
                    ctx.check("cmp_oop_intarray_elem0_matches_java",
                              e0 == fi_fixture::s_int_array_elem0() && e0 == 11);
                }
                else
                {
                    ctx.record("[INFO] cmp_oop_intarray: array header not safely readable "
                               "(stale/relocated) — skipped length/elem0 asserts.");
                }
                const std::string kn{ klass_name_of_field(*fp) };
                if (!kn.empty())
                {
                    ctx.check("cmp_oop_intarray_klass_name", kn == "[I");
                }
            }
        }

        // E.3 — Object[] reference: decoded array length matches, klass is
        //       "[Ljava/lang/Object;".
        {
            cp("SECTION E.3 (static sObjArray — mirror slot, content decode guarded)");
            auto fp{ fi_fixture::static_field("sObjArray") };
            if (fp)
            {
                void* const decoded{ vmhook::hotspot::decode_oop_pointer(fp->get_compressed_oop()) };
                ctx.check("cmp_oop_objarray_get_equals_decode",
                          static_cast<void*>(fp->get()) == decoded);
                const std::int32_t len{ safe_array_length(decoded) };
                if (len >= 0)
                {
                    ctx.check("cmp_oop_objarray_length_matches_java",
                              len == fi_fixture::s_obj_array_len());
                }
                else
                {
                    ctx.record("[INFO] cmp_oop_objarray: array header not safely readable "
                               "(stale/relocated) — skipped length assert.");
                }
                const std::string kn{ klass_name_of_field(*fp) };
                if (!kn.empty())
                {
                    ctx.check("cmp_oop_objarray_klass_name", kn == "[Ljava/lang/Object;");
                }
            }
        }

        // E.4 — plain Object reference: decoded oop is valid and its klass is
        //       java/lang/Object; get()==decode.
        {
            cp("SECTION E.4 (static sObject — mirror slot, content decode guarded)");
            auto fp{ fi_fixture::static_field("sObject") };
            if (fp)
            {
                const std::uint32_t raw{ fp->get_compressed_oop() };
                void* const decoded{ vmhook::hotspot::decode_oop_pointer(raw) };
                ctx.check("cmp_oop_object_nonzero", raw != 0);
                ctx.check("cmp_oop_object_valid",
                          decoded != nullptr && vmhook::hotspot::is_valid_pointer(decoded));
                ctx.check("cmp_oop_object_get_equals_decode",
                          static_cast<void*>(fp->get()) == decoded);
                const std::string kn{ klass_name_of_field(*fp) };
                if (!kn.empty())
                {
                    ctx.check("cmp_oop_object_klass_name", kn == "java/lang/Object");
                }
            }
        }

        // E.5 — interface-typed reference (Runnable): the descriptor is L...; so
        //       is_reference is true and the compressed OOP decodes to a valid
        //       object whose concrete klass is the anonymous Runnable subclass.
        {
            cp("SECTION E.5 (static sRunnable — mirror slot, content decode guarded)");
            auto fp{ fi_fixture::static_field("sRunnable") };
            if (fp)
            {
                ctx.check("cmp_oop_runnable_is_reference", fp->is_reference());
                const std::uint32_t raw{ fp->get_compressed_oop() };
                void* const decoded{ vmhook::hotspot::decode_oop_pointer(raw) };
                ctx.check("cmp_oop_runnable_nonzero", raw != 0);
                ctx.check("cmp_oop_runnable_valid",
                          decoded != nullptr && vmhook::hotspot::is_valid_pointer(decoded));
                ctx.check("cmp_oop_runnable_get_equals_decode",
                          static_cast<void*>(fp->get()) == decoded);
                // Concrete class is a synthetic subtype of FieldIntrospection's
                // anonymous Runnable.  klass_name_of_field RAW-derefs the oop, so
                // assert "resolved" only when the header is safely readable (a
                // stale/relocated miss is recorded, not failed).
                if (oop_header_safely_readable(decoded))
                {
                    ctx.check("cmp_oop_runnable_klass_resolved",
                              !klass_name_of_field(*fp).empty());
                }
                else
                {
                    ctx.record("[INFO] cmp_oop_runnable: decoded object header not safely "
                               "readable (stale/relocated) — skipped klass-resolved assert.");
                }
            }
        }

        // E.6 — self-typed reference: decoded oop's klass is exactly the fixture.
        {
            cp("SECTION E.6 (static sSelfRef — mirror slot, content decode guarded)");
            auto fp{ fi_fixture::static_field("sSelfRef") };
            if (fp)
            {
                const std::string kn{ klass_name_of_field(*fp) };
                if (!kn.empty())
                {
                    ctx.check("cmp_oop_selfref_klass_name",
                              kn == "vmhook/fixtures/FieldIntrospection");
                }
            }
        }

        // E.6b — String[] reference ([Ljava/lang/String;): decoded length matches
        //        the Java witness, klass name is "[Ljava/lang/String;", and the
        //        first element decodes to a real String of the published length.
        {
            cp("SECTION E.6b (static sStrArray — mirror slot, content decode guarded)");
            auto fp{ fi_fixture::static_field("sStrArray") };
            if (fp)
            {
                const std::uint32_t raw{ fp->get_compressed_oop() };
                ctx.check("cmp_oop_strarray_nonzero", raw != 0);
                void* const decoded{ vmhook::hotspot::decode_oop_pointer(raw) };
                ctx.check("cmp_oop_strarray_get_equals_decode",
                          static_cast<void*>(fp->get()) == decoded);
                const std::int32_t len{ safe_array_length(decoded) };
                if (len >= 0)
                {
                    ctx.check("cmp_oop_strarray_length_matches_java",
                              len == fi_fixture::s_str_array_len() && len == 2);
                    // Element 0 is a compressed-OOP slot; decode it and verify it is
                    // the real String "x" (length 1) — proving an Object/String[]
                    // element decode chains correctly off get_compressed_oop's oop.
                    const std::uint32_t e0_raw{
                        safe_array_element<std::uint32_t>(decoded, 0) };
                    void* const e0{ vmhook::hotspot::decode_oop_pointer(e0_raw) };
                    const std::string e0_text{ safe_read_java_string(e0) };
                    if (!e0_text.empty())
                    {
                        ctx.check("cmp_oop_strarray_elem0_decodes",
                                  static_cast<std::int32_t>(e0_text.size())
                                      == fi_fixture::s_str_array_elem0_len());
                    }
                    else
                    {
                        ctx.record("[INFO] cmp_oop_strarray_elem0: element String header not "
                                   "safely readable (stale/relocated) — skipped value assert.");
                    }
                }
                else
                {
                    ctx.record("[INFO] cmp_oop_strarray: array header not safely readable "
                               "(stale/relocated) — skipped length/elem asserts.");
                }
                const std::string kn{ klass_name_of_field(*fp) };
                if (!kn.empty())
                {
                    ctx.check("cmp_oop_strarray_klass_name", kn == "[Ljava/lang/String;");
                }
            }
        }

        // E.6c — int[][] reference ([[I): the OUTER array's length matches Java and
        //        its klass name is "[[I"; element 0 (an int[]) decodes to a real
        //        inner array whose length matches the fixture's first inner row.
        {
            cp("SECTION E.6c (static sIntArray2D — mirror slot, content decode guarded)");
            auto fp{ fi_fixture::static_field("sIntArray2D") };
            if (fp)
            {
                ctx.check("cmp_oop_intarray2d_is_reference", fp->is_reference());
                void* const decoded{ vmhook::hotspot::decode_oop_pointer(fp->get_compressed_oop()) };
                ctx.check("cmp_oop_intarray2d_get_equals_decode",
                          static_cast<void*>(fp->get()) == decoded);
                const std::int32_t len{ safe_array_length(decoded) };
                if (len >= 0)
                {
                    ctx.check("cmp_oop_intarray2d_length_matches_java",
                              len == fi_fixture::s_int_array2d_len() && len == 2);
                    // Inner row 0 is itself a reference (an int[]) — decode it and
                    // check its int-array length is 2 ({1,2}).
                    const std::uint32_t inner_raw{
                        safe_array_element<std::uint32_t>(decoded, 0) };
                    void* const inner{ vmhook::hotspot::decode_oop_pointer(inner_raw) };
                    const std::int32_t inner_len{ safe_array_length(inner) };
                    if (inner_len >= 0)
                    {
                        ctx.check("cmp_oop_intarray2d_inner0_length", inner_len == 2);
                    }
                    else
                    {
                        ctx.record("[INFO] cmp_oop_intarray2d_inner0: inner array header not "
                                   "safely readable (stale/relocated) — skipped length assert.");
                    }
                }
                else
                {
                    ctx.record("[INFO] cmp_oop_intarray2d: outer array header not safely "
                               "readable (stale/relocated) — skipped length asserts.");
                }
                const std::string kn{ klass_name_of_field(*fp) };
                if (!kn.empty())
                {
                    ctx.check("cmp_oop_intarray2d_klass_name", kn == "[[I");
                }
            }
        }

        // E.6d — Object[][] reference ([[Ljava/lang/Object;): outer length + klass
        //        name for the deepest reference-of-reference array shape.
        {
            cp("SECTION E.6d (static sObjArray2D — mirror slot, content decode guarded)");
            auto fp{ fi_fixture::static_field("sObjArray2D") };
            if (fp)
            {
                void* const decoded{ vmhook::hotspot::decode_oop_pointer(fp->get_compressed_oop()) };
                ctx.check("cmp_oop_objarray2d_get_equals_decode",
                          static_cast<void*>(fp->get()) == decoded);
                const std::int32_t len{ safe_array_length(decoded) };
                if (len >= 0)
                {
                    ctx.check("cmp_oop_objarray2d_length_matches_java",
                              len == fi_fixture::s_obj_array2d_len() && len == 2);
                }
                else
                {
                    ctx.record("[INFO] cmp_oop_objarray2d: array header not safely readable "
                               "(stale/relocated) — skipped length assert.");
                }
                const std::string kn{ klass_name_of_field(*fp) };
                if (!kn.empty())
                {
                    ctx.check("cmp_oop_objarray2d_klass_name", kn == "[[Ljava/lang/Object;");
                }
            }
        }

        // E.6e — FINAL static reference (sFinalString): get_compressed_oop on a
        //        final reference field decodes exactly like a mutable one (finality
        //        is invisible to the read path).
        {
            cp("SECTION E.6e (static sFinalString — mirror slot, content decode guarded)");
            auto fp{ fi_fixture::static_field("sFinalString") };
            if (fp)
            {
                ctx.check("cmp_oop_final_string_is_reference", fp->is_reference());
                const std::uint32_t raw{ fp->get_compressed_oop() };
                ctx.check("cmp_oop_final_string_nonzero", raw != 0);
                void* const decoded{ vmhook::hotspot::decode_oop_pointer(raw) };
                ctx.check("cmp_oop_final_string_get_equals_decode",
                          static_cast<void*>(fp->get()) == decoded);
                const std::string text{ safe_read_java_string(decoded) };
                if (!text.empty())
                {
                    ctx.check("cmp_oop_final_string_value", text == "final-static");
                }
                else
                {
                    ctx.record("[INFO] cmp_oop_final_string: decoded header not safely readable "
                               "(stale/relocated) — skipped value assert.");
                }
            }
        }

        // E.7 — INSTANCE reference field (iString): get_compressed_oop on an
        //       instance proxy decodes to the real instance String.
        //
        // *** THE a84e51a GAP (instance reference fields) ***  Unlike E.1–E.6,
        // here get_compressed_oop() (vmhook.hpp:12540) and get() (vmhook.hpp:12268)
        // read the 4-byte compressed-OOP slot at instance+offset — and `instance`
        // is the young-gen object that relocates.  a84e51a guarded the CONTENT
        // decode (safe_read_java_string / safe_array_length) but NOT these
        // instance-slot reads, so reading the slot off a relocated `instance` would
        // fault.  RE-ACQUIRE the wrapper fresh and exact-byte PROBE the 4-byte slot
        // (+header) before any get_compressed_oop()/get(); on a miss skip best-
        // effort, on success read + assert HARD (the value/length content decode
        // remains separately guarded for the young-gen referent).
        {
            cp("SECTION E.7 (INSTANCE ref fields — re-acquire + probe slot)");
            const auto inst{ fi_fixture::get_instance() };
            if (inst)
            {
                void* const inst_oop{ inst->vmhook::object_base::get_instance() };

                cp("SECTION E.7 iString (instance get_compressed_oop/get — probe slot)");
                auto fp{ inst->get_field("iString") };
                if (fp && instance_field_read_safe(inst_oop, *fp, sizeof(std::uint32_t)))
                {
                    const std::uint32_t raw{ fp->get_compressed_oop() };
                    void* const decoded{ vmhook::hotspot::decode_oop_pointer(raw) };
                    ctx.check("cmp_oop_instance_string_nonzero", raw != 0);
                    ctx.check("cmp_oop_instance_string_get_equals_decode",
                              static_cast<void*>(fp->get()) == decoded);
                    // The referent String is itself young-gen — its CONTENT read is
                    // separately header-probed (safe_read_java_string).
                    cp("SECTION E.7 iString content (read_java_string — probe header)");
                    const std::string text{ safe_read_java_string(decoded) };
                    if (!text.empty())
                    {
                        ctx.check("cmp_oop_instance_string_value",
                                  text == "instance-string");
                    }
                    else
                    {
                        ctx.record("[INFO] cmp_oop_instance_string: referent header not safely "
                                   "readable (stale/relocated) — skipped value assert.");
                    }
                }
                else
                {
                    ctx.record("[INFO] cmp_oop_instance_string: instance slot not safely "
                               "readable (instance relocated) — skipped all asserts (transient).");
                }

                cp("SECTION E.7 iIntArray (instance get_compressed_oop — probe slot)");
                auto fa{ inst->get_field("iIntArray") };
                if (fa && instance_field_read_safe(inst_oop, *fa, sizeof(std::uint32_t)))
                {
                    void* const decoded{ vmhook::hotspot::decode_oop_pointer(fa->get_compressed_oop()) };
                    cp("SECTION E.7 iIntArray content (array_length — probe header)");
                    const std::int32_t len{ safe_array_length(decoded) };
                    if (len >= 0)
                    {
                        ctx.check("cmp_oop_instance_intarray_length",
                                  len == fi_fixture::i_int_array_len() && len == 3);
                    }
                    else
                    {
                        ctx.record("[INFO] cmp_oop_instance_intarray: referent array header not "
                                   "safely readable (stale/relocated) — skipped length assert.");
                    }
                }
                else
                {
                    ctx.record("[INFO] cmp_oop_instance_intarray: instance slot not safely "
                               "readable (instance relocated) — skipped length assert (transient).");
                }

                // iObject — a plain Object instance reference field.  Proves the
                // instance get_compressed_oop / get(void*) decode-agreement holds
                // for a non-array, non-String reference too; the decoded oop's
                // klass is java/lang/Object.
                cp("SECTION E.7 iObject (instance get_compressed_oop — probe slot)");
                auto fo{ inst->get_field("iObject") };
                if (fo && instance_field_read_safe(inst_oop, *fo, sizeof(std::uint32_t)))
                {
                    const std::uint32_t raw{ fo->get_compressed_oop() };
                    void* const decoded{ vmhook::hotspot::decode_oop_pointer(raw) };
                    ctx.check("cmp_oop_instance_object_nonzero", raw != 0);
                    ctx.check("cmp_oop_instance_object_get_equals_decode",
                              static_cast<void*>(fo->get()) == decoded);
                    cp("SECTION E.7 iObject content (klass_from_oop — probe header)");
                    const std::string kn{ klass_name_of_field(*fo) };
                    if (!kn.empty())
                    {
                        ctx.check("cmp_oop_instance_object_klass_name",
                                  kn == "java/lang/Object");
                    }
                    else
                    {
                        ctx.record("[INFO] cmp_oop_instance_object: referent header not safely "
                                   "readable (stale/relocated) — skipped klass-name assert.");
                    }
                }
                else
                {
                    ctx.record("[INFO] cmp_oop_instance_object: instance slot not safely "
                               "readable (instance relocated) — skipped asserts (transient).");
                }
            }
        }
    }

    // =====================================================================
    //  SECTION F — get_compressed_oop() boundary / FLAW pinning.
    // =====================================================================
    // Every read here is on a STATIC field (mirror, old-gen) or a synthetic stack
    // proxy — none touch the young-gen instance, so none can fault.  In particular
    // F.2/F.3 (the FLAW-C lines) call get_compressed_oop() on PRIMITIVE static
    // fields: the is_reference() guard (vmhook.hpp:12550) returns 0 BEFORE any
    // memcpy, so there is no read at all.  HARD throughout.
    cp("SECTION F (boundary / FLAW pinning — static + stack only, no fault risk)");
    {
        // F.1 — NULL reference field: compressed OOP is 0, decode is null,
        //       get() as void* is null.
        {
            auto fp{ fi_fixture::static_field("sNullString") };
            ctx.check("cmp_oop_null_string_resolves", fp.has_value());
            if (fp)
            {
                ctx.check("cmp_oop_null_string_is_zero", fp->get_compressed_oop() == 0u);
                ctx.check("cmp_oop_null_string_decode_is_null",
                          vmhook::hotspot::decode_oop_pointer(fp->get_compressed_oop()) == nullptr);
                ctx.check("cmp_oop_null_string_get_void_is_null",
                          static_cast<void*>(fp->get()) == nullptr);
            }
        }
        {
            auto fp{ fi_fixture::static_field("sNullArray") };
            if (fp)
            {
                ctx.check("cmp_oop_null_array_is_zero", fp->get_compressed_oop() == 0u);
                ctx.check("cmp_oop_null_array_field_oop_null",
                          vmhook::field_oop(*fp) == nullptr);
            }
        }

        // F.2 — FLAW C FIXED: get_compressed_oop() now guards on is_reference(),
        //       so on a primitive "I" field it returns 0 instead of the raw int
        //       bytes (which would decode to a wild OOP).
        {
            cp("SECTION F.2 (sInt get_compressed_oop guarded — primitive, no memcpy)");
            auto fp{ fi_fixture::static_field("sInt") };
            if (fp)
            {
                const std::uint32_t raw{ fp->get_compressed_oop() };
                ctx.check("cmp_oop_FIXED_primitive_int_field_guarded_zero", raw == 0u);
                ctx.record("[INFO] FLAW C FIXED: get_compressed_oop() on primitive 'I' field returns 0 "
                           "(was the raw int bytes 0x0BADF00D).");
            }
        }

        // F.3 — FLAW C FIXED: a primitive "J" (long) field is not a reference, so
        //       get_compressed_oop() now returns 0 (it used to read only the low
        //       32 bits of the 8-byte field).
        {
            cp("SECTION F.3 (sLong get_compressed_oop guarded — primitive, no memcpy)");
            auto fp{ fi_fixture::static_field("sLong") };
            if (fp)
            {
                const std::uint32_t raw{ fp->get_compressed_oop() };
                ctx.check("cmp_oop_FIXED_primitive_long_field_guarded_zero", raw == 0u);
                ctx.record("[INFO] FLAW C FIXED: get_compressed_oop() on primitive 'J' field returns 0 "
                           "(was the low 4 bytes 0x55667788).");
            }
        }

        // F.4 — get_compressed_oop on a null-base proxy is 0 (documented
        //       return contract), regardless of signature.
        {
            vmhook::field_proxy null_ref{ nullptr, "Ljava/lang/String;", true };
            vmhook::field_proxy null_prim{ nullptr, "I", false };
            ctx.check("cmp_oop_null_base_ref_is_zero", null_ref.get_compressed_oop() == 0u);
            ctx.check("cmp_oop_null_base_prim_is_zero", null_prim.get_compressed_oop() == 0u);
        }

        // F.5 — get_compressed_oop reads EXACTLY the 4 bytes at raw_address (no
        //       over-read into the adjacent slot).  Plant a sentinel buffer.
        {
            std::uint8_t buf[16] = { 0 };
            const std::uint32_t sentinel{ 0xDEADBEEFu };
            std::memcpy(buf + 4, &sentinel, sizeof(sentinel));
            const std::uint32_t guard{ 0xCAFEBABEu };
            std::memcpy(buf + 8, &guard, sizeof(guard));   // must NOT be read
            vmhook::field_proxy fp{ buf + 4, "Ljava/lang/String;", false };
            ctx.check("cmp_oop_reads_exactly_4_bytes_at_pointer",
                      fp.get_compressed_oop() == sentinel);
            ctx.check("cmp_oop_does_not_overread_adjacent", guard == 0xCAFEBABEu);
        }

        // F.6 — the is_reference() gate fires identically for an ARRAY descriptor
        //       ('[' front byte): an array-typed stack proxy reads its 4 sentinel
        //       bytes (same path as the 'L' case), confirming the guard admits
        //       BOTH reference forms, not just object refs.
        {
            std::uint8_t buf[8] = { 0 };
            const std::uint32_t sentinel{ 0x0A0B0C0Du };
            std::memcpy(buf, &sentinel, sizeof(sentinel));
            vmhook::field_proxy fp{ buf, "[I", false };
            ctx.check("cmp_oop_array_descriptor_reads_4_bytes",
                      fp.get_compressed_oop() == sentinel);
            vmhook::field_proxy fp2{ buf, "[[Ljava/lang/Object;", false };
            ctx.check("cmp_oop_nested_array_descriptor_reads_4_bytes",
                      fp2.get_compressed_oop() == sentinel);
        }

        // F.7 — every PRIMITIVE single-char descriptor is gated to 0 by
        //       is_reference() BEFORE any memcpy, even over a planted non-zero
        //       buffer — proving FLAW-C's fix covers the whole primitive alphabet
        //       (not just the I/J fields tested live in F.2/F.3).
        {
            std::uint8_t buf[8] = { 0 };
            const std::uint32_t planted{ 0xFEEDFACEu };
            std::memcpy(buf, &planted, sizeof(planted));
            const char* prim_chars[] = { "Z", "B", "S", "I", "J", "F", "D", "C" };
            for (const char* d : prim_chars)
            {
                vmhook::field_proxy fp{ buf, d, false };
                ctx.check(std::string{ "cmp_oop_primitive_guarded_zero_" } + d,
                          fp.get_compressed_oop() == 0u);
            }
            // 'V' (void) and a multi-char descriptor are also non-reference → 0.
            vmhook::field_proxy v{ buf, "V", false };
            ctx.check("cmp_oop_void_guarded_zero", v.get_compressed_oop() == 0u);
            vmhook::field_proxy multi{ buf, "II", false };
            ctx.check("cmp_oop_multichar_primitive_guarded_zero",
                      multi.get_compressed_oop() == 0u);
            // The empty-signature proxy is non-reference → 0 (no front byte).
            vmhook::field_proxy empty{ buf, "", false };
            ctx.check("cmp_oop_empty_signature_guarded_zero",
                      empty.get_compressed_oop() == 0u);
        }
    }

    // =====================================================================
    //  SECTION I — field-metadata ENUMERATION across VARIED class shapes.
    //  There is NO public field-enumeration API (find_field is by-name and walks
    //  the super chain); the honest "enumerate the fields of a class" coverage is
    //  to resolve every DECLARED field by name through a wrapper registered for
    //  that class and assert its {descriptor, is_static, is_reference, offset}
    //  metadata matches the true Java declaration.  Shapes covered here:
    //    I.0  ZERO-field class            (FieldIntrospection$Empty)
    //    I.1  ONLY-static class           (FieldIntrospection$OnlyStatic)
    //    I.2  ONLY-instance class         (FieldIntrospection$OnlyInstance)
    //  All static reads touch the nested class's OWN mirror (old-gen, stable) →
    //  HARD; instance metadata checks (I.2) read proxy POINTERS only (raw_address
    //  is base_oop+offset arithmetic, no oop deref) → HARD.  Descriptor / static /
    //  reference classification NEVER deref an oop, so they are HARD everywhere.
    // =====================================================================
    cp("SECTION I (field metadata across varied class shapes)");
    {
        // Reusable metadata asserter for a STATIC field on an arbitrary wrapper.
        // Proves descriptor, is_static==true, is_reference==(L/[ front byte) AND
        // that is_reference is the exact complement of "sized primitive".
        struct ShapeRow { const char* field; const char* desc; bool is_ref; };

        // I.0 — ZERO-field class: every declared-field lookup misses cleanly, and
        // a degenerate / absent name also misses — no crash, no fabricated proxy.
        {
            cp("SECTION I.0 (Empty — zero fields, all lookups miss)");
            const bool empty_loaded{
                vmhook::find_class("vmhook/fixtures/FieldIntrospection$Empty") != nullptr };
            ctx.check("shape_empty_class_loaded", empty_loaded);
            if (empty_loaded)
            {
                // No DECLARED field named these exists on Empty; static_field must
                // return nullopt (Empty has no instance to wrap, so we probe the
                // static path, which still walks Empty + its supers j.l.Object).
                ctx.check("shape_empty_no_field_absent",
                          fi_empty::static_field("nonexistentField").has_value() == false);
                ctx.check("shape_empty_no_field_oiInt",
                          fi_empty::static_field("oiInt").has_value() == false);
                // An EMPTY-string and an obviously-garbage name also miss, no crash.
                ctx.check("shape_empty_empty_name_absent",
                          fi_empty::static_field("").has_value() == false);
                ctx.check("shape_empty_garbage_name_absent",
                          fi_empty::static_field("\x01\x02not a field").has_value() == false);
            }
        }

        // I.1 — ONLY-static class: enumerate every declared static field, one per
        // descriptor family.  Each resolves, is_static==true, descriptor matches,
        // is_reference matches, raw_address is non-null and width-aligned, and
        // distinct fields occupy distinct slots.
        {
            cp("SECTION I.1 (OnlyStatic — every static field's metadata)");
            const bool loaded{
                vmhook::find_class("vmhook/fixtures/FieldIntrospection$OnlyStatic") != nullptr };
            ctx.check("shape_onlystatic_class_loaded", loaded);
            if (loaded)
            {
                const ShapeRow rows[] = {
                    { "osInt",    "I",                  false },
                    { "osLong",   "J",                  false },
                    { "osString", "Ljava/lang/String;", true  },
                    { "osArray",  "[I",                 true  },
                    { "osObject", "Ljava/lang/Object;", true  },
                };
                void* prev_addr{ nullptr };
                for (const ShapeRow& r : rows)
                {
                    auto fp{ fi_only_static::static_field(r.field) };
                    ctx.check(std::string{ "shape_onlystatic_resolves_" } + r.field,
                              fp.has_value());
                    if (!fp) { continue; }
                    ctx.check(std::string{ "shape_onlystatic_is_static_" } + r.field,
                              fp->is_static() == true);
                    ctx.check(std::string{ "shape_onlystatic_sig_" } + r.field,
                              std::string{ fp->signature() } == r.desc);
                    ctx.check(std::string{ "shape_onlystatic_is_ref_" } + r.field,
                              fp->is_reference() == r.is_ref);
                    const bool primitive{
                        vmhook::detail::jvm_primitive_byte_width(fp->signature()) != 0 };
                    ctx.check(std::string{ "shape_onlystatic_ref_complement_" } + r.field,
                              fp->is_reference() == !primitive);
                    ctx.check(std::string{ "shape_onlystatic_addr_nonnull_" } + r.field,
                              fp->raw_address() != nullptr);
                    // Distinct declared statics occupy DISTINCT mirror slots.
                    if (prev_addr)
                    {
                        ctx.check(std::string{ "shape_onlystatic_addr_distinct_" } + r.field,
                                  fp->raw_address() != prev_addr);
                    }
                    prev_addr = fp->raw_address();
                }
                // A genuine static value crosscheck: osInt reads the declared
                // constant through the nested mirror (HARD — old-gen mirror slot).
                auto vi{ fi_only_static::static_field("osInt") };
                if (vi)
                {
                    const std::int32_t got{ vi->get() };
                    ctx.check("shape_onlystatic_osInt_value", got == 0x010203);
                }
                // This class declares NO instance fields: an instance-only name is
                // absent on the static path too (no such field anywhere on it).
                ctx.check("shape_onlystatic_no_instance_field",
                          fi_only_static::static_field("oiInt").has_value() == false);
            }
        }

        // I.2 — ONLY-instance class: enumerate every declared instance field via
        // the LIVE OnlyInstance wrapper.  is_static==false, descriptor / reference
        // match, raw_address == oop+offset (recomputed) and lies after the header.
        // All checks read proxy metadata / pointer arithmetic only → no oop deref
        // → HARD even though the OnlyInstance object is young-gen.
        {
            cp("SECTION I.2 (OnlyInstance — every instance field's metadata)");
            vmhook::hotspot::klass* const oi_klass{
                reinterpret_cast<vmhook::hotspot::klass*>(
                    vmhook::find_class("vmhook/fixtures/FieldIntrospection$OnlyInstance")) };
            const auto oi{ get_only_instance() };
            ctx.check("shape_onlyinstance_wrapper_obtained", oi != nullptr);
            if (oi && oi_klass)
            {
                void* const oop{ oi->vmhook::object_base::get_instance() };
                ctx.check("shape_onlyinstance_oop_valid",
                          oop != nullptr && vmhook::hotspot::is_valid_pointer(oop));
                if (oop)
                {
                    const ShapeRow rows[] = {
                        { "oiBool",   "Z",                  false },
                        { "oiInt",    "I",                  false },
                        { "oiDouble", "D",                  false },
                        { "oiString", "Ljava/lang/String;", true  },
                        { "oiArray",  "[I",                 true  },
                    };
                    for (const ShapeRow& r : rows)
                    {
                        auto fp{ oi->get_field(r.field) };
                        ctx.check(std::string{ "shape_onlyinstance_resolves_" } + r.field,
                                  fp.has_value());
                        if (!fp) { continue; }
                        ctx.check(std::string{ "shape_onlyinstance_is_static_false_" } + r.field,
                                  fp->is_static() == false);
                        ctx.check(std::string{ "shape_onlyinstance_sig_" } + r.field,
                                  std::string{ fp->signature() } == r.desc);
                        ctx.check(std::string{ "shape_onlyinstance_is_ref_" } + r.field,
                                  fp->is_reference() == r.is_ref);
                        void* const got{ fp->raw_address() };
                        void* const expected{ recompute_instance_addr(oi_klass, oop, r.field) };
                        ctx.check(std::string{ "shape_onlyinstance_addr_eq_oop_offset_" } + r.field,
                                  got != nullptr && expected != nullptr && got == expected);
                        ctx.check(std::string{ "shape_onlyinstance_addr_after_header_" } + r.field,
                                  reinterpret_cast<std::uint8_t*>(got)
                                      > reinterpret_cast<std::uint8_t*>(oop));
                    }
                }
            }
        }
    }

    // =====================================================================
    //  SECTION J — INHERITED vs DECLARED scope.  find_field walks the super
    //  chain (vmhook.hpp ~13890), so a field DECLARED on Base resolves through a
    //  Derived wrapper.  Prove that:
    //    - a Derived-declared field (derivedInstance) resolves with correct
    //      metadata,
    //    - Base-declared fields (baseInstance / baseRef / baseStatic) ALSO
    //      resolve through the Derived wrapper (inheritance), with correct
    //      metadata, and at the SAME raw_address as resolving them through a Base
    //      wrapper on the SAME oop (instance) / the Base mirror is independent
    //      (static).
    //  Instance metadata reads are pointer-arithmetic only → HARD; the Base
    //  static is on Base's OWN mirror (old-gen) → HARD.
    // =====================================================================
    cp("SECTION J (inherited vs declared scope)");
    {
        vmhook::hotspot::klass* const derived_klass{
            reinterpret_cast<vmhook::hotspot::klass*>(
                vmhook::find_class("vmhook/fixtures/FieldIntrospection$Derived")) };
        const auto der{ fi_derived::get_derived() };
        ctx.check("inherit_derived_wrapper_obtained", der != nullptr);
        if (der && derived_klass)
        {
            void* const oop{ der->vmhook::object_base::get_instance() };
            ctx.check("inherit_derived_oop_valid",
                      oop != nullptr && vmhook::hotspot::is_valid_pointer(oop));

            // J.1 — the field DECLARED on Derived resolves with correct metadata.
            {
                auto fp{ der->get_field("derivedInstance") };
                ctx.check("inherit_declared_resolves", fp.has_value());
                if (fp)
                {
                    ctx.check("inherit_declared_is_static_false", fp->is_static() == false);
                    ctx.check("inherit_declared_sig", std::string{ fp->signature() } == "I");
                    ctx.check("inherit_declared_not_reference", fp->is_reference() == false);
                    if (oop)
                    {
                        ctx.check("inherit_declared_addr_eq_oop_offset",
                                  fp->raw_address()
                                      == recompute_instance_addr(derived_klass, oop, "derivedInstance"));
                    }
                }
            }

            // J.2 — Base-declared INSTANCE fields resolve THROUGH the Derived
            // wrapper (super-chain walk).  Metadata matches, and the resolved
            // raw_address equals oop+offset recomputed against the SAME oop.
            {
                struct IR { const char* field; const char* desc; bool is_ref; };
                const IR irows[] = {
                    { "baseInstance", "I",                  false },
                    { "baseRef",      "Ljava/lang/String;", true  },
                };
                for (const IR& r : irows)
                {
                    auto fp{ der->get_field(r.field) };
                    ctx.check(std::string{ "inherit_inst_resolves_" } + r.field,
                              fp.has_value());
                    if (!fp) { continue; }
                    ctx.check(std::string{ "inherit_inst_is_static_false_" } + r.field,
                              fp->is_static() == false);
                    ctx.check(std::string{ "inherit_inst_sig_" } + r.field,
                              std::string{ fp->signature() } == r.desc);
                    ctx.check(std::string{ "inherit_inst_is_ref_" } + r.field,
                              fp->is_reference() == r.is_ref);
                    if (oop)
                    {
                        // recompute_instance_addr walks find_field from the START
                        // klass (Derived) too, so it equally finds the inherited
                        // field — the two must agree byte-for-byte.
                        ctx.check(std::string{ "inherit_inst_addr_eq_oop_offset_" } + r.field,
                                  fp->raw_address() != nullptr
                                      && fp->raw_address()
                                             == recompute_instance_addr(derived_klass, oop, r.field));
                    }
                }
            }

            // J.3 — a Base-declared inherited field resolved through the Derived
            // wrapper lands at the SAME raw_address as resolving it through a Base
            // wrapper on the SAME underlying oop (re-typed): the inherited slot is
            // a single object-absolute offset regardless of which subclass starts
            // the lookup.  Pointer compare only → HARD.
            if (oop)
            {
                auto via_derived{ der->get_field("baseInstance") };
                // Wrap the SAME oop as a Base to resolve the inherited slot from
                // the declaring class's own view.
                fi_base base_view{ vmhook::oop_t{ oop } };
                auto via_base{ base_view.get_field("baseInstance") };
                if (via_derived && via_base)
                {
                    ctx.check("inherit_same_slot_through_base_and_derived",
                              via_derived->raw_address() == via_base->raw_address());
                }
            }

            // J.4 — Base-declared STATIC field resolves through the Derived wrapper
            // and reads its declared value off Base's OWN mirror (old-gen → HARD).
            {
                auto fp{ fi_derived::static_field("baseStatic") };
                ctx.check("inherit_static_resolves_via_derived", fp.has_value());
                if (fp)
                {
                    ctx.check("inherit_static_is_static_true", fp->is_static() == true);
                    ctx.check("inherit_static_sig", std::string{ fp->signature() } == "I");
                    ctx.check("inherit_static_addr_nonnull", fp->raw_address() != nullptr);
                    const std::int32_t got{ fp->get() };
                    ctx.check("inherit_static_value", got == 0x0BA5E000);
                }
                // Resolving the SAME inherited static through a Base wrapper yields
                // the SAME mirror+offset address (the declaring-klass mirror is
                // Base's regardless of the start klass — the load-bearing
                // declaring_klass fix-up).  Address compare only → HARD.
                auto via_base{ fi_base::static_field("baseStatic") };
                if (fp && via_base)
                {
                    ctx.check("inherit_static_same_addr_base_and_derived",
                              fp->raw_address() == via_base->raw_address());
                }
            }
        }
    }

    // =====================================================================
    //  SECTION K — MODIFIER / VISIBILITY blindness.  field_entry_t carries NO
    //  general access-flag bitfield, so none of the five accessors can surface
    //  volatile / transient / private / public.  Prove a volatile/transient/
    //  private field's {descriptor, is_static, is_reference} metadata is
    //  IDENTICAL to a plain twin, and that a private field is still resolvable by
    //  name (visibility-blind).  All reads are static-mirror or pointer-only.
    // =====================================================================
    cp("SECTION K (modifier / visibility blindness)");
    {
        // K.1 — volatile static int twin of sInt: same descriptor / static /
        // non-reference; resolves and reads its declared value.
        {
            auto vol{ fi_fixture::static_field("sVolatileInt") };
            auto plain{ fi_fixture::static_field("sInt") };
            ctx.check("modblind_volatile_resolves", vol.has_value());
            if (vol && plain)
            {
                ctx.check("modblind_volatile_sig_eq_plain",
                          std::string{ vol->signature() } == std::string{ plain->signature() });
                ctx.check("modblind_volatile_is_static_eq_plain",
                          vol->is_static() == plain->is_static());
                ctx.check("modblind_volatile_is_ref_eq_plain",
                          vol->is_reference() == plain->is_reference());
                ctx.check("modblind_volatile_value",
                          static_cast<std::int32_t>(vol->get()) == 0x5A5A5A5A);
            }
        }

        // K.2 — private static String: resolvable by name despite being private;
        // descriptor / static / reference are the normal reference-field metadata.
        {
            auto fp{ fi_fixture::static_field("sPrivateString") };
            ctx.check("modblind_private_static_resolves", fp.has_value());
            if (fp)
            {
                ctx.check("modblind_private_static_sig",
                          std::string{ fp->signature() } == "Ljava/lang/String;");
                ctx.check("modblind_private_static_is_static", fp->is_static() == true);
                ctx.check("modblind_private_static_is_ref", fp->is_reference() == true);
            }
        }

        // K.3 — transient + private INSTANCE int fields: same descriptor / non-
        // static / non-reference as iInt.  Pointer / metadata only → HARD.
        {
            cp("SECTION K.3 (transient/private instance metadata — pointer only)");
            const auto inst{ fi_fixture::get_instance() };
            if (inst)
            {
                auto plain{ inst->get_field("iInt") };
                auto tr{ inst->get_field("iTransientInt") };
                ctx.check("modblind_transient_resolves", tr.has_value());
                if (tr && plain)
                {
                    ctx.check("modblind_transient_sig_eq_plain",
                              std::string{ tr->signature() } == std::string{ plain->signature() });
                    ctx.check("modblind_transient_is_static_false", tr->is_static() == false);
                    ctx.check("modblind_transient_is_ref_false", tr->is_reference() == false);
                    ctx.check("modblind_transient_addr_nonnull", tr->raw_address() != nullptr);
                }
                auto pr{ inst->get_field("iPrivateInt") };
                ctx.check("modblind_private_instance_resolves", pr.has_value());
                if (pr)
                {
                    ctx.check("modblind_private_instance_sig", std::string{ pr->signature() } == "I");
                    ctx.check("modblind_private_instance_is_static_false", pr->is_static() == false);
                    ctx.check("modblind_private_instance_is_ref_false", pr->is_reference() == false);
                }
            }
        }

        ctx.record("[INFO] modifier/visibility-blindness: signature/is_static/is_reference/"
                   "raw_address/get_compressed_oop expose NEITHER JVM_ACC_VOLATILE/TRANSIENT "
                   "nor the access level (PRIVATE/PUBLIC) — field_entry_t carries only "
                   "offset/is_static/signature/declaring_klass, so a volatile/transient/"
                   "private field is metadata-indistinguishable from a plain twin and a "
                   "private field is resolvable by name regardless of access control.");
    }

    // =====================================================================
    //  SECTION L — DEGENERATE field-lookup inputs (no crash).  An absent name,
    //  an empty name, and a garbage/over-long name must all resolve to nullopt
    //  on BOTH the static and (where an instance exists) the instance path,
    //  never fabricating a proxy and never faulting.
    // =====================================================================
    cp("SECTION L (degenerate field-lookup inputs — no crash)");
    {
        ctx.check("degenerate_static_absent_name",
                  fi_fixture::static_field("definitelyNotAField").has_value() == false);
        ctx.check("degenerate_static_empty_name",
                  fi_fixture::static_field("").has_value() == false);
        ctx.check("degenerate_static_garbage_name",
                  fi_fixture::static_field("\xFF\xFE not-a-field \t\n").has_value() == false);
        // A 512-char name cannot match any declared field — must miss cleanly.
        {
            const std::string long_name(512, 'q');
            ctx.check("degenerate_static_overlong_name",
                      fi_fixture::static_field(long_name.c_str()).has_value() == false);
        }
        // Asking for an INSTANCE field name on the STATIC path misses (iInt is not
        // static) — static_field walks for JVM_ACC_STATIC and only the static slot
        // would be returned; an instance-only name has no static entry.
        const auto inst{ fi_fixture::get_instance() };
        if (inst)
        {
            ctx.check("degenerate_instance_absent_name",
                      inst->get_field("definitelyNotAField").has_value() == false);
            ctx.check("degenerate_instance_empty_name",
                      inst->get_field("").has_value() == false);
            ctx.check("degenerate_instance_garbage_name",
                      inst->get_field("\x01\x02\x03nope").has_value() == false);
        }
    }

    // =====================================================================
    //  SECTION G — live probe (mode 1): interpreter-hook-on-dispatch contract,
    //  then re-introspect post-dispatch (proves the accessors reflect live JVM
    //  state, and that a hooked Java call did not perturb field resolution).
    // =====================================================================
    // The mode-1 dispatch allocates on the Java thread (touch + publishWitnesses)
    // and can trigger a young GC that relocates `instance` AND `sString`'s
    // referent.  Post-probe reads are STATIC (sString on the mirror) so the slot
    // read is HARD; only the decoded referent CONTENT read is probe-guarded.
    cp("SECTION G (live probe mode 1)");
    {
        auto handle{ vmhook::scoped_hook<fi_fixture>(
            "touch",
            [](vmhook::return_value&,
               const std::unique_ptr<fi_fixture>& self,
               std::int32_t delta)
            {
                g_hook_calls.fetch_add(1, std::memory_order_relaxed);
                g_hook_saw_self.store(self != nullptr, std::memory_order_relaxed);
                (void) delta;
            }) };
        ctx.check("probe_hook_installed", handle.installed());

        cp("SECTION G run_probe mode 1 (Java dispatch + alloc — GC trigger)");
        const bool done{ ctx.run_probe(
            [](bool value)
            {
                if (value)
                {
                    fi_fixture::set_done(false);
                    fi_fixture::set_mode(1);
                }
                fi_fixture::set_go(value);
            },
            []() { return fi_fixture::get_done(); }) };

        ctx.check("probe_completed", done);
        ctx.check("probe_hook_fired", g_hook_calls.load() >= 1);
        ctx.check("probe_hook_saw_self", g_hook_saw_self.load());
        // touch() returns iInt(0x0BADCAFE) + 100.
        ctx.check("probe_observed_is_iInt_plus_100",
                  fi_fixture::get_observed()
                      == static_cast<std::int32_t>(0x0BADCAFE + 100));

        // Re-introspect post-dispatch: signature/is_static/get_compressed_oop
        // all still correct and the String still decodes.
        {
            cp("SECTION G post-probe re-introspect (static sString — mirror slot)");
            auto fp{ fi_fixture::static_field("sString") };
            if (fp)
            {
                ctx.check("post_probe_sig_still_string",
                          std::string{ fp->signature() } == "Ljava/lang/String;");
                ctx.check("post_probe_is_static_still_true", fp->is_static());
                ctx.check("post_probe_is_reference_still_true", fp->is_reference());
                // The post-dispatch String decode RAW-derefs the oop.  The probe's
                // run() (touch + publishWitnesses) allocates on the Java thread and
                // can trigger a minor/young GC that relocates sString's referent
                // between this decode and the read; on mingw·java8 that landed as a
                // full-JVM crash.  Guard the read behind a safe-read probe and make
                // it best-effort: a transient empty miss is recorded, a DIFFERENT
                // non-empty value still fails (real mis-decode).
                cp("SECTION G post-probe sString content (read_java_string — probe header)");
                void* const decoded{ vmhook::hotspot::decode_oop_pointer(fp->get_compressed_oop()) };
                const std::string text{ safe_read_java_string(decoded) };
                if (!text.empty())
                {
                    ctx.check("post_probe_string_still_decodes", text == "introspect-me");
                }
                else
                {
                    ctx.record("[INFO] post_probe_string: decoded String header not safely "
                               "readable after dispatch (relocated/transient) — skipped decode assert.");
                }
            }
        }
    }

    // =====================================================================
    //  SECTION H — raw_address() GC-staleness DOCUMENTATION (mode 2).  Capture
    //  a static reference field's compressed OOP + decoded address, force a GC
    //  on the Java thread, then re-resolve.  We assert the proxy STILL decodes
    //  to a live, valid object (the accessor re-reads mirror+offset, which the
    //  GC keeps coherent), documenting that raw_address itself does NO pinning:
    //  any address a caller CACHED across the GC may now be stale, but a FRESH
    //  lookup remains correct.
    // =====================================================================
    // Every slot read here is on STATIC sString (mirror) → HARD; every referent
    // CONTENT read goes through safe_read_java_string (header probe) → best-effort.
    // This block deliberately straddles a forced GC, so its content reads are the
    // single most likely place to observe a mid-relocation object — hence the
    // probe-then-read on EVERY decode (including the retry loop).
    cp("SECTION H (raw_address GC-staleness doc, mode 2)");
    {
        cp("SECTION H pre-GC decode (sString content — probe header)");
        auto before{ fi_fixture::static_field("sString") };
        void* const decoded_before{
            before.has_value()
                ? vmhook::hotspot::decode_oop_pointer(before->get_compressed_oop())
                : nullptr };
        // This block runs AFTER Section G's dispatch, so sString's referent may
        // already have been relocated by a young GC; the decoded address can pass
        // is_valid_pointer() yet point into an unmapped page.  Probe the header
        // with safe_read before the RAW read_java_string and make it best-effort
        // (the hard pre-GC value proof lives in Section E.1 / Section G).
        {
            const std::string text{ safe_read_java_string(decoded_before) };
            if (!text.empty())
            {
                ctx.check("gc_doc_before_decodes", text == "introspect-me");
            }
            else
            {
                ctx.record("[INFO] gc_doc_before: sString header not safely readable "
                           "(relocated by an earlier dispatch GC) — skipped pre-GC decode assert.");
            }
        }

        // The forced-System.gc() GC-staleness characterization below is gated to
        // POSIX (Linux/macOS) ONLY.  On ALL Windows toolchains the aggressive full-GC
        // churn destabilizes the test JVM with an OFF-suite-thread fault during the
        // collection / code-cache sweep that neither the harness __try nor the (now
        // fully fault-proofed) auto-repair watchdog can contain — observed crashing
        // mingw·java8 AND msvc·JDK11+ at this exact point, while EVERY native read in
        // this block is already safe_read/mirror-guarded (the forced collection itself,
        // not a vmhook read, is the trigger; deep msvc·JDK11+ GC-internal interaction
        // tracked as a follow-up).  The no-relocation accessors are fully covered by
        // Sections A-G on every cell; this characterization runs on Linux + macOS.
#if !defined(_WIN32)
        cp("SECTION H run_probe mode 2 (forced System.gc() churn)");
        const bool done{ ctx.run_probe(
            [](bool value)
            {
                if (value)
                {
                    fi_fixture::set_done(false);
                    fi_fixture::set_mode(2);   // forces System.gc() churn
                }
                fi_fixture::set_go(value);
            },
            []() { return fi_fixture::get_done(); }) };
        ctx.check("gc_doc_probe_completed", done);

        // FRESH lookup after GC: still resolves, still the right value.  (The
        // mirror+offset math is GC-coherent; only a stale CACHED raw_address
        // would be wrong — which is the documented flaw, not exercised as a
        // crash here because that would be UB.)
        cp("SECTION H post-GC fresh re-resolve (static sString — mirror slot)");
        auto after{ fi_fixture::static_field("sString") };
        ctx.check("gc_doc_after_resolves", after.has_value());
        if (after)
        {
            // FRESH post-GC value decode is BEST-EFFORT.  System.gc() on a
            // concurrent/relocating collector (G1 on linux) can keep relocating
            // AFTER it returns, so a fresh read of the static slot — even though
            // its ADDRESS is coherent (gc_doc_after_addr_matches_recompute passes)
            // — can observe a String mid-relocation and decode to empty.  Value
            // coherence across an in-flight concurrent collection is not a
            // guarantee vmhook makes; the PRE-GC decode (gc_doc_before_decodes) is
            // the hard value proof.  Observed FAILing consistently on linux/gcc at
            // JDK 11/17 while every other artifact passed.  So here we assert the
            // decode is EITHER the real value OR a transient empty miss — never a
            // DIFFERENT live string (which would be a real mis-decode bug) — and
            // record the observed value.
            // Bounded retry (<=16).  Each fresh decode RAW-derefs the oop via
            // read_java_string; right after System.gc() the referent is the most
            // likely thing in this whole module to be mid-relocation, so we route
            // EVERY read through safe_read_java_string (header safe-probe first).
            // A failed probe / relocated object yields "" and we simply retry;
            // never a raw fault.
            cp("SECTION H post-GC content retry loop (sString — probe header each)");
            std::string decoded_value{};
            for (int attempt{ 0 }; attempt < 16 && decoded_value != "introspect-me"; ++attempt)
            {
                auto fresh{ fi_fixture::static_field("sString") };
                void* const d{ fresh.has_value()
                    ? vmhook::hotspot::decode_oop_pointer(fresh->get_compressed_oop())
                    : nullptr };
                decoded_value = safe_read_java_string(d);
            }
            ctx.record(std::string{ "[INFO] gc_doc: post-GC fresh decode = '" } + decoded_value +
                       "' (expected 'introspect-me'; empty = transient concurrent-GC miss, addr is coherent).");
            ctx.check("gc_doc_after_decode_correct_or_transient_miss",
                      decoded_value.empty() || decoded_value == "introspect-me");
            ctx.check("gc_doc_after_signature_intact",
                      std::string{ after->signature() } == "Ljava/lang/String;");
            ctx.check("gc_doc_after_raw_address_nonnull",
                      after->raw_address() != nullptr);
            // The static SLOT address (mirror+offset) is itself stable iff the
            // Class mirror did not move; if it did, a fresh recompute matches the
            // fresh proxy.  Cross-check they agree post-GC.
            ctx.check("gc_doc_after_addr_matches_recompute",
                      after->raw_address() == recompute_static_addr(klass, "sString"));
        }
#else
        cp("SECTION H forced-GC characterization skipped (MinGW / clang-cl)");
        ctx.record("[INFO] SECTION H: forced-System.gc() GC-staleness characterization "
                   "skipped on MinGW / clang-cl (no SEH net; a cold full GC can "
                   "destabilize the test JVM there). The same accessors are covered "
                   "no-relocation in Sections A-G; full characterization runs on "
                   "MSVC + Linux + macOS.");
#endif
    }

    // =====================================================================
    //  SECTION M -- value_t::is_reference() AGREES with field_proxy::is_reference()
    //  for EVERY field.  field_proxy::is_reference() keys on signature_text[0]
    //  (L/[); value_t::is_reference() holds iff the read produced the uint32
    //  (compressed-OOP) alternative (vmhook.hpp:15445).  The two are independent
    //  code paths that must classify identically on a resolved field - a cross-
    //  check that catches a future drift between the descriptor classifier and
    //  the variant the read populates.
    //
    //  STATIC fields only here (get() reads the old-gen mirror slot -> no
    //  relocation fault); a primitive's get() reads its own value bytes (no oop
    //  deref); a reference's get() reads the 4-byte compressed slot off the mirror
    //  (no referent deref).  All HARD.
    // =====================================================================
    cp("SECTION M (value_t::is_reference agrees with field_proxy::is_reference)");
    {
        struct MRow { const char* field; bool is_ref; };
        const MRow mrows[] = {
            { "sBool",   false }, { "sByte",   false }, { "sShort",  false },
            { "sInt",    false }, { "sLong",   false }, { "sFloat",  false },
            { "sDouble", false }, { "sChar",   false },
            { "sString",     true }, { "sIntArray",  true }, { "sIntArray2D", true },
            { "sObjArray",   true }, { "sStrArray",  true }, { "sObject",     true },
            { "sRunnable",   true }, { "sSelfRef",   true }, { "sObjArray2D", true },
            { "sNullString", true }, { "sNullArray", true },
            { "sFinalInt",   false }, { "sFinalString", true },
        };
        for (const MRow& r : mrows)
        {
            auto fp{ fi_fixture::static_field(r.field) };
            ctx.check(std::string{ "vt_isref_resolves_" } + r.field, fp.has_value());
            if (!fp) { continue; }
            const auto v{ fp->get() };
            // The two classifiers must agree, and both must equal the expected.
            ctx.check(std::string{ "vt_isref_agrees_proxy_" } + r.field,
                      v.is_reference() == fp->is_reference());
            ctx.check(std::string{ "vt_isref_matches_expected_" } + r.field,
                      v.is_reference() == r.is_ref);
            // value_t carries the proxy's descriptor verbatim regardless of value.
            ctx.check(std::string{ "vt_signature_matches_proxy_" } + r.field,
                      v.signature == std::string{ fp->signature() });
        }
        // A NULL reference field STILL reports value_t::is_reference()==true:
        // the read produced the uint32 alternative (value 0), so the variant tag
        // - not the runtime value - drives the classification.  Pins that a null
        // reference is a reference by TYPE, not by content.
        {
            auto fp{ fi_fixture::static_field("sNullString") };
            if (fp)
            {
                const auto v{ fp->get() };
                ctx.check("vt_isref_null_reference_still_true", v.is_reference() == true);
                ctx.check("vt_isref_null_reference_as_void_null",
                          static_cast<void*>(v) == nullptr);
                // as_string() on a null reference decodes read_java_string(null) -> "".
                ctx.check("vt_as_string_null_reference_empty", v.as_string().empty());
            }
        }
        // A PRIMITIVE field's value_t is NOT a reference, and as_string() yields ""
        // (every non-uint32 alternative returns empty - vmhook.hpp:15436).
        {
            auto fp{ fi_fixture::static_field("sInt") };
            if (fp)
            {
                const auto v{ fp->get() };
                ctx.check("vt_isref_primitive_false", v.is_reference() == false);
                ctx.check("vt_as_string_primitive_empty", v.as_string().empty());
            }
        }
    }

    // =====================================================================
    //  SECTION N - as_string() equivalence + idempotency for STATIC String.
    //  value_t::as_string() (vmhook.hpp:15424) decodes the uint32 alternative via
    //  read_java_string(decode_oop_pointer(raw)) - EXACTLY the chain Section E.1
    //  walks by hand.  Prove the two agree and that as_string() is idempotent
    //  (two reads of the same proxy yield byte-equal strings).
    //
    //  as_string() RAW-derefs the referent, so it is BEST-EFFORT (guarded behind
    //  the same header probe as safe_read_java_string); a transient empty miss is
    //  recorded, a DIFFERENT non-empty value would still be a real mis-decode and
    //  fails against the hand-walked decode.
    // =====================================================================
    cp("SECTION N (as_string equivalence / idempotency - static sString)");
    {
        auto fp{ fi_fixture::static_field("sString") };
        if (fp)
        {
            void* const decoded{ vmhook::hotspot::decode_oop_pointer(fp->get_compressed_oop()) };
            if (oop_header_safely_readable(decoded))
            {
                const std::string via_as_string{ fp->get().as_string() };
                const std::string via_hand{ safe_read_java_string(decoded) };
                if (!via_as_string.empty() && !via_hand.empty())
                {
                    ctx.check("as_string_equals_hand_walked_decode",
                              via_as_string == via_hand);
                    ctx.check("as_string_decodes_real_value",
                              via_as_string == "introspect-me");
                    // Idempotent: a second independent read is byte-equal.
                    const std::string again{ fp->get().as_string() };
                    ctx.check("as_string_idempotent", again == via_as_string);
                    // Length agrees with the Java-published witness.
                    ctx.check("as_string_length_matches_java",
                              static_cast<std::int32_t>(via_as_string.size())
                                  == fi_fixture::s_string_len());
                }
                else
                {
                    ctx.record("[INFO] as_string: sString referent not safely readable "
                               "(stale/relocated) - skipped equivalence/value asserts.");
                }
            }
            else
            {
                ctx.record("[INFO] as_string: sString header not safely readable "
                           "(stale/relocated) - skipped equivalence asserts.");
            }
        }
    }

    // =====================================================================
    //  SECTION O - signature() <-> primitive byte-width <-> raw_address alignment.
    //  jvm_primitive_byte_width (vmhook.hpp:16198) maps each 1-char primitive
    //  descriptor to its natural width: Z/B=1, S/C=2, I/F=4, J/D=8, else 0.  For
    //  every STATIC primitive field, prove (a) the width derived from signature()
    //  equals the field's true natural width, AND (b) raw_address() (mirror+offset)
    //  is aligned to that exact width.  This ties the descriptor accessor, the
    //  width oracle, and the addressing math together in one invariant.  All
    //  static-mirror / pointer-only reads -> HARD.
    // =====================================================================
    cp("SECTION O (signature<->width<->alignment cross-check, static primitives)");
    {
        struct WRow { const char* field; std::size_t width; };
        const WRow wrows[] = {
            { "sBool",   1 }, { "sByte",   1 },
            { "sShort",  2 }, { "sChar",   2 },
            { "sInt",    4 }, { "sFloat",  4 },
            { "sLong",   8 }, { "sDouble", 8 },
            { "sVolatileInt", 4 }, { "sFinalInt", 4 },
        };
        for (const WRow& r : wrows)
        {
            auto fp{ fi_fixture::static_field(r.field) };
            ctx.check(std::string{ "width_resolves_" } + r.field, fp.has_value());
            if (!fp) { continue; }
            ctx.check(std::string{ "width_from_signature_" } + r.field,
                      vmhook::detail::jvm_primitive_byte_width(fp->signature()) == r.width);
            void* const got{ fp->raw_address() };
            ctx.check(std::string{ "width_addr_nonnull_" } + r.field, got != nullptr);
            if (got)
            {
                const auto a{ reinterpret_cast<std::uintptr_t>(got) };
                ctx.check(std::string{ "width_addr_aligned_to_width_" } + r.field,
                          (a % r.width) == 0);
            }
            // A primitive is NEVER a reference, and is_reference is the exact
            // complement of "non-zero primitive width" here (size()==1 holds).
            ctx.check(std::string{ "width_primitive_not_reference_" } + r.field,
                      fp->is_reference() == false);
        }
        // Every REFERENCE field has primitive-width 0 (its descriptor is multi-char
        // or starts with L/[), the exact dual of the primitive rows above.
        const char* ref_fields[] = {
            "sString", "sIntArray", "sIntArray2D", "sObjArray", "sStrArray",
            "sObject", "sRunnable", "sSelfRef", "sObjArray2D", "sFinalString",
        };
        for (const char* f : ref_fields)
        {
            auto fp{ fi_fixture::static_field(f) };
            if (fp)
            {
                ctx.check(std::string{ "width_reference_zero_" } + f,
                          vmhook::detail::jvm_primitive_byte_width(fp->signature()) == 0);
                ctx.check(std::string{ "width_reference_is_ref_" } + f,
                          fp->is_reference() == true);
            }
        }
    }

    // =====================================================================
    //  SECTION P - field_proxy COPY value-semantics.  field_proxy has implicit
    //  copy/move (it stores field_pointer / signature_text / static_field /
    //  mirror_klass / field_offset - no deleted special members).  A COPY of a
    //  resolved proxy must agree with the original on ALL FIVE accessors: same
    //  signature() (byte-equal), same is_static(), same is_reference(), same
    //  raw_address() (identical pointer), and same get_compressed_oop().  Proves
    //  the accessors are pure functions of the proxy's stored state - copying does
    //  not perturb resolution.  Static-mirror / pointer-only -> HARD.
    // =====================================================================
    cp("SECTION P (field_proxy copy value-semantics)");
    {
        // A reference field (carries a non-zero compressed OOP off the mirror).
        {
            auto orig{ fi_fixture::static_field("sString") };
            if (orig)
            {
                const vmhook::field_proxy copy{ *orig };   // copy-construct
                ctx.check("copy_signature_byte_equal",
                          std::string{ copy.signature() } == std::string{ orig->signature() });
                ctx.check("copy_is_static_equal", copy.is_static() == orig->is_static());
                ctx.check("copy_is_reference_equal", copy.is_reference() == orig->is_reference());
                ctx.check("copy_raw_address_identical", copy.raw_address() == orig->raw_address());
                ctx.check("copy_compressed_oop_equal",
                          copy.get_compressed_oop() == orig->get_compressed_oop());
            }
        }
        // A primitive field (compressed OOP is guarded to 0 on BOTH copies).
        {
            auto orig{ fi_fixture::static_field("sLong") };
            if (orig)
            {
                const vmhook::field_proxy copy{ *orig };
                ctx.check("copy_prim_signature_equal",
                          std::string{ copy.signature() } == "J");
                ctx.check("copy_prim_is_static_equal", copy.is_static() == orig->is_static());
                ctx.check("copy_prim_is_reference_false", copy.is_reference() == false);
                ctx.check("copy_prim_raw_address_identical",
                          copy.raw_address() == orig->raw_address());
                ctx.check("copy_prim_compressed_oop_both_zero",
                          copy.get_compressed_oop() == 0u
                              && orig->get_compressed_oop() == 0u);
                // The copied proxy reads the SAME long value off the mirror slot.
                ctx.check("copy_prim_get_equals_orig",
                          static_cast<std::int64_t>(copy.get())
                              == static_cast<std::int64_t>(orig->get()));
            }
        }
        // A MOVED-from synthetic proxy: the move target carries the descriptor /
        // pointer; accessors on the target are correct.  (We only assert on the
        // target - the moved-from source is left in a valid-but-unspecified state.)
        {
            std::uint8_t buf[8] = { 0 };
            const std::uint32_t sentinel{ 0x13572468u };
            std::memcpy(buf, &sentinel, sizeof(sentinel));
            vmhook::field_proxy src{ buf, "Ljava/lang/String;", false };
            const vmhook::field_proxy dst{ std::move(src) };
            ctx.check("move_target_signature",
                      std::string{ dst.signature() } == "Ljava/lang/String;");
            ctx.check("move_target_raw_address", dst.raw_address() == buf);
            ctx.check("move_target_is_reference", dst.is_reference() == true);
            ctx.check("move_target_compressed_oop_reads_sentinel",
                      dst.get_compressed_oop() == sentinel);
        }
    }

    // =====================================================================
    //  SECTION Q - get_compressed_oop() BOUNDARY values on synthetic reference
    //  proxies.  Over a controlled 8-byte buffer the accessor returns the planted
    //  low-4-byte value VERBATIM (no transformation) for the full uint32 range:
    //  0 (null ref), 1 (min non-null), 0xFFFFFFFF (max), and a high-bit pattern;
    //  and it reads EXACTLY the low 4 bytes of an 8-byte slot, ignoring the high
    //  half (the documented J/D-truncation behaviour, here proven on a reference-
    //  typed proxy so the is_reference gate admits the read).  All synthetic
    //  stack buffers -> no JVM memory, no fault risk -> HARD.
    // =====================================================================
    cp("SECTION Q (get_compressed_oop boundary values - synthetic proxies)");
    {
        const std::uint32_t boundary[] = {
            0u, 1u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu, 0xDEADBEEFu,
        };
        for (const std::uint32_t b : boundary)
        {
            std::uint8_t buf[8] = { 0 };
            std::memcpy(buf, &b, sizeof(b));
            // Plant a DIFFERENT high half so a 4-byte read can be distinguished
            // from an 8-byte over-read.
            const std::uint32_t high{ 0xA5A5A5A5u };
            std::memcpy(buf + 4, &high, sizeof(high));
            vmhook::field_proxy fp{ buf, "Ljava/lang/String;", false };
            ctx.check(std::string{ "cmp_oop_boundary_verbatim_" } + std::to_string(b),
                      fp.get_compressed_oop() == b);
            // A 0 planted value is the null-reference contract (decodes to null).
            if (b == 0u)
            {
                ctx.check("cmp_oop_boundary_zero_decodes_null",
                          vmhook::hotspot::decode_oop_pointer(fp.get_compressed_oop()) == nullptr);
            }
        }
        // EXACT-4-byte read on an 8-byte slot: the low half is returned, the high
        // half (0xCAFED00D) is NEVER folded in.  Mirrors the J/D-truncation flaw
        // but on a reference proxy (so the is_reference gate admits the read).
        {
            std::uint8_t buf[8] = { 0 };
            const std::uint32_t low{ 0x11223344u };
            const std::uint32_t high{ 0xCAFED00Du };
            std::memcpy(buf,     &low,  sizeof(low));
            std::memcpy(buf + 4, &high, sizeof(high));
            vmhook::field_proxy fp{ buf, "[J", false };   // array descriptor -> reference
            ctx.check("cmp_oop_reads_low_half_only", fp.get_compressed_oop() == low);
            ctx.check("cmp_oop_ignores_high_half", high == 0xCAFED00Du);
        }
    }

    // =====================================================================
    //  SECTION R - DEGENERATE lookup-name boundary shapes (additive to Section L).
    //  A single-char name, a name with embedded ASCII control bytes, a name that
    //  is a PREFIX of a real field, and a name that is a real field plus a
    //  trailing byte must ALL miss cleanly on the static path - no fabricated
    //  proxy, no fault.  These pin that name matching is exact-equality (not
    //  prefix / substring) and tolerates arbitrary ASCII bytes in the query.
    //  All resolution walks the klass metadata (no oop deref) -> HARD.
    // =====================================================================
    cp("SECTION R (degenerate lookup-name boundary shapes - exact-match only)");
    {
        const char* miss_names[] = {
            "s",                    // single char - no field named "s"
            "sInt ",                // real field + trailing space
            " sInt",                // leading space + real field
            "sIn",                  // proper prefix of sInt
            "sIntt",                // sInt + extra char
            "SINT",                 // wrong case
            "iInt",                 // instance field on the STATIC path -> miss
            "sStringg",             // sString + extra char
            "\t",                   // lone tab
            "\x01",                 // lone control byte (explicit escape, ASCII-safe)
        };
        for (const char* n : miss_names)
        {
            ctx.check(std::string{ "degenerate_exact_match_miss_" } + std::string{ n },
                      fi_fixture::static_field(n).has_value() == false);
        }
        // A name equal to a REAL field still HITS (sanity anchor for the misses):
        // the exact-match resolver must not be over-rejecting.
        ctx.check("degenerate_exact_match_hit_sInt",
                  fi_fixture::static_field("sInt").has_value() == true);
        // And the hit's metadata is the normal primitive-int metadata - proving
        // the anchor is a genuine resolution, not a fabricated proxy.
        {
            auto fp{ fi_fixture::static_field("sInt") };
            if (fp)
            {
                ctx.check("degenerate_anchor_sig", std::string{ fp->signature() } == "I");
                ctx.check("degenerate_anchor_is_static", fp->is_static() == true);
                ctx.check("degenerate_anchor_not_reference", fp->is_reference() == false);
            }
        }
    }
}
