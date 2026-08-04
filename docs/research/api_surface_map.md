# VMHook — Public API Surface Map & Raw-OOP Boundary Audit

**Target:** `vmhook/ext/vmhook/vmhook.hpp` (19 824 lines, C++23, single header, header-only)
**Version:** `VMHOOK_VERSION 0.5.3` (macros at lines 68–70)
**Audit date:** 2026-08-04 — READ-ONLY. No source file was modified.
**Purpose:** establish the exact existing object model and every raw-oop crossing of the public
API boundary, as input to designing a library-managed Java-object handle/reference model.

All line numbers refer to `vmhook/ext/vmhook/vmhook.hpp` unless a different file is named.

---

## 0. Executive framing for the handle design

Three facts dominate everything below.

1. **There is no lifetime primitive today.** `vmhook::jni::global_ref` (19725) *looks* like a pin
   but its documented body is a **no-op**: it stores the raw `void*` captured at construction and
   `oop()` returns it verbatim (19775–19781). The de-JNI effort removed `NewGlobalRef`; the class
   survived as an API-compatible shell. So `pin()` (19803, 19815) currently buys nothing but a
   move-only wrapper. This is the single largest hole a real handle model must fill.

2. **`oop_t` is literally `void*`.** `using oop_type_t = void*;` (15924), `using oop_t = oop_type_t;`
   (15933). Every wrapper, every collection, every field read, every frame argument traffics in it.
   Because it is an alias, not a distinct type, a handle type cannot be introduced by changing the
   alias — every constructor taking `oop_t` would need an overload or the alias must become a class
   with an implicit `void*` conversion (which would break `static_cast<void*>` disambiguation in
   `cast_for_variant`, see §6).

3. **The validity contract is "duration of the current hook invocation".** Stated verbatim in the
   `oop_type_t` doc block (15919–15922) and repeated on `object_base` (16000-ish note),
   `thread_info` (8905–8911), and `for_each_instance` (9059–9062). Nothing enforces it.

---

## 1. Public API surface map

Legend: **[USER]** = intended end-user API · **[ADV]** = public but advanced/escape-hatch ·
**[INT]** = internal (namespace `detail`, or public-but-documented-as-implementation).

### 1.1 Diagnostics / errors — `namespace vmhook`, 401–443

| Line | Symbol | Kind | Purpose |
|---|---|---|---|
| 328–389 | `vmhook::detail::format_log`, `emit_log_line` | [INT] | mutex-serialised log emit; `std::vformat` when available, raw fmt string otherwise |
| 391–399 | `VMHOOK_LOG(...)` macro | [USER] | the library's only error channel; no-op unless `VMHOOK_DEBUG_LOGS` |
| 408–410 | `error_tag`, `warning_tag`, `info_tag` | [USER] | `inline constexpr std::string_view` log prefixes |
| 418–435 | `class exception final : std::exception` | [INT] | thrown internally only; never escapes a public function |

Feature macros: `VMHOOK_ARCH_X86_64` (174), `VMHOOK_HAS_STD_FORMAT` (219), `VMHOOK_HAS_DEDUCING_THIS`
(259), `VMHOOK_DEBUG_LOGS` (303), `VMHOOK_HAS_HW_DATA_BREAKPOINTS` (1234), `VMHOOK_DISABLE_AUTO_REPAIR`
(11598), `VMHOOK_LOG_FILE` (373).

### 1.2 OS abstraction — `namespace vmhook::os`, 444–1326  **[ADV]**

| Line | Signature | Purpose |
|---|---|---|
| 447/450 | `using module_handle = ::HMODULE` / `void*` | opaque module handle |
| 448/451 | `using thread_id_t = ::DWORD` / `std::uint64_t` | OS thread id |
| 457 | `enum class memory_protection : std::uint32_t` | portable page protections |
| 472 | `struct region_info { void* base; std::size_t size; bool committed/free/readable/executable/guarded; }` | `query_region` result |
| 486 | `auto page_size() noexcept -> std::size_t` | |
| 501 | `auto allocation_granularity() noexcept -> std::size_t` | |
| 529 | `auto find_loaded_module(const char*) noexcept -> module_handle` | |
| 553 | `auto find_jvm_module() noexcept -> module_handle` | jvm.dll / libjvm.so / libjvm.dylib |
| 581 | `auto get_proc_address(module_handle, const char*) noexcept -> void*` | |
| 601 | `auto current_thread_id() noexcept -> thread_id_t` | |
| 648 | `auto protect(void*, std::size_t, memory_protection, std::uint32_t* old = nullptr) noexcept -> bool` | |
| 703 | `auto allocate_rwx(void* hint, std::size_t) noexcept -> void*` | trampoline pages |
| 774 | `auto release(void*, std::size_t) noexcept -> void` | |
| 793 | `auto query_region(const void*) noexcept -> region_info` | |
| **988** | `auto safe_read(void* dst, const void* src, std::size_t) noexcept -> bool` | **the fault-safe read primitive used everywhere** |
| **1081** | `auto safe_write(void* dst, const void* src, std::size_t) noexcept -> bool` | fault-safe write |
| 1126 | `auto seh_guarded_copy(void*, const void*, std::size_t) noexcept -> bool` | real `cl.exe` only (`__try/__except`) |
| 1171 | `auto safe_read_fast(void*, const void*, std::size_t) noexcept -> bool` | SEH-first, kernel fallback |
| 1197 | `auto flush_instruction_cache(void*, std::size_t) noexcept -> void` | |
| 901–975 | `os::detail_signal` | [INT] Linux/Android SIGSEGV/SIGBUS sigsetjmp recovery; **chains to HotSpot's handler** |
| 1261–1324 | `os::detail_dr` | [INT] Windows x86_64 DR0–DR3 hardware-breakpoint plumbing |

There is **no** `os::seh_invoke`; the SEH-around-a-detour helper is
`hotspot::seh_invoke_detour` (7498) and is gated to real MSVC.

### 1.3 HotSpot metadata layer — `namespace vmhook::hotspot`, 1328–1576 (fwd) + 1909–8369  **[ADV]**

Every type here is a raw HotSpot structure view. Full per-member listing in §2.5; the structural
map is in §5.2. Key public types users can touch:

| Line | Type | Notes |
|---|---|---|
| 1596/1912 | `vm_type_entry_t`, `vm_struct_entry_t` (1923) | gHotSpotVMStructs/Types rows |
| 1928 | `struct return_slot { bool cancel; std::int64_t retval; }` | written by trampoline, read by `return_value` |
| 2263 | `struct symbol` | `to_string()` |
| 2350 | `struct constant_pool` | |
| 2427 | `struct const_method` | |
| 2658 | `struct method` | Method* view — entries, access flags, code, adapter |
| 3439 | `struct field_entry_t { std::uint32_t offset; bool is_static; std::string signature; klass* declaring_klass; }` | resolved field descriptor |
| 3465 | `struct klass` | Klass/InstanceKlass view |
| 4386 | `struct class_loader_data` | |
| 4562 | `struct dictionary` | |
| 4724 | `struct class_loader_data_graph` | |
| 4923 | `enum class java_thread_state : std::int8_t` | |
| 4951 | `struct java_thread` | state, suspend flags, os tid, `allocate_tlab` |
| 6240 | `using detour_function_t = std::int64_t(*)(frame*, java_thread*, return_slot*)` | trampoline ABI |
| 6248 | `struct method_args` | type-erased decoded-arg container |
| 6326 | `struct frame` | interpreter frame; **`get_arguments`** lives here |
| 6797 | `class midi2i_hook final` | trampoline generator |
| 7384 / 7420 | `struct hooked_method`, `struct i2i_hook_data` | hook registries |

### 1.4 Hook-callback surface

| Line | Symbol | Kind | Purpose |
|---|---|---|---|
| 1377 | `class return_value` | [USER] | first argument of every detour |
| 1380 | `explicit return_value(hotspot::return_slot*, hotspot::frame* = nullptr) noexcept` | [INT] | constructed by the library |
| 1387 | `template<typename value_type> auto set(const value_type) noexcept -> void` | [USER] | force a return value, suppress the body |
| 1435–1441 | `template<typename wrapper_type> requires std::is_base_of_v<object_base, wrapper_type> auto set(std::nullptr_t) noexcept -> void` | [USER] | force a null object return |
| 1444 | `auto cancel() noexcept -> void` | [USER] | suppress a void body |
| 1468 | `template<typename value_type> auto set_arg(std::int32_t index, value_type&&) noexcept -> bool` | [USER] | mutate an argument slot in place (def. 10497) |
| 1480 | `struct caller_info { hotspot::method* method; std::string class_name, method_name, signature; auto valid() const noexcept -> bool; }` | [USER] | |
| 1510 | `auto caller() const noexcept -> caller_info` | [USER] | immediate interpreter caller (def. 10232) |
| 1555 | `auto stack_trace(std::size_t max_depth = 64) const noexcept -> std::vector<caller_info>` | [USER] | (def. 10355) |
| 1565 | `auto frame() const noexcept -> hotspot::frame*` | [ADV] | escape hatch |

### 1.5 Class lookup & registry

| Line | Signature | Kind |
|---|---|---|
| 1678 | `inline std::unordered_map<std::type_index, std::string> type_to_class_map{}` | [INT] but `inline` and reachable |
| 1679 | `inline std::mutex registration_mutex{}` | [INT] |
| 1702 | `using type_factory_function_t = class object_base*(*)(void* instance)` | [INT] |
| 1703 | `inline std::unordered_map<std::string, type_factory_function_t> g_type_factory_map{}` | [INT] |
| 1705/9419 | `template<class wrapper_type> static auto register_class(std::string_view class_name) noexcept -> bool` | **[USER]** |
| 8403 | `inline std::unordered_map<std::string, hotspot::klass*> klass_lookup_cache{}` | [INT] |
| 8404 | `inline std::mutex klass_lookup_cache_mutex{}` | [INT] |
| 8425 | `static auto find_class(const std::string_view class_name) -> hotspot::klass*` | **[USER]** |
| 8408/8556 | `static auto resolve_array_klass(std::string_view descriptor) noexcept -> hotspot::klass*` | [INT] |
| 12495 | `inline auto find_class_via_oop(void* anchor_oop, std::string_view) noexcept -> hotspot::klass*` | [USER] |
| 12531 | `inline auto override_class_lookup(std::string_view, hotspot::klass*) noexcept -> void` | [ADV] |
| 12549 | `inline auto evict_class_lookup(std::string_view) noexcept -> void` | [ADV] |
| 12574 | `inline auto reanchor_classes_via_oop(void* anchor_oop, std::initializer_list<std::string_view>) noexcept -> bool` | **[USER]** |

### 1.6 Reflection / enumeration

| Line | Signature | Kind |
|---|---|---|
| 8679 | `template<typename visitor_type> inline auto for_each_loaded_class(visitor_type&&) -> void` | [USER] — visitor receives `hotspot::klass*` |
| 8745 | `template<typename predicate_type> inline auto deoptimize_methods_if(predicate_type&&) noexcept -> std::size_t` | [ADV] — predicate `(const std::string&, hotspot::method*)` |
| 8894 | `inline auto deoptimize_all_jit_compiled_methods() noexcept -> std::size_t` | [ADV] |
| 8912 | `struct thread_info { hotspot::java_thread* thread; hotspot::java_thread_state state; os::thread_id_t os_thread_id; }` | [USER] |
| 8953 | `template<typename visitor_type> inline auto for_each_thread(visitor_type&&) -> void` | [USER] — visitor takes `const thread_info&` |
| 9100 | `template<typename T, typename visitor_type> inline auto for_each_instance(visitor_type&&, std::size_t max_visits = SIZE_MAX) -> std::size_t` | **[USER]** — visitor takes `std::unique_ptr<T>` |
| 9304 | `template<typename visitor_type> inline auto for_each_instance_of(hotspot::klass* target, visitor_type&&, std::size_t max_visits = SIZE_MAX) -> std::size_t` | [ADV] — visitor takes raw `void*` |
| 9483 | `detail::collect_klass_methods(hotspot::klass*) noexcept -> std::vector<std::pair<std::string,std::string>>` | [INT] |
| 9552 | `inline auto get_class_methods(std::string_view class_name) noexcept -> std::vector<std::pair<std::string,std::string>>` | **[USER]** |
| 9564 | `template<class wrapper_type> inline auto get_class_methods() noexcept -> std::vector<std::pair<std::string,std::string>>` | **[USER]** |
| 9589 | `template<class wrapper_type> inline auto log_class_methods() noexcept -> void` | [USER] debug |
| 9615 | `template<class wrapper_type> inline auto find_methods_by_signature(std::string_view descriptor) noexcept -> std::vector<std::string>` | **[USER]** (obfuscated-build selector) |

### 1.7 Hooking

| Line | Signature | Kind |
|---|---|---|
| 9647 | `class watch_handle` (move-only RAII; `control_block` 9650, `stop()` 9689, `running()` 9723) | [USER] |
| 9753 | `class hook_handle` (move-only RAII; `hook_handle(hotspot::method*)` 9757, `installed()` 9790, `stop()` 9806/11861) | [USER] |
| 10742 | `template<class wrapper_type> static auto hook(std::string_view method_name, std::string_view method_signature, auto&& user_detour, bool* already_hooked = nullptr) -> bool` | **[USER]** |
| 10748 | `template<class wrapper_type> static auto hook(std::string_view method_name, auto&& user_detour) -> bool` | **[USER]** |
| 11193 | `static auto verify_hooks() noexcept -> std::size_t` | [ADV] |
| 11664 | `inline auto auto_repair_enabled() noexcept -> bool` | [ADV] |
| 11700 | `inline auto set_auto_repair_enabled(bool) noexcept -> void` | [ADV] |
| 11748 | `static auto shutdown_hooks() noexcept -> void` | **[USER]** |
| 11958 | `template<class wrapper_type> inline auto hook_by_signature(std::string_view jvm_descriptor, auto&& user_detour) -> bool` | **[USER]** |
| 12006 | `template<class wrapper_type> inline auto scoped_hook(std::string_view name, std::string_view signature, auto&& detour) -> hook_handle` | **[USER]** |
| 12119 | `template<class wrapper_type> inline auto scoped_hook(std::string_view name, auto&& detour) -> hook_handle` | **[USER]** |
| 11530 | `namespace detail::auto_repair` | [INT] watchdog |

### 1.8 Allocation / field / array primitives

| Line | Signature | Kind |
|---|---|---|
| 12620 | `template<typename wrapper_type, typename... args_t> static auto make_unique(args_t&&...) -> std::unique_ptr<wrapper_type>` | **[USER]** |
| 12699 | `inline std::unordered_map<hotspot::klass*, std::unordered_map<std::string, hotspot::field_entry_t>> g_field_cache{}` | [INT] |
| 12719 | `static auto find_field(hotspot::klass*, std::string_view) -> std::optional<hotspot::field_entry_t>` | [ADV] |
| 12784 | `template<typename value_type> static auto get_field(void* object, hotspot::klass*, std::string_view) -> value_type` | [ADV] free fn |
| 12843 | `template<typename value_type> static auto set_field(void* object, hotspot::klass*, std::string_view, value_type) -> void` | [ADV] free fn |
| 12910 | `inline auto make_java_object(hotspot::klass*, std::size_t requested_size) noexcept -> void*` | [ADV] |
| 13034 | `inline auto make_java_array(std::string_view class_name, std::int32_t length, std::size_t element_size, bool retained_for_abi = true) noexcept -> void*` | [ADV] |
| 13100 | `inline auto make_java_string(std::string_view) noexcept -> void*` | [ADV] |
| 13288 | `inline constexpr std::size_t k_max_safe_container_elems{ 1ull << 24 }` | [INT] |
| 13308 | `inline constexpr auto clamp_safe_container_count(std::int32_t) noexcept` | [INT] |
| 13333 | `inline static auto array_length(void* array_oop) noexcept -> std::int32_t` | [ADV] |
| 13370 | `template<typename element_type> static auto get_array_element(void* array_oop, std::int32_t index) -> element_type` | [ADV] |
| 13415 | `template<typename element_type> static auto set_array_element(void* array_oop, std::int32_t index, element_type) -> void` | [ADV] |

### 1.9 `field_proxy` — 13461–14748  **[USER]**

| Line | Member | Notes |
|---|---|---|
| 13484 | `struct value_t` | `std::variant<bool,int8,int16,int32,int64,float,double,uint16,uint32> data; std::string signature;` — `uint32` alternative *is* the compressed oop |
| 14004 | `template<typename target_type> requires detail::value_t_convertible_target_v<target_type> operator target_type() const noexcept` | the implicit-conversion engine |
| 14031 | `auto as_string() const noexcept -> std::string` | |
| 14052 | `auto is_reference() const noexcept -> bool` | true iff variant holds `uint32` |
| 14066 | `template<typename element_type> auto to_vector() const -> std::vector<std::unique_ptr<element_type>>` | out-of-line def 18350 |
| 14080 | `template<typename key_type, typename value_type> auto to_entries() const -> std::vector<std::pair<std::unique_ptr<key_type>, std::unique_ptr<value_type>>>` | out-of-line def 18412 |
| 14092 | `field_proxy(void* field_pointer, std::string sig, bool is_static_flag) noexcept` | 3-arg ctor |
| 14112 | `field_proxy(void* field_pointer, std::string sig, bool is_static_flag, hotspot::klass* mirror_klass, std::size_t field_offset) noexcept` | GC-stable static ctor |
| 14132 | `auto get() const noexcept -> value_t` | |
| 14281 | `template<typename value_type> auto set(const value_type&) const noexcept -> void` | |
| 14470 | `auto signature() const noexcept -> std::string_view` | |
| 14484 | `auto raw_address() const noexcept -> void*` | |
| 14498 | `auto is_static() const noexcept -> bool` | |
| 14516 | `auto is_reference() const noexcept -> bool` | |
| 14531 | `auto get_compressed_oop() const noexcept -> std::uint32_t` | |
| 14621 | `auto store_object_oop(void* decoded_oop) const noexcept -> bool` | |
| 14691 | `auto store_string(std::string_view) const noexcept -> bool` | |
| 14727–14746 | private: `void* field_pointer; std::string signature_text; bool static_field; hotspot::klass* mirror_klass{nullptr}; std::size_t field_offset{0};` | |

### 1.10 `method_proxy` — 14859–15911  **[USER]**

| Line | Member | Notes |
|---|---|---|
| 14868 | `struct value_t` | `std::variant<std::monostate,bool,int8,int16,int32,int64,float,double,uint16,uint32,std::string>` |
| 14911 | `template<typename target_type> requires detail::value_t_convertible_target_v<target_type> operator target_type() const noexcept` | `void*` target routes through `decode_oop_pointer` |
| 14989 / 14997 / 15015 | `is_void()`, `is_string()`, `as_string()` | |
| 15043 | `method_proxy(void* owning_object, hotspot::method* method_ptr, std::string sig, bool pinned = false) noexcept` | |
| 15053 | `template<typename... args_t> auto call(args_t&&...) const noexcept -> value_t` | |
| 15362 / 15375 / 15395 / 15425 | `name()`, `signature()`, `is_static()`, `is_reference()` | |
| 15446 | `auto get_compressed_oop() const noexcept -> std::uint32_t` | receiver's compressed oop |
| 15472 | `auto raw_method() const noexcept -> hotspot::method*` | |
| 15492 | `static auto klass_from_object_header(void* oop) noexcept -> hotspot::klass*` | private |
| 15586 / 15688 / 15733 / 15778 | `argument_matches_descriptor`, `next_argument_descriptor`, `signature_matches_arguments`, `resolve_compatible_method` | private overload resolution |
| 15875–15884 | private: `void* object; hotspot::method* method; std::string signature_text; bool signature_pinned{false};` + mutable JNI caches | |

### 1.11 Object model — 15913–17069

| Line | Symbol | Kind |
|---|---|---|
| 15924 | `using oop_type_t = void*;` | **[USER]** |
| 15933 | `using oop_t = oop_type_t;` | **[USER]** |
| 15978 | `class object_base` | **[USER]** base for all wrappers |
| 15986 | `explicit object_base(oop_type_t instance = nullptr) noexcept` | |
| 15991 | `virtual ~object_base() = default` | |
| 15999/16004 | copy ctor / copy assign `= default` (raw pointer copy) | |
| 16013/16019 | move ctor / move assign (nulls the source) | |
| 16033 | `auto get_instance() const noexcept -> oop_type_t` | |
| 16059 | `auto get_field(std::string_view) const -> std::optional<field_proxy>` | |
| 16123 | `static auto get_field(std::type_index wrapper_type, std::string_view) -> std::optional<field_proxy>` | |
| 16181 | `auto get_method(std::string_view) const -> std::optional<method_proxy>` | |
| 16246 | `auto get_method(std::string_view, std::string_view signature) const -> std::optional<method_proxy>` | |
| 16316 | `static auto get_method(std::type_index, std::string_view) -> std::optional<method_proxy>` | |
| 16379 | `static auto get_method(std::type_index, std::string_view, std::string_view) -> std::optional<method_proxy>` | |
| 16444 | `auto resolve_klass() const -> hotspot::klass*` | private-ish |
| 16464 | `static auto resolve_klass(std::type_index) -> hotspot::klass*` | |
| 16516–16821 | `static_method_only`, `safe_interface_methods`, `safe_method_name_signature`, `safe_method_is_default`, `find_interface_default_method` | [INT] interface-default-method fallback |
| 16945 | `template<typename derived = void> class object : public object_base` | **[USER]** CRTP-ish |
| 16971/16977/16983 | deducing-this `get_field`/`get_method` (`VMHOOK_HAS_DEDUCING_THIS`) | |
| 16996/16997 | `using object_base::get_field/get_method` (pre-C++23 path) | |
| 17010/17016/17022 | static-context `get_field`/`get_method` fallbacks | |
| 17032 | `static auto static_field(std::string_view) -> std::optional<field_proxy>` | portable |
| 17041/17050 | `static auto static_method(...)` | portable |
| 17070 | `inline auto klass_from_oop(void* oop) noexcept -> hotspot::klass*` | **[USER]** |
| 17160 | `inline auto is_instance_of(void* object_oop, std::string_view class_name) noexcept -> bool` | **[USER]** |
| 17203 | `class oop_reflective_base : public object_base` | [INT] resolves via the *live oop's* klass, not the registry |
| 17213 | `auto oop_klass() const noexcept -> hotspot::klass*` | protected |
| 17221 | `auto get_field_by_oop_klass(std::string_view) const -> std::optional<field_proxy>` | protected |
| 17262 | `auto get_method_by_oop_klass(std::string_view) const -> std::optional<method_proxy>` | protected |

### 1.12 Collection wrappers — 17292–17772  **[USER]**

| Line | Symbol | Notes |
|---|---|---|
| 17316 | `class collection : public oop_reflective_base` | |
| 17329 | `explicit collection(oop_t oop) noexcept` | |
| 17342 / 17360 | `size() -> std::int32_t`, `is_empty() -> bool` | |
| 17393 | `template<typename element_type> auto to_vector() const -> std::vector<std::unique_ptr<element_type>>` | ArrayList / LinkedList / HashSet / TreeSet paths |
| 17564 | `class list : public collection` (ctor 17567) | type tag |
| 17585 | `class set : public collection` (ctor 17588) | type tag |
| 17606 | `class linked_list : public list` (ctor 17609) | type tag |
| 17629 | `class map : public oop_reflective_base` (ctor 17636) | |
| 17649 / 17667 | `size()`, `is_empty()` | |
| 17687 | `template<typename key_type, typename value_type> auto to_entries() const -> std::vector<std::pair<std::unique_ptr<key_type>, std::unique_ptr<value_type>>>` | HashMap `table` then TreeMap `root` |
| 17727 | `class hash_map : public map` (ctor 17730) | type tag |

**There is no `tree_map` class.** TreeMap support exists only as the free walk helpers
`tree_map_walk_entries` (18096) and `tree_map_walk_keys` (18227), reached from
`map::to_entries` / `collection::to_vector`. If the memory note or a doc claims a `tree_map`
wrapper, that is inaccurate.

### 1.13 Walk helpers & string/array helpers

| Line (fwd / def) | Signature |
|---|---|
| 17292 / 17833 | `template<typename element_type, typename out_t> inline auto linked_list_walk_items(void* list_oop, std::int32_t size, out_t& out) -> void` |
| 17294 / 17911 | `template<typename key_type, typename value_type, typename out_t> inline auto hash_map_walk_entries(void* map_oop, out_t& out) -> void` |
| 17296 / 18004 | `template<typename element_type, typename out_t> inline auto hash_map_walk_keys(void* map_oop, out_t& out) -> void` |
| 17298 / 18096 | `template<typename key_type, typename value_type, typename out_t> inline auto tree_map_walk_entries(void* map_oop, out_t& out) -> void` |
| 17300 / 18227 | `template<typename element_type, typename out_t> inline auto tree_map_walk_keys(void* map_oop, out_t& out) -> void` |
| 17773 | `inline auto read_compressed_oop_at(const void* holder_oop, ...)` |
| 17808 | `inline auto read_compressed_oop_at_safe(const void* holder_oop, ...)` |
| 1731 / 18468 | `inline auto read_java_string(void* string_oop) -> std::string` |
| 1734 / 13100 | `inline auto make_java_string(std::string_view) noexcept -> void*` |
| 1737 / 18757 | `inline auto write_java_string(void* string_oop, std::string_view) noexcept -> void` |
| 1740 / 18981 | `inline auto decode_array_oop(std::uint32_t compressed) -> void*` |
| 1743 / 18833 | `inline auto set_str_field(const field_proxy&, std::string_view) noexcept -> void` |
| 1746 / 18734 | `inline auto field_oop(const field_proxy&) noexcept -> void*` |
| 1749 / 18852 | `inline auto set_bool_array(const field_proxy&, const std::vector<bool>&) noexcept -> void` |
| 1752 / 18957 | `inline auto set_str_array(const field_proxy&, const std::vector<std::string>&) noexcept -> void` |
| 1755 / 18884 | `template<typename element_type> inline auto set_prim_array(const field_proxy&, const std::vector<element_type>&) noexcept -> void` |

### 1.14 Watchers — 19004–19661  **[USER]**

| Line | Signature |
|---|---|
| 19225 | `template<class wrapper_type, typename field_type, typename callback_type> inline auto watch_static_field(std::string_view field_name, callback_type on_change) -> watch_handle` |
| 19383 | `template<typename callback_type> inline auto on_class_loaded(callback_type on_load) -> watch_handle` |
| 19549 | `template<typename callback_type> inline auto on_exception(callback_type on_throw) -> watch_handle` |
| 19330 / 19495 | `detail::class_load_*` / `detail::exception_*` registries, `class_loader_wrapper`, `throwable_wrapper` |
| 19672 | `detail::reset_watcher_latches() noexcept -> void` |

### 1.15 Lifetime primitives (today) — 19687–19822

| Line | Signature | Reality |
|---|---|---|
| 19700 | `namespace jni` | vestigial name; no JNI remains |
| 19725 | `class global_ref final` | move-only, non-copyable |
| 19731 | `explicit global_ref(oop_t raw_oop) noexcept` | **stores the pointer verbatim** |
| 19775 | `auto oop() const noexcept -> oop_t` | **returns it verbatim — no re-read, no GC root** |
| 19783 | `auto reset() noexcept -> void` | nulls the field |
| 19790 | `auto handle() const noexcept -> void*` | ABI-compat alias for `oop()` |
| 19795 | `explicit operator bool() const noexcept` | |
| 19803 | `inline auto pin(oop_t oop) noexcept -> jni::global_ref` | |
| 19815 | `template<typename wrapper_type> inline auto pin(const std::unique_ptr<wrapper_type>&) noexcept -> jni::global_ref` | static_asserts `is_base_of_v<object_base, T>` |

---

## 2. Raw-OOP boundary crossings (the critical deliverable)

Every public signature that **takes** or **returns** a raw `void*` / `oop_t` / `hotspot::*` pointer,
or a bare `std::uint32_t` compressed oop. Grouped by how a handle model would have to intercept it.

Notation for the proposed replacement: `ref` = the new library-managed handle
(strawman: `vmhook::ref` / `vmhook::ref<T>`), `class_ref` = a handle for `klass*`,
`method_ref` = a handle for `method*`.

### 2.1 Category A — object oops crossing INTO user code (the ones that matter most)

| # | Line | Current signature | Handle-based replacement |
|---|---|---|---|
| A1 | 15924/15933 | `using oop_type_t = void*; using oop_t = oop_type_t;` | keep as the *raw* escape-hatch alias; introduce `class ref` as the default currency. Do **not** redefine the alias — `cast_for_variant` (13884) has a `std::is_same_v<target_type, void*>` branch that a class type would silently disable. |
| A2 | 15986 | `explicit object_base(oop_type_t instance = nullptr) noexcept` | `explicit object_base(ref instance = {}) noexcept` — the single most load-bearing change; every user wrapper's ctor forwards `vmhook::oop_t` today (see 19510 `throwable_wrapper`, 19350 `class_loader_wrapper`, and every wrapper generated by `viewer/src/wrapper_gen.hpp`). Needs a transition overload taking `oop_t`. |
| A3 | 16033 | `auto get_instance() const noexcept -> oop_type_t` | `auto get_ref() const noexcept -> ref` + keep `get_instance()` returning the raw pointer, documented as "valid this frame only". Used by `pin()` (19819) and by every generated wrapper. |
| A4 | 6501–6503 | `frame::template<typename... types> auto get_arguments() const noexcept -> std::tuple<types...>` | `std::tuple<...>` where a `ref`-typed element is produced instead of `oop_t`. This is *the* detour entry path. |
| A5 | 6554 | `frame::auto get_arguments() const noexcept -> hotspot::method_args` | `method_args` holds `ref` per slot rather than `void* decoded_oop` |
| A6 | 6268 | `method_args::auto operator[](std::size_t) const noexcept -> void*` | `-> ref` |
| A7 | 6284–6286 | `method_args::template<typename wrapper_type> auto as(std::size_t) const noexcept -> wrapper_type*` | `-> std::unique_ptr<wrapper_type>` or a handle-owning wrapper; today it does a bare `new wrapper_type{ raw void* }` at 6297 and hands the user an owning raw pointer |
| A8 | 6690–6692 | `frame::template<typename argument_type> auto get_argument(std::int32_t index) const noexcept -> argument_type` (private) | same, add a `ref` specialisation |
| A9 | 10066–10068 | `detail::template<typename value_type> auto extract_frame_arg(hotspot::frame* frame, std::int32_t index) -> std::remove_cvref_t<value_type>` | the choke point where `std::unique_ptr<T>` detour args are materialised (10152+). **A handle model that hooks only this function covers every detour argument.** |
| A10 | 6424 | `frame::auto get_locals() const noexcept -> void**` | leave raw ([ADV]) |
| A11 | 9100 | `for_each_instance<T>(visitor, max_visits)` — visitor receives `std::unique_ptr<T>` built from a raw scan cursor (9265–9267) | visitor receives a handle-owning `T`; the doc at 9059–9062 already warns the wrappers must not outlive the call |
| A12 | 9304 | `for_each_instance_of(hotspot::klass* target_klass, visitor, max_visits)` — visitor receives raw `void*` (9395) | `for_each_instance_of(class_ref, visitor)` with visitor taking `ref` |
| A13 | 8953 | `for_each_thread(visitor)` — `thread_info::thread` is a raw `hotspot::java_thread*` (8914) | `thread_ref`; note the doc at 8905–8911 explicitly says "valid only during the visit" |

### 2.2 Category B — field access

| # | Line | Current signature | Handle-based replacement |
|---|---|---|---|
| B1 | 14092 | `field_proxy(void* field_pointer, std::string sig, bool is_static_flag) noexcept` | `field_proxy(ref holder, std::size_t offset, std::string sig, bool is_static)` — computing `holder + offset` **at read time** is what makes it relocation-safe; the 5-arg static ctor (14112) already proves the pattern for mirrors |
| B2 | 14112 | `field_proxy(void*, std::string, bool, hotspot::klass* mirror_klass, std::size_t field_offset) noexcept` | `field_proxy(class_ref mirror_klass, std::size_t offset, std::string sig)` |
| B3 | 14484 | `auto raw_address() const noexcept -> void*` | keep [ADV]; it is required by `watch_static_field` (19240) for the DR breakpoint address |
| B4 | 14531 | `auto get_compressed_oop() const noexcept -> std::uint32_t` | `auto get_ref() const noexcept -> ref` |
| B5 | 14621 | `auto store_object_oop(void* decoded_oop) const noexcept -> bool` | `auto store(ref) const noexcept -> bool` — the doc block at 14606–14614 spells out the *exact* GC hole a handle fixes: "the CALLER must keep `decoded_oop` reachable across this call" |
| B6 | 13484–13496 | `field_proxy::value_t::data` holds `std::uint32_t` for reference/array | add a `ref` alternative, or keep `uint32` and expose `as_ref()` |
| B7 | 14066 | `value_t::to_vector<element_type>() -> std::vector<std::unique_ptr<element_type>>` | vector of handle-owning wrappers |
| B8 | 14080 | `value_t::to_entries<K,V>() -> std::vector<std::pair<std::unique_ptr<K>, std::unique_ptr<V>>>` | same |
| B9 | 13884 (`cast_for_variant`) 13930–13946 | produces `clean_target_type{ new wrapper_type{ decoded } }` from a `uint32` | produce a handle-owning wrapper |
| B10 | 12784 | `template<typename value_type> static auto get_field(void* object, hotspot::klass*, std::string_view) -> value_type` | `get_field<T>(ref object, class_ref, std::string_view)` |
| B11 | 12843 | `template<typename value_type> static auto set_field(void* object, hotspot::klass*, std::string_view, value_type) -> void` | `set_field<T>(ref object, class_ref, std::string_view, T)` |
| B12 | 12719 | `static auto find_field(hotspot::klass*, std::string_view) -> std::optional<hotspot::field_entry_t>` | `find_field(class_ref, std::string_view)` |
| B13 | 18734 | `inline auto field_oop(const field_proxy&) noexcept -> void*` | `-> ref` |
| B14 | 1743/18833 | `set_str_field(const field_proxy&, std::string_view) noexcept -> void` | unchanged externally, but internally must root the new String |

### 2.3 Category C — method invocation

| # | Line | Current signature | Handle-based replacement |
|---|---|---|---|
| C1 | 15043 | `method_proxy(void* owning_object, hotspot::method* method_ptr, std::string sig, bool pinned = false) noexcept` | `method_proxy(ref receiver, method_ref, std::string sig, bool pinned)` — the receiver `void*` (member at 15875) is held for the *lifetime of the proxy*, which is the second-biggest dangling risk after `object_base::instance` |
| C2 | 15053 | `template<typename... args_t> auto call(args_t&&...) const noexcept -> value_t` | `ref`-typed args accepted; a `ref`-returning `value_t` |
| C3 | 14868–14880 | `method_proxy::value_t::data` holds `std::uint32_t` for reference returns | add `ref` alternative / `as_ref()` |
| C4 | 15446 | `auto get_compressed_oop() const noexcept -> std::uint32_t` | `-> ref` |
| C5 | 15472 | `auto raw_method() const noexcept -> hotspot::method*` | `-> method_ref` (keep raw as [ADV]) |
| C6 | 15492 | `static auto klass_from_object_header(void* oop) noexcept -> hotspot::klass*` (private) | `(ref) -> class_ref` |
| C7 | 16181/16246/16316/16379 | `object_base::get_method(...) -> std::optional<method_proxy>` | unchanged externally; the proxy it builds must carry a `ref` receiver (constructed at 16211, 16283, …) |

### 2.4 Category D — allocation, strings, arrays, class lookup

| # | Line | Current signature | Handle-based replacement |
|---|---|---|---|
| D1 | 12910 | `inline auto make_java_object(hotspot::klass*, std::size_t requested_size) noexcept -> void*` | `-> ref` (must be rooted **before** returning; today the caller has an unrooted oop) |
| D2 | 13034 | `inline auto make_java_array(std::string_view, std::int32_t length, std::size_t element_size, bool = true) noexcept -> void*` | `-> ref`; the `retained_for_abi` bool exists *because* of the unrooted-intermediate hazard (13024–13031) — a handle model deletes the parameter |
| D3 | 13100 | `inline auto make_java_string(std::string_view) noexcept -> void*` | `-> ref` |
| D4 | 18468 | `inline auto read_java_string(void* string_oop) -> std::string` | `(ref) -> std::string` |
| D5 | 18757 | `inline auto write_java_string(void* string_oop, std::string_view) noexcept -> void` | `(ref, std::string_view)` |
| D6 | 18981 | `inline auto decode_array_oop(std::uint32_t compressed) -> void*` | `(std::uint32_t) -> ref` |
| D7 | 13333 | `inline static auto array_length(void* array_oop) noexcept -> std::int32_t` | `(ref) -> std::int32_t` |
| D8 | 13370 | `template<typename element_type> static auto get_array_element(void* array_oop, std::int32_t) -> element_type` | `(ref, std::int32_t) -> element_type` |
| D9 | 13415 | `template<typename element_type> static auto set_array_element(void* array_oop, std::int32_t, element_type) -> void` | `(ref, std::int32_t, T)` |
| D10 | 12620 | `template<typename wrapper_type, typename... args_t> static auto make_unique(args_t&&...) -> std::unique_ptr<wrapper_type>` | returns a handle-owning wrapper; note it already calls `make_java_object` (12666) and immediately `std::make_unique<wrapper_type>(object_pointer)` (12674) with **no rooting in between** |
| D11 | 8425 | `static auto find_class(std::string_view) -> hotspot::klass*` | `-> class_ref` (a `klass*` is metaspace, not heap — a *different* lifetime problem: class unloading, see §4) |
| D12 | 12495 | `inline auto find_class_via_oop(void* anchor_oop, std::string_view) noexcept -> hotspot::klass*` | `(ref anchor, std::string_view) -> class_ref` |
| D13 | 12531 | `inline auto override_class_lookup(std::string_view, hotspot::klass*) noexcept -> void` | `(std::string_view, class_ref)` |
| D14 | 12574 | `inline auto reanchor_classes_via_oop(void* anchor_oop, std::initializer_list<std::string_view>) noexcept -> bool` | `(ref anchor, ...)` |
| D15 | 17070 | `inline auto klass_from_oop(void* oop) noexcept -> hotspot::klass*` | `(ref) -> class_ref` |
| D16 | 17160 | `inline auto is_instance_of(void* object_oop, std::string_view) noexcept -> bool` | `(ref, std::string_view) -> bool` |
| D17 | 8679 | `for_each_loaded_class(visitor)` — visitor receives `hotspot::klass*` | visitor receives `class_ref` |
| D18 | 8745 | `deoptimize_methods_if(pred)` — pred receives `(const std::string&, hotspot::method*)` | `(const std::string&, method_ref)` |

### 2.5 Category E — collection wrappers (all take a bare `oop_t` by value)

| # | Line | Current signature | Replacement |
|---|---|---|---|
| E1 | 17329 | `explicit collection(oop_t oop) noexcept` | `explicit collection(ref) noexcept` |
| E2 | 17567 | `explicit list(oop_t oop) noexcept` | ditto |
| E3 | 17588 | `explicit set(oop_t oop) noexcept` | ditto |
| E4 | 17609 | `explicit linked_list(oop_t oop) noexcept` | ditto |
| E5 | 17636 | `explicit map(oop_t oop) noexcept` | ditto |
| E6 | 17730 | `explicit hash_map(oop_t oop) noexcept` | ditto |
| E7 | 17393 | `collection::to_vector<element_type>() -> std::vector<std::unique_ptr<element_type>>` | handle-owning elements |
| E8 | 17687 | `map::to_entries<K,V>() -> std::vector<std::pair<std::unique_ptr<K>, std::unique_ptr<V>>>` | handle-owning |
| E9 | 17833 | `linked_list_walk_items<element_type, out_t>(void* list_oop, std::int32_t size, out_t& out)` | `(ref, …)` |
| E10 | 17911 | `hash_map_walk_entries<K,V,out_t>(void* map_oop, out_t& out)` | `(ref, …)` |
| E11 | 18004 | `hash_map_walk_keys<element_type,out_t>(void* map_oop, out_t& out)` | `(ref, …)` |
| E12 | 18096 | `tree_map_walk_entries<K,V,out_t>(void* map_oop, out_t& out)` | `(ref, …)` |
| E13 | 18227 | `tree_map_walk_keys<element_type,out_t>(void* map_oop, out_t& out)` | `(ref, …)` |
| E14 | 17773 | `read_compressed_oop_at(const void* holder_oop, …)` | `(ref holder, …) -> ref` |
| E15 | 17808 | `read_compressed_oop_at_safe(const void* holder_oop, …)` | `(ref holder, …) -> ref` |
| E16 | 17213 | `oop_reflective_base::oop_klass() const noexcept -> hotspot::klass*` | `-> class_ref` |

### 2.6 Category F — lifetime primitives (to be replaced outright)

| # | Line | Current signature | Fate |
|---|---|---|---|
| F1 | 19725 | `class jni::global_ref final` | **replace** with the real handle; keep the name as a deprecated alias |
| F2 | 19731 | `explicit global_ref(oop_t raw_oop) noexcept` | `explicit ref(oop_t)` becomes the *registration* point |
| F3 | 19775 | `auto oop() const noexcept -> oop_t` | must become a **re-read through the handle slot**, which is exactly what the current doc admits it does not do |
| F4 | 19790 | `auto handle() const noexcept -> void*` | keep as raw escape hatch |
| F5 | 19803 | `inline auto pin(oop_t) noexcept -> jni::global_ref` | `inline auto pin(oop_t) noexcept -> ref` |
| F6 | 19815 | `template<typename wrapper_type> inline auto pin(const std::unique_ptr<wrapper_type>&) noexcept -> jni::global_ref` | `-> ref` |

### 2.7 Category G — hook/return-value surface (hotspot pointers, not heap oops)

| # | Line | Current signature | Note |
|---|---|---|---|
| G1 | 1380 | `explicit return_value(hotspot::return_slot*, hotspot::frame* = nullptr) noexcept` | library-constructed; leave raw |
| G2 | 1565 | `auto frame() const noexcept -> hotspot::frame*` | [ADV] escape hatch |
| G3 | 1482 | `caller_info::hotspot::method* method` | `method_ref` candidate |
| G4 | 9757 | `explicit hook_handle(hotspot::method* m) noexcept` | `method_ref` candidate |
| G5 | 1435–1441 | `template<wrapper_type> auto set(std::nullptr_t)` | already handle-shaped (writes 0) — a `set(ref)` overload is the natural addition, and **does not exist today**: you cannot currently force a hooked method to return an object you constructed, only null or a primitive |
| G6 | 1468/10497 | `template<typename value_type> auto set_arg(std::int32_t, value_type&&) noexcept -> bool` | needs a `ref` overload for object args |

### 2.8 Category H — raw HotSpot accessors reachable from public code

These are `[ADV]` and probably stay raw, but a handle design must decide whether they are part
of the "never hand the user a raw pointer" promise.

`klass::get_java_mirror() -> void*` (3711) · `klass::get_super() -> klass*` (3768) ·
`klass::get_methods_ptr() -> method**` (3576) · `klass::get_interfaces_ptr(int32&) -> klass**` (3644) ·
`klass::get_name() -> symbol*` (3472) · `klass::find_field(string_view) -> optional<field_entry_t>` (4117) ·
`klass::collect_fields()` (4256) · `class_loader_data::get_class_loader_oop() -> void*` (4515) ·
`class_loader_data::get_klasses() -> klass*` (4393) · `dictionary::find_klass(string_view) -> klass*` (4609) ·
`class_loader_data_graph::find_klass(string_view) -> klass*` (4761) ·
`java_thread::allocate_tlab(std::size_t) -> void*` (5189) ·
`method::get_i2i_entry/get_code/get_adapter/... -> void*` (2667, 3144, 3350, 2716, 3256) ·
`hotspot::decode_oop_pointer(std::uint32_t) -> void*` (5634) ·
`hotspot::encode_oop_pointer(void*) -> std::uint32_t` (5680) ·
`hotspot::decode_klass_pointer(std::uint32_t) -> void*` (5751) ·
`hotspot::encode_klass_pointer(void*) -> std::uint32_t` (5868) ·
`hotspot::read_klass_from_header_buffer(const void*) -> klass*` (5823) ·
`hotspot::is_valid_pointer(const void*) -> bool` (2080) ·
`hotspot::safe_read_pointer(const void*) -> const void*` (2139) ·
`hotspot::cold_read_frame_pointer/cold_read_metadata_pointer(const void*) -> void*` (2197, 2245).

**Count:** 13 (A) + 14 (B) + 7 (C) + 18 (D) + 16 (E) + 6 (F) + 6 (G) crossings in the
user-facing tiers, plus the Category-H raw layer.

---

## 3. Ownership & lifetime today

### 3.1 `object_base` holds a bare `void*` with no owner

```cpp
// 15986
explicit object_base(oop_type_t instance = nullptr) noexcept
    : instance{ instance }
{
}
// 16033
auto get_instance() const noexcept
    -> oop_type_t
{
    return this->instance;
}
```

The member is `oop_type_t instance` (a `void*`). Copy ctor/assign are `= default` and the doc block
at 15995–15998 says it outright:

> `@brief Copy constructor — copies the raw OOP pointer.`
> `Both the source and the copy point to the same Java object.`
> `The pointer is not a GC handle so no reference counting occurs.`

Move (16013–16031) nulls the source purely so the moved-from object is "safely destructible" — there
is nothing to destruct. **Nobody owns the oop. The JVM owns it and can move or collect it at any
safepoint.** The contract is the `oop_type_t` doc block (15919–15922):

> `It is NOT a JNI global reference - no GC handles are created and the pointer remains valid only
> for the duration of the hook.`

and repeated at the bottom of the `object_base` doc (15975–15977):

> `@note The wrapped pointer is a raw decoded OOP, not a JNI global reference.`
> `It is valid for the duration of the hook invocation only.`

### 3.2 Where `std::unique_ptr<T>` returns appear, and why

`std::unique_ptr<T>` in this codebase is **C++-side allocation ownership of the wrapper object**,
not Java-side ownership of the oop. It appears in five places:

1. **Detour arguments** — `detail::extract_frame_arg` (10066) materialises
   `std::unique_ptr<element_t>` for a detour parameter declared
   `const std::unique_ptr<my_class>&` (10152+). The `unique_ptr` owns the heap-allocated *wrapper*;
   the wrapper's `instance` is a bare frame-slot oop.
2. **`make_unique<T>(args...)`** (12620) — allocates a Java object via `make_java_object` (12666)
   then `std::make_unique<wrapper_type>(object_pointer)` (12674). Returns
   `std::unique_ptr<wrapper_type>`. **Between allocation and wrapper construction the oop is
   unrooted.**
3. **`field_proxy::value_t::to_vector<T>()`** (14066 / 18350) and **`to_entries<K,V>()`**
   (14080 / 18412) — `std::vector<std::unique_ptr<T>>`.
4. **`collection::to_vector<T>()`** (17393) and **`map::to_entries<K,V>()`** (17687) — same shape;
   elements are `std::make_unique<element_type>(static_cast<oop_t>(element_oop))` (17435).
5. **`for_each_instance<T>`** (9100) — the visitor receives `std::unique_ptr<T>` built at 9265–9266.

`cast_for_variant`'s unique_ptr branch is the canonical example (13930–13946): it validates the
signature is `'L'`, decodes, range-checks, klass-checks, then
`return clean_target_type{ new wrapper_type{ decoded } };`.

**Consequence for the design:** a handle model does not need to change the `unique_ptr` return
shape at all — it needs to change what the *wrapper inside* holds. That is a much smaller blast
radius than it first appears.

### 3.3 How collection wrappers hold element oops

They do not hold them. `collection` / `map` derive from `oop_reflective_base` (17203) which derives
from `object_base` and **adds no data members** (documented explicitly at 17190–17193). They hold
one `void*` — the container's own oop — and re-walk the Java structure on every `to_vector()` /
`to_entries()` call. Elements are decoded transiently inside the walk (e.g. 17429–17441) and
immediately wrapped into fresh `unique_ptr<T>`s. Every returned element wrapper carries a raw oop
snapshot taken during that one walk.

### 3.4 `field_proxy` — a partially solved case that is the template for the fix

`field_proxy` already implements a *narrow* version of the handle idea for static fields. Its
members (14727–14746):

```cpp
void* field_pointer;
std::string signature_text;
bool        static_field;
vmhook::hotspot::klass* mirror_klass{ nullptr };
std::size_t             field_offset{ 0 };
```

and the rationale comment (14732–14746) is the clearest statement of the problem in the whole
header:

> `For a static field, field_pointer == mirror_oop + field_offset, but the mirror is a relocatable
> young/heap oop.  A relocating GC (G1) can move it between the moment static_field() computed
> field_pointer and the moment get() dereferences it, leaving field_pointer pointing at a stale
> (possibly unmapped) old address -> wrong bytes or a fault.  To stay correct, remember the
> GC-STABLE root for the mirror — the declaring klass, whose _java_mirror OopHandle survives
> relocation — plus the field's offset, so get() can re-resolve the *current* mirror address at
> read time`

**Instance fields do not get this treatment** — `field_pointer` is `instance + offset` computed once
(16093, 16110) and never re-resolved. A handle model generalises the static-field trick to instance
fields: store `(ref holder, offset)` and compute the address at read time.

### 3.5 `method_proxy` holds a receiver oop for its whole lifetime

`void* object;` (15875), captured at construction (15043) from `this->instance` (16211, 16283,
16341, …). The proxy is returned by value inside `std::optional` and can be stored by the user for
arbitrarily long. This is a live dangling hazard with no mitigation today.

### 3.6 The `pin` illusion

```cpp
// 19775-19781 (doc trimmed)
//   PURE-VM LIMITATION: creating a real GC root (what JNIEnv::NewGlobalRef used to do)
//   requires a call into the VM, which is exactly the JNI dependency this build removes.
//   vmhook therefore stores the raw OOP captured at construction and returns it as-is.
auto oop() const noexcept -> vmhook::oop_t
{
    return this->oop_;
}
```

Any handle design has to answer the question this comment dodges: **without JNI, where does the GC
root live?** Options visible from this header: a HotSpot `OopHandle`/`OopStorage` reached via
VMStructs (the same mechanism `klass::get_java_mirror` at 3711 already dereferences —
`struct OopHandle { oop* _obj; }` local at 3731); a JNI-handle-block on a `java_thread`; or a
side table plus a mutator barrier. `klass::get_java_mirror` is the existing proof that the library
can read through an indirection cell.

---

## 4. The type registry

### 4.1 What is stored

Two process-global `inline` maps under one mutex:

| Line | Container | Key | Value |
|---|---|---|---|
| 1678 | `type_to_class_map` | `std::type_index` of the C++ wrapper | internal JVM class name, `/`-separated |
| 1703 | `g_type_factory_map` | internal JVM class name (`std::string`) | `object_base*(*)(void* instance)` factory |
| 1679 | `registration_mutex` | guards both | |

Plus two lookup caches:

| Line | Container | Key | Value | Guarded by |
|---|---|---|---|---|
| 8403 | `klass_lookup_cache` | class name string | `hotspot::klass*` | `klass_lookup_cache_mutex` (8404) |
| 12699 | `g_field_cache` | `klass*` → field name | `hotspot::field_entry_t` | `g_field_cache_mutex` (12700) |

### 4.2 Binding path

`register_class<T>("a/b/C")` (9420):
1. `find_class(class_name)` (9423) — must succeed, i.e. **the class must already be loaded**.
2. Takes `registration_mutex` (9439).
3. Inserts `typeid(T) → "a/b/C"` into `type_to_class_map` and a `+[](void* oop){ return new T{oop}; }`
   factory into `g_type_factory_map`.

Resolution back to a klass, at use time, is always
`type_to_class_map[typeid(*this)] → find_class(name) → klass*`:
- `object_base::resolve_klass()` (16444) uses `typeid(*this)` — the **dynamic** type, so the
  wrapper must be polymorphic (it is: `virtual ~object_base` at 15991) and the *most-derived* type
  must be the registered one.
- `object_base::resolve_klass(std::type_index)` (16464) is the static-context variant, fed by
  `object<derived>::static_field/static_method` (17032/17041) using `typeid(derived)`.

Notably the registry is **not** consulted for wrapper-typed *field* reads: the comment at 1690–1697
says `field_proxy` inlines `new T{ decoded_oop }` in `cast_for_variant` directly, bypassing
`g_type_factory_map`. Only `extract_frame_arg` (10152+) uses the factory map.

### 4.3 What invalidates it

- `find_class` results are memoised in `klass_lookup_cache` **forever** unless explicitly evicted.
  The cache-hit guard rejects a stored `nullptr` and erases it (documented at 12539–12543), so
  negative caching self-heals; positive caching never expires.
- `override_class_lookup(name, k)` (12531) — last-writer-wins, process-global, no scoping, no
  refcount.
- `evict_class_lookup(name)` (12549) — the only invalidation.
- `reanchor_classes_via_oop(anchor_oop, {names...})` (12574) — for each name,
  `find_class_via_oop(anchor, name)` then `override_class_lookup`. Returns `true` only if **every**
  name resolved.
- `type_to_class_map` / `g_type_factory_map` are **append-only**; there is no `unregister_class`.
- `g_field_cache` is keyed on raw `klass*` and is **never invalidated**. The comment at 12694–12697
  admits it: *"Raw klass pointers are safe as keys for process-lifetime injection; class unloading
  would invalidate them, but that is not a concern for the typical use case here."*

### 4.4 Class reloads and multiple classloaders — the honest state

`find_class_via_oop` is now a **stub**. Its body (12502–12514):

> `Pure-VM: the former JNI path called anchor.getClass().getClassLoader().loadClass(name), which
> both disambiguated the loader AND triggered class loading.  Without JNI, validate the anchor and
> return the class via the global ClassLoaderDataGraph walk, which finds it across every loader
> once it is LOADED.  Loader-specific selection among multiple loaded copies, and
> loadClass-triggered class loading, are not available pure-VM.`

So: **loader disambiguation does not work today.** `find_class` walks the whole
`ClassLoaderDataGraph` (8425 → `class_loader_data_graph::find_klass` 4761) and returns the *first*
match across all loaders. `reanchor_classes_via_oop` therefore reduces to "re-run the same global
walk and pin the result in the cache" — useful for *timing* (re-resolve after a late class load),
useless for *loader selection*.

On a class reload (new `ClassLoaderData`, same name): the old `klass*` stays in
`klass_lookup_cache` and in `g_field_cache`, and every `object_base` resolving through
`type_to_class_map` keeps hitting the stale metaspace pointer. The documented remedy is a manual
`evict_class_lookup` + `reanchor_classes_via_oop`. **A handle model for `klass*` (a `class_ref` that
re-resolves by name, or that registers with a class-unload watcher) would be a separate but
adjacent win.**

---

## 5. Extension points

### 5.1 Where a new handle type should live

**Recommended placement: a new section immediately after `using oop_t` (15933) and immediately
before `class object_base` (15978).** Concretely, insert between line 15933 and the `object_base`
doc block at 15935.

Justification from the ordering constraints:

- The handle needs `hotspot::decode_oop_pointer` / `encode_oop_pointer` (defined 5634 / 5680),
  `hotspot::is_valid_pointer` (2080), `os::safe_read`/`safe_write` (988 / 1081), and
  `hotspot::klass::get_java_mirror`'s `OopHandle` trick (3711) — all complete by line 8369.
- It needs `hotspot::java_thread` (4951) if the root lives in a thread-local handle block, and
  `java_thread::allocate_tlab` (5189) / `find_allocation_thread` (5437) if the root storage is
  heap-allocated. All complete by 5526.
- It must be visible to `object_base` (15978), `field_proxy` (13461), `method_proxy` (14859), and
  every collection ctor (17329+). `field_proxy` and `method_proxy` come *before* 15933 — so if the
  handle is to appear in their signatures, either
  (a) place the handle **before** `field_proxy` at ~13455 (right after the array-element helpers at
      13255–13432, and before the `// --- Field proxy ---` marker at 13433), or
  (b) place it at 15933 and forward-declare it near 1709–1717 with the other forward declarations,
      keeping the *definition* late and only using `ref` by reference/pointer in
      `field_proxy`/`method_proxy` members.
- **Option (a) is cleaner.** At 13433 everything the handle needs is already defined
  (`make_java_object` is forward-declared at 12601 and defined at 12910, both before 13433), and
  `field_proxy` / `method_proxy` / `object_base` / the collections all follow. The only thing that
  comes *later* and might be needed is `read_java_string` (18468) and `klass_from_oop` (17070) — both
  already forward-declared at 1731 and reachable.

**Namespace:** top-level `namespace vmhook` alongside `oop_t`, not `vmhook::jni` (that namespace is
vestigial — no JNI remains anywhere in the header; keep it only for the `global_ref` compat alias).
Implementation internals go in `vmhook::detail`, which is reopened freely (328, 1639, 1761, 8371,
9482, 9820, 12132, 14753, 19004, 19330, 19495, 19663).

**Forward declaration:** add to the existing forward-declaration block at 1709–1717, which already
carries `class object_base; template<typename derived = void> class object; class field_proxy;
class collection; class list; class set; class linked_list; class map; class hash_map;`.

### 5.2 Major section boundaries (for correct placement)

In-file `// --- ` markers plus the structural landmarks:

| Line | Section |
|---|---|
| 1–60 | file doc block (purpose, thread safety, complexity, exception safety) |
| 61–83 | version macros |
| 85–107 | includes |
| 110–200 | platform / arch detection (`VMHOOK_OS_*`, `VMHOOK_ARCH_X86_64`) |
| 200–310 | feature detection (`VMHOOK_HAS_STD_FORMAT`, `VMHOOK_HAS_DEDUCING_THIS`, `VMHOOK_DEBUG_LOGS`) |
| 328–399 | `detail` logging + `VMHOOK_LOG` |
| 401–435 | tags + `class exception` |
| **444–1326** | **`namespace os`** — memory, modules, threads, `safe_read`/`safe_write`; `detail_signal` 901, `detail_dr` 1261 |
| 1328–1360 | `namespace hotspot` fwd (`return_slot`, `method`, `klass`, `symbol`, …) |
| 1361–1576 | `class return_value` + `caller_info` |
| **1575** | `// --- Forward declarations ---` — registry maps, `register_class` decl, wrapper class fwds, string/array helper fwds |
| **1759** | `// --- Compile-time type traits ---` (`is_vector_v`, `is_unique_ptr_v`, `function_traits`, `value_t_convertible_target_v`) |
| **1907 / 1909** | `// --- HotSpot internals ---` / `namespace hotspot` opens |
| 1912–2050 | VMStructs entry types + table resolution + `iterate_struct_entries` |
| 2050–2263 | pointer validation + `safe_read_pointer` / cold-read helpers |
| 2263–3438 | `symbol`, `constant_pool`, `const_method`, `method` |
| 3439–4385 | `field_entry_t`, `klass` (+ JDK21 fieldinfo stream decoder at 3918) |
| 4386–4922 | `class_loader_data`, `dictionary`, `class_loader_data_graph` |
| 4923–5234 | `java_thread_state`, `java_thread` |
| **5235** | `// --- Thread management ---` |
| **5526** | `// --- OOP and Klass pointer encoding / decoding ---` |
| 5915–6239 | pattern scan, `find_hook_location`, `allocate_nearby_memory` |
| 6240–6796 | `detour_function_t`, `method_args`, **`struct frame`** (6326) |
| 6797–7383 | `class midi2i_hook` (trampoline) |
| 7384–7650 | hook registries + globals + `seh_invoke_detour` (7498) + `common_detour` (7563) |
| 7651–8368 | method flags / `set_dont_inline` / adapter+c2i layer |
| 8369 | `namespace hotspot` closes |
| **8385** | `// --- Cache and class lookup ---` — `klass_lookup_cache`, `find_class` (8425), `resolve_array_klass` |
| 8678–9418 | `for_each_loaded_class`, deopt sweeps, `thread_info`, `for_each_thread`, `for_each_instance(_of)` |
| 9419–9628 | `register_class` def, `collect_klass_methods`, `get_class_methods`, `find_methods_by_signature` |
| 9629–9817 | watchers preamble: `watch_handle` (9647), `hook_handle` (9753) |
| **9818** | `// --- Internal helpers for typed hook API ---` — `function_traits`, `java_slot_offsets`, `extract_frame_arg` (10066) |
| 10232–10684 | `return_value::caller` / `stack_trace` / `set_arg` out-of-line defs |
| **10685** | `// --- Hooking ---` — `hook<T>` (10742/10763), `verify_hooks` (11193) |
| 11505–11745 | `detail::auto_repair` watchdog + `auto_repair_enabled` / `set_auto_repair_enabled` |
| 11748–12126 | `shutdown_hooks`, `hook_handle::stop`, `hook_by_signature`, `scoped_hook` |
| **12127** | `// --- JNI helper layer ---` (**now empty of JNI**; holds `detail` helpers 12132–12461) |
| 12464–12618 | `find_class_via_oop`, `override_class_lookup`, `evict_class_lookup`, `reanchor_classes_via_oop` |
| 12619–12689 | `make_unique<T>` |
| **12690** | `// --- Field access ---` — `g_field_cache`, `find_field`, `get_field<T>`, `set_field<T>` |
| **12886** | `// --- Java object / array / string allocation helpers ---` — `make_java_object` (12910), `make_java_array` (13034), `make_java_string` (13100) |
| **13255** | `// --- Array element access ---` — `clamp_safe_container_count`, `array_length`, `get_array_element`, `set_array_element` |
| **13433** | `// --- Field proxy ---` — `class field_proxy` (13461) ← **recommended handle insertion point is just above this marker** |
| **14750** | `// --- detail helpers that depend on hotspot types ---` (`find_call_stub_entry`) |
| **14842** | `// --- Method proxy ---` — `class method_proxy` (14859) |
| **15913** | `// --- Object base class ---` — `oop_type_t` (15924), `oop_t` (15933), `object_base` (15978), `object<derived>` (16945), `klass_from_oop` (17070), `is_instance_of` (17160), `oop_reflective_base` (17203) |
| **17057** | `// --- Built-in Java collection wrappers ---` — walk fwds (17292), `collection` (17316), `list`/`set`/`linked_list`/`map`/`hash_map` |
| **17736** | `// --- Collection / Map / Set walk helpers ---` — `read_compressed_oop_at` (17773) + the five walk defs |
| 18349–18427 | out-of-line `field_proxy::value_t::to_vector` / `to_entries` |
| **18428** | `// --- Helper: read a Java String OOP to std::string ---` — `read_java_string` (18468), `field_oop` (18734), `write_java_string` (18757), `set_str_field` (18833), `set_bool_array` (18852), `set_prim_array` (18884), `set_str_array` (18957) |
| **18974** | `// --- Helper: decode compressed array OOP ---` — `decode_array_oop` (18981) |
| 19004–19223 | `detail` hardware-breakpoint plumbing (`VMHOOK_HAS_HW_DATA_BREAKPOINTS`) |
| 19224–19325 | `watch_static_field` |
| 19326–19494 | class-load watcher registry + `on_class_loaded` (19383) |
| 19495–19662 | exception watcher registry + `on_exception` (19549) |
| 19663–19686 | `detail::reset_watcher_latches` |
| **19687–19822** | JNI global-reference section — `namespace jni` (19700), `global_ref` (19725), `pin` (19803/19815) |
| 19823 | `}` closes `namespace vmhook` |

### 5.3 Ordering constraints a new handle must respect

1. **Two-phase lookup / GCC `-Wtemplate-body`.** GCC's first-phase lookup inside templates is strict
   about qualified names. The header works around this three times with explicit forward
   declarations placed early and definitions late: `detail::jvm_primitive_byte_width` (1653–1660),
   `detail::auto_repair::ensure_started` (10731), `detail::reset_watcher_latches` (11746). If the
   handle is used inside a template body before its definition, follow the same pattern.
2. **Incomplete-type-in-`unique_ptr`-destructor.** The comment at 1692–1701 records that libstdc++
   and libc++ eagerly instantiate `sizeof(object_base)` inside `unique_ptr`'s destructor
   `static_assert` when a lambda returning `std::unique_ptr<object_base>` is *parsed*. That is why
   `type_factory_function_t` returns a raw `object_base*`. Any handle that owns a wrapper must
   avoid the same trap.
3. **Mutual recursion between wrapper types.** Handled today by split decl/def (see the wrapper
   generator's approach and the walk-helper fwds at 17292–17301). A handle appearing in wrapper
   constructor signatures must be complete before line 13461.
4. **`value_t_convertible_target_v` (in `detail`, ~1800s).** It deliberately excises `const char*`,
   `char*`, `W*`, and `std::nullptr_t` from the conversion-operator productions so
   `static_cast<std::string>` and `static_cast<std::unique_ptr<W>>` stay unambiguous on MSVC
   `/permissive-`. **Adding a `ref` alternative to `value_t` or a `ref` conversion target requires
   re-auditing this trait**, or MSVC will start reporting ambiguity.

---

## 6. Conventions (house style)

### 6.1 Naming

- `snake_case` for everything: types, functions, variables, template parameters.
- Template parameters end in `_type` or `_t`: `wrapper_type`, `element_type`, `value_type`,
  `visitor_type`, `callback_type`, `args_t`, `out_t`, `traits_t`.
- Type aliases end in `_t`: `oop_type_t`, `oop_t`, `field_entry_t`, `thread_id_t`,
  `detour_function_t`, `type_factory_function_t`, `value_t`, `base_t`, `clean_target_type`.
- Globals get a `g_` prefix: `g_field_cache`, `g_hooked_methods`, `g_type_factory_map`,
  `g_shutdown_requested`, `g_auto_repair_enabled`. Constants get `k_`: `k_max_hooked_methods`,
  `k_max_safe_container_elems`.
- Members are always addressed as `this->member` inside function bodies — universal, no exceptions
  observed.
- HotSpot mirror types keep HotSpot's field names verbatim in strings (`"_methods"`,
  `"_java_mirror"`, `"_metadata._compressed_klass"`).

### 6.2 Declaration style

- **Trailing return types everywhere**, and the `-> T` goes on its **own line**, indented to match
  the body's opening brace level minus one:

```cpp
        auto get_compressed_oop() const noexcept
            -> std::uint32_t
        {
```

  Very short accessors occasionally inline the arrow (`auto get_super() const noexcept -> klass*`,
  3768), but the two-line form dominates.
- **Brace initialisation everywhere**, including for locals with deduced types:
  `const std::int32_t safe_n{ vmhook::clamp_safe_container_count(n) };`,
  `void* const mirror{ mirror_klass->get_java_mirror() };`,
  `std::lock_guard<std::mutex> lock{ vmhook::registration_mutex };`.
- `const` on locals is aggressive: `void* const`, `const auto`, `vmhook::hotspot::klass* const`.
- Braces on their own line (Allman), 4-space indent, no tabs.
- Free functions in namespace `vmhook` are marked `inline` or `static` (both appear; `static` is
  used for the older API — `find_class`, `hook`, `register_class`, `get_field`, `make_unique`,
  `shutdown_hooks`, `verify_hooks` — and `inline` for newer additions). `/wd4505` in the MSVC flag
  set exists precisely because the header carries many unreferenced `static` functions by design.

### 6.3 Doxygen comment blocks

Not `///` and not `/**` — a plain `/* ... */` block with `@`-tags inside, indented to the
declaration:

```cpp
    /*
        @brief Finds a loaded Java class by its internal name using HotSpot internals only.
        @param class_name The internal JVM class name using '/' separators
                          (e.g. "java/lang/String", "net/minecraft/client/Minecraft").
        @return Pointer to the matching klass if found, nullptr otherwise.
        @details
        Searches all loaded classloaders for a class matching the given name by walking
        the HotSpot ClassLoaderDataGraph entirely via gHotSpotVMStructs, without any
        JNI or JVMTI calls. Results are cached on first lookup.

        Thread-safe: cache reads and writes are serialised via
        klass_lookup_cache_mutex.  The HotSpot walk itself is safe to run
        concurrently — it only reads exported VMStructs.
    */
    static auto find_class(const std::string_view class_name)
        -> vmhook::hotspot::klass*
```

Tag order in practice: `@brief`, `@details`, then `Complexity:` / `Exception safety:` /
`Thread safety:` as free-text lines, then `@tparam`, `@param`, `@return`, `@note`, `@see`.
The Complexity/Exception-safety/Thread-safety triple is close to mandatory on public functions and
is mirrored in the file-level doc block (23–60). Longer entries embed a usage `Example:` or an
`@code`/`@endcode` fence (see `for_each_thread` 8940–8949, `return_value::stack_trace` 1530–1540).

**Second exemplary block — the "why", not the "what"** (this style of long inline rationale is
pervasive and is arguably the house signature):

```cpp
        /*
            @brief Returns true if this field's JVM descriptor is a reference / array (L or [).
            @details
            Provided so callers can branch on "is this a reference field?" without
            inspecting signature()[0] by hand.  get_compressed_oop() only makes sense
            for reference / array fields; calling it on a primitive field reads the
            first 4 bytes of the primitive's storage as if they were a compressed
            OOP, which decodes to garbage.

            Complexity: O(1).
            Exception safety: noexcept — returns false on empty signature.
        */
```

**Third — the failure-mode narrative inside a body** (14606–14614, `store_object_oop`):

```cpp
            GC-SAFETY OF THE VALUE: the field slot is itself a GC root once the
            reference lands in it, but the window between allocating `decoded_oop`
            and this store is not — the CALLER must keep `decoded_oop` reachable
            (e.g. via a live JNI local reference) across this call so an
            interleaved GC cannot collect or relocate it out from under the write.
```

Comments regularly cite audit findings by tag (`FLAW A fix`, `FLAW B fix`, `FLAW C fix`,
`robustness bug #29`, `audit hook_arg_decoding_primitives`) and reference other line numbers.
Any new code should carry the same density of rationale.

### 6.4 Error handling — the three-tier contract

Stated in the file doc block (49–59) and honoured throughout:

1. **Public API: catch and degrade.** Every public function either is `noexcept` or wraps its body
   in `try { ... } catch (const std::exception& ex) { VMHOOK_LOG(...); return <safe default>; }`.
   Safe defaults are `nullptr`, `std::nullopt`, `false`, `0`, `{}`, or `T{}`. No exception ever
   escapes the library boundary.
2. **Internal helpers: `noexcept`, return null.** `iterate_struct_entries`, `iterate_type_entries`,
   `safe_read_pointer`, `is_valid_pointer` etc. are `noexcept` and signal failure by value.
3. **`vmhook::exception` (418) is thrown only internally**, and only by the value-returning
   templates `get_field<T>` (12784) / `set_field<T>` (12843) and `midi2i_hook`'s ctor — always
   caught by the immediate public caller.

Additional invariants that a new feature must uphold:

- **Never raw-deref a HotSpot pointer.** Every cold read goes through `os::safe_read` /
  `hotspot::safe_read_pointer` / `cold_read_metadata_pointer`. The reason is spelled out in ~20
  places: MinGW and clang-on-windows have **no working SEH**, so a hardware AV inside
  `seh_invoke_detour`'s `catch(...)` is uncontained and kills the JVM. `is_valid_pointer` is
  explicitly described as "only a range+alignment+poison heuristic".
- **`static_assert` for compile-time contracts**, with `detail::dependent_false_v<T>` (1757-ish) for
  discarded-branch assertions (C++23 P2593R1). Example: `get_field<T>` asserts
  `std::is_trivially_copyable_v<value_type>`; `pin(unique_ptr<T>)` asserts
  `std::is_base_of_v<object_base, wrapper_type>`.
- **Silent no-ops are treated as bugs.** `field_proxy::set` ends its `if constexpr` chain with a
  `static_assert` whose message reads "…silently no-op'd - the set call returned and the field was
  unchanged with no compile-time error or runtime warning" (14455–14464).
- **Mutex discipline.** `registration_mutex`, `klass_lookup_cache_mutex`, `g_field_cache_mutex`,
  `g_hooked_methods_mutex`, `class_load_mutex`, `exception_mutex`, `dr_mutex`. The documented
  contract (file doc 23–27) is that read-only operations are *not* thread-safe and the caller
  synchronises; the mutexes exist to protect the caches against rehash-during-insert corruption.

### 6.5 Warnings-as-errors and the compiler matrix

`CMakeLists.txt:101–102`:

```cmake
option(VMHOOK_WARNINGS_AS_ERRORS "Treat warnings as errors for first-party code"
       OFF)
```

Applied by `function(vmhook_apply_warnings target)` (`CMakeLists.txt:137–181`). A second positional
argument `NO_WERROR` keeps the warning set but skips escalation.

**MSVC branch** (`CMakeLists.txt:142–162`) — note `if(MSVC)` is also true for **clang-cl**, so
clang-cl takes this branch, not the GCC/Clang one:

```
/W4 /permissive- /Zc:__cplusplus /utf-8 /wd4505 /wd4101
/EHa                    # load-bearing for seh_invoke_detour (async SEH + C++ unwind)
/WX                     # when VMHOOK_WARNINGS_AS_ERRORS AND NOT NO_WERROR
-DWIN32_LEAN_AND_MEAN -DNOMINMAX -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00
```

CMake's default `/EHsc` is regex-scrubbed out of `CMAKE_CXX_FLAGS` at `CMakeLists.txt:17–20` so
`/EHa` is the sole EH model (otherwise D9025 + `/WX` is a hard error).

**GCC / Clang branch** (`CMakeLists.txt:163–180`) — MinGW g++, clang++, Apple clang:

```
-Wall -Wextra -Wpedantic
-Wno-unused-function -Wno-unused-local-typedefs -Wno-unused-but-set-variable -Wno-cast-function-type
-Werror                 # when VMHOOK_WARNINGS_AS_ERRORS AND NOT NO_WERROR
```

Standard: `CMAKE_CXX_STANDARD 23`, `CMAKE_CXX_STANDARD_REQUIRED ON`, `CMAKE_CXX_EXTENSIONS OFF`
(`CMakeLists.txt:108–110`). Minimum CMake 3.20; project version 0.5.3.

**CI matrix** (`.github/workflows/ci.yml`):

- `build-and-unit-test` (6 cells): windows/msvc, windows/clang, windows/mingw, linux/gcc-14,
  linux/clang-18, macos/clang.
- JDK fan-out: `["8","11","17","21","24","25","26"]`.
- `jvm-windows` = 3 compilers × 7 JDKs = 21 cells; `jvm-linux` = 2 × 7 = 14;
  `jvm-macos` = 1 × 6 = 6 (Java 8 excluded, no Temurin 8 for macOS arm64). **41 JVM cells total.**
- `android-build` (NDK arm64-v8a + x86_64), `ios-build` (iphoneos + iphonesimulator).
- `warnings-as-errors` job: **Linux only** — `g++-14` and `clang++-18`, `-DVMHOOK_WARNINGS_AS_ERRORS=ON`.

> **Gap worth flagging to the owner:** `/WX` (the MSVC/clang-cl escalation at `CMakeLists.txt:158`)
> is **never exercised in CI** — no job sets `VMHOOK_WARNINGS_AS_ERRORS=ON` on a Windows runner.
> MSVC-only and clang-cl-only warnings can regress unnoticed. A handle type using
> deducing-this / concepts / `std::variant` changes is exactly the kind of code that trips
> MSVC-only diagnostics.

Call sites: `CMakeLists.txt:251` `vmhook_apply_warnings(vmhook_example NO_WERROR)` (JVM test modules
exempt), `CMakeLists.txt:265` `vmhook_apply_warnings(vmhook_injector)` (strict),
`tests/CMakeLists.txt:16` per-test (strict). `viewer/CMakeLists.txt` is a separate sub-project with
its own flags and **no** `-Werror`.

Practical rule, from the project memory and confirmed by the config: validate locally with
`-DVMHOOK_WARNINGS_AS_ERRORS=ON`; **MinGW alone misses MSVC-ABI failures**, so a header change of
this size needs at minimum a MinGW g++ pass *and* an MSVC `cl.exe` pass before push.

---

## 7. Summary of what a handle model must decide

1. **Where the GC root lives** without JNI. `klass::get_java_mirror` (3711) already reads through a
   HotSpot `OopHandle` indirection cell — that is the existing proof-of-concept in this header.
2. **Whether `oop_t` stays `void*`.** Recommendation: yes (breaking it breaks `cast_for_variant`'s
   `void*` branch at 13950 and the `value_t_convertible_target_v` disambiguation), and introduce a
   distinct `vmhook::ref` alongside.
3. **Handle placement:** just above `// --- Field proxy ---` (13433), forward-declared in the block
   at 1709–1717.
4. **The minimum viable intercept set** to cover 90 % of user exposure: `object_base` ctor +
   `get_instance` (A2/A3), `detail::extract_frame_arg` (A9), `field_proxy` ctors + `store_object_oop`
   (B1/B2/B5), `method_proxy` receiver (C1), the six collection ctors (E1–E6), and
   `make_java_object`/`make_java_array`/`make_java_string` (D1–D3).
5. **`jni::global_ref` must be replaced, not extended** — it is a no-op today and its `oop()`
   docstring is an admission of that.
