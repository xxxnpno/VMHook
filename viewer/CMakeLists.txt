cmake_minimum_required(VERSION 3.20)
project(vmhook_viewer LANGUAGES CXX)

# Windows-only: this tool uses Win32 process discovery + CreateRemoteThread
# injection + DirectX 11.  The vmhook library it drives is portable; this
# front-end is not.
if(NOT WIN32)
    message(FATAL_ERROR "vmhook_viewer is Windows-only (Win32 + DirectX 11 + injection).")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static-link the MSVC runtime so the injected payload DLL is self-contained in
# the target JVM (no VCRUNTIME140 dependency) and the viewer needs no VC redist.
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# vmhook.hpp needs async exception unwinding (/EHa) on MSVC; CMake injects the
# default /EHsc, which conflicts.  Scrub it so /EHa is the single EH model.
if(MSVC)
    string(REGEX REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    string(REGEX REPLACE "/EHs"  "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
endif()

# Both binaries land in the same directory so the viewer finds the payload DLL
# next to its own exe.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

set(VMHOOK_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/../vmhook/ext")
set(IMGUI_DIR      "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui")

if(NOT EXISTS "${IMGUI_DIR}/imgui.cpp")
    message(FATAL_ERROR
        "ImGui not found at ${IMGUI_DIR}.\n"
        "Fetch it with:  git clone --depth 1 https://github.com/ocornut/imgui \"${IMGUI_DIR}\"")
endif()

# JNI headers — the payload attaches its serve thread to the JVM and triggers a
# hook detour via a real JNI call (the only safe way to run a Java method from an
# external thread; see payload.cpp).  Locate the JDK's jni.h via find_package,
# falling back to a glob of the standard Windows install dir (JAVA_HOME is often
# a stale symlink), so the build doesn't hard-code a JDK version.
find_package(JNI QUIET)
if(NOT JNI_INCLUDE_DIRS)
    file(GLOB _vmhook_jdk_jni "C:/Program Files/Java/*/include/jni.h")
    if(_vmhook_jdk_jni)
        list(GET _vmhook_jdk_jni 0 _vmhook_jni_h)
        get_filename_component(_vmhook_jni_inc "${_vmhook_jni_h}" DIRECTORY)
        set(JNI_INCLUDE_DIRS "${_vmhook_jni_inc}" "${_vmhook_jni_inc}/win32")
    endif()
endif()
if(NOT JNI_INCLUDE_DIRS)
    message(FATAL_ERROR "JNI headers (jni.h) not found — install a JDK or set JAVA_HOME so the payload can invoke Java methods.")
endif()

# ── payload DLL — includes vmhook.hpp, enumerates the JVM, streams over a pipe ──
add_library(vmhook_payload SHARED payload/payload.cpp)
target_include_directories(vmhook_payload PRIVATE "${VMHOOK_INCLUDE}" ${JNI_INCLUDE_DIRS})
target_compile_features(vmhook_payload PRIVATE cxx_std_23)
if(MSVC)
    target_compile_options(vmhook_payload PRIVATE /EHa /bigobj /Zc:preprocessor /Zc:__cplusplus)
endif()

# ── ImGui (core + Win32/DX11 backends) as a static lib ────────────────────────
add_library(imgui STATIC
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_win32.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_dx11.cpp")
target_include_directories(imgui PUBLIC "${IMGUI_DIR}" "${IMGUI_DIR}/backends")
target_compile_features(imgui PRIVATE cxx_std_17)

# ── viewer GUI exe ────────────────────────────────────────────────────────────
# Vendored professional widgets: cmdwtf/imgui_toggle (0BSD) for animated toggle
# switches, and BalazsJako/ImGuiColorTextEdit (MIT) for the Scripts code editor
# (C++ syntax highlighting + line numbers + error markers).
set(TOGGLE_DIR   "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui_toggle")
set(TEXTEDIT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ImGuiColorTextEdit")
add_executable(vmhook_viewer WIN32 src/main.cpp assets/vmhook.rc
    "${TOGGLE_DIR}/imgui_toggle.cpp"
    "${TOGGLE_DIR}/imgui_toggle_palette.cpp"
    "${TOGGLE_DIR}/imgui_toggle_presets.cpp"
    "${TOGGLE_DIR}/imgui_toggle_renderer.cpp"
    "${TEXTEDIT_DIR}/TextEditor.cpp")
target_include_directories(vmhook_viewer PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${TOGGLE_DIR}" "${TEXTEDIT_DIR}")
target_compile_features(vmhook_viewer PRIVATE cxx_std_23)
target_compile_definitions(vmhook_viewer PRIVATE IMGUI_DEFINE_MATH_OPERATORS)
target_link_libraries(vmhook_viewer PRIVATE imgui d3d11 dxgi dwmapi)
if(MSVC)
    target_compile_options(vmhook_viewer PRIVATE /Zc:preprocessor /Zc:__cplusplus)
endif()

# Build the payload alongside the viewer.
add_dependencies(vmhook_viewer vmhook_payload)

# Bundle the Font Awesome icon font next to the exe so the viewer can merge it
# into its font atlas at startup (graceful no-op if the file is missing).
add_custom_command(TARGET vmhook_viewer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/fa-solid-900.ttf"
        "$<TARGET_FILE_DIR:vmhook_viewer>/fa-solid-900.ttf"
    VERBATIM)

# Bundle the vmhook header next to the exe so the "Scripts" feature can compile
# user scripts (which #include <vmhook/vmhook.hpp>) against it at runtime.
add_custom_command(TARGET vmhook_viewer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${VMHOOK_INCLUDE}/vmhook/vmhook.hpp"
        "$<TARGET_FILE_DIR:vmhook_viewer>/vmhook/vmhook.hpp"
    VERBATIM)

# ── headless end-to-end probe (test) ─────────────────────────────────────────
add_executable(vmhook_cli_probe test/cli_probe.cpp)
target_include_directories(vmhook_cli_probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(vmhook_cli_probe PRIVATE cxx_std_23)
add_dependencies(vmhook_cli_probe vmhook_payload)

# ── vmhook_cli — JSON front-end used by the MCP server ───────────────────────
add_executable(vmhook_cli mcp/vmhook_cli.cpp)
target_include_directories(vmhook_cli PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(vmhook_cli PRIVATE cxx_std_23)
add_dependencies(vmhook_cli vmhook_payload)
