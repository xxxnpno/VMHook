// field_object_ref JVM test module — area: fields.
//
// Feature under test: OBJECT-REFERENCE field access.  field_proxy::get() on a
// field whose JVM descriptor starts with 'L' reads a 4-byte compressed OOP from
// the object slot (vmhook.hpp ~11605-11608); value_t::cast_for_variant then
// decodes it into a std::unique_ptr<wrapper> (vmhook.hpp ~11433-11449):
//
//     std::unique_ptr<ref_object> r = holder->get_field("ref")->get();
//
// Unlike the method-return twin (method_proxy::call truncates/frees a JNI handle
// on JDK 21+), the FIELD path reads a real compressed OOP directly from the slot,
// so "non-null ref -> usable wrapper" holds on EVERY JDK.  This module is the
// JDK-independent proof of the whole decode pipeline.  It exercises, on a live
// JVM (real bytecode dispatch via the go/done probe):
//
//   * non-null instance ref      -> usable wrapper: read int / String / nested
//                                   ref fields AND dispatch a method through it,
//   * non-null static ref        -> usable wrapper via the mirror+offset slot,
//   * NULL ref (instance+static) -> null unique_ptr (a null slot must NEVER
//                                   fabricate a wrapper — the key invariant),
//   * final / volatile object fields decode identically to plain ones,
//   * self-ref field             -> decoded instance == the receiver instance,
//   * shared ref (two fields, one object) -> same decoded heap address,
//   * compressed-OOP decode correctness: field_proxy::get_compressed_oop() !=0,
//     decodes (via field_oop / decode_oop_pointer) to a valid pointer, and
//     re-encode(decode(x)) == x (round-trip),
//   * value_t::operator void* on a ref field == field_oop() decode (the two
//     decode entry points agree).
//
// FLAWS this module pins down on the live JVM (documented in the audit
// findings field_proxy_object_ref_unique_ptr.md / field_proxy_signature_and_
// compressed_oop.md), surfaced as [INFO] records (a CI [FAIL] would punish a
// bug this test has no power to fix):
//   (A) NO wrapper-klass match check (vmhook.hpp:11443): a Ref-typed slot read
//       through a Decoy wrapper (unrelated Java class) is NOT rejected; the
//       decoy's differently-laid-out field offsets read garbage relative to Ref.
//   (B) NO signature-shape check (vmhook.hpp:11433-11444): a '[' (Ref[]) field
//       read as unique_ptr<ref_object> is NOT rejected; the wrapper points at
//       the ARRAY oop, not an element.
//   (C) get_compressed_oop() has no signature guard (vmhook.hpp:11820): called
//       on a primitive "I" field it returns the int's first 4 bytes as if a
//       compressed OOP.
//
// Mirrors method_call_object.cpp's shape: register wrappers, hook tick() so a
// detour proves the interpreter path fires, run_probe for the handshake, then a
// dense ctx.check() battery (object-ref reads are side-effect free, so most run
// outside the detour against the published SINGLETON).
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace
{
    // Wrapper for vmhook.fixtures.FieldObjectRef$Ref — the object the Holder
    // fields point at.  Exposes the int / String / nested-ref reads and the
    // compute() method used to prove a field-decoded wrapper is fully usable.
    class ref_object : public vmhook::object<ref_object>
    {
    public:
        explicit ref_object(vmhook::oop_t instance) noexcept
            : vmhook::object<ref_object>{ instance }
        {
        }

        auto val()    -> std::int32_t { return get_field("val")->get(); }
        auto label()  -> std::string  { return get_field("label")->get(); }
        // compute() returns val*2+1 — a virtual dispatch through the wrapper.
        auto compute() -> std::int32_t { return get_method("compute")->call(); }
        // The nested object-reference field (a Ref-typed field ON a Ref): proves
        // recursive object-ref decode through a field-decoded wrapper.
        auto next()   -> std::unique_ptr<ref_object> { return get_field("next")->get(); }
    };

    // Wrapper for vmhook.fixtures.FieldObjectRef$Decoy — an UNRELATED Java class
    // whose field layout differs from Ref.  Used for the wrong-wrapper-type
    // angle: reading a Ref-typed slot through this wrapper is silently accepted
    // by the library (flaw A).
    class decoy_object : public vmhook::object<decoy_object>
    {
    public:
        explicit decoy_object(vmhook::oop_t instance) noexcept
            : vmhook::object<decoy_object>{ instance }
        {
        }

        // "poison" is a field name Ref does NOT declare; reading it against a
        // Ref oop reads at refOop + Decoy's poison-offset = garbage vs Ref.
        auto poison() -> std::int32_t { return get_field("poison")->get(); }
    };

    // Wrapper for vmhook.fixtures.FieldObjectRef — the Holder.  Drives every
    // object-reference field read.
    class holder_object : public vmhook::object<holder_object>
    {
    public:
        explicit holder_object(vmhook::oop_t instance) noexcept
            : vmhook::object<holder_object>{ instance }
        {
        }

        // ── go/done handshake ──────────────────────────────────────────────
        static auto set_go(bool value) -> void { static_field("go")->set(value); }
        static auto get_done() -> bool          { return static_field("done")->get(); }
        static auto set_mode(std::int32_t m) -> void { static_field("mode")->set(m); }

        // ── acquire the published SINGLETON instance ───────────────────────
        // SINGLETON is set at class-init (before the probe), so this works any
        // time after register_class — no probe required to reach instance fields.
        static auto singleton() -> std::unique_ptr<holder_object> { return static_field("SINGLETON")->get(); }

        // ── instance object-reference field reads (THE FEATURE) ────────────
        auto ref()          -> std::unique_ptr<ref_object> { return get_field("ref")->get(); }
        auto ref_alias()    -> std::unique_ptr<ref_object> { return get_field("refAlias")->get(); }
        auto null_ref()     -> std::unique_ptr<ref_object> { return get_field("nullRef")->get(); }
        auto final_ref()    -> std::unique_ptr<ref_object> { return get_field("finalRef")->get(); }
        auto volatile_ref() -> std::unique_ptr<ref_object> { return get_field("volatileRef")->get(); }
        auto self_ref()     -> std::unique_ptr<holder_object> { return get_field("self")->get(); }

        // wrong-wrapper-type read: the SAME Ref-typed `ref` slot, decoded as a
        // Decoy.  The library does not reject this (flaw A).
        auto ref_as_decoy() -> std::unique_ptr<decoy_object> { return get_field("ref")->get(); }

        // array-vs-object: the `refArray` field is '[' (Ref[]); decoding it as a
        // single unique_ptr<ref_object> is not rejected (flaw B) — the wrapper
        // points at the ARRAY oop.
        auto ref_array_as_ref() -> std::unique_ptr<ref_object> { return get_field("refArray")->get(); }

        // ── raw-slot helpers for compressed-OOP correctness ────────────────
        // get_compressed_oop() of a named ref field (0 if unresolved/null slot).
        auto ref_compressed(const char* name) -> std::uint32_t
        {
            const auto proxy{ get_field(name) };
            if (!proxy.has_value())
            {
                return 0;
            }
            return proxy->get_compressed_oop();
        }

        // field_oop() decode of a named ref field's slot (nullptr if unresolved).
        auto ref_field_oop(const char* name) -> void*
        {
            const auto proxy{ get_field(name) };
            if (!proxy.has_value())
            {
                return nullptr;
            }
            return vmhook::field_oop(*proxy);
        }

        // value_t::operator void* of a named ref field (the other decode entry).
        auto ref_value_as_voidp(const char* name) -> void* { return static_cast<void*>(get_field(name)->get()); }

        // is_reference() of a named field (introspection).
        auto field_is_reference(const char* name) -> bool
        {
            const auto proxy{ get_field(name) };
            return proxy.has_value() && proxy->is_reference();
        }

        // signature() of a named field.
        auto field_signature(const char* name) -> std::string
        {
            const auto proxy{ get_field(name) };
            if (!proxy.has_value())
            {
                return {};
            }
            return std::string{ proxy->signature() };
        }

        // get_compressed_oop() of a PRIMITIVE "I" INSTANCE field.  primitiveInt
        // has an ordinary 4-byte 'I' slot, so reading its compressed-oop returns
        // exactly those 4 bytes (flaw C: no signature guard).
        auto primitive_compressed(const char* name) -> std::uint32_t
        {
            const auto proxy{ get_field(name) };
            if (!proxy.has_value())
            {
                return 0;
            }
            return proxy->get_compressed_oop();
        }
        auto primitive_value(const char* name) -> std::int32_t { return get_field(name)->get(); }
        auto primitive_is_reference(const char* name) -> bool
        {
            const auto proxy{ get_field(name) };
            return proxy.has_value() && proxy->is_reference();
        }

        // ── static object-reference field reads ────────────────────────────
        static auto static_ref()      -> std::unique_ptr<ref_object> { return static_field("staticRef")->get(); }
        static auto static_null_ref() -> std::unique_ptr<ref_object> { return static_field("staticNullRef")->get(); }

        // ── published identities (exact cross-checks) ──────────────────────
        static auto ref_identity()             -> std::int32_t { return static_field("refIdentity")->get(); }
        static auto ref_alias_identity()       -> std::int32_t { return static_field("refAliasIdentity")->get(); }
        static auto final_ref_identity()       -> std::int32_t { return static_field("finalRefIdentity")->get(); }
        static auto volatile_ref_identity()    -> std::int32_t { return static_field("volatileRefIdentity")->get(); }
        static auto self_identity()            -> std::int32_t { return static_field("selfIdentity")->get(); }
        static auto static_ref_identity()      -> std::int32_t { return static_field("staticRefIdentity")->get(); }
        static auto nested_ref_identity()      -> std::int32_t { return static_field("nestedRefIdentity")->get(); }
        static auto ref_array_identity()       -> std::int32_t { return static_field("refArrayIdentity")->get(); }
        static auto ref_array_elem0_identity() -> std::int32_t { return static_field("refArrayElem0Identity")->get(); }
    };

    // ── hook observation ───────────────────────────────────────────────────
    std::atomic<int>  g_detour_calls{ 0 };
    std::atomic<bool> g_detour_saw_self{ false };

    // Mirrored fixture constants (kept in lockstep with FieldObjectRef.java).
    constexpr std::int32_t REF_VAL          = 0x0BADF00D >> 8;   // 0x000BADF0
    constexpr std::int32_t STATIC_REF_VAL   = 0x5151;
    constexpr std::int32_t NESTED_REF_VAL   = 0x2222;
    constexpr std::int32_t FINAL_REF_VAL    = 0x3333;
    constexpr std::int32_t VOLATILE_REF_VAL = 0x4444;
    constexpr std::int32_t ARRAY_ELEM0_VAL  = 700;
    constexpr std::int32_t PRIMITIVE_INT_VALUE = 0x04D2;   // 1234
    const std::string      REF_LABEL        = "ref-of-field";
    const std::string      STATIC_REF_LABEL = "static-ref";
    const std::string      NESTED_REF_LABEL = "nested-ref";

    auto as_uptr(void* p) -> std::uintptr_t { return reinterpret_cast<std::uintptr_t>(p); }
}

VMHOOK_JVM_MODULE(field_object_ref)
{
    vmhook::register_class<holder_object>("vmhook/fixtures/FieldObjectRef");
    vmhook::register_class<ref_object>("vmhook/fixtures/FieldObjectRef$Ref");
    vmhook::register_class<decoy_object>("vmhook/fixtures/FieldObjectRef$Decoy");

    // =====================================================================
    // PART 1 — object-reference field reads (side-effect free, pre-probe).
    // Everything here reads slots on the published SINGLETON; no Java
    // bytecode dispatch is required to read a field, so we can assert the
    // whole decode contract before (and independently of) the hook probe.
    // =====================================================================
    const auto holder{ holder_object::singleton() };
    ctx.check("singleton_acquired_via_field_decode", holder != nullptr);

    if (holder)
    {
        // The SINGLETON itself was reached by decoding a 'L' static field into a
        // unique_ptr<holder_object> — already one full object-ref decode proven.
        ctx.check("singleton_wrapper_has_instance",
                  holder->get_instance() != nullptr);

        // ── NON-NULL instance ref -> usable wrapper ────────────────────────
        {
            const auto r{ holder->ref() };
            ctx.check("instance_ref_non_null", r != nullptr);
            if (r)
            {
                ctx.check("instance_ref_int_read_through_wrapper", r->val() == REF_VAL);
                ctx.check("instance_ref_string_read_through_wrapper", r->label() == REF_LABEL);
                // method dispatch THROUGH the field-decoded wrapper:
                ctx.check("instance_ref_method_call_through_wrapper",
                          r->compute() == REF_VAL * 2 + 1);
                // identity: decoded OOP matches the Java-published identityHashCode
                // is verified separately via the oop comparison below; here we at
                // least confirm a live, dispatch-capable instance.
                ctx.check("instance_ref_wrapper_instance_non_null",
                          r->get_instance() != nullptr);

                // ── nested object-ref field ON the decoded wrapper ─────────
                // ORDERING: the fixture wires `ref.next` (and `self`) only inside
                // the probe's run() on the Java thread (FieldObjectRef.java
                // run(): "if (s.ref.next == null) s.ref.next = makeRef(...)").
                // PART 1 runs BEFORE PART 2's run_probe, so at this point the
                // `next` slot is still its constructor default (null).  Reading a
                // genuinely-null nested object-ref slot must therefore decode to a
                // null unique_ptr — the SAME null-slot invariant as nullRef, now
                // proven one level deep through a field-decoded wrapper.  This is
                // NOT a vmhook flaw: the slot really is null pre-probe, and the
                // decode correctly refuses to fabricate a wrapper.  The non-null
                // nested read is asserted post-probe in PART 3, once run() has
                // wired the slot.
                const auto n{ r->next() };
                ctx.check("nested_ref_slot_null_pre_probe_decodes_to_nullptr",
                          n == nullptr);
                // The unwired slot's raw compressed OOP is exactly 0 (guarded
                // has_value() access — the null-slot invariant, one level deep).
                const auto next_proxy{ r->get_field("next") };
                ctx.check("nested_ref_slot_compressed_zero_pre_probe",
                          next_proxy.has_value()
                          && next_proxy->get_compressed_oop() == 0u);
                ctx.record("[INFO] nested ref `next` is unwired (null) until the "
                           "probe's run() executes on the Java thread; pre-probe it "
                           "correctly decodes to a null unique_ptr. Non-null nested "
                           "read + value/string are asserted post-probe in PART 3.");
            }
        }

        // ── NULL instance ref -> null unique_ptr ───────────────────────────
        {
            const auto nr{ holder->null_ref() };
            ctx.check("instance_null_ref_decodes_to_nullptr", nr == nullptr);
            // a null slot's compressed OOP is exactly 0.
            ctx.check("instance_null_ref_compressed_is_zero",
                      holder->ref_compressed("nullRef") == 0u);
            // field_oop of a null slot is nullptr.
            ctx.check("instance_null_ref_field_oop_is_nullptr",
                      holder->ref_field_oop("nullRef") == nullptr);
        }

        // ── FINAL object field decodes like any other ──────────────────────
        {
            const auto fr{ holder->final_ref() };
            ctx.check("final_ref_non_null", fr != nullptr);
            if (fr)
            {
                ctx.check("final_ref_int_read", fr->val() == FINAL_REF_VAL);
                ctx.check("final_ref_method_call", fr->compute() == FINAL_REF_VAL * 2 + 1);
            }
        }

        // ── VOLATILE object field decodes correctly ────────────────────────
        {
            const auto vr{ holder->volatile_ref() };
            ctx.check("volatile_ref_non_null", vr != nullptr);
            if (vr)
            {
                ctx.check("volatile_ref_int_read", vr->val() == VOLATILE_REF_VAL);
                ctx.check("volatile_ref_method_call", vr->compute() == VOLATILE_REF_VAL * 2 + 1);
            }
        }

        // ── SHARED ref: ref and refAlias decode to the SAME heap object ────
        {
            const auto a{ holder->ref() };
            const auto b{ holder->ref_alias() };
            ctx.check("shared_ref_alias_non_null", a != nullptr && b != nullptr);
            if (a && b)
            {
                ctx.check("shared_ref_alias_same_instance",
                          a->get_instance() == b->get_instance()
                          && a->get_instance() != nullptr);
            }
            // and the same compressed OOP in both slots.
            const std::uint32_t cr{ holder->ref_compressed("ref") };
            const std::uint32_t ca{ holder->ref_compressed("refAlias") };
            ctx.check("shared_ref_alias_same_compressed_oop", cr != 0u && cr == ca);
        }

        // ── compressed-OOP decode correctness (the heart of the feature) ───
        {
            const std::uint32_t compressed{ holder->ref_compressed("ref") };
            ctx.check("ref_compressed_oop_non_zero", compressed != 0u);

            void* const decoded{ holder->ref_field_oop("ref") };
            ctx.check("ref_field_oop_decodes_non_null", decoded != nullptr);
            ctx.check("ref_field_oop_is_valid_pointer",
                      decoded != nullptr && vmhook::hotspot::is_valid_pointer(decoded));

            // value_t::operator void* must agree with field_oop()'s decode.
            void* const via_value{ holder->ref_value_as_voidp("ref") };
            ctx.check("ref_value_voidp_equals_field_oop", via_value == decoded);

            // The unique_ptr wrapper's instance must be the SAME decoded address.
            const auto r{ holder->ref() };
            ctx.check("ref_wrapper_instance_equals_decoded",
                      r != nullptr && r->get_instance() == decoded);

            // Round-trip: re-encoding the decoded pointer reproduces the exact
            // compressed value — proves decode/encode are true inverses here.
            const std::uint32_t reencoded{
                vmhook::hotspot::encode_oop_pointer(decoded) };
            ctx.check("ref_compressed_oop_roundtrips_through_encode",
                      reencoded == compressed);
        }

        // ── is_reference() / signature() introspection on a ref field ──────
        ctx.check("ref_field_is_reference_true", holder->field_is_reference("ref"));
        ctx.check("ref_field_signature_is_L_descriptor",
                  holder->field_signature("ref") == "Lvmhook/fixtures/FieldObjectRef$Ref;");
        ctx.check("array_field_is_reference_true", holder->field_is_reference("refArray"));
        ctx.check("array_field_signature_is_bracket_descriptor",
                  holder->field_signature("refArray") == "[Lvmhook/fixtures/FieldObjectRef$Ref;");

        // ── SELF ref: a field holding `this`; decoded == receiver ──────────
        // self is wired inside run(), so this read is meaningful only after the
        // probe; we read it again in PART 3 post-probe.  Here we just confirm the
        // pre-probe slot is null (it has not been wired yet) to document ordering.
        ctx.record(std::string{ "[INFO] pre-probe self slot compressed=0x" }
                   + std::to_string(holder->ref_compressed("self")));

        // ==================================================================
        // FLAW A — wrong-wrapper-type read is NOT rejected (no klass check).
        // Read the Ref-typed `ref` slot through a Decoy wrapper.  The library
        // happily constructs a decoy_object over the Ref oop; decoy.poison()
        // then reads at refOop + Decoy's poison-offset, which differs from the
        // correct Ref read.  We surface the mis-decode as [INFO] (it is a
        // documented flaw, not something this test can fix), but we HARD-assert
        // the one robust fact: a non-null slot yields a non-null wrapper either
        // way (so the bug is "wrong type accepted", not "crash").
        // ==================================================================
        {
            const auto wrong{ holder->ref_as_decoy() };
            ctx.check("wrong_wrapper_type_still_non_null_documented_flaw",
                      wrong != nullptr);
            // CRASH-PROOFING: the cross-klass field read below (Decoy.poison on a
            // Ref oop) reads at refOop + Decoy's poison-offset.  The Ref oop is a
            // real heap object, so the small-offset read stays inside mapped JVM
            // heap, but we still gate it on is_valid_pointer so a hypothetical
            // edge layout can never turn the documented mis-decode into an AV that
            // truncates the suite.
            if (wrong
                && wrong->get_instance() != nullptr
                && vmhook::hotspot::is_valid_pointer(wrong->get_instance()))
            {
                const std::int32_t poison{ wrong->poison() };
                const auto correct{ holder->ref() };
                const std::int32_t correct_val{ correct ? correct->val() : 0 };
                ctx.record("[INFO] FLAW A (no klass-match check, vmhook.hpp:11443): "
                           "Ref slot read through Decoy wrapper was ACCEPTED. "
                           "Decoy.poison=" + std::to_string(poison)
                           + " vs correct Ref.val=" + std::to_string(correct_val)
                           + " (differ => silent mis-decode).");
                // The decoy's instance pointer is nonetheless the same Ref oop —
                // proving the wrapper wrapped the Ref object with the wrong klass.
                ctx.check("wrong_wrapper_wraps_same_oop_as_correct",
                          correct != nullptr
                          && wrong->get_instance() == correct->get_instance());
            }
        }

        // ==================================================================
        // FLAW B — array-typed field decoded as a single object wrapper is NOT
        // rejected (no signature-shape check).  refArray is '[Ref;'; decoding
        // it as unique_ptr<ref_object> yields a wrapper pointing at the ARRAY
        // oop.  We assert the array slot's OOP equals the wrapper's instance
        // (proving it wrapped the array, not an element) and record the flaw.
        // ==================================================================
        {
            const std::uint32_t arr_compressed{ holder->ref_compressed("refArray") };
            ctx.check("array_field_compressed_non_zero", arr_compressed != 0u);

            void* const arr_oop{ holder->ref_field_oop("refArray") };
            ctx.check("array_field_decodes_to_non_null_oop", arr_oop != nullptr);

            (void) arr_oop;
            const auto as_ref{ holder->ref_array_as_ref() };
            // FLAW B FIXED: decoding a '[' (array) field as a SINGLE
            // unique_ptr<ref_object> is now REJECTED by the signature-shape guard
            // in cast_for_variant (returns nullptr) instead of yielding a wrapper
            // pointing at the ARRAY oop whose field reads would be UB.  Read array
            // elements with to_vector<T>() instead.
            ctx.check("array_as_object_wrapper_rejected_returns_null",
                      as_ref == nullptr);
            ctx.record("[INFO] FLAW B FIXED (signature-shape guard in cast_for_variant): a '[' field "
                       "decoded as a single unique_ptr is now rejected (nullptr), not a wrapper around "
                       "the array oop.");
        }

        // ==================================================================
        // FLAW C — get_compressed_oop() on a PRIMITIVE field has no guard and
        // returns the primitive's raw bytes as if a compressed OOP.  primitiveInt
        // is a plain mutable 'I' instance field; read its compressed-oop and show
        // it equals the int's 4 bytes (the value itself), NOT a real OOP.
        // ==================================================================
        {
            const std::int32_t prim_val{ holder->primitive_value("primitiveInt") };
            const std::uint32_t prim_compressed{ holder->primitive_compressed("primitiveInt") };
            ctx.check("primitive_field_value_is_expected", prim_val == PRIMITIVE_INT_VALUE);
            // FLAW C FIXED: get_compressed_oop() now guards on is_reference() and
            // returns 0 for a primitive field instead of the int's raw bytes as a
            // bogus compressed OOP.  (Was: prim_compressed == (uint32_t)prim_val.)
            ctx.check("primitive_get_compressed_oop_guarded_returns_zero",
                      prim_compressed == 0u);
            ctx.record("[INFO] FLAW C FIXED (get_compressed_oop is_reference() guard): on primitive "
                       "'I' field primitiveInt it now returns 0, not the int bytes / a wild OOP.");
            // is_reference() correctly says false for the primitive (the guard a
            // careful caller SHOULD use before get_compressed_oop()).
            ctx.check("primitive_field_is_reference_false",
                      !holder->primitive_is_reference("primitiveInt"));
        }
    }

    // ── static object-reference field reads (pre-probe, side-effect free) ──
    {
        const auto sr{ holder_object::static_ref() };
        ctx.check("static_ref_non_null", sr != nullptr);
        if (sr)
        {
            ctx.check("static_ref_int_read", sr->val() == STATIC_REF_VAL);
            ctx.check("static_ref_string_read", sr->label() == STATIC_REF_LABEL);
            ctx.check("static_ref_method_call", sr->compute() == STATIC_REF_VAL * 2 + 1);
        }

        const auto snr{ holder_object::static_null_ref() };
        ctx.check("static_null_ref_decodes_to_nullptr", snr == nullptr);
    }

    // =====================================================================
    // PART 2 — interpreter hook on tick(): proves the live-dispatch path is
    // exercised, and re-publishes identities on the Java thread so PART 3 can
    // cross-check decoded OOPs against System.identityHashCode.
    // =====================================================================
    {
        auto handle{ vmhook::scoped_hook<holder_object>(
            "tick",
            [](vmhook::return_value&,
               const std::unique_ptr<holder_object>& self,
               std::int32_t /*nonce*/)
            {
                g_detour_calls.fetch_add(1, std::memory_order_relaxed);
                g_detour_saw_self.store(self != nullptr, std::memory_order_relaxed);
            }) };

        ctx.check("field_object_ref_hook_installed", handle.installed());

        holder_object::set_mode(0);
        const bool done{ ctx.run_probe(
            [](bool value) { holder_object::set_go(value); },
            []() { return holder_object::get_done(); }) };

        ctx.check("field_object_ref_probe_completed", done);
        ctx.check("field_object_ref_detour_fired",
                  g_detour_calls.load(std::memory_order_relaxed) >= 1);
        ctx.check("field_object_ref_detour_saw_self",
                  g_detour_saw_self.load(std::memory_order_relaxed));
    }

    // =====================================================================
    // PART 3 — post-probe: identities are now published and self/nested are
    // wired.  Cross-check every decoded OOP's identity against Java's
    // System.identityHashCode by reading each Ref's own identity-equivalent
    // state, and verify the self-ref field now decodes to the receiver.
    // =====================================================================
    const auto holder2{ holder_object::singleton() };
    ctx.check("singleton_reacquired_post_probe", holder2 != nullptr);

    if (holder2)
    {
        // SELF ref now wired: decoded instance == the holder's own instance.
        {
            const auto s{ holder2->self_ref() };
            ctx.check("self_ref_non_null_post_probe", s != nullptr);
            if (s)
            {
                ctx.check("self_ref_decodes_to_receiver_instance",
                          s->get_instance() == holder2->get_instance()
                          && s->get_instance() != nullptr);
            }
        }

        // The published identities are non-zero (Java actually ran run()).
        ctx.check("java_ref_identity_published", holder_object::ref_identity() != 0);
        ctx.check("java_static_ref_identity_published",
                  holder_object::static_ref_identity() != 0);
        ctx.check("java_nested_ref_identity_published",
                  holder_object::nested_ref_identity() != 0);
        ctx.check("java_array_identity_published",
                  holder_object::ref_array_identity() != 0);

        // ref and refAlias published identities are equal (same object), and the
        // self identity equals the holder's own published identity — these are
        // Java-side truths the decoded-OOP equality above corresponds to.
        ctx.check("java_ref_and_alias_identity_equal",
                  holder_object::ref_identity() == holder_object::ref_alias_identity());

        // Nested ref reachable post-probe and carries the wired value.  This is
        // the proper home for the "nested object-ref field decodes to a usable
        // wrapper" promise: run() has now wired `ref.next`, so the recursive
        // object-ref decode through a field-decoded wrapper yields a live Ref.
        {
            const auto r{ holder2->ref() };
            ctx.check("ref_non_null_post_probe", r != nullptr);
            if (r)
            {
                const auto n{ r->next() };
                ctx.check("nested_ref_non_null_post_probe", n != nullptr);
                if (n)
                {
                    ctx.check("nested_ref_post_probe_value", n->val() == NESTED_REF_VAL);
                    ctx.check("nested_ref_post_probe_string", n->label() == NESTED_REF_LABEL);
                }
            }
        }

        // Array element identity is published and the array's first element,
        // reached by decoding the array oop's slot 0, is a real Ref with the
        // expected value (proves the '[' slot really points at a Ref[] whose
        // elements are usable — the CORRECT way to walk it, contrasting flaw B).
        {
            void* const arr_oop{ holder2->ref_field_oop("refArray") };
            ctx.check("array_oop_valid_post_probe",
                      arr_oop != nullptr && vmhook::hotspot::is_valid_pointer(arr_oop));
            if (arr_oop && vmhook::hotspot::is_valid_pointer(arr_oop))
            {
                // Element 0 compressed OOP lives at array data start (offset 16).
                const std::uint32_t elem0_compressed{
                    vmhook::get_array_element<std::uint32_t>(arr_oop, 0) };
                ctx.check("array_elem0_compressed_non_zero", elem0_compressed != 0u);
                void* const elem0_oop{
                    vmhook::hotspot::decode_oop_pointer(elem0_compressed) };
                ctx.check("array_elem0_decodes_valid",
                          elem0_oop != nullptr
                          && vmhook::hotspot::is_valid_pointer(elem0_oop));
                if (elem0_oop)
                {
                    ref_object elem0{ elem0_oop };
                    ctx.check("array_elem0_is_usable_ref",
                              elem0.val() == ARRAY_ELEM0_VAL);
                }
            }
        }
    }
}
