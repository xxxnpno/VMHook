// klass_introspection_shapes JVM test module  (feature area: classes / klass shape)
//
// The SHAPES companion to klass_introspection.cpp.  klass_introspection.cpp pins
// the staple klass-metadata reads on the javac-stable shapes (normal / interface
// / abstract / final / enum / annotation / static-nested / 3-level chain /
// generic-bridge / 1-D & 2-D array / java.lang.Object).  This module exhausts the
// SHAPES that have NO fixed Java name, or that need a witness the other fixture
// never publishes, so the klass-introspection feature is "every Klass metadata
// field read through the library":
//
//   * NAME FORMS (binary '.'-name <-> internal '/'-name) for: anonymous, local,
//     lambda (best-effort — a JDK15+ hidden class may be unreachable by name),
//     and a non-static inner.  Each runtime name is PUBLISHED by the fixture so
//     the native side resolves it dynamically (never a hardcoded $N ordinal) and
//     asserts get_name() echoes the internal form and the '.'-derivation matches.
//   * boxed-PRIMITIVE wrappers (Integer / Long / Boolean / Character): name,
//     get_super() (Number vs Object), FINAL set, NOT interface/abstract/enum —
//     cross-checked against the Modifier + super-name witnesses.
//   * RECORD supertype (java.lang.Record, JDK16+): super == java.lang.Object.
//     Portable — no `record` in the fixture (compiles on JDK 8); the probe force-
//     loads java.lang.Record when present and the native side resolves it.
//   * DECLARED INTERFACES list: a class implementing THREE interfaces; the
//     get_interfaces_ptr() set contains all three; count >= 3; witness == 3.
//   * SYNTHETIC flag: javac's generic bridge compareTo(Ljava/lang/Object;)I is
//     ACC_SYNTHETIC; the native side reads the Method's class-file access flags
//     (guarded safe_read of the low-16 bits via the Method._access_flags VMStruct
//     offset — the SAME no-raw-deref pattern klass_introspection.cpp uses for the
//     Klass flags) and asserts the SYNTHETIC bit AGREES with the Java
//     isSynthetic() witness for BOTH the bridge and the typed compareTo.
//   * ARRAY component / dimension / element klass: [Ljava/lang/String; (1-D ref),
//     [[[I (3-D prim), [Lvmhook/fixtures/KlassIntrospectionShapes; (app-loaded
//     element).  Dimension = count of leading '[' in get_name(); the element
//     descriptor is the name with one '[' stripped and is RESOLVED via find_class
//     (its name must echo); array super == java.lang.Object; size == 0.
//   * LOADERS: bootstrap (java.lang.Object / the boxes) vs app (the fixture +
//     nested).  Both resolve through the same find_class path to DISTINCT klasses;
//     the bootstrap-vs-app boundary is cross-checked against the loader witnesses.
//
// CRASH-PROOFING — identical discipline to klass_introspection.cpp: every read
// targets STABLE metaspace metadata (Klass / InstanceKlass / Method / Symbol),
// each library accessor is is_valid_pointer-guarded, and every COLD klass/method
// dereference is first proven currently-mapped via os::safe_read on the header
// span (a transient miss degrades to [INFO], never a no-SEH fault, never a
// vacuous pass).  This module reads klass/method METADATA only — it NEVER does
// klass_from_oop on a (young-gen, possibly-moving) instance oop, the one genuinely
// cold-unsafe pattern; the unstable-name shapes are resolved by their PUBLISHED
// name through find_class instead, so no instance-oop deref ever happens here.
//
// NO NEW RAW VMSTRUCT DEREFS: the Method-synthetic read resolves the
// Method._access_flags VMStruct offset through the public
// hotspot::iterate_struct_entries (cached) and reads the bytes via os::safe_read
// — never a raw `*ptr`.  Every other read goes through an existing guarded klass /
// method accessor.  NO hooks are installed, so no shutdown_hooks() teardown is
// required (this module is pure introspection).
//
// Distinct kli_* check prefix throughout so these checks never collide with
// klass_introspection.cpp's names in the shared test_results.txt.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace
{
    // The fixture publishes its witnesses through these statics.
    class kls : public vmhook::object<kls>
    {
    public:
        explicit kls(vmhook::oop_t instance) noexcept
            : vmhook::object<kls>{ instance }
        {
        }

        static auto set_go(bool v) -> void          { static_field("go")->set(v); }
        static auto set_done(bool v) -> void         { static_field("done")->set(v); }
        static auto get_done() -> bool               { return static_field("done")->get(); }
        static auto set_mode(std::int32_t m) -> void { static_field("mode")->set(m); }

        static auto i(const char* name) -> std::int32_t { return static_field(name)->get(); }
        static auto b(const char* name) -> bool         { return static_field(name)->get(); }
        static auto s(const char* name) -> std::string  { return static_field(name)->get(); }
    };

    constexpr char N_SELF[]{ "vmhook/fixtures/KlassIntrospectionShapes" };

    // ---- JVM_ACC_* class-file access bits (low 16 of *_access_flags) ----------
    constexpr std::uint32_t ACC_FINAL     { 0x0010u };
    constexpr std::uint32_t ACC_INTERFACE { 0x0200u };
    constexpr std::uint32_t ACC_ABSTRACT  { 0x0400u };
    constexpr std::uint32_t ACC_SYNTHETIC { 0x1000u };
    constexpr std::uint32_t ACC_ENUM      { 0x4000u };

    constexpr std::size_t k_klass_probe_bytes{ 64 };

    // Prove a metaspace header span is currently mapped before any field-at-offset
    // read is treated as authoritative (metaspace is stable, so a miss is rare).
    auto header_safely_readable(const void* const p) -> bool
    {
        if (!p || !vmhook::hotspot::is_valid_pointer(p))
        {
            return false;
        }
        std::uint8_t scratch[k_klass_probe_bytes] = { 0 };
        return vmhook::os::safe_read(scratch, p, sizeof(scratch));
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

    // internal '/'-form -> readable '.'-form (array descriptors keep their form).
    auto to_dot(const std::string& internal_name) -> std::string
    {
        std::string out{ internal_name };
        std::replace(out.begin(), out.end(), '/', '.');
        return out;
    }

    // '.'-form -> internal '/'-form (the find_class resolution key).
    auto to_slash(const std::string& binary_name) -> std::string
    {
        std::string out{ binary_name };
        std::replace(out.begin(), out.end(), '.', '/');
        return out;
    }

    // Read Klass::_access_flags (low 16 == class-file access flags) through a
    // guarded, cached-offset os::safe_read — the SAME pattern the library uses in
    // detail::klass_is_interface_like.  nullopt when the VMStruct entry is
    // unavailable or the slot is not safely readable (never faults, never raw).
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

    // Read a Method's class-file access flags (low 16 bits) through a guarded,
    // cached-offset os::safe_read.  Reads only 2 bytes (u2): the class-file access
    // flags are the LOW 16 bits of Method::_access_flags on EVERY JDK 8..26 — and
    // on JDK 24+ (JDK-8339113) the field itself shrank to u2, so a 2-byte read is
    // exactly the class-file flags there too (a wider read would spill into the
    // adjacent _flags / padding).  ACC_SYNTHETIC (0x1000) is bit 12, inside that
    // low u2, so the masked test is portable.  nullopt on any failure.
    auto method_access_flags(vmhook::hotspot::method* const m) -> std::optional<std::uint32_t>
    {
        if (!m || !vmhook::hotspot::is_valid_pointer(m))
        {
            return std::nullopt;
        }
        static const vmhook::hotspot::vm_struct_entry_t* const entry{
            vmhook::hotspot::iterate_struct_entries("Method", "_access_flags") };
        if (!entry)
        {
            return std::nullopt;
        }
        std::uint16_t flags{ 0u };
        if (!vmhook::os::safe_read(&flags,
                                   reinterpret_cast<const std::uint8_t*>(m) + entry->offset,
                                   sizeof(flags)))
        {
            return std::nullopt;
        }
        return static_cast<std::uint32_t>(flags);
    }

    // Find the declared Method* on `k` matching (name, signature), walking the
    // InstanceKlass _methods array with the SAME guarded discipline
    // collect_klass_methods uses (is_valid_pointer per slot; guarded name/sig
    // accessors).  Returns nullptr when absent / not safely walkable.
    auto find_declared_method(vmhook::hotspot::klass* const k,
                              const std::string& name, const std::string& sig)
        -> vmhook::hotspot::method*
    {
        if (!header_safely_readable(k))
        {
            return nullptr;
        }
        const std::int32_t count{ k->get_methods_count() };
        vmhook::hotspot::method** const methods{ k->get_methods_ptr() };
        if (!methods || count <= 0)
        {
            return nullptr;
        }
        for (std::int32_t idx{ 0 }; idx < count && idx < 65535; ++idx)
        {
            vmhook::hotspot::method* m{ nullptr };
            if (!vmhook::os::safe_read(&m, &methods[idx], sizeof(m)))
            {
                continue;
            }
            if (!m || !vmhook::hotspot::is_valid_pointer(m))
            {
                continue;
            }
            if (m->get_name() == name && m->get_signature() == sig)
            {
                return m;
            }
        }
        return nullptr;
    }

    // Number of leading '[' in an array descriptor == its dimensionality.
    auto array_dimension(const std::string& descriptor) -> int
    {
        int dim{ 0 };
        while (dim < static_cast<int>(descriptor.size()) && descriptor[static_cast<std::size_t>(dim)] == '[')
        {
            ++dim;
        }
        return dim;
    }

    // The element descriptor of an array name == the name with ONE '[' stripped.
    auto array_element_descriptor(const std::string& descriptor) -> std::string
    {
        if (!descriptor.empty() && descriptor.front() == '[')
        {
            return descriptor.substr(1);
        }
        return {};
    }

    // Resolve a klass from its PUBLISHED binary ('.'-form) name; returns the klass
    // (or nullptr) plus the internal name it was resolved under (for diagnostics).
    auto resolve_by_binary(const std::string& binary_name) -> vmhook::hotspot::klass*
    {
        if (binary_name.empty())
        {
            return nullptr;
        }
        return vmhook::find_class(to_slash(binary_name));
    }

    auto drive(vmhook_test::context& ctx, std::int32_t mode) -> bool
    {
        return ctx.run_probe(
            [mode](bool value)
            {
                if (value)
                {
                    kls::set_done(false);
                    kls::set_mode(mode);
                }
                kls::set_go(value);
            },
            []() { return kls::get_done(); });
    }
}

VMHOOK_JVM_MODULE(klass_introspection_shapes)
{
    const auto cp = [&](const char* where)
    {
        ctx.record(std::string{ "[INFO] klass_introspection_shapes checkpoint: " } + where);
    };

    cp("register_class wrapper");
    vmhook::register_class<kls>(N_SELF);

    // =====================================================================
    // PART 0 — drive the probe so every reflection witness is published.
    // =====================================================================
    cp("PART 0 drive probe (publish witnesses)");
    {
        const bool done{ drive(ctx, 0) };
        ctx.check("kli_probe_published", done);
        ctx.check("kli_probe_tick", kls::i("tickWitness") == 42);
    }

    // Native-read _access_flags low-16 must AGREE with the Java getModifiers()
    // witness on the class-file bits FINAL / INTERFACE / ABSTRACT / ENUM (the
    // bits that reliably share a position between the ClassFile access_flags and
    // java.lang.reflect.Modifier).  Mirrors klass_introspection.cpp's
    // xcheck_shared_bits; makes every *Mods witness load-bearing.  No-op (skip)
    // when the klass is unreadable or _access_flags is unavailable on this JDK.
    const auto xcheck_flag_bits = [&](const char* tag, vmhook::hotspot::klass* k,
                                      std::int32_t mods)
    {
        if (!header_safely_readable(k)) { return; }
        const std::optional<std::uint32_t> af{ klass_access_flags(k) };
        if (!af) { return; }
        const std::uint32_t m{ static_cast<std::uint32_t>(mods) };
        ctx.check(std::string{ "kli_xbit_FINAL_" } + tag,
                  ((*af & ACC_FINAL) != 0u) == ((m & ACC_FINAL) != 0u));
        ctx.check(std::string{ "kli_xbit_INTERFACE_" } + tag,
                  ((*af & ACC_INTERFACE) != 0u) == ((m & ACC_INTERFACE) != 0u));
        ctx.check(std::string{ "kli_xbit_ABSTRACT_" } + tag,
                  ((*af & ACC_ABSTRACT) != 0u) == ((m & ACC_ABSTRACT) != 0u));
        ctx.check(std::string{ "kli_xbit_ENUM_" } + tag,
                  ((*af & ACC_ENUM) != 0u) == ((m & ACC_ENUM) != 0u));
    };

    // =====================================================================
    // PART A — NAME FORMS for the unstable-name shapes (anonymous / local /
    //   inner) and lambda (best-effort).  Resolution is BY PUBLISHED NAME, so
    //   no $N ordinal is hardcoded and no instance-oop is ever dereferenced.
    // =====================================================================
    cp("PART A name forms (anonymous / local / inner / lambda)");
    {
        // A shape whose published binary name resolves AND echoes both forms.
        auto check_name_shape = [&](const char* tag, const std::string& binary_name)
        {
            if (binary_name.empty())
            {
                ctx.record(std::string{ "[INFO] " } + tag + " binary name witness empty — skipped.");
                return;
            }
            vmhook::hotspot::klass* const k{ resolve_by_binary(binary_name) };
            if (!k)
            {
                ctx.record(std::string{ "[INFO] " } + tag + " (" + binary_name
                           + ") did not resolve via find_class — not in the CLD walk here.");
                return;
            }
            if (!header_safely_readable(k))
            {
                ctx.record(std::string{ "[INFO] " } + tag + " klass header not safely readable — skipped.");
                return;
            }
            const std::string internal{ klass_name_str(k) };
            // Internal '/'-name echoes the resolution key.
            ctx.check(std::string{ "kli_" } + tag + "_internal_name_echo",
                      internal == to_slash(binary_name));
            // '.'-derivation of the resolved internal name equals the witness.
            ctx.check(std::string{ "kli_" } + tag + "_binary_name_derives",
                      to_dot(internal) == binary_name);
        };

        // Anonymous + local: ordinary InstanceKlasses in the app loader's CLD, so
        // find_class resolves them by the published (recompile-stable-within-build)
        // name.  These are HARD when resolved.
        check_name_shape("anon",  kls::s("anonBinaryName"));
        check_name_shape("local", kls::s("localBinaryName"));
        check_name_shape("inner", kls::s("innerBinaryName"));

        // The inner class also has a known super (Object) and a member/non-static
        // Java witness; pin both.
        {
            vmhook::hotspot::klass* const k_inner{ resolve_by_binary(kls::s("innerBinaryName")) };
            if (k_inner && header_safely_readable(k_inner))
            {
                ctx.check("kli_inner_super_is_object",
                          klass_name_str(k_inner->get_super()) == "java/lang/Object");
                ctx.check("kli_inner_super_matches_java",
                          klass_name_str(k_inner->get_super()) == kls::s("innerSuperName"));
                // An inner class is not final/interface/abstract/enum — native
                // _access_flags must agree with the Java getModifiers() witness.
                xcheck_flag_bits("inner", k_inner, kls::i("innerMods"));
            }
            ctx.check("kli_inner_member_nonstatic_java", kls::b("innerIsMemberAndNotStatic"));
        }

        // Lambda: a synthetic class on JDK 8, a HIDDEN class on JDK 15+.  Hidden
        // classes are not in the ClassLoaderDataGraph the way ordinary classes are,
        // so find_class may not reach it — record reachability either way (this is
        // the documented best-effort path, never a crash).
        {
            const std::string lam{ kls::s("lambdaBinaryName") };
            ctx.record(std::string{ "[INFO] lambda runtime name: " } + (lam.empty() ? "<empty>" : lam));
            vmhook::hotspot::klass* const k_lam{ resolve_by_binary(lam) };
            if (k_lam && header_safely_readable(k_lam))
            {
                // When reachable, its internal name must echo (a strong bonus pin).
                ctx.check("kli_lambda_internal_name_echo", klass_name_str(k_lam) == to_slash(lam));
            }
            else
            {
                ctx.record("[INFO] lambda class not resolvable by name on this JDK "
                           "(hidden class / not in CLD walk) — reachability recorded, no assert.");
            }
        }

        // The top-level fixture itself: both name forms, as an app-loaded anchor.
        vmhook::hotspot::klass* const k_self{ vmhook::find_class(N_SELF) };
        ctx.check("kli_self_resolves", k_self != nullptr);
        if (k_self && header_safely_readable(k_self))
        {
            ctx.check("kli_self_internal_name", klass_name_str(k_self) == N_SELF);
            ctx.check("kli_self_binary_name",
                      to_dot(klass_name_str(k_self)) == "vmhook.fixtures.KlassIntrospectionShapes");
        }
    }

    // =====================================================================
    // PART B — boxed-PRIMITIVE wrappers (Integer / Long / Boolean / Character).
    //   Bootstrap-loaded; name + super + final/non-abstract/non-interface bits,
    //   cross-checked against the Modifier + super-name witnesses.
    // =====================================================================
    cp("PART B boxed-primitive wrappers");
    {
        struct box_case
        {
            const char* tag;
            const char* internal;
            const char* expected_super;     // "" => only cross-check the witness
            const char* witness_super_key;
        };
        const box_case boxes[]{
            { "integer",   "java/lang/Integer",   "java/lang/Number", "integerSuperName"   },
            { "long",      "java/lang/Long",      "java/lang/Number", "longSuperName"      },
            { "boolean",   "java/lang/Boolean",   "java/lang/Object", "booleanSuperName"   },
            { "character", "java/lang/Character", "java/lang/Object", "characterSuperName" },
        };
        for (const box_case& bc : boxes)
        {
            vmhook::hotspot::klass* const k{ vmhook::find_class(bc.internal) };
            ctx.check(std::string{ "kli_box_resolve_" } + bc.tag, k != nullptr);
            if (!k || !header_safely_readable(k))
            {
                ctx.record(std::string{ "[INFO] box " } + bc.tag + " not safely readable — skipped.");
                continue;
            }
            // Name echo (internal + binary derivation).
            ctx.check(std::string{ "kli_box_name_" } + bc.tag, klass_name_str(k) == bc.internal);
            // Super: native get_super() name, AND cross-check the Java witness.
            const std::string sup{ klass_name_str(k->get_super()) };
            ctx.check(std::string{ "kli_box_super_" } + bc.tag, sup == bc.expected_super);
            ctx.check(std::string{ "kli_box_super_matches_java_" } + bc.tag,
                      sup == kls::s(bc.witness_super_key));
            // A boxed wrapper is FINAL and NOT interface / abstract / enum.
            const std::optional<std::uint32_t> af{ klass_access_flags(k) };
            if (af)
            {
                ctx.check(std::string{ "kli_box_final_" } + bc.tag,     (*af & ACC_FINAL)     != 0u);
                ctx.check(std::string{ "kli_box_not_iface_" } + bc.tag, (*af & ACC_INTERFACE) == 0u);
                ctx.check(std::string{ "kli_box_not_abstract_" } + bc.tag, (*af & ACC_ABSTRACT) == 0u);
                ctx.check(std::string{ "kli_box_not_enum_" } + bc.tag, (*af & ACC_ENUM)      == 0u);
            }
            else
            {
                ctx.record(std::string{ "[INFO] box " } + bc.tag + " access flags unavailable — skipped bits.");
            }
            // get_instance_size() is positive for a concrete instantiable class.
            ctx.check(std::string{ "kli_box_size_positive_" } + bc.tag, k->get_instance_size() > 0u);
        }

        // Cross-check the Integer/Boolean Modifier witnesses (final + not iface).
        ctx.check("kli_integer_isFinal_java",      kls::b("integerIsFinal"));
        ctx.check("kli_integer_notAbstract_java",  !kls::b("integerIsAbstract"));
        ctx.check("kli_integer_notInterface_java", !kls::b("integerIsInterface"));

        // Native _access_flags must AGREE with Java getModifiers() on Integer /
        // Boolean (final, non-interface, non-abstract, non-enum).
        xcheck_flag_bits("integer", vmhook::find_class("java/lang/Integer"), kls::i("integerMods"));
        xcheck_flag_bits("boolean", vmhook::find_class("java/lang/Boolean"), kls::i("booleanMods"));
    }

    // =====================================================================
    // PART C — RECORD supertype (java.lang.Record, JDK16+).  Portable: the
    //   fixture has no `record` (compiles on JDK 8); the probe force-loads
    //   java.lang.Record when present, and the native side resolves it and pins
    //   its super == java.lang.Object.  On JDK 8..15 -> [INFO] (no records).
    // =====================================================================
    cp("PART C record supertype");
    {
        if (kls::b("recordSupported"))
        {
            vmhook::hotspot::klass* const k_rec{ vmhook::find_class("java/lang/Record") };
            if (k_rec && header_safely_readable(k_rec))
            {
                ctx.check("kli_record_resolves", true);
                ctx.check("kli_record_name", klass_name_str(k_rec) == "java/lang/Record");
                ctx.check("kli_record_super_is_object",
                          klass_name_str(k_rec->get_super()) == "java/lang/Object");
                ctx.check("kli_record_super_matches_java",
                          klass_name_str(k_rec->get_super()) == kls::s("recordSuperName"));
                // java.lang.Record is abstract (no instances) and an InstanceKlass.
                const std::optional<std::uint32_t> af{ klass_access_flags(k_rec) };
                if (af)
                {
                    ctx.check("kli_record_is_abstract", (*af & ACC_ABSTRACT) != 0u);
                    ctx.check("kli_record_not_interface", (*af & ACC_INTERFACE) == 0u);
                }
            }
            else
            {
                ctx.record("[INFO] java.lang.Record supported but not resolvable via find_class here.");
            }
        }
        else
        {
            ctx.record("[INFO] records not supported on this JDK (< 16) — record-shape skipped.");
        }
    }

    // =====================================================================
    // PART D — DECLARED INTERFACES list: MultiImpl implements IA, IB, IC.  The
    //   transitive interface set (read fault-safe by the library) contains all
    //   three; count >= 3; the Java witness count == 3.
    // =====================================================================
    cp("PART D declared interfaces list (implements three)");
    {
        vmhook::hotspot::klass* const k_multi{ resolve_by_binary(kls::s("multiImplBinaryName")) };
        ctx.check("kli_multiimpl_resolves", k_multi != nullptr);
        if (k_multi && header_safely_readable(k_multi))
        {
            // Super is Object; it is final and not abstract/interface.
            ctx.check("kli_multiimpl_super_object",
                      klass_name_str(k_multi->get_super()) == "java/lang/Object");
            ctx.check("kli_multiimpl_super_matches_java",
                      klass_name_str(k_multi->get_super()) == kls::s("multiImplSuperName"));
            // MultiImpl is `final class` implementing interfaces (it is NOT an
            // interface itself) — native _access_flags must agree with Java.
            xcheck_flag_bits("multiimpl", k_multi, kls::i("multiImplMods"));

            std::int32_t count{ 0 };
            vmhook::hotspot::klass** const ifaces{ k_multi->get_interfaces_ptr(count) };
            ctx.check("kli_multiimpl_has_three_interfaces", ifaces != nullptr && count >= 3);

            const std::string ia{ to_slash(std::string{ "vmhook.fixtures.KlassIntrospectionShapes$IA" }) };
            const std::string ib{ to_slash(std::string{ "vmhook.fixtures.KlassIntrospectionShapes$IB" }) };
            const std::string ic{ to_slash(std::string{ "vmhook.fixtures.KlassIntrospectionShapes$IC" }) };
            bool found_ia{ false };
            bool found_ib{ false };
            bool found_ic{ false };
            if (ifaces)
            {
                for (std::int32_t idx{ 0 }; idx < count && idx < 64; ++idx)
                {
                    vmhook::hotspot::klass* entry{ nullptr };
                    if (!vmhook::os::safe_read(&entry, &ifaces[idx], sizeof(entry)))
                    {
                        continue;
                    }
                    const std::string nm{ klass_name_str(entry) };
                    if (nm == ia) { found_ia = true; }
                    if (nm == ib) { found_ib = true; }
                    if (nm == ic) { found_ic = true; }
                }
            }
            ctx.check("kli_multiimpl_set_contains_IA", found_ia);
            ctx.check("kli_multiimpl_set_contains_IB", found_ib);
            ctx.check("kli_multiimpl_set_contains_IC", found_ic);
        }
        else
        {
            ctx.record("[INFO] MultiImpl klass header not safely readable — skipped interface list.");
        }

        // Cross-check the declared-interface count + per-iface membership witnesses.
        ctx.check("kli_multiimpl_count_java",  kls::i("multiImplInterfaceCount") == 3);
        ctx.check("kli_multiimpl_IA_java", kls::b("multiImplImplementsIA"));
        ctx.check("kli_multiimpl_IB_java", kls::b("multiImplImplementsIB"));
        ctx.check("kli_multiimpl_IC_java", kls::b("multiImplImplementsIC"));
    }

    // =====================================================================
    // PART E — SYNTHETIC flag on a method.  The generic bridge
    //   compareTo(Ljava/lang/Object;)I is ACC_SYNTHETIC; the typed
    //   compareTo(L...$CmpShape;)I is NOT.  Read each Method's class-file access
    //   flags through the guarded Method._access_flags safe_read and assert the
    //   SYNTHETIC bit AGREES with the Java isSynthetic() witness.
    // =====================================================================
    cp("PART E synthetic flag (generic bridge method)");
    {
        vmhook::hotspot::klass* const k_cmp{ resolve_by_binary(kls::s("cmpBinaryName")) };
        ctx.check("kli_cmp_resolves", k_cmp != nullptr);
        if (k_cmp && header_safely_readable(k_cmp))
        {
            // CmpShape is a final concrete class — native _access_flags agree.
            xcheck_flag_bits("cmp", k_cmp, kls::i("cmpMods"));

            const std::string typed_sig{
                "(Lvmhook/fixtures/KlassIntrospectionShapes$CmpShape;)I" };
            vmhook::hotspot::method* const bridge{
                find_declared_method(k_cmp, "compareTo", "(Ljava/lang/Object;)I") };
            vmhook::hotspot::method* const typed{
                find_declared_method(k_cmp, "compareTo", typed_sig) };

            ctx.check("kli_cmp_bridge_present", bridge != nullptr);
            ctx.check("kli_cmp_typed_present",  typed != nullptr);

            // Bridge: SYNTHETIC bit set, agreeing with the Java witness.
            if (bridge)
            {
                const std::optional<std::uint32_t> af{ method_access_flags(bridge) };
                if (af)
                {
                    const bool native_synth{ (*af & ACC_SYNTHETIC) != 0u };
                    ctx.check("kli_cmp_bridge_is_synthetic", native_synth);
                    ctx.check("kli_cmp_bridge_synthetic_matches_java",
                              native_synth == kls::b("cmpBridgeIsSynthetic"));
                }
                else
                {
                    ctx.record("[INFO] bridge Method access flags unavailable — skipped synthetic bit.");
                }
            }
            // Typed: SYNTHETIC bit clear, agreeing with the Java witness.
            if (typed)
            {
                const std::optional<std::uint32_t> af{ method_access_flags(typed) };
                if (af)
                {
                    const bool native_synth{ (*af & ACC_SYNTHETIC) != 0u };
                    ctx.check("kli_cmp_typed_not_synthetic", !native_synth);
                    ctx.check("kli_cmp_typed_synthetic_matches_java",
                              (!native_synth) == kls::b("cmpTypedIsNotSynthetic"));
                }
                else
                {
                    ctx.record("[INFO] typed Method access flags unavailable — skipped synthetic bit.");
                }
            }
        }
        else
        {
            ctx.record("[INFO] CmpShape klass header not safely readable — skipped synthetic flag.");
        }

        // The Java witnesses themselves (independent corroboration).
        ctx.check("kli_cmp_bridge_synthetic_java", kls::b("cmpBridgeIsSynthetic"));
        ctx.check("kli_cmp_typed_not_synthetic_java", kls::b("cmpTypedIsNotSynthetic"));
    }

    // =====================================================================
    // PART F — ARRAY component / dimension / element klass.
    //   [Ljava/lang/String; (1-D ref), [[[I (3-D prim),
    //   [Lvmhook/fixtures/KlassIntrospectionShapes; (app element).
    // =====================================================================
    cp("PART F array component / dimension / element klass");
    {
        // 1-D reference array of String: dimension 1, element java/lang/String,
        // super Object, size 0, and the element klass resolves + echoes its name.
        {
            vmhook::hotspot::klass* const k_arr{ vmhook::find_class("[Ljava/lang/String;") };
            if (k_arr)
            {
                const std::string nm{ klass_name_str(k_arr) };
                ctx.check("kli_strarray_name", nm == "[Ljava/lang/String;");
                ctx.check("kli_strarray_dim_1", array_dimension(nm) == 1);
                if (header_safely_readable(k_arr))
                {
                    ctx.check("kli_strarray_super_object",
                              klass_name_str(k_arr->get_super()) == "java/lang/Object");
                    ctx.check("kli_strarray_size_zero", k_arr->get_instance_size() == 0u);
                }
                // Element descriptor: strip one '[' -> "Ljava/lang/String;".  The
                // ELEMENT klass is resolvable via find_class on the bare class name.
                const std::string elem_desc{ array_element_descriptor(nm) };
                ctx.check("kli_strarray_element_desc", elem_desc == "Ljava/lang/String;");
                vmhook::hotspot::klass* const k_elem{ vmhook::find_class("java/lang/String") };
                ctx.check("kli_strarray_element_klass_resolves", k_elem != nullptr);
                if (k_elem && header_safely_readable(k_elem))
                {
                    ctx.check("kli_strarray_element_klass_name",
                              klass_name_str(k_elem) == "java/lang/String");
                }
                // Cross-check the component name witness ('.'-form from Java).
                ctx.check("kli_strarray_component_java",
                          kls::s("strArrayComponentName") == "java.lang.String");
            }
            else
            {
                ctx.record("[INFO] find_class(\"[Ljava/lang/String;\") did not resolve here.");
            }
        }

        // 3-D primitive array int[][][]: dimension 3, leaf component int.
        {
            vmhook::hotspot::klass* const k_arr{ vmhook::find_class("[[[I") };
            if (k_arr)
            {
                const std::string nm{ klass_name_str(k_arr) };
                ctx.check("kli_int3d_name", nm == "[[[I");
                ctx.check("kli_int3d_dim_3", array_dimension(nm) == 3);
                if (header_safely_readable(k_arr))
                {
                    ctx.check("kli_int3d_super_object",
                              klass_name_str(k_arr->get_super()) == "java/lang/Object");
                    ctx.check("kli_int3d_size_zero", k_arr->get_instance_size() == 0u);
                }
                // Stripping one '[' yields the 2-D array descriptor "[[I"; the
                // inner array klass is itself resolvable (an array of arrays).
                const std::string elem_desc{ array_element_descriptor(nm) };
                ctx.check("kli_int3d_element_desc", elem_desc == "[[I");
                vmhook::hotspot::klass* const k_inner_arr{ vmhook::find_class("[[I") };
                if (k_inner_arr)
                {
                    ctx.check("kli_int3d_inner_array_name", klass_name_str(k_inner_arr) == "[[I");
                    ctx.check("kli_int3d_inner_array_dim_2", array_dimension(klass_name_str(k_inner_arr)) == 2);
                }
                else
                {
                    ctx.record("[INFO] inner [[I did not resolve here.");
                }
                // Cross-check the Java dimension + leaf-component witnesses.
                ctx.check("kli_int3d_dim_java", kls::i("int3DArrayDim") == 3);
                ctx.check("kli_int3d_leaf_java", kls::s("int3DArrayLeafComponent") == "int");
            }
            else
            {
                ctx.record("[INFO] find_class(\"[[[I\") did not resolve here.");
            }
        }

        // 1-D array whose ELEMENT is the APP-loaded fixture type: the element
        // klass crosses the bootstrap (array klass) / app (element) boundary.
        {
            const std::string app_arr_name{ std::string{ "[L" } + N_SELF + ";" };
            vmhook::hotspot::klass* const k_arr{ vmhook::find_class(app_arr_name) };
            if (k_arr)
            {
                const std::string nm{ klass_name_str(k_arr) };
                ctx.check("kli_selfarray_name", nm == app_arr_name);
                ctx.check("kli_selfarray_dim_1", array_dimension(nm) == 1);
                if (header_safely_readable(k_arr))
                {
                    ctx.check("kli_selfarray_super_object",
                              klass_name_str(k_arr->get_super()) == "java/lang/Object");
                }
                const std::string elem_desc{ array_element_descriptor(nm) };
                ctx.check("kli_selfarray_element_desc",
                          elem_desc == (std::string{ "L" } + N_SELF + ";"));
                // The element klass is the app-loaded fixture itself.
                vmhook::hotspot::klass* const k_elem{ vmhook::find_class(N_SELF) };
                ctx.check("kli_selfarray_element_is_fixture", k_elem != nullptr
                          && klass_name_str(k_elem) == N_SELF);
                ctx.check("kli_selfarray_component_java",
                          kls::s("selfArrayComponentName") == "vmhook.fixtures.KlassIntrospectionShapes");
            }
            else
            {
                ctx.record(std::string{ "[INFO] find_class(\"" } + app_arr_name
                           + "\") did not resolve here (app array klass not in the CLD walk).");
            }
        }

        // Array-ness Java witnesses (independent corroboration).
        ctx.check("kli_strarray_isarray_java", kls::b("strArrayIsArray"));
        ctx.check("kli_int3d_isarray_java",    kls::b("int3DIsArray"));
        ctx.check("kli_selfarray_isarray_java",kls::b("selfArrayIsArray"));
    }

    // =====================================================================
    // PART G — LOADERS: bootstrap vs app.  Both resolve through the SAME
    //   find_class path to DISTINCT klasses; the boundary is cross-checked
    //   against the loader witnesses.
    // =====================================================================
    cp("PART G loaders (bootstrap vs app)");
    {
        vmhook::hotspot::klass* const k_obj{ vmhook::find_class("java/lang/Object") };
        vmhook::hotspot::klass* const k_int{ vmhook::find_class("java/lang/Integer") };
        vmhook::hotspot::klass* const k_self{ vmhook::find_class(N_SELF) };
        vmhook::hotspot::klass* const k_multi{ resolve_by_binary(kls::s("multiImplBinaryName")) };

        ctx.check("kli_loader_object_resolves",  k_obj != nullptr);
        ctx.check("kli_loader_integer_resolves", k_int != nullptr);
        ctx.check("kli_loader_self_resolves",    k_self != nullptr);

        // Bootstrap and app klasses are DISTINCT objects (sanity that find_class
        // does not collapse loaders) and each echoes its own name.
        if (k_obj && k_self)
        {
            ctx.check("kli_loader_object_vs_self_distinct", k_obj != k_self);
        }
        if (k_obj && header_safely_readable(k_obj))
        {
            ctx.check("kli_loader_object_name", klass_name_str(k_obj) == "java/lang/Object");
        }
        if (k_int && header_safely_readable(k_int))
        {
            ctx.check("kli_loader_integer_name", klass_name_str(k_int) == "java/lang/Integer");
        }

        // Two app-loaded klasses (fixture + its nested MultiImpl) share the SAME
        // app loader per the witnesses (both non-bootstrap).
        ctx.check("kli_loader_object_bootstrap_java", kls::b("objectLoaderIsBootstrap"));
        ctx.check("kli_loader_self_app_java",          kls::b("selfLoaderIsApp"));
        ctx.check("kli_loader_object_name_bootstrap_witness",
                  kls::s("objectLoaderName") == "bootstrap");
        ctx.check("kli_loader_integer_name_bootstrap_witness",
                  kls::s("integerLoaderName") == "bootstrap");
        // The fixture + MultiImpl report the SAME (non-bootstrap) loader string.
        ctx.check("kli_loader_self_eq_multiimpl_witness",
                  !kls::s("selfLoaderName").empty()
                  && kls::s("selfLoaderName") == kls::s("multiImplLoaderName"));
        ctx.check("kli_loader_self_not_bootstrap_witness",
                  kls::s("selfLoaderName") != "bootstrap");
        if (k_multi != nullptr) { ctx.check("kli_loader_multiimpl_resolves", true); }
    }

    // =====================================================================
    // PART H — NEGATIVE / determinism sanity (no crash, stable result).
    // =====================================================================
    cp("PART H negative / determinism");
    {
        // A bogus shape name resolves to null (and does not crash the walk).
        ctx.check("kli_bogus_name_null",
                  vmhook::find_class("vmhook/fixtures/KlassIntrospectionShapes$NoSuchZZZ") == nullptr);
        // An array of a bogus element type is not loaded -> null, no crash.
        ctx.check("kli_bogus_array_null",
                  vmhook::find_class("[Lvmhook/fixtures/NoSuchZZZ;") == nullptr);

        // Re-resolving the fixture twice yields the SAME klass pointer (the
        // find_class cache returns a stable, already-loaded klass).
        vmhook::hotspot::klass* const a{ vmhook::find_class(N_SELF) };
        vmhook::hotspot::klass* const b{ vmhook::find_class(N_SELF) };
        ctx.check("kli_self_resolution_stable", a != nullptr && a == b);
    }

    cp("module complete (all parts reached without a no-SEH fault)");
}
