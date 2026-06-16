---
slug: read_java_string
title: Read Java String
category: jni
status: in_progress
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/in_progress, risk/medium, category/jni, tag/jni, tag/string, tag/utf-16, tag/latin1, tag/coder, tag/compact-strings, tag/safe-read]
---

# Read Java String

> **Category:** [[categories/jni|JNI plumbing (arg packing / refs / java values)]]  ·  **Status:** `in_progress`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/read_java_string-specialist.md`

## Description

`vmhook::read_java_string(void* string_oop)` takes a decoded heap OOP
pointing at a live `java.lang.String` instance and returns its contents
as a UTF-8 `std::string`, or an empty string on any failure.  It is the
shared decode core for every higher-level String reader in the library:
the `field_proxy` String getter, the array/collection/map element readers,
and the method-return decoder all funnel through this one function.

The function handles both JDK 8 (`char[]` value, always UTF-16, no `coder`
field) and JDK 9+ compact-string layouts (`byte[]` value + a `coder` byte:
LATIN1=0 or UTF16=1).  It dispatches on the presence of the `coder` field
in VMStructs, reads the `value` compressed OOP and the `coder` byte via
`os::safe_read` (never a raw dereference), decodes the backing-array header
at `+12` (length) and body at `+16`, then UTF-8-encodes each code unit via
an `append_utf8` lambda — including proper surrogate-pair combination for
astral code points.

Every cross-page dereference goes through `os::safe_read`
(ReadProcessMemory / process_vm_readv), so a GC-relocated String degrades
to `""` rather than faulting the JVM on no-SEH toolchains.  Null / invalid
OOP, zero backing-array pointer, length out of range, and safe_read failure
all return the empty string silently (no crash, no log for the zero-pointer
case).  The character-count ceiling is `read_java_string_max_units`
(16 Mi chars, uniform across all three layouts), applied to the *decoded*
char count so LATIN1, UTF-16, and JDK-8 strings share the same effective
limit.

## Depends on

- [[features/compressed_oops_decode|compressed_oops_decode]]
- [[features/decode_oop_and_pointers|decode_oop_and_pointers]]
- [[features/array_element_helpers|array_element_helpers]]

## Related

- [[features/field_string|field_string]]
- [[features/make_java_string|make_java_string]]

## Depended on by

- [[features/field_string|field_string]]
- [[features/method_call_string|method_call_string]]

## Implementation anchors

- `vmhook::read_java_string_max_units` — `vmhook/ext/vmhook/vmhook.hpp:1668-1668` — 16 Mi char ceiling constant (raised from the old hard 4096 cap, bug #29 fix)
- `vmhook::read_java_string (forward declaration)` — `vmhook/ext/vmhook/vmhook.hpp:1670-1671` — forward declaration; full definition at line 20010
- `vmhook::read_java_string (definition)` — `vmhook/ext/vmhook/vmhook.hpp:20010-20261` — full implementation: null guard, find_class, value safe_read, decode_oop_pointer, +12 length safe_read, char_count ceiling, body safe_read, append_utf8, utf16_to_utf8, JDK8/LATIN1/UTF16 dispatch
- `vmhook::hotspot::is_valid_pointer` — `vmhook/ext/vmhook/vmhook.hpp:2018-2055` — heuristic pointer guard (range + alignment + sentinel); NOT a mapped-memory check — mitigated here by safe_read at every subsequent dereference
- `detail::append_utf8 (lambda inside read_java_string)` — `vmhook/ext/vmhook/vmhook.hpp:20187-20211` — 1-4 byte UTF-8 encoder; replaced the old lossy non-ASCII -> '?' substitution
- `detail::utf16_to_utf8 (lambda inside read_java_string)` — `vmhook/ext/vmhook/vmhook.hpp:20215-20233` — surrogate-pair combining decoder; reads native-endian uint16_t units

## Tests

- `tests/jvm/modules/read_java_string.cpp`

## Known bugs

- **[low]** Coder dispatch uses a bare `else` for the UTF-16 branch (vmhook.hpp:20255) instead of an explicit `else if (coder == 1)`.  Any future or unknown coder value is silently decoded as UTF-16 (reading length/2 char units) rather than refused with a diagnostic.  Low risk: HotSpot defines only LATIN1=0 and UTF16=1 today.
- **[low]** UTF-16 code units are read via `reinterpret_cast<const std::uint16_t*>(data)` at vmhook.hpp:20244 and 20258, using the host's native byte order.  This is correct for x86-64 little-endian HotSpot (the only supported target) but there is no static_assert pinning that assumption, making the helper non-portable to a big-endian host.
- **[medium]** Previously (robustness bug #29): strings longer than 4096 characters decoded to the empty string instead of being truncated as documented — and the asymmetric raw-byte ceiling capped UTF-16 strings at 2048 logical chars while LATIN1/JDK-8 allowed 4096.  Fixed at vmhook.hpp:1668 and 20104/20147: the ceiling is now `read_java_string_max_units` (16 Mi) applied uniformly to the decoded char count across all three layouts. Noted here as a cross-reference for callers that tested against the old 4096-char boundary.

## Notes

JDK-version sensitivity: the `has_coder` branch (presence of the `coder`
field in VMStructs) is the sole layout selector.  JDK 8 char[] is always
UTF-16 (char_count == array length).  JDK 9+ byte[] carries the coder byte
at an offset resolved through VMStructs, so the decode works even on a
locked-down runtime where Java reflection on `String.coder` is denied.

The arrayOop header convention (+12 = length, +16 = first element) is shared
with every sibling array reader in the library and holds only under
UseCompressedClassPointers (the default for heaps below ~32 GB).
Running with -XX:-UseCompressedClassPointers shifts the header layout;
this is a global precondition the library does not guard against.

field_string.yaml depends on this feature (read_java_string is the decode
core for field_proxy's String getter at vmhook.hpp:7486).  Bugs found
here propagate to the array element reader (14628), list/collection reader
(14990), map key/value readers (15135 / 16035 / 16104), and the
method-return decoder (17078).

The `is_valid_pointer` first-gate concern (LIBRARY_BUGS.md api_surface_no_jvm
entry) is substantially mitigated in this function: every subsequent
dereference — value field, coder field, +12 length, +16 body — goes through
os::safe_read, so a stale-but-heuristically-valid OOP degrades to ""
rather than faulting.  The residual exposure is the initial `is_valid_pointer`
call at 20013 being a necessary but not sufficient readability check.
