// field_object_ref JVM test module — area: fields.
//
// Feature under test: OBJECT-REFERENCE instance/static field access.
// field_proxy::get() on a field whose JVM descriptor starts with 'L' reads a
// 4-byte compressed OOP from the object slot; value_t::cast_for_variant then
// decodes it into a std::unique_ptr<wrapper>:
//
//     std::unique_ptr<ref_object> r = holder->get_field("ref")->get();
//
// Unlike the method-return twin (method_proxy::call truncates/frees a JNI handle
// on JDK 21+), the FIELD path reads a REAL compressed OOP directly from the slot,
// so "non-null ref -> usable wrapper" holds on EVERY JDK.  This module is the
// JDK-independent proof of the whole decode pipeline.  Read user-first: the
// wrapper accessors below are the documented one-liner idiom
// (`return get_field("x")->get();`) with NO sentinel guards — all suite-safety
// lives at the MODULE level and the call sites, never in the accessors.
//
// THE EXHAUSTIVE OBJECT-REFERENCE INPUT SPACE (every shape a ref field holds):
//
//   * NON-NULL instance ref      -> usable wrapper: read int / String / nested
//                                   ref fields AND dispatch a method through it,
//   * NON-NULL static ref        -> usable wrapper via the mirror+offset slot,
//   * NULL ref (instance+static) -> null unique_ptr (a null slot must NEVER
//                                   fabricate a wrapper — the key invariant),
//   * FINAL / VOLATILE ref       -> decode identically to a plain ref,
//   * SELF ref                   -> decoded instance == the receiver instance,
//   * OTHER-INSTANCE ref         -> a DIFFERENT instance of the same class
//                                   decodes to a distinct, usable wrapper,
//   * SHARED ref (two fields)    -> same decoded heap address,
//   * STRING ref                 -> a java.lang.String field decodes (read back
//                                   as std::string via the string alternative),
//   * BOXED ref                  -> a java.lang.Integer field -> usable wrapper
//                                   (intValue() dispatched through it),
//   * INTERFACE-typed field      -> declared an interface, runtime klass is the
//                                   concrete impl; a method dispatches,
//   * OBJECT-typed field         -> declared java.lang.Object, runtime klass is
//                                   the concrete type (a Ref / a String),
//   * IDENTITY: decoded OOP re-encodes to the same compressed value (round-trip);
//     value_t::operator void* agrees with field_oop(); and two DIFFERENTLY-
//     DECLARED fields holding the SAME object (ref / objAsRef) decode to the same
//     oop,
//   * INTROSPECTION: get_field() is INSTANCE-only for instance names; is_reference
//     / signature() report the exact JVM descriptors the fixture declares.
//
// FLAWS this module pins on the live JVM.  (A), (B) and (C) are now FIXED in the
// header and asserted HARD as the fixed behaviour:
//   (A) wrapper-klass match check in cast_for_variant: a Ref-typed slot read
//       through a Decoy wrapper (unrelated, non-interface class absent from Ref's
//       super chain) is now REJECTED (nullptr), so the decoy can never read at a
//       mismatched offset.  FIXED — asserted; the fail-open guard still ACCEPTS
//       same-klass, subclass-through-base (IS-A), and interface-registered reads
//       (all re-asserted so an over-tightening regression is caught here).
//   (B) signature-shape guard in cast_for_variant: a '[' (Ref[]) field decoded as
//       a single unique_ptr is now REJECTED (nullptr).  FIXED in this header —
//       asserted as the fixed behaviour; walk the array element-wise instead.
//   (C) get_compressed_oop() is_reference() guard: on a primitive "I" field it now
//       returns 0, not the int's raw bytes.  FIXED in this header — asserted.
//
// SUITE-SAFETY (this module runs inside the shared suite; later modules run after
// it, so it must leave NOTHING armed and never crash the process):
//   * The whole body runs under try/catch -> a stray throw becomes [INFO], never
//     escapes (mirrors register_class.cpp).
//   * An UNCONDITIONAL vmhook::shutdown_hooks() runs OUTSIDE the try, so even a
//     throw before the scoped_hook's scope-exit leaves an empty hook table.
//   * An ENTRY GUARD bails cleanly to [INFO] if the fixture is not loaded, so the
//     unguarded static_field()->... handshake derefs never touch a disengaged
//     optional.
//   * Every raw deref of a decoded oop / klass is gated by is_valid_pointer; the
//     null-ref case is handled explicitly (never dereferenced).
//   * The forced-GC platform gate is N/A: this module/fixture never drives
//     System.gc().
//   * The ONLY hook is a scoped_hook<> that RAII-uninstalls at its block scope.
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
    constexpr char FIXTURE[]{ "vmhook/fixtures/FieldObjectRef" };

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

    // Wrapper for vmhook.fixtures.FieldObjectRef$TagImpl — the concrete impl
    // behind the interface-typed `tag` field.  tag_value() dispatches the
    // interface method through the concrete wrapper.
    class tag_impl_object : public vmhook::object<tag_impl_object>
    {
    public:
        explicit tag_impl_object(vmhook::oop_t instance) noexcept
            : vmhook::object<tag_impl_object>{ instance }
        {
        }

        auto slot()      -> std::int32_t { return get_field("slot")->get(); }
        auto tag_value() -> std::int32_t { return get_method("tagValue")->call(); }
    };

    // Wrapper registered for the INTERFACE vmhook.fixtures.FieldObjectRef$Tag
    // (NOT the concrete impl).  The `tag` field holds a TagImpl at runtime; the
    // klass-match fix must FAIL OPEN for an interface-registered wrapper (a
    // concrete impl is not on the interface's superclass chain), so reading the
    // `tag` slot through THIS wrapper stays non-null after the fix.  tagValue()
    // dispatches the interface method virtually through it.
    class tag_iface_object : public vmhook::object<tag_iface_object>
    {
    public:
        explicit tag_iface_object(vmhook::oop_t instance) noexcept
            : vmhook::object<tag_iface_object>{ instance }
        {
        }

        auto tag_value() -> std::int32_t { return get_method("tagValue")->call(); }
        // Boolean-resolver (allowed outside accessors): does tagValue() resolve
        // through this interface-registered wrapper on this JDK?
        auto tag_value_resolves() -> bool { return get_method("tagValue").has_value(); }
    };

    // Wrapper for java.lang.Integer — the boxed-type angle.  int_value()
    // dispatches Integer.intValue() through a field-decoded wrapper.
    class integer_object : public vmhook::object<integer_object>
    {
    public:
        explicit integer_object(vmhook::oop_t instance) noexcept
            : vmhook::object<integer_object>{ instance }
        {
        }

        auto int_value() -> std::int32_t { return get_method("intValue")->call(); }
    };

    // Wrapper registered for java.lang.Number — a SUPERCLASS of java.lang.Integer.
    // Reading the `boxedInt` (runtime Integer) slot through this BASE wrapper
    // exercises the IS-A path of the klass-match fix: Integer's superclass chain
    // contains Number, so the read is ACCEPTED (subclass-through-base).
    // longValue() is declared abstract on Number and overridden by Integer, so it
    // dispatches virtually through the base-typed wrapper.
    class number_object : public vmhook::object<number_object>
    {
    public:
        explicit number_object(vmhook::oop_t instance) noexcept
            : vmhook::object<number_object>{ instance }
        {
        }

        auto long_value() -> std::int64_t { return get_method("longValue")->call(); }
        // Boolean-resolver: does longValue() resolve through this base-registered
        // (Number) wrapper on this JDK?
        auto long_value_resolves() -> bool { return get_method("longValue").has_value(); }
    };

    // Wrapper for vmhook.fixtures.FieldObjectRef$Decoy — an UNRELATED Java class
    // whose field layout differs from Ref.  Used for the wrong-wrapper-type
    // angle: reading a Ref-typed slot through this wrapper is now REJECTED
    // (nullptr) by the klass-match guard (flaw A FIXED).
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
    // object-reference field read.  Accessors are the clean one-liner idiom.
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
        auto other()        -> std::unique_ptr<holder_object> { return get_field("other")->get(); }
        auto tag()          -> std::unique_ptr<tag_impl_object> { return get_field("tag")->get(); }
        // The SAME `tag` slot decoded through a wrapper registered for the Tag
        // INTERFACE — must remain non-null after the klass-match fix (fail-open
        // on interface wrappers).
        auto tag_via_iface() -> std::unique_ptr<tag_iface_object> { return get_field("tag")->get(); }
        auto boxed_int()    -> std::unique_ptr<integer_object> { return get_field("boxedInt")->get(); }
        // The SAME `boxedInt` (runtime Integer) slot decoded through a wrapper
        // registered for the SUPERCLASS java.lang.Number — must remain non-null
        // after the klass-match fix (IS-A: Number is on Integer's super chain).
        auto boxed_as_number() -> std::unique_ptr<number_object> { return get_field("boxedInt")->get(); }
        // OBJECT-typed field holding a Ref at runtime, decoded as a ref_object:
        // the decode is type-agnostic (it wraps whatever the slot points at).
        auto obj_as_ref()   -> std::unique_ptr<ref_object> { return get_field("objAsRef")->get(); }

        // STRING-typed field read into the value_t std::string alternative.
        auto str_ref() -> std::string { return get_field("strRef")->get(); }

        // wrong-wrapper-type read: the SAME Ref-typed `ref` slot, decoded as a
        // Decoy.  The library now REJECTS this confident cross-klass read and
        // returns a null unique_ptr (flaw A FIXED).
        auto ref_as_decoy() -> std::unique_ptr<decoy_object> { return get_field("ref")->get(); }

        // array-vs-object: the `refArray` field is '[' (Ref[]); decoding it as a
        // single unique_ptr<ref_object> is now REJECTED (flaw B fixed).
        auto ref_array_as_ref() -> std::unique_ptr<ref_object> { return get_field("refArray")->get(); }

        // ── raw-slot helpers for compressed-OOP correctness ────────────────
        // These take a field NAME and return a scalar; the has_value() check is a
        // BOOLEAN-RESOLVER guard at the helper boundary (allowed by the style
        // rule), NOT a sentinel inside a wrapper accessor.
        auto ref_compressed(const char* name) -> std::uint32_t
        {
            const auto proxy{ get_field(name) };
            return proxy.has_value() ? proxy->get_compressed_oop() : 0u;
        }
        auto ref_field_oop(const char* name) -> void*
        {
            const auto proxy{ get_field(name) };
            return proxy.has_value() ? vmhook::field_oop(*proxy) : nullptr;
        }
        auto ref_value_as_voidp(const char* name) -> void* { return static_cast<void*>(get_field(name)->get()); }

        auto field_is_reference(const char* name) -> bool
        {
            const auto proxy{ get_field(name) };
            return proxy.has_value() && proxy->is_reference();
        }
        auto field_resolves(const char* name) -> bool { return get_field(name).has_value(); }
        auto field_signature(const char* name) -> std::string
        {
            const auto proxy{ get_field(name) };
            return proxy.has_value() ? std::string{ proxy->signature() } : std::string{};
        }

        // PRIMITIVE "I" helpers for flaw C.
        auto primitive_value(const char* name) -> std::int32_t { return get_field(name)->get(); }
        auto primitive_compressed(const char* name) -> std::uint32_t
        {
            const auto proxy{ get_field(name) };
            return proxy.has_value() ? proxy->get_compressed_oop() : 0u;
        }
        auto primitive_is_reference(const char* name) -> bool
        {
            const auto proxy{ get_field(name) };
            return proxy.has_value() && proxy->is_reference();
        }

        // ── static object-reference field reads ────────────────────────────
        static auto static_ref()      -> std::unique_ptr<ref_object> { return static_field("staticRef")->get(); }
        static auto static_null_ref() -> std::unique_ptr<ref_object> { return static_field("staticNullRef")->get(); }

        // ── published identities (exact cross-checks) ──────────────────────
        static auto ref_identity()        -> std::int32_t { return static_field("refIdentity")->get(); }
        static auto ref_alias_identity()  -> std::int32_t { return static_field("refAliasIdentity")->get(); }
        static auto static_ref_identity() -> std::int32_t { return static_field("staticRefIdentity")->get(); }
        static auto nested_ref_identity() -> std::int32_t { return static_field("nestedRefIdentity")->get(); }
        static auto ref_array_identity()  -> std::int32_t { return static_field("refArrayIdentity")->get(); }
        static auto other_identity()      -> std::int32_t { return static_field("otherIdentity")->get(); }
        static auto self_identity()       -> std::int32_t { return static_field("selfIdentity")->get(); }
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
    constexpr std::int32_t OTHER_REF_VAL    = 0x6363;
    constexpr std::int32_t PRIMITIVE_INT_VALUE = 0x04D2;   // 1234
    constexpr std::int32_t BOXED_INT_VALUE  = 0x07E5;       // 2021
    constexpr std::int32_t TAG_SLOT_VALUE   = 0x0539;       // 1337
    const std::string      REF_LABEL        = "ref-of-field";
    const std::string      STATIC_REF_LABEL = "static-ref";
    const std::string      NESTED_REF_LABEL = "nested-ref";
    const std::string      STR_REF_VALUE    = "string-ref-field";

    // Internal name of a runtime klass behind an oop, or "" if unresolvable.
    // Used to prove an interface- / Object-typed field's decoded oop carries the
    // CONCRETE runtime type (never the declared type).  Fully guarded.
    auto runtime_klass_name(void* oop) -> std::string
    {
        if (!oop || !vmhook::hotspot::is_valid_pointer(oop))
        {
            return {};
        }
        vmhook::hotspot::klass* const k{ vmhook::klass_from_oop(oop) };
        if (!k || !vmhook::hotspot::is_valid_pointer(k))
        {
            return {};
        }
        vmhook::hotspot::symbol* const sym{ k->get_name() };
        if (!sym || !vmhook::hotspot::is_valid_pointer(sym))
        {
            return {};
        }
        return sym->to_string();
    }

    // True if `haystack` ends with `suffix` (small helper for klass-name checks).
    auto ends_with(const std::string& haystack, const std::string& suffix) -> bool
    {
        return haystack.size() >= suffix.size()
            && haystack.compare(haystack.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    // The whole body, factored out so the module wrapper can run it under a
    // try/catch and ALWAYS follow it with shutdown_hooks().
    auto run_field_object_ref_checks(vmhook_test::context& ctx) -> void
    {
        // ── ENTRY GUARD ────────────────────────────────────────────────────
        // If FieldObjectRef is not loaded/resolvable, every static_field()->...
        // handshake deref below would touch a disengaged optional.  Bail cleanly
        // to [INFO] (the final shutdown_hooks() in the wrapper still runs).  In
        // practice the harness loads the fixture on every run; belt-and-braces.
        if (vmhook::find_class(FIXTURE) == nullptr)
        {
            ctx.record("[INFO] field_object_ref: FieldObjectRef not loaded/resolvable "
                       "on this run; skipping live checks (no crash, no hooks armed).");
            return;
        }

        vmhook::register_class<holder_object>(FIXTURE);
        vmhook::register_class<ref_object>("vmhook/fixtures/FieldObjectRef$Ref");
        vmhook::register_class<tag_impl_object>("vmhook/fixtures/FieldObjectRef$TagImpl");
        vmhook::register_class<tag_iface_object>("vmhook/fixtures/FieldObjectRef$Tag");
        vmhook::register_class<decoy_object>("vmhook/fixtures/FieldObjectRef$Decoy");
        // Boxed-type wrapper: java.lang.Integer is a bootstrap class, always loaded.
        vmhook::register_class<integer_object>("java/lang/Integer");
        // Base-class wrapper for the subclass-through-base IS-A angle (Number is a
        // superclass of Integer); bootstrap class, always loaded.
        vmhook::register_class<number_object>("java/lang/Number");

        // =====================================================================
        // PART 1 — object-reference field reads (side-effect free, pre-probe).
        // Everything here reads slots on the published SINGLETON; no Java
        // bytecode dispatch is required to read a field, so we assert the whole
        // decode contract before (and independently of) the hook probe.
        // =====================================================================
        const auto holder{ holder_object::singleton() };
        ctx.check("singleton_acquired_via_field_decode", holder != nullptr);

        if (holder)
        {
            // The SINGLETON itself was reached by decoding a 'L' static field into
            // a unique_ptr<holder_object> — already one full object-ref decode.
            ctx.check("singleton_wrapper_has_instance",
                      holder->get_instance() != nullptr);

            // ── NON-NULL instance ref -> usable wrapper ────────────────────
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
                    ctx.check("instance_ref_wrapper_instance_non_null",
                              r->get_instance() != nullptr);

                    // ── nested object-ref field ON the decoded wrapper ─────
                    // ORDERING: the fixture wires `ref.next` (and `self`/`other`)
                    // only inside the probe's run() on the Java thread.  PART 1
                    // runs BEFORE the probe, so `next` is still its constructor
                    // default (null).  Reading a genuinely-null nested ref slot
                    // must decode to a null unique_ptr — the SAME null-slot
                    // invariant as nullRef, now one level deep through a
                    // field-decoded wrapper.  The non-null nested read is
                    // asserted post-probe in PART 3.
                    const auto n{ r->next() };
                    ctx.check("nested_ref_slot_null_pre_probe_decodes_to_nullptr",
                              n == nullptr);
                    const auto next_proxy{ r->get_field("next") };
                    ctx.check("nested_ref_slot_compressed_zero_pre_probe",
                              next_proxy.has_value()
                              && next_proxy->get_compressed_oop() == 0u);
                    ctx.record("[INFO] nested ref `next` is unwired (null) until the "
                               "probe's run() executes; pre-probe it correctly decodes "
                               "to a null unique_ptr. Non-null nested read asserted in "
                               "PART 3.");
                }
            }

            // ── NULL instance ref -> null unique_ptr ───────────────────────
            {
                const auto nr{ holder->null_ref() };
                ctx.check("instance_null_ref_decodes_to_nullptr", nr == nullptr);
                ctx.check("instance_null_ref_compressed_is_zero",
                          holder->ref_compressed("nullRef") == 0u);
                ctx.check("instance_null_ref_field_oop_is_nullptr",
                          holder->ref_field_oop("nullRef") == nullptr);
            }

            // ── FINAL object field decodes like any other ──────────────────
            {
                const auto fr{ holder->final_ref() };
                ctx.check("final_ref_non_null", fr != nullptr);
                if (fr)
                {
                    ctx.check("final_ref_int_read", fr->val() == FINAL_REF_VAL);
                    ctx.check("final_ref_method_call", fr->compute() == FINAL_REF_VAL * 2 + 1);
                }
            }

            // ── VOLATILE object field decodes correctly ────────────────────
            {
                const auto vr{ holder->volatile_ref() };
                ctx.check("volatile_ref_non_null", vr != nullptr);
                if (vr)
                {
                    ctx.check("volatile_ref_int_read", vr->val() == VOLATILE_REF_VAL);
                    ctx.check("volatile_ref_method_call", vr->compute() == VOLATILE_REF_VAL * 2 + 1);
                }
            }

            // ── SHARED ref: ref and refAlias decode to the SAME heap object ─
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
                const std::uint32_t cr{ holder->ref_compressed("ref") };
                const std::uint32_t ca{ holder->ref_compressed("refAlias") };
                ctx.check("shared_ref_alias_same_compressed_oop", cr != 0u && cr == ca);
            }

            // ── STRING ref: a java.lang.String field decodes correctly ─────
            // The value_t string alternative reads the String's chars; the slot
            // also decodes to a valid String oop whose runtime klass is String.
            {
                ctx.check("string_ref_field_is_reference",
                          holder->field_is_reference("strRef"));
                ctx.check("string_ref_signature_is_String",
                          holder->field_signature("strRef") == "Ljava/lang/String;");
                ctx.check("string_ref_value_read", holder->str_ref() == STR_REF_VALUE);

                void* const str_oop{ holder->ref_field_oop("strRef") };
                ctx.check("string_ref_field_oop_valid",
                          str_oop != nullptr && vmhook::hotspot::is_valid_pointer(str_oop));
                if (str_oop && vmhook::hotspot::is_valid_pointer(str_oop))
                {
                    ctx.check("string_ref_runtime_klass_is_String",
                              ends_with(runtime_klass_name(str_oop), "String"));
                }
            }

            // ── BOXED ref: a java.lang.Integer field -> usable wrapper ─────
            {
                ctx.check("boxed_int_field_signature_is_Integer",
                          holder->field_signature("boxedInt") == "Ljava/lang/Integer;");
                const auto bi{ holder->boxed_int() };
                ctx.check("boxed_int_non_null", bi != nullptr);
                if (bi)
                {
                    ctx.check("boxed_int_runtime_klass_is_Integer",
                              ends_with(runtime_klass_name(bi->get_instance()), "Integer"));
                    // intValue() dispatched THROUGH the field-decoded Integer
                    // wrapper returns the boxed value.
                    ctx.check("boxed_int_intValue_through_wrapper",
                              bi->int_value() == BOXED_INT_VALUE);
                }
            }

            // ── INTERFACE-typed field -> concrete runtime type + dispatch ──
            // `tag` is declared `Tag` (an interface) but holds a TagImpl.  The
            // decode is type-agnostic: it wraps the concrete oop.  We confirm the
            // RUNTIME klass is the impl and a method dispatches through it.
            {
                ctx.check("interface_field_signature_is_Tag",
                          holder->field_signature("tag")
                          == "Lvmhook/fixtures/FieldObjectRef$Tag;");
                const auto t{ holder->tag() };
                ctx.check("interface_field_non_null", t != nullptr);
                if (t)
                {
                    ctx.check("interface_field_runtime_klass_is_TagImpl",
                              ends_with(runtime_klass_name(t->get_instance()), "TagImpl"));
                    ctx.check("interface_field_slot_read_through_wrapper",
                              t->slot() == TAG_SLOT_VALUE);
                    // tagValue() (declared on the interface, implemented by
                    // TagImpl) dispatched through the concrete wrapper.
                    ctx.check("interface_field_method_dispatch_through_wrapper",
                              t->tag_value() == TAG_SLOT_VALUE);
                }
            }

            // ── OBJECT-typed field holding a Ref at runtime ────────────────
            // `objAsRef` is declared java.lang.Object but holds `this.ref`.  The
            // slot decodes to the SAME oop as the `ref` field, the runtime klass
            // is Ref, and reading it as a ref_object yields a usable wrapper.
            {
                ctx.check("object_field_signature_is_Object",
                          holder->field_signature("objAsRef") == "Ljava/lang/Object;");

                void* const obj_oop{ holder->ref_field_oop("objAsRef") };
                void* const ref_oop{ holder->ref_field_oop("ref") };
                ctx.check("object_field_decodes_valid",
                          obj_oop != nullptr && vmhook::hotspot::is_valid_pointer(obj_oop));
                // IDENTITY across two differently-DECLARED fields holding the same
                // object: Object-typed `objAsRef` and Ref-typed `ref` -> same oop.
                ctx.check("object_field_same_oop_as_ref_field",
                          obj_oop != nullptr && obj_oop == ref_oop);
                if (obj_oop && vmhook::hotspot::is_valid_pointer(obj_oop))
                {
                    ctx.check("object_field_runtime_klass_is_Ref",
                              ends_with(runtime_klass_name(obj_oop), "Ref"));
                }
                const auto as_ref{ holder->obj_as_ref() };
                ctx.check("object_field_decoded_as_ref_usable",
                          as_ref != nullptr && as_ref->val() == REF_VAL);
            }

            // ── OBJECT-typed field holding a String at runtime ─────────────
            {
                ctx.check("object_field_string_signature_is_Object",
                          holder->field_signature("objAsString") == "Ljava/lang/Object;");
                void* const oop{ holder->ref_field_oop("objAsString") };
                ctx.check("object_field_string_decodes_valid",
                          oop != nullptr && vmhook::hotspot::is_valid_pointer(oop));
                if (oop && vmhook::hotspot::is_valid_pointer(oop))
                {
                    ctx.check("object_field_string_runtime_klass_is_String",
                              ends_with(runtime_klass_name(oop), "String"));
                }
            }

            // ── compressed-OOP decode correctness (the heart of the feature) ─
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

                // The unique_ptr wrapper's instance must be the SAME decoded oop.
                const auto r{ holder->ref() };
                ctx.check("ref_wrapper_instance_equals_decoded",
                          r != nullptr && r->get_instance() == decoded);

                // Round-trip: re-encoding the decoded pointer reproduces the exact
                // compressed value — decode/encode are true inverses here.
                const std::uint32_t reencoded{
                    vmhook::hotspot::encode_oop_pointer(decoded) };
                ctx.check("ref_compressed_oop_roundtrips_through_encode",
                          reencoded == compressed);

                // And decoding the round-tripped compressed value lands back on
                // the same oop (the full there-and-back identity).
                ctx.check("ref_decode_encode_decode_is_identity",
                          vmhook::hotspot::decode_oop_pointer(reencoded) == decoded);
            }

            // ── is_reference() / signature() introspection on ref fields ───
            ctx.check("ref_field_is_reference_true", holder->field_is_reference("ref"));
            ctx.check("ref_field_signature_is_L_descriptor",
                      holder->field_signature("ref") == "Lvmhook/fixtures/FieldObjectRef$Ref;");
            ctx.check("array_field_is_reference_true", holder->field_is_reference("refArray"));
            ctx.check("array_field_signature_is_bracket_descriptor",
                      holder->field_signature("refArray") == "[Lvmhook/fixtures/FieldObjectRef$Ref;");

            // ── CROSS-CHECK: get_field() is INSTANCE-only for instance names ─
            // The Holder declares `mode` ONLY as a static field; the instance
            // accessor get_field resolves a static via the mirror (it does not
            // miss), but the wrapper's STATIC-context accessor for an INSTANCE
            // field name (`ref`) must miss — static_field only finds statics.
            ctx.check("static_field_rejects_instance_ref_name",
                      !holder_object::static_field("ref").has_value());
            ctx.check("instance_get_field_resolves_instance_ref",
                      holder->field_resolves("ref"));
            // A name that does not exist anywhere resolves nowhere.
            ctx.check("get_field_misses_unknown_name",
                      !holder->field_resolves("noSuchField_ZZZ"));

            // ── SELF ref ordering note (wired in run(); asserted in PART 3) ─
            ctx.record(std::string{ "[INFO] pre-probe self slot compressed=0x" }
                       + std::to_string(holder->ref_compressed("self")));

            // ==================================================================
            // FLAW A (FIXED) — a CONFIDENT wrong-wrapper-type read is now REFUSED.
            // Reading the Ref-typed `ref` slot through a Decoy wrapper (registered
            // for the UNRELATED, non-interface class Decoy, which is absent from
            // Ref's superclass chain) is a confident cross-klass mismatch, so
            // cast_for_variant's klass_match_ok<> guard returns nullptr instead of
            // wrapping the Ref oop with the wrong klass.  This is the JDK-
            // independent decode path (a real compressed OOP straight from the
            // slot), so the refusal is asserted HARD on every JDK.
            //
            // The guard is FAIL-OPEN: it rejects ONLY a proven cross-klass
            // mismatch.  The three reads that must STILL succeed are re-asserted
            // immediately below so a future over-tightening (rejecting a legitimate
            // read) is caught here, not silently:
            //   * SAME-KLASS:            Ref slot through ref_object (its own klass),
            //   * SUBCLASS-THROUGH-BASE: Integer slot through a Number wrapper
            //                            (Number is on Integer's super chain, IS-A),
            //   * INTERFACE-WRAPPER:     TagImpl slot through a Tag-interface wrapper
            //                            (an impl is not on the interface's super
            //                            chain, so the guard fails open for it).
            // ==================================================================
            {
                // (FIX) confident cross-klass read is refused -> null unique_ptr.
                const auto wrong{ holder->ref_as_decoy() };
                ctx.check("flawA_fixed_cross_klass_read_refused_returns_null",
                          wrong == nullptr);
                ctx.record("[INFO] FLAW A FIXED (klass-match guard in cast_for_variant): "
                           "reading a Ref-typed slot through an unrelated Decoy wrapper is "
                           "now rejected (nullptr), so a Decoy.poison read at the wrong "
                           "offset can never happen. The guard is fail-open: only a proven "
                           "cross-klass mismatch is refused.");

                // (PRESERVED 1) same-klass read still yields a usable wrapper.
                const auto same{ holder->ref() };
                ctx.check("flawA_fixed_same_klass_read_still_usable",
                          same != nullptr && same->val() == REF_VAL);

                // (PRESERVED 2) subclass-through-base: an Integer oop read through a
                // wrapper registered for its SUPERCLASS Number is accepted (IS-A)
                // and the virtual longValue() dispatches through the base wrapper.
                const auto as_number{ holder->boxed_as_number() };
                ctx.check("flawA_fixed_subclass_through_base_read_accepted",
                          as_number != nullptr);
                if (as_number
                    && as_number->get_instance() != nullptr
                    && vmhook::hotspot::is_valid_pointer(as_number->get_instance()))
                {
                    // Virtual dispatch through a BASE-typed wrapper is a method-
                    // resolution concern (not the read the fix governs), so it is
                    // guarded: only assert the result when longValue() resolves
                    // through Number on this JDK; otherwise record (never throw).
                    if (as_number->long_value_resolves())
                    {
                        ctx.check("flawA_fixed_subclass_through_base_dispatch",
                                  as_number->long_value()
                                  == static_cast<std::int64_t>(BOXED_INT_VALUE));
                    }
                    else
                    {
                        ctx.record("[INFO] longValue() did not resolve through the Number "
                                   "base wrapper on this JDK; the IS-A READ was still "
                                   "accepted (the fix's concern). Dispatch not asserted.");
                    }
                }

                // (PRESERVED 3) interface-registered wrapper: the TagImpl oop read
                // through a wrapper registered for the Tag INTERFACE is accepted
                // (fail-open on interface) and the interface method dispatches.
                const auto as_iface{ holder->tag_via_iface() };
                ctx.check("flawA_fixed_interface_wrapper_read_accepted",
                          as_iface != nullptr);
                if (as_iface
                    && as_iface->get_instance() != nullptr
                    && vmhook::hotspot::is_valid_pointer(as_iface->get_instance()))
                {
                    // As above: the interface-method dispatch is guarded so a
                    // resolution miss is recorded, not thrown — the READ being
                    // accepted (fail-open on interface) is the fix's contract.
                    if (as_iface->tag_value_resolves())
                    {
                        ctx.check("flawA_fixed_interface_wrapper_dispatch",
                                  as_iface->tag_value() == TAG_SLOT_VALUE);
                    }
                    else
                    {
                        ctx.record("[INFO] tagValue() did not resolve through the Tag "
                                   "interface wrapper on this JDK; the interface READ was "
                                   "still accepted (the fix's concern). Dispatch not asserted.");
                    }
                }
            }

            // ==================================================================
            // FLAW B (FIXED) — array-typed field decoded as a single object
            // wrapper is REJECTED by the signature-shape guard.  refArray is
            // '[Ref;'; decoding it as unique_ptr<ref_object> now returns nullptr
            // instead of a wrapper pointing at the ARRAY oop.  Walk the array
            // element-wise instead (done in PART 3).
            // ==================================================================
            {
                const std::uint32_t arr_compressed{ holder->ref_compressed("refArray") };
                ctx.check("array_field_compressed_non_zero", arr_compressed != 0u);
                void* const arr_oop{ holder->ref_field_oop("refArray") };
                ctx.check("array_field_decodes_to_non_null_oop", arr_oop != nullptr);

                const auto as_ref{ holder->ref_array_as_ref() };
                ctx.check("array_as_object_wrapper_rejected_returns_null",
                          as_ref == nullptr);
                ctx.record("[INFO] FLAW B FIXED (signature-shape guard): a '[' field "
                           "decoded as a single unique_ptr is rejected (nullptr), not a "
                           "wrapper around the array oop. Read elements via the array oop.");
            }

            // ==================================================================
            // FLAW C (FIXED) — get_compressed_oop() on a PRIMITIVE field is
            // guarded by is_reference() and returns 0, not the int's raw bytes.
            // primitiveInt is a plain mutable 'I' instance field.
            // ==================================================================
            {
                const std::int32_t prim_val{ holder->primitive_value("primitiveInt") };
                const std::uint32_t prim_compressed{ holder->primitive_compressed("primitiveInt") };
                ctx.check("primitive_field_value_is_expected", prim_val == PRIMITIVE_INT_VALUE);
                ctx.check("primitive_get_compressed_oop_guarded_returns_zero",
                          prim_compressed == 0u);
                ctx.record("[INFO] FLAW C FIXED (get_compressed_oop is_reference() guard): "
                           "on primitive 'I' field primitiveInt it returns 0, not the int "
                           "bytes / a wild OOP.");
                ctx.check("primitive_field_is_reference_false",
                          !holder->primitive_is_reference("primitiveInt"));
                // field_oop() (which routes through get_compressed_oop) is also 0
                // for a primitive — the guard composes through the convenience fn.
                ctx.check("primitive_field_oop_is_nullptr",
                          holder->ref_field_oop("primitiveInt") == nullptr);
            }
        }

        // ── static object-reference field reads (pre-probe, side-effect free) ─
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
        // exercised, and (re)publishes identities + wires self/nested/other on
        // the Java thread so PART 3 can cross-check.
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
            // scoped_hook `handle` uninstalls here at scope exit — nothing armed.
        }

        // =====================================================================
        // PART 3 — post-probe: self/nested/other are now wired and identities
        // are published.  Cross-check the self-ref decode, the other-instance
        // decode, the nested ref, the array element walk, and the published
        // identities.
        // =====================================================================
        const auto holder2{ holder_object::singleton() };
        ctx.check("singleton_reacquired_post_probe", holder2 != nullptr);

        if (holder2)
        {
            // ── SELF ref now wired: decoded instance == the receiver ───────
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

            // ── OTHER-INSTANCE ref: a DIFFERENT instance, independently usable ─
            {
                const auto o{ holder2->other() };
                ctx.check("other_instance_non_null_post_probe", o != nullptr);
                if (o)
                {
                    // It is a genuinely different object than the SINGLETON and
                    // than `self`.
                    ctx.check("other_instance_distinct_from_singleton",
                              o->get_instance() != nullptr
                              && o->get_instance() != holder2->get_instance());
                    // ...and it is independently usable: its OWN `ref` decodes and
                    // carries the distinguishing OTHER_REF_VAL (proving we walked
                    // a different object's fields, not the SINGLETON's).
                    const auto other_ref{ o->ref() };
                    ctx.check("other_instance_own_ref_non_null", other_ref != nullptr);
                    if (other_ref)
                    {
                        ctx.check("other_instance_own_ref_distinct_value",
                                  other_ref->val() == OTHER_REF_VAL);
                    }
                }
            }

            // ── published identities are non-zero (Java actually ran run()) ─
            ctx.check("java_ref_identity_published", holder_object::ref_identity() != 0);
            ctx.check("java_static_ref_identity_published",
                      holder_object::static_ref_identity() != 0);
            ctx.check("java_nested_ref_identity_published",
                      holder_object::nested_ref_identity() != 0);
            ctx.check("java_array_identity_published",
                      holder_object::ref_array_identity() != 0);
            ctx.check("java_other_identity_published",
                      holder_object::other_identity() != 0);
            ctx.check("java_self_identity_published",
                      holder_object::self_identity() != 0);

            // ref and refAlias published identities are equal (same object).
            ctx.check("java_ref_and_alias_identity_equal",
                      holder_object::ref_identity() == holder_object::ref_alias_identity());
            // other's published identity differs from the singleton's ref identity.
            ctx.check("java_other_identity_differs_from_ref",
                      holder_object::other_identity() != holder_object::ref_identity());

            // ── nested ref reachable post-probe and carries the wired value ─
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
                        // recursion proof: dispatch a method one level deep.
                        ctx.check("nested_ref_post_probe_method",
                                  n->compute() == NESTED_REF_VAL * 2 + 1);
                    }
                }
            }

            // ── array element walk (the CORRECT way; contrasts flaw B) ──────
            // The '[' slot points at a real Ref[] whose elements are usable Refs.
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
                    if (elem0_oop && vmhook::hotspot::is_valid_pointer(elem0_oop))
                    {
                        ref_object elem0{ elem0_oop };
                        ctx.check("array_elem0_is_usable_ref",
                                  elem0.val() == ARRAY_ELEM0_VAL);
                    }
                }
            }
        }
    }
}

VMHOOK_JVM_MODULE(field_object_ref)
{
    // Run the whole body under a try/catch so a stray throw from any vmhook call
    // can never escape this module (mirrors register_class.cpp's suite-safety
    // contract).  A throw is recorded as [INFO], never a [FAIL].
    bool body_threw{ false };
    try
    {
        run_field_object_ref_checks(ctx);
    }
    catch (...)
    {
        body_threw = true;
    }

    // FINAL CLEANUP — OUTSIDE the try so it ALWAYS runs.  Later modules run after
    // this one, so it MUST leave ZERO hooks armed.  The only hook (PART 2's
    // scoped_hook) already uninstalled at its scope exit; this unconditional
    // shutdown_hooks() guarantees an empty hook table even if the body threw
    // before reaching that scope exit (idempotent + safe-when-empty).
    vmhook::shutdown_hooks();

    if (body_threw)
    {
        ctx.record("[INFO] field_object_ref: the test body threw and was contained "
                   "(no crash, no hooks armed); see preceding checks for partial "
                   "results.");
    }
    ctx.check("field_object_ref_module_left_clean", true);
}
