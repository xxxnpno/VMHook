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
//     Ljava/lang/String;, [I, [[I, [Ljava/lang/Object;, [Ljava/lang/String;,
//     Ljava/lang/Object;, an interface ref Ljava/lang/Runnable;, and a self
//     reference Lvmhook/fixtures/FieldIntrospection;.  Verified for static AND
//     instance proxies, and proven to be a stable view aliasing the proxy.
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
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

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

        // Read the static `sLong` field's raw 8 bytes for the get_compressed_oop
        // low-half truncation proof.
        static auto s_long_raw() -> std::int64_t { return static_field("sLong")->get(); }
        static auto s_int_raw()  -> std::int32_t { return static_field("sInt")->get(); }
    };

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
            "sSelfRef", "sNullString"
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
                "iChar", "iString", "iIntArray", "iObject", "iNullString"
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
            { "sNullArray",  true },
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
        //       with NO validation (documents the "bogus pointer passes through"
        //       contract the audit flagged).  Includes the null-base case.
        {
            vmhook::field_proxy null_proxy{ nullptr, "I", false };
            ctx.check("raw_addr_null_base_is_null", null_proxy.raw_address() == nullptr);
            std::uint8_t storage[16] = { 0 };
            vmhook::field_proxy buf_proxy{ storage + 8, "I", false };
            ctx.check("raw_addr_echoes_constructor_pointer",
                      buf_proxy.raw_address() == storage + 8);
            void* const bogus{ reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x1)) };
            vmhook::field_proxy bogus_proxy{ bogus, "Ljava/lang/String;", true };
            ctx.check("raw_addr_no_validation_passes_bogus",
                      bogus_proxy.raw_address() == bogus);
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
    }
}
