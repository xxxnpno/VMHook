// Standalone (no-JVM) tests for detail::adapter_fingerprint.
//
// This function decides which methods may lend their c2i adapter to which.  A
// key that is too STRICT makes vmhook refuse to deoptimise (measured: 23 of 24
// signatures refused on a live JDK 26, because `()V` and `()Lsome/Class;` were
// treated as different when HotSpot gives them the same adapter).  A key that is
// too LOOSE is far worse: it writes another signature's adapter into a live
// Method and the VM dies at the next compiled call.
//
// So the rule is pinned here against HotSpot's own, from
// SharedRuntime::AdapterFingerPrint / adapter_encoding:
//   * ARGUMENTS ONLY -- the return type is not part of the fingerprint
//   * Z B S C I    -> int
//   * object array -> long   (one 64-bit word on LP64, same as J)
//   * J            -> long
//   * F            -> float
//   * D            -> double
//   * plus a leading receiver word for a non-static method
//
// Pure function, no JVM: every case below is exact.

#include <vmhook/vmhook.hpp>

#include <cstdio>
#include <string>

namespace
{
    int failures{ 0 };
    int checks{ 0 };

    auto check(const char* const name, const bool ok) -> void
    {
        ++checks;
        if (!ok)
        {
            ++failures;
            std::printf("[FAIL] %s\n", name);
        }
    }

    auto fp(const char* const descriptor, const bool is_static) -> std::string
    {
        return vmhook::detail::adapter_fingerprint(descriptor, is_static);
    }

    auto same(const char* const a, const bool a_static,
              const char* const b, const bool b_static) -> bool
    {
        const std::string ka{ fp(a, a_static) };
        return !ka.empty() && ka == fp(b, b_static);
    }
}

int main()
{
    std::printf("adapter_fingerprint no-JVM unit test\n");

    // ---- the receiver word -------------------------------------------------
    check("static_no_args_is_empty",        fp("()V", true).empty());
    check("instance_no_args_is_receiver",   fp("()V", false) == "L");
    check("static_and_instance_differ",     fp("()V", true) != fp("()V", false));

    // ---- the return type is NOT part of the key ---------------------------
    // This is the one that mattered: refusing these as different keys is what
    // made 23 of 24 real lookups fail.
    check("return_void_vs_object_same",
          same("()V", false, "()Lnet/minecraft/client/Minecraft;", false));
    check("return_void_vs_int_same",   same("()V", false, "()I", false));
    check("return_void_vs_array_same", same("()V", false, "()[B", false));
    check("return_type_ignored_with_args",
          same("(Z)V", false, "(Z)Ljava/lang/String;", false));

    // ---- integral promotion: Z B S C I all collapse to one class ----------
    check("bool_is_int",  same("(Z)V", false, "(I)V", false));
    check("byte_is_int",  same("(B)V", false, "(I)V", false));
    check("short_is_int", same("(S)V", false, "(I)V", false));
    check("char_is_int",  same("(C)V", false, "(I)V", false));
    check("runTick_key_is_receiver_plus_int", fp("(Z)V", false) == "LI");

    // ---- references and arrays are one long-sized word --------------------
    check("object_is_long_word", same("(Ljava/lang/Object;)V", false, "(J)V", false));
    check("array_is_long_word",  same("([I)V", false, "(J)V", false));
    check("nested_array_is_one_word",
          same("([[[Ljava/lang/String;)V", false, "(J)V", false));
    check("distinct_classes_same_key",
          same("(Ljava/lang/String;)V", false, "(Lfoo/Bar;)V", false));

    // ---- float and double keep their own classes --------------------------
    check("float_not_int",     !same("(F)V", false, "(I)V", false));
    check("double_not_long",   !same("(D)V", false, "(J)V", false));
    check("float_not_double",  !same("(F)V", false, "(D)V", false));

    // ---- arity and order are significant ----------------------------------
    check("arity_matters",  !same("(II)V", false, "(I)V", false));
    check("order_matters",  !same("(IF)V", false, "(FI)V", false));
    check("mixed_shape",    fp("(IJF[BLfoo/Bar;D)V", false) == "LILFLLD");

    // ---- a static method has no receiver word -----------------------------
    check("static_drops_receiver", fp("(I)V", true) == "I");
    check("static_vs_instance_one_arg",
          fp("(I)V", false) == "LI" && fp("(I)V", true) == "I");

    // ---- malformed input refuses rather than inventing a key --------------
    // An empty key is the signal to decline; it must never silently collapse
    // to "no arguments", which would alias every unparsable descriptor onto
    // the commonest adapter in the VM.
    check("no_parens_empty",        fp("V", false).empty());
    check("missing_close_empty",    fp("(IV", false).empty());
    check("reversed_parens_empty",  fp(")I(", false).empty());
    check("unterminated_object",    fp("(Ljava/lang/String", false).empty());
    check("object_past_close",      fp("(Ljava)V", false).empty());
    check("unknown_type_char",      fp("(Q)V", false).empty());
    check("empty_descriptor",       fp("", false).empty());
    check("array_with_no_element",  fp("([)V", false).empty());

    // A well-formed no-arg STATIC descriptor also yields "" -- same value, but
    // it is reached legitimately.  The caller cannot distinguish the two, so
    // the index must never be keyed on an empty string; assert the one shape
    // that would make that ambiguity dangerous is impossible for an instance
    // method, which is what every hook target is.
    check("instance_key_never_empty_when_valid", !fp("()V", false).empty());

    if (failures == 0)
    {
        std::printf("OK (%d checks)\n", checks);
        return 0;
    }
    std::printf("FAIL: %d of %d\n", failures, checks);
    return 1;
}
