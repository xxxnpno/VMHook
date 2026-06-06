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
vmhook_add_test(jni_arg_packing                SOURCES test_jni_arg_packing.cpp)
vmhook_add_test(signature_parsing              SOURCES test_signature_parsing.cpp)
vmhook_add_test(decode_oop_and_pointers        SOURCES test_decode_oop_and_pointers.cpp)
vmhook_add_test(decode_u5                       SOURCES test_decode_u5.cpp)
vmhook_add_test(iterate_entries_safety         SOURCES test_iterate_entries_safety.cpp)
vmhook_add_test(os_release_and_protect_edges   SOURCES test_os_release_and_protect_edges.cpp)
vmhook_add_test(array_element_helpers          SOURCES test_array_element_helpers.cpp)
vmhook_add_test(return_value                    SOURCES test_return_value.cpp)
vmhook_add_test(object_factory                  SOURCES test_object_factory.cpp)
vmhook_add_test(version_macros                  SOURCES test_version_macros.cpp)
target_compile_definitions("vmhook_test_version_macros" PRIVATE
    VMHOOK_CMAKE_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    VMHOOK_CMAKE_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    VMHOOK_CMAKE_VERSION_PATCH=${PROJECT_VERSION_PATCH})
vmhook_add_test(platform_capability_macros     SOURCES test_platform_capability_macros.cpp)
vmhook_add_test(traits_extra                    SOURCES test_traits_extra.cpp)
vmhook_add_test(api_surface_extended           SOURCES test_api_surface_extended.cpp)
vmhook_add_test(logging_format                  SOURCES test_logging_format.cpp)
vmhook_add_test(collection_type_tags           SOURCES test_collection_type_tags.cpp)
vmhook_add_test(global_ref                      SOURCES test_global_ref.cpp)
vmhook_add_test(method_enumeration             SOURCES test_method_enumeration.cpp)
vmhook_add_test(classloader_reanchor           SOURCES test_classloader_reanchor.cpp)
vmhook_add_test(jni_forwarders                  SOURCES test_jni_forwarders.cpp)
