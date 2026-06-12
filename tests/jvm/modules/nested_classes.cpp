// nested_classes JVM test module  (feature area: classes / klass resolution)
//
// THE nested-class authority: exhaustively exercises vmhook's handling of EVERY
// Java nested/inner-class shape, resolved by its javac-generated internal
// `$`-name through vmhook::find_class, with the headline contract being the
// synthetic `this$N` outer back-reference of a non-static inner class decoded
// back to the very enclosing instance javac wired in.
//
// SHAPES COVERED (nest_* check prefix), each force-loaded by the fixture:
//
//   STABLE-name shapes (a fixed Java `$`-name identifies them):
//     * STATIC nested class      NestedClasses$Host$StaticNested  (no this$0)
//     * non-static INNER class   NestedClasses$Host$Inner         (this$0 -> Host)
//     * a SECOND inner of Host   NestedClasses$Host$SecondInner   (own this$0 -> Host)
//     * INNER inside INNER       NestedClasses$Host$Inner$InnerInner
//                                   (synthetic field is named this$1 -> Inner)
//     * STATIC-in-STATIC (A$B$C$D, deeply nested, no synthetic ref at any level)
//                                NestedClasses$Host$StaticNested$DeepNested
//     * NESTED INTERFACE         NestedClasses$NestedIface
//     * NESTED ENUM              NestedClasses$NestedEnum
//     * NESTED ANNOTATION        NestedClasses$NestedAnno
//     * GENERIC nested (erased)  NestedClasses$GenericBox  (boxed: Ljava/lang/Object;)
//
//   UNSTABLE-name shapes (the `$N` ordinal is source-order-assigned, so NO fixed
//   name identifies them — resolved by reading the klass off a PUBLISHED INSTANCE
//   oop, never by name; the JDK8 enumeration quirk is characterised [INFO]):
//     * ANONYMOUS class          NestedClasses$1     (a Runnable; this$0 -> fixture)
//     * LOCAL class              NestedClasses$1LocalCounter (this$0 -> fixture)
//
// WHAT THIS MODULE PROVES on a live JVM (Java 8/11/17/21/24/25 x MSVC/Clang/GCC):
//   * find_class resolves EACH STABLE nested klass by its internal `$` name, and
//     the resolved klass's own name symbol echoes that exact `$` name (right
//     klass, not a stale cache hit), and the resolved klasses are distinct;
//   * a field read off each instance returns the mirrored value (outerField==7,
//     value==42, innerValue==99, secondValue==55, innerInnerValue==11,
//     deepValue==1000, GenericBox.boxed unboxes to 321);
//   * the decoded instance OOPs carry the klass that find_class resolved
//     (klass_from_oop(instance) == find_class("...$Name")) — ties by-name klass
//     resolution to the actual objects the field reads run against;
//   * the SYNTHETIC `this$0` of Inner / SecondInner / anon / local decodes to a
//     usable wrapper whose OOP is IDENTICAL to the enclosing instance, and the
//     SYNTHETIC `this$1` of InnerInner decodes to the enclosing Inner instance —
//     javac's hidden outer links point exactly where it wired them (is_valid +
//     pointer identity);  the depth shows up in the field NAME (this$0 vs this$1);
//   * the documented composites — outerField+innerValue==106, secondValue path
//     ==62, innerInner sum-through-both-outers==117, deepDoubled==2000 — proven
//     the robust JDK-independent way by driving the methods through REAL bytecode
//     in the fixture probe (modes 1 & 2) and reading the published results, AND
//     attempted natively (degrades to [INFO], never FAIL, when the interpreter
//     call gate for a no-arg int instance method is unavailable on a JDK build);
//   * the UNSTABLE anon/local klasses resolve from their instance oops, carry the
//     expected Outer$N... internal-name shape, and their this$0 points back at
//     the fixture SELF instance.
//
// Harness shape mirrors enum_singleton / field_object_ref: register_class for each
// wrapper, a `mode` selector with a `done` reset on the rising edge of go, and a
// dense battery of ctx.check()s.
//
// SAFETY (mandatory; an edit must preserve all of it):
//   * Every OOP/klass/symbol deref is gated with vmhook::hotspot::is_valid_pointer.
//   * Every reader that RAW-derefs an instance oop (klass_from_oop at oop+8;
//     get_field()->get() memcpy at instance+offset; the this$N reference-slot read)
//     is additionally guarded by an os::safe_read probe of the oop HEADER first:
//     a GC-relocated young-gen singleton's OLD address still passes is_valid_pointer
//     yet faults on the raw load, and MinGW/gcc have no SEH net.  A probe miss
//     degrades to [INFO] + skips the STRONG assertion (never a fault, never a
//     vacuous pass).  Checks that do NOT deref an oop (find_class, name echo,
//     has_value, distinctness, the pure pointer-IDENTITY comparison) stay HARD.
//   * NO NEW RAW VMSTRUCT DEREFS: every klass/symbol read uses an existing guarded
//     library accessor (find_class / klass::get_name / symbol::to_string /
//     klass_from_oop); the only os::safe_read is the header-liveness probe.
//   * All value_t / unique_ptr extractions are COPY-INIT (never brace-init):
//     value_t has a templated conversion operator, so unique_ptr<W>{ proxy->get() }
//     is ambiguous on MSVC.
//   * No hooks are armed.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace
{
    // ---- Wrappers for the fixture + its nested classes ---------------------
    // Each is registered (below, in the module body) to its internal `$` name so
    // resolve_klass()/static_field()/get_field() find the right klass.

    // The top-level fixture: used for its static publication fields, so a null
    // instance is fine (every accessor here is a static_field reach).
    class nc : public vmhook::object<nc>
    {
    public:
        explicit nc(vmhook::oop_t instance) noexcept
            : vmhook::object<nc>{ instance }
        {
        }

        // ---- handshake + scenario selector (all via static_field) ----
        static auto set_go(bool value) -> void       { static_field("go")->set(value); }
        static auto set_done(bool value) -> void      { static_field("done")->set(value); }
        static auto get_done() -> bool                { return static_field("done")->get(); }
        static auto set_mode(std::int32_t m) -> void  { static_field("mode")->set(m); }

        // ---- acquire a published nested-instance wrapper -------------------
        // COPY-INIT from value_t -> unique_ptr<W> (never brace-init): value_t has
        // a templated conversion operator, so unique_ptr<W>{ proxy->get() } is
        // ambiguous on MSVC.
        template<typename wrapper_type>
        static auto acquire(const char* name) -> std::unique_ptr<wrapper_type>
        {
            return static_field(name)->get();
        }

        // ---- the raw decoded OOP of a published reference static field -----
        // For the UNSTABLE shapes (anon/local) we cannot register a typed wrapper
        // by name, so we read the static field's compressed OOP and decode it to
        // a bare void* whose klass we resolve from the header.
        static auto field_oop(const char* name) -> void*
        {
            const auto proxy{ static_field(name) };
            if (!proxy.has_value())
            {
                return nullptr;
            }
            return vmhook::field_oop(*proxy);
        }

        // ---- published int (identity hashes, composite results) ------------
        static auto get_int(const char* name) -> std::int32_t
        {
            return static_field(name)->get();
        }
    };

    // The enclosing Host: holds `int outerField`.
    class host_w : public vmhook::object<host_w>
    {
    public:
        explicit host_w(vmhook::oop_t instance) noexcept
            : vmhook::object<host_w>{ instance }
        {
        }

        auto get_outer_field() const -> std::int32_t { return get_field("outerField")->get(); }
    };

    // The STATIC nested class: `int value` + `int doubled()`.
    class static_nested_w : public vmhook::object<static_nested_w>
    {
    public:
        explicit static_nested_w(vmhook::oop_t instance) noexcept
            : vmhook::object<static_nested_w>{ instance }
        {
        }

        auto get_value() const -> std::int32_t { return get_field("value")->get(); }

        // Calls the no-arg int instance method via the interpreter call gate.
        // Returns the value_t so the caller can distinguish "returned 84" from
        // "the call gate was unavailable" (monostate) and degrade to [INFO].
        auto call_doubled() const -> vmhook::method_proxy::value_t
        {
            const auto m{ get_method("doubled") };
            if (!m.has_value())
            {
                return vmhook::method_proxy::value_t{ std::monostate{} };
            }
            // COPY-INIT: value_t by value (never brace-init).
            const vmhook::method_proxy::value_t v = m->call();
            return v;
        }
    };

    // The non-static INNER class: `int innerValue`, synthetic `this$0` -> Host,
    // and `int outerPlusInner()`.
    class inner_w : public vmhook::object<inner_w>
    {
    public:
        explicit inner_w(vmhook::oop_t instance) noexcept
            : vmhook::object<inner_w>{ instance }
        {
        }

        auto get_inner_value() const -> std::int32_t { return get_field("innerValue")->get(); }

        // The synthetic outer back-reference: read the compressed OOP in the
        // `this$0` slot and decode it into a usable Host wrapper.
        auto get_this0_host() const -> std::unique_ptr<host_w> { return get_field("this$0")->get(); }

        // Whether the synthetic field resolves at all (descriptor 'L...;').
        auto this0_resolves() const -> bool { return get_field("this$0").has_value(); }

        // The descriptor of the synthetic this$0 slot (proves it names Host).
        auto this0_signature() const -> std::string
        {
            const auto proxy{ get_field("this$0") };
            if (!proxy.has_value())
            {
                return std::string{};
            }
            return std::string{ proxy->signature() };
        }

        // No-arg int instance method that reads outerField through this$0.
        auto call_outer_plus_inner() const -> vmhook::method_proxy::value_t
        {
            const auto m{ get_method("outerPlusInner") };
            if (!m.has_value())
            {
                return vmhook::method_proxy::value_t{ std::monostate{} };
            }
            const vmhook::method_proxy::value_t v = m->call();
            return v;
        }

        // Build an InnerInner against this Inner via newInnerInner() (best-effort;
        // returns the raw value_t so the caller can degrade to [INFO]).
        auto get_instance_oop() const -> void* { return this->vmhook::object_base::get_instance(); }
    };

    // The SECOND non-static inner of Host: `int secondValue`, own `this$0` -> Host.
    class second_inner_w : public vmhook::object<second_inner_w>
    {
    public:
        explicit second_inner_w(vmhook::oop_t instance) noexcept
            : vmhook::object<second_inner_w>{ instance }
        {
        }

        auto get_second_value() const -> std::int32_t { return get_field("secondValue")->get(); }
        auto this0_resolves() const -> bool { return get_field("this$0").has_value(); }
        auto get_this0_host() const -> std::unique_ptr<host_w> { return get_field("this$0")->get(); }
    };

    // The INNER-inside-INNER: `int innerInnerValue`, synthetic `this$1` -> Inner.
    // NOTE the field name is this$1 (depth 1), NOT this$0 — javac numbers the
    // synthetic outer reference by enclosing-instance depth.
    class inner_inner_w : public vmhook::object<inner_inner_w>
    {
    public:
        explicit inner_inner_w(vmhook::oop_t instance) noexcept
            : vmhook::object<inner_inner_w>{ instance }
        {
        }

        auto get_inner_inner_value() const -> std::int32_t { return get_field("innerInnerValue")->get(); }

        // The depth-1 synthetic field is named `this$1` and points at the Inner.
        auto this1_resolves() const -> bool { return get_field("this$1").has_value(); }
        auto get_this1_inner() const -> std::unique_ptr<inner_w> { return get_field("this$1")->get(); }
        auto this1_signature() const -> std::string
        {
            const auto proxy{ get_field("this$1") };
            if (!proxy.has_value())
            {
                return std::string{};
            }
            return std::string{ proxy->signature() };
        }

        // A non-static inner at depth 1 has NO this$0 of its own (its only
        // synthetic outer ref is this$1) — assert the negative.
        auto this0_absent() const -> bool { return !get_field("this$0").has_value(); }
    };

    // The deeply-nested (4-level, all static) class: `int deepValue` + deepDoubled().
    class deep_nested_w : public vmhook::object<deep_nested_w>
    {
    public:
        explicit deep_nested_w(vmhook::oop_t instance) noexcept
            : vmhook::object<deep_nested_w>{ instance }
        {
        }

        auto get_deep_value() const -> std::int32_t { return get_field("deepValue")->get(); }

        // A static nested class has NO synthetic outer reference at any level.
        auto no_this0() const -> bool { return !get_field("this$0").has_value(); }
        auto no_this1() const -> bool { return !get_field("this$1").has_value(); }
    };

    // The generic (erased) nested box: declared field `boxed` of erased type
    // java.lang.Object (descriptor Ljava/lang/Object;).
    class generic_box_w : public vmhook::object<generic_box_w>
    {
    public:
        explicit generic_box_w(vmhook::oop_t instance) noexcept
            : vmhook::object<generic_box_w>{ instance }
        {
        }

        auto boxed_resolves() const -> bool { return get_field("boxed").has_value(); }
        auto boxed_signature() const -> std::string
        {
            const auto proxy{ get_field("boxed") };
            if (!proxy.has_value())
            {
                return std::string{};
            }
            return std::string{ proxy->signature() };
        }
        // The boxed reference oop (an Integer); read as void* (the unique_ptr
        // decode is correctly rejected for an unregistered element type — we only
        // need the oop to be non-null/valid here).
        auto boxed_oop() const -> void*
        {
            const auto proxy{ get_field("boxed") };
            if (!proxy.has_value())
            {
                return nullptr;
            }
            return vmhook::field_oop(*proxy);
        }
    };

    // Fully-qualified internal `$` names (the STABLE resolution targets).
    constexpr const char* k_fixture_name      = "vmhook/fixtures/NestedClasses";
    constexpr const char* k_host_name         = "vmhook/fixtures/NestedClasses$Host";
    constexpr const char* k_static_name       = "vmhook/fixtures/NestedClasses$Host$StaticNested";
    constexpr const char* k_inner_name        = "vmhook/fixtures/NestedClasses$Host$Inner";
    constexpr const char* k_second_inner_name = "vmhook/fixtures/NestedClasses$Host$SecondInner";
    constexpr const char* k_inner_inner_name  = "vmhook/fixtures/NestedClasses$Host$Inner$InnerInner";
    constexpr const char* k_deep_name         = "vmhook/fixtures/NestedClasses$Host$StaticNested$DeepNested";
    constexpr const char* k_iface_name        = "vmhook/fixtures/NestedClasses$NestedIface";
    constexpr const char* k_enum_name         = "vmhook/fixtures/NestedClasses$NestedEnum";
    constexpr const char* k_anno_name         = "vmhook/fixtures/NestedClasses$NestedAnno";
    constexpr const char* k_generic_name      = "vmhook/fixtures/NestedClasses$GenericBox";

    // Descriptors the synthetic outer-reference slots must carry.
    constexpr const char* k_host_descriptor  = "Lvmhook/fixtures/NestedClasses$Host;";
    constexpr const char* k_inner_descriptor = "Lvmhook/fixtures/NestedClasses$Host$Inner;";

    // The resolved klass's own name symbol == the requested `$` name?  Gated so
    // a null/garbage klass or symbol degrades to false rather than AV-ing.
    auto klass_name_is(vmhook::hotspot::klass* const k, const char* const expected) -> bool
    {
        if (!k || !vmhook::hotspot::is_valid_pointer(k))
        {
            return false;
        }
        const vmhook::hotspot::symbol* const name_sym{ k->get_name() };
        if (!name_sym || !vmhook::hotspot::is_valid_pointer(name_sym))
        {
            return false;
        }
        return name_sym->to_string() == std::string{ expected };
    }

    // The resolved klass's internal '/'-name (or "" on any failure).  Used for
    // the UNSTABLE anon/local shapes whose exact name we cannot hardcode but whose
    // SHAPE we can assert ("starts with the outer name + '$'").
    auto klass_name_str(vmhook::hotspot::klass* const k) -> std::string
    {
        if (!k || !vmhook::hotspot::is_valid_pointer(k))
        {
            return {};
        }
        const vmhook::hotspot::symbol* const name_sym{ k->get_name() };
        if (!name_sym || !vmhook::hotspot::is_valid_pointer(name_sym))
        {
            return {};
        }
        return name_sym->to_string();
    }

    auto starts_with(const std::string& s, const std::string& prefix) -> bool
    {
        return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
    }

    // ── CRASH-PROOFING: validate an OOP before any RAW dereference ───────────
    //
    // WHY is_valid_pointer() alone is NOT enough here (the cold-JVM crash):
    //   This module acquires the fixture's force-instantiated singletons by
    //   reading their compressed OOPs out of static fields, then later reads
    //   CONTENT off those oops:
    //     * klass_from_oop(oop)           RAW-derefs the narrow-klass at oop+8,
    //                                     gated ONLY by is_valid_pointer;
    //     * get_field("…")->get()         RAW std::memcpy's the field bytes at
    //                                     instance+offset — no validity filter
    //                                     beyond a null field_pointer check;
    //     * get_field("this$N")->get()    reads the synthetic reference slot the
    //                                     same RAW way before decoding it.
    //   The singletons are freshly allocated in <clinit> and, on a COLD JVM, still
    //   live in the young generation.  The harness's own work (and the mode probe)
    //   allocates on the Java thread and can trigger a young/minor GC that
    //   RELOCATES those objects mid-module.  A relocated object's OLD address is
    //   still canonical + aligned + in range, so it PASSES is_valid_pointer() while
    //   pointing into a now-unmapped/evacuated page — the raw read then segfaults.
    //
    // os::safe_read() (ReadProcessMemory on Windows / a fault-safe path on Linux)
    // is the ONLY check that actually proves the page is currently mapped: it reads
    // through a kernel path that returns false instead of faulting.  We probe the
    // OOP header region every reader touches (mark word @0 .. narrow-klass @8 — the
    // first 16 bytes) BEFORE handing the oop to any raw-deref helper.
    //
    // GC timing makes this BEST-EFFORT, not a hard guarantee (a collector can
    // relocate between the probe and the read), so a failed probe is treated as a
    // transient miss: record [INFO] and skip the STRONG assertion, never fail —
    // mirroring tests/jvm/modules/field_introspection.cpp.
    constexpr std::size_t k_oop_header_probe_bytes{ 16 };

    auto oop_header_safely_readable(void* const oop) -> bool
    {
        if (!oop || !vmhook::hotspot::is_valid_pointer(oop))
        {
            return false;
        }
        std::uint8_t scratch[k_oop_header_probe_bytes] = { 0 };
        return vmhook::os::safe_read(scratch, oop, sizeof(scratch));
    }

    // Drive one probe cycle for `mode`: clears the latched `done` and programs the
    // selector on the rising edge of go, then waits for done.
    auto drive(vmhook_test::context& ctx, std::int32_t mode) -> bool
    {
        return ctx.run_probe(
            [mode](bool value)
            {
                if (value)
                {
                    nc::set_done(false);
                    nc::set_mode(mode);
                }
                nc::set_go(value);
            },
            []() { return nc::get_done(); });
    }
}

VMHOOK_JVM_MODULE(nested_classes)
{
    // Entry guard: nothing to test if the fixture isn't loaded.
    if (vmhook::find_class(k_fixture_name) == nullptr)
    {
        ctx.record("[INFO] nested_classes: fixture vmhook/fixtures/NestedClasses not loaded; "
                   "skipping (build issue or fixture not on the classpath).");
        return;
    }

    vmhook::register_class<nc>(k_fixture_name);
    vmhook::register_class<host_w>(k_host_name);
    vmhook::register_class<static_nested_w>(k_static_name);
    vmhook::register_class<inner_w>(k_inner_name);
    vmhook::register_class<second_inner_w>(k_second_inner_name);
    vmhook::register_class<inner_inner_w>(k_inner_inner_name);
    vmhook::register_class<deep_nested_w>(k_deep_name);
    vmhook::register_class<generic_box_w>(k_generic_name);

    // =====================================================================
    //  0. The fixture resolves and its publication fields are reachable.
    // =====================================================================
    {
        vmhook::hotspot::klass* const fixture_klass{ vmhook::find_class(k_fixture_name) };
        ctx.check("fixture_klass_resolves", fixture_klass != nullptr);
        ctx.check("fixture_static_field_resolves", nc::static_field("outerPlusInnerValue").has_value());
    }

    // =====================================================================
    //  1. find_class resolves EACH STABLE nested klass by its internal `$`
    //     name, and the resolved klass echoes that exact name (right klass,
    //     not stale).  Covers all 3 `$`-levels and 4 `$`-levels.
    // =====================================================================
    vmhook::hotspot::klass* const host_klass{ vmhook::find_class(k_host_name) };
    vmhook::hotspot::klass* const static_klass{ vmhook::find_class(k_static_name) };
    vmhook::hotspot::klass* const inner_klass{ vmhook::find_class(k_inner_name) };
    vmhook::hotspot::klass* const second_inner_klass{ vmhook::find_class(k_second_inner_name) };
    vmhook::hotspot::klass* const inner_inner_klass{ vmhook::find_class(k_inner_inner_name) };
    vmhook::hotspot::klass* const deep_klass{ vmhook::find_class(k_deep_name) };
    vmhook::hotspot::klass* const iface_klass{ vmhook::find_class(k_iface_name) };
    vmhook::hotspot::klass* const enum_klass{ vmhook::find_class(k_enum_name) };
    vmhook::hotspot::klass* const anno_klass{ vmhook::find_class(k_anno_name) };
    vmhook::hotspot::klass* const generic_klass{ vmhook::find_class(k_generic_name) };

    ctx.check("find_class_host_resolves", host_klass != nullptr);
    ctx.check("find_class_static_nested_resolves", static_klass != nullptr);
    ctx.check("find_class_inner_resolves", inner_klass != nullptr);
    ctx.check("find_class_second_inner_resolves", second_inner_klass != nullptr);
    ctx.check("find_class_inner_inner_resolves", inner_inner_klass != nullptr);
    ctx.check("find_class_deep_nested_resolves", deep_klass != nullptr);
    ctx.check("find_class_nested_iface_resolves", iface_klass != nullptr);
    ctx.check("find_class_nested_enum_resolves", enum_klass != nullptr);
    ctx.check("find_class_nested_anno_resolves", anno_klass != nullptr);
    ctx.check("find_class_generic_resolves", generic_klass != nullptr);

    ctx.check("host_klass_name_echoes_dollar_name", klass_name_is(host_klass, k_host_name));
    ctx.check("static_nested_klass_name_echoes_dollar_name", klass_name_is(static_klass, k_static_name));
    ctx.check("inner_klass_name_echoes_dollar_name", klass_name_is(inner_klass, k_inner_name));
    ctx.check("second_inner_klass_name_echoes_dollar_name", klass_name_is(second_inner_klass, k_second_inner_name));
    ctx.check("inner_inner_klass_name_echoes_dollar_name", klass_name_is(inner_inner_klass, k_inner_inner_name));
    ctx.check("deep_nested_klass_name_echoes_dollar_name", klass_name_is(deep_klass, k_deep_name));
    ctx.check("nested_iface_klass_name_echoes_dollar_name", klass_name_is(iface_klass, k_iface_name));
    ctx.check("nested_enum_klass_name_echoes_dollar_name", klass_name_is(enum_klass, k_enum_name));
    ctx.check("nested_anno_klass_name_echoes_dollar_name", klass_name_is(anno_klass, k_anno_name));
    ctx.check("generic_klass_name_echoes_dollar_name", klass_name_is(generic_klass, k_generic_name));

    // The two inner classes of the SAME outer Host are DISTINCT klasses (one of
    // the headline multiplicity facts).
    ctx.check("two_inners_of_same_host_are_distinct_klasses",
              inner_klass != nullptr && second_inner_klass != nullptr
              && inner_klass != second_inner_klass);

    // All the STABLE nested klasses are mutually distinct objects.
    ctx.check("all_stable_nested_klasses_distinct",
              host_klass && static_klass && inner_klass && second_inner_klass
              && inner_inner_klass && deep_klass && iface_klass && enum_klass
              && anno_klass && generic_klass
              && host_klass != static_klass && host_klass != inner_klass
              && static_klass != inner_klass && inner_klass != inner_inner_klass
              && static_klass != deep_klass && iface_klass != enum_klass
              && enum_klass != anno_klass && anno_klass != generic_klass);

    // =====================================================================
    //  2. Acquire the force-instantiated STABLE singletons and read each
    //     instance field through the matching wrapper (the mirrored values).
    // =====================================================================
    const auto host{ nc::acquire<host_w>("host") };
    const auto static_nested{ nc::acquire<static_nested_w>("staticNested") };
    const auto inner{ nc::acquire<inner_w>("innerInst") };
    const auto second_inner{ nc::acquire<second_inner_w>("secondInnerInst") };
    const auto inner_inner{ nc::acquire<inner_inner_w>("innerInnerInst") };
    const auto deep_nested{ nc::acquire<deep_nested_w>("deepNestedInst") };
    const auto generic_box{ nc::acquire<generic_box_w>("genericBoxInst") };

    ctx.check("host_instance_acquired", host != nullptr);
    ctx.check("static_nested_instance_acquired", static_nested != nullptr);
    ctx.check("inner_instance_acquired", inner != nullptr);
    ctx.check("second_inner_instance_acquired", second_inner != nullptr);
    ctx.check("inner_inner_instance_acquired", inner_inner != nullptr);
    ctx.check("deep_nested_instance_acquired", deep_nested != nullptr);
    ctx.check("generic_box_instance_acquired", generic_box != nullptr);

    // Each get_*() RAW-memcpy's the field bytes at instance+offset, so probe the
    // instance oop header first.  On a probe miss, record [INFO] and skip — the
    // assertion stays HARD on success.  A small helper keeps each read uniform.
    auto read_int_field = [&](const char* tag, void* oop, auto reader, std::int32_t expected)
    {
        if (oop_header_safely_readable(oop))
        {
            ctx.check(tag, reader() == expected);
        }
        else
        {
            ctx.record(std::string{ "[INFO] nested_classes: " } + tag
                       + " instance header not safely readable (stale/relocated) -- skipped read.");
        }
    };

    if (host)
    {
        read_int_field("host_outerField_is_7", host->get_instance(),
                       [&] { return host->get_outer_field(); }, 7);
    }
    if (static_nested)
    {
        read_int_field("static_nested_value_is_42", static_nested->get_instance(),
                       [&] { return static_nested->get_value(); }, 42);
    }
    if (inner)
    {
        read_int_field("inner_innerValue_is_99", inner->get_instance(),
                       [&] { return inner->get_inner_value(); }, 99);
    }
    if (second_inner)
    {
        read_int_field("second_inner_secondValue_is_55", second_inner->get_instance(),
                       [&] { return second_inner->get_second_value(); }, 55);
    }
    if (inner_inner)
    {
        read_int_field("inner_inner_value_is_11", inner_inner->get_instance(),
                       [&] { return inner_inner->get_inner_inner_value(); }, 11);
    }
    if (deep_nested)
    {
        read_int_field("deep_nested_value_is_1000", deep_nested->get_instance(),
                       [&] { return deep_nested->get_deep_value(); }, 1000);
    }

    // =====================================================================
    //  3. The decoded instance OOPs carry the klass find_class resolved.
    //     Ties "resolve klass by `$` name" to the actual objects the field
    //     reads ran against (klass_from_oop(instance) == find_class(name)).
    // =====================================================================
    auto klass_tie_back = [&](const char* tag, void* oop, vmhook::hotspot::klass* expected)
    {
        if (oop_header_safely_readable(oop))
        {
            ctx.check(tag, vmhook::klass_from_oop(oop) == expected);
        }
        else
        {
            ctx.record(std::string{ "[INFO] nested_classes: " } + tag
                       + " instance header not safely readable (stale/relocated) -- skipped klass tie-back.");
        }
    };

    if (host)         { klass_tie_back("host_oop_klass_matches_find_class", host->get_instance(), host_klass); }
    if (static_nested){ klass_tie_back("static_nested_oop_klass_matches_find_class", static_nested->get_instance(), static_klass); }
    if (inner)        { klass_tie_back("inner_oop_klass_matches_find_class", inner->get_instance(), inner_klass); }
    if (second_inner) { klass_tie_back("second_inner_oop_klass_matches_find_class", second_inner->get_instance(), second_inner_klass); }
    if (inner_inner)  { klass_tie_back("inner_inner_oop_klass_matches_find_class", inner_inner->get_instance(), inner_inner_klass); }
    if (deep_nested)  { klass_tie_back("deep_nested_oop_klass_matches_find_class", deep_nested->get_instance(), deep_klass); }

    // =====================================================================
    //  4. The SYNTHETIC `this$0` back-reference of the Inner instance decodes
    //     to a usable Host wrapper whose OOP is IDENTICAL to the Host instance.
    //     This is the headline inner-class contract: javac's hidden outer link
    //     points exactly where it wired it.
    // =====================================================================
    if (inner)
    {
        // Field-metadata existence + descriptor (walks the klass field list); no
        // oop deref, so HARD.
        ctx.check("inner_synthetic_this0_field_resolves", inner->this0_resolves());
        ctx.check("inner_this0_descriptor_names_host",
                  inner->this0_signature() == std::string{ k_host_descriptor });

        if (oop_header_safely_readable(inner->get_instance()))
        {
            const auto this0_host{ inner->get_this0_host() };
            ctx.check("inner_this0_decodes_to_nonnull_wrapper", this0_host != nullptr);

            if (this0_host)
            {
                vmhook::oop_t const this0_oop{ this0_host->get_instance() };
                ctx.check("inner_this0_oop_is_valid",
                          this0_oop != nullptr && vmhook::hotspot::is_valid_pointer(this0_oop));

                // IDENTITY: the this$0 OOP must be the very Host instance we
                // acquired from the static `host` field.  Pure POINTER COMPARISON
                // (no dereference) -> cannot fault -> stays HARD.  This is the
                // load-bearing assertion distinguishing "correct" from "decoded
                // some other live object"; never weaken it to a non-null check.
                if (host && this0_oop && vmhook::hotspot::is_valid_pointer(this0_oop))
                {
                    ctx.check("inner_this0_identity_is_host_instance",
                              this0_oop == host->get_instance());

                    if (oop_header_safely_readable(this0_oop))
                    {
                        ctx.check("inner_this0_oop_klass_is_host_klass",
                                  vmhook::klass_from_oop(this0_oop) == host_klass);
                    }
                    else
                    {
                        ctx.record("[INFO] nested_classes: this$0 oop header not safely readable "
                                   "(stale/relocated) -- skipped klass tie-back (identity proof above HARD).");
                    }

                    // Reading Host.outerField THROUGH the this$0-decoded wrapper
                    // RAW-memcpy's host_oop+offset -> probe that wrapper's header.
                    if (oop_header_safely_readable(this0_host->get_instance()))
                    {
                        ctx.check("inner_this0_outerField_readback_7",
                                  this0_host->get_outer_field() == 7);
                    }
                    else
                    {
                        ctx.record("[INFO] nested_classes: this$0-decoded Host header not safely "
                                   "readable (stale/relocated) -- skipped outerField readback "
                                   "(identity proof above HARD).");
                    }
                }
            }
        }
        else
        {
            ctx.record("[INFO] nested_classes: inner instance header not safely readable "
                       "(stale/relocated) -- skipped this$0 decode + back-reference checks.");
        }
    }

    // =====================================================================
    //  4b. The SECOND inner's OWN this$0 ALSO points at the same Host — proves
    //      each inner of one outer has an INDEPENDENT, correctly-wired this$0.
    // =====================================================================
    if (second_inner)
    {
        ctx.check("second_inner_synthetic_this0_field_resolves", second_inner->this0_resolves());

        if (oop_header_safely_readable(second_inner->get_instance()))
        {
            const auto si_this0{ second_inner->get_this0_host() };
            ctx.check("second_inner_this0_decodes_to_nonnull_wrapper", si_this0 != nullptr);
            if (si_this0 && host)
            {
                vmhook::oop_t const si_this0_oop{ si_this0->get_instance() };
                ctx.check("second_inner_this0_oop_is_valid",
                          si_this0_oop != nullptr && vmhook::hotspot::is_valid_pointer(si_this0_oop));
                // Identity: SecondInner's this$0 IS the same Host instance.
                ctx.check("second_inner_this0_identity_is_host_instance",
                          si_this0_oop == host->get_instance());
                // And it is the SAME Host the first Inner's this$0 pointed at.
                ctx.check("both_inners_share_the_same_host_instance",
                          si_this0_oop == host->get_instance());
            }
        }
        else
        {
            ctx.record("[INFO] nested_classes: secondInner instance header not safely readable "
                       "-- skipped its this$0 back-reference checks.");
        }
    }

    // =====================================================================
    //  4c. INNER-inside-INNER: the synthetic field is named `this$1` (NOT
    //      this$0), names the enclosing Inner, decodes to it by IDENTITY, and a
    //      depth-1 inner has NO this$0 of its own.  Depth shows up in the NAME.
    // =====================================================================
    if (inner_inner)
    {
        // The depth-1 synthetic field is this$1, and there is NO this$0.
        ctx.check("inner_inner_has_this1_field", inner_inner->this1_resolves());
        ctx.check("inner_inner_has_no_this0_field", inner_inner->this0_absent());
        ctx.check("inner_inner_this1_descriptor_names_inner",
                  inner_inner->this1_signature() == std::string{ k_inner_descriptor });

        if (oop_header_safely_readable(inner_inner->get_instance()))
        {
            const auto this1_inner{ inner_inner->get_this1_inner() };
            ctx.check("inner_inner_this1_decodes_to_nonnull_wrapper", this1_inner != nullptr);
            if (this1_inner && inner)
            {
                vmhook::oop_t const this1_oop{ this1_inner->get_instance() };
                ctx.check("inner_inner_this1_oop_is_valid",
                          this1_oop != nullptr && vmhook::hotspot::is_valid_pointer(this1_oop));
                // IDENTITY: this$1 IS the Inner instance we acquired (innerInst).
                ctx.check("inner_inner_this1_identity_is_inner_instance",
                          this1_oop == inner->get_instance());
                if (oop_header_safely_readable(this1_oop))
                {
                    ctx.check("inner_inner_this1_oop_klass_is_inner_klass",
                              vmhook::klass_from_oop(this1_oop) == inner_klass);
                    // Read innerValue THROUGH the this$1-decoded Inner wrapper.
                    ctx.check("inner_inner_this1_innerValue_readback_99",
                              this1_inner->get_inner_value() == 99);
                }
            }
        }
        else
        {
            ctx.record("[INFO] nested_classes: innerInner instance header not safely readable "
                       "-- skipped this$1 back-reference checks.");
        }
    }

    // =====================================================================
    //  4d. The deeply-nested (all-static) class has NO synthetic outer ref at
    //      ANY level — the static-nesting negative that distinguishes it from
    //      the inner shapes.
    // =====================================================================
    if (deep_nested)
    {
        ctx.check("deep_nested_has_no_this0", deep_nested->no_this0());
        ctx.check("deep_nested_has_no_this1", deep_nested->no_this1());
    }
    // The top-level static nested likewise carries no synthetic outer reference.
    if (static_nested)
    {
        ctx.check("static_nested_has_no_this0",
                  !static_nested->get_field("this$0").has_value());
    }

    // =====================================================================
    //  5. GENERIC nested (erased): the declared field `boxed` resolves with the
    //     ERASED descriptor Ljava/lang/Object; (the type parameter is gone), and
    //     its reference oop (an Integer) is valid; the unboxed int is proven via
    //     the mode-2 probe (phase 9).
    // =====================================================================
    if (generic_box)
    {
        ctx.check("generic_box_boxed_field_resolves", generic_box->boxed_resolves());
        ctx.check("generic_box_boxed_descriptor_is_erased_object",
                  generic_box->boxed_signature() == std::string{ "Ljava/lang/Object;" });
        if (oop_header_safely_readable(generic_box->get_instance()))
        {
            void* const boxed{ generic_box->boxed_oop() };
            ctx.check("generic_box_boxed_oop_is_valid",
                      boxed != nullptr && vmhook::hotspot::is_valid_pointer(boxed));
        }
    }

    // =====================================================================
    //  6. NESTED INTERFACE / ENUM / ANNOTATION resolve by `$`-name and expose
    //     their declared members — proving the `$`-name path is shape-agnostic.
    //     (Generic klass-shape bit testing lives in klass_introspection; here we
    //     only prove resolution + a declared member per shape.)
    // =====================================================================
    {
        // Interface: its static-final IFACE_CONST field + abstract/default methods.
        const auto iface_const{ vmhook::find_field(iface_klass, "IFACE_CONST") };
        ctx.check("nested_iface_IFACE_CONST_field_resolves", iface_const.has_value());
        const auto iface_methods{ vmhook::get_class_methods(k_iface_name) };
        bool iface_has_op{ false };
        bool iface_has_default{ false };
        for (const auto& m : iface_methods)
        {
            if (m.first == "ifaceOp")      { iface_has_op = true; }
            if (m.first == "ifaceDefault") { iface_has_default = true; }
        }
        ctx.check("nested_iface_has_abstract_ifaceOp", iface_has_op);
        ctx.check("nested_iface_has_default_ifaceDefault", iface_has_default);

        // Enum: synthetic values()/valueOf + the declared rank() + a constant
        // static field (GAMMA) resolve.
        const auto enum_methods{ vmhook::get_class_methods(k_enum_name) };
        bool enum_has_values{ false };
        bool enum_has_rank{ false };
        for (const auto& m : enum_methods)
        {
            if (m.first == "values") { enum_has_values = true; }
            if (m.first == "rank")   { enum_has_rank = true; }
        }
        ctx.check("nested_enum_has_synthetic_values", enum_has_values);
        ctx.check("nested_enum_has_declared_rank", enum_has_rank);
        const auto enum_gamma{ vmhook::find_field(enum_klass, "GAMMA") };
        ctx.check("nested_enum_GAMMA_constant_field_resolves", enum_gamma.has_value());

        // Annotation: its element-accessor methods (label/weight) live in _methods.
        const auto anno_methods{ vmhook::get_class_methods(k_anno_name) };
        bool anno_has_label{ false };
        bool anno_has_weight{ false };
        for (const auto& m : anno_methods)
        {
            if (m.first == "label")  { anno_has_label = true; }
            if (m.first == "weight") { anno_has_weight = true; }
        }
        ctx.check("nested_anno_has_label_element", anno_has_label);
        ctx.check("nested_anno_has_weight_element", anno_has_weight);
    }

    // =====================================================================
    //  7. UNSTABLE shapes — ANONYMOUS + LOCAL.  Their `$N` ordinal is assigned
    //     by source order and can shift on a recompile, so they are resolved by
    //     reading the klass off a PUBLISHED INSTANCE oop, NEVER by name.  We
    //     assert: the klass resolves from the oop, its name has the
    //     Outer$<something> SHAPE, and (for the anon/local that carry one) the
    //     synthetic this$0 points back at the fixture SELF instance.
    //
    //     [INFO] JDK ENUMERATION QUIRK: there is no by-name find_class target for
    //     these — find_class("vmhook/fixtures/NestedClasses$1") MIGHT resolve on a
    //     given build, but the ordinal is not contractual (this fixture has TWO
    //     anonymous classes: the Runnable AND the Harness.Probe, so $1/$2 depend
    //     on source order), so the by-name route is recorded, not asserted.
    // =====================================================================
    {
        void* const anon_oop{ nc::field_oop("anonymousInst") };
        void* const local_oop{ nc::field_oop("localInst") };
        void* const self_oop{ nc::field_oop("SELF") };

        ctx.check("anonymous_instance_oop_nonnull",
                  anon_oop != nullptr && vmhook::hotspot::is_valid_pointer(anon_oop));
        ctx.check("local_instance_oop_nonnull",
                  local_oop != nullptr && vmhook::hotspot::is_valid_pointer(local_oop));
        ctx.check("self_instance_oop_nonnull",
                  self_oop != nullptr && vmhook::hotspot::is_valid_pointer(self_oop));

        const std::string fixture_prefix{ std::string{ k_fixture_name } + "$" };

        // -- ANONYMOUS: klass from the oop, name shape Outer$<ordinal> ----------
        if (anon_oop && oop_header_safely_readable(anon_oop))
        {
            vmhook::hotspot::klass* const anon_klass{ vmhook::klass_from_oop(anon_oop) };
            ctx.check("anonymous_klass_from_oop_nonnull", anon_klass != nullptr);
            const std::string anon_name{ klass_name_str(anon_klass) };
            ctx.check("anonymous_klass_name_has_outer_dollar_shape",
                      starts_with(anon_name, fixture_prefix));
            // The anonymous-class name's segment after the last '$' is purely
            // numeric (NestedClasses$1) — the unstable-ordinal signature.
            const std::string::size_type last_dollar{ anon_name.find_last_of('$') };
            bool anon_tail_numeric{ last_dollar != std::string::npos
                                    && last_dollar + 1 < anon_name.size() };
            for (std::string::size_type i{ last_dollar + 1 };
                 anon_tail_numeric && i < anon_name.size(); ++i)
            {
                if (anon_name[i] < '0' || anon_name[i] > '9') { anon_tail_numeric = false; }
            }
            ctx.check("anonymous_klass_name_tail_is_numeric_ordinal", anon_tail_numeric);
            ctx.record(std::string{ "[INFO] nested_classes: anonymous class resolved by INSTANCE "
                                    "(name='" } + anon_name + "'); its $N ordinal is source-order "
                       "assigned and NOT a contractual find_class target.");

            // Its synthetic this$0 -> the fixture SELF instance.  Use the free
            // find_field on the resolved klass (no typed wrapper for an anon).
            const auto anon_this0{ vmhook::find_field(anon_klass, "this$0") };
            ctx.check("anonymous_has_synthetic_this0", anon_this0.has_value());
            if (anon_this0.has_value() && self_oop)
            {
                const std::uint32_t narrow{ vmhook::get_field<std::uint32_t>(
                    anon_oop, anon_klass, "this$0") };
                void* const decoded{ vmhook::hotspot::decode_oop_pointer(narrow) };
                ctx.check("anonymous_this0_identity_is_fixture_self",
                          decoded == self_oop);
            }
        }
        else if (anon_oop)
        {
            ctx.record("[INFO] nested_classes: anonymous instance header not safely readable "
                       "-- skipped klass/this$0 resolution.");
        }

        // -- LOCAL: klass from the oop, name shape Outer$<ordinal><Name> --------
        if (local_oop && oop_header_safely_readable(local_oop))
        {
            vmhook::hotspot::klass* const local_klass{ vmhook::klass_from_oop(local_oop) };
            ctx.check("local_klass_from_oop_nonnull", local_klass != nullptr);
            const std::string local_name{ klass_name_str(local_klass) };
            ctx.check("local_klass_name_has_outer_dollar_shape",
                      starts_with(local_name, fixture_prefix));
            // The local-class name carries the source name as a suffix after the
            // ordinal (NestedClasses$1LocalCounter ends with "LocalCounter").
            const std::string local_suffix{ "LocalCounter" };
            ctx.check("local_klass_name_ends_with_source_name",
                      local_name.size() >= local_suffix.size()
                      && local_name.compare(local_name.size() - local_suffix.size(),
                                            local_suffix.size(), local_suffix) == 0);
            ctx.record(std::string{ "[INFO] nested_classes: local class resolved by INSTANCE "
                                    "(name='" } + local_name + "'); the $N ordinal prefix is "
                       "source-order assigned and NOT a contractual find_class target.");

            // Its synthetic this$0 -> the fixture SELF instance.
            const auto local_this0{ vmhook::find_field(local_klass, "this$0") };
            ctx.check("local_has_synthetic_this0", local_this0.has_value());
            if (local_this0.has_value() && self_oop)
            {
                const std::uint32_t narrow{ vmhook::get_field<std::uint32_t>(
                    local_oop, local_klass, "this$0") };
                void* const decoded{ vmhook::hotspot::decode_oop_pointer(narrow) };
                ctx.check("local_this0_identity_is_fixture_self", decoded == self_oop);
            }
        }
        else if (local_oop)
        {
            ctx.record("[INFO] nested_classes: local instance header not safely readable "
                       "-- skipped klass/this$0 resolution.");
        }

        // The anonymous and local klasses are DISTINCT from each other and from
        // every STABLE nested klass (a nesting-multiplicity fact).
        if (anon_oop && local_oop
            && oop_header_safely_readable(anon_oop) && oop_header_safely_readable(local_oop))
        {
            vmhook::hotspot::klass* const ak{ vmhook::klass_from_oop(anon_oop) };
            vmhook::hotspot::klass* const lk{ vmhook::klass_from_oop(local_oop) };
            ctx.check("anonymous_and_local_klasses_distinct",
                      ak != nullptr && lk != nullptr && ak != lk
                      && ak != inner_klass && lk != inner_klass
                      && ak != host_klass && lk != host_klass);
        }
    }

    // =====================================================================
    //  8. Native interpreter-call ATTEMPTS (degrade gracefully to [INFO]).
    //     A no-arg int instance method on a nested class may return monostate
    //     via the call_jni fallback on some JDK builds; never FAIL on that.
    //     The authoritative 84 / 106 / 62 / 117 / 2000 come from the mode probes
    //     (phase 9), so skipping here loses no coverage.  Two cold-JVM hazards
    //     are gated out before invoking (mirroring enum_singleton / poly_inherited
    //     _oop): the call gate may be absent, and a GC-relocated receiver must not
    //     be passed into the call machinery.
    // =====================================================================
    const bool call_gate_present{ vmhook::detail::find_call_stub_entry() != nullptr };
    ctx.record(std::string{ "[INFO] nested_classes call gate: " }
               + (call_gate_present
                      ? "StubRoutines::_call_stub_entry present (call_stub fast path)"
                      : "call_stub_entry absent (JNI fallback) -- native call attempts skipped; "
                        "mode probes are authoritative"));

    if (static_nested && call_gate_present
        && oop_header_safely_readable(static_nested->get_instance()))
    {
        const vmhook::method_proxy::value_t dv{ static_nested->call_doubled() };
        if (!dv.is_void())
        {
            const std::int32_t doubled = dv;
            ctx.check("native_static_nested_doubled_is_84", doubled == 84);
        }
        else
        {
            ctx.record("[INFO] nested_classes: native StaticNested.doubled() returned monostate "
                       "(no-arg int interpreter call gate unavailable on this JDK build) -- "
                       "covered authoritatively by the mode-1 probe below.");
        }
    }
    else if (static_nested)
    {
        ctx.record("[INFO] nested_classes: native StaticNested.doubled() attempt skipped "
                   "(call gate absent or receiver header not safely readable) -- "
                   "covered authoritatively by the mode-1 probe below.");
    }
    if (inner && call_gate_present
        && oop_header_safely_readable(inner->get_instance()))
    {
        const vmhook::method_proxy::value_t ov{ inner->call_outer_plus_inner() };
        if (!ov.is_void())
        {
            const std::int32_t opi = ov;
            ctx.check("native_inner_outerPlusInner_is_106", opi == 106);
        }
        else
        {
            ctx.record("[INFO] nested_classes: native Inner.outerPlusInner() returned monostate "
                       "(synthetic-this$0 no-arg int call via JNI fallback unavailable on this JDK "
                       "build) -- covered authoritatively by the mode-1 probe below.");
        }
    }
    else if (inner)
    {
        ctx.record("[INFO] nested_classes: native Inner.outerPlusInner() attempt skipped "
                   "(call gate absent or receiver header not safely readable) -- "
                   "covered authoritatively by the mode-1 probe below.");
    }

    // =====================================================================
    //  9. AUTHORITATIVE composite proofs via REAL bytecode (mode 1 + mode 2).
    //     The fixture runs the nested-class methods on the Java thread and
    //     publishes the results; the documented composites hold JDK-independently.
    // =====================================================================
    {
        const bool done1{ drive(ctx, 1) };
        ctx.check("composite_probe_mode1_completed", done1);
        if (done1)
        {
            ctx.check("probe_outerPlusInner_is_106", nc::get_int("outerPlusInnerValue") == 106);
            ctx.check("probe_doubled_is_84", nc::get_int("doubledValue") == 84);
            ctx.check("probe_outerPlusSecond_is_62", nc::get_int("outerPlusSecondValue") == 62);
            // The documented invariant, spelled out: 7 + 99 == 106.
            ctx.check("composite_parts_sum_to_106", (7 + 99) == nc::get_int("outerPlusInnerValue"));
        }

        const bool done2{ drive(ctx, 2) };
        ctx.check("composite_probe_mode2_completed", done2);
        if (done2)
        {
            // InnerInner sum-through-BOTH-outers: 7 (Host) + 99 (Inner) + 11 == 117.
            ctx.check("probe_innerInner_sum_is_117", nc::get_int("innerInnerSumValue") == 117);
            ctx.check("composite_inner_inner_parts_sum_to_117",
                      (7 + 99 + 11) == nc::get_int("innerInnerSumValue"));
            // Deeply-nested (all static) doubled: 1000 * 2 == 2000.
            ctx.check("probe_deepDoubled_is_2000", nc::get_int("deepDoubledValue") == 2000);
            // Nested enum constant rank: GAMMA.ordinal()+1 == 3.
            ctx.check("probe_nestedEnum_rank_is_3", nc::get_int("nestedEnumRankValue") == 3);
            // Generic box unboxed through real bytecode: 321.
            ctx.check("probe_genericBox_unboxed_is_321", nc::get_int("genericBoxUnboxedValue") == 321);
            // Local class read-through (selfMarker 4242 + localValue 7777 == 12019).
            ctx.check("probe_local_readback_is_12019", nc::get_int("localReadbackValue") == (4242 + 7777));
        }
    }

    // =====================================================================
    // 10. Published identity hashes are non-zero (sanity that the singletons
    //     are the live objects whose OOPs the earlier phases decoded).  Pointer
    //     identity in phases 4/4b/4c is the strong proof; this is a cheap
    //     corroborant covering every shape.
    // =====================================================================
    {
        ctx.check("host_identity_published_nonzero", nc::get_int("hostIdentity") != 0);
        ctx.check("inner_identity_published_nonzero", nc::get_int("innerIdentity") != 0);
        ctx.check("static_nested_identity_published_nonzero", nc::get_int("staticNestedIdentity") != 0);
        ctx.check("second_inner_identity_published_nonzero", nc::get_int("secondInnerIdentity") != 0);
        ctx.check("inner_inner_identity_published_nonzero", nc::get_int("innerInnerIdentity") != 0);
        ctx.check("deep_nested_identity_published_nonzero", nc::get_int("deepNestedIdentity") != 0);
        ctx.check("generic_box_identity_published_nonzero", nc::get_int("genericBoxIdentity") != 0);
        ctx.check("self_identity_published_nonzero", nc::get_int("selfIdentity") != 0);
        ctx.check("anonymous_identity_published_nonzero", nc::get_int("anonymousIdentity") != 0);
        ctx.check("local_identity_published_nonzero", nc::get_int("localIdentity") != 0);
    }
}
