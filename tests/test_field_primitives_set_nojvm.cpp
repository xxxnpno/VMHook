// Standalone unit test: field_proxy::set() primitive-set angle, no JVM.
//
// Wave-30 LEDGER GAPS this file closes:
//   - cold-state set<T>() on a NULL field_pointer is a no-op (no crash) across
//     every JVM primitive width (Z/B/S/C/I/J/F/D), in BOTH static and instance
//     dispatch flavours;
//   - idempotency: repeated set() of the SAME value on a real backing slot
//     leaves the slot at the value (last-write-wins also covered) and never
//     spills into the surrounding sentinel bytes;
//   - static_asserts: field_proxy::set<T> returns void and is invocable for
//     every primitive width, plus the noexcept signature of the width oracle
//     vmhook::detail::jvm_primitive_byte_width.
//
// All assertions here are HARD (deterministic): no JVM is involved, no
// floating-point register routing, and the only memory we touch is a stack
// canvas with sentinel bytes around an 8-byte slot.  Cross-platform (Linux,
// macOS, Windows MSVC/clang/mingw) — no libc++/int64_t/-Werror traps.

#include <vmhook/vmhook.hpp>
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <array>
#include <type_traits>

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

namespace
{
    constexpr std::size_t k_lead{ 8 };
    constexpr std::size_t k_slot{ 8 };
    constexpr std::size_t k_trail{ 16 };
    constexpr std::uint8_t k_sentinel{ 0xCD };

    struct canvas
    {
        std::array<std::uint8_t, k_lead + k_slot + k_trail> bytes{};
        canvas() { bytes.fill(k_sentinel); }
        auto field_ptr() -> void* { return bytes.data() + k_lead; }
        auto sentinels_intact() const -> bool
        {
            for (std::size_t i{ 0 }; i < bytes.size(); ++i)
            {
                if (i >= k_lead && i < k_lead + k_slot) { continue; }
                if (bytes[i] != k_sentinel) { return false; }
            }
            return true;
        }
    };

    // Drive set() N times with the same value, check the slot still holds it
    // bit-exact and the sentinels never moved.  Idempotency: writing the same
    // value twice is indistinguishable from writing it once.
    template<typename T>
    auto idempotent_set(const char* sig, T value, int times) -> bool
    {
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), sig, false };
        for (int i{ 0 }; i < times; ++i) { proxy.set(value); }
        T read{};
        std::memcpy(&read, c.field_ptr(), sizeof(read));
        std::uint8_t vbytes[sizeof(T)]{};
        std::memcpy(vbytes, &value, sizeof(T));
        std::uint8_t rbytes[sizeof(T)]{};
        std::memcpy(rbytes, &read, sizeof(T));
        for (std::size_t i{ 0 }; i < sizeof(T); ++i)
        {
            if (rbytes[i] != vbytes[i]) { return false; }
        }
        return c.sentinels_intact();
    }
}

// =====================================================================
// STATIC_ASSERTS — pure-trait pins on the set() signature.
// =====================================================================

// set<T> returns void for every primitive value type.
static_assert(std::is_same_v<
    decltype(std::declval<vmhook::field_proxy&>().set(std::declval<bool>())),
    void>, "field_proxy::set<bool> must return void");
static_assert(std::is_same_v<
    decltype(std::declval<vmhook::field_proxy&>().set(std::declval<std::int8_t>())),
    void>, "field_proxy::set<int8_t> must return void");
static_assert(std::is_same_v<
    decltype(std::declval<vmhook::field_proxy&>().set(std::declval<std::int16_t>())),
    void>, "field_proxy::set<int16_t> must return void");
static_assert(std::is_same_v<
    decltype(std::declval<vmhook::field_proxy&>().set(std::declval<char16_t>())),
    void>, "field_proxy::set<char16_t> must return void");
static_assert(std::is_same_v<
    decltype(std::declval<vmhook::field_proxy&>().set(std::declval<std::int32_t>())),
    void>, "field_proxy::set<int32_t> must return void");
static_assert(std::is_same_v<
    decltype(std::declval<vmhook::field_proxy&>().set(std::declval<std::int64_t>())),
    void>, "field_proxy::set<int64_t> must return void");
static_assert(std::is_same_v<
    decltype(std::declval<vmhook::field_proxy&>().set(std::declval<float>())),
    void>, "field_proxy::set<float> must return void");
static_assert(std::is_same_v<
    decltype(std::declval<vmhook::field_proxy&>().set(std::declval<double>())),
    void>, "field_proxy::set<double> must return void");

// The width oracle is constexpr-noexcept and consults a string_view; both
// the oracle and its signature byte are pinned compile-time-evaluated, so a
// future change to runtime-dispatch / throwing-style is caught here.
static_assert(noexcept(vmhook::detail::jvm_primitive_byte_width("I")),
    "jvm_primitive_byte_width must be noexcept");

int main()
{
    // ==================================================================
    // SECTION A — cold-state null-pointer set() across EVERY primitive
    // width, for both static (true) and instance (false) flavours.
    // The proxy is constructed with a null backing pointer; reaching the
    // assertion below WITHOUT an access violation IS the proof.
    // ==================================================================
    for (bool is_static : { false, true })
    {
        const char* tag{ is_static ? "_static" : "_instance" };
        char name[64]{};

        // Z
        {
            vmhook::field_proxy proxy{ nullptr, "Z", is_static };
            proxy.set(true);
            proxy.set(false);
            std::snprintf(name, sizeof(name), "null_set_Z%s", tag);
            check(name, true);
        }
        // B
        {
            vmhook::field_proxy proxy{ nullptr, "B", is_static };
            proxy.set(std::int8_t{ 0x7F });
            proxy.set(std::int8_t{ -1 });
            std::snprintf(name, sizeof(name), "null_set_B%s", tag);
            check(name, true);
        }
        // S
        {
            vmhook::field_proxy proxy{ nullptr, "S", is_static };
            proxy.set(std::int16_t{ 0x1234 });
            std::snprintf(name, sizeof(name), "null_set_S%s", tag);
            check(name, true);
        }
        // C — both the widening shortcut (1-byte value) and the verbatim path
        {
            vmhook::field_proxy proxy{ nullptr, "C", is_static };
            proxy.set(char{ 'A' });               // widening shortcut path
            proxy.set(char16_t{ 0x20AC });        // verbatim 2-byte path
            std::snprintf(name, sizeof(name), "null_set_C%s", tag);
            check(name, true);
        }
        // I
        {
            vmhook::field_proxy proxy{ nullptr, "I", is_static };
            proxy.set(std::int32_t{ 0x0BADF00D });
            std::snprintf(name, sizeof(name), "null_set_I%s", tag);
            check(name, true);
        }
        // J
        {
            vmhook::field_proxy proxy{ nullptr, "J", is_static };
            proxy.set(std::int64_t{ 0x0123456789ABCDEFll });
            std::snprintf(name, sizeof(name), "null_set_J%s", tag);
            check(name, true);
        }
        // F
        {
            vmhook::field_proxy proxy{ nullptr, "F", is_static };
            proxy.set(float{ 3.5F });
            std::snprintf(name, sizeof(name), "null_set_F%s", tag);
            check(name, true);
        }
        // D
        {
            vmhook::field_proxy proxy{ nullptr, "D", is_static };
            proxy.set(double{ 2.25 });
            std::snprintf(name, sizeof(name), "null_set_D%s", tag);
            check(name, true);
        }

        // Also cold-state with a too-wide value: the null-pointer early-return
        // fires before the width guard, so still a clean no-op.
        {
            vmhook::field_proxy proxy{ nullptr, "Z", is_static };
            proxy.set(std::int64_t{ -1 });
            std::snprintf(name, sizeof(name), "null_set_Z%s_oversized", tag);
            check(name, true);
        }
    }

    // ==================================================================
    // SECTION B — IDEMPOTENCY: writing the same value twice is the same
    // as writing it once, and the sentinels never move.  HARD on every
    // primitive width.
    // ==================================================================
    check("idem_Z_true_x3",  idempotent_set("Z", std::uint8_t{ 0x01 }, 3));
    check("idem_B_x3",       idempotent_set("B", std::int8_t{ -7 }, 3));
    check("idem_S_x3",       idempotent_set("S", std::int16_t{ 0x1234 }, 3));
    check("idem_C_x3",       idempotent_set("C", std::uint16_t{ 0x4E2D }, 3));
    check("idem_I_x3",       idempotent_set("I", std::int32_t{ 0x0BADF00D }, 3));
    check("idem_J_x3",       idempotent_set("J", std::int64_t{ 0x0123456789ABCDEFll }, 3));
    check("idem_F_x3",       idempotent_set("F", float{ 3.5F }, 3));
    check("idem_D_x3",       idempotent_set("D", double{ 2.25 }, 3));

    // Many writes (16x) — no accumulation / OR'ing behaviour, the slot just
    // holds the final value exactly.
    check("idem_I_x16",      idempotent_set("I", std::int32_t{ 0x7FFFFFFF }, 16));
    check("idem_J_x16",      idempotent_set("J", std::int64_t{ -1 }, 16));

    // Last-write-wins: a fresh value completely overwrites a previous one (no
    // OR / accumulate).  Written natively to the canvas slot and read back as
    // bytes to prove no stale high bits.
    {
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "I", false };
        proxy.set(std::int32_t{ -1 });               // 0xFFFFFFFF
        proxy.set(std::int32_t{ 0x00000001 });       // must fully overwrite
        std::int32_t read{};
        std::memcpy(&read, c.field_ptr(), sizeof(read));
        check("last_write_wins_I_overwrites_high_bits", read == std::int32_t{ 1 });
        check("last_write_wins_I_keeps_sentinels", c.sentinels_intact());
    }
    {
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "J", false };
        proxy.set(std::int64_t{ -1 });
        proxy.set(std::int64_t{ 0 });
        std::int64_t read{};
        std::memcpy(&read, c.field_ptr(), sizeof(read));
        check("last_write_wins_J_overwrites_high_bits", read == std::int64_t{ 0 });
        check("last_write_wins_J_keeps_sentinels", c.sentinels_intact());
    }
    {
        // Float -> different float fully replaces (no FP-OR fantasy)
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "F", false };
        proxy.set(float{ 1.5F });
        proxy.set(float{ -0.25F });
        float read{};
        std::memcpy(&read, c.field_ptr(), sizeof(read));
        check("last_write_wins_F_overwrites", read == -0.25F);
        check("last_write_wins_F_keeps_sentinels", c.sentinels_intact());
    }

    if (failures == 0) { std::printf("[OK] all field_primitives_set checks passed\n"); }
    else                { std::printf("[FAIL] %d field_primitives_set failures\n", failures); }
    return failures == 0 ? 0 : 1;
}
