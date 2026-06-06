// Runtime null-safety / never-throw checks for vmhook public entry points
// when NO JVM is loaded in the process.  Every documented entry point must
// no-op safely (return its safe-default, invoke no visitor, never crash).
// Extends tests/test_api_surface.cpp, which only checks that the surface
// type-checks; this file actually *runs* main() with no JVM behind it.
#include <vmhook/vmhook.hpp>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

// A minimal wrapper type used for register_class<T> / make_unique<T> /
// for_each_instance<T>.  Derives from vmhook::object<T> with the required
// explicit T(vmhook::oop_t) constructor, mirroring the pattern in
// test_api_surface.cpp.
class dummy_wrapper : public vmhook::object<dummy_wrapper>
{
public:
    explicit dummy_wrapper(vmhook::oop_t oop) noexcept
        : vmhook::object<dummy_wrapper>{ oop }
    {
    }
};

int main()
{
    // --- find_class: no JVM -> nullptr, never throws ---------------------
    {
        vmhook::hotspot::klass* k{ nullptr };
        bool threw{ false };
        try { k = vmhook::find_class("java/lang/String"); }
        catch (...) { threw = true; }
        check("find_class_string_returns_null_without_jvm", k == nullptr);
        check("find_class_does_not_throw_without_jvm", !threw);
    }
    {
        // A class that does not exist anywhere must also be null, not throw.
        vmhook::hotspot::klass* k{ vmhook::find_class("definitely/Not/A/Real/Class") };
        check("find_class_missing_class_returns_null", k == nullptr);
    }
    {
        // Empty class name is still a safe lookup that yields null.
        vmhook::hotspot::klass* k{ vmhook::find_class("") };
        check("find_class_empty_name_returns_null", k == nullptr);
    }

    // --- read_java_string: null oop -> empty string, never throws --------
    {
        std::string s{ "sentinel" };
        bool threw{ false };
        try { s = vmhook::read_java_string(nullptr); }
        catch (...) { threw = true; }
        check("read_java_string_null_returns_empty", s.empty());
        check("read_java_string_null_does_not_throw", !threw);
    }
    {
        // A bogus non-null pointer is rejected by is_valid_pointer and must
        // yield an empty string rather than dereferencing garbage.
        void* const bogus{ reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x1)) };
        std::string s{ "sentinel" };
        bool threw{ false };
        try { s = vmhook::read_java_string(bogus); }
        catch (...) { threw = true; }
        check("read_java_string_bogus_ptr_returns_empty", s.empty());
        check("read_java_string_bogus_ptr_does_not_throw", !threw);
    }

    // --- shutdown_hooks: safe to call with no hooks installed ------------
    {
        bool threw{ false };
        try
        {
            vmhook::shutdown_hooks();
            // Idempotent: a second call with nothing installed is still safe.
            vmhook::shutdown_hooks();
        }
        catch (...) { threw = true; }
        check("shutdown_hooks_no_hooks_does_not_throw", !threw);
    }

    // --- for_each_loaded_class: no JVM -> visitor never invoked ----------
    {
        int count{ 0 };
        bool threw{ false };
        try
        {
            vmhook::for_each_loaded_class(
                [&count](const std::string&, vmhook::hotspot::klass*)
                {
                    ++count;
                });
        }
        catch (...) { threw = true; }
        check("for_each_loaded_class_visitor_not_invoked_without_jvm", count == 0);
        check("for_each_loaded_class_does_not_throw_without_jvm", !threw);
    }

    // --- for_each_thread: no JVM -> visitor never invoked ----------------
    {
        int count{ 0 };
        bool threw{ false };
        try
        {
            vmhook::for_each_thread(
                [&count](const vmhook::thread_info&)
                {
                    ++count;
                });
        }
        catch (...) { threw = true; }
        check("for_each_thread_visitor_not_invoked_without_jvm", count == 0);
        check("for_each_thread_does_not_throw_without_jvm", !threw);
    }

    // --- register_class<T>: no JVM -> returns false (find_class fails) ---
    bool registered{ true };
    {
        bool threw{ false };
        try { registered = vmhook::register_class<dummy_wrapper>("my/Dummy"); }
        catch (...) { threw = true; }
        check("register_class_returns_false_without_jvm", registered == false);
        check("register_class_does_not_throw_without_jvm", !threw);
    }

    // --- for_each_instance<T>: no JVM -> 0 instances, visitor not run ----
    // for_each_instance resolves T's registered klass first; with no JVM the
    // type was never registered (register_class returned false), so it must
    // bail out reporting zero and never touch the visitor.
    {
        int count{ 0 };
        std::size_t reported{ 123 };
        bool threw{ false };
        try
        {
            reported = vmhook::for_each_instance<dummy_wrapper>(
                [&count](std::unique_ptr<dummy_wrapper>)
                {
                    ++count;
                });
        }
        catch (...) { threw = true; }
        check("for_each_instance_visitor_not_invoked_without_jvm", count == 0);
        check("for_each_instance_reports_zero_without_jvm", reported == 0);
        check("for_each_instance_does_not_throw_without_jvm", !threw);
    }
    {
        // Same, but with an explicit max_visits cap argument exercised.
        int count{ 0 };
        std::size_t reported{ vmhook::for_each_instance<dummy_wrapper>(
            [&count](std::unique_ptr<dummy_wrapper>) { ++count; },
            8) };
        check("for_each_instance_with_max_visits_reports_zero", reported == 0);
        check("for_each_instance_with_max_visits_visitor_not_invoked", count == 0);
    }

    // --- make_unique<T>: no JVM -> nullptr, never throws -----------------
    {
        std::unique_ptr<dummy_wrapper> obj{ reinterpret_cast<dummy_wrapper*>(0) };
        bool threw{ false };
        try { obj = vmhook::make_unique<dummy_wrapper>(); }
        catch (...) { threw = true; }
        check("make_unique_returns_null_without_jvm", obj == nullptr);
        check("make_unique_does_not_throw_without_jvm", !threw);
    }

    // --- on_class_loaded: no JVM -> empty handle, running()==false -------
    // The class-load hook install requires resolving java.lang.ClassLoader,
    // which fails without a JVM; the returned watch_handle must therefore be
    // inert (running() == false) and must not fire the callback.
    {
        int fired{ 0 };
        bool threw{ false };
        bool running_true{ true };
        try
        {
            vmhook::watch_handle handle{ vmhook::on_class_loaded(
                [&fired](const std::string&) { ++fired; }) };
            running_true = handle.running();
        }
        catch (...) { threw = true; }
        check("on_class_loaded_handle_not_running_without_jvm", running_true == false);
        check("on_class_loaded_callback_not_fired_without_jvm", fired == 0);
        check("on_class_loaded_does_not_throw_without_jvm", !threw);
    }

    // --- on_exception: no JVM -> empty handle, running()==false ----------
    // Mirrors on_class_loaded: installing the Throwable.fillInStackTrace hook
    // needs a live JVM; without one the handle is inert.
    {
        int fired{ 0 };
        bool threw{ false };
        bool running_true{ true };
        try
        {
            vmhook::watch_handle handle{ vmhook::on_exception(
                [&fired](const std::string&) { ++fired; }) };
            running_true = handle.running();
        }
        catch (...) { threw = true; }
        check("on_exception_handle_not_running_without_jvm", running_true == false);
        check("on_exception_callback_not_fired_without_jvm", fired == 0);
        check("on_exception_does_not_throw_without_jvm", !threw);
    }

    // --- watch_handle default-construct is inert -------------------------
    {
        vmhook::watch_handle handle{};
        check("default_watch_handle_not_running", handle.running() == false);
    }

    // --- deoptimize_methods_if: no JVM -> 0, predicate never invoked -----
    // deoptimize_methods_if delegates to for_each_loaded_class, which is a
    // no-op without a live JVM (the class iterator finds nothing).  Because
    // the predicate is only consulted *inside* the per-method loop, and that
    // loop never runs, the predicate must never fire and the returned count
    // must be exactly 0.  std::size_t return type is asserted by binding it.
    {
        int calls{ 0 };
        std::size_t deopt{ 999 };
        bool threw{ false };
        try
        {
            deopt = vmhook::deoptimize_methods_if(
                [&calls](const std::string&, vmhook::hotspot::method*)
                {
                    ++calls;
                    return true;  // always-true predicate
                });
        }
        catch (...) { threw = true; }
        check("deoptimize_methods_if_always_true_returns_zero", deopt == 0);
        check("deoptimize_methods_if_always_true_predicate_not_invoked", calls == 0);
        check("deoptimize_methods_if_always_true_does_not_throw", !threw);
    }
    {
        // An always-false predicate must likewise never be called and return 0.
        int calls{ 0 };
        std::size_t deopt{ vmhook::deoptimize_methods_if(
            [&calls](const std::string&, vmhook::hotspot::method*)
            {
                ++calls;
                return false;
            }) };
        check("deoptimize_methods_if_always_false_returns_zero", deopt == 0);
        check("deoptimize_methods_if_always_false_predicate_not_invoked", calls == 0);
    }
    {
        // A name-based predicate (the documented Minecraft-style use case)
        // must behave identically: zero result, predicate untouched.
        int calls{ 0 };
        std::size_t deopt{ vmhook::deoptimize_methods_if(
            [&calls](const std::string& class_name, vmhook::hotspot::method*)
            {
                ++calls;
                return class_name.starts_with("net/minecraft/");
            }) };
        check("deoptimize_methods_if_name_based_returns_zero", deopt == 0);
        check("deoptimize_methods_if_name_based_predicate_not_invoked", calls == 0);
    }
    {
        // The std::size_t return type is part of the contract; this fails to
        // compile if the return type ever changes to a signed/narrower type.
        static_assert(
            std::is_same_v<decltype(vmhook::deoptimize_methods_if(
                [](const std::string&, vmhook::hotspot::method*) { return true; })),
                std::size_t>,
            "deoptimize_methods_if must return std::size_t");
        check("deoptimize_methods_if_return_type_is_size_t", true);
    }
    {
        // Idempotent / repeatable: calling it twice still yields 0 each time
        // and still never invokes the predicate.
        int calls{ 0 };
        std::size_t first{ vmhook::deoptimize_methods_if(
            [&calls](const std::string&, vmhook::hotspot::method*) { ++calls; return true; }) };
        std::size_t second{ vmhook::deoptimize_methods_if(
            [&calls](const std::string&, vmhook::hotspot::method*) { ++calls; return true; }) };
        check("deoptimize_methods_if_repeated_first_zero", first == 0);
        check("deoptimize_methods_if_repeated_second_zero", second == 0);
        check("deoptimize_methods_if_repeated_predicate_not_invoked", calls == 0);
    }

    // --- deoptimize_all_jit_compiled_methods: no JVM -> 0, never throws --
    // Convenience wrapper over deoptimize_methods_if with an always-true
    // predicate; without a JVM it must return 0 and stay crash-free.
    {
        std::size_t deopt{ 999 };
        bool threw{ false };
        try { deopt = vmhook::deoptimize_all_jit_compiled_methods(); }
        catch (...) { threw = true; }
        check("deoptimize_all_jit_returns_zero_without_jvm", deopt == 0);
        check("deoptimize_all_jit_does_not_throw_without_jvm", !threw);
    }
    {
        // Idempotent across repeated calls with no JVM.
        std::size_t a{ vmhook::deoptimize_all_jit_compiled_methods() };
        std::size_t b{ vmhook::deoptimize_all_jit_compiled_methods() };
        std::size_t c{ vmhook::deoptimize_all_jit_compiled_methods() };
        check("deoptimize_all_jit_idempotent_zero",
              a == 0 && b == 0 && c == 0);
        static_assert(
            std::is_same_v<decltype(vmhook::deoptimize_all_jit_compiled_methods()),
                std::size_t>,
            "deoptimize_all_jit_compiled_methods must return std::size_t");
        check("deoptimize_all_jit_return_type_is_size_t", true);
    }

    // --- scoped_hook<T>: no JVM -> inert handle, never throws ------------
    // scoped_hook<T> first calls hook<T>(); with no JVM the throwaway wrapper
    // type was never registered (register_class returned false above) so
    // hook<T>() raises vmhook::exception internally, but hook<T>() catches
    // every std::exception itself and returns false.  scoped_hook sees the
    // false, logs, and returns an empty hook_handle.  The contract is
    // therefore: returns an inert handle (installed() == false) and does NOT
    // throw.  The callback must never fire.
    {
        int fired{ 0 };
        bool threw{ false };
        bool installed_true{ true };
        try
        {
            vmhook::hook_handle handle{ vmhook::scoped_hook<dummy_wrapper>(
                "doStuff",
                [&fired](vmhook::return_value&) { ++fired; }) };
            installed_true = handle.installed();
        }
        catch (...) { threw = true; }
        check("scoped_hook_handle_not_installed_without_jvm", installed_true == false);
        check("scoped_hook_does_not_throw_without_jvm", !threw);
        check("scoped_hook_callback_not_fired_without_jvm", fired == 0);
    }
    {
        // Same for the explicit-signature overload: it routes through the
        // same hook<T>() pre-check and behaves identically with no JVM.
        int fired{ 0 };
        bool threw{ false };
        bool installed_true{ true };
        try
        {
            vmhook::hook_handle handle{ vmhook::scoped_hook<dummy_wrapper>(
                "doStuff", "()V",
                [&fired](vmhook::return_value&) { ++fired; }) };
            installed_true = handle.installed();
        }
        catch (...) { threw = true; }
        check("scoped_hook_with_signature_handle_not_installed", installed_true == false);
        check("scoped_hook_with_signature_does_not_throw", !threw);
        check("scoped_hook_with_signature_callback_not_fired", fired == 0);
    }
    {
        // hook_handle inert-state contract (the state scoped_hook returns on a
        // post-install resolution failure): default-constructed handle is not
        // installed, is move-constructible, the moved-from source becomes
        // inert too, and destruction of an empty handle is safe.
        bool threw{ false };
        bool src_installed{ true };
        bool dst_installed{ true };
        try
        {
            vmhook::hook_handle empty{};
            check("scoped_hook_default_handle_not_installed",
                  empty.installed() == false);

            vmhook::hook_handle moved{ std::move(empty) };
            dst_installed = moved.installed();
            src_installed = empty.installed();  // NOLINT(bugprone-use-after-move)
            // `moved` and `empty` both destruct here at end of scope.
        }
        catch (...) { threw = true; }
        check("scoped_hook_moved_to_handle_not_installed", dst_installed == false);
        check("scoped_hook_moved_from_handle_not_installed", src_installed == false);
        check("scoped_hook_inert_handle_destructor_safe", !threw);
    }
    {
        // Move-assignment of an inert handle is also safe and stays inert.
        bool threw{ false };
        bool assigned_installed{ true };
        try
        {
            vmhook::hook_handle a{};
            vmhook::hook_handle b{};
            b = std::move(a);
            assigned_installed = b.installed();
        }
        catch (...) { threw = true; }
        check("scoped_hook_move_assigned_handle_not_installed",
              assigned_installed == false);
        check("scoped_hook_move_assign_inert_handle_safe", !threw);
    }

    // --- watch_static_field<T, value_t>: no JVM -> inert handle ----------
    // On Windows x86_64 (hardware data breakpoints available) the real path
    // runs and first resolves the field via object_base::get_field, which
    // returns nullopt without a JVM (klass not resolved) -> inert handle.
    // On every other platform the function returns an inert handle directly.
    // Either way running() must be false and the callback must never fire.
    // Exercised across the field-width-distinct value types the DR LEN field
    // selection depends on (1/2/4/8 bytes + bool + float/double).
    {
        int fired{ 0 };
        bool threw{ false };
        bool running_true{ true };
        try
        {
            vmhook::watch_handle handle{
                vmhook::watch_static_field<dummy_wrapper, std::int32_t>(
                    "counter",
                    [&fired](std::int32_t, std::int32_t) { ++fired; }) };
            running_true = handle.running();
        }
        catch (...) { threw = true; }
        check("watch_static_field_int32_not_running_without_jvm", running_true == false);
        check("watch_static_field_int32_callback_not_fired", fired == 0);
        check("watch_static_field_int32_does_not_throw", !threw);
    }
    {
        int fired{ 0 };
        std::int64_t dummy{ 0 };
        (void)dummy;
        vmhook::watch_handle handle{
            vmhook::watch_static_field<dummy_wrapper, std::int64_t>(
                "longCounter",
                [&fired](std::int64_t, std::int64_t) { ++fired; }) };
        check("watch_static_field_int64_not_running_without_jvm", handle.running() == false);
        check("watch_static_field_int64_callback_not_fired", fired == 0);
    }
    {
        int fired{ 0 };
        vmhook::watch_handle handle{
            vmhook::watch_static_field<dummy_wrapper, float>(
                "ratio",
                [&fired](float, float) { ++fired; }) };
        check("watch_static_field_float_not_running_without_jvm", handle.running() == false);
        check("watch_static_field_float_callback_not_fired", fired == 0);
    }
    {
        int fired{ 0 };
        vmhook::watch_handle handle{
            vmhook::watch_static_field<dummy_wrapper, double>(
                "scale",
                [&fired](double, double) { ++fired; }) };
        check("watch_static_field_double_not_running_without_jvm", handle.running() == false);
        check("watch_static_field_double_callback_not_fired", fired == 0);
    }
    {
        // bool is a single-byte field width (exercises the one_byte LEN arm).
        int fired{ 0 };
        bool threw{ false };
        bool running_true{ true };
        try
        {
            vmhook::watch_handle handle{
                vmhook::watch_static_field<dummy_wrapper, bool>(
                    "flag",
                    [&fired](bool, bool) { ++fired; }) };
            running_true = handle.running();
        }
        catch (...) { threw = true; }
        check("watch_static_field_bool_not_running_without_jvm", running_true == false);
        check("watch_static_field_bool_callback_not_fired", fired == 0);
        check("watch_static_field_bool_does_not_throw", !threw);
    }
    {
        // Inert watch_handle from watch_static_field destructs safely when it
        // drops out of scope (its on_stop is empty, so no DR teardown runs).
        bool threw{ false };
        try
        {
            vmhook::watch_handle handle{
                vmhook::watch_static_field<dummy_wrapper, std::int32_t>(
                    "x", [](std::int32_t, std::int32_t) {}) };
        }
        catch (...) { threw = true; }
        check("watch_static_field_inert_handle_destructor_safe", !threw);
    }

    // --- klass_from_oop: null + junk pointers -> nullptr, never throws ---
    // klass_from_oop guards with is_valid_pointer() *before* dereferencing
    // (it reads the narrow-klass at oop+8 only for valid pointers).  null,
    // sub-floor (<= 0xFFFF), and non-canonical (>= ceiling) pointers are all
    // rejected up front, so no garbage is ever dereferenced.
    {
        bool threw{ false };
        vmhook::hotspot::klass* k{ reinterpret_cast<vmhook::hotspot::klass*>(0x1) };
        try { k = vmhook::klass_from_oop(nullptr); }
        catch (...) { threw = true; }
        check("klass_from_oop_null_returns_null", k == nullptr);
        check("klass_from_oop_null_does_not_throw", !threw);
    }
    {
        // 0x1000 (4096) is <= user_address_floor (0xFFFF) -> rejected by
        // is_valid_pointer, so klass_from_oop returns null without reading it.
        void* const small_junk{ reinterpret_cast<void*>(
            static_cast<std::uintptr_t>(0x1000)) };
        bool threw{ false };
        vmhook::hotspot::klass* k{ reinterpret_cast<vmhook::hotspot::klass*>(0x1) };
        try { k = vmhook::klass_from_oop(small_junk); }
        catch (...) { threw = true; }
        check("klass_from_oop_small_junk_returns_null", k == nullptr);
        check("klass_from_oop_small_junk_does_not_throw", !threw);
    }
    {
        // A non-canonical high pointer (>= user_address_ceiling) is likewise
        // rejected before any dereference.
        void* const high_junk{ reinterpret_cast<void*>(
            static_cast<std::uintptr_t>(0xDEAD000000000000ull)) };
        bool threw{ false };
        vmhook::hotspot::klass* k{ reinterpret_cast<vmhook::hotspot::klass*>(0x1) };
        try { k = vmhook::klass_from_oop(high_junk); }
        catch (...) { threw = true; }
        check("klass_from_oop_high_noncanonical_returns_null", k == nullptr);
        check("klass_from_oop_high_noncanonical_does_not_throw", !threw);
    }
    {
        // An odd (mis-aligned) pointer is rejected by the alignment check.
        void* const odd_junk{ reinterpret_cast<void*>(
            static_cast<std::uintptr_t>(0x100001ull)) };
        vmhook::hotspot::klass* const k{ vmhook::klass_from_oop(odd_junk) };
        check("klass_from_oop_odd_pointer_returns_null", k == nullptr);
    }

    return failures == 0 ? 0 : 1;
}
