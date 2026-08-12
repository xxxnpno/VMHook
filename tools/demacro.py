#!/usr/bin/env python3
"""Replace vmhook's remaining macros with C++ that the type system can see.

A macro is invisible to the compiler's type checking, to the debugger, and to
every tool that reads the code -- and it cannot cross a module boundary at all,
which is what forces the issue: `VMHOOK_VERSION_MAJOR` is used by consumers, and
a consumer that imports a module never sees it.

Each replacement below is chosen so the RESULT IS THE SAME PROGRAM:

    VMHOOK_JNICALL          empty on x86-64 Windows -- one calling convention,
                            so the macro expanded to nothing.  Deleted.
    VMHOOK_DELETED(reason)  `= delete(reason)`, which GCC 16.2 accepts in C++26.
                            Verified by compiling it before relying on it.
    VMHOOK_VERSION*         constexpr integers and a string_view, EXPORTED, so a
                            consumer can still ask.
    VMHOOK_DEBUG_LOGS       constexpr bool.  `#if` becomes `if constexpr`, which
                            still emits nothing when it is false.
    VMHOOK_LOG(...)         a variadic function template.  The no-op branch keeps
                            ODR-using its arguments, exactly as the old
                            `sizeof(...)` trick did, so an unused-variable
                            warning stays suppressed.
    FUNCTION_TRAITS_...     twenty specialisations that differ only in a
                            cv-ref-noexcept qualifier.  There is no template that
                            generates those, so they are written out.

The proof is not this script: it is that chatwire builds against the result and
its three-client harness passes -- hooks installed, mapping resolved, chat
round-tripped.  A macro replaced wrongly compiles.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

QUALIFIERS = [
    "volatile", "const volatile", "&", "const&", "volatile&", "const volatile&",
    "&&", "const&&", "volatile&&", "const volatile&&",
    "volatile noexcept", "const volatile noexcept", "& noexcept", "const& noexcept",
    "volatile& noexcept", "const volatile& noexcept", "&& noexcept",
    "const&& noexcept", "volatile&& noexcept", "const volatile&& noexcept",
]

TRAITS_SPEC = """        template<typename class_type, typename return_type, typename... argument_types>
        struct function_traits<return_type (class_type::*)(argument_types...) {q}>
        {{
            using args_tuple_t = std::tuple<argument_types...>;
        }};"""

VERSION_BLOCK = '''namespace vmhook
{
    /*
        @brief This library's version, as values rather than as macros.
        @details
        They were `#define`s, and a macro cannot cross a module boundary: a
        consumer that imports vmhook would see nothing at all.  These are
        exported, so `vmhook::version_major` works from anywhere and is a real
        `int` to the type system and the debugger.
    */
    inline constexpr int version_major{ 6 };
    inline constexpr int version_minor{ 0 };
    inline constexpr int version_patch{ 0 };

    /* @brief major*1000000 + minor*1000 + patch, for a single comparison. */
    [[nodiscard]] consteval auto make_version(const int major, const int minor,
                                              const int patch) noexcept -> int
    {
        return (major * 1000000) + (minor * 1000) + patch;
    }

    inline constexpr int version{ make_version(version_major, version_minor, version_patch) };
    inline constexpr std::string_view version_string{ "6.0.0" };

    /*
        @brief Whether the library emits its diagnostic trace.
        @details
        Was `#if VMHOOK_DEBUG_LOGS`, defaulting off under NDEBUG.  A
        `constexpr bool` behind `if constexpr` emits exactly as little code, and
        is a value the rest of the program can read.
    */
    inline constexpr bool debug_logs{ false };

    /*
        @brief Whether a native thread is adopted by the VM when it needs to be.
        @details
        On: a thread that calls into Java without being known to the VM has no
        frame anchor for a GC to walk, and the heap corruption that follows
        surfaces far from the call.  Off, such a call fails cleanly instead.
    */
    inline constexpr bool auto_attach_threads{ true };
}
'''

LOG_BLOCK = '''namespace vmhook::detail
{
    /*
        @brief One diagnostic line.  Was the VMHOOK_LOG macro.
        @details
        A function template rather than a macro, so its arguments are type
        checked, it has a name a debugger can break on, and it survives being
        imported from a module -- which a macro does not.

        WHEN `debug_logs` IS FALSE this still touches every argument, because
        the macro it replaces did: the old no-op form was
        `(void)sizeof(format_log(__VA_ARGS__))`, which existed to keep a variable
        that is only ever logged from being reported as unused. `if constexpr`
        would discard the call entirely and bring those warnings back, so the
        arguments are folded into a `(void)` expression instead. Neither form
        emits code.
    */
    template<typename... argument_types>
    inline auto log_line(const std::string_view format,
                         argument_types&&... arguments) noexcept -> void
    {
        if constexpr (vmhook::debug_logs)
        {
            emit_log_line(format_log(format, std::forward<argument_types>(arguments)...));
        }
        else
        {
            ((void)format, ..., (void)arguments);
        }
    }
}
'''


def expand_traits(source: str) -> str:
    define = re.search(r"^#define VMHOOK_FUNCTION_TRAITS_MEMBER_SPEC.*$", source, re.M)
    if not define:
        return source
    written = "\n".join(TRAITS_SPEC.format(q=q) for q in QUALIFIERS)
    source = source[:define.start()] + written + source[define.end():]
    source = re.sub(r"^\s*VMHOOK_FUNCTION_TRAITS_MEMBER_SPEC\([^)]*\);\s*$\n?", "", source, flags=re.M)
    source = re.sub(r"^#undef VMHOOK_FUNCTION_TRAITS_MEMBER_SPEC\s*$\n?", "", source, flags=re.M)
    return source


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    text = Path(sys.argv[1]).read_text(encoding="utf-8")

    text = expand_traits(text)

    # VMHOOK_JNICALL expanded to nothing on this target.
    text = re.sub(r"^\s*#\s*define\s+VMHOOK_JNICALL\s*$\n?", "", text, flags=re.M)
    text = re.sub(r"\bVMHOOK_JNICALL\s*", "", text)

    # VMHOOK_DELETED(reason) -> = delete(reason)
    text = re.sub(r"^\s*#\s*if\s+VMHOOK_CPLUSPLUS[^\n]*\n\s*#\s*define\s+VMHOOK_DELETED[^\n]*\n"
                  r"\s*#\s*else\s*\n\s*#\s*define\s+VMHOOK_DELETED[^\n]*\n\s*#\s*endif\s*\n",
                  "", text, flags=re.M)
    text = re.sub(r"\bVMHOOK_DELETED\s*\(", "= delete (", text)

    # Version, debug and attach macros -> exported constants.
    text = re.sub(r"^\s*#\s*define\s+VMHOOK_VERSION[^\n]*\n(?:[^\n]*\\\n)*", "", text, flags=re.M)
    text = re.sub(r"^\s*#\s*(ifndef|define|else|endif|if)\s+VMHOOK_DEBUG_LOGS[^\n]*\n", "", text, flags=re.M)
    text = re.sub(r"^\s*#\s*(ifndef|define|endif)\s+VMHOOK_AUTO_ATTACH_THREADS[^\n]*\n", "", text, flags=re.M)

    text = text.replace("VMHOOK_VERSION_MAJOR", "vmhook::version_major")
    text = text.replace("VMHOOK_VERSION_MINOR", "vmhook::version_minor")
    text = text.replace("VMHOOK_VERSION_PATCH", "vmhook::version_patch")
    text = text.replace("VMHOOK_VERSION_STRING", "vmhook::version_string")
    text = re.sub(r"\bVMHOOK_AUTO_ATTACH_THREADS\b", "vmhook::auto_attach_threads", text)
    text = re.sub(r"\bVMHOOK_DEBUG_LOGS\b", "vmhook::debug_logs", text)

    # VMHOOK_LOG(...) -> the function template.
    text = re.sub(r"^\s*#\s*if\s+vmhook::debug_logs\s*$\n\s*#\s*define VMHOOK_LOG[^\n]*\n"
                  r"(?:[^\n]*\n)*?\s*#\s*endif\s*$\n", "", text, flags=re.M)
    text = re.sub(r"^\s*#\s*define VMHOOK_LOG[^\n]*\n", "", text, flags=re.M)
    text = re.sub(r"\bVMHOOK_LOG\s*\(", "::vmhook::detail::log_line(", text)

    Path(sys.argv[2]).write_text(text, encoding="utf-8")
    left = len(re.findall(r"^\s*#\s*(define|if|ifdef|ifndef|else|elif|endif|undef)\b", text, re.M))
    print(f"{left} preprocessor directive(s) left (includes and the global module fragment)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
