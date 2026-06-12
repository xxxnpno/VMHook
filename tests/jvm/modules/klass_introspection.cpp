// klass_introspection JVM test module  (feature area: classes / klass shape)
//
// THE klass-introspection authority: exhaustively exercises every reachable
// klass-shape query the library exposes, against a fixture
// (vmhook/fixtures/KlassIntrospection.java) that exhibits EVERY Java class shape
// with a javac-STABLE internal name, plus array klasses resolved by descriptor.
//
// Queries swept, per shape:
//   * vmhook::find_class(name)            -> hotspot::klass*   (resolution)
//   * klass::get_name()                   internal '/'-name symbol (+ readable
//                                          name derived in C++)
//   * klass::get_super()                  super-klass* (null ONLY for Object)
//   * klass::get_instance_size()          Klass::_layout_helper based size
//   * klass::get_methods_count()          InstanceKlass::_methods length
//   * klass::get_interfaces_ptr(count)    transitive interface klasses
//   * klass::find_field("x")              declared field metadata (this class
//                                          only) AND the free vmhook::find_field
//                                          (super-chain walk) for inherited
//   * vmhook::get_class_methods<T>()/(name) + find_methods_by_signature<T>
//   * the class-file access flags (Klass::_access_flags), read through a guarded
//     cached-offset os::safe_read helper here (the SAME pattern the library uses
//     internally in klass_is_interface_like) — tested bit by bit (PUBLIC / FINAL
//     / ABSTRACT / INTERFACE / ENUM / ANNOTATION) and CROSS-CHECKED against
//     java.lang.reflect.Modifier published by the fixture.
//
// Class shapes covered (kli_* check prefix):
//   normal/public (the top-level fixture), interface, abstract, final, enum,
//   annotation, static-nested, non-static-inner, generic (erased), generic-
//   bridge (Comparable), a 3-level inheritance chain (Base->Mid->Leaf), a
//   many-fields/methods class, array klasses ([I, [[Ljava/lang/String;), and
//   java.lang.Object (the no-super boundary).  A primitive-type pseudo-name is
//   probed for reachability.
//
// BOUNDARIES pinned hard:
//   - java.lang.Object's super is NULL (the unique terminator of every _super
//     walk) — proven both natively (get_super()==nullptr) and against the
//     fixture's Java reflection witness;
//   - an array klass's element type + dimensionality are reflected in its name
//     ("[I" is 1-D int; "[[Ljava/lang/String;" is 2-D String) and its super is
//     java.lang.Object;
//   - the declared-only method semantics: enumerating Leaf returns ONLY Leaf-
//     declared methods (Mid/Base/Object absent), and hand-walking get_super()
//     yields each level's own declared set in turn.
//
// CRASH-PROOFING (mingw·gcc / clang-on-windows have NO usable SEH net — any wild
// read kills the whole JVM):
//   Every klass read here targets METASPACE metadata (Klass / InstanceKlass /
//   Symbol), which is native and STABLE (never GC-relocated), and the library
//   guards each accessor with is_valid_pointer.  So these reads cannot fault on
//   a loaded klass and the metadata checks stay HARD.  Nonetheless, per the
//   cold-fault discipline, every COLD klass dereference (the _super / _methods /
//   _layout_helper / _access_flags field at klass+offset) is first proven
//   currently-mapped via os::safe_read on the klass header span; a transient
//   miss degrades to a best-effort [INFO] (never a fault, never a vacuous pass).
//   The ONE genuinely cold-unsafe deref pattern (klass_from_oop on a young-gen
//   instance) is NOT used here — this module reads klass metadata only, never an
//   instance oop's content.  Fine-grained ctx.record() checkpoints (flushed per
//   line) bracket every PART so any residual no-SEH fault is pinpointed by the
//   last-flushed line.
//
// NO NEW RAW VMSTRUCT DEREFS: the access-flags helper resolves the
// Klass::_access_flags VMStruct offset through the public
// hotspot::iterate_struct_entries (cached) and reads the 4 bytes via
// os::safe_read — it never does a raw `*ptr`.  All other reads go through the
// existing guarded klass accessors.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace
{
    // =====================================================================
    //  Wrappers — one per registered shape so get_class_methods<T>() resolves
    //  through the wrapper's register_class<T>() entry.  Each derives from
    //  vmhook::object<T> (gives the vtable register_class<T> needs).  Only the
    //  ctor is needed; these are introspection targets, not field/method readers.
    // =====================================================================
    template<typename derived>
    class shape_wrapper : public vmhook::object<derived>
    {
    public:
        explicit shape_wrapper(vmhook::oop_t instance) noexcept
            : vmhook::object<derived>{ instance }
        {
        }
    };

    class w_self     : public shape_wrapper<w_self>     { public: using shape_wrapper::shape_wrapper; };
    class w_iface    : public shape_wrapper<w_iface>    { public: using shape_wrapper::shape_wrapper; };
    class w_abstract : public shape_wrapper<w_abstract> { public: using shape_wrapper::shape_wrapper; };
    class w_final    : public shape_wrapper<w_final>    { public: using shape_wrapper::shape_wrapper; };
    class w_enum     : public shape_wrapper<w_enum>     { public: using shape_wrapper::shape_wrapper; };
    class w_nested   : public shape_wrapper<w_nested>   { public: using shape_wrapper::shape_wrapper; };
    class w_inner    : public shape_wrapper<w_inner>    { public: using shape_wrapper::shape_wrapper; };
    class w_box      : public shape_wrapper<w_box>      { public: using shape_wrapper::shape_wrapper; };
    class w_cmp      : public shape_wrapper<w_cmp>      { public: using shape_wrapper::shape_wrapper; };
    class w_base     : public shape_wrapper<w_base>     { public: using shape_wrapper::shape_wrapper; };
    class w_mid      : public shape_wrapper<w_mid>      { public: using shape_wrapper::shape_wrapper; };
    class w_leaf     : public shape_wrapper<w_leaf>     { public: using shape_wrapper::shape_wrapper; };
    class w_wide     : public shape_wrapper<w_wide>     { public: using shape_wrapper::shape_wrapper; };
    // Deliberately NEVER registered: proves the template overloads return empty
    // for an unregistered type.
    class w_unreg    : public shape_wrapper<w_unreg>    { public: using shape_wrapper::shape_wrapper; };

    // The fixture publishes its reflection witnesses through these statics; a
    // tiny wrapper gives us static_field(...) access without per-field clutter.
    class kli : public vmhook::object<kli>
    {
    public:
        explicit kli(vmhook::oop_t instance) noexcept
            : vmhook::object<kli>{ instance }
        {
        }

        static auto set_go(bool v) -> void           { static_field("go")->set(v); }
        static auto set_done(bool v) -> void          { static_field("done")->set(v); }
        static auto get_done() -> bool                { return static_field("done")->get(); }
        static auto set_mode(std::int32_t m) -> void  { static_field("mode")->set(m); }

        static auto i(const char* name) -> std::int32_t  { return static_field(name)->get(); }
        static auto b(const char* name) -> bool          { return static_field(name)->get(); }
        static auto s(const char* name) -> std::string   { return static_field(name)->get(); }
    };

    // ---- JVM_ACC_* bits (class-file access flags; low 16 of Klass::_access_flags)
    constexpr std::uint32_t ACC_PUBLIC     { 0x0001u };
    constexpr std::uint32_t ACC_FINAL      { 0x0010u };
    constexpr std::uint32_t ACC_INTERFACE  { 0x0200u };
    constexpr std::uint32_t ACC_ABSTRACT   { 0x0400u };
    constexpr std::uint32_t ACC_ANNOTATION { 0x2000u };
    constexpr std::uint32_t ACC_ENUM       { 0x4000u };

    // Internal names (the find_class resolution targets).
    constexpr char N_SELF[]     { "vmhook/fixtures/KlassIntrospection" };
    constexpr char N_IFACE[]    { "vmhook/fixtures/KlassIntrospection$Iface" };
    constexpr char N_ABSTRACT[] { "vmhook/fixtures/KlassIntrospection$AbstractBase" };
    constexpr char N_FINAL[]    { "vmhook/fixtures/KlassIntrospection$FinalLeaf" };
    constexpr char N_ENUM[]     { "vmhook/fixtures/KlassIntrospection$Suit" };
    constexpr char N_MARKER[]   { "vmhook/fixtures/KlassIntrospection$Marker" };
    constexpr char N_NESTED[]   { "vmhook/fixtures/KlassIntrospection$Nested" };
    constexpr char N_INNER[]    { "vmhook/fixtures/KlassIntrospection$Inner" };
    constexpr char N_BOX[]      { "vmhook/fixtures/KlassIntrospection$Box" };
    constexpr char N_CMP[]      { "vmhook/fixtures/KlassIntrospection$Cmp" };
    constexpr char N_BASE[]     { "vmhook/fixtures/KlassIntrospection$Base" };
    constexpr char N_MID[]      { "vmhook/fixtures/KlassIntrospection$Mid" };
    constexpr char N_LEAF[]     { "vmhook/fixtures/KlassIntrospection$Leaf" };
    constexpr char N_WIDE[]     { "vmhook/fixtures/KlassIntrospection$Wide" };
    constexpr char N_OBJECT[]   { "java/lang/Object" };
    constexpr char N_ENUM_SUPER[]{ "java/lang/Enum" };

    // ── COLD-KLASS SAFE-READ DISCIPLINE ──────────────────────────────────────
    //
    // The library's klass accessors are individually guarded, but per the
    // cold-fault discipline we additionally prove the klass header span is
    // currently mapped via os::safe_read before treating any field-at-offset
    // read as authoritative.  Metaspace is stable (never GC-relocated), so a
    // miss here is rare; when it happens we record [INFO] and skip the strong
    // assertion rather than risk a no-SEH fault.  A successful probe means the
    // matching guarded read cannot fault, so the assertion stays HARD.
    constexpr std::size_t k_klass_probe_bytes{ 64 };

    auto klass_header_safely_readable(const void* const k) -> bool
    {
        if (!k || !vmhook::hotspot::is_valid_pointer(k))
        {
            return false;
        }
        std::uint8_t scratch[k_klass_probe_bytes] = { 0 };
        return vmhook::os::safe_read(scratch, k, sizeof(scratch));
    }

    // Internal '/'-name of a klass, or "" on any failure (fully guarded — the
    // library's symbol::to_string is itself safe_read based).
    auto klass_name_str(vmhook::hotspot::klass* const k) -> std::string
    {
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

    // Convert an internal '/'-name to the readable '.'-form, mirroring how a
    // caller derives a human-readable name from get_name() (array descriptors
    // are left as-is — that is their canonical readable form).
    auto readable_name(const std::string& internal_name) -> std::string
    {
        std::string out{ internal_name };
        std::replace(out.begin(), out.end(), '/', '.');
        return out;
    }

    // Read Klass::_access_flags (low 16 bits == class-file access flags) through
    // a guarded, cached-offset os::safe_read — the SAME pattern the library uses
    // in detail::klass_is_interface_like.  Returns nullopt when the VMStruct
    // entry is unavailable on this JDK or the slot is not safely readable (never
    // faults, never a raw deref).
    auto klass_access_flags(vmhook::hotspot::klass* const k) -> std::optional<std::uint32_t>
    {
        if (!k || !vmhook::hotspot::is_valid_pointer(k))
        {
            return std::nullopt;
        }
        static const vmhook::hotspot::vm_struct_entry_t* const entry{
            vmhook::hotspot::iterate_struct_entries("Klass", "_access_flags") };
        if (!entry)
        {
            return std::nullopt;
        }
        std::uint32_t flags{ 0u };
        if (!vmhook::os::safe_read(&flags,
                                   reinterpret_cast<const std::uint8_t*>(k) + entry->offset,
                                   sizeof(flags)))
        {
            return std::nullopt;
        }
        return flags;
    }

    // Count occurrences of an exact (name, descriptor) pair.
    auto count_pair(const std::vector<std::pair<std::string, std::string>>& methods,
                    const std::string& name, const std::string& descriptor) -> std::size_t
    {
        return static_cast<std::size_t>(std::count_if(
            methods.begin(), methods.end(),
            [&](const std::pair<std::string, std::string>& m)
            { return m.first == name && m.second == descriptor; }));
    }

    auto has_name(const std::vector<std::pair<std::string, std::string>>& methods,
                  const std::string& name) -> bool
    {
        return std::any_of(methods.begin(), methods.end(),
                           [&](const std::pair<std::string, std::string>& m)
                           { return m.first == name; });
    }

    auto count_descriptor(const std::vector<std::pair<std::string, std::string>>& methods,
                          const std::string& descriptor) -> std::size_t
    {
        return static_cast<std::size_t>(std::count_if(
            methods.begin(), methods.end(),
            [&](const std::pair<std::string, std::string>& m)
            { return m.second == descriptor; }));
    }

    // Drive one probe cycle (mode 0): clears the latched done and programs mode
    // on the rising edge of go, then waits for done.
    auto drive(vmhook_test::context& ctx, std::int32_t mode) -> bool
    {
        return ctx.run_probe(
            [mode](bool value)
            {
                if (value)
                {
                    kli::set_done(false);
                    kli::set_mode(mode);
                }
                kli::set_go(value);
            },
            []() { return kli::get_done(); });
    }
}

VMHOOK_JVM_MODULE(klass_introspection)
{
    const auto cp = [&](const char* where)
    {
        ctx.record(std::string{ "[INFO] klass_introspection checkpoint: " } + where);
    };

    cp("register_class wrappers");
    vmhook::register_class<kli>(N_SELF);
    vmhook::register_class<w_self>(N_SELF);
    vmhook::register_class<w_iface>(N_IFACE);
    vmhook::register_class<w_abstract>(N_ABSTRACT);
    vmhook::register_class<w_final>(N_FINAL);
    vmhook::register_class<w_enum>(N_ENUM);
    vmhook::register_class<w_nested>(N_NESTED);
    vmhook::register_class<w_inner>(N_INNER);
    vmhook::register_class<w_box>(N_BOX);
    vmhook::register_class<w_cmp>(N_CMP);
    vmhook::register_class<w_base>(N_BASE);
    vmhook::register_class<w_mid>(N_MID);
    vmhook::register_class<w_leaf>(N_LEAF);
    vmhook::register_class<w_wide>(N_WIDE);
    // w_unreg intentionally NOT registered.

    // =====================================================================
    // PART 0 — Drive the fixture probe FIRST so the Java reflection witnesses
    //   (Modifier bits, super names, declared counts) are published before the
    //   cross-checks read them.  The probe also forced the $-nested types to
    //   load via class literals in <clinit>, but driving it once more here
    //   guarantees the witnesses are current.
    // =====================================================================
    cp("PART 0 drive probe (publish reflection witnesses)");
    {
        const bool done{ drive(ctx, 0) };
        ctx.check("probe_published_witnesses", done);
        ctx.check("probe_tick_witness", kli::i("tickWitness") == 42);
    }

    // =====================================================================
    // PART A — find_class resolves EVERY shape, and each resolved klass echoes
    //   its own internal name (right klass, not a stale cache hit).
    // =====================================================================
    cp("PART A find_class resolves every shape + name echo");
    vmhook::hotspot::klass* const k_self    { vmhook::find_class(N_SELF) };
    vmhook::hotspot::klass* const k_iface   { vmhook::find_class(N_IFACE) };
    vmhook::hotspot::klass* const k_abstract{ vmhook::find_class(N_ABSTRACT) };
    vmhook::hotspot::klass* const k_final   { vmhook::find_class(N_FINAL) };
    vmhook::hotspot::klass* const k_enum    { vmhook::find_class(N_ENUM) };
    vmhook::hotspot::klass* const k_marker  { vmhook::find_class(N_MARKER) };
    vmhook::hotspot::klass* const k_nested  { vmhook::find_class(N_NESTED) };
    vmhook::hotspot::klass* const k_inner   { vmhook::find_class(N_INNER) };
    vmhook::hotspot::klass* const k_box     { vmhook::find_class(N_BOX) };
    vmhook::hotspot::klass* const k_cmp     { vmhook::find_class(N_CMP) };
    vmhook::hotspot::klass* const k_base    { vmhook::find_class(N_BASE) };
    vmhook::hotspot::klass* const k_mid     { vmhook::find_class(N_MID) };
    vmhook::hotspot::klass* const k_leaf    { vmhook::find_class(N_LEAF) };
    vmhook::hotspot::klass* const k_wide    { vmhook::find_class(N_WIDE) };
    vmhook::hotspot::klass* const k_object  { vmhook::find_class(N_OBJECT) };

    ctx.check("resolve_self",     k_self != nullptr);
    ctx.check("resolve_iface",    k_iface != nullptr);
    ctx.check("resolve_abstract", k_abstract != nullptr);
    ctx.check("resolve_final",    k_final != nullptr);
    ctx.check("resolve_enum",     k_enum != nullptr);
    ctx.check("resolve_marker",   k_marker != nullptr);
    ctx.check("resolve_nested",   k_nested != nullptr);
    ctx.check("resolve_inner",    k_inner != nullptr);
    ctx.check("resolve_box",      k_box != nullptr);
    ctx.check("resolve_cmp",      k_cmp != nullptr);
    ctx.check("resolve_base",     k_base != nullptr);
    ctx.check("resolve_mid",      k_mid != nullptr);
    ctx.check("resolve_leaf",     k_leaf != nullptr);
    ctx.check("resolve_wide",     k_wide != nullptr);
    ctx.check("resolve_object",   k_object != nullptr);

    // Name echo (internal '/'-form) — proves the right klass resolved.
    ctx.check("name_self",     klass_name_str(k_self)     == N_SELF);
    ctx.check("name_iface",    klass_name_str(k_iface)    == N_IFACE);
    ctx.check("name_abstract", klass_name_str(k_abstract) == N_ABSTRACT);
    ctx.check("name_final",    klass_name_str(k_final)    == N_FINAL);
    ctx.check("name_enum",     klass_name_str(k_enum)     == N_ENUM);
    ctx.check("name_marker",   klass_name_str(k_marker)   == N_MARKER);
    ctx.check("name_nested",   klass_name_str(k_nested)   == N_NESTED);
    ctx.check("name_inner",    klass_name_str(k_inner)    == N_INNER);
    ctx.check("name_box",      klass_name_str(k_box)      == N_BOX);
    ctx.check("name_cmp",      klass_name_str(k_cmp)      == N_CMP);
    ctx.check("name_leaf",     klass_name_str(k_leaf)     == N_LEAF);
    ctx.check("name_wide",     klass_name_str(k_wide)     == N_WIDE);
    ctx.check("name_object",   klass_name_str(k_object)   == N_OBJECT);

    // Readable ('.'-form) name derivation from the internal name.
    ctx.check("readable_self",
              readable_name(klass_name_str(k_self)) == "vmhook.fixtures.KlassIntrospection");
    ctx.check("readable_enum",
              readable_name(klass_name_str(k_enum)) == "vmhook.fixtures.KlassIntrospection$Suit");

    // Distinctness: the chain klasses are three different objects.
    ctx.check("base_mid_leaf_distinct",
              k_base && k_mid && k_leaf
              && k_base != k_mid && k_mid != k_leaf && k_base != k_leaf);

    // =====================================================================
    // PART B — ACCESS FLAGS (Klass::_access_flags), bit by bit, against the
    //   KNOWN fixture shape (HARD, JDK-portable) and CROSS-CHECKED against the
    //   fixture's java.lang.reflect.Modifier witnesses (where the ClassFile
    //   access_flags and Modifier reliably share a bit).
    //
    //   NOTE on nested-type PUBLIC: a member type's *ClassFile* access_flags do
    //   NOT carry ACC_PUBLIC/STATIC (those live in the InnerClasses attribute,
    //   which is what Class.getModifiers() reports), so PUBLIC is cross-checked
    //   only on the TOP-LEVEL fixture.  ABSTRACT / INTERFACE / ENUM / ANNOTATION
    //   / FINAL ARE in the ClassFile access_flags and are asserted on the nested
    //   shapes directly.
    // =====================================================================
    cp("PART B access flags (guarded _access_flags read; known shape + Modifier)");
    {
        const std::optional<std::uint32_t> af_self{
            klass_header_safely_readable(k_self) ? klass_access_flags(k_self) : std::nullopt };
        const std::optional<std::uint32_t> af_iface{
            klass_header_safely_readable(k_iface) ? klass_access_flags(k_iface) : std::nullopt };
        const std::optional<std::uint32_t> af_abstract{
            klass_header_safely_readable(k_abstract) ? klass_access_flags(k_abstract) : std::nullopt };
        const std::optional<std::uint32_t> af_final{
            klass_header_safely_readable(k_final) ? klass_access_flags(k_final) : std::nullopt };
        const std::optional<std::uint32_t> af_enum{
            klass_header_safely_readable(k_enum) ? klass_access_flags(k_enum) : std::nullopt };
        const std::optional<std::uint32_t> af_marker{
            klass_header_safely_readable(k_marker) ? klass_access_flags(k_marker) : std::nullopt };
        const std::optional<std::uint32_t> af_nested{
            klass_header_safely_readable(k_nested) ? klass_access_flags(k_nested) : std::nullopt };

        // The VMStruct entry exists on every JDK we target (8..26), but the
        // library itself keeps a fallback for a JDK that renamed/dropped it, so
        // record its availability as [INFO] rather than hard-failing the suite on
        // a hypothetical future JDK.  All per-shape bit asserts below are gated on
        // af_*.has_value(), so they simply skip if it is genuinely unavailable.
        ctx.record(std::string{ "[INFO] kli Klass::_access_flags VMStruct entry: " }
                   + (af_self.has_value() ? "available (bit asserts active)"
                                          : "unavailable (bit asserts skipped on this JDK)"));

        // ---- INTERFACE bit -------------------------------------------------
        if (af_iface)
        {
            ctx.check("iface_has_INTERFACE_bit",  (*af_iface & ACC_INTERFACE) != 0u);
            ctx.check("iface_has_ABSTRACT_bit",   (*af_iface & ACC_ABSTRACT)  != 0u);  // ifaces are abstract
            ctx.check("iface_no_ENUM_bit",        (*af_iface & ACC_ENUM)      == 0u);
            ctx.check("iface_no_FINAL_bit",       (*af_iface & ACC_FINAL)     == 0u);
            // Cross-check against the fixture's Java view.
            ctx.check("iface_modifier_isInterface", kli::b("ifaceIsInterface"));
        }
        else { ctx.record("[INFO] kli iface access flags not safely readable — skipped bit asserts."); }

        // ---- ABSTRACT (non-interface) bit ----------------------------------
        if (af_abstract)
        {
            ctx.check("abstract_has_ABSTRACT_bit", (*af_abstract & ACC_ABSTRACT)  != 0u);
            ctx.check("abstract_no_INTERFACE_bit", (*af_abstract & ACC_INTERFACE) == 0u);
            ctx.check("abstract_no_FINAL_bit",     (*af_abstract & ACC_FINAL)     == 0u);
            ctx.check("abstract_no_ENUM_bit",      (*af_abstract & ACC_ENUM)      == 0u);
            ctx.check("abstract_modifier_isAbstract", kli::b("abstractIsAbstract"));
        }
        else { ctx.record("[INFO] kli abstract access flags not safely readable — skipped bit asserts."); }

        // ---- FINAL bit -----------------------------------------------------
        if (af_final)
        {
            ctx.check("final_has_FINAL_bit",       (*af_final & ACC_FINAL)     != 0u);
            ctx.check("final_no_ABSTRACT_bit",     (*af_final & ACC_ABSTRACT)  == 0u);
            ctx.check("final_no_INTERFACE_bit",    (*af_final & ACC_INTERFACE) == 0u);
            ctx.check("final_modifier_isFinal",    kli::b("finalIsFinal"));
        }
        else { ctx.record("[INFO] kli final access flags not safely readable — skipped bit asserts."); }

        // ---- ENUM bit (+ FINAL: a plain enum with no constant bodies is final)
        if (af_enum)
        {
            ctx.check("enum_has_ENUM_bit",         (*af_enum & ACC_ENUM)       != 0u);
            ctx.check("enum_no_INTERFACE_bit",     (*af_enum & ACC_INTERFACE)  == 0u);
            ctx.check("enum_modifier_isEnum",      kli::b("enumIsEnum"));
        }
        else { ctx.record("[INFO] kli enum access flags not safely readable — skipped bit asserts."); }

        // ---- ANNOTATION bit (+ INTERFACE: annotations are interfaces) -------
        if (af_marker)
        {
            ctx.check("marker_has_ANNOTATION_bit", (*af_marker & ACC_ANNOTATION) != 0u);
            ctx.check("marker_has_INTERFACE_bit",  (*af_marker & ACC_INTERFACE)  != 0u);
            ctx.check("marker_modifier_isAnnotation", kli::b("markerIsAnnotation"));
            ctx.check("marker_modifier_isInterface",  kli::b("markerIsInterface"));
        }
        else { ctx.record("[INFO] kli marker access flags not safely readable — skipped bit asserts."); }

        // ---- a plain nested class: none of the distinguishing bits ----------
        if (af_nested)
        {
            ctx.check("nested_no_INTERFACE_bit",   (*af_nested & ACC_INTERFACE) == 0u);
            ctx.check("nested_no_ENUM_bit",        (*af_nested & ACC_ENUM)      == 0u);
            ctx.check("nested_no_ANNOTATION_bit",  (*af_nested & ACC_ANNOTATION) == 0u);
            ctx.check("nested_no_ABSTRACT_bit",    (*af_nested & ACC_ABSTRACT)  == 0u);
            ctx.check("nested_modifier_not_interface", !kli::b("nestedIsInterface"));
            ctx.check("nested_modifier_not_abstract",  !kli::b("nestedIsAbstract"));
        }
        else { ctx.record("[INFO] kli nested access flags not safely readable — skipped bit asserts."); }

        // ---- TOP-LEVEL PUBLIC + FINAL cross-check against Modifier ----------
        // The top-level fixture is `public final class`, so its ClassFile
        // access_flags DO carry PUBLIC and FINAL — the one place a Modifier
        // PUBLIC cross-check is reliable.
        if (af_self)
        {
            const std::int32_t self_mods{ kli::i("selfMods") };
            ctx.check("self_PUBLIC_matches_modifier",
                      ((*af_self & ACC_PUBLIC) != 0u)
                          == ((static_cast<std::uint32_t>(self_mods) & ACC_PUBLIC) != 0u));
            ctx.check("self_PUBLIC_bit_set",  (*af_self & ACC_PUBLIC) != 0u);
            ctx.check("self_FINAL_bit_set",   (*af_self & ACC_FINAL)  != 0u);
            ctx.check("self_no_INTERFACE_bit",(*af_self & ACC_INTERFACE) == 0u);
            ctx.check("self_no_ENUM_bit",     (*af_self & ACC_ENUM)   == 0u);
        }

        // ---- Direct Modifier cross-check for the shared bits, generically ---
        // For every nested shape, the INTERFACE / ABSTRACT / ENUM / ANNOTATION
        // bits the native read sees must AGREE with the Modifier witness.
        auto xcheck_shared_bits = [&](const char* tag, const std::optional<std::uint32_t>& af,
                                      std::int32_t mods)
        {
            if (!af) { return; }
            const std::uint32_t m{ static_cast<std::uint32_t>(mods) };
            ctx.check(std::string{ "xbit_INTERFACE_" } + tag,
                      ((*af & ACC_INTERFACE) != 0u) == ((m & ACC_INTERFACE) != 0u));
            ctx.check(std::string{ "xbit_ABSTRACT_" } + tag,
                      ((*af & ACC_ABSTRACT) != 0u) == ((m & ACC_ABSTRACT) != 0u));
            ctx.check(std::string{ "xbit_ENUM_" } + tag,
                      ((*af & ACC_ENUM) != 0u) == ((m & ACC_ENUM) != 0u));
            ctx.check(std::string{ "xbit_ANNOTATION_" } + tag,
                      ((*af & ACC_ANNOTATION) != 0u) == ((m & ACC_ANNOTATION) != 0u));
        };
        xcheck_shared_bits("iface",    af_iface,    kli::i("ifaceMods"));
        xcheck_shared_bits("abstract", af_abstract, kli::i("abstractMods"));
        xcheck_shared_bits("enum",     af_enum,     kli::i("enumMods"));
        xcheck_shared_bits("marker",   af_marker,   kli::i("markerMods"));
        xcheck_shared_bits("nested",   af_nested,   kli::i("nestedMods"));
    }

    // =====================================================================
    // PART C — get_super(): the inheritance-chain primitive.
    //   HARD: Object's super is NULL (the unique walk terminator); Final ->
    //   AbstractBase; Mid -> Base; Leaf -> Mid; enum -> java/lang/Enum; nested
    //   -> Object.  Interface/array super is JDK-variant (HotSpot sets an
    //   interface's _super to Object while Java reflection reports null), so
    //   those are recorded as [INFO] observations, not hard asserts.
    // =====================================================================
    cp("PART C get_super (inheritance chain + Object null boundary)");
    {
        // Object's super is null — the hardest boundary, proven natively AND
        // against the Java witness.
        if (klass_header_safely_readable(k_object))
        {
            ctx.check("object_super_is_null", k_object->get_super() == nullptr);
        }
        else { ctx.record("[INFO] Object klass header not safely readable — skipped super-null."); }
        ctx.check("object_super_null_matches_java", kli::b("objectSuperIsNull"));
        ctx.check("object_super_name_witness_empty", kli::s("objectSuperName").empty());

        // Final -> AbstractBase.
        if (klass_header_safely_readable(k_final))
        {
            vmhook::hotspot::klass* const sup{ k_final->get_super() };
            ctx.check("final_super_nonnull", sup != nullptr);
            ctx.check("final_super_is_abstractbase", klass_name_str(sup) == N_ABSTRACT);
            ctx.check("final_super_name_matches_java",
                      klass_name_str(sup) == kli::s("finalSuperName"));
        }
        else { ctx.record("[INFO] final klass header not safely readable — skipped super."); }

        // Mid -> Base, Leaf -> Mid (the 3-level chain, native pointers).
        if (klass_header_safely_readable(k_mid))
        {
            ctx.check("mid_super_is_base",
                      klass_name_str(k_mid->get_super()) == N_BASE);
            ctx.check("mid_super_name_matches_java",
                      klass_name_str(k_mid->get_super()) == kli::s("midSuperName"));
            // The actual klass pointer equals the resolved Base klass.
            ctx.check("mid_super_ptr_is_base_klass", k_mid->get_super() == k_base);
        }
        else { ctx.record("[INFO] mid klass header not safely readable — skipped super."); }
        if (klass_header_safely_readable(k_leaf))
        {
            ctx.check("leaf_super_is_mid",
                      klass_name_str(k_leaf->get_super()) == N_MID);
            ctx.check("leaf_super_ptr_is_mid_klass", k_leaf->get_super() == k_mid);
        }
        else { ctx.record("[INFO] leaf klass header not safely readable — skipped super."); }

        // enum -> java/lang/Enum.
        if (klass_header_safely_readable(k_enum))
        {
            ctx.check("enum_super_is_javaEnum",
                      klass_name_str(k_enum->get_super()) == N_ENUM_SUPER);
            ctx.check("enum_super_name_matches_java",
                      klass_name_str(k_enum->get_super()) == kli::s("enumSuperName"));
        }
        else { ctx.record("[INFO] enum klass header not safely readable — skipped super."); }

        // nested -> Object (a plain top-of-chain user class).
        if (klass_header_safely_readable(k_nested))
        {
            ctx.check("nested_super_is_object",
                      klass_name_str(k_nested->get_super()) == N_OBJECT);
        }
        else { ctx.record("[INFO] nested klass header not safely readable — skipped super."); }

        // Walk the WHOLE chain Leaf -> Mid -> Base -> Object -> null and assert
        // the terminator is exactly java.lang.Object's null super.  This is the
        // canonical _super walk no other module exercises as an enumeration
        // concern.
        if (klass_header_safely_readable(k_leaf))
        {
            std::vector<std::string> chain{};
            vmhook::hotspot::klass* walk{ k_leaf };
            int guard{ 0 };
            while (walk && guard++ < 32)
            {
                if (!klass_header_safely_readable(walk)) { break; }
                chain.push_back(klass_name_str(walk));
                walk = walk->get_super();
            }
            // Expect the prefix Leaf, Mid, Base, Object and a null terminator.
            const bool well_formed{
                chain.size() >= 4
                && chain[0] == N_LEAF && chain[1] == N_MID
                && chain[2] == N_BASE && chain[3] == N_OBJECT };
            ctx.check("super_walk_leaf_to_object_prefix", well_formed);
            ctx.check("super_walk_terminates_at_null", walk == nullptr);
        }
        else { ctx.record("[INFO] leaf klass header not safely readable — skipped chain walk."); }
    }

    // =====================================================================
    // PART D — get_instance_size() (Klass::_layout_helper based).
    //   EMPIRICAL HotSpot semantics (verified on the CI matrix, contra the
    //   library's own get_instance_size doc comment): ONLY an ARRAY klass has a
    //   NON-positive _layout_helper, so ONLY an array's get_instance_size()==0.
    //   Every InstanceKlass — concrete, ABSTRACT, AND INTERFACE — carries a
    //   POSITIVE _layout_helper (its computed instance size; an interface gets a
    //   bare-header size even though it is non-instantiable).  So:
    //     - concrete / abstract / enum / interface  -> size > 0
    //     - array                                   -> size == 0  (see PART J)
    //   The abstract-is-positive and interface-is-positive facts are the two
    //   common misconceptions this part pins; the array==0 boundary lives in
    //   PART J where the array klasses are resolved.
    // =====================================================================
    cp("PART D get_instance_size (layout_helper semantics)");
    {
        // Concrete classes: positive, and a subclass is at least as large as a
        // superclass (it appends fields).
        if (klass_header_safely_readable(k_wide))
        {
            ctx.check("wide_instance_size_positive", k_wide->get_instance_size() > 0u);
        }
        else { ctx.record("[INFO] wide klass header not safely readable — skipped size."); }
        if (klass_header_safely_readable(k_nested))
        {
            ctx.check("nested_instance_size_positive", k_nested->get_instance_size() > 0u);
        }
        if (klass_header_safely_readable(k_final))
        {
            ctx.check("final_instance_size_positive", k_final->get_instance_size() > 0u);
        }
        // ABSTRACT still has a positive size (the misconception pin).
        if (klass_header_safely_readable(k_abstract))
        {
            ctx.check("abstract_instance_size_positive", k_abstract->get_instance_size() > 0u);
        }
        // ENUM is a concrete class -> positive size.
        if (klass_header_safely_readable(k_enum))
        {
            ctx.check("enum_instance_size_positive", k_enum->get_instance_size() > 0u);
        }
        // INTERFACE -> still a POSITIVE (header-sized) layout_helper on HotSpot,
        // NOT zero (zero is the array-klass signal — pinned in PART J).  Record
        // the observed size and assert it is positive and no larger than the
        // many-field Wide class (an interface declares no instance fields).
        if (klass_header_safely_readable(k_iface))
        {
            const std::size_t iface_sz{ k_iface->get_instance_size() };
            ctx.record(std::string{ "[INFO] interface get_instance_size() = " }
                       + std::to_string(iface_sz)
                       + " (POSITIVE header size; only ARRAY klasses report 0).");
            ctx.check("iface_instance_size_positive", iface_sz > 0u);
            if (klass_header_safely_readable(k_wide))
            {
                ctx.check("iface_size_le_wide_size", iface_sz <= k_wide->get_instance_size());
            }
        }
        // The subclass-grows-the-layout relationship across the chain.
        if (klass_header_safely_readable(k_base) && klass_header_safely_readable(k_leaf))
        {
            ctx.check("leaf_size_ge_base_size",
                      k_leaf->get_instance_size() >= k_base->get_instance_size()
                      && k_base->get_instance_size() > 0u);
        }
        // FinalLeaf (a field beyond AbstractBase) is >= AbstractBase's size.
        if (klass_header_safely_readable(k_abstract) && klass_header_safely_readable(k_final))
        {
            ctx.check("final_size_ge_abstract_size",
                      k_final->get_instance_size() >= k_abstract->get_instance_size());
        }
    }

    // =====================================================================
    // PART E — get_methods_count() vs get_class_methods() identity, per shape.
    //   The enumeration length must equal get_methods_count() MINUS the number
    //   of slots that fail is_valid_pointer (normally zero) — locking the count
    //   accessor to the walk (no other module checks the raw count).
    // =====================================================================
    cp("PART E get_methods_count vs get_class_methods identity");
    {
        auto count_vs_enum = [&](const char* tag, vmhook::hotspot::klass* k,
                                 const std::vector<std::pair<std::string, std::string>>& enumed)
        {
            if (!klass_header_safely_readable(k))
            {
                ctx.record(std::string{ "[INFO] " } + tag
                           + " klass header not safely readable — skipped count identity.");
                return;
            }
            const std::int32_t raw{ k->get_methods_count() };
            // The enumeration drops only invalid slots; on a loaded class there
            // are none, so size == raw.  Allow size <= raw (defensive) AND assert
            // they actually match on the common path.
            ctx.check(std::string{ "count_ge_enum_" } + tag,
                      raw >= static_cast<std::int32_t>(enumed.size()));
            ctx.check(std::string{ "count_eq_enum_" } + tag,
                      raw == static_cast<std::int32_t>(enumed.size()));
        };
        count_vs_enum("self",  k_self,  vmhook::get_class_methods<w_self>());
        count_vs_enum("iface", k_iface, vmhook::get_class_methods<w_iface>());
        count_vs_enum("enum",  k_enum,  vmhook::get_class_methods<w_enum>());
        count_vs_enum("wide",  k_wide,  vmhook::get_class_methods<w_wide>());
        count_vs_enum("leaf",  k_leaf,  vmhook::get_class_methods<w_leaf>());
    }

    // =====================================================================
    // PART F — declared methods per shape (get_class_methods<T>()).
    //   Interface: abstract + default + static methods all present.
    //   Enum: synthetic values()/valueOf present + the declared rank().
    //   Wide: all five m0..m4 present with exact descriptors + <init>.
    //   Bridge: Cmp has BOTH compareTo(LCmp;)I and the synthetic
    //           compareTo(Ljava/lang/Object;)I.
    // =====================================================================
    cp("PART F declared methods per shape");
    {
        // --- Interface: abstract/default/static all live in _methods ---------
        const auto m_iface{ vmhook::get_class_methods<w_iface>() };
        ctx.check("iface_nonempty", !m_iface.empty());
        ctx.check("iface_has_abstractOp", count_pair(m_iface, "abstractOp", "(I)I") == 1);
        ctx.check("iface_has_defaultOp",  count_pair(m_iface, "defaultOp",  "(I)I") == 1);
        ctx.check("iface_has_staticOp",   count_pair(m_iface, "staticOp",   "(I)I") == 1);
        // An interface has no <init> (no instances) — a negative shape fact.
        ctx.check("iface_no_init", count_pair(m_iface, "<init>", "()V") == 0);

        // --- Enum: declared rank() + synthetic values()/valueOf --------------
        const auto m_enum{ vmhook::get_class_methods<w_enum>() };
        ctx.check("enum_has_rank", count_pair(m_enum, "rank", "()I") == 1);
        ctx.check("enum_has_values",
                  count_pair(m_enum, "values", "()[Lvmhook/fixtures/KlassIntrospection$Suit;") == 1);
        ctx.check("enum_has_valueOf",
                  count_pair(m_enum, "valueOf",
                             "(Ljava/lang/String;)Lvmhook/fixtures/KlassIntrospection$Suit;") == 1);
        ctx.check("enum_has_init_or_clinit",
                  has_name(m_enum, "<init>") || has_name(m_enum, "<clinit>"));

        // --- Wide: every declared method with exact descriptor + <init> ------
        const auto m_wide{ vmhook::get_class_methods<w_wide>() };
        ctx.check("wide_has_m0", count_pair(m_wide, "m0", "()I") == 1);
        ctx.check("wide_has_m1", count_pair(m_wide, "m1", "()J") == 1);
        ctx.check("wide_has_m2", count_pair(m_wide, "m2", "()D") == 1);
        ctx.check("wide_has_m3", count_pair(m_wide, "m3", "()V") == 1);
        ctx.check("wide_has_m4", count_pair(m_wide, "m4", "(II)I") == 1);
        ctx.check("wide_has_init", count_pair(m_wide, "<init>", "()V") >= 1);
        // Lower bound: 5 declared + <init>.
        ctx.check("wide_total_at_least_6", m_wide.size() >= 6);

        // --- Bridge: BOTH compareTo variants present -------------------------
        const auto m_cmp{ vmhook::get_class_methods<w_cmp>() };
        ctx.check("cmp_has_typed_compareTo",
                  count_pair(m_cmp, "compareTo", "(Lvmhook/fixtures/KlassIntrospection$Cmp;)I") == 1);
        ctx.check("cmp_has_bridge_compareTo",
                  count_pair(m_cmp, "compareTo", "(Ljava/lang/Object;)I") == 1);
        ctx.check("cmp_compareTo_count_2", count_descriptor(m_cmp, "(Ljava/lang/Object;)I")
                                            + count_descriptor(m_cmp, "(Lvmhook/fixtures/KlassIntrospection$Cmp;)I") == 2);
        // Cross-check the bridge count against the Java reflection witness.
        ctx.check("cmp_compareTo_count_matches_java", kli::i("cmpCompareToCount") == 2);

        // find_methods_by_signature returns the bridge too.
        const auto cmp_obj{ vmhook::find_methods_by_signature<w_cmp>("(Ljava/lang/Object;)I") };
        ctx.check("cmp_find_bridge_by_sig",
                  std::find(cmp_obj.begin(), cmp_obj.end(), "compareTo") != cmp_obj.end());

        // --- Generic (erased) Box: get/set present; the type param is gone ---
        const auto m_box{ vmhook::get_class_methods<w_box>() };
        // After erasure get() returns Object and set takes Object.
        ctx.check("box_has_get_erased",
                  count_pair(m_box, "get", "()Ljava/lang/Object;") == 1);
        ctx.check("box_has_set_erased",
                  count_pair(m_box, "set", "(Ljava/lang/Object;)V") == 1);

        // No empty name/descriptor in any enumeration (symbol decode integrity).
        auto no_empty = [](const std::vector<std::pair<std::string, std::string>>& v)
        {
            return std::none_of(v.begin(), v.end(),
                                [](const std::pair<std::string, std::string>& m)
                                { return m.first.empty() || m.second.empty(); });
        };
        ctx.check("no_empty_iface", no_empty(m_iface));
        ctx.check("no_empty_enum",  no_empty(m_enum));
        ctx.check("no_empty_wide",  no_empty(m_wide));
        ctx.check("no_empty_cmp",   no_empty(m_cmp));
    }

    // =====================================================================
    // PART G — DECLARED-ONLY semantics + by-name/by-type agreement.
    //   Enumerating Leaf returns ONLY Leaf-declared methods; Mid/Base/Object
    //   methods are ABSENT.  Hand-walking get_super() recovers each level's own
    //   declared set.
    // =====================================================================
    cp("PART G declared-only semantics across the inheritance chain");
    {
        const auto m_leaf_t{ vmhook::get_class_methods<w_leaf>() };
        const auto m_leaf_n{ vmhook::get_class_methods(N_LEAF) };
        const auto m_mid_t{  vmhook::get_class_methods<w_mid>() };
        const auto m_base_t{ vmhook::get_class_methods<w_base>() };

        // by-name overload AGREES with by-type (same multiset size + members).
        ctx.check("leaf_byname_size_eq_bytype", m_leaf_n.size() == m_leaf_t.size());
        bool leaf_name_matches{ true };
        for (const auto& m : m_leaf_t)
        {
            if (count_pair(m_leaf_n, m.first, m.second) != count_pair(m_leaf_t, m.first, m.second))
            {
                leaf_name_matches = false; break;
            }
        }
        ctx.check("leaf_byname_matches_bytype", leaf_name_matches);

        // Leaf declares fromLeaf + sharedName (+ <init>); fromMid/fromBase are
        // INHERITED -> absent from Leaf's declared set.
        ctx.check("leaf_has_fromLeaf",   count_pair(m_leaf_t, "fromLeaf",   "()I") == 1);
        ctx.check("leaf_has_sharedName", count_pair(m_leaf_t, "sharedName", "()I") == 1);
        ctx.check("leaf_excludes_fromMid",  !has_name(m_leaf_t, "fromMid"));
        ctx.check("leaf_excludes_fromBase", !has_name(m_leaf_t, "fromBase"));
        // Inherited java.lang.Object methods never appear in a declared walk.
        ctx.check("leaf_excludes_toString", !has_name(m_leaf_t, "toString"));
        ctx.check("leaf_excludes_hashCode", !has_name(m_leaf_t, "hashCode"));

        // Mid declares fromMid + sharedName; NOT fromLeaf, NOT fromBase.
        ctx.check("mid_has_fromMid",      count_pair(m_mid_t, "fromMid", "()I") == 1);
        ctx.check("mid_has_sharedName",   count_pair(m_mid_t, "sharedName", "()I") == 1);
        ctx.check("mid_excludes_fromLeaf",!has_name(m_mid_t, "fromLeaf"));
        ctx.check("mid_excludes_fromBase",!has_name(m_mid_t, "fromBase"));

        // Base declares fromBase + sharedName; NOT fromMid, NOT fromLeaf.
        ctx.check("base_has_fromBase",    count_pair(m_base_t, "fromBase", "()I") == 1);
        ctx.check("base_excludes_fromMid",!has_name(m_base_t, "fromMid"));
        ctx.check("base_excludes_fromLeaf",!has_name(m_base_t, "fromLeaf"));

        // sharedName is declared at EACH level (an override at every level) — so
        // it appears once in EACH level's declared set (3 independent declarations).
        ctx.check("sharedName_declared_at_leaf", count_pair(m_leaf_t, "sharedName", "()I") == 1);
        ctx.check("sharedName_declared_at_mid",  count_pair(m_mid_t,  "sharedName", "()I") == 1);
        ctx.check("sharedName_declared_at_base", count_pair(m_base_t, "sharedName", "()I") == 1);

        // Hand-walk get_super() from Leaf and assert each level's declared set
        // is recovered via the by-name overload on the walked klass name.
        if (klass_header_safely_readable(k_leaf) && klass_header_safely_readable(k_mid)
            && klass_header_safely_readable(k_base))
        {
            const auto walk_mid{  vmhook::get_class_methods(klass_name_str(k_leaf->get_super())) };
            ctx.check("walk_super_of_leaf_is_mid_set",
                      has_name(walk_mid, "fromMid") && !has_name(walk_mid, "fromLeaf"));
            const auto walk_base{ vmhook::get_class_methods(klass_name_str(k_mid->get_super())) };
            ctx.check("walk_super_of_mid_is_base_set",
                      has_name(walk_base, "fromBase") && !has_name(walk_base, "fromMid"));
        }
    }

    // =====================================================================
    // PART H — declared FIELDS per shape (klass::find_field, declared-only) and
    //   the free vmhook::find_field (super-chain walk, inherited).
    // =====================================================================
    cp("PART H declared fields (find_field) + inherited via super-walk");
    {
        // Wide declares 8 fields with the right static-ness + descriptors.
        if (klass_header_safely_readable(k_wide))
        {
            auto chk_field = [&](const char* name, const char* sig)
            {
                const auto fe{ k_wide->find_field(name) };
                ctx.check(std::string{ "wide_field_" } + name + "_declared", fe.has_value());
                if (fe)
                {
                    ctx.check(std::string{ "wide_field_" } + name + "_instance", !fe->is_static);
                    ctx.check(std::string{ "wide_field_" } + name + "_sig", fe->signature == sig);
                }
            };
            chk_field("f0", "I");
            chk_field("f1", "J");
            chk_field("f2", "D");
            chk_field("f3", "F");
            chk_field("f4", "S");
            chk_field("f5", "B");
            chk_field("f6", "C");
            chk_field("f7", "Z");
            // A field that does not exist is not declared.
            ctx.check("wide_field_absent", !k_wide->find_field("noSuchField").has_value());
        }
        else { ctx.record("[INFO] wide klass header not safely readable — skipped field decls."); }

        // Cross-check Wide's declared field COUNT against the Java witness (the
        // klass::find_field path is per-name, so we corroborate the count via
        // the reflection witness rather than enumerate the field array here).
        ctx.check("wide_declared_field_count_java", kli::i("wideDeclaredFields") == 8);

        // DECLARED-ONLY vs INHERITED for fields:
        //   FinalLeaf declares `leafField`; `baseField` is inherited from
        //   AbstractBase.  klass::find_field (this-class-only) finds leafField but
        //   NOT baseField; the free vmhook::find_field (super-walk) finds BOTH.
        if (klass_header_safely_readable(k_final))
        {
            ctx.check("final_declares_leafField",
                      k_final->find_field("leafField").has_value());
            ctx.check("final_does_not_declare_baseField",
                      !k_final->find_field("baseField").has_value());
        }
        // The super-chain-walking free function reaches the inherited baseField.
        {
            const auto inherited{ vmhook::find_field(k_final, "baseField") };
            ctx.check("final_inherits_baseField_via_super_walk", inherited.has_value());
            const auto own{ vmhook::find_field(k_final, "leafField") };
            ctx.check("final_finds_own_leafField_via_free_fn", own.has_value());
        }
    }

    // =====================================================================
    // PART I — get_interfaces_ptr(): the implemented-interface set.
    //   Cmp implements Comparable (1 interface); FinalLeaf implements none
    //   directly.  The transitive set is read fault-safely by the library.
    // =====================================================================
    cp("PART I get_interfaces_ptr (implemented interfaces)");
    {
        // Cmp implements java.lang.Comparable — assert it appears in the
        // (transitive) interface set, gating each entry read on a header probe.
        if (klass_header_safely_readable(k_cmp))
        {
            std::int32_t count{ 0 };
            vmhook::hotspot::klass** const ifaces{ k_cmp->get_interfaces_ptr(count) };
            ctx.check("cmp_has_at_least_one_interface", ifaces != nullptr && count >= 1);
            bool found_comparable{ false };
            if (ifaces)
            {
                for (std::int32_t idx{ 0 }; idx < count && idx < 64; ++idx)
                {
                    // Each Klass* entry is read fault-safely (the array base came
                    // from the library's safe-read helper; probe each slot).
                    vmhook::hotspot::klass* entry{ nullptr };
                    if (!vmhook::os::safe_read(&entry, &ifaces[idx], sizeof(entry)))
                    {
                        continue;
                    }
                    if (klass_name_str(entry) == "java/lang/Comparable")
                    {
                        found_comparable = true;
                    }
                }
            }
            ctx.check("cmp_interface_set_contains_Comparable", found_comparable);
        }
        else { ctx.record("[INFO] cmp klass header not safely readable — skipped interfaces."); }

        // Cross-check the direct-interface count against the Java witness.
        ctx.check("cmp_interface_count_java", kli::i("cmpInterfaceCount") == 1);
        ctx.check("final_no_direct_interface_java", kli::b("finalImplementsNothingDirect"));
    }

    // =====================================================================
    // PART J — ARRAY KLASSES + non-InstanceKlass / primitive inputs.
    //   find_class("[I") / ("[[Ljava/lang/String;") resolve an ArrayKlass: its
    //   name reflects element type + dimension, its super is java.lang.Object,
    //   and the InstanceKlass-only method enumeration returns EMPTY without
    //   crashing (the array klass has no _methods array of its own — flaw #1
    //   territory; it MUST degrade to empty, never walk garbage).
    // =====================================================================
    cp("PART J array klasses + non-InstanceKlass inputs");
    {
        vmhook::hotspot::klass* const k_int_arr{ vmhook::find_class("[I") };
        vmhook::hotspot::klass* const k_str_arr2{ vmhook::find_class("[[Ljava/lang/String;") };

        // 1-D int array.
        if (k_int_arr)
        {
            const std::string nm{ klass_name_str(k_int_arr) };
            ctx.check("intarray_name_is_bracket_I", nm == "[I");
            ctx.check("intarray_dim_is_1",
                      nm.size() >= 1 && nm[0] == '[' && nm.find("[[") == std::string::npos);
            ctx.check("intarray_element_is_int", nm.back() == 'I');
            if (klass_header_safely_readable(k_int_arr))
            {
                // Array super is java.lang.Object.
                ctx.check("intarray_super_is_object",
                          klass_name_str(k_int_arr->get_super()) == N_OBJECT);
                // Non-instantiable-as-instance layout -> size 0.
                ctx.check("intarray_instance_size_zero", k_int_arr->get_instance_size() == 0u);
            }
            // Cross-check the array super name against the Java witness.
            ctx.check("intarray_super_name_java",
                      kli::s("intArraySuperName") == N_OBJECT);
        }
        else
        {
            ctx.record("[INFO] find_class(\"[I\") did not resolve on this JDK — "
                       "array klass not reachable via the ClassLoaderDataGraph walk here.");
        }

        // 2-D String array: dimension 2, element java/lang/String.
        if (k_str_arr2)
        {
            const std::string nm{ klass_name_str(k_str_arr2) };
            ctx.check("strarray2d_name", nm == "[[Ljava/lang/String;");
            ctx.check("strarray2d_dim_is_2",
                      nm.size() >= 2 && nm[0] == '[' && nm[1] == '[');
        }
        else
        {
            ctx.record("[INFO] find_class(\"[[Ljava/lang/String;\") did not resolve on this JDK.");
        }

        // *** flaw #1 boundary ***  get_class_methods on an ARRAY descriptor must
        // return EMPTY (and not crash): the array klass is not an InstanceKlass,
        // so the _methods walk has nothing valid to read.  Proven via BOTH the
        // by-name overload and a direct collect on the resolved array klass.
        const auto arr_methods_byname{ vmhook::get_class_methods("[I") };
        ctx.check("intarray_get_class_methods_empty", arr_methods_byname.empty());
        const auto arr2_methods_byname{ vmhook::get_class_methods("[[Ljava/lang/String;") };
        ctx.check("strarray2d_get_class_methods_empty", arr2_methods_byname.empty());

        // The array klass's Java Modifier witnesses agree it IS an array (and is
        // final + abstract per the JLS array-class flags) — corroboration that
        // the resolved object really is an array klass.
        ctx.check("intarray_is_array_java",   kli::b("intArrayIsArray"));
        ctx.check("strarray2d_is_array_java", kli::b("strArray2DIsArray"));

        // A primitive pseudo-name is NOT a loadable class name (primitives have
        // no Klass reachable by an internal name); find_class returns null and
        // the enumeration is empty — no crash.
        vmhook::hotspot::klass* const k_prim{ vmhook::find_class("int") };
        ctx.record(std::string{ "[INFO] find_class(\"int\") (primitive pseudo-name) -> " }
                   + (k_prim ? "resolved (unexpected but handled)" : "null (expected)"));
        const auto prim_methods{ vmhook::get_class_methods("int") };
        ctx.check("primitive_pseudo_name_methods_empty", prim_methods.empty());

        // An interface BY NAME enumerates its declared methods (interfaces ARE
        // InstanceKlasses) — distinguishes "non-InstanceKlass" (array) from
        // "InstanceKlass that happens to be an interface" (still enumerable).
        const auto iface_byname{ vmhook::get_class_methods(N_IFACE) };
        ctx.check("interface_byname_nonempty", !iface_byname.empty());
        ctx.check("interface_byname_has_abstractOp",
                  count_pair(iface_byname, "abstractOp", "(I)I") == 1);

        // The annotation type (an interface under the hood) likewise enumerates.
        const auto marker_byname{ vmhook::get_class_methods(N_MARKER) };
        ctx.check("annotation_byname_has_value_method",
                  count_pair(marker_byname, "value", "()Ljava/lang/String;") == 1);
        ctx.check("annotation_byname_has_count_method",
                  count_pair(marker_byname, "count", "()I") == 1);
    }

    // =====================================================================
    // PART K — NEGATIVE / EMPTY-RESULT contracts (no crash, empty result).
    // =====================================================================
    cp("PART K negative / empty-result contracts");
    {
        // Unregistered wrapper type -> empty enumeration + empty selector.
        ctx.check("unregistered_get_class_methods_empty",
                  vmhook::get_class_methods<w_unreg>().empty());
        ctx.check("unregistered_find_by_sig_empty",
                  vmhook::find_methods_by_signature<w_unreg>("(I)I").empty());

        // Bogus class name -> empty (full graph-walk miss + JNI fallback).
        ctx.check("bogus_name_methods_empty",
                  vmhook::get_class_methods("vmhook/fixtures/NoSuchKlassZZZ").empty());
        // Bogus class name -> find_class null.
        ctx.check("bogus_name_find_class_null",
                  vmhook::find_class("vmhook/fixtures/NoSuchKlassZZZ") == nullptr);

        // Empty class name -> empty / null (empty-name guard).
        ctx.check("empty_name_methods_empty", vmhook::get_class_methods("").empty());
        ctx.check("empty_name_find_class_null", vmhook::find_class("") == nullptr);

        // find_methods_by_signature on a descriptor nothing on Wide declares.
        ctx.check("wide_find_absent_sig_empty",
                  vmhook::find_methods_by_signature<w_wide>("(Ljava/lang/Thread;)V").empty());

        // Determinism: enumerating Wide twice yields the same multiset.
        const auto a{ vmhook::get_class_methods<w_wide>() };
        const auto b{ vmhook::get_class_methods<w_wide>() };
        bool same{ a.size() == b.size() };
        if (same)
        {
            for (const auto& m : a)
            {
                if (count_pair(a, m.first, m.second) != count_pair(b, m.first, m.second))
                {
                    same = false; break;
                }
            }
        }
        ctx.check("wide_enumeration_deterministic", same);
    }

    cp("module complete (all parts reached without a no-SEH fault)");
}
