// invocation_capability JVM test module -- area: methods / pure-VM invocation.
//
// FEATURE: the RESTORED vmhook::method_proxy::call() dispatch gate.
//
// WHY THIS MODULE EXISTS.  vmhook::detail::find_call_stub_entry() used to be a
// single VMStructs lookup of StubRoutines::_call_stub_entry -- an entry HotSpot
// has NEVER published, on any version (measured on live JDK 8 / 21 / 26, and
// absent from jdk8u / jdk21u / jdk master vmStructs.cpp alike).  It therefore
// returned nullptr on EVERY JDK, method_proxy::call() was dead everywhere, and
// NOTHING IN THE SUITE NOTICED for the entire life of the feature.  The entry is
// now derived from StubRoutines::_call_stub_return_address through four
// validated tiers (published / adjacency / data scan / code scan), each proved
// byte-for-byte before it is accepted.  The cheapest test that would have caught
// the original bug is "assert the stub resolves"; that assertion is SECTION A
// below and it is the regression guard this module exists for.
//
// SECTION A -- CAPABILITY (no Java needed, runs on the suite worker thread):
//   * find_call_stub_entry() resolves to a non-null address            (HARD)
//   * that address lies INSIDE the code cache                          (HARD,
//     gated on CodeCache::_heap -> CodeHeap::_memory resolving)
//   * it re-passes call_stub_entry_is_valid() -- enter() prologue at the entry
//     and the `call c_rarg1` two bytes before the published return address
//                                                                      (HARD)
//   * the resolution is CACHED (a second call returns the same pointer) (HARD)
//   * java_call_layout().usable -- without it call() refuses, because a
//     partially-resolved layout would write a synthetic JavaCallWrapper the VM
//     then reads at the wrong offsets                                  (HARD)
//
// SECTION B -- ROUND TRIPS (inside a scoped_hook detour on trigger(), the
// supported production path -- a real JavaThread in _thread_in_Java).  Every
// shape here is one the previous implementation got wrong:
//   * OBJECT argument + OBJECT return -- the returned oop must be the very
//     object that was passed in                                        (HARD)
//   * STRING argument + STRING return                                  (HARD)
//   * 2-slot LONG argument + long return (the value belongs in the HIGH slot;
//     the old packer put it in the low one)                            (HARD)
//   * 2-slot DOUBLE argument + double return                           (HARD)
//   * an INT argument widened into a J parameter                       (HARD)
//   * a VOID return -- is_void() plus the Java-side side effect         (HARD)
//   * a THROWING callee -- value_t::threw(), exception_class, a zeroed payload,
//     and NO pending exception left on the thread afterwards           (HARD)
//   * a NATIVE callee (java.lang.System.currentTimeMillis) with the
//     JNIHandleBlock watermark (_active_handles->_top) preserved across it
//                                                                      (HARD)
//   * a value-returning call AFTER the throwing one still works        (HARD)
//
// SECTION C -- GC ACROSS A SYNTHETIC ENTRY FRAME (second probe cycle):
//   * System.gc() invoked THROUGH call() -- the collector walks the
//     JavaCallWrapper the call built.  The old `link = -1` argument made a GC
//     dereference ((JavaCallWrapper*)-1)->_anchor and take the process down at
//     address 0x1f, so surviving this is the whole point                (HARD)
//   * invocation still works after that collection                     (HARD)
//
// ARCHITECTURE GATE.  The derivation validates x86-64 instruction bytes
// (55 48 8B EC ... FF D2 / FF D6).  call_stub_entry_is_valid() returns false
// unconditionally on any other architecture, so on aarch64 (the macOS CI legs)
// the whole capability is legitimately absent.  The entire module degrades to a
// single [INFO] there rather than reporting failures for an unimplemented port.
//
// WHY THIS MODULE RUNS LAST (vmhook_test::priority::last).  MEASURED on live
// JDK 21 (Temurin 21.0.11, G1, MinGW build, 2026-08-04): SECTION C's
// System.gc()-through-a-synthetic-entry-frame call itself SUCCEEDS -- the
// collection completes, the call returns void, and the immediately following
// invocation still works -- but it leaves the JavaThread in a state where a
// LATER stop-the-world collection crashes HotSpot's own GC worker while it walks
// that thread:
//
//   Current thread: WorkerThread "GC Thread#11"
//   V [jvm.dll+0xcad1]   EXCEPTION_ACCESS_VIOLATION reading 0x100
//   JavaThread ... was being processed
//   j java.lang.Runtime.gc() / java.lang.System.gc() / <the next module's probe>
//
// Isolated by bisection: this module ALONE completes (TOTAL printed);
// gc_relocation_detector ALONE completes (55/55); the two together die inside
// the LATER module's System.gc(); and with SECTION C disabled the same pair
// completes cleanly (100/106).  So the poisoning is specifically the GC-across-a-
// synthetic-entry-frame path, i.e. a live LIBRARY defect in the restored
// invocation path, not a defect in either test.  Until that is fixed, running
// this module last means the damage cannot swallow another module's results or
// turn a whole CI cell into INCOMPLETE (which reports nothing at all) -- the
// suite still reaches its TOTAL line.  When the library is fixed, drop the
// priority back to the default and this comment with it.
//
// SAFETY.  Nothing here raw-dereferences JVM memory: the stub bytes are read by
// the library through os::safe_read, and this module's own reads of
// _active_handles->_top and ThreadShadow::_pending_exception go through
// os::safe_read as well, so a cold / unexpected layout yields false instead of
// an access violation the no-SEH toolchains (MinGW, clang-on-Windows) cannot
// contain.  The hook is a scoped_hook in a block scope; shutdown_hooks() is
// never called from here.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>

namespace
{
    // ---------------------------------------------------------------------
    //  Wrappers
    // ---------------------------------------------------------------------

    // vmhook.fixtures.InvokeCapability -- the fixture whose methods are invoked.
    class invoke_fixture : public vmhook::object<invoke_fixture>
    {
    public:
        explicit invoke_fixture(vmhook::oop_t instance) noexcept
            : vmhook::object<invoke_fixture>{ instance }
        {
        }

        // -- go / done / mode handshake --
        static auto set_go(bool value) -> void       { static_field("go")->set(value); }
        static auto set_done(bool value) -> void      { static_field("done")->set(value); }
        static auto get_done() -> bool                { return static_field("done")->get(); }
        static auto set_mode(std::int32_t m) -> void  { static_field("mode")->set(m); }

        // -- Java-side observables --
        static auto warm_rounds() -> std::int32_t { return static_field("warmRounds")->get(); }
        static auto void_hits() -> std::int32_t   { return static_field("voidHits")->get(); }
        static auto boom_calls() -> std::int32_t  { return static_field("boomCalls")->get(); }
        static auto echo_calls() -> std::int32_t  { return static_field("echoCalls")->get(); }
        static auto pair_calls() -> std::int32_t  { return static_field("pairCalls")->get(); }

        auto seed() -> std::int32_t { return get_field("seed")->get(); }
    };

    // java.lang.System -- the source of a NATIVE callee (currentTimeMillis) and
    // of the collection driven THROUGH a synthetic entry frame (gc).
    class system_class : public vmhook::object<system_class>
    {
    public:
        explicit system_class(vmhook::oop_t instance) noexcept
            : vmhook::object<system_class>{ instance }
        {
        }
    };

    // ---------------------------------------------------------------------
    //  Sentinels.  Every one sets bits across the full width of its type, so a
    //  mis-slotted / truncated / sign-extended argument cannot accidentally
    //  produce the expected answer.
    // ---------------------------------------------------------------------
    constexpr std::int32_t k_peer_seed{ static_cast<std::int32_t>(0x51C0FFEE) };
    const std::string      k_echo_arg{ "vmhook-invoke" };
    const std::string      k_echo_expected{ "vmhook-invoke-echoed" };
    constexpr std::int64_t k_long_arg{ static_cast<std::int64_t>(0x7EDCBA9876543210LL) };
    constexpr std::int64_t k_long_expected{ k_long_arg + 1 };
    constexpr std::int32_t k_widen_arg{ -42 };
    constexpr std::int64_t k_widen_expected{
        static_cast<std::int64_t>(k_widen_arg) ^ static_cast<std::int64_t>(0x0102030405060708LL) };
    constexpr double       k_double_arg{ -2.5e300 };
    constexpr double       k_double_expected{ k_double_arg / 2.0 };
    constexpr std::int64_t k_addlong_after_throw_arg{ 7 };
    // java.lang.System.currentTimeMillis() cannot plausibly be below this on any
    // machine that can run this suite (2020-09-13T12:26:40Z).
    constexpr std::int64_t k_epoch_floor_ms{ 1600000000000LL };

    // ---------------------------------------------------------------------
    //  Observations.  The detour writes; the module body reads and asserts.
    //  -1 means "the detour never got far enough to record this".
    // ---------------------------------------------------------------------
    std::atomic<int>  g_detour_calls{ 0 };
    std::atomic<bool> g_detour_saw_self{ false };

    std::atomic<int>  g_peer_built{ -1 };
    std::atomic<int>  g_peer_seed_ok{ -1 };

    // OBJECT argument + OBJECT return.
    std::atomic<int>  g_pair_proxy_found{ -1 };
    std::atomic<int>  g_pair_threw{ -1 };
    std::atomic<int>  g_pair_identity{ -1 };     // returned oop == the peer we passed
    std::atomic<int>  g_pair_returned_nonnull{ -1 };
    std::atomic<int>  g_pair_payload{ -1 };      // ...and it still carries our seed

    // STRING argument + STRING return.
    std::atomic<int>  g_echo_proxy_found{ -1 };
    std::atomic<int>  g_echo_threw{ -1 };
    std::atomic<int>  g_echo_is_string{ -1 };
    std::string       g_echo_value{};
    std::string       g_echo_exception{};
    // The same shape driven through the const char* packer branch, so a failure
    // can be attributed to ONE of the two String-argument packers rather than to
    // "String arguments" as a whole.
    std::atomic<int>  g_echo_cstr_threw{ -1 };
    std::string       g_echo_cstr_value{};
    // Isolation probe: can a Java String be built natively at all right here?
    std::atomic<int>  g_make_string_ok{ -1 };
    std::atomic<int>  g_make_string_roundtrips{ -1 };

    // 2-slot LONG.
    std::atomic<int>          g_long_proxy_found{ -1 };
    std::atomic<int>          g_long_threw{ -1 };
    std::atomic<std::int64_t> g_long_value{ 0 };

    // INT widened into a J parameter.
    std::atomic<int>          g_widen_proxy_found{ -1 };
    std::atomic<std::int64_t> g_widen_value{ 0 };

    // 2-slot DOUBLE (compared through its exact bit pattern).
    std::atomic<int>          g_double_proxy_found{ -1 };
    std::atomic<std::int64_t> g_double_bits{ 0 };

    // VOID return.
    std::atomic<int>  g_void_proxy_found{ -1 };
    std::atomic<int>  g_void_is_void{ -1 };
    std::atomic<int>  g_void_threw{ -1 };

    // THROWING callee.
    std::atomic<int>          g_throw_proxy_found{ -1 };
    std::atomic<int>          g_throw_flagged{ -1 };
    std::atomic<std::int64_t> g_throw_payload{ -1 };
    std::string               g_throw_class{};
    std::atomic<int>          g_pending_readable{ -1 };
    std::atomic<int>          g_pending_cleared{ -1 };

    // NATIVE callee + JNIHandleBlock watermark.
    std::atomic<int>          g_native_proxy_found{ -1 };
    std::atomic<int>          g_native_threw{ -1 };
    std::atomic<std::int64_t> g_native_millis{ 0 };
    std::atomic<int>          g_top_readable{ -1 };
    std::atomic<int>          g_top_preserved{ -1 };
    std::atomic<std::int32_t> g_top_before{ -1 };
    std::atomic<std::int32_t> g_top_after{ -1 };

    // Non-corruption after the throwing dispatch.
    std::atomic<std::int64_t> g_after_throw_value{ 0 };

    // SECTION C -- GC through the synthetic entry frame.
    std::atomic<int>          g_gc_proxy_found{ -1 };
    std::atomic<int>          g_gc_threw{ -1 };
    std::atomic<int>          g_gc_is_void{ -1 };
    std::atomic<int>          g_gc_survived{ -1 };
    std::atomic<std::int64_t> g_after_gc_value{ 0 };

    // The phase the next trigger() detour should run (mirrors the Java `mode`).
    std::atomic<int> g_phase{ 0 };

    // ---------------------------------------------------------------------
    //  Fault-safe readers for the two JavaThread fields this module inspects.
    //  NEVER a raw dereference: MinGW / clang-on-Windows have no working SEH,
    //  so an access violation here would kill the JVM and the whole suite.
    // ---------------------------------------------------------------------

    // Reads JavaThread::_active_handles->_top (the JNI handle-block watermark).
    auto read_handle_block_top(std::int32_t& out) noexcept -> bool
    {
        const vmhook::detail::java_call_layout_t& layout{ vmhook::detail::java_call_layout() };
        if (!layout.usable || !layout.has_active_handles || !layout.has_handle_block_top)
        {
            return false;
        }
        if (!vmhook::hotspot::ensure_current_java_thread())
        {
            return false;
        }
        const auto* const thread{
            reinterpret_cast<const std::uint8_t*>(vmhook::hotspot::current_java_thread) };
        if (!thread || !vmhook::hotspot::is_valid_pointer(thread))
        {
            return false;
        }
        void* handles{ nullptr };
        if (!vmhook::os::safe_read(&handles, thread + layout.active_handles_offset, sizeof(handles))
            || !handles || !vmhook::hotspot::is_valid_pointer(handles))
        {
            return false;
        }
        std::int32_t top{ -1 };
        if (!vmhook::os::safe_read(&top,
                                   static_cast<const std::uint8_t*>(handles) + layout.handle_block_top_offset,
                                   sizeof(top)))
        {
            return false;
        }
        out = top;
        return true;
    }

    // Reads ThreadShadow::_pending_exception off the current JavaThread.
    auto read_pending_exception(void*& out) noexcept -> bool
    {
        const vmhook::detail::java_call_layout_t& layout{ vmhook::detail::java_call_layout() };
        if (!layout.usable || !layout.has_pending_exception)
        {
            return false;
        }
        if (!vmhook::hotspot::ensure_current_java_thread())
        {
            return false;
        }
        const auto* const thread{
            reinterpret_cast<const std::uint8_t*>(vmhook::hotspot::current_java_thread) };
        if (!thread || !vmhook::hotspot::is_valid_pointer(thread))
        {
            return false;
        }
        void* pending{ nullptr };
        if (!vmhook::os::safe_read(&pending, thread + layout.pending_exception_offset, sizeof(pending)))
        {
            return false;
        }
        out = pending;
        return true;
    }

    // Drives exactly one probe cycle for `mode` (the house rising-edge idiom).
    auto drive(vmhook_test::context& ctx, const std::int32_t mode) -> bool
    {
        if (!ctx.run_probe)
        {
            return false;
        }
        g_phase.store(mode, std::memory_order_relaxed);
        return ctx.run_probe(
            [mode](bool value)
            {
                if (value)
                {
                    invoke_fixture::set_done(false);
                    invoke_fixture::set_mode(mode);
                }
                invoke_fixture::set_go(value);
            },
            []() { return invoke_fixture::get_done(); });
    }

    auto as_hex(const void* const pointer) -> std::string
    {
        std::ostringstream oss{};
        oss << "0x" << std::hex << reinterpret_cast<std::uintptr_t>(pointer);
        return oss.str();
    }
}

// Runs after EVERY other module -- see the "WHY THIS MODULE RUNS LAST" note in
// the file header.  Not a preference: SECTION C provably poisons the thread for
// the next module's collection on today's header.
//
// The priority is deliberately BEYOND vmhook_test::priority::last (100) rather
// than equal to it: hook_reinstall_after_shutdown already claims `last`, and
// run_all()'s stable_sort keeps equal priorities in registration order -- which
// is static-initializer order, i.e. UNSPECIFIED and reversed by GNU ld.  A value
// of 200 makes "strictly after everything" a property of the sort key instead of
// a link-order coincidence.  run_all() only ever compares static_cast<int>(prio),
// so an out-of-enumerator value is well-defined here (scoped enum with a fixed
// int underlying type).
VMHOOK_JVM_MODULE_PRIORITY(invocation_capability, static_cast<vmhook_test::priority>(200))
{
#if VMHOOK_ARCH_X86_64
    // =====================================================================
    //  SECTION A -- CAPABILITY.  The regression guard the repo never had.
    // =====================================================================
    void* const stub{ vmhook::detail::find_call_stub_entry() };
    ctx.check("invocation_call_stub_entry_resolves", stub != nullptr);

    // The resolution is cached process-wide; a second call must hand back the
    // identical pointer (a resolver that re-derived every time would be both
    // slow and a sign the cache broke).
    ctx.check("invocation_call_stub_entry_is_cached",
              stub != nullptr && vmhook::detail::find_call_stub_entry() == stub);

    // StubRoutines::_call_stub_return_address is the ONE published static the
    // whole derivation hangs off; without it there is nothing to derive from.
    const vmhook::hotspot::vm_struct_entry_t* const return_address_entry{
        vmhook::hotspot::iterate_struct_entries("StubRoutines", "_call_stub_return_address") };
    std::uintptr_t return_address{ 0 };
    if (return_address_entry && return_address_entry->address)
    {
        void* raw{ nullptr };
        if (vmhook::os::safe_read(&raw, return_address_entry->address, sizeof(raw)))
        {
            return_address = reinterpret_cast<std::uintptr_t>(raw);
        }
    }
    ctx.check("invocation_call_stub_return_address_is_published",
              return_address_entry != nullptr && return_address_entry->address != nullptr
                  && return_address != 0);

    // The entry must live INSIDE the code cache -- it is generated code.  Gated
    // on the bounds resolving at all (CodeCache::_heap -> CodeHeap::_memory ->
    // VirtualSpace::_low/_high); a VM that does not export them leaves the
    // library's own containment check disabled too, so asserting here would be
    // asserting something the feature never claimed.
    const vmhook::detail::code_cache_bounds_t bounds{ vmhook::detail::resolve_code_cache_bounds() };
    if (bounds.low != 0 && bounds.high > bounds.low)
    {
        ctx.check("invocation_call_stub_entry_inside_code_cache",
                  stub != nullptr && bounds.contains(reinterpret_cast<std::uintptr_t>(stub)));
    }
    else
    {
        ctx.record("[INFO] invocation_capability: code-cache bounds did not resolve on this JDK "
                   "(CodeCache::_heap / CodeHeap::_memory / VirtualSpace::_low+_high); the "
                   "entry-inside-code-cache containment check is not asserted this run -- the "
                   "library's own validator likewise skips the bounds test when they are absent.");
    }

    // Independently re-run the library's positive validation on the resolved
    // address: MacroAssembler::enter() at the entry, `call c_rarg1` in the two
    // bytes before the published return address, same-stub distance, in-cache.
    // This is what makes "non-null" mean "the real stub" rather than "some
    // pointer".  Both reads inside are os::safe_read.
    if (stub != nullptr && return_address != 0)
    {
        ctx.check("invocation_call_stub_entry_revalidates",
                  vmhook::detail::call_stub_entry_is_valid(
                      reinterpret_cast<std::uintptr_t>(stub), return_address, bounds));
    }
    else
    {
        ctx.check("invocation_call_stub_entry_revalidates", false);
    }

    // Without the JavaCallWrapper / frame-anchor layout, call() refuses -- and
    // it is right to: the previous implementation passed `link = -1` and a GC
    // walking the entry frame dereferenced ((JavaCallWrapper*)-1)->_anchor.
    const vmhook::detail::java_call_layout_t& layout{ vmhook::detail::java_call_layout() };
    ctx.check("invocation_java_call_layout_usable", layout.usable);
    ctx.check("invocation_java_call_wrapper_size_is_64", layout.wrapper_size == 64);
    ctx.check("invocation_java_call_wrapper_anchor_at_32", layout.wrapper_anchor_offset == 32);

    {
        const bool published{
            vmhook::hotspot::iterate_struct_entries("StubRoutines", "_call_stub_entry") != nullptr };
        std::ostringstream oss{};
        oss << "[INFO] invocation_capability: call stub entry=" << as_hex(stub)
            << " return_address=" << as_hex(reinterpret_cast<void*>(return_address))
            << " distance=" << std::dec
            << (stub != nullptr && return_address != 0
                    ? static_cast<long long>(return_address - reinterpret_cast<std::uintptr_t>(stub))
                    : -1LL)
            << " _call_stub_entry_published=" << (published ? "yes (tier 0)" : "no (derived)")
            << " code_cache=[" << as_hex(reinterpret_cast<void*>(bounds.low)) << ","
            << as_hex(reinterpret_cast<void*>(bounds.high)) << ")";
        ctx.record(oss.str());
    }
    {
        std::ostringstream oss{};
        oss << "[INFO] invocation_capability: java_call_layout usable=" << (layout.usable ? 1 : 0)
            << " wrapper_size=" << layout.wrapper_size
            << " wrapper_anchor=" << layout.wrapper_anchor_offset
            << " thread_anchor=" << layout.thread_anchor_offset
            << " anchor_sp/pc/fp=" << layout.anchor_sp_offset << "/" << layout.anchor_pc_offset
            << "/" << (layout.has_anchor_fp ? layout.anchor_fp_offset : 0)
            << " active_handles=" << (layout.has_active_handles ? layout.active_handles_offset : 0)
            << " handle_block_top=" << (layout.has_handle_block_top ? layout.handle_block_top_offset : 0)
            << " pending_exception=" << (layout.has_pending_exception ? layout.pending_exception_offset : 0);
        ctx.record(oss.str());
    }

    // =====================================================================
    //  SECTIONS B + C -- the live round trips, inside a detour.
    // =====================================================================
    if (vmhook::find_class("vmhook/fixtures/InvokeCapability") == nullptr)
    {
        ctx.record("[INFO] invocation_capability: the InvokeCapability fixture is not loaded on "
                   "this run; the live invocation round trips are skipped (no crash, no hooks "
                   "armed).  SECTION A above still ran.");
        return;
    }

    vmhook::register_class<invoke_fixture>("vmhook/fixtures/InvokeCapability");
    vmhook::register_class<system_class>("java/lang/System");

    {
        auto handle{ vmhook::scoped_hook<invoke_fixture>(
            "trigger",
            [](vmhook::return_value&,
               const std::unique_ptr<invoke_fixture>& self)
            {
                g_detour_calls.fetch_add(1, std::memory_order_relaxed);
                g_detour_saw_self.store(self != nullptr, std::memory_order_relaxed);
                if (!self)
                {
                    return;
                }

                const int phase{ g_phase.load(std::memory_order_relaxed) };

                // =========================================================
                //  PHASE 1 -- every call shape the old implementation broke.
                // =========================================================
                if (phase == 1)
                {
                    // -- a peer object to pass and get back --------------
                    auto peer{ vmhook::make_unique<invoke_fixture>(k_peer_seed) };
                    g_peer_built.store(peer ? 1 : 0, std::memory_order_relaxed);
                    if (!peer)
                    {
                        return;
                    }
                    // vmhook::make_unique() allocates + stamps the object header
                    // but deliberately does NOT run the Java <init> chain, so the
                    // payload is stamped natively here.  It is what makes the
                    // object-return check an IDENTITY proof: the returned oop is
                    // re-wrapped and its seed re-read.
                    if (const auto seed_field{ peer->get_field("seed") })
                    {
                        seed_field->set(k_peer_seed);
                    }
                    g_peer_seed_ok.store(peer->seed() == k_peer_seed ? 1 : 0,
                                         std::memory_order_relaxed);
                    void* const peer_oop{ peer->vmhook::object_base::get_instance() };

                    // -- OBJECT argument + OBJECT return -----------------
                    // pair(peer) hands the peer straight back, so the result
                    // must be the SAME oop we passed in.  That is an identity
                    // proof, not merely "some object came back".
                    {
                        const auto proxy{ self->get_method("pair") };
                        g_pair_proxy_found.store(proxy.has_value() ? 1 : 0, std::memory_order_relaxed);
                        if (proxy.has_value())
                        {
                            const vmhook::method_proxy::value_t v{ proxy->call(peer) };
                            g_pair_threw.store(v.threw() ? 1 : 0, std::memory_order_relaxed);
                            void* const returned{ static_cast<void*>(v) };
                            g_pair_returned_nonnull.store(returned != nullptr ? 1 : 0,
                                                          std::memory_order_relaxed);
                            g_pair_identity.store(
                                (returned != nullptr && peer_oop != nullptr && returned == peer_oop) ? 1 : 0,
                                std::memory_order_relaxed);
                            // Payload proof: re-wrap the RETURNED oop and read the
                            // seed we stamped, so "the right object came back" is
                            // established by content as well as by address.
                            if (returned != nullptr && vmhook::hotspot::is_valid_pointer(returned))
                            {
                                invoke_fixture back{ returned };
                                g_pair_payload.store(back.seed() == k_peer_seed ? 1 : 0,
                                                     std::memory_order_relaxed);
                            }
                        }
                    }

                    // -- STRING argument + STRING return -----------------
                    // Isolation probe first: if a Java String cannot be built
                    // natively here at all, a null String ARGUMENT is a
                    // make_java_string problem, not an argument-packing one.
                    {
                        void* const probe{ vmhook::make_java_string("invoke-capability-probe") };
                        g_make_string_ok.store(probe != nullptr ? 1 : 0, std::memory_order_relaxed);
                        if (probe != nullptr && vmhook::hotspot::is_valid_pointer(probe))
                        {
                            g_make_string_roundtrips.store(
                                vmhook::read_java_string(probe) == "invoke-capability-probe" ? 1 : 0,
                                std::memory_order_relaxed);
                        }
                    }
                    {
                        const auto proxy{ self->get_method("echo") };
                        g_echo_proxy_found.store(proxy.has_value() ? 1 : 0, std::memory_order_relaxed);
                        if (proxy.has_value())
                        {
                            const vmhook::method_proxy::value_t v{ proxy->call(k_echo_arg) };
                            g_echo_threw.store(v.threw() ? 1 : 0, std::memory_order_relaxed);
                            g_echo_is_string.store(v.is_string() ? 1 : 0, std::memory_order_relaxed);
                            g_echo_value = v.as_string();
                            g_echo_exception = v.exception_class;

                            // Same call through the const char* packer branch.
                            const vmhook::method_proxy::value_t w{ proxy->call(k_echo_arg.c_str()) };
                            g_echo_cstr_threw.store(w.threw() ? 1 : 0, std::memory_order_relaxed);
                            g_echo_cstr_value = w.as_string();
                        }
                    }

                    // -- 2-slot LONG argument + long return --------------
                    {
                        const auto proxy{ self->get_method("addLong") };
                        g_long_proxy_found.store(proxy.has_value() ? 1 : 0, std::memory_order_relaxed);
                        if (proxy.has_value())
                        {
                            const vmhook::method_proxy::value_t v{ proxy->call(k_long_arg) };
                            g_long_threw.store(v.threw() ? 1 : 0, std::memory_order_relaxed);
                            g_long_value.store(static_cast<std::int64_t>(v), std::memory_order_relaxed);
                        }
                    }

                    // -- an INT argument widened into a J parameter -------
                    {
                        const auto proxy{ self->get_method("widenLong") };
                        g_widen_proxy_found.store(proxy.has_value() ? 1 : 0, std::memory_order_relaxed);
                        if (proxy.has_value())
                        {
                            const vmhook::method_proxy::value_t v{ proxy->call(k_widen_arg) };
                            g_widen_value.store(static_cast<std::int64_t>(v), std::memory_order_relaxed);
                        }
                    }

                    // -- 2-slot DOUBLE argument + double return ----------
                    {
                        const auto proxy{ self->get_method("halve") };
                        g_double_proxy_found.store(proxy.has_value() ? 1 : 0, std::memory_order_relaxed);
                        if (proxy.has_value())
                        {
                            const vmhook::method_proxy::value_t v{ proxy->call(k_double_arg) };
                            const double got{ static_cast<double>(v) };
                            std::int64_t bits{ 0 };
                            std::memcpy(&bits, &got, sizeof(bits));
                            g_double_bits.store(bits, std::memory_order_relaxed);
                        }
                    }

                    // -- VOID return -------------------------------------
                    {
                        const auto proxy{ self->get_method("bump") };
                        g_void_proxy_found.store(proxy.has_value() ? 1 : 0, std::memory_order_relaxed);
                        if (proxy.has_value())
                        {
                            const vmhook::method_proxy::value_t v{ proxy->call() };
                            g_void_is_void.store(v.is_void() ? 1 : 0, std::memory_order_relaxed);
                            g_void_threw.store(v.threw() ? 1 : 0, std::memory_order_relaxed);
                        }
                    }

                    // -- a NATIVE callee + the JNI handle-block watermark -
                    // System.currentTimeMillis() enters through the native
                    // entry, which pushes and pops JNI local handles.  If the
                    // caller's _active_handles->_top were not saved/restored,
                    // every JNI local ref alive across the call would be
                    // silently invalidated.
                    {
                        std::int32_t top_before{ -1 };
                        const bool readable{ read_handle_block_top(top_before) };
                        g_top_readable.store(readable ? 1 : 0, std::memory_order_relaxed);
                        g_top_before.store(top_before, std::memory_order_relaxed);

                        const auto proxy{ system_class::static_method("currentTimeMillis") };
                        g_native_proxy_found.store(proxy.has_value() ? 1 : 0, std::memory_order_relaxed);
                        if (proxy.has_value())
                        {
                            const vmhook::method_proxy::value_t v{ proxy->call() };
                            g_native_threw.store(v.threw() ? 1 : 0, std::memory_order_relaxed);
                            g_native_millis.store(static_cast<std::int64_t>(v), std::memory_order_relaxed);

                            std::int32_t top_after{ -1 };
                            if (readable && read_handle_block_top(top_after))
                            {
                                g_top_after.store(top_after, std::memory_order_relaxed);
                                g_top_preserved.store(top_after == top_before ? 1 : 0,
                                                      std::memory_order_relaxed);
                            }
                        }
                    }

                    // -- a THROWING callee -------------------------------
                    // The exception must be SURFACED (threw + classified), the
                    // payload value-initialised rather than the stub's garbage,
                    // and the thread left with NO pending exception.
                    {
                        const auto proxy{ self->get_method("boom") };
                        g_throw_proxy_found.store(proxy.has_value() ? 1 : 0, std::memory_order_relaxed);
                        if (proxy.has_value())
                        {
                            const vmhook::method_proxy::value_t v{ proxy->call() };
                            g_throw_flagged.store(v.threw() ? 1 : 0, std::memory_order_relaxed);
                            g_throw_payload.store(static_cast<std::int64_t>(v), std::memory_order_relaxed);
                            g_throw_class = v.exception_class;

                            void* pending{ reinterpret_cast<void*>(static_cast<std::uintptr_t>(1)) };
                            if (read_pending_exception(pending))
                            {
                                g_pending_readable.store(1, std::memory_order_relaxed);
                                g_pending_cleared.store(pending == nullptr ? 1 : 0,
                                                        std::memory_order_relaxed);
                            }
                            else
                            {
                                g_pending_readable.store(0, std::memory_order_relaxed);
                            }
                        }
                    }

                    // -- non-corruption: a value call right after the throw --
                    {
                        const auto proxy{ self->get_method("addLong") };
                        if (proxy.has_value())
                        {
                            const vmhook::method_proxy::value_t v{
                                proxy->call(k_addlong_after_throw_arg) };
                            g_after_throw_value.store(static_cast<std::int64_t>(v),
                                                      std::memory_order_relaxed);
                        }
                    }
                    return;
                }

                // =========================================================
                //  PHASE 2 -- a full GC across a live synthetic entry frame.
                // =========================================================
                if (phase == 2)
                {
                    const auto gc_proxy{ system_class::static_method("gc") };
                    g_gc_proxy_found.store(gc_proxy.has_value() ? 1 : 0, std::memory_order_relaxed);
                    if (gc_proxy.has_value())
                    {
                        const vmhook::method_proxy::value_t v{ gc_proxy->call() };
                        g_gc_threw.store(v.threw() ? 1 : 0, std::memory_order_relaxed);
                        g_gc_is_void.store(v.is_void() ? 1 : 0, std::memory_order_relaxed);
                        g_gc_survived.store(1, std::memory_order_relaxed);
                    }

                    // The VM (and this thread) must still be able to dispatch.
                    const auto proxy{ self->get_method("addLong") };
                    if (proxy.has_value())
                    {
                        const vmhook::method_proxy::value_t v{ proxy->call(k_long_arg) };
                        g_after_gc_value.store(static_cast<std::int64_t>(v), std::memory_order_relaxed);
                    }
                    return;
                }
            }) };

        ctx.check("invocation_hook_installed", handle.installed());

        // ---------------- PHASE 1 ----------------
        const bool done1{ drive(ctx, 1) };
        ctx.check("invocation_phase1_probe_completed", done1);
        ctx.check("invocation_java_warmup_ran", invoke_fixture::warm_rounds() >= 1);
        ctx.check("invocation_hook_fired", g_detour_calls.load(std::memory_order_relaxed) >= 1);
        ctx.check("invocation_hook_saw_self", g_detour_saw_self.load(std::memory_order_relaxed));

        ctx.check("invocation_peer_object_allocated",
                  g_peer_built.load(std::memory_order_relaxed) == 1);
        ctx.check("invocation_peer_seed_is_sentinel",
                  g_peer_seed_ok.load(std::memory_order_relaxed) == 1);

        // OBJECT argument + OBJECT return.
        ctx.check("invocation_object_call_proxy_found",
                  g_pair_proxy_found.load(std::memory_order_relaxed) == 1);
        ctx.check("invocation_object_call_did_not_throw",
                  g_pair_threw.load(std::memory_order_relaxed) == 0);
        ctx.check("invocation_object_return_is_nonnull",
                  g_pair_returned_nonnull.load(std::memory_order_relaxed) == 1);
        ctx.check("invocation_object_return_is_the_argument_object",
                  g_pair_identity.load(std::memory_order_relaxed) == 1);
        ctx.check("invocation_object_return_carries_the_expected_payload",
                  g_pair_payload.load(std::memory_order_relaxed) == 1);
        ctx.check("invocation_object_call_reached_java_body",
                  invoke_fixture::pair_calls() == 1);

        // STRING argument + STRING return, through BOTH String packer branches.
        // The DISPATCH half is unconditionally HARD: whatever the argument turns
        // out to be, both calls must reach the Java body.
        ctx.check("invocation_string_call_proxy_found",
                  g_echo_proxy_found.load(std::memory_order_relaxed) == 1);
        ctx.check("invocation_string_call_reached_java_body",
                  invoke_fixture::echo_calls() == 2);

        // The VALUE half is gated on the native String BUILDER working.  A String
        // argument is packed by calling vmhook::make_java_string(); if that
        // returns null the callee legitimately receives null and NPEs, and the
        // defect belongs to make_java_string -- a different feature, hard-asserted
        // by its own module -- not to the invocation path this module guards.
        const bool string_builder_ok{ g_make_string_ok.load(std::memory_order_relaxed) == 1
                                   && g_make_string_roundtrips.load(std::memory_order_relaxed) == 1 };
        if (string_builder_ok)
        {
            ctx.check("invocation_string_call_did_not_throw",
                      g_echo_threw.load(std::memory_order_relaxed) == 0);
            ctx.check("invocation_string_return_is_a_string",
                      g_echo_is_string.load(std::memory_order_relaxed) == 1);
            ctx.check("invocation_string_return_value_matches", g_echo_value == k_echo_expected);
            ctx.check("invocation_string_call_via_cstr_did_not_throw",
                      g_echo_cstr_threw.load(std::memory_order_relaxed) == 0);
            ctx.check("invocation_string_call_via_cstr_value_matches",
                      g_echo_cstr_value == k_echo_expected);
        }
        else
        {
            ctx.record("[INFO] invocation_capability: vmhook::make_java_string() could not build a "
                       "usable java.lang.String on this JVM (an in-detour probe returned null or "
                       "failed to round-trip), so a String ARGUMENT is packed as a null reference "
                       "and the callee NPEs on it.  That is a make_java_string defect -- covered "
                       "HARD by tests/jvm/modules/make_java_string.cpp "
                       "(native_roundtrip_majority_constructed) -- not an invocation-path one, so "
                       "the String-argument VALUE round trip is not asserted here.  The "
                       "OBJECT-argument + OBJECT-return proof above (a real fixture object passed "
                       "in, handed back, and verified by both address and payload) is unaffected "
                       "and stays HARD.");
        }
        ctx.record("[INFO] invocation_capability: make_java_string probe non_null="
                   + std::to_string(g_make_string_ok.load(std::memory_order_relaxed))
                   + " round_trips="
                   + std::to_string(g_make_string_roundtrips.load(std::memory_order_relaxed))
                   + "; String-argument call exception_class='" + g_echo_exception
                   + "' std_string_result='" + g_echo_value
                   + "' cstr_result='" + g_echo_cstr_value + "'");

        // 2-slot LONG.
        ctx.check("invocation_long_call_proxy_found",
                  g_long_proxy_found.load(std::memory_order_relaxed) == 1);
        ctx.check("invocation_long_call_did_not_throw",
                  g_long_threw.load(std::memory_order_relaxed) == 0);
        ctx.check("invocation_long_two_slot_arg_and_return",
                  g_long_value.load(std::memory_order_relaxed) == k_long_expected);

        // INT widened into a J parameter.
        ctx.check("invocation_widen_call_proxy_found",
                  g_widen_proxy_found.load(std::memory_order_relaxed) == 1);
        ctx.check("invocation_int_argument_widened_into_J_parameter",
                  g_widen_value.load(std::memory_order_relaxed) == k_widen_expected);

        // 2-slot DOUBLE (exact bit pattern).
        ctx.check("invocation_double_call_proxy_found",
                  g_double_proxy_found.load(std::memory_order_relaxed) == 1);
        {
            std::int64_t expected_bits{ 0 };
            const double expected{ k_double_expected };
            std::memcpy(&expected_bits, &expected, sizeof(expected_bits));
            ctx.check("invocation_double_two_slot_arg_and_return",
                      g_double_bits.load(std::memory_order_relaxed) == expected_bits);
        }

        // VOID return.
        ctx.check("invocation_void_call_proxy_found",
                  g_void_proxy_found.load(std::memory_order_relaxed) == 1);
        ctx.check("invocation_void_return_is_void",
                  g_void_is_void.load(std::memory_order_relaxed) == 1);
        ctx.check("invocation_void_call_did_not_throw",
                  g_void_threw.load(std::memory_order_relaxed) == 0);
        ctx.check("invocation_void_call_reached_java_body",
                  invoke_fixture::void_hits() == 1);

        // NATIVE callee + handle-block watermark.
        ctx.check("invocation_native_call_proxy_found",
                  g_native_proxy_found.load(std::memory_order_relaxed) == 1);
        ctx.check("invocation_native_call_did_not_throw",
                  g_native_threw.load(std::memory_order_relaxed) == 0);
        ctx.check("invocation_native_call_returned_a_plausible_epoch",
                  g_native_millis.load(std::memory_order_relaxed) > k_epoch_floor_ms);
        if (g_top_readable.load(std::memory_order_relaxed) == 1)
        {
            ctx.check("invocation_handle_block_top_preserved_across_native_callee",
                      g_top_preserved.load(std::memory_order_relaxed) == 1);
        }
        else
        {
            ctx.record("[INFO] invocation_capability: JavaThread::_active_handles / "
                       "JNIHandleBlock::_top could not be resolved+read through VMStructs on this "
                       "JDK, so the handle-block watermark check is not asserted this run (a "
                       "fault-safe read returned false; nothing was dereferenced).");
        }
        {
            std::ostringstream oss{};
            oss << "[INFO] invocation_capability: JNIHandleBlock::_top before/after the native "
                   "callee = " << g_top_before.load(std::memory_order_relaxed) << "/"
                << g_top_after.load(std::memory_order_relaxed);
            ctx.record(oss.str());
        }

        // THROWING callee.
        ctx.check("invocation_throwing_call_proxy_found",
                  g_throw_proxy_found.load(std::memory_order_relaxed) == 1);
        ctx.check("invocation_throwing_callee_is_flagged",
                  g_throw_flagged.load(std::memory_order_relaxed) == 1);
        ctx.check("invocation_throwing_callee_is_classified",
                  g_throw_class == "java/lang/IllegalStateException");
        ctx.check("invocation_throwing_callee_payload_is_zeroed",
                  g_throw_payload.load(std::memory_order_relaxed) == 0);
        ctx.check("invocation_throwing_callee_reached_java_body",
                  invoke_fixture::boom_calls() == 1);
        if (g_pending_readable.load(std::memory_order_relaxed) == 1)
        {
            ctx.check("invocation_pending_exception_cleared_after_throw",
                      g_pending_cleared.load(std::memory_order_relaxed) == 1);
        }
        else
        {
            ctx.record("[INFO] invocation_capability: ThreadShadow::_pending_exception could not "
                       "be resolved+read through VMStructs on this JDK, so the "
                       "pending-exception-cleared check is not asserted this run (fault-safe read "
                       "returned false; nothing was dereferenced).  value_t::threw() above still "
                       "proves the throw was surfaced.");
        }
        ctx.check("invocation_value_call_after_a_throw_still_works",
                  g_after_throw_value.load(std::memory_order_relaxed)
                      == k_addlong_after_throw_arg + 1);

        // ---------------- PHASE 2: GC across the synthetic entry frame ------
        const bool done2{ drive(ctx, 2) };
        ctx.check("invocation_phase2_probe_completed", done2);
        ctx.check("invocation_gc_call_proxy_found",
                  g_gc_proxy_found.load(std::memory_order_relaxed) == 1);
        ctx.check("invocation_system_gc_through_entry_frame_survived",
                  g_gc_survived.load(std::memory_order_relaxed) == 1);
        ctx.check("invocation_system_gc_through_entry_frame_did_not_throw",
                  g_gc_threw.load(std::memory_order_relaxed) == 0);
        ctx.check("invocation_system_gc_through_entry_frame_returned_void",
                  g_gc_is_void.load(std::memory_order_relaxed) == 1);
        ctx.check("invocation_still_works_after_a_full_gc",
                  g_after_gc_value.load(std::memory_order_relaxed) == k_long_expected);
        ctx.record("[INFO] invocation_capability: the collection above ran THROUGH a live "
                   "synthetic entry frame and the frame walk survived it, which is what the old "
                   "`link = -1` argument could not do.  KNOWN AFTER-EFFECT (measured on live JDK "
                   "21 / G1): a LATER stop-the-world collection can then crash HotSpot's GC "
                   "worker while it walks this JavaThread, so this module is pinned to "
                   "vmhook_test::priority::last -- see the file header for the bisection.");
    }
#else
    ctx.record("[INFO] invocation_capability: skipped -- pure-VM invocation is x86-64 only.  The "
               "call-stub derivation validates HotSpot's x86-64 enter() prologue (55 48 8B EC) and "
               "the `call c_rarg1` (FF D2 / FF D6) that dispatches into Java, so "
               "call_stub_entry_is_valid() returns false unconditionally on every other "
               "architecture and find_call_stub_entry() correctly reports the capability as "
               "absent.  Porting needs a per-architecture prologue pattern and a different "
               "frame::entry_frame_call_wrapper_offset (see docs/research/pure_vm_invocation.md "
               "section 9).");
#endif
}
