# vmhook feature index (auto-generated)

This index is rebuilt from `audit/features/*.yaml`.  Edit the manifests, not this file.

- **Features:** 109
- **Categories:** 13
- **`depends_on` edges:** 246

## Status

- `queued`: 0
- `seeded`: 58
- `in_progress`: 51
- `audited`: 0
- `perfected`: 0

## By category

### [[categories/bootstrap|Bootstrap / DLL entry]] · 1 feature(s)

- [[features/dllmain_bootstrap|dllmain_bootstrap]] — `in_progress` / `high` — Dllmain Bootstrap

### [[categories/hook|Hooking machinery (install / dispatch / trampolines)]] · 13 feature(s)

- [[features/adapter_recovery_c2i|adapter_recovery_c2i]] — `in_progress` / `high` — Adapter Recovery C2I
- [[features/dont_inline_dont_compile|dont_inline_dont_compile]] — `in_progress` / `critical` — Dont Inline Dont Compile
- [[features/hook_basic|hook_basic]] — `in_progress` / `critical` — Hook Install (basic)
- [[features/hook_chaining|hook_chaining]] — `in_progress` / `high` — Hook Chaining
- [[features/hook_common_detour_dispatch|hook_common_detour_dispatch]] — `in_progress` / `critical` — Hook Common Detour Dispatch
- [[features/hook_install_after_jit|hook_install_after_jit]] — `in_progress` / `high` — Hook Install After Jit
- [[features/hook_reinstall_after_shutdown|hook_reinstall_after_shutdown]] — `in_progress` / `high` — Hook Reinstall After Shutdown
- [[features/hook_signature|hook_signature]] — `in_progress` / `medium` — Hook Signature
- [[features/hook_unhook_double_free|hook_unhook_double_free]] — `in_progress` / `high` — Hook Unhook Double Free
- [[features/hook_verify_repair|hook_verify_repair]] — `in_progress` / `critical` — Hook Verify Repair
- [[features/method_entry_points_i2i_i2c|method_entry_points_i2i_i2c]] — `seeded` / `critical` — Method Entry Points I2I I2C
- [[features/midi2i_trampoline_alloc|midi2i_trampoline_alloc]] — `seeded` / `critical` — Midi2I Trampoline Alloc
- [[features/seh_invoke_detour|seh_invoke_detour]] — `seeded` / `high` — Seh Invoke Detour

### [[categories/deopt|De-optimisation]] · 1 feature(s)

- [[features/deoptimize_methods|deoptimize_methods]] — `in_progress` / `medium` — Deoptimize Methods

### [[categories/klass|Class / Klass introspection]] · 15 feature(s)

- [[features/classloader_reanchor|classloader_reanchor]] — `in_progress` / `medium` — Classloader Reanchor
- [[features/compressed_klass_decode|compressed_klass_decode]] — `in_progress` / `medium` — Compressed Klass Decode
- [[features/compressed_oops_decode|compressed_oops_decode]] — `in_progress` / `medium` — Compressed Oops Decode
- [[features/const_method_bounds|const_method_bounds]] — `in_progress` / `medium` — Const Method Bounds
- [[features/constantpool_access|constantpool_access]] — `in_progress` / `high` — Constantpool Access
- [[features/decode_oop_and_pointers|decode_oop_and_pointers]] — `in_progress` / `medium` — Decode Oop And Pointers
- [[features/find_class_context_loader|find_class_context_loader]] — `in_progress` / `high` — Find Class Context Loader
- [[features/find_class_fallback|find_class_fallback]] — `in_progress` / `medium` — Find Class Fallback
- [[features/instanceklass_methods_walk|instanceklass_methods_walk]] — `seeded` / `high` — Instanceklass Methods Walk
- [[features/interface_polymorphism|interface_polymorphism]] — `seeded` / `medium` — Interface Polymorphism
- [[features/klass_introspection|klass_introspection]] — `seeded` / `high` — Klass Introspection
- [[features/nested_classes|nested_classes]] — `seeded` / `medium` — Nested Classes
- [[features/poly_inherited_oop|poly_inherited_oop]] — `seeded` / `medium` — Poly Inherited Oop
- [[features/register_class|register_class]] — `seeded` / `medium` — Register Class
- [[features/vmstructs_offset_resolution|vmstructs_offset_resolution]] — `seeded` / `high` — Vmstructs Offset Resolution

### [[categories/method|Method proxies (resolve / call / dispatch)]] · 19 feature(s)

- [[features/find_methods_by_signature|find_methods_by_signature]] — `in_progress` / `low` — Find Methods By Signature
- [[features/method_call_jni_fallback|method_call_jni_fallback]] — `seeded` / `high` — Method Call Jni Fallback
- [[features/method_call_object|method_call_object]] — `seeded` / `high` — Method Call Object
- [[features/method_call_primitives|method_call_primitives]] — `seeded` / `medium` — Method Call Primitives
- [[features/method_call_return_void|method_call_return_void]] — `seeded` / `medium` — Method Call Return Void
- [[features/method_call_string|method_call_string]] — `seeded` / `high` — Method Call String
- [[features/method_call_wide_args|method_call_wide_args]] — `seeded` / `medium` — Method Call Wide Args
- [[features/method_enumeration|method_enumeration]] — `seeded` / `high` — Method Enumeration
- [[features/method_explicit_signature|method_explicit_signature]] — `seeded` / `medium` — Method Explicit Signature
- [[features/method_flags_width|method_flags_width]] — `seeded` / `high` — Method Flags Width
- [[features/method_is_reference|method_is_reference]] — `seeded` / `low` — Method Is Reference
- [[features/method_overload|method_overload]] — `seeded` / `high` — Method Overload
- [[features/method_overload_java_dispatch|method_overload_java_dispatch]] — `seeded` / `medium` — Method Overload Java Dispatch
- [[features/method_proxy_value_t|method_proxy_value_t]] — `seeded` / `medium` — Method Proxy Value T
- [[features/method_return_types|method_return_types]] — `seeded` / `medium` — Method Return Types
- [[features/method_static|method_static]] — `seeded` / `high` — Method Static
- [[features/method_static_portability|method_static_portability]] — `seeded` / `medium` — Method Static Portability
- [[features/method_throwing_call_site|method_throwing_call_site]] — `seeded` / `high` — Method Throwing Call Site
- [[features/signature_parsing|signature_parsing]] — `seeded` / `medium` — Signature Parsing

### [[categories/field|Field proxies (get / set / introspection)]] · 14 feature(s)

- [[features/field_arrays_object|field_arrays_object]] — `in_progress` / `medium` — Field Arrays Object
- [[features/field_arrays_primitive|field_arrays_primitive]] — `in_progress` / `high` — Field Arrays Primitive
- [[features/field_inherited|field_inherited]] — `in_progress` / `medium` — Field Inherited
- [[features/field_introspection|field_introspection]] — `in_progress` / `medium` — Field Introspection
- [[features/field_null_safety|field_null_safety]] — `in_progress` / `low` — Field Null Safety
- [[features/field_object_ref|field_object_ref]] — `in_progress` / `medium` — Field Object Ref
- [[features/field_primitives_get|field_primitives_get]] — `in_progress` / `high` — Field Primitives Get
- [[features/field_primitives_set|field_primitives_set]] — `in_progress` / `medium` — Field Primitives Set
- [[features/field_proxy_set_guards|field_proxy_set_guards]] — `in_progress` / `medium` — Field Proxy Set Guards
- [[features/field_proxy_value_t|field_proxy_value_t]] — `in_progress` / `medium` — Field Proxy Value T
- [[features/field_set_size_guard|field_set_size_guard]] — `in_progress` / `low` — Field Set Size Guard
- [[features/field_static|field_static]] — `in_progress` / `medium` — Field Static
- [[features/field_string|field_string]] — `in_progress` / `high` — Field String
- [[features/watch_static_field|watch_static_field]] — `seeded` / `high` — Watch Static Field

### [[categories/collection|Collection wrappers + element helpers]] · 8 feature(s)

- [[features/array_element_helpers|array_element_helpers]] — `in_progress` / `high` — Array Element Helpers
- [[features/collection_hash_tree_map|collection_hash_tree_map]] — `in_progress` / `high` — Collection Hash Tree Map
- [[features/collection_iteration_safety|collection_iteration_safety]] — `in_progress` / `medium` — Collection Iteration Safety
- [[features/collection_linked_list|collection_linked_list]] — `in_progress` / `medium` — Collection Linked List
- [[features/collection_list|collection_list]] — `in_progress` / `medium` — Collection List
- [[features/collection_map|collection_map]] — `in_progress` / `high` — Collection Map
- [[features/collection_set|collection_set]] — `in_progress` / `medium` — Collection Set
- [[features/collection_type_tags|collection_type_tags]] — `in_progress` / `medium` — Collection Type Tags

### [[categories/return|return_value (detour-side return manipulation)]] · 8 feature(s)

- [[features/interpreter_frame_walk|interpreter_frame_walk]] — `seeded` / `high` — Interpreter Frame Walk
- [[features/return_caller|return_caller]] — `seeded` / `high` — Return Caller
- [[features/return_frame_raw_access|return_frame_raw_access]] — `seeded` / `high` — Return Frame Raw Access
- [[features/return_set_arg|return_set_arg]] — `seeded` / `high` — Return Set Arg
- [[features/return_set_primitives|return_set_primitives]] — `seeded` / `medium` — Return Set Primitives
- [[features/return_set_wrapper_null|return_set_wrapper_null]] — `seeded` / `medium` — Return Set Wrapper Null
- [[features/return_stack_trace_depth|return_stack_trace_depth]] — `seeded` / `high` — Return Stack Trace Depth
- [[features/return_value_cancel|return_value_cancel]] — `seeded` / `medium` — Return Value Cancel

### [[categories/jni|JNI plumbing (arg packing / refs / java values)]] · 6 feature(s)

- [[features/global_ref|global_ref]] — `in_progress` / `medium` — Global Ref
- [[features/jni_arg_packing|jni_arg_packing]] — `seeded` / `medium` — Jni Arg Packing
- [[features/jni_local_ref_hygiene|jni_local_ref_hygiene]] — `seeded` / `high` — Jni Local Ref Hygiene
- [[features/make_java_array|make_java_array]] — `seeded` / `high` — Make Java Array
- [[features/make_java_string|make_java_string]] — `in_progress` / `medium` — Make Java String
- [[features/read_java_string|read_java_string]] — `seeded` / `medium` — Read Java String

### [[categories/enumeration|Live-VM enumeration (heap / classes / threads)]] · 4 feature(s)

- [[features/for_each_instance|for_each_instance]] — `in_progress` / `medium` — For Each Instance
- [[features/for_each_loaded_class|for_each_loaded_class]] — `in_progress` / `medium` — For Each Loaded Class
- [[features/for_each_thread|for_each_thread]] — `in_progress` / `medium` — For Each Thread
- [[features/iterate_entries_safety|iterate_entries_safety]] — `seeded` / `high` — Iterate Entries Safety

### [[categories/os|OS abstraction (memory / signals / breakpoints)]] · 7 feature(s)

- [[features/hw_breakpoint_dr7|hw_breakpoint_dr7]] — `seeded` / `low` — Hw Breakpoint Dr7
- [[features/os_allocate_release|os_allocate_release]] — `seeded` / `high` — Os Allocate Release
- [[features/os_page_size_granularity|os_page_size_granularity]] — `seeded` / `medium` — Os Page Size Granularity
- [[features/os_protect|os_protect]] — `seeded` / `high` — Os Protect
- [[features/os_query_region|os_query_region]] — `seeded` / `medium` — Os Query Region
- [[features/os_safe_read|os_safe_read]] — `seeded` / `critical` — Os Safe Read
- [[features/os_signal_handler|os_signal_handler]] — `seeded` / `critical` — Os Signal Handler

### [[categories/lifecycle|Lifecycle hooks (shutdown / class-load / exception / enum)]] · 4 feature(s)

- [[features/enum_singleton|enum_singleton]] — `in_progress` / `medium` — Enum Singleton
- [[features/on_class_loaded|on_class_loaded]] — `seeded` / `medium` — On Class Loaded
- [[features/on_exception|on_exception]] — `seeded` / `medium` — On Exception
- [[features/shutdown_hooks_teardown|shutdown_hooks_teardown]] — `seeded` / `high` — Shutdown Hooks Teardown

### [[categories/infra|Infrastructure (wrappers, traits, macros, logging)]] · 9 feature(s)

- [[features/api_surface_no_jvm|api_surface_no_jvm]] — `in_progress` / `medium` — API Surface (no-JVM contract)
- [[features/decode_u5_unsigned5|decode_u5_unsigned5]] — `in_progress` / `high` — Decode U5 Unsigned5
- [[features/logging_format|logging_format]] — `seeded` / `low` — Logging Format
- [[features/make_unique|make_unique]] — `seeded` / `high` — Make Unique
- [[features/platform_capability_macros|platform_capability_macros]] — `seeded` / `medium` — Platform Capability Macros
- [[features/traits_function_traits|traits_function_traits]] — `seeded` / `medium` — Traits Function Traits
- [[features/unified_call_syntax|unified_call_syntax]] — `seeded` / `medium` — Unified Call Syntax
- [[features/version_macros|version_macros]] — `seeded` / `low` — Version Macros
- [[features/wrapper_pattern|wrapper_pattern]] — `in_progress` / `high` — Wrapper Pattern
