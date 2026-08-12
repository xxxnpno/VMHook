#!/usr/bin/env python3
"""The last five macros, and what replaced each.

Run after demacro.py.  Together they leave a file whose only directives are the
`#include`s that belong in a module's global module fragment.

    <meta> guard          DEAD on this toolchain -- GCC 16.2 does not define
                          __cpp_impl_reflection, so the include never happened
                          and nothing in the file mentions std::meta.  Removed
                          rather than translated; it comes back as a real module
                          when the compiler implements the macro it tests.
    VMHOOK_LOG_FILE       a consumer `#define`d this before including, which a
                          module cannot see at all.  It becomes a RUNTIME path:
                          strictly more useful, since it can be set after the
                          fact, and one fewer thing decided at compile time.
    VMHOOK_DISABLE_AUTO_REPAIR    constexpr bool + `if constexpr`.
    VMHOOK_AUTO_REPAIR_INTERVAL_MS  constexpr milliseconds.
    VMHOOK_MAKE_VERSION / VMHOOK_CPLUSPLUS   nothing references them now.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

LOG_FILE_OLD = """#ifdef VMHOOK_LOG_FILE
            // Opened once on first use, kept open for the lifetime of the
            // process.  std::ios::app means every write is positioned at EOF,
            // so a crashed process can be re-run without truncating earlier
            // diagnostics.
            static std::ofstream log_file{ VMHOOK_LOG_FILE, std::ios::out | std::ios::app };
            if (log_file.is_open())
            {
                log_file << line << '\\n';
                log_file.flush();
                return;
            }
#endif"""

LOG_FILE_NEW = """            // RUNTIME, not a macro a consumer defines before including -- a
            // module cannot see one of those at all.  Opened once, on the first
            // line written after a path is set, and kept open for the lifetime
            // of the process.  std::ios::app means every write is positioned at
            // EOF, so a crashed process can be re-run without truncating the
            // diagnostics from the run that crashed.
            if (!vmhook::log_file_path.empty())
            {
                static std::ofstream file{ std::string{ vmhook::log_file_path },
                                           std::ios::out | std::ios::app };
                if (file.is_open())
                {
                    file << line << '\\n';
                    file.flush();
                    return;
                }
            }"""

INTERVAL_OLD = """            constexpr auto interval{ std::chrono::milliseconds{
#ifdef VMHOOK_AUTO_REPAIR_INTERVAL_MS
                VMHOOK_AUTO_REPAIR_INTERVAL_MS
#else
                1000
#endif
            } };"""

INTERVAL_NEW = """            constexpr auto interval{
                std::chrono::milliseconds{ vmhook::auto_repair_interval_ms } };"""

SETTINGS = """
    /*
        @brief Where the diagnostic trace is written, or empty for std::cout.
        @details
        Was a macro a consumer defined before including the header, which a
        module makes impossible -- and which was always a compile-time answer to
        a question that is better asked at run time.  Set it before the first
        line is written; the stream is opened once and kept.
    */
    inline std::string log_file_path{};

    /*
        @brief Whether the watchdog that re-installs displaced hooks runs.
        @details
        A hook can be overwritten by anything else that patches the same entry.
        The watchdog notices and puts it back.  `set_auto_repair_enabled(false)`
        is the run-time switch; this is the compile-time one, and turning it off
        removes the thread entirely.
    */
    inline constexpr bool auto_repair{ true };

    /* @brief How often the watchdog checks, in milliseconds. */
    inline constexpr int auto_repair_interval_ms{ 1000 };
"""


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    text = Path(sys.argv[1]).read_text(encoding="utf-8")

    # The dead <meta> guard.
    text = re.sub(r"^#if VMHOOK_CPLUSPLUS[^\n]*__has_include\(<meta>\)\s*\n"
                  r"\s*#include <meta>\s*\n#else\s*\n#endif\s*\n",
                  "", text, flags=re.M)

    # The two VMHOOK_CPLUSPLUS defines and the MAKE_VERSION one: unreferenced now.
    text = re.sub(r"^#if defined\(_MSVC_LANG\)\s*\n\s*#define VMHOOK_CPLUSPLUS[^\n]*\n"
                  r"#else\s*\n\s*#define VMHOOK_CPLUSPLUS[^\n]*\n#endif\s*\n",
                  "", text, flags=re.M)
    text = re.sub(r"^#define VMHOOK_MAKE_VERSION[^\n]*\n", "", text, flags=re.M)

    if LOG_FILE_OLD not in text:
        print("! the log-file block did not match", file=sys.stderr)
        return 1
    text = text.replace(LOG_FILE_OLD, LOG_FILE_NEW, 1)

    if INTERVAL_OLD not in text:
        print("! the interval block did not match", file=sys.stderr)
        return 1
    text = text.replace(INTERVAL_OLD, INTERVAL_NEW, 1)

    # #ifndef VMHOOK_DISABLE_AUTO_REPAIR ... #endif  ->  if constexpr
    text = re.sub(r"^#ifndef VMHOOK_DISABLE_AUTO_REPAIR\s*\n",
                  "            if constexpr (!vmhook::auto_repair) { return; }\n", text, flags=re.M)

    # Put the new settings into the version namespace demacro.py wrote.
    text = text.replace("    inline constexpr bool auto_attach_threads{ true };",
                        "    inline constexpr bool auto_attach_threads{ true };\n" + SETTINGS, 1)

    Path(sys.argv[2]).write_text(text, encoding="utf-8")
    left = sorted(set(re.findall(r"^\s*#\s*(\w+)", text, re.M)))
    print("directives left:", ", ".join(left))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
