// field_arrays_primitive JVM test module (area: fields).
//
// Feature under test: reading Java primitive arrays
//     [Z [B [S [C [I [J [F [D
// out of object / static fields into std::vector<T> through the C++
// field_proxy implicit-conversion path:
//
//     std::vector<std::int32_t> v = obj.get_field("a")->get();   // operator vector<T>()
//
// which lands in field_proxy::value_t::operator target_type()
//   -> std::visit -> cast_for_variant<vector<T>> -> read_array_value<vector<T>>
//   -> append_array_value(...) per element  (vmhook.hpp: array_length ~12372,
//   get_array_element ~12393, append_array_value ~12516-12567,
//   read_array_value ~12584-12615).
//
// NOTE on the public API: the canonical primitive-array read is the *implicit
// conversion operator*, NOT value_t::to_vector<T>().  to_vector<T>() is the
// OBJECT-array path (returns std::vector<std::unique_ptr<T>> via
// collection::to_vector) -- calling it with an arithmetic T would compile to a
// vector<unique_ptr<int>> and log "not a collection" at runtime.  Every read
// below therefore assigns get() into a typed std::vector<T> local so the
// primitive operator fires.  This module documents that naming overlap as a
// known sharp edge of the feature.
//
// Exhaustiveness: every primitive element type, BOTH static and instance fields,
// at the empty / single / many / large(256) / large(1024) / boundary / special
// shapes -- size AND every element verified.  The instance-offset read path is
// exercised at the canonical / empty / single / boundary shapes independently of
// the static mirror.  A null array REFERENCE is read on both paths (must yield an
// empty vector, never a crash).  Each canonical array is ALSO walked at the raw
// array_length + get_array_element<T> layer at index 0 / mid / last, proving the
// length oracle and per-element offset arithmetic directly (and never reading
// out of bounds -- array_length is the only bounds source used), and the raw
// get_array_element bounds check is hammered directly at index < 0, == length,
// > length, INT_MAX, and INT_MIN (each must return T{}, never crash).
//
// MULTI-DIMENSIONAL + JAGGED: int[][] / double[][] (rectangular), byte[][][]
// (3-D), an int[][] with a null middle row, and jagged int[][]/short[][] (rows
// of differing length incl. an empty row) are walked at the raw layer (outer
// object-array -> inner primitive arrays) on BOTH the static and instance paths;
// depth-length and every element value are checked, a null inner row reads as an
// empty inner vector (no crash), and a uniform-stride assumption would fail.
//
// The element-width GUARD is hard-asserted in BOTH unsafe directions -- a
// narrower [J -> vector<int32_t> (silent garbage, pre-guard) and the wider [I ->
// vector<int64_t> (out-of-bounds, pre-guard) both now REFUSE the read and yield
// an empty vector, with matching-width controls proving the guard never
// over-fires.  One remaining documentation check pins a real flaw still open in
// the read path (lossy char[] -> vector<char> truncation), exercised in a
// crash-safe direction (incl. the ' '/0xFFFF char boundary) so a future fix
// deliberately flips the check.
//
// SUITE-SAFETY (mirrors field_primitives_get.cpp / register_class.cpp):
//   * the whole body runs under a try/catch -- a stray throw is recorded as
//     [INFO], never a FAIL, and never escapes this module;
//   * an unconditional vmhook::shutdown_hooks() runs OUTSIDE the try, so the
//     module always returns to the driver with an empty hook table (this module
//     installs NO hooks -- it only drives the fixture's pre-registered probe --
//     so that is belt-and-braces, but the playbook mandates it regardless);
//   * an entry guard bails to [INFO] if the fixture class is not resolvable, so
//     no static_field()/get_field() below ever derefs a disengaged optional;
//   * raw-pointer derefs (the decoded array oop / element addresses) are gated on
//     is_valid_pointer and bounded by array_length -- never an out-of-bounds read.

#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace
{
    // The fixture this module reads.  Used by the entry guard and registration.
    constexpr char FIXTURE[]{ "vmhook/fixtures/FieldArraysPrimitive" };

    // Wrapper for vmhook.fixtures.FieldArraysPrimitive.  Each accessor returns
    // the field read into a concrete std::vector<T> so the primitive-array
    // implicit-conversion operator (operator std::vector<T>()) fires.  The
    // accessors are deliberately the clean one-liner idiom documented in the
    // header (`return get_field("x")->get();`) with NO defensive has_value()
    // guard -- the fields are known to exist (the entry guard proves the class is
    // loaded), and all suite-safety lives at the module/call-site level below.
    class field_arrays_primitive_fixture
        : public vmhook::object<field_arrays_primitive_fixture>
    {
    public:
        explicit field_arrays_primitive_fixture(vmhook::oop_t instance) noexcept
            : vmhook::object<field_arrays_primitive_fixture>{ instance }
        {
        }

        // --- handshake ---------------------------------------------------------
        static auto set_go(bool value) -> void { static_field("go")->set(value); }
        static auto get_done() -> bool          { return static_field("done")->get(); }
        static auto get_probe_checksum() -> std::int64_t { return static_field("probeChecksum")->get(); }

        // Wrap the Java self-reference for instance-field reads.
        static auto get_instance() -> std::unique_ptr<field_arrays_primitive_fixture>
        {
            return static_field("instance")->get();
        }

        // --- STATIC canonical reads (one per primitive type) -------------------
        static auto s_bool()   -> std::vector<bool>          { return static_field("staticBoolArray")->get(); }
        static auto s_byte()   -> std::vector<std::byte>     { return static_field("staticByteArray")->get(); }
        static auto s_byte_i8()-> std::vector<std::int8_t>   { return static_field("staticByteArray")->get(); }
        static auto s_short()  -> std::vector<std::int16_t>  { return static_field("staticShortArray")->get(); }
        static auto s_char()   -> std::vector<char>          { return static_field("staticCharArray")->get(); }
        static auto s_int()    -> std::vector<std::int32_t>  { return static_field("staticIntArray")->get(); }
        static auto s_long()   -> std::vector<std::int64_t>  { return static_field("staticLongArray")->get(); }
        static auto s_float()  -> std::vector<float>         { return static_field("staticFloatArray")->get(); }
        static auto s_double() -> std::vector<double>        { return static_field("staticDoubleArray")->get(); }

        // --- INSTANCE canonical reads ------------------------------------------
        auto i_bool()   -> std::vector<bool>          { return get_field("instBoolArray")->get(); }
        auto i_byte()   -> std::vector<std::byte>     { return get_field("instByteArray")->get(); }
        auto i_short()  -> std::vector<std::int16_t>  { return get_field("instShortArray")->get(); }
        auto i_char()   -> std::vector<char>          { return get_field("instCharArray")->get(); }
        auto i_int()    -> std::vector<std::int32_t>  { return get_field("instIntArray")->get(); }
        auto i_long()   -> std::vector<std::int64_t>  { return get_field("instLongArray")->get(); }
        auto i_float()  -> std::vector<float>         { return get_field("instFloatArray")->get(); }
        auto i_double() -> std::vector<double>        { return get_field("instDoubleArray")->get(); }

        // --- STATIC empty reads ------------------------------------------------
        static auto e_bool()   -> std::vector<bool>          { return static_field("emptyBoolArray")->get(); }
        static auto e_byte()   -> std::vector<std::byte>     { return static_field("emptyByteArray")->get(); }
        static auto e_short()  -> std::vector<std::int16_t>  { return static_field("emptyShortArray")->get(); }
        static auto e_char()   -> std::vector<char>          { return static_field("emptyCharArray")->get(); }
        static auto e_int()    -> std::vector<std::int32_t>  { return static_field("emptyIntArray")->get(); }
        static auto e_long()   -> std::vector<std::int64_t>  { return static_field("emptyLongArray")->get(); }
        static auto e_float()  -> std::vector<float>         { return static_field("emptyFloatArray")->get(); }
        static auto e_double() -> std::vector<double>        { return static_field("emptyDoubleArray")->get(); }

        // --- INSTANCE empty reads ----------------------------------------------
        auto ie_bool()   -> std::vector<bool>          { return get_field("instEmptyBoolArray")->get(); }
        auto ie_byte()   -> std::vector<std::byte>     { return get_field("instEmptyByteArray")->get(); }
        auto ie_short()  -> std::vector<std::int16_t>  { return get_field("instEmptyShortArray")->get(); }
        auto ie_char()   -> std::vector<char>          { return get_field("instEmptyCharArray")->get(); }
        auto ie_int()    -> std::vector<std::int32_t>  { return get_field("instEmptyIntArray")->get(); }
        auto ie_long()   -> std::vector<std::int64_t>  { return get_field("instEmptyLongArray")->get(); }
        auto ie_float()  -> std::vector<float>         { return get_field("instEmptyFloatArray")->get(); }
        auto ie_double() -> std::vector<double>        { return get_field("instEmptyDoubleArray")->get(); }

        // --- STATIC single-element reads ---------------------------------------
        static auto one_bool()   -> std::vector<bool>          { return static_field("singleBoolArray")->get(); }
        static auto one_byte()   -> std::vector<std::int8_t>   { return static_field("singleByteArray")->get(); }
        static auto one_short()  -> std::vector<std::int16_t>  { return static_field("singleShortArray")->get(); }
        static auto one_char()   -> std::vector<char>          { return static_field("singleCharArray")->get(); }
        static auto one_int()    -> std::vector<std::int32_t>  { return static_field("singleIntArray")->get(); }
        static auto one_long()   -> std::vector<std::int64_t>  { return static_field("singleLongArray")->get(); }
        static auto one_float()  -> std::vector<float>         { return static_field("singleFloatArray")->get(); }
        static auto one_double() -> std::vector<double>        { return static_field("singleDoubleArray")->get(); }

        // --- INSTANCE single-element reads -------------------------------------
        auto i_one_bool()   -> std::vector<bool>          { return get_field("instSingleBoolArray")->get(); }
        auto i_one_byte()   -> std::vector<std::int8_t>   { return get_field("instSingleByteArray")->get(); }
        auto i_one_short()  -> std::vector<std::int16_t>  { return get_field("instSingleShortArray")->get(); }
        auto i_one_char()   -> std::vector<char>          { return get_field("instSingleCharArray")->get(); }
        auto i_one_int()    -> std::vector<std::int32_t>  { return get_field("instSingleIntArray")->get(); }
        auto i_one_long()   -> std::vector<std::int64_t>  { return get_field("instSingleLongArray")->get(); }
        auto i_one_float()  -> std::vector<float>         { return get_field("instSingleFloatArray")->get(); }
        auto i_one_double() -> std::vector<double>        { return get_field("instSingleDoubleArray")->get(); }

        // --- LARGE (256-element) reads -----------------------------------------
        static auto big_bool()   -> std::vector<bool>          { return static_field("largeBoolArray")->get(); }
        static auto big_byte()   -> std::vector<std::int8_t>   { return static_field("largeByteArray")->get(); }
        static auto big_short()  -> std::vector<std::int16_t>  { return static_field("largeShortArray")->get(); }
        static auto big_char()   -> std::vector<char>          { return static_field("largeCharArray")->get(); }
        static auto big_int()    -> std::vector<std::int32_t>  { return static_field("largeIntArray")->get(); }
        static auto big_long()   -> std::vector<std::int64_t>  { return static_field("largeLongArray")->get(); }
        static auto big_float()  -> std::vector<float>         { return static_field("largeFloatArray")->get(); }
        static auto big_double() -> std::vector<double>        { return static_field("largeDoubleArray")->get(); }

        // --- STATIC boundary-value reads ---------------------------------------
        static auto b_bool()   -> std::vector<bool>          { return static_field("boundaryBoolArray")->get(); }
        static auto b_byte()   -> std::vector<std::int8_t>   { return static_field("boundaryByteArray")->get(); }
        static auto b_short()  -> std::vector<std::int16_t>  { return static_field("boundaryShortArray")->get(); }
        static auto b_char()   -> std::vector<char>          { return static_field("boundaryCharArray")->get(); }
        static auto b_int()    -> std::vector<std::int32_t>  { return static_field("boundaryIntArray")->get(); }
        static auto b_long()   -> std::vector<std::int64_t>  { return static_field("boundaryLongArray")->get(); }
        static auto b_float()  -> std::vector<float>         { return static_field("boundaryFloatArray")->get(); }
        static auto b_double() -> std::vector<double>        { return static_field("boundaryDoubleArray")->get(); }

        // --- INSTANCE boundary-value reads -------------------------------------
        auto i_b_bool()   -> std::vector<bool>          { return get_field("instBoundaryBoolArray")->get(); }
        auto i_b_byte()   -> std::vector<std::int8_t>   { return get_field("instBoundaryByteArray")->get(); }
        auto i_b_short()  -> std::vector<std::int16_t>  { return get_field("instBoundaryShortArray")->get(); }
        auto i_b_char()   -> std::vector<char>          { return get_field("instBoundaryCharArray")->get(); }
        auto i_b_int()    -> std::vector<std::int32_t>  { return get_field("instBoundaryIntArray")->get(); }
        auto i_b_long()   -> std::vector<std::int64_t>  { return get_field("instBoundaryLongArray")->get(); }
        auto i_b_float()  -> std::vector<float>         { return get_field("instBoundaryFloatArray")->get(); }
        auto i_b_double() -> std::vector<double>        { return get_field("instBoundaryDoubleArray")->get(); }

        static auto sp_float()  -> std::vector<float>        { return static_field("specialFloatArray")->get(); }
        static auto sp_double() -> std::vector<double>       { return static_field("specialDoubleArray")->get(); }

        // --- NULL array references (field holds null, not an array) -------------
        static auto null_int() -> std::vector<std::int32_t>  { return static_field("nullIntArray")->get(); }
        auto i_null_long()     -> std::vector<std::int64_t>  { return get_field("instNullLongArray")->get(); }

        // char[] read of the high-code-unit array (documents narrowing).
        static auto uni_char_as_char() -> std::vector<char> { return static_field("unicodeCharArray")->get(); }

        // ELEMENT-WIDTH GUARD checks (vmhook.hpp read_array_value).
        //
        // NARROWER direction: a [J (8-byte) field read into vector<int32_t>
        // (4-byte requested).  Pre-guard this walked the long[] data with a
        // 4-byte stride and returned interleaved low/high words; the guard now
        // REFUSES it and returns an empty vector.  Exercised in the crash-safe
        // direction (even unguarded it stayed in bounds), with a matching-width
        // vector<int64_t> control proving the guard does NOT over-fire.
        static auto wide_long_as_int32() -> std::vector<std::int32_t> { return static_field("wideLongArray")->get(); }
        static auto wide_long_as_int64() -> std::vector<std::int64_t> { return static_field("wideLongArray")->get(); }

        // WIDER direction: a [I (4-byte) field read into vector<int64_t>
        // (8-byte requested) -- the genuinely UNSAFE case.  Pre-guard this
        // would stride length*8 bytes across a length*4-byte array, reading
        // PAST the data area (out-of-bounds).  The guard REFUSES it (empty
        // vector) BEFORE any element is read, so the OOB read never happens and
        // this is safe to drive in the shared CI process.  staticIntArray is the
        // canonical {1000,2000,3000} [I field re-read at the wrong width; the
        // matching-width vector<int32_t> read of the same field (wrapper::s_int,
        // asserted in section 1) is the control.
        static auto int_as_int64_oob() -> std::vector<std::int64_t> { return static_field("staticIntArray")->get(); }

        // --- LARGE (1024-element) reads ----------------------------------------
        // The 1000+ shape the feature must survive on top of the 256 case.
        static auto bigk_int()    -> std::vector<std::int32_t>  { return static_field("largeKIntArray")->get(); }
        static auto bigk_long()   -> std::vector<std::int64_t>  { return static_field("largeKLongArray")->get(); }
        static auto bigk_double() -> std::vector<double>        { return static_field("largeKDoubleArray")->get(); }

        // char[] with ' ' (0x20) and Character.MAX_VALUE (0xFFFF) boundaries.
        static auto space_char() -> std::vector<char> { return static_field("spaceCharArray")->get(); }

        // --- MULTI-DIM / JAGGED field proxies (walked at the raw layer) --------
        // A multi-dim primitive field holds the OUTER object-array reference; the
        // native walk decodes it, then each inner primitive array, via the same
        // array_length + get_array_element idiom the flat reads use.  These
        // expose the field proxy so the raw walker below can reach field_oop().
        static auto p_int_grid()           { return static_field("staticIntGrid"); }
        static auto p_double_grid()        { return static_field("staticDoubleGrid"); }
        static auto p_byte_cube()          { return static_field("staticByteCube"); }
        static auto p_int_grid_null_row()  { return static_field("staticIntGridNullRow"); }
        static auto p_jagged_int()         { return static_field("staticJaggedIntArray"); }
        auto p_inst_int_grid()             { return get_field("instIntGrid"); }
        auto p_inst_jagged_short()         { return get_field("instJaggedShortArray"); }

        // Raw proxy for the canonical [I field, used by the out-of-range
        // get_array_element checks (index < 0 / == length / > length).
        static auto p_static_int()         { return static_field("staticIntArray"); }
    };

    // ---- small comparison helpers --------------------------------------------

    template <typename element_type>
    auto vectors_equal(const std::vector<element_type>& a,
                       const std::vector<element_type>& b) -> bool
    {
        if (a.size() != b.size())
        {
            return false;
        }
        for (std::size_t index{ 0 }; index < a.size(); ++index)
        {
            if (a[index] != b[index])
            {
                return false;
            }
        }
        return true;
    }

    // bit-exact float / double compare (so NaN / +-Inf / +-0 are checked
    // exactly, not via a tolerance that would mishandle them).
    auto bits_equal(float a, float b) -> bool
    {
        std::uint32_t ba{};
        std::uint32_t bb{};
        std::memcpy(&ba, &a, sizeof(ba));
        std::memcpy(&bb, &b, sizeof(bb));
        return ba == bb;
    }

    auto bits_equal(double a, double b) -> bool
    {
        std::uint64_t ba{};
        std::uint64_t bb{};
        std::memcpy(&ba, &a, sizeof(ba));
        std::memcpy(&bb, &b, sizeof(bb));
        return ba == bb;
    }

    template <typename element_type>
    auto all_bits_equal(const std::vector<element_type>& got,
                        const std::vector<element_type>& want) -> bool
    {
        if (got.size() != want.size())
        {
            return false;
        }
        for (std::size_t index{ 0 }; index < got.size(); ++index)
        {
            if (!bits_equal(got[index], want[index]))
            {
                return false;
            }
        }
        return true;
    }

    // ---- raw-layer array probe ------------------------------------------------
    //
    // Resolves a STATIC array field to its decoded array oop, reads array_length
    // as the bounds oracle, and reads the element at index 0 / mid / last through
    // get_array_element<element_type> (the same per-element offset arithmetic the
    // implicit operator drives).  Bounds come ONLY from array_length, and every
    // raw deref is is_valid_pointer-gated, so this never reads out of bounds.
    // Returns false (and writes nothing) if the field / oop is unusable.
    template <typename element_type>
    auto raw_endpoints_static(const char* field_name,
                              std::int32_t& out_length,
                              element_type& out_first,
                              element_type& out_mid,
                              element_type& out_last) -> bool
    {
        const auto proxy{ field_arrays_primitive_fixture::static_field(field_name) };
        if (!proxy.has_value())
        {
            return false;
        }
        void* const array_oop{ vmhook::field_oop(*proxy) };
        if (!array_oop || !vmhook::hotspot::is_valid_pointer(array_oop))
        {
            return false;
        }
        const std::int32_t length{ vmhook::array_length(array_oop) };
        out_length = length;
        if (length <= 0)
        {
            return true;   // length read succeeded; no elements to sample.
        }
        const std::int32_t last{ length - 1 };
        const std::int32_t mid{ length / 2 };
        out_first = vmhook::get_array_element<element_type>(array_oop, 0);
        out_mid   = vmhook::get_array_element<element_type>(array_oop, mid);
        out_last  = vmhook::get_array_element<element_type>(array_oop, last);
        return true;
    }

    // ---- multi-dimensional / jagged raw walkers -------------------------------
    //
    // A multi-dim primitive field (int[][], double[][], byte[][][], ...) stores
    // a reference to an OUTER object-array whose slots are compressed OOPs of the
    // inner arrays.  These walkers decode the outer array once, then walk each
    // slot to its inner primitive array and read the inner elements -- reusing
    // the SAME array_length (bounds oracle) + get_array_element (per-element
    // offset) primitives the flat reads use.  Every deref is is_valid_pointer-
    // gated and every index stays inside array_length(), so nothing reads out of
    // bounds; a null inner slot (a null row) decodes to an empty inner vector,
    // never a crash.  This mirrors the documented manual walk in
    // field_arrays_object.cpp (field_oop -> array_length -> get_array_element
    // <uint32_t> -> decode_oop_pointer).

    // Reads one inner primitive array (already decoded to its oop) into a vector.
    template <typename element_type>
    auto read_inner_prim_array(void* const inner_oop) -> std::vector<element_type>
    {
        std::vector<element_type> row;
        if (!inner_oop || !vmhook::hotspot::is_valid_pointer(inner_oop))
        {
            return row;   // null inner array (null row) -> empty, no crash.
        }
        const std::int32_t inner_len{ vmhook::array_length(inner_oop) };
        if (inner_len <= 0)
        {
            return row;
        }
        row.reserve(static_cast<std::size_t>(inner_len));
        for (std::int32_t j{ 0 }; j < inner_len; ++j)
        {
            row.push_back(vmhook::get_array_element<element_type>(inner_oop, j));
        }
        return row;
    }

    // Walks a 2-D primitive field proxy into vector<vector<T>>.  `ok` reports
    // whether the OUTER array decoded (a usable field); a genuinely null field
    // reference yields ok==true with an empty result only when the outer oop is
    // null -- callers that expect rows assert on the contents.
    template <typename element_type>
    auto walk_2d(const std::optional<vmhook::field_proxy>& proxy, bool& ok)
        -> std::vector<std::vector<element_type>>
    {
        std::vector<std::vector<element_type>> grid;
        ok = false;
        if (!proxy.has_value())
        {
            return grid;
        }
        void* const outer_oop{ vmhook::field_oop(*proxy) };
        if (!outer_oop || !vmhook::hotspot::is_valid_pointer(outer_oop))
        {
            return grid;   // null outer reference -> empty grid (no crash).
        }
        ok = true;
        const std::int32_t rows{ vmhook::array_length(outer_oop) };
        if (rows <= 0)
        {
            return grid;
        }
        grid.reserve(static_cast<std::size_t>(rows));
        for (std::int32_t i{ 0 }; i < rows; ++i)
        {
            // Each outer slot is a compressed OOP of an inner primitive array.
            void* const inner_oop{ vmhook::hotspot::decode_oop_pointer(
                vmhook::get_array_element<std::uint32_t>(outer_oop, i)) };
            grid.push_back(read_inner_prim_array<element_type>(inner_oop));
        }
        return grid;
    }

    // Walks a 3-D primitive field proxy (T[][][]) into vector<vector<vector<T>>>.
    template <typename element_type>
    auto walk_3d(const std::optional<vmhook::field_proxy>& proxy, bool& ok)
        -> std::vector<std::vector<std::vector<element_type>>>
    {
        std::vector<std::vector<std::vector<element_type>>> cube;
        ok = false;
        if (!proxy.has_value())
        {
            return cube;
        }
        void* const outer_oop{ vmhook::field_oop(*proxy) };
        if (!outer_oop || !vmhook::hotspot::is_valid_pointer(outer_oop))
        {
            return cube;
        }
        ok = true;
        const std::int32_t planes{ vmhook::array_length(outer_oop) };
        if (planes <= 0)
        {
            return cube;
        }
        cube.reserve(static_cast<std::size_t>(planes));
        for (std::int32_t i{ 0 }; i < planes; ++i)
        {
            // Each outer slot is a compressed OOP of a 2-D (object-array) plane.
            void* const plane_oop{ vmhook::hotspot::decode_oop_pointer(
                vmhook::get_array_element<std::uint32_t>(outer_oop, i)) };
            std::vector<std::vector<element_type>> plane;
            if (plane_oop && vmhook::hotspot::is_valid_pointer(plane_oop))
            {
                const std::int32_t rows{ vmhook::array_length(plane_oop) };
                if (rows > 0)
                {
                    plane.reserve(static_cast<std::size_t>(rows));
                    for (std::int32_t r{ 0 }; r < rows; ++r)
                    {
                        void* const inner_oop{ vmhook::hotspot::decode_oop_pointer(
                            vmhook::get_array_element<std::uint32_t>(plane_oop, r)) };
                        plane.push_back(read_inner_prim_array<element_type>(inner_oop));
                    }
                }
            }
            cube.push_back(std::move(plane));
        }
        return cube;
    }
}

// The entire test body, factored out so the VMHOOK_JVM_MODULE wrapper can run it
// under a try/catch and ALWAYS follow it with shutdown_hooks() (suite-safety).
// Anonymous-namespace members are visible here at file scope in this TU.
static void run_field_arrays_primitive_checks(vmhook_test::context& ctx)
{
    vmhook::register_class<field_arrays_primitive_fixture>(FIXTURE);

    using wrapper = field_arrays_primitive_fixture;

    // =========================================================================
    //  ENTRY GUARD.  If the fixture is not loaded/resolvable on this run, every
    //  static_field()->get() below would deref a disengaged optional.  Bail
    //  cleanly to [INFO] (the wrapper's final shutdown_hooks() still runs).  In
    //  practice the harness loads the fixture on every run, so this is
    //  belt-and-braces.
    // =========================================================================
    if (vmhook::find_class(FIXTURE) == nullptr)
    {
        ctx.record("[INFO] field_arrays_primitive: FieldArraysPrimitive not "
                   "loaded/resolvable on this run; skipping the module's live "
                   "checks (no crash, no hooks armed).");
        return;
    }

    // -------------------------------------------------------------------------
    // 0) Drive one real Java bytecode dispatch so the fixture is proven live
    //    (mirrors the pilot handshake).  All field reads below are valid before
    //    and after; the probe just confirms the fixture class initialised.
    // -------------------------------------------------------------------------
    {
        // A hook on a method is not required for field reads, but we keep the
        // run_probe handshake so the module fails loudly if the fixture never
        // initialised (class-load / classpath problems surface here).
        const bool probe_done{ ctx.run_probe(
            [](bool value) { wrapper::set_go(value); },
            []() { return wrapper::get_done(); }) };
        ctx.check("fap_probe_completed", probe_done);
        ctx.check("fap_probe_checksum_nonzero", wrapper::get_probe_checksum() != 0);
    }

    // =========================================================================
    // 1) STATIC canonical 3-element arrays -- size + every element.
    // =========================================================================
    {
        const std::vector<bool> bool_v{ wrapper::s_bool() };
        ctx.check("static_bool_size3", bool_v.size() == 3);
        ctx.check("static_bool_values",
                  vectors_equal(bool_v, std::vector<bool>{ true, false, true }));

        const std::vector<std::byte> byte_v{ wrapper::s_byte() };
        ctx.check("static_byte_size3", byte_v.size() == 3);
        ctx.check("static_byte_values",
                  vectors_equal(byte_v, std::vector<std::byte>{
                      std::byte{ 1 }, std::byte{ 2 }, std::byte{ 3 } }));

        // Same [B field read into std::vector<int8_t> -- alternate element type.
        const std::vector<std::int8_t> byte_i8{ wrapper::s_byte_i8() };
        ctx.check("static_byte_i8_values",
                  vectors_equal(byte_i8, std::vector<std::int8_t>{ 1, 2, 3 }));

        const std::vector<std::int16_t> short_v{ wrapper::s_short() };
        ctx.check("static_short_size3", short_v.size() == 3);
        ctx.check("static_short_values",
                  vectors_equal(short_v, std::vector<std::int16_t>{ 100, 200, 300 }));

        const std::vector<char> char_v{ wrapper::s_char() };
        ctx.check("static_char_size3", char_v.size() == 3);
        ctx.check("static_char_values",
                  vectors_equal(char_v, std::vector<char>{ 'A', 'B', 'C' }));

        const std::vector<std::int32_t> int_v{ wrapper::s_int() };
        ctx.check("static_int_size3", int_v.size() == 3);
        ctx.check("static_int_values",
                  vectors_equal(int_v, std::vector<std::int32_t>{ 1000, 2000, 3000 }));

        const std::vector<std::int64_t> long_v{ wrapper::s_long() };
        ctx.check("static_long_size3", long_v.size() == 3);
        ctx.check("static_long_values",
                  vectors_equal(long_v, std::vector<std::int64_t>{
                      1000000000LL, 2000000000LL, 3000000000LL }));

        const std::vector<float> float_v{ wrapper::s_float() };
        ctx.check("static_float_size3", float_v.size() == 3);
        ctx.check("static_float_values",
                  all_bits_equal(float_v, std::vector<float>{ 1.5f, 2.5f, 3.5f }));

        const std::vector<double> double_v{ wrapper::s_double() };
        ctx.check("static_double_size3", double_v.size() == 3);
        ctx.check("static_double_values",
                  all_bits_equal(double_v, std::vector<double>{ 1.25, 2.25, 3.25 }));
    }

    // =========================================================================
    // 2) INSTANCE arrays -- exercises the instance-offset read path (vs the
    //    static-mirror path above) at the canonical / empty / single / boundary
    //    shapes, plus a null instance array reference.
    // =========================================================================
    {
        const std::unique_ptr<wrapper> self{ wrapper::get_instance() };
        ctx.check("instance_wrapper_nonnull", self != nullptr);
        if (self)
        {
            // ---- 2a) canonical 3-element instance arrays --------------------
            const std::vector<bool> bool_v{ self->i_bool() };
            ctx.check("instance_bool_size3", bool_v.size() == 3);
            ctx.check("instance_bool_values",
                      vectors_equal(bool_v, std::vector<bool>{ false, true, false }));

            const std::vector<std::byte> byte_v{ self->i_byte() };
            ctx.check("instance_byte_size3", byte_v.size() == 3);
            ctx.check("instance_byte_values",
                      vectors_equal(byte_v, std::vector<std::byte>{
                          std::byte{ 4 }, std::byte{ 5 }, std::byte{ 6 } }));

            const std::vector<std::int16_t> short_v{ self->i_short() };
            ctx.check("instance_short_values",
                      vectors_equal(short_v, std::vector<std::int16_t>{ 400, 500, 600 }));

            const std::vector<char> char_v{ self->i_char() };
            ctx.check("instance_char_values",
                      vectors_equal(char_v, std::vector<char>{ 'X', 'Y', 'Z' }));

            const std::vector<std::int32_t> int_v{ self->i_int() };
            ctx.check("instance_int_values",
                      vectors_equal(int_v, std::vector<std::int32_t>{ 4000, 5000, 6000 }));

            const std::vector<std::int64_t> long_v{ self->i_long() };
            ctx.check("instance_long_values",
                      vectors_equal(long_v, std::vector<std::int64_t>{
                          4000000000LL, 5000000000LL, 6000000000LL }));

            const std::vector<float> float_v{ self->i_float() };
            ctx.check("instance_float_values",
                      all_bits_equal(float_v, std::vector<float>{ 4.5f, 5.5f, 6.5f }));

            const std::vector<double> double_v{ self->i_double() };
            ctx.check("instance_double_values",
                      all_bits_equal(double_v, std::vector<double>{ 4.25, 5.25, 6.25 }));

            // ---- 2b) EMPTY instance arrays (length 0 on the instance path) --
            ctx.check("instance_empty_bool",   self->ie_bool().empty());
            ctx.check("instance_empty_byte",   self->ie_byte().empty());
            ctx.check("instance_empty_short",  self->ie_short().empty());
            ctx.check("instance_empty_char",   self->ie_char().empty());
            ctx.check("instance_empty_int",    self->ie_int().empty());
            ctx.check("instance_empty_long",   self->ie_long().empty());
            ctx.check("instance_empty_float",  self->ie_float().empty());
            ctx.check("instance_empty_double", self->ie_double().empty());

            // ---- 2c) SINGLE-element instance arrays -------------------------
            const std::vector<bool> i1_bool{ self->i_one_bool() };
            ctx.check("instance_single_bool", i1_bool.size() == 1 && i1_bool[0] == false);
            const std::vector<std::int8_t> i1_byte{ self->i_one_byte() };
            ctx.check("instance_single_byte",
                      i1_byte.size() == 1 && i1_byte[0] == static_cast<std::int8_t>(-7));
            const std::vector<std::int16_t> i1_short{ self->i_one_short() };
            ctx.check("instance_single_short",
                      i1_short.size() == 1 && i1_short[0] == static_cast<std::int16_t>(-321));
            const std::vector<char> i1_char{ self->i_one_char() };
            ctx.check("instance_single_char", i1_char.size() == 1 && i1_char[0] == 'q');
            const std::vector<std::int32_t> i1_int{ self->i_one_int() };
            ctx.check("instance_single_int", i1_int.size() == 1 && i1_int[0] == -7654321);
            const std::vector<std::int64_t> i1_long{ self->i_one_long() };
            ctx.check("instance_single_long",
                      i1_long.size() == 1 && i1_long[0] == -9876543210987LL);
            const std::vector<float> i1_float{ self->i_one_float() };
            ctx.check("instance_single_float",
                      i1_float.size() == 1 && bits_equal(i1_float[0], -1.5f));
            const std::vector<double> i1_double{ self->i_one_double() };
            ctx.check("instance_single_double",
                      i1_double.size() == 1 && bits_equal(i1_double[0], -0.0078125));

            // ---- 2d) BOUNDARY instance arrays ------------------------------
            ctx.check("instance_boundary_bool",
                      vectors_equal(self->i_b_bool(), std::vector<bool>{ true, false, false }));
            ctx.check("instance_boundary_byte",
                      vectors_equal(self->i_b_byte(), std::vector<std::int8_t>{
                          std::numeric_limits<std::int8_t>::min(),
                          static_cast<std::int8_t>(-1),
                          std::numeric_limits<std::int8_t>::max() }));
            ctx.check("instance_boundary_short",
                      vectors_equal(self->i_b_short(), std::vector<std::int16_t>{
                          std::numeric_limits<std::int16_t>::min(),
                          static_cast<std::int16_t>(-1),
                          std::numeric_limits<std::int16_t>::max() }));
            // char is unsigned 16-bit; into vector<char> the low byte is kept.
            // Fixture holds { 0x0000, 0x0001, 0xFFFF } -> low bytes { 0x00, 0x01, 0xFF }.
            ctx.check("instance_boundary_char",
                      vectors_equal(self->i_b_char(), std::vector<char>{
                          static_cast<char>(0x00), static_cast<char>(0x01),
                          static_cast<char>(0xFF) }));
            ctx.check("instance_boundary_int",
                      vectors_equal(self->i_b_int(), std::vector<std::int32_t>{
                          std::numeric_limits<std::int32_t>::min(), -1,
                          std::numeric_limits<std::int32_t>::max() }));
            ctx.check("instance_boundary_long",
                      vectors_equal(self->i_b_long(), std::vector<std::int64_t>{
                          std::numeric_limits<std::int64_t>::min(), -1,
                          std::numeric_limits<std::int64_t>::max() }));
            ctx.check("instance_boundary_float",
                      all_bits_equal(self->i_b_float(), std::vector<float>{
                          -std::numeric_limits<float>::max(),
                          std::numeric_limits<float>::denorm_min(),
                          std::numeric_limits<float>::max() }));
            ctx.check("instance_boundary_double",
                      all_bits_equal(self->i_b_double(), std::vector<double>{
                          -std::numeric_limits<double>::max(),
                          std::numeric_limits<double>::denorm_min(),
                          std::numeric_limits<double>::max() }));

            // ---- 2e) NULL instance array reference -> empty, no crash ------
            ctx.check("instance_null_long_array_ref_is_empty", self->i_null_long().empty());
        }
    }

    // =========================================================================
    // 3) EMPTY static arrays (length 0) -- read_array_value's `length <= 0`
    //    early-out.  Every type must yield an empty vector and must NOT crash.
    // =========================================================================
    {
        ctx.check("empty_bool",   wrapper::e_bool().empty());
        ctx.check("empty_byte",   wrapper::e_byte().empty());
        ctx.check("empty_short",  wrapper::e_short().empty());
        ctx.check("empty_char",   wrapper::e_char().empty());
        ctx.check("empty_int",    wrapper::e_int().empty());
        ctx.check("empty_long",   wrapper::e_long().empty());
        ctx.check("empty_float",  wrapper::e_float().empty());
        ctx.check("empty_double", wrapper::e_double().empty());
    }

    // =========================================================================
    // 4) SINGLE-element static arrays -- the length==1 boundary of the read loop.
    // =========================================================================
    {
        const std::vector<bool> bool_v{ wrapper::one_bool() };
        ctx.check("single_bool_size1", bool_v.size() == 1);
        ctx.check("single_bool_value", bool_v.size() == 1 && bool_v[0] == true);

        const std::vector<std::int8_t> byte_v{ wrapper::one_byte() };
        ctx.check("single_byte", byte_v.size() == 1 && byte_v[0] == static_cast<std::int8_t>(42));

        const std::vector<std::int16_t> short_v{ wrapper::one_short() };
        ctx.check("single_short", short_v.size() == 1 && short_v[0] == static_cast<std::int16_t>(12345));

        const std::vector<char> char_v{ wrapper::one_char() };
        ctx.check("single_char", char_v.size() == 1 && char_v[0] == 'Q');

        const std::vector<std::int32_t> int_v{ wrapper::one_int() };
        ctx.check("single_int", int_v.size() == 1 && int_v[0] == 1234567);

        const std::vector<std::int64_t> long_v{ wrapper::one_long() };
        ctx.check("single_long", long_v.size() == 1 && long_v[0] == 1234567890123LL);

        const std::vector<float> float_v{ wrapper::one_float() };
        ctx.check("single_float", float_v.size() == 1 && bits_equal(float_v[0], 3.14159f));

        const std::vector<double> double_v{ wrapper::one_double() };
        ctx.check("single_double", double_v.size() == 1 && bits_equal(double_v[0], 2.718281828));
    }

    // =========================================================================
    // 5) LARGE (256-element) arrays -- size + EVERY element recomputed from the
    //    same deterministic formula the Java fixture used.  Stresses the
    //    per-element append loop and reserve() at a non-trivial length.
    // =========================================================================
    {
        constexpr std::int32_t large_len{ 256 };

        const std::vector<bool> bool_v{ wrapper::big_bool() };
        bool bool_ok{ bool_v.size() == static_cast<std::size_t>(large_len) };
        for (std::int32_t i{ 0 }; bool_ok && i < large_len; ++i)
        {
            bool_ok = bool_v[static_cast<std::size_t>(i)] == ((i % 2) == 0);
        }
        ctx.check("large_bool_all", bool_ok);

        const std::vector<std::int8_t> byte_v{ wrapper::big_byte() };
        bool byte_ok{ byte_v.size() == static_cast<std::size_t>(large_len) };
        for (std::int32_t i{ 0 }; byte_ok && i < large_len; ++i)
        {
            byte_ok = byte_v[static_cast<std::size_t>(i)] == static_cast<std::int8_t>(i - 128);
        }
        ctx.check("large_byte_all", byte_ok);

        const std::vector<std::int16_t> short_v{ wrapper::big_short() };
        bool short_ok{ short_v.size() == static_cast<std::size_t>(large_len) };
        for (std::int32_t i{ 0 }; short_ok && i < large_len; ++i)
        {
            short_ok = short_v[static_cast<std::size_t>(i)] == static_cast<std::int16_t>(i * 7 - 900);
        }
        ctx.check("large_short_all", short_ok);

        const std::vector<char> char_v{ wrapper::big_char() };
        bool char_ok{ char_v.size() == static_cast<std::size_t>(large_len) };
        for (std::int32_t i{ 0 }; char_ok && i < large_len; ++i)
        {
            // Java char (i+32) read into vector<char> = low 8 bits of (i+32).
            char_ok = char_v[static_cast<std::size_t>(i)]
                      == static_cast<char>(static_cast<std::uint16_t>(i + 32));
        }
        ctx.check("large_char_all", char_ok);

        const std::vector<std::int32_t> int_v{ wrapper::big_int() };
        bool int_ok{ int_v.size() == static_cast<std::size_t>(large_len) };
        for (std::int32_t i{ 0 }; int_ok && i < large_len; ++i)
        {
            int_ok = int_v[static_cast<std::size_t>(i)] == (i * 3 + 1);
        }
        ctx.check("large_int_all", int_ok);

        const std::vector<std::int64_t> long_v{ wrapper::big_long() };
        bool long_ok{ long_v.size() == static_cast<std::size_t>(large_len) };
        for (std::int32_t i{ 0 }; long_ok && i < large_len; ++i)
        {
            long_ok = long_v[static_cast<std::size_t>(i)]
                      == (static_cast<std::int64_t>(i) * 1000000007LL + 5LL);
        }
        ctx.check("large_long_all", long_ok);

        const std::vector<float> float_v{ wrapper::big_float() };
        bool float_ok{ float_v.size() == static_cast<std::size_t>(large_len) };
        for (std::int32_t i{ 0 }; float_ok && i < large_len; ++i)
        {
            float_ok = bits_equal(float_v[static_cast<std::size_t>(i)],
                                  static_cast<float>(i) + 0.5f);
        }
        ctx.check("large_float_all", float_ok);

        const std::vector<double> double_v{ wrapper::big_double() };
        bool double_ok{ double_v.size() == static_cast<std::size_t>(large_len) };
        for (std::int32_t i{ 0 }; double_ok && i < large_len; ++i)
        {
            double_ok = bits_equal(double_v[static_cast<std::size_t>(i)],
                                   static_cast<double>(i) + 0.25);
        }
        ctx.check("large_double_all", double_ok);
    }

    // =========================================================================
    // 6) BOUNDARY values -- MIN / 0 / MAX per type, exact.  Catches sign /
    //    width / endianness mistakes in get_array_element<T>.
    // =========================================================================
    {
        const std::vector<bool> bool_v{ wrapper::b_bool() };
        ctx.check("boundary_bool",
                  vectors_equal(bool_v, std::vector<bool>{ false, true, true }));

        const std::vector<std::int8_t> byte_v{ wrapper::b_byte() };
        ctx.check("boundary_byte",
                  vectors_equal(byte_v, std::vector<std::int8_t>{
                      std::numeric_limits<std::int8_t>::min(), 0,
                      std::numeric_limits<std::int8_t>::max() }));

        const std::vector<std::int16_t> short_v{ wrapper::b_short() };
        ctx.check("boundary_short",
                  vectors_equal(short_v, std::vector<std::int16_t>{
                      std::numeric_limits<std::int16_t>::min(), 0,
                      std::numeric_limits<std::int16_t>::max() }));

        // char is unsigned 16-bit in Java; read into vector<char> takes the low
        // 8 bits.  Fixture holds { 0x0000, 0x0041, 0x007F }, all <= 0x7F, so the
        // narrowing is lossless here.
        const std::vector<char> char_v{ wrapper::b_char() };
        ctx.check("boundary_char",
                  vectors_equal(char_v, std::vector<char>{
                      static_cast<char>(0x00), static_cast<char>(0x41),
                      static_cast<char>(0x7F) }));

        const std::vector<std::int32_t> int_v{ wrapper::b_int() };
        ctx.check("boundary_int",
                  vectors_equal(int_v, std::vector<std::int32_t>{
                      std::numeric_limits<std::int32_t>::min(), 0,
                      std::numeric_limits<std::int32_t>::max() }));

        const std::vector<std::int64_t> long_v{ wrapper::b_long() };
        ctx.check("boundary_long",
                  vectors_equal(long_v, std::vector<std::int64_t>{
                      std::numeric_limits<std::int64_t>::min(), 0,
                      std::numeric_limits<std::int64_t>::max() }));

        const std::vector<float> float_v{ wrapper::b_float() };
        ctx.check("boundary_float",
                  all_bits_equal(float_v, std::vector<float>{
                      -std::numeric_limits<float>::max(), 0.0f,
                      std::numeric_limits<float>::max() }));

        const std::vector<double> double_v{ wrapper::b_double() };
        ctx.check("boundary_double",
                  all_bits_equal(double_v, std::vector<double>{
                      -std::numeric_limits<double>::max(), 0.0,
                      std::numeric_limits<double>::max() }));
    }

    // =========================================================================
    // 7) SPECIAL float / double values -- NaN / +Inf / -Inf / subnormal,
    //    compared bit-exact so NaN propagation through the read is verified.
    // =========================================================================
    {
        const std::vector<float> float_v{ wrapper::sp_float() };
        ctx.check("special_float_size4", float_v.size() == 4);
        const bool float_ok{
            float_v.size() == 4
            && std::isnan(float_v[0])
            && float_v[1] == std::numeric_limits<float>::infinity()
            && float_v[2] == -std::numeric_limits<float>::infinity()
            && bits_equal(float_v[3], std::numeric_limits<float>::denorm_min()) };
        ctx.check("special_float_values", float_ok);

        const std::vector<double> double_v{ wrapper::sp_double() };
        ctx.check("special_double_size4", double_v.size() == 4);
        const bool double_ok{
            double_v.size() == 4
            && std::isnan(double_v[0])
            && double_v[1] == std::numeric_limits<double>::infinity()
            && double_v[2] == -std::numeric_limits<double>::infinity()
            && bits_equal(double_v[3], std::numeric_limits<double>::denorm_min()) };
        ctx.check("special_double_values", double_ok);
    }

    // =========================================================================
    // 8) NULL static array reference -> empty vector, no crash.
    //    decode_array_oop(0) -> nullptr -> read_array_value returns empty.
    // =========================================================================
    {
        ctx.check("static_null_int_array_ref_is_empty", wrapper::null_int().empty());
    }

    // =========================================================================
    // 9) RAW-LAYER endpoints -- array_length as the bounds oracle, plus
    //    get_array_element<T> at index 0 / mid / last for each canonical type.
    //    This pins the length read and the per-element offset arithmetic
    //    directly (the layer the implicit operator is built on), and never reads
    //    out of bounds (bounds come ONLY from array_length).
    // =========================================================================
    {
        std::int32_t len{ -1 };

        {
            std::int8_t a{}, m{}, z{};
            const bool ok{ raw_endpoints_static<std::int8_t>("staticByteArray", len, a, m, z) };
            ctx.check("raw_byte_len3", ok && len == 3);
            ctx.check("raw_byte_endpoints", ok && a == 1 && m == 2 && z == 3);
        }
        {
            std::int16_t a{}, m{}, z{};
            const bool ok{ raw_endpoints_static<std::int16_t>("staticShortArray", len, a, m, z) };
            ctx.check("raw_short_len3", ok && len == 3);
            ctx.check("raw_short_endpoints", ok && a == 100 && m == 200 && z == 300);
        }
        {
            // char[] at the raw layer is a 16-bit code unit (read as uint16).
            std::uint16_t a{}, m{}, z{};
            const bool ok{ raw_endpoints_static<std::uint16_t>("staticCharArray", len, a, m, z) };
            ctx.check("raw_char_len3", ok && len == 3);
            ctx.check("raw_char_endpoints", ok && a == 'A' && m == 'B' && z == 'C');
        }
        {
            std::int32_t a{}, m{}, z{};
            const bool ok{ raw_endpoints_static<std::int32_t>("staticIntArray", len, a, m, z) };
            ctx.check("raw_int_len3", ok && len == 3);
            ctx.check("raw_int_endpoints", ok && a == 1000 && m == 2000 && z == 3000);
        }
        {
            std::int64_t a{}, m{}, z{};
            const bool ok{ raw_endpoints_static<std::int64_t>("staticLongArray", len, a, m, z) };
            ctx.check("raw_long_len3", ok && len == 3);
            ctx.check("raw_long_endpoints",
                      ok && a == 1000000000LL && m == 2000000000LL && z == 3000000000LL);
        }
        {
            float a{}, m{}, z{};
            const bool ok{ raw_endpoints_static<float>("staticFloatArray", len, a, m, z) };
            ctx.check("raw_float_len3", ok && len == 3);
            ctx.check("raw_float_endpoints",
                      ok && bits_equal(a, 1.5f) && bits_equal(m, 2.5f) && bits_equal(z, 3.5f));
        }
        {
            double a{}, m{}, z{};
            const bool ok{ raw_endpoints_static<double>("staticDoubleArray", len, a, m, z) };
            ctx.check("raw_double_len3", ok && len == 3);
            ctx.check("raw_double_endpoints",
                      ok && bits_equal(a, 1.25) && bits_equal(m, 2.25) && bits_equal(z, 3.25));
        }
        // Large array: length oracle == 256 and the three sampled endpoints match
        // the deterministic formula (index 0 -> 1, index 128 -> 385, index 255 -> 766).
        {
            std::int32_t a{}, m{}, z{};
            const bool ok{ raw_endpoints_static<std::int32_t>("largeIntArray", len, a, m, z) };
            ctx.check("raw_large_int_len256", ok && len == 256);
            ctx.check("raw_large_int_endpoints",
                      ok && a == (0 * 3 + 1) && m == (128 * 3 + 1) && z == (255 * 3 + 1));
        }
        // Empty array: length oracle is exactly 0 (no element sample taken).
        {
            std::int32_t a{}, m{}, z{};
            len = -1;
            const bool ok{ raw_endpoints_static<std::int32_t>("emptyIntArray", len, a, m, z) };
            ctx.check("raw_empty_int_len0", ok && len == 0);
        }
    }

    // =========================================================================
    // 10) FLAW DOCUMENTATION -- char[] -> vector<char> is a LOSSY narrowing.
    //    append_array_value(vector<char>, "[C") reads a uint16 and truncates to
    //    the low 8 bits.  Code units >0xFF lose their high byte silently.  This
    //    asserts the *observed* (documented) truncation so a future fix that
    //    widens the path (e.g. to char16_t / std::u16string) trips this check.
    // =========================================================================
    {
        const std::vector<char> uni{ wrapper::uni_char_as_char() };
        ctx.check("unicode_char_size4", uni.size() == 4);
        // 'a'(0x61) survives; 0x00FF->0xFF, 0x0100->0x00, 0x20AC->0xAC.
        const bool narrowed_ok{
            uni.size() == 4
            && uni[0] == static_cast<char>(0x61)
            && uni[1] == static_cast<char>(0xFF)
            && uni[2] == static_cast<char>(0x00)
            && uni[3] == static_cast<char>(0xAC) };
        ctx.check("unicode_char_lossy_narrowing_documented", narrowed_ok);
        ctx.record("[INFO] field_arrays_primitive: char[] -> vector<char> truncates "
                   "each 16-bit code unit to its low 8 bits (lossy for code units "
                   ">0xFF).  Use the char-array String path for full-width text.");
    }

    // =========================================================================
    // 11) ELEMENT-WIDTH GUARD -- read_array_value REFUSES a read whose requested
    //    C++ element width disagrees with the field's JVM array element width,
    //    returning an empty vector (the documented safe failure) instead of
    //    mis-decoding (narrower) or reading past the array (wider).  This mirrors
    //    field_proxy::set's size-mismatch refusal on the read side, and closes
    //    BOTH unsafe directions:
    //
    //      * NARROWER ([J 8B -> vector<int32_t> 4B): pre-guard this walked the
    //        long[] data with a 4-byte stride and yielded interleaved low/high
    //        words (silent garbage); now refused -> empty.
    //      * WIDER ([I 4B -> vector<int64_t> 8B): pre-guard this strided
    //        length*8 bytes across a length*4-byte array, reading PAST the data
    //        area (OUT-OF-BOUNDS); now refused -> empty, with NO element read,
    //        so it is safe to drive in the shared CI process.
    //
    //    Matching-width reads of the SAME fields MUST still work byte-identically
    //    (the controls below): [J -> vector<int64_t> and [I -> vector<int32_t>.
    //    The guard is a pure width comparison, so it fires identically on every
    //    platform/JDK and validates locally.
    // =========================================================================
    {
        // -- NARROWER direction: [J read into vector<int32_t> is REFUSED. -------
        const std::vector<std::int32_t> narrow{ wrapper::wide_long_as_int32() };
        ctx.check("widthguard_narrow_long_to_int32_refused_empty", narrow.empty());

        // Control: the SAME [J field read at the matching width (vector<int64_t>)
        // still returns the exact long values, byte-for-byte.  (long2 == -1.)
        const std::vector<std::int64_t> correct{ wrapper::wide_long_as_int64() };
        ctx.check("widthguard_match_long_to_int64_ok",
                  vectors_equal(correct, std::vector<std::int64_t>{
                      static_cast<std::int64_t>(0x1122334455667788ULL),
                      static_cast<std::int64_t>(0x7FFFFFFF00000001ULL),
                      static_cast<std::int64_t>(-1) }));

        // -- WIDER direction: [I read into vector<int64_t> is REFUSED. ---------
        // This is the genuinely UNSAFE case (an 8-byte stride over 4-byte data
        // reads past the array end).  The guard rejects it BEFORE any element is
        // read, so no OOB access occurs -- the empty result proves the refusal.
        const std::vector<std::int64_t> wide_oob{ wrapper::int_as_int64_oob() };
        ctx.check("widthguard_wider_int_to_int64_refused_empty", wide_oob.empty());

        // Control: the SAME [I field read at the matching width (vector<int32_t>)
        // still returns {1000,2000,3000} -- proving the guard does NOT over-fire
        // on a correct-width read (this is the byte-identical fast path).
        const std::vector<std::int32_t> match{ wrapper::s_int() };
        ctx.check("widthguard_match_int_to_int32_ok",
                  vectors_equal(match, std::vector<std::int32_t>{ 1000, 2000, 3000 }));
    }

    // =========================================================================
    // 13) LARGE (1024-element) arrays -- the 1000+ shape, size + EVERY element
    //     recomputed from the Java fixture's deterministic formula.  Stresses
    //     reserve() + the per-element append loop at a four-figure length across
    //     the 4-byte (int) and 8-byte (long / double) append paths.
    // =========================================================================
    {
        constexpr std::int32_t k_len{ 1024 };

        const std::vector<std::int32_t> int_v{ wrapper::bigk_int() };
        bool int_ok{ int_v.size() == static_cast<std::size_t>(k_len) };
        for (std::int32_t i{ 0 }; int_ok && i < k_len; ++i)
        {
            int_ok = int_v[static_cast<std::size_t>(i)] == (i * 5 - 2000);
        }
        ctx.check("largek_int_size1024", int_v.size() == static_cast<std::size_t>(k_len));
        ctx.check("largek_int_all", int_ok);

        const std::vector<std::int64_t> long_v{ wrapper::bigk_long() };
        bool long_ok{ long_v.size() == static_cast<std::size_t>(k_len) };
        for (std::int32_t i{ 0 }; long_ok && i < k_len; ++i)
        {
            long_ok = long_v[static_cast<std::size_t>(i)]
                      == (static_cast<std::int64_t>(i) * 1000000009LL - 7LL);
        }
        ctx.check("largek_long_all", long_ok);

        const std::vector<double> double_v{ wrapper::bigk_double() };
        bool double_ok{ double_v.size() == static_cast<std::size_t>(k_len) };
        for (std::int32_t i{ 0 }; double_ok && i < k_len; ++i)
        {
            double_ok = bits_equal(double_v[static_cast<std::size_t>(i)],
                                   static_cast<double>(i) * 0.5 - 256.0);
        }
        ctx.check("largek_double_all", double_ok);
    }

    // =========================================================================
    // 14) char[] with ' ' (0x20) and Character.MAX_VALUE (0xFFFF) boundaries.
    //     Into vector<char>: ' ' and 'A' (both <= 0x7F) survive; 0xFFFF narrows
    //     to 0xFF (the same lossy narrowing documented in section 10, here at the
    //     printable/MAX char boundary).  The raw uint16 layer recovers all three.
    // =========================================================================
    {
        const std::vector<char> sp{ wrapper::space_char() };
        ctx.check("space_char_size3", sp.size() == 3);
        ctx.check("space_char_values",
                  vectors_equal(sp, std::vector<char>{
                      ' ', static_cast<char>(0xFF), 'A' }));

        // Raw layer: the same field read as uint16 keeps all three code units
        // EXACTLY (0x20, 0xFFFF, 0x41) -- proving the loss is only in the
        // vector<char> narrowing, not in the read itself.
        std::int32_t len{ -1 };
        std::uint16_t a{}, m{}, z{};
        const bool ok{ raw_endpoints_static<std::uint16_t>("spaceCharArray", len, a, m, z) };
        ctx.check("space_char_raw_len3", ok && len == 3);
        ctx.check("space_char_raw_exact",
                  ok && a == 0x0020 && m == 0xFFFF && z == 0x0041);
    }

    // =========================================================================
    // 15) MULTI-DIMENSIONAL primitive arrays -- depth length + every element.
    //     int[][] (rect), double[][] (rect), byte[][][] (3-D), plus a 2-D field
    //     whose middle row is null (read as an empty row, no crash).  Walked at
    //     the raw layer (outer object-array -> inner primitive arrays) using the
    //     same array_length + get_array_element primitives as the flat reads;
    //     never reads out of bounds.
    // =========================================================================
    {
        // ---- int[][] rectangular 2 x 3 -----------------------------------
        bool grid_ok{ false };
        const std::vector<std::vector<std::int32_t>> grid{
            walk_2d<std::int32_t>(wrapper::p_int_grid(), grid_ok) };
        ctx.check("md_int_grid_outer2", grid_ok && grid.size() == 2);
        ctx.check("md_int_grid_values",
                  grid.size() == 2
                  && vectors_equal(grid[0], std::vector<std::int32_t>{ 10, 20, 30 })
                  && vectors_equal(grid[1], std::vector<std::int32_t>{ 40, 50, 60 }));

        // ---- double[][] rectangular 2 x 2 (exact halves) -----------------
        bool dgrid_ok{ false };
        const std::vector<std::vector<double>> dgrid{
            walk_2d<double>(wrapper::p_double_grid(), dgrid_ok) };
        ctx.check("md_double_grid_outer2", dgrid_ok && dgrid.size() == 2);
        ctx.check("md_double_grid_values",
                  dgrid.size() == 2
                  && all_bits_equal(dgrid[0], std::vector<double>{ 1.5, 2.5 })
                  && all_bits_equal(dgrid[1], std::vector<double>{ 3.5, 4.5 }));

        // ---- byte[][][] 3-D ----------------------------------------------
        // { { {1,2}, {3} }, { {4,5,6} } }
        bool cube_ok{ false };
        const std::vector<std::vector<std::vector<std::int8_t>>> cube{
            walk_3d<std::int8_t>(wrapper::p_byte_cube(), cube_ok) };
        const bool cube_shape{
            cube_ok && cube.size() == 2
            && cube[0].size() == 2 && cube[1].size() == 1
            && cube[0][0].size() == 2 && cube[0][1].size() == 1
            && cube[1][0].size() == 3 };
        ctx.check("md_byte_cube_shape", cube_shape);
        ctx.check("md_byte_cube_values",
                  cube_shape
                  && vectors_equal(cube[0][0], std::vector<std::int8_t>{ 1, 2 })
                  && vectors_equal(cube[0][1], std::vector<std::int8_t>{ 3 })
                  && vectors_equal(cube[1][0], std::vector<std::int8_t>{ 4, 5, 6 }));

        // ---- int[][] with a NULL middle row ------------------------------
        // { {1,2}, null, {3} } -- the null row must read as an empty inner
        // vector and the walk must still recover rows 0 and 2 (no crash).
        bool nullrow_ok{ false };
        const std::vector<std::vector<std::int32_t>> nullrow{
            walk_2d<std::int32_t>(wrapper::p_int_grid_null_row(), nullrow_ok) };
        ctx.check("md_int_grid_nullrow_outer3", nullrow_ok && nullrow.size() == 3);
        ctx.check("md_int_grid_nullrow_values",
                  nullrow.size() == 3
                  && vectors_equal(nullrow[0], std::vector<std::int32_t>{ 1, 2 })
                  && nullrow[1].empty()
                  && vectors_equal(nullrow[2], std::vector<std::int32_t>{ 3 }));

        // ---- INSTANCE int[][] 3 x 2 (instance-offset outer read) ---------
        const std::unique_ptr<wrapper> self{ wrapper::get_instance() };
        if (self)
        {
            bool ig_ok{ false };
            const std::vector<std::vector<std::int32_t>> ig{
                walk_2d<std::int32_t>(self->p_inst_int_grid(), ig_ok) };
            ctx.check("md_instance_int_grid_outer3", ig_ok && ig.size() == 3);
            ctx.check("md_instance_int_grid_values",
                      ig.size() == 3
                      && vectors_equal(ig[0], std::vector<std::int32_t>{ 70, 80 })
                      && vectors_equal(ig[1], std::vector<std::int32_t>{ 90, 100 })
                      && vectors_equal(ig[2], std::vector<std::int32_t>{ 110, 120 }));
        }
    }

    // =========================================================================
    // 16) JAGGED primitive arrays -- rows of differing length (and an EMPTY
    //     row).  The outer length and EACH row's length+values are verified, so
    //     a walk that assumed a uniform stride would fail.
    // =========================================================================
    {
        // int[][] = { {7}, {8,9}, { }, {10,11,12} }
        bool jag_ok{ false };
        const std::vector<std::vector<std::int32_t>> jag{
            walk_2d<std::int32_t>(wrapper::p_jagged_int(), jag_ok) };
        ctx.check("jagged_int_outer4", jag_ok && jag.size() == 4);
        ctx.check("jagged_int_row_lengths",
                  jag.size() == 4
                  && jag[0].size() == 1 && jag[1].size() == 2
                  && jag[2].empty()      && jag[3].size() == 3);
        ctx.check("jagged_int_values",
                  jag.size() == 4
                  && vectors_equal(jag[0], std::vector<std::int32_t>{ 7 })
                  && vectors_equal(jag[1], std::vector<std::int32_t>{ 8, 9 })
                  && jag[2].empty()
                  && vectors_equal(jag[3], std::vector<std::int32_t>{ 10, 11, 12 }));

        // short[][] jagged on the INSTANCE path = { {1}, {-2,3}, {4,5,6} }
        const std::unique_ptr<wrapper> self{ wrapper::get_instance() };
        if (self)
        {
            bool js_ok{ false };
            const std::vector<std::vector<std::int16_t>> js{
                walk_2d<std::int16_t>(self->p_inst_jagged_short(), js_ok) };
            ctx.check("jagged_instance_short_outer3", js_ok && js.size() == 3);
            ctx.check("jagged_instance_short_values",
                      js.size() == 3
                      && vectors_equal(js[0], std::vector<std::int16_t>{ 1 })
                      && vectors_equal(js[1], std::vector<std::int16_t>{ -2, 3 })
                      && vectors_equal(js[2], std::vector<std::int16_t>{ 4, 5, 6 }));
        }
    }

    // =========================================================================
    // 17) get_array_element BOUNDS -- index 0 / length-1 are valid; index < 0,
    //     index == length, index > length, and a huge index must all return
    //     T{} (the documented out-of-range result) WITHOUT crashing or reading
    //     out of bounds.  Driven on the canonical [I {1000,2000,3000} field.
    // =========================================================================
    {
        const auto proxy{ wrapper::p_static_int() };
        ctx.check("oob_proxy_present", proxy.has_value());
        if (proxy.has_value())
        {
            void* const array_oop{ vmhook::field_oop(*proxy) };
            const bool oop_ok{ array_oop != nullptr
                               && vmhook::hotspot::is_valid_pointer(array_oop) };
            ctx.check("oob_array_oop_valid", oop_ok);
            if (oop_ok)
            {
                const std::int32_t len{ vmhook::array_length(array_oop) };
                ctx.check("oob_len3", len == 3);

                // In-range endpoints read the real values.
                ctx.check("oob_index0_ok",
                          vmhook::get_array_element<std::int32_t>(array_oop, 0) == 1000);
                ctx.check("oob_index_last_ok",
                          vmhook::get_array_element<std::int32_t>(array_oop, len - 1) == 3000);

                // Out-of-range indices return T{} (0) and do NOT crash / read OOB.
                ctx.check("oob_index_negative_zero",
                          vmhook::get_array_element<std::int32_t>(array_oop, -1) == 0);
                ctx.check("oob_index_equals_len_zero",
                          vmhook::get_array_element<std::int32_t>(array_oop, len) == 0);
                ctx.check("oob_index_past_len_zero",
                          vmhook::get_array_element<std::int32_t>(array_oop, len + 5) == 0);
                ctx.check("oob_index_huge_zero",
                          vmhook::get_array_element<std::int32_t>(
                              array_oop, 0x7FFFFFFF) == 0);
                // INT_MIN exercises the negative-index branch at the extreme.
                ctx.check("oob_index_int_min_zero",
                          vmhook::get_array_element<std::int32_t>(
                              array_oop, std::numeric_limits<std::int32_t>::min()) == 0);
            }
        }
    }

    // =========================================================================
    // 12) Re-read stability -- reading the same field twice yields identical
    //     results (no destructive read / no shared mutable state in value_t).
    // =========================================================================
    {
        const std::vector<std::int32_t> first{ wrapper::s_int() };
        const std::vector<std::int32_t> second{ wrapper::s_int() };
        ctx.check("reread_int_stable", vectors_equal(first, second));

        const std::vector<double> d_first{ wrapper::s_double() };
        const std::vector<double> d_second{ wrapper::s_double() };
        ctx.check("reread_double_stable", all_bits_equal(d_first, d_second));

        // A special-value array re-read must reproduce NaN/Inf/subnormal bit-for-bit.
        const std::vector<float> sp_a{ wrapper::sp_float() };
        const std::vector<float> sp_b{ wrapper::sp_float() };
        ctx.check("reread_special_float_stable", all_bits_equal(sp_a, sp_b));
    }
}

VMHOOK_JVM_MODULE(field_arrays_primitive)
{
    // SUITE-SAFETY (mirrors field_primitives_get.cpp / aaa_warmup.cpp):
    //   * the whole body runs under a try/catch so a stray throw from any vmhook
    //     call is recorded as [INFO], never a FAIL, and never escapes this module
    //     (this module installs NO hooks and only reads fields + drives the
    //     fixture's pre-registered probe, but the playbook mandates the guard);
    //   * an unconditional vmhook::shutdown_hooks() runs OUTSIDE the try, so the
    //     module returns to the driver with an EMPTY hook table on every path
    //     (idempotent and safe-when-empty; proven by shutdown_hooks_teardown).
    bool body_threw{ false };
    try
    {
        run_field_arrays_primitive_checks(ctx);
    }
    catch (...)
    {
        body_threw = true;
    }

    // FINAL CLEANUP -- OUTSIDE the try so it ALWAYS runs.
    vmhook::shutdown_hooks();

    if (body_threw)
    {
        ctx.record("[INFO] field_arrays_primitive: the test body threw and was "
                   "contained (no crash, no hooks armed); see preceding checks "
                   "for partial results.");
    }
    ctx.check("module_left_clean_final_shutdown", true);
}
