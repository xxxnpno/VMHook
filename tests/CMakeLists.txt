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
vmhook_add_test(method_proxy_value_t           SOURCES test_method_proxy_value_t.cpp)
vmhook_add_test(method_flags_width             SOURCES test_method_flags_width.cpp)
vmhook_add_test(jni_arg_packing                SOURCES test_jni_arg_packing.cpp)
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
vmhook_add_test(global_ref                      SOURCES test_global_ref.cpp)
vmhook_add_test(method_enumeration             SOURCES test_method_enumeration.cpp)
vmhook_add_test(classloader_reanchor           SOURCES test_classloader_reanchor.cpp)
vmhook_add_test(jni_forwarders                  SOURCES test_jni_forwarders.cpp)
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
vmhook_add_test(seh_invoke_detour              SOURCES test_seh_invoke_detour.cpp)
