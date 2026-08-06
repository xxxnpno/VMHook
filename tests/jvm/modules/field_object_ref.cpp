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

    // Forward declaration: ref_object::owner() decodes a back-reference to the
    // holding FieldObjectRef, whose wrapper is defined further down.  The body is
    // therefore defined out-of-line after holder_object is complete.
    class holder_object;

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
        // The INHERITED object-reference field (declared on Ref): a back-ref to
        // the holding FieldObjectRef.  Read through the base wrapper here and,
        // because SubRef inherits it, through the SubRef wrapper too.  Defined
        // out-of-line (holder_object is not yet complete here).
        auto owner() -> std::unique_ptr<holder_object>;
        // Boolean-resolver at the helper boundary (allowed outside accessors): is
        // the named field resolvable on THIS wrapper's klass?
        auto field_oop_of(const char* name) -> void*
        {
            const auto proxy{ get_field(name) };
            return proxy.has_value() ? vmhook::field_oop(*proxy) : nullptr;
        }
    };

    // Wrapper for vmhook.fixtures.FieldObjectRef$SubRef — a SUBCLASS of Ref used
    // for the polymorphic / inherited angle.  Reads the SubRef-only `extra` field
    // and the inherited `val`; compute() is overridden on SubRef so dispatch
    // through this concrete wrapper lands in SubRef's body (val*3+extra).
    class sub_ref_object : public vmhook::object<sub_ref_object>
    {
    public:
        explicit sub_ref_object(vmhook::oop_t instance) noexcept
            : vmhook::object<sub_ref_object>{ instance }
        {
        }

        auto val()     -> std::int32_t { return get_field("val")->get(); }
        auto extra()   -> std::int32_t { return get_field("extra")->get(); }
        auto compute() -> std::int32_t { return get_method("compute")->call(); }
        // The INHERITED object field, reached through the CONCRETE SubRef wrapper
        // (declared on the base Ref, present at the same offset on SubRef).
        // Defined out-of-line (holder_object incomplete here).
        auto owner() -> std::unique_ptr<holder_object>;
        auto owner_oop() -> void*
        {
            const auto proxy{ get_field("owner") };
            return proxy.has_value() ? vmhook::field_oop(*proxy) : nullptr;
        }
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

        // POLYMORPHIC: a Ref-declared field holding a SubRef.  Reading it through
        // the BASE ref_object wrapper is a subclass-through-base (IS-A) read; the
        // same slot read through the CONCRETE sub_ref_object wrapper exposes the
        // SubRef-only `extra` field.
        auto poly_ref()        -> std::unique_ptr<ref_object>     { return get_field("polyRef")->get(); }
        auto poly_ref_as_sub() -> std::unique_ptr<sub_ref_object> { return get_field("polyRef")->get(); }

        // SELF-CYCLE: a Ref whose `next` is wired (in run()) to itself.  Reading
        // cycleRef.next must decode to the SAME oop as cycleRef — a one-step
        // self-loop the decode reads WITHOUT recursing.
        auto cycle_ref() -> std::unique_ptr<ref_object> { return get_field("cycleRef")->get(); }
        // DEPTH-2 chain head: chainHead.next.next walks two levels of nested
        // object-ref decode, each level a fresh field decode.
        auto chain_head() -> std::unique_ptr<ref_object> { return get_field("chainHead")->get(); }

        // NULL shapes across every declared reference type (null-slot invariant).
        auto null_obj()   -> std::unique_ptr<ref_object>      { return get_field("nullObj")->get(); }
        auto null_tag()   -> std::unique_ptr<tag_impl_object> { return get_field("nullTag")->get(); }
        auto null_array() -> std::unique_ptr<ref_object>      { return get_field("nullArray")->get(); }
        auto null_boxed() -> std::unique_ptr<integer_object>  { return get_field("nullBoxed")->get(); }
        auto null_str()   -> std::string                      { return get_field("nullStr")->get(); }

        // Mutable scratch slots for the object-reference SET/GET round-trip.
        auto writable_ref() -> std::unique_ptr<ref_object> { return get_field("writableRef")->get(); }
        auto set_target()   -> std::unique_ptr<ref_object> { return get_field("setTarget")->get(); }
        auto writable_str() -> std::string                 { return get_field("writableStr")->get(); }
        // Object-reference SET: rebind `writableRef` to the object the supplied
        // wrapper points at (a null/empty unique_ptr writes a NULL reference).
        // This is the public field_proxy::set(unique_ptr<W>) putfield path; the
        // wrapper's referent must stay reachable (it is always another live field
        // slot of the SAME object in this test) across the store.
        auto set_writable_ref(const std::unique_ptr<ref_object>& target) -> void
        {
            get_field("writableRef")->set(target);
        }
        // String-field SET: rebind `writableStr` to a freshly-built String.
        auto set_writable_str(const std::string& v) -> void { get_field("writableStr")->set(v); }

        // STRING-typed field read into the value_t std::string alternative.
        auto str_ref() -> std::string { return get_field("strRef")->get(); }

        // wrong-wrapper-type read: the SAME Ref-typed `ref` slot, decoded as a
        // Decoy.  The library now REJECTS this confident cross-klass read and
        // returns a null unique_ptr (flaw A FIXED).
        auto ref_as_decoy() -> std::unique_ptr<decoy_object> { return get_field("ref")->get(); }

        // array-vs-object: the `refArray` field is '[' (Ref[]); decoding it as a
        // single unique_ptr<ref_object> is now REJECTED (flaw B fixed).
        auto ref_array_as_ref() -> std::unique_ptr<ref_object> { return get_field("refArray")->get(); }

        // Generic wrong-wrapper-type read of ANY named Ref-typed slot through the
        // Decoy wrapper — the klass-match guard must refuse every one (flaw A), not
        // just the single `ref` slot.  Returns the (expected-null) decoy wrapper.
        auto field_as_decoy(const char* name) -> std::unique_ptr<decoy_object>
        {
            return get_field(name)->get();
        }
        // Generic same-klass Ref read of a named slot (must STILL be usable after
        // the guard) — paired with field_as_decoy to prove the guard is fail-open.
        auto field_as_ref(const char* name) -> std::unique_ptr<ref_object>
        {
            return get_field(name)->get();
        }

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

        // Static-slot raw helpers (mirror+offset path) for the static round-trip /
        // operator-void* identity.  has_value() is a boolean-resolver guard at the
        // helper boundary, not a sentinel inside a wrapper accessor.
        static auto static_compressed(const char* name) -> std::uint32_t
        {
            const auto proxy{ static_field(name) };
            return proxy.has_value() ? proxy->get_compressed_oop() : 0u;
        }
        static auto static_field_oop(const char* name) -> void*
        {
            const auto proxy{ static_field(name) };
            return proxy.has_value() ? vmhook::field_oop(*proxy) : nullptr;
        }
        static auto static_value_as_voidp(const char* name) -> void*
        {
            return static_cast<void*>(static_field(name)->get());
        }
        static auto static_field_is_reference(const char* name) -> bool
        {
            const auto proxy{ static_field(name) };
            return proxy.has_value() && proxy->is_reference();
        }
        static auto static_field_signature(const char* name) -> std::string
        {
            const auto proxy{ static_field(name) };
            return proxy.has_value() ? std::string{ proxy->signature() } : std::string{};
        }

        // ── published identities (exact cross-checks) ──────────────────────
        static auto ref_identity()        -> std::int32_t { return static_field("refIdentity")->get(); }
        static auto ref_alias_identity()  -> std::int32_t { return static_field("refAliasIdentity")->get(); }
        static auto static_ref_identity() -> std::int32_t { return static_field("staticRefIdentity")->get(); }
        static auto nested_ref_identity() -> std::int32_t { return static_field("nestedRefIdentity")->get(); }
        static auto ref_array_identity()  -> std::int32_t { return static_field("refArrayIdentity")->get(); }
        static auto other_identity()      -> std::int32_t { return static_field("otherIdentity")->get(); }
        static auto self_identity()       -> std::int32_t { return static_field("selfIdentity")->get(); }
        static auto poly_ref_identity()   -> std::int32_t { return static_field("polyRefIdentity")->get(); }
        static auto obj_as_ref_identity() -> std::int32_t { return static_field("objAsRefIdentity")->get(); }
        static auto owner_identity()      -> std::int32_t { return static_field("ownerIdentity")->get(); }
        static auto poly_owner_identity() -> std::int32_t { return static_field("polyOwnerIdentity")->get(); }
        static auto cycle_ref_identity()  -> std::int32_t { return static_field("cycleRefIdentity")->get(); }
        static auto chain_tail_identity() -> std::int32_t { return static_field("chainTailIdentity")->get(); }
    };

    // Out-of-line bodies for the inherited-object-field accessors (holder_object
    // is now complete, so the unique_ptr<holder_object> decode is well-formed).
    inline auto ref_object::owner() -> std::unique_ptr<holder_object>
    {
        return get_field("owner")->get();
    }
    inline auto sub_ref_object::owner() -> std::unique_ptr<holder_object>
    {
        return get_field("owner")->get();
    }

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
    constexpr std::int32_t ARRAY_ELEM1_VAL  = 800;
    constexpr std::int32_t ARRAY_LEN        = 2;
    constexpr std::int32_t OTHER_REF_VAL    = 0x6363;
    constexpr std::int32_t PRIMITIVE_INT_VALUE = 0x04D2;   // 1234
    constexpr std::int32_t BOXED_INT_VALUE  = 0x07E5;       // 2021
    constexpr std::int32_t TAG_SLOT_VALUE   = 0x0539;       // 1337
    constexpr std::int32_t POLY_REF_VAL     = 0x7070;
    constexpr std::int32_t POLY_REF_EXTRA   = 0x000A;
    constexpr std::int32_t WRITABLE_REF_VAL = 0x1357;
    constexpr std::int32_t SET_TARGET_VAL   = 0x2468;
    constexpr std::int32_t CYCLE_REF_VAL    = 0x1A2B;
    constexpr std::int32_t CHAIN_HEAD_VAL   = 0x0C0C;
    constexpr std::int32_t CHAIN_MID_VAL    = 0x0D0D;
    constexpr std::int32_t CHAIN_TAIL_VAL   = 0x0E0E;
    const std::string      REF_LABEL        = "ref-of-field";
    const std::string      STATIC_REF_LABEL = "static-ref";
    const std::string      NESTED_REF_LABEL = "nested-ref";
    const std::string      STR_REF_VALUE    = "string-ref-field";
    const std::string      POLY_REF_LABEL    = "poly-ref";
    const std::string      WRITABLE_STR_SEED = "writable-seed";
    const std::string      SET_STR_VALUE     = "set-via-native";

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
        vmhook::register_class<sub_ref_object>("vmhook/fixtures/FieldObjectRef$SubRef");
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
                              vmhook_test::no_object(n));
                    const auto next_proxy{ r->get_field("next") };
                    ctx.check("nested_ref_slot_compressed_zero_pre_probe",
                              next_proxy.has_value()
                              && next_proxy->get_compressed_oop() == 0u);
                    ctx.record("[INFO] nested ref `next` is unwired (null) until the "
                               "probe's run() executes; pre-probe it correctly decodes "
                               "to a null unique_ptr. Non-null nested read asserted in "
                               "PART 3.");

                    // INHERITED OBJECT FIELD pre-probe: `owner` is declared on Ref
                    // and wired to the holder only inside run().  Pre-probe it is a
                    // genuinely-null inherited 'L' slot — it must resolve (the field
                    // EXISTS on Ref) yet decode to a null unique_ptr and a zero
                    // compressed OOP.  The same field reached through a SubRef
                    // wrapper proves the slot is truly inherited.
                    ctx.check("inherited_owner_field_resolves_on_ref",
                              r->get_field("owner").has_value());
                    ctx.check("inherited_owner_is_reference_on_ref",
                              r->get_field("owner").has_value()
                              && r->get_field("owner")->is_reference());
                    const auto own_pre{ r->owner() };
                    ctx.check("inherited_owner_null_pre_probe_decodes_to_nullptr",
                              vmhook_test::no_object(own_pre));
                    ctx.check("inherited_owner_field_oop_nullptr_pre_probe",
                              r->field_oop_of("owner") == nullptr);
                    ctx.record("[INFO] inherited object field `owner` (declared on Ref) "
                               "is unwired (null) pre-probe; it resolves and correctly "
                               "decodes to a null unique_ptr. Non-null inherited read "
                               "(via Ref AND SubRef) asserted in PART 3.");
                }
            }

            // ── SELF-CYCLE + DEPTH-2 CHAIN are wired in run(); pre-probe their
            //    nested `next` slots are still null (the same null-slot invariant
            //    one level deep).  The wired self-loop / two-level walk is asserted
            //    in PART 3. ───────────────────────────────────────────────────────
            {
                const auto cyc{ holder->cycle_ref() };
                ctx.check("cycle_ref_seed_non_null_pre_probe", cyc != nullptr);
                if (cyc)
                {
                    ctx.check("cycle_ref_seed_value", cyc->val() == CYCLE_REF_VAL);
                    ctx.check("cycle_ref_next_null_pre_probe", vmhook_test::no_object(cyc->next()));
                }
                const auto ch{ holder->chain_head() };
                ctx.check("chain_head_seed_non_null_pre_probe", vmhook_test::has_object(ch));
                if (ch)
                {
                    ctx.check("chain_head_seed_value", ch->val() == CHAIN_HEAD_VAL);
                    ctx.check("chain_head_next_null_pre_probe", vmhook_test::no_object(ch->next()));
                }
            }

            // ── NULL instance ref -> null unique_ptr ───────────────────────
            {
                const auto nr{ holder->null_ref() };
                ctx.check("instance_null_ref_decodes_to_nullptr", vmhook_test::no_object(nr));
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
                // The value_t string alternative keys on the TARGET type, not the
                // declared 'Ljava/lang/Object;' signature: an Object-typed slot
                // that holds a real String oop reads back as the String's chars
                // via the blessed as_string() spelling.
                const auto obj_str_proxy{ holder->get_field("objAsString") };
                ctx.check("object_field_string_reads_as_std_string",
                          obj_str_proxy.has_value()
                          && obj_str_proxy->get().as_string() == STR_REF_VALUE);
                // ...and it is the SAME oop as the dedicated `strRef` String field
                // (both hold the SAME interned String literal STR_REF_VALUE).
                ctx.check("object_field_string_same_oop_as_strRef",
                          oop != nullptr && oop == holder->ref_field_oop("strRef"));
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

            // ── FLAW A is PER-DECODE: the cross-klass refusal fires for EVERY
            //    Ref-typed slot, not just `ref`.  Read finalRef / volatileRef /
            //    polyRef (all hold a Ref or a SubRef IS-A Ref) through the Decoy
            //    wrapper — each must be refused (nullptr) — and re-read each through
            //    the correct Ref wrapper to prove the guard stayed fail-open.  The
            //    field's compressed OOP is non-zero throughout (the slot is live),
            //    so a null wrapper here is a REFUSAL, never an empty slot. ─────────
            {
                const char* const ref_typed_slots[]{ "finalRef", "volatileRef", "polyRef" };
                for (const char* const name : ref_typed_slots)
                {
                    const std::string base{ std::string{ "flawA_per_slot_" } + name };
                    // The slot is genuinely non-null (a real Ref/SubRef oop).
                    ctx.check(base + "_slot_is_live",
                              holder->ref_compressed(name) != 0u);
                    // Decoy read of THIS Ref-typed slot is refused.
                    ctx.check(base + "_decoy_read_refused",
                              holder->field_as_decoy(name) == nullptr);
                    // Same slot through the correct Ref wrapper is still usable.
                    const auto ok{ holder->field_as_ref(name) };
                    ctx.check(base + "_ref_read_still_usable",
                              ok != nullptr && ok->get_instance() != nullptr);
                }
                ctx.record("[INFO] FLAW A guard verified PER-SLOT: the cross-klass "
                           "refusal is a property of every Ref-typed decode (finalRef / "
                           "volatileRef / polyRef), not a one-off on `ref`; each stays "
                           "fail-open for the correct Ref wrapper.");
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
                          vmhook_test::no_object(as_ref));
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

            // ==================================================================
            // POLYMORPHIC / INHERITED — a Ref-declared field holding a SubRef.
            // (1) Read through the BASE ref_object wrapper: a subclass-through-
            //     base (IS-A) read is ACCEPTED, the inherited `val` slot reads
            //     back, and the OVERRIDDEN virtual compute() dispatches to
            //     SubRef's body (val*3+extra), NOT Ref's (val*2+1) — the proof
            //     that a field-decoded base wrapper does true virtual dispatch.
            // (2) Read the SAME slot through the CONCRETE sub_ref_object wrapper:
            //     the SubRef-only `extra` field is reachable, and the runtime
            //     klass is SubRef.  Both decode to the SAME oop.
            // ==================================================================
            {
                ctx.check("poly_field_signature_is_Ref",
                          holder->field_signature("polyRef")
                          == "Lvmhook/fixtures/FieldObjectRef$Ref;");

                const auto pbase{ holder->poly_ref() };
                ctx.check("poly_ref_through_base_wrapper_non_null", pbase != nullptr);
                if (pbase)
                {
                    ctx.check("poly_ref_inherited_int_read", pbase->val() == POLY_REF_VAL);
                    // The inherited String slot reads back through the base wrapper.
                    ctx.check("poly_ref_inherited_string_read",
                              pbase->label() == POLY_REF_LABEL);
                    // VIRTUAL DISPATCH: compute() is overridden on SubRef, so a
                    // call through the base wrapper must land in SubRef's body.
                    ctx.check("poly_ref_overridden_method_dispatches_to_subclass",
                              pbase->compute() == POLY_REF_VAL * 3 + POLY_REF_EXTRA);
                    // ...and decidedly NOT Ref's compute() (val*2+1).
                    ctx.check("poly_ref_dispatch_is_not_base_body",
                              pbase->compute() != POLY_REF_VAL * 2 + 1);
                    ctx.check("poly_ref_runtime_klass_is_SubRef",
                              ends_with(runtime_klass_name(pbase->get_instance()), "SubRef"));
                }

                const auto psub{ holder->poly_ref_as_sub() };
                ctx.check("poly_ref_through_concrete_wrapper_non_null", psub != nullptr);
                if (psub)
                {
                    ctx.check("poly_ref_subclass_only_field_read",
                              psub->extra() == POLY_REF_EXTRA);
                    ctx.check("poly_ref_subclass_inherited_field_read",
                              psub->val() == POLY_REF_VAL);
                    ctx.check("poly_ref_subclass_method_dispatch",
                              psub->compute() == POLY_REF_VAL * 3 + POLY_REF_EXTRA);
                }

                // Both wrappers decoded the SAME slot -> the SAME oop.
                if (pbase && psub)
                {
                    ctx.check("poly_ref_base_and_concrete_same_oop",
                              pbase->get_instance() == psub->get_instance()
                              && pbase->get_instance() != nullptr);
                }
            }

            // ==================================================================
            // NULL across EVERY declared reference shape — the null-slot invariant
            // must hold uniformly: a null Object / interface / array / boxed /
            // String slot decodes to a null wrapper (or "" for String), a zero
            // compressed OOP, and a null field_oop.  (The plain Ref `nullRef` and
            // the static null are asserted elsewhere; this is the type matrix.)
            // ==================================================================
            {
                struct null_case { const char* name; const char* sig; };
                const null_case cases[]{
                    { "nullObj",   "Ljava/lang/Object;" },
                    { "nullTag",   "Lvmhook/fixtures/FieldObjectRef$Tag;" },
                    { "nullArray", "[Lvmhook/fixtures/FieldObjectRef$Ref;" },
                    { "nullBoxed", "Ljava/lang/Integer;" },
                    { "nullStr",   "Ljava/lang/String;" },
                };
                for (const auto& c : cases)
                {
                    const std::string base{ std::string{ "null_shape_" } + c.name };
                    ctx.check(base + "_is_reference_true",
                              holder->field_is_reference(c.name));
                    ctx.check(base + "_signature_exact",
                              holder->field_signature(c.name) == c.sig);
                    ctx.check(base + "_compressed_is_zero",
                              holder->ref_compressed(c.name) == 0u);
                    ctx.check(base + "_field_oop_is_nullptr",
                              holder->ref_field_oop(c.name) == nullptr);
                    // value_t::operator void* of a null slot also decodes to null.
                    ctx.check(base + "_value_voidp_is_nullptr",
                              holder->ref_value_as_voidp(c.name) == nullptr);
                }
                // The typed wrappers / string read of the same null slots.
                ctx.check("null_shape_obj_wrapper_nullptr",   vmhook_test::no_object(holder->null_obj()));
                ctx.check("null_shape_tag_wrapper_nullptr",   vmhook_test::no_object(holder->null_tag()));
                ctx.check("null_shape_array_wrapper_nullptr", vmhook_test::no_object(holder->null_array()));
                ctx.check("null_shape_boxed_wrapper_nullptr", vmhook_test::no_object(holder->null_boxed()));
                ctx.check("null_shape_str_empty_string",      holder->null_str().empty());
            }

            // ==================================================================
            // COMPRESSED-OOP ROUND-TRIP across a BATTERY of reference shapes (not
            // just `ref`).  For each non-null reference field: the decoded oop is
            // valid, re-encode(decode(x)) == x, decode(re-encode) lands on the
            // same oop, AND value_t::operator void* agrees with field_oop().
            // This generalises the single-field identity proof to every shape the
            // fixture declares (instance / final / volatile / boxed / interface /
            // string / Object / poly), so a per-shape decode regression is caught.
            // ==================================================================
            {
                const char* const roundtrip_fields[]{
                    "ref", "finalRef", "volatileRef", "boxedInt", "tag",
                    "strRef", "objAsRef", "objAsString", "polyRef", "refArray",
                };
                for (const char* const name : roundtrip_fields)
                {
                    const std::string base{ std::string{ "roundtrip_" } + name };
                    const std::uint32_t compressed{ holder->ref_compressed(name) };
                    void* const decoded{ holder->ref_field_oop(name) };
                    ctx.check(base + "_compressed_non_zero", compressed != 0u);
                    ctx.check(base + "_decodes_valid",
                              decoded != nullptr
                              && vmhook::hotspot::is_valid_pointer(decoded));
                    if (decoded && vmhook::hotspot::is_valid_pointer(decoded))
                    {
                        const std::uint32_t reencoded{
                            vmhook::hotspot::encode_oop_pointer(decoded) };
                        ctx.check(base + "_reencode_equals_compressed",
                                  reencoded == compressed);
                        ctx.check(base + "_decode_reencode_is_identity",
                                  vmhook::hotspot::decode_oop_pointer(reencoded) == decoded);
                        ctx.check(base + "_value_voidp_equals_field_oop",
                                  holder->ref_value_as_voidp(name) == decoded);
                    }
                }
            }

            // ==================================================================
            // DISTINCTNESS MATRIX — independently-allocated reference fields must
            // decode to DISTINCT heap oops, while the two declared aliases (ref /
            // refAlias / objAsRef) must coincide.  Proves the decode is reading
            // each field's OWN slot, not echoing one cached oop everywhere.
            // ==================================================================
            {
                void* const o_ref{ holder->ref_field_oop("ref") };
                void* const o_final{ holder->ref_field_oop("finalRef") };
                void* const o_vol{ holder->ref_field_oop("volatileRef") };
                void* const o_poly{ holder->ref_field_oop("polyRef") };
                void* const o_str{ holder->ref_field_oop("strRef") };
                void* const o_boxed{ holder->ref_field_oop("boxedInt") };
                ctx.check("distinct_ref_vs_final",  o_ref != nullptr && o_ref != o_final);
                ctx.check("distinct_ref_vs_vol",    o_ref != nullptr && o_ref != o_vol);
                ctx.check("distinct_ref_vs_poly",   o_ref != nullptr && o_ref != o_poly);
                ctx.check("distinct_final_vs_vol",  o_final != nullptr && o_final != o_vol);
                ctx.check("distinct_str_vs_ref",    o_str != nullptr && o_str != o_ref);
                ctx.check("distinct_boxed_vs_ref",  o_boxed != nullptr && o_boxed != o_ref);
                // The three declared aliases of the SAME object coincide.
                void* const o_alias{ holder->ref_field_oop("refAlias") };
                void* const o_obj{ holder->ref_field_oop("objAsRef") };
                ctx.check("alias_ref_refAlias_objAsRef_all_equal",
                          o_ref != nullptr && o_ref == o_alias && o_ref == o_obj);
            }

            // ==================================================================
            // OBJECT-REFERENCE SET/GET ROUND-TRIP (the "set" half of get/set).
            // writableRef is a mutable Ref slot seeded with WRITABLE_REF_VAL.
            //   (1) GET sees the seed,
            //   (2) SET it to the `setTarget` object (a putfield via
            //       set(unique_ptr<W>)); GET now sees SET_TARGET_VAL and the same
            //       oop as setTarget,
            //   (3) SET it to a NULL reference (empty unique_ptr); GET decodes to
            //       a null wrapper and a zero compressed OOP — proving a write can
            //       install the null-slot state the read invariant depends on,
            //   (4) RESTORE the original referent so later modules / re-reads see
            //       a clean fixture.
            // setTarget stays a live field the whole time, so its referent is a
            // GC root across every store (GC-safe value contract).
            // ==================================================================
            {
                const auto seed{ holder->writable_ref() };
                ctx.check("set_get_seed_non_null", seed != nullptr);
                if (seed)
                {
                    ctx.check("set_get_seed_value", seed->val() == WRITABLE_REF_VAL);
                }
                void* const original_oop{ holder->ref_field_oop("writableRef") };

                // (2) SET -> setTarget.
                {
                    auto target{ holder->set_target() };
                    void* const target_oop{ holder->ref_field_oop("setTarget") };
                    ctx.check("set_get_target_seed_non_null",
                              target != nullptr && target_oop != nullptr);
                    holder->set_writable_ref(target);
                    const auto after{ holder->writable_ref() };
                    ctx.check("set_get_after_set_non_null", after != nullptr);
                    if (after)
                    {
                        ctx.check("set_get_after_set_value", after->val() == SET_TARGET_VAL);
                        ctx.check("set_get_after_set_same_oop_as_target",
                                  after->get_instance() == target_oop);
                    }
                    ctx.check("set_get_after_set_compressed_matches_target",
                              holder->ref_compressed("writableRef")
                              == holder->ref_compressed("setTarget"));
                }

                // (3) SET -> null reference via an empty unique_ptr.
                {
                    const std::unique_ptr<ref_object> nothing{};
                    holder->set_writable_ref(nothing);
                    const auto after_null{ holder->writable_ref() };
                    ctx.check("set_get_after_null_set_decodes_to_nullptr",
                              vmhook_test::no_object(after_null));
                    ctx.check("set_get_after_null_set_compressed_zero",
                              holder->ref_compressed("writableRef") == 0u);
                    ctx.check("set_get_after_null_set_field_oop_nullptr",
                              holder->ref_field_oop("writableRef") == nullptr);
                }

                // (4) RESTORE the original referent (re-encode the original oop
                // through the writableRef slot via the seed wrapper we still hold).
                if (seed)
                {
                    holder->set_writable_ref(seed);
                    const auto restored{ holder->writable_ref() };
                    ctx.check("set_get_restored_non_null", restored != nullptr);
                    if (restored)
                    {
                        ctx.check("set_get_restored_value",
                                  restored->val() == WRITABLE_REF_VAL);
                        ctx.check("set_get_restored_same_oop",
                                  restored->get_instance() == original_oop);
                    }
                }
            }

            // ==================================================================
            // STRING-FIELD SET/GET ROUND-TRIP — rebinding a String field via
            // set(std::string) is an object-reference store of a freshly-built
            // String (library bug #30).  Write a new value, read it back, then
            // RESTORE the seed so the fixture is left clean.
            // ==================================================================
            {
                ctx.check("str_set_seed_value", holder->writable_str() == WRITABLE_STR_SEED);
                holder->set_writable_str(SET_STR_VALUE);
                ctx.check("str_set_after_write_value",
                          holder->writable_str() == SET_STR_VALUE);
                // The rebound slot still decodes to a valid String oop.
                void* const new_str_oop{ holder->ref_field_oop("writableStr") };
                ctx.check("str_set_rebound_oop_valid",
                          new_str_oop != nullptr
                          && vmhook::hotspot::is_valid_pointer(new_str_oop));
                if (new_str_oop && vmhook::hotspot::is_valid_pointer(new_str_oop))
                {
                    ctx.check("str_set_rebound_runtime_klass_is_String",
                              ends_with(runtime_klass_name(new_str_oop), "String"));
                }
                // RESTORE.
                holder->set_writable_str(WRITABLE_STR_SEED);
                ctx.check("str_set_restored_value",
                          holder->writable_str() == WRITABLE_STR_SEED);
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
            ctx.check("static_null_ref_decodes_to_nullptr", vmhook_test::no_object(snr));

            // ── STATIC-SLOT introspection + compressed-OOP ROUND-TRIP ──────────
            // The static path resolves the slot through the java.lang.Class mirror
            // + field offset, NOT an object header.  The full decode identity must
            // hold there too: is_reference / signature are exact, the compressed
            // OOP is non-zero, re-encode(decode(x)) == x, decode(re-encode) lands on
            // the same oop, operator void* agrees with field_oop(), and the wrapper
            // instance equals the decoded oop.
            ctx.check("static_ref_is_reference_true",
                      holder_object::static_field_is_reference("staticRef"));
            ctx.check("static_ref_signature_is_Ref",
                      holder_object::static_field_signature("staticRef")
                      == "Lvmhook/fixtures/FieldObjectRef$Ref;");
            {
                const std::uint32_t compressed{ holder_object::static_compressed("staticRef") };
                void* const decoded{ holder_object::static_field_oop("staticRef") };
                ctx.check("static_ref_compressed_non_zero", compressed != 0u);
                ctx.check("static_ref_field_oop_valid",
                          decoded != nullptr && vmhook::hotspot::is_valid_pointer(decoded));
                if (decoded && vmhook::hotspot::is_valid_pointer(decoded))
                {
                    const std::uint32_t reencoded{
                        vmhook::hotspot::encode_oop_pointer(decoded) };
                    ctx.check("static_ref_reencode_equals_compressed",
                              reencoded == compressed);
                    ctx.check("static_ref_decode_reencode_is_identity",
                              vmhook::hotspot::decode_oop_pointer(reencoded) == decoded);
                    ctx.check("static_ref_value_voidp_equals_field_oop",
                              holder_object::static_value_as_voidp("staticRef") == decoded);
                    ctx.check("static_ref_wrapper_instance_equals_decoded",
                              sr != nullptr && sr->get_instance() == decoded);
                }
            }
            // The static NULL slot: a null static reference decodes to a null
            // wrapper, a zero compressed OOP, and a null field_oop (the null-slot
            // invariant on the mirror+offset path).
            ctx.check("static_null_ref_is_reference_true",
                      holder_object::static_field_is_reference("staticNullRef"));
            ctx.check("static_null_ref_compressed_is_zero",
                      holder_object::static_compressed("staticNullRef") == 0u);
            ctx.check("static_null_ref_field_oop_is_nullptr",
                      holder_object::static_field_oop("staticNullRef") == nullptr);
            ctx.check("static_null_ref_value_voidp_is_nullptr",
                      holder_object::static_value_as_voidp("staticNullRef") == nullptr);
            // The static and the INSTANCE staticRef-vs-ref oops are DISTINCT objects.
            if (holder)
            {
                ctx.check("static_ref_distinct_from_instance_ref",
                          holder_object::static_field_oop("staticRef") != nullptr
                          && holder_object::static_field_oop("staticRef")
                             != holder->ref_field_oop("ref"));
            }
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
            // objAsRef (Object-declared) and ref (Ref-declared) hold the SAME
            // object, so their PUBLISHED identities (computed Java-side) match —
            // the Java-truth twin of the native "same oop" alias check.
            ctx.check("java_objAsRef_and_ref_identity_equal",
                      holder_object::obj_as_ref_identity() == holder_object::ref_identity());
            // other's published identity differs from the singleton's ref identity.
            ctx.check("java_other_identity_differs_from_ref",
                      holder_object::other_identity() != holder_object::ref_identity());
            // poly identity published, distinct from ref (a different object).
            ctx.check("java_poly_identity_published",
                      holder_object::poly_ref_identity() != 0);
            ctx.check("java_poly_identity_differs_from_ref",
                      holder_object::poly_ref_identity() != holder_object::ref_identity());

            // ── SELF ref is a SHARED ref to the receiver: self.ref oop == the
            //    receiver's own ref oop (a self-loop walked one level deep).  `s`
            //    is a holder_object (self holds `this`), so it carries the SAME
            //    guarded ref_field_oop helper. ──────────────────────────────────
            {
                const auto s{ holder2->self_ref() };
                if (s)
                {
                    void* const self_ref_oop{ s->ref_field_oop("ref") };
                    void* const recv_ref_oop{ holder2->ref_field_oop("ref") };
                    ctx.check("self_ref_nested_ref_equals_receiver_ref",
                              self_ref_oop != nullptr && self_ref_oop == recv_ref_oop);
                }
            }

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

            // ── INHERITED OBJECT FIELD (declared on Ref) decodes to the owner ──
            // `owner` lives on the base Ref and is wired in run() to the holder.
            // (1) Through the BASE ref_object wrapper it decodes to the SINGLETON
            //     (a usable holder_object) — a plain inherited 'L' slot read.
            // (2) Through the CONCRETE SubRef wrapper (polyRef holds a SubRef, which
            //     INHERITS owner at the same offset) it decodes to the SAME holder
            //     oop — proving the inherited slot is honoured by a subclass wrapper.
            {
                const auto r{ holder2->ref() };
                if (r)
                {
                    const auto own{ r->owner() };
                    ctx.check("inherited_owner_non_null_post_probe", own != nullptr);
                    if (own)
                    {
                        // The owner back-ref decodes to the receiver (the holder).
                        ctx.check("inherited_owner_decodes_to_holder",
                                  own->get_instance() == holder2->get_instance()
                                  && own->get_instance() != nullptr);
                        // ...and it is fully usable: its own `ref` reads REF_VAL.
                        const auto owner_ref{ own->ref() };
                        ctx.check("inherited_owner_is_usable_holder",
                                  owner_ref != nullptr && owner_ref->val() == REF_VAL);
                    }
                    // The `ref.owner` oop equals the SINGLETON's own instance oop.
                    ctx.check("inherited_owner_oop_equals_holder_instance",
                              r->field_oop_of("owner") == holder2->get_instance()
                              && r->field_oop_of("owner") != nullptr);
                }

                // Same inherited slot, reached through the concrete SubRef wrapper.
                const auto psub{ holder2->poly_ref_as_sub() };
                ctx.check("inherited_owner_subref_wrapper_non_null", psub != nullptr);
                if (psub)
                {
                    const auto sub_owner{ psub->owner() };
                    ctx.check("inherited_owner_via_subref_non_null", sub_owner != nullptr);
                    if (sub_owner)
                    {
                        ctx.check("inherited_owner_via_subref_decodes_to_holder",
                                  sub_owner->get_instance() == holder2->get_instance()
                                  && sub_owner->get_instance() != nullptr);
                    }
                    // The inherited slot read through the SubRef wrapper resolves to
                    // the SAME holder oop as via the base wrapper — the offset of an
                    // inherited field is identical on the subclass.
                    ctx.check("inherited_owner_subref_oop_equals_holder",
                              psub->owner_oop() == holder2->get_instance()
                              && psub->owner_oop() != nullptr);
                }

                // Java-published witnesses: ref.owner and polyRef.owner both point
                // at the singleton, so both identities equal the singleton's own
                // identity (the Java-truth twin of the native "== holder" checks).
                ctx.check("java_owner_identity_published",
                          holder_object::owner_identity() != 0);
                ctx.check("java_poly_owner_identity_published",
                          holder_object::poly_owner_identity() != 0);
                ctx.check("java_owner_and_poly_owner_identity_equal",
                          holder_object::owner_identity()
                          == holder_object::poly_owner_identity());
            }

            // ── SELF-CYCLE: cycleRef.next is wired to cycleRef itself ──────────
            // The decode reads a slot, never recurses, so a one-step self-loop is
            // observable: cycleRef.next decodes to the SAME oop as cycleRef.  Proven
            // both by oop identity and by reading the same `val` one hop deep.
            {
                const auto cyc{ holder2->cycle_ref() };
                ctx.check("cycle_ref_non_null_post_probe", cyc != nullptr);
                if (cyc)
                {
                    void* const cyc_oop{ cyc->get_instance() };
                    const auto loop{ cyc->next() };
                    ctx.check("cycle_ref_next_non_null_post_probe", loop != nullptr);
                    if (loop)
                    {
                        // The self-loop: next decodes back to the SAME oop.
                        ctx.check("cycle_ref_next_is_self_same_oop",
                                  loop->get_instance() == cyc_oop && cyc_oop != nullptr);
                        // ...and one hop deep reads the SAME val (it IS the same obj).
                        ctx.check("cycle_ref_next_reads_same_value",
                                  loop->val() == CYCLE_REF_VAL);
                        // The compressed OOP of cycleRef.next equals cycleRef's slot.
                        const auto cyc_proxy{ holder2->get_field("cycleRef") };
                        const auto loop_proxy{ cyc->get_field("next") };
                        ctx.check("cycle_ref_next_compressed_equals_self",
                                  cyc_proxy.has_value() && loop_proxy.has_value()
                                  && cyc_proxy->get_compressed_oop() != 0u
                                  && cyc_proxy->get_compressed_oop()
                                     == loop_proxy->get_compressed_oop());
                    }
                }
                ctx.check("java_cycle_ref_identity_published",
                          holder_object::cycle_ref_identity() != 0);
            }

            // ── DEPTH-2 CHAIN: chainHead -> mid -> tail, each a fresh decode ──
            // Walks two levels of nested object-ref decode through field-decoded
            // wrappers and proves each level is a DISTINCT object carrying its own
            // wired value (head/mid/tail), and a method dispatches at the deepest.
            {
                const auto head{ holder2->chain_head() };
                ctx.check("chain_head_non_null_post_probe", head != nullptr);
                if (head)
                {
                    ctx.check("chain_head_value_post_probe", head->val() == CHAIN_HEAD_VAL);
                    const auto mid{ head->next() };
                    ctx.check("chain_mid_non_null", mid != nullptr);
                    if (mid)
                    {
                        ctx.check("chain_mid_value", mid->val() == CHAIN_MID_VAL);
                        ctx.check("chain_mid_distinct_from_head",
                                  mid->get_instance() != head->get_instance()
                                  && mid->get_instance() != nullptr);
                        const auto tail{ mid->next() };
                        ctx.check("chain_tail_non_null", tail != nullptr);
                        if (tail)
                        {
                            ctx.check("chain_tail_value", tail->val() == CHAIN_TAIL_VAL);
                            ctx.check("chain_tail_distinct_from_mid",
                                      tail->get_instance() != mid->get_instance()
                                      && tail->get_instance() != head->get_instance()
                                      && tail->get_instance() != nullptr);
                            // method dispatch TWO levels deep through field-decoded
                            // wrappers.
                            ctx.check("chain_tail_method_dispatch",
                                      tail->compute() == CHAIN_TAIL_VAL * 2 + 1);
                            // The deepest level is unterminated (tail.next is null).
                            ctx.check("chain_tail_next_is_null",
                                      vmhook_test::no_object(tail->next()));
                        }
                    }
                }
                ctx.check("java_chain_tail_identity_published",
                          holder_object::chain_tail_identity() != 0);
            }

            // ── array element walk (the CORRECT way; contrasts flaw B) ──────
            // The '[' slot points at a real Ref[] whose elements are usable Refs.
            {
                void* const arr_oop{ holder2->ref_field_oop("refArray") };
                ctx.check("array_oop_valid_post_probe",
                          arr_oop != nullptr && vmhook::hotspot::is_valid_pointer(arr_oop));
                if (arr_oop && vmhook::hotspot::is_valid_pointer(arr_oop))
                {
                    // The Ref[] header reports the declared element count.
                    ctx.check("array_length_matches_fixture",
                              vmhook::array_length(arr_oop) == ARRAY_LEN);
                    // The array's runtime klass is a Ref[] (its name ends with the
                    // element descriptor, "[Lvmhook/fixtures/FieldObjectRef$Ref;").
                    ctx.check("array_runtime_klass_is_ref_array",
                              ends_with(runtime_klass_name(arr_oop),
                                        "FieldObjectRef$Ref;"));

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
                        ctx.check("array_elem0_method_dispatch",
                                  elem0.compute() == ARRAY_ELEM0_VAL * 2 + 1);
                    }

                    // Element 1 (the SECOND element) decodes to the OTHER Ref and
                    // is distinct from element 0 — proves the per-index stride is
                    // honest, not echoing element 0.
                    const std::uint32_t elem1_compressed{
                        vmhook::get_array_element<std::uint32_t>(arr_oop, 1) };
                    ctx.check("array_elem1_compressed_non_zero", elem1_compressed != 0u);
                    ctx.check("array_elem0_and_elem1_distinct",
                              elem0_compressed != elem1_compressed);
                    void* const elem1_oop{
                        vmhook::hotspot::decode_oop_pointer(elem1_compressed) };
                    if (elem1_oop && vmhook::hotspot::is_valid_pointer(elem1_oop))
                    {
                        ref_object elem1{ elem1_oop };
                        ctx.check("array_elem1_is_usable_ref",
                                  elem1.val() == ARRAY_ELEM1_VAL);
                    }

                    // OUT-OF-BOUNDS read is bounds-checked and yields the zero
                    // default (no fault, no garbage) — the array helper's safety.
                    ctx.check("array_oob_index_returns_zero",
                              vmhook::get_array_element<std::uint32_t>(arr_oop, ARRAY_LEN) == 0u);
                    ctx.check("array_negative_index_returns_zero",
                              vmhook::get_array_element<std::uint32_t>(arr_oop, -1) == 0u);
                }

                // ── '[L' field walked DIRECTLY as a vector of usable wrappers ──
                // value_t::to_vector<ref_object>() is the LIBRARY-blessed way to
                // read a Ref[] field (contrast flaw B's single-wrapper reject): it
                // decodes the array oop and builds one wrapper per element.
                const auto proxy{ holder2->get_field("refArray") };
                if (proxy.has_value())
                {
                    const auto vec{ proxy->get().to_vector<ref_object>() };
                    ctx.check("array_to_vector_length",
                              static_cast<std::int32_t>(vec.size()) == ARRAY_LEN);
                    if (vec.size() == static_cast<std::size_t>(ARRAY_LEN)
                        && vec[0] && vec[1])
                    {
                        ctx.check("array_to_vector_elem0_usable",
                                  vec[0]->val() == ARRAY_ELEM0_VAL);
                        ctx.check("array_to_vector_elem1_usable",
                                  vec[1]->val() == ARRAY_ELEM1_VAL);
                        ctx.check("array_to_vector_elements_distinct",
                                  vec[0]->get_instance() != vec[1]->get_instance()
                                  && vec[0]->get_instance() != nullptr);
                        // method dispatch through a vector-decoded wrapper.
                        ctx.check("array_to_vector_elem0_method",
                                  vec[0]->compute() == ARRAY_ELEM0_VAL * 2 + 1);
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
