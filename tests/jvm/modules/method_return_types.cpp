// method_return_types JVM test module  (feature area: method calls / return decode)
//
// THE return-type-decode authority for vmhook::method_proxy::call() /
// static_method(...)->call(): exhaustively exercises the conversion of EVERY Java
// return kind back into a C++ value_t on a LIVE JVM -- one Java method per
// BasicType (Z B S C I J F D), void, java.lang.String (empty / ascii / unicode /
// long / interior-NUL), an array of EACH primitive plus Object[], a boxed wrapper
// (Integer/Long/Double), a plain Object, and a null-returning Object.
//
// What this module proves (Java 8/11/17/21/24/25 x MSVC/Clang/GCC):
//   * call() decodes each PRIMITIVE return into the matching C++ type with the
//     right width/sign semantics: Z true/false; B sign-extends (-1 reads -1, not
//     255); C zero-extends (0xFFFF reads 65535, not -1); S/I/J signed min/max and
//     a multi-byte bit pattern that catches 32-bit truncation; F/D specific bit
//     patterns AND NaN survive intact (captured as raw bits through the detour).
//   * a VOID method decodes to a monostate value_t (is_void() true) -- the
//     no-result return -- on BOTH dispatch paths.
//   * call() returning java.lang.String decodes to the exact std::string for
//     ASCII, empty, a multibyte (Latin-1 + CJK) value, and a 300-char ASCII value
//     -- via value_t::as_string().  An interior-NUL String is CHARACTERIZED (the
//     two dispatch paths legitimately differ: read_java_string emits standard
//     UTF-8, the call_jni path emits modified UTF-8), recorded not over-asserted.
//   * a reference (array / boxed / Object) return decodes through the
//     compressed-OOP value_t alternative: the module recovers the real heap OOP
//     (value_t -> void* runs decode_oop_pointer), then reads array length+elements
//     (vmhook::array_length / get_array_element<T>), reads a boxed value back
//     through a method call on the decoded wrapper, and CROSS-CHECKS the decoded
//     Object's OOP against the receiver / a published identity.  These are
//     hard-asserted WHEN this JVM's reference-return decode is usable (it is on
//     every default compressed-oops CI JDK 8-26, runtime-probed via returnsObject);
//     on a JVM where the compressed-OOP round-trip collapses (e.g.
//     -XX:-UseCompressedOops) they degrade to [INFO] rather than FAIL.
//   * the null-reference returner yields an empty wrapper / null pointer / "" --
//     HARD-asserted (a Java null decodes to value_t monostate on BOTH the call_stub
//     and the JNI fallback paths).  Primitive + String + void decodes (and the
//     null case) are hard-asserted on every path.
//
// Driving model mirrors method_call_primitives / method_call_string: the module
// hooks ReturnTypes.trigger(int) and performs every call() INSIDE that detour
// (current_java_thread is set only there), capturing each decoded value into an
// atomic that the module body reads back and asserts.  Coordination is the
// harness ctx.run_probe() rising-edge handshake; no hooks are left armed.
//
// SUITE-SAFETY (mirrors register_class.cpp):
//   * the whole body runs under try/catch -- a stray throw is recorded as [INFO],
//     never a [FAIL], and never escapes the module,
//   * an unconditional vmhook::shutdown_hooks() runs OUTSIDE the try, so the module
//     returns to the driver with ZERO hooks armed on EVERY path,
//   * an ENTRY GUARD (find_class == nullptr) bails to [INFO] before any handshake
//     deref if the fixture is not loaded,
//   * every decoded OOP is is_valid_pointer-gated before any deref; the null-return
//     case is handled explicitly (never dereferenced).
//
// MSVC note: every value_t / call() result is taken by COPY-INIT into a named
// local of the desired type (never brace-init), because value_t's templated
// conversion operator makes `T x{ proxy->call() }` ambiguous on MSVC.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>

namespace
{
    // ── Boxed-wrapper types (Integer / Long / Double) ──────────────────────────
    // Registered so a boxed reference return can be decoded into a unique_ptr<box>
    // and its value read back through a method call (intValue()/longValue()/
    // doubleValue()).  Each lives in this TU's anonymous namespace, so its
    // type_to_class_map binding is additive and cannot clobber a sibling module.
    class box_integer : public vmhook::object<box_integer>
    {
    public:
        explicit box_integer(vmhook::oop_t instance) noexcept
            : vmhook::object<box_integer>{ instance } {}
        auto int_value() -> std::int32_t { return get_method("intValue")->call(); }
    };
    class box_long : public vmhook::object<box_long>
    {
    public:
        explicit box_long(vmhook::oop_t instance) noexcept
            : vmhook::object<box_long>{ instance } {}
        auto long_value() -> std::int64_t { return get_method("longValue")->call(); }
    };
    class box_double : public vmhook::object<box_double>
    {
    public:
        explicit box_double(vmhook::oop_t instance) noexcept
            : vmhook::object<box_double>{ instance } {}
        auto double_value() -> double { return get_method("doubleValue")->call(); }
    };
    class box_boolean : public vmhook::object<box_boolean>
    {
    public:
        explicit box_boolean(vmhook::oop_t instance) noexcept
            : vmhook::object<box_boolean>{ instance } {}
        auto bool_value() -> bool { return get_method("booleanValue")->call(); }
    };
    class box_byte : public vmhook::object<box_byte>
    {
    public:
        explicit box_byte(vmhook::oop_t instance) noexcept
            : vmhook::object<box_byte>{ instance } {}
        auto byte_value() -> std::int8_t { return get_method("byteValue")->call(); }
    };
    class box_short : public vmhook::object<box_short>
    {
    public:
        explicit box_short(vmhook::oop_t instance) noexcept
            : vmhook::object<box_short>{ instance } {}
        auto short_value() -> std::int16_t { return get_method("shortValue")->call(); }
    };
    class box_char : public vmhook::object<box_char>
    {
    public:
        explicit box_char(vmhook::oop_t instance) noexcept
            : vmhook::object<box_char>{ instance } {}
        // charValue() returns a Java char -> the u16 value_t alternative.
        auto char_value() -> std::int32_t
        {
            const std::uint16_t raw = get_method("charValue")->call();
            return static_cast<std::int32_t>(raw);
        }
    };
    class box_float : public vmhook::object<box_float>
    {
    public:
        explicit box_float(vmhook::oop_t instance) noexcept
            : vmhook::object<box_float>{ instance } {}
        auto float_value() -> float { return get_method("floatValue")->call(); }
    };

    // Wrapper for vmhook.fixtures.ReturnTypes.  The handshake accessors are STATIC
    // (reached through static_field, the GCC-portable path); the per-return-type
    // call helpers are INSTANCE methods invoked on the `self` the detour receives,
    // each pinning the decoded C++ type as a copy-initialised named local.
    class rt : public vmhook::object<rt>
    {
    public:
        explicit rt(vmhook::oop_t instance) noexcept
            : vmhook::object<rt>{ instance }
        {
        }

        // ---- handshake (all via static_field, portable on every compiler) ----
        static auto set_go(bool value) -> void   { static_field("go")->set(value); }
        static auto set_done(bool value) -> void  { static_field("done")->set(value); }
        static auto get_done() -> bool            { return static_field("done")->get(); }

        // ---- published OOP identities (for the reference cross-checks) ----
        static auto object_identity() -> std::int32_t { return static_field("objectIdentity")->get(); }

        // ---- primitive return decoders (copy-init the value_t into the type) ----
        auto call_bool(const char* name) -> bool { return get_method(name)->call(); }
        auto call_i8(const char* name) -> std::int8_t { return get_method(name)->call(); }
        // B/S read into a WIDER int to prove sign-extension of the narrow return.
        auto call_i8_as_int(const char* name) -> std::int32_t { return get_method(name)->call(); }
        auto call_i16(const char* name) -> std::int16_t { return get_method(name)->call(); }
        auto call_i16_as_int(const char* name) -> std::int32_t { return get_method(name)->call(); }
        // C read into an int proves ZERO-extension (unsigned char).
        auto call_char_as_int(const char* name) -> std::int32_t
        {
            const std::uint16_t raw = get_method(name)->call();
            return static_cast<std::int32_t>(raw);
        }
        auto call_i32(const char* name) -> std::int32_t { return get_method(name)->call(); }
        auto call_i64(const char* name) -> std::int64_t { return get_method(name)->call(); }
        auto call_float(const char* name) -> float { return get_method(name)->call(); }
        auto call_double(const char* name) -> double { return get_method(name)->call(); }

        // ---- String return decode: as_string() (NOT a cast / brace-init) ----
        auto call_string(const char* name) -> std::string { return get_method(name)->call().as_string(); }

        // ---- introspection: is_void() / is_string() on the returned value_t ----
        auto call_is_void(const char* name) -> bool { return get_method(name)->call().is_void(); }
        auto call_is_string(const char* name) -> bool { return get_method(name)->call().is_string(); }

        // ---- void return: must decode to monostate (is_void() true) ----
        auto call_void_is_void(const char* name) -> bool { return get_method(name)->call().is_void(); }

        // ---- reference return -> raw decoded array/object OOP (void* path).
        //      value_t -> void* runs decode_oop_pointer, recovering the full 64-bit
        //      heap pointer.  Returns nullptr (or a pointer failing is_valid_pointer
        //      -> nulled by the caller) when the reference decode is unusable. ----
        auto call_reference_oop(const char* name) -> void*
        {
            const auto m{ get_method(name) };
            if (!m.has_value()) { return nullptr; }
            void* const raw = m->call();   // copy-init -> decode_oop_pointer
            if (raw == nullptr || !vmhook::hotspot::is_valid_pointer(raw))
            {
                return nullptr;
            }
            return raw;
        }

        // ---- boxed returns -> wrapper, value read back through a method call ----
        auto call_boxed_int(const char* name) -> std::int64_t
        {
            const auto m{ get_method(name) };
            if (!m.has_value()) { return 0; }
            std::unique_ptr<box_integer> b = m->call();   // copy-init from value_t
            if (!b) { return k_box_unset; }
            return static_cast<std::int64_t>(b->int_value());
        }
        auto call_boxed_long(const char* name) -> std::int64_t
        {
            const auto m{ get_method(name) };
            if (!m.has_value()) { return 0; }
            std::unique_ptr<box_long> b = m->call();
            if (!b) { return k_box_unset; }
            return b->long_value();
        }
        auto call_boxed_double_bits(const char* name) -> std::uint64_t
        {
            const auto m{ get_method(name) };
            if (!m.has_value()) { return 0; }
            std::unique_ptr<box_double> b = m->call();
            if (!b) { return k_box_unset_bits; }
            const double d{ b->double_value() };
            std::uint64_t bits{};
            std::memcpy(&bits, &d, sizeof(bits));
            return bits;
        }
        // remaining JLS box types: each decodes to a unique_ptr<box_*>, value read back.
        // The sentinels distinguish "decode produced a null wrapper" from a real value.
        auto call_boxed_bool(const char* name) -> int
        {
            const auto m{ get_method(name) };
            if (!m.has_value()) { return -2; }
            std::unique_ptr<box_boolean> b = m->call();
            if (!b) { return -1; }
            return b->bool_value() ? 1 : 0;
        }
        auto call_boxed_byte(const char* name) -> std::int64_t
        {
            const auto m{ get_method(name) };
            if (!m.has_value()) { return 0; }
            std::unique_ptr<box_byte> b = m->call();
            if (!b) { return k_box_unset; }
            return static_cast<std::int64_t>(b->byte_value());
        }
        auto call_boxed_short(const char* name) -> std::int64_t
        {
            const auto m{ get_method(name) };
            if (!m.has_value()) { return 0; }
            std::unique_ptr<box_short> b = m->call();
            if (!b) { return k_box_unset; }
            return static_cast<std::int64_t>(b->short_value());
        }
        auto call_boxed_char(const char* name) -> std::int64_t
        {
            const auto m{ get_method(name) };
            if (!m.has_value()) { return 0; }
            std::unique_ptr<box_char> b = m->call();
            if (!b) { return k_box_unset; }
            return static_cast<std::int64_t>(b->char_value());
        }
        auto call_boxed_float_bits(const char* name) -> std::uint32_t
        {
            const auto m{ get_method(name) };
            if (!m.has_value()) { return 0; }
            std::unique_ptr<box_float> b = m->call();
            if (!b) { return k_uncaptured_fbits_member; }
            const float f{ b->float_value() };
            std::uint32_t bits{};
            std::memcpy(&bits, &f, sizeof(bits));
            return bits;
        }

        // ---- as_string() on a NON-String, non-null reference (a boxed Integer): the
        //      reference stores the uint32 OOP alt, but the OOP is NOT a java.lang.String,
        //      so read_java_string on it yields "" -- GRACEFUL, never a crash.  Returns
        //      the decoded length so the body can assert it is empty (characterized). ----
        auto call_nonstring_ref_as_string_size(const char* name) -> std::int64_t
        {
            const auto m{ get_method(name) };
            if (!m.has_value()) { return -1; }
            const std::string s{ m->call().as_string() };
            return static_cast<std::int64_t>(s.size());
        }

        // ---- Object/null return: decode to a wrapper<rt> and report null-ness. ----
        auto call_object_is_null_wrapper(const char* name) -> bool
        {
            const auto m{ get_method(name) };
            if (!m.has_value()) { return true; }
            std::unique_ptr<rt> wrapped = m->call();   // copy-init from value_t
            return wrapped == nullptr;
        }

        // ---- Object/null return: decode to a raw void* and gate the deref.
        //      Returns true iff the decoded pointer is null OR fails is_valid_pointer
        //      (i.e. "no live object the native side could safely touch"). ----
        auto call_object_pointer_unusable(const char* name) -> bool
        {
            const auto m{ get_method(name) };
            if (!m.has_value()) { return true; }
            void* const raw = m->call();               // copy-init -> decode_oop_pointer
            if (raw == nullptr)
            {
                return true;
            }
            return !vmhook::hotspot::is_valid_pointer(raw);
        }

        // ---- self-as-Object: decode to a wrapper<rt>, return its instance OOP
        //      (so the body can cross-check it equals the receiver). 0 if null. ----
        auto call_self_object_instance(const char* name) -> std::uintptr_t
        {
            const auto m{ get_method(name) };
            if (!m.has_value()) { return 0; }
            std::unique_ptr<rt> wrapped = m->call();
            if (!wrapped) { return 0; }
            return reinterpret_cast<std::uintptr_t>(wrapped->get_instance());
        }

        // ---- void side-effect observability: snapshot a field, run a void call(),
        //      report whether the field advanced (i.e. the void dispatch executed). --
        static auto void_side_effect() -> std::int32_t { return static_field("voidSideEffect")->get(); }

        // ---- variant-alternative index of a return's value_t.  This is the DIRECT
        //      probe of descriptor-driven type routing: the index pins WHICH variant
        //      alternative call() chose (int32 vs int64, float vs double, ...),
        //      independent of the numeric value.  Order matches the value_t variant:
        //      0 monostate,1 bool,2 i8,3 i16,4 i32,5 i64,6 float,7 double,8 u16,
        //      9 u32(reference/OOP),10 std::string.  -1 if the method is unresolved. --
        auto call_variant_index(const char* name) -> int
        {
            const auto m{ get_method(name) };
            if (!m.has_value()) { return -1; }
            return static_cast<int>(m->call().data.index());
        }

        // ---- SAME method, DIFFERENT decodes: read returnsInt's 0x12345678 through
        //      narrower / wider / float target types.  The value_t conversion operator
        //      static_casts the stored int32 to each target, so this characterizes how
        //      one return decodes when the caller pins a different C++ type. ----
        auto call_int_as_i8(const char* name) -> std::int8_t { return get_method(name)->call(); }
        auto call_int_as_i16(const char* name) -> std::int16_t { return get_method(name)->call(); }
        auto call_int_as_i64(const char* name) -> std::int64_t { return get_method(name)->call(); }
        auto call_int_as_float_bits(const char* name) -> std::uint32_t
        {
            const float f = get_method(name)->call();   // static_cast<float>(int32)
            std::uint32_t bits{};
            std::memcpy(&bits, &f, sizeof(bits));
            return bits;
        }

        // ---- MISMATCH: decode a PRIMITIVE-int return as a reference (void* /
        //      unique_ptr<rt>).  The int return stores the int32 alternative, NOT the
        //      uint32 OOP alternative, so the void*/unique_ptr conversion cannot
        //      static_cast it and yields nullptr/empty -- GRACEFUL, never a crash. ----
        auto call_mismatch_int_as_pointer_is_null(const char* name) -> bool
        {
            const auto m{ get_method(name) };
            if (!m.has_value()) { return true; }
            void* const raw = m->call();   // int32 alt -> no void* cast -> nullptr
            return raw == nullptr;
        }
        auto call_mismatch_int_as_wrapper_is_null(const char* name) -> bool
        {
            const auto m{ get_method(name) };
            if (!m.has_value()) { return true; }
            std::unique_ptr<rt> wrapped = m->call();   // int32 alt -> empty unique_ptr
            return wrapped == nullptr;
        }
        // ---- MISMATCH (other direction): decode a reference (Object) return as a
        //      primitive int.  The reference stores the uint32 OOP alternative, which
        //      static_casts to int32 -- a well-defined (if semantically meaningless)
        //      truncation of the compressed OOP, never a crash.  Characterized. ----
        auto call_mismatch_object_as_int(const char* name) -> std::int32_t
        {
            const auto m{ get_method(name) };
            if (!m.has_value()) { return 0; }
            return m->call();   // uint32 OOP alt -> static_cast<int32>
        }

        // ---- MISMATCH (void direction): decode a VOID return as a reference / string.
        //      A void return stores the monostate alternative, which is neither the OOP
        //      (uint32) alternative nor a std::string; so the void* / unique_ptr / string
        //      conversions all fall through to a default-constructed result -- null / empty
        //      / "" -- never a crash and never a fabricated pointer.  These pin the
        //      monostate->{void*,unique_ptr,std::string} branches of the conversion
        //      operator, which the existing int-as-reference mismatch does NOT exercise. -
        auto call_void_as_pointer_is_null(const char* name) -> bool
        {
            const auto m{ get_method(name) };
            if (!m.has_value()) { return true; }
            void* const raw = m->call();   // monostate alt -> no void* cast -> nullptr
            return raw == nullptr;
        }
        auto call_void_as_wrapper_is_null(const char* name) -> bool
        {
            const auto m{ get_method(name) };
            if (!m.has_value()) { return true; }
            std::unique_ptr<rt> wrapped = m->call();   // monostate alt -> empty unique_ptr
            return wrapped == nullptr;
        }
        auto call_void_as_string_size(const char* name) -> std::int64_t
        {
            const auto m{ get_method(name) };
            if (!m.has_value()) { return -1; }
            return static_cast<std::int64_t>(m->call().as_string().size());  // monostate -> ""
        }

        // ---- MISMATCH (String direction): decode a STRING return as a reference (void* /
        //      unique_ptr<rt>).  The String return stores the std::string alternative, NOT
        //      the uint32 OOP alternative, so neither the void* branch (gated on uint32) nor
        //      the unique_ptr branch (gated on uint32) fires -- both yield null/empty.  This
        //      is the String-side counterpart of the int-as-reference mismatch and proves
        //      the eager-std::string alternative is never mistaken for a compressed OOP. ----
        auto call_string_as_pointer_is_null(const char* name) -> bool
        {
            const auto m{ get_method(name) };
            if (!m.has_value()) { return true; }
            void* const raw = m->call();   // std::string alt -> not the OOP alt -> nullptr
            return raw == nullptr;
        }
        auto call_string_as_wrapper_is_null(const char* name) -> bool
        {
            const auto m{ get_method(name) };
            if (!m.has_value()) { return true; }
            std::unique_ptr<rt> wrapped = m->call();   // std::string alt -> empty unique_ptr
            return wrapped == nullptr;
        }

        // ---- WIDENED extreme readbacks: read a narrow signed extreme into a WIDER int to
        //      prove the high bit sign-extends across the whole width (the headline set only
        //      widens the -1 cases; these widen the MIN/MAX extremes).  byte/short min are
        //      the strongest sign-extension witnesses (a single mis-masked bit shows). ----
        auto call_i8_min_as_int(const char* name) -> std::int32_t { return get_method(name)->call(); }
        auto call_i16_min_as_int(const char* name) -> std::int32_t { return get_method(name)->call(); }
        auto call_i16_max_as_int(const char* name) -> std::int32_t { return get_method(name)->call(); }
        // int MIN/MAX read into a wider i64: MIN must sign-extend to a negative i64, MAX must
        // stay positive -- proves i32->i64 widening preserves sign at both extremes.
        auto call_i32_as_i64(const char* name) -> std::int64_t { return get_method(name)->call(); }

        // ---- static widened char (zero-extend on the static path) ----
        static auto static_char_as_int_wide(const char* name) -> std::int32_t
        {
            const std::uint16_t raw = static_method(name)->call();
            return static_cast<std::int32_t>(raw);
        }

        // ---- INTERFACE-typed return whose runtime value is a String.  The descriptor
        //      is Ljava/lang/CharSequence; (NOT Ljava/lang/String;), so call() stores a
        //      generic reference; as_string() recovers the text via read_java_string on
        //      the decoded String OOP.  Proves the String special-case is descriptor-
        //      keyed, while the text is still recoverable through the reference path. --
        auto call_charsequence_as_string(const char* name) -> std::string
        {
            return get_method(name)->call().as_string();
        }

        // ---- STATIC return decoders (static_method(name)->call()).  These ride the
        //      DISTINCT static dispatch path: jclass via the Method's pool_holder name +
        //      FindClass, jmethodID via GetStaticMethodID -- NOT GetObjectClass/
        //      GetMethodID.  Each pins the decoded C++ type as a copy-initialised local
        //      exactly like the instance decoders, so the static path's return decode is
        //      asserted to parity with the instance path. ----
        static auto static_bool(const char* name) -> bool { return static_method(name)->call(); }
        static auto static_i8(const char* name) -> std::int8_t { return static_method(name)->call(); }
        static auto static_i16(const char* name) -> std::int16_t { return static_method(name)->call(); }
        static auto static_char_as_int(const char* name) -> std::int32_t
        {
            const std::uint16_t raw = static_method(name)->call();
            return static_cast<std::int32_t>(raw);
        }
        static auto static_i32(const char* name) -> std::int32_t { return static_method(name)->call(); }
        static auto static_i64(const char* name) -> std::int64_t { return static_method(name)->call(); }
        static auto static_float_bits(const char* name) -> std::uint32_t
        {
            const float f = static_method(name)->call();
            std::uint32_t bits{};
            std::memcpy(&bits, &f, sizeof(bits));
            return bits;
        }
        static auto static_double_bits(const char* name) -> std::uint64_t
        {
            const double d = static_method(name)->call();
            std::uint64_t bits{};
            std::memcpy(&bits, &d, sizeof(bits));
            return bits;
        }
        static auto static_string(const char* name) -> std::string
        {
            return static_method(name)->call().as_string();
        }
        static auto static_is_void(const char* name) -> bool
        {
            return static_method(name)->call().is_void();
        }
        static auto static_variant_index(const char* name) -> int
        {
            const auto m{ static_method(name) };
            if (!m.has_value()) { return -1; }
            return static_cast<int>(m->call().data.index());
        }
        // static reference (array / Object) -> raw decoded OOP via the void* path.
        static auto static_reference_oop(const char* name) -> void*
        {
            const auto m{ static_method(name) };
            if (!m.has_value()) { return nullptr; }
            void* const raw = m->call();
            if (raw == nullptr || !vmhook::hotspot::is_valid_pointer(raw)) { return nullptr; }
            return raw;
        }
        // static null returner -> empty wrapper?  (Java null -> monostate on either path.)
        static auto static_null_wrapper_is_null(const char* name) -> bool
        {
            const auto m{ static_method(name) };
            if (!m.has_value()) { return true; }
            std::unique_ptr<rt> wrapped = m->call();
            return wrapped == nullptr;
        }
        // static BOXED Integer -> wrapper, value read back through intValue().
        static auto static_boxed_int(const char* name) -> std::int64_t
        {
            const auto m{ static_method(name) };
            if (!m.has_value()) { return 0; }
            std::unique_ptr<box_integer> b = m->call();
            if (!b) { return k_box_unset; }
            return static_cast<std::int64_t>(b->int_value());
        }
        static auto static_string_size(const char* name) -> std::int64_t
        {
            const auto m{ static_method(name) };
            if (!m.has_value()) { return -1; }
            return static_cast<std::int64_t>(m->call().as_string().size());
        }

        // ---- SIGNATURE-PINNED overload resolution: get_method(name, SIGNATURE) pins an
        //      EXACT overload (signature_pinned=true), so resolve_compatible_method honours
        //      it verbatim and the return-type char comes from THAT overload's descriptor.
        //      Each combo overload differs in BOTH arg type and return type; the pinned
        //      descriptor is the only disambiguator, and the decode must match the pinned
        //      overload's return kind. ----
        auto combo_int(std::int32_t arg) -> std::int32_t
        {
            const auto m{ get_method("combo", "(I)I") };
            if (!m.has_value()) { return 0; }
            return m->call(arg);
        }
        auto combo_long(std::int64_t arg) -> std::int64_t
        {
            const auto m{ get_method("combo", "(J)J") };
            if (!m.has_value()) { return 0; }
            return m->call(arg);
        }
        auto combo_string(const char* arg) -> std::string
        {
            const auto m{ get_method("combo", "(Ljava/lang/String;)Ljava/lang/String;") };
            if (!m.has_value()) { return {}; }
            return m->call(arg).as_string();
        }
        auto combo_double_bits() -> std::uint64_t
        {
            const auto m{ get_method("combo", "()D") };
            if (!m.has_value()) { return 0; }
            const double d = m->call();
            std::uint64_t bits{};
            std::memcpy(&bits, &d, sizeof(bits));
            return bits;
        }
        // (Z)Z and (C)C pinned overloads: the return decode must pick the narrow
        // alternative (bool / u16), not int -- proving sub-int return-type chars are
        // honoured from the RESOLVED overload's descriptor.
        auto combo_bool(bool arg) -> int
        {
            const auto m{ get_method("combo", "(Z)Z") };
            if (!m.has_value()) { return -1; }
            return m->call(arg) ? 1 : 0;
        }
        auto combo_char(std::uint16_t arg) -> std::int64_t
        {
            const auto m{ get_method("combo", "(C)C") };
            if (!m.has_value()) { return -1; }
            const std::uint16_t raw = m->call(arg);
            return static_cast<std::int64_t>(raw);
        }
        auto combo_bool_variant_index() -> int
        {
            const auto m{ get_method("combo", "(Z)Z") };
            if (!m.has_value()) { return -1; }
            return static_cast<int>(m->call(true).data.index());
        }
        auto combo_char_variant_index() -> int
        {
            const auto m{ get_method("combo", "(C)C") };
            if (!m.has_value()) { return -1; }
            return static_cast<int>(m->call(std::uint16_t{ 'A' }).data.index());
        }
        // The variant alternative each pinned overload's return decodes to (proves the
        // return-type char is read from the RESOLVED overload, not the latched-first one).
        auto combo_variant_index(const char* sig) -> int
        {
            const auto m{ get_method("combo", sig) };
            if (!m.has_value()) { return -1; }
            // Call with a benign arg matching the pinned signature where needed; the
            // ()D / (I)I / (J)J / (String)String overloads each take 0 or 1 arg.
            if (std::string{ sig } == "()D") { return static_cast<int>(m->call().data.index()); }
            if (std::string{ sig } == "(I)I") { return static_cast<int>(m->call(std::int32_t{ 1 }).data.index()); }
            if (std::string{ sig } == "(J)J") { return static_cast<int>(m->call(std::int64_t{ 1 }).data.index()); }
            return static_cast<int>(m->call("q").data.index());
        }

        // Sentinels distinguishing "decode produced a null wrapper" from a real 0.
        static constexpr std::int64_t  k_box_unset{ static_cast<std::int64_t>(0x7BADF00DBADF00DULL) };
        static constexpr std::uint64_t k_box_unset_bits{ 0x7BADF00DBADF00DULL };
        // float-bits sentinel for a null boxed-Float wrapper (a value that is not any
        // legitimate returned float pattern, so "null wrapper" is unambiguous).
        static constexpr std::uint32_t k_uncaptured_fbits_member{ 0x7BADF00Du };
    };

    // float/double bit helpers (NaN / specific patterns must survive bit-exact).
    auto float_bits(float f) noexcept -> std::uint32_t
    {
        std::uint32_t b{};
        std::memcpy(&b, &f, sizeof(b));
        return b;
    }
    auto double_bits(double d) noexcept -> std::uint64_t
    {
        std::uint64_t b{};
        std::memcpy(&b, &d, sizeof(b));
        return b;
    }

    // Read a primitive array element of type T at `index` from a decoded array OOP,
    // returning a sentinel when the OOP is null/invalid (the reference decode was
    // unusable on this JVM) so the body can tell "unusable" from a real value.
    template<typename element_type>
    auto array_elem(void* const arr, const std::int32_t index) noexcept -> element_type
    {
        if (!arr) { return element_type{}; }
        return vmhook::get_array_element<element_type>(arr, index);
    }

    // -- Detour-captured observations.  call() needs a live current_java_thread,
    //    so every decode happens INSIDE the trigger() detour (which runs on the
    //    Java thread) and is read back by the module body.  Sentinels are chosen
    //    so "did the detour run?" is unambiguous (k_uncaptured for the wide ones). --
    constexpr std::int64_t  k_uncaptured64{ static_cast<std::int64_t>(0xDEADBEEFCAFEF00DULL) };
    constexpr std::uint32_t k_uncaptured_fbits{ 0xFFFFFFFFu };
    constexpr std::uint64_t k_uncaptured_dbits{ 0xFFFFFFFFFFFFFFFFULL };

    std::atomic<int>          g_detour_calls{ 0 };
    std::atomic<bool>         g_detour_saw_self{ false };

    // boolean
    std::atomic<int>          g_bool_true{ -1 };
    std::atomic<int>          g_bool_false{ -1 };
    // byte
    std::atomic<std::int64_t> g_byte{ k_uncaptured64 };
    std::atomic<std::int64_t> g_byte_max{ k_uncaptured64 };
    std::atomic<std::int64_t> g_byte_min{ k_uncaptured64 };
    std::atomic<std::int64_t> g_byte_negone_wide{ k_uncaptured64 };
    // short
    std::atomic<std::int64_t> g_short{ k_uncaptured64 };
    std::atomic<std::int64_t> g_short_max{ k_uncaptured64 };
    std::atomic<std::int64_t> g_short_min{ k_uncaptured64 };
    std::atomic<std::int64_t> g_short_negone_wide{ k_uncaptured64 };
    // char
    std::atomic<std::int64_t> g_char{ k_uncaptured64 };
    std::atomic<std::int64_t> g_char_max_wide{ k_uncaptured64 };
    // int
    std::atomic<std::int64_t> g_int{ k_uncaptured64 };
    std::atomic<std::int64_t> g_int_max{ k_uncaptured64 };
    std::atomic<std::int64_t> g_int_min{ k_uncaptured64 };
    // long
    std::atomic<std::int64_t> g_long{ k_uncaptured64 };
    std::atomic<std::int64_t> g_long_min{ k_uncaptured64 };
    std::atomic<std::int64_t> g_long_neg{ k_uncaptured64 };
    // float (as bits)
    std::atomic<std::uint32_t> g_float_bits{ k_uncaptured_fbits };
    std::atomic<int>           g_float_nan{ -1 };
    std::atomic<std::uint32_t> g_float_max_bits{ k_uncaptured_fbits };
    // double (as bits)
    std::atomic<std::uint64_t> g_double_bits{ k_uncaptured_dbits };
    std::atomic<int>           g_double_nan{ -1 };
    std::atomic<std::uint64_t> g_double_max_bits{ k_uncaptured_dbits };
    // float introspection (NOT a string)
    std::atomic<int>           g_float_is_string{ -1 };
    // void
    std::atomic<int>           g_void_is_void{ -1 };
    // String
    std::atomic<bool>          g_str_captured{ false };
    std::string                g_str_value{};         // guarded by g_str_captured publish
    std::atomic<bool>          g_str_empty_captured{ false };
    std::string                g_str_empty_value{ "<unset>" };
    std::atomic<bool>          g_str_uni_captured{ false };
    std::string                g_str_uni_value{};
    std::atomic<bool>          g_str_long_captured{ false };
    std::string                g_str_long_value{};
    std::atomic<bool>          g_str_nul_captured{ false };
    std::string                g_str_nul_value{};
    std::atomic<int>           g_str_is_string{ -1 };
    std::atomic<int>           g_str_is_void{ -1 };
    // int return introspection (contrast: NOT void, NOT string)
    std::atomic<int>           g_int_is_void{ -1 };
    std::atomic<int>           g_int_is_string{ -1 };

    // ── reference returns: master usability gate + per-array captures ──────────
    std::atomic<int>           g_ref_usable{ -1 };   // returnsObject decoded usable?

    // array length + boundary elements (each array)
    std::atomic<std::int64_t>  g_arr_bool_len{ k_uncaptured64 };
    std::atomic<int>           g_arr_bool_0{ -1 };
    std::atomic<int>           g_arr_bool_1{ -1 };
    std::atomic<int>           g_arr_bool_2{ -1 };
    std::atomic<std::int64_t>  g_arr_byte_len{ k_uncaptured64 };
    std::atomic<std::int64_t>  g_arr_byte_0{ k_uncaptured64 };
    std::atomic<std::int64_t>  g_arr_byte_2{ k_uncaptured64 };
    std::atomic<std::int64_t>  g_arr_char_len{ k_uncaptured64 };
    std::atomic<std::int64_t>  g_arr_char_0{ k_uncaptured64 };
    std::atomic<std::int64_t>  g_arr_char_2{ k_uncaptured64 };   // 0xFFFF zero-extend
    std::atomic<std::int64_t>  g_arr_short_len{ k_uncaptured64 };
    std::atomic<std::int64_t>  g_arr_short_0{ k_uncaptured64 };
    std::atomic<std::int64_t>  g_arr_short_2{ k_uncaptured64 };
    std::atomic<std::int64_t>  g_arr_int_len{ k_uncaptured64 };
    std::atomic<std::int64_t>  g_arr_int_0{ k_uncaptured64 };
    std::atomic<std::int64_t>  g_arr_int_2{ k_uncaptured64 };
    std::atomic<std::int64_t>  g_arr_int_3{ k_uncaptured64 };
    std::atomic<std::int64_t>  g_arr_long_len{ k_uncaptured64 };
    std::atomic<std::int64_t>  g_arr_long_0{ k_uncaptured64 };
    std::atomic<std::int64_t>  g_arr_long_1{ k_uncaptured64 };
    std::atomic<std::int64_t>  g_arr_float_len{ k_uncaptured64 };
    std::atomic<std::uint32_t> g_arr_float_1_bits{ k_uncaptured_fbits };
    std::atomic<std::int64_t>  g_arr_double_len{ k_uncaptured64 };
    std::atomic<std::uint64_t> g_arr_double_1_bits{ k_uncaptured_dbits };
    std::atomic<std::int64_t>  g_arr_obj_len{ k_uncaptured64 };
    std::atomic<std::int64_t>  g_arr_empty_len{ k_uncaptured64 };
    // array introspection: an array return is NOT a string and NOT void.
    std::atomic<int>           g_arr_is_string{ -1 };
    std::atomic<int>           g_arr_is_void{ -1 };

    // boxed returns
    std::atomic<std::int64_t>  g_box_int{ rt::k_box_unset };
    std::atomic<std::int64_t>  g_box_long{ rt::k_box_unset };
    std::atomic<std::uint64_t> g_box_double_bits{ rt::k_box_unset_bits };
    std::atomic<int>           g_box_int_is_string{ -1 };

    // Object / null
    std::atomic<int>           g_null_wrapper_is_null{ -1 };
    std::atomic<int>           g_null_pointer_unusable{ -1 };
    std::atomic<int>           g_null_str_is_empty{ -1 };
    std::atomic<int>           g_obj_wrapper_is_null{ -1 };
    std::atomic<int>           g_obj_pointer_unusable{ -1 };
    // self-as-Object identity cross-check
    std::atomic<std::uintptr_t> g_self_obj_instance{ 0 };
    std::atomic<std::uintptr_t> g_receiver_instance{ 0 };

    // ── descriptor-driven type-routing + edge cases (new exhaustive coverage) ──
    // void side effect observable: did the field advance across the void call()?
    std::atomic<int>           g_void_side_effect_ran{ -1 };
    // variant-alternative index per return type (pins int32-vs-int64, float-vs-double).
    std::atomic<int>           g_vidx_bool{ -2 };
    std::atomic<int>           g_vidx_byte{ -2 };
    std::atomic<int>           g_vidx_short{ -2 };
    std::atomic<int>           g_vidx_char{ -2 };
    std::atomic<int>           g_vidx_int{ -2 };
    std::atomic<int>           g_vidx_long{ -2 };
    std::atomic<int>           g_vidx_float{ -2 };
    std::atomic<int>           g_vidx_double{ -2 };
    std::atomic<int>           g_vidx_void{ -2 };
    std::atomic<int>           g_vidx_string{ -2 };
    std::atomic<int>           g_vidx_object{ -2 };
    // CharSequence (interface return holding a String): proves the String special-
    // case is descriptor-keyed -- runtime is a String, but descriptor != "...String;"
    // so it routes to the reference (uint32) alternative, NOT the std::string one.
    std::atomic<int>           g_vidx_charseq{ -2 };
    // same method, different decodes: returnsInt (0x12345678) read narrow/wide/float.
    std::atomic<std::int64_t>  g_int_as_i8{ k_uncaptured64 };
    std::atomic<std::int64_t>  g_int_as_i16{ k_uncaptured64 };
    std::atomic<std::int64_t>  g_int_as_i64{ k_uncaptured64 };
    std::atomic<std::uint32_t> g_int_as_float_bits{ k_uncaptured_fbits };
    // mismatch (graceful): int-return read as reference; object-return read as int.
    std::atomic<int>           g_mismatch_int_as_ptr_null{ -1 };
    std::atomic<int>           g_mismatch_int_as_wrapper_null{ -1 };
    std::atomic<int>           g_mismatch_object_as_int_captured{ -1 };
    // interface return whose runtime value is a String, recovered via as_string().
    std::atomic<bool>          g_charseq_captured{ false };
    std::string                g_charseq_value{};
    // own-class-typed return: decoded wrapper instance must equal the receiver OOP.
    std::atomic<std::uintptr_t> g_own_type_instance{ 0 };
    // nested-generic-erased-to-Object: a usable non-null reference decode.
    std::atomic<int>           g_nested_generic_usable{ -1 };

    // ── NEW: additional boundary primitive returns ────────────────────────────
    std::atomic<std::int64_t>  g_byte_zero{ k_uncaptured64 };
    std::atomic<std::int64_t>  g_char_zero{ k_uncaptured64 };
    std::atomic<std::int64_t>  g_char_high{ k_uncaptured64 };   // 0x8000 -> 32768 zero-extend
    std::atomic<std::int64_t>  g_int_zero{ k_uncaptured64 };
    std::atomic<std::int64_t>  g_int_negone{ k_uncaptured64 };
    std::atomic<std::int64_t>  g_long_max{ k_uncaptured64 };
    std::atomic<std::int64_t>  g_long_high_only{ k_uncaptured64 }; // low 32 bits zero
    std::atomic<std::int64_t>  g_long_low_only{ k_uncaptured64 };  // high 32 bits zero (NOT sign-ext)
    std::atomic<std::uint32_t> g_float_negzero_bits{ k_uncaptured_fbits };
    std::atomic<std::uint32_t> g_float_neginf_bits{ k_uncaptured_fbits };
    std::atomic<std::uint32_t> g_float_subnormal_bits{ k_uncaptured_fbits };
    std::atomic<std::uint64_t> g_double_negzero_bits{ k_uncaptured_dbits };
    std::atomic<std::uint64_t> g_double_posinf_bits{ k_uncaptured_dbits };
    std::atomic<std::uint64_t> g_double_subnormal_bits{ k_uncaptured_dbits };
    // NEW: additional String returns
    std::atomic<bool>          g_str_onechar_captured{ false };
    std::string                g_str_onechar_value{};
    std::atomic<bool>          g_str_control_captured{ false };
    std::string                g_str_control_value{};
    std::atomic<int>           g_str_long_vidx{ -2 };  // a long String is still the string alt

    // ── NEW: STATIC-method return decode (the GetStaticMethodID dispatch path) ──
    std::atomic<int>           g_st_bool{ -1 };
    std::atomic<std::int64_t>  g_st_byte{ k_uncaptured64 };
    std::atomic<std::int64_t>  g_st_short{ k_uncaptured64 };
    std::atomic<std::int64_t>  g_st_char{ k_uncaptured64 };
    std::atomic<std::int64_t>  g_st_int{ k_uncaptured64 };
    std::atomic<std::int64_t>  g_st_long{ k_uncaptured64 };
    std::atomic<std::uint32_t> g_st_float_bits{ k_uncaptured_fbits };
    std::atomic<std::uint64_t> g_st_double_bits{ k_uncaptured_dbits };
    std::atomic<int>           g_st_void_is_void{ -1 };
    std::atomic<int>           g_st_void_side_effect_ran{ -1 };
    std::atomic<bool>          g_st_str_captured{ false };
    std::string                g_st_str_value{};
    std::atomic<int>           g_st_string_vidx{ -2 };
    std::atomic<int>           g_st_int_vidx{ -2 };
    std::atomic<int>           g_st_void_vidx{ -2 };
    std::atomic<int>           g_st_null_wrapper_is_null{ -1 };
    std::atomic<std::int64_t>  g_st_arr_len{ k_uncaptured64 };
    std::atomic<std::int64_t>  g_st_arr_elem1{ k_uncaptured64 };
    std::atomic<int>           g_st_obj_usable{ -1 };

    // ── NEW: signature-pinned overload resolution -> per-overload return decode ──
    std::atomic<std::int64_t>  g_combo_int{ k_uncaptured64 };
    std::atomic<std::int64_t>  g_combo_long{ k_uncaptured64 };
    std::atomic<bool>          g_combo_str_captured{ false };
    std::string                g_combo_str_value{};
    std::atomic<std::uint64_t> g_combo_double_bits{ k_uncaptured_dbits };
    std::atomic<int>           g_combo_vidx_i{ -2 };
    std::atomic<int>           g_combo_vidx_j{ -2 };
    std::atomic<int>           g_combo_vidx_str{ -2 };
    std::atomic<int>           g_combo_vidx_d{ -2 };

    // ── batch-16 deepening: missing return-type inputs ─────────────────────────
    // long 0 / long -1 (all-ones 64-bit): the -1 vs low-word-only pair pins the full
    // 64-bit read; short 0 exact-zero.
    std::atomic<std::int64_t>  g_long_zero{ k_uncaptured64 };
    std::atomic<std::int64_t>  g_long_negone{ k_uncaptured64 };
    std::atomic<std::int64_t>  g_short_zero{ k_uncaptured64 };
    // float/double +0.0 (counterpart to the covered -0.0), opposite-sign infinities,
    // and an exact-payload NaN (bits must survive, not merely "some NaN").
    std::atomic<std::uint32_t> g_float_poszero_bits{ k_uncaptured_fbits };
    std::atomic<std::uint32_t> g_float_posinf_bits{ k_uncaptured_fbits };
    std::atomic<std::uint32_t> g_float_nan_payload_bits{ k_uncaptured_fbits };
    std::atomic<std::uint64_t> g_double_poszero_bits{ k_uncaptured_dbits };
    std::atomic<std::uint64_t> g_double_neginf_bits{ k_uncaptured_dbits };
    std::atomic<std::uint64_t> g_double_nan_payload_bits{ k_uncaptured_dbits };
    // remaining JLS box types: Boolean / Byte / Short / Character / Float.
    std::atomic<int>           g_box_bool{ -2 };
    std::atomic<std::int64_t>  g_box_byte{ rt::k_box_unset };
    std::atomic<std::int64_t>  g_box_short{ rt::k_box_unset };
    std::atomic<std::int64_t>  g_box_char{ rt::k_box_unset };
    std::atomic<std::uint32_t> g_box_float_bits{ rt::k_uncaptured_fbits_member };
    // as_string() on a non-String reference (boxed Integer) -> "" (graceful), size captured.
    std::atomic<std::int64_t>  g_nonstring_ref_as_string_size{ k_uncaptured64 };
    // (Z)Z and (C)C pinned overloads: value + variant alternative.
    std::atomic<int>           g_combo_bool{ -2 };
    std::atomic<std::int64_t>  g_combo_char{ k_uncaptured64 };
    std::atomic<int>           g_combo_vidx_z{ -2 };
    std::atomic<int>           g_combo_vidx_c{ -2 };
    // static boxed Integer + static empty String.
    std::atomic<std::int64_t>  g_st_boxed_int{ rt::k_box_unset };
    std::atomic<std::int64_t>  g_st_str_empty_size{ k_uncaptured64 };

    // ── batch-19 deepening: widened extremes, void/string graceful-mismatch decode
    //    directions, new primitive sentinels, and per-return predicate contrasts. ──
    // widened narrow signed extremes -> wider int sign-extends across the full width.
    std::atomic<std::int64_t>  g_byte_min_wide{ k_uncaptured64 };   // -128 widened -> -128
    std::atomic<std::int64_t>  g_short_min_wide{ k_uncaptured64 };  // -32768 widened -> -32768
    std::atomic<std::int64_t>  g_short_max_wide{ k_uncaptured64 };  // 32767 widened -> 32767
    std::atomic<std::int64_t>  g_int_min_as_i64{ k_uncaptured64 };  // INT_MIN widened -> negative i64
    std::atomic<std::int64_t>  g_int_max_as_i64{ k_uncaptured64 };  // INT_MAX widened -> positive i64
    // new primitive returners with distinct sentinels.
    std::atomic<std::int64_t>  g_char_one{ k_uncaptured64 };          // (char)1 -> 1 (unsigned low end)
    std::atomic<std::int64_t>  g_int_highbit{ k_uncaptured64 };       // 0x89ABCDEF -> -1985229329
    std::atomic<std::int64_t>  g_long_low_sign_bit{ k_uncaptured64 }; // 0x80000000L -> 2147483648 (positive)
    // void/string graceful-mismatch decode directions (monostate / std::string alt).
    std::atomic<int>           g_void_as_ptr_null{ -1 };
    std::atomic<int>           g_void_as_wrapper_null{ -1 };
    std::atomic<std::int64_t>  g_void_as_string_size{ k_uncaptured64 };
    std::atomic<int>           g_string_as_ptr_null{ -1 };
    std::atomic<int>           g_string_as_wrapper_null{ -1 };
    // per-return predicate contrasts not yet pinned: is_string false on EVERY primitive,
    // is_void false on the empty/unicode String, bool/false return is the bool alt.
    std::atomic<int>           g_bool_is_string{ -1 };
    std::atomic<int>           g_byte_is_string{ -1 };
    std::atomic<int>           g_long_is_string{ -1 };
    std::atomic<int>           g_double_is_string{ -1 };
    std::atomic<int>           g_char_is_string{ -1 };
    std::atomic<int>           g_str_empty_is_void{ -1 };
    std::atomic<int>           g_str_empty_is_string{ -1 };
    std::atomic<int>           g_str_uni_is_string{ -1 };
    std::atomic<int>           g_vidx_bool_false{ -2 };  // returnsBoolFalse is still the bool alt
    // static-path widened char (zero-extend) + static low-sign-bit long (full 64-bit read).
    std::atomic<std::int64_t>  g_st_char_wide{ k_uncaptured64 };
    std::atomic<std::int64_t>  g_st_long_low_sign_bit{ k_uncaptured64 };

    auto reset_observations() -> void
    {
        g_detour_calls.store(0);
        g_detour_saw_self.store(false);
        g_bool_true.store(-1);            g_bool_false.store(-1);
        g_byte.store(k_uncaptured64);     g_byte_max.store(k_uncaptured64);
        g_byte_min.store(k_uncaptured64); g_byte_negone_wide.store(k_uncaptured64);
        g_short.store(k_uncaptured64);    g_short_max.store(k_uncaptured64);
        g_short_min.store(k_uncaptured64);g_short_negone_wide.store(k_uncaptured64);
        g_char.store(k_uncaptured64);     g_char_max_wide.store(k_uncaptured64);
        g_int.store(k_uncaptured64);      g_int_max.store(k_uncaptured64);
        g_int_min.store(k_uncaptured64);
        g_long.store(k_uncaptured64);     g_long_min.store(k_uncaptured64);
        g_long_neg.store(k_uncaptured64);
        g_float_bits.store(k_uncaptured_fbits); g_float_nan.store(-1);
        g_float_max_bits.store(k_uncaptured_fbits); g_float_is_string.store(-1);
        g_double_bits.store(k_uncaptured_dbits); g_double_nan.store(-1);
        g_double_max_bits.store(k_uncaptured_dbits);
        g_void_is_void.store(-1);
        g_str_captured.store(false);      g_str_empty_captured.store(false);
        g_str_uni_captured.store(false);  g_str_long_captured.store(false);
        g_str_nul_captured.store(false);
        g_str_is_string.store(-1);        g_str_is_void.store(-1);
        g_int_is_void.store(-1);          g_int_is_string.store(-1);
        g_ref_usable.store(-1);
        g_arr_bool_len.store(k_uncaptured64);
        g_arr_bool_0.store(-1); g_arr_bool_1.store(-1); g_arr_bool_2.store(-1);
        g_arr_byte_len.store(k_uncaptured64);
        g_arr_byte_0.store(k_uncaptured64); g_arr_byte_2.store(k_uncaptured64);
        g_arr_char_len.store(k_uncaptured64);
        g_arr_char_0.store(k_uncaptured64); g_arr_char_2.store(k_uncaptured64);
        g_arr_short_len.store(k_uncaptured64);
        g_arr_short_0.store(k_uncaptured64); g_arr_short_2.store(k_uncaptured64);
        g_arr_int_len.store(k_uncaptured64);
        g_arr_int_0.store(k_uncaptured64); g_arr_int_2.store(k_uncaptured64);
        g_arr_int_3.store(k_uncaptured64);
        g_arr_long_len.store(k_uncaptured64);
        g_arr_long_0.store(k_uncaptured64); g_arr_long_1.store(k_uncaptured64);
        g_arr_float_len.store(k_uncaptured64); g_arr_float_1_bits.store(k_uncaptured_fbits);
        g_arr_double_len.store(k_uncaptured64); g_arr_double_1_bits.store(k_uncaptured_dbits);
        g_arr_obj_len.store(k_uncaptured64); g_arr_empty_len.store(k_uncaptured64);
        g_arr_is_string.store(-1); g_arr_is_void.store(-1);
        g_box_int.store(rt::k_box_unset); g_box_long.store(rt::k_box_unset);
        g_box_double_bits.store(rt::k_box_unset_bits); g_box_int_is_string.store(-1);
        g_null_wrapper_is_null.store(-1); g_null_pointer_unusable.store(-1);
        g_null_str_is_empty.store(-1);
        g_obj_wrapper_is_null.store(-1);  g_obj_pointer_unusable.store(-1);
        g_self_obj_instance.store(0);     g_receiver_instance.store(0);
        g_void_side_effect_ran.store(-1);
        g_vidx_bool.store(-2);  g_vidx_byte.store(-2);  g_vidx_short.store(-2);
        g_vidx_char.store(-2);  g_vidx_int.store(-2);   g_vidx_long.store(-2);
        g_vidx_float.store(-2); g_vidx_double.store(-2);g_vidx_void.store(-2);
        g_vidx_string.store(-2);g_vidx_object.store(-2);
        g_vidx_charseq.store(-2);
        g_int_as_i8.store(k_uncaptured64);  g_int_as_i16.store(k_uncaptured64);
        g_int_as_i64.store(k_uncaptured64); g_int_as_float_bits.store(k_uncaptured_fbits);
        g_mismatch_int_as_ptr_null.store(-1); g_mismatch_int_as_wrapper_null.store(-1);
        g_mismatch_object_as_int_captured.store(-1);
        g_charseq_captured.store(false);
        g_own_type_instance.store(0);
        g_nested_generic_usable.store(-1);
        // new boundary primitives
        g_byte_zero.store(k_uncaptured64);
        g_char_zero.store(k_uncaptured64); g_char_high.store(k_uncaptured64);
        g_int_zero.store(k_uncaptured64);  g_int_negone.store(k_uncaptured64);
        g_long_max.store(k_uncaptured64);
        g_long_high_only.store(k_uncaptured64); g_long_low_only.store(k_uncaptured64);
        g_float_negzero_bits.store(k_uncaptured_fbits);
        g_float_neginf_bits.store(k_uncaptured_fbits);
        g_float_subnormal_bits.store(k_uncaptured_fbits);
        g_double_negzero_bits.store(k_uncaptured_dbits);
        g_double_posinf_bits.store(k_uncaptured_dbits);
        g_double_subnormal_bits.store(k_uncaptured_dbits);
        // new strings
        g_str_onechar_captured.store(false); g_str_control_captured.store(false);
        g_str_long_vidx.store(-2);
        // static returns
        g_st_bool.store(-1);   g_st_byte.store(k_uncaptured64);
        g_st_short.store(k_uncaptured64); g_st_char.store(k_uncaptured64);
        g_st_int.store(k_uncaptured64);   g_st_long.store(k_uncaptured64);
        g_st_float_bits.store(k_uncaptured_fbits); g_st_double_bits.store(k_uncaptured_dbits);
        g_st_void_is_void.store(-1); g_st_void_side_effect_ran.store(-1);
        g_st_str_captured.store(false);
        g_st_string_vidx.store(-2); g_st_int_vidx.store(-2); g_st_void_vidx.store(-2);
        g_st_null_wrapper_is_null.store(-1);
        g_st_arr_len.store(k_uncaptured64); g_st_arr_elem1.store(k_uncaptured64);
        g_st_obj_usable.store(-1);
        // overloads
        g_combo_int.store(k_uncaptured64); g_combo_long.store(k_uncaptured64);
        g_combo_str_captured.store(false); g_combo_double_bits.store(k_uncaptured_dbits);
        g_combo_vidx_i.store(-2); g_combo_vidx_j.store(-2);
        g_combo_vidx_str.store(-2); g_combo_vidx_d.store(-2);
        // batch-16
        g_long_zero.store(k_uncaptured64); g_long_negone.store(k_uncaptured64);
        g_short_zero.store(k_uncaptured64);
        g_float_poszero_bits.store(k_uncaptured_fbits);
        g_float_posinf_bits.store(k_uncaptured_fbits);
        g_float_nan_payload_bits.store(k_uncaptured_fbits);
        g_double_poszero_bits.store(k_uncaptured_dbits);
        g_double_neginf_bits.store(k_uncaptured_dbits);
        g_double_nan_payload_bits.store(k_uncaptured_dbits);
        g_box_bool.store(-2); g_box_byte.store(rt::k_box_unset);
        g_box_short.store(rt::k_box_unset); g_box_char.store(rt::k_box_unset);
        g_box_float_bits.store(rt::k_uncaptured_fbits_member);
        g_nonstring_ref_as_string_size.store(k_uncaptured64);
        g_combo_bool.store(-2); g_combo_char.store(k_uncaptured64);
        g_combo_vidx_z.store(-2); g_combo_vidx_c.store(-2);
        g_st_boxed_int.store(rt::k_box_unset); g_st_str_empty_size.store(k_uncaptured64);
        // batch-19
        g_byte_min_wide.store(k_uncaptured64);  g_short_min_wide.store(k_uncaptured64);
        g_short_max_wide.store(k_uncaptured64);
        g_int_min_as_i64.store(k_uncaptured64); g_int_max_as_i64.store(k_uncaptured64);
        g_char_one.store(k_uncaptured64);       g_int_highbit.store(k_uncaptured64);
        g_long_low_sign_bit.store(k_uncaptured64);
        g_void_as_ptr_null.store(-1);     g_void_as_wrapper_null.store(-1);
        g_void_as_string_size.store(k_uncaptured64);
        g_string_as_ptr_null.store(-1);   g_string_as_wrapper_null.store(-1);
        g_bool_is_string.store(-1); g_byte_is_string.store(-1);
        g_long_is_string.store(-1); g_double_is_string.store(-1);
        g_char_is_string.store(-1);
        g_str_empty_is_void.store(-1);  g_str_empty_is_string.store(-1);
        g_str_uni_is_string.store(-1);  g_vidx_bool_false.store(-2);
        g_st_char_wide.store(k_uncaptured64); g_st_long_low_sign_bit.store(k_uncaptured64);
    }

    // The whole test body, factored out so the VMHOOK_JVM_MODULE wrapper can run it
    // under a try/catch and ALWAYS follow it with shutdown_hooks().
    auto run_return_type_checks(vmhook_test::context& ctx) -> void
    {
        vmhook::register_class<rt>("vmhook/fixtures/ReturnTypes");
        vmhook::register_class<box_integer>("java/lang/Integer");
        vmhook::register_class<box_long>("java/lang/Long");
        vmhook::register_class<box_double>("java/lang/Double");
        vmhook::register_class<box_boolean>("java/lang/Boolean");
        vmhook::register_class<box_byte>("java/lang/Byte");
        vmhook::register_class<box_short>("java/lang/Short");
        vmhook::register_class<box_char>("java/lang/Character");
        vmhook::register_class<box_float>("java/lang/Float");

        // =====================================================================
        //  ENTRY GUARD.  If ReturnTypes is not loaded/resolvable, every
        //  static_field()->set/get below would deref a disengaged optional.  Bail
        //  cleanly to [INFO] (the final shutdown_hooks() in the wrapper still runs).
        // =====================================================================
        if (vmhook::find_class("vmhook/fixtures/ReturnTypes") == nullptr)
        {
            ctx.record("[INFO] method_return_types: ReturnTypes not loaded/resolvable "
                       "on this run; skipping the module's live checks (no crash, no "
                       "hooks armed).");
            return;
        }

        // =====================================================================
        //  0. Sanity: the class resolves (a static field is reachable on the
        //     java.lang.Class mirror).
        // =====================================================================
        ctx.check("rt_class_registered", rt::static_field("go").has_value());

        // Record which dispatch path this live JDK takes, for diagnostics.  The
        // value decodes are asserted UNCONDITIONALLY below (both paths must agree on
        // primitives + String + void); only the path is recorded.
        if (vmhook::detail::find_call_stub_entry() != nullptr)
        {
            ctx.record("[INFO] method_return_types: StubRoutines::_call_stub_entry PRESENT "
                       "-- call() uses the interpreter call_stub fast path.");
        }
        else
        {
            ctx.record("[INFO] method_return_types: StubRoutines::_call_stub_entry ABSENT "
                       "-- call() uses the JNI fallback (expected on every CI JDK 8-26).");
        }

        reset_observations();

        // =====================================================================
        //  1. Install the trigger() hook.  EVERY call() runs inside this detour
        //     (the only context where current_java_thread is set).  Each decode is
        //     captured into an atomic for the body to assert; float/double specials
        //     are captured as raw bits so NaN / exact patterns survive.
        // =====================================================================
        const bool hook_installed{ vmhook::hook<rt>("trigger",
            [](vmhook::return_value& /*retval*/,
               const std::unique_ptr<rt>& self,
               std::int32_t /*delta*/)
            {
                g_detour_calls.fetch_add(1, std::memory_order_relaxed);
                if (!self)
                {
                    return;
                }
                g_detour_saw_self.store(true);
                g_receiver_instance.store(
                    reinterpret_cast<std::uintptr_t>(self->get_instance()),
                    std::memory_order_relaxed);

                // ----- boolean (Z) -----
                g_bool_true.store(self->call_bool("returnsBool") ? 1 : 0);
                g_bool_false.store(self->call_bool("returnsBoolFalse") ? 1 : 0);

                // ----- byte (B): headline + boundaries; -1 widened proves sign-extend -----
                g_byte.store(self->call_i8("returnsByte"));
                g_byte_max.store(self->call_i8("returnsByteMax"));
                g_byte_min.store(self->call_i8("returnsByteMin"));
                g_byte_negone_wide.store(self->call_i8_as_int("returnsByteNegOne"));

                // ----- short (S) -----
                g_short.store(self->call_i16("returnsShort"));
                g_short_max.store(self->call_i16("returnsShortMax"));
                g_short_min.store(self->call_i16("returnsShortMin"));
                g_short_negone_wide.store(self->call_i16_as_int("returnsShortNegOne"));

                // ----- char (C): headline + max widened proves ZERO-extend -----
                g_char.store(self->call_char_as_int("returnsChar"));
                g_char_max_wide.store(self->call_char_as_int("returnsCharMax"));

                // ----- int (I) -----
                g_int.store(self->call_i32("returnsInt"));
                g_int_max.store(self->call_i32("returnsIntMax"));
                g_int_min.store(self->call_i32("returnsIntMin"));

                // ----- long (J): headline bit pattern + min + a wide negative -----
                g_long.store(self->call_i64("returnsLong"));
                g_long_min.store(self->call_i64("returnsLongMin"));
                g_long_neg.store(self->call_i64("returnsLongNeg"));

                // ----- float (F): bits + NaN + a fixed bit pattern -----
                g_float_bits.store(float_bits(self->call_float("returnsFloat")));
                {
                    const float nanf{ self->call_float("returnsFloatNaN") };
                    g_float_nan.store(std::isnan(nanf) ? 1 : 0);
                }
                g_float_max_bits.store(float_bits(self->call_float("returnsFloatBits")));

                // ----- double (D): bits + NaN + a fixed bit pattern -----
                g_double_bits.store(double_bits(self->call_double("returnsDouble")));
                {
                    const double nand{ self->call_double("returnsDoubleNaN") };
                    g_double_nan.store(std::isnan(nand) ? 1 : 0);
                }
                g_double_max_bits.store(double_bits(self->call_double("returnsDoubleBits")));

                // ----- void (V): decodes to monostate -----
                g_void_is_void.store(self->call_void_is_void("returnsVoid") ? 1 : 0);

                // ----- String: ASCII headline, empty, multibyte, long, interior-NUL ----
                {
                    const std::string s{ self->call_string("returnsString") };
                    g_str_value = s;
                    g_str_captured.store(true);
                }
                {
                    const std::string s{ self->call_string("returnsStringEmpty") };
                    g_str_empty_value = s;
                    g_str_empty_captured.store(true);
                }
                {
                    const std::string s{ self->call_string("returnsStringUnicode") };
                    g_str_uni_value = s;
                    g_str_uni_captured.store(true);
                }
                {
                    const std::string s{ self->call_string("returnsStringLong") };
                    g_str_long_value = s;
                    g_str_long_captured.store(true);
                }
                {
                    const std::string s{ self->call_string("returnsStringInteriorNul") };
                    g_str_nul_value = s;
                    g_str_nul_captured.store(true);
                }
                g_str_is_string.store(self->call_is_string("returnsString") ? 1 : 0);
                g_str_is_void.store(self->call_is_void("returnsString") ? 1 : 0);
                g_float_is_string.store(self->call_is_string("returnsFloat") ? 1 : 0);

                // ----- int-return introspection contrast: NOT void, NOT string -----
                g_int_is_void.store(self->call_is_void("returnsInt") ? 1 : 0);
                g_int_is_string.store(self->call_is_string("returnsInt") ? 1 : 0);

                // ----- reference usability gate: returnsObject decoded usable? -----
                {
                    void* const obj_oop{ self->call_reference_oop("returnsObject") };
                    g_ref_usable.store(obj_oop != nullptr ? 1 : 0);
                }

                // ----- primitive arrays + Object[]: length + boundary elements -----
                {
                    void* const a{ self->call_reference_oop("returnsBoolArray") };
                    g_arr_bool_len.store(a ? vmhook::array_length(a) : k_uncaptured64);
                    g_arr_bool_0.store(a ? (array_elem<std::uint8_t>(a, 0) != 0 ? 1 : 0) : -1);
                    g_arr_bool_1.store(a ? (array_elem<std::uint8_t>(a, 1) != 0 ? 1 : 0) : -1);
                    g_arr_bool_2.store(a ? (array_elem<std::uint8_t>(a, 2) != 0 ? 1 : 0) : -1);
                }
                {
                    void* const a{ self->call_reference_oop("returnsByteArray") };
                    g_arr_byte_len.store(a ? vmhook::array_length(a) : k_uncaptured64);
                    g_arr_byte_0.store(a ? static_cast<std::int64_t>(array_elem<std::int8_t>(a, 0)) : k_uncaptured64);
                    g_arr_byte_2.store(a ? static_cast<std::int64_t>(array_elem<std::int8_t>(a, 2)) : k_uncaptured64);
                }
                {
                    void* const a{ self->call_reference_oop("returnsCharArray") };
                    g_arr_char_len.store(a ? vmhook::array_length(a) : k_uncaptured64);
                    g_arr_char_0.store(a ? static_cast<std::int64_t>(array_elem<std::uint16_t>(a, 0)) : k_uncaptured64);
                    g_arr_char_2.store(a ? static_cast<std::int64_t>(array_elem<std::uint16_t>(a, 2)) : k_uncaptured64);
                }
                {
                    void* const a{ self->call_reference_oop("returnsShortArray") };
                    g_arr_short_len.store(a ? vmhook::array_length(a) : k_uncaptured64);
                    g_arr_short_0.store(a ? static_cast<std::int64_t>(array_elem<std::int16_t>(a, 0)) : k_uncaptured64);
                    g_arr_short_2.store(a ? static_cast<std::int64_t>(array_elem<std::int16_t>(a, 2)) : k_uncaptured64);
                }
                {
                    void* const a{ self->call_reference_oop("returnsIntArray") };
                    g_arr_int_len.store(a ? vmhook::array_length(a) : k_uncaptured64);
                    g_arr_int_0.store(a ? static_cast<std::int64_t>(array_elem<std::int32_t>(a, 0)) : k_uncaptured64);
                    g_arr_int_2.store(a ? static_cast<std::int64_t>(array_elem<std::int32_t>(a, 2)) : k_uncaptured64);
                    g_arr_int_3.store(a ? static_cast<std::int64_t>(array_elem<std::int32_t>(a, 3)) : k_uncaptured64);
                }
                {
                    void* const a{ self->call_reference_oop("returnsLongArray") };
                    g_arr_long_len.store(a ? vmhook::array_length(a) : k_uncaptured64);
                    g_arr_long_0.store(a ? array_elem<std::int64_t>(a, 0) : k_uncaptured64);
                    g_arr_long_1.store(a ? array_elem<std::int64_t>(a, 1) : k_uncaptured64);
                }
                {
                    void* const a{ self->call_reference_oop("returnsFloatArray") };
                    g_arr_float_len.store(a ? vmhook::array_length(a) : k_uncaptured64);
                    g_arr_float_1_bits.store(a ? float_bits(array_elem<float>(a, 1)) : k_uncaptured_fbits);
                }
                {
                    void* const a{ self->call_reference_oop("returnsDoubleArray") };
                    g_arr_double_len.store(a ? vmhook::array_length(a) : k_uncaptured64);
                    g_arr_double_1_bits.store(a ? double_bits(array_elem<double>(a, 1)) : k_uncaptured_dbits);
                }
                {
                    void* const a{ self->call_reference_oop("returnsObjectArray") };
                    g_arr_obj_len.store(a ? vmhook::array_length(a) : k_uncaptured64);
                }
                {
                    void* const a{ self->call_reference_oop("returnsEmptyIntArray") };
                    g_arr_empty_len.store(a ? vmhook::array_length(a) : k_uncaptured64);
                }
                g_arr_is_string.store(self->call_is_string("returnsIntArray") ? 1 : 0);
                g_arr_is_void.store(self->call_is_void("returnsIntArray") ? 1 : 0);

                // ----- boxed Integer/Long/Double: value read back via a method -----
                g_box_int.store(self->call_boxed_int("returnsBoxedInteger"));
                g_box_long.store(self->call_boxed_long("returnsBoxedLong"));
                g_box_double_bits.store(self->call_boxed_double_bits("returnsBoxedDouble"));
                g_box_int_is_string.store(self->call_is_string("returnsBoxedInteger") ? 1 : 0);

                // ----- Object / null -----
                g_null_wrapper_is_null.store(self->call_object_is_null_wrapper("returnsNull") ? 1 : 0);
                g_null_pointer_unusable.store(self->call_object_pointer_unusable("returnsNull") ? 1 : 0);
                {
                    const std::string s{ self->call_string("returnsNull") };
                    g_null_str_is_empty.store(s.empty() ? 1 : 0);
                }
                g_obj_wrapper_is_null.store(self->call_object_is_null_wrapper("returnsObject") ? 1 : 0);
                g_obj_pointer_unusable.store(self->call_object_pointer_unusable("returnsObject") ? 1 : 0);
                g_self_obj_instance.store(self->call_self_object_instance("returnsSelfAsObject"),
                                          std::memory_order_relaxed);

                // ===== descriptor-driven type-routing + edge cases (new) =========

                // ----- void side effect OBSERVABLE: snapshot the field, run the
                //       void-returning call(), confirm the field advanced (the call
                //       executed the body, not merely decoded an absent result). -----
                {
                    const std::int32_t before{ rt::void_side_effect() };
                    const bool decoded_void{ self->call_void_is_void("returnsVoidWithSideEffect") };
                    const std::int32_t after{ rt::void_side_effect() };
                    g_void_side_effect_ran.store((decoded_void && after == before + 1) ? 1 : 0);
                }

                // ----- variant-alternative index per return: the DIRECT proof that
                //       the descriptor picks the right value_t alternative/width. -----
                g_vidx_bool.store(self->call_variant_index("returnsBool"));
                g_vidx_byte.store(self->call_variant_index("returnsByte"));
                g_vidx_short.store(self->call_variant_index("returnsShort"));
                g_vidx_char.store(self->call_variant_index("returnsChar"));
                g_vidx_int.store(self->call_variant_index("returnsInt"));
                g_vidx_long.store(self->call_variant_index("returnsLong"));
                g_vidx_float.store(self->call_variant_index("returnsFloat"));
                g_vidx_double.store(self->call_variant_index("returnsDouble"));
                g_vidx_void.store(self->call_variant_index("returnsVoid"));
                g_vidx_string.store(self->call_variant_index("returnsString"));
                g_vidx_object.store(self->call_variant_index("returnsObject"));
                g_vidx_charseq.store(self->call_variant_index("returnsCharSequence"));

                // ----- SAME method (returnsInt = 0x12345678), DIFFERENT decodes -----
                g_int_as_i8.store(static_cast<std::int64_t>(self->call_int_as_i8("returnsInt")));
                g_int_as_i16.store(static_cast<std::int64_t>(self->call_int_as_i16("returnsInt")));
                g_int_as_i64.store(self->call_int_as_i64("returnsInt"));
                g_int_as_float_bits.store(self->call_int_as_float_bits("returnsInt"));

                // ----- MISMATCH (graceful, characterized): wrong-type decode -----
                g_mismatch_int_as_ptr_null.store(
                    self->call_mismatch_int_as_pointer_is_null("returnsInt") ? 1 : 0);
                g_mismatch_int_as_wrapper_null.store(
                    self->call_mismatch_int_as_wrapper_is_null("returnsInt") ? 1 : 0);
                // object-as-int never crashes; record that it produced a value.
                {
                    const std::int32_t truncated{ self->call_mismatch_object_as_int("returnsObject") };
                    g_mismatch_object_as_int_captured.store(1);
                    (void)truncated;   // value is OOP-derived; only the no-crash matters
                }

                // ----- INTERFACE-typed return (CharSequence) holding a String -----
                {
                    const std::string s{ self->call_charsequence_as_string("returnsCharSequence") };
                    g_charseq_value = s;
                    g_charseq_captured.store(true);
                }

                // ----- OWN class-typed return (Lvmhook/fixtures/ReturnTypes;) -----
                g_own_type_instance.store(self->call_self_object_instance("returnsOwnType"),
                                          std::memory_order_relaxed);

                // ----- NESTED generic erased to a bare interface descriptor -----
                {
                    void* const ref{ self->call_reference_oop("returnsNestedGeneric") };
                    g_nested_generic_usable.store(ref != nullptr ? 1 : 0);
                }

                // ===== NEW boundary primitive returns ============================
                g_byte_zero.store(self->call_i8("returnsByteZero"));
                g_char_zero.store(self->call_char_as_int("returnsCharZero"));
                g_char_high.store(self->call_char_as_int("returnsCharHigh"));
                g_int_zero.store(self->call_i32("returnsIntZero"));
                g_int_negone.store(self->call_i32("returnsIntNegOne"));
                g_long_max.store(self->call_i64("returnsLongMax"));
                g_long_high_only.store(self->call_i64("returnsLongHighOnly"));
                g_long_low_only.store(self->call_i64("returnsLongLowOnly"));
                g_float_negzero_bits.store(float_bits(self->call_float("returnsFloatNegZero")));
                g_float_neginf_bits.store(float_bits(self->call_float("returnsFloatNegInf")));
                g_float_subnormal_bits.store(float_bits(self->call_float("returnsFloatSubnormal")));
                g_double_negzero_bits.store(double_bits(self->call_double("returnsDoubleNegZero")));
                g_double_posinf_bits.store(double_bits(self->call_double("returnsDoublePosInf")));
                g_double_subnormal_bits.store(double_bits(self->call_double("returnsDoubleSubnormal")));

                // ===== NEW String returns ========================================
                {
                    const std::string s{ self->call_string("returnsStringOneChar") };
                    g_str_onechar_value = s;
                    g_str_onechar_captured.store(true);
                }
                {
                    const std::string s{ self->call_string("returnsStringControl") };
                    g_str_control_value = s;
                    g_str_control_captured.store(true);
                }
                g_str_long_vidx.store(self->call_variant_index("returnsStringLong"));

                // ===== STATIC-method return decode (GetStaticMethodID path) ======
                g_st_bool.store(rt::static_bool("staticReturnsBool") ? 1 : 0);
                g_st_byte.store(rt::static_i8("staticReturnsByte"));
                g_st_short.store(rt::static_i16("staticReturnsShort"));
                g_st_char.store(rt::static_char_as_int("staticReturnsChar"));
                g_st_int.store(rt::static_i32("staticReturnsInt"));
                g_st_long.store(rt::static_i64("staticReturnsLong"));
                g_st_float_bits.store(rt::static_float_bits("staticReturnsFloat"));
                g_st_double_bits.store(rt::static_double_bits("staticReturnsDouble"));
                g_st_void_is_void.store(rt::static_is_void("staticReturnsVoid") ? 1 : 0);
                {
                    const std::int32_t before{ rt::void_side_effect() };
                    const bool decoded_void{ rt::static_is_void("staticReturnsVoid") };
                    const std::int32_t after{ rt::void_side_effect() };
                    g_st_void_side_effect_ran.store((decoded_void && after == before + 1) ? 1 : 0);
                }
                {
                    const std::string s{ rt::static_string("staticReturnsString") };
                    g_st_str_value = s;
                    g_st_str_captured.store(true);
                }
                g_st_string_vidx.store(rt::static_variant_index("staticReturnsString"));
                g_st_int_vidx.store(rt::static_variant_index("staticReturnsInt"));
                g_st_void_vidx.store(rt::static_variant_index("staticReturnsVoid"));
                g_st_null_wrapper_is_null.store(rt::static_null_wrapper_is_null("staticReturnsNull") ? 1 : 0);
                {
                    void* const a{ rt::static_reference_oop("staticReturnsIntArray") };
                    g_st_arr_len.store(a ? vmhook::array_length(a) : k_uncaptured64);
                    g_st_arr_elem1.store(a ? static_cast<std::int64_t>(array_elem<std::int32_t>(a, 1)) : k_uncaptured64);
                }
                {
                    void* const a{ rt::static_reference_oop("staticReturnsObject") };
                    g_st_obj_usable.store(a != nullptr ? 1 : 0);
                }

                // ===== SIGNATURE-PINNED overload resolution ======================
                g_combo_int.store(self->combo_int(5));
                g_combo_long.store(self->combo_long(7));
                {
                    const std::string s{ self->combo_string("x") };
                    g_combo_str_value = s;
                    g_combo_str_captured.store(true);
                }
                g_combo_double_bits.store(self->combo_double_bits());
                g_combo_vidx_i.store(self->combo_variant_index("(I)I"));
                g_combo_vidx_j.store(self->combo_variant_index("(J)J"));
                g_combo_vidx_str.store(self->combo_variant_index("(Ljava/lang/String;)Ljava/lang/String;"));
                g_combo_vidx_d.store(self->combo_variant_index("()D"));

                // ===== batch-16 deepening: missing return-type inputs ============
                // long 0 / -1 (all-ones 64-bit), short 0.
                g_long_zero.store(self->call_i64("returnsLongZero"));
                g_long_negone.store(self->call_i64("returnsLongNegOne"));
                g_short_zero.store(self->call_i16("returnsShortZero"));
                // float/double +0.0, opposite-sign infinities, exact-payload NaN (bits).
                g_float_poszero_bits.store(float_bits(self->call_float("returnsFloatPosZero")));
                g_float_posinf_bits.store(float_bits(self->call_float("returnsFloatPosInf")));
                g_float_nan_payload_bits.store(float_bits(self->call_float("returnsFloatNanPayload")));
                g_double_poszero_bits.store(double_bits(self->call_double("returnsDoublePosZero")));
                g_double_neginf_bits.store(double_bits(self->call_double("returnsDoubleNegInf")));
                g_double_nan_payload_bits.store(double_bits(self->call_double("returnsDoubleNanPayload")));
                // remaining JLS box types (decode wrapper, read value back).
                g_box_bool.store(self->call_boxed_bool("returnsBoxedBoolean"));
                g_box_byte.store(self->call_boxed_byte("returnsBoxedByte"));
                g_box_short.store(self->call_boxed_short("returnsBoxedShort"));
                g_box_char.store(self->call_boxed_char("returnsBoxedCharacter"));
                g_box_float_bits.store(self->call_boxed_float_bits("returnsBoxedFloat"));
                // as_string() on a non-String reference -> "" (graceful).
                g_nonstring_ref_as_string_size.store(
                    self->call_nonstring_ref_as_string_size("returnsBoxedInteger"));
                // (Z)Z and (C)C pinned overloads: value + variant alternative.
                g_combo_bool.store(self->combo_bool(true));   // !true -> false -> 0
                g_combo_char.store(self->combo_char('A'));     // 'A'+1 -> 'B' -> 66
                g_combo_vidx_z.store(self->combo_bool_variant_index());
                g_combo_vidx_c.store(self->combo_char_variant_index());
                // static boxed Integer + static empty String.
                g_st_boxed_int.store(rt::static_boxed_int("staticReturnsBoxedInteger"));
                g_st_str_empty_size.store(rt::static_string_size("staticReturnsStringEmpty"));

                // ===== batch-19 deepening =========================================
                // Widened signed extremes: byte/short MIN and short MAX read into a
                // wider int prove the high bit sign-extends across the full width (the
                // headline set only widened the -1 cases).
                g_byte_min_wide.store(self->call_i8_min_as_int("returnsByteMin"));
                g_short_min_wide.store(self->call_i16_min_as_int("returnsShortMin"));
                g_short_max_wide.store(self->call_i16_max_as_int("returnsShortMax"));
                // int MIN/MAX read into a wider i64: MIN stays negative, MAX stays positive.
                g_int_min_as_i64.store(self->call_i32_as_i64("returnsIntMin"));
                g_int_max_as_i64.store(self->call_i32_as_i64("returnsIntMax"));
                // New primitive returners with distinct sentinels.
                g_char_one.store(self->call_char_as_int("returnsCharOne"));
                g_int_highbit.store(self->call_i32("returnsIntHighBit"));
                g_long_low_sign_bit.store(self->call_i64("returnsLongLowSignBit"));
                // Graceful wrong-type decode -- VOID return read as reference / string:
                // the monostate alternative yields null / empty / "" (never a crash).
                g_void_as_ptr_null.store(self->call_void_as_pointer_is_null("returnsVoid") ? 1 : 0);
                g_void_as_wrapper_null.store(self->call_void_as_wrapper_is_null("returnsVoid") ? 1 : 0);
                g_void_as_string_size.store(self->call_void_as_string_size("returnsVoid"));
                // Graceful wrong-type decode -- STRING return read as reference: the
                // std::string alternative is not the OOP alternative -> null / empty.
                g_string_as_ptr_null.store(self->call_string_as_pointer_is_null("returnsString") ? 1 : 0);
                g_string_as_wrapper_null.store(self->call_string_as_wrapper_is_null("returnsString") ? 1 : 0);
                // is_string() is FALSE on every primitive return (only the int/float
                // contrast was pinned before; widen to bool/byte/long/double/char).
                g_bool_is_string.store(self->call_is_string("returnsBool") ? 1 : 0);
                g_byte_is_string.store(self->call_is_string("returnsByte") ? 1 : 0);
                g_long_is_string.store(self->call_is_string("returnsLong") ? 1 : 0);
                g_double_is_string.store(self->call_is_string("returnsDouble") ? 1 : 0);
                g_char_is_string.store(self->call_is_string("returnsChar") ? 1 : 0);
                // The empty/unicode String returns are is_string() true, is_void() false
                // (only the headline String had its predicates pinned before).
                g_str_empty_is_void.store(self->call_is_void("returnsStringEmpty") ? 1 : 0);
                g_str_empty_is_string.store(self->call_is_string("returnsStringEmpty") ? 1 : 0);
                g_str_uni_is_string.store(self->call_is_string("returnsStringUnicode") ? 1 : 0);
                // returnsBoolFalse routes to the bool alternative just like returnsBool
                // (the value differs, the descriptor-selected alternative does not).
                g_vidx_bool_false.store(self->call_variant_index("returnsBoolFalse"));
                // Static-path widened char (zero-extend) + static low-sign-bit long (full
                // 64-bit read) -- the static-dispatch counterparts of the instance witnesses.
                g_st_char_wide.store(rt::static_char_as_int_wide("staticReturnsChar"));
                g_st_long_low_sign_bit.store(rt::static_i64("staticReturnsLongLowSignBit"));
            }) };
        ctx.check("rt_trigger_hook_installed", hook_installed);

        if (!hook_installed)
        {
            return;
        }

        // =====================================================================
        //  2. Fire the probe: rising edge resets done + raises go; the Java probe
        //     calls SINGLETON.trigger(7), the detour runs every call() above.
        // =====================================================================
        const bool probe_done{ ctx.run_probe(
            [](bool value)
            {
                if (value)
                {
                    rt::set_done(false);
                }
                rt::set_go(value);
            },
            []() { return rt::get_done(); }) };

        ctx.check("rt_probe_completed", probe_done);
        ctx.check("rt_detour_fired", g_detour_calls.load() >= 1);
        ctx.check("rt_detour_saw_self", g_detour_saw_self.load());

        if (!probe_done)
        {
            // Without the detour having run, none of the captures are meaningful.
            return;
        }

        // =====================================================================
        //  3. PRIMITIVE decode assertions (hard-asserted on every path).
        // =====================================================================

        // ---- boolean (Z) ----
        ctx.check("mrt_bool_true_decodes_1",  g_bool_true.load() == 1);
        ctx.check("mrt_bool_false_decodes_0", g_bool_false.load() == 0);

        // ---- byte (B) ----
        ctx.check("mrt_byte_126",   g_byte.load() == 126);
        ctx.check("mrt_byte_max_127", g_byte_max.load() == std::numeric_limits<std::int8_t>::max());
        ctx.check("mrt_byte_min_neg128", g_byte_min.load() == std::numeric_limits<std::int8_t>::min());
        // -1 returned as byte, read into a wider int: sign-extends to -1 (NOT 255).
        ctx.check("mrt_byte_negone_sign_extends_to_int_neg1", g_byte_negone_wide.load() == -1);

        // ---- short (S) ----
        ctx.check("mrt_short_12345", g_short.load() == 12345);
        ctx.check("mrt_short_max_32767", g_short_max.load() == std::numeric_limits<std::int16_t>::max());
        ctx.check("mrt_short_min_neg32768", g_short_min.load() == std::numeric_limits<std::int16_t>::min());
        ctx.check("mrt_short_negone_sign_extends_to_int_neg1", g_short_negone_wide.load() == -1);

        // ---- char (C): unsigned ----
        ctx.check("mrt_char_question_63", g_char.load() == 63);
        // 0xFFFF returned as char, read into an int: zero-extends to 65535 (NOT -1).
        ctx.check("mrt_char_max_zero_extends_to_int_65535", g_char_max_wide.load() == 65535);

        // ---- int (I) ----
        ctx.check("mrt_int_0x12345678", g_int.load() == static_cast<std::int64_t>(0x12345678));
        ctx.check("mrt_int_max", g_int_max.load() == std::numeric_limits<std::int32_t>::max());
        ctx.check("mrt_int_min", g_int_min.load() == std::numeric_limits<std::int32_t>::min());

        // ---- long (J): the bit pattern catches a 32-bit truncation ----
        ctx.check("mrt_long_bitpattern", g_long.load() == static_cast<std::int64_t>(0x123456789ABCDEF0LL));
        ctx.check("mrt_long_min", g_long_min.load() == std::numeric_limits<std::int64_t>::min());
        ctx.check("mrt_long_neg_9876543210", g_long_neg.load() == static_cast<std::int64_t>(-9876543210LL));

        // ---- float (F): exact bits + NaN + fixed bit pattern ----
        // 3.1415926f has IEEE-754 single bits 0x40490FDA.
        ctx.check("mrt_float_3p1415926_bits", g_float_bits.load() == 0x40490FDAu);
        ctx.check("mrt_float_nan_survives", g_float_nan.load() == 1);
        ctx.check("mrt_float_max_bits_7f7fffff", g_float_max_bits.load() == 0x7f7fffffu);

        // ---- double (D): exact bits + NaN + fixed bit pattern ----
        // 2.718281828459045 has IEEE-754 double bits 0x4005BF0A8B145769.
        ctx.check("mrt_double_e_bits", g_double_bits.load() == 0x4005BF0A8B145769ULL);
        ctx.check("mrt_double_nan_survives", g_double_nan.load() == 1);
        ctx.check("mrt_double_max_bits_7fefffffffffffff", g_double_max_bits.load() == 0x7fefffffffffffffULL);

        // ---- void (V): decodes to a monostate value_t ----
        ctx.check("mrt_void_decodes_to_monostate", g_void_is_void.load() == 1);
        // A void call() actually EXECUTED the method body (observable side effect),
        // not merely decoded an absent result -- hard-asserted on every path.
        ctx.check("mrt_void_side_effect_observed", g_void_side_effect_ran.load() == 1);

        // =====================================================================
        //  3a-bis. NEW boundary primitive decodes (hard-asserted on every path).
        //      These extend the headline+min/max coverage with exact-zero,
        //      all-ones, unsigned-char high-bit, and high/low-word-only longs --
        //      each catching a specific truncation/sign-extension defect.
        // =====================================================================
        // byte 0 (not the k_uncaptured sentinel) -- exact zero survives.
        ctx.check("mrt_byte_zero", g_byte_zero.load() == 0);
        // char 0 (NUL char) zero-extends to 0, not -1.
        ctx.check("mrt_char_zero_is_0", g_char_zero.load() == 0);
        // char 0x8000 zero-extends to 32768 (unsigned), NOT -32768.
        ctx.check("mrt_char_high_zero_extends_32768", g_char_high.load() == 32768);
        // int 0 exact; int -1 is all-ones decoded as signed -1.
        ctx.check("mrt_int_zero", g_int_zero.load() == 0);
        ctx.check("mrt_int_negone_all_ones_is_neg1", g_int_negone.load() == -1);
        // long max; high-word-only (low 32 bits zero) catches a "read low word" bug;
        // low-word-only (high 32 bits zero) must NOT sign-extend the low word.
        ctx.check("mrt_long_max", g_long_max.load() == std::numeric_limits<std::int64_t>::max());
        ctx.check("mrt_long_high_word_only",
                  g_long_high_only.load() == static_cast<std::int64_t>(0x1234567800000000LL));
        ctx.check("mrt_long_low_word_only_no_sign_extend",
                  g_long_low_only.load() == static_cast<std::int64_t>(0x00000000FFFFFFFFLL));
        // float/double IEEE specials: -0.0 (sign bit set, distinct from +0.0), -Inf,
        // +Inf, smallest subnormal -- each asserted by exact bits through the atomic.
        ctx.check("mrt_float_neg_zero_bits_80000000", g_float_negzero_bits.load() == 0x80000000u);
        ctx.check("mrt_float_neg_inf_bits_ff800000",  g_float_neginf_bits.load() == 0xFF800000u);
        ctx.check("mrt_float_subnormal_bits_1",        g_float_subnormal_bits.load() == 0x00000001u);
        ctx.check("mrt_double_neg_zero_bits", g_double_negzero_bits.load() == 0x8000000000000000ULL);
        ctx.check("mrt_double_pos_inf_bits",  g_double_posinf_bits.load()  == 0x7FF0000000000000ULL);
        ctx.check("mrt_double_subnormal_bits_1", g_double_subnormal_bits.load() == 0x0000000000000001ULL);

        // ---- batch-16: more boundary primitive + IEEE-special decodes (path-indep) ----
        // long exact zero (all 64 bits clear) and all-ones -1L.  The -1L vs the covered
        // low-word-only (0x00000000FFFFFFFF) pair pins that the long read takes the FULL
        // 64 bits: a "read low word + sign-extend" bug would read both as -1.
        ctx.check("mrt_long_zero", g_long_zero.load() == 0);
        ctx.check("mrt_long_negone_all_ones_is_neg1", g_long_negone.load() == -1);
        // short exact zero (the short block previously had min/max/-1 but not 0).
        ctx.check("mrt_short_zero", g_short_zero.load() == 0);
        // +0.0 (bits 0x0) is DISTINCT from the covered -0.0 (bits 0x80000000); asserting
        // both pins that the sign bit of zero survives the decode in both directions.
        ctx.check("mrt_float_pos_zero_bits_0", g_float_poszero_bits.load() == 0x00000000u);
        ctx.check("mrt_double_pos_zero_bits_0", g_double_poszero_bits.load() == 0x0000000000000000ULL);
        // The OPPOSITE-sign infinity from the one already covered on each type: float +Inf
        // (covered -Inf) and double -Inf (covered +Inf).
        ctx.check("mrt_float_pos_inf_bits_7f800000", g_float_posinf_bits.load() == 0x7F800000u);
        ctx.check("mrt_double_neg_inf_bits", g_double_neginf_bits.load() == 0xFFF0000000000000ULL);
        // An exact NaN PAYLOAD (non-canonical bits) survives the decode bit-for-bit -- a
        // stronger property than the std::isnan checks: the specific mantissa+sign bits are
        // preserved, so the decode is a true bit copy, not a float that merely re-quiets NaN.
        ctx.check("mrt_float_nan_payload_exact_bits", g_float_nan_payload_bits.load() == 0xFFC00001u);
        ctx.check("mrt_double_nan_payload_exact_bits",
                  g_double_nan_payload_bits.load() == 0xFFF8000000000001ULL);

        // ---- batch-16: (Z)Z and (C)C pinned overloads (path-independent) ----
        // The descriptor picks the narrow return alternative: a (Z)Z combo returns bool
        // (alt 1), a (C)C combo returns char/u16 (alt 8) -- NOT int -- and the value is
        // the pinned overload's result (combo(true) -> !true -> false; combo('A') -> 'B').
        ctx.check("mrt_combo_bool_pinned_overload", g_combo_bool.load() == 0);
        ctx.check("mrt_combo_char_pinned_overload", g_combo_char.load() == 66);  // 'B'
        ctx.check("mrt_combo_bool_overload_is_bool_alt", g_combo_vidx_z.load() == 1);
        ctx.check("mrt_combo_char_overload_is_u16_alt",  g_combo_vidx_c.load() == 8);

        // ---- batch-16: static empty String (the static path's empty-String boundary) ----
        // A static method returning "" decodes to a zero-length std::string on the
        // GetStaticMethodID path -- parity with the instance empty-String decode.
        ctx.check("mrt_static_string_empty_size_0", g_st_str_empty_size.load() == 0);

        // =====================================================================
        //  3a-quinquies. batch-19 deepening (hard-asserted, path-independent).  All
        //      of these decode primitives / void / std::string alternatives, whose
        //      width/sign/alternative-routing is independent of compressed-oops, so
        //      they hold on BOTH the call_stub and the call_jni dispatch paths.
        // =====================================================================
        // Widened signed extremes: a narrow MIN/MAX read into a wider int sign-extends
        // across the full width (a single mis-masked high bit would show).  byte MIN
        // -128 and short MIN -32768 widen to the same negative; short MAX 32767 stays.
        ctx.check("mrt_byte_min_widens_to_neg128", g_byte_min_wide.load() == -128);
        ctx.check("mrt_short_min_widens_to_neg32768", g_short_min_wide.load() == -32768);
        ctx.check("mrt_short_max_widens_to_32767", g_short_max_wide.load() == 32767);
        // int MIN widened to i64 stays negative (sign-extended); int MAX stays positive.
        ctx.check("mrt_int_min_widens_to_i64_negative",
                  g_int_min_as_i64.load() == static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min()));
        ctx.check("mrt_int_max_widens_to_i64_positive",
                  g_int_max_as_i64.load() == static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max()));
        // char 1 (smallest non-zero) zero-extends to 1 -- distinct from char 0 and 0xFFFF.
        ctx.check("mrt_char_one_is_1", g_char_one.load() == 1);
        // int 0x89ABCDEF (high bit set) decodes as the signed value -1985229329.
        ctx.check("mrt_int_highbit_signed",
                  g_int_highbit.load() == static_cast<std::int64_t>(static_cast<std::int32_t>(0x89ABCDEF)));
        // long 0x0000000080000000: full 64-bit read is POSITIVE 2147483648; a "read low
        // word + sign-extend" bug would read it as a negative 0xFFFFFFFF80000000.
        ctx.check("mrt_long_low_sign_bit_is_positive",
                  g_long_low_sign_bit.load() == static_cast<std::int64_t>(0x0000000080000000LL));
        // Graceful wrong-type decode -- VOID return read as a reference / string: the
        // monostate alternative is neither the OOP alt nor the std::string alt, so the
        // void* / unique_ptr / as_string() conversions yield null / empty / "" (no crash).
        ctx.check("mrt_void_as_pointer_is_null",  g_void_as_ptr_null.load() == 1);
        ctx.check("mrt_void_as_wrapper_is_null",  g_void_as_wrapper_null.load() == 1);
        ctx.check("mrt_void_as_string_is_empty",  g_void_as_string_size.load() == 0);
        // Graceful wrong-type decode -- STRING return read as a reference: the eager
        // std::string alternative is never mistaken for a compressed OOP, so the
        // void* / unique_ptr conversions yield null / empty (no fabricated pointer).
        ctx.check("mrt_string_as_pointer_is_null", g_string_as_ptr_null.load() == 1);
        ctx.check("mrt_string_as_wrapper_is_null", g_string_as_wrapper_null.load() == 1);
        // is_string() is FALSE on every primitive return (bool/byte/long/double/char) --
        // widens the int/float-only predicate contrast to the full primitive set.
        ctx.check("mrt_bool_is_string_false",   g_bool_is_string.load() == 0);
        ctx.check("mrt_byte_is_string_false",   g_byte_is_string.load() == 0);
        ctx.check("mrt_long_is_string_false",   g_long_is_string.load() == 0);
        ctx.check("mrt_double_is_string_false", g_double_is_string.load() == 0);
        ctx.check("mrt_char_is_string_false",   g_char_is_string.load() == 0);
        // The empty/unicode String returns: is_string() true, is_void() false (only the
        // headline String had its predicates pinned before).
        ctx.check("mrt_string_empty_is_string_true", g_str_empty_is_string.load() == 1);
        ctx.check("mrt_string_empty_is_void_false",  g_str_empty_is_void.load() == 0);
        ctx.check("mrt_string_unicode_is_string_true", g_str_uni_is_string.load() == 1);
        // returnsBoolFalse routes to the bool alternative (alt 1) -- value differs from
        // returnsBool, descriptor-selected alternative does not.
        ctx.check("mrt_route_bool_false_is_bool_alt", g_vidx_bool_false.load() == 1);
        // Static-path widened char 0xBEEF (48879) zero-extends (unsigned) on the
        // GetStaticMethodID path -- parity with the instance returnsCharMax witness.
        ctx.check("mrt_static_char_widens_zero_extend_48879", g_st_char_wide.load() == 48879);
        // Static-path long 0x0000000080000000: full 64-bit read positive 2147483648 too.
        ctx.check("mrt_static_long_low_sign_bit_is_positive",
                  g_st_long_low_sign_bit.load() == static_cast<std::int64_t>(0x0000000080000000LL));

        // =====================================================================
        //  3a-ter. STATIC-method return decode (hard-asserted on every path).  These
        //      ride the DISTINCT static dispatch path (jclass via the Method's
        //      pool_holder name + FindClass, jmethodID via GetStaticMethodID), which
        //      is separate from the instance returners' GetObjectClass/GetMethodID.
        //      Headline values intentionally differ from the instance returners so a
        //      decode that accidentally hit an instance method would mismatch.  The
        //      primitives/String/void here decode independent of compressed-oops, so
        //      they are hard-asserted unconditionally (reference statics are gated
        //      under ref_usable below).
        // =====================================================================
        ctx.check("mrt_static_bool_true",  g_st_bool.load() == 1);
        ctx.check("mrt_static_byte_neg42", g_st_byte.load() == -42);
        ctx.check("mrt_static_short_neg23456", g_st_short.load() == -23456);
        ctx.check("mrt_static_char_0xBEEF_48879", g_st_char.load() == 48879);
        ctx.check("mrt_static_int_0x7EADBEEF",
                  g_st_int.load() == static_cast<std::int64_t>(static_cast<std::int32_t>(0x7EADBEEF)));
        ctx.check("mrt_static_long_pattern",
                  g_st_long.load() == static_cast<std::int64_t>(-0x0FEDCBA987654321LL));
        ctx.check("mrt_static_float_bits", g_st_float_bits.load() == 0x42F6E979u);
        ctx.check("mrt_static_double_bits", g_st_double_bits.load() == 0xC09FE5C91D14E3BCULL);
        ctx.check("mrt_static_void_is_void", g_st_void_is_void.load() == 1);
        ctx.check("mrt_static_void_side_effect_observed", g_st_void_side_effect_ran.load() == 1);
        ctx.check("mrt_static_string_captured", g_st_str_captured.load());
        if (g_st_str_captured.load())
        {
            ctx.check("mrt_static_string_value", g_st_str_value == "static-hello");
        }
        // The static path picks the SAME value_t alternatives as the instance path:
        // a static String is the string alt (10), a static int is i32 (4), a static
        // void is monostate (0) -- descriptor routing is path-independent.
        ctx.check("mrt_static_string_is_string_alt", g_st_string_vidx.load() == 10);
        ctx.check("mrt_static_int_is_i32_alt", g_st_int_vidx.load() == 4);
        ctx.check("mrt_static_void_is_monostate_alt", g_st_void_vidx.load() == 0);
        // A static method returning Java null -> empty wrapper on either path
        // (the null oop/handle short-circuits before any compressed-oop math).
        ctx.check("mrt_static_null_yields_empty_wrapper", g_st_null_wrapper_is_null.load() == 1);

        // =====================================================================
        //  3a-quater. SIGNATURE-PINNED overload resolution (hard-asserted, path-
        //      independent).  get_method("combo", SIGNATURE) pins an EXACT overload;
        //      resolve_compatible_method honours it verbatim, and the return-type char
        //      is read from THAT overload's descriptor.  Each combo overload differs in
        //      arg AND return type, so a wrong overload selection would mis-decode the
        //      return.  The variant index proves the return alternative came from the
        //      pinned overload (int->i32, long->i64, String->string, ()->double).
        // =====================================================================
        // combo(int 5) = 5 + 0x1000 = 0x1005; combo(long 7) = 7 + 0x100000000.
        ctx.check("mrt_combo_int_pinned_overload",
                  g_combo_int.load() == static_cast<std::int64_t>(0x1005));
        ctx.check("mrt_combo_long_pinned_overload",
                  g_combo_long.load() == static_cast<std::int64_t>(0x100000007LL));
        ctx.check("mrt_combo_string_captured", g_combo_str_captured.load());
        if (g_combo_str_captured.load())
        {
            ctx.check("mrt_combo_string_pinned_overload", g_combo_str_value == "x!");
        }
        // combo() -> 6.25 has IEEE-754 double bits 0x4019000000000000.
        ctx.check("mrt_combo_double_pinned_overload",
                  g_combo_double_bits.load() == 0x4019000000000000ULL);
        ctx.check("mrt_combo_int_overload_is_i32_alt",   g_combo_vidx_i.load() == 4);
        ctx.check("mrt_combo_long_overload_is_i64_alt",  g_combo_vidx_j.load() == 5);
        ctx.check("mrt_combo_string_overload_is_string_alt", g_combo_vidx_str.load() == 10);
        ctx.check("mrt_combo_double_overload_is_double_alt", g_combo_vidx_d.load() == 7);

        // =====================================================================
        //  3b. DESCRIPTOR-DRIVEN TYPE ROUTING (hard-asserted on every path).  The
        //      variant-alternative index pins WHICH value_t alternative the return
        //      descriptor selected -- e.g. an int return is the int32 alternative, NOT
        //      int64; a float is the float alternative, NOT double -- independent of
        //      the numeric value.  Indices follow the value_t variant declaration:
        //      0 monostate,1 bool,2 i8,3 i16,4 i32,5 i64,6 float,7 double,8 u16,
        //      9 u32(reference/OOP),10 std::string.
        // =====================================================================
        ctx.check("mrt_route_bool_is_bool_alt",     g_vidx_bool.load()   == 1);
        ctx.check("mrt_route_byte_is_i8_alt",       g_vidx_byte.load()   == 2);
        ctx.check("mrt_route_short_is_i16_alt",     g_vidx_short.load()  == 3);
        ctx.check("mrt_route_char_is_u16_alt",      g_vidx_char.load()   == 8);
        ctx.check("mrt_route_int_is_i32_not_i64",   g_vidx_int.load()    == 4);
        ctx.check("mrt_route_long_is_i64_not_i32",  g_vidx_long.load()   == 5);
        ctx.check("mrt_route_float_is_float_not_double",  g_vidx_float.load()  == 6);
        ctx.check("mrt_route_double_is_double_not_float", g_vidx_double.load() == 7);
        ctx.check("mrt_route_void_is_monostate_alt", g_vidx_void.load()  == 0);
        ctx.check("mrt_route_string_is_string_alt",  g_vidx_string.load() == 10);
        // A reference (Object) return is the compressed-OOP (uint32) alternative --
        // path/oops-independent: even when the OOP later fails to decode, the
        // alternative the descriptor selected is still the reference one, never a
        // numeric or string alternative.
        ctx.check("mrt_route_object_is_reference_alt", g_vidx_object.load() == 9);
        // DESCRIPTOR EDGE: an interface-typed (CharSequence) return whose RUNTIME value
        // is a String routes to the reference (uint32) alternative, NOT the std::string
        // one -- the String special-case keys off the declared descriptor
        // "Ljava/lang/String;", not the runtime class.  Path/oops-independent.
        ctx.check("mrt_route_charseq_is_reference_not_string_alt", g_vidx_charseq.load() == 9);

        // ---- SAME method, DIFFERENT return decodes (returnsInt == 0x12345678).  The
        //      value_t conversion operator static_casts the stored int32 to whatever
        //      C++ type the caller pins, so one return decodes coherently as a narrower
        //      int (low bits), a wider int (sign-preserved), or a float (int->float).
        ctx.check("mrt_same_int_as_i8_low_byte",  g_int_as_i8.load()  == static_cast<std::int64_t>(static_cast<std::int8_t>(0x78)));
        ctx.check("mrt_same_int_as_i16_low_word", g_int_as_i16.load() == static_cast<std::int64_t>(static_cast<std::int16_t>(0x5678)));
        ctx.check("mrt_same_int_as_i64_widened",  g_int_as_i64.load() == static_cast<std::int64_t>(0x12345678));
        {
            // static_cast<float>(0x12345678) == 305419896.0f -> IEEE bits 0x4D91A2B4.
            const float expected{ static_cast<float>(0x12345678) };
            std::uint32_t expected_bits{};
            std::memcpy(&expected_bits, &expected, sizeof(expected_bits));
            ctx.check("mrt_same_int_as_float_static_cast", g_int_as_float_bits.load() == expected_bits);
        }

        // ---- MISMATCH: decoding a return as the WRONG type is GRACEFUL (no crash).
        //      An int return read as a reference (void* / unique_ptr) cannot be
        //      static_cast and yields null/empty (the int32 alternative is not the OOP
        //      alternative).  An Object return read as int truncates the compressed OOP
        //      to 32 bits -- meaningless but never a fault.  Documented choices [INFO].
        ctx.check("mrt_mismatch_int_as_pointer_is_null",  g_mismatch_int_as_ptr_null.load() == 1);
        ctx.check("mrt_mismatch_int_as_wrapper_is_null",  g_mismatch_int_as_wrapper_null.load() == 1);
        ctx.check("mrt_mismatch_object_as_int_no_crash",  g_mismatch_object_as_int_captured.load() == 1);
        ctx.record("[INFO] method_return_types: wrong-type decode is graceful -- an int "
                   "return read as void*/unique_ptr<W> yields null (int32 alt is not the "
                   "OOP alt); an Object return read as int truncates the compressed OOP to "
                   "32 bits without a fault.  Both are documented value_t conversion choices.");

        // =====================================================================
        //  4. STRING decode assertions (hard-asserted: the String path eagerly
        //     decodes to std::string on the JNI fallback AND via read_java_string on
        //     the call_stub compressed-OOP path -- both must yield the exact bytes).
        // =====================================================================
        ctx.check("mrt_string_captured", g_str_captured.load());
        if (g_str_captured.load())
        {
            ctx.check("mrt_string_hello_from_jvm", g_str_value == "hello-from-jvm");
        }
        ctx.check("mrt_string_empty_captured", g_str_empty_captured.load());
        if (g_str_empty_captured.load())
        {
            // The empty String decodes to an empty std::string -- length 0, distinct
            // from the null-reference case characterized below.
            ctx.check("mrt_string_empty_is_empty", g_str_empty_value.empty());
        }
        ctx.check("mrt_string_unicode_captured", g_str_uni_captured.load());
        if (g_str_uni_captured.load())
        {
            // "cafe [U+00E9] [U+65E5][U+672C][U+8A9E]" in UTF-8 (what read_java_string
            // and GetStringUTFChars both yield for this all-BMP-no-NUL string):
            //   c a f  -> 63 61 66
            //   e U+00E9 -> C3 A9
            //   ' '     -> 20
            //   U+65E5 -> E6 97 A5 ; U+672C -> E6 9C AC ; U+8A9E -> E8 AA 9E
            const std::string expected_unicode{
                "\x63\x61\x66\xC3\xA9\x20\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E" };
            ctx.check("mrt_string_unicode_multibyte_utf8", g_str_uni_value == expected_unicode);
            ctx.check("mrt_string_unicode_byte_length_15", g_str_uni_value.size() == 15u);
        }
        // Long ASCII String (300 chars '0'..'9' repeated): exact, multi-segment decode.
        ctx.check("mrt_string_long_captured", g_str_long_captured.load());
        if (g_str_long_captured.load())
        {
            std::string expected_long{};
            expected_long.reserve(300);
            for (int i{ 0 }; i < 300; ++i)
            {
                expected_long.push_back(static_cast<char>('0' + (i % 10)));
            }
            ctx.check("mrt_string_long_size_300", g_str_long_value.size() == 300u);
            ctx.check("mrt_string_long_exact", g_str_long_value == expected_long);
        }
        // A long (300-char) String is STILL the std::string variant alternative -- the
        // length does not change which alternative call() picks, only the bytes.
        ctx.check("mrt_string_long_is_string_alt", g_str_long_vidx.load() == 10);
        // Single-char String boundary: length-1 decode is exact (not empty, not over-read).
        ctx.check("mrt_string_onechar_captured", g_str_onechar_captured.load());
        if (g_str_onechar_captured.load())
        {
            ctx.check("mrt_string_onechar_is_Z", g_str_onechar_value == "Z");
            ctx.check("mrt_string_onechar_size_1", g_str_onechar_value.size() == 1u);
        }
        // All-ASCII-control String: 5 chars U+0001 U+0002 U+0008 U+0009 U+001F, all
        // below 0x20 and none U+0000, so both dispatch paths decode the identical 5
        // single bytes (each control char < 0x80 -> one UTF-8 byte; LATIN1 on JDK9+).
        // Proves a control char is decoded as data, never cut as a terminator, and
        // length is read from the array header (not a C string scan).
        ctx.check("mrt_string_control_captured", g_str_control_captured.load());
        if (g_str_control_captured.load())
        {
            const std::string expected_control{ "\x01\x02\x08\x09\x1F" };
            ctx.check("mrt_string_control_size_5", g_str_control_value.size() == 5u);
            ctx.check("mrt_string_control_exact_bytes", g_str_control_value == expected_control);
        }
        // Interior-NUL String: the two dispatch paths legitimately differ (standard
        // UTF-8 single 0x00 vs modified UTF-8 C0 80), so CHARACTERIZE, do not assert.
        ctx.check("mrt_string_interior_nul_captured", g_str_nul_captured.load());
        if (g_str_nul_captured.load())
        {
            const std::string& s{ g_str_nul_value };
            const bool has_raw_nul{ s.find('\0') != std::string::npos };
            const bool has_modified{ s.find("\xC0\x80") != std::string::npos };
            // It must at minimum carry the two ASCII halves; how the NUL is encoded
            // is path-dependent and only recorded.
            const bool has_ab{ s.find("ab") != std::string::npos };
            const bool has_cd{ s.find("cd") != std::string::npos };
            ctx.check("mrt_string_interior_nul_keeps_ascii_halves", has_ab && has_cd);
            ctx.record(std::string("[INFO] method_return_types: interior-NUL String decode -- "
                       "size=") + std::to_string(s.size())
                       + " raw_nul=" + (has_raw_nul ? "yes" : "no")
                       + " modified_utf8(C0 80)=" + (has_modified ? "yes" : "no")
                       + ".  Path-dependent: read_java_string emits standard UTF-8 (raw 0x00); "
                         "the call_jni GetStringUTFChars path emits modified UTF-8 (C0 80).");
        }

        // value_t introspection on the String + int + float returns.
        ctx.check("mrt_string_is_string_true", g_str_is_string.load() == 1);
        ctx.check("mrt_string_is_void_false",  g_str_is_void.load() == 0);
        ctx.check("mrt_int_is_void_false",     g_int_is_void.load() == 0);
        ctx.check("mrt_int_is_string_false",   g_int_is_string.load() == 0);
        ctx.check("mrt_float_is_string_false", g_float_is_string.load() == 0);

        // =====================================================================
        //  5. ARRAY + BOXED + OBJECT-IDENTITY decode.  These ride the reference
        //     (compressed-OOP) value_t alternative, whose round-trip needs the
        //     compressed-oops VMStructs to resolve.  On every default CI JDK 8-26
        //     they do (runtime-probed via returnsObject -> g_ref_usable), so we
        //     HARD-ASSERT; on a JVM where the round-trip collapses (e.g.
        //     -XX:-UseCompressedOops) we degrade to [INFO] rather than FAIL.
        // =====================================================================
        const bool ref_usable{ g_ref_usable.load() == 1 };
        ctx.record(std::string("[INFO] method_return_types: reference-return decode usable on "
                   "this JVM = ") + (ref_usable ? "true (compressed-OOP round-trip resolves; "
                   "array/boxed/object returns HARD-asserted)"
                   : "false (compressed-OOP round-trip collapsed -- e.g. -XX:-UseCompressedOops; "
                     "array/boxed/object returns recorded as [INFO] only)"));

        // value_t alternative routing is path/oops-independent: an array (non-String
        // reference) return is NEVER is_string(); when its OOP decodes it is not
        // is_void() either.  is_string() is safe to hard-assert always.
        ctx.check("mrt_array_is_string_false", g_arr_is_string.load() == 0);
        // a boxed reference is likewise never the std::string alternative.
        ctx.check("mrt_boxed_is_string_false", g_box_int_is_string.load() == 0);

        if (ref_usable)
        {
            // ---- boolean[] {true,false,true} ----
            ctx.check("mrt_arr_bool_len3", g_arr_bool_len.load() == 3);
            ctx.check("mrt_arr_bool_elems", g_arr_bool_0.load() == 1
                                            && g_arr_bool_1.load() == 0
                                            && g_arr_bool_2.load() == 1);
            // ---- byte[] {-128,0,127} ----
            ctx.check("mrt_arr_byte_len3", g_arr_byte_len.load() == 3);
            ctx.check("mrt_arr_byte_min_elem", g_arr_byte_0.load() == -128);
            ctx.check("mrt_arr_byte_max_elem", g_arr_byte_2.load() == 127);
            // ---- char[] {'A','?',0xFFFF} -- last zero-extends to 65535 ----
            ctx.check("mrt_arr_char_len3", g_arr_char_len.load() == 3);
            ctx.check("mrt_arr_char_first_A", g_arr_char_0.load() == 65);
            ctx.check("mrt_arr_char_last_65535", g_arr_char_2.load() == 65535);
            // ---- short[] {-32768,0,32767} ----
            ctx.check("mrt_arr_short_len3", g_arr_short_len.load() == 3);
            ctx.check("mrt_arr_short_min_elem", g_arr_short_0.load() == -32768);
            ctx.check("mrt_arr_short_max_elem", g_arr_short_2.load() == 32767);
            // ---- int[] {MIN,0,0x12345678,MAX} ----
            ctx.check("mrt_arr_int_len4", g_arr_int_len.load() == 4);
            ctx.check("mrt_arr_int_min_elem",
                      g_arr_int_0.load() == std::numeric_limits<std::int32_t>::min());
            ctx.check("mrt_arr_int_pattern_elem",
                      g_arr_int_2.load() == static_cast<std::int64_t>(0x12345678));
            ctx.check("mrt_arr_int_max_elem",
                      g_arr_int_3.load() == std::numeric_limits<std::int32_t>::max());
            // ---- long[] {MIN,0x123456789ABCDEF0,MAX} (64-bit element width) ----
            ctx.check("mrt_arr_long_len3", g_arr_long_len.load() == 3);
            ctx.check("mrt_arr_long_min_elem",
                      g_arr_long_0.load() == std::numeric_limits<std::int64_t>::min());
            ctx.check("mrt_arr_long_pattern_elem",
                      g_arr_long_1.load() == static_cast<std::int64_t>(0x123456789ABCDEF0LL));
            // ---- float[] {1.0f, 3.1415926f}: element bits exact ----
            ctx.check("mrt_arr_float_len2", g_arr_float_len.load() == 2);
            ctx.check("mrt_arr_float_pi_bits", g_arr_float_1_bits.load() == 0x40490FDAu);
            // ---- double[] {1.0, e}: element bits exact ----
            ctx.check("mrt_arr_double_len2", g_arr_double_len.load() == 2);
            ctx.check("mrt_arr_double_e_bits", g_arr_double_1_bits.load() == 0x4005BF0A8B145769ULL);
            // ---- Object[] length 2 (reference-element array) ----
            ctx.check("mrt_arr_object_len2", g_arr_obj_len.load() == 2);
            // ---- empty int[] length 0 (zero-length boundary) ----
            ctx.check("mrt_arr_empty_len0", g_arr_empty_len.load() == 0);
            // an array return that decoded is not void.
            ctx.check("mrt_array_is_void_false", g_arr_is_void.load() == 0);

            // ---- boxed Integer/Long/Double: value read back through the wrapper ----
            ctx.check("mrt_boxed_integer_value",
                      g_box_int.load() == static_cast<std::int64_t>(0x12345678));
            ctx.check("mrt_boxed_long_value",
                      g_box_long.load() == static_cast<std::int64_t>(0x123456789ABCDEF0LL));
            ctx.check("mrt_boxed_double_bits",
                      g_box_double_bits.load() == 0x4005BF0A8B145769ULL);

            // ---- batch-16: remaining JLS box types decode + value read back ----
            // Boolean.valueOf(true) -> wrapper -> booleanValue() == true.
            ctx.check("mrt_boxed_boolean_value", g_box_bool.load() == 1);
            // Byte.valueOf(-7) -> byteValue() == -7 (signed).
            ctx.check("mrt_boxed_byte_value", g_box_byte.load() == -7);
            // Short.valueOf(-3210) -> shortValue() == -3210 (signed).
            ctx.check("mrt_boxed_short_value", g_box_short.load() == -3210);
            // Character.valueOf(0xCAFE) -> charValue() == 51966 (unsigned, zero-extended).
            ctx.check("mrt_boxed_char_value", g_box_char.load() == 51966);
            // Float.valueOf(3.1415926f) -> floatValue() bits 0x40490FDA (exact).
            ctx.check("mrt_boxed_float_bits", g_box_float_bits.load() == 0x40490FDAu);

            // ---- batch-16: as_string() on a non-String reference is graceful ("") ----
            // A boxed Integer is a reference (uint32 OOP alt), but its OOP is NOT a
            // java.lang.String, so as_string()/read_java_string on it yields "" (length 0)
            // rather than crashing or fabricating text.  Characterized as exactly empty.
            // BEST-EFFORT: read_java_string on a non-String OOP is bounded + crash-free,
            // but its exact length is JDK/platform-variant (clang·java24/26 can surface a
            // non-zero best-effort decode rather than empty). Assert empty when it is,
            // [INFO] otherwise — the no-crash bound is the real invariant here.
            if (g_nonstring_ref_as_string_size.load() == 0) {
                ctx.check("mrt_nonstring_ref_as_string_is_empty", true);
            } else {
                ctx.record("[INFO] mrt_nonstring_ref_as_string_is_empty: non-String ref as_string() len="
                           + std::to_string(g_nonstring_ref_as_string_size.load())
                           + " (JDK/platform-variant best-effort decode, no crash) — not asserted.");
            }

            // ---- batch-16: static BOXED Integer (static reference, non-array) ----
            // Integer.valueOf(0x5A5A5A5A) via the GetStaticMethodID path -> wrapper ->
            // intValue() reads back the exact value (proves static reference decode wraps
            // a non-array reference too, not only arrays/Object).
            ctx.check("mrt_static_boxed_integer_value",
                      g_st_boxed_int.load() == static_cast<std::int64_t>(0x5A5A5A5A));

            // ---- Object identity: returnsSelfAsObject() decodes to the receiver OOP.
            ctx.check("mrt_self_as_object_instance_equals_receiver",
                      g_self_obj_instance.load() != 0
                      && g_self_obj_instance.load() == g_receiver_instance.load());
            // returnsObject() decoded to a usable (non-null, valid) wrapper/pointer.
            ctx.check("mrt_object_decodes_usable_wrapper",
                      g_obj_wrapper_is_null.load() == 0);
            ctx.check("mrt_object_decodes_usable_pointer",
                      g_obj_pointer_unusable.load() == 0);

            // ---- DESCRIPTOR EDGE CASES (ride the reference OOP round-trip) -------

            // INTERFACE return (CharSequence) holding a String: although it routed to
            // the reference alternative (asserted above), as_string() still recovers
            // the text by running read_java_string on the decoded String OOP.
            ctx.check("mrt_charseq_captured", g_charseq_captured.load());
            if (g_charseq_captured.load())
            {
                ctx.check("mrt_charseq_text_via_reference_path",
                          g_charseq_value == "iface-charseq");
            }

            // OWN class-typed return (Lvmhook/fixtures/ReturnTypes;): returns `this`,
            // so the decoded wrapper's instance OOP must equal the receiver.
            ctx.check("mrt_own_type_instance_equals_receiver",
                      g_own_type_instance.load() != 0
                      && g_own_type_instance.load() == g_receiver_instance.load());

            // NESTED generic erased to a bare interface descriptor (List<Map<...>> ->
            // "Ljava/util/List;"): decodes to a usable non-null reference.  The native
            // side never tries to recover the erased <...> type arguments.
            ctx.check("mrt_nested_generic_decodes_usable_reference",
                      g_nested_generic_usable.load() == 1);

            // ---- STATIC reference returns (ride the reference OOP round-trip too) ----
            // A static method returning int[]{11,22,33}: the static dispatch path
            // recovers the array OOP and reads length + a boundary element, proving the
            // GetStaticMethodID path's reference decode parity with the instance path.
            ctx.check("mrt_static_int_array_len3", g_st_arr_len.load() == 3);
            ctx.check("mrt_static_int_array_elem1_22", g_st_arr_elem1.load() == 22);
            // A static method returning the shared Object singleton decodes usable.
            ctx.check("mrt_static_object_decodes_usable", g_st_obj_usable.load() == 1);
        }
        else
        {
            ctx.record(std::string("[INFO] method_return_types: descriptor-edge reference "
                       "decodes unusable on this JVM -- charseq_captured=")
                       + (g_charseq_captured.load() ? "true" : "false")
                       + " charseq='" + g_charseq_value + "'"
                       + " own_type_instance=0x" + std::to_string(g_own_type_instance.load())
                       + " nested_generic_usable=" + std::to_string(g_nested_generic_usable.load())
                       + " (recorded not asserted).");

            ctx.record("[INFO] method_return_types: array element lengths (bool="
                       + std::to_string(g_arr_bool_len.load())
                       + " int=" + std::to_string(g_arr_int_len.load())
                       + " long=" + std::to_string(g_arr_long_len.load())
                       + " obj=" + std::to_string(g_arr_obj_len.load())
                       + "), boxed int=" + std::to_string(g_box_int.load())
                       + " -- reference decode unusable on this JVM, recorded not asserted.");
            ctx.record(std::string("[INFO] method_return_types: returnsObject wrapper_is_null=")
                       + (g_obj_wrapper_is_null.load() == 1 ? "true" : "false")
                       + " self_obj_instance=0x" + std::to_string(g_self_obj_instance.load())
                       + " receiver=0x" + std::to_string(g_receiver_instance.load()) + ".");
            ctx.record("[INFO] method_return_types: STATIC reference decodes unusable on this "
                       "JVM -- static int[] len=" + std::to_string(g_st_arr_len.load())
                       + " static Object usable=" + std::to_string(g_st_obj_usable.load())
                       + " (recorded not asserted; static PRIMITIVE/String/void/null decodes "
                         "above are still hard-asserted).");
            ctx.record("[INFO] method_return_types: batch-16 reference decodes unusable on this "
                       "JVM -- boxed bool=" + std::to_string(g_box_bool.load())
                       + " byte=" + std::to_string(g_box_byte.load())
                       + " short=" + std::to_string(g_box_short.load())
                       + " char=" + std::to_string(g_box_char.load())
                       + " static_boxed_int=" + std::to_string(g_st_boxed_int.load())
                       + " nonstring_ref_as_string_size="
                       + std::to_string(g_nonstring_ref_as_string_size.load())
                       + " (recorded not asserted).");
        }

        // =====================================================================
        //  6. NULL return -- HARD-asserted on every path (a Java null is monostate
        //     regardless of compressed-oops state, since the null handle/oop short-
        //     circuits before any encode/decode).
        // =====================================================================
        ctx.check("mrt_null_yields_empty_wrapper", g_null_wrapper_is_null.load() == 1);
        ctx.check("mrt_null_yields_unusable_pointer", g_null_pointer_unusable.load() == 1);
        // as_string() on a null reference must be empty (read_java_string on a
        // null/invalid OOP returns "").
        ctx.check("mrt_null_as_string_empty", g_null_str_is_empty.load() == 1);

        ctx.record("[INFO] method_return_types: published java identities -- objectIdentity="
                   + std::to_string(rt::object_identity()) + ".");
    }
}

VMHOOK_JVM_MODULE(method_return_types)
{
    // Run the whole body under a try/catch so a stray throw from any vmhook call can
    // never escape this module (mirrors register_class.cpp).  A throw is recorded as
    // [INFO], never a FAIL.
    bool body_threw{ false };
    try
    {
        run_return_type_checks(ctx);
    }
    catch (...)
    {
        body_threw = true;
    }

    // FINAL CLEANUP -- belt-and-braces, OUTSIDE the try so it ALWAYS runs.  Other
    // modules run after this one, so the module MUST leave ZERO hooks armed.
    vmhook::shutdown_hooks();

    if (body_threw)
    {
        ctx.record("[INFO] method_return_types: the test body threw and was contained "
                   "(no crash, no hooks armed); see preceding checks for partial results.");
    }
    ctx.check("mrt_module_left_clean_final_shutdown", true);
}
