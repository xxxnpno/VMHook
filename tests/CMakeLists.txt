cmake_minimum_required(VERSION 3.20)

# Each test below either compiles the header in isolation (catches compile-time
# regressions for each compiler/platform combo) or exercises pieces of the API
# that work without a live HotSpot JVM (the os::* shims, type traits, etc.).

# Helper to register a test executable with the common configuration.
function(vmhook_add_test name)
    set(options)
    set(oneValueArgs)
    set(multiValueArgs SOURCES)
    cmake_parse_arguments(_t "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    add_executable("vmhook_test_${name}" ${_t_SOURCES})
    target_link_libraries("vmhook_test_${name}" PRIVATE vmhook::vmhook)
    vmhook_apply_warnings("vmhook_test_${name}")

    # Apple Xcode-generated builds need a bundle identifier on every
    # binary they produce; CMake doesn't fill one in by default.  iOS
    # builds further require code-signing identities (which the
    # GitHub-hosted runners don't have) — we disable signing for the
    # test targets since we never deploy or run them on a device.
    if(VMHOOK_PLATFORM_IOS OR APPLE)
        set_target_properties("vmhook_test_${name}" PROPERTIES
            XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.vmhook.test.${name}"
            XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED          "NO"
            XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY            ""
            XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED         "NO"
            XCODE_ATTRIBUTE_ENABLE_BITCODE                "NO"
            XCODE_ATTRIBUTE_EXPANDED_CODE_SIGN_IDENTITY   "-")
    endif()

    add_test(NAME "${name}" COMMAND "vmhook_test_${name}")
endfunction()

# 1) Smoke-compile the header on its own (no JVM, no other includes).
vmhook_add_test(header_compile SOURCES test_header_compile.cpp)

# 2) Header included from two TUs to catch ODR / inline issues.
vmhook_add_test(odr_two_translation_units
    SOURCES test_odr_unit_a.cpp test_odr_unit_b.cpp)

# 3) Type-trait + concept compile-time checks (no JVM required).
vmhook_add_test(traits_static_assertions SOURCES test_traits.cpp)

# 4) OS abstraction layer — page/protect/allocate round-trip (no JVM required).
vmhook_add_test(os_layer_roundtrip SOURCES test_os_layer.cpp)

# 5) Static API surface — make sure register_class, hook<>, etc. are callable
#    with the right signatures even when no JVM is present (they no-op).
vmhook_add_test(api_surface_no_jvm SOURCES test_api_surface.cpp)

# 6) Unified `get_field` / `get_method` call syntax across static + instance.
#    Toolchains with C++23 deducing-this resolve the overloads correctly in
#    static context; the file is guarded by VMHOOK_HAS_DEDUCING_THIS so older
#    Clang / GCC compile a reduced subset.
vmhook_add_test(unified_call_syntax SOURCES test_unified_call_syntax.cpp)

# 7) Helper functions — version macros, decode_u5 (UNSIGNED5), pointer
#    filters / untag, JVM sig -> BasicType, memory_protection enum mapping,
#    build_dr7 (Windows-only), array_length/get_array_element/set_array_element
#    on a fake buffer, and format_log fallback.  Pure-logic checks, no JVM.
vmhook_add_test(helpers SOURCES test_helpers.cpp)
target_compile_definitions("vmhook_test_helpers" PRIVATE
    VMHOOK_CMAKE_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    VMHOOK_CMAKE_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    VMHOOK_CMAKE_VERSION_PATCH=${PROJECT_VERSION_PATCH})

# 8) Tighter coverage of vmhook::os: non-page-aligned protect, page-crossing
#    protect, safe_read against a PROT_NONE page, allocation_granularity vs
#    page_size relationship, query_region attributes on a freshly allocated
#    RWX block.  Pure OS-layer checks, no JVM required.
vmhook_add_test(os_protect_interaction SOURCES test_os_protect_interaction.cpp)

# ─────────────────────────────────────────────────────────────────────────
# Expanded coverage (9+): one standalone executable per feature cluster.
# These exercise the no-JVM-testable surface of each feature from every
# angle — pure logic, null-safety, signature parsing, value_t conversions,
# boundary helpers, compile-time capability macros.  All run on every
# OS/compiler in the CI matrix.
# ─────────────────────────────────────────────────────────────────────────
vmhook_add_test(field_proxy_value_conversions SOURCES test_field_proxy_value_conversions.cpp)
vmhook_add_test(field_proxy_set_guards         SOURCES test_field_proxy_set_guards.cpp)
vmhook_add_test(field_set_size_guard_nojvm     SOURCES test_field_set_size_guard_nojvm.cpp)
vmhook_add_test(field_string_cold_nojvm        SOURCES test_field_string_cold_nojvm.cpp)
vmhook_add_test(field_primitives_set_nojvm     SOURCES test_field_primitives_set_nojvm.cpp)
vmhook_add_test(method_proxy_value_t           SOURCES test_method_proxy_value_t.cpp)
vmhook_add_test(method_flags_width             SOURCES test_method_flags_width.cpp)
vmhook_add_test(method_entry_points            SOURCES test_method_entry_points.cpp)
vmhook_add_test(signature_parsing              SOURCES test_signature_parsing.cpp)
vmhook_add_test(decode_oop_and_pointers        SOURCES test_decode_oop_and_pointers.cpp)
vmhook_add_test(compressed_klass_decode        SOURCES test_compressed_klass_decode.cpp)
vmhook_add_test(decode_u5                       SOURCES test_decode_u5.cpp)
vmhook_add_test(iterate_entries_safety         SOURCES test_iterate_entries_safety.cpp)
vmhook_add_test(os_release_and_protect_edges   SOURCES test_os_release_and_protect_edges.cpp)
vmhook_add_test(os_safe_read                   SOURCES test_os_safe_read.cpp)
vmhook_add_test(os_query_region                SOURCES test_os_query_region.cpp)
vmhook_add_test(array_element_helpers          SOURCES test_array_element_helpers.cpp)
vmhook_add_test(return_value                    SOURCES test_return_value.cpp)
vmhook_add_test(object_factory                  SOURCES test_object_factory.cpp)
vmhook_add_test(make_java_array_nojvm           SOURCES test_make_java_array_nojvm.cpp)
vmhook_add_test(version_macros                  SOURCES test_version_macros.cpp)
target_compile_definitions("vmhook_test_version_macros" PRIVATE
    VMHOOK_CMAKE_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    VMHOOK_CMAKE_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    VMHOOK_CMAKE_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    VMHOOK_TEST_EXPECT_CMAKE_VERSION=1)
vmhook_add_test(platform_capability_macros     SOURCES test_platform_capability_macros.cpp)
vmhook_add_test(traits_extra                    SOURCES test_traits_extra.cpp)
vmhook_add_test(api_surface_extended           SOURCES test_api_surface_extended.cpp)
vmhook_add_test(logging_format                  SOURCES test_logging_format.cpp)
vmhook_add_test(collection_type_tags           SOURCES test_collection_type_tags.cpp)
vmhook_add_test(collection_map_cold_nojvm      SOURCES test_collection_map_cold_nojvm.cpp)
vmhook_add_test(collection_linked_list_nojvm   SOURCES test_collection_linked_list_nojvm.cpp)
vmhook_add_test(set_wrapper_nojvm              SOURCES test_set_wrapper_nojvm.cpp)
vmhook_add_test(global_ref                      SOURCES test_global_ref.cpp)
vmhook_add_test(method_enumeration             SOURCES test_method_enumeration.cpp)
vmhook_add_test(method_explicit_signature_nojvm SOURCES test_method_explicit_signature_nojvm.cpp)
vmhook_add_test(method_overload_nojvm           SOURCES test_method_overload_nojvm.cpp)
vmhook_add_test(method_is_reference_nojvm      SOURCES test_method_is_reference_nojvm.cpp)
vmhook_add_test(classloader_reanchor           SOURCES test_classloader_reanchor.cpp)
vmhook_add_test(find_class_contracts           SOURCES test_find_class_contracts.cpp)
vmhook_add_test(traits_function_traits         SOURCES test_traits_function_traits.cpp)
vmhook_add_test(make_unique                     SOURCES test_make_unique.cpp)
vmhook_add_test(os_page_size_granularity       SOURCES test_os_page_size_granularity.cpp)
# logging_format only reaches the std::cout sink (VMHOOK_LOG_FILE undefined).
# The opt-in FILE sink (detail::emit_log_line's `#ifdef VMHOOK_LOG_FILE` arm)
# is selected at compile time, so it gets two dedicated TUs built with
# -DVMHOOK_LOG_FILE set to a per-build path.  VMHOOK_TEST_LOG_PATH carries the
# SAME path as a plain string the test reads back / checks for.  Forward slashes
# work in C++ file opens on every platform incl. Windows; the writable file
# lives directly in CMAKE_BINARY_DIR (which always exists), the unwritable one
# inside a directory that is intentionally never created.
set(_vmhook_log_file "${CMAKE_BINARY_DIR}/vmhook_test_filelog.log")
set(_vmhook_bad_log_file "${CMAKE_BINARY_DIR}/vmhook_no_such_dir/vmhook_test_filelog.log")
vmhook_add_test(logging_logfile                 SOURCES test_logging_logfile.cpp)
target_compile_definitions("vmhook_test_logging_logfile" PRIVATE
    "VMHOOK_LOG_FILE=\"${_vmhook_log_file}\""
    "VMHOOK_TEST_LOG_PATH=\"${_vmhook_log_file}\"")
vmhook_add_test(logging_logfile_unwritable      SOURCES test_logging_logfile_unwritable.cpp)
target_compile_definitions("vmhook_test_logging_logfile_unwritable" PRIVATE
    "VMHOOK_LOG_FILE=\"${_vmhook_bad_log_file}\""
    "VMHOOK_TEST_LOG_PATH=\"${_vmhook_bad_log_file}\"")
vmhook_add_test(const_method_bounds            SOURCES test_const_method_bounds.cpp)
vmhook_add_test(compressed_oops_decode         SOURCES test_compressed_oops_decode.cpp)
vmhook_add_test(vmstructs_offset_resolution    SOURCES test_vmstructs_offset_resolution.cpp)
vmhook_add_test(interpreter_frame_walk         SOURCES test_interpreter_frame_walk.cpp)
vmhook_add_test(midi2i_trampoline_alloc        SOURCES test_midi2i_trampoline_alloc.cpp)
vmhook_add_test(read_java_string_nul_astral    SOURCES test_read_java_string_nul_astral.cpp)
vmhook_add_test(method_call_arg_cap            SOURCES test_method_call_arg_cap.cpp)
vmhook_add_test(method_call_string_cold_nojvm  SOURCES test_method_call_string_cold_nojvm.cpp)
vmhook_add_test(method_call_primitives_nojvm   SOURCES test_method_call_primitives_nojvm.cpp)
vmhook_add_test(method_call_return_void_nojvm  SOURCES test_method_call_return_void_nojvm.cpp)
vmhook_add_test(method_call_return_type_sfinae_nojvm SOURCES test_method_call_return_type_sfinae_nojvm.cpp)
vmhook_add_test(method_call_wide_args_nojvm    SOURCES test_method_call_wide_args_nojvm.cpp)
vmhook_add_test(method_call_object_nojvm       SOURCES test_method_call_object_nojvm.cpp)
# NOTE: the name is now misleading — the JNI invocation fallback this file was
# written against was deleted by the de-JNI refactor, and what it actually
# exercises today is method_proxy::call()'s cold guard arm (return-type
# invariance, the noexcept arity matrix, value_t's variant API, and
# copy/move-proxy idempotence).  Re-registered under its historical name to keep
# this change reviewable; the rename is queued for a follow-up pass.
vmhook_add_test(method_call_jni_fallback_nojvm SOURCES test_method_call_jni_fallback_nojvm.cpp)
vmhook_add_test(seh_invoke_detour              SOURCES test_seh_invoke_detour.cpp)
vmhook_add_test(dont_inline_dont_compile       SOURCES test_dont_inline_dont_compile.cpp)
vmhook_add_test(hook_common_detour_dispatch    SOURCES test_hook_common_detour_dispatch.cpp)
vmhook_add_test(klass_introspection_nojvm      SOURCES test_klass_introspection_nojvm.cpp)
vmhook_add_test(field_introspection_nojvm      SOURCES test_field_introspection_nojvm.cpp)
vmhook_add_test(field_null_safety_nojvm        SOURCES test_field_null_safety_nojvm.cpp)
vmhook_add_test(field_inherited_nojvm          SOURCES test_field_inherited_nojvm.cpp)
vmhook_add_test(field_object_ref_nojvm         SOURCES test_field_object_ref_nojvm.cpp)
vmhook_add_test(field_arrays_object_nojvm      SOURCES test_field_arrays_object_nojvm.cpp)
vmhook_add_test(field_static_nojvm              SOURCES test_field_static_nojvm.cpp)
vmhook_add_test(field_primitives_get_nojvm     SOURCES test_field_primitives_get_nojvm.cpp)
vmhook_add_test(field_arrays_primitive_nojvm   SOURCES test_field_arrays_primitive_nojvm.cpp)
vmhook_add_test(constantpool_access_nojvm      SOURCES test_constantpool_access_nojvm.cpp)
vmhook_add_test(instanceklass_methods_walk_nojvm SOURCES test_instanceklass_methods_walk_nojvm.cpp)
vmhook_add_test(hook_verify_repair_nojvm       SOURCES test_hook_verify_repair_nojvm.cpp)
vmhook_add_test(hook_install_after_jit_nojvm   SOURCES test_hook_install_after_jit_nojvm.cpp)
vmhook_add_test(dllmain_bootstrap_nojvm        SOURCES test_dllmain_bootstrap_nojvm.cpp)
vmhook_add_test(wrapper_pattern_nojvm          SOURCES test_wrapper_pattern_nojvm.cpp)
vmhook_add_test(interface_polymorphism_nojvm   SOURCES test_interface_polymorphism_nojvm.cpp)
vmhook_add_test(poly_inherited_oop_nojvm       SOURCES test_poly_inherited_oop_nojvm.cpp)
vmhook_add_test(make_java_string_nojvm         SOURCES test_make_java_string_nojvm.cpp)
# NOT REGISTERED: test_jni_local_ref_hygiene_nojvm.cpp.  It exists solely to
# exercise vmhook::detail::jni_delete_local_ref (a test-local RAII wrapper used
# the library function as its deleter); that function — and the whole notion of
# a JNI local reference — was deleted by the de-JNI refactor, and pure-VM there
# is no successor to re-point it at.  Recommended for deletion; left in tree so
# the decision is made deliberately rather than silently.
vmhook_add_test(for_each_instance_nojvm        SOURCES test_for_each_instance_nojvm.cpp)
vmhook_add_test(for_each_thread_nojvm          SOURCES test_for_each_thread_nojvm.cpp)
vmhook_add_test(for_each_loaded_class_nojvm    SOURCES test_for_each_loaded_class_nojvm.cpp)
vmhook_add_test(on_exception_install_nojvm     SOURCES test_on_exception_install_nojvm.cpp)
vmhook_add_test(on_class_loaded_install_nojvm  SOURCES test_on_class_loaded_install_nojvm.cpp)
vmhook_add_test(watch_static_field_nojvm       SOURCES test_watch_static_field_nojvm.cpp)
vmhook_add_test(return_frame_raw_access_nojvm  SOURCES test_return_frame_raw_access_nojvm.cpp)
vmhook_add_test(method_static_nojvm            SOURCES test_method_static_nojvm.cpp)
vmhook_add_test(nested_classes_nojvm           SOURCES test_nested_classes_nojvm.cpp)
vmhook_add_test(shutdown_hooks_teardown_nojvm  SOURCES test_shutdown_hooks_teardown_nojvm.cpp)
vmhook_add_test(enum_singleton_nojvm           SOURCES test_enum_singleton_nojvm.cpp)
vmhook_add_test(hook_unhook_double_free_nojvm  SOURCES test_hook_unhook_double_free_nojvm.cpp)
vmhook_add_test(deoptimize_methods_nojvm       SOURCES test_deoptimize_methods_nojvm.cpp)

# Layer 0 (vm_capabilities) + Layer 1 (gc_epoch / relocation detector) —
# graceful-degradation contract with no JVM in the process: capabilities read
# unknown/unsupported, gc_epoch() fails closed instead of faulting, and a
# global_ref built on a fabricated address reports stale rather than handing
# the address back.  Live-VM behaviour is out of scope here by construction.
vmhook_add_test(gc_relocation_detector_nojvm   SOURCES test_gc_relocation_detector_nojvm.cpp)

# Layer 2 (anchored references) — vmhook::ref / root / borrowed / ref_vector /
# object_id.  Two clusters, both no-JVM:
#   * anchored_ref_nojvm          — anchor construction, chain depth ceiling,
#     resolution degrading to nullptr rather than faulting, ephemeral expiry
#     (the no-JVM spelling of "the epoch moved"), value semantics, the access
#     binding proxy, lazy root binding.
#   * anchored_ref_identity_nojvm — object_id derivation, hash/equality
#     consistency, real unordered_map / map key use, and the static_asserts that
#     pin ref_vector's "no raw address can get in" contract (axiom A4 is the
#     ABSENCE of an API, so it can only be asserted at compile time).
# Live relocation behaviour needs a real VM and is out of scope here by
# construction; nothing below asserts it.
vmhook_add_test(anchored_ref_nojvm             SOURCES test_anchored_ref_nojvm.cpp)
vmhook_add_test(anchored_ref_identity_nojvm    SOURCES test_anchored_ref_identity_nojvm.cpp)
