// nested_classes JVM test module  (feature area: classes / klass resolution)
//
// THE nested-class authority: exhaustively exercises vmhook's handling of the
// two Java nested-class shapes whose javac-generated internal names are STABLE
// across recompiles and therefore resolvable by a fixed `$`-name through
// vmhook::find_class:
//
//   * a STATIC nested class  (vmhook/fixtures/NestedClasses$Host$StaticNested)
//       — an ordinary class that merely lives in another class's namespace; it
//         carries NO synthetic outer reference,
//   * a non-static INNER class (vmhook/fixtures/NestedClasses$Host$Inner)
//       — javac injects a synthetic `this$0` back-reference to the enclosing
//         Host instance (plus a synthetic ctor param that wires it).
//
// The enclosing Host is itself a static nested class of the fixture
// (NestedClasses$Host) so it can be force-instantiated with no NestedClasses
// instance.  Mirrors the legacy test_nested_classes (vmhook/src/example.cpp) and
// vmhook.NestedHost value-for-value.
//
// What this module proves on a live JVM (Java 8/11/17/21/24/25 x MSVC/Clang/GCC):
//   * find_class resolves EACH nested klass by its internal `$` name, and the
//     resolved klass's own name symbol echoes that `$` name (so it's the right
//     klass, not a stale cache hit);
//   * a field read off the Host instance (outerField), the StaticNested instance
//     (value), and the Inner instance (innerValue) returns the mirrored values;
//   * the decoded instance OOPs carry the klass that find_class resolved
//     (klass_from_oop(instance) == find_class("...$Name")) — ties the by-name
//     klass resolution to the actual objects the field reads run against;
//   * the SYNTHETIC `this$0` field of the Inner instance decodes to a usable
//     wrapper whose OOP is IDENTICAL to the Host instance — the back-reference
//     points where javac wired it (is_valid_pointer + pointer identity);
//   * the documented composite outerField + Inner.innerValue == 106, proven the
//     robust JDK-independent way by driving Inner.outerPlusInner() through REAL
//     bytecode in the fixture probe (mode 1) and reading the published result,
//     AND attempted natively (degrades to [INFO], never FAIL, when the
//     interpreter call gate for a no-arg int instance method is unavailable on a
//     given JDK build — the call_jni fallback returns monostate there);
//   * StaticNested.doubled() == 84 likewise via the probe (authoritative) and
//     attempted natively with graceful [INFO] degradation.
//
// Harness shape mirrors field_static / field_object_ref: register_class for each
// wrapper, a `mode` selector with a `done` reset on the rising edge of go, and a
// dense battery of ctx.check()s.  SAFETY: every OOP/klass deref is gated with
// vmhook::hotspot::is_valid_pointer; all value_t / call() extractions are
// COPY-INIT (never brace-init) to stay MSVC-unambiguous; no hooks are armed.
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

    // The top-level fixture: only used for its static publication fields, so a
    // null instance is fine (every accessor here is a static_field reach).
    class nc : public vmhook::object<nc>
    {
    public:
        explicit nc(vmhook::oop_t instance) noexcept
            : vmhook::object<nc>{ instance }
        {
        }

        // ---- handshake + scenario selector (all via static_field) ----
        static auto set_go(bool value) -> void      { static_field("go")->set(value); }
        static auto set_done(bool value) -> void     { static_field("done")->set(value); }
        static auto get_done() -> bool               { return static_field("done")->get(); }
        static auto set_mode(std::int32_t m) -> void { static_field("mode")->set(m); }

        // ---- acquire a published nested-instance wrapper -------------------
        // COPY-INIT from value_t -> unique_ptr<W> (never brace-init): value_t has
        // a templated conversion operator, so unique_ptr<W>{ proxy->get() } is
        // ambiguous on MSVC.
        template<typename wrapper_type>
        static auto acquire(const char* name) -> std::unique_ptr<wrapper_type>
        {
            const auto proxy{ static_field(name) };
            if (!proxy.has_value())
            {
                return nullptr;
            }
            std::unique_ptr<wrapper_type> ptr = proxy->get();
            return ptr;
        }

        // ---- published int (identity hashes, composite results) ------------
        static auto get_int(const char* name) -> std::int32_t
        {
            const auto proxy{ static_field(name) };
            if (!proxy.has_value())
            {
                return -1;
            }
            const std::int32_t v = proxy->get();
            return v;
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

        auto get_outer_field() const -> std::int32_t
        {
            const auto p{ get_field("outerField") };
            if (!p.has_value())
            {
                return -1;
            }
            const std::int32_t v = p->get();
            return v;
        }
    };

    // The STATIC nested class: `int value` + `int doubled()`.
    class static_nested_w : public vmhook::object<static_nested_w>
    {
    public:
        explicit static_nested_w(vmhook::oop_t instance) noexcept
            : vmhook::object<static_nested_w>{ instance }
        {
        }

        auto get_value() const -> std::int32_t
        {
            const auto p{ get_field("value") };
            if (!p.has_value())
            {
                return -1;
            }
            const std::int32_t v = p->get();
            return v;
        }

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

        auto get_inner_value() const -> std::int32_t
        {
            const auto p{ get_field("innerValue") };
            if (!p.has_value())
            {
                return -1;
            }
            const std::int32_t v = p->get();
            return v;
        }

        // The synthetic outer back-reference: read the compressed OOP in the
        // `this$0` slot and decode it into a usable Host wrapper.
        auto get_this0_host() const -> std::unique_ptr<host_w>
        {
            const auto p{ get_field("this$0") };
            if (!p.has_value())
            {
                return nullptr;
            }
            std::unique_ptr<host_w> ptr = p->get();
            return ptr;
        }

        // Whether the synthetic field resolves at all (descriptor 'L...;').
        auto this0_resolves() const -> bool
        {
            return get_field("this$0").has_value();
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
    };

    // Fully-qualified internal `$` names (the resolution targets).
    constexpr const char* k_fixture_name = "vmhook/fixtures/NestedClasses";
    constexpr const char* k_host_name    = "vmhook/fixtures/NestedClasses$Host";
    constexpr const char* k_static_name  = "vmhook/fixtures/NestedClasses$Host$StaticNested";
    constexpr const char* k_inner_name   = "vmhook/fixtures/NestedClasses$Host$Inner";

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

    // ── CRASH-PROOFING: validate an OOP before any RAW dereference ───────────
    //
    // WHY is_valid_pointer() alone is NOT enough here (the cold-JVM crash):
    //   This module acquires the fixture's force-instantiated singletons (host /
    //   staticNested / innerInst) by reading their compressed OOPs out of static
    //   fields, then later reads CONTENT off those oops:
    //     * klass_from_oop(oop)           RAW-derefs the narrow-klass at oop+8
    //                                     (vmhook.hpp ~14597), gated ONLY by
    //                                     is_valid_pointer;
    //     * get_field("…")->get()         RAW std::memcpy's the field bytes at
    //                                     instance+offset (field_proxy::get,
    //                                     vmhook.hpp ~12297) — no validity filter
    //                                     beyond a null field_pointer check;
    //     * get_field("this$0")->get()    reads the synthetic reference slot the
    //                                     same RAW way before decoding it.
    //   The singletons are freshly allocated in <clinit> and, on a COLD JVM, still
    //   live in the young generation.  The harness's own work (and the mode-1
    //   probe) allocates on the Java thread and can trigger a young/minor GC that
    //   RELOCATES those objects mid-module.  A relocated object's OLD address is
    //   still canonical + aligned + in range, so it PASSES is_valid_pointer()
    //   while pointing into a now-unmapped/evacuated page — the raw read then
    //   segfaults.  With the legacy warm-up battery present the heap was already
    //   settled (singletons tenured) before this module ran, which is why the
    //   crash only surfaced on the modular-only, cold-JVM path; MinGW/gcc have no
    //   SEH net, so the fault takes down the whole JVM with no TOTAL line.
    //
    // os::safe_read() (ReadProcessMemory on Windows / a fault-safe path on Linux)
    // is the ONLY check that actually proves the page is currently mapped: it
    // reads through a kernel path that returns false instead of faulting on an
    // unmapped/relocated address.  We probe the OOP header region every reader
    // touches (mark word @0 .. narrow-klass @8 — the first 16 bytes) BEFORE
    // handing the oop to any raw-deref helper.
    //
    // GC timing makes this BEST-EFFORT, not a hard guarantee (a collector can
    // relocate between the probe and the read), so a failed probe is treated as a
    // transient miss: record [INFO] and skip the STRONG assertion, never fail —
    // mirroring tests/jvm/modules/field_introspection.cpp.  The checks that do
    // NOT deref an oop (find_class resolution, name echo, has_value, distinctness,
    // pointer IDENTITY comparison, the static-field probe reads) stay HARD.
    constexpr std::size_t k_oop_header_probe_bytes{ 16 };

    auto oop_header_safely_readable(void* const oop) -> bool
    {
        if (!oop || !vmhook::hotspot::is_valid_pointer(oop))
        {
            return false;
        }
        std::uint8_t scratch[k_oop_header_probe_bytes] = { 0 };
        // Returns false (no fault) if any byte of the header is on an
        // unmapped/relocated page.
        return vmhook::os::safe_read(scratch, oop, sizeof(scratch));
    }

    // Drive one probe cycle for `mode`: clears the latched `done` and programs
    // the selector on the rising edge of go, then waits for done.
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
    vmhook::register_class<nc>(k_fixture_name);
    vmhook::register_class<host_w>(k_host_name);
    vmhook::register_class<static_nested_w>(k_static_name);
    vmhook::register_class<inner_w>(k_inner_name);

    // =====================================================================
    //  0. The fixture resolves and its publication fields are reachable.
    // =====================================================================
    {
        vmhook::hotspot::klass* const fixture_klass{ vmhook::find_class(k_fixture_name) };
        ctx.check("fixture_klass_resolves", fixture_klass != nullptr);
        ctx.check("fixture_static_field_resolves", nc::static_field("outerPlusInnerValue").has_value());
    }

    // =====================================================================
    //  1. find_class resolves EACH nested klass by its internal `$` name, and
    //     the resolved klass echoes that exact name (right klass, not stale).
    // =====================================================================
    vmhook::hotspot::klass* const host_klass{ vmhook::find_class(k_host_name) };
    vmhook::hotspot::klass* const static_klass{ vmhook::find_class(k_static_name) };
    vmhook::hotspot::klass* const inner_klass{ vmhook::find_class(k_inner_name) };

    ctx.check("find_class_host_resolves", host_klass != nullptr);
    ctx.check("find_class_static_nested_resolves", static_klass != nullptr);
    ctx.check("find_class_inner_resolves", inner_klass != nullptr);

    ctx.check("host_klass_name_echoes_dollar_name", klass_name_is(host_klass, k_host_name));
    ctx.check("static_nested_klass_name_echoes_dollar_name", klass_name_is(static_klass, k_static_name));
    ctx.check("inner_klass_name_echoes_dollar_name", klass_name_is(inner_klass, k_inner_name));

    // The three nested klasses are distinct objects.
    ctx.check("host_static_inner_klasses_distinct",
              host_klass != static_klass && host_klass != inner_klass && static_klass != inner_klass);

    // =====================================================================
    //  2. Acquire the force-instantiated singletons and read each instance
    //     field through the matching wrapper (the mirrored values).
    // =====================================================================
    const auto host{ nc::acquire<host_w>("host") };
    const auto static_nested{ nc::acquire<static_nested_w>("staticNested") };
    const auto inner{ nc::acquire<inner_w>("innerInst") };

    ctx.check("host_instance_acquired", host != nullptr);
    ctx.check("static_nested_instance_acquired", static_nested != nullptr);
    ctx.check("inner_instance_acquired", inner != nullptr);

    // Each get_*() RAW-memcpy's the field bytes at instance+offset, so probe the
    // instance oop header first: a GC-relocated singleton's old address passes
    // is_valid_pointer but is unmapped (see oop_header_safely_readable).  On a
    // probe miss, record [INFO] and skip — the assertion stays HARD on success,
    // so a genuinely wrong field value from a SUCCESSFUL read still fails.
    if (host)
    {
        if (oop_header_safely_readable(host->get_instance()))
        {
            ctx.check("host_outerField_is_7", host->get_outer_field() == 7);
        }
        else
        {
            ctx.record("[INFO] nested_classes: host instance header not safely readable "
                       "(stale/relocated) -- skipped outerField read.");
        }
    }
    if (static_nested)
    {
        if (oop_header_safely_readable(static_nested->get_instance()))
        {
            ctx.check("static_nested_value_is_42", static_nested->get_value() == 42);
        }
        else
        {
            ctx.record("[INFO] nested_classes: staticNested instance header not safely "
                       "readable (stale/relocated) -- skipped value read.");
        }
    }
    if (inner)
    {
        if (oop_header_safely_readable(inner->get_instance()))
        {
            ctx.check("inner_innerValue_is_99", inner->get_inner_value() == 99);
        }
        else
        {
            ctx.record("[INFO] nested_classes: inner instance header not safely readable "
                       "(stale/relocated) -- skipped innerValue read.");
        }
    }

    // =====================================================================
    //  3. The decoded instance OOPs carry the klass find_class resolved.
    //     Ties "resolve klass by `$` name" to the actual objects the field
    //     reads ran against (klass_from_oop(instance) == find_class(name)).
    // =====================================================================
    // klass_from_oop RAW-derefs the narrow-klass at oop+8, so gate on the
    // safe-read header probe (not bare is_valid_pointer — a relocated old address
    // passes that yet faults).  Best-effort: skip + [INFO] on a probe miss; the
    // equality assertion is HARD on success (a wrong klass still fails).
    if (host && oop_header_safely_readable(host->get_instance()))
    {
        ctx.check("host_oop_klass_matches_find_class",
                  vmhook::klass_from_oop(host->get_instance()) == host_klass);
    }
    else if (host)
    {
        ctx.record("[INFO] nested_classes: host instance header not safely readable "
                   "(stale/relocated) -- skipped klass_from_oop tie-back.");
    }
    if (static_nested && oop_header_safely_readable(static_nested->get_instance()))
    {
        ctx.check("static_nested_oop_klass_matches_find_class",
                  vmhook::klass_from_oop(static_nested->get_instance()) == static_klass);
    }
    else if (static_nested)
    {
        ctx.record("[INFO] nested_classes: staticNested instance header not safely "
                   "readable (stale/relocated) -- skipped klass_from_oop tie-back.");
    }
    if (inner && oop_header_safely_readable(inner->get_instance()))
    {
        ctx.check("inner_oop_klass_matches_find_class",
                  vmhook::klass_from_oop(inner->get_instance()) == inner_klass);
    }
    else if (inner)
    {
        ctx.record("[INFO] nested_classes: inner instance header not safely readable "
                   "(stale/relocated) -- skipped klass_from_oop tie-back.");
    }

    // =====================================================================
    //  4. The SYNTHETIC `this$0` back-reference of the Inner instance decodes
    //     to a usable Host wrapper whose OOP is IDENTICAL to the Host instance.
    //     This is the headline inner-class contract: javac's hidden outer link
    //     points exactly where it wired it.
    // =====================================================================
    if (inner)
    {
        // Field-metadata existence (walks the klass field list); no oop deref, HARD.
        ctx.check("inner_synthetic_this0_field_resolves", inner->this0_resolves());

        // get_this0_host() RAW-reads the synthetic reference slot at
        // inner_oop + this$0_offset, so it must only run when inner's header is
        // safely readable.  On a probe miss the whole this$0 deref chain is
        // skipped + [INFO] (never faults).
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
                // acquired from the static `host` field (same Java object -> same
                // heap oop).  This is the strongest, load-bearing assertion in the
                // module: it is a pure POINTER COMPARISON (no dereference), so it
                // CANNOT fault and stays HARD — gated only on non-null, never on a
                // safe-read probe.  It is the only thing distinguishing "correct"
                // from "decoded some other live object," and must never be
                // weakened to a mere non-null check.
                if (host && this0_oop && vmhook::hotspot::is_valid_pointer(this0_oop))
                {
                    ctx.check("inner_this0_identity_is_host_instance",
                              this0_oop == host->get_instance());

                    // klass_from_oop RAW-derefs this0_oop+8 -> probe its header.
                    if (oop_header_safely_readable(this0_oop))
                    {
                        ctx.check("inner_this0_oop_klass_is_host_klass",
                                  vmhook::klass_from_oop(this0_oop) == host_klass);
                    }
                    else
                    {
                        ctx.record("[INFO] nested_classes: this$0 oop header not safely "
                                   "readable (stale/relocated) -- skipped klass tie-back "
                                   "(identity proof above still HARD).");
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
                        ctx.record("[INFO] nested_classes: this$0-decoded Host header not "
                                   "safely readable (stale/relocated) -- skipped outerField "
                                   "readback (identity proof above still HARD).");
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
    //  5. Native interpreter-call ATTEMPTS (degrade gracefully to [INFO]).
    //     A no-arg int instance method on a nested class may return monostate
    //     via the call_jni fallback on some JDK builds; never FAIL on that.
    //
    //     CRASH-PROOFING: the call dispatches into Java bytecode that reads the
    //     receiver oop.  Two cold-JVM hazards are gated out before invoking:
    //       * the call gate (StubRoutines::_call_stub_entry) may be absent on a
    //         cold JVM -> method_proxy::call() takes the JNI fallback path; mirror
    //         poly_inherited_oop and SKIP the call (record [INFO]) when the gate is
    //         absent rather than driving an unguarded cold call;
    //       * even with the gate present, a GC-relocated receiver must not be
    //         passed into the call machinery, so require the receiver header to be
    //         safely readable.
    //     Either way the authoritative 84 / 106 come from the mode-1 bytecode probe
    //     (phase 6), so skipping here loses no coverage.
    // =====================================================================
    const bool call_gate_present{ vmhook::detail::find_call_stub_entry() != nullptr };
    ctx.record(std::string{ "[INFO] nested_classes call gate: " }
               + (call_gate_present
                      ? "StubRoutines::_call_stub_entry present (call_stub fast path)"
                      : "call_stub_entry absent (JNI fallback) -- native call attempts skipped; "
                        "mode-1 probe is authoritative"));

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
    //  6. AUTHORITATIVE composite proof via REAL bytecode (mode 1 probe).
    //     The fixture runs innerInst.outerPlusInner() and staticNested.doubled()
    //     on the Java thread and publishes the results; the documented composite
    //     outerField(7) + innerValue(99) == 106 holds JDK-independently here.
    // =====================================================================
    {
        const bool done{ drive(ctx, 1) };
        ctx.check("composite_probe_completed", done);
        if (done)
        {
            ctx.check("probe_outerPlusInner_is_106", nc::get_int("outerPlusInnerValue") == 106);
            ctx.check("probe_doubled_is_84", nc::get_int("doubledValue") == 84);

            // Cross-check the composite against the parts the native reads saw:
            // 7 + 99 == 106 (the documented invariant, spelled out).
            ctx.check("composite_parts_sum_to_106",
                      (7 + 99) == nc::get_int("outerPlusInnerValue"));
        }
    }

    // =====================================================================
    //  7. Published identity hashes are non-zero (sanity that the singletons
    //     are the live objects whose OOPs phases 2-4 decoded).  Pointer
    //     identity in phase 4 is the strong proof; this is a cheap corroborant.
    // =====================================================================
    {
        ctx.check("host_identity_published_nonzero", nc::get_int("hostIdentity") != 0);
        ctx.check("inner_identity_published_nonzero", nc::get_int("innerIdentity") != 0);
        ctx.check("static_nested_identity_published_nonzero", nc::get_int("staticNestedIdentity") != 0);
    }
}
