// Standalone (no-JVM) unit test for the JNI-local-reference RAII *contract*
// that vmhook's detour code relies on, and that the upstream library function
// vmhook::detail::jni_delete_local_ref (vmhook.hpp slot-23 wrapper, ~L11699)
// already satisfies on the cold path:
//
//   * null handle => documented JNI no-op, mirrored as an early `return;`
//     (vmhook.hpp ~L11702).  So calling jni_delete_local_ref(nullptr) with NO
//     live JNIEnv is observable as: returns, does not crash, does not log,
//     does not touch vmhook::hotspot::current_jni_env beyond the noexcept
//     function-pointer lookup which itself returns nullptr on cold state.
//   * non-null handle with no JVM => jni_function<23>(nullptr_env) returns
//     nullptr, the inner if-guard skips the dispatch, again no UB.
//
// That cold-safety is what makes a *scoped* RAII wrapper around the slot-23
// release a sound idiom in this codebase: a wrapper whose destructor calls
// jni_delete_local_ref on a possibly-null handle is safe to default-construct,
// move-from (leaving the source null), assign-over, and destroy even when the
// process never attached a JVM at all.  This file pins that contract on a
// TEST-LOCAL wrapper that uses the LIBRARY function as its deleter — so we
// catch (a) any future change to jni_delete_local_ref's null-handling or
// noexcept-ness as a hard build break, and (b) any regression in the RAII
// invariants the call sites (and any future in-library wrapper) depend on.
//
// What is intentionally out of scope: invoking a REAL JNIEnv::DeleteLocalRef
// (no JVM here, slot-23 pointer is nullptr), measuring the HotSpot local-ref
// table overflow at 16 (that is the live-JVM jni_local_ref_hygiene module's
// job in tests/jvm/modules/), or fabricating a fake jobject and feeding it to
// the slot-23 trampoline.  The deepening here is purely the wrapper RAII
// contract on cold state.

#include "vmhook/vmhook.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <type_traits>
#include <utility>

namespace
{
    // Test-local scoped wrapper around the library slot-23 release.  The
    // deleter is vmhook::detail::jni_delete_local_ref, which is noexcept and
    // null-safe and cold-safe (no-JVM).  Move-only: copying a local-ref handle
    // would double-release on destruction.
    class scoped_local_ref
    {
    public:
        constexpr scoped_local_ref() noexcept = default;

        explicit constexpr scoped_local_ref(void* h) noexcept : h_{ h } {}

        scoped_local_ref(scoped_local_ref const&)            = delete;
        scoped_local_ref& operator=(scoped_local_ref const&) = delete;

        scoped_local_ref(scoped_local_ref&& other) noexcept
            : h_{ other.h_ }
        {
            other.h_ = nullptr;
        }

        scoped_local_ref& operator=(scoped_local_ref&& other) noexcept
        {
            if (this != &other)
            {
                reset(other.h_);
                other.h_ = nullptr;
            }
            return *this;
        }

        ~scoped_local_ref() noexcept
        {
            // Library contract: null is a documented JNI no-op and cold-state
            // (no JNIEnv) returns without touching the slot.  Either is safe.
            vmhook::detail::jni_delete_local_ref(h_);
        }

        constexpr void* get() const noexcept { return h_; }
        constexpr explicit operator bool() const noexcept { return h_ != nullptr; }

        void* release() noexcept
        {
            void* h = h_;
            h_      = nullptr;
            return h;
        }

        void reset(void* h = nullptr) noexcept
        {
            if (h_ != h)
            {
                vmhook::detail::jni_delete_local_ref(h_);
                h_ = h;
            }
        }

    private:
        void* h_{ nullptr };
    };

    // === Compile-time RAII contract pins =====================================
    static_assert(std::is_nothrow_default_constructible_v<scoped_local_ref>,
                  "default ctor must be noexcept (cold-state safe)");
    static_assert(std::is_nothrow_destructible_v<scoped_local_ref>,
                  "dtor must be noexcept (matches jni_delete_local_ref noexcept)");
    static_assert(std::is_nothrow_move_constructible_v<scoped_local_ref>,
                  "move ctor must be noexcept (vector-friendly, no surprise throws)");
    static_assert(std::is_nothrow_move_assignable_v<scoped_local_ref>,
                  "move assign must be noexcept");
    static_assert(!std::is_copy_constructible_v<scoped_local_ref>,
                  "move-only: copying a local-ref handle would double-release");
    static_assert(!std::is_copy_assignable_v<scoped_local_ref>,
                  "move-only: copy-assigning a local-ref handle would double-release");

    // The library deleter we depend on is itself noexcept — pin it so any
    // future loss of noexcept becomes a hard compile break here, not a silent
    // change of the call-site contract.
    static_assert(noexcept(vmhook::detail::jni_delete_local_ref(nullptr)),
                  "vmhook::detail::jni_delete_local_ref must remain noexcept");

    int g_static_assert_count = 7;
}

#define EXPECT(cond) do {                                                       \
    if (!(cond)) {                                                              \
        std::fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
        return 1;                                                               \
    }                                                                           \
} while (0)

int main()
{
    int asserts = 0;

    // 1. Default-construct => null handle; dtor on null is safe (no JNIEnv).
    {
        scoped_local_ref r{};
        EXPECT(r.get() == nullptr); ++asserts;
        EXPECT(!r);                 ++asserts;
        // dtor runs here on null -> must not crash, must not touch JNI table.
    }

    // 2. Cold-state direct call into the library deleter on nullptr is safe
    //    AND is the same path the dtor takes.  Re-pin the contract observably.
    vmhook::detail::jni_delete_local_ref(nullptr);
    EXPECT(true); ++asserts;

    // 3. reset() on a default wrapper is a no-op; subsequent get() stays null.
    {
        scoped_local_ref r{};
        r.reset();                 // null -> null
        EXPECT(r.get() == nullptr); ++asserts;
        r.reset(nullptr);          // explicit nullptr -> null
        EXPECT(r.get() == nullptr); ++asserts;
    }

    // 4. release() on a null wrapper returns null and leaves it null.
    {
        scoped_local_ref r{};
        void* h = r.release();
        EXPECT(h == nullptr);       ++asserts;
        EXPECT(r.get() == nullptr); ++asserts;
    }

    // 5. Move-construct from a null wrapper: both null, dtors safe.
    {
        scoped_local_ref a{};
        scoped_local_ref b{ std::move(a) };
        EXPECT(a.get() == nullptr); ++asserts;
        EXPECT(b.get() == nullptr); ++asserts;
    }

    // 6. Move-assign null -> null wrapper is a no-op.
    {
        scoped_local_ref a{};
        scoped_local_ref b{};
        b = std::move(a);
        EXPECT(a.get() == nullptr); ++asserts;
        EXPECT(b.get() == nullptr); ++asserts;
    }

    // 7. Move-construct TRANSFERS the handle and leaves the source null.
    //    We use a non-null SENTINEL pointer that is NEVER dereferenced — the
    //    library deleter's null-guard fails (handle is non-null), then the
    //    function-pointer lookup returns nullptr (no JVM), then the inner
    //    if-guard skips the dispatch.  So this pointer is never read.
    {
        // Pick a clearly fake but well-aligned sentinel; never dereferenced.
        auto* sentinel = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xC0DE0FF1CEull));
        scoped_local_ref a{ sentinel };
        EXPECT(a.get() == sentinel); ++asserts;
        EXPECT(static_cast<bool>(a)); ++asserts;

        scoped_local_ref b{ std::move(a) };
        EXPECT(a.get() == nullptr); ++asserts; // source nulled
        EXPECT(b.get() == sentinel); ++asserts; // target took ownership
        // b's dtor will run here: cold-state library deleter never touches it.
    }

    // 8. Move-assign with a non-null source transfers ownership; the previous
    //    contents of the target are released exactly once (cold-state no-op).
    {
        auto* sentinel = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xBADC0FFEEull));
        scoped_local_ref a{ sentinel };
        scoped_local_ref b{};
        b = std::move(a);
        EXPECT(a.get() == nullptr);  ++asserts;
        EXPECT(b.get() == sentinel); ++asserts;
    }

    // 9. Self-move-assign is a no-op (does NOT release the held handle).
    {
        auto* sentinel = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xFEEDFACEull));
        scoped_local_ref a{ sentinel };
        scoped_local_ref& a_ref = a;          // launder through ref to dodge -Wself-move
        a = std::move(a_ref);
        EXPECT(a.get() == sentinel); ++asserts;
    }

    // 10. reset(new) on a non-null wrapper: replaces (and would release the
    //     prior under a live JVM); cold-state both releases are no-ops.
    {
        auto* s1 = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x1111ull));
        auto* s2 = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x2222ull));
        scoped_local_ref r{ s1 };
        r.reset(s2);
        EXPECT(r.get() == s2); ++asserts;
        r.reset();
        EXPECT(r.get() == nullptr); ++asserts;
    }

    // 11. reset(same) is a no-op and crucially does NOT release-and-rebind
    //     (which would invalidate the handle the caller just rebound).
    {
        auto* sentinel = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x3333ull));
        scoped_local_ref r{ sentinel };
        r.reset(sentinel);
        EXPECT(r.get() == sentinel); ++asserts;
    }

    // 12. release() on a non-null wrapper hands the handle to the caller and
    //     leaves the wrapper null; the caller is then responsible (or, here,
    //     we drop it — cold-state, no leak observable).
    {
        auto* sentinel = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x4444ull));
        scoped_local_ref r{ sentinel };
        void* h = r.release();
        EXPECT(h == sentinel);      ++asserts;
        EXPECT(r.get() == nullptr); ++asserts;
        EXPECT(!r);                 ++asserts;
    }

    // 13. Loop test mirroring the live-JVM "100+ iterations past the 16-entry
    //     table" pattern but here, cold-state: every iteration default-
    //     constructs, then dtors.  Pins that the wrapper has no static or
    //     thread-local state that could leak across iterations.
    for (int i = 0; i < 256; ++i)
    {
        scoped_local_ref r{};
        EXPECT(r.get() == nullptr);
    }
    ++asserts; // counted once for the loop

    std::printf("[OK] jni_local_ref_hygiene_nojvm: %d runtime asserts, %d static_asserts\n",
                asserts, g_static_assert_count);
    return 0;
}
