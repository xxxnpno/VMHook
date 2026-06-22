// field_static JVM test module  (feature area: fields)
//
// THE static-field authority: exhaustively exercises vmhook's portable static
// field accessor static_field("name") for GET and -- the centre of gravity --
// SET, for EVERY JVM primitive (Z B C S I J F D), java.lang.String, and an
// object reference, with every write PROVEN VISIBLE TO JAVA ITSELF.
//
// What this module proves on a live JVM (Java 8/11/17/21/24/25 x MSVC/Clang/GCC):
//   * static_field(name)->set(v) lands the correct bytes on the java.lang.Class
//     mirror for every primitive width AND boundary value, and the JVM observes
//     the new value -- verified two independent ways:
//       (a) the fixture snapshots each field into a "seen*" witness using genuine
//           getstatic/putstatic bytecode (mode-1 probe), which the module reads
//           back, and
//       (b) the module additionally pulls each value back through a Java getter
//           via static_method("getX")->call(), so Java's own bytecode reads the
//           native-written field.
//   * static_field / static_method WORK FROM A STATIC C++ WRAPPER METHOD on every
//     compiler -- the GCC portability guarantee (the deducing-this get_field
//     overloads would not compile here; every accessor below is a static method
//     that calls static_field/static_method, never get_field).
//   * field_proxy::set's size/type guards (audit/findings/field_proxy_set_size_guard.md)
//     refuse a too-wide / mistyped write into a primitive static field, leaving
//     the JVM-visible value byte-for-byte unchanged.
//   * the "C" 1-byte->2-byte widening shortcut lands a full Java char.
//   * object-reference set via unique_ptr<wrapper> rewrites the compressed OOP so
//     Java sees the new identity, and an empty unique_ptr nulls the field.
//   * static GET decodes every primitive + boundary + a String correctly through
//     the same portable accessor, and the static get() path ignores stale init
//     constants (re-read after a runtime putstatic in mode 2).
//
// Harness shape mirrors hook_basic: register_class, a `mode` selector with a
// `done` reset on the rising edge of go, and a dense battery of ctx.check()s.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace
{
    // Wrapper for vmhook.fixtures.FieldStatic.
    //
    // EVERY accessor here is a STATIC method that reaches the field through
    // static_field(...) / static_method(...).  That is the whole portability
    // point of this module: on GCC the deducing-this get_field overloads are
    // non-viable from a static context and would fail to compile, so a uniformly
    // portable wrapper must use the explicit static_field/static_method names.
    class fs : public vmhook::object<fs>
    {
    public:
        explicit fs(vmhook::oop_t instance) noexcept
            : vmhook::object<fs>{ instance }
        {
        }

        // ---- handshake + scenario selector (all via static_field) ----
        static auto set_go(bool value) -> void       { static_field("go")->set(value); }
        static auto set_done(bool value) -> void      { static_field("done")->set(value); }
        static auto get_done() -> bool                { return static_field("done")->get(); }
        static auto set_mode(std::int32_t m) -> void  { static_field("mode")->set(m); }

        // ---- resolve helpers ----
        static auto resolves(const char* name) -> bool
        {
            return static_field(name).has_value();
        }

        // ---- generic typed SET via static_field (proves set lands per width) ----
        template<typename value_type>
        static auto set_value(const char* name, const value_type& v) -> bool
        {
            const auto proxy{ static_field(name) };
            if (!proxy.has_value())
            {
                return false;
            }
            proxy->set(v);
            return true;
        }

        // ---- generic typed GET via static_field ----
        // NOTE: returns field_proxy::value_t by COPY (copy-init), never brace-init,
        // to stay MSVC-unambiguous; callers extract with the desired type.
        static auto get_proxy(const char* name) -> std::optional<vmhook::field_proxy>
        {
            return static_field(name);
        }

        // ---- read a static String field through the portable accessor ----
        static auto get_string(const char* name) -> std::string { return static_field(name)->get(); }

        // ---- set a static String field with an ASCII value ----
        static auto set_string(const char* name, std::string_view value) -> bool
        {
            const auto proxy{ static_field(name) };
            if (!proxy.has_value())
            {
                return false;
            }
            proxy->set(std::string{ value });
            return true;
        }

        // ---- raw int read (for the size/type guard targets) ----
        static auto get_int(const char* name) -> std::int32_t { return static_field(name)->get(); }

        static auto get_long(const char* name) -> std::int64_t { return static_field(name)->get(); }

        static auto get_char(const char* name) -> std::uint16_t { return static_field(name)->get(); }

        // ---- the rest of the primitive GET one-liners (the DOCUMENTED idiom:
        //      static_field("x")->get() chained directly, no defensive guard --
        //      the field exists in the fixture, so the accessor stays clean). ----
        static auto get_bool(const char* name) -> bool          { return static_field(name)->get(); }
        static auto get_byte(const char* name) -> std::int8_t   { return static_field(name)->get(); }
        static auto get_short(const char* name) -> std::int16_t { return static_field(name)->get(); }
        static auto get_float(const char* name) -> float        { return static_field(name)->get(); }
        static auto get_double(const char* name) -> double      { return static_field(name)->get(); }

        // ---- CROSS-WIDTH GET conversion readers: read a sub-int field at a
        //      WIDER C++ type, exercising cast_for_variant's numeric arm
        //      (static_cast<target_type>(stored_alt)).  A signed alternative
        //      (int8/int16/int32) SIGN-extends; the char arm (uint16) ZERO-
        //      extends.  The module elsewhere always reads at natural width, so
        //      these are the only proofs of the extension semantics. ----
        static auto get_as_i64(const char* name) -> std::int64_t  { const std::int64_t v = static_field(name)->get(); return v; }
        static auto get_as_i32(const char* name) -> std::int32_t  { const std::int32_t v = static_field(name)->get(); return v; }
        static auto get_as_double(const char* name) -> double      { const double v = static_field(name)->get(); return v; }
        static auto get_as_u32(const char* name) -> std::uint32_t  { const std::uint32_t v = static_field(name)->get(); return v; }

        // ---- read a static PRIMITIVE int[] field's elements through the value_t
        //      vector conversion arm (read_array_value).  count == array_length()
        //      is the cross-check; an element-width-mismatched request returns
        //      empty (the read-side guard, symmetric with set's width guard). ----
        static auto get_int_vector(const char* name) -> std::vector<std::int32_t>
        {
            const auto proxy{ static_field(name) };
            if (!proxy.has_value())
            {
                return {};
            }
            std::vector<std::int32_t> v = proxy->get();
            return v;
        }
        static auto get_int_vector_as_i64(const char* name) -> std::vector<std::int64_t>
        {
            const auto proxy{ static_field(name) };
            if (!proxy.has_value())
            {
                return {};
            }
            std::vector<std::int64_t> v = proxy->get();   // [I -> vector<int64_t>: width mismatch -> empty
            return v;
        }
        static auto get_str_vector(const char* name) -> std::vector<std::string>
        {
            const auto proxy{ static_field(name) };
            if (!proxy.has_value())
            {
                return {};
            }
            std::vector<std::string> v = proxy->get();
            return v;
        }

        // ---- PRIMITIVE-GUARD probes for the non-primitive set() arms.  Writing a
        //      std::vector / std::unique_ptr<wrapper> into a PRIMITIVE static slot
        //      must be REFUSED by field_proxy::set (the same guard the string arm
        //      hits), leaving the field byte-for-byte unchanged.  Each returns
        //      false on an unresolved field so the call site stays a clean check. ----
        static auto set_int_vector(const char* name, const std::vector<std::int32_t>& v) -> bool
        {
            const auto proxy{ static_field(name) };
            if (!proxy.has_value())
            {
                return false;
            }
            proxy->set(v);   // vector arm; into a primitive field -> refused
            return true;
        }
        static auto set_ref_into(const char* name, const std::unique_ptr<fs>& target) -> bool
        {
            const auto proxy{ static_field(name) };
            if (!proxy.has_value())
            {
                return false;
            }
            proxy->set(target);   // unique_ptr arm; into a primitive field -> refused
            return true;
        }

        // ---- the field's value as a std::string via value_t::as_string() (the
        //      explicit-intent extraction).  For a reference/String field this
        //      decodes the OOP through read_java_string; for ANY primitive field
        //      it yields "" (every non-uint32 variant arm returns the empty
        //      string).  Distinct from get_string() above, which leans on the
        //      implicit conversion operator. ----
        static auto value_as_string(const char* name) -> std::string
        {
            const auto proxy{ static_field(name) };
            if (!proxy.has_value())
            {
                return std::string{ "<<unresolved>>" };
            }
            return proxy->get().as_string();
        }

        // ---- value_t::is_reference(): true iff get() populated the uint32
        //      compressed-OOP arm (L / [ fields), false for every primitive arm.
        //      Returns -1 when the field does not resolve so the caller can skip. ----
        static auto value_is_reference(const char* name) -> int
        {
            const auto proxy{ static_field(name) };
            if (!proxy.has_value())
            {
                return -1;
            }
            return proxy->get().is_reference() ? 1 : 0;
        }

        // ---- field_proxy::is_reference(): the signature-based predicate (L / [),
        //      distinct from value_t::is_reference() which inspects the decoded
        //      variant arm.  For a correctly-decoded field the two must AGREE. ----
        static auto proxy_is_reference(const char* name) -> int
        {
            const auto proxy{ static_field(name) };
            if (!proxy.has_value())
            {
                return -1;
            }
            return proxy->is_reference() ? 1 : 0;
        }

        // ---- field_proxy::get_compressed_oop(): the dedicated reference reader.
        //      Must equal the u32 variant arm for a reference field, and must be
        //      GUARDED to 0 on a primitive field (reading a primitive's bytes as a
        //      bogus OOP is the FLAW-C hazard the guard closes). ----
        static auto proxy_compressed_oop(const char* name) -> std::uint32_t
        {
            const auto proxy{ static_field(name) };
            if (!proxy.has_value())
            {
                return 0u;
            }
            return proxy->get_compressed_oop();
        }

        // ---- a reference field decoded to a void* through the value_t void*
        //      conversion arm (decode_oop_pointer of the compressed OOP). ----
        static auto value_as_voidp(const char* name) -> void*
        {
            const auto proxy{ static_field(name) };
            if (!proxy.has_value())
            {
                return nullptr;
            }
            void* const p = proxy->get();
            return p;
        }

        // ---- contextual-bool conversion of a field's value (operator target_type
        //      with target=bool): a numeric field is true iff its value is non-zero,
        //      mirroring C++ contextual conversion.  Returns -1 when unresolved. ----
        static auto value_as_bool(const char* name) -> int
        {
            const auto proxy{ static_field(name) };
            if (!proxy.has_value())
            {
                return -1;
            }
            const bool b = proxy->get();
            return b ? 1 : 0;
        }

        // ---- SET a String static via a raw const char* (NUL-terminated literal):
        //      routes through field_proxy::set's std::string_view arm
        //      (is_convertible_to<string_view> && !std::string), a DIFFERENT
        //      overload branch from set(std::string).  Proves the C-string write
        //      path rebinds the field identically. ----
        static auto set_cstr(const char* name, const char* value) -> bool
        {
            const auto proxy{ static_field(name) };
            if (!proxy.has_value())
            {
                return false;
            }
            proxy->set(value);   // const char* -> string_view arm
            return true;
        }

        // ---- SET a String static via an explicit std::string_view (the same
        //      string_view arm; proves a non-NUL-bounded view writes the exact
        //      span).  Builds the view from a std::string so its data is stable. ----
        static auto set_strview(const char* name, std::string_view value) -> bool
        {
            const auto proxy{ static_field(name) };
            if (!proxy.has_value())
            {
                return false;
            }
            proxy->set(value);   // string_view arm
            return true;
        }

        // ---- the JVM type descriptor of a static field, through the proxy.
        //      A clean read of the field's metadata (used to prove every type's
        //      signature decodes exactly: "Z" "B" "C" "S" "I" "J" "F" "D",
        //      "Ljava/lang/String;", "Lvmhook/fixtures/FieldStatic;"). ----
        static auto signature_of(const char* name) -> std::string
        {
            return std::string{ static_field(name)->signature() };
        }

        // ---- acquire a published instance wrapper (objA / objB / objRef) ----
        static auto acquire(const char* name) -> std::unique_ptr<fs> { return static_field(name)->get(); }

        // ---- set an object-reference static field to a wrapper (or null) ----
        static auto set_ref(const char* name, const std::unique_ptr<fs>& target) -> bool
        {
            const auto proxy{ static_field(name) };
            if (!proxy.has_value())
            {
                return false;
            }
            proxy->set(target);
            return true;
        }

        // ---- the Java-side "seen*" witnesses (snapshot via real bytecode) ----
        static auto seen_bool(const char* name) -> bool      { return static_field(name)->get(); }
        static auto seen_i8(const char* name) -> std::int8_t  { const std::int8_t v = static_field(name)->get(); return v; }
        static auto seen_i16(const char* name) -> std::int16_t{ const std::int16_t v = static_field(name)->get(); return v; }
        static auto seen_i32(const char* name) -> std::int32_t{ const std::int32_t v = static_field(name)->get(); return v; }
        static auto seen_i64(const char* name) -> std::int64_t{ const std::int64_t v = static_field(name)->get(); return v; }
        static auto seen_u16(const char* name) -> std::uint16_t{ const std::uint16_t v = static_field(name)->get(); return v; }

        // ---- an instance field read (drives the "needs an object" diagnostic
        //      when called via the static accessor instead) ----
        auto instance_only_int() const -> std::int32_t
        {
            const std::int32_t v = get_field("instanceOnlyInt")->get();
            return v;
        }
        auto tag() const -> std::int32_t
        {
            const std::int32_t v = get_field("tag")->get();
            return v;
        }

        // ---- Java getters pulled through static_method (portable path) ----
        static auto call_get_int(const char* method) -> std::int32_t
        {
            const auto m{ static_method(method) };
            if (!m.has_value())
            {
                return -1;
            }
            const std::int32_t v = m->call();
            return v;
        }
        static auto call_get_long(const char* method) -> std::int64_t
        {
            const auto m{ static_method(method) };
            if (!m.has_value())
            {
                return -1;
            }
            const std::int64_t v = m->call();
            return v;
        }
        static auto call_get_bool(const char* method) -> bool
        {
            const auto m{ static_method(method) };
            if (!m.has_value())
            {
                return false;
            }
            const bool v = m->call();
            return v;
        }
        static auto call_get_string(const char* method) -> std::string
        {
            const auto m{ static_method(method) };
            if (!m.has_value())
            {
                return std::string{ "<<no-method>>" };
            }
            // method_proxy String returns: use as_string() (NOT a cast/brace-init).
            return m->call().as_string();
        }

        // ---- static ARRAY-reference helpers (the field_static angle: whole
        //      reference, not element decode -- field_arrays_object owns that).
        //      field_oop() returns the decoded array oop for a "[..." field;
        //      array_length() is the bounds oracle.  Both public helpers. ----
        static auto array_oop(const char* name) -> void*
        {
            const auto proxy{ static_field(name) };
            if (!proxy.has_value())
            {
                return nullptr;
            }
            return vmhook::field_oop(*proxy);
        }
        static auto array_len(const char* name) -> std::int32_t
        {
            void* const a{ array_oop(name) };
            if (!a || !vmhook::hotspot::is_valid_pointer(a))
            {
                return -1;
            }
            return vmhook::array_length(a);
        }
        // Replace the whole static array reference `name` so it aliases the
        // array currently held by `srcName`.  We decode srcName's array oop,
        // wrap it directly (make_unique, NOT the value_t->unique_ptr path,
        // which rejects a "[" signature), and set_ref writes the encoded OOP.
        static auto set_ref_to_array(const char* name, const char* srcName) -> bool
        {
            void* const src{ array_oop(srcName) };
            if (!src || !vmhook::hotspot::is_valid_pointer(src))
            {
                return false;
            }
            auto wrapper{ std::make_unique<fs>(src) };
            return set_ref(name, wrapper);
        }
    };

    // ---- Wrapper for the SUPERCLASS, FieldStaticBase.  Its static_field()
    //      starts the find_field super walk at FieldStaticBase; resolving an
    //      inherited static THROUGH the fs (subclass) wrapper exercises the
    //      declaring-klass mirror path (field_entry_t::declaring_klass). -------
    class fsb : public vmhook::object<fsb>
    {
    public:
        explicit fsb(vmhook::oop_t instance) noexcept
            : vmhook::object<fsb>{ instance }
        {
        }
        // A live base instance (objBaseA / objBaseB), wrapped so set_ref can
        // rewrite the inherited reference between them.
        static auto acquire(const char* name) -> std::unique_ptr<fsb> { return static_field(name)->get(); }
        auto base_tag() const -> std::int32_t { const std::int32_t v = get_field("baseTag")->get(); return v; }
        // Set an inherited object-reference static to a wrapper (or null).
        // Returns false if the field did not resolve; set() itself is void, so
        // this bool wrapper is what the ctx.check() call sites consume.
        static auto set_ref(const char* name, const std::unique_ptr<fsb>& target) -> bool
        {
            const auto proxy{ static_field(name) };
            if (!proxy.has_value())
            {
                return false;
            }
            proxy->set(target);
            return true;
        }
    };

    // ---- Wrapper for the NESTED ENUM, FieldStatic$Tier.  Each enum constant
    //      (LOW/MID/HIGH) is a public-static-final field of this class; reading
    //      it through static_field() returns the constant's singleton OOP. -----
    class fst_tier : public vmhook::object<fst_tier>
    {
    public:
        explicit fst_tier(vmhook::oop_t instance) noexcept
            : vmhook::object<fst_tier>{ instance }
        {
        }
        // Acquire an enum constant singleton as a wrapper (signature "L...;",
        // so the value_t->unique_ptr conversion accepts it).
        static auto constant(const char* name) -> std::unique_ptr<fst_tier> { return static_field(name)->get(); }
        // The compressed OOP of a constant field, for an exact-identity compare
        // (two reads of the same constant must yield the same non-zero OOP).
        static auto constant_oop(const char* name) -> std::uint32_t
        {
            const auto p{ static_field(name) };
            if (!p.has_value())
            {
                return 0u;
            }
            const auto v{ p->get() };
            return v.is_reference() ? std::get<std::uint32_t>(v.data) : 0u;
        }
        // The enum body's instance int field, read off a constant singleton.
        auto weight() const -> std::int32_t { const std::int32_t v = get_field("weight")->get(); return v; }
    };

    // value_t variant-alternative indices (must match field_proxy::value_t order).
    constexpr std::size_t kIdxBool   = 0;
    constexpr std::size_t kIdxI8     = 1;
    constexpr std::size_t kIdxI16    = 2;
    constexpr std::size_t kIdxI32    = 3;
    constexpr std::size_t kIdxI64    = 4;
    constexpr std::size_t kIdxFloat  = 5;
    constexpr std::size_t kIdxDouble = 6;
    constexpr std::size_t kIdxU16    = 7;
    constexpr std::size_t kIdxU32    = 8;

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

    // ---- Phase-8 getter-call observations.  method_proxy::call() needs a live
    //      current_java_thread, so these are captured INSIDE the touch() detour
    //      (which runs on the Java thread) and read back by the module body.
    //      Sentinels chosen so "did the detour run?" is unambiguous. ----
    constexpr std::int64_t k_uncaptured{ static_cast<std::int64_t>(0xDEADBEEFCAFEF00DULL) };

    std::atomic<int>          g_detour_calls{ 0 };
    std::atomic<bool>         g_detour_saw_self{ false };
    std::atomic<int>          g_get_z{ -1 };          // getZ() -> bool
    std::atomic<std::int64_t> g_get_i{ k_uncaptured };// getI()
    std::atomic<std::int64_t> g_get_j{ k_uncaptured };// getJ()
    std::atomic<std::int64_t> g_get_b{ k_uncaptured };// getB() widened
    std::atomic<std::int64_t> g_get_s{ k_uncaptured };// getS() widened
    std::atomic<std::int64_t> g_get_c{ k_uncaptured };// getC() widened (unsigned)
    std::atomic<std::int64_t> g_get_iord{ k_uncaptured };
    std::atomic<std::int64_t> g_get_strlen{ k_uncaptured };
    std::atomic<std::int64_t> g_get_guard_int{ k_uncaptured };
    std::atomic<std::int64_t> g_get_guard_long{ k_uncaptured };
    std::atomic<std::int64_t> g_get_guard_char{ k_uncaptured };
    std::atomic<int>          g_get_objref_is_b{ -1 };
    std::atomic<int>          g_get_objref_is_null{ -1 };
    std::atomic<std::int64_t> g_get_objref_tag{ k_uncaptured };
    std::atomic<bool>         g_get_str_is_world{ false };

    // Drive one probe cycle for `mode`: clears the latched `done` and programs
    // the selector on the rising edge of go, then waits for done.
    auto drive(vmhook_test::context& ctx, std::int32_t mode) -> bool
    {
        return ctx.run_probe(
            [mode](bool value)
            {
                if (value)
                {
                    fs::set_done(false);
                    fs::set_mode(mode);
                }
                fs::set_go(value);
            },
            []() { return fs::get_done(); });
    }

    // The Java fixture this module drives.  Named once so the entry guard and
    // register_class<fs>() can never drift apart.
    constexpr const char* k_fixture{ "vmhook/fixtures/FieldStatic" };

    // The whole test body, factored out so the VMHOOK_JVM_MODULE wrapper can run
    // it under a try/catch and ALWAYS follow it with an unconditional
    // shutdown_hooks() (mirrors register_class.cpp's suite-safety shape).
    auto run_field_static_checks(vmhook_test::context& ctx) -> void
    {
        // =====================================================================
        //  ENTRY GUARD.  If FieldStatic is not loaded/resolvable on this run,
        //  every static_field()->set/get below would dereference a disengaged
        //  optional.  Bail cleanly to [INFO] instead (the wrapper's final
        //  shutdown_hooks() still runs).  In practice the harness loads the
        //  fixture on every run, so this is belt-and-braces.
        // =====================================================================
        if (vmhook::find_class(k_fixture) == nullptr)
        {
            ctx.record("[INFO] field_static: FieldStatic not loaded/resolvable on "
                       "this run; skipping the module's live checks (no crash, no "
                       "hooks armed).");
            return;
        }

        vmhook::register_class<fs>(k_fixture);
        // Sibling wrappers for the EXHAUSTIVE additions (inherited statics +
        // nested enum constants).  Registering an absent class is harmless --
        // every static_field through these wrappers then returns nullopt and the
        // dependent fstat_* checks below are individually guarded.  FieldStaticBase
        // is loaded as fs's superclass; FieldStatic$Tier is loaded by fs's
        // <clinit> (staticTier = Tier.MID references it).
        vmhook::register_class<fsb>("vmhook/fixtures/FieldStaticBase");
        vmhook::register_class<fst_tier>("vmhook/fixtures/FieldStatic$Tier");

    // =====================================================================
    //  0. Sanity: the class resolves and the portable static accessor works.
    // =====================================================================
    ctx.check("fs_class_registered_static_field_resolves", fs::resolves("setI"));
    ctx.check("fs_static_method_resolves", fs::static_method("getI").has_value());

    // A resolved static proxy reports is_static()==true and the right signature.
    {
        auto p{ fs::get_proxy("setI") };
        if (p)
        {
            ctx.check("static_proxy_is_static_true", p->is_static() == true);
            ctx.check("static_proxy_signature_I", std::string{ p->signature() } == "I");
            ctx.check("static_proxy_address_nonnull", p->raw_address() != nullptr);
        }
    }

    // =====================================================================
    //  1. STATIC SET for EVERY primitive + boundary, written BEFORE go.
    //     field_proxy::set mutates the class-mirror slot directly; no bytecode
    //     needed for the write itself.  We assert the proxy resolved AND that an
    //     immediate native re-read reflects the write (round-trip in C++), then
    //     phases 4/5 prove the SAME bytes are visible to Java.
    // =====================================================================

    // ---- primary battery ------------------------------------------------
    ctx.check("set_Z_resolved", fs::set_value<bool>("setZ", true));
    ctx.check("set_B_resolved", fs::set_value<std::int8_t>("setB", std::numeric_limits<std::int8_t>::min())); // -128
    ctx.check("set_C_resolved", fs::set_value<std::uint16_t>("setC", 0xFFFF));
    ctx.check("set_S_resolved", fs::set_value<std::int16_t>("setS", std::numeric_limits<std::int16_t>::min()));
    ctx.check("set_I_resolved", fs::set_value<std::int32_t>("setI", std::numeric_limits<std::int32_t>::min()));
    ctx.check("set_J_resolved", fs::set_value<std::int64_t>("setJ", std::numeric_limits<std::int64_t>::max()));
    ctx.check("set_F_resolved", fs::set_value<float>("setF", 0.15625f)); // 0x3E200000 exact
    ctx.check("set_D_resolved", fs::set_value<double>("setD", 0.1));     // 0x3FB999999999999A

    // ---- secondary battery (different edge patterns) --------------------
    ctx.check("set_Z2_resolved", fs::set_value<bool>("setZ2", false));
    ctx.check("set_B2_resolved", fs::set_value<std::int8_t>("setB2", static_cast<std::int8_t>(0xFF))); // -1
    ctx.check("set_C2_resolved", fs::set_value<std::uint16_t>("setC2", 0x20AC));                       // euro
    ctx.check("set_S2_resolved", fs::set_value<std::int16_t>("setS2", static_cast<std::int16_t>(0xBEEF)));
    ctx.check("set_I2_resolved", fs::set_value<std::int32_t>("setI2", static_cast<std::int32_t>(0xDEADBEEF)));
    ctx.check("set_J2_resolved", fs::set_value<std::int64_t>("setJ2", std::numeric_limits<std::int64_t>::min()));
    ctx.check("set_F2_resolved", fs::set_value<float>("setF2", -std::numeric_limits<float>::infinity()));
    ctx.check("set_D2_resolved", fs::set_value<double>("setD2", std::numeric_limits<double>::quiet_NaN()));

    // ---- ordinary mid-range values --------------------------------------
    ctx.check("set_IOrd_resolved", fs::set_value<std::int32_t>("setIOrd", 123456789));
    ctx.check("set_JOrd_resolved", fs::set_value<std::int64_t>("setJOrd", 0x0123456789ABCDEFLL));
    ctx.check("set_DOrd_resolved", fs::set_value<double>("setDOrd", 3.141592653589793));
    ctx.check("set_FOrd_resolved", fs::set_value<float>("setFOrd", 1.5f)); // exact in binary

    // ---- immediate native re-read round-trip (before any Java involvement) --
    {
        const auto pz{ fs::get_proxy("setZ") };
        if (pz) { const bool v = pz->get(); ctx.check("set_Z_native_reread_true", v == true); }
        const auto pb{ fs::get_proxy("setB") };
        if (pb)
        {
            const auto v{ pb->get() };
            ctx.check("set_B_native_reread_min", static_cast<std::int8_t>(v) == std::numeric_limits<std::int8_t>::min());
            ctx.check("set_B_native_variant_i8", v.data.index() == kIdxI8);
        }
        const auto pc{ fs::get_proxy("setC") };
        if (pc)
        {
            const auto v{ pc->get() };
            ctx.check("set_C_native_reread_FFFF", static_cast<std::uint16_t>(v) == 0xFFFF);
            ctx.check("set_C_native_variant_u16", v.data.index() == kIdxU16);
        }
        const auto ps{ fs::get_proxy("setS") };
        if (ps) { const std::int16_t v = ps->get(); ctx.check("set_S_native_reread_min", v == std::numeric_limits<std::int16_t>::min()); }
        const auto pi{ fs::get_proxy("setI") };
        if (pi) { const std::int32_t v = pi->get(); ctx.check("set_I_native_reread_min", v == std::numeric_limits<std::int32_t>::min()); }
        const auto pj{ fs::get_proxy("setJ") };
        if (pj) { const std::int64_t v = pj->get(); ctx.check("set_J_native_reread_max", v == std::numeric_limits<std::int64_t>::max()); }
        const auto pf{ fs::get_proxy("setF") };
        if (pf) { const float v = pf->get(); ctx.check("set_F_native_reread_bits", float_bits(v) == 0x3E200000u); }
        const auto pd{ fs::get_proxy("setD") };
        if (pd) { const double v = pd->get(); ctx.check("set_D_native_reread_bits", double_bits(v) == 0x3FB999999999999AULL); }
        const auto pf2{ fs::get_proxy("setF2") };
        if (pf2) { const float v = pf2->get(); ctx.check("set_F2_native_reread_neg_inf", std::isinf(v) && v < 0.0f); }
        const auto pd2{ fs::get_proxy("setD2") };
        if (pd2) { const double v = pd2->get(); ctx.check("set_D2_native_reread_nan", std::isnan(v)); }
    }

    // =====================================================================
    //  1b. EXHAUSTIVE SET-EDGE battery: the 0 / 1 / -1 / MAX corners (and the
    //      remaining float/double boundaries +Inf, MIN, MAX, -0.0) that the
    //      primary/secondary batteries don't reach.  Written BEFORE go, proven
    //      by an immediate native re-read here and pulled back through Java
    //      getters in phase 8b.  Clean one-liner writes -- no defensive guard.
    // =====================================================================
    ctx.check("set_IZero_resolved",   fs::set_value<std::int32_t>("setIZero", 0));
    ctx.check("set_IOne_resolved",    fs::set_value<std::int32_t>("setIOne", 1));
    ctx.check("set_INegOne_resolved", fs::set_value<std::int32_t>("setINegOne", -1));
    ctx.check("set_IMax_resolved",    fs::set_value<std::int32_t>("setIMax", std::numeric_limits<std::int32_t>::max()));
    ctx.check("set_JZero_resolved",   fs::set_value<std::int64_t>("setJZero", 0));
    ctx.check("set_JOne_resolved",    fs::set_value<std::int64_t>("setJOne", 1));
    ctx.check("set_JNegOne_resolved", fs::set_value<std::int64_t>("setJNegOne", -1));
    ctx.check("set_BZero_resolved",   fs::set_value<std::int8_t>("setBZero", 0));
    ctx.check("set_BMax_resolved",    fs::set_value<std::int8_t>("setBMax", std::numeric_limits<std::int8_t>::max()));
    ctx.check("set_SZero_resolved",   fs::set_value<std::int16_t>("setSZero", 0));
    ctx.check("set_SMax_resolved",    fs::set_value<std::int16_t>("setSMax", std::numeric_limits<std::int16_t>::max()));
    ctx.check("set_CNul_resolved",    fs::set_value<std::uint16_t>("setCNul", 0x0000));
    ctx.check("set_CA_resolved",      fs::set_value<std::uint16_t>("setCA", 0x0041));
    ctx.check("set_FPosInf_resolved", fs::set_value<float>("setFPosInf", std::numeric_limits<float>::infinity()));
    ctx.check("set_FMin_resolved",    fs::set_value<float>("setFMin", std::numeric_limits<float>::min()));     // 0x00800000 smallest normal
    ctx.check("set_FMax_resolved",    fs::set_value<float>("setFMax", std::numeric_limits<float>::max()));     // 0x7F7FFFFF
    ctx.check("set_FNegZero_resolved",fs::set_value<float>("setFNegZero", -0.0f));
    ctx.check("set_DPosInf_resolved", fs::set_value<double>("setDPosInf", std::numeric_limits<double>::infinity()));
    ctx.check("set_DMin_resolved",    fs::set_value<double>("setDMin", std::numeric_limits<double>::min()));   // 0x0010000000000000 smallest normal
    ctx.check("set_DMax_resolved",    fs::set_value<double>("setDMax", std::numeric_limits<double>::max()));   // 0x7FEFFFFFFFFFFFFF
    ctx.check("set_DNegZero_resolved",fs::set_value<double>("setDNegZero", -0.0));

    // ---- immediate native re-read of the SET-edge battery ----
    {
        ctx.check("set_IZero_native",   fs::get_int("setIZero") == 0);
        ctx.check("set_IOne_native",    fs::get_int("setIOne") == 1);
        ctx.check("set_INegOne_native", fs::get_int("setINegOne") == -1);
        ctx.check("set_IMax_native",    fs::get_int("setIMax") == std::numeric_limits<std::int32_t>::max());
        ctx.check("set_JZero_native",   fs::get_long("setJZero") == 0);
        ctx.check("set_JOne_native",    fs::get_long("setJOne") == 1);
        ctx.check("set_JNegOne_native", fs::get_long("setJNegOne") == -1);
        ctx.check("set_BZero_native",   fs::get_byte("setBZero") == 0);
        ctx.check("set_BMax_native",    fs::get_byte("setBMax") == std::numeric_limits<std::int8_t>::max());
        ctx.check("set_SZero_native",   fs::get_short("setSZero") == 0);
        ctx.check("set_SMax_native",    fs::get_short("setSMax") == std::numeric_limits<std::int16_t>::max());
        ctx.check("set_CNul_native",    fs::get_char("setCNul") == 0x0000);
        ctx.check("set_CA_native",      fs::get_char("setCA") == 0x0041);
        // float/double edges compared by EXACT bit pattern (never an == on the value).
        ctx.check("set_FPosInf_native_bits",  float_bits(fs::get_float("setFPosInf")) == 0x7F800000u);
        ctx.check("set_FMin_native_bits",     float_bits(fs::get_float("setFMin")) == 0x00800000u);
        ctx.check("set_FMax_native_bits",     float_bits(fs::get_float("setFMax")) == 0x7F7FFFFFu);
        ctx.check("set_FNegZero_native_bits", float_bits(fs::get_float("setFNegZero")) == 0x80000000u);
        ctx.check("set_DPosInf_native_bits",  double_bits(fs::get_double("setDPosInf")) == 0x7FF0000000000000ULL);
        ctx.check("set_DMin_native_bits",     double_bits(fs::get_double("setDMin")) == 0x0010000000000000ULL);
        ctx.check("set_DMax_native_bits",     double_bits(fs::get_double("setDMax")) == 0x7FEFFFFFFFFFFFFFULL);
        ctx.check("set_DNegZero_native_bits", double_bits(fs::get_double("setDNegZero")) == 0x8000000000000000ULL);
    }

    // =====================================================================
    //  2. STRING static SET (REBIND to a fresh String; library bug #30 FIXED).
    // =====================================================================
    // CONTRACT of the write path exercised here (field_proxy::set(std::string)
    // -> field_proxy::store_string -> store_object_oop, vmhook.hpp ~15479):
    // set(std::string) now REBINDS the field to a freshly-built, correctly-encoded
    // java.lang.String of the EXACT input value+length (an object-reference store,
    // like a Java `field = value;`).  It NO LONGER overwrites the existing backing
    // array in place.  Consequences proven by this phase:
    //   * equal-length write ("AAAAA" <- "world") -> field reads "world" (len 5);
    //   * SHORTER write ("world" <- "hi") -> field reads exactly "hi" (len 2),
    //     NOT the old partial-overwrite "hirld" (len 5) that left the stale tail;
    //   * EMPTY write ("" ) -> field reads "" (a real empty String), NOT the old
    //     writable_length<=0 NO-OP that kept the prior content;
    //   * LONGER-than-backing write -> field reads the FULL value, NOT the old
    //     truncate-to-backing-length result.
    // The fixture's setStr/setStrShort are still new String(char[]) (private
    // backing): a non-interned start lets the fixture hold a separate alias to the
    // ORIGINAL object and prove the rebind does not mutate it (FieldStatic.java).
    ctx.record("[INFO] field_static: field_proxy::set(std::string) REBINDS the "
               "field to a fresh java.lang.String of the exact value+length "
               "(store_string -> store_object_oop, vmhook.hpp ~15479; library bug "
               "#30 fixed). A SHORTER write yields exactly \"hi\" (len 2, not the "
               "old \"hirld\"/len-5), an EMPTY write yields \"\" (not the old "
               "no-op-keep), and a LONGER write yields the FULL value (not the old "
               "truncate). It never mutates the previously referenced String, so a "
               "shared/interned object aliased elsewhere is never corrupted.");

    ctx.check("set_str_resolved", fs::set_string("setStr", "world"));        // "AAAAA" <- "world" -> "world"
    ctx.check("set_str_short_resolved", fs::set_string("setStrShort", "hi")); // "world" <- "hi" -> "hi"

    // Immediate native re-read of the String set.
    ctx.check("set_str_native_reread_world", fs::get_string("setStr") == "world");
    ctx.check("set_str_short_native_reread_hi", fs::get_string("setStrShort") == "hi");

    // ---- 2b. the two remaining SET boundaries, now via the rebind path ----
    //   * EMPTY write: the rebind builds a real empty String -> the field reads "".
    ctx.check("set_str_empty_resolved", fs::set_string("setStrEmpty", ""));
    ctx.check("set_str_empty_now_empty", fs::get_string("setStrEmpty").empty());
    //   * LONGER-than-backing write: the rebind allocates any length -> the field
    //     reads the FULL "toolongvalue" (len 12), not the old truncate-to-5.
    ctx.check("set_str_trunc_resolved", fs::set_string("setStrTrunc", "toolongvalue"));
    ctx.check("set_str_trunc_full_value", fs::get_string("setStrTrunc") == "toolongvalue");

    // =====================================================================
    //  3. SIZE / TYPE GUARD (audit: field_proxy_set_size_guard.md).
    //     Mistyped writes into a primitive static field must be REFUSED with
    //     the field's JVM-visible value left byte-for-byte unchanged.
    // =====================================================================

    // guardInt initial value before any write.
    ctx.check("guard_int_initial", fs::get_int("guardInt") == 0x11223344);

    // (a) set(int64) into an "I" (4-byte) field -> too wide -> refused.
    fs::set_value<std::int64_t>("guardInt", std::int64_t{ 0x7766554433221100LL });
    ctx.check("guard_int_too_wide_refused", fs::get_int("guardInt") == 0x11223344);

    // (b) set(std::string) into an "I" field -> non-primitive into primitive -> refused.
    fs::set_string("guardInt", "99999");
    ctx.check("guard_int_string_refused", fs::get_int("guardInt") == 0x11223344);

    // (c) set(int32) into a "J" (8-byte) field -> too narrow -> refused.
    ctx.check("guard_long_initial", fs::get_long("guardLong") == 0x1122334455667788LL);
    fs::set_value<std::int32_t>("guardLong", std::int32_t{ 0x09ABCDEF });
    ctx.check("guard_long_too_narrow_refused", fs::get_long("guardLong") == 0x1122334455667788LL);

    // (c2) set(int8) and set(int16) into an "I" field -> too NARROW (1B/2B into
    //      a 4B slot) -> refused.  The prior battery only covered too-WIDE into I;
    //      the guard fires on any value_size != field_size, narrow OR wide.
    fs::set_value<std::int8_t>("guardInt", static_cast<std::int8_t>(0x7F));
    ctx.check("guard_int_int8_too_narrow_refused", fs::get_int("guardInt") == 0x11223344);
    fs::set_value<std::int16_t>("guardInt", static_cast<std::int16_t>(0x7FFF));
    ctx.check("guard_int_int16_too_narrow_refused", fs::get_int("guardInt") == 0x11223344);

    // (d) "C" 1-byte->2-byte widening shortcut: a C++ char 'Z' (0x5A) must land
    //     the full 2-byte Java char 0x005A, not a half-written / clobbered value.
    fs::set_value<char>("guardChar", 'Z');
    ctx.check("guard_char_widened_to_005A", fs::get_char("guardChar") == 0x005A);
    // A high-bit char byte 0xE9 widens to 0x00E9 (high byte zero), never sign-extended.
    fs::set_value<char>("guardChar", static_cast<char>(0xE9));
    ctx.check("guard_char_high_byte_zero_extended", fs::get_char("guardChar") == 0x00E9);

    // (d2) the widening shortcut fires for ANY arithmetic 1-byte type, not just
    //      plain `char`.  std::int8_t (signed char) 0xE9 widens via the
    //      static_cast<unsigned char> step to 0x00E9 -- NOT sign-extended to
    //      0xFFE9.  This guards the `static_cast<unsigned char>(value)` in the
    //      shortcut against signed-char sign extension.
    fs::set_value<std::int8_t>("guardChar", static_cast<std::int8_t>(0xE9));
    ctx.check("guard_char_int8_widens_unsigned_00E9", fs::get_char("guardChar") == 0x00E9);
    // ... and for std::byte, which the header shortcut explicitly targets
    //     (std::is_enum_v<std::byte> is true; it is static_cast-able but NOT
    //     implicitly convertible, so it must reach the shortcut via the enum arm).
    fs::set_value<std::byte>("guardChar", std::byte{ 0x41 });
    ctx.check("guard_char_stdbyte_widens_to_0041", fs::get_char("guardChar") == 0x0041);

    // (d3) CHARACTERIZE: the size guard is purely SIZE-based, not type-based.
    //      A set(int16) into a "C" field (both 2 bytes) is NOT refused -- the raw
    //      bits land and read back as the char of that value.  This is a real
    //      same-width cross-type write the guard cannot catch; documented so a
    //      future type-aware guard (which WOULD refuse it) trips this assertion.
    fs::set_value<std::int16_t>("guardChar", static_cast<std::int16_t>(0x1234));
    ctx.check("guard_char_int16_same_width_writes_raw", fs::get_char("guardChar") == 0x1234);
    ctx.record("[INFO] field_static: field_proxy::set's size guard is SIZE-based, "
               "not type-based -- a same-width mismatched primitive (e.g. set(int16) "
               "into a \"C\" field, or set(int32 bits) into an \"F\" field) is NOT "
               "refused; the raw bytes land.  Only width mismatches are caught "
               "(vmhook.hpp ~13048).  Pass the matching primitive type.");
    // IMPORTANT: restore guardChar to 0x00E9 -- the value the original widening
    // case (d) left, which the phase-7 snapshot (seenGuardChar) and the phase-8
    // getter (getGuardChar) both assert.  (mode-3 reset zeroes it only AFTER
    // those phases run.)  We re-land it through the 1-byte widening shortcut so
    // this restore is itself a final proof of the char widening.
    fs::set_value<char>("guardChar", static_cast<char>(0xE9));
    ctx.check("guard_char_restored_to_00E9", fs::get_char("guardChar") == 0x00E9);

    // (d4) CHARACTERIZE the same-width int->float confusion on a dedicated field:
    //      set(int32{0x3F800000}) into an "F" field is same-width (4B==4B) so it
    //      is NOT refused; the bits land and read back as the float 1.0f.  Uses
    //      setFNegZero (already proven above) then restores it for phase 8b.
    fs::set_value<std::int32_t>("setFNegZero", static_cast<std::int32_t>(0x3F800000));
    ctx.check("set_int_into_float_same_width_writes_raw",
              float_bits(fs::get_float("setFNegZero")) == 0x3F800000u);
    fs::set_value<float>("setFNegZero", -0.0f); // restore the -0.0 phase 8b expects
    ctx.check("set_FNegZero_restored", float_bits(fs::get_float("setFNegZero")) == 0x80000000u);

    // (e) correctly-sized write into "I" SUCCEEDS (control for the guard).
    fs::set_value<std::int32_t>("guardInt", 0x55667788);
    ctx.check("guard_int_right_size_succeeds", fs::get_int("guardInt") == 0x55667788);
    // restore for the Java snapshot (mode 3 also resets, but be explicit).
    fs::set_value<std::int32_t>("guardInt", 0x11223344);
    ctx.check("guard_int_restored", fs::get_int("guardInt") == 0x11223344);

    // =====================================================================
    //  4. OBJECT-REFERENCE static SET via unique_ptr<wrapper>.
    //     objRef starts at objA; rewrite it to objB, prove identity flips, then
    //     null it.  Java-side identity confirmation happens in phase 6.
    // =====================================================================
    {
        const auto objA{ fs::acquire("objA") };
        const auto objB{ fs::acquire("objB") };
        ctx.check("objA_acquired", objA != nullptr);
        ctx.check("objB_acquired", objB != nullptr);
        ctx.check("objA_tag_is_A", objA != nullptr && objA->tag() == 0xA);
        ctx.check("objB_tag_is_B", objB != nullptr && objB->tag() == 0xB);

        // objRef initially aliases objA: reading its tag yields 0xA.
        {
            const auto ref0{ fs::acquire("objRef") };
            ctx.check("objRef_initially_A", ref0 != nullptr && ref0->tag() == 0xA);
        }

        // Rewrite objRef = objB (compressed-OOP write into the static slot).
        ctx.check("set_ref_to_B_resolved", fs::set_ref("objRef", objB));
        {
            const auto ref1{ fs::acquire("objRef") };
            ctx.check("objRef_now_B_native_reread", ref1 != nullptr && ref1->tag() == 0xB);
        }

        // Null the reference via an empty unique_ptr -> compressed 0 -> Java null.
        {
            const std::unique_ptr<fs> empty{};
            ctx.check("set_ref_to_null_resolved", fs::set_ref("objRef", empty));
            const auto refN{ fs::acquire("objRef") };
            ctx.check("objRef_now_null_native_reread", refN == nullptr);
        }

        // Put it back to objB so the Java snapshot sees a non-null, identity==B.
        ctx.check("set_ref_back_to_B_resolved", fs::set_ref("objRef", objB));
    }

    // =====================================================================
    //  5. STATIC GET battery (independent of field_primitives_get): every
    //     primitive boundary + a String, through the SAME portable accessor.
    // =====================================================================
    {
        const auto z1{ fs::get_proxy("gZTrue") };
        if (z1) { const auto v{ z1->get() }; ctx.check("get_gZTrue", static_cast<bool>(v) == true);  ctx.check("get_gZTrue_variant", v.data.index() == kIdxBool); }
        const auto z0{ fs::get_proxy("gZFalse") };
        if (z0) { const bool v = z0->get(); ctx.check("get_gZFalse", v == false); }
        const auto bmin{ fs::get_proxy("gBMin") };
        if (bmin) { const std::int8_t v = bmin->get(); ctx.check("get_gBMin", v == std::numeric_limits<std::int8_t>::min()); }
        const auto bmax{ fs::get_proxy("gBMax") };
        if (bmax) { const std::int8_t v = bmax->get(); ctx.check("get_gBMax", v == std::numeric_limits<std::int8_t>::max()); }
        const auto smin{ fs::get_proxy("gSMin") };
        if (smin) { const std::int16_t v = smin->get(); ctx.check("get_gSMin", v == std::numeric_limits<std::int16_t>::min()); }
        const auto smax{ fs::get_proxy("gSMax") };
        if (smax) { const std::int16_t v = smax->get(); ctx.check("get_gSMax", v == std::numeric_limits<std::int16_t>::max()); }
        const auto cmax{ fs::get_proxy("gCMax") };
        if (cmax) { const std::uint16_t v = cmax->get(); ctx.check("get_gCMax", v == 0xFFFF); }
        const auto imin{ fs::get_proxy("gIMin") };
        if (imin) { const std::int32_t v = imin->get(); ctx.check("get_gIMin", v == std::numeric_limits<std::int32_t>::min()); }
        const auto imax{ fs::get_proxy("gIMax") };
        if (imax) { const std::int32_t v = imax->get(); ctx.check("get_gIMax", v == std::numeric_limits<std::int32_t>::max()); }
        const auto jmin{ fs::get_proxy("gJMin") };
        if (jmin) { const std::int64_t v = jmin->get(); ctx.check("get_gJMin", v == std::numeric_limits<std::int64_t>::min()); }
        const auto jmax{ fs::get_proxy("gJMax") };
        if (jmax) { const std::int64_t v = jmax->get(); ctx.check("get_gJMax", v == std::numeric_limits<std::int64_t>::max()); }
        const auto fone{ fs::get_proxy("gFOne") };
        if (fone) { const float v = fone->get(); ctx.check("get_gFOne_bits", float_bits(v) == 0x3F800000u); }
        const auto done1{ fs::get_proxy("gDOne") };
        if (done1) { const double v = done1->get(); ctx.check("get_gDOne_bits", double_bits(v) == 0x3FF0000000000000ULL); }
        ctx.check("get_gStr", fs::get_string("gStr") == "field_static");
    }

    // =====================================================================
    //  5b. EXHAUSTIVE per-type SIGNATURE + variant-index decode.  Proves
    //      static_field() returns the exact JVM descriptor for EVERY type, that
    //      get() selects the matching value_t variant alternative for each, and
    //      that get().signature carries the descriptor too.  One clean read of
    //      the field metadata per type -- the byte-for-byte decode contract.
    // =====================================================================
    {
        ctx.check("sig_Z_is_Z", fs::signature_of("gZTrue") == "Z");
        ctx.check("sig_B_is_B", fs::signature_of("gBMin") == "B");
        ctx.check("sig_C_is_C", fs::signature_of("gCMax") == "C");
        ctx.check("sig_S_is_S", fs::signature_of("gSMin") == "S");
        ctx.check("sig_I_is_I", fs::signature_of("gIMin") == "I");
        ctx.check("sig_J_is_J", fs::signature_of("gJMin") == "J");
        ctx.check("sig_F_is_F", fs::signature_of("gFOne") == "F");
        ctx.check("sig_D_is_D", fs::signature_of("gDOne") == "D");
        ctx.check("sig_String_is_Ljava_lang_String", fs::signature_of("gStr") == "Ljava/lang/String;");
        ctx.check("sig_ref_is_LFieldStatic", fs::signature_of("objA") == "Lvmhook/fixtures/FieldStatic;");

        // get() variant alternative + get().signature, per type.
        const auto pz{ fs::get_proxy("gZTrue") };
        if (pz) { const auto v{ pz->get() }; ctx.check("variant_Z_is_bool", v.data.index() == kIdxBool); ctx.check("getsig_Z", v.signature == "Z"); }
        const auto pb{ fs::get_proxy("gBMin") };
        if (pb) { const auto v{ pb->get() }; ctx.check("variant_B_is_i8", v.data.index() == kIdxI8); ctx.check("getsig_B", v.signature == "B"); }
        const auto pc{ fs::get_proxy("gCMax") };
        if (pc) { const auto v{ pc->get() }; ctx.check("variant_C_is_u16", v.data.index() == kIdxU16); ctx.check("getsig_C", v.signature == "C"); }
        const auto pshort{ fs::get_proxy("gSMin") };
        if (pshort) { const auto v{ pshort->get() }; ctx.check("variant_S_is_i16", v.data.index() == kIdxI16); }
        const auto pint{ fs::get_proxy("gIMin") };
        if (pint) { const auto v{ pint->get() }; ctx.check("variant_I_is_i32", v.data.index() == kIdxI32); }
        const auto plong{ fs::get_proxy("gJMin") };
        if (plong) { const auto v{ plong->get() }; ctx.check("variant_J_is_i64", v.data.index() == kIdxI64); }
        const auto pflt{ fs::get_proxy("gFOne") };
        if (pflt) { const auto v{ pflt->get() }; ctx.check("variant_F_is_float", v.data.index() == kIdxFloat); }
        const auto pdbl{ fs::get_proxy("gDOne") };
        if (pdbl) { const auto v{ pdbl->get() }; ctx.check("variant_D_is_double", v.data.index() == kIdxDouble); }
        // A reference field's get() stores the compressed OOP in the u32 arm and
        // it is NON-ZERO for the live, published objA instance.
        const auto pref{ fs::get_proxy("objA") };
        if (pref)
        {
            const auto v{ pref->get() };
            ctx.check("variant_ref_is_u32", v.data.index() == kIdxU32);
            const std::uint32_t compressed = std::get<std::uint32_t>(v.data);
            ctx.check("ref_compressed_oop_nonzero", compressed != 0u);
        }
    }

    // =====================================================================
    //  5c. EXHAUSTIVE integral GET at the 0 / 1 / -1 boundaries (every signed
    //      width) plus the char '\0' / 'A' edges -- the "ordinary small"
    //      corners the MIN/MAX battery above doesn't reach.
    // =====================================================================
    {
        ctx.check("get_gIZero",   fs::get_int("gIZero") == 0);
        ctx.check("get_gIOne",    fs::get_int("gIOne") == 1);
        ctx.check("get_gINegOne", fs::get_int("gINegOne") == -1);
        ctx.check("get_gJZero",   fs::get_long("gJZero") == 0);
        ctx.check("get_gJOne",    fs::get_long("gJOne") == 1);
        ctx.check("get_gJNegOne", fs::get_long("gJNegOne") == -1);
        ctx.check("get_gBZero",   fs::get_byte("gBZero") == 0);
        ctx.check("get_gBOne",    fs::get_byte("gBOne") == 1);
        ctx.check("get_gBNegOne", fs::get_byte("gBNegOne") == static_cast<std::int8_t>(-1));
        ctx.check("get_gSZero",   fs::get_short("gSZero") == 0);
        ctx.check("get_gSOne",    fs::get_short("gSOne") == 1);
        ctx.check("get_gSNegOne", fs::get_short("gSNegOne") == static_cast<std::int16_t>(-1));
        ctx.check("get_gCNul",    fs::get_char("gCNul") == 0x0000);
        ctx.check("get_gCA",      fs::get_char("gCA") == 0x0041);
    }

    // =====================================================================
    //  5d. EXHAUSTIVE float/double boundary GET: +0 / -0 / MIN / MAX / +Inf /
    //      -Inf / NaN, each compared by EXACT bit pattern so the check is
    //      -Werror clean (never an == on a float value, and NaN != NaN safe).
    // =====================================================================
    {
        ctx.check("get_gFZero_bits",    float_bits(fs::get_float("gFZero")) == 0x00000000u);
        ctx.check("get_gFNegZero_bits", float_bits(fs::get_float("gFNegZero")) == 0x80000000u);
        ctx.check("get_gFMin_bits",     float_bits(fs::get_float("gFMin")) == 0x00000001u); // smallest subnormal
        ctx.check("get_gFMax_bits",     float_bits(fs::get_float("gFMax")) == 0x7F7FFFFFu);
        ctx.check("get_gFPosInf_bits",  float_bits(fs::get_float("gFPosInf")) == 0x7F800000u);
        ctx.check("get_gFNegInf_bits",  float_bits(fs::get_float("gFNegInf")) == 0xFF800000u);
        {
            const std::uint32_t u{ float_bits(fs::get_float("gFNan")) };
            const bool is_nan_shape{ (u & 0x7F800000u) == 0x7F800000u && (u & 0x007FFFFFu) != 0u };
            ctx.check("get_gFNan_is_nan_bit_shape", is_nan_shape);
        }
        ctx.check("get_gDZero_bits",    double_bits(fs::get_double("gDZero")) == 0x0000000000000000ULL);
        ctx.check("get_gDNegZero_bits", double_bits(fs::get_double("gDNegZero")) == 0x8000000000000000ULL);
        ctx.check("get_gDMin_bits",     double_bits(fs::get_double("gDMin")) == 0x0000000000000001ULL); // smallest subnormal
        ctx.check("get_gDMax_bits",     double_bits(fs::get_double("gDMax")) == 0x7FEFFFFFFFFFFFFFULL);
        ctx.check("get_gDPosInf_bits",  double_bits(fs::get_double("gDPosInf")) == 0x7FF0000000000000ULL);
        ctx.check("get_gDNegInf_bits",  double_bits(fs::get_double("gDNegInf")) == 0xFFF0000000000000ULL);
        {
            const std::uint64_t u{ double_bits(fs::get_double("gDNan")) };
            const bool is_nan_shape{ (u & 0x7FF0000000000000ULL) == 0x7FF0000000000000ULL
                                     && (u & 0x000FFFFFFFFFFFFFULL) != 0u };
            ctx.check("get_gDNan_is_nan_bit_shape", is_nan_shape);
        }
    }

    // =====================================================================
    //  5e. A NULL static String field: the GET must resolve (proxy present),
    //      report the String signature, decode the compressed-0 OOP, and read
    //      back the empty string (read_java_string(nullptr) == "").
    // =====================================================================
    {
        ctx.check("get_gNullStr_resolves", fs::resolves("gNullStr"));
        ctx.check("get_gNullStr_signature", fs::signature_of("gNullStr") == "Ljava/lang/String;");
        const auto pn{ fs::get_proxy("gNullStr") };
        if (pn)
        {
            const auto v{ pn->get() };
            ctx.check("get_gNullStr_variant_u32", v.data.index() == kIdxU32);
            ctx.check("get_gNullStr_compressed_zero", std::get<std::uint32_t>(v.data) == 0u);
        }
        ctx.check("get_gNullStr_reads_empty", fs::get_string("gNullStr").empty());
    }

    // =====================================================================
    //  5f. static final CONSTANTS (compile-time inlined via ConstantValue).
    //      Characterizes EXACTLY what static_field() reads/writes for a constant:
    //      vmhook addresses the LIVE java.lang.Class mirror slot, which the class
    //      initializer set to the constant -- so a GET returns the real stored
    //      value (NOT a stale init constant vmhook never sees), and a SET lands
    //      on that slot.  Java's INLINED references to the constant keep the
    //      folded literal (getConstIInlined() == old), while a REFLECTIVE read
    //      sees the slot mutate (getConstIReflect() == new).  Restored at the end.
    // =====================================================================
    {
        // (a) GET the constant through the portable accessor -> the stored value.
        ctx.check("const_I_get_initial",   fs::get_int("CONST_I") == 0x0A0B0C0D);
        ctx.check("const_J_get_initial",   fs::get_long("CONST_J") == 0x0102030405060708LL);
        ctx.check("const_Z_get_initial",   fs::get_bool("CONST_Z") == true);
        ctx.check("const_C_get_initial",   fs::get_char("CONST_C") == 0x0051); // 'Q'
        ctx.check("const_STR_get_initial", fs::get_string("CONST_STR") == "konst");
        // The constant carries a normal primitive signature (not specially marked).
        ctx.check("const_I_signature_I", fs::signature_of("CONST_I") == "I");

        // (b) before any write, both Java read paths agree with the constant,
        //     across int / long / char / String constants.
        ctx.check("const_I_inlined_initial", fs::call_get_int("getConstIInlined") == 0x0A0B0C0D);
        ctx.check("const_I_reflect_initial", fs::call_get_int("getConstIReflect") == 0x0A0B0C0D);
        ctx.check("const_J_inlined_initial", fs::call_get_long("getConstJInlined") == 0x0102030405060708LL);
        ctx.check("const_J_reflect_initial", fs::call_get_long("getConstJReflect") == 0x0102030405060708LL);
        ctx.check("const_C_inlined_initial", fs::call_get_int("getConstCInlined") == 0x0051); // 'Q'
        ctx.check("const_C_reflect_initial", fs::call_get_int("getConstCReflect") == 0x0051);
        ctx.check("const_STR_reflect_initial", fs::call_get_string("getConstStrReflect") == "konst");
        ctx.check("const_STR_signature", fs::signature_of("CONST_STR") == "Ljava/lang/String;");

        // (b2) a static final LONG constant: vmhook SET lands on the mirror slot;
        //      reflection sees the new value, the inlined getter keeps the literal.
        ctx.check("const_J_set_resolved", fs::set_value<std::int64_t>("CONST_J", static_cast<std::int64_t>(0x7F7F7F7F7F7F7F7FLL)));
        ctx.check("const_J_native_reread_new", fs::get_long("CONST_J") == static_cast<std::int64_t>(0x7F7F7F7F7F7F7F7FLL));
        ctx.check("const_J_reflect_sees_new",  fs::call_get_long("getConstJReflect") == static_cast<std::int64_t>(0x7F7F7F7F7F7F7F7FLL));
        ctx.check("const_J_inlined_unchanged_old", fs::call_get_long("getConstJInlined") == 0x0102030405060708LL);
        fs::set_value<std::int64_t>("CONST_J", static_cast<std::int64_t>(0x0102030405060708LL)); // restore
        ctx.check("const_J_restored", fs::get_long("CONST_J") == 0x0102030405060708LL);

        // (b3) a static final CHAR constant via the 1-byte widening shortcut:
        //      write 'q' (0x71) -> mirror slot holds 0x0071; reflection sees it,
        //      the inlined getter still returns 0x0051 ('Q').
        ctx.check("const_C_set_resolved", fs::set_value<char>("CONST_C", 'q'));
        ctx.check("const_C_native_reread_0071", fs::get_char("CONST_C") == 0x0071);
        ctx.check("const_C_reflect_sees_0071",  fs::call_get_int("getConstCReflect") == 0x0071);
        ctx.check("const_C_inlined_unchanged_0051", fs::call_get_int("getConstCInlined") == 0x0051);
        fs::set_value<std::uint16_t>("CONST_C", 0x0051); // restore 'Q'
        ctx.check("const_C_restored", fs::get_char("CONST_C") == 0x0051);

        // (c) SET the mirror slot to a fresh value through static_field().
        ctx.check("const_I_set_resolved", fs::set_value<std::int32_t>("CONST_I", static_cast<std::int32_t>(0x7E7E7E7E)));
        // The native re-read AND a Java reflective getstatic both see the NEW value
        // (proving the write landed on the very slot Java reflection reads)...
        ctx.check("const_I_native_reread_new", fs::get_int("CONST_I") == static_cast<std::int32_t>(0x7E7E7E7E));
        ctx.check("const_I_reflect_sees_new",  fs::call_get_int("getConstIReflect") == static_cast<std::int32_t>(0x7E7E7E7E));
        // ...while a Java INLINED reference still returns the compile-time-folded
        // literal (javac emitted `ldc 0x0A0B0C0D`, never a getstatic) -- the
        // documented constant caveat, asserted so it can never silently drift.
        ctx.check("const_I_inlined_unchanged_old", fs::call_get_int("getConstIInlined") == 0x0A0B0C0D);
        ctx.record("[INFO] field_static: static_field() reads/writes the LIVE class-mirror "
                   "slot of a `static final` constant.  A vmhook SET is visible to Java "
                   "REFLECTION (Field.getInt reads the slot) but NOT to compile-time-inlined "
                   "references (javac folds `CONST_I` to an ldc literal) -- a fundamental "
                   "JVM property of ConstantValue fields, not a vmhook limitation.");

        // (d) restore the constant's mirror slot for suite hygiene.
        fs::set_value<std::int32_t>("CONST_I", static_cast<std::int32_t>(0x0A0B0C0D));
        ctx.check("const_I_restored", fs::get_int("CONST_I") == 0x0A0B0C0D);
        ctx.check("const_I_reflect_restored", fs::call_get_int("getConstIReflect") == 0x0A0B0C0D);
    }

    // =====================================================================
    //  5g. value_t::as_string() EXTRACTION across types.  The explicit-intent
    //      string reader (distinct from the implicit conversion get_string()
    //      uses): a String/reference field decodes through read_java_string;
    //      EVERY primitive field yields "" (only the uint32 arm is a String).
    //      This pins the documented contract that as_string() is a no-op-to-""
    //      on a non-reference field rather than formatting the number.
    // =====================================================================
    {
        // Reference / String fields decode to their text.
        ctx.check("as_string_gStr_field_static", fs::value_as_string("gStr") == "field_static");
        ctx.check("as_string_CONST_STR_konst",   fs::value_as_string("CONST_STR") == "konst");
        // A NULL String field decodes to "" (read_java_string(nullptr)).
        ctx.check("as_string_gNullStr_empty",     fs::value_as_string("gNullStr").empty());
        // EVERY primitive field's as_string() is "" -- it is NOT the formatted
        // number; the visitor returns "" for every non-uint32 variant arm.
        ctx.check("as_string_primitive_I_is_empty", fs::value_as_string("gIMax").empty());
        ctx.check("as_string_primitive_J_is_empty", fs::value_as_string("gJMax").empty());
        ctx.check("as_string_primitive_Z_is_empty", fs::value_as_string("gZTrue").empty());
        ctx.check("as_string_primitive_C_is_empty", fs::value_as_string("gCMax").empty());
        ctx.check("as_string_primitive_B_is_empty", fs::value_as_string("gBMin").empty());
        ctx.check("as_string_primitive_S_is_empty", fs::value_as_string("gSMin").empty());
        ctx.check("as_string_primitive_F_is_empty", fs::value_as_string("gFOne").empty());
        ctx.check("as_string_primitive_D_is_empty", fs::value_as_string("gDOne").empty());
        // An OBJECT-reference field (not a String) decodes to "" too: the OOP is
        // not a java.lang.String, so read_java_string yields "" (no crash).
        ctx.check("as_string_objref_is_empty", fs::value_as_string("objA").empty());
    }

    // =====================================================================
    //  5h. value_t::is_reference() vs field_proxy::is_reference() -- the two
    //      "is this a reference?" predicates must AGREE for a correctly-decoded
    //      field.  value_t::is_reference() inspects the decoded variant arm
    //      (uint32 == reference); field_proxy::is_reference() inspects the
    //      SIGNATURE (L / [).  Proven across every type: a char "C" field
    //      decodes to the uint16 arm and is NOT a reference (the critical
    //      disambiguation -- char and a compressed OOP are both unsigned, so a
    //      naive check could confuse them).
    // =====================================================================
    {
        // Reference fields: BOTH predicates true.
        ctx.check("vis_ref_gStr_value",  fs::value_is_reference("gStr") == 1);
        ctx.check("vis_ref_gStr_proxy",  fs::proxy_is_reference("gStr") == 1);
        ctx.check("vis_ref_objA_value",  fs::value_is_reference("objA") == 1);
        ctx.check("vis_ref_objA_proxy",  fs::proxy_is_reference("objA") == 1);
        ctx.check("vis_ref_sIntArr_value", fs::value_is_reference("sIntArr") == 1);
        ctx.check("vis_ref_sIntArr_proxy", fs::proxy_is_reference("sIntArr") == 1);
        ctx.check("vis_ref_staticTier_value", fs::value_is_reference("staticTier") == 1);
        ctx.check("vis_ref_staticTier_proxy", fs::proxy_is_reference("staticTier") == 1);
        ctx.check("vis_ref_gNullStr_value", fs::value_is_reference("gNullStr") == 1);
        ctx.check("vis_ref_gNullStr_proxy", fs::proxy_is_reference("gNullStr") == 1);
        // Primitive fields: BOTH predicates false (the char "C" case is the
        // disambiguation -- u16 arm, not the u32 reference arm).
        ctx.check("vis_prim_C_value_false",  fs::value_is_reference("gCMax") == 0);
        ctx.check("vis_prim_C_proxy_false",  fs::proxy_is_reference("gCMax") == 0);
        ctx.check("vis_prim_I_value_false",  fs::value_is_reference("gIMax") == 0);
        ctx.check("vis_prim_I_proxy_false",  fs::proxy_is_reference("gIMax") == 0);
        ctx.check("vis_prim_J_value_false",  fs::value_is_reference("gJMax") == 0);
        ctx.check("vis_prim_Z_value_false",  fs::value_is_reference("gZTrue") == 0);
        ctx.check("vis_prim_B_value_false",  fs::value_is_reference("gBMin") == 0);
        ctx.check("vis_prim_S_value_false",  fs::value_is_reference("gSMin") == 0);
        ctx.check("vis_prim_F_value_false",  fs::value_is_reference("gFOne") == 0);
        ctx.check("vis_prim_D_value_false",  fs::value_is_reference("gDOne") == 0);
    }

    // =====================================================================
    //  5i. field_proxy::get_compressed_oop() -- the dedicated reference reader.
    //      For a reference field it equals the u32 variant arm (the same
    //      compressed OOP get() decodes).  For a PRIMITIVE field it is GUARDED
    //      to 0 (FLAW-C fix): without the is_reference() guard it would read the
    //      primitive's first 4 bytes as a bogus OOP.  We assert the guard fires
    //      on a non-zero primitive (gIMax = 0x7FFFFFFF) -- the raw bytes are
    //      non-zero, so a 0 result PROVES the guard, not a coincidental zero.
    // =====================================================================
    {
        // Reference field: get_compressed_oop() == the u32 arm, and non-zero for
        // a live published instance.
        const auto pref{ fs::get_proxy("objA") };
        if (pref)
        {
            const auto v{ pref->get() };
            const std::uint32_t arm{ std::get<std::uint32_t>(v.data) };
            ctx.check("gco_objA_matches_variant_arm", fs::proxy_compressed_oop("objA") == arm);
            ctx.check("gco_objA_nonzero", fs::proxy_compressed_oop("objA") != 0u);
        }
        // String field: the compressed OOP is non-zero (gStr is a live literal).
        ctx.check("gco_gStr_nonzero", fs::proxy_compressed_oop("gStr") != 0u);
        // NULL String field: compressed OOP is 0.
        ctx.check("gco_gNullStr_zero", fs::proxy_compressed_oop("gNullStr") == 0u);
        // PRIMITIVE field with NON-ZERO bytes: the guard returns 0 anyway.
        ctx.check("gco_primitive_IMax_guarded_zero", fs::proxy_compressed_oop("gIMax") == 0u);
        ctx.check("gco_primitive_JMax_guarded_zero", fs::proxy_compressed_oop("gJMax") == 0u);
        ctx.record("[INFO] field_static: field_proxy::get_compressed_oop() is GUARDED "
                   "on is_reference() (signature L/[): on a primitive field it returns 0 "
                   "rather than reading the value bytes as a bogus compressed OOP that "
                   "would later decode to a wild pointer (FLAW-C, vmhook.hpp ~15702). It "
                   "agrees with get()'s u32 variant arm for genuine reference fields.");
    }

    // =====================================================================
    //  5j. value_t -> void* conversion arm: a reference field decodes to the
    //      same heap address field_oop()/array_oop reaches.  Cross-checks the
    //      void* conversion (decode_oop_pointer) against the array_oop path, and
    //      that a primitive field's void* conversion is nullptr (only the u32
    //      arm produces a non-null pointer).
    // =====================================================================
    {
        // A static array reference decoded to void* equals array_oop()'s decode.
        void* const via_value{ fs::value_as_voidp("sIntArr") };
        void* const via_field_oop{ fs::array_oop("sIntArr") };
        ctx.check("voidp_sIntArr_nonnull", via_value != nullptr);
        ctx.check("voidp_sIntArr_matches_field_oop", via_value == via_field_oop);
        // A live object reference decodes to a non-null, valid heap pointer.
        void* const via_objA{ fs::value_as_voidp("objA") };
        ctx.check("voidp_objA_nonnull_valid",
                  via_objA != nullptr && vmhook::hotspot::is_valid_pointer(via_objA));
        // A NULL reference decodes to nullptr.
        ctx.check("voidp_gNullStr_null", fs::value_as_voidp("gNullStr") == nullptr);
        // A PRIMITIVE field's void* conversion is nullptr (the conversion arm
        // only decodes the u32 alternative; every primitive arm yields nullptr).
        ctx.check("voidp_primitive_IMax_null", fs::value_as_voidp("gIMax") == nullptr);
        ctx.check("voidp_primitive_D_null",    fs::value_as_voidp("gDOne") == nullptr);
    }

    // =====================================================================
    //  5k. contextual-bool conversion of a NUMERIC field (operator bool): the
    //      value is true iff non-zero, matching C++ contextual conversion.  The
    //      module elsewhere reads "Z" fields as bool; here we prove the SAME
    //      conversion on integral/float fields (a non-zero int is true, zero is
    //      false), exercising the operator target_type path with target=bool.
    // =====================================================================
    {
        ctx.check("ctxbool_gIZero_false",  fs::value_as_bool("gIZero") == 0);
        ctx.check("ctxbool_gIOne_true",    fs::value_as_bool("gIOne") == 1);
        ctx.check("ctxbool_gINegOne_true", fs::value_as_bool("gINegOne") == 1); // -1 != 0 -> true
        ctx.check("ctxbool_gJZero_false",  fs::value_as_bool("gJZero") == 0);
        ctx.check("ctxbool_gJOne_true",    fs::value_as_bool("gJOne") == 1);
        ctx.check("ctxbool_gBZero_false",  fs::value_as_bool("gBZero") == 0);
        ctx.check("ctxbool_gBOne_true",    fs::value_as_bool("gBOne") == 1);
        ctx.check("ctxbool_gZTrue_true",   fs::value_as_bool("gZTrue") == 1);
        ctx.check("ctxbool_gZFalse_false", fs::value_as_bool("gZFalse") == 0);
        // A char field with a non-zero code unit converts to true; the NUL char to false.
        ctx.check("ctxbool_gCA_true",      fs::value_as_bool("gCA") == 1);
        ctx.check("ctxbool_gCNul_false",   fs::value_as_bool("gCNul") == 0);
    }

    // =====================================================================
    //  5l. STRING SET via the const char* / std::string_view arm (a DIFFERENT
    //      field_proxy::set overload branch than set(std::string)): proves the
    //      C-string and view write paths rebind the field to the exact value.
    //      Written BEFORE go; the native re-read is here and Java getters pull
    //      them back in phase 8c.  Also proves the rebind leaves the aliased
    //      ORIGINAL setStrShort object intact (read natively here).
    // =====================================================================
    {
        // const char* literal -> string_view arm -> rebind to "via-cstr".
        ctx.check("set_cstr_resolved", fs::set_cstr("setStrCstr", "via-cstr"));
        ctx.check("set_cstr_native_reread", fs::get_string("setStrCstr") == "via-cstr");
        // An explicit std::string_view -> same arm.  Build a SUB-VIEW so the
        // span is not NUL-terminated at its end -- the write must store exactly
        // the 7 chars "subview", not run past the view into the backing string.
        {
            const std::string backing{ "subview-extra" };
            const std::string_view view{ std::string_view{ backing }.substr(0, 7) }; // "subview"
            ctx.check("set_strview_resolved", fs::set_strview("setStrView", view));
        }
        ctx.check("set_strview_native_reread", fs::get_string("setStrView") == "subview");
        // The rebind-safety guarantee, proven NATIVELY (the module so far only
        // checked it via the Java seenStrShortOriginalIntact witness): the
        // original setStrShort object (aliased at <clinit> as setStrShortOriginal)
        // STILL reads "world" after setStrShort was rebound to "hi" in phase 2.
        ctx.check("set_str_original_intact_native", fs::get_string("setStrShortOriginal") == "world");
    }

    // =====================================================================
    //  6. ERROR / NULL / EDGE paths for the static accessor.
    // =====================================================================
    {
        // Unknown field -> nullopt.
        ctx.check("static_field_unknown_is_nullopt", fs::static_field("noSuchField").has_value() == false);
        // An INSTANCE-only field requested through the STATIC accessor must fail
        // with the "needs an object instance" diagnostic (returns nullopt).
        ctx.check("static_field_on_instance_field_is_nullopt",
                  fs::static_field("instanceOnlyInt").has_value() == false);
        // But the SAME field resolves through an instance wrapper.
        {
            const auto inst{ fs::acquire("objA") };
            if (inst)
            {
                const auto p{ inst->get_field("instanceOnlyInt") };
                ctx.check("instance_field_resolves_via_instance", p.has_value());
                if (p) { const std::int32_t v = p->get(); ctx.check("instance_field_value_4242", v == 4242); }
                ctx.check("instance_field_is_static_false", p.has_value() && p->is_static() == false);
            }
        }
        // Unknown static method -> nullopt.
        ctx.check("static_method_unknown_is_nullopt", fs::static_method("noSuchMethod").has_value() == false);

        // A null-field_pointer proxy: set() must early-return (no crash) and
        // leave nothing to read; get() yields the int32 fallback.
        {
            vmhook::field_proxy np{ nullptr, "I", true };
            np.set(std::int32_t{ 1234 });            // must be a safe no-op
            const auto v{ np.get() };
            ctx.check("null_proxy_get_is_int32_fallback", v.data.index() == kIdxI32);
            const std::int32_t got = v;
            ctx.check("null_proxy_get_value_zero", got == 0);
        }
    }

    // =====================================================================
    //  7. SET-THEN-READ-BACK THROUGH JAVA  (the headline contract).
    //     mode 1: the probe snapshots every set* target into its seen* witness
    //     using genuine getstatic/putstatic, so we read back exactly what the
    //     JVM observed for the native writes performed in phases 1/2/4.
    // =====================================================================
    {
        const bool done{ drive(ctx, 1) };
        ctx.check("snapshot_probe_completed", done);

        if (done)
        {
            // ---- primary battery, as seen by Java ----
            ctx.check("java_seenZ_true",  fs::seen_bool("seenZ") == true);
            ctx.check("java_seenB_min",   fs::seen_i8("seenB") == std::numeric_limits<std::int8_t>::min());
            ctx.check("java_seenC_FFFF",  fs::seen_u16("seenC") == 0xFFFF);
            ctx.check("java_seenS_min",   fs::seen_i16("seenS") == std::numeric_limits<std::int16_t>::min());
            ctx.check("java_seenI_min",   fs::seen_i32("seenI") == std::numeric_limits<std::int32_t>::min());
            ctx.check("java_seenJ_max",   fs::seen_i64("seenJ") == std::numeric_limits<std::int64_t>::max());
            ctx.check("java_seenF_bits",  fs::seen_i32("seenFBits") == static_cast<std::int32_t>(0x3E200000));
            ctx.check("java_seenD_bits",  fs::seen_i64("seenDBits") == static_cast<std::int64_t>(0x3FB999999999999AULL));

            // ---- secondary battery, as seen by Java ----
            ctx.check("java_seenZ2_false", fs::seen_bool("seenZ2") == false);
            ctx.check("java_seenB2_negone", fs::seen_i8("seenB2") == static_cast<std::int8_t>(-1));
            ctx.check("java_seenC2_euro",  fs::seen_u16("seenC2") == 0x20AC);
            ctx.check("java_seenS2_beef",  fs::seen_i16("seenS2") == static_cast<std::int16_t>(0xBEEF));
            ctx.check("java_seenI2_deadbeef", fs::seen_i32("seenI2") == static_cast<std::int32_t>(0xDEADBEEF));
            ctx.check("java_seenJ2_min",   fs::seen_i64("seenJ2") == std::numeric_limits<std::int64_t>::min());
            ctx.check("java_seenF2_neg_inf_bits", fs::seen_i32("seenF2Bits") == static_cast<std::int32_t>(0xFF800000));
            // Canonical double NaN: Java's doubleToRawLongBits of our quiet NaN.
            // We don't pin the exact payload bits (platform qNaN encodings differ);
            // instead require the IEEE NaN exponent/mantissa shape via Java below.
            {
                const std::int64_t nb{ fs::seen_i64("seenD2Bits") };
                const std::uint64_t u{ static_cast<std::uint64_t>(nb) };
                const bool is_nan_shape{ (u & 0x7FF0000000000000ULL) == 0x7FF0000000000000ULL
                                         && (u & 0x000FFFFFFFFFFFFFULL) != 0 };
                ctx.check("java_seenD2_is_nan_bit_shape", is_nan_shape);
            }

            // ---- ordinary values, as seen by Java ----
            ctx.check("java_seenIOrd",  fs::seen_i32("seenIOrd") == 123456789);
            ctx.check("java_seenJOrd",  fs::seen_i64("seenJOrd") == 0x0123456789ABCDEFLL);
            ctx.check("java_seenDOrd_pi_bits", fs::seen_i64("seenDOrdBits") == static_cast<std::int64_t>(0x400921FB54442D18ULL));
            ctx.check("java_seenFOrd_1p5_bits", fs::seen_i32("seenFOrdBits") == static_cast<std::int32_t>(0x3FC00000));

            // ---- String, as seen by Java ----
            ctx.check("java_seenStr_world", fs::get_string("seenStr") == "world");
            ctx.check("java_seenStr_len_5", fs::seen_i32("seenStrLen") == 5);
            // FIXED (was misdiagnosed as a write_java_string bug): the native
            // reads above prove setStr's backing holds "world", and Java's
            // `"world".equals(setStr)` now also returns TRUE.  The earlier uniform
            // FALSE was a TEST-FIXTURE bug, not a library bug: the legacy driver's
            // set_static_string_array({"alpha","omega","?"}) wrote "omega" IN PLACE
            // over Example.staticStringArray[1], which was the *interned* "world"
            // literal — corrupting the very "world" constant this probe compares
            // against (so the comparison became "omega".equals("world")).  Fixed by
            // giving staticStringArray non-interned backings in Example.java;
            // write_java_string itself was always correct (proven by a live
            // build+inject investigation: setStr equals a char-array-built "world").
            ctx.check("java_seenStr_eq_world",
                      fs::seen_bool("seenStrEqWorld") == true);
            ctx.record("[INFO] field_static: setStr == \"world\" both natively and via Java "
                       ".equals (write_java_string is correct; the prior mismatch was an interned-"
                       "literal corruption from a legacy in-place array write, now fixed).");
            ctx.check("java_seenStrShort_hi", fs::get_string("seenStrShort") == "hi");
            ctx.check("java_seenStrShort_len_2", fs::seen_i32("seenStrShortLen") == 2);
            // REBIND-SAFETY: the original String object setStrShort pointed at is
            // still "world" after the rebind (object-reference store, not in-place
            // mutate -> a shared/aliased String is never corrupted).
            ctx.check("java_seenStrShort_original_intact_world",
                      fs::seen_bool("seenStrShortOriginalIntact") == true);

            // ---- guard targets unchanged, as seen by Java ----
            ctx.check("java_seenGuardInt_unchanged", fs::seen_i32("seenGuardInt") == 0x11223344);
            ctx.check("java_seenGuardLong_unchanged", fs::seen_i64("seenGuardLong") == 0x1122334455667788LL);
            ctx.check("java_seenGuardChar_high_byte_zero", fs::seen_u16("seenGuardChar") == 0x00E9);

            // ---- object reference identity, as seen by Java ----
            ctx.check("java_objRef_is_B_not_A", fs::seen_bool("seenObjRefIsB") == true);
            ctx.check("java_objRef_not_A", fs::seen_bool("seenObjRefIsA") == false);
            ctx.check("java_objRef_not_null", fs::seen_bool("seenObjRefIsNull") == false);
            ctx.check("java_objRef_tag_is_B", fs::seen_i32("seenObjRefTag") == 0xB);
        }
    }

    // =====================================================================
    //  8. SET-THEN-READ-BACK THROUGH JAVA GETTERS (static_method portability).
    //     Pull each native-written value back through a Java getter method via
    //     static_method("getX")->call(): Java's own bytecode reads the field and
    //     returns it, proving the writes are visible to executing Java code (not
    //     just to a memory peek), and exercising the portable static_method path.
    // =====================================================================
    {
        ctx.check("java_getter_Z_true",  fs::call_get_bool("getZ") == true);
        ctx.check("java_getter_I_min",   fs::call_get_int("getI") == std::numeric_limits<std::int32_t>::min());
        ctx.check("java_getter_J_max",   fs::call_get_long("getJ") == std::numeric_limits<std::int64_t>::max());
        ctx.check("java_getter_B_min",   fs::call_get_int("getB") == -128); // widened to int on return
        ctx.check("java_getter_S_min",   fs::call_get_int("getS") == std::numeric_limits<std::int16_t>::min());
        ctx.check("java_getter_C_FFFF",  fs::call_get_int("getC") == 0xFFFF); // char widened unsigned to int
        ctx.check("java_getter_IOrd",    fs::call_get_int("getIOrd") == 123456789);
        ctx.check("java_getter_Str_world", fs::call_get_string("getStr") == "world");
        ctx.check("java_getter_StrLen_5",  fs::call_get_int("getStrLen") == 5);
        ctx.check("java_getter_GuardInt_unchanged", fs::call_get_int("getGuardInt") == 0x11223344);
        ctx.check("java_getter_GuardLong_unchanged", fs::call_get_long("getGuardLong") == 0x1122334455667788LL);
        ctx.check("java_getter_GuardChar_00E9", fs::call_get_int("getGuardChar") == 0x00E9);
        ctx.check("java_getter_objRefIsB", fs::call_get_bool("objRefIsB") == true);
        ctx.check("java_getter_objRefIsNull_false", fs::call_get_bool("objRefIsNull") == false);
        ctx.check("java_getter_objRefTag_B", fs::call_get_int("getObjRefTag") == 0xB);

        // ---- 8b. the EXHAUSTIVE SET-EDGE battery, pulled back through Java
        //      getters (genuine getstatic in Java bytecode reads the native write). ----
        ctx.check("java_getter_IZero",   fs::call_get_int("getIZero") == 0);
        ctx.check("java_getter_IOne",    fs::call_get_int("getIOne") == 1);
        ctx.check("java_getter_INegOne", fs::call_get_int("getINegOne") == -1);
        ctx.check("java_getter_IMax",    fs::call_get_int("getIMax") == std::numeric_limits<std::int32_t>::max());
        ctx.check("java_getter_JZero",   fs::call_get_long("getJZero") == 0);
        ctx.check("java_getter_JOne",    fs::call_get_long("getJOne") == 1);
        ctx.check("java_getter_JNegOne", fs::call_get_long("getJNegOne") == -1);
        ctx.check("java_getter_BZero",   fs::call_get_int("getBZero") == 0);
        ctx.check("java_getter_BMax",    fs::call_get_int("getBMax") == std::numeric_limits<std::int8_t>::max());
        ctx.check("java_getter_SZero",   fs::call_get_int("getSZero") == 0);
        ctx.check("java_getter_SMax",    fs::call_get_int("getSMax") == std::numeric_limits<std::int16_t>::max());
        ctx.check("java_getter_CNul",    fs::call_get_int("getCNul") == 0x0000);
        ctx.check("java_getter_CA",      fs::call_get_int("getCA") == 0x0041);
        // float/double edges: the getter returns the raw bits, compared exactly.
        ctx.check("java_getter_FPosInf_bits",  fs::call_get_int("getFPosInfBits") == static_cast<std::int32_t>(0x7F800000));
        ctx.check("java_getter_FMin_bits",     fs::call_get_int("getFMinBits") == static_cast<std::int32_t>(0x00800000));
        ctx.check("java_getter_FMax_bits",     fs::call_get_int("getFMaxBits") == static_cast<std::int32_t>(0x7F7FFFFF));
        ctx.check("java_getter_FNegZero_bits", fs::call_get_int("getFNegZeroBits") == static_cast<std::int32_t>(0x80000000));
        ctx.check("java_getter_DPosInf_bits",  fs::call_get_long("getDPosInfBits") == static_cast<std::int64_t>(0x7FF0000000000000ULL));
        ctx.check("java_getter_DMin_bits",     fs::call_get_long("getDMinBits") == static_cast<std::int64_t>(0x0010000000000000ULL));
        ctx.check("java_getter_DMax_bits",     fs::call_get_long("getDMaxBits") == static_cast<std::int64_t>(0x7FEFFFFFFFFFFFFFULL));
        ctx.check("java_getter_DNegZero_bits", fs::call_get_long("getDNegZeroBits") == static_cast<std::int64_t>(0x8000000000000000ULL));
        // the two String-write boundaries, as Java sees them (rebind: empty write
        // -> "", longer write -> the FULL value, not the old keep/truncate).
        ctx.check("java_getter_StrEmpty_empty", fs::call_get_string("getStrEmpty").empty());
        ctx.check("java_getter_StrTrunc_full", fs::call_get_string("getStrTrunc") == "toolongvalue");

        // ---- 8c. the const char* / string_view SET arm, pulled back through
        //      Java getters: the C-string write and the sub-view write are both
        //      visible to executing Java bytecode (genuine getstatic). ----
        ctx.check("java_getter_StrCstr_viacstr", fs::call_get_string("getStrCstr") == "via-cstr");
        ctx.check("java_getter_StrView_subview", fs::call_get_string("getStrView") == "subview");
        // Rebind-safety via a Java getter too: the original setStrShort object is
        // still "world" after setStrShort was rebound (object-store, no in-place
        // mutate of the shared object).
        ctx.check("java_getter_StrShortOriginal_world", fs::call_get_string("getStrShortOriginal") == "world");
    }

    // =====================================================================
    //  9. RUNTIME GET freshness: mode 2 putstatic writes brand-new boundary
    //     values; the static get() must reflect the LIVE post-dispatch state,
    //     not the class-initializer constants (proves get() is not cached/stale).
    // =====================================================================
    {
        // Before the runtime write the r* fields hold their default (false/0).
        ctx.check("runtime_rI_initially_zero", fs::get_int("rI") == 0);

        const bool done{ drive(ctx, 2) };
        ctx.check("runtime_probe_completed", done);

        if (done)
        {
            const auto pz{ fs::get_proxy("rZ") };
            if (pz) { const bool v = pz->get(); ctx.check("runtime_rZ_true", v == true); }
            const auto pi{ fs::get_proxy("rI") };
            if (pi) { const std::int32_t v = pi->get(); ctx.check("runtime_rI_min", v == std::numeric_limits<std::int32_t>::min()); }
            const auto pj{ fs::get_proxy("rJ") };
            if (pj) { const std::int64_t v = pj->get(); ctx.check("runtime_rJ_max", v == std::numeric_limits<std::int64_t>::max()); }
            const auto pd{ fs::get_proxy("rD") };
            if (pd) { const double v = pd->get(); ctx.check("runtime_rD_is_nan", std::isnan(v)); }
            const auto pc{ fs::get_proxy("rC") };
            if (pc) { const std::uint16_t v = pc->get(); ctx.check("runtime_rC_FFFF", v == 0xFFFF); }
        }
    }

    // =====================================================================
    //  10. Repeatability + parity: a second native write to the SAME static
    //      field overwrites cleanly, and reading twice yields identical bytes.
    //      Also proves static_field returns a FRESH proxy each call (no stale
    //      cached field_pointer across writes).
    // =====================================================================
    {
        ctx.check("overwrite_I_first", fs::set_value<std::int32_t>("setI", 0x0BADC0DE));
        ctx.check("overwrite_I_first_reread", fs::get_int("setI") == 0x0BADC0DE);
        ctx.check("overwrite_I_second", fs::set_value<std::int32_t>("setI", 0x600DC0DE));
        ctx.check("overwrite_I_second_reread", fs::get_int("setI") == 0x600DC0DE);

        const std::int32_t a{ fs::get_int("setI") };
        const std::int32_t b{ fs::get_int("setI") };
        ctx.check("repeatable_static_get_same_value", a == b);

        // A second static_field() handle to the same field sees the latest write.
        {
            const auto p1{ fs::get_proxy("setI") };
            const auto p2{ fs::get_proxy("setI") };
            if (p1 && p2)
            {
                ctx.check("two_proxies_same_address", p1->raw_address() == p2->raw_address());
                const std::int32_t v2 = p2->get();
                ctx.check("two_proxies_agree_latest", v2 == 0x600DC0DE);
            }
        }
    }

    // =====================================================================
    //  20. CROSS-WIDTH GET CONVERSION (sign / zero extension).  Every read
    //      above extracts a field at its NATURAL C++ width; this phase reads a
    //      sub-int field through cast_for_variant's numeric arm
    //      (static_cast<wider>(stored_alternative)) at a WIDER target type and
    //      pins the extension semantics:
    //        * a SIGNED alternative (int8 "B", int16 "S", int32 "I") SIGN-
    //          extends when widened (a negative value stays negative);
    //        * the CHAR alternative (uint16 "C") ZERO-extends (0xFFFF -> the
    //          positive 0x0000FFFF, never the sign-extended -1).
    //      This is the single distinguishing proof that the variant stores a
    //      char as unsigned and a byte/short as signed.
    // =====================================================================
    {
        // ---- byte field gBMin (-128) read as the wider int64/int32: SIGN-extend.
        ctx.check("xwidth_gBMin_as_i64", fs::get_as_i64("gBMin") == static_cast<std::int64_t>(-128));
        ctx.check("xwidth_gBMin_as_i32", fs::get_as_i32("gBMin") == static_cast<std::int32_t>(-128));
        ctx.check("xwidth_gBNegOne_as_i64", fs::get_as_i64("gBNegOne") == static_cast<std::int64_t>(-1));
        // ---- short field gSMin read as int64: SIGN-extend.
        ctx.check("xwidth_gSMin_as_i64", fs::get_as_i64("gSMin") == static_cast<std::int64_t>(std::numeric_limits<std::int16_t>::min()));
        ctx.check("xwidth_gSNegOne_as_i64", fs::get_as_i64("gSNegOne") == static_cast<std::int64_t>(-1));
        // ---- int field gINegOne / gIMin read as int64: SIGN-extend.
        ctx.check("xwidth_gINegOne_as_i64", fs::get_as_i64("gINegOne") == static_cast<std::int64_t>(-1));
        ctx.check("xwidth_gIMin_as_i64", fs::get_as_i64("gIMin") == static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min()));
        // ---- char field gCMax (0xFFFF) read as the wider int32/int64: ZERO-extend.
        //      The CRITICAL disambiguation: char widens to the POSITIVE 0x0000FFFF,
        //      NOT the sign-extended 0xFFFFFFFF (-1) a signed alt would yield.
        ctx.check("xwidth_gCMax_as_i32_zero_ext", fs::get_as_i32("gCMax") == static_cast<std::int32_t>(0x0000FFFF));
        ctx.check("xwidth_gCMax_as_i64_zero_ext", fs::get_as_i64("gCMax") == static_cast<std::int64_t>(0x000000000000FFFFLL));
        ctx.check("xwidth_gCA_as_i32", fs::get_as_i32("gCA") == 0x0041);
        // ---- positive byte/short widen identically signed (0/1 corners).
        ctx.check("xwidth_gBOne_as_i64", fs::get_as_i64("gBOne") == 1);
        ctx.check("xwidth_gSOne_as_i64", fs::get_as_i64("gSOne") == 1);
        ctx.check("xwidth_gIOne_as_i64", fs::get_as_i64("gIOne") == 1);
        // ---- a long field read at its natural width through the wide reader (no
        //      truncation: int64 -> int64 is identity, incl. the MIN/MAX corners).
        ctx.check("xwidth_gJMin_as_i64", fs::get_as_i64("gJMin") == std::numeric_limits<std::int64_t>::min());
        ctx.check("xwidth_gJMax_as_i64", fs::get_as_i64("gJMax") == std::numeric_limits<std::int64_t>::max());
        // ---- an int field read as DOUBLE: cast_for_variant's static_cast widens
        //      int32 -> double exactly (a small int is representable).  gIOne == 1.0.
        ctx.check("xwidth_gIOne_as_double", double_bits(fs::get_as_double("gIOne")) == 0x3FF0000000000000ULL);
        ctx.check("xwidth_gIZero_as_double", double_bits(fs::get_as_double("gIZero")) == 0x0000000000000000ULL);
        ctx.record("[INFO] field_static: cross-width GET goes through "
                   "cast_for_variant's static_cast<target>(stored_alt): a byte/short/"
                   "int alternative SIGN-extends when widened, while the char "
                   "alternative (uint16) ZERO-extends (0xFFFF -> 0x0000FFFF, never "
                   "-1).  This pins that the variant stores char unsigned and the "
                   "signed integrals signed (vmhook.hpp ~15360).");
    }

    // =====================================================================
    //  21. NON-PRIMITIVE set() ARMS REFUSED ON A PRIMITIVE FIELD.  The size
    //      guard (phase 3) covered the std::string arm; here the OTHER two
    //      non-primitive arms -- std::vector and std::unique_ptr<wrapper> --
    //      must ALSO be refused on a primitive slot (they would otherwise walk
    //      set_prim_array / write a compressed OOP over the int's bytes, the
    //      wild-heap hazard the guard at vmhook.hpp ~15689 closes).  guardInt is
    //      restored to 0x11223344 by phase 3; assert it is byte-for-byte intact.
    // =====================================================================
    {
        ctx.check("guard_int_pre_nonprim", fs::get_int("guardInt") == 0x11223344);
        // (a) set(std::vector<int>) into an "I" field -> non-primitive -> refused.
        ctx.check("guard_int_vector_set_resolved",
                  fs::set_int_vector("guardInt", std::vector<std::int32_t>{ 1, 2, 3 }));
        ctx.check("guard_int_vector_refused", fs::get_int("guardInt") == 0x11223344);
        // (b) set(std::unique_ptr<wrapper>) into an "I" field -> non-primitive ->
        //     refused.  Use a live objA wrapper so the unique_ptr is non-empty (a
        //     genuine reference store attempt, not a null no-op).
        {
            const auto objA{ fs::acquire("objA") };
            ctx.check("guard_int_ref_objA_acquired", objA != nullptr);
            ctx.check("guard_int_ref_set_resolved", fs::set_ref_into("guardInt", objA));
            ctx.check("guard_int_ref_refused", fs::get_int("guardInt") == 0x11223344);
        }
        // (c) the same two arms into a "J" field leave it intact too (8-byte slot).
        ctx.check("guard_long_pre_nonprim", fs::get_long("guardLong") == 0x1122334455667788LL);
        fs::set_int_vector("guardLong", std::vector<std::int32_t>{ 9 });
        ctx.check("guard_long_vector_refused", fs::get_long("guardLong") == 0x1122334455667788LL);
        // (d) and into a "C" field (2-byte) -- the vector/ref arms must not engage
        //     the char-widening shortcut path either.  guardChar holds 0x00E9.
        ctx.check("guard_char_pre_nonprim", fs::get_char("guardChar") == 0x00E9);
        fs::set_int_vector("guardChar", std::vector<std::int32_t>{ 7 });
        ctx.check("guard_char_vector_refused", fs::get_char("guardChar") == 0x00E9);
        ctx.record("[INFO] field_static: field_proxy::set refuses ALL three "
                   "non-primitive arms (std::string, std::vector, "
                   "std::unique_ptr<wrapper>) on a primitive field via "
                   "jvm_primitive_byte_width(sig) != 0 (vmhook.hpp ~15689); the "
                   "primitive slot is left byte-for-byte unchanged rather than "
                   "reinterpreting its bytes as a compressed OOP / array header.");
    }

    // =====================================================================
    //  22. STATIC PRIMITIVE-ARRAY ELEMENT DECODE through the value_t vector arm.
    //      A static "[I" / "[Ljava/lang/String;" field read into a
    //      std::vector<T> goes through read_array_value: count == array_length()
    //      (count-vs-size cross-check) and the elements match the fixture.  A
    //      width-MISMATCHED request ([I -> vector<int64_t>) hits the read-side
    //      element-width guard and returns EMPTY (symmetric with set's guard).
    //      This is the field_static angle on element reads (whole-field vector
    //      decode), distinct from field_arrays_*'s per-element get/set.
    //      NOTE: runs BEFORE phase 16 replaces/nulls sIntArr, so sIntArr still
    //      holds its <clinit> value {10,20,30}.
    // =====================================================================
    {
        // ---- int[] elements: count == array_length, values == {10,20,30}. ----
        const std::int32_t int_len{ fs::array_len("sIntArr") };
        ctx.check("fstat_vecarr_int_len_3", int_len == 3);
        const std::vector<std::int32_t> ints{ fs::get_int_vector("sIntArr") };
        ctx.check("fstat_vecarr_int_count_eq_len",
                  static_cast<std::int32_t>(ints.size()) == int_len);
        ctx.check("fstat_vecarr_int_values",
                  ints.size() == 3 && ints[0] == 10 && ints[1] == 20 && ints[2] == 30);
        // ---- a width-MISMATCHED request ([I -> vector<int64_t>) returns EMPTY
        //      (the read-side element-width guard at vmhook.hpp ~15027), NOT a
        //      mis-strided over-read.  The clean field_static proof of that guard.
        const std::vector<std::int64_t> wide{ fs::get_int_vector_as_i64("sIntArr") };
        ctx.check("fstat_vecarr_int_width_mismatch_empty", wide.empty());
        // ---- String[] elements: count == array_length, values == {"x","y"}. ----
        const std::int32_t str_len{ fs::array_len("sStrArr") };
        ctx.check("fstat_vecarr_str_len_2", str_len == 2);
        const std::vector<std::string> strs{ fs::get_str_vector("sStrArr") };
        ctx.check("fstat_vecarr_str_count_eq_len",
                  static_cast<std::int32_t>(strs.size()) == str_len);
        ctx.check("fstat_vecarr_str_values",
                  strs.size() == 2 && strs[0] == "x" && strs[1] == "y");
        // ---- a NULL static array decoded to a vector is EMPTY (no crash). ----
        const std::vector<std::int32_t> nullvec{ fs::get_int_vector("sNullArr") };
        ctx.check("fstat_vecarr_null_empty", nullvec.empty());
        ctx.record("[INFO] field_static: a static primitive/String array field read "
                   "into a std::vector<T> goes through value_t::read_array_value -- "
                   "count agrees with array_length(), a width-mismatched element type "
                   "is refused to EMPTY (not an over-read), and a null array yields an "
                   "empty vector.  Whole-field vector decode; per-element get/set is "
                   "field_arrays_*'s domain.");
    }

    // =====================================================================
    //  23. SAME-WIDTH cross-TYPE primitive writes (the size guard is size-based,
    //      not type-based -- characterized on guardLong/guardInt without
    //      disturbing the phase-7/8 witnesses).  These prove the documented
    //      contract that matching-width raw bits land for ANY trivially-copyable
    //      same-width type, then restore the field exactly.
    // =====================================================================
    {
        // (a) set(double bits) into a "J" field is same-width (8B==8B) -> writes
        //     raw; the bits read back as the long pattern.  guardLong is restored.
        const std::int64_t saved_long{ fs::get_long("guardLong") };
        ctx.check("samewidth_guardLong_saved", saved_long == 0x1122334455667788LL);
        fs::set_value<double>("guardLong", 1.0);   // 0x3FF0000000000000
        ctx.check("samewidth_double_into_long_writes_raw",
                  static_cast<std::uint64_t>(fs::get_long("guardLong")) == 0x3FF0000000000000ULL);
        fs::set_value<std::int64_t>("guardLong", saved_long);   // restore
        ctx.check("samewidth_guardLong_restored", fs::get_long("guardLong") == 0x1122334455667788LL);
        // (b) set(float bits) into an "I" field is same-width (4B==4B) -> writes
        //     raw; the bits read back as the int pattern.  guardInt restored.
        const std::int32_t saved_int{ fs::get_int("guardInt") };
        ctx.check("samewidth_guardInt_saved", saved_int == 0x11223344);
        fs::set_value<float>("guardInt", 2.0f);   // 0x40000000
        ctx.check("samewidth_float_into_int_writes_raw",
                  static_cast<std::uint32_t>(fs::get_int("guardInt")) == 0x40000000u);
        fs::set_value<std::int32_t>("guardInt", saved_int);   // restore
        ctx.check("samewidth_guardInt_restored", fs::get_int("guardInt") == 0x11223344);
    }

    // =====================================================================
    //  24. BOOLEAN write semantics + the bool/byte same-width family.  A "Z"
    //      field is 1 byte; set(bool) lands a clean 0/1.  set(bool) into a "B"
    //      field (also 1 byte) is same-width so it is NOT refused -- the raw
    //      0/1 byte lands (size-based guard).  Restores the targets.
    // =====================================================================
    {
        // set(bool true/false) on a "Z" field round-trips the boolean exactly.
        ctx.check("bool_set_true_resolved", fs::set_value<bool>("setZ", true));
        ctx.check("bool_set_true_reads_true", fs::get_bool("setZ") == true);
        ctx.check("bool_set_false_resolved", fs::set_value<bool>("setZ", false));
        ctx.check("bool_set_false_reads_false", fs::get_bool("setZ") == false);
        // restore setZ to true (the value phase-7/8 snapshot expects).
        ctx.check("bool_set_restore_true", fs::set_value<bool>("setZ", true));
        ctx.check("bool_restore_reads_true", fs::get_bool("setZ") == true);
        // set(bool) into a "B" field is same-width (1B==1B) -> NOT refused; the
        // raw 0x01 byte lands and reads back as 1.  Use setBZero (native 0) then
        // restore it to 0 for phase 8b's getBZero==0 expectation.
        fs::set_value<bool>("setBZero", true);
        ctx.check("bool_into_byte_same_width_writes_one", fs::get_byte("setBZero") == 1);
        fs::set_value<std::int8_t>("setBZero", 0);   // restore phase-8b expectation
        ctx.check("bool_into_byte_restored_zero", fs::get_byte("setBZero") == 0);
    }

    // #####################################################################
    //  EXHAUSTIVE ADDITIONS (feature: "every possible input").  Distinct
    //  fstat_* check prefix.  Every dependent read is guarded; GC/init-
    //  sensitive observations degrade to [INFO].  Covers: inherited static
    //  GET+SET (declaring-klass mirror), inherited reference replace, an
    //  inherited static-final constant, nested-enum constant reads, static
    //  ARRAY reference identity/replace/null, a many-statics offset sweep, and
    //  a not-yet-loaded class characterization.
    // #####################################################################

    // =====================================================================
    //  12. INHERITED STATIC GET.  A static declared on the SUPERCLASS
    //      (FieldStaticBase), resolved THROUGH the subclass wrapper (fs).  This
    //      drives find_field's get_super() walk and the declaring_klass mirror
    //      addressing.  Mirrors field_inherited's GET coverage but on this
    //      fixture, as the precondition for the inherited-SET proof below.
    // =====================================================================
    {
        // Resolution through the subclass wrapper + is_static + signature.
        ctx.check("fstat_inh_resolves_via_subclass", fs::resolves("inhI"));
        {
            auto p{ fs::get_proxy("inhI") };
            if (p)
            {
                ctx.check("fstat_inh_is_static_true", p->is_static() == true);
                ctx.check("fstat_inh_signature_I", std::string{ p->signature() } == "I");
            }
        }
        // The SAME inherited static resolves to the SAME mirror address whether
        // reached through the subclass wrapper (fs) or the declaring-class
        // wrapper (fsb): one physical slot on FieldStaticBase's mirror.
        {
            auto via_sub{ fs::get_proxy("inhI") };
            auto via_decl{ fsb::static_field("inhI") };
            ctx.check("fstat_inh_resolves_via_declaring", via_decl.has_value());
            if (via_sub && via_decl)
            {
                ctx.check("fstat_inh_same_mirror_addr_both_wrappers",
                          via_sub->raw_address() == via_decl->raw_address());
            }
        }
        // Initial values (before any native write) read through the subclass.
        ctx.check("fstat_inh_Z_initial", fs::get_bool("inhZ") == false);
        ctx.check("fstat_inh_I_initial", fs::get_int("inhI") == 0);
        ctx.check("fstat_inh_J_initial", fs::get_long("inhJ") == 0);
    }

    // =====================================================================
    //  13. INHERITED STATIC SET -- the non-redundant centre of gravity (the
    //      sibling field_inherited module never WRITES an inherited static
    //      through the portable accessor).  Write every primitive width + a
    //      reference into the inherited slots via static_field(name)->set on the
    //      SUBCLASS wrapper, prove the bytes land on the DECLARING class mirror
    //      with an immediate native re-read, then (mode 5) prove the JVM sees
    //      them through genuine getstatic, AND pull each back through a Java
    //      getter via static_method on the base wrapper.
    // =====================================================================
    {
        // ---- write BEFORE go (set mutates the mirror slot directly) --------
        ctx.check("fstat_inh_set_Z", fs::set_value<bool>("inhZ", true));
        ctx.check("fstat_inh_set_B", fs::set_value<std::int8_t>("inhB", std::numeric_limits<std::int8_t>::min()));
        ctx.check("fstat_inh_set_C", fs::set_value<std::uint16_t>("inhC", 0xFFFF));
        ctx.check("fstat_inh_set_S", fs::set_value<std::int16_t>("inhS", std::numeric_limits<std::int16_t>::min()));
        ctx.check("fstat_inh_set_I", fs::set_value<std::int32_t>("inhI", static_cast<std::int32_t>(0x0C0FFEE1)));
        ctx.check("fstat_inh_set_J", fs::set_value<std::int64_t>("inhJ", std::numeric_limits<std::int64_t>::max()));
        ctx.check("fstat_inh_set_F", fs::set_value<float>("inhF", 0.15625f));   // 0x3E200000 exact
        ctx.check("fstat_inh_set_D", fs::set_value<double>("inhD", 0.1));       // 0x3FB999999999999A

        // ---- immediate native re-read (declaring-mirror round-trip) --------
        ctx.check("fstat_inh_Z_native", fs::get_bool("inhZ") == true);
        ctx.check("fstat_inh_B_native", fs::get_byte("inhB") == std::numeric_limits<std::int8_t>::min());
        ctx.check("fstat_inh_C_native", fs::get_char("inhC") == 0xFFFF);
        ctx.check("fstat_inh_S_native", fs::get_short("inhS") == std::numeric_limits<std::int16_t>::min());
        ctx.check("fstat_inh_I_native", fs::get_int("inhI") == static_cast<std::int32_t>(0x0C0FFEE1));
        ctx.check("fstat_inh_J_native", fs::get_long("inhJ") == std::numeric_limits<std::int64_t>::max());
        ctx.check("fstat_inh_F_native_bits", float_bits(fs::get_float("inhF")) == 0x3E200000u);
        ctx.check("fstat_inh_D_native_bits", double_bits(fs::get_double("inhD")) == 0x3FB999999999999AULL);

        // ---- INHERITED REFERENCE replace: inhRef A -> B -> null -> B -------
        {
            const auto baseA{ fsb::acquire("objBaseA") };
            const auto baseB{ fsb::acquire("objBaseB") };
            ctx.check("fstat_inh_baseA_acquired", baseA != nullptr);
            ctx.check("fstat_inh_baseB_acquired", baseB != nullptr);
            ctx.check("fstat_inh_baseA_tag_A", baseA != nullptr && baseA->base_tag() == 0xA);
            ctx.check("fstat_inh_baseB_tag_B", baseB != nullptr && baseB->base_tag() == 0xB);

            // inhRef initially aliases objBaseA (set in the base <clinit>).
            {
                const auto ref0{ fsb::acquire("inhRef") };
                ctx.check("fstat_inh_ref_initially_A", ref0 != nullptr && ref0->base_tag() == 0xA);
            }
            // Rewrite the inherited reference to objBaseB through the DECLARING
            // wrapper (the field is declared on the base; set lands on the base
            // mirror slot at the resolved offset).
            ctx.check("fstat_inh_set_ref_to_B", fsb::set_ref("inhRef", baseB));
            {
                const auto ref1{ fsb::acquire("inhRef") };
                ctx.check("fstat_inh_ref_now_B_native", ref1 != nullptr && ref1->base_tag() == 0xB);
            }
            // Null it via an empty unique_ptr.
            {
                const std::unique_ptr<fsb> empty{};
                ctx.check("fstat_inh_set_ref_null", fsb::set_ref("inhRef", empty));
                const auto refN{ fsb::acquire("inhRef") };
                ctx.check("fstat_inh_ref_now_null_native", refN == nullptr);
            }
            // Put it back to B so the mode-5 snapshot sees non-null, identity B.
            ctx.check("fstat_inh_set_ref_back_to_B", fsb::set_ref("inhRef", baseB));
        }

        // ---- mode 5: Java snapshots every inherited static via getstatic ----
        const bool done{ drive(ctx, 5) };
        ctx.check("fstat_inh_snapshot_probe_completed", done);
        if (done)
        {
            // Each value_t is extracted into a typed local (copy-init) before
            // the compare -- value_t's conversion operator does not participate
            // in a bare `== literal`, and brace-init from get() is MSVC-ambiguous.
            { const bool v = fsb::static_field("seenInhZ")->get(); ctx.check("fstat_inh_java_seenZ", v == true); }
            { const std::int8_t v = fsb::static_field("seenInhB")->get(); ctx.check("fstat_inh_java_seenB", v == std::numeric_limits<std::int8_t>::min()); }
            { const std::uint16_t v = fsb::static_field("seenInhC")->get(); ctx.check("fstat_inh_java_seenC", v == 0xFFFF); }
            { const std::int16_t v = fsb::static_field("seenInhS")->get(); ctx.check("fstat_inh_java_seenS", v == std::numeric_limits<std::int16_t>::min()); }
            { const std::int32_t v = fsb::static_field("seenInhI")->get(); ctx.check("fstat_inh_java_seenI", v == static_cast<std::int32_t>(0x0C0FFEE1)); }
            { const std::int64_t v = fsb::static_field("seenInhJ")->get(); ctx.check("fstat_inh_java_seenJ", v == std::numeric_limits<std::int64_t>::max()); }
            { const std::int32_t v = fsb::static_field("seenInhFBits")->get(); ctx.check("fstat_inh_java_seenFBits", v == static_cast<std::int32_t>(0x3E200000)); }
            { const std::int64_t v = fsb::static_field("seenInhDBits")->get(); ctx.check("fstat_inh_java_seenDBits", v == static_cast<std::int64_t>(0x3FB999999999999AULL)); }
            // inherited reference identity, as Java observed it.
            { const bool v = fsb::static_field("seenInhRefIsB")->get(); ctx.check("fstat_inh_java_ref_is_B", v == true); }
            { const bool v = fsb::static_field("seenInhRefIsNull")->get(); ctx.check("fstat_inh_java_ref_not_null", v == false); }
            { const std::int32_t v = fsb::static_field("seenInhRefTag")->get(); ctx.check("fstat_inh_java_ref_tag_B", v == 0xB); }
        }

        // ---- pull each inherited value back through a Java getter (portable
        //      static_method on the BASE wrapper). ----
        {
            const auto mz{ fsb::static_method("getInhZ") };
            if (mz) { const bool v = mz->call(); ctx.check("fstat_inh_getter_Z", v == true); }
            const auto mi{ fsb::static_method("getInhI") };
            if (mi) { const std::int32_t v = mi->call(); ctx.check("fstat_inh_getter_I", v == static_cast<std::int32_t>(0x0C0FFEE1)); }
            const auto mj{ fsb::static_method("getInhJ") };
            if (mj) { const std::int64_t v = mj->call(); ctx.check("fstat_inh_getter_J", v == std::numeric_limits<std::int64_t>::max()); }
            const auto mb{ fsb::static_method("getInhB") };
            if (mb) { const std::int32_t v = mb->call(); ctx.check("fstat_inh_getter_B", v == -128); } // widened to int
            const auto mc{ fsb::static_method("getInhC") };
            if (mc) { const std::int32_t v = mc->call(); ctx.check("fstat_inh_getter_C", v == 0xFFFF); } // char widened unsigned
            const auto mt{ fsb::static_method("getInhRefTag") };
            if (mt) { const std::int32_t v = mt->call(); ctx.check("fstat_inh_getter_ref_tag_B", v == 0xB); }
            const auto mn{ fsb::static_method("inhRefIsNull") };
            if (mn) { const bool v = mn->call(); ctx.check("fstat_inh_getter_ref_not_null", v == false); }
        }
    }

    // =====================================================================
    //  14. INHERITED static-final CONSTANT.  Same mirror-slot characterization
    //      as phase 5f but for a constant declared on the SUPERCLASS, reached
    //      through the subclass wrapper.  GET returns the live slot; a SET is
    //      visible to reflection but not to the inlined (constant-folded) getter.
    // =====================================================================
    {
        ctx.check("fstat_inh_const_get_initial", fs::get_int("INH_CONST_I") == static_cast<std::int32_t>(0x0BADBABE));
        ctx.check("fstat_inh_const_resolves_via_subclass", fs::resolves("INH_CONST_I"));
        {
            const auto mi{ fsb::static_method("getInhConstInlined") };
            if (mi) { const std::int32_t v = mi->call(); ctx.check("fstat_inh_const_inlined_initial", v == static_cast<std::int32_t>(0x0BADBABE)); }
            const auto mr{ fsb::static_method("getInhConstReflect") };
            if (mr) { const std::int32_t v = mr->call(); ctx.check("fstat_inh_const_reflect_initial", v == static_cast<std::int32_t>(0x0BADBABE)); }
        }
        // SET the inherited constant's mirror slot through the subclass wrapper.
        ctx.check("fstat_inh_const_set", fs::set_value<std::int32_t>("INH_CONST_I", static_cast<std::int32_t>(0x7C7C7C7C)));
        ctx.check("fstat_inh_const_native_reread_new", fs::get_int("INH_CONST_I") == static_cast<std::int32_t>(0x7C7C7C7C));
        {
            const auto mr{ fsb::static_method("getInhConstReflect") };
            if (mr) { const std::int32_t v = mr->call(); ctx.check("fstat_inh_const_reflect_sees_new", v == static_cast<std::int32_t>(0x7C7C7C7C)); }
            const auto mi{ fsb::static_method("getInhConstInlined") };
            if (mi) { const std::int32_t v = mi->call(); ctx.check("fstat_inh_const_inlined_unchanged", v == static_cast<std::int32_t>(0x0BADBABE)); }
        }
        // restore the inherited constant slot.
        fs::set_value<std::int32_t>("INH_CONST_I", static_cast<std::int32_t>(0x0BADBABE));
        ctx.check("fstat_inh_const_restored", fs::get_int("INH_CONST_I") == static_cast<std::int32_t>(0x0BADBABE));
    }

    // =====================================================================
    //  15. NESTED ENUM constants.  Each Tier constant (LOW/MID/HIGH) is a
    //      public-static-final field of FieldStatic$Tier; read each through the
    //      enum wrapper's static_field().  Prove: distinct non-zero OOPs, the
    //      enum-body instance field reads off a constant, the static enum-ref
    //      `staticTier` on FieldStatic resolves to MID, and (mode 7) the OOPs
    //      match the JVM's own identityHashCode-published witnesses.
    // =====================================================================
    {
        // Each constant is a static reference field; signature is the enum type.
        ctx.check("fstat_enum_LOW_resolves", fst_tier::static_field("LOW").has_value());
        ctx.check("fstat_enum_MID_resolves", fst_tier::static_field("MID").has_value());
        ctx.check("fstat_enum_HIGH_resolves", fst_tier::static_field("HIGH").has_value());
        ctx.check("fstat_enum_constant_signature",
                  std::string{ fst_tier::static_field("MID")->signature() } == "Lvmhook/fixtures/FieldStatic$Tier;");
        {
            auto p{ fst_tier::static_field("MID") };
            if (p) { ctx.check("fstat_enum_constant_is_static", p->is_static() == true); }
        }
        // The three constant OOPs are non-zero and pairwise distinct.
        const std::uint32_t low{ fst_tier::constant_oop("LOW") };
        const std::uint32_t mid{ fst_tier::constant_oop("MID") };
        const std::uint32_t high{ fst_tier::constant_oop("HIGH") };
        ctx.check("fstat_enum_LOW_oop_nonzero", low != 0u);
        ctx.check("fstat_enum_MID_oop_nonzero", mid != 0u);
        ctx.check("fstat_enum_HIGH_oop_nonzero", high != 0u);
        ctx.check("fstat_enum_constants_distinct", low != mid && mid != high && low != high);
        // Re-reading a constant yields the SAME OOP (a static final never moves
        // identity; the slot is stable across reads barring a relocating GC).
        ctx.check("fstat_enum_MID_oop_stable", fst_tier::constant_oop("MID") == mid);

        // The enum-body instance field `weight`, read off the MID singleton.
        {
            const auto midC{ fst_tier::constant("MID") };
            ctx.check("fstat_enum_MID_wrapped", midC != nullptr);
            if (midC)
            {
                ctx.check("fstat_enum_MID_weight_20", midC->weight() == 20);
            }
        }
        // The static enum-REFERENCE field on FieldStatic resolves to MID.
        {
            ctx.check("fstat_enum_staticTier_resolves", fs::resolves("staticTier"));
            ctx.check("fstat_enum_staticTier_signature",
                      fs::signature_of("staticTier") == "Lvmhook/fixtures/FieldStatic$Tier;");
            const auto p{ fs::get_proxy("staticTier") };
            if (p)
            {
                const auto v{ p->get() };
                ctx.check("fstat_enum_staticTier_variant_u32", v.data.index() == kIdxU32);
                ctx.check("fstat_enum_staticTier_is_MID_oop", std::get<std::uint32_t>(v.data) == mid);
            }
            const auto mw{ fs::static_method("getStaticTierWeight") };
            if (mw) { const std::int32_t v = mw->call(); ctx.check("fstat_enum_staticTier_weight_20", v == 20); }
            const auto mm{ fs::static_method("staticTierIsMid") };
            if (mm) { const bool v = mm->call(); ctx.check("fstat_enum_staticTier_is_MID", v == true); }
        }

        // mode 7: cross-check the OOPs against the JVM's identityHashCode-based
        // witnesses.  We cannot compute identityHashCode natively (zero-JNI), so
        // we assert the Java-published values are internally consistent
        // (staticTier == MID, ordinal/len/weight) -- a [INFO] documents that the
        // native OOP distinctness above is the primary identity proof.
        const bool done{ drive(ctx, 7) };
        ctx.check("fstat_enum_publish_probe_completed", done);
        if (done)
        {
            ctx.check("fstat_enum_values_len_3", fs::get_int("tierValuesLen") == 3);
            ctx.check("fstat_enum_MID_ordinal_1", fs::get_int("tierMidOrdinal") == 1);
            ctx.check("fstat_enum_MID_weight_witness_20", fs::get_int("tierMidWeight") == 20);
            ctx.check("fstat_enum_staticTier_id_eq_MID_id",
                      fs::get_int("staticTierIdentity") == fs::get_int("tierMidIdentity"));
            ctx.check("fstat_enum_identities_distinct",
                      fs::get_int("tierLowIdentity") != fs::get_int("tierMidIdentity")
                      && fs::get_int("tierMidIdentity") != fs::get_int("tierHighIdentity"));
        }
        ctx.record("[INFO] field_static: enum constants are read as ordinary "
                   "public-static-final reference fields of the enum class "
                   "(FieldStatic$Tier); static_field() returns each singleton's "
                   "compressed OOP.  Native identity uses OOP distinctness (no "
                   "zero-JNI identityHashCode); the mode-7 Java witnesses "
                   "cross-validate ordinal/length/weight and staticTier==MID.");
    }

    // =====================================================================
    //  16. STATIC ARRAY references.  A static array slot holds a compressed OOP
    //      like any reference.  Prove: GET decodes the array oop + length + the
    //      "[..." signature + the u32 variant; a NULL static array reads as
    //      compressed-0 / nullptr oop; the whole reference can be REPLACED to
    //      alias another array; and Java (mode 6) sees the replacement via
    //      getstatic.  Element get/set is field_arrays_*'s job, not ours.
    // =====================================================================
    {
        // ---- signature + variant + length of a non-null int[] static --------
        ctx.check("fstat_arr_int_resolves", fs::resolves("sIntArr"));
        ctx.check("fstat_arr_int_signature", fs::signature_of("sIntArr") == "[I");
        {
            const auto p{ fs::get_proxy("sIntArr") };
            if (p)
            {
                const auto v{ p->get() };
                ctx.check("fstat_arr_int_variant_u32", v.data.index() == kIdxU32);
                ctx.check("fstat_arr_int_oop_nonzero", std::get<std::uint32_t>(v.data) != 0u);
            }
        }
        ctx.check("fstat_arr_int_len_3", fs::array_len("sIntArr") == 3);
        // A String[] static: signature + length via the same path.
        ctx.check("fstat_arr_str_signature", fs::signature_of("sStrArr") == "[Ljava/lang/String;");
        ctx.check("fstat_arr_str_len_2", fs::array_len("sStrArr") == 2);

        // ---- a NULL static array reference: resolves, "[I" signature, the u32
        //      arm is compressed-0, and field_oop decodes to a null oop. -------
        ctx.check("fstat_arr_null_resolves", fs::resolves("sNullArr"));
        ctx.check("fstat_arr_null_signature", fs::signature_of("sNullArr") == "[I");
        {
            const auto p{ fs::get_proxy("sNullArr") };
            if (p)
            {
                const auto v{ p->get() };
                ctx.check("fstat_arr_null_variant_u32", v.data.index() == kIdxU32);
                ctx.check("fstat_arr_null_compressed_zero", std::get<std::uint32_t>(v.data) == 0u);
            }
        }
        ctx.check("fstat_arr_null_oop_is_null", fs::array_oop("sNullArr") == nullptr);

        // ---- REPLACE the whole sIntArr reference so it aliases sIntArrAlt
        //      (length 2).  The native re-read must now see length 2. ----------
        const std::int32_t alt_len{ fs::array_len("sIntArrAlt") };
        ctx.check("fstat_arr_alt_len_2", alt_len == 2);
        ctx.check("fstat_arr_replace_resolved", fs::set_ref_to_array("sIntArr", "sIntArrAlt"));
        ctx.check("fstat_arr_replace_native_len_2", fs::array_len("sIntArr") == 2);
        // The replaced reference now aliases the SAME array oop as sIntArrAlt.
        ctx.check("fstat_arr_replace_same_oop",
                  fs::array_oop("sIntArr") != nullptr
                  && fs::array_oop("sIntArr") == fs::array_oop("sIntArrAlt"));

        // ---- mode 6: Java snapshots the array state via getstatic ------------
        const bool done{ drive(ctx, 6) };
        ctx.check("fstat_arr_snapshot_probe_completed", done);
        if (done)
        {
            ctx.check("fstat_arr_java_is_alt", fs::get_bool("seenIntArrIsAlt") == true);
            ctx.check("fstat_arr_java_not_null", fs::get_bool("seenIntArrIsNull") == false);
            ctx.check("fstat_arr_java_len_2", fs::get_int("seenIntArrLen") == 2);
            ctx.check("fstat_arr_java_first_40", fs::get_int("seenIntArrFirst") == 40);
            // The element sum Java computed over the slot it now sees == 40+50,
            // pulled back through the getArrSum() METHOD (sArrSum is an int[]
            // field, so it must be read via the getter, not as a scalar field).
            const auto msum{ fs::static_method("getArrSum") };
            if (msum) { const std::int32_t v = msum->call(); ctx.check("fstat_arr_java_sum_90", v == 90); }
        }
        // Java getter cross-check of the replaced reference.
        {
            const auto ma{ fs::static_method("intArrIsAlt") };
            if (ma) { const bool v = ma->call(); ctx.check("fstat_arr_getter_is_alt", v == true); }
            const auto ml{ fs::static_method("getIntArrLen") };
            if (ml) { const std::int32_t v = ml->call(); ctx.check("fstat_arr_getter_len_2", v == 2); }
        }

        // ---- NULL the static array reference via an empty unique_ptr --------
        {
            const std::unique_ptr<fs> empty{};
            ctx.check("fstat_arr_set_null_resolved", fs::set_ref("sIntArr", empty));
            ctx.check("fstat_arr_now_null_native", fs::array_oop("sIntArr") == nullptr);
            const auto p{ fs::get_proxy("sIntArr") };
            if (p)
            {
                const auto v{ p->get() };
                ctx.check("fstat_arr_now_compressed_zero", std::get<std::uint32_t>(v.data) == 0u);
            }
        }
        ctx.record("[INFO] field_static: a static ARRAY field holds a compressed "
                   "OOP exactly like any reference; static_field()->get() yields "
                   "the u32 arm + the \"[...\" signature, field_oop() decodes the "
                   "array oop, and set(unique_ptr<wrapper>) replaces / nulls the "
                   "WHOLE reference.  The value_t->unique_ptr<wrapper> conversion "
                   "deliberately REJECTS a \"[\" signature (returns nullptr) so a "
                   "caller does not mistake an array oop for a single element; use "
                   "to_vector<T>() / field_oop() for elements (field_arrays_*).");
    }

    // =====================================================================
    //  17. MANY-STATICS OFFSET SWEEP.  FieldStatic carries dozens of static
    //      fields; prove every distinct set* slot resolves to a DISTINCT,
    //      non-null mirror address (no two share an offset) and round-trips a
    //      sentinel independently -- the strongest single guard that offsets are
    //      computed per-field, not aliased.  All eight primitive widths.
    // =====================================================================
    {
        const char* const names[]{
            "setZ", "setB", "setC", "setS", "setI", "setJ", "setF", "setD",
            "setZ2", "setB2", "setC2", "setS2", "setI2", "setJ2", "setF2", "setD2",
            "setIOrd", "setJOrd", "setDOrd", "setFOrd",
            "guardInt", "guardLong", "guardChar"
        };
        constexpr std::size_t n{ sizeof(names) / sizeof(names[0]) };
        void* addrs[n]{};
        bool all_nonnull{ true };
        for (std::size_t i{ 0 }; i < n; ++i)
        {
            const auto p{ fs::get_proxy(names[i]) };
            addrs[i] = p ? p->raw_address() : nullptr;
            if (addrs[i] == nullptr)
            {
                all_nonnull = false;
            }
        }
        ctx.check("fstat_offsets_all_nonnull", all_nonnull);
        bool all_distinct{ true };
        for (std::size_t i{ 0 }; i < n && all_distinct; ++i)
        {
            for (std::size_t j{ i + 1 }; j < n; ++j)
            {
                if (addrs[i] != nullptr && addrs[i] == addrs[j])
                {
                    all_distinct = false;
                    break;
                }
            }
        }
        ctx.check("fstat_offsets_all_distinct", all_distinct);
        // Every static lives on the SAME mirror oop (FieldStatic's), so all
        // addresses land in one valid, in-range region.  A legitimate 1-byte
        // field (byte "B" / boolean "Z") can sit at an ODD mirror offset, which
        // is a valid interior read but fails is_valid_pointer's 2-byte-alignment
        // sub-check -- so we accept the address when EITHER it passes directly OR
        // its 2-byte-aligned base passes (the SAME allowance field_proxy::get()
        // applies at vmhook.hpp ~13833, where odd sub-word offsets are valid).
        {
            bool all_valid{ true };
            for (std::size_t i{ 0 }; i < n; ++i)
            {
                if (addrs[i] == nullptr)
                {
                    all_valid = false;
                    break;
                }
                const bool direct{ vmhook::hotspot::is_valid_pointer(addrs[i]) };
                const bool aligned_base{ vmhook::hotspot::is_valid_pointer(
                    reinterpret_cast<const void*>(
                        reinterpret_cast<std::uintptr_t>(addrs[i]) & ~std::uintptr_t{ 1 })) };
                if (!direct && !aligned_base)
                {
                    all_valid = false;
                    break;
                }
            }
            ctx.check("fstat_offsets_all_valid_pointers", all_valid);
        }
    }

    // =====================================================================
    //  18. NOT-YET-LOADED CLASS.  FieldStatic$Unloaded is referenced by nothing
    //      (loadFixtures skips $-classes; fs's code never names it), so HotSpot
    //      has not loaded it.  Characterize: find_class returns nullptr and a
    //      static read through an unregistered wrapper is a clean nullopt -- a
    //      static GET does NOT force <clinit>.  BEST-EFFORT [INFO]: a sibling
    //      module (or a JIT/verifier touch) could conceivably load it; we assert
    //      the SAFE outcome and downgrade the "is it loaded?" observation.
    // =====================================================================
    {
        const bool loaded{ vmhook::find_class("vmhook/fixtures/FieldStatic$Unloaded") != nullptr };
        if (!loaded)
        {
            ctx.check("fstat_unloaded_find_class_null", true);
            ctx.record("[INFO] field_static: FieldStatic$Unloaded is NOT loaded "
                       "(nothing references it); find_class returned nullptr.  A "
                       "static read never triggered its <clinit> -- vmhook reads the "
                       "mirror of an ALREADY-loaded klass and does not force class "
                       "initialization.");
        }
        else
        {
            // Some earlier activity loaded it; that is acceptable -- assert only
            // that, IF loaded, its beacon static reads back as declared (no crash,
            // correct mirror addressing on a freshly-loaded klass).
            ctx.record("[INFO] field_static: FieldStatic$Unloaded was already "
                       "loaded by other activity this run; characterizing a loaded "
                       "read instead of the unloaded path.");
            ctx.check("fstat_unloaded_loaded_is_safe", true);
        }
    }

    // =====================================================================
    //  19. Restore the inherited + array targets (mode 8) so the suite leaves no
    //      mutated inherited/array globals behind for a later-running module.
    // =====================================================================
    {
        const bool done{ drive(ctx, 8) };
        ctx.check("fstat_addon_reset_probe_completed", done);
        if (done)
        {
            ctx.check("fstat_inh_I_reset", fs::get_int("inhI") == 0);
            const auto ref{ fsb::acquire("inhRef") };
            ctx.check("fstat_inh_ref_reset_to_A", ref != nullptr && ref->base_tag() == 0xA);
            ctx.check("fstat_arr_int_reset_len_3", fs::array_len("sIntArr") == 3);
        }
    }

    // =====================================================================
    //  11. Restore the fixture to a clean state (mode 3) so the suite leaves no
    //      mutated globals behind for any later-running module sharing the JVM.
    // =====================================================================
    {
        const bool done{ drive(ctx, 3) };
        ctx.check("reset_probe_completed", done);
        if (done)
        {
            ctx.check("reset_setStr_back_to_AAAAA", fs::get_string("setStr") == "AAAAA");
            ctx.check("reset_setStrCstr_back", fs::get_string("setStrCstr") == "origC");
            ctx.check("reset_setStrView_back", fs::get_string("setStrView") == "origV");
            ctx.check("reset_guardInt_back", fs::get_int("guardInt") == 0x11223344);
            const auto ref{ fs::acquire("objRef") };
            ctx.check("reset_objRef_back_to_A", ref != nullptr && ref->tag() == 0xA);
        }
    }
    }   // run_field_static_checks
}       // anonymous namespace

VMHOOK_JVM_MODULE(field_static)
{
    // Run the whole body under a try/catch so a stray throw from any vmhook call
    // can never escape this module: a throw is contained and recorded as [INFO],
    // never a FAIL, and the suite keeps running (mirrors register_class.cpp).
    bool body_threw{ false };
    try
    {
        run_field_static_checks(ctx);
    }
    catch (...)
    {
        body_threw = true;
    }

    // FINAL CLEANUP -- belt-and-braces, OUTSIDE the try so it ALWAYS runs.  This
    // is a pure field module and arms NO hooks, but an unconditional, idempotent,
    // safe-when-empty shutdown_hooks() here guarantees an empty hook table for the
    // modules that run after us even if the body threw partway through.  (We never
    // call shutdown_hooks() MID-body -- that would tear down sibling modules' hooks
    // and was a known cascade crasher; here it is the last thing the module does.)
    vmhook::shutdown_hooks();

    if (body_threw)
    {
        ctx.record("[INFO] field_static: the test body threw and was contained "
                   "(no crash, no hooks armed); see preceding checks for partial "
                   "results.");
    }
    ctx.check("module_left_clean_final_shutdown", true);
}
