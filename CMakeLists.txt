cmake_minimum_required(VERSION 3.20)

project(vmhook
    VERSION 6.0.0
    DESCRIPTION "Single-header HotSpot JVM hooking library"
    LANGUAGES CXX)

# ── Where things land ───────────────────────────────────────────────────────
# Everything you might actually want ends up in ONE place, <repo>/build:
# vmhook.dll, injector.exe, libvmhook.so, the test executables, the viewer.
# Everything you do not care about — CMakeCache, CMakeFiles, ninja files, .obj
# — stays in whatever binary dir you configured, which is conventionally
# <repo>/build/etc:
#
#     cmake -S . -B build/etc && cmake --build build/etc
#     ctest --test-dir build/etc
#
# Configuring somewhere else still works; only the BINARY destination is
# pinned.  The per-config variables matter for multi-config generators (Visual
# Studio, Xcode), which otherwise append Debug/ or Release/ to the path and
# scatter the binaries again.
#
# Passing -DCMAKE_RUNTIME_OUTPUT_DIRECTORY=... overrides it, which is what any
# harness building the SAME source tree with several toolchains at once must do
# -- otherwise every tree writes its vmhook.dll to the one path and they clobber
# each other.
if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/build")
endif()
if(NOT CMAKE_LIBRARY_OUTPUT_DIRECTORY)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
endif()
if(NOT CMAKE_ARCHIVE_OUTPUT_DIRECTORY)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
endif()
foreach(_vmhook_cfg IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${_vmhook_cfg} "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${_vmhook_cfg} "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${_vmhook_cfg} "${CMAKE_ARCHIVE_OUTPUT_DIRECTORY}")
endforeach()

# ── MSVC EH model: drop CMake's default /EHsc so we can pick /EHa per-target ──
# CMake injects /EHsc into CMAKE_CXX_FLAGS for every MSVC build.  vmhook's
# seh_invoke_detour needs ASYNC exception unwinding (/EHa) so a hardware
# 0xC0000005 raised inside a detour properly unwinds the intervening C++
# frames before the outer __except catches it.  Leaving /EHsc in place
# and adding /EHa per-target produces D9025 "overriding /EHsc with /EHa",
# which /WX turns into a hard error.  Scrub the default here so the
# per-target /EHa we set in vmhook_apply_warnings is the single, coherent
# EH model for the whole build.
if(MSVC)
    string(REGEX REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    string(REGEX REPLACE "/EHs"  "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
endif()

# ── Detect target platform ───────────────────────────────────────────────────
# The library compiles on Windows / Linux / macOS / iOS / Android.  Runtime
# functionality (HotSpot interpreter hooking) needs a HotSpot JVM in-process,
# which is only available on the first three.  iOS and Android ship their own
# VMs (no HotSpot) so vmhook is build-only on those platforms.

set(VMHOOK_PLATFORM_WINDOWS FALSE)
set(VMHOOK_PLATFORM_LINUX   FALSE)
set(VMHOOK_PLATFORM_MACOS   FALSE)
set(VMHOOK_PLATFORM_IOS     FALSE)
set(VMHOOK_PLATFORM_ANDROID FALSE)

if(WIN32)
    set(VMHOOK_PLATFORM_WINDOWS TRUE)
elseif(ANDROID OR CMAKE_SYSTEM_NAME STREQUAL "Android")
    set(VMHOOK_PLATFORM_ANDROID TRUE)
elseif(IOS OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(VMHOOK_PLATFORM_IOS TRUE)
elseif(APPLE)
    set(VMHOOK_PLATFORM_MACOS TRUE)
elseif(UNIX)
    set(VMHOOK_PLATFORM_LINUX TRUE)
endif()

# Runtime hooking (the trampoline) is x86_64-only and excludes iOS where
# Apple disallows runtime code generation in user processes.
set(VMHOOK_HAS_RUNTIME_HOOKING TRUE)
if(NOT (CMAKE_SIZEOF_VOID_P EQUAL 8))
    message(STATUS "vmhook: 32-bit build detected — runtime hooking disabled, headers still compile")
    set(VMHOOK_HAS_RUNTIME_HOOKING FALSE)
endif()
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm|aarch).*" OR
   CMAKE_OSX_ARCHITECTURES MATCHES "arm" OR
   ANDROID_ABI MATCHES "arm" OR
   IOS)
    set(VMHOOK_HAS_RUNTIME_HOOKING FALSE)
endif()
if(VMHOOK_PLATFORM_IOS)
    set(VMHOOK_HAS_RUNTIME_HOOKING FALSE)
endif()

set(_vmhook_platform_label "unknown")
if(VMHOOK_PLATFORM_WINDOWS)
    set(_vmhook_platform_label "Windows")
elseif(VMHOOK_PLATFORM_LINUX)
    set(_vmhook_platform_label "Linux")
elseif(VMHOOK_PLATFORM_MACOS)
    set(_vmhook_platform_label "macOS")
elseif(VMHOOK_PLATFORM_IOS)
    set(_vmhook_platform_label "iOS")
elseif(VMHOOK_PLATFORM_ANDROID)
    set(_vmhook_platform_label "Android")
endif()
message(STATUS "vmhook platform = ${_vmhook_platform_label}"
              "; runtime hooking = ${VMHOOK_HAS_RUNTIME_HOOKING}")

# ── Project-wide options ─────────────────────────────────────────────────────
# Defaults are chosen per platform so a plain `cmake -S . -B build` works
# everywhere: Windows / Linux / macOS get the full build; iOS / Android get
# the header + tests but skip the JVM-injection-only example DLL and the
# Windows-only injector.

# Determine the default for VMHOOK_BUILD_EXAMPLE.  The example DLL is
# useful as a self-test even on platforms where runtime hooking is not
# functional (the field-access, klass-walking, and JNI-OnLoad paths still
# work; only the interpreter-stub trampoline degrades).  We only disable
# the example by default on cross-compile targets that have no JVM at
# all: iOS and Android.
set(_vmhook_example_default TRUE)
if(VMHOOK_PLATFORM_IOS OR VMHOOK_PLATFORM_ANDROID)
    set(_vmhook_example_default FALSE)
endif()

option(VMHOOK_BUILD_EXAMPLE  "Build the example test DLL/SO that runs the hook suite"
       ${_vmhook_example_default})
option(VMHOOK_BUILD_INJECTOR "Build the Windows-only injector executable"
       ${VMHOOK_PLATFORM_WINDOWS})
option(VMHOOK_BUILD_TESTS    "Build the standalone unit-test suite (no JVM needed)"
       ON)
option(VMHOOK_WARNINGS_AS_ERRORS "Treat warnings as errors for first-party code"
       OFF)

# The C++20 named-module interface (vmhook.ixx), letting consumers write
# `import vmhook;` instead of including the 24k-line header in every TU.
#
# OFF by default, and deliberately so: building a named module needs CMake 3.28+,
# a generator that scans for module dependencies (Ninja or VS), and a toolchain
# whose module support is solid.  None of that is required to USE vmhook -- the
# header remains the primary interface and the module is a thin re-export of it
# -- so making it opt-in keeps every existing consumer and CI cell unaffected.
option(VMHOOK_BUILD_MODULE   "Build the C++20 named module (import vmhook;)"
       OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT GENERATOR_IS_MULTI_CONFIG)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build configuration" FORCE)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Interface target for the single-header library ──────────────────────────
add_library(vmhook INTERFACE)
add_library(vmhook::vmhook ALIAS vmhook)
target_include_directories(vmhook INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/vmhook/ext>"
    "$<INSTALL_INTERFACE:include>")

# POSIX dependencies: dl / pthread / mach (only the libs that are actually
# available on each variant).
if(VMHOOK_PLATFORM_LINUX OR VMHOOK_PLATFORM_ANDROID)
    target_link_libraries(vmhook INTERFACE dl)
    if(VMHOOK_PLATFORM_LINUX)
        target_link_libraries(vmhook INTERFACE pthread)
    endif()
elseif(VMHOOK_PLATFORM_MACOS OR VMHOOK_PLATFORM_IOS)
    # libdl and pthread are part of libSystem on Apple platforms — no
    # explicit link required.  We use mach VM APIs through libSystem too.
endif()

# ── Helper: apply our standard warning set to a target ──────────────────────
# Pass NO_WERROR as a second argument to keep the warning set but skip the
# -Werror / /WX escalation.  Used for the example DLL: its tests/jvm/modules/*
# are voluminous, multi-author test code where a style warning (an unused index
# constant, an unused local) is noise, not a defect — it must not fail CI, while
# the library and the first-party standalone tests stay strict.
function(vmhook_apply_warnings target)
    set(_no_werror FALSE)
    if(ARGC GREATER 1 AND "${ARGV1}" STREQUAL "NO_WERROR")
        set(_no_werror TRUE)
    endif()
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4 /permissive- /Zc:__cplusplus /utf-8
            /wd4505     # unreferenced static function (header carries multiple by design)
            /wd4101)    # unreferenced local (release builds compile out VMHOOK_LOG)
        # /EHa: async (SEH + C++) exception unwinding.  Load-bearing for
        # seh_invoke_detour: a hardware AV (0xC0000005) raised inside a
        # detour's extract_frame_arg has C++ frames between the fault site
        # and the outer __except; the default /EHsc only guarantees sync
        # C++ unwinding, so destructors and partial state on those frames
        # may not unwind cleanly, and the __except catch can be skipped.
        # /EHa restores the documented crash-safety contract on MSVC.
        # The top-of-file scrub removes CMake's default /EHsc from
        # CMAKE_CXX_FLAGS, so /EHa is the sole EH model — no D9025.
        target_compile_options(${target} PRIVATE /EHa)
        if(VMHOOK_WARNINGS_AS_ERRORS AND NOT _no_werror)
            target_compile_options(${target} PRIVATE /WX)
        endif()
        target_compile_definitions(${target} PRIVATE
            WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
            _WIN32_WINNT=0x0A00 WINVER=0x0A00)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic
            -Wno-unused-function
            -Wno-unused-local-typedefs
            -Wno-unused-but-set-variable
            -Wno-cast-function-type)
        if(_no_werror)
            # Test-module noise: named index constants / locals that aren't used
            # in every build configuration.
            target_compile_options(${target} PRIVATE
                -Wno-unused-variable
                -Wno-unused-const-variable)
        endif()
        if(VMHOOK_WARNINGS_AS_ERRORS AND NOT _no_werror)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

# ── Compiled library: vmhook.lib / libvmhook.a ──────────────────────────────
# vmhook is header-only, so this archive is not where the code lives — it is
# where the PRECOMPILED HEADER lives.  Linking vmhook::compiled gives your
# targets a PCH built from vmhook.hpp, so each of your TUs stops re-parsing
# 27k lines.  Measured on a 7-TU consumer, full rebuild of its sources:
#   MSVC 19.44   12.3 s -> 2.2 s
#   GCC 15       12.1 s -> 6.8 s
#
# It is separate from vmhook::vmhook and not linked by anything in this project:
# the PCH itself costs a few seconds to build once per consuming target, so a
# single-TU target (every test here is one) pays for it without amortising it.
# Opting in is the consumer's call.
option(VMHOOK_BUILD_COMPILED "Build the compiled library (vmhook.lib) and the shared precompiled header"
       ON)

if(VMHOOK_BUILD_COMPILED)
    add_library(vmhook_compiled STATIC vmhook/ext/vmhook/vmhook.cpp)
    add_library(vmhook::compiled ALIAS vmhook_compiled)
    set_target_properties(vmhook_compiled PROPERTIES OUTPUT_NAME "vmhook")
    target_link_libraries(vmhook_compiled PUBLIC vmhook)
    # PUBLIC: consumers inherit the header and build their own PCH from it, which
    # is what makes their TUs cheap.  BUILD_INTERFACE-only because a PCH is tied
    # to the exact compiler and flags that made it and cannot be shipped; an
    # installed consumer adds target_precompile_headers() itself (see README).
    target_precompile_headers(vmhook_compiled PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/vmhook/ext/vmhook/vmhook.hpp>")
    vmhook_apply_warnings(vmhook_compiled)
endif()

# ---------------------------------------------------------------------------
# Optional: the C++20 named module.
#
# `vmhook_module` is a real library target whose sole source is a CXX_MODULES
# file set holding vmhook.ixx.  It links the INTERFACE header target, because
# the module's global module fragment includes vmhook.hpp -- the module does not
# duplicate the library, it re-exports it.
#
# Requires CMake 3.28+ for stable CXX_MODULES support.  We check rather than
# raise the project-wide minimum, so a consumer on an older CMake still gets the
# header-only library and simply cannot opt into the module.
# ---------------------------------------------------------------------------
if(VMHOOK_BUILD_MODULE)
    if(CMAKE_VERSION VERSION_LESS "3.28")
        message(WARNING
            "VMHOOK_BUILD_MODULE requires CMake 3.28+ for C++20 module support "
            "(found ${CMAKE_VERSION}); skipping the module target.  The "
            "header-only vmhook::vmhook target is unaffected.")
    else()
        add_library(vmhook_module)
        add_library(vmhook::module ALIAS vmhook_module)
        target_sources(vmhook_module
            PUBLIC FILE_SET CXX_MODULES FILES
                "${CMAKE_CURRENT_SOURCE_DIR}/vmhook/ext/vmhook/vmhook.ixx")
        target_link_libraries(vmhook_module PUBLIC vmhook::vmhook)
        target_compile_features(vmhook_module PUBLIC cxx_std_23)
        set_target_properties(vmhook_module PROPERTIES CXX_SCAN_FOR_MODULES ON)
        vmhook_apply_warnings(vmhook_module)
    endif()
endif()

# ── Self-contained MinGW binaries ───────────────────────────────────────────
# Statically link the GCC / libstdc++ / winpthread runtime into MinGW-built
# Windows binaries so they carry NO external libstdc++-6.dll / libgcc_s_seh-1.dll
# / libwinpthread-1.dll dependency.
#
# Why this matters for the injected DLL: the CI JVM jobs download the build
# artefact and inject vmhook.dll into java.exe via CreateRemoteThread+LoadLibraryW.
# LoadLibraryW resolves vmhook.dll's imports against the JVM PROCESS's DLL search
# path (java.exe's directory + System32 + PATH) — NOT vmhook.dll's own directory —
# and the JVM job never installs MSYS2/MinGW, so the GCC runtime DLLs are not on
# that path.  A dynamically-linked MinGW vmhook.dll therefore fails to load and the
# injector reports "exit=1 / Injection failed".  This is environment-dependent: it
# passed only while the runner image happened to carry the MinGW runtime on PATH,
# and broke the instant the image updated (win25-vs2026/20260608).  Static linking
# removes the dependency entirely, so injection works on any runner image.
function(vmhook_static_runtime target)
    if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_link_options(${target} PRIVATE
            -static-libgcc -static-libstdc++ -static)
    endif()
endfunction()

# ── Example DLL / shared library that drives the JVM tests ──────────────────
# The example DLL is the JVM test harness, which lives under tests/jvm/ and is
# likewise developer-only.  Without it there is nothing to build here.
if(VMHOOK_BUILD_EXAMPLE AND NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vmhook/src/example.cpp")
    message(STATUS "vmhook: vmhook/src/ is not present in this checkout — skipping the example DLL")
    set(VMHOOK_BUILD_EXAMPLE OFF)
endif()

if(VMHOOK_BUILD_EXAMPLE AND NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/jvm/harness.cpp")
    message(STATUS "vmhook: tests/jvm/ is not present in this checkout — skipping the example DLL")
    set(VMHOOK_BUILD_EXAMPLE OFF)
endif()

if(VMHOOK_BUILD_EXAMPLE)
    set(_vmhook_example_sources vmhook/src/example.cpp)

    # Modular JVM test harness: the registry implementation plus every
    # self-registering per-feature module under tests/jvm/modules/.  Each module
    # is one feature's exhaustive JVM tests; CONFIGURE_DEPENDS re-globs when a
    # new module file is added so authors never edit this list.
    list(APPEND _vmhook_example_sources tests/jvm/harness.cpp)
    file(GLOB _vmhook_jvm_modules CONFIGURE_DEPENDS
         "${CMAKE_CURRENT_SOURCE_DIR}/tests/jvm/modules/*.cpp")
    list(APPEND _vmhook_example_sources ${_vmhook_jvm_modules})

    # Optional JNI-side speed test.  Only built when the JDK include
    # path is locatable, since pure-JNI calls require <jni.h> — vmhook
    # itself stays JNI-header-free, the bench is in a separate TU.
    find_package(JNI QUIET COMPONENTS JVM)
    if(NOT JNI_FOUND)
        find_package(JNI QUIET)
    endif()
    if(JNI_FOUND)
        message(STATUS "vmhook bench: JNI found at ${JNI_INCLUDE_DIRS}")
        list(APPEND _vmhook_example_sources vmhook/src/speedtest.cpp)
    else()
        message(STATUS "vmhook bench: <jni.h> not found, speedtest will be a no-op")
    endif()

    add_library(vmhook_example MODULE ${_vmhook_example_sources})
    set_target_properties(vmhook_example PROPERTIES
        OUTPUT_NAME "vmhook"
        PREFIX     ""
        SUFFIX     "${CMAKE_SHARED_MODULE_SUFFIX}")
    if(VMHOOK_PLATFORM_LINUX OR VMHOOK_PLATFORM_ANDROID
       OR VMHOOK_PLATFORM_MACOS OR VMHOOK_PLATFORM_IOS)
        # Java's System.loadLibrary("vmhook") expects "libvmhook" on POSIX.
        set_target_properties(vmhook_example PROPERTIES PREFIX "lib")
    endif()
    target_link_libraries(vmhook_example PRIVATE vmhook::vmhook)
    # The modular JVM harness (tests/jvm/harness.cpp + modules/*.cpp) is part of
    # the CMake example build only; the macro lets example.cpp skip it under the
    # legacy MSBuild vcxproj, which has a hardcoded source list.
    target_compile_definitions(vmhook_example PRIVATE VMHOOK_MODULAR_HARNESS=1)
    if(JNI_FOUND)
        target_include_directories(vmhook_example PRIVATE ${JNI_INCLUDE_DIRS})
        target_compile_definitions(vmhook_example PRIVATE VMHOOK_BENCH_USE_JNI=1)
    endif()
    vmhook_apply_warnings(vmhook_example NO_WERROR)
    # Self-contained on MinGW: java.exe's LoadLibraryW must resolve this DLL with
    # no external libstdc++-6 / libgcc_s_seh-1 / libwinpthread-1 on its search path.
    vmhook_static_runtime(vmhook_example)
endif()

# ── Windows-only injector (CreateRemoteThread + LoadLibraryW) ───────────────
# Developer tooling, not part of the published library — a consumer's checkout
# does not have injector/, so build it only when it is actually there.
if(VMHOOK_BUILD_INJECTOR AND VMHOOK_PLATFORM_WINDOWS
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/injector/src/main.cpp")
    add_executable(vmhook_injector injector/src/main.cpp)
    set_target_properties(vmhook_injector PROPERTIES OUTPUT_NAME "injector")
    target_link_libraries(vmhook_injector PRIVATE kernel32)
    vmhook_apply_warnings(vmhook_injector)
    # Self-contained on MinGW: the JVM runner has no MinGW runtime on PATH.
    vmhook_static_runtime(vmhook_injector)
endif()

# ── Standalone tests (header compiles + smoke-level pure-logic tests) ───────
# The test suite and the JVM harness are developer-only and are not published
# with the library, so a consumer's checkout simply will not have tests/.
# Default the option to whether the directory is actually there: a clone builds
# the library, and a working copy that has the tests still builds them.
if(VMHOOK_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
elseif(VMHOOK_BUILD_TESTS)
    message(STATUS "vmhook: tests/ is not present in this checkout — skipping the test suite")
endif()

# ── Install support ─────────────────────────────────────────────────────────
include(GNUInstallDirs)
install(DIRECTORY vmhook/ext/vmhook
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(TARGETS vmhook EXPORT vmhookTargets)
if(VMHOOK_BUILD_COMPILED)
    install(TARGETS vmhook_compiled EXPORT vmhookTargets
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()
install(EXPORT vmhookTargets
        NAMESPACE vmhook::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/vmhook
        FILE vmhookConfig.cmake)
