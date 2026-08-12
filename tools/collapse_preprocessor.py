#!/usr/bin/env python3
"""Collapse vmhook's portability preprocessor down to one toolchain.

vmhook was written to build on MSVC, clang and gcc, on Windows, Linux, macOS,
iOS and Android, on x86-64 and aarch64.  It is not any more: the project targets
the newest g++ on Windows x86-64 and nothing else.  Every `#if` asking which
compiler or which OS this is now has one answer, and the branches that answer
differently are dead code that still has to be read.

This resolves them.  It knows the value of the platform and feature symbols for
GCC 16 on Windows x86-64, evaluates the directives that depend only on those,
and deletes the branches that cannot be taken.

IT REFUSES TO GUESS.  A condition mentioning a symbol this does not know is left
exactly as it was, nested block and all.  Silently deleting a branch because the
evaluator did not understand it is the one failure mode that would be worse than
not running at all -- the result would still compile.

WHAT MAKES THE RESULT TRUSTWORTHY is not this script.  It is that `g++ -E` on the
original and on the output produce the same translation unit; see `--verify` in
the README of this directory.  The script is a convenience, the diff is the
proof.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

#: What is true for GCC 16 on Windows x86-64.  Anything not named here makes a
#: condition unevaluable, which leaves it untouched.
KNOWN: dict[str, int] = {
    # Compiler
    "__GNUC__": 16, "__GNUC_MINOR__": 2, "__clang__": 0, "_MSC_VER": 0,
    "__EDG__": 0, "__INTEL_COMPILER": 0,
    # OS
    "_WIN32": 1, "_WIN64": 1, "__ANDROID__": 0, "__APPLE__": 0, "__linux__": 0,
    "__unix__": 0, "TARGET_OS_IPHONE": 0,
    # Architecture
    "__x86_64__": 1, "_M_X64": 1, "__aarch64__": 0, "_M_ARM64": 0,
    "__i386__": 0, "_M_IX86": 0,
    # vmhook's own derived flags
    "VMHOOK_OS_WINDOWS": 1, "VMHOOK_OS_LINUX": 0, "VMHOOK_OS_MACOS": 0,
    "VMHOOK_OS_IOS": 0, "VMHOOK_OS_ANDROID": 0,
    "VMHOOK_ARCH_X86_64": 1, "VMHOOK_ARCH_ARM64": 0,
    "VMHOOK_COMPILER_GCC": 1, "VMHOOK_COMPILER_CLANG": 0, "VMHOOK_COMPILER_MSVC": 0,
    # MEASURED, not assumed.  Every one of these came out of
    #     g++ -std=c++26 -freflection -E   on a file that includes vmhook.hpp
    # and four of them are not what a reasonable person would guess: GCC 16.2
    # reports __cplusplus as 202400L for -std=c++26, and vmhook's own probes for
    # std::expected, deducing-this and reflection all currently come out FALSE.
    # Guessing them produced a header that compiled and had a different body,
    # which is the failure this whole file is arranged to make impossible.
    "VMHOOK_HAS_STD_FORMAT": 1, "VMHOOK_HAS_STD_PRINT": 1,
    "VMHOOK_HAS_STD_EXPECTED": 0, "VMHOOK_HAS_DEDUCING_THIS": 0,
    "VMHOOK_HAS_REFLECTION": 0, "VMHOOK_HAS_HW_DATA_BREAKPOINTS": 1,
    "VMHOOK_RUNTIME_HOOKING_AVAILABLE": 1,
    "VMHOOK_CPLUSPLUS": 202400,
    "__cplusplus": 202400,
    # Second pass, after the macros became values: the feature probes vmhook
    # runs on itself, measured the same way as the rest.
    "__cpp_lib_print": 202207, "__cpp_explicit_this_parameter": 0,
    "__cpp_lib_expected": 0, "VMHOOK_OS_APPLE": 0, "VMHOOK_OS_POSIX": 0,
    "NDEBUG": 1, "VMHOOK_DISABLE_AUTO_REPAIR": 0, "VMHOOK_LOG_FILE": 0,
    "__clang_major__": 0,

    # NOT here, deliberately: VMHOOK_DEBUG_LOGS and VMHOOK_LOG_FILE.  They are
    # set by the BUILD (NDEBUG, or the consumer defining them before including),
    # so they have no single value to collapse to and are left as directives.
}

#: Derived flags whose own `#define` lines go once every use of them is resolved.
DERIVED = {
    "VMHOOK_OS_WINDOWS", "VMHOOK_OS_LINUX", "VMHOOK_OS_MACOS", "VMHOOK_OS_IOS",
    "VMHOOK_OS_ANDROID", "VMHOOK_OS_POSIX", "VMHOOK_OS_APPLE",
    "VMHOOK_ARCH_X86_64", "VMHOOK_ARCH_ARM64",
    "VMHOOK_COMPILER_GCC", "VMHOOK_COMPILER_CLANG", "VMHOOK_COMPILER_MSVC",
    "VMHOOK_HAS_STD_FORMAT", "VMHOOK_HAS_STD_PRINT", "VMHOOK_HAS_STD_EXPECTED",
    "VMHOOK_HAS_DEDUCING_THIS",
    # NOT VMHOOK_CPLUSPLUS: conditionals this script cannot evaluate still
    # test it, and a dropped define reads as 0 -- which quietly took the
    # other branch of every = delete(reason) in the library.
    "VMHOOK_HAS_REFLECTION", "VMHOOK_HAS_HW_DATA_BREAKPOINTS",
    "VMHOOK_RUNTIME_HOOKING_AVAILABLE",
}

DIRECTIVE = re.compile(r"^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)$")
HAS_INCLUDE = re.compile(r"__has_include\s*\(\s*<[^>]+>\s*\)")
IDENTIFIER = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")


class Unevaluable(Exception):
    """The condition names something this script has no business assuming."""


def evaluate(condition: str) -> bool:
    """True/False for a condition built only from KNOWN symbols."""
    text = condition.strip()

    # Every header vmhook probes for exists in GCC 16's libstdc++; the ones it
    # does not probe for are not mentioned.
    text = HAS_INCLUDE.sub("1", text)
    # Integer suffixes: 202207L is a number, and the L is not an identifier this
    # should be asked about.  Without this every probe against a feature-test
    # macro came out unevaluable and was left in place.
    text = re.sub(r"(\d+)[uUlL]+", lambda m: m.group(1), text)
    text = re.sub(r"defined\s*\(\s*([A-Za-z_]\w*)\s*\)", r"__defined_\1", text)
    text = re.sub(r"defined\s+([A-Za-z_]\w*)", r"__defined_\1", text)

    def substitute(match: re.Match[str]) -> str:
        name = match.group(0)
        if name.startswith("__defined_"):
            symbol = name[len("__defined_"):]
            if symbol not in KNOWN:
                raise Unevaluable(symbol)
            return "1" if KNOWN[symbol] else "0"
        if name in ("true", "false"):
            return "1" if name == "true" else "0"
        if name not in KNOWN:
            raise Unevaluable(name)
        return str(KNOWN[name])

    text = IDENTIFIER.sub(substitute, text)
    text = text.replace("&&", " and ").replace("||", " or ").replace("!", " not ")
    text = re.sub(r"\bnot=", "!=", text)                 # undo `!=` broken above
    try:
        return bool(eval(text, {"__builtins__": {}}, {}))   # noqa: S307
    except Exception as e:                                   # noqa: BLE001
        raise Unevaluable(text) from e


def logical_lines(source: str) -> list[str]:
    """Join backslash continuations, so a directive is ONE entry.

    A `#if` whose condition spans continuation lines is a single directive to
    the preprocessor and was three separate lines to this script, which then
    evaluated a TRUNCATED condition -- and a truncated condition can evaluate
    cleanly and wrongly.  That is how `#if A \ && defined(B)` became `#if A`,
    took the wrong branch, and produced a header that compiled.
    """
    out: list[str] = []
    pending = ""
    for line in source.splitlines(keepends=True):
        stripped = line.rstrip(chr(13) + chr(10))
        if stripped.endswith("\\"):
            pending += stripped[:-1]
            continue
        out.append(pending + line)
        pending = ""
    if pending:
        out.append(pending)
    return out


def collapse(source: str) -> tuple[str, int, int]:
    """Return (collapsed source, blocks resolved, blocks left alone)."""
    lines = logical_lines(source)
    out: list[str] = []
    resolved = 0
    kept = 0

    # Each open conditional: (evaluable, taken_so_far, emitting)
    stack: list[tuple[bool, bool, bool]] = []

    def emitting() -> bool:
        return all(frame[2] for frame in stack)

    for line in lines:
        m = DIRECTIVE.match(line)
        if not m:
            if emitting():
                out.append(line)
            continue

        kind, rest = m.group(1), m.group(2)

        if kind in ("if", "ifdef", "ifndef"):
            condition = rest
            if kind == "ifdef":
                condition = f"defined({rest.strip()})"
            elif kind == "ifndef":
                condition = f"!defined({rest.strip()})"
            try:
                value = evaluate(condition)
            except Unevaluable:
                kept += 1
                stack.append((False, False, emitting()))
                if emitting() or True:
                    out.append(line)
                continue
            resolved += 1
            stack.append((True, value, emitting() and value))

        elif kind == "elif":
            if not stack:
                out.append(line)
                continue
            evaluable, taken, _ = stack[-1]
            if not evaluable:
                out.append(line)
                continue
            try:
                value = (not taken) and evaluate(rest)
            except Unevaluable:
                # An unevaluable elif inside an evaluated block: keep the whole
                # thing rather than half of it.
                out.append(line)
                continue
            outer = all(f[2] for f in stack[:-1])
            stack[-1] = (True, taken or value, outer and value)

        elif kind == "else":
            if not stack:
                out.append(line)
                continue
            evaluable, taken, _ = stack[-1]
            if not evaluable:
                out.append(line)
                continue
            outer = all(f[2] for f in stack[:-1])
            stack[-1] = (True, True, outer and not taken)

        elif kind == "endif":
            if not stack:
                out.append(line)
                continue
            evaluable, _, _ = stack.pop()
            if not evaluable:
                out.append(line)

    return "".join(out), resolved, kept


def drop_derived_defines(source: str) -> str:
    """Remove `#define`s for flags NOTHING REFERENCES ANY MORE.

    The "nothing references" half is the whole of it.  Dropping
    `#define VMHOOK_CPLUSPLUS __cplusplus` while one surviving `#if` still tested
    it made that test read 0, which silently took the other branch: every
    `= delete("reason")` in the library became a bare `= delete`.  It compiled,
    and the diff against `g++ -E` on the original is the only thing that noticed.
    """
    lines = source.splitlines(keepends=True)
    out: list[str] = []
    for line in lines:
        m = re.match(r"\s*#\s*define\s+([A-Za-z_]\w*)", line)
        if not m or m.group(1) not in DERIVED:
            out.append(line)
            continue
        name = m.group(1)
        # Every other line that names it -- a directive, or code.
        uses = sum(1 for other in lines
                   if other is not line and re.search(rf"{re.escape(name)}", other))
        if uses == 0:
            continue
        out.append(line)
    return "".join(out)


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        print("usage: collapse_preprocessor.py <in.hpp> <out.hpp>")
        return 2

    source = Path(sys.argv[1]).read_text(encoding="utf-8")
    collapsed, resolved, kept = collapse(source)
    collapsed = drop_derived_defines(collapsed)

    before = len(source.splitlines())
    after = len(collapsed.splitlines())
    Path(sys.argv[2]).write_text(collapsed, encoding="utf-8")
    print(f"{resolved} conditional block(s) resolved, {kept} left alone "
          f"(a symbol this script does not know)")
    print(f"{before} -> {after} lines")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
