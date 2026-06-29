# #28 no-SEH cold-read hardening plan — ALL 7 ORDERED FIXES LANDED

> **Status (2026-06-29):** All seven ordered fixes from the original audit are
> in master. This document is retained as a historical record of the campaign.
> The original auto-generated plan has been condensed to a status table; full
> historical text removed to prevent confusion with stale line citations.

## Status

The original audit identified 7 distinct un-hardened reads/writes that kept
windows-clang / windows-mingw quarantined. Each has been verified against
current source as LANDED:

| # | Site | Fix shape | Status |
|---|------|-----------|--------|
| 1 | `constant_pool::get_length()` raw int32 deref | `os::safe_read` cross-platform, -1 sentinel on bad page | DONE |
| 2 | `Method::_access_flags` RMW at teardown (shutdown_hooks + hook_handle::stop) | `safe_access_flags_and` sibling of `safe_access_flags_or` | DONE |
| 3 | `~midi2i_hook` raw `target[0]==JMP_OPCODE` probe | `os::safe_read(&first_byte, target, 1)` then compare | DONE |
| 4 | const_method `get_name`/`get_signature` terminal Symbol* slot | `cold_read_metadata_pointer` (cross-platform safe_read) | DONE |
| 5 | `_pool_holder` Klass* raw deref (call_jni + resolve_compatible_method) | `cold_read_metadata_pointer` at both sites | DONE |
| 6 | `method_proxy::is_static()` raw `_access_flags` deref | `safe_access_flags_test(0x0008u, found)` | DONE |
| 7 | `detect_adapter_offset_from_method` try_offset memcpy scan | `os::safe_read(&candidate, probe_bytes+offset, 8)` | DONE |

**Conclusion:** the no-SEH cold-read surface that was the #28 quarantine root
is closed. The clang+mingw windows quarantine was lifted at 0420c40 in a
separate fix campaign (PR #4) that addressed off-suite-thread fault sources
rather than the cold-read hardening itself.

## Adjacent hardening that landed under the same discipline (2026-06-29)

This session extended the safe_read pattern to neighbouring sites the original
audit didn't enumerate:

- `for_each_thread::get_os_thread_id`: `_osthread` + `_thread_id` reads via os::safe_read
- `klass_from_object_header` + `klass_from_oop` + `for_each_instance` scanner +
  `jni_make_unique` diagnostic: routed through `read_klass_from_header_buffer`
  which resolves `_compressed_klass` vs `_klass` via VMStructs (handles
  `-XX:-UseCompressedClassPointers`, previously decoded garbage)
- POSIX `sigaction` install_once: chains to the previously-installed HotSpot
  SIGSEGV/SIGBUS handlers instead of resetting to SIG_DFL (preserves implicit
  NPE / safepoint poll / stack-bang->SOE)
- `decode_klass_pointer`: rejects non-8-aligned decoded pointers (Metaspace
  always 8-byte-aligns Klass; a torn narrow-klass decoding to misaligned slipped
  past is_valid_pointer's heuristic)
- `g_hooked_methods`: reserved at static-init via inline lambda initializer;
  `hook_handle::stop` tombstones in place instead of `vector::erase` (which
  shifted std::function cells under the lock-free common_detour reader → UAF)
- `seh_invoke_detour`: __except filter narrowed to blacklist STACK_OVERFLOW
  (was catch-all, swallowing stack-overflow without guard-page reset = UB)

**Outstanding cold-read items:** none from the original audit. Adjacent
hardening continues in `[[library_fix_session_20260629]]`.
