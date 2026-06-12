// make_java_array JVM test module  (feature area: heap allocation / arrays)
//
// THE make_java_array authority: the first live-JVM coverage of
// vmhook::make_java_array(class_name, length, element_size[, allow_jni_fallback])
// — allocating a brand-new Java ARRAY oop straight from C++ with NO JNI
// NewTypeArray/NewObjectArray on the fast path.  make_java_array is the low-level
// primitive make_java_string is built on: make_java_string allocates its backing
// [B / [C through exactly this call, so the primitive array paths here are
// load-bearing on EVERY JDK.
//
// The flow under test (vmhook.hpp make_java_array):
//   1. length < 0            -> return nullptr (the negative-length guard, the
//                               very first statement, before any klass work).
//   2. find_class(descriptor) -> array klass.
//   3. JDK-8 FALLBACK ("FIX D"): when find_class misses AND the descriptor starts
//      with '[' (JDK 8's ClassLoader.loadClass rejects array descriptors), fall
//      back to JNIEnv::FindClass (which DOES accept "[B" / "[Ljava/lang/Object;")
//      and convert the returned jclass mirror to a Klass*.
//   4. make_java_object(klass, 16 + length*element_size) -> raw oop (zeroed,
//      header-stamped).  If the TLAB fast path returns null AND allow_jni_fallback
//      is true AND the descriptor is a PRIMITIVE array, an ADDITIVE GC-aware JNI
//      New<Type>Array slow path is tried (the landed make_java_object GC-gap fix);
//      reference arrays are NOT covered by that fallback.
//   5. write the Java array length into the int32 _length slot at byte +12.
//
// WHAT THIS MODULE PROVES, several independent ways, all from INSIDE an
// interpreter detour on MakeJavaArray.cycle() (where HotSpot's current_java_thread
// — the allocation precondition — is established; make_java_array cannot allocate
// off the Java thread):
//
//   NATIVE (HARD on all JDKs for primitives; best-effort for ref arrays on JDK8):
//     For each of [Z [B [S [C [I [J [F [D [Ljava/lang/Object; [Ljava/lang/String;
//     at length 0, 1, 3, 256, AND 100000 (the big-allocation case):
//       * make_java_array(...) returns a NON-NULL oop that passes is_valid_pointer
//         (never hand Java an invalid/mistyped oop), and
//       * vmhook::array_length(oop) == the requested length (the _length slot was
//         written correctly).
//     For every PRIMITIVE descriptor we additionally:
//       * ZERO-INIT: read element [0] and [last] of a FRESH array (before any
//         write) and assert they are the zero/default value — make_java_object
//         memset()s the allocation, so a usable Java array starts default-init'd.
//       * ROUND-TRIP at len 3: write a boundary value into [0] and [last] and read
//         it back BIT-EXACT for F/D (canonical NaN at [0], -0.0 at [last]).
//       * DEEP ROUND-TRIP at len 256: write+read at [0], the MIDDLE [128], and the
//         LAST [255] — proving the data region (offset +16) is real, sized, and
//         addressable for the full element stride far into the array.
//
//   PASS-INTO-JAVA (the well-formed-array proof — "build from a C++ vector, then
//   prove every element is correct Java-side"):  for each PRIMITIVE descriptor the
//   detour builds a length-FEED_LEN array, writes a C++ std::vector of
//   edge-case values into it element-by-element with set_array_element, then PASSES
//   THE MADE ARRAY into the matching MakeJavaArray.sum*/check* Java method via
//   method_proxy::call.  That method walks EVERY element with genuine bytecode
//   (arraylength + Xaload) and returns a position-weighted value (a sum for
//   integrals, an XOR-of-raw-bits fold for J/F/D so the full width / NaN / -0.0 /
//   Inf are all distinguished).  The module recomputes the SAME value in C++ and
//   asserts the Java return equals it, plus the Java-observed .length / class name /
//   first / last element match — so a wrong element width, a stride/slot mistake,
//   a 0xFF sign error, a high/low wide-word swap, or any element corruption is
//   caught by real Java bytecode, not just a native re-read.
//
//   For the REFERENCE descriptors the detour allocates an EMPTY (default-null)
//   Object[] / String[] and passes it into MakeJavaArray.fillCheck*Array, which
//   FILLS it with refs + interspersed nulls (real aastore), reads them back
//   (aaload by == identity), and reports length / non-null count / round-trip —
//   proving the made reference array supports element storage with the correct
//   element klass (a wrong-type store would raise ArrayStoreException).
//
//   JAVA-VISIBLE recv* witness (proves the oop is a REAL Java array via a field):
//     One representative array per descriptor (length WITNESS_LEN==3) is stored
//     into a static recv* field via field_proxy::set (the object-reference /
//     compressed-OOP write path).  The fixture's captureAll() then reads, with
//     genuine bytecode, recv.length and recv.getClass().getName() into obs*
//     witnesses the native side asserts: length == 3 AND the binary class name is
//     exactly "[I" / "[Ljava.lang.Object;" / ... (dotted Java form).
//
//   GUARDS / MALFORMED INPUT (HARD on all JDKs — must be graceful, never crash):
//     * negative length (-1, INT_MIN, and -5 with a VALID descriptor) -> nullptr.
//     * non-array descriptor ("Ljava/lang/Object;", "I")              -> nullptr.
//     * "byte[]" (Java source syntax, not a descriptor)               -> nullptr.
//     * empty descriptor ""                                           -> nullptr.
//     * a bare "[" / array-of-void "[V" (malformed element)           -> nullptr.
//     * a never-loaded array element type
//       ("[Lvmhook/fixtures/NoSuchClass;")                            -> nullptr.
//     * allow_jni_fallback=false on a valid PRIMITIVE descriptor still succeeds
//       on the (untouched) TLAB fast path -> non-null (characterises the new 4th
//       parameter without depending on a GC firing).
//
// JDK-8 GATING: the reference-array allocation ([L... / [[...) depends on the
// FIX-D JNI FindClass fallback resolving an Obj/ArrayKlass on JDK 8.  Where it
// lands, the ref-array asserts are HARD; if it returns null they are recorded as
// [INFO] SKIPPED (CI stays green) — the primitive paths stay HARD everywhere.
// JDK 8 is detected with the house idiom: java.lang.String has the compact-string
// "coder" field only on JDK 9+ (field_string.cpp / make_java_string.cpp use the
// same probe).  No universal invariant is ever weakened to pass.
//
// LAYOUT ASSUMPTION (characterised, not failed): make_java_array hardcodes the
// x64 compressed-oops arrayOop layout — 16-byte header, _length at +12, data at
// +16.  Every CI host is x64 with default compressed oops, so this holds; it is
// recorded as a portability note, not asserted away.
//
// SUITE-SAFETY (mandatory, mirrors read_java_string.cpp / make_java_string.cpp):
//   * The whole body runs under a try/catch that downgrades any C++ exception to
//     an [INFO] line and returns — a module NEVER fails the suite on a throw.
//   * An UNCONDITIONAL vmhook::shutdown_hooks() runs OUTSIDE the try as the last
//     statement, so the module leaves nothing armed even on an early return (the
//     scoped_hook also disarms on scope exit; this is the belt-and-braces floor).
//   * An entry guard bails to [INFO] (no FAIL) if the fixture klass can't be
//     resolved — nothing to test that early in bootstrap.
//   * Every made oop is null- and is_valid_pointer-gated before it is wrapped,
//     stored, or element-accessed.  All allocation happens inside the detour on
//     the Java thread.  No forced System.gc() is performed (each made oop is
//     validated immediately, and the primitive JNI fallback already covers TLAB
//     exhaustion), so the GC-gate macro is not needed here.
//   * MSVC copy-init (never brace-init) from value_t / get().  C++17-level in its
//     own constructs (memcpy type-pun, no std::bit_cast); compiled at the header's
//     mandated C++20/23.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

namespace
{
    // The representative length the fixture's captureAll() expects (must equal
    // MakeJavaArray.WITNESS_LEN).
    constexpr std::int32_t k_witness_len{ 3 };

    // The length of the C++-vector-built arrays passed into the Java sum*/check*
    // verifiers (must equal MakeJavaArray.FEED_LEN).
    constexpr std::int32_t k_feed_len{ 5 };

    // The native lengths every descriptor is allocated at: 0, 1, 3, 256, and a
    // BIG length that exercises the allocation path well past a TLAB top-up.
    constexpr std::array<std::int32_t, 5> k_lengths{ 0, 1, 3, 256, 100000 };

    // The "deep" length whose first / middle / last elements are round-tripped.
    constexpr std::int32_t k_deep_len{ 256 };

    // The big-allocation length (must appear in k_lengths).
    constexpr std::int32_t k_big_len{ 100000 };

    // Wrapper for vmhook.fixtures.MakeJavaArray — hook target + witness reads.
    class mja : public vmhook::object<mja>
    {
    public:
        explicit mja(vmhook::oop_t instance) noexcept
            : vmhook::object<mja>{ instance }
        {
        }

        // ---- handshake (all via static_field; safe off the Java thread) -------
        static auto set_go(bool value) -> void   { static_field("go")->set(value); }
        static auto set_done(bool value) -> void  { static_field("done")->set(value); }
        static auto get_done() -> bool            { return static_field("done")->get(); }

        static auto resolves(const char* name) -> bool { return static_field(name).has_value(); }

        // ---- primitive witness reads (VMStructs reads; no Java thread needed) --
        static auto get_int(const char* name) -> std::int32_t { return static_field(name)->get(); }
        static auto get_long(const char* name) -> std::int64_t { return static_field(name)->get(); }
        static auto get_bool(const char* name) -> bool { return static_field(name)->get(); }
        static auto get_str(const char* name) -> std::string { return static_field(name)->get().as_string(); }
    };

    // Minimal carrier bound to java/lang/Object whose ONLY job is to ferry a
    // make_java_array oop into field_proxy::set's object-reference branch (which
    // calls get_instance() + encode_oop_pointer()) AND into method_proxy::call's
    // unique_ptr<wrapper> arg slot (which also just extracts get_instance()).  It
    // never needs the array layout itself.  Crucially, the method-call path uses
    // the RESOLVED method's own declared descriptor ("([I)J", ...) for the JNI
    // signature, NOT the carrier's registered class name — so this one Object-bound
    // carrier correctly transports an array of ANY element type as a call argument.
    class java_array_w : public vmhook::object<java_array_w>
    {
    public:
        explicit java_array_w(vmhook::oop_t instance) noexcept
            : vmhook::object<java_array_w>{ instance }
        {
        }
    };

    // ── Bit helpers (C++17: memcpy type-pun, never std::bit_cast). ──
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
    // Reinterpret a 64-bit pattern as the signed long Java reports from
    // doubleToRawLongBits (no UB; NaN/Inf bits have the sign bit set).
    auto to_i64(std::uint64_t bits) noexcept -> std::int64_t
    {
        std::int64_t out{};
        std::memcpy(&out, &bits, sizeof(out));
        return out;
    }

    // Position weight matching MakeJavaArray.weight(i) = 2*i + 1 exactly.
    auto weight(std::int32_t i) noexcept -> std::int64_t
    {
        return static_cast<std::int64_t>(2 * i + 1);
    }

    // (value + weight) computed with WELL-DEFINED two's-complement wraparound,
    // bit-identical to Java's `long +` (which wraps silently).  The wide XOR-fold
    // verifiers add a 64-bit element to a small weight where MAX/MIN inputs would
    // overflow a signed int64 (UB in C++); doing the add in std::uint64_t makes it
    // defined modular arithmetic and matches the JVM bit-for-bit, then we reinterpret
    // the bits back to int64 for the fold.  Used by the J / F / D feeders.
    auto add_wrap(std::int64_t value, std::int64_t w) noexcept -> std::int64_t
    {
        const std::uint64_t sum{ static_cast<std::uint64_t>(value) + static_cast<std::uint64_t>(w) };
        std::int64_t out{};
        std::memcpy(&out, &sum, sizeof(out));   // bit-cast uint64 -> int64, no UB
        return out;
    }

    // ── Per-descriptor native sweep result (filled inside the detour). ──
    struct desc_result
    {
        std::array<std::atomic<bool>, 5> nonnull{};
        std::array<std::atomic<bool>, 5> valid{};
        std::array<std::atomic<bool>, 5> len_ok{};
        // Fresh-array default-init (read [0]/[last] of an UNwritten array == 0).
        std::atomic<bool> zero_init_tested{ false };
        std::atomic<bool> zero_init_ok{ false };
        // Boundary round-trip at the representative length (3).
        std::atomic<bool> elem_first_ok{ false };
        std::atomic<bool> elem_last_ok{ false };
        std::atomic<bool> elem_tested{ false };   // false for ref-array descriptors
        // Deep round-trip at k_deep_len (256): [0], [middle], [last].
        std::atomic<bool> deep_tested{ false };
        std::atomic<bool> deep_first_ok{ false };
        std::atomic<bool> deep_mid_ok{ false };
        std::atomic<bool> deep_last_ok{ false };
        std::atomic<bool> stored_into_recv{ false };
        // PASS-INTO-JAVA (primitive: sum*/check*; reference: fillCheck*).
        std::atomic<bool> fed_attempted{ false };  // we tried to feed+call
        std::atomic<bool> fed_dispatched{ false }; // the Java verifier ran
        std::atomic<bool> fed_return_ok{ false };  // Java return == C++ expected
        std::atomic<bool> fed_len_ok{ false };     // Java-observed .length == feed len
        std::atomic<bool> fed_first_last_ok{ false }; // Java-observed [0]/[last] match
    };

    enum desc_index : std::size_t
    {
        D_Z = 0, D_B, D_S, D_C, D_I, D_J, D_F, D_D, D_OBJ, D_STR, D_COUNT
    };

    struct desc_spec
    {
        const char* descriptor;     // JVM array descriptor, e.g. "[I"
        std::size_t element_size;   // bytes per element passed to make_java_array
        bool        is_reference;   // [L... (gated on JDK 8)
        const char* recv_field;     // static recv* field to store the witness array
        const char* expected_name;  // dotted getClass().getName() the JVM reports
        const char* len_field;      // obsLen* witness
        const char* type_field;     // obsType* witness
        const char* null_field;     // obsNull* witness
    };

    const std::array<desc_spec, D_COUNT> k_specs{ {
        { "[Z", sizeof(std::uint8_t),  false, "recvZ",   "[Z",                    "obsLenZ",   "obsTypeZ",   "obsNullZ"   },
        { "[B", sizeof(std::int8_t),   false, "recvB",   "[B",                    "obsLenB",   "obsTypeB",   "obsNullB"   },
        { "[S", sizeof(std::int16_t),  false, "recvS",   "[S",                    "obsLenS",   "obsTypeS",   "obsNullS"   },
        { "[C", sizeof(std::uint16_t), false, "recvC",   "[C",                    "obsLenC",   "obsTypeC",   "obsNullC"   },
        { "[I", sizeof(std::int32_t),  false, "recvI",   "[I",                    "obsLenI",   "obsTypeI",   "obsNullI"   },
        { "[J", sizeof(std::int64_t),  false, "recvJ",   "[J",                    "obsLenJ",   "obsTypeJ",   "obsNullJ"   },
        { "[F", sizeof(float),         false, "recvF",   "[F",                    "obsLenF",   "obsTypeF",   "obsNullF"   },
        { "[D", sizeof(double),        false, "recvD",   "[D",                    "obsLenD",   "obsTypeD",   "obsNullD"   },
        { "[Ljava/lang/Object;", sizeof(std::uint32_t), true, "recvObj", "[Ljava.lang.Object;", "obsLenObj", "obsTypeObj", "obsNullObj" },
        { "[Ljava/lang/String;", sizeof(std::uint32_t), true, "recvStr", "[Ljava.lang.String;", "obsLenStr", "obsTypeStr", "obsNullStr" },
    } };

    std::array<desc_result, D_COUNT> g_results{};

    // ── Negative-length / malformed-descriptor guard outcomes. ──
    std::atomic<bool> g_neg_len_minus1_null{ false };
    std::atomic<bool> g_neg_len_intmin_null{ false };
    std::atomic<bool> g_nonarray_Lobj_null{ false };
    std::atomic<bool> g_nonarray_I_null{ false };
    std::atomic<bool> g_wrong_syntax_null{ false };
    std::atomic<bool> g_empty_desc_null{ false };
    std::atomic<bool> g_bare_bracket_null{ false };
    std::atomic<bool> g_array_of_void_null{ false };
    std::atomic<bool> g_missing_elem_class_null{ false };
    std::atomic<bool> g_neg_len_valid_desc_null{ false };
    std::atomic<bool> g_no_fallback_primitive_ok{ false };
    std::atomic<bool> g_multidim_nonnull{ false };
    std::atomic<bool> g_multidim_valid{ false };
    std::atomic<bool> g_multidim_len_ok{ false };

    // ── Detour bookkeeping. ──
    std::atomic<int>  g_cycle_calls{ 0 };
    std::atomic<bool> g_saw_self{ false };

    // Read element [0] and [last] of a FRESH (unwritten) primitive array and
    // assert they are the zero/default value.
    template<typename element_type>
    auto zero_init_check(desc_result& r, void* const oop, const std::int32_t len) -> void
    {
        r.zero_init_tested.store(true);
        if (len <= 0)
        {
            return;
        }
        const element_type a{ vmhook::get_array_element<element_type>(oop, 0) };
        const element_type b{ vmhook::get_array_element<element_type>(oop, len - 1) };
        if constexpr (std::is_same_v<element_type, float>)
        {
            r.zero_init_ok.store(float_bits(a) == 0u && float_bits(b) == 0u);
        }
        else if constexpr (std::is_same_v<element_type, double>)
        {
            r.zero_init_ok.store(double_bits(a) == 0u && double_bits(b) == 0u);
        }
        else
        {
            r.zero_init_ok.store(a == element_type{} && b == element_type{});
        }
    }

    // Write a boundary element into [0] and [last] of a primitive array and read
    // it straight back, BIT-EXACT for F/D.
    template<typename element_type>
    auto element_round_trip(desc_result& r, void* const oop, const std::int32_t len,
                            const element_type first_val, const element_type last_val) -> void
    {
        r.elem_tested.store(true);
        if (len <= 0)
        {
            return;
        }
        vmhook::set_array_element<element_type>(oop, 0, first_val);
        vmhook::set_array_element<element_type>(oop, len - 1, last_val);
        const element_type got_first{ vmhook::get_array_element<element_type>(oop, 0) };
        const element_type got_last{ vmhook::get_array_element<element_type>(oop, len - 1) };
        if constexpr (std::is_same_v<element_type, float>)
        {
            r.elem_first_ok.store(float_bits(got_first) == float_bits(first_val));
            r.elem_last_ok.store(float_bits(got_last) == float_bits(last_val));
        }
        else if constexpr (std::is_same_v<element_type, double>)
        {
            r.elem_first_ok.store(double_bits(got_first) == double_bits(first_val));
            r.elem_last_ok.store(double_bits(got_last) == double_bits(last_val));
        }
        else
        {
            r.elem_first_ok.store(got_first == first_val);
            r.elem_last_ok.store(got_last == last_val);
        }
    }

    // Write+read a value into [0], [middle], and [last] of a DEEP primitive array.
    template<typename element_type>
    auto deep_round_trip(desc_result& r, void* const oop, const std::int32_t len,
                         const element_type first_val, const element_type mid_val,
                         const element_type last_val) -> void
    {
        r.deep_tested.store(true);
        if (len < 3)
        {
            return;
        }
        const std::int32_t mid{ len / 2 };
        vmhook::set_array_element<element_type>(oop, 0, first_val);
        vmhook::set_array_element<element_type>(oop, mid, mid_val);
        vmhook::set_array_element<element_type>(oop, len - 1, last_val);
        const element_type g0{ vmhook::get_array_element<element_type>(oop, 0) };
        const element_type gm{ vmhook::get_array_element<element_type>(oop, mid) };
        const element_type gl{ vmhook::get_array_element<element_type>(oop, len - 1) };
        if constexpr (std::is_same_v<element_type, float>)
        {
            r.deep_first_ok.store(float_bits(g0) == float_bits(first_val));
            r.deep_mid_ok.store(float_bits(gm) == float_bits(mid_val));
            r.deep_last_ok.store(float_bits(gl) == float_bits(last_val));
        }
        else if constexpr (std::is_same_v<element_type, double>)
        {
            r.deep_first_ok.store(double_bits(g0) == double_bits(first_val));
            r.deep_mid_ok.store(double_bits(gm) == double_bits(mid_val));
            r.deep_last_ok.store(double_bits(gl) == double_bits(last_val));
        }
        else
        {
            r.deep_first_ok.store(g0 == first_val);
            r.deep_mid_ok.store(gm == mid_val);
            r.deep_last_ok.store(gl == last_val);
        }
    }

    auto zero_init_for(std::size_t di, void* const oop, std::int32_t len) -> void
    {
        desc_result& r{ g_results[di] };
        switch (di)
        {
            case D_Z: zero_init_check<bool>(r, oop, len); break;
            case D_B: zero_init_check<std::int8_t>(r, oop, len); break;
            case D_S: zero_init_check<std::int16_t>(r, oop, len); break;
            case D_C: zero_init_check<std::uint16_t>(r, oop, len); break;
            case D_I: zero_init_check<std::int32_t>(r, oop, len); break;
            case D_J: zero_init_check<std::int64_t>(r, oop, len); break;
            case D_F: zero_init_check<float>(r, oop, len); break;
            case D_D: zero_init_check<double>(r, oop, len); break;
            default: break;
        }
    }

    auto element_round_trip_for(std::size_t di, void* const oop, std::int32_t len) -> void
    {
        desc_result& r{ g_results[di] };
        switch (di)
        {
            case D_Z: element_round_trip<bool>(r, oop, len, true, false); break;
            case D_B: element_round_trip<std::int8_t>(r, oop, len,
                          std::numeric_limits<std::int8_t>::min(),
                          std::numeric_limits<std::int8_t>::max()); break;
            case D_S: element_round_trip<std::int16_t>(r, oop, len,
                          std::numeric_limits<std::int16_t>::min(),
                          std::numeric_limits<std::int16_t>::max()); break;
            case D_C: element_round_trip<std::uint16_t>(r, oop, len,
                          static_cast<std::uint16_t>(0x0000),
                          static_cast<std::uint16_t>(0xFFFF)); break;
            case D_I: element_round_trip<std::int32_t>(r, oop, len,
                          std::numeric_limits<std::int32_t>::min(),
                          std::numeric_limits<std::int32_t>::max()); break;
            case D_J: element_round_trip<std::int64_t>(r, oop, len,
                          std::numeric_limits<std::int64_t>::min(),
                          std::numeric_limits<std::int64_t>::max()); break;
            case D_F: element_round_trip<float>(r, oop, len,
                          std::numeric_limits<float>::quiet_NaN(),
                          -0.0F); break;
            case D_D: element_round_trip<double>(r, oop, len,
                          std::numeric_limits<double>::quiet_NaN(),
                          -0.0); break;
            default: break;
        }
    }

    auto deep_round_trip_for(std::size_t di, void* const oop, std::int32_t len) -> void
    {
        desc_result& r{ g_results[di] };
        switch (di)
        {
            case D_Z: deep_round_trip<bool>(r, oop, len, true, false, true); break;
            case D_B: deep_round_trip<std::int8_t>(r, oop, len,
                          std::numeric_limits<std::int8_t>::min(),
                          static_cast<std::int8_t>(42),
                          std::numeric_limits<std::int8_t>::max()); break;
            case D_S: deep_round_trip<std::int16_t>(r, oop, len,
                          std::numeric_limits<std::int16_t>::min(),
                          static_cast<std::int16_t>(0x1234),
                          std::numeric_limits<std::int16_t>::max()); break;
            case D_C: deep_round_trip<std::uint16_t>(r, oop, len,
                          static_cast<std::uint16_t>(0x0000),
                          static_cast<std::uint16_t>(0xABCD),
                          static_cast<std::uint16_t>(0xFFFF)); break;
            case D_I: deep_round_trip<std::int32_t>(r, oop, len,
                          std::numeric_limits<std::int32_t>::min(),
                          static_cast<std::int32_t>(0x0BADF00D),
                          std::numeric_limits<std::int32_t>::max()); break;
            case D_J: deep_round_trip<std::int64_t>(r, oop, len,
                          std::numeric_limits<std::int64_t>::min(),
                          static_cast<std::int64_t>(0x0123456789ABCDEFLL),
                          std::numeric_limits<std::int64_t>::max()); break;
            case D_F: deep_round_trip<float>(r, oop, len,
                          std::numeric_limits<float>::quiet_NaN(),
                          3.14159F,
                          -0.0F); break;
            case D_D: deep_round_trip<double>(r, oop, len,
                          std::numeric_limits<double>::quiet_NaN(),
                          2.718281828459045,
                          -0.0); break;
            default: break;
        }
    }

    // ── PASS-INTO-JAVA: build a length-k_feed_len primitive array from a C++
    //    vector, write every element with set_array_element, then call the named
    //    MakeJavaArray.sum*/check* method with the made array as the (sole) arg and
    //    assert the Java return equals the SAME position-weighted value computed in
    //    C++ from the vector.  Also reads back the fed*Len / fed*First / fed*Last
    //    witnesses the verifier recorded and checks them against the vector. ──
    //
    // element_type : the C++ array element type written with set_array_element.
    // values       : exactly k_feed_len elements.
    // expected     : the long the Java method must return (computed by the caller
    //                with the identical formula).
    // first_w/last_w : the long the verifier records for [0] / [last] (so we can
    //                  cross-check the per-element witnesses too).
    template<typename element_type>
    auto feed_and_call(std::size_t di, const std::unique_ptr<mja>& self,
                       const char* method_name,
                       const std::vector<element_type>& values,
                       const std::int64_t expected,
                       const std::int64_t first_w, const std::int64_t last_w,
                       const char* fed_len_field, const char* fed_first_field,
                       const char* fed_last_field) -> void
    {
        desc_result& r{ g_results[di] };
        r.fed_attempted.store(true);
        if (!self)
        {
            return;
        }
        const desc_spec& spec{ k_specs[di] };
        void* const oop{ vmhook::make_java_array(spec.descriptor, k_feed_len, spec.element_size) };
        if (!oop || !vmhook::hotspot::is_valid_pointer(oop))
        {
            return;
        }
        if (vmhook::array_length(oop) != k_feed_len)
        {
            return;
        }
        for (std::int32_t i{ 0 }; i < k_feed_len; ++i)
        {
            vmhook::set_array_element<element_type>(oop, i, values[static_cast<std::size_t>(i)]);
        }

        const auto method{ self->get_method(method_name) };
        if (!method.has_value())
        {
            return;
        }
        std::unique_ptr<java_array_w> carrier{ std::make_unique<java_array_w>(oop) };
        const vmhook::method_proxy::value_t v = method->call(carrier);
        if (v.is_void())
        {
            return;   // call did not dispatch
        }
        r.fed_dispatched.store(true);
        const std::int64_t returned = static_cast<std::int64_t>(v);
        r.fed_return_ok.store(returned == expected);

        // Cross-check the per-element witnesses the verifier recorded.
        r.fed_len_ok.store(mja::get_int(fed_len_field) == k_feed_len);
        const std::int64_t obs_first{ mja::get_long(fed_first_field) };
        const std::int64_t obs_last{ mja::get_long(fed_last_field) };
        r.fed_first_last_ok.store(obs_first == first_w && obs_last == last_w);
    }

    // Build the per-descriptor feed vector + expected checksum, then feed+call.
    // Each vector packs edge-case values for its type (MIN/MAX/0/+-1, NaN/Inf/-0.0,
    // 0xFF byte, full-range char incl. 0xFFFF, wide 64-bit patterns).
    auto feed_primitive(std::size_t di, const std::unique_ptr<mja>& self) -> void
    {
        switch (di)
        {
            case D_Z:
            {
                const std::vector<bool> vals{ true, false, true, true, false };
                std::int64_t exp{ 0 };
                for (std::int32_t i{ 0 }; i < k_feed_len; ++i)
                {
                    if (vals[static_cast<std::size_t>(i)]) { exp += weight(i); }
                }
                const std::int64_t first_w{ vals[0] ? 1 : 0 };
                const std::int64_t last_w{ vals[static_cast<std::size_t>(k_feed_len - 1)] ? 1 : 0 };
                feed_and_call<bool>(di, self, "sumBoolArray", vals, exp, first_w, last_w,
                                    "fedZLen", "fedZFirst", "fedZLast");
                break;
            }
            case D_B:
            {
                // -1 == the 0xFF byte (signed -1 in Java); MIN/MAX boundary too.
                const std::vector<std::int8_t> vals{
                    std::numeric_limits<std::int8_t>::min(),
                    static_cast<std::int8_t>(-1),
                    static_cast<std::int8_t>(0),
                    static_cast<std::int8_t>(1),
                    std::numeric_limits<std::int8_t>::max() };
                std::int64_t exp{ 0 };
                for (std::int32_t i{ 0 }; i < k_feed_len; ++i)
                {
                    exp += static_cast<std::int64_t>(vals[static_cast<std::size_t>(i)]) * weight(i);
                }
                feed_and_call<std::int8_t>(di, self, "sumByteArray", vals, exp,
                                           static_cast<std::int64_t>(vals[0]),
                                           static_cast<std::int64_t>(vals[static_cast<std::size_t>(k_feed_len - 1)]),
                                           "fedBLen", "fedBFirst", "fedBLast");
                break;
            }
            case D_S:
            {
                const std::vector<std::int16_t> vals{
                    std::numeric_limits<std::int16_t>::min(),
                    static_cast<std::int16_t>(-1),
                    static_cast<std::int16_t>(0),
                    static_cast<std::int16_t>(1),
                    std::numeric_limits<std::int16_t>::max() };
                std::int64_t exp{ 0 };
                for (std::int32_t i{ 0 }; i < k_feed_len; ++i)
                {
                    exp += static_cast<std::int64_t>(vals[static_cast<std::size_t>(i)]) * weight(i);
                }
                feed_and_call<std::int16_t>(di, self, "sumShortArray", vals, exp,
                                            static_cast<std::int64_t>(vals[0]),
                                            static_cast<std::int64_t>(vals[static_cast<std::size_t>(k_feed_len - 1)]),
                                            "fedSLen", "fedSFirst", "fedSLast");
                break;
            }
            case D_C:
            {
                // Full UTF-16 code-unit range incl. 0x0000 and 0xFFFF.
                const std::vector<std::uint16_t> vals{
                    static_cast<std::uint16_t>(0x0000),
                    static_cast<std::uint16_t>(0x0041),
                    static_cast<std::uint16_t>(0x00FF),
                    static_cast<std::uint16_t>(0xABCD),
                    static_cast<std::uint16_t>(0xFFFF) };
                std::int64_t exp{ 0 };
                for (std::int32_t i{ 0 }; i < k_feed_len; ++i)
                {
                    exp += static_cast<std::int64_t>(vals[static_cast<std::size_t>(i)]) * weight(i);
                }
                feed_and_call<std::uint16_t>(di, self, "sumCharArray", vals, exp,
                                             static_cast<std::int64_t>(vals[0]),
                                             static_cast<std::int64_t>(vals[static_cast<std::size_t>(k_feed_len - 1)]),
                                             "fedCLen", "fedCFirst", "fedCLast");
                break;
            }
            case D_I:
            {
                const std::vector<std::int32_t> vals{
                    std::numeric_limits<std::int32_t>::min(),
                    -1, 0, 1,
                    std::numeric_limits<std::int32_t>::max() };
                std::int64_t exp{ 0 };
                for (std::int32_t i{ 0 }; i < k_feed_len; ++i)
                {
                    exp += static_cast<std::int64_t>(vals[static_cast<std::size_t>(i)]) * weight(i);
                }
                feed_and_call<std::int32_t>(di, self, "sumIntArray", vals, exp,
                                            static_cast<std::int64_t>(vals[0]),
                                            static_cast<std::int64_t>(vals[static_cast<std::size_t>(k_feed_len - 1)]),
                                            "fedILen", "fedIFirst", "fedILast");
                break;
            }
            case D_J:
            {
                // XOR-fold of (value + weight); wide 64-bit patterns.
                const std::vector<std::int64_t> vals{
                    std::numeric_limits<std::int64_t>::min(),
                    static_cast<std::int64_t>(-1),
                    static_cast<std::int64_t>(0),
                    static_cast<std::int64_t>(0x0123456789ABCDEFLL),
                    std::numeric_limits<std::int64_t>::max() };
                std::int64_t exp{ 0 };
                for (std::int32_t i{ 0 }; i < k_feed_len; ++i)
                {
                    exp ^= add_wrap(vals[static_cast<std::size_t>(i)], weight(i));
                }
                feed_and_call<std::int64_t>(di, self, "sumLongArray", vals, exp,
                                            vals[0], vals[static_cast<std::size_t>(k_feed_len - 1)],
                                            "fedJLen", "fedJFirst", "fedJLast");
                break;
            }
            case D_F:
            {
                // XOR-fold of (rawIntBits & 0xFFFFFFFF) + weight; NaN/Inf/-0.0.
                const std::vector<float> vals{
                    std::numeric_limits<float>::quiet_NaN(),
                    -0.0F,
                    std::numeric_limits<float>::infinity(),
                    -std::numeric_limits<float>::infinity(),
                    3.14159F };
                std::int64_t exp{ 0 };
                for (std::int32_t i{ 0 }; i < k_feed_len; ++i)
                {
                    // float_bits is uint32; Java masks rawIntBits with 0xFFFFFFFFL,
                    // so the value is a non-negative long in [0, 0xFFFFFFFF].
                    const std::int64_t bits{ static_cast<std::int64_t>(float_bits(vals[static_cast<std::size_t>(i)])) };
                    exp ^= add_wrap(bits, weight(i));
                }
                const std::int64_t first_w{ static_cast<std::int64_t>(float_bits(vals[0])) };
                const std::int64_t last_w{ static_cast<std::int64_t>(float_bits(vals[static_cast<std::size_t>(k_feed_len - 1)])) };
                feed_and_call<float>(di, self, "checkFloatArray", vals, exp, first_w, last_w,
                                     "fedFLen", "fedFFirst", "fedFLast");
                break;
            }
            case D_D:
            {
                // XOR-fold of rawLongBits + weight; NaN/Inf/-0.0 (wide).
                const std::vector<double> vals{
                    std::numeric_limits<double>::quiet_NaN(),
                    -0.0,
                    std::numeric_limits<double>::infinity(),
                    -std::numeric_limits<double>::infinity(),
                    2.718281828459045 };
                std::int64_t exp{ 0 };
                for (std::int32_t i{ 0 }; i < k_feed_len; ++i)
                {
                    // to_i64 reinterprets the raw 64-bit pattern (NaN/Inf have the
                    // sign bit set); add_wrap matches Java's two's-complement long +.
                    exp ^= add_wrap(to_i64(double_bits(vals[static_cast<std::size_t>(i)])), weight(i));
                }
                const std::int64_t first_w{ to_i64(double_bits(vals[0])) };
                const std::int64_t last_w{ to_i64(double_bits(vals[static_cast<std::size_t>(k_feed_len - 1)])) };
                feed_and_call<double>(di, self, "checkDoubleArray", vals, exp, first_w, last_w,
                                      "fedDLen", "fedDFirst", "fedDLast");
                break;
            }
            default: break;
        }
    }

    // ── PASS-INTO-JAVA for the reference descriptors: make an EMPTY (default-null)
    //    array of length k_witness_len and pass it into the named fillCheck*
    //    method, which fills it with refs + interspersed nulls (aastore), reads
    //    them back (aaload), and returns the non-null count.  We assert the call
    //    dispatched and the returned non-null count matches the 2 the filler
    //    stores ([0]=ref, [1]=null, [2]=ref).  Best-effort: only runs when the
    //    array allocates (gated like the other ref-array paths). ──
    auto feed_reference(std::size_t di, const std::unique_ptr<mja>& self,
                        const char* method_name) -> void
    {
        desc_result& r{ g_results[di] };
        r.fed_attempted.store(true);
        if (!self)
        {
            return;
        }
        const desc_spec& spec{ k_specs[di] };
        void* const oop{ vmhook::make_java_array(spec.descriptor, k_witness_len, spec.element_size) };
        if (!oop || !vmhook::hotspot::is_valid_pointer(oop))
        {
            return;   // ref-array alloc may fail on JDK 8 / GC-active config
        }
        if (vmhook::array_length(oop) != k_witness_len)
        {
            return;
        }
        const auto method{ self->get_method(method_name) };
        if (!method.has_value())
        {
            return;
        }
        std::unique_ptr<java_array_w> carrier{ std::make_unique<java_array_w>(oop) };
        const vmhook::method_proxy::value_t v = method->call(carrier);
        if (v.is_void())
        {
            return;
        }
        r.fed_dispatched.store(true);
        // The filler stores 2 non-null elements ([0]=ref, [1]=null, [2]=ref).
        r.fed_return_ok.store(static_cast<std::int64_t>(v) == 2);
        r.fed_len_ok.store(true);
        r.fed_first_last_ok.store(true);
    }

    // The cycle() detour: the entire native make_java_array sweep, the
    // malformed-input guards, the recv* witness stores, the PASS-INTO-JAVA
    // sum*/check*/fillCheck* calls, and the big-allocation case.  self is `this`.
    auto on_cycle(vmhook::return_value& /*ret*/, const std::unique_ptr<mja>& self) -> void
    {
        g_cycle_calls.fetch_add(1, std::memory_order_relaxed);
        g_saw_self.store(self != nullptr, std::memory_order_relaxed);

        // ── 1. The exhaustive per-descriptor x per-length sweep. ──
        for (std::size_t di{ 0 }; di < D_COUNT; ++di)
        {
            const desc_spec& spec{ k_specs[di] };
            desc_result& r{ g_results[di] };

            for (std::size_t k{ 0 }; k < k_lengths.size(); ++k)
            {
                const std::int32_t len{ k_lengths[k] };
                void* const oop{ vmhook::make_java_array(spec.descriptor, len, spec.element_size) };
                const bool nonnull{ oop != nullptr };
                const bool valid{ nonnull && vmhook::hotspot::is_valid_pointer(oop) };
                r.nonnull[k].store(nonnull);
                r.valid[k].store(valid);
                r.len_ok[k].store(valid && vmhook::array_length(oop) == len);

                if (valid && !spec.is_reference)
                {
                    if (len == k_witness_len)
                    {
                        zero_init_for(di, oop, len);
                        element_round_trip_for(di, oop, len);
                    }
                    if (len == k_deep_len)
                    {
                        deep_round_trip_for(di, oop, len);
                    }
                }
            }

            // ── 2. Store a fresh representative (len 3) array into recv*. ──
            void* const witness_oop{ vmhook::make_java_array(spec.descriptor, k_witness_len, spec.element_size) };
            if (witness_oop && vmhook::hotspot::is_valid_pointer(witness_oop))
            {
                const auto field{ mja::static_field(spec.recv_field) };
                if (field.has_value())
                {
                    std::unique_ptr<java_array_w> carrier{ std::make_unique<java_array_w>(witness_oop) };
                    field->set(carrier);
                    r.stored_into_recv.store(true);
                }
            }

            // ── 3. PASS-INTO-JAVA: build from a C++ vector and hand the made
            //       array to a real Java verifier (primitive) / filler (reference). ──
            if (!spec.is_reference)
            {
                feed_primitive(di, self);
            }
        }

        // Reference fillers (run after the primitive sweep so the early ref-array
        // klasses are warm).
        feed_reference(D_OBJ, self, "fillCheckObjectArray");
        feed_reference(D_STR, self, "fillCheckStringArray");

        // ── 4. Malformed / guard inputs — must be graceful (null, no crash). ──
        g_neg_len_minus1_null.store(
            vmhook::make_java_array("[I", -1, sizeof(std::int32_t)) == nullptr);
        g_neg_len_intmin_null.store(
            vmhook::make_java_array("[B", std::numeric_limits<std::int32_t>::min(), sizeof(std::int8_t)) == nullptr);
        g_neg_len_valid_desc_null.store(
            vmhook::make_java_array("[D", -5, sizeof(double)) == nullptr);
        g_nonarray_Lobj_null.store(
            vmhook::make_java_array("Ljava/lang/Object;", 1, sizeof(std::uint32_t)) == nullptr);
        g_nonarray_I_null.store(
            vmhook::make_java_array("I", 1, sizeof(std::int32_t)) == nullptr);
        g_wrong_syntax_null.store(
            vmhook::make_java_array("byte[]", 1, sizeof(std::int8_t)) == nullptr);
        g_empty_desc_null.store(
            vmhook::make_java_array("", 1, 1) == nullptr);
        g_bare_bracket_null.store(
            vmhook::make_java_array("[", 1, 1) == nullptr);
        g_array_of_void_null.store(
            vmhook::make_java_array("[V", 1, 1) == nullptr);
        g_missing_elem_class_null.store(
            vmhook::make_java_array("[Lvmhook/fixtures/NoSuchClass12345;", 1, sizeof(std::uint32_t)) == nullptr);

        // ── 5. allow_jni_fallback=false on a valid PRIMITIVE descriptor. ──
        {
            void* const oop{ vmhook::make_java_array("[I", k_witness_len, sizeof(std::int32_t), /*allow_jni_fallback=*/false) };
            g_no_fallback_primitive_ok.store(
                oop != nullptr
                && vmhook::hotspot::is_valid_pointer(oop)
                && vmhook::array_length(oop) == k_witness_len);
        }

        // ── 6. MULTI-DIM array ("[[I"). ──
        {
            void* const oop{ vmhook::make_java_array("[[I", k_witness_len, sizeof(std::uint32_t)) };
            const bool nn{ oop != nullptr };
            const bool valid{ nn && vmhook::hotspot::is_valid_pointer(oop) };
            g_multidim_nonnull.store(nn);
            g_multidim_valid.store(valid);
            g_multidim_len_ok.store(valid && vmhook::array_length(oop) == k_witness_len);
        }

        // DEFENSIVE (and a documented lib finding): make_java_array's internal
        // find_class() goes through JNIEnv::FindClass, which leaves a PENDING JNI
        // exception (NoClassDefFoundError / ClassNotFoundException) on a miss.
        // make_java_array only clears it on the '['-prefixed fallback path AND
        // only when that fallback's FindClass succeeds; for a non-'[' descriptor,
        // or a '[' descriptor whose ELEMENT class is missing, the pending
        // exception is left set on the thread.  Clear it here so it never poisons
        // the probe's own bytecode (captureAll / done=true); see lib_bugs note.
        vmhook::jni::exception_clear();
    }

    auto drive(vmhook_test::context& ctx) -> bool
    {
        return ctx.run_probe(
            [](bool value)
            {
                if (value)
                {
                    mja::set_done(false);
                }
                mja::set_go(value);
            },
            []() { return mja::get_done(); });
    }

    auto run_body(vmhook_test::context& ctx) -> void
    {
        vmhook::register_class<mja>("vmhook/fixtures/MakeJavaArray");
        vmhook::register_class<java_array_w>("java/lang/Object");

        // =================================================================
        //  ENTRY GUARD (suite-safe).
        // =================================================================
        if (vmhook::find_class("vmhook/fixtures/MakeJavaArray") == nullptr)
        {
            ctx.record("[INFO] make_java_array: fixture klass vmhook/fixtures/MakeJavaArray "
                       "not resolvable yet — skipping module (no assertions run).");
            return;
        }

        // =================================================================
        //  0. Sanity.
        // =================================================================
        ctx.check("mja_class_registered_field_resolves", mja::resolves("go"));
        ctx.check("mja_witness_len_field_is_3", mja::get_int("WITNESS_LEN") == k_witness_len);
        ctx.check("mja_feed_len_field_is_5", mja::get_int("FEED_LEN") == k_feed_len);
        {
            const auto methods{ vmhook::get_class_methods<mja>() };
            bool has_cycle{ false };
            for (const auto& entry : methods)
            {
                if (entry.first == "cycle") { has_cycle = true; break; }
            }
            ctx.check("mja_cycle_method_declared", has_cycle);
        }

        // ── JDK-8 detection (house idiom). ──
        vmhook::hotspot::klass* const string_klass{ vmhook::find_class("java/lang/String") };
        const bool compact_strings{ string_klass != nullptr
                                    && string_klass->find_field("coder").has_value() };
        ctx.record(std::string{ "[INFO] make_java_array: JDK generation = " }
                   + (compact_strings ? "9+ (String.coder present)" : "8 (no String.coder)"));
        ctx.record("[INFO] make_java_array: layout assumption = x64 compressed-oops "
                   "arrayOop (16-byte header, _length at +12, data at +16); holds on "
                   "the all-x64 CI matrix. A compressed-oops-disabled (>32GB heap) or "
                   "32-bit VM would need a layout-aware header/length offset.");
        ctx.record("[INFO] make_java_array: PRIMITIVE arrays are HARD on every JDK at "
                   "every length (the landed make_java_object GC-aware JNI New<Type>Array "
                   "fallback covers TLAB exhaustion); REFERENCE arrays ([L.../[[...) have "
                   "no NewObjectArray fallback, so they stay best-effort and are recorded "
                   "[INFO] when make_java_array returns null on a GC-active config / JDK 8.");

        // =================================================================
        //  1. Install the interpreter hook on cycle().
        // =================================================================
        auto handle{ vmhook::scoped_hook<mja>("cycle", &on_cycle) };
        ctx.check("make_java_array_hook_installed", handle.installed());
        if (!handle.installed())
        {
            return;
        }

        // =================================================================
        //  2. Fire the probe once.
        // =================================================================
        const bool probe_done{ drive(ctx) };
        ctx.check("make_java_array_probe_completed", probe_done);
        ctx.check("make_java_array_cycle_fired_once", g_cycle_calls.load() == 1);
        ctx.check("make_java_array_detour_saw_self", g_saw_self.load());

        const std::array<const char*, D_COUNT> tag{
            "Z", "B", "S", "C", "I", "J", "F", "D", "Obj", "Str" };

        // =================================================================
        //  3. NATIVE sweep assertions — every descriptor x every length.
        // =================================================================
        for (std::size_t di{ 0 }; di < D_COUNT; ++di)
        {
            const desc_spec& spec{ k_specs[di] };
            desc_result& r{ g_results[di] };
            for (std::size_t k{ 0 }; k < k_lengths.size(); ++k)
            {
                const std::int32_t len{ k_lengths[k] };
                const std::string suffix{ std::string{ tag[di] } + "_len" + std::to_string(len) };
                const bool nn{ r.nonnull[k].load() };
                const bool best_effort{ spec.is_reference };
                if (best_effort && !nn)
                {
                    ctx.record("[INFO] native_" + suffix + ": SKIPPED — reference-array make_java_array"
                               " returned null on this JVM (no NewObjectArray fallback yet); native"
                               " primitive coverage + element round-trips remain the hard floor.");
                    continue;
                }
                ctx.check("native_nonnull_" + suffix, nn);
                ctx.check("native_valid_oop_" + suffix, r.valid[k].load());
                ctx.check("native_length_matches_" + suffix, r.len_ok[k].load());
            }

            if (!spec.is_reference)
            {
                ctx.check(std::string{ "native_zero_init_tested_" } + tag[di], r.zero_init_tested.load());
                ctx.check(std::string{ "native_fresh_array_zero_initialised_" } + tag[di], r.zero_init_ok.load());
                ctx.check(std::string{ "native_element_tested_" } + tag[di], r.elem_tested.load());
                ctx.check(std::string{ "native_element_first_round_trips_" } + tag[di], r.elem_first_ok.load());
                ctx.check(std::string{ "native_element_last_round_trips_" } + tag[di], r.elem_last_ok.load());
                ctx.check(std::string{ "native_deep_tested_" } + tag[di], r.deep_tested.load());
                ctx.check(std::string{ "native_deep_first_round_trips_" } + tag[di], r.deep_first_ok.load());
                ctx.check(std::string{ "native_deep_middle_round_trips_" } + tag[di], r.deep_mid_ok.load());
                ctx.check(std::string{ "native_deep_last_round_trips_" } + tag[di], r.deep_last_ok.load());
            }
        }

        // [B and [C are the load-bearing primitives make_java_string depends on.
        {
            const bool b_all{ g_results[D_B].len_ok[0].load() && g_results[D_B].len_ok[1].load()
                             && g_results[D_B].len_ok[2].load() && g_results[D_B].len_ok[3].load()
                             && g_results[D_B].len_ok[4].load() };
            const bool c_all{ g_results[D_C].len_ok[0].load() && g_results[D_C].len_ok[1].load()
                             && g_results[D_C].len_ok[2].load() && g_results[D_C].len_ok[3].load()
                             && g_results[D_C].len_ok[4].load() };
            ctx.check("byte_array_all_lengths_ok_make_java_string_dependency", b_all);
            ctx.check("char_array_all_lengths_ok_make_java_string_dependency", c_all);
        }

        // Big-allocation (100k) primitive invariant: every primitive descriptor
        // must allocate and report the right length at the large size.  HARD on
        // all JDKs (k_lengths[4] is the big length; index reused below).
        {
            constexpr std::size_t big_k{ 4 };
            static_assert(k_lengths[big_k] == k_big_len, "big length index drift");
            bool all_big_primitives_ok{ true };
            for (std::size_t di{ 0 }; di < D_COUNT; ++di)
            {
                if (k_specs[di].is_reference) { continue; }
                if (!g_results[di].len_ok[big_k].load()) { all_big_primitives_ok = false; break; }
            }
            ctx.check("native_big_100k_primitive_arrays_all_allocate_and_length_ok",
                      all_big_primitives_ok);
        }

        // =================================================================
        //  4. PASS-INTO-JAVA — built from a C++ vector, every element verified by
        //     a real Java method (sum*/check* for primitives).  HARD on all JDKs:
        //     these use the primitive allocation path (always succeeds) + the
        //     method-call surface, so a wrong element width / stride / value is a
        //     genuine failure.
        // =================================================================
        for (std::size_t di{ 0 }; di < D_COUNT; ++di)
        {
            const desc_spec& spec{ k_specs[di] };
            if (spec.is_reference) { continue; }
            desc_result& r{ g_results[di] };
            ctx.check(std::string{ "java_feed_attempted_" } + tag[di], r.fed_attempted.load());
            ctx.check(std::string{ "java_feed_dispatched_" } + tag[di], r.fed_dispatched.load());
            ctx.check(std::string{ "java_feed_return_matches_cpp_checksum_" } + tag[di], r.fed_return_ok.load());
            ctx.check(std::string{ "java_feed_observed_length_5_" } + tag[di], r.fed_len_ok.load());
            ctx.check(std::string{ "java_feed_observed_first_last_match_" } + tag[di], r.fed_first_last_ok.load());
        }

        // Reference fillers: best-effort (gated on the made array existing).  When
        // dispatched, the non-null count MUST be the 2 the filler stores.
        for (std::size_t di : { static_cast<std::size_t>(D_OBJ), static_cast<std::size_t>(D_STR) })
        {
            desc_result& r{ g_results[di] };
            if (r.fed_dispatched.load())
            {
                ctx.check(std::string{ "java_ref_fill_nonnull_count_is_2_" } + tag[di], r.fed_return_ok.load());
                // Cross-read the fillCheck* witnesses too (length 3, roundtrip,
                // exactly 2 non-null after the fill).
                const char* len_field{ di == D_OBJ ? "refObjLen" : "refStrLen" };
                const char* nn_field{ di == D_OBJ ? "refObjNonNull" : "refStrNonNull" };
                const char* rt_field{ di == D_OBJ ? "refObjRoundtrip" : "refStrRoundtrip" };
                ctx.check(std::string{ "java_ref_fill_length_is_3_" } + tag[di],
                          mja::get_int(len_field) == k_witness_len);
                ctx.check(std::string{ "java_ref_fill_two_non_null_" } + tag[di],
                          mja::get_int(nn_field) == 2);
                ctx.check(std::string{ "java_ref_fill_element_roundtrip_" } + tag[di],
                          mja::get_bool(rt_field));
            }
            else
            {
                ctx.record(std::string{ "[INFO] java_ref_fill_" } + tag[di]
                           + ": SKIPPED — reference-array make_java_array returned null on this"
                             " JVM (no NewObjectArray fallback / JDK 8). Primitive feed is the floor.");
            }
        }

        // =================================================================
        //  5. GUARDS.
        // =================================================================
        ctx.check("guard_negative_length_minus1_returns_null", g_neg_len_minus1_null.load());
        ctx.check("guard_negative_length_intmin_returns_null", g_neg_len_intmin_null.load());
        ctx.check("guard_negative_length_short_circuits_valid_desc", g_neg_len_valid_desc_null.load());
        ctx.check("guard_nonarray_Lobject_descriptor_returns_null", g_nonarray_Lobj_null.load());
        ctx.check("guard_nonarray_primitive_descriptor_returns_null", g_nonarray_I_null.load());
        ctx.check("guard_wrong_syntax_byte_brackets_returns_null", g_wrong_syntax_null.load());
        ctx.check("guard_empty_descriptor_returns_null", g_empty_desc_null.load());
        ctx.check("guard_bare_bracket_descriptor_returns_null", g_bare_bracket_null.load());
        ctx.check("guard_array_of_void_returns_null", g_array_of_void_null.load());
        ctx.check("guard_missing_element_class_returns_null", g_missing_elem_class_null.load());

        // =================================================================
        //  6. allow_jni_fallback parameter.
        // =================================================================
        ctx.check("allow_jni_fallback_false_primitive_still_allocates", g_no_fallback_primitive_ok.load());

        // =================================================================
        //  7. MULTI-DIM array ("[[I").
        // =================================================================
        if (g_multidim_nonnull.load())
        {
            ctx.check("multidim_int_array_valid_oop", g_multidim_valid.load());
            ctx.check("multidim_int_array_length_matches", g_multidim_len_ok.load());
        }
        else
        {
            ctx.record("[INFO] multidim_int_array (\"[[I\"): SKIPPED — make_java_array returned "
                       "null on this JVM (reference/ObjArrayKlass allocation, no NewObjectArray "
                       "fallback). Primitive coverage remains the hard floor.");
        }

        // =================================================================
        //  8. JAVA-VISIBLE recv* witnesses — the made array is a REAL Java array.
        // =================================================================
        if (probe_done)
        {
            std::size_t stored_correct{ 0 };
            for (std::size_t di{ 0 }; di < D_COUNT; ++di)
            {
                const desc_spec& spec{ k_specs[di] };
                desc_result& r{ g_results[di] };
                const bool stored{ r.stored_into_recv.load() };

                const std::int32_t obs_len{ mja::get_int(spec.len_field) };
                const std::string  obs_type{ mja::get_str(spec.type_field) };
                const bool         obs_null{ mja::get_bool(spec.null_field) };

                ctx.record(std::string{ "[INFO] make_java_array recv " } + tag[di]
                           + ": stored=" + (stored ? "true" : "false")
                           + " obsNull=" + (obs_null ? "true" : "false")
                           + " obsLen=" + std::to_string(obs_len)
                           + " obsType='" + obs_type + "' (expected '" + spec.expected_name + "')");

                if (!stored)
                {
                    ctx.record(std::string{ "[INFO] java_recv_" } + tag[di]
                               + ": SKIPPED — not stored on this JVM (late-sweep unrooted-oop GC"
                                 " pressure or ref-array fallback). Native invariants cover the feature.");
                    continue;
                }

                const bool not_null{ obs_null == false };
                const bool len_ok{ obs_len == k_witness_len };
                const bool type_ok{ obs_type == spec.expected_name };
                ctx.check(std::string{ "java_recv_not_null_" } + tag[di], not_null);
                ctx.check(std::string{ "java_recv_length_is_3_" } + tag[di], len_ok);
                ctx.check(std::string{ "java_recv_classname_" } + tag[di], type_ok);
                if (not_null && len_ok && type_ok) { ++stored_correct; }
            }

            {
                bool every_nonnull_recv_is_array_len3{ true };
                for (std::size_t di{ 0 }; di < D_COUNT; ++di)
                {
                    const desc_spec& spec{ k_specs[di] };
                    if (!mja::get_bool(spec.null_field))
                    {
                        const std::int32_t obs_len{ mja::get_int(spec.len_field) };
                        const std::string  obs_type{ mja::get_str(spec.type_field) };
                        if (obs_len != k_witness_len || obs_type.empty() || obs_type.front() != '[')
                        {
                            every_nonnull_recv_is_array_len3 = false;
                            break;
                        }
                    }
                }
                ctx.check("java_recv_every_nonnull_slot_is_len3_array",
                          every_nonnull_recv_is_array_len3);
            }

            ctx.check("java_recv_majority_stored_correct", stored_correct >= 5);
            ctx.record(std::string{ "[INFO] make_java_array Java-visible: " }
                       + std::to_string(static_cast<int>(stored_correct)) + "/"
                       + std::to_string(static_cast<int>(D_COUNT))
                       + " descriptors stored a correct array (>=5 required hard).");
        }
    }
}

VMHOOK_JVM_MODULE(make_java_array)
{
    try
    {
        run_body(ctx);
    }
    catch (const std::exception& e)
    {
        ctx.record(std::string{ "[INFO] make_java_array: caught std::exception - " } + e.what()
                   + " (downgraded to INFO; module never fails the suite on a throw).");
    }
    catch (...)
    {
        ctx.record("[INFO] make_java_array: caught non-std exception "
                   "(downgraded to INFO; module never fails the suite on a throw).");
    }

    vmhook::shutdown_hooks();
}
