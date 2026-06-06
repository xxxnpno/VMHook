#pragma once

/*
    VMHook - Single-header, header-only C++20/23 library for hooking Java methods
    and accessing fields at runtime via HotSpot VMStructs.

    Purpose:
        Provides a zero-JNI, zero-JVMTI API to intercept Java method calls
        and read/write instance/static fields by walking HotSpot's internal
        metadata tables (gHotSpotVMStructs / gHotSpotVMTypes) exported by
        jvm.dll on Windows or libjvm.so on Linux.

    Dependencies:
        - C++20-or-newer compiler. <print>/std::format are used when available
          (MSVC 19.36+, libstdc++ 14+, libc++ 18+); otherwise the header falls
          back to std::format-only or std::ostringstream formatting.
        - Tested with MSVC 19.4x, GCC 13+, Clang 16+ on Windows and Linux.
        - A running HotSpot JVM (Temurin, Corretto, Liberica, Microsoft, etc.)
        - The JVM's runtime shared library loaded in the current process with
          gHotSpotVMStructs / gHotSpotVMTypes exported (jvm.dll / libjvm.so).

    Thread safety:
        - read-only operations (find_class, find_field, get_field)
          are NOT thread-safe; the caller must synchronise access.
        - hook installation and shutdown_hooks() MUST be called from a single thread.
        - The internal caches (klass_lookup_cache, g_field_cache, type_to_class_map)
          use std::unordered_map which is NOT thread-safe for concurrent mutation.

    Template requirements:
        - get_field<T>, set_field<T>:
          T must be std::is_trivially_copyable_v<T> == true.
        - register_class<T>: T must be a complete class type with a virtual table
          (typeid(*this) is used to resolve the class name).
        - hook<T>: T must have been registered via register_class<T>() beforehand.

    Complexity guarantees:
        - find_class(class_name):       O(N*M) worst case, where N = number of loaded
                                        classloaders, M = average classes per classloader.
                                        O(1) after first lookup (cached in klass_lookup_cache).
        - find_field(klass, name):      O(F) where F = number of fields on the klass.
                                        O(1) after first lookup (cached in g_field_cache).
        - get_field<T> / set_field<T>:  O(1) after field is cached; O(F) on first call.
        - hook<T>(method_name, detour): O(M) where M = number of methods on the klass.
        - shutdown_hooks():             O(H) where H = number of active hooks.
        - klass.get_methods_count():    O(1).
        - klass.find_field(name):       O(F) where F = number of fields on the klass.
        - dictionary.find_klass(name):  O(B*E) where B = bucket count, E = entries per bucket.
        - class_loader_data_graph.find_klass(name): O(N*M) full graph walk.

    Exception safety:
        - All public API functions catch vmhook::exception internally and report
          the message through VMHOOK_LOG before returning a safe default value
          (nullptr, std::nullopt, false, or T{}). Release builds compile
          VMHOOK_LOG to a no-op unless VMHOOK_DEBUG_LOGS is overridden.
        - Internal helpers (iterate_struct_entries, iterate_type_entries, etc.)
          are noexcept and return nullptr on failure.
        - midi2i_hook constructor may throw vmhook::exception on allocation failure;
          the caller (hook<T>) catches it and returns false.
*/

// ---------------------------------------------------------------------------
// Library version.  Bump these macros for every released change.  Semantic
// versioning: MAJOR for API breaks, MINOR for additive features, PATCH for
// fixes.  The packed VMHOOK_VERSION integer makes it easy to gate consumer
// code on a minimum version via `#if VMHOOK_VERSION >= VMHOOK_MAKE_VERSION(0,3,0)`.
// ---------------------------------------------------------------------------
#define VMHOOK_VERSION_MAJOR 0
#define VMHOOK_VERSION_MINOR 5
#define VMHOOK_VERSION_PATCH 3

#define VMHOOK_MAKE_VERSION(major, minor, patch) \
    (((major) * 1000000) + ((minor) * 1000) + (patch))

#define VMHOOK_VERSION \
    VMHOOK_MAKE_VERSION(VMHOOK_VERSION_MAJOR, VMHOOK_VERSION_MINOR, VMHOOK_VERSION_PATCH)

#define VMHOOK_VERSION_STRING_HELPER2(x) #x
#define VMHOOK_VERSION_STRING_HELPER(x) VMHOOK_VERSION_STRING_HELPER2(x)
#define VMHOOK_VERSION_STRING                                    \
    VMHOOK_VERSION_STRING_HELPER(VMHOOK_VERSION_MAJOR) "."        \
    VMHOOK_VERSION_STRING_HELPER(VMHOOK_VERSION_MINOR) "."        \
    VMHOOK_VERSION_STRING_HELPER(VMHOOK_VERSION_PATCH)

#include <array>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <typeindex>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <chrono>
#include <algorithm>
#include <atomic>
#include <type_traits>
#include <tuple>
#include <cstring>
#include <optional>
#include <variant>
#include <functional>
#include <limits>

// ---------------------------------------------------------------------------
// Platform / compiler detection
//
// Five OS macros are exposed.  Exactly one is set to 1; the others are 0.
// Two convenience aggregates are also defined:
//
//   VMHOOK_OS_POSIX   = Linux | macOS | iOS | Android  (true POSIX backends)
//   VMHOOK_OS_APPLE   = macOS | iOS
//
// The header is callable (and the standalone unit tests run) on every
// platform listed here.  Runtime functionality (HotSpot interpreter
// hooking, gHotSpotVMStructs lookup) requires a HotSpot JVM in-process;
// that exists on Windows / Linux / macOS desktop JDKs but not on iOS or
// Android, where the platform VMs (Apple's restricted no-JIT environment
// and Android's ART) replace HotSpot.  See README / CONTRIBUTING for the
// platform-capability matrix.
// ---------------------------------------------------------------------------

#if defined(__ANDROID__)
    #define VMHOOK_OS_WINDOWS 0
    #define VMHOOK_OS_LINUX   0
    #define VMHOOK_OS_MACOS   0
    #define VMHOOK_OS_IOS     0
    #define VMHOOK_OS_ANDROID 1
#elif defined(_WIN32) || defined(_WIN64)
    #define VMHOOK_OS_WINDOWS 1
    #define VMHOOK_OS_LINUX   0
    #define VMHOOK_OS_MACOS   0
    #define VMHOOK_OS_IOS     0
    #define VMHOOK_OS_ANDROID 0
#elif defined(__APPLE__)
    #include <TargetConditionals.h>
    #if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
        #define VMHOOK_OS_WINDOWS 0
        #define VMHOOK_OS_LINUX   0
        #define VMHOOK_OS_MACOS   0
        #define VMHOOK_OS_IOS     1
        #define VMHOOK_OS_ANDROID 0
    #else
        #define VMHOOK_OS_WINDOWS 0
        #define VMHOOK_OS_LINUX   0
        #define VMHOOK_OS_MACOS   1
        #define VMHOOK_OS_IOS     0
        #define VMHOOK_OS_ANDROID 0
    #endif
#elif defined(__linux__)
    #define VMHOOK_OS_WINDOWS 0
    #define VMHOOK_OS_LINUX   1
    #define VMHOOK_OS_MACOS   0
    #define VMHOOK_OS_IOS     0
    #define VMHOOK_OS_ANDROID 0
#else
    #define VMHOOK_OS_WINDOWS 0
    #define VMHOOK_OS_LINUX   0
    #define VMHOOK_OS_MACOS   0
    #define VMHOOK_OS_IOS     0
    #define VMHOOK_OS_ANDROID 0
    #error "vmhook supports Windows, Linux, macOS, iOS, or Android (x86_64 / arm64)."
#endif

#define VMHOOK_OS_POSIX (VMHOOK_OS_LINUX | VMHOOK_OS_MACOS | VMHOOK_OS_IOS | VMHOOK_OS_ANDROID)
#define VMHOOK_OS_APPLE (VMHOOK_OS_MACOS | VMHOOK_OS_IOS)

#if defined(__x86_64__) || defined(_M_X64)
    #define VMHOOK_ARCH_X86_64 1
    #define VMHOOK_ARCH_ARM64  0
#elif defined(__aarch64__) || defined(_M_ARM64)
    #define VMHOOK_ARCH_X86_64 0
    #define VMHOOK_ARCH_ARM64  1
#else
    #define VMHOOK_ARCH_X86_64 0
    #define VMHOOK_ARCH_ARM64  0
    #error "vmhook supports x86_64 or arm64 only."
#endif

// HotSpot runtime hooking is x86_64-only: the trampoline emits Microsoft
// x64 / System V AMD64 bytes and walks the HotSpot interpreter frame
// layout, which differs on arm64.  On arm64 hosts the header still
// compiles and the OS layer is fully functional, but `vmhook::hook<T>`
// returns false at runtime.  Set VMHOOK_RUNTIME_HOOKING_AVAILABLE to 0
// in that build configuration so consumers can gate their use of the
// runtime API.
#if VMHOOK_ARCH_X86_64 && !VMHOOK_OS_IOS
    #define VMHOOK_RUNTIME_HOOKING_AVAILABLE 1
#else
    #define VMHOOK_RUNTIME_HOOKING_AVAILABLE 0
#endif

#if defined(_MSC_VER) && !defined(__clang__)
    #define VMHOOK_COMPILER_MSVC 1
#else
    #define VMHOOK_COMPILER_MSVC 0
#endif

#if defined(__clang__)
    #define VMHOOK_COMPILER_CLANG 1
#else
    #define VMHOOK_COMPILER_CLANG 0
#endif

#if defined(__GNUC__) && !defined(__clang__)
    #define VMHOOK_COMPILER_GCC 1
#else
    #define VMHOOK_COMPILER_GCC 0
#endif

// std::format requires GCC 13+ / Clang 14+ / MSVC 19.29+
#if __has_include(<format>)
    #include <format>
    #define VMHOOK_HAS_STD_FORMAT 1
#else
    #define VMHOOK_HAS_STD_FORMAT 0
#endif

// std::print/std::println require GCC 14+ / Clang 18+ (libc++) / MSVC 19.37+
#if __has_include(<print>) && (defined(__cpp_lib_print) && __cpp_lib_print >= 202207L)
    #include <print>
    #define VMHOOK_HAS_STD_PRINT 1
#else
    #define VMHOOK_HAS_STD_PRINT 0
#endif

// C++23 deducing-this support test.  The feature itself is implemented in
// MSVC 19.32+ / GCC 14+ / Clang 18+, but GCC's overload resolution still
// considers explicit-object member functions in *static-call* contexts
// (where no implicit object is available) and then errors with "cannot
// call without object".  Upstream LLVM Clang and MSVC correctly exclude
// them from static-call overload resolution.
//
// The Android NDK Clang shows the same overload-resolution behavior as
// GCC here even though it self-identifies as Clang, so we also exclude
// __ANDROID__.
//
// The deducing-this path is therefore enabled only on MSVC and non-NDK
// Clang — that is where vmhook::object<T>::get_field can be invoked from
// both instance and static methods uniformly.
#if defined(__cpp_explicit_this_parameter) && __cpp_explicit_this_parameter >= 202110L \
    && (defined(__clang__) || defined(_MSC_VER)) \
    && !defined(__ANDROID__)
    #define VMHOOK_HAS_DEDUCING_THIS 1
#else
    #define VMHOOK_HAS_DEDUCING_THIS 0
#endif

#if VMHOOK_OS_WINDOWS
    // <windows.h> defines macros (min, max, ERROR, etc.) that clash with C++.
    // We guard them and undefine the worst offenders right after include.
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
    #include <tlhelp32.h>   // CreateToolhelp32Snapshot, THREADENTRY32, ...
#elif VMHOOK_OS_POSIX
    #include <cerrno>
    #include <cstdio>
    #include <dlfcn.h>
    #include <fcntl.h>
    #include <setjmp.h>     // sigjmp_buf, sigsetjmp, siglongjmp — POSIX (not in <csetjmp>)
    #include <signal.h>     // POSIX sigaction, SIGSEGV, etc. (not in <csignal> for SA_*)
    #include <sys/mman.h>
    #include <unistd.h>
    #if VMHOOK_OS_LINUX || VMHOOK_OS_ANDROID
        // Linux-and-Android-only helpers we use for fast safe memory reads.
        #include <sys/syscall.h>
        #include <sys/uio.h>
    #endif
    #if VMHOOK_OS_APPLE
        #include <mach/mach.h>
        #include <pthread.h>
        #include <libkern/OSCacheControl.h>   // sys_icache_invalidate
        // mach_vm.h is "unsupported" in the iOS SDK (it builds but the
        // mach_vm_* APIs are not callable from a user-space iOS process).
        // On macOS the same header lives at <mach/mach_vm.h>.  Only include
        // it when we are actually going to call it.
        #if VMHOOK_OS_MACOS
            #include <mach/mach_vm.h>
        #endif
    #endif
#endif

#ifndef VMHOOK_DEBUG_LOGS
    #ifdef NDEBUG
        #define VMHOOK_DEBUG_LOGS 0
    #else
        #define VMHOOK_DEBUG_LOGS 1
    #endif
#endif

// ---------------------------------------------------------------------------
// VMHOOK_LOG_FILE - opt-in file logging.
//
// Define VMHOOK_LOG_FILE before including this header (or via the build system,
// e.g. -DVMHOOK_LOG_FILE=\"vmhook.log\") to a string literal containing the
// path of the log file.  When set, every VMHOOK_LOG() call (errors, warnings,
// info) is appended to that file instead of being written to std::cout.  The
// stream is opened lazily on first use, kept open for the lifetime of the
// process, and writes are serialised with a mutex so concurrent log lines from
// different threads cannot interleave.  Falls back to std::cout silently if
// the file cannot be opened.
//
// Example:
//   #define VMHOOK_LOG_FILE "vmhook.log"
//   #include <vmhook/vmhook.hpp>
// ---------------------------------------------------------------------------

namespace vmhook::detail
{
    /*
        @brief Format a log line using std::format if available, otherwise stream-format.
        @details
        Provides a single API used by VMHOOK_LOG so callers do not need to know whether
        the host toolchain has shipped std::format / std::print.  When std::format is
        present we use it; otherwise we ignore the format specifiers and concatenate
        the format string verbatim, which is enough for diagnostic output.
    */
#if VMHOOK_HAS_STD_FORMAT
    template <typename... args_t>
    inline auto format_log(std::string_view fmt, args_t&&... args)
        -> std::string
    {
        try
        {
            return std::vformat(fmt, std::make_format_args(args...));
        }
        catch (...)
        {
            return std::string{ fmt };
        }
    }
#else
    template <typename... args_t>
    inline auto format_log(std::string_view fmt, args_t&&...)
        -> std::string
    {
        return std::string{ fmt };
    }
#endif

    inline auto emit_log_line(std::string const& line) noexcept
        -> void
    {
        try
        {
            static std::mutex log_mutex;
            std::lock_guard<std::mutex> lock{ log_mutex };
#ifdef VMHOOK_LOG_FILE
            // Opened once on first use, kept open for the lifetime of the
            // process.  std::ios::app means every write is positioned at EOF,
            // so a crashed process can be re-run without truncating earlier
            // diagnostics.
            static std::ofstream log_file{ VMHOOK_LOG_FILE, std::ios::out | std::ios::app };
            if (log_file.is_open())
            {
                log_file << line << '\n';
                log_file.flush();
                return;
            }
#endif
            std::cout << line << '\n';
            std::cout.flush();
        }
        catch (...)
        {
            // Never let logging escape.
        }
    }
}

#if VMHOOK_DEBUG_LOGS
    #define VMHOOK_LOG(...) ::vmhook::detail::emit_log_line(::vmhook::detail::format_log(__VA_ARGS__))
#else
    // No-op form: use sizeof on an unevaluated call so the arguments are
    // ODR-used as far as the compiler is concerned (suppresses C4101 on
    // catch-bound exception objects that are only referenced inside VMHOOK_LOG)
    // without emitting any runtime code.
    #define VMHOOK_LOG(...) ((void)sizeof(::vmhook::detail::format_log(__VA_ARGS__)))
#endif

namespace vmhook
{
    /*
        @brief Log-prefix tags used in diagnostic messages emitted by this library.
        @details  These are prepended to every VMHOOK_LOG() call so that output from
        VMHook can be filtered in the host process console.
    */
    inline constexpr std::string_view error_tag{ "[VMHook ERROR]" };
    inline constexpr std::string_view warning_tag{ "[VMHook WARNING]" };
    inline constexpr std::string_view info_tag{ "[VMHook INFO]" };

    /*
        @brief Exception type thrown internally by VMHook to report unrecoverable errors.
        @details  All public API functions catch vmhook::exception and log the message
        through VMHOOK_LOG before returning a safe default value, so callers never
        see uncaught exceptions escaping the library boundary.
    */
    class exception final : public std::exception
    {
    public:
        explicit exception(const std::string_view msg)
            : message{ msg }
        {

        }

        auto what() const noexcept
            -> const char* override
        {
            return this->message.c_str();
        }

    private:
        std::string message;
    };

    // -------------------------------------------------------------------------
    // OS abstraction layer
    //
    // Wraps Windows-only and POSIX-only primitives behind a portable surface so
    // the HotSpot probing/hooking code can stay platform-agnostic.  All members
    // are noexcept and never throw; failure is reported via returned values.
    // -------------------------------------------------------------------------
    namespace os
    {
#if VMHOOK_OS_WINDOWS
        using module_handle = ::HMODULE;
        using thread_id_t   = ::DWORD;
#else
        using module_handle = void*;
        using thread_id_t   = std::uint64_t;
#endif

        /*
            @brief Memory-protection flags expressed in portable terms.
        */
        enum class memory_protection : std::uint32_t
        {
            no_access      = 0,
            read           = 1,
            read_write     = 2,
            execute_read   = 3,
            execute_rw     = 4,
        };

        /*
            @brief Information about a single VM memory region (allocated or free).
            @details
            Returned by query_region() and used by the trampoline allocator to find
            a free region within +/- 2 GiB of the hook target.
        */
        struct region_info
        {
            void*       base{ nullptr };
            std::size_t size{ 0 };
            bool        committed{ false };
            bool        free{ false };
            bool        readable{ false };
            bool        executable{ false };
            bool        guarded{ false };
        };

        /*
            @brief Returns the host CPU page size in bytes.
        */
        inline auto page_size() noexcept -> std::size_t
        {
#if VMHOOK_OS_WINDOWS
            SYSTEM_INFO si{};
            ::GetSystemInfo(&si);
            return static_cast<std::size_t>(si.dwPageSize);
#else
            const long ps{ ::sysconf(_SC_PAGESIZE) };
            return ps > 0 ? static_cast<std::size_t>(ps) : static_cast<std::size_t>(4096);
#endif
        }

        /*
            @brief Returns the host VM allocation granularity (aligned-to bytes).
        */
        inline auto allocation_granularity() noexcept -> std::size_t
        {
#if VMHOOK_OS_WINDOWS
            SYSTEM_INFO si{};
            ::GetSystemInfo(&si);
            return static_cast<std::size_t>(si.dwAllocationGranularity);
#else
            return page_size();
#endif
        }

        /*
            @brief Maximum user-space address; pointers above this are kernel/non-canonical.
        */
        inline constexpr std::uintptr_t user_address_ceiling{ 0x00007FFFFFFFFFFFull };

        /*
            @brief Minimum sane user-space address; pointers below this are noise.
        */
        inline constexpr std::uintptr_t user_address_floor{ 0xFFFFull };

        /*
            @brief Looks up a previously loaded shared library by leaf filename.
            @details
            Tries the platform's "find loaded module" call (GetModuleHandle on
            Windows, dlopen(name, RTLD_NOW | RTLD_NOLOAD) on Linux).  Returns
            nullptr if the module is not currently loaded into the process.
        */
        inline auto find_loaded_module(const char* name) noexcept -> module_handle
        {
#if VMHOOK_OS_WINDOWS
            return ::GetModuleHandleA(name);
#else
            // RTLD_NOLOAD: only return handle if already loaded.  glibc
            // increments the soname refcount on a successful RTLD_NOLOAD probe.
            // We deliberately do NOT dlclose the result here: that would
            // dangle the handle if we ever happen to hold the last ref.
            // The JVM lives for the process lifetime in every supported use
            // case, so the per-call refcount bump is a benign no-op.
            return ::dlopen(name, RTLD_LAZY | RTLD_NOLOAD);
#endif
        }

        /*
            @brief Locates the JVM runtime shared library currently loaded.
            @details
            Tries the platform-specific candidate names until one resolves to
            a non-null module.  On Android and iOS this normally returns
            nullptr because those platforms ship their own VM (ART /
            JavaScriptCore-derived) rather than a HotSpot libjvm; the
            higher-level vmhook APIs degrade gracefully in that case.
        */
        inline auto find_jvm_module() noexcept -> module_handle
        {
#if VMHOOK_OS_WINDOWS
            static const char* const candidates[]{ "jvm.dll" };
#elif VMHOOK_OS_MACOS
            static const char* const candidates[]{
                "libjvm.dylib",
                "@rpath/libjvm.dylib",
                "libjvm.so",
            };
#elif VMHOOK_OS_LINUX || VMHOOK_OS_ANDROID
            static const char* const candidates[]{ "libjvm.so", "libjvm.so.0" };
#else
            static const char* const candidates[]{ "libjvm.dylib", "libjvm.so" };
#endif
            for (const char* name : candidates)
            {
                if (module_handle const handle{ find_loaded_module(name) })
                {
                    return handle;
                }
            }
            return nullptr;
        }

        /*
            @brief Resolves an exported symbol from a previously loaded module.
        */
        inline auto get_proc_address(module_handle module, const char* symbol) noexcept -> void*
        {
            if (!module || !symbol)
            {
                return nullptr;
            }
#if VMHOOK_OS_WINDOWS
            return reinterpret_cast<void*>(::GetProcAddress(module, symbol));
#else
            return ::dlsym(module, symbol);
#endif
        }

        /*
            @brief Returns the OS-level identifier of the calling thread.
            @details
            Win32 thread ID on Windows, kernel TID (gettid) on Linux/Android,
            mach thread port number on Apple platforms.  The value space
            differs per platform; the type is uint64_t so all of them fit.
        */
        inline auto current_thread_id() noexcept -> thread_id_t
        {
#if VMHOOK_OS_WINDOWS
            return ::GetCurrentThreadId();
#elif VMHOOK_OS_LINUX || VMHOOK_OS_ANDROID
            return static_cast<thread_id_t>(::syscall(SYS_gettid));
#elif VMHOOK_OS_APPLE
            std::uint64_t tid{ 0 };
            ::pthread_threadid_np(nullptr, &tid);
            return static_cast<thread_id_t>(tid);
#else
            return 0;
#endif
        }

#if VMHOOK_OS_WINDOWS
        inline auto to_native_protect(memory_protection prot) noexcept -> DWORD
        {
            switch (prot)
            {
            case memory_protection::no_access:    return PAGE_NOACCESS;
            case memory_protection::read:         return PAGE_READONLY;
            case memory_protection::read_write:   return PAGE_READWRITE;
            case memory_protection::execute_read: return PAGE_EXECUTE_READ;
            case memory_protection::execute_rw:   return PAGE_EXECUTE_READWRITE;
            }
            return PAGE_NOACCESS;
        }
#else
        inline auto to_native_protect(memory_protection prot) noexcept -> int
        {
            switch (prot)
            {
            case memory_protection::no_access:    return PROT_NONE;
            case memory_protection::read:         return PROT_READ;
            case memory_protection::read_write:   return PROT_READ | PROT_WRITE;
            case memory_protection::execute_read: return PROT_READ | PROT_EXEC;
            case memory_protection::execute_rw:   return PROT_READ | PROT_WRITE | PROT_EXEC;
            }
            return PROT_NONE;
        }
#endif

        /*
            @brief Changes the protection of a memory region in place.
            @return true on success.
        */
        inline auto protect(void* address, std::size_t size, memory_protection prot,
                            std::uint32_t* old_prot = nullptr) noexcept -> bool
        {
            if (!address || size == 0)
            {
                return false;
            }
#if VMHOOK_OS_WINDOWS
            DWORD prev{};
            const BOOL ok{ ::VirtualProtect(address, size, to_native_protect(prot), &prev) };
            if (ok && old_prot)
            {
                *old_prot = static_cast<std::uint32_t>(prev);
            }
            return ok != 0;
#else
            // mprotect requires page-aligned base + length.
            const std::size_t ps{ page_size() };
            std::uintptr_t base{ reinterpret_cast<std::uintptr_t>(address) };
            const std::uintptr_t end{ base + size };
            base &= ~(static_cast<std::uintptr_t>(ps) - 1);
            const std::size_t aligned_size{ static_cast<std::size_t>(end - base + ps - 1) & ~(ps - 1) };
            const int rc{ ::mprotect(reinterpret_cast<void*>(base), aligned_size, to_native_protect(prot)) };
            if (rc == 0 && old_prot)
            {
                *old_prot = 0;
            }
            return rc == 0;
#endif
        }

        /*
            @brief Reserves and commits `size` bytes of writable, executable memory.
            @details
            On Windows uses VirtualAlloc with PAGE_EXECUTE_READWRITE.
            On Linux uses mmap with PROT_READ|PROT_WRITE|PROT_EXEC, MAP_ANONYMOUS|MAP_PRIVATE.
            `address_hint` is treated as a non-binding placement preference.
        */
        inline auto allocate_rwx(void* address_hint, std::size_t size) noexcept -> void*
        {
            if (size == 0)
            {
                return nullptr;
            }
#if VMHOOK_OS_WINDOWS
            return ::VirtualAlloc(address_hint, size,
                                  MEM_COMMIT | MEM_RESERVE,
                                  PAGE_EXECUTE_READWRITE);
#else
            // Try RWX first (succeeds on Linux, Android, x86_64 macOS, and
            // older iOS).  On Apple arm64 / current iOS the kernel enforces
            // W^X and refuses PROT_WRITE | PROT_EXEC simultaneously without
            // the JIT entitlement; fall back to RW so the caller at least
            // gets a usable buffer (the caller can mprotect to RX later via
            // os::protect, which is also entitlement-gated on Apple).
            void* result{ ::mmap(address_hint, size,
                                 PROT_READ | PROT_WRITE | PROT_EXEC,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) };
            if (result == MAP_FAILED)
            {
                result = ::mmap(address_hint, size,
                                PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                if (result == MAP_FAILED)
                {
                    return nullptr;
                }
            }
            return result;
#endif
        }

        /*
            @brief Releases memory previously returned by allocate_rwx.
            @details
            size is ignored on Windows (VirtualFree releases the whole reservation),
            but Linux munmap returns EINVAL for size == 0.  Returning early on a
            zero size keeps the cross-platform behaviour symmetric and avoids the
            useless syscall on POSIX.
        */
        inline auto release(void* address, std::size_t size) noexcept -> void
        {
            if (!address || size == 0)
            {
                return;
            }
#if VMHOOK_OS_WINDOWS
            (void)size;
            ::VirtualFree(address, 0, MEM_RELEASE);
#else
            ::munmap(address, size);
#endif
        }

        /*
            @brief Returns information about the VM region containing `address`.
            @details
            Used by safe_readable() and by the trampoline allocator.
        */
        inline auto query_region(const void* address) noexcept -> region_info
        {
            region_info info{};
            if (!address)
            {
                return info;
            }
#if VMHOOK_OS_WINDOWS
            MEMORY_BASIC_INFORMATION mbi{};
            if (::VirtualQuery(address, &mbi, sizeof(mbi)) == 0)
            {
                return info;
            }
            info.base = mbi.BaseAddress;
            info.size = mbi.RegionSize;
            info.committed = (mbi.State == MEM_COMMIT);
            info.free = (mbi.State == MEM_FREE);
            const DWORD prot{ mbi.Protect };
            info.readable = (prot & (PAGE_READONLY | PAGE_READWRITE
                                     | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE
                                     | PAGE_EXECUTE_WRITECOPY | PAGE_WRITECOPY)) != 0;
            info.executable = (prot & (PAGE_EXECUTE | PAGE_EXECUTE_READ
                                       | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
            info.guarded = (prot & PAGE_GUARD) != 0;
            return info;
#elif VMHOOK_OS_MACOS
            // Use mach_vm_region to walk the task's memory map.  Apple has no
            // /proc filesystem; mach_vm_region is the supported API on macOS.
            mach_vm_address_t region_addr{ reinterpret_cast<mach_vm_address_t>(address) };
            mach_vm_size_t    region_size{ 0 };
            vm_region_basic_info_data_64_t mach_info{};
            mach_msg_type_number_t info_count{ VM_REGION_BASIC_INFO_COUNT_64 };
            mach_port_t object_name{};
            const kern_return_t rc{
                ::mach_vm_region(::mach_task_self(),
                                 &region_addr,
                                 &region_size,
                                 VM_REGION_BASIC_INFO_64,
                                 reinterpret_cast<vm_region_info_t>(&mach_info),
                                 &info_count,
                                 &object_name) };
            if (rc != KERN_SUCCESS)
            {
                return info;
            }
            info.base = reinterpret_cast<void*>(region_addr);
            info.size = static_cast<std::size_t>(region_size);
            info.committed = true;
            info.readable  = (mach_info.protection & VM_PROT_READ)    != 0;
            info.executable= (mach_info.protection & VM_PROT_EXECUTE) != 0;
            return info;
#elif VMHOOK_OS_IOS
            // iOS does not expose mach_vm_region / /proc/self/maps.  Return
            // a permissive "looks committed" result so the higher-level
            // validity checks defer to the user-supplied pointer rather
            // than rejecting everything.
            info.base = const_cast<void*>(address);
            info.size = vmhook::os::page_size();
            info.committed = true;
            info.readable  = true;
            return info;
#else
            // Linux and Android both expose /proc/self/maps.
            const std::uintptr_t target{ reinterpret_cast<std::uintptr_t>(address) };
            std::ifstream maps{ "/proc/self/maps" };
            std::string line;
            std::uintptr_t prev_end{ 0 };
            while (std::getline(maps, line))
            {
                if (line.empty())
                {
                    continue;
                }
                std::uintptr_t begin{ 0 };
                std::uintptr_t end{ 0 };
                char perms[5]{};
                const int parsed{ std::sscanf(line.c_str(), "%lx-%lx %4s",
                                              &begin, &end, perms) };
                if (parsed < 3)
                {
                    continue;
                }
                if (target < begin)
                {
                    info.base = reinterpret_cast<void*>(prev_end);
                    info.size = static_cast<std::size_t>(begin - prev_end);
                    info.free = true;
                    return info;
                }
                if (target >= begin && target < end)
                {
                    info.base = reinterpret_cast<void*>(begin);
                    info.size = static_cast<std::size_t>(end - begin);
                    info.committed = true;
                    info.readable   = perms[0] == 'r';
                    info.executable = perms[2] == 'x';
                    return info;
                }
                prev_end = end;
            }
            info.base = reinterpret_cast<void*>(prev_end);
            info.size = (std::numeric_limits<std::uintptr_t>::max)() - prev_end;
            info.free = true;
            return info;
#endif
        }

#if VMHOOK_OS_LINUX || VMHOOK_OS_ANDROID
        namespace detail_signal
        {
            // Thread-local jmp-buf machinery to recover from SIGSEGV during
            // the safe_read sigsetjmp fallback on Linux / Android.  macOS
            // and iOS use mach_vm_read_overwrite so the signal machinery
            // is not needed there; Windows uses ReadProcessMemory.
            struct probe_state
            {
                bool          active{ false };
                volatile bool fault{ false };
                sigjmp_buf    env{};
            };

            inline thread_local probe_state* active_state{ nullptr };

            inline auto handler(int /*sig*/, siginfo_t* /*info*/, void* /*ctx*/) -> void
            {
                if (active_state)
                {
                    active_state->fault = true;
                    ::siglongjmp(active_state->env, 1);
                }
                // Not in a probe; let the default handler take over.
                struct sigaction sa{};
                sa.sa_handler = SIG_DFL;
                ::sigaction(SIGSEGV, &sa, nullptr);
            }

            inline auto install_once() noexcept -> bool
            {
                static const bool installed{ []() noexcept
                {
                    struct sigaction sa{};
                    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
                    sa.sa_sigaction = &handler;
                    ::sigemptyset(&sa.sa_mask);
                    return ::sigaction(SIGSEGV, &sa, nullptr) == 0
                        && ::sigaction(SIGBUS,  &sa, nullptr) == 0;
                }() };
                return installed;
            }
        } // namespace detail_signal
#endif

        /*
            @brief Reads `size` bytes from `src` into `dst` without faulting on bad pointers.
            @details
            On Windows uses ReadProcessMemory on the current process so the
            kernel performs the read with a fault-safe path.  On Linux /
            Android uses process_vm_readv (zero-copy) and falls back to a
            SIGSEGV-catching sigsetjmp probed read.  On macOS / iOS uses
            mach_vm_read_overwrite which kernel-validates the source.
            Returns true on success.
        */
        inline auto safe_read(void* dst, const void* src, std::size_t size) noexcept -> bool
        {
            if (!dst || !src || size == 0)
            {
                return false;
            }
#if VMHOOK_OS_WINDOWS
            SIZE_T transferred{ 0 };
            const BOOL ok{ ::ReadProcessMemory(::GetCurrentProcess(), src, dst,
                                               size, &transferred) };
            return ok && transferred == size;
#elif VMHOOK_OS_MACOS
            mach_vm_size_t transferred{ 0 };
            const kern_return_t rc{ ::mach_vm_read_overwrite(
                ::mach_task_self(),
                reinterpret_cast<mach_vm_address_t>(src),
                static_cast<mach_vm_size_t>(size),
                reinterpret_cast<mach_vm_address_t>(dst),
                &transferred) };
            return rc == KERN_SUCCESS && transferred == size;
#elif VMHOOK_OS_IOS
            // No mach_vm on iOS; do a best-effort memcpy.  Bad pointers
            // will fault — there is no user-callable fault-safe read API
            // on iOS without entitlements.
            std::memcpy(dst, src, size);
            return true;
#elif VMHOOK_OS_LINUX || VMHOOK_OS_ANDROID
            iovec local{ dst, size };
            iovec remote{ const_cast<void*>(src), size };
            const ssize_t n{ ::process_vm_readv(::getpid(), &local, 1, &remote, 1, 0) };
            if (n == static_cast<ssize_t>(size))
            {
                return true;
            }
            // Fall back to signal-protected read.
            if (!detail_signal::install_once())
            {
                return false;
            }
            detail_signal::probe_state state{};
            state.active = true;
            detail_signal::active_state = &state;
            bool success{ false };
            if (::sigsetjmp(state.env, 1) == 0)
            {
                std::memcpy(dst, src, size);
                success = true;
            }
            detail_signal::active_state = nullptr;
            return success && !state.fault;
#else
            (void)dst; (void)src; (void)size;
            return false;
#endif
        }

        /*
            @brief Hints the CPU/OS that an instruction range has been written.
        */
        inline auto flush_instruction_cache(void* address, std::size_t size) noexcept -> void
        {
            if (!address || size == 0)
            {
                return;
            }
#if VMHOOK_OS_WINDOWS
            ::FlushInstructionCache(::GetCurrentProcess(), address, size);
#elif VMHOOK_OS_APPLE
            // Apple ships sys_icache_invalidate as the user-callable API.
            // __builtin___clear_cache emits a reference to compiler-rt's
            // ___clear_cache, which iOS does not link by default.
            ::sys_icache_invalidate(address, size);
#elif defined(__GNUC__) || defined(__clang__)
            __builtin___clear_cache(static_cast<char*>(address),
                                    static_cast<char*>(address) + size);
#else
            (void)address;
            (void)size;
#endif
        }

        // ------------------------------------------------------------
        // Hardware data breakpoints
        // ------------------------------------------------------------
        // x86_64 exposes four debug registers (DR0–DR3) that fire a
        // #DB exception when a configured memory access matches.  On
        // Windows we set them via SetThreadContext and catch the
        // exception with a vectored exception handler; the watch
        // remains zero-overhead until the trap actually fires.
        //
        // Currently a Windows-only path; on other platforms the
        // companion vmhook::watch_static_field<> returns an empty
        // handle and logs an error (there is no polling fallback).
        // The capability flag below lets callers query support at
        // compile time.

#if VMHOOK_OS_WINDOWS && VMHOOK_ARCH_X86_64
        #define VMHOOK_HAS_HW_DATA_BREAKPOINTS 1
#else
        #define VMHOOK_HAS_HW_DATA_BREAKPOINTS 0
#endif

        /*
            @brief Type of memory access the breakpoint should trap on.
        */
        enum class data_breakpoint_kind : std::uint8_t
        {
            write    = 0b01,  // only writes
            read_write = 0b11,  // reads and writes (no execute)
        };

        /*
            @brief Length of the memory window the breakpoint guards.
        */
        enum class data_breakpoint_length : std::uint8_t
        {
            one_byte    = 0b00,
            two_bytes   = 0b01,
            eight_bytes = 0b10,
            four_bytes  = 0b11,
        };

#if VMHOOK_HAS_HW_DATA_BREAKPOINTS
        namespace detail_dr
        {
            /*
                @brief Builds a DR7 control mask enabling the given slot
                       (0-3) for the given access kind / length.
                @details
                Per Intel SDM:
                  - L0/L1/L2/L3 (bits 0,2,4,6) enable the local breakpoint.
                  - R/W and LEN fields live at (16+4*slot) and (18+4*slot)
                    respectively.  We keep the global enables (G*) cleared
                    so the trap only applies to threads we explicitly
                    configure.
            */
            inline auto build_dr7(int slot, data_breakpoint_kind rw,
                                  data_breakpoint_length len) noexcept -> std::uint64_t
            {
                const std::uint64_t local_enable{ std::uint64_t{ 1 } << (slot * 2) };
                const std::uint64_t rw_bits     { static_cast<std::uint64_t>(rw)
                                                  << (16 + slot * 4) };
                const std::uint64_t len_bits    { static_cast<std::uint64_t>(len)
                                                  << (18 + slot * 4) };
                return local_enable | rw_bits | len_bits;
            }

            /*
                @brief Enumerates every thread of the current process.
                @details
                Uses Toolhelp on Windows.  Each thread that belongs to
                this process is passed to `callback(HANDLE)`; the handle
                is closed automatically when the callback returns.
            */
            template<typename callback_type>
            inline auto for_each_thread(callback_type&& callback) -> void
            {
                const HANDLE snap{ ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0) };
                if (snap == INVALID_HANDLE_VALUE)
                {
                    return;
                }
                THREADENTRY32 te{};
                te.dwSize = sizeof(te);
                if (::Thread32First(snap, &te))
                {
                    const DWORD self_pid{ ::GetCurrentProcessId() };
                    do
                    {
                        if (te.dwSize >= FIELD_OFFSET(THREADENTRY32, th32OwnerProcessID) + sizeof(te.th32OwnerProcessID)
                            && te.th32OwnerProcessID == self_pid)
                        {
                            const HANDLE thread{ ::OpenThread(
                                THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME,
                                FALSE, te.th32ThreadID) };
                            if (thread)
                            {
                                callback(thread);
                                ::CloseHandle(thread);
                            }
                        }
                        te.dwSize = sizeof(te);
                    } while (::Thread32Next(snap, &te));
                }
                ::CloseHandle(snap);
            }
        } // namespace detail_dr
#endif
    } // namespace os

    namespace hotspot
    {
        struct frame;

        /*
            @brief Shared memory slot written by the hook trampoline and read by the callback.
            @details
            The trampoline allocates one return_slot on the native stack before invoking
            the user callback.  If the callback calls return_value::set() or
            return_value::cancel(), cancel is set to true and retval holds the raw
            bit-pattern of the value to return.  The trampoline checks cancel after the
            callback returns and either restores normal execution or short-circuits to
            the stored retval.

            Complexity: O(1) — plain struct with brace-initialised fields.
            Exception safety: noexcept — no dynamic allocation.
            Thread safety: not thread-safe; owned by a single trampoline invocation.
        */
        struct return_slot
        {
            bool         cancel{ false };
            std::int64_t retval{ 0 };
        };

        // Forward declarations used by return_value::caller_info; full
        // definitions appear further down in the file.
        struct method;
        struct klass;
        struct symbol;
        struct const_method;
        struct constant_pool;
    }

    class object_base;

    /*
        @brief Handle passed as the first argument to every hook callback.
        @details
        Call set(value) to suppress the original Java method body and return a
        custom value to the caller.  Call cancel() to suppress without a return
        value (for void methods).  If neither is called the original method runs
        normally.

        Example:
            vmhook::hook<MyClass>("getScore",
                [](vmhook::return_value& ret, const std::unique_ptr<MyClass>& self) {
                    ret.set(std::int32_t{ 9999 });   // always return 9999
                });
    */
    class return_value
    {
    public:
        explicit return_value(vmhook::hotspot::return_slot* slot, vmhook::hotspot::frame* frame = nullptr) noexcept
            : return_slot{ slot }, stack_frame{ frame }
        {

        }

        template<typename value_type>
        auto set(const value_type value) noexcept
            -> void
        {
            static_assert(sizeof(value_type) <= sizeof(std::int64_t),
                          "return type too large for hook slot");
            static_assert(std::is_trivially_copyable_v<value_type>,
                          "return_value::set: value_type must be trivially copyable - the slot is "
                          "a raw 64-bit cell, not a managed object.  Pass a primitive, an oop "
                          "pointer (void*), or a small POD by value.");
            this->return_slot->cancel = true;

            // Sign-extend signed integers smaller than int64 BEFORE writing the
            // slot.  Otherwise a hook returning int8_t{-1} would land in the
            // 64-bit retval cell as 0x00000000000000FF and the interpreter's
            // ireturn would pop +255 instead of -1.  memcpy alone only copies
            // the low N bytes, with the upper bits left at zero from the
            // assignment below.
            if constexpr (std::is_signed_v<value_type>
                       && std::is_integral_v<value_type>
                       && sizeof(value_type) < sizeof(std::int64_t))
            {
                this->return_slot->retval = static_cast<std::int64_t>(value);
            }
            else
            {
                this->return_slot->retval = 0;
                std::memcpy(&this->return_slot->retval, &value, sizeof(value_type));
            }
        }

        /*
            @brief Typed null-return overload for hooks on methods that return a
                   Java reference type.
            @details
            Equivalent to set<vmhook::oop_t>(nullptr) but documents the slot's
            Java type at the call site:

                // Before - works but says nothing about what null means
                ret.set<vmhook::oop_t>(nullptr);

                // After - reads as "return null MovingObjectPosition"
                ret.set<sdk::moving_object_position>(nullptr);

            The wrapper_type template argument is documentation only - we never
            touch an instance, just write a null OOP into the 64-bit retval slot.
            Selected only when wrapper_type derives from vmhook::object_base, so
            primitive set<int32_t>(...) calls remain on the integer path above.
        */
        template<typename wrapper_type>
            requires std::is_base_of_v<vmhook::object_base, wrapper_type>
        auto set(std::nullptr_t) noexcept
            -> void
        {
            this->return_slot->cancel = true;
            this->return_slot->retval = 0;
        }

        auto cancel() noexcept
            -> void
        {
            this->return_slot->cancel = true;
        }

        /*
            @brief Mutates a Java method argument in-place on the interpreter stack.
            @details
            Writes value directly into the local-variable slot at the given index within
            the intercepted frame.  index 0 is 'this' for instance methods; index 0 is
            the first argument for static methods.  The frame pointer must be valid
            (i.e. the callback was registered via vmhook::hook with frame support enabled).

            Complexity: O(1).
            Exception safety: noexcept — writes to pre-allocated stack memory.
            Thread safety: not thread-safe; must only be called from the hook callback.

            @param index  Zero-based index of the local-variable slot to overwrite.
            @param value  New value to store; must be trivially copyable and fit in a
                          Java local-variable slot (up to 8 bytes).
            @return true if the write succeeded, false if frame is nullptr or index is
                    out of range.
        */
        template<typename value_type>
        auto set_arg(std::int32_t index, value_type&& value) noexcept
            -> bool;

        /*
            @brief Information about the method that invoked the hooked method.
            @details
            Populated by return_value::caller() when the call originates from
            another HotSpot interpreter frame.  All string fields are empty and
            method is nullptr when the caller frame is not interpreted (compiled,
            native, or unidentifiable).
        */
        struct caller_info
        {
            vmhook::hotspot::method* method{ nullptr };
            std::string class_name{};
            std::string method_name{};
            std::string signature{};

            /*
                @brief Returns true when the caller frame was identified.
            */
            auto valid() const noexcept -> bool
            {
                return this->method != nullptr;
            }
        };

        /*
            @brief Returns information about the caller of the hooked method.
            @details
            Walks the saved-rbp chain on the interpreter stack: the caller's
            frame base lives at [rbp], and its Method* lives at
            [caller_rbp - 24] in the HotSpot x64 interpreter layout.  We
            validate every pointer with the existing safe-read helpers before
            dereferencing, so an unfamiliar frame layout produces an empty
            caller_info rather than a crash.

            Complexity: O(1) once the VMStruct offsets are cached.
            Exception safety: noexcept — bad pointers map to an empty result.
            Thread safety: must be called only from the hook callback.
        */
        auto caller() const noexcept -> caller_info;

        /*
            @brief Captures the full interpreter call stack as a vector
                   of caller_info, starting with the hooked method's
                   immediate caller (index 0) and walking outward.
            @details
            Walks the saved-rbp chain the same way `caller()` does, but
            keeps going as long as every read passes the safe-pointer
            checks.  The walk terminates when:
              - `max_depth` frames have been captured, or
              - the next saved-rbp slot fails validation (typically
                because we have walked into a compiled or native frame
                that does not follow the interpreter layout), or
              - the next Method* fails validation.

            The first entry (index 0) is the same value `caller()`
            returns.  Subsequent entries describe progressively older
            interpreter frames.  An empty result means the immediate
            caller already failed validation (compiled / native / no
            frame).

            Typical use from inside a hook detour:

            @code
                auto detour = [](vmhook::return_value& ret, const std::unique_ptr<my_class>& self)
                {
                    for (auto const& frame : ret.stack_trace())
                    {
                        VMHOOK_LOG("  at {}.{}{}",
                                   frame.class_name, frame.method_name, frame.signature);
                    }
                };
            @endcode

            Complexity: O(D) where D = depth of interpreter frames.
            Exception safety: noexcept — every dereference is gated by
                vmhook::hotspot::is_valid_pointer.
            Thread safety: must be called only from the hook callback,
                while the interpreter frame layout above the hook is
                still intact.

            @param max_depth Hard cap on the number of frames returned;
                             defaults to 64.  Pass 0 for the default.
        */
        auto stack_trace(std::size_t max_depth = 64) const noexcept
            -> std::vector<caller_info>;

        /*
            @brief Returns the intercepted interpreter frame, or nullptr.
            @details
            Exposed for advanced use cases that need to walk the call stack
            themselves; callers should prefer caller() when only the
            immediate caller is required.
        */
        auto frame() const noexcept -> vmhook::hotspot::frame*
        {
            return this->stack_frame;
        }

    private:
        vmhook::hotspot::return_slot* return_slot{ nullptr };
        vmhook::hotspot::frame* stack_frame{ nullptr };
    };

    // --- Forward declarations ------------------------------------------------

    namespace hotspot
    {
        struct vm_struct_entry_t;
        struct vm_type_entry_t;
        struct symbol;
        struct constant_pool;
        struct const_method;
        struct method;
        struct klass;
        struct class_loader_data;
        struct class_loader_data_graph;
        struct dictionary;
        struct java_thread;
        struct frame;
        class  midi2i_hook;
        struct hooked_method;
        struct i2i_hook_data;
        struct field_entry_t;

        /*
            @brief Decodes a compressed 32-bit OOP into a full 64-bit heap pointer.
            @details
            HotSpot stores object references as 32-bit values when UseCompressedOops
            is enabled.  This function reverses that encoding.
            Defined later in the file after the OOP-compression constants are resolved.

            Complexity: O(1).
            Exception safety: noexcept.
        */
        static auto decode_oop_pointer(std::uint32_t compressed) noexcept
            -> void*;

        /*
            @brief Encodes a full 64-bit heap pointer as a compressed 32-bit OOP.
            @details
            Inverse of decode_oop_pointer.  Used when writing object references back
            into Java fields that store compressed OOPs.
            Defined later in the file after the OOP-compression constants are resolved.

            Complexity: O(1).
            Exception safety: noexcept.
        */
        static auto encode_oop_pointer(void* decoded) noexcept
            -> std::uint32_t;

        /*
            @brief Attaches the calling native thread to the JVM as a Java thread if needed.
            @details
            Checks whether the current OS thread is already registered in HotSpot's
            thread list.  If not, performs the attach dance so that JNI calls and heap
            allocations from C++ code are legal.
            Defined later in the file after java_thread is fully declared.

            Complexity: O(N) where N = number of active Java threads (thread-list walk).
            Exception safety: noexcept — returns false on failure.
        */
        static auto ensure_current_java_thread() noexcept
            -> bool;
    }

    namespace detail
    {
        inline auto find_call_stub_entry() noexcept -> void*;

        /*
            @brief Forward declaration for the host-context-classloader inheritance
                   helper, defined further down alongside the JNI helpers it relies on.
        */
        inline auto inherit_host_context_classloader_for_current_thread() noexcept
            -> void;

        /*
            @brief Forward declaration for the JVM primitive field-width helper used
                   by field_proxy::set's size-mismatch guard.  Defined alongside
                   sig_char_to_basic_type so the actual table lives next to its
                   sibling helper, but the field_proxy template body needs to see
                   the name at parse time (GCC's -Wtemplate-body is strict about
                   qualified-name lookup inside templates).
        */
        inline auto jvm_primitive_byte_width(std::string_view signature) noexcept
            -> std::size_t;
    }

    /*
        @brief Maps C++ wrapper types to their corresponding internal Java class names.
        @details
        Populated by vmhook::register_class() when a C++ wrapper type is associated
        with a Java class name.  Used by vmhook::hook() and other APIs that need to
        look up the Java class corresponding to a given C++ wrapper type at runtime.
        Keys   - std::type_index values derived from typeid() of the C++ wrapper type.
        Values - internal JVM class names using '/' separators (e.g. "java/lang/String").
        @see vmhook::register_class, vmhook::hook

        Thread-safety: every read / mutation of this map AND of g_type_factory_map
        below must hold registration_mutex.  register_class() can be called from
        whichever thread the user runs setup on, while extract_frame_arg /
        jni_signature_for_arg / on_class_loaded read the map from hook detour
        threads.  Without the mutex, a racing insert during an unordered_map
        rehash corrupts the buckets.  The map only ever grows, so a plain
        std::mutex is enough — read contention is irrelevant on the steady-state
        cache-hit path.
    */
    inline std::unordered_map<std::type_index, std::string> type_to_class_map{};
    inline std::mutex registration_mutex{};

    /*
        @brief Factory function type that creates a std::unique_ptr<T> from a raw OOP.
        @details
        Populated by vmhook::register_class() alongside type_to_class_map.
        Used by field_proxy::get_as<T>() and frame::get_arguments() to construct
        C++ wrapper objects from decoded Java object references.
        Keys   - internal JVM class names (e.g. "java/lang/String").
        Values - lambda functions: +[](void* oop) { return std::make_unique<T>(oop); }
    */
    // The factory returns a heap-allocated wrapper as a raw pointer so the
    // factory's signature is free of `std::unique_ptr<object_base>`.
    // libstdc++ and libc++ both eagerly check sizeof(object_base) inside
    // unique_ptr's destructor static_assert when the lambda is parsed, even
    // though the unique_ptr never actually destructs (the caller calls
    // .release() immediately).  Returning the raw pointer avoids the
    // incomplete-type-in-destructor instantiation entirely.  Callers wrap
    // the returned pointer in a unique_ptr at the consumption site where
    // object_base is already complete.
    using type_factory_function_t = class object_base*(*)(void* instance);
    inline std::unordered_map<std::string, type_factory_function_t> g_type_factory_map{};

    template<class wrapper_type>
    static auto register_class(std::string_view class_name) noexcept
        -> bool;

    class object_base;
    template<typename derived = void> class object;
    class field_proxy;
    class collection;
    class list;
    class set;
    class linked_list;
    class map;
    class hash_map;

    inline auto read_java_string(void* string_oop)
        -> std::string;

    inline auto make_java_string(std::string_view value) noexcept
        -> void*;

    inline auto write_java_string(void* string_oop, std::string_view value) noexcept
        -> void;

    inline auto decode_array_oop(std::uint32_t compressed)
        -> void*;

    inline auto set_str_field(const field_proxy& field, std::string_view value) noexcept
        -> void;

    inline auto field_oop(const field_proxy& field) noexcept
        -> void*;

    inline auto set_bool_array(const field_proxy& field, const std::vector<bool>& values) noexcept
        -> void;

    inline auto set_str_array(const field_proxy& field, const std::vector<std::string>& values) noexcept
        -> void;

    template<typename element_type>
    inline auto set_prim_array(const field_proxy& field, const std::vector<element_type>& values) noexcept
        -> void;

    // --- Compile-time type traits --------------------------------------------

    namespace detail
    {
        /*
            @brief Type trait that detects whether a type is a specialisation of std::vector.
            @details
            Primary template inherits from std::false_type.  The partial specialisation for
            std::vector<value_type, allocator_type> inherits from std::true_type and
            exposes the element type as value_type_t.  cv-ref qualifiers are stripped via
            std::remove_cvref_t before the check is applied (see is_vector_v).

            Exception safety: noexcept — compile-time trait, no runtime cost.
        */
        // Helper for `static_assert(false)`-in-discarded-branch.  Pre-C++23
        // this would fire eagerly during template-definition; C++23 P2593R1
        // makes it lazy ONLY if it depends on the template parameter.  We
        // target C++23, so a one-line `dependent_false_v<T>` keeps the
        // assertion benign for branches that are discarded for a particular
        // T but still fires loudly when T actually reaches the bad branch.
        template<typename...>
        inline constexpr bool dependent_false_v{ false };

        template<typename type>
        struct is_vector : std::false_type {};

        // Same template-parameter-shadowing rule as is_unique_ptr below:
        // do not name the parameter `value_type` — the std::true_type base
        // already exports a `value_type` typedef (= bool, inherited from
        // std::integral_constant<bool, true>) which silently wins in name
        // lookup over the same-named template parameter, so
        // `using value_type_t = value_type;` resolves to `bool`.  No
        // caller currently reaches is_vector<...>::value_type_t (only
        // `::value` is used), but ship the safe spelling so a future
        // user doesn't get bitten.
        template<typename element_type, typename allocator_type>
        struct is_vector<std::vector<element_type, allocator_type>> : std::true_type
        {
            using value_type_t = element_type;
        };

        /*
            @brief Convenience bool constant for vmhook::detail::is_vector<T>.
            @details
            Strips cv-ref qualifiers from type before testing so that
            is_vector_v<const std::vector<int>&> == true.
        */
        template<typename type>
        inline constexpr bool is_vector_v{ is_vector<std::remove_cvref_t<type>>::value };

        /*
            @brief Type trait that detects whether a type is a specialisation of std::unique_ptr.
            @details
            Primary template inherits from std::false_type.  The partial specialisation for
            std::unique_ptr<value_type, deleter_type> inherits from std::true_type and
            exposes the pointed-to type as value_type_t.  Used by hook argument dispatch
            to identify wrapper objects passed by unique_ptr and unwrap the raw OOP.

            Exception safety: noexcept — compile-time trait, no runtime cost.
        */
        template<typename type>
        struct is_unique_ptr : std::false_type {};

        // NOTE: the template parameter cannot be named `value_type` — the
        // primary `std::true_type` base brings in its own `value_type`
        // typedef (= bool, from std::integral_constant<bool, true>), and
        // that inherited typedef shadows a same-named template parameter
        // when looking up names inside the class body.  The result is that
        // `using value_type_t = value_type;` silently resolves to
        // `using value_type_t = bool;`, every unique_ptr<Wrapper> arg
        // collapses to is_base_of<object_base, bool> == false at the call
        // site, and the JNI arg slot is left zero-initialised.  Use `wrapped_type`
        // (or any name that does not collide with the base typedef) instead.
        template<typename wrapped_type, typename deleter_type>
        struct is_unique_ptr<std::unique_ptr<wrapped_type, deleter_type>> : std::true_type
        {
            using value_type_t = wrapped_type;
        };

        /*
            @brief Convenience bool constant for vmhook::detail::is_unique_ptr<T>.
            @details
            Strips cv-ref qualifiers from type before testing so that
            is_unique_ptr_v<const std::unique_ptr<Foo>&> == true.
        */
        template<typename type>
        inline constexpr bool is_unique_ptr_v{ is_unique_ptr<std::remove_cvref_t<type>>::value };

    }

    // --- HotSpot internals ----------------------------------------------------

    namespace hotspot
    {
        // https://github.com/openjdk/jdk/blob/master/src/hotspot/share/runtime/vmStructs.hpp
        struct vm_type_entry_t
        {
            const char* type_name;
            const char* superclass_name;
            std::int32_t  is_oop_type_type;
            std::int32_t  is_integer_type;
            std::int32_t  is_unsigned;
            std::uint64_t size;
        };

        // https://github.com/openjdk/jdk/blob/master/src/hotspot/share/runtime/vmStructs.hpp
        struct vm_struct_entry_t
        {
            const char* type_name;
            const char* field_name;
            const char* type_string;
            std::int32_t  is_static;
            std::uint64_t offset;
            void* address;
        };

        /*
            @brief Returns the module handle of the HotSpot JVM library loaded in the process.
            @details
            Returns the handle for jvm.dll on Windows, or libjvm.so on Linux.  Cached
            after the first lookup; if no JVM library is present the function returns
            nullptr (callers must check before dereferencing the result).
        */
        inline auto get_jvm_module() noexcept
            -> vmhook::os::module_handle
        {
            static vmhook::os::module_handle module{ vmhook::os::find_jvm_module() };
            return module;
        }

        /*
            @brief Returns a pointer to the global array of HotSpot VM type entries.
            @details
            Resolves gHotSpotVMTypes from the JVM module via the OS dynamic-symbol
            API on first call and caches the typed pointer so subsequent calls are free.
        */
        inline auto get_vm_types() noexcept
            -> vmhook::hotspot::vm_type_entry_t*
        {
            static vmhook::hotspot::vm_type_entry_t* pointer{ []() noexcept
                -> vmhook::hotspot::vm_type_entry_t*
                {
                    void* const procedure_address{ vmhook::os::get_proc_address(
                        vmhook::hotspot::get_jvm_module(), "gHotSpotVMTypes") };
                    if (!procedure_address)
                    {
                        return nullptr;
                    }
                    return *reinterpret_cast<vmhook::hotspot::vm_type_entry_t**>(procedure_address);
                }() };
            return pointer;
        }

        /*
            @brief Returns a pointer to the global array of HotSpot VM struct entries.
            @details
            Resolves gHotSpotVMStructs from the JVM module via the OS dynamic-symbol
            API on first call and caches the typed pointer so subsequent calls are free.
        */
        inline auto get_vm_structs() noexcept
            -> vmhook::hotspot::vm_struct_entry_t*
        {
            static vmhook::hotspot::vm_struct_entry_t* pointer{ []() noexcept
                -> vmhook::hotspot::vm_struct_entry_t*
                {
                    void* const procedure_address{ vmhook::os::get_proc_address(
                        vmhook::hotspot::get_jvm_module(), "gHotSpotVMStructs") };
                    if (!procedure_address)
                    {
                        return nullptr;
                    }
                    return *reinterpret_cast<vmhook::hotspot::vm_struct_entry_t**>(procedure_address);
                }() };
            return pointer;
        }

        /*
            @brief Searches the gHotSpotVMTypes array for a type entry by name.
        */
        static auto iterate_type_entries(const char* const type_name) noexcept
            -> vmhook::hotspot::vm_type_entry_t*
        {
            if (!type_name)
            {
                return nullptr;
            }
            for (vmhook::hotspot::vm_type_entry_t* entry{ vmhook::hotspot::get_vm_types() }; entry && entry->type_name; ++entry)
            {
                if (!std::strcmp(entry->type_name, type_name))
                {
                    return entry;
                }
            }
            return nullptr;
        }

        /*
            @brief Searches the gHotSpotVMStructs array for a field entry by type and field name.
            @details
            Defensively skips any entry whose field_name is null - the standard HotSpot
            terminator zeroes both type_name and field_name, but custom JVMs / JVMTI
            agents have been observed to publish partial entries where type_name is set
            and field_name is null.  Without the guard, strcmp(nullptr, x) is UB and
            crashes the host process on the first lookup that walks past such an entry.
        */
        static auto iterate_struct_entries(const char* const type_name, const char* const field_name) noexcept
            -> vmhook::hotspot::vm_struct_entry_t*
        {
            if (!type_name || !field_name)
            {
                return nullptr;
            }
            for (vmhook::hotspot::vm_struct_entry_t* entry{ vmhook::hotspot::get_vm_structs() }; entry && entry->type_name; ++entry)
            {
                if (!entry->field_name)
                {
                    continue;
                }
                if (!std::strcmp(entry->type_name, type_name) && !std::strcmp(entry->field_name, field_name))
                {
                    return entry;
                }
            }
            return nullptr;
        }

        /*
            @brief Checks whether a pointer refers to committed readable memory.
            @details
            Extends is_valid_pointer with an OS-region query to verify that the memory
            region containing pointer is actually committed and readable.  Implementation
            uses VirtualQuery on Windows and /proc/self/maps on Linux via os::query_region.
        */
        static auto is_readable_pointer(const void* const pointer) noexcept
            -> bool
        {
            const std::uintptr_t addr{ reinterpret_cast<std::uintptr_t>(pointer) };

            if (addr <= vmhook::os::user_address_floor
                || addr >= vmhook::os::user_address_ceiling
                || (addr & 0x7) != 0)
            {
                return false;
            }

            const vmhook::os::region_info info{ vmhook::os::query_region(pointer) };
            return info.committed && info.readable && !info.guarded;
        }

        /*
            @brief Checks whether a pointer is likely valid for dereferencing.
            @details
            Filters out null pointers, low sentinel values used by HotSpot to mark
            the end of linked lists, kernel-space addresses above the user ceiling,
            AND well-known debug-poison patterns (0xDEADBEEF, 0xCAFEBABE,
            0xCCCCCCCC, 0xCDCDCDCD, 0xBAADF00D, 0xFEEEFEEE).  The previous
            implementation only checked the address range, so those patterns
            (which fall inside user-space) sneaked through and crashed on
            dereference - the very thing the function exists to prevent.
            Pointers are also required to be at least 2-byte aligned; HotSpot
            never produces odd-address Klass / Method / oop pointers.
        */
        inline static auto is_valid_pointer(const void* const pointer) noexcept
            -> bool
        {
            const std::uintptr_t addr{ reinterpret_cast<std::uintptr_t>(pointer) };
            if (addr <= vmhook::os::user_address_floor || addr >= vmhook::os::user_address_ceiling)
            {
                return false;
            }
            // HotSpot oops / Klass / Method are at least 8-byte aligned in
            // practice; require 2 to reject obviously-bogus odd addresses
            // without rejecting unaligned interior pointers used by some
            // helpers (e.g. byte-pointer reads inside a struct).
            if ((addr & 0x1u) != 0u)
            {
                return false;
            }
            // Reject the common debug-fill / sentinel patterns the MSVC and
            // mingw runtimes leave in uninitialised memory and freed blocks.
            // A pointer whose low 32 bits exactly match one of these is
            // overwhelmingly a tag, not a real address.
            const std::uint32_t low32{ static_cast<std::uint32_t>(addr) };
            switch (low32)
            {
                case 0xDEADBEEFu:   // generic C debug sentinel
                case 0xCAFEBABEu:   // Java class-file magic, also commonly used as a sentinel
                case 0xCCCCCCCCu:   // MSVC uninitialised stack
                case 0xCDCDCDCDu:   // MSVC uninitialised heap
                case 0xBAADF00Du:   // Windows LocalAlloc uninitialised
                case 0xFEEEFEEEu:   // Windows HeapFree
                case 0xABABABABu:   // Windows HeapAlloc no-man's land
                case 0xFDFDFDFDu:   // MSVC heap no-man's land
                case 0xDDDDDDDDu:   // MSVC freed heap
                    return false;
                default:
                    break;
            }
            return true;
        }

        /*
            @brief Removes GC tag bits from a HotSpot pointer to recover the real address.
            @details
            Masking with user_address_ceiling strips high GC tag bits and recovers the
            underlying canonical user-space address.
        */
        inline static auto untag_pointer(const void* const pointer) noexcept
            -> const void*
        {
            return reinterpret_cast<const void*>(
                reinterpret_cast<std::uintptr_t>(pointer) & vmhook::os::user_address_ceiling);
        }

        /*
            @brief Reads a 32-bit pointer field from a JVM structure and zero-extends it.
        */
        template<typename structure_type>
        inline static auto read_pointer(const void* const base, const std::uint64_t offset) noexcept
            -> structure_type*
        {
            const std::uint32_t raw{ *reinterpret_cast<const std::uint32_t*>(reinterpret_cast<const std::uint8_t*>(base) + offset) };
            return reinterpret_cast<structure_type*>(static_cast<std::uintptr_t>(raw));
        }

        /*
            @brief Safely reads a pointer value from a memory address without faulting.
            @details
            Uses os::safe_read which maps to ReadProcessMemory on Windows and a
            fault-safe path on Linux.  Pre-checks filter out null, low, non-canonical,
            and unaligned addresses before crossing the OS boundary.
        */
        static auto safe_read_pointer(const void* const pointer) noexcept
            -> const void*
        {
            if (!pointer)
            {
                return nullptr;
            }

            const std::uintptr_t addr{ reinterpret_cast<std::uintptr_t>(pointer) };

            if (addr <= vmhook::os::user_address_floor
                || addr >= vmhook::os::user_address_ceiling
                || (addr & 0x7) != 0)
            {
                return nullptr;
            }

            const void* result{ nullptr };
            if (!vmhook::os::safe_read(&result, pointer, sizeof(result)))
            {
                return nullptr;
            }

            return result;
        }

        /*
            @brief Represents a HotSpot internal Symbol object.
            @details
            Symbols are interned strings used throughout the JVM to represent class names,
            method names, field names, and type signatures. The layout is resolved at
            runtime via gHotSpotVMStructs using the offsets of Symbol._length and Symbol._body.
        */
        struct symbol
        {
            /*
                @brief Converts this HotSpot Symbol to a std::string.
                @return A std::string containing a copy of the symbol's character data,
                        or an empty string on failure.
            */
            auto to_string() const
                -> std::string
            {
                static const vmhook::hotspot::vm_struct_entry_t* const length_entry{ vmhook::hotspot::iterate_struct_entries("Symbol", "_length") };
                static const vmhook::hotspot::vm_struct_entry_t* const body_entry{ vmhook::hotspot::iterate_struct_entries("Symbol", "_body") };

                try
                {
                    if (!length_entry)
                    {
                        throw vmhook::exception{ "Failed to find Symbol._length entry." };
                    }

                    if (!body_entry)
                    {
                        throw vmhook::exception{ "Failed to find Symbol._body entry." };
                    }

                    if (!vmhook::hotspot::safe_read_pointer(this))
                    {
                        return std::string{};
                    }

                    const std::uint16_t length{ *reinterpret_cast<const std::uint16_t*>(reinterpret_cast<const std::uint8_t*>(this) + length_entry->offset) };
                    const char* const body{ reinterpret_cast<const char*>(reinterpret_cast<const std::uint8_t*>(this) + body_entry->offset) };

                    if (!vmhook::hotspot::is_valid_pointer(body) || length == 0 || length > 0x1000)
                    {
                        return std::string{};
                    }

                    return std::string{ body, length };
                }
                catch (const std::exception& exception)
                {
                    VMHOOK_LOG("{} symbol.to_string() {}", vmhook::error_tag, exception.what());
                    return std::string{};
                }
            }
        };

        /*
            @brief Represents a HotSpot internal ConstantPool object.
            @details
            The constant pool holds all constants referenced by a Java class.
            The layout is resolved at runtime via gHotSpotVMStructs using the size
            of the ConstantPool type to locate the base of the pool entries array.
        */
        struct constant_pool
        {
            /*
                @brief Returns a pointer to the base of the constant pool entries array.
                @details
                ConstantPool entries are stored immediately after the fixed-size ConstantPool
                header in memory (no separate heap allocation):
                  [ ConstantPool header (size from gHotSpotVMTypes) ][ entry[0] ][ entry[1] ] ...
                Entry indices are 1-based: index 0 is unused, valid indices start at 1.
                Each entry is pointer-sized (8 bytes on x64): it may hold a Symbol*, a primitive
                constant, or other data depending on the tag byte in _tags.
                The size of the header is read at runtime from gHotSpotVMTypes so this works
                across all JDK versions regardless of header size changes.
            */
            auto get_base() const
                -> void**
            {
                static const vmhook::hotspot::vm_type_entry_t* const entry{ vmhook::hotspot::iterate_type_entries("ConstantPool") };

                try
                {
                    if (!entry)
                    {
                        throw vmhook::exception{ "Failed to find ConstantPool entry." };
                    }

                    return reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::constant_pool*>(this)) + entry->size);
                }
                catch (const std::exception& exception)
                {
                    VMHOOK_LOG("{} constant_pool.get_base() {}", vmhook::error_tag, exception.what());
                    return nullptr;
                }
            }

            /*
                @brief Returns the number of entries in the constant pool, or -1
                       when the length field is not exported by this JDK.
                @details
                ConstantPool::_length is a VMStruct-exported int.  Callers use it
                to bound an entry index before reading base[index]; a return of -1
                means "unknown, skip the bound check" so behaviour degrades to the
                pre-existing is_valid_pointer / is_readable_pointer guards rather
                than rejecting every lookup on a JDK that drops the field.
            */
            auto get_length() const noexcept
                -> std::int32_t
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("ConstantPool", "_length") };
                if (!entry || !vmhook::hotspot::is_valid_pointer(this))
                {
                    return -1;
                }
                return *reinterpret_cast<const std::int32_t*>(reinterpret_cast<const std::uint8_t*>(this) + entry->offset);
            }
        };

        /*
            @brief Represents a HotSpot internal ConstMethod object.
            @details
            ConstMethod holds the immutable metadata of a Java method, including its name,
            signature, and a reference to the owning class constant pool.
            The layout is resolved at runtime via gHotSpotVMStructs.
        */
        struct const_method
        {
            /*
                @brief Returns a pointer to the constant pool of the owning class.
            */
            auto get_constants() const
                -> vmhook::hotspot::constant_pool*
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("ConstMethod", "_constants") };

                try
                {
                    if (!entry)
                    {
                        throw vmhook::exception{ "Failed to find ConstMethod._constants entry." };
                    }

                    return *reinterpret_cast<constant_pool**>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::const_method*>(this)) + entry->offset);
                }
                catch (const std::exception& exception)
                {
                    VMHOOK_LOG("{} const_method.get_constants() {}", vmhook::error_tag, exception.what());
                    return nullptr;
                }
            }

            /*
                @brief Returns the symbol representing the name of this method.
            */
            auto get_name() const
                -> vmhook::hotspot::symbol*
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("ConstMethod", "_name_index") };

                try
                {
                    if (!entry)
                    {
                        throw vmhook::exception{ "Failed to find ConstMethod._name_index entry." };
                    }
                    if (!vmhook::hotspot::is_valid_pointer(this))
                    {
                        return nullptr;
                    }

                    const std::uint16_t index{ *reinterpret_cast<std::uint16_t*>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::const_method*>(this)) + entry->offset) };
                    auto* const cp{ this->get_constants() };
                    if (!cp || !vmhook::hotspot::is_valid_pointer(cp))
                    {
                        return nullptr;
                    }
                    void** const base{ cp->get_base() };
                    if (!base || !vmhook::hotspot::is_valid_pointer(base))
                    {
                        return nullptr;
                    }
                    // Bound the index against ConstantPool::_length (when the JDK
                    // exports it) so a corrupt/out-of-range _name_index can't read
                    // past the entry array, and verify the specific slot is mapped
                    // before dereferencing it - an unmapped element read here would
                    // be an access violation rather than a recoverable nullptr.
                    const std::int32_t cp_length{ cp->get_length() };
                    if (cp_length >= 0 && index >= cp_length)
                    {
                        return nullptr;
                    }
                    if (!vmhook::hotspot::is_readable_pointer(&base[index]))
                    {
                        return nullptr;
                    }
                    void* const entry_pointer{ base[index] };
                    if (!entry_pointer || !vmhook::hotspot::is_valid_pointer(entry_pointer))
                    {
                        return nullptr;
                    }
                    return reinterpret_cast<vmhook::hotspot::symbol*>(entry_pointer);
                }
                catch (const std::exception& exception)
                {
                    VMHOOK_LOG("{} const_method.get_name() {}", vmhook::error_tag, exception.what());
                    return nullptr;
                }
            }

            /*
                @brief Returns the symbol representing the signature of this method.
            */
            auto get_signature() const
                -> vmhook::hotspot::symbol*
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("ConstMethod", "_signature_index") };

                try
                {
                    if (!entry)
                    {
                        throw vmhook::exception{ "Failed to find ConstMethod._signature_index entry." };
                    }
                    if (!vmhook::hotspot::is_valid_pointer(this))
                    {
                        return nullptr;
                    }

                    const std::uint16_t index{ *reinterpret_cast<std::uint16_t*>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::const_method*>(this)) + entry->offset) };
                    auto* const cp{ this->get_constants() };
                    if (!cp || !vmhook::hotspot::is_valid_pointer(cp))
                    {
                        return nullptr;
                    }
                    void** const base{ cp->get_base() };
                    if (!base || !vmhook::hotspot::is_valid_pointer(base))
                    {
                        return nullptr;
                    }
                    // Bound the index against ConstantPool::_length (when the JDK
                    // exports it) so a corrupt/out-of-range _signature_index can't
                    // read past the entry array, and verify the specific slot is
                    // mapped before dereferencing it - an unmapped element read here
                    // would be an access violation rather than a recoverable nullptr.
                    const std::int32_t cp_length{ cp->get_length() };
                    if (cp_length >= 0 && index >= cp_length)
                    {
                        return nullptr;
                    }
                    if (!vmhook::hotspot::is_readable_pointer(&base[index]))
                    {
                        return nullptr;
                    }
                    void* const entry_pointer{ base[index] };
                    if (!entry_pointer || !vmhook::hotspot::is_valid_pointer(entry_pointer))
                    {
                        return nullptr;
                    }
                    return reinterpret_cast<vmhook::hotspot::symbol*>(entry_pointer);
                }
                catch (const std::exception& exception)
                {
                    VMHOOK_LOG("{} const_method.get_signature() {}", vmhook::error_tag, exception.what());
                    return nullptr;
                }
            }
        };

        struct method;

        /*
            @brief Heuristic recovery of Method::_adapter on JDKs where the
                   field is no longer exported via gHotSpotVMStructs.
            @details
            JDK 9 dropped _adapter from the VMStructs export (the
            serviceability agent stopped needing it).  This helper scans
            candidate 8-byte-aligned slots inside the given probe Method,
            dereferences each one, and treats the target as an
            AdapterHandlerEntry candidate.  Each candidate is validated
            by reading the AHE's exported _c2i_entry / _i2c_entry slots
            and verifying both point into executable memory (the code
            cache).  Returns the byte offset within Method where _adapter
            lives, or 0 on failure.

            Defined out-of-line below the method struct so it can call
            into the method's own getters.

            Exception safety: noexcept.
        */
        inline auto detect_adapter_offset_from_method(method* probe) noexcept
            -> std::size_t;

        /*
            @brief Represents a HotSpot internal Method object.
            @details
            The Method object is the JVM's internal representation of a Java method.
            It holds all runtime metadata needed to invoke, compile, and profile a method,
            including its entry points, access flags, compilation flags, and a pointer
            to its immutable ConstMethod.
            The layout is resolved at runtime via gHotSpotVMStructs.
        */
        struct method
        {
            /*
                @brief Returns the interpreter-to-interpreter entry point of this method.
                @details
                The i2i entry is the native code stub invoked when an interpreted method
                calls another interpreted method. It is used as the hook location target
                in midi2i_hook.
            */
            auto get_i2i_entry() const
                -> void*
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("Method", "_i2i_entry") };

                try
                {
                    if (!entry)
                    {
                        throw vmhook::exception{ "Failed to find Method._i2i_entry entry." };
                    }

                    return *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::method*>(this)) + entry->offset);
                }
                catch (const std::exception& exception)
                {
                    VMHOOK_LOG("{} method.get_i2i_entry() {}", vmhook::error_tag, exception.what());
                    return nullptr;
                }
            }

            /*
                @brief Returns the from-interpreted entry point of this method.
            */
            auto get_from_interpreted_entry() const
                -> void*
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("Method", "_from_interpreted_entry") };

                try
                {
                    if (!entry)
                    {
                        throw vmhook::exception{ "Failed to find Method._from_interpreted_entry entry." };
                    }

                    return *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::method*>(this)) + entry->offset);
                }
                catch (const std::exception& exception)
                {
                    VMHOOK_LOG("{} method.get_from_interpreted_entry() {}", vmhook::error_tag, exception.what());
                    return nullptr;
                }
            }

            /*
                @brief Returns a pointer to the access flags of this method.
                @details
                Access flags encode Java visibility modifiers (public, private, static, etc.)
                as well as JVM-internal flags such as NO_COMPILE.
            */
            auto get_access_flags() const
                -> std::uint32_t*
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("Method", "_access_flags") };

                try
                {
                    if (!entry)
                    {
                        throw vmhook::exception{ "Failed to find Method._access_flags entry." };
                    }

                    return reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::method*>(this)) + entry->offset);
                }
                catch (const std::exception& exception)
                {
                    VMHOOK_LOG("{} method.get_access_flags() {}", vmhook::error_tag, exception.what());
                    return nullptr;
                }
            }

            /*
                @brief Returns a pointer to the internal method flags of this method.
                @details
                These flags encode HotSpot-internal method properties such as _dont_inline.
            */
            auto get_flags() const
                -> std::uint16_t*
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("Method", "_flags") };

                if (!entry)
                {
                    return nullptr;
                }

                return reinterpret_cast<std::uint16_t*>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::method*>(this)) + entry->offset);
            }

            /*
                @brief Returns a pointer to the ConstMethod of this method.
            */
            auto get_const_method() const
                -> vmhook::hotspot::const_method*
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("Method", "_constMethod") };

                try
                {
                    if (!entry)
                    {
                        throw vmhook::exception{ "Failed to find Method._constMethod entry." };
                    }

                    return *reinterpret_cast<vmhook::hotspot::const_method**>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::method*>(this)) + entry->offset);
                }
                catch (const std::exception& exception)
                {
                    VMHOOK_LOG("{} method.get_const_method() {}", vmhook::error_tag, exception.what());
                    return nullptr;
                }
            }

            /*
                @brief Returns the name of this method as a std::string.
            */
            auto get_name() const
                -> std::string
            {
                // Validate `this` (this Method*) before dereferencing.
                // Stack walks (e.g. return_value::stack_trace) can hand
                // us a Method* that passes is_valid_pointer but isn't a
                // real Method, so any subsequent field read may crash.
                if (!vmhook::hotspot::is_valid_pointer(this))
                {
                    return std::string{};
                }
                const vmhook::hotspot::const_method* const const_method_pointer{ this->get_const_method() };

                try
                {
                    if (!const_method_pointer || !vmhook::hotspot::is_valid_pointer(const_method_pointer))
                    {
                        throw vmhook::exception{ "ConstMethod is nullptr or invalid." };
                    }

                    const vmhook::hotspot::symbol* const symbol_pointer{ const_method_pointer->get_name() };
                    if (!symbol_pointer || !vmhook::hotspot::is_valid_pointer(symbol_pointer))
                    {
                        throw vmhook::exception{ "Symbol is nullptr or invalid." };
                    }

                    return symbol_pointer->to_string();
                }
                catch (const std::exception& exception)
                {
                    VMHOOK_LOG("{} method.get_name() {}", vmhook::error_tag, exception.what());
                    return std::string{};
                }
            }

            /*
                @brief Returns the signature of this method as a std::string.
            */
            auto get_signature() const
                -> std::string
            {
                // Same defensive pattern as get_name(): validate `this`
                // and the ConstMethod / Symbol pointers so stack walks
                // that drift into garbage memory don't crash here.
                if (!vmhook::hotspot::is_valid_pointer(this))
                {
                    return std::string{};
                }
                const vmhook::hotspot::const_method* const const_method_pointer{ this->get_const_method() };

                try
                {
                    if (!const_method_pointer || !vmhook::hotspot::is_valid_pointer(const_method_pointer))
                    {
                        throw vmhook::exception{ "ConstMethod is nullptr or invalid." };
                    }

                    const vmhook::hotspot::symbol* const symbol_pointer{ const_method_pointer->get_signature() };
                    if (!symbol_pointer || !vmhook::hotspot::is_valid_pointer(symbol_pointer))
                    {
                        throw vmhook::exception{ "Symbol is nullptr or invalid." };
                    }

                    return symbol_pointer->to_string();
                }
                catch (const std::exception& exception)
                {
                    VMHOOK_LOG("{} method.get_signature() {}", vmhook::error_tag, exception.what());
                    return std::string{};
                }
            }

            /*
                @brief Returns the current nmethod pointer (_code field).
                @details Non-null when the method has been JIT-compiled; null when interpreted.
                         Writing null forces HotSpot dispatch to treat the method as uncompiled
                         without freeing the compiled code.
            */
            auto get_code() const noexcept
                -> void*
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("Method", "_code") };
                if (!entry)
                {
                    return nullptr;
                }

                return *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::method*>(this)) + entry->offset);
            }

            /*
                @brief Overwrites the _code (nmethod) pointer for this method.
                @details
                Setting code to nullptr forces HotSpot to treat the method as
                interpreted even if a compiled version exists, without invalidating
                the nmethod.  Used during hook installation to suppress the
                compiled entry so that all calls route through the interpreter stub
                that we have already patched.

                Complexity: O(1).
                Exception safety: noexcept — writes a single pointer through a cached offset.

                @param code  New nmethod pointer, or nullptr to deoptimise.
            */
            auto set_code(void* const code) noexcept
                -> void
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("Method", "_code") };
                if (!entry)
                {
                    return;
                }

                *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(this) + entry->offset) = code;
            }

            /*
                @brief Overwrites _from_interpreted_entry.
                @details Reset to _i2i_entry during deoptimisation so interpreted callers
                         route through the interpreter entry stub (which we have patched).
            */
            auto set_from_interpreted_entry(void* const entry_point) noexcept
                -> void
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("Method", "_from_interpreted_entry") };
                if (!entry)
                {
                    return;
                }

                *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(this) + entry->offset) = entry_point;
            }

            /*
                @brief Returns the from-compiled entry point.
                @details The VMStruct field name changed across JDK versions:
                         JDK <= 20: "_from_compiled_code_entry_point"
                         JDK 21+:   "_from_compiled_entry"
                         Both names are tried at first call; the result is cached.
            */
            auto get_from_compiled_entry() const noexcept
                -> void*
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ []() noexcept
                    -> const vmhook::hotspot::vm_struct_entry_t*
                        {
                            const vmhook::hotspot::vm_struct_entry_t* found_entry{ vmhook::hotspot::iterate_struct_entries("Method", "_from_compiled_code_entry_point") };
                            if (!found_entry)
                            {
                                found_entry = vmhook::hotspot::iterate_struct_entries("Method", "_from_compiled_entry");
                            }

                            return found_entry;
                        }()
                };
                if (!entry)
                {
                    return nullptr;
                }

                return *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::method*>(this)) + entry->offset);
            }

            /*
                @brief Overwrites the from-compiled entry point.
                @details Set to the c2i adapter during deoptimisation so compiled callers
                         transition to the interpreter (and reach our patched i2i stub).
            */
            auto set_from_compiled_entry(void* const entry_point) noexcept
                -> void
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ []() noexcept
                    -> const vmhook::hotspot::vm_struct_entry_t*
                    {
                        const vmhook::hotspot::vm_struct_entry_t* found_entry{ vmhook::hotspot::iterate_struct_entries("Method", "_from_compiled_code_entry_point") };
                        if (!found_entry)
                        {
                            found_entry = vmhook::hotspot::iterate_struct_entries("Method", "_from_compiled_entry");
                        }

                        return found_entry;
                    }()
                };
                if (!entry)
                {
                    return;
                }

                *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(this) + entry->offset) = entry_point;
            }

            /*
                @brief Returns the AdapterHandlerEntry* (_adapter field).
                @details Stores the calling-convention adapters (i2c / c2i) for this method.
                         Used to obtain the c2i entry when deoptimising a compiled method.

                JDK 8 exports Method::_adapter via gHotSpotVMStructs and the
                first branch returns immediately.  JDK 9+ dropped the field
                from VMStructs (SA stopped needing it), so the second
                branch falls back to a heuristic offset detection that
                scans candidate slots in this Method, dereferences each,
                and accepts the first one whose target looks like an
                AdapterHandlerEntry (validated by reading the AHE's
                exported _c2i_entry / _i2c_entry and checking they point
                to executable memory).  The detected offset is cached
                process-wide on first success.
            */
            auto get_adapter() const noexcept
                -> void*
            {
                // Fast path: VMStructs export (JDK 8).
                static const vmhook::hotspot::vm_struct_entry_t* const exported_entry{
                    vmhook::hotspot::iterate_struct_entries("Method", "_adapter") };
                if (exported_entry)
                {
                    return *reinterpret_cast<void**>(
                        reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::method*>(this))
                        + exported_entry->offset);
                }

                // Slow path: JDK 9+ — heuristic detection, cached on success
                // only.  Used to also cache the failure case as SIZE_MAX,
                // but that was disastrous on Forge 1.8.9 / JDK 17: the
                // first JIT-compiled method we probe (runTick) sometimes
                // has its _adapter slot in an in-flight state where
                // validation fails, which locked the cache to "failed"
                // forever and made every later c2i lookup return nullptr.
                // Downstream that bumped every JIT-compiled hook
                // (orientCamera, rayTraceBlocks, ...) into the
                // forced-deopt fallback - compiled callers kept bypassing
                // the hook and e.g. camera_no_clip never fired.
                //
                // Retrying on every call until one Method validates lets a
                // later, well-initialised Method (rayTraceBlocks, etc.)
                // discover the offset, after which the cache is set and
                // every subsequent call is a fast read.  get_adapter() is
                // only invoked from hook install / shutdown, never from
                // the hot detour path, so the per-call probe is cheap.
                static std::atomic<std::size_t> cached_offset{ 0 };
                std::size_t offset{ cached_offset.load(std::memory_order_acquire) };
                if (offset == 0)
                {
                    offset = vmhook::hotspot::detect_adapter_offset_from_method(
                        const_cast<vmhook::hotspot::method*>(this));
                    if (offset != 0)
                    {
                        cached_offset.store(offset, std::memory_order_release);
                    }
                }
                if (offset == 0)
                {
                    return nullptr;
                }
                return *reinterpret_cast<void**>(
                    reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::method*>(this))
                    + offset);
            }
        };

        /*
            @brief Describes a Java field discovered from InstanceKlass._fields.
            @details
            - offset     Byte offset of the field's value within the object (instance fields)
                         or within the java.lang.Class mirror object (static fields).
            - is_static  true when JVM_ACC_STATIC (0x0008) is set on the field.
            - signature  JVM type descriptor, e.g. "I", "J", "Z", "Ljava/lang/String;"
        */
        struct field_entry_t
        {
            std::uint32_t offset;
            bool          is_static;
            std::string   signature;

            // The klass that actually DECLARES this field, set by
            // vmhook::find_field() while it walks the super chain.  For an
            // instance field the offset is object-absolute (laid out cumulatively
            // from the super down), so the declaring klass is informational only.
            // For a STATIC field the offset is relative to the declaring class's
            // own java.lang.Class mirror, so callers MUST add `offset` to
            // `declaring_klass->get_java_mirror()` — NOT to the mirror of the
            // (sub)class the lookup started from, whose mirror is a different,
            // differently-sized oop.  May be nullptr for entries built by the
            // per-klass klass::find_field() helper directly (the field is then
            // known to be declared on that very klass).
            vmhook::hotspot::klass* declaring_klass{ nullptr };
        };

        /*
            @brief Represents a HotSpot internal Klass object.
            @details
            Klass is the JVM's internal representation of a Java class or interface.
            The layout is resolved at runtime via gHotSpotVMStructs.
        */
        struct klass
        {
            /*
                @brief Returns the symbol representing the internal name of this class.
                @return Pointer to the symbol containing the class name using '/' separators,
                        or nullptr on failure.
            */
            auto get_name() const
                -> vmhook::hotspot::symbol*
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("Klass", "_name") };

                try
                {
                    if (!entry)
                    {
                        throw vmhook::exception{ "Failed to find Klass._name entry." };
                    }

                    if (!vmhook::hotspot::is_valid_pointer(this))
                    {
                        return nullptr;
                    }

                    const void* const raw{ vmhook::hotspot::safe_read_pointer(reinterpret_cast<const std::uint8_t*>(this) + entry->offset) };
                    vmhook::hotspot::symbol* const symbol_pointer{ reinterpret_cast<vmhook::hotspot::symbol*>(const_cast<void*>(vmhook::hotspot::untag_pointer(raw))) };

                    return vmhook::hotspot::is_valid_pointer(symbol_pointer) ? symbol_pointer : nullptr;
                }
                catch (const std::exception& exception)
                {
                    VMHOOK_LOG("{} klass.get_name() {}", vmhook::error_tag, exception.what());
                    return nullptr;
                }
            }

            /*
                @brief Returns the next sibling klass in the ClassLoaderData klass linked list.
            */
            auto get_next_link() const
                -> vmhook::hotspot::klass*
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("Klass", "_next_link") };

                if (!entry)
                {
                    throw vmhook::exception{ "Failed to find Klass._next_link entry." };
                }

                if (!vmhook::hotspot::is_valid_pointer(this))
                {
                    return nullptr;
                }

                vmhook::hotspot::klass* const next{ *reinterpret_cast<vmhook::hotspot::klass**>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::klass*>(this)) + entry->offset) };

                return vmhook::hotspot::is_valid_pointer(next) ? next : nullptr;
            }

            /*
                @brief Returns the number of methods declared by this InstanceKlass.
                @details
                Reads InstanceKlass._methods via its VMStruct offset, then reads
                the Array<Method*>::_length field at offset 0 of the array.
                @note This klass* must be an InstanceKlass* (i.e. a regular Java class).
            */
            auto get_methods_count() const noexcept
                -> std::int32_t
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("InstanceKlass", "_methods") };

                if (!entry || !vmhook::hotspot::is_valid_pointer(this))
                {
                    return 0;
                }

                void* const array{ *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(const_cast<klass*>(this)) + entry->offset) };

                if (!vmhook::hotspot::is_valid_pointer(array))
                {
                    return 0;
                }

                return *reinterpret_cast<std::int32_t*>(array);
            }

            /*
                @brief Returns a pointer to the first Method* in this InstanceKlass's methods array.
                @details
                Reads InstanceKlass._methods via its VMStruct offset. The Array<Method*> layout
                in HotSpot is: [int _length (4 bytes)] [4 bytes padding] [Method* _data[...]].
                Data starts at offset 8 from the array base pointer.
                @note This klass* must be an InstanceKlass* (i.e. a regular Java class).
            */
            auto get_methods_ptr() const noexcept
                -> vmhook::hotspot::method**
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("InstanceKlass", "_methods") };

                if (!entry || !vmhook::hotspot::is_valid_pointer(this))
                {
                    return nullptr;
                }

                void* const array{ *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::klass*>(this)) + entry->offset) };

                if (!vmhook::hotspot::is_valid_pointer(array))
                {
                    return nullptr;
                }

                // HotSpot Array<T> layout on x64:
                //   +0  int32_t _length   (4 bytes)
                //   +4  int32_t _padding  (4 bytes alignment padding)
                //   +8  T       _data[0]   first element starts here
                return reinterpret_cast<vmhook::hotspot::method**>(reinterpret_cast<std::uint8_t*>(array) + 8);
            }

            /*
                @brief Returns the java.lang.Class mirror object associated with this klass.
                @details
                Reads Klass::_java_mirror, which is an OopHandle (introduced as such in JDK 17).
                An OopHandle wraps an oop* that points into an OopStorage slot.
                Dereferencing that slot yields the full 64-bit address of the
                java.lang.Class instance. Static field values are stored inside this mirror.
                @return Pointer to the java.lang.Class object, or nullptr on failure.
            */
            auto get_java_mirror() const noexcept
                -> void*
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("Klass", "_java_mirror") };

                if (!entry || !vmhook::hotspot::is_valid_pointer(this))
                {
                    return nullptr;
                }

                /*
                    The type and indirection level of _java_mirror changed across JDK versions.

                    JDK 8 through JDK 16:
                      _java_mirror is a plain `oop` (a direct 64-bit pointer to the
                      java.lang.Class object). One read is enough.
                      VMStructs type_string: "oop"

                    JDK 17+:
                      _java_mirror was changed to an OopHandle:
                        struct OopHandle { oop* _obj; };
                      The value at klass+offset is an oop* pointing into an OopStorage
                      arena. Dereferencing that slot yields the actual java.lang.Class oop.
                      VMStructs type_string: "OopHandle"

                    Detection: inspect entry->type_string at runtime so we stay
                    version-agnostic without any hardcoded JDK version numbers.
                */
                static const bool is_oop_handle{ entry->type_string && std::strcmp(entry->type_string, "OopHandle") == 0 };

                const void* const field_addr{ reinterpret_cast<const std::uint8_t*>(this) + entry->offset };

                if (is_oop_handle)
                {
                    // JDK 17+: two-level indirection through OopStorage.
                    //   klass + offset -> OopHandle._obj  (oop* into OopStorage)
                    //   *OopHandle._obj -> java.lang.Class oop  (full 64-bit, not compressed)
                    const void* const oop_storage_slot{ vmhook::hotspot::safe_read_pointer(field_addr) };
                    if (!vmhook::hotspot::is_valid_pointer(oop_storage_slot))
                    {
                        return nullptr;
                    }

                    return const_cast<void*>(vmhook::hotspot::safe_read_pointer(oop_storage_slot));
                }
                else
                {
                    // JDK 8 through JDK 16: the field is a direct full-width oop pointer.
                    // Read it as a 64-bit value; no further dereference needed.
                    const void* const mirror{ vmhook::hotspot::safe_read_pointer(field_addr) };
                    return vmhook::hotspot::is_valid_pointer(mirror) ? const_cast<void*>(mirror) : nullptr;
                }
            }

            /*
                @brief Returns the super-klass of this klass, or nullptr for java.lang.Object.
            */
            auto get_super() const noexcept -> klass*
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{
                    vmhook::hotspot::iterate_struct_entries("Klass", "_super") };
                if (!entry || !vmhook::hotspot::is_valid_pointer(this))
                {
                    return nullptr;
                }
                auto* const super{
                    *reinterpret_cast<klass**>(reinterpret_cast<std::uint8_t*>(
                        const_cast<klass*>(this)) + entry->offset) };
                return vmhook::hotspot::is_valid_pointer(super) ? super : nullptr;
            }

            /*
                @brief Returns the size in bytes of one instance of this class.
                @details
                Reads Klass._layout_helper, which encodes instance layout information.
                For normal instance classes the layout helper is a positive integer
                whose value (with the low tag bit masked off) is the instance size in
                bytes.  A value of zero or negative indicates an array klass or an
                abstract / interface class that cannot be instantiated directly.

                Complexity: O(1) after the VMStruct offset is cached on first call.
                Exception safety: noexcept — returns 0 on any failure.

                @return Size in bytes of one heap-allocated instance, or 0 if the klass
                        is not a normal instance class.
            */
            auto get_instance_size() const noexcept
                -> std::size_t
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("Klass", "_layout_helper") };

                if (!entry || !vmhook::hotspot::is_valid_pointer(this))
                {
                    return 0;
                }

                const std::int32_t layout_helper{ *reinterpret_cast<const std::int32_t*>(reinterpret_cast<const std::uint8_t*>(this) + entry->offset) };
                if (layout_helper <= 0)
                {
                    return 0;
                }

                // Bit 0 of _layout_helper is a tag; mask it off to get the raw byte size.
                return static_cast<std::size_t>(layout_helper & ~1);
            }

            /*
                @brief Returns the mark-word prototype stored in this klass.
                @details
                Klass._prototype_header holds the template mark-word that HotSpot stamps
                into every freshly-allocated instance.  It encodes the default biased-locking
                epoch and the klass identity hash pattern before any actual locking occurs.
                Used during object allocation to initialise the mark word without
                a separate heap lookup.

                Complexity: O(1) after the VMStruct offset is cached on first call.
                Exception safety: noexcept — returns 1 (neutral mark word) on any failure.

                @return The prototype mark-word value, or 1 on failure.
            */
            auto get_prototype_header() const noexcept
                -> std::uintptr_t
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("Klass", "_prototype_header") };

                if (!entry || !vmhook::hotspot::is_valid_pointer(this))
                {
                    return 1;
                }

                return *reinterpret_cast<const std::uintptr_t*>(reinterpret_cast<const std::uint8_t*>(this) + entry->offset);
            }

            /*
                @brief Searches the InstanceKlass _fields array for a field by name.
                @param name The exact Java field name (e.g. "health", "x", "INSTANCE").
                @return A field_entry_t with offset, static flag, and type descriptor,
                        or std::nullopt when the field is not declared directly by this class.
                @details
                Parses InstanceKlass._fields, which is an Array<u2>.  Each field occupies
                exactly 6 consecutive u2 slots (FieldInfo::field_slots):
                  [0] access_flags  [1] name_index  [2] signature_index
                  [3] initval_index [4] low_packed   [5] high_packed
                The byte offset is recovered as: ((high_packed << 16) | low_packed) >> 2
                (FIELDINFO_TAG_SIZE = 2).
                Name and signature are resolved from the class constant pool.
                @note Searches only this class, not superclasses.  Walk the superclass
                      chain manually to locate inherited fields.
                @note Covers JDK 8 through JDK 21. JDK 22+ uses a different FieldInfoStream encoding.
            */
            /*
                @brief Decodes one UNSIGNED5 value from a byte stream (JDK 21+ FieldInfoStream).
                @details
                Algorithm (from src/hotspot/share/utilities/unsigned5.hpp):
                  sum = sum of (b_i - 1) * 64^i   for i = 0, 1, 2, ...
                  Stop after the first byte b_i where b_i < 192 (a "low byte").
                  Byte 0 is never emitted; it marks the stream End and returns ~0u.
            */
            inline static auto decode_u5(const std::uint8_t* data, int& stream_pos) noexcept
                -> std::uint32_t
            {
                std::uint32_t sum{ 0 };
                for (int byte_position{ 0 }; byte_position < 5; ++byte_position)
                {
                    const std::uint8_t current_byte{ data[stream_pos++] };
                    if (current_byte == 0)
                    {
                        --stream_pos;
                        return ~0u;  // End marker - never a valid value
                    }
                    sum += static_cast<std::uint32_t>(current_byte - 1) << (6 * byte_position);
                    if (current_byte < 192)
                    {
                        return sum;  // Low byte - sequence complete
                    }
                }
                return sum;
            }

            /*
                @brief Looks up a field by name from an InstanceKlass._fieldinfo_stream
                       (JDK 21+ FieldInfoStream format, Array<u1>).
                @details
                Stream grammar (from fieldInfo.hpp):
                  FieldInfoStream := j(num_java) k(num_injected) Field[j+k] End(0)
                  Field := name_idx sig_idx offset access_flags field_flags
                           [initval_idx  if field_flags & 0x01]
                           [gsig_idx     if field_flags & 0x04]
                           [group        if field_flags & 0x10]
                All integers encoded with UNSIGNED5 (see decode_u5).
            */
            auto find_field_in_stream(const std::string_view name, void** constant_pool_base) const noexcept
                -> std::optional<vmhook::hotspot::field_entry_t>
            {
                static const vmhook::hotspot::vm_struct_entry_t* const fis_entry{ vmhook::hotspot::iterate_struct_entries("InstanceKlass", "_fieldinfo_stream") };

                if (!fis_entry || !vmhook::hotspot::is_valid_pointer(this) || !vmhook::hotspot::is_valid_pointer(constant_pool_base))
                {
                    return std::nullopt;
                }

                // Read Array<u1>* pointer from InstanceKlass
                const void* const arr_ptr{ *reinterpret_cast<void* const*>(reinterpret_cast<const std::uint8_t*>(this) + fis_entry->offset) };

                if (!vmhook::hotspot::is_valid_pointer(arr_ptr))
                {
                    return std::nullopt;
                }

                // Array<u1> layout on x64 HotSpot:
                //   +0  int32_t _length   (4 bytes)
                //   +4  u1      _data[0]   data starts here (no padding: u1 has alignment 1)
                // Note: Array<Method*> uses +8 because 8-byte pointers need 8-byte alignment,
                // requiring 4 bytes of padding after _length.  u1 and u2 arrays do NOT need this.
                const std::int32_t length{ *reinterpret_cast<const std::int32_t*>(arr_ptr) };
                if (length <= 0 || length > 0x4000)
                {
                    return std::nullopt;
                }

                const std::uint8_t* const data{ reinterpret_cast<const std::uint8_t*>(arr_ptr) + 4 };

                std::int32_t stream_pos{ 0 };

                // Header: number of Java-declared fields, then number of VM-injected fields.
                const std::uint32_t num_java_fields{ decode_u5(data, stream_pos) };
                const std::uint32_t num_injected_fields{ decode_u5(data, stream_pos) };
                if (num_java_fields == ~0u || num_injected_fields == ~0u || num_java_fields + num_injected_fields > 4096u)
                {
                    return std::nullopt;
                }

                for (std::uint32_t field_index{ 0 }; field_index < num_java_fields + num_injected_fields && stream_pos < length; ++field_index)
                {
                    const std::uint32_t name_index{ decode_u5(data, stream_pos) };
                    if (name_index == ~0u)
                    {
                        break;
                    }

                    const std::uint32_t sig_index{ decode_u5(data, stream_pos) };
                    const std::uint32_t field_offset{ decode_u5(data, stream_pos) };
                    const std::uint32_t access_flags{ decode_u5(data, stream_pos) };
                    const std::uint32_t field_flags{ decode_u5(data, stream_pos) };

                    // Consume optional trailing entries whose presence is signalled by field_flags bits:
                    //   bit 0 (0x01): initval_index  - compile-time constant initialiser value
                    //   bit 2 (0x04): generic_sig_index - generic type signature (e.g. List<T>)
                    //   bit 4 (0x10): contended_group - @Contended padding group id
                    if (field_flags & 0x01u)
                    {
                        vmhook::hotspot::klass::decode_u5(data, stream_pos);
                    }
                    if (field_flags & 0x04u)
                    {
                        vmhook::hotspot::klass::decode_u5(data, stream_pos);
                    }
                    if (field_flags & 0x10u)
                    {
                        vmhook::hotspot::klass::decode_u5(data, stream_pos);
                    }

                    // Resolve the field name from the constant pool and compare.
                    if (name_index && vmhook::hotspot::is_valid_pointer(constant_pool_base[name_index]))
                    {
                        const vmhook::hotspot::symbol* const name_symbol{ reinterpret_cast<const vmhook::hotspot::symbol*>(constant_pool_base[name_index]) };
                        if (vmhook::hotspot::is_valid_pointer(name_symbol) && name_symbol->to_string() == name)
                        {
                            const bool is_static{ (access_flags & 0x0008u) != 0u };
                            std::string signature;
                            if (sig_index && vmhook::hotspot::is_valid_pointer(constant_pool_base[sig_index]))
                            {
                                const vmhook::hotspot::symbol* const signature_symbol{ reinterpret_cast<const vmhook::hotspot::symbol*>(constant_pool_base[sig_index]) };
                                if (vmhook::hotspot::is_valid_pointer(signature_symbol))
                                {
                                    signature = signature_symbol->to_string();
                                }
                            }
                            return vmhook::hotspot::field_entry_t{ field_offset, is_static, signature };
                        }
                    }
                }
                return std::nullopt;
            }

            /*
                @brief Searches the InstanceKlass field metadata for a field by name.
                @details
                Automatically selects the correct field storage format based on what
                is exported in gHotSpotVMStructs for this JDK build:

                JDK 8 through early JDK 21:
                  InstanceKlass._fields -> Array<u2>, 6 slots per field
                  Byte offset = ((high_packed << 16) | low_packed) >> FIELDINFO_TAG_SIZE(2)

                JDK 21.0.x+ and JDK 22+:
                  InstanceKlass._fieldinfo_stream -> Array<u1>, UNSIGNED5 compressed
                  Stream grammar: j(num_java) k(num_injected) Field[j+k] End(0)
                  Per field: name_idx sig_idx offset access_flags field_flags [optionals]

                @note Searches only fields declared directly on this class.
                      Walk the superclass chain to find inherited fields.
            */
            auto find_field(const std::string_view name) const noexcept
                -> std::optional<vmhook::hotspot::field_entry_t>
            {
                static const vmhook::hotspot::vm_struct_entry_t* const fields_entry{ vmhook::hotspot::iterate_struct_entries("InstanceKlass", "_fields") };
                static const vmhook::hotspot::vm_struct_entry_t* const fis_entry{ vmhook::hotspot::iterate_struct_entries("InstanceKlass", "_fieldinfo_stream") };
                static const vmhook::hotspot::vm_struct_entry_t* const constants_entry{ vmhook::hotspot::iterate_struct_entries("InstanceKlass", "_constants") };

                if (!vmhook::hotspot::is_valid_pointer(this) || !constants_entry)
                {
                    return std::nullopt;
                }

                // Resolve constant pool (needed by both paths)
                vmhook::hotspot::constant_pool* const constant_pool_ptr{ *reinterpret_cast<vmhook::hotspot::constant_pool**>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::klass*>(this)) + constants_entry->offset) };

                if (!vmhook::hotspot::is_valid_pointer(constant_pool_ptr))
                {
                    return std::nullopt;
                }

                void** const constant_pool_base{ constant_pool_ptr->get_base() };
                if (!vmhook::hotspot::is_valid_pointer(constant_pool_base))
                {
                    return std::nullopt;
                }

                // -- JDK 21+ path: FieldInfoStream ----------------------------
                if (fis_entry)
                {
                    return vmhook::hotspot::klass::find_field_in_stream(name, constant_pool_base);
                }

                // -- JDK 8 through JDK 17 path: Array<u2> with 6-slot FieldInfo records --
                if (!fields_entry)
                {
                    return std::nullopt;
                }

                void* const fields_array{ *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::klass*>(this)) + fields_entry->offset) };

                if (!vmhook::hotspot::is_valid_pointer(fields_array))
                {
                    return std::nullopt;
                }

                // Array<u2>: int32_t _length at +0, data at +8
                const std::int32_t array_length{ *reinterpret_cast<const std::int32_t*>(fields_array) };

                static const std::int32_t field_slots{ 6 };

                // In JDK 8, the _fields Array<u2> may include a trailing u2
                // storing _java_fields_count after all the 6-slot field records.
                // Use integer division to safely ignore any trailing partial slot.
                if (array_length <= 0 || array_length < field_slots)
                {
                    return std::nullopt;
                }

                // Array<u2> layout on x64 HotSpot:
                //   +0  int32_t _length   (4 bytes)
                //   +4  u2      _data[0]   data starts here (u2 needs 2-byte alignment;
                //                           offset 4 is already 2-byte aligned, no padding needed)
                const std::uint16_t* const data{ reinterpret_cast<const std::uint16_t*>(reinterpret_cast<const std::uint8_t*>(fields_array) + 4) };

                // FieldInfo::field_slots layout for each field record (JDK 8 through JDK 20):
                //   slot 0  u16  access_flags   - JVM_ACC_* flags (static flag = bit 3 / 0x0008)
                //   slot 1  u16  name_index     - constant-pool index of the field's name Symbol
                //   slot 2  u16  signature_index- constant-pool index of the type descriptor Symbol
                //   slot 3  u16  initval_index  - cp index of compile-time constant value (or 0)
                //   slot 4  u16  low_packed     - bits [1:0] FIELDINFO_TAG, bits [15:2] offset_low
                //   slot 5  u16  high_packed    - offset_high (upper 16 bits of the packed offset)
                for (std::int32_t field_slot_index{ 0 }; field_slot_index < array_length / field_slots; ++field_slot_index)
                {
                    const std::uint16_t name_index{ data[field_slot_index * field_slots + 1] };
                    if (!name_index)
                    {
                        continue;  // slot 1: name_index == 0 means VM-injected field, skip
                    }

                    const vmhook::hotspot::symbol* const name_symbol{ reinterpret_cast<const vmhook::hotspot::symbol*>(constant_pool_base[name_index]) };
                    if (!vmhook::hotspot::is_valid_pointer(name_symbol) || name_symbol->to_string() != name)
                    {
                        continue;
                    }

                    const std::uint16_t access_flags{ data[field_slot_index * field_slots + 0] };  // slot 0
                    const std::uint16_t sig_index{ data[field_slot_index * field_slots + 2] };  // slot 2
                    const std::uint16_t low_packed{ data[field_slot_index * field_slots + 4] };  // slot 4
                    const std::uint16_t high_packed{ data[field_slot_index * field_slots + 5] };  // slot 5

                    // Reconstruct the byte offset from the packed representation:
                    //   packed = (high_packed << 16) | low_packed
                    //   offset = packed >> FIELDINFO_TAG_SIZE   (FIELDINFO_TAG_SIZE = 2)
                    // The 2 lowest bits of packed are the FIELDINFO_TAG and carry no offset data.
                    const std::uint32_t packed{ (static_cast<std::uint32_t>(high_packed) << 16) | low_packed };
                    const std::uint32_t offset{ packed >> 2 };

                    const bool is_static{ (access_flags & 0x0008u) != 0u };

                    const vmhook::hotspot::symbol* const signature_symbol{ reinterpret_cast<const vmhook::hotspot::symbol*>(constant_pool_base[sig_index]) };
                    const std::string signature{ vmhook::hotspot::is_valid_pointer(signature_symbol) ? signature_symbol->to_string() : std::string{} };

                    return vmhook::hotspot::field_entry_t{ offset, is_static, signature };
                }

                return std::nullopt;
            }
        };

        /*
            @brief Represents a HotSpot ClassLoaderData node.
            @details
            Each ClassLoader in the JVM has a corresponding ClassLoaderData that
            tracks every Klass it has loaded. The ClassLoaderData nodes are chained
            together via _next into a global linked list whose head is held by
            ClassLoaderDataGraph::_head.
        */
        struct class_loader_data
        {
            /*
                @brief Returns the head of the klass linked list for this classloader.
                @details
                Reads ClassLoaderData::_klasses using its offset from gHotSpotVMStructs.
            */
            auto get_klasses() const
                -> vmhook::hotspot::klass*
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("ClassLoaderData", "_klasses") };

                try
                {
                    if (!entry)
                    {
                        throw vmhook::exception{ "Failed to find ClassLoaderData._klasses entry." };
                    }

                    if (!vmhook::hotspot::is_valid_pointer(this))
                    {
                        return nullptr;
                    }

                    // _klasses is a Klass* (full 8-byte native pointer), not a compressed OOP.
                    vmhook::hotspot::klass* const result{ *reinterpret_cast<vmhook::hotspot::klass**>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::class_loader_data*>(this)) + entry->offset) };

                    return vmhook::hotspot::is_valid_pointer(result) ? result : nullptr;
                }
                catch (const std::exception& exception)
                {
                    VMHOOK_LOG("{} class_loader_data.get_klasses() {}", vmhook::error_tag, exception.what());
                    return nullptr;
                }
            }

            /*
                @brief Returns the next ClassLoaderData node in the global linked list.
            */
            auto get_next() const
                -> vmhook::hotspot::class_loader_data*
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ iterate_struct_entries("ClassLoaderData", "_next") };

                try
                {
                    if (!entry)
                    {
                        throw vmhook::exception{ "Failed to find ClassLoaderData._next entry." };
                    }

                    if (!vmhook::hotspot::is_valid_pointer(this))
                    {
                        return nullptr;
                    }

                    vmhook::hotspot::class_loader_data* const next{ reinterpret_cast<vmhook::hotspot::class_loader_data*>(const_cast<void*>(vmhook::hotspot::safe_read_pointer(reinterpret_cast<const std::uint8_t*>(this) + entry->offset))) };

                    return vmhook::hotspot::is_valid_pointer(next) ? next : nullptr;
                }
                catch (const std::exception& exception)
                {
                    VMHOOK_LOG("{} class_loader_data.get_next() {}", vmhook::error_tag, exception.what());
                    return nullptr;
                }
            }

            /*
                @brief Returns the Dictionary associated with this classloader.
            */
            auto get_dictionary() const
                -> vmhook::hotspot::dictionary*
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("ClassLoaderData", "_dictionary") };

                try
                {
                    if (!entry)
                    {
                        return nullptr;
                    }

                    if (!vmhook::hotspot::is_valid_pointer(this))
                    {
                        return nullptr;
                    }

                    vmhook::hotspot::dictionary* const dict{ reinterpret_cast<vmhook::hotspot::dictionary*>(const_cast<void*>(vmhook::hotspot::safe_read_pointer(reinterpret_cast<const std::uint8_t*>(this) + entry->offset))) };

                    return vmhook::hotspot::is_valid_pointer(dict) ? dict : nullptr;
                }
                catch (const std::exception& exception)
                {
                    VMHOOK_LOG("{} class_loader_data.get_dictionary() {}", vmhook::error_tag, exception.what());
                    return nullptr;
                }
            }

            /*
                @brief Returns the java.lang.ClassLoader instance OOP for this CLD.
                @details
                Reads ClassLoaderData::_class_loader.  Layout depends on JDK:

                  JDK 8-9:   oop _class_loader              (direct heap pointer)
                  JDK 10+:   OopHandle _class_loader        ({ oop* _obj })

                Detected from gHotSpotVMStructs type_string at runtime so the
                code stays version-agnostic.

                The bootstrap loader is represented by a null pointer here,
                so a null return is a valid result for bootstrap classes
                (java.lang, jdk.internal, etc.).  For everything else this is
                the heap ClassLoader oop suitable for use as a fake JNI handle
                target.

                Complexity: O(1) after VMStructs offset is cached on first call.
                Exception safety: noexcept — returns nullptr on any failure.

                @return  Raw heap pointer to the java.lang.ClassLoader instance,
                         or nullptr for the bootstrap loader / on failure.
            */
            auto get_class_loader_oop() const noexcept
                -> void*
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{
                    vmhook::hotspot::iterate_struct_entries("ClassLoaderData", "_class_loader") };

                if (!entry || !vmhook::hotspot::is_valid_pointer(this))
                {
                    return nullptr;
                }

                static const bool is_oop_handle{
                    entry->type_string && std::strcmp(entry->type_string, "OopHandle") == 0 };

                const void* const field_addr{
                    reinterpret_cast<const std::uint8_t*>(this) + entry->offset };

                if (is_oop_handle)
                {
                    const void* const oop_storage_slot{ vmhook::hotspot::safe_read_pointer(field_addr) };
                    if (!vmhook::hotspot::is_valid_pointer(oop_storage_slot))
                    {
                        return nullptr;
                    }
                    void* const loader_oop{ const_cast<void*>(vmhook::hotspot::safe_read_pointer(oop_storage_slot)) };
                    return vmhook::hotspot::is_valid_pointer(loader_oop) ? loader_oop : nullptr;
                }

                const void* const loader_oop{ vmhook::hotspot::safe_read_pointer(field_addr) };
                return vmhook::hotspot::is_valid_pointer(loader_oop) ? const_cast<void*>(loader_oop) : nullptr;
            }
        };

        /*
            @brief Represents a HotSpot Dictionary - the per-classloader class registry.
            @details
            Each ClassLoaderData owns a Dictionary which is a hashtable mapping class
            names to their corresponding Klass* objects. It inherits from
            BasicHashtable<mtInternal> whose layout is:
            - offset 0: _table_size (int32) - number of buckets
            - offset 8: _buckets (HashtableBucket*) - pointer to the bucket array
            Each bucket slot holds a pointer to the head of a linked list of
            DictionaryEntry nodes. Each DictionaryEntry has:
            - offset 0:  _next (DictionaryEntry*) - next entry in the chain
            - offset 8:  _hash (uint32)
            - offset 16: _literal (Klass*) - the actual class
        */
        struct dictionary
        {
            /*
                @brief Returns the number of hash buckets in this dictionary.
                @details
                BasicHashtable<mtInternal> layout (x64):
                  +0  int32_t _table_size  - number of buckets at this offset
                  +4  (4 bytes alignment padding)
                  +8  HashtableBucket* _buckets
            */
            inline auto get_table_size() const noexcept
                -> std::int32_t
            {
                // _table_size is the first field of BasicHashtable at offset 0.
                return *reinterpret_cast<const std::int32_t*>(this);
            }

            /*
                @brief Returns a pointer to the bucket array of this dictionary.
                @details
                _buckets is the second field of BasicHashtable at offset +8 (after
                _table_size int32 + 4 bytes padding on x64).
                Each element is a pointer to the head of a singly-linked DictionaryEntry chain.
            */
            inline auto get_buckets() const noexcept
                -> const std::uint8_t*
            {
                // +8 bytes = sizeof(int32_t _table_size) + 4 bytes alignment padding.
                return *reinterpret_cast<const std::uint8_t* const*>(reinterpret_cast<const std::uint8_t*>(this) + 8);
            }

            /*
                @brief Searches this dictionary for a klass by its internal name.
            */
            auto find_klass(const std::string_view class_name) const
                -> vmhook::hotspot::klass*
            {
                const std::int32_t table_size{ this->get_table_size() };
                const std::uint8_t* const buckets{ this->get_buckets() };

                if (!vmhook::hotspot::is_valid_pointer(buckets) || table_size <= 0 || table_size > 0x186A0)
                {
                    return nullptr;
                }

                for (std::int32_t bucket_index{ 0 }; bucket_index < table_size; ++bucket_index)
                {
                    const std::uint8_t* dict_entry{ reinterpret_cast<const std::uint8_t*>(vmhook::hotspot::untag_pointer(vmhook::hotspot::safe_read_pointer(buckets + bucket_index * 8))) };

                    while (vmhook::hotspot::is_valid_pointer(dict_entry))
                    {
                        // DictionaryEntry (extends HashtableEntry<InstanceKlass*, mtClass>) layout:
                        //   +0   void*       _next    (next entry in the chain)
                        //   +8   uint32_t    _hash    (pre-computed hash, 4 bytes + 4 padding)
                        //   +16  Klass*      _literal, the actual class pointer
                        const void* const raw_klass{ vmhook::hotspot::safe_read_pointer(dict_entry + 16) };
                        const vmhook::hotspot::klass* const candidate_klass{ reinterpret_cast<const vmhook::hotspot::klass*>(vmhook::hotspot::untag_pointer(raw_klass)) };

                        if (vmhook::hotspot::is_valid_pointer(candidate_klass))
                        {
                            const vmhook::hotspot::symbol* const sym{ candidate_klass->get_name() };
                            if (vmhook::hotspot::is_valid_pointer(sym) && sym->to_string() == class_name)
                            {
                                return const_cast<vmhook::hotspot::klass*>(candidate_klass);
                            }
                        }

                        dict_entry = reinterpret_cast<const std::uint8_t*>(vmhook::hotspot::untag_pointer(vmhook::hotspot::safe_read_pointer(dict_entry)));
                    }
                }

                return nullptr;
            }

            /*
                @brief Invokes a callback for every klass stored in this
                       dictionary by walking the full hashtable.
                @details
                Same iteration as find_klass, but without the name match
                filter — every entry is visited.  Used by
                class_loader_data_graph::for_each_klass on JDK builds
                where ClassLoaderData::_klasses is not exposed in
                VMStructs (JDK 8-17).
            */
            template<typename callback_type>
            auto for_each_klass(callback_type&& callback) const -> void
            {
                const std::int32_t table_size{ this->get_table_size() };
                const std::uint8_t* const buckets{ this->get_buckets() };

                if (!vmhook::hotspot::is_valid_pointer(buckets) || table_size <= 0 || table_size > 0x186A0)
                {
                    return;
                }

                for (std::int32_t bucket_index{ 0 }; bucket_index < table_size; ++bucket_index)
                {
                    const std::uint8_t* dict_entry{ reinterpret_cast<const std::uint8_t*>(vmhook::hotspot::untag_pointer(vmhook::hotspot::safe_read_pointer(buckets + bucket_index * 8))) };

                    std::int32_t chain_visited{ 0 };
                    while (vmhook::hotspot::is_valid_pointer(dict_entry) && chain_visited < 1048576)
                    {
                        ++chain_visited;
                        const void* const raw_klass{ vmhook::hotspot::safe_read_pointer(dict_entry + 16) };
                        auto* const candidate_klass{ reinterpret_cast<vmhook::hotspot::klass*>(
                            const_cast<void*>(vmhook::hotspot::untag_pointer(raw_klass))) };

                        if (vmhook::hotspot::is_valid_pointer(candidate_klass))
                        {
                            if (const auto* const sym{ candidate_klass->get_name() };
                                vmhook::hotspot::is_valid_pointer(sym))
                            {
                                callback(sym->to_string(), candidate_klass);
                            }
                        }

                        dict_entry = reinterpret_cast<const std::uint8_t*>(vmhook::hotspot::untag_pointer(vmhook::hotspot::safe_read_pointer(dict_entry)));
                    }
                }
            }
        };

        /*
            @brief Represents the HotSpot ClassLoaderDataGraph - the global registry of all classloaders.
            @details
            ClassLoaderDataGraph::_head is a static field holding the head of a global
            linked list of ClassLoaderData nodes, one per classloader registered in the JVM.
        */
        struct class_loader_data_graph
        {
            /*
                @brief Returns the head of the global ClassLoaderData linked list.
            */
            auto get_head() const
                -> vmhook::hotspot::class_loader_data*
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("ClassLoaderDataGraph", "_head") };

                try
                {
                    if (!entry)
                    {
                        throw vmhook::exception{ "Failed to find ClassLoaderDataGraph._head entry." };
                    }

                    vmhook::hotspot::class_loader_data* const head{ *reinterpret_cast<vmhook::hotspot::class_loader_data* const*>(entry->address) };

                    return vmhook::hotspot::is_valid_pointer(head) ? head : nullptr;
                }
                catch (const std::exception& exception)
                {
                    VMHOOK_LOG("{} class_loader_data_graph.get_head() {}", vmhook::error_tag, exception.what());
                    return nullptr;
                }
            }

            /*
                @brief Searches all loaded classloaders for a klass by its internal name.
                @param class_name The internal JVM class name using '/' separators
                                  (e.g. "java/lang/String", "net/minecraft/client/Minecraft").
                @return Pointer to the matching klass if found, nullptr otherwise.
                @details
                Walks the full ClassLoaderDataGraph using only HotSpot internal structures
                resolved via gHotSpotVMStructs, without any JNI or JVMTI calls.
            */
            auto find_klass(const std::string_view class_name) const
                -> vmhook::hotspot::klass*
            {
                // Adaptive strategy - detected once at startup via VMStructs presence:
                //   JDK 21+  exports _klasses  but not _dictionary
                //   JDK 8-17 exports _dictionary but not _klasses
                static const bool use_klasses{ vmhook::hotspot::iterate_struct_entries("ClassLoaderData", "_klasses") != nullptr };

                vmhook::hotspot::class_loader_data* class_loader_data{ this->get_head() };

                while (vmhook::hotspot::is_valid_pointer(class_loader_data) && class_loader_data)
                {
                    if (use_klasses)
                    {
                        // JDK 21+: walk the _klasses linked list
                        vmhook::hotspot::klass* current_klass{ class_loader_data->get_klasses() };
                        while (current_klass && vmhook::hotspot::is_valid_pointer(current_klass))
                        {
                            const vmhook::hotspot::symbol* const sym{ current_klass->get_name() };
                            if (vmhook::hotspot::is_valid_pointer(sym) && sym->to_string() == class_name)
                            {
                                return current_klass;
                            }

                            current_klass = current_klass->get_next_link();
                        }
                    }
                    else
                    {
                        // JDK 8-17: search via per-CLD Dictionary hashtable
                        // (may return null if _dictionary not in VMStructs for this JDK build)
                        vmhook::hotspot::dictionary* const dict{ class_loader_data->get_dictionary() };
                        if (vmhook::hotspot::is_valid_pointer(dict))
                        {
                            vmhook::hotspot::klass* const found_klass{ dict->find_klass(class_name) };
                            if (found_klass)
                            {
                                return found_klass;
                            }
                        }
                    }

                    vmhook::hotspot::class_loader_data* const next{ class_loader_data->get_next() };
                    class_loader_data = vmhook::hotspot::is_valid_pointer(next) ? next : nullptr;
                }

                // JDK 8 fallback: ClassLoaderData._dictionary not in VMStructs,
                // but SystemDictionary._dictionary (bootstrap CL) and
                // SystemDictionary._shared_dictionary (CDS) ARE exported as statics.
                // These cover all bootstrap-loaded classes (java.*, javax.*, sun.*, etc.)
                // and most Minecraft client classes (loaded by the same classloader chain).
                static const vmhook::hotspot::vm_struct_entry_t* const sd_main{ vmhook::hotspot::iterate_struct_entries("SystemDictionary", "_dictionary") };
                static const vmhook::hotspot::vm_struct_entry_t* const sd_shared{ vmhook::hotspot::iterate_struct_entries("SystemDictionary", "_shared_dictionary") };

                for (const vmhook::hotspot::vm_struct_entry_t* system_dictionary_entry : { sd_main, sd_shared })
                {
                    if (!system_dictionary_entry || !system_dictionary_entry->address)
                    {
                        continue;
                    }
                    vmhook::hotspot::dictionary* const dictionary_pointer{ *reinterpret_cast<vmhook::hotspot::dictionary**>(system_dictionary_entry->address) };
                    if (!vmhook::hotspot::is_valid_pointer(dictionary_pointer))
                    {
                        continue;
                    }
                    klass* const found_klass{ dictionary_pointer->find_klass(class_name) };
                    if (found_klass)
                    {
                        return found_klass;
                    }
                }

                return nullptr;
            }

            /*
                @brief Invokes a callback for every klass currently reachable
                       through the ClassLoaderData graph.
                @details
                Used by the class-load watcher to snapshot loaded classes.
                The callback receives the klass's internal name (with '/'
                separators) and the raw klass pointer.  Iteration is best-
                effort: unreadable nodes are skipped silently.

                Complexity: O(N) where N = total loaded classes.
                Exception safety: noexcept boundary — callback exceptions
                    are propagated as-is; iteration stops at the throw.
            */
            template<typename callback_type>
            auto for_each_klass(callback_type&& callback) const -> void
            {
                static const bool use_klasses{
                    vmhook::hotspot::iterate_struct_entries("ClassLoaderData", "_klasses") != nullptr };
                static const bool use_cld_dictionary{
                    vmhook::hotspot::iterate_struct_entries("ClassLoaderData", "_dictionary") != nullptr };

                auto* class_loader_data{ this->get_head() };
                std::int32_t visited{ 0 };
                while (vmhook::hotspot::is_valid_pointer(class_loader_data)
                    && class_loader_data
                    && visited < 65536)
                {
                    ++visited;

                    if (use_klasses)
                    {
                        // JDK 21+: walk the _klasses linked list.
                        auto* current_klass{ class_loader_data->get_klasses() };
                        std::int32_t kl_visited{ 0 };
                        while (current_klass
                            && vmhook::hotspot::is_valid_pointer(current_klass)
                            && kl_visited < 1048576)
                        {
                            ++kl_visited;
                            const auto* const sym{ current_klass->get_name() };
                            if (vmhook::hotspot::is_valid_pointer(sym))
                            {
                                callback(sym->to_string(), current_klass);
                            }
                            current_klass = current_klass->get_next_link();
                        }
                    }
                    else if (use_cld_dictionary)
                    {
                        // JDK 8-17: walk the per-CLD Dictionary hashtable.
                        auto* const dict{ class_loader_data->get_dictionary() };
                        if (vmhook::hotspot::is_valid_pointer(dict))
                        {
                            dict->for_each_klass(callback);
                        }
                    }

                    auto* const next{ class_loader_data->get_next() };
                    class_loader_data = vmhook::hotspot::is_valid_pointer(next) ? next : nullptr;
                }

                // JDK 8 also requires walking SystemDictionary directly
                // because ClassLoaderData::_dictionary may not be in
                // VMStructs.  SystemDictionary._dictionary covers
                // bootstrap classes (java.*, javax.*, sun.*) and
                // _shared_dictionary covers CDS-archived classes.
                if (!use_klasses)
                {
                    static const vmhook::hotspot::vm_struct_entry_t* const sd_main{
                        vmhook::hotspot::iterate_struct_entries("SystemDictionary", "_dictionary") };
                    static const vmhook::hotspot::vm_struct_entry_t* const sd_shared{
                        vmhook::hotspot::iterate_struct_entries("SystemDictionary", "_shared_dictionary") };

                    for (const auto* const sd_entry : { sd_main, sd_shared })
                    {
                        if (!sd_entry || !sd_entry->address) { continue; }
                        auto* const dict{ *reinterpret_cast<vmhook::hotspot::dictionary**>(sd_entry->address) };
                        if (!vmhook::hotspot::is_valid_pointer(dict)) { continue; }
                        dict->for_each_klass(callback);
                    }
                }
            }
        };

        /*
            @brief Represents the possible execution states of a HotSpot JavaThread.
        */
        enum class java_thread_state : std::int8_t
        {
            _thread_uninitialized = 0,
            _thread_new = 2,
            _thread_new_trans = 3,
            _thread_in_native = 4,
            _thread_in_native_trans = 5,
            _thread_in_vm = 6,
            _thread_in_vm_trans = 7,
            /*
                @brief The thread is currently executing Java bytecode in the interpreter.
                @note This is the only state in which method hooks are safely intercepted.
            */
            _thread_in_Java = 8,
            _thread_in_Java_trans = 9,
            _thread_blocked = 10,
            _thread_blocked_trans = 11,
            _thread_max_state = 12
        };

        /*
            @brief Represents a HotSpot internal JavaThread object.
            @details
            JavaThread is the JVM's internal representation of a Java thread.
            On x64 HotSpot the current JavaThread pointer is always held in register r15,
            making it directly accessible from low-level hook stubs.
            The layout is resolved at runtime via gHotSpotVMStructs.
        */
        struct java_thread
        {
            /*
                @brief Returns the current execution state of this thread.
            */
            auto get_thread_state() const
                -> vmhook::hotspot::java_thread_state
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("JavaThread", "_thread_state") };

                try
                {
                    if (!entry)
                    {
                        throw vmhook::exception{ "Failed to find JavaThread._thread_state entry." };
                    }

                    return *reinterpret_cast<java_thread_state*>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::java_thread*>(this)) + entry->offset);
                }
                catch (const std::exception& exception)
                {
                    VMHOOK_LOG("{} java_thread.get_thread_state() {}", vmhook::error_tag, exception.what());
                    return vmhook::hotspot::java_thread_state::_thread_uninitialized;
                }
            }

            /*
                @brief Sets the execution state of this thread.
                @warning Incorrect use of this function can corrupt the JVM thread state machine.
            */
            auto set_thread_state(const vmhook::hotspot::java_thread_state state) const
                -> void
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("JavaThread", "_thread_state") };

                try
                {
                    if (!entry)
                    {
                        throw vmhook::exception{ "Failed to find JavaThread._thread_state entry." };
                    }

                    *reinterpret_cast<java_thread_state*>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::java_thread*>(this)) + entry->offset) = state;
                }
                catch (const std::exception& exception)
                {
                    VMHOOK_LOG("{} java_thread.set_thread_state() {}", vmhook::error_tag, exception.what());
                }
            }

            /*
                @brief Returns the current suspension flags of this thread.
            */
            auto get_suspend_flags() const
                -> std::uint32_t
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("JavaThread", "_suspend_flags") };

                try
                {
                    if (!entry)
                    {
                        throw vmhook::exception{ "Failed to find JavaThread._suspend_flags entry." };
                    }

                    return *reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::java_thread*>(this)) + entry->offset);
                }
                catch (const std::exception& exception)
                {
                    VMHOOK_LOG("{} java_thread.get_suspend_flags() {}", vmhook::error_tag, exception.what());
                    return 0;
                }
            }

            /*
                @brief Returns the next thread in HotSpot's global thread linked list.
                @details
                HotSpot maintains all JavaThread instances in an intrusive singly-linked list.
                The field was _next on JavaThread in older JDKs and moved to Thread in later
                JDKs.  This function probes both locations and uses whichever is present so
                that the code remains version-agnostic.

                Complexity: O(1) after the VMStruct offset is cached on first call.
                Exception safety: noexcept — returns nullptr on any failure.

                @return Pointer to the next JavaThread, or nullptr if this is the last entry.
            */
            auto get_next() const noexcept
                -> vmhook::hotspot::java_thread*
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ []()
                    -> const vmhook::hotspot::vm_struct_entry_t*
                    {
                        {
                            auto* found_entry{ vmhook::hotspot::iterate_struct_entries("JavaThread", "_next") };
                            if (found_entry)
                            {
                                return found_entry;
                            }
                        }
                        return vmhook::hotspot::iterate_struct_entries("Thread", "_next");
                    }()
                };

                if (!entry || !vmhook::hotspot::is_valid_pointer(this))
                {
                    return nullptr;
                }

                vmhook::hotspot::java_thread* const next_thread{ *reinterpret_cast<vmhook::hotspot::java_thread* const*>(reinterpret_cast<const std::uint8_t*>(this) + entry->offset) };
                return vmhook::hotspot::is_valid_pointer(next_thread) ? next_thread : nullptr;
            }

            /*
                @brief Returns the OS-level thread ID of this JavaThread.
                @details
                Walks JavaThread (or Thread) -> OSThread -> _thread_id.
                _osthread is probed on both JavaThread and Thread for JDK version
                compatibility.  On Windows the value is a Win32 thread ID; on Linux
                it is the kernel TID (same value returned by gettid()).

                Complexity: O(1) after VMStruct offsets are cached on first call.
                Exception safety: noexcept — returns 0 on any failure.

                @return The OS thread ID, or 0 if the field cannot be located.
            */
            auto get_os_thread_id() const noexcept
                -> vmhook::os::thread_id_t
            {
                static const vmhook::hotspot::vm_struct_entry_t* const osthread_entry{ []()
                    -> const vmhook::hotspot::vm_struct_entry_t*
                    {
                        if (auto* const entry{ vmhook::hotspot::iterate_struct_entries("JavaThread", "_osthread") })
                        {
                            return entry;
                        }

                        return vmhook::hotspot::iterate_struct_entries("Thread", "_osthread");
                    }()
                };
                static const vmhook::hotspot::vm_struct_entry_t* const thread_id_entry{ []()
                    -> const vmhook::hotspot::vm_struct_entry_t*
                    {
                        return vmhook::hotspot::iterate_struct_entries("OSThread", "_thread_id");
                    }()
                };

                if (!osthread_entry || !thread_id_entry || !vmhook::hotspot::is_valid_pointer(this))
                {
                    return 0;
                }

                void* const os_thread{ *reinterpret_cast<void* const*>(reinterpret_cast<const std::uint8_t*>(this) + osthread_entry->offset) };
                if (!os_thread || !vmhook::hotspot::is_valid_pointer(os_thread))
                {
                    return 0;
                }

                // HotSpot exports OSThread::_thread_id with a platform-specific underlying
                // type (32-bit DWORD on Windows, pid_t on Linux). Read the raw 32-bit slot
                // and zero-extend so callers always see a uniformly-typed value.
                return static_cast<vmhook::os::thread_id_t>(
                    *reinterpret_cast<const std::uint32_t*>(
                        reinterpret_cast<const std::uint8_t*>(os_thread) + thread_id_entry->offset));
            }

            /*
                @brief Bump-allocates byte_size bytes from this thread's TLAB.
                @details
                A Thread-Local Allocation Buffer (TLAB) is a private region of the Java heap
                assigned to each thread.  Allocating from the TLAB is a simple pointer bump:
                advance _top by byte_size and return the old _top.  No locking is required.
                If the TLAB has insufficient space this function returns nullptr and the caller
                must fall back to a slower global allocation path.

                The _tlab field is probed on both JavaThread and Thread for JDK version
                compatibility.

                Complexity: O(1).
                Exception safety: noexcept — returns nullptr on any failure or if the TLAB
                                  has insufficient space.

                @param byte_size  Number of bytes to allocate; must be > 0 and aligned
                                  to the JVM object alignment (typically 8 bytes).
                @return Pointer to the allocated region, or nullptr if allocation failed.
            */
            auto allocate_tlab(const std::size_t byte_size) const noexcept
                -> void*
            {
                static const vmhook::hotspot::vm_struct_entry_t* const tlab_entry{ []()
                    -> const vmhook::hotspot::vm_struct_entry_t*
                    {
                        {
                            auto* entry{ vmhook::hotspot::iterate_struct_entries("JavaThread", "_tlab") };
                            if (entry)
                            {
                                return entry;
                            }
                        }
                        return vmhook::hotspot::iterate_struct_entries("Thread", "_tlab");
                    }()
                };
                static const vmhook::hotspot::vm_struct_entry_t* const top_entry{ vmhook::hotspot::iterate_struct_entries("ThreadLocalAllocBuffer", "_top") };
                static const vmhook::hotspot::vm_struct_entry_t* const end_entry{ vmhook::hotspot::iterate_struct_entries("ThreadLocalAllocBuffer", "_end") };

                if (!tlab_entry || !top_entry || !end_entry || byte_size == 0)
                {
                    return nullptr;
                }

                std::uint8_t* const tlab{ reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::java_thread*>(this)) + tlab_entry->offset };
                std::uint8_t** const top_address{ reinterpret_cast<std::uint8_t**>(tlab + top_entry->offset) };
                std::uint8_t** const end_address{ reinterpret_cast<std::uint8_t**>(tlab + end_entry->offset) };

                std::uint8_t* const top{ *top_address };
                std::uint8_t* const end{ *end_address };

                if (!vmhook::hotspot::is_valid_pointer(top) || !vmhook::hotspot::is_valid_pointer(end) || top > end)
                {
                    return nullptr;
                }

                if (static_cast<std::size_t>(end - top) < byte_size)
                {
                    return nullptr;
                }

                *top_address = top + byte_size;
                return top;
            }
        };

        // --- Thread management -----------------------------------------------

        /*
            @brief Cached JavaThread* for the calling OS thread.
            @details
            Set by ensure_current_java_thread() when the thread is first identified in the
            HotSpot thread list.  Thread-local so that each OS thread maintains its own
            cached pointer without synchronisation.
        */
        inline thread_local vmhook::hotspot::java_thread* current_java_thread{ nullptr };

        /*
            @brief Cached JNIEnv* for the calling OS thread.
            @details
            Set by attach_current_native_thread() after a successful
            AttachCurrentThread / AttachCurrentThreadAsDaemon call.  Thread-local for
            the same reason as current_java_thread.
        */
        inline thread_local void* current_jni_env{ nullptr };

        /*
            @brief The most recently observed JavaThread* across all OS threads.
            @details
            Updated atomically (memory_order_relaxed) by ensure_current_java_thread()
            and find_allocation_thread().  Used as a fast fallback by
            find_allocation_thread() when no thread-local cached pointer is available,
            avoiding a full thread-list walk in the common case.
        */
        inline std::atomic<vmhook::hotspot::java_thread*> last_java_thread{ nullptr };

        /*
            @brief Returns the first JavaThread in HotSpot's global thread list.
            @details
            Reads the head pointer from Threads._thread_list, which is a static field
            exported through gHotSpotVMStructs.  This is the entry point for walking
            the intrusive linked list of all live Java threads.

            Complexity: O(1).
            Exception safety: noexcept — returns nullptr if the list is empty or not found.

            @return Pointer to the head JavaThread, or nullptr on failure.
        */
        static auto find_any_java_thread() noexcept
            -> vmhook::hotspot::java_thread*
        {
            static const vmhook::hotspot::vm_struct_entry_t* const thread_list_entry{
                vmhook::hotspot::iterate_struct_entries("Threads", "_thread_list") };

            if (!thread_list_entry || !thread_list_entry->address)
            {
                return nullptr;
            }

            vmhook::hotspot::java_thread* const head{ *reinterpret_cast<vmhook::hotspot::java_thread**>(thread_list_entry->address) };
            return vmhook::hotspot::is_valid_pointer(head) ? head : nullptr;
        }

        /*
            @brief Finds the JavaThread whose OS thread ID matches os_thread_id.
            @details
            First walks the classic _thread_list linked list (up to 4 096 entries).
            If the thread is not found there, falls back to ThreadsSMRSupport._java_thread_list
            which is the Safe Memory Reclamation (SMR) snapshot used by JDK 10+.

            Complexity: O(N) where N = number of live Java threads.
            Exception safety: noexcept — returns nullptr if not found.

            @param os_thread_id  OS thread ID (Win32 thread ID on Windows, kernel TID on Linux).
            @return Matching JavaThread*, or nullptr if not found.
        */
        static auto find_java_thread_by_os_thread_id(const vmhook::os::thread_id_t os_thread_id) noexcept
            -> vmhook::hotspot::java_thread*
        {
            if (os_thread_id == 0)
            {
                return nullptr;
            }

            std::int32_t visited_threads{ 0 };
            for (vmhook::hotspot::java_thread* thread{ vmhook::hotspot::find_any_java_thread() };
                thread && vmhook::hotspot::is_valid_pointer(thread) && visited_threads < 4096;
                thread = thread->get_next(), ++visited_threads)
            {
                if (thread->get_os_thread_id() == os_thread_id)
                {
                    return thread;
                }
            }

            static const vmhook::hotspot::vm_struct_entry_t* const list_entry{
                vmhook::hotspot::iterate_struct_entries("ThreadsSMRSupport", "_java_thread_list") };
            static const vmhook::hotspot::vm_struct_entry_t* const length_entry{
                vmhook::hotspot::iterate_struct_entries("ThreadsList", "_length") };
            static const vmhook::hotspot::vm_struct_entry_t* const threads_entry{
                vmhook::hotspot::iterate_struct_entries("ThreadsList", "_threads") };

            if (!list_entry || !length_entry || !threads_entry || !list_entry->address)
            {
                return nullptr;
            }

            void* const list{ *reinterpret_cast<void**>(list_entry->address) };
            if (!list || !vmhook::hotspot::is_valid_pointer(list))
            {
                return nullptr;
            }

            const std::int32_t length{ *reinterpret_cast<const std::int32_t*>(reinterpret_cast<const std::uint8_t*>(list) + length_entry->offset) };
            if (length <= 0 || length > 4096)
            {
                return nullptr;
            }

            auto** const threads{ *reinterpret_cast<vmhook::hotspot::java_thread***>(reinterpret_cast<std::uint8_t*>(list) + threads_entry->offset) };
            if (!threads || !vmhook::hotspot::is_valid_pointer(threads))
            {
                return nullptr;
            }

            for (std::int32_t index{ 0 }; index < length; ++index)
            {
                vmhook::hotspot::java_thread* const thread{ threads[index] };
                if (thread && vmhook::hotspot::is_valid_pointer(thread) && thread->get_os_thread_id() == os_thread_id)
                {
                    return thread;
                }
            }

            return nullptr;
        }

        /*
            @brief Attaches the calling native (non-Java) thread to the JVM.
            @details
            Calls JNI_GetCreatedJavaVMs to locate the running JavaVM, then tries in order:
              1. GetEnv   — the thread is already attached; just record the JNIEnv.
              2. AttachCurrentThreadAsDaemon — preferred for injected native threads.
              3. AttachCurrentThread         — fallback.
            Stores the resulting JNIEnv* in current_jni_env on success.

            Complexity: O(1) — JNI vtable dispatches.
            Exception safety: noexcept — returns false on any failure.

            @return true if a JNIEnv was obtained, false otherwise.
        */
        static auto attach_current_native_thread() noexcept
            -> bool
        {
            vmhook::os::module_handle const jvm_module{ vmhook::hotspot::get_jvm_module() };
            if (!jvm_module)
            {
                return false;
            }

            using jint = int;
            struct JNIEnv__;
            struct JavaVM__;
            using JNIEnv = JNIEnv__;
            using JavaVM = JavaVM__;

            struct JNIInvokeInterface_
            {
                void* reserved0;
                void* reserved1;
                void* reserved2;
                jint (*DestroyJavaVM)(JavaVM*);
                jint (*AttachCurrentThread)(JavaVM*, void**, void*);
                jint (*DetachCurrentThread)(JavaVM*);
                jint (*GetEnv)(JavaVM*, void**, jint);
                jint (*AttachCurrentThreadAsDaemon)(JavaVM*, void**, void*);
            };

            struct JavaVM__
            {
                const JNIInvokeInterface_* functions;
            };

            using get_created_java_vms_t = jint (*)(JavaVM**, jint, jint*);
            auto* const get_created_java_vms{ reinterpret_cast<get_created_java_vms_t>(
                vmhook::os::get_proc_address(jvm_module, "JNI_GetCreatedJavaVMs")) };
            if (!get_created_java_vms)
            {
                return false;
            }

            JavaVM* vm{};
            jint vm_count{};
            if (get_created_java_vms(&vm, 1, &vm_count) != 0 || vm_count <= 0 || !vm || !vm->functions)
            {
                return false;
            }

            constexpr jint jni_version_1_8{ 0x00010008 };
            void* env{};
            if (vm->functions->GetEnv && vm->functions->GetEnv(vm, &env, jni_version_1_8) == 0)
            {
                vmhook::hotspot::current_jni_env = env;
                vmhook::detail::inherit_host_context_classloader_for_current_thread();
                return true;
            }

            if (vm->functions->AttachCurrentThreadAsDaemon && vm->functions->AttachCurrentThreadAsDaemon(vm, &env, nullptr) == 0)
            {
                vmhook::hotspot::current_jni_env = env;
                vmhook::detail::inherit_host_context_classloader_for_current_thread();
                return true;
            }

            if (vm->functions->AttachCurrentThread && vm->functions->AttachCurrentThread(vm, &env, nullptr) == 0)
            {
                vmhook::hotspot::current_jni_env = env;
                vmhook::detail::inherit_host_context_classloader_for_current_thread();
                return true;
            }

            return false;
        }

        /*
            @brief Ensures the calling thread has a valid JavaThread* and JNIEnv*.
            @details
            Resolution order:
              1. Thread-local current_java_thread is already set — fast return.
              2. Search the HotSpot thread list for the current OS thread ID.
              3. Attach the thread via JNI (attach_current_native_thread) and retry
                 up to 64 times with thread yields while HotSpot registers it.

            Complexity: O(N) worst case on first call, where N = number of live Java threads.
            Exception safety: noexcept — returns false on failure.
            Thread safety: safe to call from any OS thread; state is stored thread-locally.

            @return true if current_java_thread is valid after the call, false otherwise.
        */
        static auto ensure_current_java_thread() noexcept
            -> bool
        {
            if (vmhook::hotspot::current_java_thread && vmhook::hotspot::is_valid_pointer(vmhook::hotspot::current_java_thread))
            {
                if (!vmhook::hotspot::current_jni_env)
                {
                    vmhook::hotspot::attach_current_native_thread();
                }

                return true;
            }

            const vmhook::os::thread_id_t current_os_thread_id{ vmhook::os::current_thread_id() };
            if (vmhook::hotspot::java_thread* const existing_thread{ vmhook::hotspot::find_java_thread_by_os_thread_id(current_os_thread_id) })
            {
                vmhook::hotspot::current_java_thread = existing_thread;
                vmhook::hotspot::last_java_thread.store(existing_thread, std::memory_order_relaxed);
                VMHOOK_LOG("{} ensure_current_java_thread(): adopted JavaThread 0x{:016X} for OS thread {}", vmhook::info_tag, reinterpret_cast<std::uintptr_t>(existing_thread), current_os_thread_id);
                return true;
            }

            if (!vmhook::hotspot::attach_current_native_thread())
            {
                VMHOOK_LOG("{} ensure_current_java_thread(): AttachCurrentThread failed for OS thread {}", vmhook::error_tag, current_os_thread_id);
                return false;
            }

            for (std::int32_t attempt{ 0 }; attempt < 64; ++attempt)
            {
                if (vmhook::hotspot::java_thread* const attached_thread{ vmhook::hotspot::find_java_thread_by_os_thread_id(current_os_thread_id) })
                {
                    vmhook::hotspot::current_java_thread = attached_thread;
                    vmhook::hotspot::last_java_thread.store(attached_thread, std::memory_order_relaxed);
                    VMHOOK_LOG("{} ensure_current_java_thread(): attached JavaThread 0x{:016X} for OS thread {}", vmhook::info_tag, reinterpret_cast<std::uintptr_t>(attached_thread), current_os_thread_id);
                    return true;
                }

                std::this_thread::yield();
            }

            VMHOOK_LOG("{} ensure_current_java_thread(): attached thread was not found in HotSpot thread list for OS thread {}", vmhook::error_tag, current_os_thread_id);
            return false;
        }

        /*
            @brief Returns a JavaThread suitable for TLAB allocation without ensure_current_java_thread.
            @details
            Faster than ensure_current_java_thread because it never attaches the thread.
            Resolution order:
              1. Thread-local current_java_thread — zero overhead.
              2. last_java_thread atomic — one relaxed load, suitable from injected threads.
              3. find_any_java_thread  — walks the thread list once.

            Complexity: O(1) in cases 1 and 2, O(N) in case 3.
            Exception safety: noexcept — returns nullptr if no thread can be found.
            Thread safety: safe for concurrent reads; writes to last_java_thread are relaxed.

            @return A valid JavaThread*, or nullptr if none is available.
        */
        static auto find_allocation_thread() noexcept
            -> vmhook::hotspot::java_thread*
        {
            if (vmhook::hotspot::current_java_thread && vmhook::hotspot::is_valid_pointer(vmhook::hotspot::current_java_thread))
            {
                vmhook::hotspot::last_java_thread.store(vmhook::hotspot::current_java_thread, std::memory_order_relaxed);
                return vmhook::hotspot::current_java_thread;
            }

            vmhook::hotspot::java_thread* const cached_thread{ vmhook::hotspot::last_java_thread.load(std::memory_order_relaxed) };
            if (cached_thread && vmhook::hotspot::is_valid_pointer(cached_thread))
            {
                return cached_thread;
            }

            vmhook::hotspot::java_thread* const discovered_thread{ vmhook::hotspot::find_any_java_thread() };
            if (discovered_thread)
            {
                vmhook::hotspot::last_java_thread.store(discovered_thread, std::memory_order_relaxed);
            }
            return discovered_thread;
        }

        /*
            @brief Allocates byte_size bytes from any thread's TLAB by scanning the SMR thread list.
            @details
            Used as a fallback when find_allocation_thread() returns nullptr.  Iterates
            ThreadsSMRSupport._java_thread_list and calls java_thread::allocate_tlab() on
            each entry until one succeeds.  Updates last_java_thread to the thread that
            provided the allocation so that future calls hit the fast path.

            Complexity: O(N) where N = number of live Java threads.
            Exception safety: noexcept — returns nullptr if no TLAB has sufficient space.

            @param byte_size  Number of bytes requested; must be > 0.
            @return Pointer to the allocated memory, or nullptr on failure.
        */
        static auto allocate_from_threads_list(const std::size_t byte_size) noexcept
            -> void*
        {
            static const vmhook::hotspot::vm_struct_entry_t* const list_entry{
                vmhook::hotspot::iterate_struct_entries("ThreadsSMRSupport", "_java_thread_list") };
            static const vmhook::hotspot::vm_struct_entry_t* const length_entry{
                vmhook::hotspot::iterate_struct_entries("ThreadsList", "_length") };
            static const vmhook::hotspot::vm_struct_entry_t* const threads_entry{
                vmhook::hotspot::iterate_struct_entries("ThreadsList", "_threads") };

            if (!list_entry || !length_entry || !threads_entry || !list_entry->address)
            {
                return nullptr;
            }

            void* const list{ *reinterpret_cast<void**>(list_entry->address) };
            if (!list || !vmhook::hotspot::is_valid_pointer(list))
            {
                return nullptr;
            }

            const std::int32_t length{ *reinterpret_cast<const std::int32_t*>(reinterpret_cast<const std::uint8_t*>(list) + length_entry->offset) };
            if (length <= 0 || length > 4096)
            {
                return nullptr;
            }

            auto** const threads{ *reinterpret_cast<vmhook::hotspot::java_thread***>(reinterpret_cast<std::uint8_t*>(list) + threads_entry->offset) };
            if (!threads || !vmhook::hotspot::is_valid_pointer(threads))
            {
                return nullptr;
            }

            for (std::int32_t index{ 0 }; index < length; ++index)
            {
                vmhook::hotspot::java_thread* const thread{ threads[index] };
                if (!thread || !vmhook::hotspot::is_valid_pointer(thread))
                {
                    continue;
                }

                void* const object{ thread->allocate_tlab(byte_size) };
                if (object)
                {
                    vmhook::hotspot::last_java_thread.store(thread, std::memory_order_relaxed);
                    return object;
                }
            }

            return nullptr;
        }

        // --- OOP and Klass pointer encoding / decoding -----------------------

        /*
            @brief Decodes a compressed OOP to a real 64-bit pointer.
            @param compressed The 32-bit compressed OOP value read from a JVM structure.
            @return The decoded real pointer, or nullptr if compressed is 0.
            @details
            HotSpot stores heap object pointers in 32-bit "compressed OOP" form to reduce
            memory usage. The formula to recover the real 64-bit address is:
              real_address = narrow_oop_base + (compressed << narrow_oop_shift)

            - narrow_oop_base  - base address of the Java heap (0 when -Xmx < 4 GB and
                                  heap starts at address 0, otherwise the heap start).
            - narrow_oop_shift - how many bits to left-shift the compressed value (typically
                                  0 for heap < 4 GB, 3 for heap up to 32 GB with 8-byte aligned oops).

            Both values are read from CompressedOops::_narrow_oop.{_base,_shift} via
            gHotSpotVMStructs so this works across all JDK versions.
        */
        static auto decode_oop_pointer(const std::uint32_t compressed) noexcept
            -> void*
        {
            if (!compressed)
            {
                return nullptr;
            }

            // VMStruct field names for the narrow OOP base/shift changed across versions:
            //   JDK  8-16: Universe::_narrow_oop._base/shift
            //   JDK 17-24: CompressedOops::_narrow_oop._base/shift
            //   JDK 25+  : CompressedOops::_base/shift  (_narrow_oop. prefix dropped)
            static const vmhook::hotspot::vm_struct_entry_t* const base_entry{ []()
                -> const vmhook::hotspot::vm_struct_entry_t*
                {
                    {
                        auto* entry{ vmhook::hotspot::iterate_struct_entries("CompressedOops", "_narrow_oop._base") };
                        if (entry)
                        {
                            return entry;
                        }
                    }
                    {
                        auto* entry{ vmhook::hotspot::iterate_struct_entries("CompressedOops", "_base") };
                        if (entry)
                        {
                            return entry;
                        }
                    }
                    return vmhook::hotspot::iterate_struct_entries("Universe", "_narrow_oop._base");
                }()
            };

            static const vmhook::hotspot::vm_struct_entry_t* const shift_entry{ []()
                -> const vmhook::hotspot::vm_struct_entry_t*
                {
                    {
                        auto* entry{ vmhook::hotspot::iterate_struct_entries("CompressedOops", "_narrow_oop._shift") };
                        if (entry)
                        {
                            return entry;
                        }
                    }
                    {
                        auto* entry{ vmhook::hotspot::iterate_struct_entries("CompressedOops", "_shift") };
                        if (entry)
                        {
                            return entry;
                        }
                    }
                    return vmhook::hotspot::iterate_struct_entries("Universe", "_narrow_oop._shift");
                }()
            };

            if (!base_entry || !shift_entry)
            {
                return nullptr;
            }

            const std::uint64_t narrow_oop_base{ *reinterpret_cast<const std::uint64_t*>(base_entry->address) };
            const std::uint32_t narrow_oop_shift{ *reinterpret_cast<const std::uint32_t*>(shift_entry->address) };

            // real_address = narrow_oop_base + (compressed << narrow_oop_shift)
            return reinterpret_cast<void*>(narrow_oop_base + (static_cast<std::uint64_t>(compressed) << narrow_oop_shift));
        }

        /*
            @brief Compresses a decoded OOP pointer back into HotSpot's narrow OOP form.
            @details
            This is the inverse of decode_oop_pointer() and is used when assigning
            object wrapper fields through field_proxy::set().
        */
        static auto encode_oop_pointer(void* const decoded) noexcept
            -> std::uint32_t
        {
            if (!decoded)
            {
                return 0;
            }

            static const vmhook::hotspot::vm_struct_entry_t* const base_entry{ []()
                -> const vmhook::hotspot::vm_struct_entry_t*
                {
                    {
                        auto* entry{ vmhook::hotspot::iterate_struct_entries("CompressedOops", "_narrow_oop._base") };
                        if (entry)
                        {
                            return entry;
                        }
                    }
                    {
                        auto* entry{ vmhook::hotspot::iterate_struct_entries("CompressedOops", "_base") };
                        if (entry)
                        {
                            return entry;
                        }
                    }
                    return vmhook::hotspot::iterate_struct_entries("Universe", "_narrow_oop._base");
                }()
            };

            static const vmhook::hotspot::vm_struct_entry_t* const shift_entry{ []()
                -> const vmhook::hotspot::vm_struct_entry_t*
                {
                    {
                        auto* entry{ vmhook::hotspot::iterate_struct_entries("CompressedOops", "_narrow_oop._shift") };
                        if (entry)
                        {
                            return entry;
                        }
                    }
                    {
                        auto* entry{ vmhook::hotspot::iterate_struct_entries("CompressedOops", "_shift") };
                        if (entry)
                        {
                            return entry;
                        }
                    }
                    return vmhook::hotspot::iterate_struct_entries("Universe", "_narrow_oop._shift");
                }()
            };

            if (!base_entry || !shift_entry)
            {
                return 0;
            }

            const std::uint64_t narrow_oop_base{ *reinterpret_cast<const std::uint64_t*>(base_entry->address) };
            const std::uint32_t narrow_oop_shift{ *reinterpret_cast<const std::uint32_t*>(shift_entry->address) };
            const std::uint64_t decoded_address{ reinterpret_cast<std::uint64_t>(decoded) };
            if (decoded_address < narrow_oop_base)
            {
                return 0;
            }

            return static_cast<std::uint32_t>((decoded_address - narrow_oop_base) >> narrow_oop_shift);
        }

        /*
            @brief Decodes a compressed Klass pointer to a real 64-bit pointer.
            @details
            Klass pointers use a separate compressed-pointer scheme from object OOPs,
            stored in CompressedKlassPointers::_narrow_klass.{_base,_shift}.
            The decoding formula is identical: real_address = base + (compressed << shift).
        */
        static auto decode_klass_pointer(const std::uint32_t compressed) noexcept
            -> void*
        {
            if (!compressed)
            {
                return nullptr;
            }

            // VMStruct field names changed the same way as for CompressedOops:
            //   JDK  8-16: Universe::_narrow_klass._base/shift
            //   JDK 17-24: CompressedKlassPointers::_narrow_klass._base/shift
            //   JDK 25+  : CompressedKlassPointers::_base/shift
            static const vmhook::hotspot::vm_struct_entry_t* const base_entry{ []()
                -> const vmhook::hotspot::vm_struct_entry_t*
                {
                    {
                        auto* entry{ vmhook::hotspot::iterate_struct_entries("CompressedKlassPointers", "_narrow_klass._base") };
                        if (entry)
                        {
                            return entry;
                        }
                    }
                    {
                        auto* entry{ vmhook::hotspot::iterate_struct_entries("CompressedKlassPointers", "_base") };
                        if (entry)
                        {
                            return entry;
                        }
                    }
                    return vmhook::hotspot::iterate_struct_entries("Universe", "_narrow_klass._base");
                }()
            };
            static const vmhook::hotspot::vm_struct_entry_t* const shift_entry{ []()
                -> const vmhook::hotspot::vm_struct_entry_t*
                {
                    {
                        auto* entry{ vmhook::hotspot::iterate_struct_entries("CompressedKlassPointers", "_narrow_klass._shift") };
                        if (entry)
                        {
                            return entry;
                        }
                    }
                    {
                        auto* entry{ vmhook::hotspot::iterate_struct_entries("CompressedKlassPointers", "_shift") };
                        if (entry)
                        {
                            return entry;
                        }
                    }
                    return vmhook::hotspot::iterate_struct_entries("Universe", "_narrow_klass._shift");
                }()
            };

            if (!base_entry || !shift_entry)
            {
                return nullptr;
            }

            const std::uint64_t base{ *reinterpret_cast<const std::uint64_t*>(base_entry->address) };
            const std::uint32_t shift{ *reinterpret_cast<const std::uint32_t*>(shift_entry->address) };

            return reinterpret_cast<void*>(base + (static_cast<std::uint64_t>(compressed) << shift));
        }

        /*
            @brief Compresses a decoded Klass pointer back into HotSpot's narrow Klass form.
            @details
            Inverse of decode_klass_pointer().  The formula is:
              compressed = (decoded_address - narrow_klass_base) >> narrow_klass_shift
            Used when writing class-pointer fields (e.g. the mark-word klass field in an
            object header) back into JVM structures.

            Complexity: O(1) after VMStruct entries are cached on first call.
            Exception safety: noexcept — returns 0 on failure or if decoded is null.

            @param decoded  Full 64-bit Klass pointer to compress.
            @return 32-bit compressed Klass pointer, or 0 on failure.
        */
        static auto encode_klass_pointer(void* const decoded) noexcept
            -> std::uint32_t
        {
            if (!decoded)
            {
                return 0;
            }

            static const vmhook::hotspot::vm_struct_entry_t* const base_entry{ []()
                -> const vmhook::hotspot::vm_struct_entry_t*
                {
                    {
                        auto* entry{ vmhook::hotspot::iterate_struct_entries("CompressedKlassPointers", "_narrow_klass._base") };
                        if (entry)
                        {
                            return entry;
                        }
                    }
                    {
                        auto* entry{ vmhook::hotspot::iterate_struct_entries("CompressedKlassPointers", "_base") };
                        if (entry)
                        {
                            return entry;
                        }
                    }
                    return vmhook::hotspot::iterate_struct_entries("Universe", "_narrow_klass._base");
                }()
            };
            static const vmhook::hotspot::vm_struct_entry_t* const shift_entry{ []()
                -> const vmhook::hotspot::vm_struct_entry_t*
                {
                    {
                        auto* entry{ vmhook::hotspot::iterate_struct_entries("CompressedKlassPointers", "_narrow_klass._shift") };
                        if (entry)
                        {
                            return entry;
                        }
                    }
                    {
                        auto* entry{ vmhook::hotspot::iterate_struct_entries("CompressedKlassPointers", "_shift") };
                        if (entry)
                        {
                            return entry;
                        }
                    }
                    return vmhook::hotspot::iterate_struct_entries("Universe", "_narrow_klass._shift");
                }()
            };

            if (!base_entry || !shift_entry)
            {
                return 0;
            }

            const std::uint64_t base{ *reinterpret_cast<const std::uint64_t*>(base_entry->address) };
            const std::uint32_t shift{ *reinterpret_cast<const std::uint32_t*>(shift_entry->address) };
            const std::uint64_t decoded_address{ reinterpret_cast<std::uint64_t>(decoded) };

            if (decoded_address < base)
            {
                return 0;
            }

            return static_cast<std::uint32_t>((decoded_address - base) >> shift);
        }

        /*
            @brief Checks whether a memory region matches a given byte pattern.
            @details
            A pattern byte of 0x00 acts as a wildcard and always matches.
        */
        inline static auto match_pattern(const std::uint8_t* const address, const std::uint8_t* const pattern, const std::size_t size)
            -> bool
        {
            for (std::size_t byte_index{ 0 }; byte_index < size; ++byte_index)
            {
                if (pattern[byte_index] == 0x00)
                {
                    continue;
                }
                if (address[byte_index] != pattern[byte_index])
                {
                    return false;
                }
            }
            return true;
        }

        /*
            @brief Scans a memory region for the first occurrence of a byte pattern.
            @details
            A pattern byte of 0x00 acts as a wildcard and always matches.
        */
        inline static auto scan(const std::uint8_t* const start, const std::size_t range, const std::uint8_t* pattern, const std::size_t size)
            -> std::uint8_t*
        {
            for (std::size_t scan_offset{ 0 }; scan_offset < range; ++scan_offset)
            {
                if (vmhook::hotspot::match_pattern(start + scan_offset, pattern, size))
                {
                    return const_cast<std::uint8_t*>(start + scan_offset);
                }
            }

            return nullptr;
        }

        /*
            @brief Determines the safe scannable size of a JVM generated code stub.
            @details
            Uses VirtualQuery to retrieve the memory region information and computes
            how many bytes remain from start to the end of the region, capped at 0x2000.
        */
        static auto find_stub_size(const std::uint8_t* start)
            -> std::size_t
        {
            const vmhook::os::region_info info{ vmhook::os::query_region(start) };
            if (!info.base || info.size == 0)
            {
                return static_cast<std::size_t>(0x2000);
            }
            const std::uint8_t* const region_end{
                reinterpret_cast<const std::uint8_t*>(info.base) + info.size };
            if (region_end <= start)
            {
                return static_cast<std::size_t>(0x2000);
            }
            const std::size_t remaining{ static_cast<std::size_t>(region_end - start) };
            return (std::min)(remaining, static_cast<std::size_t>(0x2000));
        }

        /*
            @brief Byte offset used to retrieve the local variables pointer from the interpreter frame.
            @details
            Determined at runtime by find_hook_location(). Defaults to -56.
        */
        inline constinit std::int8_t locals_offset{ -56 };

        /*
            @brief Locates the optimal injection point within a HotSpot i2i interpreter stub.
            @param i2i_entry Pointer to the beginning of the i2i interpreter stub to scan.
            @return Pointer to the injection point within the stub, or nullptr on failure.
            @details
            Scans the i2i stub for a known sequence of instructions that appears at a stable
            location across JVM builds, immediately before the interpreter begins executing
            the actual Java bytecode. All method arguments are fully set up at this point.
            Also scans backwards to find and cache the locals_offset value.
            @note Two patterns are tried in order:
                  1. Full pattern (JDK 8 through early JDK 21): 4-mov-spill + mov BYTE PTR [r15+??],??
                     Hook injected at the thread-state-write instruction (last 8 bytes).
                  2. Fallback (JDK 21 release / JDK 22+): just mov BYTE PTR [r15+??],??
                     Hook injected at the start of that instruction directly.
        */
        static auto find_hook_location(const void* i2i_entry)
            -> void*
        {
            /*
                Primary pattern (JDK 8 through early JDK 21 builds):
                  Four consecutive `mov [rsp+imm32], eax` instructions that spill the first
                  four Windows x64 integer arguments to the shadow area, followed by
                  `mov BYTE PTR [r15+imm32], imm8` which writes a thread-status byte.
                  Wildcard bytes (0x00) match any value.
            */
            static constexpr std::uint8_t pattern_full[]
            {
                0x89, 0x84, 0x24, 0x00, 0x00, 0x00, 0x00, // mov [rsp+??], eax
                0x89, 0x84, 0x24, 0x00, 0x00, 0x00, 0x00, // mov [rsp+??], eax
                0x89, 0x84, 0x24, 0x00, 0x00, 0x00, 0x00, // mov [rsp+??], eax
                0x89, 0x84, 0x24, 0x00, 0x00, 0x00, 0x00, // mov [rsp+??], eax
                0x41, 0xC6, 0x87, 0x00, 0x00, 0x00, 0x00, 0x00 // mov BYTE PTR [r15+??], ??
            };

            /*
                Fallback pattern (JDK 21 release builds, JDK 22+):
                  The 4-mov spill block is absent or different, but the
                  `mov BYTE PTR [r15+imm32], imm8` thread-status write is always present.
                  0x41 0xC6 0x87 = REX.B MOV r/m8,imm8 with ModRM selecting [r15+disp32].
                  All four displacement bytes and the imm8 are wildcards.
            */
            static constexpr std::uint8_t pattern_fallback[]
            {
                0x41, 0xC6, 0x87, 0x00, 0x00, 0x00, 0x00, 0x00 // mov BYTE PTR [r15+??], ??
            };

            static constexpr std::uint8_t locals_pattern[]
            {
                0x4C, 0x8B, 0x75, 0x00, 0xC3 // mov r14, QWORD PTR [rbp+??] ; ret
            };

            const std::uint8_t* const current{ reinterpret_cast<const std::uint8_t*>(i2i_entry) };
            const std::size_t scan_size{ vmhook::hotspot::find_stub_size(current) };

            // Try the full 4-spill + thread-state-write pattern first (JDK 8 through early JDK 21).
            // The injection point is at the START of the thread-state-write instruction,
            // which sits at the END of the full pattern (offset = sizeof - 8).
            std::uint8_t* injection_point{ nullptr };
            std::uint8_t* const full_match{ vmhook::hotspot::scan(current, scan_size, pattern_full, sizeof(pattern_full)) };
            if (full_match)
            {
                // The thread-state instruction is the last 8 bytes of the full match.
                injection_point = full_match + sizeof(pattern_full) - 8;
            }
            else
            {
                // Fallback: scan directly for `mov BYTE PTR [r15+??], ??` (JDK 21+).
                // The injection point is at the START of the matched instruction.
                std::uint8_t* const fallback_match{ vmhook::hotspot::scan(current, scan_size, pattern_fallback, sizeof(pattern_fallback)) };
                if (fallback_match)
                {
                    injection_point = fallback_match;
                }
            }

            try
            {
                if (!injection_point)
                {
                    throw vmhook::exception{ "Failed to find hook pattern (tried full and fallback)." };
                }

                // Scan backwards from the injection point to find the locals pointer load:
                //   mov r14, QWORD PTR [rbp+disp8]  ; ret
                // The displacement byte is the rbp-relative offset of the locals pointer.
                for (std::uint8_t* scan_ptr{ injection_point }; scan_ptr > current; --scan_ptr)
                {
                    if (vmhook::hotspot::match_pattern(scan_ptr, locals_pattern, sizeof(locals_pattern)))
                    {
                        // Byte [3] of `4C 8B 75 ??` is the signed 8-bit displacement.
                        locals_offset = static_cast<std::int8_t>(scan_ptr[3]);
                        break;
                    }
                }

                return injection_point;
            }
            catch (const std::exception& exception)
            {
                VMHOOK_LOG("{} find_hook_location() {}", vmhook::error_tag, exception.what());
                return nullptr;
            }
        }

        /*
            @brief Allocates a block of executable memory within a 32-bit relative jump
                   range of a given address.
            @details
            Walks the process address space and allocates inside a free region that is
            reachable by a 5-byte relative JMP.  HotSpot often reserves dense areas around
            code stubs, so blindly probing exact offsets can fail even when a usable
            nearby free region exists.  The implementation uses the platform-specific
            primitives wrapped behind vmhook::os::query_region / vmhook::os::allocate_rwx.
        */
        static auto allocate_nearby_memory(std::uint8_t* nearby_addr, const std::size_t size) noexcept
            -> std::uint8_t*
        {
            if (!nearby_addr || size == 0)
            {
                return nullptr;
            }

            const std::uintptr_t minimum_application_address{ static_cast<std::uintptr_t>(0x10000) };
            const std::uintptr_t maximum_application_address{ vmhook::os::user_address_ceiling };
            const std::uintptr_t allocation_granularity{ static_cast<std::uintptr_t>(
                vmhook::os::allocation_granularity()) };
            const std::uintptr_t target_address{ reinterpret_cast<std::uintptr_t>(nearby_addr) };
            const std::uintptr_t relative_limit{ static_cast<std::uintptr_t>(
                (std::numeric_limits<std::int32_t>::max)()) };

            const auto align_up = [](const std::uintptr_t value, const std::uintptr_t alignment) noexcept
                -> std::uintptr_t
            {
                return (value + alignment - 1) & ~(alignment - 1);
            };

            const auto align_down = [](const std::uintptr_t value, const std::uintptr_t alignment) noexcept
                -> std::uintptr_t
            {
                return value & ~(alignment - 1);
            };

            const std::uintptr_t search_min{
                (std::max)(minimum_application_address,
                    target_address > relative_limit
                        ? target_address - relative_limit
                        : minimum_application_address)
            };
            const std::uintptr_t search_max{
                target_address > maximum_application_address - relative_limit
                    ? maximum_application_address
                    : (std::min)(maximum_application_address, target_address + relative_limit)
            };

            auto try_allocate_in_region = [&](const std::uintptr_t region_base,
                                              const std::uintptr_t region_end) noexcept
                -> std::uint8_t*
            {
                if (region_end <= region_base || region_end - region_base < size)
                {
                    return nullptr;
                }

                const std::uintptr_t usable_begin{ (std::max)(region_base, search_min) };
                const std::uintptr_t usable_end{ (std::min)(region_end, search_max + 1) };
                if (usable_end <= usable_begin || usable_end - usable_begin < size)
                {
                    return nullptr;
                }

                const std::uintptr_t first_candidate{ align_up(usable_begin, allocation_granularity) };
                const std::uintptr_t last_candidate{ align_down(usable_end - size, allocation_granularity) };
                if (first_candidate > last_candidate)
                {
                    return nullptr;
                }

                const std::uintptr_t preferred_candidate{
                    target_address < first_candidate ? first_candidate :
                    target_address > last_candidate ? last_candidate :
                    align_down(target_address, allocation_granularity)
                };

                if (void* const allocated{ vmhook::os::allocate_rwx(
                        reinterpret_cast<void*>(preferred_candidate), size) })
                {
                    return reinterpret_cast<std::uint8_t*>(allocated);
                }

                if (preferred_candidate != first_candidate)
                {
                    if (void* const allocated{ vmhook::os::allocate_rwx(
                            reinterpret_cast<void*>(first_candidate), size) })
                    {
                        return reinterpret_cast<std::uint8_t*>(allocated);
                    }
                }

                if (preferred_candidate != last_candidate && first_candidate != last_candidate)
                {
                    if (void* const allocated{ vmhook::os::allocate_rwx(
                            reinterpret_cast<void*>(last_candidate), size) })
                    {
                        return reinterpret_cast<std::uint8_t*>(allocated);
                    }
                }

                return nullptr;
            };

            const std::size_t page_size{ vmhook::os::page_size() };
            for (std::uintptr_t current{ search_min }; current < search_max; )
            {
                const vmhook::os::region_info info{ vmhook::os::query_region(
                    reinterpret_cast<void*>(current)) };

                if (!info.base)
                {
                    current += page_size;
                    continue;
                }

                const std::uintptr_t region_base{ reinterpret_cast<std::uintptr_t>(info.base) };
                const std::uintptr_t region_size{ info.size };
                const std::uintptr_t region_end{ region_base + region_size };

                if (info.free)
                {
                    if (std::uint8_t* const allocated{ try_allocate_in_region(region_base, region_end) })
                    {
                        return allocated;
                    }
                }

                if (region_end <= current)
                {
                    break;
                }
                current = region_end;
            }

            return nullptr;
        }

        /*
            @brief Raw function pointer type for the low-level hook trampoline.
            @details
            Every hook trampoline generated by midi2i_hook calls through a pointer of this
            type.  The three arguments are passed in the System V / Microsoft x64 calling
            convention by the trampoline assembly:
              frame*        - rbp of the intercepted interpreter frame.
              java_thread*  - current JavaThread* (from r15 on HotSpot x64).
              return_slot*  - pre-allocated slot for the callback to record cancellation
                              and the return value.
        */
        using detour_function_t = void(*)(vmhook::hotspot::frame*, vmhook::hotspot::java_thread*, vmhook::hotspot::return_slot*);

        /*
            @brief Type-erased container for method arguments obtained via auto-detection.
            @details
            Returned by frame::get_arguments() (no template parameters). Stores raw decoded
            OOP pointers and class names. Call as<T>() to construct a wrapper on demand.
        */
        struct method_args
        {
            /*
                @brief One decoded argument from an intercepted Java method call.
                @details
                decoded_oop holds the full 64-bit heap pointer after OOP decompression,
                or zero for primitive arguments that do not have an OOP representation.
                class_name holds the internal JVM class name (slash-separated), used by
                method_args::as<T>() to construct the correct C++ wrapper type.
            */
            struct argument_entry
            {
                void* decoded_oop;
                std::string class_name;
            };

            /*
                @brief Returns the raw decoded OOP at the given index.
                @param index Zero-based argument index (0 = this / first parameter).
            */
            auto operator[](const std::size_t index) const noexcept
                -> void*
            {
                if (index >= this->arguments.size())
                {
                    return nullptr;
                }
                return this->arguments[index].decoded_oop;
            }

            /*
                @brief Returns the argument at the given index as a C++ wrapper.
                @tparam wrapper_type The C++ wrapper class (must be constructible from void* OOP).
                @param index Zero-based argument index.
                @return A new wrapper_type* constructed from the decoded OOP, or nullptr.
            */
            template<typename wrapper_type>
            auto as(const std::size_t index) const noexcept
                -> wrapper_type*
            {
                if (index >= this->arguments.size())
                {
                    return nullptr;
                }
                void* const raw{ this->arguments[index].decoded_oop };
                if (!raw)
                {
                    return nullptr;
                }
                return new wrapper_type{ raw };
            }

            /*
                @brief Returns the number of arguments in this container.
                @details
                Includes both reference-type (object) and primitive arguments.

                Complexity: O(1).
                Exception safety: noexcept.
            */
            auto size() const noexcept
                -> std::size_t
            {
                return this->arguments.size();
            }

            std::vector<argument_entry> arguments{};
        };


        /*
            @brief Represents a HotSpot interpreter frame on the call stack.
            @details
            In HotSpot's interpreter, each method invocation creates a frame on the
            native call stack. On x64 HotSpot, the base pointer (rbp) always points
            to the current frame. The Method pointer is at -24 bytes from rbp, and
            the local variables pointer is at locals_offset bytes from rbp.
        */
        struct frame
        {
        public:
            /*
                @brief Returns a pointer to the Method object of the currently executing method.
                @details
                On x64 HotSpot, `this` is rbp (the interpreter frame base pointer).
                The x64 interpreter frame layout relative to rbp is:
                  rbp + 0   saved caller rbp
                  rbp - 8   return address (pushed by call instruction)
                  rbp - 16  last_sp / expression stack bottom
                  rbp - 24  Method* pointer at this offset
                This corresponds to interpreter_frame_method_offset = -3 words = -24 bytes
                as defined in frame_x86.hpp.
            */
            inline auto get_method() const noexcept
                -> vmhook::hotspot::method*
            {
                // -24 bytes = -3 * sizeof(void*): the Method* slot in the interpreter frame.
                return *reinterpret_cast<vmhook::hotspot::method**>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::frame*>(this)) - 24);
            }

            /*
                @brief Returns a pointer to the local variables array of the currently executing method.
                @details
                The frame slot at [rbp + locals_offset] encodes the locals pointer in one of two
                formats depending on the JDK era (see get_locals() implementation comment for
                the full derivation and proof from the hs_err crash dump).
                The actual offset within the frame is determined at hook time by scanning the i2i
                stub backwards for `mov r14, QWORD PTR [rbp+??]; ret`.  Defaults to -56.
            */
            inline auto get_locals() const noexcept
                -> void**
            {
                /*
                    How the locals pointer is encoded in the frame differs by JDK era.

                    JDK 8 through JDK 20:
                      The stub spills r14 (the locals register) directly into the frame
                      via `mov r14, QWORD PTR [rbp+locals_offset]; ret` in a helper.
                      So [rbp + locals_offset] IS the locals pointer - a valid stack
                      address that passes vmhook::hotspot::is_valid_pointer().

                    JDK 21+:
                      The stub computes and spills an INDEX instead:
                        mov rax, r14
                        sub rax, rbp        ; rax = r14 - rbp  (positive, locals are above rbp)
                        shr rax, 3          ; rax = (r14 - rbp) >> 3
                        push rax            ; [rbp - 56] = index
                      The raw value at [rbp + locals_offset] is therefore a small positive
                      integer (e.g. 3), NOT a pointer.  Recover via: r14 = rbp + index * 8.

                    Detection: if the value at the frame slot is a valid user-space pointer
                    (> 0xFFFF), treat it as a direct locals pointer (JDK 8-20).
                    Otherwise treat it as a slot index (JDK 21+).

                    Proof from hs_err crash dump for JDK 21:
                      RBP = 0x243f888    [rbp-56] = 3
                      R14 = 0x243f8a0    rbp + 3*8 = 0x243f8a0
                */
                const void* const frame_slot_value{ *reinterpret_cast<void* const*>(reinterpret_cast<const std::uint8_t*>(this) + locals_offset) };

                // JDK 8-20: direct pointer stored in the frame slot.
                if (vmhook::hotspot::is_valid_pointer(frame_slot_value))
                {
                    return const_cast<void**>(reinterpret_cast<const void* const*>(frame_slot_value));
                }

                // JDK 21+: the slot holds (r14 - rbp) >> 3 - a non-negative slot index.
                // Recover r14 = rbp + index * sizeof(void*).
                const std::uintptr_t slot_index{ reinterpret_cast<std::uintptr_t>(frame_slot_value) };
                if (slot_index < 0x1000u)
                {
                    return reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::frame*>(this)) + slot_index * sizeof(void*));
                }

                return nullptr;
            }

            /*
                @brief Retrieves all method arguments as a typed tuple.
                @tparam types The C++ types of the method arguments in declaration order.
                @return A std::tuple containing all arguments converted to their C++ types.
                @details
                For primitive types, the raw slot value is reinterpreted directly.
                For pointer types, the compressed OOP is decoded via decode_oop_pointer().
                For index 0 on instance methods, this holds the implicit `this` reference.
            */
            template<typename... types>
            auto get_arguments() const noexcept
                -> std::tuple<types...>
            {
                if constexpr (sizeof...(types) == 0)
                {
                    return std::tuple<>{};
                }
                else
                {
                    // A Java long/double occupies TWO interpreter local slots;
                    // everything else (int/float/short/…, object references,
                    // unique_ptr wrappers, strings) occupies one.  Each arg's slot
                    // index is the running sum of prior widths, NOT the tuple index
                    // — reading by tuple index made every arg AFTER a long/double
                    // come from the wrong slot (audit hook_arg_decoding_primitives
                    // [HIGH]).  The modern detail::extract_frame_arg path already
                    // widens via detail::java_slot_offsets; this mirrors it for the
                    // public frame::get_arguments<...>() accessor.  (Defined inline
                    // because java_slot_offsets/is_java_double_slot_v live further
                    // down the header; the predicate matches is_java_double_slot_v.)
                    constexpr bool wide[]{
                        (std::is_same_v<std::remove_cvref_t<types>, std::int64_t>
                      || std::is_same_v<std::remove_cvref_t<types>, std::uint64_t>
                      || std::is_same_v<std::remove_cvref_t<types>, double>)... };
                    std::array<std::int32_t, sizeof...(types)> slots{};
                    std::int32_t acc{ 0 };
                    for (std::size_t i{ 0 }; i < sizeof...(types); ++i)
                    {
                        slots[i] = acc;
                        acc += wide[i] ? 2 : 1;
                    }
                    return [this, &slots]<std::size_t... I>(std::index_sequence<I...>)
                    {
                        return std::tuple<types...>{ this->get_argument<types>(slots[I])... };
                    }(std::index_sequence_for<types...>{});
                }
            }

            /*
                @brief Retrieves all method arguments by auto-detecting types from the method signature.
                @return A method_args container with one entry per argument.
                @details
                Parses the method descriptor (e.g. "(ILjava/lang/String;)V") to determine
                how many arguments there are and which slots correspond to reference types.
                For reference-type arguments the factory registered via vmhook::register_class<T>()
                is used to construct a std::unique_ptr<T>. Primitive arguments are skipped
                (only reference-type args produce entries).

                Usage:
                    auto args = frame->get_arguments();
                    auto* player = args.as<test_target>(0);  // first ref arg
            */
            auto get_arguments() const noexcept
                -> vmhook::hotspot::method_args
            {
                vmhook::hotspot::method_args result{};

                vmhook::hotspot::method* const current_method{ this->get_method() };
                if (!current_method || !vmhook::hotspot::is_valid_pointer(current_method))
                {
                    return result;
                }

                const std::string descriptor{ current_method->get_signature() };
                if (descriptor.empty())
                {
                    return result;
                }

                // Parse the parameter section: everything between '(' and ')'
                const std::size_t open_paren{ descriptor.find('(') };
                const std::size_t close_paren{ descriptor.find(')') };
                if (open_paren == std::string::npos || close_paren == std::string::npos || close_paren <= open_paren)
                {
                    return result;
                }

                void** const locals{ this->get_locals() };
                if (!locals)
                {
                    return result;
                }

                std::int32_t slot_index{ 0 };
                for (std::size_t pos{ open_paren + 1 }; pos < close_paren; )
                {
                    const char ch{ descriptor[pos] };

                    if (ch == 'L')
                    {
                        // Reference type: find the semicolon end, decode OOP
                        const std::size_t semi{ descriptor.find(';', pos) };
                        if (semi == std::string::npos)
                        {
                            break;
                        }
                        const std::string class_name{ descriptor.substr(pos + 1, semi - pos - 1) };

                        void* raw_value{ locals[-slot_index] };
                        void* decoded{ nullptr };
                        if (raw_value)
                        {
                            const std::uintptr_t raw_bits{ reinterpret_cast<std::uintptr_t>(raw_value) };
                            if (raw_bits <= 0xFFFFFFFFull)
                            {
                                decoded = vmhook::hotspot::decode_oop_pointer(static_cast<std::uint32_t>(raw_bits));
                            }
                            else
                            {
                                decoded = raw_value;
                            }
                        }

                        if (decoded && vmhook::hotspot::is_valid_pointer(decoded))
                        {
                            result.arguments.push_back({ decoded, class_name });
                        }
                        else
                        {
                            result.arguments.push_back({ nullptr, class_name });
                        }

                        pos = semi + 1;
                    }
                    else if (ch == '[')
                    {
                        // Array type: skip array dimensions, then skip the base type
                        ++pos;
                        if (pos < close_paren && descriptor[pos] == 'L')
                        {
                            const std::size_t semi{ descriptor.find(';', pos) };
                            pos = (semi == std::string::npos) ? close_paren : semi + 1;
                        }
                        else
                        {
                            // Primitive array like [I, [Z - one char for base type
                            ++pos;
                        }
                    }
                    else
                    {
                        // Primitive: B, C, D, F, I, J, S, Z
                        ++pos;
                    }

                    // long and double occupy two local slots
                    if (ch == 'J' || ch == 'D')
                    {
                        ++slot_index;
                    }
                    ++slot_index;
                }

                return result;
            }

        private:
            /*
                @brief Retrieves a single method argument at the given index.
                @tparam type The C++ type to interpret the argument as.
                @param index Zero-based index into the local variables array.
                @return The argument value, or a default-constructed value on failure.
                @details
                HotSpot lays out the locals array in reverse order relative to the frame:
                  locals[0]  = argument 0  (this for instance methods)
                  locals[-1] = argument 1
                  locals[-2] = argument 2, ...
                So the slot for argument `index` is at locals[-index].
                - Pointer types: the slot holds a 32-bit compressed OOP (zero-extended to 64 bits
                  by the interpreter). vmhook::hotspot::decode_oop_pointer() reconstructs the real
                  address using narrow_oop_base + (compressed << narrow_oop_shift).
                - Primitive types (sizeof <= 8): the bits are copied verbatim via std::memcpy.
            */
            template<typename argument_type>
            auto get_argument(const std::int32_t index) const noexcept
                -> argument_type
            {
                // Bounds-guard the slot index exactly as return_value::set_arg
                // does (JVM max_locals is a u2).  A negative index makes -index a
                // huge positive offset and any index past max_locals walks off the
                // interpreter local-variable array into adjacent thread state —
                // an out-of-range read returns operand-stack / saved-register
                // garbage that silently corrupts the user's decoded arg rather
                // than failing cleanly (audit hook_arg_decoding_primitives [HIGH]).
                constexpr std::int32_t max_jvm_locals{ 0xFFFF };
                if (index < 0 || index > max_jvm_locals)
                {
                    return argument_type{};
                }

                void** const locals{ this->get_locals() };
                if (!locals)
                {
                    return argument_type{};
                }

                // locals[-index]: arguments are stored in descending slot order.
                void* raw_value{ locals[-index] };

                if constexpr (std::is_pointer_v<argument_type>)
                {
                    if (!raw_value)
                    {
                        return nullptr;
                    }
                    const std::uintptr_t raw_bits{ reinterpret_cast<std::uintptr_t>(raw_value) };

                    // HotSpot can expose object arguments either as:
                    //  - compressed oops (32-bit narrow value in the slot), or
                    //  - direct oop pointers (already decoded 64-bit address).
                    // Prefer decode only for narrow-looking values; otherwise use the
                    // direct pointer as-is.
                    if (raw_bits <= 0xFFFFFFFFull)
                    {
                        return reinterpret_cast<argument_type>(vmhook::hotspot::decode_oop_pointer(static_cast<std::uint32_t>(raw_bits)));
                    }

                    return reinterpret_cast<argument_type>(raw_value);
                }
                else if constexpr (sizeof(argument_type) <= sizeof(void*))
                {
                    // Two-word primitives (long/double, 8 bytes) span local slots
                    // [index] and [index+1].  HotSpot's interpreter stores the
                    // 64-bit value at the LOWER address — locals[-(index+1)] — per
                    // LOCALS_LONG / LOCALS_DOUBLE in bytecodeInterpreter.hpp.
                    // One-word primitives (int/float/short/char/byte/boolean, ≤4
                    // bytes) live directly at locals[-index] (== raw_value).
                    // Reading locals[-index] for a long/double picks up the wrong
                    // slot and yields a garbage / halves-swapped value.
                    void* slot_value{ raw_value };
                    if constexpr (sizeof(argument_type) > sizeof(std::int32_t))
                    {
                        slot_value = locals[-(index + 1)];
                    }
                    argument_type result{};
                    std::memcpy(&result, &slot_value, sizeof(argument_type));
                    return result;
                }
                else
                {
                    return argument_type{};
                }
            }
        };

        /*
            @brief Installs a low-level hook on a HotSpot interpreter-to-interpreter (i2i) stub.
            @details
            midi2i_hook patches the i2i interpreter stub of a Java method at the injection
            point found by find_hook_location() with a 5-byte relative JMP instruction that
            redirects execution to an allocated trampoline stub. The trampoline saves all
            volatile registers, calls common_detour with (frame*, java_thread*, return_slot*), and
            then either returns the custom value or resumes normal execution depending on
            whether the detour set the cancel flag.

            The trampoline stub is allocated within 32-bit relative JMP range of the target.
            The stub layout is: [original 8 bytes] [assembly trampoline].
            Only one trampoline is allocated per unique i2i entry point, even if multiple
            methods share the same stub.
        */
        class midi2i_hook final
        {
        public:
            /*
                @brief Installs the hook on the given target address.
                @param target         Pointer to the injection point within the i2i stub.
                @param detour         The C++ function to call when the hook fires.
                @param chain_resume   Optional address to jump to after our detour returns
                                      "don't cancel".  Defaults to `target + HOOK_SIZE`
                                      (resume the original i2i past the patched window),
                                      but the install site can pass another DLL's trampoline
                                      entry here to chain hooks: our detour runs, then we
                                      jmp into their trampoline, which runs their detour
                                      and itself resumes at `target + HOOK_SIZE`.  Both
                                      hooks fire, no cross-DLL conflict.
            */
            midi2i_hook(std::uint8_t* const target,
                        const vmhook::hotspot::detour_function_t detour,
                        const void* const chain_resume = nullptr)
                : target{ target }
                , allocated{ nullptr }
                , allocated_size{ 0 }
                // Defence in depth: vmhook::hook<T>() already filters
                // chain_resume through is_valid_pointer, but the constructor
                // is the only place that bakes the address into the
                // trampoline's resume JMP.  Re-validate so that a direct
                // caller (anyone using midi2i_hook outside the hook<T>
                // gateway) cannot trick the trampoline into resuming at an
                // arbitrary address.  Treat anything that fails the check
                // as nullptr, which makes the trampoline fall through to
                // its default `target + HOOK_SIZE` resume.
                , current_chain_resume{ (chain_resume && vmhook::hotspot::is_valid_pointer(chain_resume))
                                             ? const_cast<void*>(chain_resume)
                                             : nullptr }
                , error{ true }
            {
#if !VMHOOK_RUNTIME_HOOKING_AVAILABLE
                // The trampoline emits Windows/SysV x64 bytes and depends on
                // the HotSpot interpreter frame layout for that ABI.  arm64
                // and iOS (no JIT, no HotSpot) cannot use this path; leave
                // the hook in its error state so callers see a clean false.
                (void)target;
                (void)detour;
                return;
#else
                static constexpr std::int32_t HOOK_SIZE{ 8 };
                static constexpr std::int32_t JMP_SIZE{ 5 };
                static constexpr std::uint8_t JMP_OPCODE{ 0xE9 };

#if VMHOOK_OS_WINDOWS
                // ------------------------------------------------------------
                // Microsoft x64 calling convention trampoline.
                // Args: rcx, rdx, r8, r9.  Shadow space: 32 bytes.
                // Caller-saved: rax, rcx, rdx, r8, r9, r10, r11.
                // ------------------------------------------------------------
                static constexpr std::int32_t JE_OFFSET{ 0x32 };   // offset of je in assembly
                static constexpr std::int32_t JE_SIZE{ 6 };
                static constexpr std::int32_t RESUME_OFFSET{ 0x63 };
                static constexpr std::int32_t RESUME_JMP_OFFSET{ 0x73 };
                static constexpr std::int32_t RESUME_JMP_SIZE{ 5 };
                static constexpr std::int32_t DETOUR_ADDRESS_OFFSET{ 0x78 };

                // Stack layout after the two pushes:
                //   [rsp+0]  return_slot::cancel  (bool, 1 byte; rest zeroed)
                //   [rsp+8]  return_slot::retval  (int64_t)
                std::uint8_t assembly[]
                {
                    0x50,                                           // push rax
                    0x51,                                           // push rcx
                    0x52,                                           // push rdx
                    0x41, 0x50,                                     // push r8
                    0x41, 0x51,                                     // push r9
                    0x41, 0x52,                                     // push r10
                    0x41, 0x53,                                     // push r11
                    0x55,                                           // push rbp
                    0x6A, 0x00,                                     // push 0x0  ; return_slot::retval  (slot +8)
                    0x6A, 0x00,                                     // push 0x0  ; return_slot::cancel  (slot +0)

                    0x48, 0x89, 0xE9,                               // mov rcx, rbp   ; frame*
                    0x4C, 0x89, 0xFA,                               // mov rdx, r15   ; java_thread*
                    0x4C, 0x8D, 0x04, 0x24,                         // lea r8, [rsp]  ; return_slot*

                    0x48, 0x89, 0xE5,                               // mov rbp, rsp
                    0x48, 0x83, 0xE4, 0xF0,                         // and rsp, -16
                    0x48, 0x83, 0xEC, 0x20,                         // sub rsp, 0x20

                    0xFF, 0x15, 0x4D, 0x00, 0x00, 0x00,             // call [rip+0x4D]

                    0x48, 0x89, 0xEC,                               // mov rsp, rbp

                    0x80, 0x3C, 0x24, 0x00,                         // cmp byte ptr [rsp], 0
                    0x0F, 0x84, 0x00, 0x00, 0x00, 0x00,             // je resume  ; cancel==false

                    // cancel path (cancel==true, falls through):
                    0x48, 0x8B, 0x44, 0x24, 0x08,                   // mov rax, [rsp+8]    ; return_slot::retval
                    0x66, 0x48, 0x0F, 0x6E, 0xC0,                   // movq xmm0, rax      ; float/double return
                    0x48, 0x83, 0xC4, 0x10,                         // add rsp, 0x10       ; discard return_slot
                    0x5D,                                           // pop rbp
                    0x41, 0x5B,                                     // pop r11
                    0x41, 0x5A,                                     // pop r10
                    0x41, 0x59,                                     // pop r9
                    0x41, 0x58,                                     // pop r8
                    0x5A,                                           // pop rdx
                    0x59,                                           // pop rcx
                    0x48, 0x83, 0xC4, 0x08,                         // add rsp, 0x8        ; discard saved original rax
                    0x48, 0x8B, 0x5D, 0xF8,                         // mov rbx, [rbp-8]
                    0x48, 0x89, 0xEC,                               // mov rsp, rbp
                    0x5D,                                           // pop rbp
                    0x5E,                                           // pop rsi
                    0x48, 0x89, 0xDC,                               // mov rsp, rbx
                    0xFF, 0xE6,                                     // jmp rsi

                    // resume path (cancel==false):
                    0x48, 0x83, 0xC4, 0x10,                         // add rsp, 0x10       ; discard return_slot
                    0x5D,                                           // pop rbp
                    0x41, 0x5B,                                     // pop r11
                    0x41, 0x5A,                                     // pop r10
                    0x41, 0x59,                                     // pop r9
                    0x41, 0x58,                                     // pop r8
                    0x5A,                                           // pop rdx
                    0x59,                                           // pop rcx
                    0x58,                                           // pop rax
                    0xE9, 0x00, 0x00, 0x00, 0x00,                   // jmp target+HOOK_SIZE

                    // data slot: detour function pointer
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
                };
#else
                // ------------------------------------------------------------
                // System V AMD64 calling convention trampoline.
                // Args: rdi, rsi, rdx, rcx, r8, r9.  No shadow space.
                // Caller-saved: rax, rcx, rdx, rsi, rdi, r8, r9, r10, r11.
                //
                // Byte layout (the offsets below are runtime constants; the
                // values match the cumulative widths of the instructions
                // above each landmark):
                //
                //   off 0   push rax/rdi/rsi/rdx/rcx/r8/r9/r10/r11/rbp  (14)
                //   off 14  push 0, push 0                              (4)
                //   off 18  mov rdi,rbp ; mov rsi,r15 ; mov rdx,rsp     (9)
                //   off 27  mov rbp,rsp ; and rsp,-16                   (7)
                //   off 34  call [rip+disp32]                           (6)
                //   off 40  mov rsp,rbp                                 (3)
                //   off 43  cmp byte [rsp],0                            (4)
                //   off 47  je rel32                                    (6)  <- JE_OFFSET
                //   off 53  cancel path                                 (45)
                //   off 98  resume path                                 (23) <- RESUME_OFFSET
                //   off 116 jmp rel32 (inside resume path)              (5)  <- RESUME_JMP_OFFSET
                //   off 121 detour-function-pointer slot                (8)  <- DETOUR_ADDRESS_OFFSET
                //   off 129 end
                //
                // The 6-byte call uses disp32 = 121 − 40 = 81 = 0x51 so
                // [rip+0x51] dereferences the detour function pointer.
                // ------------------------------------------------------------
                static constexpr std::int32_t JE_OFFSET{ 0x2F };
                static constexpr std::int32_t JE_SIZE{ 6 };
                static constexpr std::int32_t RESUME_OFFSET{ 0x62 };
                static constexpr std::int32_t RESUME_JMP_OFFSET{ 0x74 };
                static constexpr std::int32_t RESUME_JMP_SIZE{ 5 };
                static constexpr std::int32_t DETOUR_ADDRESS_OFFSET{ 0x79 };

                std::uint8_t assembly[]
                {
                    0x50,                                           // push rax
                    0x57,                                           // push rdi
                    0x56,                                           // push rsi
                    0x52,                                           // push rdx
                    0x51,                                           // push rcx
                    0x41, 0x50,                                     // push r8
                    0x41, 0x51,                                     // push r9
                    0x41, 0x52,                                     // push r10
                    0x41, 0x53,                                     // push r11
                    0x55,                                           // push rbp
                    0x6A, 0x00,                                     // push 0  ; retval
                    0x6A, 0x00,                                     // push 0  ; cancel

                    0x48, 0x89, 0xEF,                               // mov rdi, rbp   ; arg1 frame*
                    0x4C, 0x89, 0xFE,                               // mov rsi, r15   ; arg2 java_thread*
                    0x48, 0x89, 0xE2,                               // mov rdx, rsp   ; arg3 return_slot*

                    0x48, 0x89, 0xE5,                               // mov rbp, rsp
                    0x48, 0x83, 0xE4, 0xF0,                         // and rsp, -16

                    0xFF, 0x15, 0x51, 0x00, 0x00, 0x00,             // call [rip+0x51]

                    0x48, 0x89, 0xEC,                               // mov rsp, rbp

                    0x80, 0x3C, 0x24, 0x00,                         // cmp byte ptr [rsp], 0
                    0x0F, 0x84, 0x00, 0x00, 0x00, 0x00,             // je resume  (offset filled below)

                    // cancel path (offset 0x35..0x61, 45 bytes):
                    0x48, 0x8B, 0x44, 0x24, 0x08,                   // mov rax, [rsp+8]
                    0x66, 0x48, 0x0F, 0x6E, 0xC0,                   // movq xmm0, rax
                    0x48, 0x83, 0xC4, 0x10,                         // add rsp, 0x10
                    0x5D,                                           // pop rbp
                    0x41, 0x5B,                                     // pop r11
                    0x41, 0x5A,                                     // pop r10
                    0x41, 0x59,                                     // pop r9
                    0x41, 0x58,                                     // pop r8
                    0x59,                                           // pop rcx
                    0x5A,                                           // pop rdx
                    0x5E,                                           // pop rsi
                    0x5F,                                           // pop rdi
                    0x48, 0x83, 0xC4, 0x08,                         // add rsp, 0x8 ; discard saved rax
                    0x48, 0x8B, 0x5D, 0xF8,                         // mov rbx, [rbp-8]
                    0x48, 0x89, 0xEC,                               // mov rsp, rbp
                    0x5D,                                           // pop rbp
                    0x5E,                                           // pop rsi
                    0x48, 0x89, 0xDC,                               // mov rsp, rbx
                    0xFF, 0xE6,                                     // jmp rsi

                    // resume path (offset 0x62..0x78, 23 bytes):
                    0x48, 0x83, 0xC4, 0x10,                         // add rsp, 0x10
                    0x5D,                                           // pop rbp
                    0x41, 0x5B,                                     // pop r11
                    0x41, 0x5A,                                     // pop r10
                    0x41, 0x59,                                     // pop r9
                    0x41, 0x58,                                     // pop r8
                    0x59,                                           // pop rcx
                    0x5A,                                           // pop rdx
                    0x5E,                                           // pop rsi
                    0x5F,                                           // pop rdi
                    0x58,                                           // pop rax
                    0xE9, 0x00, 0x00, 0x00, 0x00,                   // jmp target+HOOK_SIZE

                    // data slot (offset 0x79..0x80):
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
                };
#endif

                const std::size_t total_size{ static_cast<std::size_t>(HOOK_SIZE) + sizeof(assembly) };
                this->allocated = vmhook::hotspot::allocate_nearby_memory(target, total_size);
                this->allocated_size = total_size;

                try
                {
                    if (!this->allocated)
                    {
                        throw vmhook::exception{ "Failed to allocate memory for hook." };
                    }
                }
                catch (const std::exception& exception)
                {
                    VMHOOK_LOG("{} midi2i_hook::midi2i_hook {}", vmhook::error_tag, exception.what());
                    return;
                }

                const std::int32_t je_delta{ RESUME_OFFSET - (JE_OFFSET + JE_SIZE) };
                *reinterpret_cast<std::int32_t*>(assembly + JE_OFFSET + 2) = je_delta;

                // Where to resume after the detour returns "don't cancel".
                // Default: `target + HOOK_SIZE` (continue past the patched window
                // in the same stub).  When chain_resume is non-null we point at
                // another hooker's trampoline instead, so vmhook coexists with
                // any prior i2i-patching DLL: we run our detour, then JMP into
                // their trampoline, which runs its detour and resumes at the
                // original `target + HOOK_SIZE` from its own copy of the saved
                // bytes.  Both hooks fire, neither overwrites the other.
                //
                // Use the constructor-validated this->current_chain_resume here,
                // not the raw `chain_resume` parameter: the former is null when
                // the input pointer failed is_valid_pointer, so a bad parameter
                // gracefully falls through to the default resume instead of
                // baking an arbitrary address into the trampoline.
                const std::uint8_t* const effective_resume{
                    this->current_chain_resume
                        ? static_cast<const std::uint8_t*>(this->current_chain_resume)
                        : target + HOOK_SIZE
                };

                const std::int32_t resume_jmp_delta{ static_cast<std::int32_t>(
                    effective_resume - (this->allocated + HOOK_SIZE + RESUME_JMP_OFFSET + RESUME_JMP_SIZE)) };
                *reinterpret_cast<std::int32_t*>(assembly + RESUME_JMP_OFFSET + 1) = resume_jmp_delta;

                *reinterpret_cast<vmhook::hotspot::detour_function_t*>(assembly + DETOUR_ADDRESS_OFFSET) = detour;

                std::memcpy(this->allocated, target, HOOK_SIZE);
                std::memcpy(this->allocated + HOOK_SIZE, assembly, sizeof(assembly));

                std::uint32_t old_protect{};
                vmhook::os::protect(this->allocated, total_size,
                                    vmhook::os::memory_protection::execute_read, &old_protect);
                vmhook::os::protect(target, JMP_SIZE,
                                    vmhook::os::memory_protection::execute_rw, &old_protect);

                target[0] = JMP_OPCODE;
                const std::int32_t jmp_delta{ static_cast<std::int32_t>(this->allocated - (target + JMP_SIZE)) };
                *reinterpret_cast<std::int32_t*>(target + 1) = jmp_delta;

                // Restore the target page's original protection.  We don't have a portable
                // way to spell the original native flags, so we apply execute_read which
                // matches the JVM's normal state for generated code.
                vmhook::os::protect(target, JMP_SIZE,
                                    vmhook::os::memory_protection::execute_read, &old_protect);
                vmhook::os::flush_instruction_cache(target, JMP_SIZE);
                vmhook::os::flush_instruction_cache(this->allocated, total_size);

                this->error = false;
#endif  // VMHOOK_RUNTIME_HOOKING_AVAILABLE
            }

            /*
                @brief Removes the hook and restores the original code at the target.
            */
            ~midi2i_hook()
            {
                if (this->error)
                {
                    return;
                }

                static constexpr std::uint8_t JMP_OPCODE{ 0xE9 };

                std::uint32_t old_protect{};
                if (this->target[0] == JMP_OPCODE
                    && vmhook::os::protect(this->target, 5,
                                           vmhook::os::memory_protection::execute_rw, &old_protect))
                {
                    std::memcpy(this->target, this->allocated, 5);
                    vmhook::os::protect(this->target, 5,
                                        vmhook::os::memory_protection::execute_read, &old_protect);
                    vmhook::os::flush_instruction_cache(this->target, 5);
                }

                vmhook::os::release(this->allocated, this->allocated_size);
            }

            inline auto has_error() const noexcept -> bool
            {
                return this->error;
            }

            /*
                @brief Re-applies our 5-byte JMP at the injection point.  If the
                       bytes there look like a JMP into another DLL's trampoline
                       (overwritten since install), re-do chain detection and
                       update our trampoline's resume target to chain to it.
                @details
                Called by vmhook::verify_hooks() to recover from another hooker
                stomping our patch in the shared HotSpot i2i stub.  Returns
                true if the patch was already intact, false if we had to
                re-write it.  Logs every re-write.

                Race: re-writing 5 bytes while a Java thread is mid-instruction
                at those bytes can in theory tear an instruction.  In practice
                byte 0 is always 0xE9 (JMP opcode) both before and after, and
                the rel32 (bytes 1-4) is replaced by a memcpy.  The CPU never
                sees a non-JMP opcode, only a possibly-wrong rel32 destination
                for the few-nanosecond window during the write.  Tolerable
                given the alternative (giving up scenario-1 recovery entirely).
            */
            auto verify_and_repair() noexcept -> bool
            {
                if (this->error || !this->target || !this->allocated)
                {
                    return true;
                }

                static constexpr std::uint8_t JMP_OPCODE{ 0xE9 };
                static constexpr std::int32_t JMP_SIZE{ 5 };

                // Expected: 0xE9 + rel32 to our trampoline (this->allocated).
                const std::int32_t expected_rel{ static_cast<std::int32_t>(
                    this->allocated - (this->target + JMP_SIZE)) };
                std::uint8_t expected[JMP_SIZE]{};
                expected[0] = JMP_OPCODE;
                std::memcpy(expected + 1, &expected_rel, sizeof(expected_rel));

                if (std::memcmp(this->target, expected, JMP_SIZE) == 0)
                {
                    return true;  // hook intact, nothing to do
                }

                // Patch is gone.  If a different JMP is there, follow it -
                // someone else hooked the stub after us; chain to their
                // trampoline so both hooks fire after we re-install.
                void* new_chain{ this->current_chain_resume };
                if (this->target[0] == JMP_OPCODE)
                {
                    std::int32_t rel{};
                    std::memcpy(&rel, this->target + 1, sizeof(rel));
                    std::uint8_t* const prior_trampoline{ this->target + JMP_SIZE + rel };
                    if (prior_trampoline != this->allocated  // not pointing at ourselves
                        && vmhook::hotspot::is_valid_pointer(prior_trampoline))
                    {
                        new_chain = prior_trampoline;
                    }
                }

                // If the chain target changed, update our trampoline's resume
                // JMP delta so it lands on the new chain target after our
                // detour returns "don't cancel".
                if (new_chain != this->current_chain_resume)
                {
                    this->rewrite_chain_resume(new_chain);
                    this->current_chain_resume = new_chain;
                }

                // Re-write the 5-byte JMP at the injection point.
                std::uint32_t old_protect{};
                if (!vmhook::os::protect(this->target, JMP_SIZE,
                                         vmhook::os::memory_protection::execute_rw, &old_protect))
                {
                    return false;
                }
                std::memcpy(this->target, expected, JMP_SIZE);
                vmhook::os::protect(this->target, JMP_SIZE,
                                    vmhook::os::memory_protection::execute_read, &old_protect);
                vmhook::os::flush_instruction_cache(this->target, JMP_SIZE);

                VMHOOK_LOG("{} midi2i_hook::verify_and_repair: re-applied JMP at 0x{:016X} -> 0x{:016X}, "
                           "chain resume = 0x{:016X}.",
                           vmhook::info_tag,
                           reinterpret_cast<std::uintptr_t>(this->target),
                           reinterpret_cast<std::uintptr_t>(this->allocated),
                           reinterpret_cast<std::uintptr_t>(this->current_chain_resume));

                return false;
            }

        private:
            // Updates the rel32 inside the resume-JMP instruction at the end
            // of our trampoline so it points at new_target instead of the
            // previously-cached chain_resume (or target + HOOK_SIZE).  Only
            // called by verify_and_repair() when another hooker has changed
            // the chain since install.
            //
            // The offsets here MUST stay in sync with the assembly array in
            // the ctor.  We keep two sets of constants - one per ABI - so
            // both build paths get the right values.
            auto rewrite_chain_resume(void* const new_target) noexcept -> void
            {
#if !VMHOOK_RUNTIME_HOOKING_AVAILABLE
                (void)new_target;
                return;
#else
                static constexpr std::int32_t HOOK_SIZE{ 8 };
#if VMHOOK_OS_WINDOWS
                static constexpr std::int32_t RESUME_JMP_OFFSET{ 0x73 };
                static constexpr std::int32_t RESUME_JMP_SIZE{ 5 };
#else
                static constexpr std::int32_t RESUME_JMP_OFFSET{ 0x74 };
                static constexpr std::int32_t RESUME_JMP_SIZE{ 5 };
#endif
                std::uint8_t* const resume_rel_addr{
                    this->allocated + HOOK_SIZE + RESUME_JMP_OFFSET + 1 };
                const std::uint8_t* const next_instruction_addr{
                    this->allocated + HOOK_SIZE + RESUME_JMP_OFFSET + RESUME_JMP_SIZE };
                const std::int32_t new_rel{ static_cast<std::int32_t>(
                    static_cast<const std::uint8_t*>(new_target) - next_instruction_addr) };

                std::uint32_t old_protect{};
                if (!vmhook::os::protect(this->allocated, this->allocated_size,
                                         vmhook::os::memory_protection::execute_rw, &old_protect))
                {
                    return;
                }
                std::memcpy(resume_rel_addr, &new_rel, sizeof(new_rel));
                vmhook::os::protect(this->allocated, this->allocated_size,
                                    vmhook::os::memory_protection::execute_read, &old_protect);
                vmhook::os::flush_instruction_cache(resume_rel_addr, sizeof(new_rel));
#endif
            }

        private:
            std::uint8_t* target{ nullptr };
            std::uint8_t* allocated{ nullptr };
            std::size_t   allocated_size{ 0 };
            void*         current_chain_resume{ nullptr };
            bool          error{ true };
        };

        /*
            @brief Stores the association between a HotSpot Method object and its C++ detour function.
            @details  Also holds the original entry points and _code pointer so shutdown_hooks()
                      can fully restore the method's state, including re-linking the nmethod if the
                      method was JIT-compiled when the hook was installed.
        */
        struct hooked_method
        {
            vmhook::hotspot::method* method{ nullptr };
            std::function<void(vmhook::hotspot::frame*, vmhook::hotspot::java_thread*, vmhook::hotspot::return_slot*)> detour;
            void* original_code{ nullptr };
            void* original_from_interpreted_entry{ nullptr };
            void* original_from_compiled_entry{ nullptr };
            bool  was_compiled{ false };

            // Captured at install time so verify_hooks can detect Method*
            // identity drift caused by class redefinition / unload.  When
            // the JVM frees the original Method (because EntityRenderer or
            // World got redefined by another DLL via JVMTI RedefineClasses)
            // the allocator may hand the same address back out for an
            // unrelated method - our stored `method` pointer then points
            // at e.g. "bridge$getMainHandItemRenderState" instead of
            // "orientCamera", and common_detour stops matching the new
            // (live) orientCamera Method*.  Comparing method->get_name()
            // against expected_name surfaces the drift; comparing
            // method->get_const_method() against null surfaces the harder
            // case where the Method was freed and the slot is now garbage.
            std::string expected_class_name{};
            std::string expected_method_name{};
            std::string expected_signature{};

            // Set the first time verify_hooks() notices any drift, so
            // subsequent passes don't re-log the same warning every tick.
            bool  drift_logged{ false };
        };

        /*
            @brief Stores the association between an i2i entry point and its installed midi2i_hook.
            @details
            Since multiple Java methods can share the same i2i stub, only one trampoline
            is allocated per unique i2i entry point.
        */
        struct i2i_hook_data
        {
            void* i2i_entry{ nullptr };
            vmhook::hotspot::midi2i_hook* hook{ nullptr };
        };

        /*
            @brief Global list of all currently hooked Java methods and their detour functions.

            Thread-safety: every mutation (vmhook::hook(), shutdown_hooks(),
            hook_handle::stop()) must hold g_hooked_methods_mutex.  common_detour
            iterates the vector on every hooked-method invocation, but iteration
            is done WITHOUT acquiring the lock - it relies on the contract that
            the vector is only ever mutated from the user's setup thread BEFORE
            hook detours fire.  A reallocation mid-iteration would otherwise
            invalidate the iterator and use-after-free in the detour pointer
            cell.  install / uninstall sites pay the lock; the hot detour path
            stays lock-free.

            If you need to install/remove hooks WHILE detours are firing,
            either pre-reserve the vector to a known cap or copy the iteration
            snapshot inside common_detour - see commit log for the analysis.
        */
        inline std::vector<vmhook::hotspot::hooked_method> g_hooked_methods{};
        inline std::mutex g_hooked_methods_mutex{};

        /*
            @brief Set by shutdown_hooks() before it starts unpatching, so any
                   detour fire that races shutdown returns immediately.
            @details
            Without this flag, the sequence
              1. Java thread enters patched i2i, jumps into trampoline
              2. Trampoline calls common_detour (lock-free read of
                 g_hooked_methods)
              3. shutdown_hooks() acquires g_hooked_methods_mutex,
                 deletes trampolines, restores entries, clears the vector
              4. common_detour iterates a vector that's mid-clear
            can use-after-free in the detour cell.  Flipping the flag at the
            top of shutdown_hooks() — before taking the mutex — makes step 2
            return early and keeps the detour off the doomed vector.
        */
        inline std::atomic<bool> g_shutdown_requested{ false };

        /*
            @brief SEH wrapper around a single user detour invocation.
            @details
            Hooks run against live JVM state — the detour might receive a
            stale OOP (page unmapped after GC + class unload), null receiver,
            or hit some other access violation chasing a half-initialised
            wrapper.  Without this wrapper, an SEH exception (0xC0000005)
            tears the JVM down because vmhook's surrounding try/catch only
            catches C++ exceptions.

            __try / __except must live in a function with NO automatic
            objects that need C++ unwinding, which is why this is split
            out from common_detour.  Returns true on clean completion,
            false if SEH fired — caller logs and falls through to the
            original method body.
        */
        inline auto seh_invoke_detour(const std::function<void(vmhook::hotspot::frame*, vmhook::hotspot::java_thread*, vmhook::hotspot::return_slot*)>& detour_fn,
                                      vmhook::hotspot::frame* const frame_pointer,
                                      vmhook::hotspot::java_thread* const thread,
                                      vmhook::hotspot::return_slot* const slot) noexcept
            -> bool
        {
#if defined(_MSC_VER) && !defined(__clang__)
            __try
            {
                detour_fn(frame_pointer, thread, slot);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
#else
            try
            {
                detour_fn(frame_pointer, thread, slot);
                return true;
            }
            catch (...)
            {
                return false;
            }
#endif
        }

        /*
            @brief Global list of all i2i entry points that have been patched and their hooks.

            Shares g_hooked_methods_mutex - the two vectors are always mutated
            together (install / uninstall both touch the matching entries).
        */
        inline std::vector<vmhook::hotspot::i2i_hook_data> g_hooked_i2i_entries{};

        /*
            @brief Common detour function invoked by the trampoline stub for every intercepted method call.
            @param f      Pointer to the current HotSpot interpreter frame (rbp at hook site).
            @param thread Pointer to the current HotSpot JavaThread (r15).
            @param slot Pointer to the return slot on the trampoline stack.
            @details
            Single entry point for all midi2i_hook trampolines.  Finds the matching
            per-method detour in g_hooked_methods and dispatches it.

            The thread-state precondition check was removed because the injection point
            (the `mov BYTE PTR [r15+X], Y` instruction) is reached while the thread may
            still be in a transition state on JDK 21+ builds - the state is not
            necessarily _thread_in_Java yet at that exact instruction.  After the user
            detour returns we force the state to _thread_in_Java so the bytecode
            dispatch that follows finds the correct state.
        */
        static auto common_detour(vmhook::hotspot::frame* const frame_pointer, vmhook::hotspot::java_thread* const thread, vmhook::hotspot::return_slot* const slot)
            -> void
        {
            // Race-free shutdown: shutdown_hooks() flips this BEFORE acquiring
            // the install mutex and clearing g_hooked_methods, so any detour
            // that fires concurrently with teardown sees the flag and bails
            // before iterating the about-to-be-cleared vector.
            if (vmhook::hotspot::g_shutdown_requested.load(std::memory_order_acquire))
            {
                return;
            }

            try
            {
                if (!thread || !vmhook::hotspot::is_valid_pointer(thread))
                {
                    throw vmhook::exception{ "JavaThread pointer is null or invalid." };
                }

                const method* const current_method{ frame_pointer->get_method() };
                struct current_thread_guard
                {
                    vmhook::hotspot::java_thread* previous;

                    explicit current_thread_guard(vmhook::hotspot::java_thread* const thread_pointer) noexcept
                        : previous{ vmhook::hotspot::current_java_thread }
                    {
                        vmhook::hotspot::current_java_thread = thread_pointer;
                        vmhook::hotspot::last_java_thread.store(thread_pointer, std::memory_order_relaxed);
                    }

                    ~current_thread_guard()
                    {
                        vmhook::hotspot::current_java_thread = this->previous;
                    }
                } guard{ thread };

                for (const vmhook::hotspot::hooked_method& hook : vmhook::hotspot::g_hooked_methods)
                {
                    if (hook.method == current_method)
                    {
                        // SEH guard: a stale OOP / null-receiver deref inside
                        // the user detour throws an SEH access violation, not
                        // a C++ exception - our outer try/catch wouldn't catch
                        // it and the JVM would tear down.  seh_invoke_detour
                        // converts the AV to a false return so we log and
                        // fall through to the original Java method body.
                        if (!vmhook::hotspot::seh_invoke_detour(hook.detour, frame_pointer, thread, slot))
                        {
                            VMHOOK_LOG("{} common_detour(): detour for method 0x{:016X} raised "
                                       "SEH/AV - skipping this invocation, original method body "
                                       "will run.",
                                       vmhook::warning_tag,
                                       reinterpret_cast<std::uintptr_t>(hook.method));
                        }
                        // Ensure the thread state is _thread_in_Java after the detour
                        // so the bytecode dispatcher finds a consistent state.
                        thread->set_thread_state(vmhook::hotspot::java_thread_state::_thread_in_Java);
                        return;
                    }
                }
            }
            catch (const std::exception& exception)
            {
                VMHOOK_LOG("{} common_detour() {}", vmhook::error_tag, exception.what());
            }
        }

        /*
            @brief Bitmask of HotSpot access flags used to disable JIT compilation on hooked methods.
            @details
            OR'd into Method._access_flags to prevent C1, C2, and OSR compilation.
            - JVM_ACC_NOT_C2_COMPILABLE   (0x02000000)
            - JVM_ACC_NOT_C1_COMPILABLE   (0x04000000)
            - JVM_ACC_NOT_C2_OSR_COMPILABLE (0x08000000)
            - JVM_ACC_QUEUED              (0x01000000)
        */
        inline constexpr std::int32_t NO_COMPILE =
            0x02000000 |
            0x04000000 |
            0x08000000 |
            0x01000000;

        /*
            @brief Enables or disables the _dont_inline flag on a HotSpot Method object.
            @details
            The _dont_inline flag is stored in Method._flags at bit position 2.
            When set, it prevents the JIT from inlining this method at any call site.
        */
        static auto set_dont_inline(const vmhook::hotspot::method* const method_pointer, const bool enabled) noexcept
            -> void
        {
            std::uint16_t* const flags{ method_pointer->get_flags() };
            if (!flags)
            {
                return;
            }

            if (enabled)
            {
                *flags |= (1 << 2);
            }
            else
            {
                *flags &= static_cast<std::uint16_t>(~(1 << 2));
            }
        }

        /*
            @brief Reads the c2i (compiled-to-interpreter) adapter entry from an AdapterHandlerEntry.
            @param adapter  AdapterHandlerEntry* stored in Method._adapter.
            @return The c2i entry address, or nullptr if not available.
            @details
            Used when deoptimising a hook to redirect Method._from_compiled_entry to the
            c2i adapter, so compiled callers that miss their inline cache re-enter the
            interpreter and reach our patched i2i stub.
            AdapterHandlerEntry._c2i_entry is exported via gHotSpotVMStructs on all
            supported JDK versions (8 through 26).
        */
        static auto get_c2i_entry_from_adapter(void* const adapter) noexcept
            -> void*
        {
            if (!adapter || !vmhook::hotspot::is_valid_pointer(adapter))
            {
                return nullptr;
            }
            static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("AdapterHandlerEntry", "_c2i_entry") };
            if (!entry)
            {
                return nullptr;
            }

            return *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(adapter) + entry->offset);
        }

        /*
            @brief Validates that a candidate pointer looks like an AdapterHandlerEntry.
            @details
            Reads two slots inside the candidate at offsets known from
            VMStructs (_i2c_entry at offset 0 of every AHE layout we've
            seen, plus _c2i_entry at the exported offset) and confirms
            both point into executable memory.  Real AHE instances always
            satisfy this because their entries live in HotSpot's code
            cache, while random non-AHE pointers fail it with very high
            probability (a stray pointer that points at three consecutive
            executable-region pointers is astronomically unlikely).

            @param candidate            Pointer treated as a possible AdapterHandlerEntry*.
            @param c2i_offset_in_ahe    Byte offset of _c2i_entry within the AHE.
        */
        inline auto validate_adapter_handler_entry(void* const candidate, const std::size_t c2i_offset_in_ahe) noexcept
            -> bool
        {
            if (!candidate || !vmhook::hotspot::is_readable_pointer(candidate))
            {
                return false;
            }
            // _i2c_entry lives at offset 0 of AdapterHandlerEntry across every
            // JDK we support (the field is the first non-inherited member).
            void* i2c{};
            std::memcpy(&i2c, candidate, sizeof(i2c));
            if (!i2c || !vmhook::hotspot::is_readable_pointer(i2c))
            {
                return false;
            }
            void* c2i{};
            std::memcpy(&c2i, reinterpret_cast<const std::uint8_t*>(candidate) + c2i_offset_in_ahe, sizeof(c2i));
            if (!c2i || !vmhook::hotspot::is_readable_pointer(c2i))
            {
                return false;
            }
            // Both entries must point into executable memory (the code
            // cache).  query_region returns the page protection flags;
            // we require X to be set on both pages.
            const vmhook::os::region_info i2c_region{ vmhook::os::query_region(i2c) };
            const vmhook::os::region_info c2i_region{ vmhook::os::query_region(c2i) };
            return i2c_region.committed && i2c_region.executable
                && c2i_region.committed && c2i_region.executable;
        }

        /*
            @brief Out-of-line definition of detect_adapter_offset_from_method.
            @details
            Tries the slot immediately preceding _from_compiled_entry first
            (the layout that HotSpot has used since JDK 17 keeps _adapter
            adjacent to _from_compiled_entry).  If that doesn't validate,
            falls back to a full scan over Method bytes, skipping the slots
            corresponding to other known Method fields so we don't
            mistakenly pick e.g. _constMethod or _code.
        */
        inline auto detect_adapter_offset_from_method(method* const probe) noexcept
            -> std::size_t
        {
            if (!probe || !vmhook::hotspot::is_valid_pointer(probe))
            {
                return 0;
            }
            static const vmhook::hotspot::vm_struct_entry_t* const c2i_field{
                vmhook::hotspot::iterate_struct_entries("AdapterHandlerEntry", "_c2i_entry") };
            if (!c2i_field)
            {
                return 0;  // Cannot validate without _c2i_entry offset.
            }
            const std::size_t c2i_offset{ static_cast<std::size_t>(c2i_field->offset) };

            std::uint8_t* const probe_bytes{ reinterpret_cast<std::uint8_t*>(probe) };

            auto try_offset = [&](const std::size_t offset) noexcept -> bool
                {
                    void* candidate{};
                    std::memcpy(&candidate, probe_bytes + offset, sizeof(candidate));
                    return validate_adapter_handler_entry(candidate, c2i_offset);
                };

            // Build the skip set of well-known Method field offsets so the
            // scan doesn't dereference _constMethod / _code / etc. as if
            // they were adapter pointers.
            std::array<std::size_t, 8> skip_offsets{};
            std::size_t skip_count{ 0 };
            auto add_skip = [&](const char* const field_name) noexcept
                {
                    if (skip_count >= skip_offsets.size()) return;
                    if (const auto* const entry{ vmhook::hotspot::iterate_struct_entries("Method", field_name) })
                    {
                        skip_offsets[skip_count++] = static_cast<std::size_t>(entry->offset);
                    }
                };
            add_skip("_constMethod");
            add_skip("_method_data");
            add_skip("_method_counters");
            add_skip("_code");
            add_skip("_i2i_entry");
            add_skip("_from_interpreted_entry");

            // Preferred guess: the slot just before _from_compiled_entry
            // (or its JDK 21+ alias _from_compiled_code_entry_point).
            // Validate it before committing.
            const auto* fce_entry{ vmhook::hotspot::iterate_struct_entries("Method", "_from_compiled_code_entry_point") };
            if (!fce_entry)
            {
                fce_entry = vmhook::hotspot::iterate_struct_entries("Method", "_from_compiled_entry");
            }
            if (fce_entry && fce_entry->offset >= sizeof(void*))
            {
                const std::size_t guess{ static_cast<std::size_t>(fce_entry->offset) - sizeof(void*) };
                if (try_offset(guess))
                {
                    return guess;
                }
            }

            // Method object size — used as an upper bound for the scan.
            // VMTypes carries the size for every exported type; if Method
            // itself isn't in VMTypes (rare), cap at 512 bytes which is
            // generous for every JDK we've seen.
            std::size_t method_size{ 512 };
            if (const auto* const type{ vmhook::hotspot::iterate_type_entries("Method") })
            {
                if (type->size > 0 && type->size < 4096)
                {
                    method_size = static_cast<std::size_t>(type->size);
                }
            }

            for (std::size_t offset{ 0 }; offset + sizeof(void*) <= method_size; offset += sizeof(void*))
            {
                bool skip{ false };
                for (std::size_t i{ 0 }; i < skip_count; ++i)
                {
                    if (offset == skip_offsets[i])
                    {
                        skip = true;
                        break;
                    }
                }
                if (skip)
                {
                    continue;
                }
                if (try_offset(offset))
                {
                    return offset;
                }
            }

            return 0;
        }
    }

    namespace detail
    {
        /*
            @brief Finds a klass via JNI::FindClass using the context class loader.
            @details
            Fallback used by find_class() when the ClassLoaderDataGraph walk fails (e.g.
            when the class is loaded by a non-bootstrap class loader that is not yet
            reachable through the graph).  Calls JNIEnv::FindClass which uses the calling
            thread's context class loader, then resolves the returned jclass handle to the
            underlying Klass* via jni_klass_from_class_mirror.
            Defined after the JNI helper section.

            Complexity: O(lookup in the JVM class loader hierarchy).
            Exception safety: noexcept — returns nullptr on any failure.

            @param class_name  Internal JVM class name with '/' separators.
            @return The matching klass*, or nullptr if not found.
        */
        inline auto jni_find_class_with_context_loader(const std::string_view class_name) noexcept
            -> vmhook::hotspot::klass*;

        /*
            @brief Remembers candidate as the host-application's class source.
            @details
            Forward declaration; defined alongside the rest of the
            host-context-classloader machinery further down so it can call into
            the JNI helpers.  No-op when candidate is null or bootstrap-loaded.
        */
        inline auto capture_host_classloader_klass(vmhook::hotspot::klass* candidate) noexcept
            -> void;
    }

    // --- Cache and class lookup -----------------------------------------------

    /*
        @brief Cache of klass pointers keyed by their internal class name.
        @details
        Populated by find_class() on first lookup of each class name.
        Subsequent calls return the cached klass* directly without repeating
        the full ClassLoaderDataGraph walk.

        Concurrency: every read / write is gated by klass_lookup_cache_mutex.
        Without that, two worker threads racing through find_class() with
        different names could both trip an unordered_map rehash mid-insert
        and corrupt the bucket array (segfault or hang).  The cache only
        ever grows, so a shared_mutex would be slightly nicer for the steady
        state, but a plain mutex keeps the lock-acquisition logic simple and
        the contention is negligible (cache is warm after the first second
        of process lifetime).
    */
    inline std::unordered_map<std::string, vmhook::hotspot::klass*> klass_lookup_cache{};
    inline std::mutex klass_lookup_cache_mutex{};

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
    {
        {
            std::lock_guard<std::mutex> lock{ vmhook::klass_lookup_cache_mutex };
            const auto cache_entry{ vmhook::klass_lookup_cache.find(std::string{ class_name }) };
            if (cache_entry != vmhook::klass_lookup_cache.end())
            {
                vmhook::hotspot::klass* const cached_klass{ cache_entry->second };

                // Stale-cache check.  Class redefinition (JVMTI
                // RedefineClasses / RetransformClasses, or class
                // unloading + reload) can free the klass we cached.  A
                // subsequent find_class hit would return that dangling
                // pointer and every downstream consumer (get_field walks
                // for the SDK, Method* iteration for hook re-install, the
                // ClassLoaderDataGraph capture) would silently read
                // garbage and bail.  Cheap validation: read the klass's
                // own name symbol and compare to the requested name.
                // If they match, the klass is still the one we cached;
                // if they don't, the klass was replaced and we need to
                // re-resolve.
                //
                // klass::get_name and symbol::to_string both already
                // gate every dereference behind is_valid_pointer +
                // try/catch, so a hit on freed memory returns "" or
                // nullptr rather than AV-ing.
                if (cached_klass && vmhook::hotspot::is_valid_pointer(cached_klass))
                {
                    const vmhook::hotspot::symbol* const name_sym{ cached_klass->get_name() };
                    if (name_sym && vmhook::hotspot::is_valid_pointer(name_sym))
                    {
                        const std::string actual_name{ name_sym->to_string() };
                        if (actual_name == class_name)
                        {
                            return cached_klass;
                        }
                    }
                }

                // Cached klass is stale - evict and fall through to the
                // fresh ClassLoaderDataGraph walk below.
                vmhook::klass_lookup_cache.erase(cache_entry);
                VMHOOK_LOG("{} find_class: cached klass for '{}' is stale (likely class "
                           "redefinition or unload); evicting and re-resolving.",
                           vmhook::warning_tag, class_name);
            }
        }

        try
        {
            const vmhook::hotspot::class_loader_data_graph graph{};
            vmhook::hotspot::klass* found_klass{ graph.find_klass(class_name) };

            if (!found_klass)
            {
                found_klass = vmhook::detail::jni_find_class_with_context_loader(class_name);
                if (!found_klass)
                {
                    return nullptr;
                }
            }

            {
                std::lock_guard<std::mutex> lock{ vmhook::klass_lookup_cache_mutex };
                // Use insert (not assign) so a racing thread that beat us to
                // the punch with the same name doesn't overwrite their entry.
                vmhook::klass_lookup_cache.insert({ std::string{ class_name }, found_klass });
            }
            // Latch onto the first non-bootstrap class we see so freshly-
            // attached worker threads can inherit its loader — without this,
            // host mixins (Lunar's Adventure chat handler, Forge's wrapped
            // GuiNewChat) throw the moment they try to resolve a class via
            // Thread.currentThread().getContextClassLoader() on our threads.
            vmhook::detail::capture_host_classloader_klass(found_klass);
            return found_klass;
        }
        catch (const std::exception& exception)
        {
            VMHOOK_LOG("{} vmhook::find_class() for {}: {}", vmhook::error_tag, class_name, exception.what());
            return nullptr;
        }
    }

    /*
        @brief Invokes a callback for every loaded Java class.
        @details
        Walks the global ClassLoaderDataGraph and invokes the visitor
        once per Klass that is currently reachable through any
        ClassLoaderData node.  The visitor is called as
        `visitor(internal_name, klass*)` where `internal_name` uses
        JVM `/`-separated form (e.g. `"java/lang/String"`).

        The iteration is a *snapshot* taken at call time — classes
        loaded after `for_each_loaded_class` returns are not visited.
        Use `vmhook::on_class_loaded` for live notifications.

        On JDK 8 builds where `ClassLoaderData::_klasses` is not in
        VMStructs, iteration falls back to per-CLD dictionaries and
        may miss classes registered through internal-only paths; on
        JDK 21+ every loaded Klass is enumerated.

        Complexity: O(N) where N = total loaded classes.
        Exception safety: callback exceptions propagate; iteration
            stops at the throwing visit.
        Thread safety: safe to call concurrently with normal JVM
            execution; the underlying graph mutates on class load /
            unload so the snapshot is best-effort.

        @tparam visitor_type Callable invoked as
                             `void(const std::string&, vmhook::hotspot::klass*)`.

        Example:
        @code
            vmhook::for_each_loaded_class([](const std::string& name, vmhook::hotspot::klass*)
            {
                if (name.starts_with("net/minecraft/"))
                {
                    VMHOOK_LOG("loaded: {}", name);
                }
            });
        @endcode
    */
    template<typename visitor_type>
    inline auto for_each_loaded_class(visitor_type&& visit) -> void
    {
        try
        {
            const vmhook::hotspot::class_loader_data_graph graph{};
            graph.for_each_klass(std::forward<visitor_type>(visit));
        }
        catch (const std::exception& ex)
        {
            VMHOOK_LOG("{} for_each_loaded_class: {}", vmhook::error_tag, ex.what());
        }
    }

    /*
        @brief Forces every currently JIT-compiled Java method for which
               `predicate(class_name, method)` returns true to fall back to
               interpreted execution.
        @details
        Why this exists:  vmhook patches a method's interpreter entry
        (`_i2i_entry`).  That patch is only reached while the method runs
        interpreted.  Once HotSpot JIT-compiles either the callee OR any
        caller that inlines the callee, the compiled code bypasses the
        interpreter — and bypasses the hook.  vmhook's per-method install
        already deoptimises the hooked callee, but it does NOT invalidate
        callers that have already inlined the callee's body into their own
        nmethod, so calls reaching the hooked method through an
        already-inlined call site still bypass the hook.

        This helper closes that gap by applying the same Method-side
        deopt dance (set entry points to interpreter, clear _code) to
        every method `predicate` selects, regardless of whether the
        method was hooked or not.  HotSpot subsequently re-JITs hot
        methods, and because vmhook marks each hooked method with
        `_dont_inline`, the re-compiled callers no longer inline the
        hooked body — every dispatch now goes through the patched
        interpreter entry.

        Trade-off:  every selected method that was JIT-compiled is
        thrown away.  HotSpot has to recompile them as they warm up
        again, which produces a brief CPU spike right after the call.
        For hook installation (typically a one-time, startup-time
        operation), this cost is acceptable.

        Safety:  uses the same atomic single-pointer writes as
        `vmhook::hook<T>(...)`, with the same race-versus-running-Java
        properties (already exercised in production).  Skips any method
        whose c2i adapter cannot be recovered — leaving such a method
        JIT-compiled is safer than crashing the JVM with a
        null-code/stale-entry combination at the next safepoint.

        @tparam predicate_type   Callable invoked as
                                 `bool(const std::string& class_name,
                                       vmhook::hotspot::method* m)`.
        @return  Number of methods successfully deoptimised.

        Example:
        @code
            // Catch already-inlined Minecraft callers of any chat hook:
            vmhook::deoptimize_methods_if([](const std::string& name,
                                             vmhook::hotspot::method*)
            {
                return name.starts_with("net/minecraft/");
            });
        @endcode
    */
    template<typename predicate_type>
    inline auto deoptimize_methods_if(predicate_type&& predicate) noexcept
        -> std::size_t
    {
        std::size_t deoptimized{ 0 };
        std::size_t skipped_no_c2i{ 0 };
        vmhook::for_each_loaded_class(
            [&](const std::string& class_name, vmhook::hotspot::klass* const k)
            {
                if (!k)
                {
                    return;
                }
                const std::int32_t method_count{ k->get_methods_count() };
                vmhook::hotspot::method** const methods{ k->get_methods_ptr() };
                if (!methods || method_count <= 0)
                {
                    return;
                }
                for (std::int32_t i{ 0 }; i < method_count; ++i)
                {
                    vmhook::hotspot::method* const m{ methods[i] };
                    if (!m || !vmhook::hotspot::is_valid_pointer(m))
                    {
                        continue;
                    }
                    void* const code{ m->get_code() };
                    if (!code || !vmhook::hotspot::is_valid_pointer(code))
                    {
                        continue;  // Not JIT-compiled — nothing to do.
                    }
                    if (!predicate(class_name, m))
                    {
                        continue;
                    }

                    void* const i2i{ m->get_i2i_entry() };
                    void* const adapter{ m->get_adapter() };
                    void* const c2i{ vmhook::hotspot::get_c2i_entry_from_adapter(adapter) };

                    if (!i2i
                        || !c2i
                        || !vmhook::hotspot::is_valid_pointer(c2i))
                    {
                        ++skipped_no_c2i;
                        continue;
                    }

                    // Same ordering as the per-hook install path:
                    //   1. Redirect interpreted callers to the i2i stub.
                    //   2. Redirect compiled callers through the c2i adapter.
                    //   3. Clear _code last so the entry-point writes are
                    //      visible before HotSpot's safepoint check sees
                    //      _code == nullptr.
                    m->set_from_interpreted_entry(i2i);
                    m->set_from_compiled_entry(c2i);
                    m->set_code(nullptr);
                    ++deoptimized;
                }
            });
        VMHOOK_LOG("{} deoptimize_methods_if: {} deoptimised, {} skipped (no recoverable c2i adapter).",
                   vmhook::info_tag, deoptimized, skipped_no_c2i);
        return deoptimized;
    }

    /*
        @brief Convenience wrapper that deoptimises every JIT-compiled
               method in the JVM.
        @details
        Equivalent to `deoptimize_methods_if([](auto&, auto*) { return true; })`.
        Heavy: causes HotSpot to throw away every nmethod and recompile
        from scratch.  Use after installing all hooks you want to catch
        already-inlined callers for.  Returns the number of methods
        deoptimised.

        Recommended usage:
        @code
            auto h1 = vmhook::scoped_hook<my_class>("foo", cb1);
            auto h2 = vmhook::scoped_hook<my_class>("bar", cb2);
            vmhook::deoptimize_all_jit_compiled_methods();
        @endcode
    */
    inline auto deoptimize_all_jit_compiled_methods() noexcept
        -> std::size_t
    {
        return vmhook::deoptimize_methods_if(
            [](const std::string&, vmhook::hotspot::method*) noexcept { return true; });
    }

    /*
        @brief Snapshot of one HotSpot JavaThread, suitable for passing
               to the for_each_thread visitor.
        @details
        `thread` is the underlying JavaThread* — pass it back to
        vmhook's java_thread helpers (`get_thread_state`,
        `get_suspend_flags`, etc.) for deeper introspection.  Treat
        the pointer as valid only for the duration of the visit:
        the JVM may reclaim a JavaThread once the corresponding Java
        thread exits.
    */
    struct thread_info
    {
        vmhook::hotspot::java_thread*       thread{ nullptr };
        vmhook::hotspot::java_thread_state  state{ vmhook::hotspot::java_thread_state::_thread_uninitialized };
        vmhook::os::thread_id_t             os_thread_id{ 0 };
    };

    /*
        @brief Invokes the visitor for every JavaThread currently live
               in the JVM.
        @details
        Walks HotSpot's intrusive `Threads::_thread_list` (the same
        list used internally by `find_java_thread_by_os_thread_id`),
        capping at 4 096 entries as a runaway-list guard.  The visitor
        receives a `thread_info` snapshot — the underlying JavaThread*
        is safe to dereference during the visit, but should not be
        held past the call's return because the JVM may reclaim it
        when the Java thread exits.

        Concurrent thread creation / exit during iteration may show
        up as missed entries; HotSpot doesn't pin the list for us.
        For a stable view, walk during a quiescent period or after
        installing a JVMTI agent (out of scope for vmhook).

        Complexity: O(N) where N = number of live Java threads.
        Exception safety: callback exceptions propagate; iteration
            stops at the throwing visit.

        @tparam visitor_type Callable accepting `const thread_info&`.

        Example:
        @code
            vmhook::for_each_thread([](const vmhook::thread_info& t)
            {
                VMHOOK_LOG("thread {:p}: state={}, tid={}",
                           reinterpret_cast<void*>(t.thread),
                           static_cast<int>(t.state), t.os_thread_id);
            });
        @endcode
    */
    template<typename visitor_type>
    inline auto for_each_thread(visitor_type&& visit) -> void
    {
        const auto invoke_visitor = [&](vmhook::hotspot::java_thread* const t) -> void
        {
            if (!t || !vmhook::hotspot::is_valid_pointer(t))
            {
                return;
            }
            thread_info info{};
            info.thread       = t;
            info.state        = t->get_thread_state();
            info.os_thread_id = t->get_os_thread_id();
            visit(info);
        };

        // Path 1: classic Threads::_thread_list intrusive list,
        // present on JDK 8-9 and (where the JVM ships the VMStruct
        // entry) still useful on later builds.
        //
        // Hard cap at 4096 entries to short-circuit any runaway walk.
        // We also stop when the next pointer cycles back to a thread we
        // have already seen this iteration - a corrupted intrusive list
        // (e.g. mid-RedefineClasses while another agent re-stitches the
        // _next chain) can form a cycle, and without cycle detection we
        // would happily visit the same JavaThread up to 4096 times,
        // invoking the user visitor on duplicates.  visited_set is a
        // small unordered_set; with the cap at 4096 it stays bounded.
        std::int32_t visited_count{ 0 };
        bool         path1_visited_anything{ false };
        std::unordered_set<const vmhook::hotspot::java_thread*> visited_set;
        for (auto* current{ vmhook::hotspot::find_any_java_thread() };
             current && vmhook::hotspot::is_valid_pointer(current) && visited_count < 4096;
             ++visited_count)
        {
            if (!visited_set.insert(current).second)
            {
                // Already seen - cycle in the intrusive list.  Stop
                // here rather than reporting duplicates to the visitor.
                break;
            }
            invoke_visitor(current);
            path1_visited_anything = true;
            auto* const next{ current->get_next() };
            current = (next && vmhook::hotspot::is_valid_pointer(next)) ? next : nullptr;
        }
        if (path1_visited_anything)
        {
            return;
        }

        // Path 2: JDK 10+ Safe Memory Reclamation snapshot.  The
        // thread list lives in `ThreadsSMRSupport::_java_thread_list`
        // (static, points at a ThreadsList containing _length +
        // _threads JavaThread**).
        static const auto* const smr_list_entry{
            vmhook::hotspot::iterate_struct_entries("ThreadsSMRSupport", "_java_thread_list") };
        static const auto* const list_length_entry{
            vmhook::hotspot::iterate_struct_entries("ThreadsList", "_length") };
        static const auto* const list_threads_entry{
            vmhook::hotspot::iterate_struct_entries("ThreadsList", "_threads") };

        if (!smr_list_entry || !smr_list_entry->address
         || !list_length_entry || !list_threads_entry)
        {
            return;
        }

        void* const threads_list{
            *reinterpret_cast<void**>(smr_list_entry->address) };
        if (!vmhook::hotspot::is_valid_pointer(threads_list))
        {
            return;
        }

        const std::int32_t length{
            *reinterpret_cast<const std::int32_t*>(
                reinterpret_cast<const std::uint8_t*>(threads_list) + list_length_entry->offset) };
        auto** const threads_array{
            *reinterpret_cast<vmhook::hotspot::java_thread***>(
                reinterpret_cast<std::uint8_t*>(threads_list) + list_threads_entry->offset) };
        if (length <= 0 || length > 4096 || !threads_array)
        {
            return;
        }

        for (std::int32_t i{ 0 }; i < length; ++i)
        {
            invoke_visitor(threads_array[i]);
        }
    }

    /*
        @brief Visits every live instance of a registered C++ wrapper class.
        @details
        Walks `Universe::_collectedHeap::_reserved` linearly in 4 KiB
        chunks, decodes each candidate oop's narrow klass pointer at
        offset +8, and invokes the visitor for every header whose
        klass matches `T`'s registered class.  The visitor receives a
        freshly allocated `std::unique_ptr<T>` pointing at the live
        Java object, so it can read fields / call methods on it the
        same way a hooked-method callback would.

        The scan is single-threaded and best-effort:

          - The JVM is **not** brought to a safepoint, so a concurrent
            GC may move objects between when we see them and when the
            visitor accesses them.  Don't hold the wrappers past the
            return of `for_each_instance` if the GC is active.
          - On region-based GCs (G1) the heap reservation may include
            unmapped regions; we read in chunks and skip chunks where
            the safe-read fails.  This means *every* visit is correct
            (we only see real objects) but some objects may be missed.
          - Shenandoah / ZGC use colored pointers and forwarding
            tables that this scan does not understand.  Results on
            those collectors are undefined; prefer constructor-based
            tracking there.
          - The scan is O(heap-size).  On a 4 GiB heap it typically
            takes 0.5–2 s; pass `max_visits` to short-circuit once you
            have enough instances.

        @tparam T            Registered wrapper type (derived from
                             `vmhook::object<T>`), with an
                             `explicit T(vmhook::oop_t)` constructor.
        @tparam visitor_type Callable accepting `std::unique_ptr<T>`.
        @param  visit        Visitor invoked for each match.
        @param  max_visits   Stop after this many matches.  Defaults
                             to no limit.
        @return  Number of instances reported to the visitor.
    */
    template<typename T, typename visitor_type>
    inline auto for_each_instance(visitor_type&& visit,
                                  std::size_t   max_visits = std::numeric_limits<std::size_t>::max())
        -> std::size_t
    {
        // Resolve T's registered klass.
        const auto type_idx{ std::type_index{ typeid(T) } };
        const auto type_map_it{ vmhook::type_to_class_map.find(type_idx) };
        if (type_map_it == vmhook::type_to_class_map.end())
        {
            VMHOOK_LOG("{} for_each_instance: type not registered (call register_class<T>() first)",
                       vmhook::error_tag);
            return 0;
        }
        vmhook::hotspot::klass* const target_klass{ vmhook::find_class(type_map_it->second) };
        if (!target_klass)
        {
            VMHOOK_LOG("{} for_each_instance: find_class('{}') returned null - the registered "
                       "class is not loaded in the JVM yet (or VMStructs+JNI fallback couldn't "
                       "resolve it).  Visitor will not be invoked.",
                       vmhook::warning_tag, type_map_it->second);
            return 0;
        }

        // Resolve heap bounds via VMStructs.  All entries cached on
        // first call; subsequent calls hit a static.
        static const auto* const heap_static_entry{
            vmhook::hotspot::iterate_struct_entries("Universe", "_collectedHeap") };
        static const auto* const reserved_offset{
            vmhook::hotspot::iterate_struct_entries("CollectedHeap", "_reserved") };
        static const auto* const memregion_start_offset{
            vmhook::hotspot::iterate_struct_entries("MemRegion", "_start") };
        static const auto* const memregion_word_size_offset{
            vmhook::hotspot::iterate_struct_entries("MemRegion", "_word_size") };
        if (!heap_static_entry || !heap_static_entry->address
         || !reserved_offset
         || !memregion_start_offset
         || !memregion_word_size_offset)
        {
            VMHOOK_LOG("{} for_each_instance: heap VMStruct entries missing on this JDK",
                       vmhook::error_tag);
            return 0;
        }

        void* const collected_heap{ *reinterpret_cast<void**>(heap_static_entry->address) };
        if (!vmhook::hotspot::is_valid_pointer(collected_heap))
        {
            return 0;
        }

        auto* const memregion_addr{
            reinterpret_cast<std::uint8_t*>(collected_heap) + reserved_offset->offset };
        void* const heap_start{
            *reinterpret_cast<void**>(memregion_addr + memregion_start_offset->offset) };
        const std::size_t heap_word_count{
            *reinterpret_cast<std::size_t*>(memregion_addr + memregion_word_size_offset->offset) };
        const std::size_t heap_byte_size{ heap_word_count * sizeof(void*) };

        if (!vmhook::hotspot::is_valid_pointer(heap_start) || heap_byte_size == 0)
        {
            return 0;
        }

        auto* const scan_begin{ static_cast<std::uint8_t*>(heap_start) };
        auto* const scan_end{ scan_begin + heap_byte_size };

        // Chunked safe-read scan: copy 4 KiB at a time so each
        // candidate read stays in our process address space (no
        // per-cell SEH or signal-handler entry).
        constexpr std::size_t chunk_size{ 4096 };
        constexpr std::size_t stride{ 8 };
        alignas(8) std::uint8_t buffer[chunk_size];
        std::size_t visits{ 0 };

        for (auto* p{ scan_begin }; p < scan_end && visits < max_visits; p += chunk_size)
        {
            const std::size_t to_read{
                std::min<std::size_t>(chunk_size, static_cast<std::size_t>(scan_end - p)) };
            if (!vmhook::os::safe_read(buffer, p, to_read))
            {
                continue;
            }

            // Walk the chunk at 8-byte stride; every potential
            // object header has its mark word at +0 and a narrow
            // klass pointer at +8.
            for (std::size_t off{ 0 }; off + 12 <= to_read && visits < max_visits; off += stride)
            {
                const std::uint32_t narrow{
                    *reinterpret_cast<const std::uint32_t*>(buffer + off + 8) };
                if (narrow == 0)
                {
                    continue;
                }
                void* const decoded{ vmhook::hotspot::decode_klass_pointer(narrow) };
                if (decoded != static_cast<void*>(target_klass))
                {
                    continue;
                }

                // Hand the visitor a wrapper pointing at the real
                // heap address (not the buffer copy).
                try
                {
                    // T's constructor accepts vmhook::oop_t, which is
                    // an alias for void* defined further down in the
                    // header — use void* directly so we don't need
                    // the typedef in scope yet.
                    auto wrapper{ std::unique_ptr<T>{
                        new T{ static_cast<void*>(p + off) } } };
                    visit(std::move(wrapper));
                }
                catch (const std::exception& ex)
                {
                    VMHOOK_LOG("{} for_each_instance visitor: {}",
                               vmhook::error_tag, ex.what());
                }
                ++visits;
            }
        }

        return visits;
    }

    /*
        @brief Associates a C++ type with its corresponding Java class name.
        @tparam T The C++ type to register.
        @param class_name The internal JVM class name using '/' separators.
        @return true if the class was found in the JVM and successfully registered,
                false if the class could not be found.
        @details
        Stores the mapping from the C++ type_index of T to class_name in type_to_class_map,
        then verifies the class exists in the JVM by calling find_class().
        Registration is required before calling hook<T>().
    */
    template<class wrapper_type>
    static auto register_class(const std::string_view class_name) noexcept
        -> bool
    {
        vmhook::hotspot::klass* const verified_klass{ vmhook::find_class(class_name) };

        if (!verified_klass)
        {
            VMHOOK_LOG("{} register_class() for {}: class not found in JVM.", vmhook::error_tag, class_name);
            return false;
        }

        // Serialise both map writes behind registration_mutex.  Without
        // this, two threads racing through register_class() can trip an
        // unordered_map rehash mid-insert and corrupt the bucket array.
        // Readers in detour-hot paths (extract_frame_arg / jni_signature_for_arg /
        // on_class_loaded) do NOT take the lock - the documented contract is
        // that register_class is called from single-threaded setup BEFORE
        // hooks fire.  This lock guards against concurrent registers, which
        // can happen on multi-stage initialisation paths or when a hook
        // detour itself triggers lazy registration of a new wrapper.
        std::lock_guard<std::mutex> lock{ vmhook::registration_mutex };

        vmhook::type_to_class_map.insert_or_assign(std::type_index{ typeid(wrapper_type) }, std::string{ class_name });

        // Store a factory function so field_proxy::get_as<T>() and frame::get_arguments()
        // can construct C++ wrapper objects from decoded Java object references.
        // The factory returns a raw pointer; consumers immediately wrap it in
        // a unique_ptr at the call site.  See type_factory_function_t for why.
        vmhook::g_type_factory_map.emplace(std::string{ class_name }, +[](void* instance)
            -> object_base*
            {
                return new wrapper_type{ instance };
            }
        );

        return true;
    }

    // -------------------------------------------------------------------------
    // Method enumeration / introspection.
    //
    // The motivating problem (from real obfuscated-build hooking): the JVM
    // method NAME rotates per client / per build, but its descriptor stays
    // stable.  So "hook the method whose descriptor is S" is the operation you
    // actually need — and that requires being able to list a class's methods.
    // These helpers expose the InstanceKlass::_methods walk that hook<T> already
    // runs privately, as public data (not just a log line, which is compiled out
    // in release).  No JNI; safe from any thread once the class is loaded.
    // -------------------------------------------------------------------------

    namespace detail
    {
        /*
            @brief Walks a klass's _methods array into (name, JVM-descriptor) pairs.
            @details Skips slots that fail is_valid_pointer.  noexcept; empty on
            null klass.  Shared by every get_class_methods overload.
        */
        inline auto collect_klass_methods(vmhook::hotspot::klass* const target_klass) noexcept
            -> std::vector<std::pair<std::string, std::string>>
        {
            std::vector<std::pair<std::string, std::string>> result{};
            try
            {
                if (!target_klass)
                {
                    return result;
                }
                const std::int32_t method_count{ target_klass->get_methods_count() };
                vmhook::hotspot::method** const methods_array{ target_klass->get_methods_ptr() };
                if (!methods_array || method_count <= 0)
                {
                    return result;
                }
                result.reserve(static_cast<std::size_t>(method_count));
                for (std::int32_t method_index{ 0 }; method_index < method_count; ++method_index)
                {
                    vmhook::hotspot::method* const method_ptr{ methods_array[method_index] };
                    if (!method_ptr || !vmhook::hotspot::is_valid_pointer(method_ptr))
                    {
                        continue;
                    }
                    result.emplace_back(method_ptr->get_name(), method_ptr->get_signature());
                }
            }
            catch (...)
            {
            }
            return result;
        }
    }

    /*
        @brief Lists every declared method of a class (by internal name) as
               (name, JVM-descriptor) pairs.
        @details
        Reads InstanceKlass::_methods directly — no JNI — so it works for classes
        you have NOT wrapped with register_class<T>() (useful while you are still
        discovering an obfuscated class's shape).  noexcept; empty vector if the
        class isn't loaded.

            for (auto& [name, descriptor] : vmhook::get_class_methods("net/minecraft/Foo"))
                VMHOOK_LOG("{}{}", name, descriptor);
    */
    inline auto get_class_methods(const std::string_view class_name) noexcept
        -> std::vector<std::pair<std::string, std::string>>
    {
        return vmhook::detail::collect_klass_methods(vmhook::find_class(class_name));
    }

    /*
        @brief Lists every declared method of a registered wrapper's class.
        @details Same as the by-name overload, resolving the class through the
        wrapper's register_class<T>() entry.  Empty if T is unregistered.
    */
    template<class wrapper_type>
    inline auto get_class_methods() noexcept
        -> std::vector<std::pair<std::string, std::string>>
    {
        try
        {
            const auto type_map_entry{
                vmhook::type_to_class_map.find(std::type_index{ typeid(wrapper_type) }) };
            if (type_map_entry == vmhook::type_to_class_map.end())
            {
                return {};
            }
            return vmhook::detail::collect_klass_methods(vmhook::find_class(type_map_entry->second));
        }
        catch (...)
        {
            return {};
        }
    }

    /*
        @brief Debug convenience: VMHOOK_LOGs every (name, descriptor) of T's class.
        @details Compiled out in release (VMHOOK_LOG is a no-op); use
        get_class_methods<T>() for a release-safe data channel.
    */
    template<class wrapper_type>
    inline auto log_class_methods() noexcept
        -> void
    {
        const auto methods{ vmhook::get_class_methods<wrapper_type>() };
        VMHOOK_LOG("{} log_class_methods<{}>(): {} method(s):",
                   vmhook::info_tag, typeid(wrapper_type).name(), methods.size());
        std::size_t method_index{ 0 };
        for (const auto& [name, descriptor] : methods)
        {
            VMHOOK_LOG("    [{}] {}{}", method_index++, name, descriptor);
        }
    }

    /*
        @brief Names of every method on T's class whose JVM descriptor equals
               `descriptor`.
        @details The obfuscated-build selector: the name rotates, the descriptor
        is stable.  Returns ALL matches so callers can detect a non-unique
        descriptor instead of silently taking the first.  Empty if none / T
        unregistered.

            auto names = vmhook::find_methods_by_signature<foo>("()Ljava/util/Collection;");
            if (names.size() == 1)
                vmhook::hook<foo>(names.front(), "()Ljava/util/Collection;", detour);
    */
    template<class wrapper_type>
    inline auto find_methods_by_signature(const std::string_view descriptor) noexcept
        -> std::vector<std::string>
    {
        std::vector<std::string> names{};
        for (const auto& [name, candidate] : vmhook::get_class_methods<wrapper_type>())
        {
            if (candidate == descriptor)
            {
                names.push_back(name);
            }
        }
        return names;
    }

    // -------------------------------------------------------------------------
    // Watchers: event-driven hooks for field-value changes and for
    // newly-loaded Java classes.  No background polling, no idle CPU —
    // the field watcher installs a hardware data breakpoint that fires
    // synchronously on the writing thread, and the class-load watcher
    // installs a method hook on java.lang.ClassLoader.defineClass.
    // watch_handle disarms the trap / unregisters the callback when it
    // goes out of scope.
    // -------------------------------------------------------------------------

    /*
        @brief RAII handle for an event-driven watcher (field-change / class-load).
        @details
        The handle owns a disarm callback that runs exactly once, either
        when stop() is called explicitly or when the handle is destroyed.
        There is no worker thread; the watch is passive between events.
        Move-only.
    */
    class watch_handle
    {
    public:
        struct control_block
        {
            std::function<void()> on_stop{};
            bool                  stopped{ false };
        };

        watch_handle() = default;
        explicit watch_handle(std::shared_ptr<control_block> cb) noexcept
            : block{ std::move(cb) }
        {
        }

        watch_handle(const watch_handle&) = delete;
        auto operator=(const watch_handle&) -> watch_handle & = delete;

        watch_handle(watch_handle&& other) noexcept
            : block{ std::move(other.block) }
        {
        }

        auto operator=(watch_handle&& other) noexcept -> watch_handle &
        {
            if (this != &other)
            {
                this->stop();
                this->block = std::move(other.block);
            }
            return *this;
        }

        ~watch_handle()
        {
            this->stop();
        }

        /*
            @brief Disarms the watcher (idempotent).  After stop() returns
                   no further callbacks will fire.
        */
        auto stop() noexcept -> void
        {
            if (!this->block || this->block->stopped)
            {
                this->block.reset();
                return;
            }
            this->block->stopped = true;
            if (this->block->on_stop)
            {
                try
                {
                    this->block->on_stop();
                }
                catch (const std::exception& ex)
                {
                    VMHOOK_LOG("{} watch_handle::stop(): on_stop callback threw: {} - "
                               "swallowed to honour the no-throw contract.",
                               vmhook::error_tag, ex.what());
                }
                catch (...)
                {
                    VMHOOK_LOG("{} watch_handle::stop(): on_stop callback threw an unknown "
                               "exception - swallowed to honour the no-throw contract.",
                               vmhook::error_tag);
                }
                this->block->on_stop = nullptr;
            }
            this->block.reset();
        }

        /*
            @brief Returns true while the watcher is armed.
        */
        auto running() const noexcept -> bool
        {
            return this->block && !this->block->stopped;
        }

    private:
        std::shared_ptr<control_block> block{};
    };

    /*
        @brief RAII handle for a single installed method hook.
        @details
        Returned by vmhook::scoped_hook<T>(...).  Destroying the handle
        (or calling stop()) tears down only this hook — other hooks on
        other methods stay armed.  vmhook::shutdown_hooks() still
        removes everything in one call when you want a hard reset.

        Move-only.  An empty hook_handle (default-constructed or after
        stop()) returns running() == false and does nothing on
        destruction.

        Thread safety: hook removal mutates the global
        g_hooked_methods list, which is not synchronised.  Either
        remove hooks from a single thread, or guard the removal site
        with a mutex.  In-flight callbacks for the method being
        removed must have finished before stop() returns to avoid
        racing the detour's std::function destruction; in practice
        this means ensuring no Java thread is currently inside the
        hooked method when the handle is destroyed.
    */
    class hook_handle
    {
    public:
        hook_handle() = default;
        explicit hook_handle(vmhook::hotspot::method* m) noexcept
            : method{ m }
        {
        }

        hook_handle(const hook_handle&) = delete;
        auto operator=(const hook_handle&) -> hook_handle& = delete;

        hook_handle(hook_handle&& other) noexcept
            : method{ other.method }
        {
            other.method = nullptr;
        }

        auto operator=(hook_handle&& other) noexcept -> hook_handle&
        {
            if (this != &other)
            {
                this->stop();
                this->method = other.method;
                other.method = nullptr;
            }
            return *this;
        }

        ~hook_handle()
        {
            this->stop();
        }

        /*
            @brief Returns true while the hook is still installed.
        */
        auto installed() const noexcept -> bool
        {
            return this->method != nullptr;
        }

        /*
            @brief Tears down this specific hook (idempotent).  Restores
                   the method's original entry points, clears the
                   no-inline / no-compile flags, and removes the entry
                   from the global hooked-method list so the i2i
                   common_detour stops dispatching to it.  The
                   underlying midi2i_hook trampoline is left in place
                   because it may still serve other methods sharing
                   the same i2i_entry; clearing it would risk
                   un-patching live dispatch sites.
        */
        auto stop() noexcept -> void;

    private:
        vmhook::hotspot::method* method{ nullptr };
    };

    // The function templates `watch_static_field` and `on_class_loaded`
    // depend on `vmhook::object_base::get_field` (a static method).  At
    // this point in the header object_base is only forward-declared,
    // which Clang's template-body checker flags eagerly.  The full
    // definitions live further down (search for "watch_static_field").

    // --- Internal helpers for typed hook API ---------------------------------

    namespace detail
    {
        /*
            @brief Type trait that extracts the argument-types tuple from any callable type.
            @details
            Specialisations cover the four common callable forms:
              - Plain function pointers: return_type(*)(args...)
              - std::function instances: std::function<return_type(args...)>
              - Lambdas and functors:    operator() const member pointer
              - Non-const member functions: return_type(class::*)(args...)
            The associated member type args_tuple_t is a std::tuple of the raw argument
            types.  Used by the typed hook<T>() overload to enumerate the Java parameter
            descriptors at compile time so that the interpreter-frame read is fully
            type-safe.

            Exception safety: noexcept — compile-time trait, no runtime cost.
        */
        template<typename function_type, typename = void>
        struct function_traits;

        template<typename return_type, typename... argument_types>
        struct function_traits<return_type(*)(argument_types...)>
        {
            using args_tuple_t = std::tuple<argument_types...>;
        };

        template<typename return_type, typename... argument_types>
        struct function_traits<std::function<return_type(argument_types...)>>
        {
            using args_tuple_t = std::tuple<argument_types...>;
        };

        template<typename function_type>
        struct function_traits<function_type, std::void_t<decltype(&function_type::operator())>>
            : function_traits<decltype(&function_type::operator())>
        {
        };

        template<typename class_type, typename return_type, typename... argument_types>
        struct function_traits<return_type(class_type::*)(argument_types...) const>
        {
            using args_tuple_t = std::tuple<argument_types...>;
        };

        template<typename class_type, typename return_type, typename... argument_types>
        struct function_traits<return_type(class_type::*)(argument_types...)>
        {
            using args_tuple_t = std::tuple<argument_types...>;
        };

        /*
            @brief Removes the first element from a std::tuple type.
            @details
            Used to strip the leading vmhook::return_value& argument from the hook
            callback's args_tuple_t so that only the Java-visible parameters remain.
            The resulting member type_t is the truncated tuple.

            Exception safety: noexcept — compile-time trait, no runtime cost.
        */
        template<typename tuple_type>
        struct tuple_tail;

        template<typename first_type, typename... remaining_types>
        struct tuple_tail<std::tuple<first_type, remaining_types...>>
        {
            using type_t = std::tuple<remaining_types...>;
        };

        /*
            @brief Extracts a single Java method argument from the interpreter frame as value_type.
            @details
            Reads the local-variable slot at position index (negated relative to the locals
            pointer) and converts the raw slot bits to the requested C++ type using a
            compile-time if-constexpr dispatch:
              - std::string               decoded OOP fed into read_java_string()
              - std::unique_ptr<U>        decoded OOP fed into the registered factory for U
              - pointer types             decoded compressed OOP
              - primitive / trivial types raw slot bits via memcpy

            Uses the public get_locals() path so it does not require friendship with frame.

            Complexity: O(1) for primitives; O(S) for strings where S = string length.
            Exception safety: noexcept-boundary — underlying helpers may throw internally
                              but the function returns base_t{} on any error path.

            @param frame  Interpreter frame whose locals to read.
            @param index  Zero-based argument index (0 = this / first parameter).
            @return The decoded argument value, or base_t{} on failure.
        */
        /*
            @brief Compile-time slot width of a Java arg of C++ type T.
            @details
            HotSpot's interpreter stores each `long` and `double` parameter
            in TWO adjacent local-variable slots; every other type (int,
            float, object, etc.) takes one slot.  Both `arg index` (which
            arg, in declaration order) and `slot index` (where it lives in
            the locals array) are needed when walking frames - and they
            differ as soon as any earlier arg was a long or double.

            Mapping convention:
              std::int64_t / std::uint64_t -> Java long   -> 2 slots
              double                       -> Java double -> 2 slots
              everything else              -> 1 slot
        */
        template<typename type>
        inline constexpr bool is_java_double_slot_v{
            std::is_same_v<std::remove_cvref_t<type>, std::int64_t>
            || std::is_same_v<std::remove_cvref_t<type>, std::uint64_t>
            || std::is_same_v<std::remove_cvref_t<type>, double> };

        /*
            @brief Compile-time table of JVM slot offsets, one per arg in
                   a tuple of C++ arg types.
            @details
            For a tuple `(int, long, int)`:
              result[0] = 0  // int starts at slot 0
              result[1] = 1  // long starts at slot 1, takes slots 1 and 2
              result[2] = 3  // second int starts at slot 3 (NOT slot 2)

            Built once at template instantiation; never recomputed.

            Critical: extract_frame_arg used to be called with the TUPLE
            INDEX directly (`extract_frame_arg<T>(frame, 0)`,
            `extract_frame_arg<T>(frame, 1)`, ...).  That happens to work
            when no arg is a long or double, but every method whose
            argument list contains a J/D followed by anything else read
            the wrong slot for every following arg and silently fed the
            user detour garbage.
        */
        template<typename tuple_type>
        struct java_slot_offsets;

        template<typename... arg_types>
        struct java_slot_offsets<std::tuple<arg_types...>>
        {
            static constexpr auto compute() noexcept
            {
                std::array<std::int32_t, sizeof...(arg_types)> offsets{};
                std::int32_t offset{ 0 };
                std::size_t index{ 0 };
                (
                    (offsets[index++] = offset,
                     offset += (vmhook::detail::is_java_double_slot_v<arg_types> ? 2 : 1)),
                    ...
                );
                return offsets;
            }

            static constexpr auto value{ compute() };
        };

        template<>
        struct java_slot_offsets<std::tuple<>>
        {
            static constexpr std::array<std::int32_t, 0> value{};
        };

        template<typename value_type>
        auto extract_frame_arg(vmhook::hotspot::frame* const frame, const std::int32_t index)
            -> std::remove_cvref_t<value_type>
        {
            using base_t = std::remove_cvref_t<value_type>;

            // Bounds-guard the slot index (JVM max_locals is a u2): a negative or
            // past-max_locals index turns locals[-index] into an out-of-range read
            // of adjacent thread state.  Today the index comes from the compile-time
            // java_slot_offsets table, but the signature is public and frame->
            // get_argument shares the hazard (audit hook_arg_decoding_primitives).
            constexpr std::int32_t max_jvm_locals{ 0xFFFF };
            if (index < 0 || index > max_jvm_locals)
            {
                return base_t{};
            }

            void** const locals{ frame->get_locals() };
            if (!locals)
            {
                return base_t{};
            }

            void* const raw_value{ locals[-index] };

            // Decode a compressed OOP (32-bit narrow value) to a full 64-bit pointer.
            auto decode_oop = [](void* raw_value)
                -> void*
                {
                    if (!raw_value)
                    {
                        return nullptr;
                    }

                    const std::uintptr_t bits{ reinterpret_cast<std::uintptr_t>(raw_value) };
                    return (bits <= 0xFFFFFFFFull)
                        ? vmhook::hotspot::decode_oop_pointer(static_cast<std::uint32_t>(bits))
                        : raw_value;
                };

            if constexpr (std::is_same_v<base_t, std::string>)
            {
                return vmhook::read_java_string(decode_oop(raw_value));
            }
            else if constexpr (is_unique_ptr_v<base_t>)
            {
                using element_t = typename base_t::element_type;
                void* const oop{ decode_oop(raw_value) };
                if (!oop)
                {
                    return nullptr;
                }
                const auto type_it{ vmhook::type_to_class_map.find(std::type_index{ typeid(element_t) }) };
                if (type_it == vmhook::type_to_class_map.end())
                {
                    return nullptr;
                }
                const auto factory_it{ vmhook::g_type_factory_map.find(type_it->second) };
                if (factory_it == vmhook::g_type_factory_map.end())
                {
                    return nullptr;
                }
                // factory returns object_base*; downcast to element_t* and
                // hand it to the unique_ptr at the call site.
                return base_t{ static_cast<element_t*>(factory_it->second(oop)) };
            }
            else if constexpr (std::is_pointer_v<base_t>)
            {
                return reinterpret_cast<base_t>(decode_oop(raw_value));
            }
            else if constexpr (sizeof(base_t) <= sizeof(void*))
            {
                // Two-word primitives (long/double, 8 bytes) span local slots
                // [index] and [index+1]; HotSpot stores the 64-bit value at the
                // lower slot locals[-(index+1)] (LOCALS_LONG / LOCALS_DOUBLE).
                // One-word primitives (≤4 bytes) live at locals[-index] (raw_value).
                void* slot_value{ raw_value };
                if constexpr (sizeof(base_t) > sizeof(std::int32_t))
                {
                    slot_value = locals[-(index + 1)];
                }
                base_t result{};
                std::memcpy(&result, &slot_value, sizeof(base_t));
                return result;
            }
            else
            {
                static_assert(vmhook::detail::dependent_false_v<base_t>,
                              "vmhook::detail::extract_frame_arg: unsupported argument type.  "
                              "The previous behaviour silently returned a default-constructed "
                              "value (all zeros) for any type larger than a pointer that wasn't "
                              "std::string / unique_ptr<T> / a raw pointer - user hook callbacks "
                              "saw a hidden zero arg with no compile-time or runtime warning.  "
                              "Convert the arg to one of: primitive / pointer / std::string / "
                              "unique_ptr<vmhook::object>.");
            }
        }

        /*
            @brief Type trait that detects std::unique_ptr<T> where T derives from vmhook::object_base.
            @details
            Used in return_value::set_arg() to identify arguments that are managed C++
            wrappers around Java objects.  The primary template inherits from std::false_type;
            the partial specialisation inherits from std::bool_constant<is_base_of<object_base, T>>.

            Exception safety: noexcept — compile-time trait, no runtime cost.
        */
        template<typename type>
        struct is_unique_object_ptr : std::false_type {};

        // Same shadowing rule as is_unique_ptr above: do not name the
        // template parameter `value_type`, since the std::true_type /
        // std::bool_constant base also exports a `value_type` typedef
        // (= bool).  Use `wrapped_type` so the trait stays robust if
        // someone later adds a `using value_type_t = wrapped_type;`.
        template<typename wrapped_type, typename deleter_type>
        struct is_unique_object_ptr<std::unique_ptr<wrapped_type, deleter_type>>
            : std::bool_constant<std::is_base_of_v<vmhook::object_base, wrapped_type>> {};

        /*
            @brief Decodes a JNI local reference to a raw heap OOP pointer.
            @details
            JNI object references (jobject, jclass, jstring, etc.) are handles stored in
            the current thread's JNI handle block, not direct heap pointers.  This function
            dereferences the handle to obtain the underlying OOP.
            Defined after the JNI helper section below.

            Complexity: O(1).
            Exception safety: noexcept — returns nullptr on failure.
        */
        inline auto jni_decode_object(void* object_handle) noexcept
            -> void*;

        /*
            @brief Creates a new Java String from a UTF-8 string_view using JNI.
            @details
            Calls JNIEnv::NewStringUTF via the cached current_jni_env.  Returns a JNI
            local reference (jobject handle), not a raw OOP.  Use jni_decode_object() to
            obtain the OOP if raw heap access is needed.
            Defined after the JNI helper section below.

            Complexity: O(N) where N = length of value.
            Exception safety: noexcept — returns nullptr if JNIEnv is unavailable or allocation fails.
        */
        inline auto jni_new_string_utf(std::string_view value) noexcept
            -> void*;

        /*
            @brief Releases a JNI local reference via JNIEnv::DeleteLocalRef.
            @details
            Callers that produce a local ref via jni_new_string_utf / jni_find_class /
            jni_get_object_class etc. and only need the underlying OOP must DeleteLocalRef
            the handle once they're done with it.  Long-lived attached threads (every
            HotSpot interpreter thread that runs our detour) keep accumulating local refs
            otherwise; the JNI local-ref table is small (default capacity 16) and
            EnsureLocalCapacity / native-method-frame teardown is the normal way to
            clean it up — neither runs for vmhook's detour-attached threads.
            Defined after the JNI helper section below.

            Complexity: O(1).
            Exception safety: noexcept — null handle is a safe no-op.
        */
        inline auto jni_delete_local_ref(void* object_handle) noexcept
            -> void;
    } // namespace detail

    inline auto return_value::caller() const noexcept -> caller_info
    {
        caller_info empty{};
        if (!this->stack_frame)
        {
            return empty;
        }

        // The HotSpot x64 interpreter writes the caller's rbp at [rbp+0].
        // Saved-rbp chains only work when the caller is *also* an
        // interpreted frame; if the caller is compiled or native, the
        // chain breaks and pointer checks below will reject the read.
        void* const caller_rbp_slot{ this->stack_frame };
        if (!vmhook::hotspot::is_valid_pointer(caller_rbp_slot))
        {
            return empty;
        }
        void* const caller_rbp{ *reinterpret_cast<void* const*>(caller_rbp_slot) };
        if (!vmhook::hotspot::is_valid_pointer(caller_rbp))
        {
            return empty;
        }

        // The Method* lives 3 words (24 bytes) below rbp in the
        // interpreter frame layout (see frame::get_method for the
        // matching read on the current frame).
        void* const caller_method_slot{
            reinterpret_cast<std::uint8_t*>(caller_rbp) - 24 };
        if (!vmhook::hotspot::is_valid_pointer(caller_method_slot))
        {
            return empty;
        }
        auto* const caller_method{
            *reinterpret_cast<vmhook::hotspot::method* const*>(caller_method_slot) };
        if (!caller_method || !vmhook::hotspot::is_valid_pointer(caller_method))
        {
            return empty;
        }

        // Validate by trying to read the method name through the same safe
        // path the rest of vmhook uses.  An empty string signals an
        // unidentifiable frame.
        std::string method_name{ caller_method->get_name() };
        if (method_name.empty())
        {
            return empty;
        }

        caller_info info{};
        info.method      = caller_method;
        info.method_name = std::move(method_name);
        info.signature   = caller_method->get_signature();

        // Best-effort class-name lookup.  Method -> ConstMethod ->
        // ConstantPool holds a back-pointer to the owning Klass via
        // _pool_holder; we read it through the cached VMStruct offset.
        if (const auto* const const_method{ caller_method->get_const_method() })
        {
            if (auto* const cp{ const_method->get_constants() })
            {
                static const auto* const pool_holder_entry{
                    vmhook::hotspot::iterate_struct_entries("ConstantPool", "_pool_holder") };
                if (pool_holder_entry)
                {
                    auto* const klass{ *reinterpret_cast<vmhook::hotspot::klass* const*>(
                        reinterpret_cast<const std::uint8_t*>(cp) + pool_holder_entry->offset) };
                    if (klass && vmhook::hotspot::is_valid_pointer(klass))
                    {
                        if (auto* const name_symbol{ klass->get_name() })
                        {
                            info.class_name = name_symbol->to_string();
                        }
                    }
                }
            }
        }

        return info;
    }

    inline auto return_value::stack_trace(std::size_t max_depth) const noexcept
        -> std::vector<caller_info>
    {
        std::vector<caller_info> frames{};
        if (max_depth == 0)
        {
            max_depth = 64;
        }
        if (!this->stack_frame)
        {
            return frames;
        }

        // Walk the saved-rbp chain.  Each iteration reads the caller's
        // rbp from [current_rbp+0], then their Method* from
        // [caller_rbp - 24].  Every dereference is gated by
        // is_valid_pointer so a non-interpreter frame produces an early
        // return rather than a crash.
        void* current_rbp_slot{ this->stack_frame };
        frames.reserve(8);

        static const auto* const pool_holder_entry{
            vmhook::hotspot::iterate_struct_entries("ConstantPool", "_pool_holder") };

        for (std::size_t depth{ 0 }; depth < max_depth; ++depth)
        {
            if (!vmhook::hotspot::is_valid_pointer(current_rbp_slot))
            {
                break;
            }
            void* const caller_rbp{ *reinterpret_cast<void* const*>(current_rbp_slot) };
            if (!vmhook::hotspot::is_valid_pointer(caller_rbp))
            {
                break;
            }

            // Enforce the stack-growth invariant: callers live at HIGHER
            // addresses, so a valid saved-rbp chain is strictly increasing and
            // each frame is a sane size apart.  A caller_rbp that is not above
            // the current slot — or absurdly far above it — means the chain has
            // strayed off the interpreter frames (into a native/compiled frame
            // or garbage); stop instead of dereferencing [caller_rbp - 24] at an
            // arbitrary address.  This is the load-bearing guard that keeps the
            // walk from AV-ing when it runs past the interpreted region (the
            // is_valid_pointer checks alone accept any mapped page, including a
            // garbage frame that happens to be readable) — observed crashing the
            // suite on Windows mingw/clang + JDK 17.
            {
                const std::uintptr_t cur_addr{ reinterpret_cast<std::uintptr_t>(current_rbp_slot) };
                const std::uintptr_t cal_addr{ reinterpret_cast<std::uintptr_t>(caller_rbp) };
                if (cal_addr <= cur_addr || (cal_addr - cur_addr) > (std::uintptr_t{ 1 } << 20))
                {
                    break;
                }
            }

            void* const caller_method_slot{
                reinterpret_cast<std::uint8_t*>(caller_rbp) - 24 };
            if (!vmhook::hotspot::is_valid_pointer(caller_method_slot))
            {
                break;
            }
            auto* const caller_method{
                *reinterpret_cast<vmhook::hotspot::method* const*>(caller_method_slot) };
            if (!caller_method || !vmhook::hotspot::is_valid_pointer(caller_method))
            {
                break;
            }

            // Validate the Method's ConstMethod -> ConstantPool chain BEFORE
            // reading its name/signature.  get_name()/get_signature() dereference
            // that chain WITHOUT per-step pointer gates, so a valid-looking but
            // not-real Method* — which the saved-rbp walk can land on when it
            // strays into a non-interpreter (e.g. native/compiled) frame, observed
            // as an AV on clang / JDK 17 — would crash here.  Resolving and
            // validating the chain up front lets the walk break cleanly instead,
            // and the class-name lookup below reuses the now-trusted pointers.
            auto* const const_method{ caller_method->get_const_method() };
            if (!const_method || !vmhook::hotspot::is_valid_pointer(const_method))
            {
                break;
            }
            auto* const constants{ const_method->get_constants() };
            if (!constants || !vmhook::hotspot::is_valid_pointer(constants))
            {
                break;
            }

            std::string method_name{ caller_method->get_name() };
            if (method_name.empty())
            {
                break;
            }

            caller_info info{};
            info.method      = caller_method;
            info.method_name = std::move(method_name);
            info.signature   = caller_method->get_signature();

            // Class-name lookup is best-effort: walk the now-validated
            // ConstantPool -> _pool_holder -> Klass -> name chain, still gating
            // every dereference with is_valid_pointer so a stray pointer leaves
            // the class name empty instead of segfaulting.
            if (pool_holder_entry)
            {
                const auto* const slot{
                    reinterpret_cast<const std::uint8_t*>(constants) + pool_holder_entry->offset };
                if (vmhook::hotspot::is_valid_pointer(slot))
                {
                    auto* const klass{
                        *reinterpret_cast<vmhook::hotspot::klass* const*>(slot) };
                    if (klass && vmhook::hotspot::is_valid_pointer(klass))
                    {
                        if (auto* const name_symbol{ klass->get_name() };
                            name_symbol && vmhook::hotspot::is_valid_pointer(name_symbol))
                        {
                            info.class_name = name_symbol->to_string();
                        }
                    }
                }
            }

            frames.push_back(std::move(info));
            current_rbp_slot = caller_rbp;
        }

        return frames;
    }

    template<typename value_type>
    auto return_value::set_arg(const std::int32_t index, value_type&& value) noexcept
        -> bool
    {
        // The JVM spec limits max_locals to a u2 (65535).  Reject anything past
        // that up front: locals[-index] for an out-of-range index would walk
        // off the interpreter local-variable array and start corrupting
        // adjacent thread state (operand stack, saved registers, the frame
        // header) - exactly the kind of subtle wreckage that turns into a
        // post-uninject AV cascade in the JVM rather than a clean error
        // here.  65535 is generous; real methods rarely exceed a couple of
        // hundred locals.
        constexpr std::int32_t max_jvm_locals{ 0xFFFF };
        if (!this->stack_frame || index < 0 || index > max_jvm_locals)
        {
            VMHOOK_LOG("{} return_value::set_arg(index={}): missing stack_frame, negative index, "
                       "or index above JVM max_locals limit ({}) (stack_frame={}).",
                       vmhook::error_tag, index, max_jvm_locals,
                       static_cast<const void*>(this->stack_frame));
            return false;
        }

        void** const locals{ this->stack_frame->get_locals() };
        if (!locals)
        {
            VMHOOK_LOG("{} return_value::set_arg(index={}): frame.get_locals() returned null - "
                       "cannot mutate interpreter locals on this JDK.",
                       vmhook::error_tag, index);
            return false;
        }

        using clean_value_type = std::remove_cvref_t<value_type>;

        auto store_oop = [&](void* const oop)
            -> bool
        {
            // Interpreter local-variable slots always hold a full 64-bit
            // uncompressed oop, regardless of whether the heap uses compressed
            // oops (+UseCompressedOops, the default).  Compression applies to
            // oops stored *inside* objects on the heap, not to the GC roots in
            // an interpreter frame's local array.
            //
            // The previous code tried to infer the slot width from the bits
            // already present (compress when previous_bits <= 0xFFFFFFFF).  That
            // misfires whenever the slot legitimately held a wide value whose
            // high half was zero — most importantly a null slot (all zero) — and
            // wrote a narrow/compressed pointer into a wide slot, corrupting the
            // local.  Always store the wide pointer; storing nullptr wide is the
            // correct representation of a null local too.
            locals[-index] = oop;
            return true;
        };

        if constexpr (vmhook::detail::is_unique_object_ptr<clean_value_type>::value)
        {
            // Explicit base-class qualification reaches object_base::get_instance()
            // even when the derived wrapper shadows the name (e.g. with a same-named
            // static helper).  Avoids the static_cast<object_base*> which produces an
            // "incomplete type" warning under GCC at template-definition time.
            return store_oop(value ? value->vmhook::object_base::get_instance() : nullptr);
        }
        else if constexpr (std::is_base_of_v<vmhook::object_base, clean_value_type>)
        {
            return store_oop(value.get_instance());
        }
        else if constexpr (std::is_same_v<clean_value_type, std::string> || std::is_same_v<clean_value_type, std::string_view>)
        {
            // jni_new_string_utf returns a JNI local reference.  The OOP we
            // extract goes into the interpreter local-variable slot (a GC
            // root), so the String stays reachable after we release the
            // handle.  Skipping DeleteLocalRef would leak one local per call
            // on long-lived attached threads - HotSpot's default ref table
            // capacity is 16 and there's no implicit per-detour-frame
            // teardown, so the leak surfaces as JNI "local reference table
            // overflow" warnings after enough hot-path string injections.
            void* const string_handle{ vmhook::detail::jni_new_string_utf(value) };
            void* const string_oop{ string_handle
                ? vmhook::detail::jni_decode_object(string_handle)
                : vmhook::make_java_string(value) };
            if (!string_oop)
            {
                vmhook::detail::jni_delete_local_ref(string_handle);
                VMHOOK_LOG("{} return_value::set_arg(index={}): both JNI NewStringUTF and "
                           "make_java_string fallback failed - cannot inject a Java String.",
                           vmhook::error_tag, index);
                return false;
            }

            const bool stored{ store_oop(string_oop) };
            vmhook::detail::jni_delete_local_ref(string_handle);
            return stored;
        }
        else if constexpr (std::is_same_v<clean_value_type, const char*> || std::is_same_v<clean_value_type, char*>)
        {
            const std::string_view text{ value ? std::string_view{ value } : std::string_view{} };
            void* const string_handle{ vmhook::detail::jni_new_string_utf(text) };
            void* const string_oop{ string_handle
                ? vmhook::detail::jni_decode_object(string_handle)
                : vmhook::make_java_string(text) };
            if (!string_oop)
            {
                vmhook::detail::jni_delete_local_ref(string_handle);
                VMHOOK_LOG("{} return_value::set_arg(index={}): both JNI NewStringUTF and "
                           "make_java_string fallback failed for const char* arg.",
                           vmhook::error_tag, index);
                return false;
            }

            const bool stored{ store_oop(string_oop) };
            vmhook::detail::jni_delete_local_ref(string_handle);
            return stored;
        }
        else if constexpr (std::is_trivially_copyable_v<clean_value_type> && sizeof(clean_value_type) <= sizeof(void*))
        {
            void* raw{};
            std::memcpy(&raw, &value, sizeof(clean_value_type));
            // Two-word primitives (long/double, 8 bytes) span local slots [index]
            // and [index+1]; the interpreter reads the 64-bit value from the lower
            // slot locals[-(index+1)] (LOCALS_LONG / LOCALS_DOUBLE), so a long/double
            // arg injection must land there.  One-word primitives go to locals[-index].
            if constexpr (sizeof(clean_value_type) > sizeof(std::int32_t))
            {
                locals[-(index + 1)] = raw;
            }
            else
            {
                locals[-index] = raw;
            }
            return true;
        }
        else
        {
            VMHOOK_LOG("{} return_value::set_arg(index={}): unsupported value_type '{}' "
                       "(not trivially copyable or larger than a pointer).",
                       vmhook::error_tag, index, typeid(clean_value_type).name());
            return false;
        }
    }

    // --- Hooking --------------------------------------------------------------

    /*
        @brief Installs an interpreter hook on a Java method with typed C++ parameters.
        @tparam wrapper_type The C++ wrapper class registered for the Java class that owns the method.
                             Must have been registered via register_class<T>() beforehand.
        @param method_name  The name of the Java method to hook (e.g., "toString", "update").
        @param user_detour  A callable with signature:
                              void(vmhook::return_value& retval, T1 arg1, T2 arg2, ...)
                            where T1, T2, ... correspond to the Java method's explicit parameters:
                              - int / boolean / byte / short / char / float   std::int32_t / bool / ...
                              - long                                          std::int64_t
                              - double                                        double
                              - String                                        std::string (or const std::string&)
                              - Object reference                              std::unique_ptr<WrapperClass>
                                                                               (or const std::unique_ptr<...>&)
                            For instance methods the implicit Java 'this' occupies slot 0, so it must
                            appear as the first argument (typically std::unique_ptr<wrapper_type>).
                            For static methods there is no 'this'; parameters begin at slot 0.
                            Call retval.cancel() to suppress a void method body, or
                            retval.set(value) to suppress the original body and return value to Java.
        @return true if the hook was successfully installed or was already active, false on failure.
        @details
        The hook installation process:
        1. Retrieve the klass for the registered Java class via find_class().
        2. Walk the InstanceKlass::_methods array to locate the target method by name.
        3. Disable JIT compilation by setting NO_COMPILE in Method._access_flags
           and _dont_inline in Method._flags.
        4. Register the method and its detour in g_hooked_methods for dispatch by common_detour.
        5. If the method is already compiled, clear Method._code and restore the
           interpreted entry so future dispatch reaches the interpreter hook.
        6. Check whether the i2i entry point has already been patched; if so, reuse it.
        7. If the i2i entry is new, locate the injection point via find_hook_location(),
           allocate a trampoline via midi2i_hook, and register it in g_hooked_i2i_entries.

        @note Unlike the JNI/JVMTI version, this implementation does not force a class
              retransformation to flush existing inline caches. Hooking early is still best:
              compiled callers that already cached an nmethod can keep bypassing the hook
              until HotSpot repairs that call site at a safepoint.
        @see midi2i_hook, common_detour, set_dont_inline, NO_COMPILE, shutdown_hooks
    */
    // Forward declaration of the auto-repair watchdog spawner.  hook<T>()
    // calls this at the end of a successful install; the definition lives
    // alongside shutdown_hooks() further down because the worker_loop body
    // depends on verify_hooks() being defined first.  Declaring it here
    // makes it visible to GCC's first-phase name lookup inside the template.
    namespace detail::auto_repair { static auto ensure_started() noexcept -> void; }

    template<class wrapper_type>
    static auto hook(const std::string_view method_name,
                     const std::string_view method_signature,
                     auto&& user_detour) -> bool;

    template<class wrapper_type>
    static auto hook(const std::string_view method_name, auto&& user_detour)
        -> bool
    {
        return vmhook::hook<wrapper_type>(method_name, std::string_view{}, std::forward<decltype(user_detour)>(user_detour));
    }

    /*
        @brief Signature-filtered hook overload.
        @details
        Same behaviour as hook<T>(name, callback) but selects the method by
        matching both name and JVM descriptor.  Use when the target class
        has overloaded methods sharing the same name (e.g. ClassLoader's
        five `defineClass` variants).
    */
    template<class wrapper_type>
    static auto hook(const std::string_view method_name,
                     const std::string_view method_signature,
                     auto&& user_detour) -> bool
    {
        try
        {
            using traits_t = vmhook::detail::function_traits<std::remove_cvref_t<decltype(user_detour)>>;
            using all_args_tuple_t = typename traits_t::args_tuple_t;
            using method_arg_tuple_t = typename vmhook::detail::tuple_tail<all_args_tuple_t>::type_t;

            const auto type_map_entry{ vmhook::type_to_class_map.find(std::type_index{ typeid(wrapper_type) }) };
            if (type_map_entry == vmhook::type_to_class_map.end())
            {
                throw vmhook::exception{ std::format("Class not registered for type {}. Did you call register_class<wrapper_type>()?", typeid(wrapper_type).name()) };
            }

            vmhook::hotspot::klass* const target_klass{ vmhook::find_class(type_map_entry->second) };
            if (!target_klass)
            {
                throw vmhook::exception{ std::format("Class '{}' not found in JVM.", type_map_entry->second) };
            }

            const std::int32_t method_count{ target_klass->get_methods_count() };
            vmhook::hotspot::method** const methods_array{ target_klass->get_methods_ptr() };

            if (!methods_array || method_count <= 0)
            {
                throw vmhook::exception{ std::format("No methods found on class '{}'.", type_map_entry->second) };
            }

            vmhook::hotspot::method* found_method{ nullptr };
            for (std::int32_t method_index{ 0 }; method_index < method_count; ++method_index)
            {
                vmhook::hotspot::method* const method_ptr{ methods_array[method_index] };
                if (method_ptr && vmhook::hotspot::is_valid_pointer(method_ptr) && method_ptr->get_name() == method_name)
                {
                    if (method_signature.empty() || method_ptr->get_signature() == method_signature)
                    {
                        found_method = method_ptr;
                        break;
                    }
                }
            }

            if (!found_method)
            {
                throw vmhook::exception{ std::format("Method '{}' not found in class '{}'.", method_name, type_map_entry->second) };
            }

            // Serialise the entire install path behind g_hooked_methods_mutex.
            // Without this, two threads racing through vmhook::hook<T>() can
            // (a) both decide a duplicate-membership check passed, end up
            // installing the trampoline twice, OR (b) race the push_back
            // against another thread's iteration in common_detour, since the
            // detour reads g_hooked_methods lock-free.
            std::lock_guard<std::mutex> install_lock{ vmhook::hotspot::g_hooked_methods_mutex };

            for (const vmhook::hotspot::hooked_method& hooked_method_entry : vmhook::hotspot::g_hooked_methods)
            {
                if (hooked_method_entry.method == found_method)
                {
                    return true;
                }
            }

            vmhook::hotspot::set_dont_inline(found_method, true);

            std::uint32_t* const flags{ found_method->get_access_flags() };
            if (!flags)
            {
                throw vmhook::exception{ "Failed to retrieve access flags." };
            }
            *flags |= vmhook::hotspot::NO_COMPILE;

            // -- Snapshot original entry points before any modification ----------
            void* const i2i{ found_method->get_i2i_entry() };
            if (!i2i)
            {
                throw vmhook::exception{ "Failed to retrieve i2i entry." };
            }

            void* const original_code{ found_method->get_code() };
            void* const original_from_interpreted{ found_method->get_from_interpreted_entry() };
            void* const original_from_compiled{ found_method->get_from_compiled_entry() };
            const bool was_compiled{ original_code != nullptr && vmhook::hotspot::is_valid_pointer(original_code) };

            if (was_compiled)
            {
                VMHOOK_LOG("{} hook(): '{}' is JIT-compiled (_code=0x{:016X}) - deoptimising.", vmhook::info_tag, method_name, reinterpret_cast<std::uintptr_t>(original_code));
            }
            else
            {
                VMHOOK_LOG("{} hook(): '{}' is interpreted - patching i2i stub.", vmhook::info_tag, method_name);
            }

            // Wrap the user callable: extract typed Java args from the frame and forward them.
            auto wrapper_detour = [detour = std::forward<decltype(user_detour)>(user_detour)]
            (vmhook::hotspot::frame* const frame_pointer, vmhook::hotspot::java_thread*, vmhook::hotspot::return_slot* const slot)
                {
                    vmhook::return_value retval{ slot, frame_pointer };
                    // Per-arg JVM slot offsets, computed at template-instantiation
                    // time.  Each long / double consumes TWO slots in HotSpot's
                    // interpreter local-variable array, so passing the raw tuple
                    // index (0, 1, 2, ...) to extract_frame_arg reads garbage as
                    // soon as any long/double arg precedes a later arg.  See
                    // detail::java_slot_offsets for the table.
                    auto invoke = [&]<std::size_t... indexes>(std::index_sequence<indexes...>)
                    {
                        constexpr auto slot_offsets{
                            vmhook::detail::java_slot_offsets<method_arg_tuple_t>::value };
                        detour(retval,
                            vmhook::detail::extract_frame_arg<std::tuple_element_t<indexes, method_arg_tuple_t>>(
                                frame_pointer, slot_offsets[indexes])...);
                    };
                    invoke(std::make_index_sequence<std::tuple_size_v<method_arg_tuple_t>>{});
                };

            // Snapshot the method's identity for verify_hooks' drift detector.
            // Stored as std::string (owned) so we can compare against
            // current method->get_name() etc. without worrying about the
            // backing JVM symbols outliving the hook.
            vmhook::hotspot::hooked_method hm_entry{
                found_method,
                std::move(wrapper_detour),
                original_code,
                original_from_interpreted,
                original_from_compiled,
                was_compiled };
            hm_entry.expected_class_name = type_map_entry->second;
            hm_entry.expected_method_name = std::string{ method_name };
            hm_entry.expected_signature = found_method->get_signature();
            vmhook::hotspot::g_hooked_methods.push_back(std::move(hm_entry));

            // -- Install (or reuse) the i2i stub patch ---------------------------
            bool i2i_already_patched{ false };
            for (const vmhook::hotspot::i2i_hook_data& hook_data_entry : vmhook::hotspot::g_hooked_i2i_entries)
            {
                if (hook_data_entry.i2i_entry == i2i)
                {
                    i2i_already_patched = true;
                    break;
                }
            }

            if (!i2i_already_patched)
            {
                std::uint8_t* const target{ reinterpret_cast<std::uint8_t*>(vmhook::hotspot::find_hook_location(i2i)) };
                if (!target)
                {
                    throw vmhook::exception{ "Failed to find hook location in i2i stub." };
                }

                // -- Hook chaining: detect prior i2i-patching DLLs --------------
                // HotSpot's i2i stub is shared across many Methods, so any other
                // DLL that patches it at the same find_hook_location injection
                // point will end up writing a 5-byte JMP (0xE9 + rel32) over
                // ours and we go silent.  If we see that JMP already in place
                // before our install, we follow the rel32 to compute the prior
                // hook's trampoline entry and pass it as our chain_resume:
                // our trampoline will run our detour, then JMP into the prior
                // hooker's trampoline, which finishes its work and resumes at
                // `target + HOOK_SIZE`.  Both hooks fire; neither is lost.
                //
                // This handles the "other DLL was injected first" case.  The
                // reverse case ("other DLL injects after us") is not solved
                // here - they'd just overwrite our JMP unless they also do
                // chain detection.  See the README compatibility note for a
                // discussion of options if you hit that.
                void* chain_resume{ nullptr };
                constexpr std::uint8_t JMP_OPCODE{ 0xE9 };
                if (target[0] == JMP_OPCODE)
                {
                    const std::int32_t rel{ *reinterpret_cast<const std::int32_t*>(target + 1) };
                    std::uint8_t* const prior_trampoline{ target + 5 + rel };
                    if (vmhook::hotspot::is_valid_pointer(prior_trampoline))
                    {
                        chain_resume = prior_trampoline;
                        VMHOOK_LOG("{} hook(): injection point at 0x{:016X} already JMP'd by another hooker "
                                   "to 0x{:016X}; chaining our hook in front of theirs.",
                                   vmhook::info_tag,
                                   reinterpret_cast<std::uintptr_t>(target),
                                   reinterpret_cast<std::uintptr_t>(prior_trampoline));
                    }
                    else
                    {
                        VMHOOK_LOG("{} hook(): injection point at 0x{:016X} starts with JMP but the target "
                                   "0x{:016X} doesn't pass is_valid_pointer - installing without chain (the "
                                   "other hooker's hook will be lost).",
                                   vmhook::warning_tag,
                                   reinterpret_cast<std::uintptr_t>(target),
                                   reinterpret_cast<std::uintptr_t>(prior_trampoline));
                    }
                }

                vmhook::hotspot::midi2i_hook* const hook_instance{
                    new vmhook::hotspot::midi2i_hook(target, vmhook::hotspot::common_detour, chain_resume) };
                if (hook_instance->has_error())
                {
                    delete hook_instance;
                    throw vmhook::exception{ "midi2i_hook installation failed." };
                }

                vmhook::hotspot::g_hooked_i2i_entries.push_back({ i2i, hook_instance });
            }

            // -- Deoptimise JIT-compiled methods ---------------------------------
            // Problem:  when _code != nullptr, _from_interpreted_entry points to the i2c
            //           adapter (not the i2i stub), so calls bypass our patch entirely.
            // Fix:      null _code and reset the interpreted entry so the JVM dispatches
            //           through the interpreter - and therefore through our patched i2i stub.
            // Limitation: compiled callers with stale monomorphic inline caches still call
            //             the old nmethod directly.  Those caches will be repaired the next
            //             time HotSpot reaches a safe point and re-evaluates the IC.
            //
            // c2i adapter recovery: JDK 8 exports Method::_adapter via
            // VMStructs; JDK 9+ removed it.  Method::get_adapter() now
            // falls back to a heuristic scan that recovers the same
            // pointer on JDK 17/21/24/25 (see detect_adapter_offset_from_method).
            // If that scan ALSO fails (no JDK has been observed in this
            // state, but we handle it conservatively), we skip the
            // deoptimisation entirely rather than leave the JVM in an
            // inconsistent state where _code == nullptr but
            // _from_compiled_entry still points into the (now stale)
            // nmethod — that combination crashes at the next safepoint.
            if (was_compiled)
            {
                void* const adapter{ found_method->get_adapter() };
                void* const c2i_entry{ vmhook::hotspot::get_c2i_entry_from_adapter(adapter) };

                if (c2i_entry && vmhook::hotspot::is_valid_pointer(c2i_entry))
                {
                    // 1. Redirect interpreted callers to the (now-patched) i2i stub.
                    found_method->set_from_interpreted_entry(i2i);
                    // 2. Redirect compiled callers through the c2i adapter -> interpreter -> i2i stub.
                    found_method->set_from_compiled_entry(c2i_entry);
                    // 3. Clear _code last so the above entry-point writes are visible first.
                    found_method->set_code(nullptr);
                    VMHOOK_LOG("{} hook():   deopt complete - _from_compiled_entry -> c2i @ 0x{:016X}, _code cleared.",
                               vmhook::info_tag, reinterpret_cast<std::uintptr_t>(c2i_entry));
                }
                else
                {
                    // c2i adapter unrecoverable.  The upstream-conservative
                    // policy is to leave _code intact and accept that compiled
                    // callers bypass the hook, but that defeats the entire
                    // point of installing the hook in the first place for
                    // hot methods like runTick.  Match the older vmhook
                    // behaviour: redirect the interpreted entry to the i2i
                    // stub and clear _code so subsequent dispatch falls back
                    // through the interpreter and hits the hook.  Compiled
                    // callers with stale inline caches still reach the old
                    // nmethod for one cycle, but the cache is repaired at
                    // the next safepoint when the IC re-resolves and finds
                    // _code == nullptr.
                    found_method->set_from_interpreted_entry(i2i);
                    found_method->set_code(nullptr);
                    VMHOOK_LOG("{} hook():   c2i adapter unrecoverable for '{}'; forced deopt - _code cleared, "
                               "_from_interpreted_entry redirected to i2i.  Compiled inline caches will repair "
                               "themselves at the next safepoint.", vmhook::warning_tag, method_name);
                }
            }

            // Spawn the auto-repair watchdog on first successful install
            // so hooks keep firing even when HotSpot eventually re-JITs
            // them despite our NO_COMPILE flag.  No-op on subsequent calls.
            vmhook::detail::auto_repair::ensure_started();

            return true;
        }
        catch (const std::exception& exception)
        {
            VMHOOK_LOG("{} vmhook::hook() for {}: {}", vmhook::error_tag, method_name, exception.what());
            return false;
        }
    }

    /*
        @brief Removes all active interpreter hooks and leaves the JVM in a
               safe, consistent post-hook state.
        @details
        Phase 1: Sets g_shutdown_requested so any in-flight common_detour
                 returns immediately.
        Phase 2: Deletes each midi2i_hook instance, restoring the original
                 bytes at the i2i target and freeing the trampoline page.
        Phase 3: Clears _dont_inline and NO_COMPILE so the JIT can recompile.

        Note: hooks that were JIT-compiled at install-time are deliberately
        NOT re-linked to their original nmethod here.  Earlier versions
        wrote the saved original_code back into Method::_code, but the
        nmethod sweeper may have flushed that nmethod during the hook's
        lifetime, so the restored pointer can be dangling.  Subsequent
        dispatch hits freed code-cache memory and crashes the JVM (the
        symptom observed by users on uninject was a cascade of 0xC0000005
        in the 0x10000000-0x10FFFFFF range, i.e. inside the code cache).
        Leaving _code = nullptr keeps the method in the
        installed-but-deopted state, which is identical to a freshly
        loaded never-compiled method — a configuration HotSpot already
        handles correctly.  If the method gets hot again the JIT will
        recompile from scratch.
    */
    /*
        @brief Verifies every installed hook's 5-byte JMP is still at the
               injection point; re-installs any that have been overwritten.
        @details
        Call this from your main loop on a sane cadence (e.g. every 100 ms).
        It detects the case where another i2i-patching DLL was injected
        AFTER us and stomped our patch in the shared HotSpot interpreter
        stub, then re-applies our JMP — with chain detection, so if their
        new JMP is still there we now chain in front of theirs and both
        hooks fire.

        Returns the number of hooks that needed repair.  Returns 0 on the
        common path (everything intact); a non-zero value means one or
        more hooks were overwritten and just got restored.

        Designed for low cost when everything is fine: a memcmp of 5 bytes
        per installed hook plus the mutex acquire.  Only does writes when
        a hook is actually broken.

        Race: the re-write is not atomic across the 5 bytes.  See
        midi2i_hook::verify_and_repair for the analysis - in short, byte 0
        stays 0xE9 (JMP opcode) before and after, so the worst case is
        a wrong-rel32 destination for a few-nanosecond window.  In practice
        the only time this matters is if some other hooker is rewriting
        the same bytes simultaneously, in which case you're already in
        a war and tearing is the least of your problems.
    */
    static auto verify_hooks() noexcept
        -> std::size_t
    {
        std::size_t repaired{ 0 };
        try
        {
            std::lock_guard<std::mutex> lock{ vmhook::hotspot::g_hooked_methods_mutex };
            for (const vmhook::hotspot::i2i_hook_data& entry : vmhook::hotspot::g_hooked_i2i_entries)
            {
                if (entry.hook && !entry.hook->verify_and_repair())
                {
                    ++repaired;
                }
            }

            // Re-install a hooked_method whose Method* has gone stale.
            // The shared-i2i patch we put in at install time is still in
            // place and still routes calls to common_detour; the only
            // broken link is that common_detour compares the live
            // frame.method against our stored hm.method, and our stored
            // pointer is now pointing at freed-or-aliased memory.  Look
            // up the *current* Method on its class by name+signature and
            // swap our pointer.  Re-apply the JIT inhibitors so the new
            // Method doesn't get re-compiled out from under us.
            //
            // The shared-i2i patch is per-i2i-stub (one trampoline serves
            // every method routed through that stub), so no allocation
            // happens here - pointer update + flag bits only.
            const auto try_reinstall = [](vmhook::hotspot::hooked_method& hm_to_repair) noexcept -> bool
            {
                try
                {
                    vmhook::hotspot::klass* const new_klass{ vmhook::find_class(hm_to_repair.expected_class_name) };
                    if (!new_klass)
                    {
                        return false;
                    }

                    const std::int32_t method_count{ new_klass->get_methods_count() };
                    vmhook::hotspot::method** const methods_array{ new_klass->get_methods_ptr() };
                    if (!methods_array || method_count <= 0)
                    {
                        return false;
                    }

                    vmhook::hotspot::method* new_method{ nullptr };
                    for (std::int32_t i{ 0 }; i < method_count; ++i)
                    {
                        vmhook::hotspot::method* const m{ methods_array[i] };
                        if (m && vmhook::hotspot::is_valid_pointer(m)
                            && m->get_name() == hm_to_repair.expected_method_name
                            && (hm_to_repair.expected_signature.empty()
                                || m->get_signature() == hm_to_repair.expected_signature))
                        {
                            new_method = m;
                            break;
                        }
                    }
                    if (!new_method)
                    {
                        return false;
                    }

                    // Pointer assignment is atomic on x86_64; common_detour's
                    // lock-free read picks up the new value next dispatch.
                    const vmhook::hotspot::method* const old_method{ hm_to_repair.method };
                    hm_to_repair.method = new_method;

                    // Re-apply the JIT inhibitors so the new Method holds.
                    vmhook::hotspot::set_dont_inline(new_method, true);
                    if (std::uint32_t* const flags{ new_method->get_access_flags() })
                    {
                        *flags |= vmhook::hotspot::NO_COMPILE;
                    }

                    // If the new method is already JIT-compiled, deopt it
                    // the same way the install path does.  c2i recovery is
                    // best-effort; on failure we still clear _code so the
                    // method falls back to interpreted dispatch through
                    // the (still-patched) shared i2i.
                    void* const code_now{ new_method->get_code() };
                    if (code_now != nullptr && vmhook::hotspot::is_valid_pointer(code_now))
                    {
                        void* const adapter{ new_method->get_adapter() };
                        void* const c2i_entry{ vmhook::hotspot::get_c2i_entry_from_adapter(adapter) };
                        if (c2i_entry && vmhook::hotspot::is_valid_pointer(c2i_entry))
                        {
                            new_method->set_from_compiled_entry(c2i_entry);
                        }
                        // Restore the interpreted entry to the i2i stub, mirroring
                        // the install path.  Re-anchoring onto a freshly-resolved
                        // Method whose _code is set means _from_interpreted_entry
                        // points at the i2c adapter; clearing _code below without
                        // this leaves interpreted dispatch bypassing the detour, so
                        // the re-installed hook would never fire.
                        if (void* const i2i{ new_method->get_i2i_entry() })
                        {
                            new_method->set_from_interpreted_entry(i2i);
                        }
                        new_method->set_code(nullptr);
                    }

                    // Re-arm drift detection so the next time anything
                    // changes we notice and log again.
                    hm_to_repair.drift_logged = false;

                    VMHOOK_LOG("{} verify_hooks: re-installed hook for '{}.{}{}': old Method* 0x{:016X} -> 0x{:016X}.",
                               vmhook::info_tag,
                               hm_to_repair.expected_class_name,
                               hm_to_repair.expected_method_name,
                               hm_to_repair.expected_signature,
                               reinterpret_cast<std::uintptr_t>(old_method),
                               reinterpret_cast<std::uintptr_t>(new_method));
                    return true;
                }
                catch (...)
                {
                    return false;
                }
            };

            // Per-Method drift detector.  Three distinct "hook silently
            // stopped firing" failure modes that we surface here, in
            // order of severity:
            //
            //   1. Method freed (ConstMethod null).  The JVM unloaded the
            //      class that owned this Method; our stored Method*
            //      points at uninitialised slab memory.  Hook is dead.
            //   2. Method aliased (name differs from install-time name).
            //      The JVM freed the original Method and reused the same
            //      address for an unrelated method (Lunar / Forge with
            //      JVMTI RedefineClasses does this a lot).  common_detour
            //      now matches calls to the WRONG Java method, or - more
            //      commonly - never matches the new live Method at all.
            //      Hook is dead in both cases.
            //   3. NO_COMPILE cleared or _code repopulated.  The JVM
            //      JIT-recompiled our hooked method behind our back.
            //      Hook still fires for interpreted callers but compiled
            //      callers bypass it.
            //
            // One-shot per hook (drift_logged) to keep the log readable,
            // and we read the captured expected_* fields for the log so
            // even mode-1 freed-Method cases can name the original hook
            // ("orientCamera" vs the unhelpful raw pointer).
            for (vmhook::hotspot::hooked_method& hm : vmhook::hotspot::g_hooked_methods)
            {
                if (!hm.method || hm.drift_logged)
                {
                    continue;
                }

                // Mode 1: ConstMethod gone -> Method was freed.  Skip
                // get_name() (which would log "ConstMethod is nullptr"
                // every tick from inside its try/catch) and report from
                // the captured names instead.
                const vmhook::hotspot::const_method* const const_method{ hm.method->get_const_method() };
                if (!const_method || !vmhook::hotspot::is_valid_pointer(const_method))
                {
                    VMHOOK_LOG("{} verify_hooks: hook for '{}.{}{}' has been FREED - the JVM unloaded "
                               "the class that owned this Method (typically caused by another DLL "
                               "calling JVMTI RedefineClasses / RetransformClasses).",
                               vmhook::error_tag,
                               hm.expected_class_name,
                               hm.expected_method_name,
                               hm.expected_signature);
                    hm.drift_logged = true;
                    if (try_reinstall(hm))
                    {
                        ++repaired;
                    }
                    continue;
                }

                // Mode 2: Method* address aliased with a different Method.
                const std::string current_name{ hm.method->get_name() };
                if (!current_name.empty() && current_name != hm.expected_method_name)
                {
                    VMHOOK_LOG("{} verify_hooks: hook for '{}.{}{}' now points at a DIFFERENT method "
                               "named '{}' - the original Method was freed by class redefinition and "
                               "the allocator reused the address.",
                               vmhook::error_tag,
                               hm.expected_class_name,
                               hm.expected_method_name,
                               hm.expected_signature,
                               current_name);
                    hm.drift_logged = true;
                    if (try_reinstall(hm))
                    {
                        ++repaired;
                    }
                    continue;
                }

                // Mode 3: same Method, but JIT state drifted.
                //
                // HotSpot's tiered compiler can re-populate Method::_code
                // long after our install-time deopt, even though we set
                // NO_COMPILE and _dont_inline.  Triggers we have observed:
                // another JVMTI agent enabling compilation events,
                // safepoint cleanup clearing our flags, or the JIT racing
                // an in-flight compilation through the policy gate before
                // it sees NO_COMPILE.  The symptom users hit is "the hook
                // worked, then after a while stopped firing" - interpreted
                // callers still hit the i2i patch, but every compiled
                // caller sails past it into the regenerated nmethod.
                //
                // Repair = redo exactly what the install path does: re-arm
                // both JIT inhibitors, redirect _from_compiled_entry to
                // the c2i adapter so compiled callers fall back through
                // the interpreter (and therefore through our i2i patch),
                // then clear _code last so the entry-point write is
                // visible first.  drift_logged is used as a debounce so
                // we log once per drift event, not once per repair pass.
                void* const code_now{ hm.method->get_code() };
                std::uint32_t* const flags_now{ hm.method->get_access_flags() };
                const bool no_compile_set{ flags_now && (*flags_now & vmhook::hotspot::NO_COMPILE) != 0 };
                const bool jit_drifted{ code_now != nullptr || !no_compile_set };

                if (jit_drifted)
                {
                    if (!hm.drift_logged)
                    {
                        VMHOOK_LOG("{} verify_hooks: hook for '{}.{}{}' JIT-state drifted "
                                   "(Method::_code=0x{:016X}, NO_COMPILE={}) - re-applying deopt.",
                                   vmhook::warning_tag,
                                   hm.expected_class_name,
                                   hm.expected_method_name,
                                   hm.expected_signature,
                                   reinterpret_cast<std::uintptr_t>(code_now),
                                   no_compile_set ? "set" : "cleared");
                        hm.drift_logged = true;
                    }

                    vmhook::hotspot::set_dont_inline(hm.method, true);
                    if (flags_now)
                    {
                        *flags_now |= vmhook::hotspot::NO_COMPILE;
                    }

                    if (code_now != nullptr && vmhook::hotspot::is_valid_pointer(code_now))
                    {
                        void* const adapter{ hm.method->get_adapter() };
                        void* const c2i_entry{ vmhook::hotspot::get_c2i_entry_from_adapter(adapter) };
                        if (c2i_entry && vmhook::hotspot::is_valid_pointer(c2i_entry))
                        {
                            hm.method->set_from_compiled_entry(c2i_entry);
                        }
                        // Redirect interpreted callers back through the patched
                        // i2i stub, exactly as the install path does (see the
                        // set_from_interpreted_entry(i2i) at hook() install).
                        // When HotSpot re-JITs and then this repair clears _code,
                        // _from_interpreted_entry can still point at the i2c
                        // adapter from the compiled epoch; without restoring it to
                        // the i2i stub even interpreted dispatch sails past the
                        // detour and the hook silently dies after the re-JIT.
                        if (void* const i2i{ hm.method->get_i2i_entry() })
                        {
                            hm.method->set_from_interpreted_entry(i2i);
                        }
                        hm.method->set_code(nullptr);
                    }

                    ++repaired;
                }
                else
                {
                    // Clean steady state.  Reset debounce so the next
                    // drift event is logged again.
                    hm.drift_logged = false;
                }
            }
        }
        catch (...)
        {
            // No-throw contract: callers run this from hot loops.
        }
        return repaired;
    }

    // -------------------------------------------------------------------
    // Auto-repair watchdog.
    //
    // Spawned on the first successful hook<T>() call (idempotent across
    // TUs via g_started CAS).  Periodically calls verify_hooks() to
    // repair JIT-state drift before users notice their hooks have
    // silently stopped firing.  Without this, HotSpot's tiered compiler
    // eventually re-populates Method::_code on every hot hooked method,
    // and compiled callers sail past our i2i patch into the regenerated
    // nmethod - "everything worked, then after an hour it stopped".
    //
    // Controls:
    //   VMHOOK_DISABLE_AUTO_REPAIR        — define to opt out entirely.
    //   VMHOOK_AUTO_REPAIR_INTERVAL_MS    — repair-pass interval (default 1000ms).
    //
    // Lifetime: the thread is detached after spawning so the std::thread
    // destructor never calls terminate() at program exit even if the
    // user forgets to call shutdown_hooks().  shutdown_hooks() raises
    // g_shutdown_requested and notifies the cv before taking the install
    // mutex; the watchdog observes the flag at its next wake (within
    // VMHOOK_AUTO_REPAIR_INTERVAL_MS) and returns.
    // -------------------------------------------------------------------
    namespace detail::auto_repair
    {
        inline std::atomic<bool>        g_started{ false };
        inline std::atomic<bool>        g_watchdog_running{ false };
        inline std::mutex               g_cv_mutex{};
        inline std::condition_variable  g_cv{};

        static auto worker_loop() noexcept -> void
        {
            constexpr auto interval{ std::chrono::milliseconds{
#ifdef VMHOOK_AUTO_REPAIR_INTERVAL_MS
                VMHOOK_AUTO_REPAIR_INTERVAL_MS
#else
                1000
#endif
            } };

            g_watchdog_running.store(true, std::memory_order_release);

            for (;;)
            {
                {
                    std::unique_lock<std::mutex> lk{ g_cv_mutex };
                    g_cv.wait_for(lk, interval, []() noexcept
                    {
                        return vmhook::hotspot::g_shutdown_requested.load(std::memory_order_acquire);
                    });
                }
                if (vmhook::hotspot::g_shutdown_requested.load(std::memory_order_acquire))
                {
                    g_watchdog_running.store(false, std::memory_order_release);
                    return;
                }
                try
                {
                    (void)vmhook::verify_hooks();
                }
                catch (...)
                {
                    // verify_hooks already swallows internally; this is belt-and-braces.
                }
            }
        }

        // Bounded wait for the watchdog to exit after a shutdown request.  MUST
        // run WITHOUT g_hooked_methods_mutex held — the watchdog may be in
        // verify_hooks() (which takes that mutex), so waiting under it deadlocks.
        static auto wait_for_exit() noexcept -> void
        {
            for (int spins{ 0 };
                 g_watchdog_running.load(std::memory_order_acquire) && spins < 2000;
                 ++spins)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds{ 1 });
            }
        }

        static auto ensure_started() noexcept -> void
        {
#ifndef VMHOOK_DISABLE_AUTO_REPAIR
            bool expected{ false };
            if (!g_started.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
            {
                return;
            }
            if (vmhook::hotspot::g_shutdown_requested.load(std::memory_order_acquire))
            {
                // Library has already been torn down; do not resurrect.
                return;
            }
            try
            {
                std::thread worker{ &vmhook::detail::auto_repair::worker_loop };
                worker.detach();
            }
            catch (...)
            {
                // std::thread can throw on resource exhaustion.  Roll the
                // started flag back so a later install call may retry.
                g_started.store(false, std::memory_order_release);
            }
#endif
        }

        static auto notify_shutdown() noexcept -> void
        {
#ifndef VMHOOK_DISABLE_AUTO_REPAIR
            try
            {
                // Take-and-release the cv mutex so the watchdog's wait
                // cannot miss our notify_all - any thread that is about
                // to call wait_for will observe shutdown_requested in the
                // predicate after we release.
                {
                    std::lock_guard<std::mutex> lk{ g_cv_mutex };
                }
                g_cv.notify_all();
            }
            catch (...)
            {
                // Best-effort wakeup; thread will also exit at its next
                // wait_for() timeout if the notify is lost.
            }
#endif
        }
    }

    // Forward declaration of the watcher-latch reset helper.  shutdown_hooks()
    // calls this to make the on_class_loaded / on_exception detours
    // re-installable after a teardown; the definition lives further down,
    // alongside on_exception(), because it touches detail::class_load_*/
    // detail::exception_* which are declared only there (vmhook.hpp:~16341 /
    // ~16506).  Declaring it here makes it visible to GCC's first-phase name
    // lookup inside shutdown_hooks().
    namespace detail { static auto reset_watcher_latches() noexcept -> void; }

    static auto shutdown_hooks() noexcept
        -> void
    {
        // Flip the shutdown flag BEFORE taking the install mutex.  Any
        // common_detour that fires between here and the lock-acquire below
        // observes the flag and returns immediately - so we won't iterate
        // g_hooked_methods while it's being cleared further down.
        vmhook::hotspot::g_shutdown_requested.store(true, std::memory_order_release);

        // Wake the auto-repair watchdog so it observes the shutdown flag
        // immediately instead of waiting out its next interval.  Done
        // before we take the install mutex - the watchdog also acquires
        // that mutex inside verify_hooks(), so notifying first avoids a
        // brief lock-held wait.
        vmhook::detail::auto_repair::notify_shutdown();

        // Wait for the watchdog to actually exit BEFORE taking the teardown
        // lock, so shutdown is reversible without spawning a duplicate watchdog:
        // we clear g_shutdown_requested / g_started at the end so a later
        // hook<T>() is fully live again, and that's only safe once the old
        // watchdog has exited (otherwise it keeps looping when we flip the flag
        // back to false, and the next install spawns a second one).  Must run
        // OUTSIDE g_hooked_methods_mutex — the watchdog can be mid-verify_hooks()
        // holding that mutex.
        vmhook::detail::auto_repair::wait_for_exit();

        // Lock for the whole tear-down so no in-flight vmhook::hook<T>() call
        // can push a new entry between our trampoline-delete loop and our
        // entry-restore loop.  Once the lock is released, common_detour
        // observes an empty list and stops dispatching.
        std::lock_guard<std::mutex> shutdown_lock{ vmhook::hotspot::g_hooked_methods_mutex };

        for (const vmhook::hotspot::i2i_hook_data& hook_data_entry : vmhook::hotspot::g_hooked_i2i_entries)
        {
            delete hook_data_entry.hook;
        }

        for (const vmhook::hotspot::hooked_method& hooked_method_entry : vmhook::hotspot::g_hooked_methods)
        {
            // Skip a Method* that was freed / aliased before teardown (a JVMTI
            // RedefineClasses between install and shutdown) — set_dont_inline /
            // get_access_flags deref it unconditionally, so restoring flags on a
            // stale slot would AV or scribble into a recycled Method.
            if (!hooked_method_entry.method
                || !vmhook::hotspot::is_valid_pointer(hooked_method_entry.method)
                || !hooked_method_entry.method->get_const_method())
            {
                continue;
            }

            vmhook::hotspot::set_dont_inline(hooked_method_entry.method, false);

            std::uint32_t* const flags{ hooked_method_entry.method->get_access_flags() };
            if (flags)
            {
                *flags &= static_cast<std::uint32_t>(~vmhook::hotspot::NO_COMPILE);
            }

            // DELIBERATELY no _code / _from_compiled_entry / _from_interpreted_entry
            // restoration for was_compiled methods.
            //
            // The original_code we saved at install time is a pointer to the
            // nmethod that was active back then.  Once we set _code = nullptr
            // (the install-time deopt), HotSpot's nmethod sweeper considers
            // that nmethod unreachable and eventually flushes it - the user
            // observes "Nmethod flushes (20 events)" in every hs_err log past
            // a few seconds of runtime.  Writing the now-stale pointer back
            // into _code and _from_compiled_entry at shutdown gives the JVM
            // a dangling pointer; any subsequent call into the method (which
            // happens constantly during JVM teardown - the nmethod sweeper,
            // safepoint cleanup, finalizer dispatch, etc.) jumps into freed
            // code-cache memory and access-violates somewhere in the 0x10??????
            // range.  That was the post-uninject crash cascade.
            //
            // Leaving the method in the install-time deopted state instead
            // (_code = nullptr, _from_interpreted_entry = original i2i now
            // unpatched by ~midi2i_hook, _from_compiled_entry = c2i adapter
            // we installed) is internally consistent and safe: the method
            // looks "not currently compiled" to HotSpot, which is its
            // earliest steady-state and a configuration the JVM is
            // already designed to handle for every freshly-loaded method.
            // If the method gets hot again the JIT will recompile it fresh.
        }

        vmhook::hotspot::g_hooked_methods.clear();
        vmhook::hotspot::g_hooked_i2i_entries.clear();

        // Make shutdown_hooks() REVERSIBLE.  Previously g_shutdown_requested was
        // latched true forever: after a teardown, a fresh vmhook::hook<T>() would
        // return true but common_detour's early-out on the flag meant the detour
        // never fired — silently dead.  The mod-loader "tear down + re-init on
        // world switch" pattern hits exactly this, and so did the integration
        // suite (every test after the first shutdown_hooks() observed its hook
        // not firing once the call_jni crash that masked it was fixed).  Clear
        // both latches so a subsequent install is fully live again; the
        // wait_for_exit() above guarantees the old watchdog is gone first.
        vmhook::detail::auto_repair::g_started.store(false, std::memory_order_release);
        vmhook::hotspot::g_shutdown_requested.store(false, std::memory_order_release);

        // Same "latched-forever" class of bug as the two stores above, but for
        // the high-level watchers.  shutdown_hooks() clears g_hooked_methods,
        // which removed the ClassLoader.defineClass and Throwable.fillInStackTrace
        // detours that on_class_loaded()/on_exception() installed — yet their
        // install latches (detail::class_load_hook_installed / exception_hook_installed)
        // stayed true.  A later on_class_loaded()/on_exception() then saw the stale
        // latch, SKIPPED re-installing the (now-gone) detour, and handed back a
        // live-looking watch_handle whose callback could never fire.  Reset both
        // latches and drop the stale callback lists so a post-shutdown re-arm
        // installs a firing detour again.
        vmhook::detail::reset_watcher_latches();
    }

    inline auto hook_handle::stop() noexcept -> void
    {
        if (!this->method)
        {
            return;
        }
        vmhook::hotspot::method* const target{ this->method };
        this->method = nullptr;

        try
        {
            // Single-entry removal contends with vmhook::hook<T>() installs and
            // with shutdown_hooks() on the same vector.  Lock for the whole
            // erase + restore so common_detour can never iterate while we are
            // mid-mutation.
            std::lock_guard<std::mutex> stop_lock{ vmhook::hotspot::g_hooked_methods_mutex };

            auto& hooks{ vmhook::hotspot::g_hooked_methods };
            const auto entry_it{ std::find_if(hooks.begin(), hooks.end(),
                [target](const vmhook::hotspot::hooked_method& h) noexcept
                {
                    return h.method == target;
                }) };
            if (entry_it == hooks.end())
            {
                return;
            }

            // Restore the method's original behaviour.  Mirrors
            // shutdown_hooks() but for one entry.  Leaving the
            // midi2i_hook trampoline installed is deliberate — other
            // hooks may still share the same i2i entry point, and
            // common_detour will simply skip over methods missing
            // from g_hooked_methods.
            vmhook::hotspot::set_dont_inline(entry_it->method, false);
            if (std::uint32_t* const flags{ entry_it->method->get_access_flags() })
            {
                *flags &= static_cast<std::uint32_t>(~vmhook::hotspot::NO_COMPILE);
            }
            // No _code / _from_compiled_entry / _from_interpreted_entry
            // restoration here either - see the long comment in
            // shutdown_hooks() for why.  Short version: the nmethod we
            // captured at install time may have been flushed by the JVM
            // sweeper in the meantime, so restoring _code to that
            // pointer hands the JVM a dangling code-cache address and
            // every subsequent dispatch to the method AVs in 0x10??????.

            hooks.erase(entry_it);
        }
        catch (const std::exception& ex)
        {
            VMHOOK_LOG("{} hook_handle removal: exception while uninstalling a single hook: {} - "
                       "swallowed because the user is dropping a handle and removal must not throw.",
                       vmhook::error_tag, ex.what());
        }
        catch (...)
        {
            VMHOOK_LOG("{} hook_handle removal: unknown exception while uninstalling a single hook - "
                       "swallowed because the user is dropping a handle and removal must not throw.",
                       vmhook::error_tag);
        }
    }

    /*
        @brief Hooks the method on T selected by JVM descriptor alone.
        @details
        The obfuscated-build entry point: when the method NAME rotates per
        client/build but the descriptor is stable, you cannot call
        hook<T>(name, ...) because you don't have the name.  This resolves the
        name from the descriptor (via find_methods_by_signature<T>) and installs
        the signature-filtered hook<T>(name, descriptor, detour).

        Multi-match policy: if more than one method on T shares the descriptor,
        this REFUSES the install (logs + returns false) rather than silently
        hooking an arbitrary overload — disambiguate by name in that case.

            vmhook::hook_by_signature<net_handler>(
                "()Ljava/util/Collection;", &on_player_info_map);

        @return true on a successful single-match install; false if no method
                (or more than one) matches, or the install itself fails.
    */
    template<class wrapper_type>
    inline auto hook_by_signature(const std::string_view jvm_descriptor,
                                  auto&&                  user_detour) -> bool
    {
        const auto names{ vmhook::find_methods_by_signature<wrapper_type>(jvm_descriptor) };
        if (names.empty())
        {
            VMHOOK_LOG("{} hook_by_signature<{}>('{}'): no method matches that descriptor "
                       "(class not loaded / not registered, or the descriptor is wrong).",
                       vmhook::error_tag, typeid(wrapper_type).name(), jvm_descriptor);
            return false;
        }
        if (names.size() > 1)
        {
            VMHOOK_LOG("{} hook_by_signature<{}>('{}'): {} methods share that descriptor - "
                       "refusing to guess.  Hook by name with hook<T>(name, descriptor, detour).",
                       vmhook::error_tag, typeid(wrapper_type).name(), jvm_descriptor, names.size());
            return false;
        }
        return vmhook::hook<wrapper_type>(names.front(), jvm_descriptor,
                                          std::forward<decltype(user_detour)>(user_detour));
    }

    /*
        @brief RAII-style hook install: same as vmhook::hook<T>() but
               returns a hook_handle that uninstalls just this hook on
               destruction.
        @details
        Use this when you want a hook bound to a C++ scope rather than
        persisting until shutdown_hooks().  See class hook_handle for
        thread-safety caveats around concurrent in-flight callbacks.

        @return  A hook_handle whose installed() is true on success and
                 false on failure.  Failure modes (class not found,
                 method not found, etc.) match vmhook::hook<T>.
    */
    template<class wrapper_type>
    inline auto scoped_hook(const std::string_view method_name,
                            const std::string_view method_signature,
                            auto&&                 user_detour) -> hook_handle
    {
        if (!vmhook::hook<wrapper_type>(method_name, method_signature,
                                         std::forward<decltype(user_detour)>(user_detour)))
        {
            VMHOOK_LOG("{} scoped_hook<{}>('{}{}'): underlying vmhook::hook() failed - returning empty handle.",
                       vmhook::error_tag, typeid(wrapper_type).name(),
                       method_name, method_signature);
            return hook_handle{};
        }

        // Re-resolve the Method* we just installed so the handle has
        // a stable target to disarm later.  The lookup duplicates a
        // few comparisons from hook<T>(), but install is not a hot
        // path and this keeps the public API decoupled from the
        // internal install routine.
        try
        {
            const auto type_map_entry{ vmhook::type_to_class_map.find(
                std::type_index{ typeid(wrapper_type) }) };
            if (type_map_entry == vmhook::type_to_class_map.end())
            {
                VMHOOK_LOG("{} scoped_hook<{}>('{}{}'): wrapper type not registered "
                           "(call register_class<T>() first) - returning empty handle.",
                           vmhook::error_tag, typeid(wrapper_type).name(),
                           method_name, method_signature);
                return hook_handle{};
            }
            vmhook::hotspot::klass* const target_klass{
                vmhook::find_class(type_map_entry->second) };
            if (!target_klass)
            {
                VMHOOK_LOG("{} scoped_hook<{}>('{}{}'): find_class('{}') returned null - "
                           "returning empty handle.",
                           vmhook::error_tag, typeid(wrapper_type).name(),
                           method_name, method_signature, type_map_entry->second);
                return hook_handle{};
            }
            const std::int32_t method_count{ target_klass->get_methods_count() };
            vmhook::hotspot::method** const methods_array{ target_klass->get_methods_ptr() };
            if (!methods_array || method_count <= 0)
            {
                VMHOOK_LOG("{} scoped_hook<{}>('{}{}'): klass has no methods array (count={}) - "
                           "returning empty handle.",
                           vmhook::error_tag, typeid(wrapper_type).name(),
                           method_name, method_signature, method_count);
                return hook_handle{};
            }
            for (std::int32_t i{ 0 }; i < method_count; ++i)
            {
                vmhook::hotspot::method* const m{ methods_array[i] };
                if (!m || !vmhook::hotspot::is_valid_pointer(m))
                {
                    continue;
                }
                if (m->get_name() != method_name)
                {
                    continue;
                }
                if (!method_signature.empty() && m->get_signature() != method_signature)
                {
                    continue;
                }
                return hook_handle{ m };
            }
            VMHOOK_LOG("{} scoped_hook<{}>('{}{}'): hook installed but Method* re-resolution "
                       "did not find a matching method in the klass; returning empty handle. "
                       "The hook IS active, but the caller has no way to disarm it individually.",
                       vmhook::error_tag, typeid(wrapper_type).name(),
                       method_name, method_signature);
        }
        catch (const std::exception& exception)
        {
            VMHOOK_LOG("{} scoped_hook<{}>('{}{}'): exception during Method* re-resolution: {} - "
                       "returning empty handle.",
                       vmhook::error_tag, typeid(wrapper_type).name(),
                       method_name, method_signature, exception.what());
        }
        catch (...)
        {
            VMHOOK_LOG("{} scoped_hook<{}>('{}{}'): unknown exception during Method* re-resolution - "
                       "returning empty handle.",
                       vmhook::error_tag, typeid(wrapper_type).name(),
                       method_name, method_signature);
        }
        return hook_handle{};
    }

    template<class wrapper_type>
    inline auto scoped_hook(const std::string_view method_name,
                            auto&&                 user_detour) -> hook_handle
    {
        return vmhook::scoped_hook<wrapper_type>(
            method_name, std::string_view{},
            std::forward<decltype(user_detour)>(user_detour));
    }

    // --- JNI helper layer --------------------------------------------------------
    // Low-level wrappers around the JNIEnv function table.  All functions are
    // noexcept and return nullptr / empty on failure so callers never need to
    // catch JNI exceptions explicitly (they call jni_exception_clear() instead).

    namespace detail
    {
        /*
            @brief Tagged union matching the layout of JNI's jvalue type.
            @details
            Used when building argument arrays for CallObjectMethodA and related JNI
            varargs-A variants.  Each field corresponds to a JNI primitive type:
              z = jboolean, b = jbyte, c = jchar, s = jshort,
              i = jint, j = jlong, f = jfloat, d = jdouble, l = jobject.
        */
        union jni_value
        {
            bool z;
            std::int8_t b;
            std::uint16_t c;
            std::int16_t s;
            std::int32_t i;
            std::int64_t j;
            float f;
            double d;
            void* l;
        };

        /*
            @brief Retrieves a JNI function pointer from the JNIEnv function table by index.
            @details
            A JNIEnv* is a pointer to a pointer to an array of function pointers (the
            JNI function table).  Given the zero-based JNI table index (as defined by
            the JNI specification) and the target function_type, this returns a callable
            function pointer cast to function_type, or nullptr if env is invalid.

            Complexity: O(1).
            Exception safety: noexcept — returns nullptr if env is null.

            @tparam index          Zero-based index into the JNIEnv function table.
            @tparam function_type  The expected function pointer type for this slot.
            @param env  The JNIEnv* (current_jni_env).
            @return     Callable function pointer, or nullptr on failure.
        */
        template<std::size_t index, typename function_type>
        inline auto jni_function(void* const env) noexcept
            -> function_type
        {
            if (!env)
            {
                return nullptr;
            }

            void** const table{ *reinterpret_cast<void***>(env) };
            if (!table)
            {
                return nullptr;
            }

            return reinterpret_cast<function_type>(table[index]);
        }

        /*
            @brief Dereferences a JNI local reference handle to obtain the underlying heap OOP.
            @details
            JNI local references are pointers into the current thread's JNI handle block.
            Dereferencing the handle pointer yields the raw Java heap OOP.  The result is
            validated with is_valid_pointer before being returned.

            Complexity: O(1).
            Exception safety: noexcept — returns nullptr if handle is null or OOP is invalid.

            @param object_handle  A JNI local reference (jobject, jclass, jstring, etc.).
            @return  Raw heap OOP, or nullptr on failure.
        */
        inline auto jni_decode_object(void* const object_handle) noexcept
            -> void*
        {
            if (!object_handle)
            {
                return nullptr;
            }

            void* const oop{ *reinterpret_cast<void**>(object_handle) };
            return vmhook::hotspot::is_valid_pointer(oop) ? oop : nullptr;
        }

        /*
            @brief Implementation of jni_delete_local_ref (declared above).
            @details
            JNI table slot 23 is DeleteLocalRef.  Null handles are a documented
            JNI no-op; we mirror that contract so callers don't have to repeat
            the null guard at every release site.
        */
        inline auto jni_delete_local_ref(void* const object_handle) noexcept
            -> void
        {
            if (!object_handle)
            {
                return;
            }
            using delete_local_ref_t = void(*)(void*, void*);
            delete_local_ref_t const delete_local_ref{
                vmhook::detail::jni_function<23, delete_local_ref_t>(vmhook::hotspot::current_jni_env) };
            if (delete_local_ref)
            {
                delete_local_ref(vmhook::hotspot::current_jni_env, object_handle);
            }
        }

        /*
            @brief Calls JNIEnv::NewGlobalRef (table slot 21) to pin a Java object.
            @details
            Promotes a local reference / synthetic handle to a JNI global reference.
            Global references survive across JNI calls, ticks, threads, and — crucially
            — across relocating garbage collections: the JVM rewrites the slot the
            handle points at when it moves the object, so dereferencing the global
            handle always yields the object's CURRENT heap address.  Must be released
            with jni_delete_global_ref().  Null input / no JVM returns nullptr.

            Complexity: O(1).  Exception safety: noexcept.
        */
        inline auto jni_new_global_ref(void* const object_handle) noexcept
            -> void*
        {
            if (!object_handle)
            {
                return nullptr;
            }
            using new_global_ref_t = void* (*)(void*, void*);
            new_global_ref_t const new_global_ref{
                vmhook::detail::jni_function<21, new_global_ref_t>(vmhook::hotspot::current_jni_env) };
            return new_global_ref ? new_global_ref(vmhook::hotspot::current_jni_env, object_handle) : nullptr;
        }

        /*
            @brief Calls JNIEnv::DeleteGlobalRef (table slot 22) to unpin a Java object.
            @details
            Null handles are a documented JNI no-op, mirrored here.  Releasing a global
            ref twice corrupts the JNI handle table, so ownership must be exactly-once —
            vmhook::jni::global_ref encapsulates that for you.

            Complexity: O(1).  Exception safety: noexcept.
        */
        inline auto jni_delete_global_ref(void* const global_handle) noexcept
            -> void
        {
            if (!global_handle)
            {
                return;
            }
            using delete_global_ref_t = void (*)(void*, void*);
            delete_global_ref_t const delete_global_ref{
                vmhook::detail::jni_function<22, delete_global_ref_t>(vmhook::hotspot::current_jni_env) };
            if (delete_global_ref)
            {
                delete_global_ref(vmhook::hotspot::current_jni_env, global_handle);
            }
        }

        /*
            @brief Constructs a synthetic JNI handle for a raw heap OOP without a JNI frame.
            @details
            Stores oop into handle_storage and returns a pointer to handle_storage.
            This creates a stack-allocated "fake" JNI handle that the JNI function table
            will dereference as a normal local reference.  The caller must keep
            handle_storage alive for the duration of any JNI calls that use the handle.

            Complexity: O(1).
            Exception safety: noexcept — plain pointer assignment.

            @param oop             Raw heap OOP to wrap.
            @param handle_storage  Caller-provided storage for the OOP value.
            @return  Pointer to handle_storage, suitable for passing as a JNI jobject.
        */
        inline auto jni_oop_handle(void* const oop, void*& handle_storage) noexcept
            -> void*
        {
            handle_storage = oop;
            return &handle_storage;
        }

        /*
            @brief Calls JNIEnv::FindClass to locate a class by its internal name.
            @details
            Uses JNI table slot 6 (FindClass) with current_jni_env.  The name must use
            '/' separators (e.g. "java/lang/String").  Returns a JNI local reference
            (jclass handle), not a Klass*.  Use jni_klass_from_class_mirror() to get
            the HotSpot Klass* from the returned handle.

            Complexity: O(lookup in the bootstrap class loader).
            Exception safety: noexcept — returns nullptr on any failure.

            @param class_name  Internal JVM class name with '/' separators.
            @return  jclass local reference, or nullptr if not found or JNI is unavailable.
        */
        inline auto jni_find_class(const std::string_view class_name) noexcept
            -> void*
        {
            if (!vmhook::hotspot::ensure_current_java_thread())
            {
                return nullptr;
            }

            using find_class_t = void* (*)(void*, const char*);
            find_class_t const find_class{ vmhook::detail::jni_function<6, find_class_t>(vmhook::hotspot::current_jni_env) };
            if (!find_class)
            {
                return nullptr;
            }

            const std::string name{ class_name };
            return find_class(vmhook::hotspot::current_jni_env, name.c_str());
        }

        /*
            @brief Clears any pending JNI exception on the current thread.
            @details
            Calls JNIEnv::ExceptionCheck (slot 228) to test for a pending exception, then
            JNIEnv::ExceptionClear (slot 17) to dismiss it.  Should be called after any
            JNI call that may have thrown (e.g. FindClass, GetMethodID) to prevent the
            pending exception from poisoning subsequent JNI operations.

            Complexity: O(1).
            Exception safety: noexcept — JNI vtable dispatches only.
        */
        inline auto jni_exception_clear() noexcept
            -> void
        {
            using exception_check_t = bool (*)(void*);
            using exception_clear_t = void (*)(void*);
            exception_check_t const exception_check{ vmhook::detail::jni_function<228, exception_check_t>(vmhook::hotspot::current_jni_env) };
            exception_clear_t const exception_clear{ vmhook::detail::jni_function<17, exception_clear_t>(vmhook::hotspot::current_jni_env) };
            if (exception_check && exception_clear && exception_check(vmhook::hotspot::current_jni_env))
            {
                exception_clear(vmhook::hotspot::current_jni_env);
            }
        }

        /*
            @brief Calls JNIEnv::GetObjectClass to retrieve the jclass of a JNI object handle.
            @details
            Uses JNI table slot 31 (GetObjectClass).  Returns a jclass local reference.

            Complexity: O(1).
            Exception safety: noexcept — returns nullptr if env or handle is null.

            @param object_handle  A JNI local reference to a Java object.
            @return  jclass handle for the object's runtime class, or nullptr on failure.
        */
        inline auto jni_get_object_class(void* const object_handle) noexcept
            -> void*
        {
            using get_object_class_t = void* (*)(void*, void*);
            get_object_class_t const get_object_class{ vmhook::detail::jni_function<31, get_object_class_t>(vmhook::hotspot::current_jni_env) };
            return get_object_class ? get_object_class(vmhook::hotspot::current_jni_env, object_handle) : nullptr;
        }

        /*
            @brief Calls JNIEnv::GetMethodID to look up an instance method by name and descriptor.
            @details
            Uses JNI table slot 33 (GetMethodID).  Clears any pending exception before the
            call and again after if the lookup fails, so callers do not need to handle
            pending exceptions from failed lookups.

            Complexity: O(method count in the class hierarchy).
            Exception safety: noexcept — returns nullptr and clears the exception on failure.

            @param klass      jclass handle for the class to search.
            @param name       Java method name (e.g. "getScore").
            @param signature  JNI descriptor string (e.g. "()I").
            @return  jmethodID, or nullptr if not found.
        */
        inline auto jni_get_method_id(void* const klass, const std::string& name, const std::string& signature) noexcept
            -> void*
        {
            vmhook::detail::jni_exception_clear();
            using get_method_id_t = void* (*)(void*, void*, const char*, const char*);
            get_method_id_t const get_method_id{ vmhook::detail::jni_function<33, get_method_id_t>(vmhook::hotspot::current_jni_env) };
            void* const method_id{ get_method_id ? get_method_id(vmhook::hotspot::current_jni_env, klass, name.c_str(), signature.c_str()) : nullptr };
            if (!method_id)
            {
                vmhook::detail::jni_exception_clear();
            }
            return method_id;
        }

        inline auto jni_new_string_utf(const std::string_view value) noexcept
            -> void*;

        /*
            @brief Calls JNIEnv::GetStaticMethodID to look up a static method by name and descriptor.
            @details
            Uses JNI table slot 113 (GetStaticMethodID).  The return value is a jmethodID
            suitable for jni_call_static_object_method().

            Complexity: O(method count in the class).
            Exception safety: noexcept — returns nullptr on failure.

            @param klass      jclass handle.
            @param name       Java method name.
            @param signature  JNI descriptor string.
            @return  jmethodID, or nullptr if not found.
        */
        inline auto jni_get_static_method_id(void* const klass, const std::string& name, const std::string& signature) noexcept
            -> void*
        {
            using get_static_method_id_t = void* (*)(void*, void*, const char*, const char*);
            get_static_method_id_t const get_static_method_id{ vmhook::detail::jni_function<113, get_static_method_id_t>(vmhook::hotspot::current_jni_env) };
            return get_static_method_id ? get_static_method_id(vmhook::hotspot::current_jni_env, klass, name.c_str(), signature.c_str()) : nullptr;
        }

        /*
            @brief Calls JNIEnv::GetStaticFieldID to look up a static field by name and descriptor.
            @details
            Uses JNI table slot 144 (GetStaticFieldID).  The return value is a jfieldID
            suitable for jni_get_static_object_field().

            Complexity: O(field count in the class).
            Exception safety: noexcept — returns nullptr on failure.

            @param klass      jclass handle.
            @param name       Java field name (e.g. "classLoader").
            @param signature  JNI type descriptor (e.g. "Ljava/lang/ClassLoader;").
            @return  jfieldID, or nullptr if not found.
        */
        inline auto jni_get_static_field_id(void* const klass, const std::string& name, const std::string& signature) noexcept
            -> void*
        {
            using get_static_field_id_t = void* (*)(void*, void*, const char*, const char*);
            get_static_field_id_t const get_static_field_id{ vmhook::detail::jni_function<144, get_static_field_id_t>(vmhook::hotspot::current_jni_env) };
            return get_static_field_id ? get_static_field_id(vmhook::hotspot::current_jni_env, klass, name.c_str(), signature.c_str()) : nullptr;
        }

        /*
            @brief Calls JNIEnv::GetStaticObjectField to read a static object-typed field.
            @details
            Uses JNI table slot 145 (GetStaticObjectField).  Returns a JNI local reference
            (jobject) to the field value.

            Complexity: O(1).
            Exception safety: noexcept — returns nullptr on failure.

            @param klass     jclass handle.
            @param field_id  jfieldID from jni_get_static_field_id().
            @return  jobject local reference, or nullptr on failure.
        */
        inline auto jni_get_static_object_field(void* const klass, void* const field_id) noexcept
            -> void*
        {
            using get_static_object_field_t = void* (*)(void*, void*, void*);
            get_static_object_field_t const get_static_object_field{ vmhook::detail::jni_function<145, get_static_object_field_t>(vmhook::hotspot::current_jni_env) };
            return get_static_object_field ? get_static_object_field(vmhook::hotspot::current_jni_env, klass, field_id) : nullptr;
        }

        /*
            @brief Calls JNIEnv::CallObjectMethodA to invoke an instance method.
            @details
            Uses JNI table slot 36 (CallObjectMethodA).  args may be nullptr for
            zero-argument methods.  Returns a JNI local reference to the result object.

            Complexity: O(method execution time).
            Exception safety: noexcept — returns nullptr on JNI failure.

            @param object     JNI local reference to the receiver object.
            @param method_id  jmethodID from jni_get_method_id().
            @param args       Array of jvalue arguments, or nullptr for no arguments.
            @return  jobject local reference for the return value, or nullptr on failure.
        */
        inline auto jni_call_object_method(void* const object, void* const method_id, const vmhook::detail::jni_value* const args = nullptr) noexcept
            -> void*
        {
            using call_object_method_a_t = void* (*)(void*, void*, void*, const vmhook::detail::jni_value*);
            call_object_method_a_t const call_object_method_a{ vmhook::detail::jni_function<36, call_object_method_a_t>(vmhook::hotspot::current_jni_env) };
            return call_object_method_a ? call_object_method_a(vmhook::hotspot::current_jni_env, object, method_id, args) : nullptr;
        }

        /*
            @brief Calls JNIEnv::CallStaticObjectMethodA to invoke a static method.
            @details
            Uses JNI table slot 116 (CallStaticObjectMethodA).  args may be nullptr.
            Returns a JNI local reference to the result object.

            Complexity: O(method execution time).
            Exception safety: noexcept — returns nullptr on JNI failure.

            @param klass      jclass handle for the class that declares the method.
            @param method_id  jmethodID from jni_get_static_method_id().
            @param args       Array of jvalue arguments, or nullptr for no arguments.
            @return  jobject local reference for the return value, or nullptr on failure.
        */
        inline auto jni_call_static_object_method(void* const klass, void* const method_id, const vmhook::detail::jni_value* const args = nullptr) noexcept
            -> void*
        {
            using call_static_object_method_a_t = void* (*)(void*, void*, void*, const vmhook::detail::jni_value*);
            call_static_object_method_a_t const call_static_object_method_a{ vmhook::detail::jni_function<116, call_static_object_method_a_t>(vmhook::hotspot::current_jni_env) };
            return call_static_object_method_a ? call_static_object_method_a(vmhook::hotspot::current_jni_env, klass, method_id, args) : nullptr;
        }

        /*
            @brief Extracts the HotSpot Klass* from a java.lang.Class JNI handle.
            @details
            A java.lang.Class object (the class mirror) stores a back-pointer to its Klass
            at a fixed offset defined by java_lang_Class._klass_offset, which is exported
            through gHotSpotVMStructs.  This function decodes the JNI handle to a raw OOP,
            reads the klass field, and strips GC tag bits via untag_pointer().

            Complexity: O(1).
            Exception safety: noexcept — returns nullptr on any failure.

            @param class_handle  jclass local reference (e.g. from jni_find_class()).
            @return  The underlying Klass*, or nullptr on failure.
        */
        inline auto jni_klass_from_class_mirror(void* const class_handle) noexcept
            -> vmhook::hotspot::klass*
        {
            void* const class_oop{ vmhook::detail::jni_decode_object(class_handle) };
            if (!class_oop || !vmhook::hotspot::is_valid_pointer(class_oop))
            {
                return nullptr;
            }

            static const vmhook::hotspot::vm_struct_entry_t* const klass_offset{ vmhook::hotspot::iterate_struct_entries("java_lang_Class", "_klass_offset") };
            if (!klass_offset || !klass_offset->address)
            {
                return nullptr;
            }

            const int offset{ *reinterpret_cast<const int*>(klass_offset->address) };
            void* const raw_klass{ const_cast<void*>(vmhook::hotspot::safe_read_pointer(reinterpret_cast<const std::uint8_t*>(class_oop) + offset)) };
            return vmhook::hotspot::is_valid_pointer(raw_klass) ? reinterpret_cast<vmhook::hotspot::klass*>(const_cast<void*>(vmhook::hotspot::untag_pointer(raw_klass))) : nullptr;
        }

        inline auto jni_find_class_with_context_loader(const std::string_view class_name) noexcept
            -> vmhook::hotspot::klass*
        {
            if (!vmhook::hotspot::ensure_current_java_thread())
            {
                return nullptr;
            }

            // Small RAII helper: every JNI local ref we acquire during the
            // class-loader walk lands in this vector; on return the dtor
            // DeleteLocalRef's each one.  Previously this function leaked
            // 4-8 local refs per successful call (thread_class,
            // current_thread, context_loader, name_string, class_mirror,
            // class_loader_class, system_loader, launch_class,
            // launch_loader).  find_class caches the resulting klass so
            // the leak was bounded by distinct class names, but every
            // brand-new class still ate one ref per intermediate handle -
            // detour threads that look up many classes (Minecraft + Forge
            // + Lunar mods) eventually trip the local-ref table.
            struct local_ref_bag
            {
                std::vector<void*> handles;
                ~local_ref_bag() noexcept
                {
                    for (void* const handle : handles)
                    {
                        vmhook::detail::jni_delete_local_ref(handle);
                    }
                }
                auto track(void* const handle) noexcept -> void*
                {
                    if (handle)
                    {
                        this->handles.push_back(handle);
                    }
                    return handle;
                }
            } bag;

            auto load_with_loader = [&](void* const class_loader) noexcept
                -> vmhook::hotspot::klass*
            {
                if (!class_loader)
                {
                    return nullptr;
                }

                void* const class_loader_class{ bag.track(vmhook::detail::jni_find_class("java/lang/ClassLoader")) };
                if (!class_loader_class)
                {
                    vmhook::detail::jni_exception_clear();
                    return nullptr;
                }

                void* const load_class_id{ vmhook::detail::jni_get_method_id(class_loader_class, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;") };
                if (!load_class_id)
                {
                    vmhook::detail::jni_exception_clear();
                    return nullptr;
                }

                std::string dotted_name{ class_name };
                std::replace(dotted_name.begin(), dotted_name.end(), '/', '.');
                void* const name_string{ bag.track(vmhook::detail::jni_new_string_utf(dotted_name)) };
                if (!name_string)
                {
                    vmhook::detail::jni_exception_clear();
                    return nullptr;
                }

                vmhook::detail::jni_value args[1]{};
                args[0].l = name_string;

                void* const class_mirror{ bag.track(vmhook::detail::jni_call_object_method(class_loader, load_class_id, args)) };
                vmhook::hotspot::klass* const klass{ vmhook::detail::jni_klass_from_class_mirror(class_mirror) };
                vmhook::detail::jni_exception_clear();
                return klass;
            };

            void* const thread_class{ bag.track(vmhook::detail::jni_find_class("java/lang/Thread")) };
            if (thread_class)
            {
                void* const current_thread_id{ vmhook::detail::jni_get_static_method_id(thread_class, "currentThread", "()Ljava/lang/Thread;") };
                void* const get_context_loader_id{ vmhook::detail::jni_get_method_id(thread_class, "getContextClassLoader", "()Ljava/lang/ClassLoader;") };
                if (current_thread_id && get_context_loader_id)
                {
                    void* const current_thread{ bag.track(vmhook::detail::jni_call_static_object_method(thread_class, current_thread_id)) };
                    void* const context_loader{ current_thread ? bag.track(vmhook::detail::jni_call_object_method(current_thread, get_context_loader_id)) : nullptr };
                    if (vmhook::hotspot::klass* const klass{ load_with_loader(context_loader) })
                    {
                        return klass;
                    }
                }
            }
            vmhook::detail::jni_exception_clear();

            void* const class_loader_class{ bag.track(vmhook::detail::jni_find_class("java/lang/ClassLoader")) };
            if (class_loader_class)
            {
                void* const get_system_loader_id{ vmhook::detail::jni_get_static_method_id(class_loader_class, "getSystemClassLoader", "()Ljava/lang/ClassLoader;") };
                void* const system_loader{ get_system_loader_id ? bag.track(vmhook::detail::jni_call_static_object_method(class_loader_class, get_system_loader_id)) : nullptr };
                if (vmhook::hotspot::klass* const klass{ load_with_loader(system_loader) })
                {
                    return klass;
                }
            }
            vmhook::detail::jni_exception_clear();

            void* const launch_class{ bag.track(vmhook::detail::jni_find_class("net/minecraft/launchwrapper/Launch")) };
            if (!launch_class)
            {
                vmhook::detail::jni_exception_clear();
                VMHOOK_LOG("{} jni_find_class_with_context_loader('{}'): all class loader paths "
                           "failed (thread context, system, Minecraft Launch).  Class is not "
                           "reachable from any class loader visible to JNI.",
                           vmhook::warning_tag, class_name);
                return nullptr;
            }

            void* const class_loader_field{ vmhook::detail::jni_get_static_field_id(launch_class, "classLoader", "Lnet/minecraft/launchwrapper/LaunchClassLoader;") };
            void* const launch_loader{ class_loader_field ? bag.track(vmhook::detail::jni_get_static_object_field(launch_class, class_loader_field)) : nullptr };
            if (vmhook::hotspot::klass* const klass{ load_with_loader(launch_loader) })
            {
                return klass;
            }

            vmhook::detail::jni_exception_clear();
            VMHOOK_LOG("{} jni_find_class_with_context_loader('{}'): all class loader paths "
                       "failed including Minecraft Launch loader.  Class is not loaded anywhere.",
                       vmhook::warning_tag, class_name);
            return nullptr;
        }

        // -------------------------------------------------------------------
        // Host context-classloader inheritance
        //
        // When the injector attaches a worker thread via JNI AttachCurrentThread*,
        // the JVM gives the new thread the platform / application classloader
        // as its context classloader.  That loader cannot see classes loaded by
        // the host application's custom loader chain (Forge's LaunchClassLoader,
        // Lunar Client's mixin chain, etc.), so any JNI work that runs through
        // host mixins fails with UnsupportedOperationException ("Don't know how
        // to turn ... into a string") or NullPointerException as soon as the
        // mixin tries to resolve a class via Thread.currentThread()
        // .getContextClassLoader().
        //
        // Fix: as soon as the injector finds at least one host class via
        // vmhook::find_class, remember its Klass*.  When a new native thread
        // attaches we read the loader out of that Klass's ClassLoaderData and
        // call Thread.currentThread().setContextClassLoader(...) on the new
        // thread so it inherits the host loader chain.
        // -------------------------------------------------------------------

        /*
            @brief Cached "host" Klass*.
            @details
            Populated by capture_host_classloader_klass() the first time
            vmhook::find_class succeeds for a class whose owning ClassLoaderData
            has a non-null _class_loader (i.e. a non-bootstrap loader).  Used by
            inherit_host_context_classloader_for_current_thread() as the source
            of the loader to install onto freshly-attached worker threads.

            Klass* in HotSpot lives in Metaspace and is stable across GC; the
            ClassLoader oop reachable through Klass -> ClassLoaderData ->
            _class_loader IS heap-resident and may relocate, so we re-derive it
            on every attach instead of caching the raw oop.
        */
        inline std::atomic<vmhook::hotspot::klass*> host_classloader_klass{ nullptr };

        /*
            @brief Returns the java.lang.ClassLoader oop for a given Klass.
            @details
            Walks Klass._class_loader_data -> ClassLoaderData._class_loader and
            transparently handles the JDK 10+ OopHandle indirection.  Returns
            nullptr for bootstrap-loaded classes (java/lang/Object, etc.).

            Complexity: O(1) after VMStruct offsets are cached.
            Exception safety: noexcept — returns nullptr on any failure.
        */
        inline auto klass_to_class_loader_oop(vmhook::hotspot::klass* const k) noexcept
            -> void*
        {
            static const vmhook::hotspot::vm_struct_entry_t* const cld_entry{
                vmhook::hotspot::iterate_struct_entries("Klass", "_class_loader_data") };

            if (!cld_entry || !k || !vmhook::hotspot::is_valid_pointer(k))
            {
                return nullptr;
            }

            vmhook::hotspot::class_loader_data* const cld{
                *reinterpret_cast<vmhook::hotspot::class_loader_data**>(
                    reinterpret_cast<std::uint8_t*>(k) + cld_entry->offset) };
            if (!cld || !vmhook::hotspot::is_valid_pointer(cld))
            {
                return nullptr;
            }

            return cld->get_class_loader_oop();
        }

        /*
            @brief Records a Klass whose loader is the host application's loader.
            @details
            No-op if a host klass is already cached or if the supplied Klass is
            bootstrap-loaded (loader == null).  Safe to call from any thread;
            uses a single CAS to publish the first non-bootstrap candidate.
        */
        inline auto capture_host_classloader_klass(vmhook::hotspot::klass* const candidate) noexcept
            -> void
        {
            if (vmhook::detail::host_classloader_klass.load(std::memory_order_acquire))
            {
                return;
            }
            if (!candidate || !vmhook::hotspot::is_valid_pointer(candidate))
            {
                return;
            }
            if (!vmhook::detail::klass_to_class_loader_oop(candidate))
            {
                return;
            }
            vmhook::hotspot::klass* expected{ nullptr };
            if (vmhook::detail::host_classloader_klass.compare_exchange_strong(
                    expected, candidate, std::memory_order_acq_rel))
            {
                // We just published the first host klass — the thread doing
                // the publish (usually the injector's bootstrap thread) may
                // itself need the loader for follow-up JNI work, so apply it
                // here instead of waiting for the next attach_current_native_thread
                // round-trip.
                vmhook::detail::inherit_host_context_classloader_for_current_thread();
            }
        }

        /*
            @brief Sets Thread.currentThread().contextClassLoader on the current thread.
            @details
            No-op if no host classloader has been captured yet (see
            capture_host_classloader_klass) or if any JNI step fails.  Called from
            attach_current_native_thread so that worker threads we spin up
            inherit the same loader chain Minecraft / Forge / Lunar uses for
            their own threads; without this, mixin-wrapped methods invoked by
            JNI on our worker threads throw because they cannot resolve classes
            through Thread.currentThread().getContextClassLoader().

            Exception safety: noexcept — best-effort; failures are logged once
            via VMHOOK_LOG and skipped on subsequent calls in the same process.
        */
        inline auto inherit_host_context_classloader_for_current_thread() noexcept
            -> void
        {
            vmhook::hotspot::klass* const host_klass{
                vmhook::detail::host_classloader_klass.load(std::memory_order_acquire) };
            if (!host_klass)
            {
                return;
            }

            void* const loader_oop{ vmhook::detail::klass_to_class_loader_oop(host_klass) };
            if (!loader_oop)
            {
                return;
            }

            if (!vmhook::hotspot::current_jni_env)
            {
                return;
            }

            void* const thread_class{ vmhook::detail::jni_find_class("java/lang/Thread") };
            if (!thread_class)
            {
                vmhook::detail::jni_exception_clear();
                return;
            }

            void* const current_thread_id{ vmhook::detail::jni_get_static_method_id(
                thread_class, "currentThread", "()Ljava/lang/Thread;") };
            void* const set_loader_id{ vmhook::detail::jni_get_method_id(
                thread_class, "setContextClassLoader", "(Ljava/lang/ClassLoader;)V") };
            if (!current_thread_id || !set_loader_id)
            {
                vmhook::detail::jni_exception_clear();
                return;
            }

            void* const current_thread{ vmhook::detail::jni_call_static_object_method(
                thread_class, current_thread_id) };
            if (!current_thread)
            {
                vmhook::detail::jni_exception_clear();
                return;
            }

            void* loader_storage{};
            void* const loader_handle{ vmhook::detail::jni_oop_handle(loader_oop, loader_storage) };
            vmhook::detail::jni_value args[1]{};
            args[0].l = loader_handle;

            using call_void_method_a_t = void (*)(void*, void*, void*, const vmhook::detail::jni_value*);
            auto* const call_void_method_a{
                vmhook::detail::jni_function<63, call_void_method_a_t>(vmhook::hotspot::current_jni_env) };
            if (!call_void_method_a)
            {
                return;
            }

            call_void_method_a(vmhook::hotspot::current_jni_env, current_thread, set_loader_id, args);
            vmhook::detail::jni_exception_clear();

            VMHOOK_LOG("{} inherit_host_context_classloader_for_current_thread(): installed loader 0x{:016X} on OS thread {}.",
                       vmhook::info_tag,
                       reinterpret_cast<std::uintptr_t>(loader_oop),
                       static_cast<std::uint64_t>(vmhook::os::current_thread_id()));
        }

        /*
            @brief Calls JNIEnv::NewStringUTF to create a Java String from a UTF-8 C string.
            @details
            Uses JNI table slot 167 (NewStringUTF).  Returns a JNI local reference
            (jstring handle), not a raw heap OOP.  Use jni_decode_object() to obtain
            the underlying OOP if needed.

            Complexity: O(N) where N = length of value.
            Exception safety: noexcept — returns nullptr if JNIEnv is unavailable.

            @param value  UTF-8 text to encode as a Java String.
            @return  jstring local reference, or nullptr on failure.
        */
        inline auto jni_new_string_utf(const std::string_view value) noexcept
            -> void*
        {
            using new_string_utf_t = void* (*)(void*, const char*);
            new_string_utf_t const new_string_utf{ vmhook::detail::jni_function<167, new_string_utf_t>(vmhook::hotspot::current_jni_env) };
            if (!new_string_utf)
            {
                return nullptr;
            }

            const std::string text{ value };
            return new_string_utf(vmhook::hotspot::current_jni_env, text.c_str());
        }

        /*
            @brief Reads a Java String's content as a std::string via JNI.
            @details
            Calls JNIEnv::GetStringUTFChars (slot 169) to obtain a UTF-8 C string,
            copies it into a std::string, then calls ReleaseStringUTFChars (slot 170)
            to free the buffer.

            Complexity: O(N) where N = string length.
            Exception safety: noexcept — returns empty string on failure.

            @param string_handle  jstring local reference.
            @return  The string contents, or an empty string on failure.
        */
        inline auto jni_get_string_utf(void* const string_handle) noexcept
            -> std::string
        {
            if (!string_handle)
            {
                return {};
            }

            using get_string_utf_chars_t = const char* (*)(void*, void*, bool*);
            using release_string_utf_chars_t = void (*)(void*, void*, const char*);
            get_string_utf_chars_t const get_string_utf_chars{ vmhook::detail::jni_function<169, get_string_utf_chars_t>(vmhook::hotspot::current_jni_env) };
            release_string_utf_chars_t const release_string_utf_chars{ vmhook::detail::jni_function<170, release_string_utf_chars_t>(vmhook::hotspot::current_jni_env) };
            if (!get_string_utf_chars)
            {
                return {};
            }

            bool is_copy{};
            const char* const chars{ get_string_utf_chars(vmhook::hotspot::current_jni_env, string_handle, &is_copy) };
            if (!chars)
            {
                return {};
            }

            std::string result{ chars };
            if (release_string_utf_chars)
            {
                release_string_utf_chars(vmhook::hotspot::current_jni_env, string_handle, chars);
            }
            return result;
        }

        /*
            @brief Returns the JNI type descriptor character(s) for a C++ argument type.
            @details
            Maps C++ types to their JNI descriptor string at compile time:
              std::string / string_view / const char* -> "Ljava/lang/String;"
              bool        -> "Z"
              int8/uint8  -> "B"
              int16       -> "S"
              uint16      -> "C"
              int64/uint64-> "J"
              float       -> "F"
              double      -> "D"
              other       -> "I" (treated as int)
            Used by method_proxy::call_jni() to build the JNI method descriptor string.

            Exception safety: noexcept — compile-time dispatch only.

            @tparam arg_type  The C++ argument type.
            @return  JNI type descriptor string.
        */
        template<typename arg_type>
        inline auto jni_signature_for_arg() noexcept
            -> std::string
        {
            using clean_t = std::decay_t<arg_type>;

            if constexpr (std::is_same_v<clean_t, std::string> || std::is_same_v<clean_t, std::string_view> || std::is_same_v<clean_t, const char*> || std::is_same_v<clean_t, char*>)
            {
                return "Ljava/lang/String;";
            }
            else if constexpr (std::is_same_v<clean_t, bool>)
            {
                return "Z";
            }
            else if constexpr (std::is_same_v<clean_t, std::int8_t> || std::is_same_v<clean_t, std::uint8_t>)
            {
                return "B";
            }
            else if constexpr (std::is_same_v<clean_t, std::int16_t>)
            {
                return "S";
            }
            else if constexpr (std::is_same_v<clean_t, std::uint16_t>)
            {
                return "C";
            }
            else if constexpr (std::is_same_v<clean_t, std::int64_t> || std::is_same_v<clean_t, std::uint64_t>)
            {
                return "J";
            }
            else if constexpr (std::is_same_v<clean_t, float>)
            {
                return "F";
            }
            else if constexpr (std::is_same_v<clean_t, double>)
            {
                return "D";
            }
            else if constexpr (std::is_integral_v<clean_t> && sizeof(clean_t) == sizeof(std::int32_t))
            {
                return "I";
            }
            else if constexpr (vmhook::detail::is_unique_ptr_v<clean_t>)
            {
                using wrapper_type = typename vmhook::detail::is_unique_ptr<clean_t>::value_type_t;
                static_assert(std::is_base_of_v<vmhook::object_base, wrapper_type>,
                              "vmhook::detail::jni_signature_for_arg: unique_ptr<T> arg's T must "
                              "derive from vmhook::object_base.  Pass a wrapper, not a raw object.");
                // Resolve the JVM class name from the registered wrapper map so the
                // signature reads `Lcom/example/Foo;` instead of the old silently-wrong
                // `I` fallback that broke jni_make_unique whenever someone tried to
                // construct an object whose constructor took another object.
                const auto entry{ vmhook::type_to_class_map.find(std::type_index{ typeid(wrapper_type) }) };
                if (entry == vmhook::type_to_class_map.end())
                {
                    VMHOOK_LOG("{} vmhook::detail::jni_signature_for_arg<{}>: unique_ptr<T> wrapper "
                               "not registered via vmhook::register_class<T>(name); falling back to "
                               "Ljava/lang/Object;.",
                               vmhook::warning_tag, typeid(wrapper_type).name());
                    return "Ljava/lang/Object;";
                }
                std::string sig{ "L" };
                sig.append(entry->second);
                sig.push_back(';');
                return sig;
            }
            else if constexpr (std::is_base_of_v<vmhook::object_base, clean_t>)
            {
                const auto entry{ vmhook::type_to_class_map.find(std::type_index{ typeid(clean_t) }) };
                if (entry == vmhook::type_to_class_map.end())
                {
                    VMHOOK_LOG("{} vmhook::detail::jni_signature_for_arg<{}>: object wrapper not "
                               "registered via vmhook::register_class<T>(name); falling back to "
                               "Ljava/lang/Object;.",
                               vmhook::warning_tag, typeid(clean_t).name());
                    return "Ljava/lang/Object;";
                }
                std::string sig{ "L" };
                sig.append(entry->second);
                sig.push_back(';');
                return sig;
            }
            else
            {
                static_assert(vmhook::detail::dependent_false_v<clean_t>,
                              "vmhook::detail::jni_signature_for_arg: unsupported argument type.  "
                              "The old `else return \"I\";` fallback silently mis-encoded every "
                              "wrapper-pointer / 64-bit / unknown-integral arg as Java `int`, "
                              "which then made GetMethodID fail in jni_make_unique with no clear "
                              "explanation.  Add an explicit branch above or convert the arg to one "
                              "of: string, c-string, unique_ptr<vmhook::object>, object_base-derived, "
                              "bool, integral (8/16/32/64-bit), float, double.");
            }
        }

        /*
            @brief Converts a single C++ argument to a jni_value and appends it to values.
            @details
            Handles the full range of argument types by compile-time dispatch:
              - std::string / string_view / const char* -> jni_new_string_utf + .l slot
              - unique_ptr<T extends object_base>       -> stores raw OOP in object_handles; .l slot
              - object_base derived by value            -> stores raw OOP in object_handles; .l slot
              - bool                                    -> .z slot
              - integral (<=4 bytes)                    -> .i slot
              - integral (8 bytes)                      -> .j slot
              - float                                   -> .f slot
              - double                                  -> .d slot
            Object handles are stored in the caller-provided object_handles vector to keep
            the OOP alive for the duration of the JNI call.

            Complexity: O(1) for primitives; O(N) for strings.
            Exception safety: noexcept — failures silently append a zero-initialised value.

            @param values          Output jni_value array being built.
            @param object_handles  Storage for OOP pointers wrapped as fake JNI handles.
            @param arg             The C++ argument to convert.
        */
        template<typename arg_type>
        inline auto append_jni_arg(std::vector<vmhook::detail::jni_value>& values, std::vector<void*>& object_handles, std::vector<char>& needs_release, arg_type&& arg) noexcept
            -> void
        {
            using clean_t = std::decay_t<arg_type>;
            vmhook::detail::jni_value value{};
            // Deterministic full-width clear.  jni_value is a union; value-init
            // ({}) only guarantees the FIRST member (bool z, 1 byte) + padding
            // are zeroed — the upper 7 bytes are left unspecified and differ by
            // compiler (MinGW zeroes them, Clang does not).  Writing the widest
            // member zeroes the whole 8-byte cell, so a narrow primitive (bool /
            // int / float) never leaves stale high bits in the slot.
            value.j = 0;
            // Tag tracked in lockstep with `values`: 1 ONLY for jstrings from
            // NewStringUTF (real local refs that must be DeleteLocalRef'd).
            // Reading value.l back at cleanup time is unsound because jni_value
            // is a union — a primitive arg aliases .l and would be deleted as a
            // garbage pointer.  std::vector<char> (not <bool>) so &back() is a
            // real addressable byte if ever needed.
            char release_tag{ 0 };

            if constexpr (std::is_same_v<clean_t, std::string>)
            {
                value.l = vmhook::detail::jni_new_string_utf(arg);
                release_tag = (value.l != nullptr) ? 1 : 0;
            }
            else if constexpr (std::is_same_v<clean_t, std::string_view>)
            {
                value.l = vmhook::detail::jni_new_string_utf(arg);
                release_tag = (value.l != nullptr) ? 1 : 0;
            }
            else if constexpr (std::is_same_v<clean_t, const char*> || std::is_same_v<clean_t, char*>)
            {
                value.l = vmhook::detail::jni_new_string_utf(arg ? std::string_view{ arg } : std::string_view{});
                release_tag = (value.l != nullptr) ? 1 : 0;
            }
            else if constexpr (vmhook::detail::is_unique_ptr_v<clean_t>)
            {
                using wrapper_type = typename vmhook::detail::is_unique_ptr<clean_t>::value_type_t;
                // Catch traits that silently mis-evaluate value_type_t (e.g. the
                // `value_type` shadowing bug we hit on MSVC where the inherited
                // std::true_type::value_type=bool typedef won over the template
                // parameter and made wrapper_type=bool, dropping the arg).
                static_assert(std::is_base_of_v<vmhook::object_base, wrapper_type>,
                              "vmhook::detail::append_jni_arg: unique_ptr<T> arg's T does not "
                              "derive from vmhook::object_base.  Either the wrapper type is wrong "
                              "or is_unique_ptr<>::value_type_t is mis-resolving (this caught the "
                              "value_type-shadowing trait bug).");
                object_handles.push_back(arg ? arg->get_instance() : nullptr);
                value.l = object_handles.empty() ? nullptr : &object_handles.back();
            }
            else if constexpr (std::is_base_of_v<vmhook::object_base, clean_t>)
            {
                object_handles.push_back(arg.get_instance());
                value.l = &object_handles.back();
            }
            else if constexpr (std::is_same_v<clean_t, bool>)
            {
                value.z = arg;
            }
            else if constexpr (std::is_integral_v<clean_t> && sizeof(clean_t) <= sizeof(std::int32_t))
            {
                value.i = static_cast<std::int32_t>(arg);
            }
            else if constexpr (std::is_integral_v<clean_t> && sizeof(clean_t) == sizeof(std::int64_t))
            {
                value.j = static_cast<std::int64_t>(arg);
            }
            else if constexpr (std::is_same_v<clean_t, float>)
            {
                value.f = arg;
            }
            else if constexpr (std::is_same_v<clean_t, double>)
            {
                value.d = arg;
            }
            else
            {
                static_assert(vmhook::detail::dependent_false_v<clean_t>,
                              "vmhook::detail::append_jni_arg: unsupported argument type.  "
                              "Add a branch above or convert the arg to one of: string, c-string, "
                              "unique_ptr<vmhook::object>, object_base-derived, bool, integral, "
                              "float, double.");
            }

            values.push_back(value);
            needs_release.push_back(release_tag);
        }

        /*
            @brief Builds a jni_value argument array from a variadic C++ argument pack.
            @details
            Calls append_jni_arg() for each argument in the pack using a fold expression.
            object_handles is pre-reserved to sizeof...(args_t) to avoid reallocations
            that would invalidate the pointers stored in the .l slots.

            Complexity: O(N) where N = number of arguments.
            Exception safety: noexcept — delegates to append_jni_arg which is noexcept.

            @param object_handles  Caller-owned storage for OOP handle pointers.
            @param args            C++ arguments to convert.
            @return  Vector of jni_value ready to pass to CallXMethodA.
        */
        template<typename... args_t>
        inline auto make_jni_args(std::vector<void*>& object_handles, std::vector<char>& needs_release, args_t&&... args) noexcept
            -> std::vector<vmhook::detail::jni_value>
        {
            std::vector<vmhook::detail::jni_value> values{};
            values.reserve(sizeof...(args_t));
            object_handles.reserve(sizeof...(args_t));
            needs_release.reserve(sizeof...(args_t));
            (vmhook::detail::append_jni_arg(values, object_handles, needs_release, std::forward<args_t>(args)), ...);
            return values;
        }

        /*
            @brief Fills a single jni_value slot from one C++ argument.
            @details
            Mirrors append_jni_arg's per-type conversion logic but
            writes into a fixed slot instead of pushing onto a
            std::vector.  Used by method_proxy::call_jni to pack args
            on the stack — no heap allocation in the call hot path.

            @param value    Output slot for the jvalue.
            @param storage  Storage for the indirect-handle pointer
                            when the arg is an object reference; the
                            jvalue.l points at this slot.
            @param needs_release  Set to true ONLY when this slot holds a JNI
                            local reference that the caller must DeleteLocalRef
                            (i.e. a jstring produced by NewStringUTF).  Left
                            false for primitives and synthetic object handles.
                            This is a dedicated tag because jni_value is a
                            union — reading value.l back to decide whether a
                            slot is a reference is unsound: a primitive arg
                            (e.g. jlong 0x4242'4242'4242'4242) aliases .l and
                            would be handed to DeleteLocalRef as a garbage
                            pointer.
            @param arg      C++ value to convert.
        */
        template<typename arg_type>
        inline auto write_jni_arg_to_slot(vmhook::detail::jni_value& value,
                                           void*& storage,
                                           bool& needs_release,
                                           arg_type&& arg) noexcept -> void
        {
            using clean_t = std::decay_t<arg_type>;
            // Deterministic full-width clear of the union cell — value-init of a
            // union ({}) only zeroes the first member (bool z) + padding on some
            // compilers, leaving the upper 7 bytes unspecified.  Writing the
            // widest member zeroes all 8 bytes so a narrow primitive leaves no
            // stale high bits behind.
            value.j = 0;
            needs_release = false;

            if constexpr (std::is_same_v<clean_t, std::string>
                       || std::is_same_v<clean_t, std::string_view>)
            {
                value.l = vmhook::detail::jni_new_string_utf(arg);
                needs_release = (value.l != nullptr);
            }
            else if constexpr (std::is_same_v<clean_t, const char*>
                            || std::is_same_v<clean_t, char*>)
            {
                value.l = arg ? vmhook::detail::jni_new_string_utf(std::string_view{ arg })
                              : nullptr;
                needs_release = (value.l != nullptr);
            }
            else if constexpr (vmhook::detail::is_unique_ptr_v<clean_t>)
            {
                using wrapper_type = typename vmhook::detail::is_unique_ptr<clean_t>::value_type_t;
                static_assert(std::is_base_of_v<vmhook::object_base, wrapper_type>,
                              "vmhook::detail::write_jni_arg_to_slot: unique_ptr<T> arg's T does "
                              "not derive from vmhook::object_base.  Either the wrapper type is "
                              "wrong or is_unique_ptr<>::value_type_t is mis-resolving (this "
                              "caught the value_type-shadowing trait bug that silently dropped "
                              "every IChatComponent arg).");
                storage = arg ? arg->get_instance() : nullptr;
                value.l = &storage;
            }
            else if constexpr (std::is_base_of_v<vmhook::object_base, clean_t>)
            {
                storage = arg.get_instance();
                value.l = &storage;
            }
            else if constexpr (std::is_same_v<clean_t, bool>)
            {
                value.z = arg;
            }
            else if constexpr (std::is_integral_v<clean_t> && sizeof(clean_t) <= sizeof(std::int32_t))
            {
                value.i = static_cast<std::int32_t>(arg);
            }
            else if constexpr (std::is_integral_v<clean_t> && sizeof(clean_t) == sizeof(std::int64_t))
            {
                value.j = static_cast<std::int64_t>(arg);
            }
            else if constexpr (std::is_same_v<clean_t, float>)
            {
                value.f = arg;
            }
            else if constexpr (std::is_same_v<clean_t, double>)
            {
                value.d = arg;
            }
            else
            {
                static_assert(vmhook::detail::dependent_false_v<clean_t>,
                              "vmhook::detail::write_jni_arg_to_slot: unsupported argument type.  "
                              "Add a branch above or convert the arg to one of: string, c-string, "
                              "unique_ptr<vmhook::object>, object_base-derived, bool, integral, "
                              "float, double.");
            }
        }

        /*
            @brief Constructs a Java object via JNI and wraps it in a std::unique_ptr<wrapper_type>.
            @details
            Finds the HotSpot klass for class_name, obtains its java.lang.Class mirror,
            builds the JNI argument array, locates the matching constructor via
            jni_get_method_id("<init>", ...), then calls CallObjectMethodA to instantiate
            the object.  The resulting JNI handle is decoded to a raw OOP and passed to
            the wrapper_type constructor.

            This is the JNI-backed implementation of vmhook::make_unique<>(); it is used
            when the TLAB allocation path is unavailable.

            Complexity: O(constructor lookup + constructor execution time).
            Exception safety: noexcept — returns nullptr on any failure; logs via VMHOOK_LOG.

            @tparam wrapper_type  C++ wrapper type deriving from vmhook::object_base.
            @tparam args_t        Constructor argument types.
            @param class_name     Internal JVM class name with '/' separators.
            @param args           Constructor arguments.
            @return  Newly created Java object wrapped in unique_ptr, or nullptr on failure.
        */
        template<typename wrapper_type, typename... args_t>
        inline auto jni_make_unique(const std::string& class_name, args_t&&... args) noexcept
            -> std::unique_ptr<wrapper_type>
        {
            vmhook::hotspot::klass* const hotspot_klass{ vmhook::find_class(class_name) };
            if (!hotspot_klass)
            {
                VMHOOK_LOG("{} jni_make_unique<{}>(): HotSpot class '{}' not found.", vmhook::error_tag, typeid(wrapper_type).name(), class_name);
                return nullptr;
            }

            void* const class_mirror{ hotspot_klass->get_java_mirror() };
            if (!class_mirror || !vmhook::hotspot::is_valid_pointer(class_mirror))
            {
                VMHOOK_LOG("{} jni_make_unique<{}>(): java.lang.Class mirror for '{}' is invalid.", vmhook::error_tag, typeid(wrapper_type).name(), class_name);
                return nullptr;
            }

            void* class_handle_storage{};
            void* const klass{ vmhook::detail::jni_oop_handle(class_mirror, class_handle_storage) };

            std::vector<void*> object_handles{};
            std::vector<char>  arg_needs_release{};
            std::vector<vmhook::detail::jni_value> values{ vmhook::detail::make_jni_args(object_handles, arg_needs_release, std::forward<args_t>(args)...) };

            // RAII release of any JNI local refs picked up by make_jni_args.
            // The per-slot `arg_needs_release[i]` tag is 1 ONLY for jstrings
            // from NewStringUTF; object args (whose value.l points INTO
            // object_handles) and primitives are tagged 0.  We MUST NOT read
            // value.l back to make this decision: jni_value is a union, so a
            // primitive arg (jlong / jint / jdouble) aliases .l and a
            // range-check against object_handles can spuriously decide a
            // primitive bit pattern is an out-of-range "string ref", handing
            // garbage to DeleteLocalRef.  object_handle from NewObjectA is
            // released after we decode the OOP (the OOP stays a GC root via
            // the wrapper_type unique_ptr we hand back).
            struct jni_arg_cleanup
            {
                const std::vector<char>* needs_release;
                std::vector<vmhook::detail::jni_value>* values_vec;
                void* object_handle{ nullptr };

                ~jni_arg_cleanup() noexcept
                {
                    for (std::size_t i{ 0 }; i < values_vec->size(); ++i)
                    {
                        if (i < needs_release->size() && (*needs_release)[i])
                        {
                            vmhook::detail::jni_delete_local_ref((*values_vec)[i].l);
                        }
                    }
                    if (object_handle)
                    {
                        vmhook::detail::jni_delete_local_ref(object_handle);
                    }
                }
            } cleanup{ &arg_needs_release, &values, nullptr };

            std::string signature{ "(" };
            ((signature += vmhook::detail::jni_signature_for_arg<std::remove_cvref_t<args_t>>()), ...);
            signature += ")V";

            void* const method_id{ vmhook::detail::jni_get_method_id(klass, "<init>", signature) };
            if (!method_id)
            {
                VMHOOK_LOG("{} jni_make_unique<{}>(): GetMethodID('<init>', '{}') failed.", vmhook::error_tag, typeid(wrapper_type).name(), signature);
                return nullptr;
            }

            using new_object_a_t = void* (*)(void*, void*, void*, const vmhook::detail::jni_value*);
            new_object_a_t const new_object_a{ vmhook::detail::jni_function<30, new_object_a_t>(vmhook::hotspot::current_jni_env) };
            if (!new_object_a)
            {
                VMHOOK_LOG("{} jni_make_unique<{}>(): NewObjectA slot 30 is null - the JNIEnv "
                           "function table is unusable.",
                           vmhook::error_tag, typeid(wrapper_type).name());
                return nullptr;
            }

            VMHOOK_LOG("{} jni_make_unique<{}>(): calling NewObjectA env=0x{:016X} class_mirror=0x{:016X} method_id=0x{:016X} sig='{}' arg_count={}.",
                       vmhook::info_tag,
                       typeid(wrapper_type).name(),
                       reinterpret_cast<std::uintptr_t>(vmhook::hotspot::current_jni_env),
                       reinterpret_cast<std::uintptr_t>(class_mirror),
                       reinterpret_cast<std::uintptr_t>(method_id),
                       signature,
                       static_cast<std::uint64_t>(values.size()));

            void* const object_handle{ new_object_a(vmhook::hotspot::current_jni_env, klass, method_id, values.data()) };
            cleanup.object_handle = object_handle;  // release at scope exit alongside the arg refs
            void* const oop{ vmhook::detail::jni_decode_object(object_handle) };
            if (!oop)
            {
                VMHOOK_LOG("{} jni_make_unique<{}>(): NewObjectA returned null for '{}' "
                           "(signature '{}') - likely a constructor exception or invalid args.",
                           vmhook::error_tag, typeid(wrapper_type).name(), class_name, signature);
                return nullptr;
            }

            // Verify that the resulting OOP really points to an instance of the
            // class we asked for - some JVMs / mixins can return an unrelated
            // klass through the same JNI handle, which then makes downstream
            // host code (Lunar's Adventure chat handler, Forge's GuiNewChat
            // wrapper) crash on "wrong" methods being dispatched.
            {
                const std::uint32_t narrow_klass{ *reinterpret_cast<const std::uint32_t*>(
                    reinterpret_cast<const std::uint8_t*>(oop) + 8) };
                void* const decoded_klass{ vmhook::hotspot::decode_klass_pointer(narrow_klass) };
                std::string returned_name{ "<unresolved>" };
                if (decoded_klass && vmhook::hotspot::is_valid_pointer(decoded_klass))
                {
                    auto* const k{ reinterpret_cast<vmhook::hotspot::klass*>(decoded_klass) };
                    if (auto* const sym{ k->get_name() }; sym && vmhook::hotspot::is_valid_pointer(sym))
                    {
                        returned_name = sym->to_string();
                    }
                }
                VMHOOK_LOG("{} jni_make_unique<{}>(): NewObjectA returned oop=0x{:016X} narrow_klass=0x{:08X} resolved_klass_name='{}' (expected '{}').",
                           returned_name == class_name ? vmhook::info_tag : vmhook::warning_tag,
                           typeid(wrapper_type).name(),
                           reinterpret_cast<std::uintptr_t>(oop),
                           narrow_klass,
                           returned_name,
                           class_name);
            }

            return std::make_unique<wrapper_type>(oop);
        }
    }

    // -------------------------------------------------------------------------
    // vmhook::jni — public surface for direct JNI access
    //
    // The actual implementations live in vmhook::detail::jni_* (and have for
    // a while) because most call sites in this header still reach them with
    // the long name.  This namespace is a thin, well-documented forwarding
    // layer that exposes the same helpers under the spelling we want library
    // users to write:
    //
    //     vmhook::jni::find_class("java/lang/String")
    //     vmhook::jni::get_method_id(klass, "<init>", "(I)V")
    //     vmhook::jni::new_string_utf("hello")
    //     vmhook::jni::make_unique<my_wrapper>("com/example/MyClass", 1, 2.0)
    //
    // The `jni_` prefix the underlying functions carry is redundant once a
    // dedicated namespace conveys the same thing.  vmhook::detail::jni_*
    // remains available for internal use and for source compatibility with
    // any existing consumer code; new code should prefer vmhook::jni::*.
    //
    // Thread-safety contract is the same as the underlying helpers: each
    // call requires the current thread to be JNI-attached (use
    // vmhook::hotspot::ensure_current_java_thread() if unsure).
    // -------------------------------------------------------------------------
    namespace jni
    {
        // The jvalue union HotSpot expects on the stack for CallXMethodA / NewObjectA.
        using value = vmhook::detail::jni_value;

        /*
            @brief Look up a JNI function pointer by table index.
            @tparam index      Zero-based index into the JNIEnv function table
                               (e.g. 6 = FindClass, 30 = NewObjectA, 33 = GetMethodID,
                               63 = CallVoidMethodA, 93 = CallNonvirtualVoidMethodA).
            @tparam function_t Expected function pointer type.
            @param env         JNIEnv* (use vmhook::hotspot::current_jni_env).
            @return  Callable function pointer, or nullptr if env is invalid.
        */
        template<std::size_t index, typename function_t>
        inline auto function(void* const env) noexcept
        {
            return vmhook::detail::jni_function<index, function_t>(env);
        }

        // Decode a JNI local-reference handle (jobject) to the raw heap OOP.
        inline auto decode_object(void* const object_handle) noexcept -> void*
        {
            return vmhook::detail::jni_decode_object(object_handle);
        }

        // Wrap a raw heap OOP as a fake JNI handle (pointer-to-storage).
        // handle_storage is caller-owned and must outlive the JNI call.
        inline auto oop_handle(void* const oop, void*& handle_storage) noexcept -> void*
        {
            return vmhook::detail::jni_oop_handle(oop, handle_storage);
        }

        // JNI FindClass.  Looks up a class through the calling thread's
        // context classloader.  See vmhook::find_class for a HotSpot-internal
        // lookup that doesn't need a classloader.
        inline auto find_class(const std::string_view class_name) noexcept -> void*
        {
            return vmhook::detail::jni_find_class(class_name);
        }

        // Multi-step lookup: tries thread context loader, system loader, then
        // Forge's LaunchClassLoader as last resort.  Returns the resolved
        // HotSpot Klass* (NOT a JNI handle).
        inline auto find_class_with_context_loader(const std::string_view class_name) noexcept
            -> vmhook::hotspot::klass*
        {
            return vmhook::detail::jni_find_class_with_context_loader(class_name);
        }

        // JNI ExceptionCheck + ExceptionClear.  Idempotent.
        inline auto exception_clear() noexcept -> void
        {
            vmhook::detail::jni_exception_clear();
        }

        // JNI GetObjectClass.  Returns a jclass local-ref handle.
        inline auto get_object_class(void* const object_handle) noexcept -> void*
        {
            return vmhook::detail::jni_get_object_class(object_handle);
        }

        // JNI GetMethodID (instance method).  Clears pending exceptions on failure.
        inline auto get_method_id(void* const klass,
                                  const std::string& name,
                                  const std::string& signature) noexcept -> void*
        {
            return vmhook::detail::jni_get_method_id(klass, name, signature);
        }

        // JNI GetStaticMethodID.
        inline auto get_static_method_id(void* const klass,
                                         const std::string& name,
                                         const std::string& signature) noexcept -> void*
        {
            return vmhook::detail::jni_get_static_method_id(klass, name, signature);
        }

        // JNI GetStaticFieldID.
        inline auto get_static_field_id(void* const klass,
                                        const std::string& name,
                                        const std::string& signature) noexcept -> void*
        {
            return vmhook::detail::jni_get_static_field_id(klass, name, signature);
        }

        // JNI GetStaticObjectField.  Returns a jobject local-ref handle.
        inline auto get_static_object_field(void* const klass, void* const field_id) noexcept -> void*
        {
            return vmhook::detail::jni_get_static_object_field(klass, field_id);
        }

        // JNI CallObjectMethodA.  args may be nullptr for zero-argument methods.
        inline auto call_object_method(void* const object,
                                       void* const method_id,
                                       const vmhook::jni::value* const args = nullptr) noexcept -> void*
        {
            return vmhook::detail::jni_call_object_method(object, method_id, args);
        }

        // JNI CallStaticObjectMethodA.
        inline auto call_static_object_method(void* const klass,
                                              void* const method_id,
                                              const vmhook::jni::value* const args = nullptr) noexcept -> void*
        {
            return vmhook::detail::jni_call_static_object_method(klass, method_id, args);
        }

        // Given a jclass handle (= java.lang.Class mirror), return the
        // underlying HotSpot Klass*.  Useful for converting a JNI lookup
        // back into a vmhook-internal pointer.
        inline auto klass_from_class_mirror(void* const class_handle) noexcept
            -> vmhook::hotspot::klass*
        {
            return vmhook::detail::jni_klass_from_class_mirror(class_handle);
        }

        // JNI NewStringUTF.  Returns a jstring local-ref handle.
        inline auto new_string_utf(const std::string_view value) noexcept -> void*
        {
            return vmhook::detail::jni_new_string_utf(value);
        }

        // JNI GetStringUTFChars (+ release).  Copies into a std::string.
        inline auto get_string_utf(void* const string_handle) noexcept -> std::string
        {
            return vmhook::detail::jni_get_string_utf(string_handle);
        }

        /*
            @brief Returns the JVM signature descriptor for a C++ argument type.
            @details  Compile-time table mapping; see jni_signature_for_arg for
                      the full list (std::string -> "Ljava/lang/String;",
                      bool -> "Z", float -> "F", etc.).
        */
        template<typename arg_type>
        inline auto signature_for_arg() noexcept -> std::string
        {
            return vmhook::detail::jni_signature_for_arg<arg_type>();
        }

        /*
            @brief Construct a Java object via JNI NewObjectA and return a wrapper.
            @details
            Allocates and runs the full constructor chain through the JVM's
            standard interpreter path - the resulting object has its mark
            word, klass pointer, AND every field (including inherited ones)
            properly initialised.  Prefer this entry point over the lower-level
            TLAB allocator when you have JNI available and the constructor
            does real work; see make_unique() in the parent namespace for the
            high-level wrapper that picks between this and the TLAB path.

            @tparam wrapper_type  C++ wrapper class deriving from vmhook::object_base.
            @tparam args_t        Constructor argument types.
            @param class_name     Internal JVM class name with '/' separators.
            @param args           Constructor arguments.
            @return  Constructed wrapper, or nullptr on failure (each failure
                     path logs via VMHOOK_LOG).
        */
        template<typename wrapper_type, typename... args_t>
        inline auto make_unique(const std::string& class_name, args_t&&... args) noexcept
            -> std::unique_ptr<wrapper_type>
        {
            return vmhook::detail::jni_make_unique<wrapper_type>(class_name, std::forward<args_t>(args)...);
        }
    }

    // -------------------------------------------------------------------------
    // Class resolution under multiple classloaders.
    //
    // vmhook::find_class() resolves a class by NAME across the whole
    // ClassLoaderDataGraph and returns the first match.  When a process has two
    // copies of a class under different loaders (modded games with a custom
    // launcher loader, OSGi, app servers, any plugin host), "first by name" is
    // graph-iteration-order-dependent and routinely resolves the WRONG copy —
    // the only symptom being a ClassCastException thrown deep in host code when
    // the result is handed back.  These helpers give the corrective half:
    // "resolve the copy visible from an object I already hold", and "make every
    // find_class() consumer pick up that copy".  Surfaced by the NPNOQOL
    // deep-dive (Lunar/Forge ship duplicate net.kyori.* / com.mojang.* classes).
    // -------------------------------------------------------------------------

    /*
        @brief Resolves a class through the classloader of a live anchor object.
        @details
        Walks anchor_oop -> getClass() -> getClassLoader() -> loadClass(name),
        forcing the copy of `class_name` visible from THAT object's loader rather
        than whatever find_class() happens to hit first.  Use it to disambiguate
        when the JVM has more than one class of the same name.

        Complexity: O(JNI loadClass).  Exception safety: noexcept — clears any
        pending JNI exception and returns nullptr on any failure (null anchor,
        no JVM, class not visible from that loader).

        @param anchor_oop   Raw decoded heap OOP of an object whose loader to use.
        @param class_name   Internal ('/'-separated) JVM class name.
        @return  The Klass* of the loader-specific copy, or nullptr.
    */
    inline auto find_class_via_oop(void* const anchor_oop, const std::string_view class_name) noexcept
        -> vmhook::hotspot::klass*
    {
        if (!anchor_oop || !vmhook::hotspot::ensure_current_java_thread())
        {
            return nullptr;
        }

        void* storage{};
        void* const anchor_handle{ vmhook::detail::jni_oop_handle(anchor_oop, storage) };
        void* const anchor_class_handle{ vmhook::detail::jni_get_object_class(anchor_handle) };
        if (!anchor_class_handle)
        {
            vmhook::detail::jni_exception_clear();
            return nullptr;
        }

        void* const class_class{ vmhook::detail::jni_get_object_class(anchor_class_handle) };
        if (!class_class)
        {
            vmhook::detail::jni_exception_clear();
            vmhook::detail::jni_delete_local_ref(anchor_class_handle);
            return nullptr;
        }

        void* const get_loader_id{ vmhook::detail::jni_get_method_id(
            class_class, "getClassLoader", "()Ljava/lang/ClassLoader;") };
        vmhook::detail::jni_delete_local_ref(class_class);
        if (!get_loader_id)
        {
            vmhook::detail::jni_delete_local_ref(anchor_class_handle);
            return nullptr;
        }

        void* const classloader{ vmhook::detail::jni_call_object_method(
            anchor_class_handle, get_loader_id, nullptr) };
        vmhook::detail::jni_delete_local_ref(anchor_class_handle);
        vmhook::detail::jni_exception_clear();
        if (!classloader)
        {
            return nullptr;
        }

        void* const cl_class{ vmhook::detail::jni_get_object_class(classloader) };
        if (!cl_class)
        {
            vmhook::detail::jni_delete_local_ref(classloader);
            return nullptr;
        }
        void* const load_class_id{ vmhook::detail::jni_get_method_id(
            cl_class, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;") };
        vmhook::detail::jni_delete_local_ref(cl_class);
        if (!load_class_id)
        {
            vmhook::detail::jni_delete_local_ref(classloader);
            return nullptr;
        }

        std::string dotted_name{ class_name };
        std::replace(dotted_name.begin(), dotted_name.end(), '/', '.');
        void* const name_string{ vmhook::detail::jni_new_string_utf(dotted_name) };
        if (!name_string)
        {
            vmhook::detail::jni_delete_local_ref(classloader);
            return nullptr;
        }

        vmhook::detail::jni_value args[1]{};
        args[0].l = name_string;
        void* const class_mirror{ vmhook::detail::jni_call_object_method(classloader, load_class_id, args) };
        vmhook::detail::jni_delete_local_ref(name_string);
        vmhook::detail::jni_delete_local_ref(classloader);
        vmhook::detail::jni_exception_clear();
        if (!class_mirror)
        {
            return nullptr;
        }

        vmhook::hotspot::klass* const result{ vmhook::detail::jni_klass_from_class_mirror(class_mirror) };
        vmhook::detail::jni_delete_local_ref(class_mirror);
        return result;
    }

    /*
        @brief Installs / replaces the find_class cache entry for `class_name`.
        @details
        After this call, every subsequent vmhook::find_class(class_name) returns
        `k` directly without walking the ClassLoaderDataGraph.  Pair it with
        find_class_via_oop() to nail the correct loader's copy once and have the
        whole SDK (every wrapper that calls find_class) follow it — the supported
        replacement for reaching into the internal cache.  Passing a null `k`
        seeds a negative entry; prefer evict_class_lookup() to actually forget.

        Complexity: O(1).  Exception safety: noexcept.
    */
    inline auto override_class_lookup(const std::string_view class_name, vmhook::hotspot::klass* const k) noexcept
        -> void
    {
        try
        {
            std::lock_guard<std::mutex> lock{ vmhook::klass_lookup_cache_mutex };
            vmhook::klass_lookup_cache[std::string{ class_name }] = k;
        }
        catch (...)
        {
        }
    }

    /*
        @brief Forgets any cached find_class resolution for `class_name`.
        @details Next find_class(class_name) re-walks the graph (or re-resolves).
        Complexity: O(1).  Exception safety: noexcept.
    */
    inline auto evict_class_lookup(const std::string_view class_name) noexcept
        -> void
    {
        try
        {
            std::lock_guard<std::mutex> lock{ vmhook::klass_lookup_cache_mutex };
            vmhook::klass_lookup_cache.erase(std::string{ class_name });
        }
        catch (...)
        {
        }
    }

    /*
        @brief Convenience: anchor a set of classes to an object's loader in one call.
        @details
        For each name, find_class_via_oop(anchor_oop, name) then
        override_class_lookup(name, resolved) so every later find_class() picks the
        anchor loader's copy.  Returns true only if EVERY name resolved — callers
        that anchor against an object which only becomes available later (e.g. once
        a world/context is loaded) can poll until it returns true.  Names that
        don't resolve are left un-overridden.

        Complexity: O(N · JNI loadClass).  Exception safety: noexcept.
    */
    inline auto reanchor_classes_via_oop(void* const anchor_oop,
                                         std::initializer_list<std::string_view> class_names) noexcept
        -> bool
    {
        if (!anchor_oop)
        {
            return false;
        }
        bool all_resolved{ true };
        for (const std::string_view class_name : class_names)
        {
            vmhook::hotspot::klass* const resolved{ vmhook::find_class_via_oop(anchor_oop, class_name) };
            if (resolved)
            {
                vmhook::override_class_lookup(class_name, resolved);
            }
            else
            {
                all_resolved = false;
            }
        }
        return all_resolved;
    }

    /*
        @brief Constructs a new Java object and returns a C++ wrapper.
        @tparam T The C++ wrapper class (must derive from vmhook::object).
        @param args Arguments to pass to the Java constructor.
        @return A std::unique_ptr<T> wrapping the new Java object, or nullptr on failure.
        @details
        Looks up the Java class for type T (via register_class<T>()), allocates a new
        instance, and calls the appropriate constructor with the provided arguments.

        Usage:
            vmhook::register_class<player>("com/example/Player");
            auto p = vmhook::make_unique<player>("Bob", 12);

        @note This is a minimal implementation. Full constructor dispatch requires
              parsing method descriptors and setting up interpreter frames.
    */
    template<typename wrapper_type, typename... args_t>
    static auto make_unique(args_t&&... args)
        -> std::unique_ptr<wrapper_type>
    {
        if (!vmhook::hotspot::ensure_current_java_thread())
        {
            VMHOOK_LOG("{} vmhook::make_unique<{}>(): failed to attach current native thread to the JVM.", vmhook::error_tag, typeid(wrapper_type).name());
            return nullptr;
        }

        auto map_entry{ vmhook::type_to_class_map.find(std::type_index{ typeid(wrapper_type) }) };
        if (map_entry == vmhook::type_to_class_map.end())
        {
            VMHOOK_LOG("{} vmhook::make_unique<{}>(): type not registered.", vmhook::error_tag, typeid(wrapper_type).name());
            return nullptr;
        }

        // Always prefer NewObjectA: it lets the JVM allocate the object,
        // set its mark word + (compressed) klass pointer, AND run the full
        // constructor chain via the standard interpreter path.  That last
        // bit matters - the TLAB fallback below dispatches <init> through
        // method_proxy::call_jni, which uses CallVoidMethodA on <init>;
        // that is JNI undefined behaviour and on some JDKs (notably JDK
        // 21+ where StubRoutines::_call_stub_entry is missing from
        // VMStructs) it skips the constructor body entirely, leaving
        // super-class fields (e.g. ChatComponentStyle.siblings, .style)
        // null.  Host code (Lunar's Adventure chat adapter, Forge's
        // wrapped GuiNewChat) then NPEs / throws when it walks those
        // fields.  Trying jni_make_unique first removes that whole class
        // of bug.
        if (std::unique_ptr<wrapper_type> jni_result{ vmhook::detail::jni_make_unique<wrapper_type>(map_entry->second, std::forward<args_t>(args)...) })
        {
            VMHOOK_LOG("{} vmhook::make_unique<{}>(): constructed via NewObjectA.",
                       vmhook::info_tag, typeid(wrapper_type).name());
            return jni_result;
        }

        VMHOOK_LOG("{} vmhook::make_unique<{}>(): NewObjectA path unavailable - "
                   "falling back to TLAB + <init>.  Object fields may be only "
                   "partially initialised on this JDK.",
                   vmhook::warning_tag, typeid(wrapper_type).name());

        vmhook::hotspot::klass* const klass{ vmhook::find_class(map_entry->second) };
        if (!klass)
        {
            VMHOOK_LOG("{} vmhook::make_unique<{}>(): find_class('{}') returned null - class not loaded.",
                       vmhook::error_tag, typeid(wrapper_type).name(), map_entry->second);
            return nullptr;
        }

        const std::size_t raw_size{ klass->get_instance_size() };
        const std::size_t object_size{ (raw_size + 7u) & ~static_cast<std::size_t>(7u) };
        if (object_size == 0)
        {
            VMHOOK_LOG("{} vmhook::make_unique<{}>(): failed to read HotSpot instance size.", vmhook::error_tag, typeid(wrapper_type).name());
            return nullptr;
        }

        vmhook::hotspot::java_thread* const thread{ vmhook::hotspot::find_allocation_thread() };
        void* object_pointer{};
        if (thread && vmhook::hotspot::is_valid_pointer(thread))
        {
            object_pointer = thread->allocate_tlab(object_size);
        }

        if (!object_pointer)
        {
            std::int32_t visited_threads{ 0 };
            for (vmhook::hotspot::java_thread* candidate{ vmhook::hotspot::find_any_java_thread() };
                candidate && vmhook::hotspot::is_valid_pointer(candidate) && visited_threads < 256;
                candidate = candidate->get_next(), ++visited_threads)
            {
                object_pointer = candidate->allocate_tlab(object_size);
                if (object_pointer)
                {
                    vmhook::hotspot::last_java_thread.store(candidate, std::memory_order_relaxed);
                    break;
                }
            }
        }

        if (!object_pointer)
        {
            object_pointer = vmhook::hotspot::allocate_from_threads_list(object_size);
        }

        if (!object_pointer)
        {
            VMHOOK_LOG("{} vmhook::make_unique<{}>(): current JavaThread TLAB has no room for {} bytes.", vmhook::warning_tag, typeid(wrapper_type).name(), object_size);
            return nullptr;
        }

        std::memset(object_pointer, 0, object_size);

        static const vmhook::hotspot::vm_struct_entry_t* const mark_entry{ []()
            -> const vmhook::hotspot::vm_struct_entry_t*
            {
                {
                    auto* entry{ vmhook::hotspot::iterate_struct_entries("oopDesc", "_mark") };
                    if (entry)
                    {
                        return entry;
                    }
                }
                return vmhook::hotspot::iterate_struct_entries("oopDesc", "_markWord");
            }()
        };
        static const vmhook::hotspot::vm_struct_entry_t* const compressed_klass_entry{ vmhook::hotspot::iterate_struct_entries("oopDesc", "_metadata._compressed_klass") };
        static const vmhook::hotspot::vm_struct_entry_t* const klass_entry{ vmhook::hotspot::iterate_struct_entries("oopDesc", "_metadata._klass") };

        const std::size_t mark_offset{ mark_entry ? static_cast<std::size_t>(mark_entry->offset) : 0u };
        *reinterpret_cast<std::uintptr_t*>(reinterpret_cast<std::uint8_t*>(object_pointer) + mark_offset) = klass->get_prototype_header();

        if (compressed_klass_entry)
        {
            *reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uint8_t*>(object_pointer) + compressed_klass_entry->offset) = vmhook::hotspot::encode_klass_pointer(klass);
        }
        else if (klass_entry)
        {
            *reinterpret_cast<vmhook::hotspot::klass**>(reinterpret_cast<std::uint8_t*>(object_pointer) + klass_entry->offset) = klass;
        }
        else
        {
            *reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uint8_t*>(object_pointer) + 8u) = vmhook::hotspot::encode_klass_pointer(klass);
        }

        auto result{ std::make_unique<wrapper_type>(object_pointer) };

        if constexpr (requires(wrapper_type & wrapper, args_t&&... construct_args)
        {
            wrapper.construct(std::forward<args_t>(construct_args)...);
        })
        {
            result->construct(std::forward<args_t>(args)...);
        }
        else if constexpr (sizeof...(args_t) > 0)
        {
            VMHOOK_LOG("{} vmhook::make_unique<{}>(): object allocated, but wrapper has no matching construct(...) method for the provided arguments.", vmhook::warning_tag, typeid(wrapper_type).name());
        }

        return result;
    }

    // --- Field access ---------------------------------------------------------

    /*
        @brief Cache of field entries keyed by klass* then field name.
        @details
        Populated lazily by find_field() on the first lookup of each (klass, name) pair.
        Raw klass pointers are safe as keys for process-lifetime injection; class unloading
        would invalidate them, but that is not a concern for the typical use case here.
    */
    inline std::unordered_map<vmhook::hotspot::klass*, std::unordered_map<std::string, vmhook::hotspot::field_entry_t>> g_field_cache{};
    inline std::mutex g_field_cache_mutex{};

    /*
        @brief Looks up and caches a field entry for a named field on a klass.
        @param target_klass  The klass that declares the field (obtain via find_class()).
                             Only the declaring class is searched - not superclasses.
        @param name          The exact Java field name.
        @return The cached field_entry_t, or std::nullopt if the field is not found.
        @details
        On the first call for a given (target_klass, name) pair the full InstanceKlass._fields
        array is walked; subsequent calls return the cached result directly.

        Thread-safe: cache reads / inserts are serialised via g_field_cache_mutex.
        Without that, concurrent worker threads racing through find_field() can
        trip an unordered_map rehash mid-insert and corrupt the bucket array.
        The superclass walk itself is done outside the lock (it only reads
        immutable HotSpot metadata) so the lock is only held for the
        find / insert.
    */
    static auto find_field(vmhook::hotspot::klass* const target_klass, const std::string_view name)
        -> std::optional<vmhook::hotspot::field_entry_t>
    {
        if (!target_klass || !vmhook::hotspot::is_valid_pointer(target_klass))
        {
            VMHOOK_LOG("{} vmhook::find_field() for '{}': klass pointer is null.", vmhook::error_tag, name);
            return std::nullopt;
        }

        const std::string name_str{ name };

        {
            std::lock_guard<std::mutex> lock{ vmhook::g_field_cache_mutex };
            if (const auto klass_entry{ vmhook::g_field_cache.find(target_klass) };
                klass_entry != vmhook::g_field_cache.end())
            {
                if (const auto field_cache_entry{ klass_entry->second.find(name_str) };
                    field_cache_entry != klass_entry->second.end())
                {
                    return field_cache_entry->second;
                }
            }
        }

        // Walk the superclass chain so that inherited fields are found.  Done
        // OUTSIDE the cache lock — the HotSpot metadata is immutable for
        // already-loaded classes and the walk is the slow part; we don't want
        // every field lookup serialising on each other.
        for (vmhook::hotspot::klass* k{ target_klass }; k != nullptr; k = k->get_super())
        {
            auto entry{ k->find_field(name) };
            if (entry)
            {
                // Record WHICH klass in the chain declared the field.  This is
                // load-bearing for inherited STATIC fields: their offset is
                // relative to the declaring class's own mirror, so the static
                // accessors below resolve `declaring_klass->get_java_mirror()`
                // rather than the start klass's (smaller, different) mirror —
                // reading the latter returned garbage and could run off the end
                // of the subclass mirror oop into adjacent heap (a fatal AV).
                entry->declaring_klass = k;
                std::lock_guard<std::mutex> lock{ vmhook::g_field_cache_mutex };
                vmhook::g_field_cache[target_klass].emplace(name_str, *entry);
                return entry;
            }
        }

        VMHOOK_LOG("{} vmhook::find_field() for '{}': field not found in class hierarchy.", vmhook::error_tag, name);
        return std::nullopt;
    }

    /*
        @brief Reads an instance field from a Java object by name.
        @tparam T      The C++ scalar type to read the field as (int, float, bool,
                       double, long long, short, char, std::byte, uint32_t, etc.).
        @param object  Decoded pointer to the Java object (not a compressed OOP).
                       Obtain it from frame->get_arguments<void*>() which already
                       calls decode_oop_pointer() internally.
        @param k       The klass that declares the field (from find_class()).
        @param name    The exact Java field name.
        @return The field value as T, or a default-constructed T on failure.
        @note For reference-type fields specify T = uint32_t to receive the raw
              compressed OOP, then pass it to vmhook::hotspot::decode_oop_pointer() yourself.
    */
    template<typename value_type>
    static auto get_field(void* const object, vmhook::hotspot::klass* const target_klass, const std::string_view name)
        -> value_type
    {
        static_assert(std::is_trivially_copyable_v<value_type>, "get_field<value_type>: value_type must be trivially copyable.");

        try
        {
            const auto entry{ vmhook::find_field(target_klass, name) };

            if (!entry)
            {
                throw vmhook::exception{ std::format("Field '{}' not found.", name) };
            }

            if (entry->is_static)
            {
                // A static field's offset is relative to the DECLARING class's
                // mirror, not target_klass's — they differ for an inherited
                // static (different, differently-sized mirror oops).  Fall back
                // to target_klass only when the declaring klass is unknown.
                vmhook::hotspot::klass* const mirror_klass{ entry->declaring_klass ? entry->declaring_klass : target_klass };
                void* const mirror{ mirror_klass->get_java_mirror() };
                if (!mirror || !vmhook::hotspot::is_valid_pointer(mirror))
                {
                    throw vmhook::exception{ "Failed to retrieve java.lang.Class mirror." };
                }

                value_type result{};
                std::memcpy(&result, reinterpret_cast<const std::uint8_t*>(mirror) + entry->offset, sizeof(value_type));
                return result;
            }

            if (!object || !vmhook::hotspot::is_valid_pointer(object))
            {
                throw vmhook::exception{ "Object pointer is null or invalid." };
            }

            value_type result{};
            std::memcpy(&result, reinterpret_cast<const std::uint8_t*>(object) + entry->offset, sizeof(value_type));
            return result;
        }
        catch (const std::exception& exception)
        {
            VMHOOK_LOG("{} vmhook::get_field<{}>('{}') {}", vmhook::error_tag, typeid(value_type).name(), name, exception.what());
            return value_type{};
        }
    }

    /*
        @brief Writes a value to an instance field of a Java object.
        @tparam T      The C++ scalar type to write.
        @param object  Decoded pointer to the Java object instance.
        @param k       The klass that declares the field.
        @param name    The exact Java field name.
        @param value   The value to write.
        @note Writing a reference-type field requires a properly encoded compressed OOP.
              Encoding is: (real_address - narrow_oop_base) >> narrow_oop_shift.
    */
    template<typename value_type>
    static auto set_field(void* const object, vmhook::hotspot::klass* const target_klass, const std::string_view name, const value_type value)
        -> void
    {
        static_assert(std::is_trivially_copyable_v<value_type>, "set_field<value_type>: value_type must be trivially copyable.");

        try
        {
            const auto entry{ vmhook::find_field(target_klass, name) };

            if (!entry)
            {
                throw vmhook::exception{ std::format("Field '{}' not found.", name) };
            }

            if (entry->is_static)
            {
                // Static offset is relative to the DECLARING class's mirror; see
                // the matching note in get_field<value_type>.  Inherited statics
                // would otherwise be written into the wrong (subclass) mirror.
                vmhook::hotspot::klass* const mirror_klass{ entry->declaring_klass ? entry->declaring_klass : target_klass };
                void* const mirror{ mirror_klass->get_java_mirror() };
                if (!mirror || !vmhook::hotspot::is_valid_pointer(mirror))
                {
                    throw vmhook::exception{ "Failed to retrieve java.lang.Class mirror." };
                }

                std::memcpy(reinterpret_cast<std::uint8_t*>(mirror) + entry->offset, &value, sizeof(value_type));
                return;
            }

            if (!object || !vmhook::hotspot::is_valid_pointer(object))
            {
                throw vmhook::exception{ "Object pointer is null or invalid." };
            }

            std::memcpy(reinterpret_cast<std::uint8_t*>(object) + entry->offset, &value, sizeof(value_type));
        }
        catch (const std::exception& exception)
        {
            VMHOOK_LOG("{} vmhook::set_field<{}>('{}') {}", vmhook::error_tag, typeid(value_type).name(), name, exception.what());
        }
    }

    // --- Java object / array / string allocation helpers ---------------------

    /*
        @brief Allocates a raw Java object of the given klass directly from a thread TLAB.
        @details
        Performs a zero-initialised TLAB allocation and stamps the HotSpot object header:
          - oopDesc._mark / _markWord  <- klass->get_prototype_header()
          - oopDesc._metadata._compressed_klass (or _klass) <- encoded klass pointer
        Does not call any Java constructor; the resulting object is zeroed beyond the header.
        Used as the low-level allocation primitive by make_java_string() and make_java_array().

        Allocation strategy (in order):
          1. Thread-local current_java_thread TLAB.
          2. Walk up to 256 threads from find_any_java_thread().
          3. allocate_from_threads_list() (SMR list).
        Falls back through all three before returning nullptr.

        Complexity: O(1) on the fast path; O(N) on the fallback walk.
        Exception safety: noexcept — returns nullptr on any failure.

        @param klass           HotSpot klass to stamp into the object header.
        @param requested_size  Minimum allocation size in bytes; rounded up to 8-byte alignment.
        @return  Pointer to zeroed, header-stamped object memory, or nullptr on failure.
    */
    inline auto make_java_object(vmhook::hotspot::klass* const klass, const std::size_t requested_size) noexcept
        -> void*
    {
        if (!vmhook::hotspot::ensure_current_java_thread())
        {
            VMHOOK_LOG("{} vmhook::make_java_object(): ensure_current_java_thread() failed - "
                       "cannot allocate without an attached JavaThread.",
                       vmhook::error_tag);
            return nullptr;
        }

        if (!klass || requested_size == 0)
        {
            VMHOOK_LOG("{} vmhook::make_java_object(): invalid arguments (klass={}, "
                       "requested_size={}).",
                       vmhook::error_tag, static_cast<const void*>(klass), requested_size);
            return nullptr;
        }

        const std::size_t object_size{ (requested_size + 7u) & ~static_cast<std::size_t>(7u) };
        vmhook::hotspot::java_thread* const thread{ vmhook::hotspot::find_allocation_thread() };

        void* object_pointer{};
        if (thread && vmhook::hotspot::is_valid_pointer(thread))
        {
            object_pointer = thread->allocate_tlab(object_size);
        }

        if (!object_pointer)
        {
            std::int32_t visited_threads{ 0 };
            for (vmhook::hotspot::java_thread* candidate{ vmhook::hotspot::find_any_java_thread() };
                candidate && vmhook::hotspot::is_valid_pointer(candidate) && visited_threads < 256;
                candidate = candidate->get_next(), ++visited_threads)
            {
                object_pointer = candidate->allocate_tlab(object_size);
                if (object_pointer)
                {
                    vmhook::hotspot::last_java_thread.store(candidate, std::memory_order_relaxed);
                    break;
                }
            }
        }

        if (!object_pointer)
        {
            object_pointer = vmhook::hotspot::allocate_from_threads_list(object_size);
        }

        if (!object_pointer)
        {
            VMHOOK_LOG("{} vmhook::make_java_object(): TLAB allocation failed across all "
                       "candidate JavaThreads ({} bytes).",
                       vmhook::warning_tag, object_size);
            return nullptr;
        }

        std::memset(object_pointer, 0, object_size);

        static const vmhook::hotspot::vm_struct_entry_t* const mark_entry{ []()
            -> const vmhook::hotspot::vm_struct_entry_t*
            {
                if (auto* const entry{ vmhook::hotspot::iterate_struct_entries("oopDesc", "_mark") })
                {
                    return entry;
                }

                return vmhook::hotspot::iterate_struct_entries("oopDesc", "_markWord");
            }()
        };
        static const vmhook::hotspot::vm_struct_entry_t* const compressed_klass_entry{ vmhook::hotspot::iterate_struct_entries("oopDesc", "_metadata._compressed_klass") };
        static const vmhook::hotspot::vm_struct_entry_t* const klass_entry{ vmhook::hotspot::iterate_struct_entries("oopDesc", "_metadata._klass") };

        const std::size_t mark_offset{ mark_entry ? static_cast<std::size_t>(mark_entry->offset) : 0u };
        *reinterpret_cast<std::uintptr_t*>(reinterpret_cast<std::uint8_t*>(object_pointer) + mark_offset) = klass->get_prototype_header();

        if (compressed_klass_entry)
        {
            *reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uint8_t*>(object_pointer) + compressed_klass_entry->offset) = vmhook::hotspot::encode_klass_pointer(klass);
        }
        else if (klass_entry)
        {
            *reinterpret_cast<vmhook::hotspot::klass**>(reinterpret_cast<std::uint8_t*>(object_pointer) + klass_entry->offset) = klass;
        }
        else
        {
            *reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uint8_t*>(object_pointer) + 8u) = vmhook::hotspot::encode_klass_pointer(klass);
        }

        return object_pointer;
    }

    /*
        @brief Allocates a raw Java array of the given element type and length.
        @details
        Finds the array klass by class_name (e.g. "[B" for byte[], "[C" for char[]),
        allocates `array_header_size + length * element_size` bytes via make_java_object(),
        and writes the Java array length into the 32-bit slot at byte offset 12 (the standard
        HotSpot arrayOop _length field layout on 64-bit VMs with UseCompressedOops).

        The caller is responsible for filling in the element data after this call.

        Complexity: O(1) on the fast TLAB path; O(N) on the fallback.
        Exception safety: noexcept — returns nullptr on any failure.

        @param class_name    Internal JVM array type descriptor (e.g. "[B", "[C", "[Ljava/lang/Object;").
        @param length        Number of elements; must be >= 0.
        @param element_size  Size in bytes of each array element.
        @return  Pointer to the raw array OOP with header and length initialised, or nullptr on failure.
    */
    inline auto make_java_array(const std::string_view class_name, const std::int32_t length, const std::size_t element_size) noexcept
        -> void*
    {
        if (length < 0)
        {
            VMHOOK_LOG("{} vmhook::make_java_array('{}'): negative length {}.",
                       vmhook::error_tag, class_name, length);
            return nullptr;
        }

        vmhook::hotspot::klass* array_klass{ vmhook::find_class(class_name) };
        // JDK 8 fallback: find_class resolves names through ClassLoader.loadClass,
        // which rejects array descriptors ("[B", "[Ljava/lang/Object;").  On JDK 9+
        // the array klass is reachable by name so this branch never triggers; on
        // JDK 8 it misses, so fall back to JNIEnv::FindClass, which DOES accept
        // array descriptors, and convert the returned jclass mirror to its Klass*.
        if (!array_klass && !class_name.empty() && class_name.front() == '[')
        {
            if (void* const h{ vmhook::detail::jni_find_class(class_name) })
            {
                array_klass = vmhook::detail::jni_klass_from_class_mirror(h);
                vmhook::detail::jni_delete_local_ref(h);
                vmhook::detail::jni_exception_clear();
            }
        }
        if (!array_klass)
        {
            VMHOOK_LOG("{} vmhook::make_java_array('{}'): array klass not found - the array "
                       "descriptor is not loaded yet, or class_name uses the wrong syntax "
                       "(e.g. should be \"[B\" not \"byte[]\").",
                       vmhook::error_tag, class_name);
            return nullptr;
        }

        constexpr std::size_t array_header_size{ 16u };
        void* const array_oop{ vmhook::make_java_object(array_klass, array_header_size + static_cast<std::size_t>(length) * element_size) };
        if (!array_oop)
        {
            VMHOOK_LOG("{} vmhook::make_java_array('{}'): make_java_object failed for {} elements "
                       "({} bytes total).",
                       vmhook::error_tag, class_name, length,
                       array_header_size + static_cast<std::size_t>(length) * element_size);
            return nullptr;
        }

        *reinterpret_cast<std::int32_t*>(reinterpret_cast<std::uint8_t*>(array_oop) + 12u) = length;
        return array_oop;
    }

    /*
        @brief Allocates a new java.lang.String OOP from a UTF-8 string_view.
        @details
        Allocates a java.lang.String instance via make_java_object(), then detects at
        runtime whether the JVM uses compact strings (JDK 9+, "coder" field present) or
        classic char[] strings (JDK 8):
          - Compact:  allocates a byte[] ("[B"), copies UTF-8 bytes directly, sets coder=0 (LATIN1).
          - Classic:  allocates a char[] ("[C"), widens each byte to uint16.
        Sets the "value" field of the String to the encoded OOP of the backing array.
        Capped at 4096 characters to avoid oversized allocations.

        Complexity: O(N) where N = length of value.
        Exception safety: noexcept — returns nullptr on any allocation failure.

        @param value  UTF-8 text to encode as a Java String (capped at 4096 chars).
        @return  Raw java.lang.String OOP, or nullptr on failure.
    */
    inline auto make_java_string(const std::string_view value) noexcept
        -> void*
    {
        vmhook::hotspot::klass* const string_klass{ vmhook::find_class("java/lang/String") };
        if (!string_klass)
        {
            VMHOOK_LOG("{} vmhook::make_java_string(): find_class('java/lang/String') returned null. "
                       "This indicates a very early bootstrap state or a broken VMStructs lookup.",
                       vmhook::error_tag);
            return nullptr;
        }

        void* const string_oop{ vmhook::make_java_object(string_klass, string_klass->get_instance_size()) };
        if (!string_oop)
        {
            VMHOOK_LOG("{} vmhook::make_java_string(): make_java_object for java.lang.String failed "
                       "(instance_size={} bytes).",
                       vmhook::error_tag, string_klass->get_instance_size());
            return nullptr;
        }

        const bool compact_string{ string_klass->find_field("coder").has_value() };

        // Decode the UTF-8 input into UTF-16 code units (astral code points
        // become a surrogate pair).  The previous implementation copied the raw
        // UTF-8 BYTES straight into a LATIN1 byte[] / char[], which conflated
        // byte count with char count and corrupted every non-ASCII string —
        // e.g. "é" (0xC3 0xA9) turned into the two chars U+00C3 U+00A9.
        std::vector<std::uint16_t> units;
        units.reserve(value.size());
        for (std::size_t i{ 0 }; i < value.size(); )
        {
            const std::uint8_t b0{ static_cast<std::uint8_t>(value[i]) };
            std::uint32_t cp{ 0xFFFDu }; // U+FFFD replacement for malformed input
            std::size_t adv{ 1 };
            if (b0 < 0x80u)
            {
                cp = b0;
            }
            else if ((b0 & 0xE0u) == 0xC0u && (i + 1) < value.size())
            {
                cp = ((b0 & 0x1Fu) << 6)
                   |  (static_cast<std::uint8_t>(value[i + 1]) & 0x3Fu);
                adv = 2;
            }
            else if ((b0 & 0xF0u) == 0xE0u && (i + 2) < value.size())
            {
                cp = ((b0 & 0x0Fu) << 12)
                   | ((static_cast<std::uint8_t>(value[i + 1]) & 0x3Fu) << 6)
                   |  (static_cast<std::uint8_t>(value[i + 2]) & 0x3Fu);
                adv = 3;
            }
            else if ((b0 & 0xF8u) == 0xF0u && (i + 3) < value.size())
            {
                cp = ((b0 & 0x07u) << 18)
                   | ((static_cast<std::uint8_t>(value[i + 1]) & 0x3Fu) << 12)
                   | ((static_cast<std::uint8_t>(value[i + 2]) & 0x3Fu) << 6)
                   |  (static_cast<std::uint8_t>(value[i + 3]) & 0x3Fu);
                adv = 4;
            }

            if (cp >= 0x10000u)
            {
                cp -= 0x10000u;
                units.push_back(static_cast<std::uint16_t>(0xD800u + (cp >> 10)));
                units.push_back(static_cast<std::uint16_t>(0xDC00u + (cp & 0x3FFu)));
            }
            else
            {
                units.push_back(static_cast<std::uint16_t>(cp));
            }
            i += adv;
        }

        // Bound the char count to the same 4096 ceiling read_java_string enforces.
        if (units.size() > 4096u)
        {
            units.resize(4096u);
        }
        const std::int32_t char_count{ static_cast<std::int32_t>(units.size()) };

        // A compact (JDK 9+) String may use the LATIN1 coder only when every
        // code unit fits in one byte; otherwise it must use the UTF16 coder.
        bool all_latin1{ true };
        for (const std::uint16_t unit : units)
        {
            if (unit > 0xFFu) { all_latin1 = false; break; }
        }

        if (compact_string && all_latin1)
        {
            void* const value_array{ vmhook::make_java_array("[B", char_count, sizeof(std::uint8_t)) };
            if (!value_array)
            {
                VMHOOK_LOG("{} vmhook::make_java_string(): failed to allocate the byte[] backing "
                           "array (compact LATIN1 path, chars={}).",
                           vmhook::error_tag, char_count);
                return nullptr;
            }

            auto* const bytes{ reinterpret_cast<std::uint8_t*>(value_array) + 16u };
            for (std::int32_t index{ 0 }; index < char_count; ++index)
            {
                bytes[index] = static_cast<std::uint8_t>(units[static_cast<std::size_t>(index)]);
            }
            vmhook::set_field(string_oop, string_klass, "value", vmhook::hotspot::encode_oop_pointer(value_array));
            vmhook::set_field<std::uint8_t>(string_oop, string_klass, "coder", 0u);
        }
        else if (compact_string)
        {
            // UTF16 coder: byte[] holding 2 bytes per code unit, native-endian
            // (HotSpot StringUTF16 stores chars in platform byte order; on the
            // x64 CI that is little-endian, matching the units' in-memory bytes).
            void* const value_array{ vmhook::make_java_array("[B", char_count * 2, sizeof(std::uint8_t)) };
            if (!value_array)
            {
                VMHOOK_LOG("{} vmhook::make_java_string(): failed to allocate the byte[] backing "
                           "array (compact UTF16 path, chars={}).",
                           vmhook::error_tag, char_count);
                return nullptr;
            }

            std::memcpy(reinterpret_cast<std::uint8_t*>(value_array) + 16u,
                        units.data(), static_cast<std::size_t>(char_count) * 2u);
            vmhook::set_field(string_oop, string_klass, "value", vmhook::hotspot::encode_oop_pointer(value_array));
            vmhook::set_field<std::uint8_t>(string_oop, string_klass, "coder", 1u);
        }
        else
        {
            void* const value_array{ vmhook::make_java_array("[C", char_count, sizeof(std::uint16_t)) };
            if (!value_array)
            {
                VMHOOK_LOG("{} vmhook::make_java_string(): failed to allocate the char[] backing "
                           "array (classic string path, chars={}).",
                           vmhook::error_tag, char_count);
                return nullptr;
            }

            auto* const chars{ reinterpret_cast<std::uint16_t*>(reinterpret_cast<std::uint8_t*>(value_array) + 16u) };
            for (std::int32_t index{ 0 }; index < char_count; ++index)
            {
                chars[index] = units[static_cast<std::size_t>(index)];
            }

            vmhook::set_field(string_oop, string_klass, "value", vmhook::hotspot::encode_oop_pointer(value_array));

            if (string_klass->find_field("offset").has_value())
            {
                vmhook::set_field<std::int32_t>(string_oop, string_klass, "offset", 0);
            }

            if (string_klass->find_field("count").has_value())
            {
                vmhook::set_field<std::int32_t>(string_oop, string_klass, "count", char_count);
            }
        }

        if (string_klass->find_field("hash").has_value())
        {
            vmhook::set_field<std::int32_t>(string_oop, string_klass, "hash", 0);
        }

        return string_oop;
    }

    // --- Array element access -------------------------------------------------

    /*
        @brief Reads the length of a Java primitive array.
        @param array_oop Decoded pointer to the Java array object (not compressed).
        @return Element count, or 0 if the pointer is invalid.
        @details
        HotSpot Array object layout (x64, compressed OOPs):
          +0  mark word (8 B)
          +8  klass pointer (4 B)
          +12 _length   (int)
          +16 _data[0]
    */
    inline static auto array_length(void* const array_oop) noexcept
        -> std::int32_t
    {
        if (!array_oop || !vmhook::hotspot::is_valid_pointer(array_oop))
        {
            return 0;
        }

        return *reinterpret_cast<const std::int32_t*>(reinterpret_cast<const std::uint8_t*>(array_oop) + 12);
    }

    /*
        @brief Reads the element at the given index from a Java primitive array.
        @tparam T    The C++ element type (int32_t, double, int8_t, bool, etc.).
        @param array_oop Decoded pointer to the Java array object.
        @param index Zero-based element index.
        @return The element value, or T{} if the pointer/index is invalid.
        @details
        Data starts at offset +16 from the array oop.  Element stride is sizeof(T).
        Bounds checking is performed against the array length.
    */
    template<typename element_type>
    static auto get_array_element(void* const array_oop, const std::int32_t index)
        -> element_type
    {
        static_assert(std::is_trivially_copyable_v<element_type>, "get_array_element<element_type>: element_type must be trivially copyable.");
        if (!array_oop || !vmhook::hotspot::is_valid_pointer(array_oop))
        {
            return element_type{};
        }
        const std::int32_t length{ array_length(array_oop) };
        if (index < 0 || index >= length)
        {
            return element_type{};
        }

        element_type result{};
        std::memcpy(&result, reinterpret_cast<const std::uint8_t*>(array_oop) + 16 + index * static_cast<std::int32_t>(sizeof(element_type)), sizeof(element_type));
        return result;
    }

    /*
        @brief Writes a value into a Java primitive array at the given index.
        @tparam T    The C++ element type.
        @param array_oop Decoded pointer to the Java array object.
        @param index Zero-based element index.
        @param value The value to write.
    */
    template<typename element_type>
    static auto set_array_element(void* const array_oop, const std::int32_t index, const element_type value)
        -> void
    {
        static_assert(std::is_trivially_copyable_v<element_type>, "set_array_element<element_type>: element_type must be trivially copyable.");
        if (!array_oop || !vmhook::hotspot::is_valid_pointer(array_oop))
        {
            return;
        }
        const std::int32_t length{ array_length(array_oop) };
        if (index < 0 || index >= length)
        {
            return;
        }
        std::memcpy(reinterpret_cast<std::uint8_t*>(array_oop) + 16 + index * static_cast<std::int32_t>(sizeof(element_type)), &value, sizeof(element_type));
    }

    // --- Field proxy ----------------------------------------------------------

        /*
            @brief Lightweight proxy to a single Java field value in memory.
            @details
            Returned by vmhook::object::get_field().  Reads the field value on demand,
            dispatches the correct C++ type from the JVM type descriptor (signature),
            and returns a typed copy - not a raw-pointer alias.

            Usage inside a wrapper class:

                // No trailing return type needed - auto deduces field_proxy::value_t,
                // which converts implicitly to the target type at the call site.
                 auto is_connected() -> bool { return get_field("isConnected")->get(); }
                 auto get_health()   -> int { return get_field("health")->get(); }

                 // If you want a concrete type inside the method, cast before returning:
                 auto get_max_hp()   -> int { return static_cast<int>(get_field("maxHp")->get()); }

            Usage at the call site:
                bool ok  = client.is_connected();   // operator bool()  fires
                int  hp  = client.get_health();     // operator int()   fires
                if  (client.is_connected()) { ... } // contextual bool conversion

            Writing:
                obj.get_field("health")->set(100);  // T = int, deduced
                obj.get_field("flag"  )->set(true); // T = bool, deduced
        */
    class field_proxy final
    {
    public:
        /*
            @brief A typed copy of the field's value.
            @details
            Holds one alternative from a variant whose alternatives cover every JVM
            primitive type and compressed-OOP references.  get() selects the
            alternative that matches the field's JVM type descriptor before returning,
            so the value is already correctly cast.

            Implicitly converts to any type T via std::visit + static_cast, which
            means you can write:

                bool b  = proxy->get();   // contextual / assignment conversion
                int  i  = proxy->get();
                float f = proxy->get();
                auto  v = proxy->get();   // type is field_proxy::value_t; converts lazily

            For reference-type fields (signature starts with 'L' or '[') the stored
            alternative is uint32_t (the raw compressed OOP).  Pass it to
            vmhook::hotspot::decode_oop_pointer() to recover the real 64-bit address.
        */
        struct value_t
        {
            std::variant<
                bool,
                std::int8_t,
                std::int16_t,
                std::int32_t,
                std::int64_t,
                float,
                double,
                std::uint16_t,
                std::uint32_t   // reference / array (compressed OOP)
            > data;
            std::string signature{};

            /*
                @brief Appends one boolean element from a Java boolean[] to result.
                @details
                Reads the element as uint8_t and converts non-zero to true.
                Complexity: O(1). Exception safety: noexcept.
            */
            static auto append_array_value(std::vector<bool>& result, void* const array_oop, const std::int32_t index, std::string_view) noexcept
                -> void
            {
                result.push_back(vmhook::get_array_element<std::uint8_t>(array_oop, index) != 0);
            }

            /*
                @brief Appends one Java String element from a String[] to result.
                @details
                Reads the compressed OOP at index, decodes it, then calls read_java_string().
                Complexity: O(S) where S = string length. Exception safety: noexcept.
            */
            static auto append_array_value(std::vector<std::string>& result, void* const array_oop, const std::int32_t index, std::string_view) noexcept
                -> void
            {
                const std::uint32_t element_compressed{ vmhook::get_array_element<std::uint32_t>(array_oop, index) };
                result.push_back(vmhook::read_java_string(vmhook::hotspot::decode_oop_pointer(element_compressed)));
            }

            /*
                @brief Appends one element from a Java char[] or byte[] to a std::vector<char>.
                @details
                For "[C" arrays reads a uint16 and narrows to char; for all other arrays
                reads directly as char.
                Complexity: O(1). Exception safety: noexcept.
            */
            static auto append_array_value(std::vector<char>& result, void* const array_oop, const std::int32_t index, const std::string_view signature) noexcept
                -> void
            {
                if (signature == "[C")
                {
                    result.push_back(static_cast<char>(vmhook::get_array_element<std::uint16_t>(array_oop, index)));
                }
                else
                {
                    result.push_back(vmhook::get_array_element<char>(array_oop, index));
                }
            }

            /*
                @brief Generic overload: appends one element_type element from a Java array to result.
                @details
                Used for numeric types (int, float, double, etc.) and any other trivially
                copyable element_type.
                Complexity: O(1). Exception safety: noexcept.
            */
            template<typename element_type>
            static auto append_array_value(std::vector<element_type>& result, void* const array_oop, const std::int32_t index, std::string_view) noexcept
                -> void
            {
                result.push_back(vmhook::get_array_element<element_type>(array_oop, index));
            }

            /*
                @brief Decodes a compressed array OOP and reads all elements into a std::vector<target_type>.
                @details
                Decodes compressed (the compressed array OOP) via decode_array_oop(), reads
                the Java array length, reserves the result vector, then calls append_array_value()
                for each element.  The correct append_array_value overload is selected based on
                target_type's element type at compile time.

                Complexity: O(N) where N = array length.
                Exception safety: noexcept — returns an empty vector on any failure.

                @param compressed  Compressed OOP of the Java array.
                @param signature   JVM array type descriptor (e.g. "[I", "[Ljava/lang/String;").
                @return  Vector containing all decoded elements, or an empty vector on failure.
            */
            template<typename target_type>
            static auto read_array_value(const std::uint32_t compressed, const std::string_view signature) noexcept
                -> target_type
            {
                target_type result{};
                void* const array_oop{ vmhook::decode_array_oop(compressed) };
                if (!array_oop)
                {
                    return result;
                }

                const std::int32_t length{ vmhook::array_length(array_oop) };
                if (length <= 0)
                {
                    return result;
                }

                result.reserve(static_cast<std::size_t>(length));
                for (std::int32_t index{ 0 }; index < length; ++index)
                {
                    append_array_value(result, array_oop, index, signature);
                }

                return result;
            }

            /*
                @brief Converts a variant alternative source_type to target_type with semantic dispatch.
                @details
                Called by operator target_type() via std::visit.  Applies the correct
                conversion strategy based on the target:
                  - std::string          <- decodes compressed OOP via read_java_string()
                  - std::vector<T>       <- decodes array OOP and reads all elements
                  - std::unique_ptr<T>   <- decodes compressed OOP and constructs wrapper
                  - void*                <- decodes compressed OOP to raw pointer
                  - numeric / bool types <- static_cast from the variant alternative

                Complexity: O(1) for scalars; O(N) for strings/arrays.
                Exception safety: noexcept — returns a default-constructed target_type on failure.

                @tparam target_type  Desired output type.
                @tparam source_type  The variant alternative actually stored.
                @param value         The stored alternative value.
                @return              Converted value, or default-constructed target_type on failure.
            */
            template<typename target_type, typename source_type>
            auto cast_for_variant(source_type value) const noexcept
                -> target_type
            {
                using clean_target_type = std::remove_cvref_t<target_type>;
                using clean_source_type = std::remove_cvref_t<source_type>;

                if constexpr (std::is_same_v<clean_target_type, std::string>)
                {
                    if constexpr (std::is_same_v<clean_source_type, std::uint32_t>)
                    {
                        return vmhook::read_java_string(vmhook::hotspot::decode_oop_pointer(value));
                    }
                    else
                    {
                        return {};
                    }
                }
                else if constexpr (vmhook::detail::is_vector_v<clean_target_type>)
                {
                    if constexpr (std::is_same_v<clean_source_type, std::uint32_t>)
                    {
                        return read_array_value<clean_target_type>(value, this->signature);
                    }
                    else
                    {
                        return {};
                    }
                }
                else if constexpr (vmhook::detail::is_unique_ptr_v<clean_target_type>)
                {
                    if constexpr (std::is_same_v<clean_source_type, std::uint32_t>)
                    {
                        // FLAW B fix: a unique_ptr<T> target reads a SINGLE object
                        // reference field ('L...;').  If the field is actually an
                        // ARRAY ('[...;'), decoding its compressed OOP as a single
                        // wrapper points the wrapper at the ARRAY oop, not an
                        // element — and any field read through that wrapper is UB.
                        // Reject non-'L' signatures (arrays / primitives) so the
                        // mis-typed read yields nullptr instead of a wild wrapper.
                        // (Use to_vector<T>() to read a '[L' field as elements.)
                        if (this->signature.empty() || this->signature.front() != 'L')
                        {
                            return nullptr;
                        }
                        using wrapper_type = typename clean_target_type::element_type;
                        void* const decoded{ vmhook::hotspot::decode_oop_pointer(value) };
                        if (!decoded || !vmhook::hotspot::is_valid_pointer(decoded))
                        {
                            return nullptr;
                        }
                        return clean_target_type{ new wrapper_type{ decoded } };
                    }
                    else
                    {
                        return nullptr;
                    }
                }
                else if constexpr (std::is_same_v<target_type, void*>)
                {
                    // For void*, only convert from uint32_t (compressed OOP)
                    if constexpr (std::is_same_v<clean_source_type, std::uint32_t>)
                    {
                        return vmhook::hotspot::decode_oop_pointer(value);
                    }
                    else
                    {
                        return static_cast<target_type>(nullptr);
                    }
                }
                else if constexpr (requires { static_cast<target_type>(value); })
                {
                    return static_cast<target_type>(value);
                }
                else
                {
                    return target_type{};
                }
            }

            /*
                @brief Implicit conversion operator from value_t to any supported target type.
                @details
                Delegates to std::visit + cast_for_variant<target_type> so any assignment
                or contextual conversion triggers the correct semantic:
                  bool b  = proxy->get();
                  int  i  = proxy->get();
                  auto s  = static_cast<std::string>(proxy->get());

                Complexity: O(1) for scalars; O(N) for strings/arrays.
                Exception safety: noexcept.

                @tparam target_type  The type the value is being converted to.
            */
            template<typename target_type>
            operator target_type() const noexcept
            {
                return std::visit([this](auto value) noexcept
                    -> target_type
                    {
                        return this->cast_for_variant<target_type>(value);
                    }, data);
            }

            /*
                @brief The field value as a std::string, unambiguously.
                @details
                Parity with method_proxy::value_t::as_string().  Prefer this over
                `std::string s{ proxy->get() }` or a static_cast: the templated
                conversion operator can ALSO yield `const char*` (which std::string
                constructs from), so the brace-init / cast forms are ambiguous on
                MSVC — and on the compilers that do resolve them, can silently pick
                the const char* path and build a std::string from a null pointer for
                a non-string field.  as_string() names the extraction directly: a
                reference/String field (compressed OOP, the uint32 alternative) is
                decoded through read_java_string(); every numeric / boolean
                alternative — including a null proxy's int32 zero — yields "".
            */
            auto as_string() const noexcept -> std::string
            {
                return std::visit([](const auto& v) noexcept
                    -> std::string
                    {
                        using stored_type = std::remove_cvref_t<decltype(v)>;
                        if constexpr (std::is_same_v<stored_type, std::uint32_t>)
                        {
                            return vmhook::read_java_string(vmhook::hotspot::decode_oop_pointer(v));
                        }
                        else
                        {
                            return std::string{};
                        }
                    }, data);
            }

            /*
                @brief True when this value holds a reference/String field (compressed OOP).
                Parity with method_proxy::value_t::is_string()/is_void() introspection.
            */
            auto is_reference() const noexcept -> bool
            {
                return std::holds_alternative<std::uint32_t>(data);
            }

            /*
                @brief Converts a reference-type field into a vector of unique_ptr wrappers.
                @details
                Interprets the stored compressed OOP as a Java object array, decodes each
                element, and constructs a std::unique_ptr<element_type> wrapper for each.
                Defined out-of-line after array helpers are available.

                Complexity: O(N) where N = array length.
            */
            template<typename element_type>
            auto to_vector() const
                -> std::vector<std::unique_ptr<element_type>>;

            /*
                @brief Converts a reference-type field into a vector of key/value pairs.
                @details
                Interprets the stored compressed OOP as a java.util.Map, decodes it via
                vmhook::map, and walks its entries.  Each entry is returned as a pair of
                std::unique_ptr<key_type> / std::unique_ptr<value_type> wrappers.
                Defined out-of-line after vmhook::map is complete.

                Complexity: O(N) where N = map size.
            */
            template<typename key_type, typename value_type>
            auto to_entries() const
                -> std::vector<std::pair<std::unique_ptr<key_type>, std::unique_ptr<value_type>>>;
        };

        /*
            @param field_pointer Direct pointer to the field's bytes in JVM memory
                                 (decoded object address + offset for instance fields;
                                  java.lang.Class mirror + offset for static fields).
            @param signature     JVM type descriptor, e.g. "I", "Z", "Ljava/lang/String;"
            @param is_static     true when JVM_ACC_STATIC is set on the field.
        */
        field_proxy(void* field_pointer, std::string sig, const bool is_static_flag) noexcept
            : field_pointer{ field_pointer }
            , signature_text{ std::move(sig) }
            , static_field{ is_static_flag }
        {
        }

        /*
            @brief Reads the field and returns a typed copy.
            @details
            Dispatches on the JVM type descriptor to determine how many bytes to
            read and which variant alternative to populate:
              "Z"  bool       "B"  int8_t    "S"  int16_t   "I"  int32_t
              "J"  int64_t    "F"  float     "D"  double    "C"  uint16_t
              "L"/"["  uint32_t (compressed OOP)
            The returned value is a copy - safe to store and return from methods.
        */
        auto get() const noexcept
            -> value_t
        {
            if (!this->field_pointer)
            {
                return value_t{ std::int32_t{}, this->signature_text };
            }

            if (this->signature_text == "Z")
            {
                bool value{};
                std::memcpy(&value, this->field_pointer, sizeof(value));
                return value_t{ value, this->signature_text };
            }
            if (this->signature_text == "B")
            {
                std::int8_t value{};
                std::memcpy(&value, this->field_pointer, sizeof(value));
                return value_t{ value, this->signature_text };
            }
            if (this->signature_text == "S")
            {
                std::int16_t value{};
                std::memcpy(&value, this->field_pointer, sizeof(value));
                return value_t{ value, this->signature_text };
            }
            if (this->signature_text == "I")
            {
                std::int32_t value{};
                std::memcpy(&value, this->field_pointer, sizeof(value));
                return value_t{ value, this->signature_text };
            }
            if (this->signature_text == "J")
            {
                std::int64_t value{};
                std::memcpy(&value, this->field_pointer, sizeof(value));
                return value_t{ value, this->signature_text };
            }
            if (this->signature_text == "F")
            {
                float value{};
                std::memcpy(&value, this->field_pointer, sizeof(value));
                return value_t{ value, this->signature_text };
            }
            if (this->signature_text == "D")
            {
                double value{};
                std::memcpy(&value, this->field_pointer, sizeof(value));
                return value_t{ value, this->signature_text };
            }
            if (this->signature_text == "C")
            {
                std::uint16_t value{};
                std::memcpy(&value, this->field_pointer, sizeof(value));
                return value_t{ value, this->signature_text };
            }

            // Reference or array type - store compressed OOP
            std::uint32_t value{};
            std::memcpy(&value, this->field_pointer, sizeof(value));
            return value_t{ value, this->signature_text };
        }

        /*
            @brief Writes value into the field's storage.
            @details
            This is the only field setter exposed by field_proxy. It accepts JVM
            primitives, std::string for java.lang.String, and std::vector<T> for
            Java arrays. String and array writes update the existing Java object
            in place because this zero-JNI layer cannot resize Java heap objects.
        */
        template<typename value_type>
        auto set(const value_type& value) const noexcept
            -> void
        {
            using clean_value_type = std::remove_cvref_t<value_type>;

            // Reject string / vector / unique_ptr writes when the field is a primitive.
            // Without this guard, `set(std::string{"42"})` on an "I" field would walk
            // through set_str_field -> field_oop -> decode_array_oop, reinterpreting
            // the int's bytes as a compressed OOP and writing into wherever that
            // decoded address points.  The trivially-copyable branch below has its
            // own size guard; this is the symmetric one for the non-primitive arms.
            //
            // Helper returns 0 for L / [ / V / unknown / empty signatures (i.e.
            // reference / array / void), so this fires only for genuine primitive
            // descriptors (Z B C S I J F D).
            if constexpr (std::is_same_v<clean_value_type, std::string>
                       || (std::is_convertible_v<value_type, std::string_view> && !std::is_same_v<clean_value_type, std::string>)
                       || vmhook::detail::is_vector_v<clean_value_type>
                       || vmhook::detail::is_unique_ptr_v<clean_value_type>)
            {
                if (vmhook::detail::jvm_primitive_byte_width(this->signature_text) != 0)
                {
                    VMHOOK_LOG("{} field_proxy::set: refusing to write a non-primitive "
                               "C++ value (string / vector / unique_ptr) into a primitive "
                               "JVM field (sig='{}') at {:p} ({} field) - the legacy code "
                               "path would have reinterpreted the field's bytes as a "
                               "compressed OOP and written to a wild heap address.  Pass "
                               "the matching primitive type instead.",
                               vmhook::error_tag, this->signature_text,
                               this->field_pointer,
                               this->static_field ? "static" : "instance");
                    return;
                }
            }

            if constexpr (std::is_same_v<clean_value_type, std::string>)
            {
                vmhook::set_str_field(*this, value);
            }
            else if constexpr (std::is_convertible_v<value_type, std::string_view> && !std::is_same_v<clean_value_type, std::string>)
            {
                vmhook::set_str_field(*this, std::string_view{ value });
            }
            else if constexpr (vmhook::detail::is_vector_v<clean_value_type>)
            {
                if constexpr (std::is_same_v<clean_value_type, std::vector<bool>>)
                {
                    vmhook::set_bool_array(*this, value);
                }
                else if constexpr (std::is_same_v<clean_value_type, std::vector<std::string>>)
                {
                    vmhook::set_str_array(*this, value);
                }
                else
                {
                    vmhook::set_prim_array(*this, value);
                }
            }
            else if constexpr (vmhook::detail::is_unique_ptr_v<clean_value_type>)
            {
                if (this->field_pointer)
                {
                    // Explicit base-class qualification reaches object_base::get_instance()
                    // even when the derived wrapper shadows the name with a same-named
                    // static helper (e.g. example_class::get_instance() returning a
                    // unique_ptr).  Resolved at instantiation; never requires object_base
                    // to be complete at template-definition time.
                    void* oop_pointer{ nullptr };
                    if (value)
                    {
                        oop_pointer = value->vmhook::object_base::get_instance();
                    }
                    const std::uint32_t compressed{ oop_pointer
                        ? vmhook::hotspot::encode_oop_pointer(oop_pointer)
                        : std::uint32_t{ 0 } };
                    std::memcpy(this->field_pointer, &compressed, sizeof(compressed));
                }
            }
            else if constexpr (std::is_trivially_copyable_v<clean_value_type>)
            {
                if (!this->field_pointer)
                {
                    return;
                }

                // Special case: passing a 1-byte C++ `char` to a Java `char` field
                // (descriptor "C", 2 bytes wide).  We widen via uint16 so the
                // field's full 2 bytes are written rather than only the low byte.
                if (this->signature_text == "C" && sizeof(clean_value_type) == sizeof(char))
                {
                    const std::uint16_t wide_value{ static_cast<std::uint16_t>(static_cast<unsigned char>(value)) };
                    std::memcpy(this->field_pointer, &wide_value, sizeof(wide_value));
                    return;
                }

                // Size mismatch guard.  Previously the write was an unconditional
                // memcpy of sizeof(value_type) bytes into the field's storage,
                // which silently clobbered adjacent fields when the C++ type was
                // larger than the JVM type (e.g. set(int64_t{...}) on an "I"
                // field wrote 8 bytes into a 4-byte slot, corrupting whatever
                // came after it in the object layout).  Reject the write with a
                // diagnostic instead - users who hit this had a type bug, and
                // failing loudly is far better than overwriting the next field.
                //
                // The check is skipped for reference / array signatures (the
                // helper returns 0), where the L/[ branches above handle the
                // compressed-OOP / array-set paths and never reach this branch.
                const std::size_t value_size{ sizeof(clean_value_type) };
                const std::size_t field_size{
                    vmhook::detail::jvm_primitive_byte_width(this->signature_text) };
                if (field_size != 0 && value_size != field_size)
                {
                    VMHOOK_LOG("{} field_proxy::set: size mismatch at {:p} ({} field, "
                               "value={}B, field={}B, sig='{}') - refusing the write to "
                               "avoid clobbering adjacent fields.  Convert the value to "
                               "the matching primitive type.",
                               vmhook::error_tag, this->field_pointer,
                               this->static_field ? "static" : "instance",
                               value_size, field_size, this->signature_text);
                    return;
                }

                std::memcpy(this->field_pointer, &value, value_size);
            }
            else
            {
                static_assert(vmhook::detail::dependent_false_v<clean_value_type>,
                              "field_proxy::set: unsupported value type.  Previously, types that "
                              "weren't string / string_view / const char* / std::vector / "
                              "unique_ptr<wrapper> / trivially_copyable silently no-op'd - the "
                              "set call returned and the field was unchanged with no compile-time "
                              "error or runtime warning.  Convert the value to one of those, or "
                              "extend the if-constexpr chain above with a specific branch.");
            }
        }

        /*
            @brief Returns the JVM type descriptor of this field (e.g. "I", "Z", "J").
        */
        auto signature() const noexcept
            -> std::string_view
        {
            return this->signature_text;
        }

        /*
            @brief Returns the raw memory address backing this field.
            @details
            For instance fields this is `decoded_object + field_offset`; for
            static fields it is `java.lang.Class mirror + field_offset`.
            Exposed so that watch_static_field can install a hardware
            breakpoint on the slot; most code should prefer get()/set().
        */
        auto raw_address() const noexcept -> void*
        {
            return this->field_pointer;
        }

        /*
            @brief Returns true if this field has JVM_ACC_STATIC set.
            @details
            Static fields are read from / written to the java.lang.Class mirror
            of the declaring class rather than from an object instance.

            Complexity: O(1).
            Exception safety: noexcept.
        */
        auto is_static() const noexcept
            -> bool
        {
            return this->static_field;
        }

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
        auto is_reference() const noexcept
            -> bool
        {
            if (this->signature_text.empty())
            {
                return false;
            }
            const char first{ this->signature_text.front() };
            return first == 'L' || first == '[';
        }

        /*
            @brief Returns the compressed OOP stored in this field (for reference/array types).
            @return The compressed OOP as uint32_t, or 0 if field_pointer is null.
        */
        auto get_compressed_oop() const noexcept
            -> std::uint32_t
        {
            // FLAW C fix: only reference/array fields hold a compressed OOP.  On a
            // PRIMITIVE field, memcpy'ing the first 4 bytes returns the primitive's
            // raw value bytes as a bogus "OOP" that later decodes to a wild pointer
            // (the docstring above warns of exactly this).  Guard on is_reference()
            // so a misused proxy yields null, not garbage.  Valid callers only ask
            // for the compressed OOP of reference/array fields, so this is a no-op
            // for them.
            if (!this->field_pointer || !this->is_reference())
            {
                return 0;
            }
            std::uint32_t value{};
            std::memcpy(&value, this->field_pointer, sizeof(value));
            return value;
        }

    private:
        void* field_pointer;
        std::string signature_text;
        bool        static_field;
    };

    // --- detail helpers that depend on hotspot types -------------------------
    // (reopened here because find_call_stub_entry references hotspot types
    //  that are not yet defined at the earlier detail block position)
    namespace detail
    {
        /*
            @brief Locates StubRoutines::_call_stub_entry via VMStructs.

            HotSpot's official C++-to-Java call gate. Creates a properly-typed
            "entry frame" so the GC, exception handler, and frame-walker all
            recognise the native-to-Java boundary. Unlike jumping directly to a
            c2i adapter, this prevents JVM fatal errors when the frame-walker
            encounters an unexpected native frame above the interpreter frame.

            Stub signature (Windows x64):
              rcx  = link (1 sentinel)
              rdx  = intptr_t* result holder
              r8   = BasicType (T_INT=10, T_VOID=14, )
              r9   = Method*
              stk  = entry_point, parameters*, param_count, JavaThread*
        */
        inline auto find_call_stub_entry() noexcept
            -> void*
        {
            static const vmhook::hotspot::vm_struct_entry_t* const entry{
                vmhook::hotspot::iterate_struct_entries("StubRoutines", "_call_stub_entry") };
            if (!entry || !entry->address)
            {
                return nullptr;
            }

            void* const stub{ *reinterpret_cast<void**>(entry->address) };
            return vmhook::hotspot::is_valid_pointer(stub) ? stub : nullptr;
        }

        /*
            @brief Maps a JVM type-descriptor character to a HotSpot BasicType int.
            Values are stable across all supported JDK versions.
        */
        inline auto sig_char_to_basic_type(const char c) noexcept -> int
        {
            switch (c)
            {
            case 'Z': return 4;   // T_BOOLEAN
            case 'C': return 5;   // T_CHAR
            case 'F': return 6;   // T_FLOAT
            case 'D': return 7;   // T_DOUBLE
            case 'B': return 8;   // T_BYTE
            case 'S': return 9;   // T_SHORT
            case 'I': return 10;  // T_INT
            case 'J': return 11;  // T_LONG
            case 'L': return 12;  // T_OBJECT
            case '[': return 13;  // T_ARRAY
            case 'V': return 14;  // T_VOID
            default:  return 12;  // T_OBJECT (fallback)
            }
        }

        /*
            @brief Returns the in-heap byte width of a JVM primitive field given its
                   type-descriptor signature.
            @details
            Used by field_proxy::set() to size-check the user's value against the
            actual field width before memcpy'ing.  Returns 0 for reference and
            array types (where the in-heap width depends on UseCompressedOops and
            is handled by the wrapper / set_str_field paths), and for any unknown
            signature - in both cases the caller skips the size validation.

            Width table (JVM spec § 4.3.2):
              Z (boolean) = 1, B (byte) = 1
              S (short)   = 2, C (char) = 2
              I (int)     = 4, F (float) = 4
              J (long)    = 8, D (double) = 8
        */
        inline auto jvm_primitive_byte_width(const std::string_view signature) noexcept
            -> std::size_t
        {
            if (signature.size() != 1)
            {
                return 0;
            }
            switch (signature[0])
            {
            case 'Z': case 'B': return 1;
            case 'S': case 'C': return 2;
            case 'I': case 'F': return 4;
            case 'J': case 'D': return 8;
            default:            return 0;
            }
        }
    }

    // --- Method proxy ------------------------------------------------------

    /*
        @brief Lightweight proxy to a Java method, supporting typed argument calls.
        @details
        Returned by vmhook::object::get_method().  Handles argument conversion
        and dispatches to the JVM method via the interpreter entry point.

        Usage inside a wrapper class:
             auto say_hi() -> void { get_method("sayHi")->call(); }

             auto say_string(const std::string& value) -> void {
                get_method("sayString")->call(value);
            }

        Returns a value_t that implicitly converts to the expected C++ type.
    */
    class method_proxy final
    {
    public:
        /*
            @brief Return type from method_proxy::call().
            @details
            Holds the result value as a variant, supporting implicit conversion
            to common C++ types via the conversion operator.
        */
        struct value_t
        {
            std::variant<
                std::monostate,
                bool,
                std::int8_t,
                std::int16_t,
                std::int32_t,
                std::int64_t,
                float,
                double,
                std::uint16_t,
                std::uint32_t,   // reference / array (compressed OOP)
                std::string      // for String objects
            > data;

            /*
                @brief Converts the stored value to T via static_cast.
                Falls back to a default-constructed T for variant alternatives
                that cannot be cast to the target type (std::monostate, std::string).

                Special-cases:
                  * `void*` target with a uint32_t stored value (the variant
                    alternative used for "reference / array (compressed OOP)")
                    is routed through vmhook::hotspot::decode_oop_pointer so
                    the caller gets the FULL 64-bit decoded heap pointer.
                    Without this, the default static_cast<void*>(uint32_t)
                    would silently produce a truncated 4-byte address - on
                    every JVM with a non-zero narrow_oop_base, that's not
                    even a heap address and dereferences immediately.
            */
            template<typename target_type>
            operator target_type() const noexcept
            {
                return std::visit([](const auto& v) noexcept
                    -> target_type
                    {
                        using stored_type = std::remove_cvref_t<decltype(v)>;

                        // unique_ptr<wrapper> <- compressed-OOP reference return.
                        // Mirrors field_proxy::value_t::cast_for_variant so an
                        // Object-returning Java method can be assigned straight
                        // into a std::unique_ptr<my_wrapper> instead of silently
                        // yielding null.  The uint32_t alternative is the
                        // compressed OOP (the call_stub / call_jni reference paths
                        // store encode_oop_pointer(result)); decode + validate +
                        // wrap.
                        if constexpr (vmhook::detail::is_unique_ptr_v<target_type>)
                        {
                            using wrapper_type = typename vmhook::detail::is_unique_ptr<target_type>::value_type_t;
                            static_assert(std::is_base_of_v<vmhook::object_base, wrapper_type>,
                                          "method_proxy::value_t -> std::unique_ptr<T>: T must derive "
                                          "from vmhook::object_base.  Return-by-wrapper only works for "
                                          "registered vmhook wrapper types.");
                            if constexpr (std::is_same_v<stored_type, std::uint32_t>)
                            {
                                void* const decoded{ vmhook::hotspot::decode_oop_pointer(v) };
                                if (!decoded || !vmhook::hotspot::is_valid_pointer(decoded))
                                {
                                    return target_type{};
                                }
                                return target_type{ new wrapper_type{ decoded } };
                            }
                            else
                            {
                                return target_type{};
                            }
                        }
                        // std::string <- either the eagerly-decoded std::string
                        // alternative (call_jni / call_stub String path) or a
                        // compressed OOP pointing at a java.lang.String.
                        else if constexpr (std::is_same_v<target_type, std::string>)
                        {
                            if constexpr (std::is_same_v<stored_type, std::string>)
                            {
                                return v;
                            }
                            else if constexpr (std::is_same_v<stored_type, std::uint32_t>)
                            {
                                return vmhook::read_java_string(vmhook::hotspot::decode_oop_pointer(v));
                            }
                            else
                            {
                                return target_type{};
                            }
                        }
                        // Compressed-OOP -> raw heap pointer conversion.
                        else if constexpr (std::is_same_v<target_type, void*>
                                        && std::is_same_v<stored_type, std::uint32_t>)
                        {
                            return vmhook::hotspot::decode_oop_pointer(v);
                        }
                        else if constexpr (requires { static_cast<target_type>(v); })
                        {
                            return static_cast<target_type>(v);
                        }
                        else
                        {
                            return target_type{};
                        }
                    }
                , data);
            }

            /*
                @brief True when the call returned void or failed (variant holds monostate).
                Distinguishes "method returned void / the call failed" from a
                primitive zero or an empty string.  Parity with the richer
                introspection field_proxy::value_t users have asked for.
            */
            auto is_void() const noexcept -> bool
            {
                return std::holds_alternative<std::monostate>(data);
            }

            /*
                @brief True when the call produced a java.lang.String (decoded eagerly).
            */
            auto is_string() const noexcept -> bool
            {
                return std::holds_alternative<std::string>(data);
            }

            /*
                @brief The result as a std::string, unambiguously.
                @details
                Prefer this over `std::string s = call()` / `static_cast<std::string>`:
                the templated conversion operator can also yield `const char*`
                (which std::string constructs from), so the implicit/cast forms are
                ambiguous on MSVC.  as_string() names the extraction directly:
                returns the eagerly-decoded String alternative as-is, decodes a
                compressed-OOP alternative through read_java_string, and returns ""
                for any other (numeric / monostate) alternative.
            */
            auto as_string() const noexcept -> std::string
            {
                return std::visit([](const auto& v) noexcept
                    -> std::string
                    {
                        using stored_type = std::remove_cvref_t<decltype(v)>;
                        if constexpr (std::is_same_v<stored_type, std::string>)
                        {
                            return v;
                        }
                        else if constexpr (std::is_same_v<stored_type, std::uint32_t>)
                        {
                            return vmhook::read_java_string(vmhook::hotspot::decode_oop_pointer(v));
                        }
                        else
                        {
                            return std::string{};
                        }
                    }
                , data);
            }
        };

        /*
            @param owning_object The object whose method will be called (may be null for static).
            @param method_pointer Pointer to the HotSpot Method object.
            @param signature     JVM method descriptor, e.g. "(I)Ljava/lang/String;"
        */
        method_proxy(void* owning_object, vmhook::hotspot::method* method_ptr, std::string sig,
                     const bool pinned = false) noexcept
            : object{ owning_object }
            , method{ method_ptr }
            , signature_text{ std::move(sig) }
            , static_field{ false }
            , signature_pinned{ pinned }
        {
        }

        /*
            @brief Invokes the Java method via JNI as a fallback when the call stub is unavailable.
            @details
            Builds a jni_value argument array from args, resolves the jmethodID via
            JNIEnv::GetMethodID, then dispatches to either CallObjectMethodA (for methods
            returning java.lang.String) or CallVoidMethodA (for all other return types).
            Used when detail::find_call_stub_entry() returns nullptr (call stub not yet
            resolved or JDK version incompatibility).

            Complexity: O(method lookup + method execution time).
            Exception safety: noexcept — returns monostate value_t on any failure.

            @param args  C++ arguments to pass; converted via make_jni_args().
            @return  value_t containing the result, or monostate for void/failure.
        */
        template<typename... args_t>
        auto call_jni(args_t&&... args) const noexcept
            -> value_t
        {
            auto* const env_void{ vmhook::hotspot::current_jni_env };
            if (!env_void)
            {
                VMHOOK_LOG("{} method_proxy::call_jni('{}{}'): current_jni_env is null - "
                           "have we attached the calling thread?",
                           vmhook::error_tag, this->name(), this->signature_text);
                return value_t{ std::monostate{} };
            }

            // Pick the overload whose JVM descriptor actually matches the
            // C++ arg types.  get_method("name") returns the FIRST method
            // matched by name only; on OBF builds many methods share a
            // single-letter name (e.g. on Minecraft 1.8.9 vanilla
            // EntityPlayerSP both addChatMessage(IChatComponent) and
            // attackEntityFrom(DamageSource, float) are "a"), so we have
            // to resolve the real overload here or we end up dispatching
            // the wrong method ID via JNI and the chat message never
            // reaches the JVM.  resolve_compatible_method() walks the
            // class hierarchy and returns the Method* whose descriptor
            // matches args_t exactly, falling back to this->method when
            // nothing better is found (preserving old behaviour for
            // unique method names).
            vmhook::hotspot::method* const selected_method{
                this->resolve_compatible_method<std::remove_cvref_t<args_t>...>() };
            const std::string& effective_signature{
                (selected_method && selected_method != this->method)
                    ? (this->cached_effective_signature = selected_method->get_signature(),
                       this->cached_effective_signature)
                    : this->signature_text };

            // NOTE: a fail-safe "refuse dispatch when no overload matches the C++
            // args" guard was tried here but signature_matches_arguments() has a
            // false-NEGATIVE for unique_ptr<T> / object arguments (it does not map
            // a wrapper type to its 'L...;' parameter), so the guard wrongly
            // refused valid object-argument calls.  The static-overload crash is
            // fixed by the resolve_compatible_method() hierarchy walk above (it now
            // covers statics); a proper fail-safe needs signature_matches_arguments
            // to recognise object/unique_ptr args first (tracked separately).

            // Return-type char is parsed once from effective_signature
            // and cached on the proxy.  Skips the rfind() on every
            // call.  The L/[ branch below still needs `rparen` to
            // peek at the full return descriptor for String detection,
            // so compute it lazily there too.
            std::size_t rparen{ std::string::npos };
            char ret_char{ this->cached_ret_char };
            if (!ret_char)
            {
                rparen = effective_signature.rfind(')');
                if (rparen == std::string::npos)
                {
                    VMHOOK_LOG("{} method_proxy::call_jni('{}{}'): malformed signature - no ')' found.",
                               vmhook::error_tag, this->name(), effective_signature);
                    return value_t{ std::monostate{} };
                }
                ret_char = (rparen + 1 < effective_signature.size())
                               ? effective_signature[rparen + 1]
                               : 'V';
                this->cached_ret_char = ret_char;
            }

            const bool is_static_call{ this->object == nullptr };

            // Resolve jclass + jmethodID.  Both are cached on the
            // method_proxy so subsequent calls skip the GetMethodID /
            // FindClass round-trips — they're the dominant cost for
            // tight call loops.
            if (!this->cached_method_id)
            {
                void* class_handle{ nullptr };

                if (is_static_call)
                {
                    // Static path: get the owning klass's name via the
                    // Method's pool_holder, then FindClass to obtain a
                    // jclass.
                    if (!this->method)
                    {
                        VMHOOK_LOG("{} method_proxy::call_jni('{}{}'): static call requested but "
                                   "underlying Method* is null - cannot derive jclass.",
                                   vmhook::error_tag, this->name(), this->signature_text);
                        return value_t{ std::monostate{} };
                    }
                    if (auto* const const_method{ this->method->get_const_method() };
                        const_method && vmhook::hotspot::is_valid_pointer(const_method))
                    {
                        if (auto* const cp{ const_method->get_constants() };
                            cp && vmhook::hotspot::is_valid_pointer(cp))
                        {
                            static const auto* const pool_holder_entry{
                                vmhook::hotspot::iterate_struct_entries("ConstantPool", "_pool_holder") };
                            if (pool_holder_entry)
                            {
                                auto* const holder_klass{ *reinterpret_cast<vmhook::hotspot::klass**>(
                                    reinterpret_cast<std::uint8_t*>(cp) + pool_holder_entry->offset) };
                                if (holder_klass && vmhook::hotspot::is_valid_pointer(holder_klass))
                                {
                                    if (auto* const name_sym{ holder_klass->get_name() })
                                    {
                                        class_handle = vmhook::detail::jni_find_class(name_sym->to_string());
                                    }
                                }
                            }
                        }
                    }
                    if (!class_handle)
                    {
                        VMHOOK_LOG("{} method_proxy::call_jni('{}{}'): FindClass(name from pool_holder) "
                                   "returned null - the declaring class is not reachable via JNI.",
                                   vmhook::error_tag, this->name(), this->signature_text);
                        return value_t{ std::monostate{} };
                    }
                    this->cached_class_handle = class_handle;
                    this->cached_method_id    = vmhook::detail::jni_get_static_method_id(
                        class_handle, this->name(), effective_signature);
                }
                else
                {
                    // Instance path: get_object_class to derive the
                    // jclass from the receiver, then GetMethodID.
                    void* object_storage{};
                    void* const object_handle{ vmhook::detail::jni_oop_handle(this->object, object_storage) };
                    class_handle = vmhook::detail::jni_get_object_class(object_handle);
                    if (!class_handle)
                    {
                        VMHOOK_LOG("{} method_proxy::call_jni('{}{}'): GetObjectClass returned null - "
                                   "receiver OOP is invalid or not yet visible to JNI.",
                                   vmhook::error_tag, this->name(), effective_signature);
                        return value_t{ std::monostate{} };
                    }
                    this->cached_class_handle = class_handle;
                    this->cached_method_id    = vmhook::detail::jni_get_method_id(
                        class_handle, this->name(), effective_signature);
                }

                if (!this->cached_method_id)
                {
                    // Previously: fell back to handing this->method straight
                    // through as the jmethodID.  On JDK 8 that worked because
                    // jmethodIDs WERE Method* pointers; on JDK 9+ they are
                    // tagged slot pointers (`(slot << 1) | 1`) and the JVM
                    // resolves them by `*(Method**)(id & ~1)` - handing it
                    // an untagged Method* dereferences `*ConstMethod**` and
                    // jumps to garbage on the next dispatch.  Quietly
                    // "succeeding" only to corrupt the JVM is strictly worse
                    // than failing the call.  Refuse and let the caller see
                    // the diagnostic.
                    VMHOOK_LOG("{} method_proxy::call_jni('{}{}'): GetMethodID returned null - "
                               "the receiver's class is not reachable via JNI from this thread, "
                               "or the method does not exist on the runtime type.  Ensure the "
                               "context classloader is set (vmhook now does this automatically "
                               "at attach), and that the method name + signature match exactly "
                               "(check overload resolution).  Aborting call.",
                               vmhook::error_tag, this->name(), this->signature_text);
                    vmhook::detail::jni_exception_clear();
                    return value_t{ std::monostate{} };
                }
            }

            // Build the jvalue array on the stack — no heap
            // allocation in the call hot path.  All real Java methods
            // we need to dispatch fit under arity 8.
            constexpr std::size_t arg_cap{ 8 };
            static_assert(sizeof...(args_t) <= arg_cap,
                          "method_proxy::call: max 8 arguments");
            vmhook::detail::jni_value values[arg_cap]{};
            void*                     handle_storage[arg_cap]{};
            bool                      arg_needs_release[arg_cap]{};
            std::size_t               arg_index{ 0 };
            ((vmhook::detail::write_jni_arg_to_slot(values[arg_index],
                                                     handle_storage[arg_index],
                                                     arg_needs_release[arg_index],
                                                     std::forward<args_t>(args)),
              ++arg_index), ...);
            (void)arg_index;

            // RAII cleanup of JNI local references created during arg packing.
            //
            // write_jni_arg_to_slot maps strings to NewStringUTF (returns a
            // JNI local ref) and object args to `&storage[i]` (synthetic
            // stack handle).  Without this cleanup, every string arg leaks
            // one local ref per call - HotSpot's default local-ref table
            // holds 16 entries and our long-lived attached detour threads
            // never see a JNI frame-pop to release them, so a tight call
            // loop with string args eventually trips "local reference
            // table overflow" warnings and starts losing object identity.
            //
            // The per-slot `needs_release[i]` tag — set by write_jni_arg_to_slot
            // ONLY for jstrings produced by NewStringUTF — is the discriminator.
            // We must NOT read `values[i].l` back to decide this: jni_value is a
            // union, so a primitive arg (jlong 0x4242'4242'4242'4242, jint, even
            // jboolean true -> 0x01) aliases `.l` and would be handed to
            // DeleteLocalRef as a garbage pointer.  Synthetic object handles
            // (value.l == &handle_storage[i]) and primitives both leave the tag
            // false; only real string local refs get released.
            struct string_handle_cleanup
            {
                vmhook::detail::jni_value*  values_ptr;
                const bool*                 needs_release_ptr;
                std::size_t                 count;

                ~string_handle_cleanup() noexcept
                {
                    for (std::size_t i{ 0 }; i < this->count; ++i)
                    {
                        if (this->needs_release_ptr[i])
                        {
                            vmhook::detail::jni_delete_local_ref(this->values_ptr[i].l);
                        }
                    }
                }
            } cleanup{ values, arg_needs_release, sizeof...(args_t) };

            void** const table{ *reinterpret_cast<void***>(env_void) };
            void* const  method_id{ this->cached_method_id };

            // The receiver passed to the JNI Call* function is the
            // jclass for static calls and the jobject (indirect
            // handle) for instance calls.  jni_oop_handle is cheap
            // so we recompute the instance handle each call rather
            // than caching across mutations of this->object.
            void* receiver{ this->cached_class_handle };
            void* instance_storage{};
            if (!is_static_call)
            {
                receiver = vmhook::detail::jni_oop_handle(this->object, instance_storage);
            }

            // Diagnostic: dump receiver + args + method id so we can tell at
            // a glance whether we are handing the JVM what we think we are.
            //
            // We deliberately do NOT decode the klass name by reading the OOP
            // header at +8 anymore - those reads faulted during world unload
            // on every host where vmhook's hooked detours fire on objects
            // whose backing storage the JVM had already freed.  The address
            // passed is_valid_pointer's range check but the page was
            // unmapped, so the load at +8 took a SEH access violation that
            // tore the whole JVM down.  Raw addresses are cheap, never
            // fault, and are still enough to correlate calls in the trace
            // (or paste into a debugger to inspect manually).
            {
                std::string args_dump{};
                for (std::size_t i{ 0 }; i < sizeof...(args_t); ++i)
                {
                    if (i > 0) { args_dump += ", "; }
                    // Deref ONLY a synthetic object handle (value.l points at
                    // its own handle_storage slot, which write_jni_arg_to_slot
                    // set for object args).  jni_value is a UNION: for a
                    // primitive arg (e.g. jint 1) value.l aliases the primitive
                    // bits (0x1), which is non-null but is NOT a pointer —
                    // dereferencing it took an access violation that crashed
                    // the JVM on the first primitive-arg call on JDKs without
                    // the call stub (21+).  Same union-aliasing class as the
                    // DeleteLocalRef fix; here we discriminate by the synthetic
                    // handle's self-pointer and dump raw bytes otherwise.
                    if (values[i].l == &handle_storage[i])
                    {
                        void* const arg_oop{ *reinterpret_cast<void**>(values[i].l) };
                        args_dump += std::format("oop=0x{:016X}",
                                                  reinterpret_cast<std::uintptr_t>(arg_oop));
                    }
                    else
                    {
                        args_dump += std::format("raw=0x{:016X}", values[i].j);
                    }
                }
                VMHOOK_LOG("{} method_proxy::call_jni('{}{}'): dispatching {} receiver_oop=0x{:016X} method_id=0x{:016X} args=[{}].",
                           vmhook::info_tag,
                           this->name(),
                           effective_signature,
                           is_static_call ? "static" : "instance",
                           reinterpret_cast<std::uintptr_t>(is_static_call ? nullptr : this->object),
                           reinterpret_cast<std::uintptr_t>(method_id),
                           args_dump);
            }

            // Slot indices for Call(Static)?TypeMethodA in the JNIEnv
            // function table.  Layout is stable across JNI versions:
            //   instance Object=36, Bool=39, Byte=42, Char=45,
            //            Short=48, Int=51, Long=54, Float=57,
            //            Double=60, Void=63
            //   static   Object=116, Bool=119, Byte=122, Char=125,
            //            Short=128, Int=131, Long=134, Float=137,
            //            Double=140, Void=143
            // Helper that runs after every JNI Call*Method dispatch to surface any
            // pending Java exception in our own log line.  Some host environments
            // (notably Forge with a wrapped System.err) swallow ExceptionDescribe's
            // stderr output, so we additionally extract Throwable.toString() and
            // embed it in VMHOOK_LOG.  Without this, exceptions silently poison
            // subsequent JNI operations on the thread and produce "the method
            // seemed to run but nothing happened" behaviour.
            using exception_check_t          = std::uint8_t(*)(void*);
            using exception_occurred_t       = void*(*)(void*);
            using exception_describe_t       = void(*)(void*);
            using exception_clear_t          = void(*)(void*);
            using get_object_class_t         = void*(*)(void*, void*);
            using get_method_id_t            = void*(*)(void*, void*, const char*, const char*);
            using call_object_method_t       = void*(*)(void*, void*, void*);
            using get_string_utf_chars_t     = const char*(*)(void*, void*, std::uint8_t*);
            using release_string_utf_chars_t = void(*)(void*, void*, const char*);
            using delete_local_ref_t         = void(*)(void*, void*);

            auto check_callee_exception = [&]() noexcept -> void
            {
                auto* const exc_check{ reinterpret_cast<exception_check_t>(table[228]) };
                if (!exc_check || exc_check(env_void) == 0u)
                {
                    return;
                }

                auto* const exc_occurred{ reinterpret_cast<exception_occurred_t>(table[15]) };
                void* const throwable{ exc_occurred ? exc_occurred(env_void) : nullptr };

                // ExceptionDescribe prints the stack trace AND clears the pending
                // exception as a side effect.  We call it first so stderr-capturing
                // launchers (Lunar, plain Forge launchers without stderr wrapping)
                // get the full backtrace.
                if (auto* const exc_desc{ reinterpret_cast<exception_describe_t>(table[16]) })
                {
                    exc_desc(env_void);
                }
                else if (auto* const exc_clr{ reinterpret_cast<exception_clear_t>(table[17]) })
                {
                    exc_clr(env_void);
                }

                // Now extract Throwable.toString() into a std::string so the
                // exception's class+message also lands in our VMHOOK_LOG output
                // (which is what users see when stderr is captured/wrapped).
                std::string exception_text;
                void* throwable_class{ nullptr };
                void* result_handle{ nullptr };
                if (throwable)
                {
                    auto* const get_object_class{ reinterpret_cast<get_object_class_t>(table[31]) };
                    auto* const get_method_id{ reinterpret_cast<get_method_id_t>(table[33]) };
                    auto* const call_object_method{ reinterpret_cast<call_object_method_t>(table[36]) };
                    auto* const get_utf{ reinterpret_cast<get_string_utf_chars_t>(table[169]) };
                    auto* const release_utf{ reinterpret_cast<release_string_utf_chars_t>(table[170]) };
                    auto* const exc_clr{ reinterpret_cast<exception_clear_t>(table[17]) };

                    if (get_object_class && get_method_id && call_object_method && get_utf && release_utf)
                    {
                        throwable_class = get_object_class(env_void, throwable);
                        if (throwable_class)
                        {
                            void* const to_string_id{ get_method_id(env_void, throwable_class,
                                                                     "toString", "()Ljava/lang/String;") };
                            if (exc_clr && exc_check(env_void) != 0u) { exc_clr(env_void); }

                            if (to_string_id)
                            {
                                result_handle = call_object_method(env_void, throwable, to_string_id);
                                if (exc_clr && exc_check(env_void) != 0u) { exc_clr(env_void); }

                                if (result_handle)
                                {
                                    const char* const utf{ get_utf(env_void, result_handle, nullptr) };
                                    if (utf)
                                    {
                                        exception_text.assign(utf);
                                        release_utf(env_void, result_handle, utf);
                                    }
                                }
                            }
                        }
                    }
                }

                VMHOOK_LOG("{} method_proxy::call_jni('{}{}'): Java exception thrown by callee: {}",
                           vmhook::error_tag, this->name(), this->signature_text,
                           exception_text.empty() ? "(unable to extract via Throwable.toString)" : exception_text);

                // Best-effort cleanup of local references so they don't accumulate
                // on long-lived attached threads.  Missing DeleteLocalRef just leaks
                // a few jobjects, no correctness issue.
                if (auto* const delete_local_ref{ reinterpret_cast<delete_local_ref_t>(table[23]) })
                {
                    if (throwable)       { delete_local_ref(env_void, throwable); }
                    if (throwable_class) { delete_local_ref(env_void, throwable_class); }
                    if (result_handle)   { delete_local_ref(env_void, result_handle); }
                }
            };

            switch (ret_char)
            {
                case 'V':
                {
                    // JNI spec forbids using Call(Static)?VoidMethodA on a
                    // constructor — the method ID must not be <init> or
                    // <clinit>.  Some JVMs silently no-op the call (leaving
                    // the object only partially constructed), others raise
                    // an internal error.  Dispatch <init> / <clinit>
                    // through CallNonvirtualVoidMethodA (slot 93) with the
                    // method's declaring klass instead; that is the
                    // documented way to invoke a constructor / static
                    // initialiser on an already-allocated receiver via JNI.
                    const std::string method_name{ this->name() };
                    const bool is_init_call{
                        !is_static_call
                        && (method_name == "<init>" || method_name == "<clinit>")
                        && this->cached_class_handle
                    };
                    if (is_init_call)
                    {
                        using nonvirtual_fn_t = void(*)(void*, void*, void*, void*, const vmhook::detail::jni_value*);
                        constexpr std::size_t nonvirtual_slot{ 93u };
                        if (auto* const fn{ reinterpret_cast<nonvirtual_fn_t>(table[nonvirtual_slot]) })
                        {
                            fn(env_void, receiver, this->cached_class_handle, method_id, values);
                            check_callee_exception();
                        }
                        else
                        {
                            VMHOOK_LOG("{} method_proxy::call_jni('{}{}'): JNI CallNonvirtualVoidMethodA slot {} is null.",
                                       vmhook::error_tag, this->name(), this->signature_text, nonvirtual_slot);
                        }
                        return value_t{ std::monostate{} };
                    }

                    using fn_t = void(*)(void*, void*, void*, const vmhook::detail::jni_value*);
                    const std::size_t slot{ is_static_call ? 143u : 63u };
                    if (auto* const fn{ reinterpret_cast<fn_t>(table[slot]) })
                    {
                        fn(env_void, receiver, method_id, values);
                        check_callee_exception();
                    }
                    else
                    {
                        VMHOOK_LOG("{} method_proxy::call_jni('{}{}'): JNI Call(Static)?VoidMethodA slot {} is null.",
                                   vmhook::error_tag, this->name(), this->signature_text, slot);
                    }
                    return value_t{ std::monostate{} };
                }
                case 'Z':
                {
                    using fn_t = std::uint8_t(*)(void*, void*, void*, const vmhook::detail::jni_value*);
                    const std::size_t slot{ is_static_call ? 119u : 39u };
                    auto* const fn{ reinterpret_cast<fn_t>(table[slot]) };
                    if (!fn) { return value_t{ std::monostate{} }; }
                    const std::uint8_t r{ fn(env_void, receiver, method_id, values) };
                    check_callee_exception();
                    return value_t{ r != 0 };
                }
                case 'B':
                {
                    using fn_t = std::int8_t(*)(void*, void*, void*, const vmhook::detail::jni_value*);
                    const std::size_t slot{ is_static_call ? 122u : 42u };
                    auto* const fn{ reinterpret_cast<fn_t>(table[slot]) };
                    if (!fn) { return value_t{ std::monostate{} }; }
                    const std::int8_t r{ fn(env_void, receiver, method_id, values) };
                    check_callee_exception();
                    return value_t{ r };
                }
                case 'C':
                {
                    using fn_t = std::uint16_t(*)(void*, void*, void*, const vmhook::detail::jni_value*);
                    const std::size_t slot{ is_static_call ? 125u : 45u };
                    auto* const fn{ reinterpret_cast<fn_t>(table[slot]) };
                    if (!fn) { return value_t{ std::monostate{} }; }
                    const std::uint16_t r{ fn(env_void, receiver, method_id, values) };
                    check_callee_exception();
                    return value_t{ r };
                }
                case 'S':
                {
                    using fn_t = std::int16_t(*)(void*, void*, void*, const vmhook::detail::jni_value*);
                    const std::size_t slot{ is_static_call ? 128u : 48u };
                    auto* const fn{ reinterpret_cast<fn_t>(table[slot]) };
                    if (!fn) { return value_t{ std::monostate{} }; }
                    const std::int16_t r{ fn(env_void, receiver, method_id, values) };
                    check_callee_exception();
                    return value_t{ r };
                }
                case 'I':
                {
                    using fn_t = std::int32_t(*)(void*, void*, void*, const vmhook::detail::jni_value*);
                    const std::size_t slot{ is_static_call ? 131u : 51u };
                    auto* const fn{ reinterpret_cast<fn_t>(table[slot]) };
                    if (!fn) { return value_t{ std::monostate{} }; }
                    const std::int32_t r{ fn(env_void, receiver, method_id, values) };
                    check_callee_exception();
                    return value_t{ r };
                }
                case 'J':
                {
                    using fn_t = std::int64_t(*)(void*, void*, void*, const vmhook::detail::jni_value*);
                    const std::size_t slot{ is_static_call ? 134u : 54u };
                    auto* const fn{ reinterpret_cast<fn_t>(table[slot]) };
                    if (!fn) { return value_t{ std::monostate{} }; }
                    const std::int64_t r{ fn(env_void, receiver, method_id, values) };
                    check_callee_exception();
                    return value_t{ r };
                }
                case 'F':
                {
                    using fn_t = float(*)(void*, void*, void*, const vmhook::detail::jni_value*);
                    const std::size_t slot{ is_static_call ? 137u : 57u };
                    auto* const fn{ reinterpret_cast<fn_t>(table[slot]) };
                    if (!fn) { return value_t{ std::monostate{} }; }
                    const float r{ fn(env_void, receiver, method_id, values) };
                    check_callee_exception();
                    return value_t{ r };
                }
                case 'D':
                {
                    using fn_t = double(*)(void*, void*, void*, const vmhook::detail::jni_value*);
                    const std::size_t slot{ is_static_call ? 140u : 60u };
                    auto* const fn{ reinterpret_cast<fn_t>(table[slot]) };
                    if (!fn) { return value_t{ std::monostate{} }; }
                    const double r{ fn(env_void, receiver, method_id, values) };
                    check_callee_exception();
                    return value_t{ r };
                }
                case 'L':
                case '[':
                {
                    using fn_t = void*(*)(void*, void*, void*, const vmhook::detail::jni_value*);
                    const std::size_t slot{ is_static_call ? 116u : 36u };
                    auto* const fn{ reinterpret_cast<fn_t>(table[slot]) };
                    if (!fn)
                    {
                        VMHOOK_LOG("{} method_proxy::call_jni('{}{}'): JNI Call(Static)?ObjectMethodA "
                                   "slot {} is null.",
                                   vmhook::error_tag, this->name(), this->signature_text, slot);
                        return value_t{ std::monostate{} };
                    }
                    void* const result_handle{ fn(env_void, receiver, method_id, values) };
                    check_callee_exception();
                    // Convenience: when the method returns
                    // java.lang.String, return a std::string copy.
                    if (rparen == std::string::npos)
                    {
                        rparen = effective_signature.rfind(')');
                    }
                    if (rparen != std::string::npos
                        && std::string_view{ effective_signature }.substr(rparen + 1) == "Ljava/lang/String;")
                    {
                        // Extract the UTF-8 contents into an owned std::string,
                        // then release the JNI local ref - skipping the release
                        // leaked one local per Object-returning call on the
                        // detour thread (16-entry default table fills fast).
                        std::string utf{ vmhook::detail::jni_get_string_utf(result_handle) };
                        vmhook::detail::jni_delete_local_ref(result_handle);
                        return value_t{ std::move(utf) };
                    }
                    // Any other reference type: decode the JNI handle to the
                    // real heap OOP and store it as a COMPRESSED OOP, so
                    // value_t's unique_ptr<wrapper> / void* conversion (which
                    // runs decode_oop_pointer) recovers the actual object.
                    //
                    // The previous code stored the *truncated jobject handle*
                    // as if it were a compressed OOP; decode_oop_pointer then
                    // produced a garbage address — a non-null but invalid
                    // wrapper whose first field read took an access violation
                    // (this crashed the JVM on the call_jni path, i.e. every
                    // object-returning call on JDK 21+ where the call stub is
                    // gone).  A null Java return now becomes monostate so
                    // value_t::is_void() is true and unique_ptr<wrapper> is null.
                    void* const result_oop{ vmhook::detail::jni_decode_object(result_handle) };
                    vmhook::detail::jni_delete_local_ref(result_handle);
                    if (!result_oop)
                    {
                        return value_t{ std::monostate{} };
                    }
                    return value_t{ vmhook::hotspot::encode_oop_pointer(result_oop) };
                }
                default:
                    VMHOOK_LOG("{} method_proxy::call_jni('{}{}'): unhandled return type char '{}' "
                               "(0x{:02X}) - cannot dispatch.",
                               vmhook::error_tag, this->name(), this->signature_text,
                               ret_char, static_cast<std::uint32_t>(ret_char));
                    return value_t{ std::monostate{} };
            }
        }

        /*
            @brief Calls the method with the given arguments.
            @param args Arguments to pass (C++ types: int, std::string, etc.).
            @return value_t containing the result, or empty value_t for void methods.
            @details
            Converts C++ arguments to JVM types and invokes the method through
            the interpreter entry point. Handles both instance and static methods.

            For reference-type arguments (strings, objects), pass std::string for class name
            or use an object wrapper.
        */
        /*
            @brief Invokes the Java method and returns its result.
            @param args  C++ values forwarded as the method's Java arguments.
                         For instance methods the receiver is added automatically
                         from the proxy's stored object pointer.
            @return value_t holding the Java return value, or monostate if the
                    call gate is unavailable.

            @note  Calling Java methods from native C++ requires HotSpot's
                   StubRoutines::_call_stub_entry, which sets up a properly-typed
                   "entry frame" that the GC and frame-walker understand.
                   This address is located via VMStructs at runtime; if the
                   VMStruct entry is absent (removed in some JDK releases), call()
                   falls back to returning monostate without invoking the method.

                   call() must be invoked from inside a vmhook::hook() detour
                   where vmhook::hotspot::current_java_thread is set.
        */
        template<typename... args_t>
        auto call(args_t&&... args) const noexcept
            -> value_t
        {
            if (!this->method || !vmhook::hotspot::is_valid_pointer(this->method))
            {
                VMHOOK_LOG("{} method_proxy::call(): method pointer is null or invalid.", vmhook::error_tag);
                return value_t{ std::monostate{} };
            }

            // On modern JDKs (21+) StubRoutines::_call_stub_entry is
            // often missing from VMStructs.  In that case the entire
            // call_stub-prep block below is wasted work, so we
            // short-circuit straight into the JNI path on *this* —
            // which keeps the proxy's cached_method_id / class_handle
            // warm across iterations.
            void* const call_stub{ vmhook::detail::find_call_stub_entry() };
            if (!call_stub)
            {
                if (!vmhook::hotspot::ensure_current_java_thread())
                {
                    VMHOOK_LOG("{} method_proxy::call('{}{}'): call_stub_entry missing AND "
                               "ensure_current_java_thread() failed - cannot dispatch via either path.",
                               vmhook::error_tag, this->name(), this->signature_text);
                    return value_t{ std::monostate{} };
                }
                return this->call_jni(std::forward<args_t>(args)...);
            }

            vmhook::hotspot::method* const selected_method{ this->resolve_compatible_method<std::remove_cvref_t<args_t>...>() };
            const std::string selected_signature{ selected_method ? selected_method->get_signature() : this->signature_text };

            // (See call_jni: a no-overload-matches fail-safe guard was removed
            // because signature_matches_arguments() false-negatives on
            // unique_ptr<T> / object args.  The static-overload crash is fixed by
            // the resolve_compatible_method() hierarchy walk now covering statics.)

            if (!vmhook::hotspot::ensure_current_java_thread())
            {
                VMHOOK_LOG("{} method_proxy::call('{}{}'): no current JavaThread.", vmhook::error_tag, this->name(), selected_signature);
                return value_t{ std::monostate{} };
            }

            auto* const thread{ vmhook::hotspot::current_java_thread };
            if (!thread)
            {
                VMHOOK_LOG("{} method_proxy::call('{}{}'): current JavaThread is null after attach.", vmhook::error_tag, this->name(), selected_signature);
                return value_t{ std::monostate{} };
            }

            void* const entry{ selected_method->get_from_interpreted_entry() };
            if (!entry || !vmhook::hotspot::is_valid_pointer(entry))
            {
                VMHOOK_LOG("{} method_proxy::call('{}{}'): interpreted entry is null or invalid.", vmhook::error_tag, this->name(), selected_signature);
                return value_t{ std::monostate{} };
            }

            //  Return type
            const std::string_view sig{ selected_signature };
            const std::size_t rparen{ sig.rfind(')') };
            const char ret_char{
                rparen != std::string_view::npos ? sig[rparen + 1] : 'V' };
            const int result_type{ vmhook::detail::sig_char_to_basic_type(ret_char) };

            //  Parameter slot array
            // The call_stub passes parameters[] to the interpreter as locals[].
            // Each slot is an intptr_t: primitives are zero-extended, object
            // references are uncompressed decoded OOP pointers.
            std::intptr_t params[8]{};
            std::size_t   param_idx{ 0 };

            if (this->object && !this->static_field)
            {
                params[param_idx++] = reinterpret_cast<std::intptr_t>(this->object);
            }

            auto pack = [&](auto&& a) noexcept
                {
                    if (param_idx >= 8)
                    {
                        return;
                    }
                    using clean_t = std::remove_cvref_t<decltype(a)>;
                    if constexpr (std::is_same_v<clean_t, std::string>)
                    {
                        void* const string_oop{ vmhook::make_java_string(a) };
                        if (!string_oop)
                        {
                            VMHOOK_LOG("{} method_proxy::call('{}{}'): failed to allocate Java String argument.", vmhook::error_tag, this->name(), selected_signature);
                        }
                        params[param_idx++] = reinterpret_cast<std::intptr_t>(string_oop);
                    }
                    else if constexpr (std::is_same_v<clean_t, std::string_view>)
                    {
                        void* const string_oop{ vmhook::make_java_string(a) };
                        if (!string_oop)
                        {
                            VMHOOK_LOG("{} method_proxy::call('{}{}'): failed to allocate Java String argument.", vmhook::error_tag, this->name(), selected_signature);
                        }
                        params[param_idx++] = reinterpret_cast<std::intptr_t>(string_oop);
                    }
                    else if constexpr (std::is_same_v<clean_t, const char*> || std::is_same_v<clean_t, char*>)
                    {
                        void* const string_oop{ vmhook::make_java_string(a ? std::string_view{ a } : std::string_view{}) };
                        if (!string_oop)
                        {
                            VMHOOK_LOG("{} method_proxy::call('{}{}'): failed to allocate Java String argument.", vmhook::error_tag, this->name(), selected_signature);
                        }
                        params[param_idx++] = reinterpret_cast<std::intptr_t>(string_oop);
                    }
                    else if constexpr (vmhook::detail::is_unique_ptr_v<clean_t>)
                    {
                        using wrapper_type = typename vmhook::detail::is_unique_ptr<clean_t>::value_type_t;
                        if constexpr (std::is_base_of_v<vmhook::object_base, wrapper_type>)
                        {
                            params[param_idx++] = reinterpret_cast<std::intptr_t>(a ? a->get_instance() : nullptr);
                        }
                    }
                    else if constexpr (std::is_base_of_v<vmhook::object_base, clean_t>)
                    {
                        params[param_idx++] = reinterpret_cast<std::intptr_t>(a.get_instance());
                    }
                    else
                    {
                        static_assert(sizeof(clean_t) <= 8);
                        std::intptr_t v{};
                        std::memcpy(&v, &a, sizeof(clean_t));
                        params[param_idx++] = v;
                    }
                };
            (pack(std::forward<args_t>(args)), ...);

            //  Call the stub
            // Windows x64 calling convention  8 arguments:
            //   rcx = link  (return address for stub frame, -1 sentinel)
            //   rdx = result* (where the Java return value is written)
            //   r8  = result_type (BasicType enum)
            //   r9  = Method*
            //   stk = entry_point, parameters*, param_count, JavaThread*
            using call_stub_fn_t = void (*)(
                void*,          // link
                std::intptr_t*, // result
                int,            // result_type
                void*,          // method
                void*,          // entry_point
                std::intptr_t*, // parameters
                int,            // size_of_parameters
                void*           // thread
                );

            std::intptr_t result_holder{ 0 };
            const vmhook::hotspot::java_thread_state previous_state{ thread->get_thread_state() };
            thread->set_thread_state(vmhook::hotspot::java_thread_state::_thread_in_Java);

            reinterpret_cast<call_stub_fn_t>(call_stub)(
                reinterpret_cast<void*>(static_cast<std::intptr_t>(-1)),
                &result_holder,
                result_type,
                reinterpret_cast<void*>(selected_method),
                entry,
                params,
                static_cast<int>(param_idx),
                reinterpret_cast<void*>(thread)
                );

            //  Decode result
            thread->set_thread_state(previous_state);

            switch (ret_char)
            {
            case 'Z': return value_t{ (result_holder & 1) != 0 };
            case 'B': return value_t{ static_cast<std::int8_t> (result_holder) };
            case 'S': return value_t{ static_cast<std::int16_t>(result_holder) };
            case 'I': return value_t{ static_cast<std::int32_t>(result_holder) };
            case 'J': return value_t{ static_cast<std::int64_t>(result_holder) };
            case 'C': return value_t{ static_cast<std::uint16_t>(result_holder) };
            case 'F':
            {
                float f{};
                const std::int32_t bits{ static_cast<std::int32_t>(result_holder) };
                std::memcpy(&f, &bits, sizeof(f));
                return value_t{ f };
            }
            case 'D':
            {
                double d{};
                std::memcpy(&d, &result_holder, sizeof(d));
                return value_t{ d };
            }
            case 'V': return value_t{ std::monostate{} };
            default:
            {
                // Reference / array return ('L' / '[').  The call stub leaves an
                // UNCOMPRESSED oop in result_holder (see the params[] comment
                // above).  The previous `static_cast<std::uint32_t>(result_holder)`
                // both TRUNCATED the 64-bit pointer to 32 bits AND mislabelled an
                // uncompressed oop as the compressed-oop that value_t's uint32_t
                // alternative is documented to hold - so on JDKs where the call
                // stub IS available (JDK 8/11/17) a `Ljava/lang/String;`-returning
                // call() silently produced "" while the JNI fallback path (JDK 21+)
                // returned the real text.  Now: decode java.lang.String straight
                // to UTF-8 for parity with call_jni, and re-encode any other
                // reference to a real compressed oop so value_t round-trips
                // through decode_oop_pointer instead of truncating.
                void* const result_oop{ reinterpret_cast<void*>(result_holder) };
                if (!result_oop)
                {
                    return value_t{ std::monostate{} };
                }
                if (rparen != std::string_view::npos
                    && sig.substr(rparen + 1) == "Ljava/lang/String;")
                {
                    return value_t{ vmhook::read_java_string(result_oop) };
                }
                return value_t{ vmhook::hotspot::encode_oop_pointer(result_oop) };
            }
            }
        }

        /*
            @brief Returns the method name.
        */
        auto name() const noexcept
            -> std::string
        {
            if (!this->method)
            {
                return {};
            }
            return this->method->get_name();
        }

        /*
            @brief Returns the JVM method signature.
        */
        auto signature() const noexcept
            -> std::string_view
        {
            return this->signature_text;
        }

        /*
            @brief Returns true if this method is a static Java method.
            @details
            Static methods do not receive a 'this' pointer as the first argument.
            The constructor's `static_field` member is always false (it is not
            wired to any caller), so report the JVM truth directly: read
            JVM_ACC_STATIC (0x0008) from the live Method's _access_flags.  The
            static bit lives in the low byte and is stable across every
            supported JDK, so reading the flags word as u4 and masking 0x0008
            is width-independent.  Falls back to the stored member if the
            access-flags slot can't be resolved (no VMStructs / no JVM).

            Complexity: O(1) after the first VMStruct lookup (cached).
            Exception safety: noexcept.
        */
        auto is_static() const noexcept
            -> bool
        {
            if (this->method)
            {
                if (const std::uint32_t* const flags{ this->method->get_access_flags() })
                {
                    return (*flags & 0x0008u) != 0u;
                }
            }
            return this->static_field;
        }

        /*
            @brief Returns true if this method's return type is a reference / array (L or [).
            @details
            Mirrors field_proxy::is_reference() so callers using the method-vs-field
            parity helpers (raw_method / raw_address / get_compressed_oop) can branch
            on "is this an object?" without parsing the descriptor by hand.  Reads the
            character that follows the closing ')' of the signature.

            Complexity: O(1).
            Exception safety: noexcept — returns false on a malformed/empty signature.
        */
        auto is_reference() const noexcept
            -> bool
        {
            const auto close{ this->signature_text.find(')') };
            if (close == std::string::npos || close + 1 >= this->signature_text.size())
            {
                return false;
            }
            const char ret{ this->signature_text[close + 1] };
            return ret == 'L' || ret == '[';
        }

        /*
            @brief Returns the compressed OOP of the receiver object.
            @details
            Reads the first 4 bytes of the object pointer as a compressed OOP.
            Used internally when the proxy needs to pass the receiver to call().

            Complexity: O(1).
            Exception safety: noexcept — returns 0 if object is null.
        */
        auto get_compressed_oop() const noexcept
            -> std::uint32_t
        {
            if (!this->object)
            {
                return 0;
            }
            std::uint32_t value{};
            std::memcpy(&value, this->object, sizeof(value));
            return value;
        }

        /*
            @brief Returns the underlying HotSpot Method* (for advanced consumers).
            @details
            Mirror of field_proxy::raw_address.  Lets users who want to drive
            lower-level HotSpot APIs (custom hook trampolines, deopt sweeps,
            interpreter-entry rewrites) reach the Method* without going through
            vmhook::hook<T>().  Most code should prefer call() / hook<T>().

            Complexity: O(1).
            Exception safety: noexcept.

            @return The HotSpot Method*, or nullptr if the proxy holds no method
                    (e.g. constructed with a null method pointer).
        */
        auto raw_method() const noexcept
            -> vmhook::hotspot::method*
        {
            return this->method;
        }

    private:
        /*
            @brief Reads the compressed Klass* from byte offset 8 of a HotSpot OOP header.
            @details
            On 64-bit HotSpot with UseCompressedClassPointers, oopDesc._metadata._compressed_klass
            is stored at a fixed offset of 8 bytes from the object start.  This function reads
            that slot and decodes it with decode_klass_pointer().

            Complexity: O(1).
            Exception safety: noexcept — returns nullptr on failure.

            @param oop  Decoded (full 64-bit) heap pointer to the Java object.
            @return  klass* for the runtime type of oop, or nullptr on failure.
        */
        static auto klass_from_object_header(void* const oop) noexcept
            -> vmhook::hotspot::klass*
        {
            if (!oop || !vmhook::hotspot::is_valid_pointer(oop))
            {
                return nullptr;
            }

            const std::uint32_t narrow_klass{ *reinterpret_cast<const std::uint32_t*>(
                reinterpret_cast<const std::uint8_t*>(oop) + 8) };
            void* const decoded{ vmhook::hotspot::decode_klass_pointer(narrow_klass) };
            if (!decoded || !vmhook::hotspot::is_valid_pointer(decoded))
            {
                return nullptr;
            }

            return reinterpret_cast<vmhook::hotspot::klass*>(decoded);
        }

        /*
            @brief Tests whether a C++ argument type is compatible with a JVM parameter descriptor.
            @details
            Compile-time dispatch based on argument_type:
              - string types         -> descriptor == "Ljava/lang/String;"
              - unique_ptr<T extends object_base> -> 'L' class descriptor matching T's registered name
              - object_base derived  -> same as unique_ptr case
              - bool                 -> "Z"
              - integral 1-byte      -> "B" or "Z"
              - integral 2-byte      -> "S" or "C"
              - integral 4-byte      -> "I"
              - integral 8-byte      -> "J"
              - float                -> "F"
              - double               -> "D"
              - other                -> false

            Used by signature_matches_arguments() to select the correct overloaded method.

            Complexity: O(1) — compile-time trait dispatch.
            Exception safety: noexcept.

            @tparam argument_type  The C++ argument type to test.
            @param descriptor      One JVM type descriptor token from a method signature.
            @return  true if argument_type is compatible with descriptor.
        */
        template<typename argument_type>
        static auto argument_matches_descriptor(const std::string_view descriptor) noexcept
            -> bool
        {
            using clean_type = std::remove_cvref_t<argument_type>;

            if constexpr (std::is_same_v<clean_type, std::string> || std::is_same_v<clean_type, std::string_view> || std::is_same_v<clean_type, const char*> || std::is_same_v<clean_type, char*>)
            {
                return descriptor == "Ljava/lang/String;";
            }
            else if constexpr (vmhook::detail::is_unique_ptr_v<clean_type>)
            {
                using wrapper_type = typename vmhook::detail::is_unique_ptr<clean_type>::value_type_t;
                if constexpr (std::is_base_of_v<vmhook::object_base, wrapper_type>)
                {
                    const auto type_map_entry{ vmhook::type_to_class_map.find(std::type_index{ typeid(wrapper_type) }) };
                    return descriptor.size() >= 3
                        && descriptor.front() == 'L'
                        && descriptor.back() == ';'
                        && (type_map_entry == vmhook::type_to_class_map.end() || descriptor.substr(1, descriptor.size() - 2) == type_map_entry->second);
                }
                else
                {
                    return false;
                }
            }
            else if constexpr (std::is_base_of_v<vmhook::object_base, clean_type>)
            {
                const auto type_map_entry{ vmhook::type_to_class_map.find(std::type_index{ typeid(clean_type) }) };
                return descriptor.size() >= 3
                    && descriptor.front() == 'L'
                    && descriptor.back() == ';'
                    && (type_map_entry == vmhook::type_to_class_map.end() || descriptor.substr(1, descriptor.size() - 2) == type_map_entry->second);
            }
            else if constexpr (std::is_same_v<clean_type, bool>)
            {
                return descriptor == "Z";
            }
            // Java `char` is always 2 bytes wide and unsigned (UTF-16 code unit);
            // map `char16_t` and `std::uint16_t` to "C" specifically.  This used
            // to be folded into the generic 2-byte branch alongside "S", which
            // meant a `uint16_t` argument matched BOTH the `S` and `C`
            // descriptors — recreating the same first-match-wins overload
            // resolution bug that bit vanilla Minecraft 1.8.9
            // EntityPlayerSP::a() before commit 1a536b7 fixed it on the JNI
            // dispatch side.
            else if constexpr (std::is_same_v<clean_type, char16_t> || std::is_same_v<clean_type, std::uint16_t>)
            {
                return descriptor == "C";
            }
            // Java `byte` is signed 8-bit; map `int8_t` / `uint8_t` /
            // `(un)signed char` to "B" only (NOT "Z" — that's the boolean
            // descriptor and `bool` is the only C++ type allowed to match it).
            else if constexpr (std::is_integral_v<clean_type> && sizeof(clean_type) == 1)
            {
                return descriptor == "B";
            }
            // Java `short` is signed 16-bit; signed 2-byte types map to "S"
            // only (the unsigned 2-byte branch above already claimed "C").
            else if constexpr (std::is_integral_v<clean_type> && sizeof(clean_type) == 2)
            {
                return descriptor == "S";
            }
            else if constexpr (std::is_integral_v<clean_type> && sizeof(clean_type) == 4)
            {
                return descriptor == "I";
            }
            else if constexpr (std::is_integral_v<clean_type> && sizeof(clean_type) == 8)
            {
                return descriptor == "J";
            }
            else if constexpr (std::is_same_v<clean_type, float>)
            {
                return descriptor == "F";
            }
            else if constexpr (std::is_same_v<clean_type, double>)
            {
                return descriptor == "D";
            }
            else
            {
                return false;
            }
        }

        /*
            @brief Extracts the next descriptor token from a JVM method signature.
            @details
            Advances position past any leading array brackets, then past the full
            type descriptor (a single letter for primitives, or 'L...;' for objects).
            Returns the full descriptor including leading '[' array dimensions.
            Returns an empty string_view when the parse is complete or position reaches
            close_paren.

            Complexity: O(D) where D = descriptor token length.
            Exception safety: noexcept.

            @param signature    Full JVM method signature (e.g. "(ILjava/lang/String;)V").
            @param position     Current parse position; advanced in-place past the token.
            @param close_paren  Index of the closing ')' character.
            @return  The next descriptor token, or empty on end-of-parameters.
        */
        static auto next_argument_descriptor(const std::string_view signature, std::size_t& position, const std::size_t close_paren) noexcept
            -> std::string_view
        {
            const std::size_t start{ position };
            while (position < close_paren && signature[position] == '[')
            {
                ++position;
            }

            if (position >= close_paren)
            {
                return {};
            }

            if (signature[position] == 'L')
            {
                const std::size_t semicolon{ signature.find(';', position) };
                if (semicolon == std::string_view::npos || semicolon > close_paren)
                {
                    return {};
                }
                position = semicolon + 1;
                return signature.substr(start, position - start);
            }

            ++position;
            return signature.substr(start, position - start);
        }

        /*
            @brief Returns true if the parameter list of signature is compatible with args_t.
            @details
            Parses the JVM method signature from the opening '(' to the closing ')',
            calling next_argument_descriptor() + argument_matches_descriptor<arg>() for
            each argument in the pack.  Returns true only if all arguments match and there
            are no leftover descriptor tokens.

            Complexity: O(P) where P = number of parameters.
            Exception safety: noexcept.

            @tparam args_t  C++ argument types to test against the signature.
            @param signature  Full JVM method signature.
            @return  true if args_t are all compatible with signature's parameter list.
        */
        template<typename... args_t>
        static auto signature_matches_arguments(const std::string_view signature) noexcept
            -> bool
        {
            const std::size_t open_paren{ signature.find('(') };
            const std::size_t close_paren{ signature.find(')') };
            if (open_paren == std::string_view::npos || close_paren == std::string_view::npos || close_paren < open_paren)
            {
                return false;
            }

            std::size_t position{ open_paren + 1 };
            bool matches{ true };
            ([&]
                {
                    if (!matches)
                    {
                        return;
                    }
                    const std::string_view descriptor{ next_argument_descriptor(signature, position, close_paren) };
                    matches = !descriptor.empty() && argument_matches_descriptor<args_t>(descriptor);
                }(), ...);

            return matches && position == close_paren;
        }

        /*
            @brief Finds the Method* whose signature best matches the given C++ argument types.
            @details
            First tests whether this->signature_text already matches args_t using
            signature_matches_arguments().  If so, returns this->method directly.
            Otherwise walks the runtime klass hierarchy (obtained from the object header),
            and checks every method with the same name for a matching signature.  Returns
            the first match, or this->method as a fallback if nothing better is found.

            This allows a single get_method("foo") result to handle overloaded Java methods
            when the call site provides typed arguments.

            Complexity: O(N * M) where N = number of methods in the class hierarchy,
                        M = signature parse cost per method.
            Exception safety: noexcept — returns this->method on any failure.

            @tparam args_t  C++ argument types from the call<>() invocation.
            @return  The best-matching Method*, or this->method as fallback.
        */
        template<typename... args_t>
        auto resolve_compatible_method() const noexcept
            -> vmhook::hotspot::method*
        {
            // Explicit-signature proxies (get_method/static_method(name, SIGNATURE))
            // pinned an EXACT overload; honour it verbatim.  Do NOT re-pick a
            // different overload from the C++ argument types — a std::string arg
            // must NOT turn an explicit combo(CharSequence) request into
            // combo(String).  Name-only proxies leave signature_pinned false so the
            // arg-driven resolution below still runs for them.
            if (this->signature_pinned)
            {
                return this->method;
            }

            if (signature_matches_arguments<args_t...>(this->signature_text))
            {
                return this->method;
            }

            // Instance: klass from the receiver's object header.  STATIC
            // (object == nullptr): the declaring klass comes from the Method's
            // ConstantPool _pool_holder.  Without this, a static_method(name)->
            // call(typedArg) could NOT re-pick the matching overload — it
            // dispatched whatever get_method() latched first by _methods-array
            // (Symbol-address) order.  When that overload's parameter kind
            // mismatched the C++ argument (e.g. a primitive blasted into a
            // reference slot, or vice-versa), the JVM stored a bogus oop / read a
            // wild reference and crashed (a real JVM-tearing AV, reproduced on
            // JDK 8).  Deriving the static klass here lets the hierarchy walk
            // below select the correct overload and eliminates the wrong-slot
            // dispatch.
            vmhook::hotspot::klass* resolved_klass{
                this->object ? klass_from_object_header(this->object) : nullptr };
            if (!resolved_klass && !this->object && this->method)
            {
                if (auto* const cm{ this->method->get_const_method() };
                    cm && vmhook::hotspot::is_valid_pointer(cm))
                {
                    if (auto* const cp{ cm->get_constants() };
                        cp && vmhook::hotspot::is_valid_pointer(cp))
                    {
                        static const auto* const pool_holder_entry{
                            vmhook::hotspot::iterate_struct_entries("ConstantPool", "_pool_holder") };
                        if (pool_holder_entry)
                        {
                            auto* const holder{ *reinterpret_cast<vmhook::hotspot::klass**>(
                                reinterpret_cast<std::uint8_t*>(cp) + pool_holder_entry->offset) };
                            if (holder && vmhook::hotspot::is_valid_pointer(holder))
                            {
                                resolved_klass = holder;
                            }
                        }
                    }
                }
            }
            if (!resolved_klass)
            {
                return this->method;
            }

            const std::string method_name{ this->name() };
            for (vmhook::hotspot::klass* k{ resolved_klass }; k != nullptr; k = k->get_super())
            {
                const std::int32_t method_count{ k->get_methods_count() };
                vmhook::hotspot::method** const methods_array{ k->get_methods_ptr() };
                if (!methods_array || method_count <= 0)
                {
                    continue;
                }

                for (std::int32_t method_index{ 0 }; method_index < method_count; ++method_index)
                {
                    vmhook::hotspot::method* const current_method{ methods_array[method_index] };
                    if (!current_method || !vmhook::hotspot::is_valid_pointer(current_method) || current_method->get_name() != method_name)
                    {
                        continue;
                    }

                    const std::string current_signature{ current_method->get_signature() };
                    if (signature_matches_arguments<args_t...>(current_signature))
                    {
                        return current_method;
                    }
                }
            }

            return this->method;
        }

        void* object;
        vmhook::hotspot::method* method;
        std::string signature_text;
        bool        static_field;
        // True when this proxy was created via get_method(name, SIGNATURE) /
        // static_method(name, SIGNATURE): the caller pinned an EXACT overload, so
        // resolve_compatible_method() must NOT re-pick a different overload from
        // the C++ argument types (which would silently dispatch e.g. combo(String)
        // for an explicit combo(CharSequence) request).  Name-only proxies leave
        // it false so their arg-driven overload resolution keeps working.
        bool        signature_pinned{ false };

        // Lazy caches for the JNI fallback path.  Resolving
        // jmethodID / jclass once and reusing them across calls is
        // the single biggest speedup against pure JNI for tight
        // call loops (GetMethodID is a hashtable lookup inside the
        // JVM).  Both are mutable because call() is const.
        mutable void* cached_method_id{ nullptr };
        mutable void* cached_class_handle{ nullptr };
        // Cached return-type char parsed from signature_text on the
        // first call.  Skips the per-call rfind() in the hot path.
        // Zero means "not yet computed"; the actual chars are all
        // non-zero (`I`, `J`, `V`, ...).
        mutable char  cached_ret_char{ 0 };
        // When resolve_compatible_method picks a different overload than
        // the one get_method() initially latched onto, store its descriptor
        // here so the call_jni dispatch uses the right signature for
        // GetMethodID / ret-type peek / return-type-string detection.
        mutable std::string cached_effective_signature{};
    };

    // --- Object base class ----------------------------------------------------

    /*
        @brief Alias for a decoded (uncompressed) Java object pointer.
        @details
        This is the type passed to vmhook::object constructors. Obtain it from a
        hook frame via frame->get_arguments<vmhook::oop_type_t>(), which internally calls
        vmhook::hotspot::decode_oop_pointer() to convert the 32-bit compressed OOP to a full
        64-bit address.  It is NOT a JNI global reference - no GC handles are
        created and the pointer remains valid only for the duration of the hook.
    */
    using oop_type_t = void*;

    /*
        @brief Short alias for oop_type_t; both names refer to a decoded Java heap pointer.
        @details
        Provided for brevity in user code:
            vmhook::hook<my_class>("tick",
                [](vmhook::return_value& ret, const std::unique_ptr<my_class>& self, vmhook::oop_t other) { ... });
    */
    using oop_t = oop_type_t;

    /*
        @brief Base class for C++ wrappers around live Java objects.
        @details
        Derive from this class to create a typed faade for a Java class.
        get_field() handles both instance and static fields automatically -
        the library reads JVM_ACC_STATIC from the InstanceKlass._fields array
        so there is no separate static-field accessor.

        Example:

            class http_client : public vmhook::object
            {
            public:
                explicit http_client(vmhook::oop_type_t instance)
                    : vmhook::object{ instance }
                {}

                // auto return - field_proxy::value_t converts implicitly at the call site
                auto is_connected() -> bool { return get_field("isConnected")->get(); }
                auto get_health()   -> int { return get_field("health")->get(); }

                // Cast inside the method if you want a concrete return type
                auto get_timeout()  -> int { return static_cast<int>(get_field("timeout")->get()); }

                // Static field - get_field resolves through the registered class.
                static auto get_version() -> std::string { return get_field("VERSION")->get(); }

                // Writing a field
                auto set_health(int hp) -> void { get_field("health")->set(hp); }
            };

            vmhook::register_class<http_client>("com/example/HttpClient");

            // Inside a hook detour:
            auto [self] = frame->get_arguments<vmhook::oop_type_t>();
            http_client client{ self };
            bool ok  = client.is_connected(); // operator bool()  fires
            int  hp  = client.get_health();   // operator int()   fires
            client.set_health(100);

        @note The wrapped pointer is a raw decoded OOP, not a JNI global reference.
              It is valid for the duration of the hook invocation only.
    */
    class object_base
    {
    public:
        /*
            @brief Wraps a decoded Java object pointer.
            @param instance Decoded OOP from frame->get_arguments<vmhook::oop_type_t>().
                            May be nullptr if the Java reference is null.
        */
        explicit object_base(oop_type_t instance = nullptr) noexcept
            : instance{ instance }
        {
        }

        virtual ~object_base() = default;

        /*
            @brief Copy constructor — copies the raw OOP pointer.
            @details
            Both the source and the copy point to the same Java object.
            The pointer is not a GC handle so no reference counting occurs.
        */
        object_base(const object_base&) = default;

        /*
            @brief Copy assignment — copies the raw OOP pointer.
        */
        auto operator=(const object_base&)
            -> object_base & = default;

        /*
            @brief Move constructor — transfers the OOP pointer and nulls the source.
            @details
            After the move, the source instance is set to nullptr so that
            the moved-from object is safely destructible.
        */
        object_base(object_base&& other) noexcept
            : instance{ other.instance }
        {
            other.instance = nullptr;
        }

        auto operator=(object_base&& other) noexcept
            -> object_base&
        {
            if (this != &other)
            {
                this->instance = other.instance;
                other.instance = nullptr;
            }
            return *this;
        }

        /*
            @brief Returns the raw decoded OOP pointer held by this wrapper.
        */
        auto get_instance() const noexcept
            -> oop_type_t
        {
            return this->instance;
        }

        /*
            @brief Returns a field proxy for any field declared on this class.
            @param name Exact Java field name.
            @return Optional holding the field proxy, or nullopt on failure.
            @details
            Works for both instance and static fields.  The JVM_ACC_STATIC flag is
            read from InstanceKlass._fields so no static/instance distinction is
            needed at the call site.

            Instance fields:  value lives at decoded_object_ptr + field_offset.
            Static fields:    value lives at java.lang.Class mirror + field_offset.

            The klass is resolved from type_to_class_map via typeid(*this) (dynamic type),
            so the derived C++ class must have been registered with register_class<T>()
            before this is called.  Field entries are cached after the first lookup.

            @note Dereferencing a nullopt with -> is undefined behaviour.
                  In production code always check the optional, or assert field names
                  are correct at development time.
        */
        auto get_field(const std::string_view name) const
            -> std::optional<vmhook::field_proxy>
        {
            vmhook::hotspot::klass* const resolved_klass{ this->resolve_klass() };
            if (!resolved_klass)
            {
                VMHOOK_LOG("{} object::get_field('{}'): klass not resolved for this object - "
                           "wrapper type may not be registered, or class not loaded.",
                           vmhook::error_tag, name);
                return std::nullopt;
            }

            const auto entry{ vmhook::find_field(resolved_klass, name) };
            if (!entry)
            {
                VMHOOK_LOG("{} object::get_field('{}'): field not found in class hierarchy.",
                           vmhook::error_tag, name);
                return std::nullopt;
            }

            if (entry->is_static)
            {
                // Inherited static fields live on the DECLARING class's mirror,
                // whose offset basis differs from resolved_klass's mirror; using
                // the latter read/wrote garbage (and could fault past the end of
                // a subclass mirror).  find_field() records the declaring klass.
                vmhook::hotspot::klass* const mirror_klass{ entry->declaring_klass ? entry->declaring_klass : resolved_klass };
                void* const mirror{ mirror_klass->get_java_mirror() };
                if (!mirror || !vmhook::hotspot::is_valid_pointer(mirror))
                {
                    VMHOOK_LOG("{} object::get_field('{}') failed to get java.lang.Class mirror.", vmhook::error_tag, name);
                    return std::nullopt;
                }
                void* const field_pointer{ reinterpret_cast<std::uint8_t*>(mirror) + entry->offset };
                return vmhook::field_proxy{ field_pointer, entry->signature, true };
            }

            if (!this->instance)
            {
                VMHOOK_LOG("{} object::get_field('{}') instance pointer is null.", vmhook::error_tag, name);
                return std::nullopt;
            }

            void* const field_pointer{ reinterpret_cast<std::uint8_t*>(this->instance) + entry->offset };
            return vmhook::field_proxy{ field_pointer, entry->signature, false };
        }

        /*
            @brief Returns a field proxy for a static field identified by wrapper_type and name.
            @details
            Unlike the instance overload, this variant does not need a live OOP because
            static fields live on the java.lang.Class mirror rather than on an object.
            Fails with nullopt if the field is not static (caller should use the instance
            overload in that case).

            Complexity: O(F) on first call; O(1) after caching.
            Exception safety: may throw vmhook::exception internally but returns nullopt on failure.

            @param wrapper_type  std::type_index of the registered C++ wrapper class.
            @param name          Exact Java field name.
            @return  Optional field_proxy, or nullopt on failure.
        */
        static auto get_field(const std::type_index wrapper_type, const std::string_view name)
            -> std::optional<vmhook::field_proxy>
        {
            vmhook::hotspot::klass* const resolved_klass{ resolve_klass(wrapper_type) };
            if (!resolved_klass)
            {
                VMHOOK_LOG("{} object::get_field('{}') (static): klass not resolved for wrapper "
                           "type - register_class<T>() may be missing or class not loaded.",
                           vmhook::error_tag, name);
                return std::nullopt;
            }

            const auto entry{ vmhook::find_field(resolved_klass, name) };
            if (!entry)
            {
                VMHOOK_LOG("{} object::get_field('{}') (static): field not found in class "
                           "hierarchy.",
                           vmhook::error_tag, name);
                return std::nullopt;
            }

            if (!entry->is_static)
            {
                VMHOOK_LOG("{} object::get_field('{}') needs an object instance.", vmhook::error_tag, name);
                return std::nullopt;
            }

            // Resolve the mirror of the klass that DECLARES the static, not the
            // wrapper's start klass: for an inherited static they are different
            // mirror oops and `entry->offset` only indexes the declaring one.
            vmhook::hotspot::klass* const mirror_klass{ entry->declaring_klass ? entry->declaring_klass : resolved_klass };
            void* const mirror{ mirror_klass->get_java_mirror() };
            if (!mirror || !vmhook::hotspot::is_valid_pointer(mirror))
            {
                VMHOOK_LOG("{} object::get_field('{}') failed to get java.lang.Class mirror.", vmhook::error_tag, name);
                return std::nullopt;
            }

            void* const field_pointer{ reinterpret_cast<std::uint8_t*>(mirror) + entry->offset };
            return vmhook::field_proxy{ field_pointer, entry->signature, true };
        }

        /*
            @brief Returns a method proxy for a method declared on this class.
            @param method_name Exact Java method name.
            @return Optional holding the method proxy, or nullopt on failure.
            @details
            Looks up the method in the InstanceKlass._methods array by name.
            The returned proxy can be used to call the method with arguments.

            Usage:
                auto say_hi() -> void { return get_method("sayHi")->call(); }
                auto take_values(int a, const std::string& b) -> void {
                    return get_method("takeValues")->call(a, b);
                }
        */
        auto get_method(const std::string_view method_name) const
            -> std::optional<vmhook::method_proxy>
        {
            vmhook::hotspot::klass* const resolved_klass{ this->resolve_klass() };
            if (!resolved_klass)
            {
                VMHOOK_LOG("{} object::get_method('{}'): klass not resolved for this object.",
                           vmhook::error_tag, method_name);
                return std::nullopt;
            }

            // Walk the superclass chain so inherited methods are found.
            for (vmhook::hotspot::klass* k{ resolved_klass }; k != nullptr; k = k->get_super())
            {
                const std::int32_t method_count{ k->get_methods_count() };
                vmhook::hotspot::method** const methods_array{ k->get_methods_ptr() };

                if (!methods_array || method_count <= 0)
                {
                    continue;
                }

                for (std::int32_t method_index{ 0 }; method_index < method_count; ++method_index)
                {
                    vmhook::hotspot::method* const current_method{ methods_array[method_index] };
                    if (current_method && vmhook::hotspot::is_valid_pointer(current_method)
                        && current_method->get_name() == method_name)
                    {
                        return vmhook::method_proxy{ this->instance, current_method, current_method->get_signature() };
                    }
                }
            }

            VMHOOK_LOG("{} object::get_method('{}'): method not found in class hierarchy.",
                       vmhook::error_tag, method_name);
            return std::nullopt;
        }

        /*
            @brief Returns a method proxy for a method that matches both name and JVM signature.
            @details
            Walks the class hierarchy searching for a method whose name equals method_name
            and whose JVM descriptor equals method_signature.  Use this overload when the
            class has overloaded Java methods with the same name but different signatures.

            Complexity: O(N * M) where N = methods in the hierarchy, M = name comparison cost.
            Exception safety: does not throw; returns nullopt on failure.

            @param method_name       Exact Java method name.
            @param method_signature  JVM descriptor, e.g. "(I)Ljava/lang/String;".
            @return  Optional method_proxy, or nullopt if not found.
        */
        auto get_method(const std::string_view method_name, const std::string_view method_signature) const
            -> std::optional<vmhook::method_proxy>
        {
            vmhook::hotspot::klass* const resolved_klass{ this->resolve_klass() };
            if (!resolved_klass)
            {
                VMHOOK_LOG("{} object::get_method('{}{}'): klass not resolved for this object.",
                           vmhook::error_tag, method_name, method_signature);
                return std::nullopt;
            }

            // Walk the superclass chain so inherited methods are found.
            for (vmhook::hotspot::klass* k{ resolved_klass }; k != nullptr; k = k->get_super())
            {
                const std::int32_t method_count{ k->get_methods_count() };
                vmhook::hotspot::method** const methods_array{ k->get_methods_ptr() };

                if (!methods_array || method_count <= 0)
                {
                    continue;
                }

                for (std::int32_t method_index{ 0 }; method_index < method_count; ++method_index)
                {
                    vmhook::hotspot::method* const current_method{ methods_array[method_index] };
                    if (!current_method || !vmhook::hotspot::is_valid_pointer(current_method))
                    {
                        continue;
                    }

                    const std::string current_signature{ current_method->get_signature() };
                    if (current_method->get_name() == method_name && current_signature == method_signature)
                    {
                        return vmhook::method_proxy{ this->instance, current_method, current_signature,
                                                     /*signature_pinned=*/true };
                    }
                }
            }

            VMHOOK_LOG("{} object::get_method('{}{}'): no method with this exact name+signature "
                       "found in class hierarchy.",
                       vmhook::error_tag, method_name, method_signature);
            return std::nullopt;
        }

        /*
            @brief Returns a method proxy for a static method identified by wrapper_type and name.
            @details
            Like the static get_field() overload but for methods.  The returned proxy has
            a null object pointer so it cannot be used to call instance methods.

            Complexity: O(N * M) where N = methods in the hierarchy, M = name comparison cost.
            Exception safety: does not throw; returns nullopt on failure.

            @param wrapper_type  std::type_index of the registered C++ wrapper class.
            @param method_name   Exact Java method name.
            @return  Optional method_proxy, or nullopt if not found.
        */
        static auto get_method(const std::type_index wrapper_type, const std::string_view method_name)
            -> std::optional<vmhook::method_proxy>
        {
            vmhook::hotspot::klass* const resolved_klass{ resolve_klass(wrapper_type) };
            if (!resolved_klass)
            {
                VMHOOK_LOG("{} object::get_method('{}') (static): klass not resolved for wrapper type.",
                           vmhook::error_tag, method_name);
                return std::nullopt;
            }

            // Walk the superclass chain so inherited methods are found.
            for (vmhook::hotspot::klass* k{ resolved_klass }; k != nullptr; k = k->get_super())
            {
                const std::int32_t method_count{ k->get_methods_count() };
                vmhook::hotspot::method** const methods_array{ k->get_methods_ptr() };

                if (!methods_array || method_count <= 0)
                {
                    continue;
                }

                for (std::int32_t method_index{ 0 }; method_index < method_count; ++method_index)
                {
                    vmhook::hotspot::method* const current_method{ methods_array[method_index] };
                    if (current_method && vmhook::hotspot::is_valid_pointer(current_method)
                        && current_method->get_name() == method_name)
                    {
                        return vmhook::method_proxy{ nullptr, current_method, current_method->get_signature() };
                    }
                }
            }

            VMHOOK_LOG("{} object::get_method('{}') (static): method not found in class hierarchy.",
                       vmhook::error_tag, method_name);
            return std::nullopt;
        }

        /*
            @brief Returns a method proxy for a static method matching both name and JVM signature.
            @details
            Combines the type_index-based lookup from the static name-only overload with the
            signature-matching logic from the instance name+signature overload.  Use when the
            Java class has overloaded static methods.

            Complexity: O(N * M).
            Exception safety: does not throw; returns nullopt on failure.

            @param wrapper_type      std::type_index of the registered C++ wrapper class.
            @param method_name       Exact Java method name.
            @param method_signature  JVM descriptor, e.g. "(I)V".
            @return  Optional method_proxy, or nullopt if not found.
        */
        static auto get_method(const std::type_index wrapper_type, const std::string_view method_name, const std::string_view method_signature)
            -> std::optional<vmhook::method_proxy>
        {
            vmhook::hotspot::klass* const resolved_klass{ resolve_klass(wrapper_type) };
            if (!resolved_klass)
            {
                VMHOOK_LOG("{} object::get_method('{}{}') (static): klass not resolved for wrapper type.",
                           vmhook::error_tag, method_name, method_signature);
                return std::nullopt;
            }

            // Walk the superclass chain so inherited methods are found.
            for (vmhook::hotspot::klass* k{ resolved_klass }; k != nullptr; k = k->get_super())
            {
                const std::int32_t method_count{ k->get_methods_count() };
                vmhook::hotspot::method** const methods_array{ k->get_methods_ptr() };

                if (!methods_array || method_count <= 0)
                {
                    continue;
                }

                for (std::int32_t method_index{ 0 }; method_index < method_count; ++method_index)
                {
                    vmhook::hotspot::method* const current_method{ methods_array[method_index] };
                    if (!current_method || !vmhook::hotspot::is_valid_pointer(current_method))
                    {
                        continue;
                    }

                    const std::string current_signature{ current_method->get_signature() };
                    if (current_method->get_name() == method_name && current_signature == method_signature)
                    {
                        return vmhook::method_proxy{ nullptr, current_method, current_signature,
                                                     /*signature_pinned=*/true };
                    }
                }
            }

            VMHOOK_LOG("{} object::get_method('{}{}') (static): no method with this exact "
                       "name+signature found in class hierarchy.",
                       vmhook::error_tag, method_name, method_signature);
            return std::nullopt;
        }

    protected:
        /*
            @brief The raw decoded OOP pointer to the wrapped Java object.
        */
        oop_type_t instance{ nullptr };

    private:
        /*
            @brief Resolves the HotSpot klass for the dynamic type of this object.
            @details
            Uses typeid(*this) to look up the class name in type_to_class_map, then
            calls find_class() which walks the ClassLoaderDataGraph (cached after
            the first call).
            @return Pointer to the klass, or nullptr if the type is not registered.
        */
        auto resolve_klass() const
            -> vmhook::hotspot::klass*
        {
            return resolve_klass(std::type_index{ typeid(*this) });
        }

        /*
            @brief Resolves the HotSpot klass for an arbitrary registered C++ wrapper type.
            @details
            Looks up wrapper_type in type_to_class_map and delegates to vmhook::find_class()
            which searches the ClassLoaderDataGraph (cached after the first call).
            Used by the static overloads of get_field() and get_method() that cannot use
            typeid(*this) because there is no object instance.

            Complexity: O(1) after the first call per type (cached in klass_lookup_cache).
            Exception safety: does not throw; returns nullptr and logs on failure.

            @param wrapper_type  std::type_index of the C++ wrapper class.
            @return  klass*, or nullptr if the type is not registered or the class is not found.
        */
        static auto resolve_klass(const std::type_index wrapper_type)
            -> vmhook::hotspot::klass*
        {
            const auto type_map_entry{ vmhook::type_to_class_map.find(wrapper_type) };
            if (type_map_entry == vmhook::type_to_class_map.end())
            {
                VMHOOK_LOG("{} object::resolve_klass() type '{}' not registered via register_class<T>().", vmhook::error_tag, wrapper_type.name());
                return nullptr;
            }

            vmhook::hotspot::klass* const found_klass{ vmhook::find_class(type_map_entry->second) };
            if (!found_klass)
            {
                VMHOOK_LOG("{} object::resolve_klass() class '{}' not found in JVM.", vmhook::error_tag, type_map_entry->second);
            }

            return found_klass;
        }
    };

    /*
        @brief CRTP base class for typed Java-object wrappers.
        @details
        Derive from vmhook::object<YourClass> (not object_base directly).

        Field / method access syntax:

          - Inside an instance C++ method, `get_field("name")` /
            `get_method("name")` route through the implicit `this` and
            resolve via the live OOP.  These are inherited from
            object_base.

          - Inside a static C++ method, the same `get_field("name")` /
            `get_method("name")` call resolves through the C++23
            deducing-this overloads below on MSVC and Clang.  These
            compilers correctly exclude deducing-this overloads from
            overload resolution when no implicit object is available,
            so the static fallbacks defined here win automatically.

          - On GCC the deducing-this overloads are still selected from
            a static-call context (the compiler picks the better
            argument-type match before checking object availability,
            then errors), so static methods on GCC must call
            `static_field("name")` / `static_method("name")` instead.
            The same names are available on every compiler for
            authors who want a uniformly portable call site.

        Usage:
            class my_entity : public vmhook::object<my_entity>
            {
            public:
                explicit my_entity(vmhook::oop_t oop) : vmhook::object<my_entity>{ oop } {}

                auto get_health()       -> int { return get_field("health")->get(); }

                // Portable static accessor (works on MSVC, Clang, GCC):
                static auto get_count() -> int { return static_field("entityCount")->get(); }

                // MSVC/Clang only — deducing-this resolves to the static fallback:
                // static auto get_count() -> int { return get_field("entityCount")->get(); }
            };
    */
    template<typename derived>
    class object : public object_base
    {
    public:
        using object_base::object_base;

        // -------------------------------------------------------------------
        // Deducing-this overloads (compiled only on toolchains that support
        // C++23 explicit-object parameters: MSVC 19.32+, GCC 14+, Clang 18+).
        //
        // These exist so that a call like `get_field("staticName")` made
        // from a static C++ method *of the wrapper class* dispatches to the
        // static fallback below:  the deducing-this overloads are non-viable
        // in a static-call context (no implicit object), so on MSVC and
        // Clang overload resolution falls through to the string_view static
        // `get_field`.  From an instance method they are an exact match for
        // string literals and outrank the string_view static.
        //
        // On GCC the deducing-this overloads are still considered for
        // static-call overload resolution and produce a compile error.  We
        // still emit them because instance-context calls need a non-static
        // overload (otherwise the same-named static in this class would
        // hide the inherited `object_base::get_field`).  Users targeting
        // GCC should call `static_field("name")` explicitly from static
        // methods; that name is always available below.
        // -------------------------------------------------------------------
#if VMHOOK_HAS_DEDUCING_THIS
        auto get_field(this object_base const& self, char const* name)
            -> std::optional<vmhook::field_proxy>
        {
            return self.object_base::get_field(name);
        }

        auto get_method(this object_base const& self, char const* name)
            -> std::optional<vmhook::method_proxy>
        {
            return self.object_base::get_method(name);
        }

        auto get_method(this object_base const& self, char const* name, char const* signature)
            -> std::optional<vmhook::method_proxy>
        {
            return self.object_base::get_method(name, signature);
        }
#else
        // Pre-C++23 fallback: forward via the inherited non-static overloads.
        // Brought in with using-declarations.  These cover instance-context
        // get_field("name") / get_method("name") on compilers that don't
        // support deducing-this.  The same-name static overloads further
        // below add the type_index-based static-field lookup; both are in
        // scope, and overload resolution distinguishes them via the
        // implicit object parameter.
        using object_base::get_field;
        using object_base::get_method;
#endif

        // -------------------------------------------------------------------
        // Static-context fallbacks for `get_field` / `get_method`.
        //
        // Only emitted when deducing-this is available; on older toolchains
        // a same-name same-signature static overload would hide the inherited
        // non-static brought in via the `using` directives above, which would
        // break instance-context access.  On those compilers users still
        // call `static_field` / `static_method` (always available below).
        // -------------------------------------------------------------------
#if VMHOOK_HAS_DEDUCING_THIS
        static auto get_field(std::string_view name)
            -> std::optional<vmhook::field_proxy>
        {
            return object_base::get_field(std::type_index{ typeid(derived) }, name);
        }

        static auto get_method(std::string_view name)
            -> std::optional<vmhook::method_proxy>
        {
            return object_base::get_method(std::type_index{ typeid(derived) }, name);
        }

        static auto get_method(std::string_view name, std::string_view signature)
            -> std::optional<vmhook::method_proxy>
        {
            return object_base::get_method(std::type_index{ typeid(derived) }, name, signature);
        }
#endif

        /*
            @brief Explicit static field accessor.  Portable across compilers.
        */
        static auto static_field(std::string_view name)
            -> std::optional<vmhook::field_proxy>
        {
            return object_base::get_field(std::type_index{ typeid(derived) }, name);
        }

        /*
            @brief Explicit static method accessor.  Portable across compilers.
        */
        static auto static_method(std::string_view name)
            -> std::optional<vmhook::method_proxy>
        {
            return object_base::get_method(std::type_index{ typeid(derived) }, name);
        }

        /*
            @brief Explicit static method accessor with JVM descriptor.
        */
        static auto static_method(std::string_view name, std::string_view signature)
            -> std::optional<vmhook::method_proxy>
        {
            return object_base::get_method(std::type_index{ typeid(derived) }, name, signature);
        }
    };

    // --- Built-in Java collection wrappers ------------------------------------

    /*
        @brief Resolves the HotSpot klass* directly from a decoded OOP header.

        On x64 HotSpot (compressed class pointers enabled, the default):
          offset 0  : mark word (8 bytes)
          offset 8  : narrow klass (uint32_t, 4 bytes)
        The narrow klass is decoded the same way as regular narrow oops but using
        the klass base/shift instead of the oop base/shift.

        Returns nullptr if the pointer is invalid or decoding fails.
    */
    inline auto klass_from_oop(void* const oop) noexcept -> vmhook::hotspot::klass*
    {
        if (!oop || !vmhook::hotspot::is_valid_pointer(oop))
        {
            return nullptr;
        }
        const std::uint32_t narrow{ *reinterpret_cast<const std::uint32_t*>(
            reinterpret_cast<const std::uint8_t*>(oop) + 8) };
        void* const decoded{ vmhook::hotspot::decode_klass_pointer(narrow) };
        if (!decoded || !vmhook::hotspot::is_valid_pointer(decoded))
        {
            return nullptr;
        }
        return reinterpret_cast<vmhook::hotspot::klass*>(decoded);
    }

    // Forward declarations of the templated walk helpers used by
    // collection::to_vector below.  The bodies live further down in this
    // header (after the wrapper classes) so they can be reused by every
    // wrapper that inherits from collection.  Declaring them up here lets
    // GCC's first-phase lookup find the qualified names inside the
    // templated collection::to_vector body.
    template<typename element_type, typename out_t>
    inline auto linked_list_walk_items(void* list_oop, std::int32_t size, out_t& out) -> void;
    template<typename key_type, typename value_type, typename out_t>
    inline auto hash_map_walk_entries(void* map_oop, out_t& out) -> void;
    template<typename element_type, typename out_t>
    inline auto hash_map_walk_keys(void* map_oop, out_t& out) -> void;
    template<typename key_type, typename value_type, typename out_t>
    inline auto tree_map_walk_entries(void* map_oop, out_t& out) -> void;
    template<typename element_type, typename out_t>
    inline auto tree_map_walk_keys(void* map_oop, out_t& out) -> void;

    /*
        @brief C++ wrapper for java.util.Collection objects.

        Uses the live OOP's klass pointer (read from its object header) to
        resolve fields and methods, so no register_class<T>() call is needed
        and it works with any concrete Collection implementation
        (ArrayList, LinkedList, HashSet, etc.).

        Typical usage (from a hook detour where you received a List OOP):

            collection col{ list_oop };
            std::int32_t n = col.size();
    */
    class collection : public vmhook::object_base
    {
    public:
        /*
            @brief Wraps a decoded OOP that refers to any java.util.Collection implementation.
            @details
            The oop must be a fully decoded 64-bit heap pointer (not a compressed OOP).
            The constructor does not validate the pointer; pass nullptr for a null-safe
            collection that returns empty/zero for all operations.

            Exception safety: noexcept.
            @param oop  Decoded OOP of the Java Collection object, or nullptr.
        */
        explicit collection(vmhook::oop_t oop) noexcept
            : vmhook::object_base{ oop }
        {
        }

    protected:
        /*
            @brief Returns the klass by reading the narrow klass slot in the OOP header.
            Falls back to nullptr if the OOP is invalid.
        */
        auto oop_klass() const noexcept -> vmhook::hotspot::klass*
        {
            return vmhook::klass_from_oop(this->instance);
        }

        /*
            @brief get_field variant that uses the live OOP's klass, not the C++ type registry.
        */
        auto get_field_by_oop_klass(const std::string_view name) const
            -> std::optional<vmhook::field_proxy>
        {
            vmhook::hotspot::klass* const k{ oop_klass() };
            if (!k)
            {
                return std::nullopt;
            }
            const auto entry{ vmhook::find_field(k, name) };
            if (!entry)
            {
                return std::nullopt;
            }
            if (entry->is_static)
            {
                // Inherited static: use the declaring klass's mirror (see the
                // note on field_entry_t::declaring_klass).  `k` here is the live
                // OOP's leaf klass; for a static inherited from a super the
                // offset only indexes the super's mirror.
                vmhook::hotspot::klass* const mirror_klass{ entry->declaring_klass ? entry->declaring_klass : k };
                void* const mirror{ mirror_klass->get_java_mirror() };
                if (!mirror || !vmhook::hotspot::is_valid_pointer(mirror))
                {
                    return std::nullopt;
                }
                void* const field_pointer{ reinterpret_cast<std::uint8_t*>(mirror) + entry->offset };
                return vmhook::field_proxy{ field_pointer, entry->signature, true };
            }
            if (!this->instance)
            {
                return std::nullopt;
            }
            void* const field_pointer{ reinterpret_cast<std::uint8_t*>(this->instance) + entry->offset };
            return vmhook::field_proxy{ field_pointer, entry->signature, false };
        }

        /*
            @brief get_method variant that searches the live OOP's klass hierarchy.
        */
        auto get_method_by_oop_klass(const std::string_view method_name) const
            -> std::optional<vmhook::method_proxy>
        {
            for (vmhook::hotspot::klass* k{ oop_klass() }; k != nullptr; k = k->get_super())
            {
                const std::int32_t method_count{ k->get_methods_count() };
                vmhook::hotspot::method** const methods_array{ k->get_methods_ptr() };
                if (!methods_array || method_count <= 0)
                {
                    continue;
                }
                for (std::int32_t i{ 0 }; i < method_count; ++i)
                {
                    vmhook::hotspot::method* const m{ methods_array[i] };
                    if (m && vmhook::hotspot::is_valid_pointer(m) && m->get_name() == method_name)
                    {
                        return vmhook::method_proxy{ this->instance, m, m->get_signature() };
                    }
                }
            }
            return std::nullopt;
        }

    public:
        /*
            @brief Returns the number of elements via the Java size() method.
            Resolves through the concrete class's virtual dispatch table,
            so it works for ArrayList, LinkedList, HashSet, etc.
        */
        auto size() const noexcept -> std::int32_t
        {
            const auto proxy{ get_method_by_oop_klass("size") };
            if (!proxy)
            {
                return 0;
            }
            return proxy->call();
        }

        /*
            @brief Returns true if the collection contains no elements.
            @details
            Delegates to size(); returns true when size() == 0.

            Complexity: O(1) if size() is O(1) for the underlying implementation.
            Exception safety: noexcept.
        */
        auto is_empty() const noexcept -> bool
        {
            return size() == 0;
        }

        /*
            @brief Converts this java.util.Collection to a std::vector<std::unique_ptr<element_type>>.
            @details
            Picks the right fast path by probing the live OOP for the field
            shape of each known container, in order of specificity:

              1. ArrayList     ("elementData" + "size")  → direct array walk
              2. LinkedList    ("first" + "size")        → Node first->next chain
              3. HashSet       ("map")                   → walk backing HashMap.table keys
              4. TreeSet       ("m")                     → walk backing TreeMap.root keys
              5. Generic       (size() + get(int))       → only valid for List impls

            The cascade is field-presence based, so it picks the right path
            from the runtime klass without needing the caller to know which
            wrapper type (collection / list / linked_list / set) to construct.
            That means `field_proxy::get().to_vector<T>()` does the right
            thing for ArrayList, LinkedList, HashSet, LinkedHashSet, TreeSet,
            and any other Collection whose layout matches one of these.

            Null Java elements become nullptr entries in the returned vector.

            Complexity: O(N) where N = collection size.
            Exception safety: does not throw; returns an empty vector on any failure.

            @tparam element_type  C++ wrapper whose constructor accepts vmhook::oop_t.
            @return  Vector of unique_ptr<element_type>; nullptr slots for null elements.
        */
        template<typename element_type>
        auto to_vector() const -> std::vector<std::unique_ptr<element_type>>
        {
            std::vector<std::unique_ptr<element_type>> result;

            if (!instance || !vmhook::hotspot::is_valid_pointer(instance))
            {
                return result;
            }

            //  ArrayList fast path
            const auto size_opt{ get_field_by_oop_klass("size") };
            const auto data_opt{ get_field_by_oop_klass("elementData") };

            if (size_opt && data_opt)
            {
                const std::int32_t n{ size_opt->get() };
                if (n <= 0)
                {
                    return result;
                }

                const std::uint32_t compressed_array{ static_cast<std::uint32_t>(data_opt->get()) };
                void* const array_oop{ vmhook::decode_array_oop(compressed_array) };
                if (array_oop && vmhook::hotspot::is_valid_pointer(array_oop))
                {
                    result.reserve(static_cast<std::size_t>(n));
                    for (std::int32_t index{ 0 }; index < n; ++index)
                    {
                        const std::uint32_t compressed_element{ vmhook::get_array_element<std::uint32_t>(array_oop, index) };
                        void* const element_oop{ vmhook::hotspot::decode_oop_pointer(compressed_element) };
                        if (element_oop && vmhook::hotspot::is_valid_pointer(element_oop))
                        {
                            result.push_back(std::make_unique<element_type>(static_cast<vmhook::oop_t>(element_oop)));
                        }
                        else
                        {
                            result.push_back(nullptr);
                        }
                    }
                    return result;
                }
            }

            //  LinkedList fast path — walks the first->next Node chain.
            const auto first_opt{ get_field_by_oop_klass("first") };
            if (first_opt && size_opt)
            {
                const std::int32_t n{ size_opt->get() };
                if (n > 0)
                {
                    vmhook::linked_list_walk_items<element_type>(this->instance, n, result);
                }
                return result;
            }

            //  HashSet / LinkedHashSet fast path — elements live as keys in
            //  the backing HashMap held in field "map".
            if (const auto map_field{ get_field_by_oop_klass("map") })
            {
                const std::uint32_t map_compressed{ static_cast<std::uint32_t>(map_field->get()) };
                void* const map_oop{ vmhook::hotspot::decode_oop_pointer(map_compressed) };
                if (map_oop && vmhook::hotspot::is_valid_pointer(map_oop))
                {
                    vmhook::hash_map_walk_keys<element_type>(map_oop, result);
                }
                return result;
            }

            //  TreeSet fast path — elements live as keys in the backing
            //  NavigableMap (TreeMap) held in field "m".
            if (const auto tree_field{ get_field_by_oop_klass("m") })
            {
                const std::uint32_t tree_compressed{ static_cast<std::uint32_t>(tree_field->get()) };
                void* const tree_oop{ vmhook::hotspot::decode_oop_pointer(tree_compressed) };
                if (tree_oop && vmhook::hotspot::is_valid_pointer(tree_oop))
                {
                    vmhook::tree_map_walk_keys<element_type>(tree_oop, result);
                }
                return result;
            }

            //  Generic fallback — only valid for List impls (Set has no get(int)).
            const std::int32_t n{ size() };
            if (n <= 0)
            {
                return result;
            }

            const auto get_method_opt{ get_method_by_oop_klass("get") };
            if (!get_method_opt)
            {
                return result;
            }

            result.reserve(static_cast<std::size_t>(n));
            for (std::int32_t index{ 0 }; index < n; ++index)
            {
                const auto element_value{ get_method_opt->call<std::uint32_t>(index) };
                void* const element_oop{ vmhook::hotspot::decode_oop_pointer(element_value) };
                if (element_oop && vmhook::hotspot::is_valid_pointer(element_oop))
                {
                    result.push_back(std::make_unique<element_type>(static_cast<vmhook::oop_t>(element_oop)));
                }
                else
                {
                    result.push_back(nullptr);
                }
            }

            return result;
        }
    };

    /*
        @brief C++ wrapper for java.util.List objects.

        Type-tag for fields you know are Lists.  All container dispatch
        already happens inside collection::to_vector<T>() (ArrayList,
        LinkedList, HashSet, TreeSet fast paths plus the get(int)
        fallback), so this class only exists so callers can express
        intent — e.g. taking `const std::unique_ptr<vmhook::list>&` as a
        hook detour parameter.

        Usage:
            // Java: public List<A> listOfAs;
            auto vec = get_field("listOfAs")->get().to_vector<a_class>();
            // vec is std::vector<std::unique_ptr<a_class>>
    */
    class list : public vmhook::collection
    {
    public:
        explicit list(vmhook::oop_t oop) noexcept
            : vmhook::collection{ oop }
        {
        }
    };

    /*
        @brief C++ wrapper for java.util.Set objects.

        Type-tag, same role as vmhook::list.  HashSet / LinkedHashSet /
        TreeSet fast paths are already part of collection::to_vector, so
        a Set field reached through `field_proxy::get().to_vector<T>()`
        already takes the right path.

        Usage:
            // Java: public Set<A> setOfAs;
            auto vec = get_field("setOfAs")->get().to_vector<a_class>();
    */
    class set : public vmhook::collection
    {
    public:
        explicit set(vmhook::oop_t oop) noexcept
            : vmhook::collection{ oop }
        {
        }
    };

    /*
        @brief C++ wrapper for java.util.LinkedList objects.

        Type-tag, same role as vmhook::list.  The Node first->next walk
        lives in collection::to_vector, so `get().to_vector<T>()` runs
        in O(N) regardless of which wrapper the field signature happens
        to bind to.

        Usage:
            // Java: public LinkedList<A> chainOfAs;
            auto vec = get_field("chainOfAs")->get().to_vector<a_class>();
    */
    class linked_list : public vmhook::list
    {
    public:
        explicit linked_list(vmhook::oop_t oop) noexcept
            : vmhook::list{ oop }
        {
        }
    };

    /*
        @brief C++ wrapper for java.util.Map objects.

        Generic base for Map implementations.  Provides size()/is_empty() via
        virtual dispatch on the Java side and a to_entries<K,V>() that tries
        the HashMap "table" fast path and the TreeMap "root" red-black walk
        before giving up.  Works for HashMap, LinkedHashMap, TreeMap and the
        Set wrappers that delegate to them.

        Usage (from a hook detour where you received a Map OOP):

            map m{ map_oop };
            auto entries = m.to_entries<key_wrapper, value_wrapper>();
    */
    class map : public vmhook::object_base
    {
    public:
        /*
            @brief Wraps a decoded OOP that refers to any java.util.Map implementation.
            @param oop  Decoded OOP of the Java Map object, or nullptr.
        */
        explicit map(vmhook::oop_t oop) noexcept
            : vmhook::object_base{ oop }
        {
        }

    protected:
        /*
            @brief Returns the klass by reading the narrow klass slot in the OOP header.
        */
        auto oop_klass() const noexcept -> vmhook::hotspot::klass*
        {
            return vmhook::klass_from_oop(this->instance);
        }

        /*
            @brief get_field variant that uses the live OOP's klass, not the C++ type registry.
            @details
            Mirrors vmhook::collection::get_field_by_oop_klass — duplicated here so map
            does not need to share a base with collection (Map is not a Collection in Java).
        */
        auto get_field_by_oop_klass(const std::string_view name) const
            -> std::optional<vmhook::field_proxy>
        {
            vmhook::hotspot::klass* const k{ oop_klass() };
            if (!k)
            {
                return std::nullopt;
            }
            const auto entry{ vmhook::find_field(k, name) };
            if (!entry)
            {
                return std::nullopt;
            }
            if (entry->is_static)
            {
                // Inherited static: use the declaring klass's mirror (see the
                // note on field_entry_t::declaring_klass).  `k` here is the live
                // OOP's leaf klass; for a static inherited from a super the
                // offset only indexes the super's mirror.
                vmhook::hotspot::klass* const mirror_klass{ entry->declaring_klass ? entry->declaring_klass : k };
                void* const mirror{ mirror_klass->get_java_mirror() };
                if (!mirror || !vmhook::hotspot::is_valid_pointer(mirror))
                {
                    return std::nullopt;
                }
                void* const field_pointer{ reinterpret_cast<std::uint8_t*>(mirror) + entry->offset };
                return vmhook::field_proxy{ field_pointer, entry->signature, true };
            }
            if (!this->instance)
            {
                return std::nullopt;
            }
            void* const field_pointer{ reinterpret_cast<std::uint8_t*>(this->instance) + entry->offset };
            return vmhook::field_proxy{ field_pointer, entry->signature, false };
        }

        /*
            @brief get_method variant that searches the live OOP's klass hierarchy.
        */
        auto get_method_by_oop_klass(const std::string_view method_name) const
            -> std::optional<vmhook::method_proxy>
        {
            for (vmhook::hotspot::klass* k{ oop_klass() }; k != nullptr; k = k->get_super())
            {
                const std::int32_t method_count{ k->get_methods_count() };
                vmhook::hotspot::method** const methods_array{ k->get_methods_ptr() };
                if (!methods_array || method_count <= 0)
                {
                    continue;
                }
                for (std::int32_t i{ 0 }; i < method_count; ++i)
                {
                    vmhook::hotspot::method* const m{ methods_array[i] };
                    if (m && vmhook::hotspot::is_valid_pointer(m) && m->get_name() == method_name)
                    {
                        return vmhook::method_proxy{ this->instance, m, m->get_signature() };
                    }
                }
            }
            return std::nullopt;
        }

    public:
        /*
            @brief Returns the number of entries via the Java size() method.
        */
        auto size() const noexcept -> std::int32_t
        {
            const auto proxy{ get_method_by_oop_klass("size") };
            if (!proxy)
            {
                // Fall back to the "size" field, present on every JDK Map impl.
                if (const auto size_field{ get_field_by_oop_klass("size") })
                {
                    return size_field->get();
                }
                return 0;
            }
            return proxy->call();
        }

        /*
            @brief Returns true if the map contains no entries.
        */
        auto is_empty() const noexcept -> bool
        {
            return size() == 0;
        }

        /*
            @brief Converts this java.util.Map to a vector of unique_ptr pairs.
            @details
            Tries the HashMap "table" Node[] walk first (covers HashMap and
            LinkedHashMap), then the TreeMap "root" red-black walk (covers
            TreeMap), and finally returns an empty vector if neither layout
            is present.  Iterator-based fallback would require multiple Java
            call-gate dispatches per entry; leave that to callers that opt in.

            Null keys/values become nullptr entries in the returned pairs.

            @tparam key_type    C++ wrapper for the Map key.
            @tparam value_type  C++ wrapper for the Map value.
        */
        template<typename key_type, typename value_type>
        auto to_entries() const
            -> std::vector<std::pair<std::unique_ptr<key_type>, std::unique_ptr<value_type>>>
        {
            std::vector<std::pair<std::unique_ptr<key_type>, std::unique_ptr<value_type>>> result;
            if (!instance || !vmhook::hotspot::is_valid_pointer(instance))
            {
                return result;
            }

            // HashMap / LinkedHashMap fast path
            if (get_field_by_oop_klass("table"))
            {
                vmhook::hash_map_walk_entries<key_type, value_type>(instance, result);
                return result;
            }

            // TreeMap fast path
            if (get_field_by_oop_klass("root"))
            {
                vmhook::tree_map_walk_entries<key_type, value_type>(instance, result);
                return result;
            }

            return result;
        }
    };

    /*
        @brief C++ wrapper for java.util.HashMap objects.

        Type-tag, same role as vmhook::list.  All entry-walking logic
        already lives in vmhook::map::to_entries (HashMap "table" path
        then TreeMap "root" path), so `get_field("foo")->get()
        .to_entries<K, V>()` already does the right thing.  This class
        exists so callers can declare intent — e.g. a detour parameter
        of type `const std::unique_ptr<vmhook::hash_map>&`.  Covers
        LinkedHashMap by inheritance (LinkedHashMap reuses
        HashMap.table; only the iteration order differs, via
        Node.before / Node.after).
    */
    class hash_map : public vmhook::map
    {
    public:
        explicit hash_map(vmhook::oop_t oop) noexcept
            : vmhook::map{ oop }
        {
        }
    };

    // --- Collection / Map / Set walk helpers --------------------------------
    //
    // Templated free helpers used by the wrappers above.  Defined out-of-line
    // so they can reference vmhook::map (complete by this point) and so the
    // collection / set / map wrappers can all reuse the same traversal code
    // without depending on each other's identity.

    /*
        @brief Walks a LinkedList's first->next Node chain and appends items.
        @details
        Reads the Node klass once per node (HotSpot guarantees every node
        in a single list shares the same concrete klass), then offsets into
        each node by the cached "item" and "next" field offsets.  The `size`
        argument bounds the walk so a corrupt or concurrently-mutated chain
        cannot loop forever; null `item`s become nullptr entries in `out`.
    */
    template<typename element_type, typename out_t>
    inline auto linked_list_walk_items(void* const list_oop, const std::int32_t size, out_t& out) -> void
    {
        if (!list_oop || !vmhook::hotspot::is_valid_pointer(list_oop) || size <= 0)
        {
            return;
        }
        vmhook::hotspot::klass* const list_klass{ vmhook::klass_from_oop(list_oop) };
        if (!list_klass)
        {
            return;
        }
        const auto first_entry{ vmhook::find_field(list_klass, "first") };
        if (!first_entry)
        {
            return;
        }
        const std::uint32_t first_compressed{
            *reinterpret_cast<const std::uint32_t*>(
                reinterpret_cast<const std::uint8_t*>(list_oop) + first_entry->offset) };
        void* node_oop{ vmhook::hotspot::decode_oop_pointer(first_compressed) };

        out.reserve(static_cast<std::size_t>(size));
        for (std::int32_t i{ 0 };
             i < size && node_oop && vmhook::hotspot::is_valid_pointer(node_oop);
             ++i)
        {
            vmhook::hotspot::klass* const node_klass{ vmhook::klass_from_oop(node_oop) };
            if (!node_klass)
            {
                break;
            }
            const auto item_entry{ vmhook::find_field(node_klass, "item") };
            const auto next_entry{ vmhook::find_field(node_klass, "next") };
            if (!item_entry || !next_entry)
            {
                break;
            }
            const std::uint32_t item_compressed{
                *reinterpret_cast<const std::uint32_t*>(
                    reinterpret_cast<const std::uint8_t*>(node_oop) + item_entry->offset) };
            void* const item_oop{ vmhook::hotspot::decode_oop_pointer(item_compressed) };
            if (item_oop && vmhook::hotspot::is_valid_pointer(item_oop))
            {
                out.push_back(std::make_unique<element_type>(static_cast<vmhook::oop_t>(item_oop)));
            }
            else
            {
                out.push_back(nullptr);
            }

            const std::uint32_t next_compressed{
                *reinterpret_cast<const std::uint32_t*>(
                    reinterpret_cast<const std::uint8_t*>(node_oop) + next_entry->offset) };
            node_oop = vmhook::hotspot::decode_oop_pointer(next_compressed);
        }
    }

    /*
        @brief Walks a HashMap's "table" Node[] and appends entries to `out`.
        @details
        Each bucket holds either nullptr, a HashMap$Node, or a HashMap$TreeNode
        (when a bucket has been treeified for collision resistance).  Both
        Node and TreeNode expose the same `key` / `value` / `next` fields, so
        following `next` from the bucket head visits every entry in the bucket
        regardless of which form it took.
    */
    template<typename key_type, typename value_type, typename out_t>
    inline auto hash_map_walk_entries(void* const map_oop, out_t& out) -> void
    {
        if (!map_oop || !vmhook::hotspot::is_valid_pointer(map_oop))
        {
            return;
        }
        vmhook::hotspot::klass* const map_klass{ vmhook::klass_from_oop(map_oop) };
        if (!map_klass)
        {
            return;
        }
        const auto table_entry{ vmhook::find_field(map_klass, "table") };
        if (!table_entry)
        {
            return;
        }
        const std::uint32_t table_compressed{
            *reinterpret_cast<const std::uint32_t*>(
                reinterpret_cast<const std::uint8_t*>(map_oop) + table_entry->offset) };
        void* const table_oop{ vmhook::decode_array_oop(table_compressed) };
        if (!table_oop || !vmhook::hotspot::is_valid_pointer(table_oop))
        {
            return;
        }

        const std::int32_t bucket_count{ vmhook::array_length(table_oop) };
        for (std::int32_t bucket{ 0 }; bucket < bucket_count; ++bucket)
        {
            const std::uint32_t head_compressed{
                vmhook::get_array_element<std::uint32_t>(table_oop, bucket) };
            void* node_oop{ vmhook::hotspot::decode_oop_pointer(head_compressed) };

            // Bound the per-bucket walk to defend against corrupt chains;
            // 1<<20 is far above any sane bucket length.
            for (std::int32_t guard{ 0 };
                 node_oop && vmhook::hotspot::is_valid_pointer(node_oop) && guard < (1 << 20);
                 ++guard)
            {
                vmhook::hotspot::klass* const node_klass{ vmhook::klass_from_oop(node_oop) };
                if (!node_klass)
                {
                    break;
                }
                const auto key_entry  { vmhook::find_field(node_klass, "key") };
                const auto value_entry{ vmhook::find_field(node_klass, "value") };
                const auto next_entry { vmhook::find_field(node_klass, "next") };
                if (!key_entry || !value_entry || !next_entry)
                {
                    break;
                }

                const std::uint32_t key_compressed{
                    *reinterpret_cast<const std::uint32_t*>(
                        reinterpret_cast<const std::uint8_t*>(node_oop) + key_entry->offset) };
                const std::uint32_t value_compressed{
                    *reinterpret_cast<const std::uint32_t*>(
                        reinterpret_cast<const std::uint8_t*>(node_oop) + value_entry->offset) };

                void* const key_oop  { vmhook::hotspot::decode_oop_pointer(key_compressed) };
                void* const value_oop{ vmhook::hotspot::decode_oop_pointer(value_compressed) };

                std::unique_ptr<key_type>   k_ptr;
                std::unique_ptr<value_type> v_ptr;
                if (key_oop && vmhook::hotspot::is_valid_pointer(key_oop))
                {
                    k_ptr = std::make_unique<key_type>(static_cast<vmhook::oop_t>(key_oop));
                }
                if (value_oop && vmhook::hotspot::is_valid_pointer(value_oop))
                {
                    v_ptr = std::make_unique<value_type>(static_cast<vmhook::oop_t>(value_oop));
                }
                out.emplace_back(std::move(k_ptr), std::move(v_ptr));

                const std::uint32_t next_compressed{
                    *reinterpret_cast<const std::uint32_t*>(
                        reinterpret_cast<const std::uint8_t*>(node_oop) + next_entry->offset) };
                node_oop = vmhook::hotspot::decode_oop_pointer(next_compressed);
            }
        }
    }

    /*
        @brief Walks a HashMap's table and appends each entry's key to `out`.
        Used by vmhook::set::to_vector for HashSet, whose elements are keys.
    */
    template<typename element_type, typename out_t>
    inline auto hash_map_walk_keys(void* const map_oop, out_t& out) -> void
    {
        if (!map_oop || !vmhook::hotspot::is_valid_pointer(map_oop))
        {
            return;
        }
        vmhook::hotspot::klass* const map_klass{ vmhook::klass_from_oop(map_oop) };
        if (!map_klass)
        {
            return;
        }
        const auto table_entry{ vmhook::find_field(map_klass, "table") };
        if (!table_entry)
        {
            return;
        }
        const std::uint32_t table_compressed{
            *reinterpret_cast<const std::uint32_t*>(
                reinterpret_cast<const std::uint8_t*>(map_oop) + table_entry->offset) };
        void* const table_oop{ vmhook::decode_array_oop(table_compressed) };
        if (!table_oop || !vmhook::hotspot::is_valid_pointer(table_oop))
        {
            return;
        }

        const std::int32_t bucket_count{ vmhook::array_length(table_oop) };
        for (std::int32_t bucket{ 0 }; bucket < bucket_count; ++bucket)
        {
            const std::uint32_t head_compressed{
                vmhook::get_array_element<std::uint32_t>(table_oop, bucket) };
            void* node_oop{ vmhook::hotspot::decode_oop_pointer(head_compressed) };

            for (std::int32_t guard{ 0 };
                 node_oop && vmhook::hotspot::is_valid_pointer(node_oop) && guard < (1 << 20);
                 ++guard)
            {
                vmhook::hotspot::klass* const node_klass{ vmhook::klass_from_oop(node_oop) };
                if (!node_klass)
                {
                    break;
                }
                const auto key_entry { vmhook::find_field(node_klass, "key") };
                const auto next_entry{ vmhook::find_field(node_klass, "next") };
                if (!key_entry || !next_entry)
                {
                    break;
                }
                const std::uint32_t key_compressed{
                    *reinterpret_cast<const std::uint32_t*>(
                        reinterpret_cast<const std::uint8_t*>(node_oop) + key_entry->offset) };
                void* const key_oop{ vmhook::hotspot::decode_oop_pointer(key_compressed) };
                if (key_oop && vmhook::hotspot::is_valid_pointer(key_oop))
                {
                    out.push_back(std::make_unique<element_type>(static_cast<vmhook::oop_t>(key_oop)));
                }
                else
                {
                    out.push_back(nullptr);
                }

                const std::uint32_t next_compressed{
                    *reinterpret_cast<const std::uint32_t*>(
                        reinterpret_cast<const std::uint8_t*>(node_oop) + next_entry->offset) };
                node_oop = vmhook::hotspot::decode_oop_pointer(next_compressed);
            }
        }
    }

    /*
        @brief In-order red-black walk of a TreeMap rooted at `map_oop`.

        TreeMap.Entry layout (since Java 1.2, stable across 8/11/17/21):
          K key; V value; Entry left, right, parent; boolean color.

        Uses an iterative traversal with a small stack so deeply-balanced
        trees do not blow the C++ stack.  The HashMap-equivalent depth cap
        keeps a malformed tree from looping forever.
    */
    template<typename key_type, typename value_type, typename out_t>
    inline auto tree_map_walk_entries(void* const map_oop, out_t& out) -> void
    {
        if (!map_oop || !vmhook::hotspot::is_valid_pointer(map_oop))
        {
            return;
        }
        vmhook::hotspot::klass* const map_klass{ vmhook::klass_from_oop(map_oop) };
        if (!map_klass)
        {
            return;
        }
        const auto root_entry{ vmhook::find_field(map_klass, "root") };
        if (!root_entry)
        {
            return;
        }
        const std::uint32_t root_compressed{
            *reinterpret_cast<const std::uint32_t*>(
                reinterpret_cast<const std::uint8_t*>(map_oop) + root_entry->offset) };
        void* root_oop{ vmhook::hotspot::decode_oop_pointer(root_compressed) };
        if (!root_oop || !vmhook::hotspot::is_valid_pointer(root_oop))
        {
            return;
        }

        std::vector<void*> stack;
        stack.reserve(32);
        void* node_oop{ root_oop };
        std::int32_t visited{ 0 };

        while ((node_oop && vmhook::hotspot::is_valid_pointer(node_oop)) || !stack.empty())
        {
            // Walk the left spine first.
            while (node_oop && vmhook::hotspot::is_valid_pointer(node_oop))
            {
                vmhook::hotspot::klass* const entry_klass{ vmhook::klass_from_oop(node_oop) };
                if (!entry_klass)
                {
                    node_oop = nullptr;
                    break;
                }
                const auto left_entry{ vmhook::find_field(entry_klass, "left") };
                if (!left_entry)
                {
                    node_oop = nullptr;
                    break;
                }
                stack.push_back(node_oop);
                const std::uint32_t left_compressed{
                    *reinterpret_cast<const std::uint32_t*>(
                        reinterpret_cast<const std::uint8_t*>(node_oop) + left_entry->offset) };
                node_oop = vmhook::hotspot::decode_oop_pointer(left_compressed);
            }
            if (stack.empty())
            {
                break;
            }

            node_oop = stack.back();
            stack.pop_back();

            vmhook::hotspot::klass* const entry_klass{ vmhook::klass_from_oop(node_oop) };
            if (!entry_klass)
            {
                break;
            }
            const auto key_entry  { vmhook::find_field(entry_klass, "key") };
            const auto value_entry{ vmhook::find_field(entry_klass, "value") };
            const auto right_entry{ vmhook::find_field(entry_klass, "right") };
            if (!key_entry || !value_entry || !right_entry)
            {
                break;
            }
            const std::uint32_t key_compressed{
                *reinterpret_cast<const std::uint32_t*>(
                    reinterpret_cast<const std::uint8_t*>(node_oop) + key_entry->offset) };
            const std::uint32_t value_compressed{
                *reinterpret_cast<const std::uint32_t*>(
                    reinterpret_cast<const std::uint8_t*>(node_oop) + value_entry->offset) };

            void* const key_oop  { vmhook::hotspot::decode_oop_pointer(key_compressed) };
            void* const value_oop{ vmhook::hotspot::decode_oop_pointer(value_compressed) };

            std::unique_ptr<key_type>   k_ptr;
            std::unique_ptr<value_type> v_ptr;
            if (key_oop && vmhook::hotspot::is_valid_pointer(key_oop))
            {
                k_ptr = std::make_unique<key_type>(static_cast<vmhook::oop_t>(key_oop));
            }
            if (value_oop && vmhook::hotspot::is_valid_pointer(value_oop))
            {
                v_ptr = std::make_unique<value_type>(static_cast<vmhook::oop_t>(value_oop));
            }
            out.emplace_back(std::move(k_ptr), std::move(v_ptr));

            const std::uint32_t right_compressed{
                *reinterpret_cast<const std::uint32_t*>(
                    reinterpret_cast<const std::uint8_t*>(node_oop) + right_entry->offset) };
            node_oop = vmhook::hotspot::decode_oop_pointer(right_compressed);

            // Safety cap matching the HashMap variant.
            if (++visited > (1 << 24))
            {
                break;
            }
        }
    }

    /*
        @brief In-order walk of a TreeMap, pushing each entry's key to `out`.
        Used by vmhook::set::to_vector for TreeSet.
    */
    template<typename element_type, typename out_t>
    inline auto tree_map_walk_keys(void* const map_oop, out_t& out) -> void
    {
        if (!map_oop || !vmhook::hotspot::is_valid_pointer(map_oop))
        {
            return;
        }
        vmhook::hotspot::klass* const map_klass{ vmhook::klass_from_oop(map_oop) };
        if (!map_klass)
        {
            return;
        }
        const auto root_entry{ vmhook::find_field(map_klass, "root") };
        if (!root_entry)
        {
            return;
        }
        const std::uint32_t root_compressed{
            *reinterpret_cast<const std::uint32_t*>(
                reinterpret_cast<const std::uint8_t*>(map_oop) + root_entry->offset) };
        void* root_oop{ vmhook::hotspot::decode_oop_pointer(root_compressed) };
        if (!root_oop || !vmhook::hotspot::is_valid_pointer(root_oop))
        {
            return;
        }

        std::vector<void*> stack;
        stack.reserve(32);
        void* node_oop{ root_oop };
        std::int32_t visited{ 0 };

        while ((node_oop && vmhook::hotspot::is_valid_pointer(node_oop)) || !stack.empty())
        {
            while (node_oop && vmhook::hotspot::is_valid_pointer(node_oop))
            {
                vmhook::hotspot::klass* const entry_klass{ vmhook::klass_from_oop(node_oop) };
                if (!entry_klass)
                {
                    node_oop = nullptr;
                    break;
                }
                const auto left_entry{ vmhook::find_field(entry_klass, "left") };
                if (!left_entry)
                {
                    node_oop = nullptr;
                    break;
                }
                stack.push_back(node_oop);
                const std::uint32_t left_compressed{
                    *reinterpret_cast<const std::uint32_t*>(
                        reinterpret_cast<const std::uint8_t*>(node_oop) + left_entry->offset) };
                node_oop = vmhook::hotspot::decode_oop_pointer(left_compressed);
            }
            if (stack.empty())
            {
                break;
            }

            node_oop = stack.back();
            stack.pop_back();

            vmhook::hotspot::klass* const entry_klass{ vmhook::klass_from_oop(node_oop) };
            if (!entry_klass)
            {
                break;
            }
            const auto key_entry  { vmhook::find_field(entry_klass, "key") };
            const auto right_entry{ vmhook::find_field(entry_klass, "right") };
            if (!key_entry || !right_entry)
            {
                break;
            }
            const std::uint32_t key_compressed{
                *reinterpret_cast<const std::uint32_t*>(
                    reinterpret_cast<const std::uint8_t*>(node_oop) + key_entry->offset) };
            void* const key_oop{ vmhook::hotspot::decode_oop_pointer(key_compressed) };
            if (key_oop && vmhook::hotspot::is_valid_pointer(key_oop))
            {
                out.push_back(std::make_unique<element_type>(static_cast<vmhook::oop_t>(key_oop)));
            }
            else
            {
                out.push_back(nullptr);
            }

            const std::uint32_t right_compressed{
                *reinterpret_cast<const std::uint32_t*>(
                    reinterpret_cast<const std::uint8_t*>(node_oop) + right_entry->offset) };
            node_oop = vmhook::hotspot::decode_oop_pointer(right_compressed);

            if (++visited > (1 << 24))
            {
                break;
            }
        }
    }

    /*
        @brief Out-of-line definition of field_proxy::value_t::to_vector<element_type>().
        @details
        Reads the stored compressed OOP, decodes it with decode_oop_pointer(), wraps the
        result in a vmhook::collection, and delegates to collection::to_vector<element_type>().
        Defined here (after collection is complete) because collection is an incomplete type
        at the point where value_t is declared.

        Complexity: O(N) where N = collection size.
        Exception safety: does not throw; returns an empty vector on failure.

        @tparam element_type  C++ wrapper whose constructor accepts vmhook::oop_t.
        @return  Vector of unique_ptr<element_type>.
    */
    template<typename element_type>
    auto field_proxy::value_t::to_vector() const
        -> std::vector<std::unique_ptr<element_type>>
    {
        const std::uint32_t compressed_collection{ static_cast<std::uint32_t>(*this) };
        void* const collection_oop{ vmhook::hotspot::decode_oop_pointer(compressed_collection) };
        if (!collection_oop || !vmhook::hotspot::is_valid_pointer(collection_oop))
        {
            VMHOOK_LOG("{} field_proxy::value_t::to_vector<{}>(): underlying collection OOP is "
                       "null or invalid (compressed=0x{:08X}) - returning empty vector.",
                       vmhook::warning_tag, typeid(element_type).name(), compressed_collection);
            return {};
        }

        // An OBJECT-ARRAY field ('[L...;' or '[[...') is a raw Java array, NOT a
        // collection object.  Routing it through vmhook::collection (which probes
        // the oop's klass as an InstanceKlass for "size"/"elementData"/"first"/...
        // and finds none on an ObjArrayKlass) silently returns an EMPTY vector —
        // the documented Object[] entry point was broken.  Walk the array directly
        // here; only 'L...;' reference fields (ArrayList / HashSet / LinkedList /
        // ... collection OBJECTS) fall through to collection::to_vector.
        if (this->signature.size() >= 2u && this->signature.front() == '['
            && (this->signature[1] == 'L' || this->signature[1] == '['))
        {
            std::vector<std::unique_ptr<element_type>> result;
            const std::int32_t n{ vmhook::array_length(collection_oop) };
            if (n > 0)
            {
                result.reserve(static_cast<std::size_t>(n));
                for (std::int32_t index{ 0 }; index < n; ++index)
                {
                    const std::uint32_t compressed_element{
                        vmhook::get_array_element<std::uint32_t>(collection_oop, index) };
                    void* const element_oop{ vmhook::hotspot::decode_oop_pointer(compressed_element) };
                    if (element_oop && vmhook::hotspot::is_valid_pointer(element_oop))
                    {
                        result.push_back(std::make_unique<element_type>(static_cast<vmhook::oop_t>(element_oop)));
                    }
                    else
                    {
                        result.push_back(nullptr);
                    }
                }
            }
            return result;
        }

        return vmhook::collection{ collection_oop }.to_vector<element_type>();
    }

    /*
        @brief Out-of-line definition of field_proxy::value_t::to_entries<K,V>().
        @details
        Reads the stored compressed OOP, decodes it, wraps it in a vmhook::map,
        and delegates to map::to_entries<K,V>().  Defined here so the value_t
        declaration (which is reached well before map is defined) does not
        need to see a complete vmhook::map.
    */
    template<typename key_type, typename value_type>
    auto field_proxy::value_t::to_entries() const
        -> std::vector<std::pair<std::unique_ptr<key_type>, std::unique_ptr<value_type>>>
    {
        const std::uint32_t compressed_map{ static_cast<std::uint32_t>(*this) };
        void* const map_oop{ vmhook::hotspot::decode_oop_pointer(compressed_map) };
        if (!map_oop || !vmhook::hotspot::is_valid_pointer(map_oop))
        {
            VMHOOK_LOG("{} field_proxy::value_t::to_entries<{},{}>(): underlying map OOP is null "
                       "or invalid (compressed=0x{:08X}) - returning empty entries vector.",
                       vmhook::warning_tag, typeid(key_type).name(), typeid(value_type).name(),
                       compressed_map);
            return {};
        }
        return vmhook::map{ map_oop }.to_entries<key_type, value_type>();
    }

    // --- Helper: read a Java String OOP to std::string ------------------------

    /*
        @brief Decodes a Java String object into a std::string.
        @param string_oop  A decoded OOP pointing to a java.lang.String instance.
        @return A std::string containing the string contents, or empty on failure.
        @details
        Handles both pre-Java-9 (char[] value) and Java-9+ (byte[] value + coder) layouts.
        Truncates strings longer than 4096 characters as a sanity check.
    */
    inline auto read_java_string(void* const string_oop)
        -> std::string
    {
        if (!string_oop || !vmhook::hotspot::is_valid_pointer(string_oop))
        {
            VMHOOK_LOG("{} read_java_string(): string_oop is null or invalid (0x{:016X}).",
                       vmhook::warning_tag, reinterpret_cast<std::uintptr_t>(string_oop));
            return {};
        }

        vmhook::hotspot::klass* const string_klass{ vmhook::find_class("java/lang/String") };
        if (!string_klass)
        {
            VMHOOK_LOG("{} read_java_string(): find_class('java/lang/String') returned null - "
                       "very early bootstrap, or VMStructs+JNI both failed.",
                       vmhook::error_tag);
            return {};
        }

        const std::uint32_t arr_compressed{ vmhook::get_field<std::uint32_t>(string_oop, string_klass, "value") };
        if (!arr_compressed)
        {
            // Backing-array compressed pointer is 0 - this is the legitimate
            // state for a freshly-allocated (uninitialised) String OOP.  Don't
            // log here to avoid spamming the common case.
            return {};
        }

        void* const arr_oop{ vmhook::hotspot::decode_oop_pointer(arr_compressed) };
        if (!arr_oop || !vmhook::hotspot::is_valid_pointer(arr_oop))
        {
            VMHOOK_LOG("{} read_java_string(): backing array OOP is invalid after decompression "
                       "(compressed=0x{:08X}, decoded=0x{:016X}).",
                       vmhook::warning_tag, arr_compressed,
                       reinterpret_cast<std::uintptr_t>(arr_oop));
            return {};
        }

        const auto* const arr{ reinterpret_cast<const std::uint8_t*>(arr_oop) };
        const std::int32_t length{ *reinterpret_cast<const std::int32_t*>(arr + 12) };
        if (length <= 0 || length > 4096)
        {
            VMHOOK_LOG("{} read_java_string(): array length {} out of range (must be 1..4096) - "
                       "either an empty string or the array header is corrupt.",
                       vmhook::warning_tag, length);
            return {};
        }

        const std::uint8_t* const data{ arr + 16 };
        const bool has_coder{ string_klass->find_field("coder").has_value() };

        // Append a Unicode code point to `out` as standard UTF-8 (1-4 bytes).
        // Replaces the previous lossy "non-ASCII -> '?'" substitution that
        // silently corrupted every string containing any char >= 0x80 (and the
        // LATIN1 path that copied raw 0x80-0xFF bytes as if they were UTF-8).
        const auto append_utf8 = [](std::string& out, std::uint32_t cp) -> void
        {
            if (cp < 0x80u)
            {
                out += static_cast<char>(cp);
            }
            else if (cp < 0x800u)
            {
                out += static_cast<char>(0xC0u | (cp >> 6));
                out += static_cast<char>(0x80u | (cp & 0x3Fu));
            }
            else if (cp < 0x10000u)
            {
                out += static_cast<char>(0xE0u | (cp >> 12));
                out += static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
                out += static_cast<char>(0x80u | (cp & 0x3Fu));
            }
            else
            {
                out += static_cast<char>(0xF0u | (cp >> 18));
                out += static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu));
                out += static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
                out += static_cast<char>(0x80u | (cp & 0x3Fu));
            }
        };

        // Decode `count` native-endian UTF-16 code units (combining surrogate
        // pairs into astral code points) and append them to `out` as UTF-8.
        const auto utf16_to_utf8 = [&append_utf8](std::string& out,
                                                  const std::uint16_t* const chars,
                                                  const std::int32_t count) -> void
        {
            for (std::int32_t i{ 0 }; i < count; ++i)
            {
                std::uint32_t cp{ chars[i] };
                if (cp >= 0xD800u && cp <= 0xDBFFu && (i + 1) < count)
                {
                    const std::uint16_t low{ chars[i + 1] };
                    if (low >= 0xDC00u && low <= 0xDFFFu)
                    {
                        cp = 0x10000u + ((cp - 0xD800u) << 10) + (low - 0xDC00u);
                        ++i;
                    }
                }
                append_utf8(out, cp);
            }
        };

        std::string result;
        if (!has_coder)
        {
            // JDK 8: backing char[] is always UTF-16; `length` is the char count.
            result.reserve(static_cast<std::size_t>(length));
            utf16_to_utf8(result, reinterpret_cast<const std::uint16_t*>(data), length);
        }
        else
        {
            const std::uint8_t coder{ vmhook::get_field<std::uint8_t>(string_oop, string_klass, "coder") };
            if (coder == 0)
            {
                // JDK 9+ LATIN1: one byte per char, each a code point in 0..255.
                // UTF-8-encode each (so 0xE9 'é' -> C3 A9, not a raw invalid byte).
                result.reserve(static_cast<std::size_t>(length));
                for (std::int32_t i{ 0 }; i < length; ++i)
                {
                    append_utf8(result, data[i]);
                }
            }
            else
            {
                // JDK 9+ UTF16: `length` is the byte[] length == 2 * char count.
                const std::int32_t char_count{ length / 2 };
                result.reserve(static_cast<std::size_t>(char_count));
                utf16_to_utf8(result, reinterpret_cast<const std::uint16_t*>(data), char_count);
            }
        }
        return result;
    }

    /*
        @brief Returns the decoded array or object OOP from a field_proxy.
        @details
        Reads the compressed OOP stored in field via get_compressed_oop() and decodes
        it using decode_array_oop().  Convenience wrapper used by set_str_field(),
        set_bool_array(), set_prim_array(), and set_str_array().

        Complexity: O(1).
        Exception safety: noexcept — returns nullptr if the field OOP is null or invalid.

        @param field  A field_proxy whose underlying field holds a reference or array OOP.
        @return  Decoded 64-bit heap pointer to the array/object, or nullptr on failure.
    */
    inline auto field_oop(const vmhook::field_proxy& field) noexcept
        -> void*
    {
        return vmhook::decode_array_oop(field.get_compressed_oop());
    }

    /*
        @brief Overwrites the contents of an existing java.lang.String in-place.
        @details
        Reads the backing array OOP from the String's "value" field, then writes
        value's bytes into the backing array element-by-element:
          - char[] (JDK 8):  writes uint16 per character (narrow to 0x00FF).
          - byte[] (JDK 9+): writes uint8 per character (LATIN1 coder assumed).
        Writes only up to min(existing_length, value.length()) characters.
        Does NOT change the array length; value is silently truncated if longer
        than the existing backing array.

        Complexity: O(N) where N = written character count.
        Exception safety: noexcept — returns on any failure.

        @param string_oop  Decoded OOP of a live java.lang.String instance.
        @param value       New content to write; truncated to the existing array length.
    */
    inline auto write_java_string(void* const string_oop, const std::string_view value) noexcept
        -> void
    {
        if (!string_oop || !vmhook::hotspot::is_valid_pointer(string_oop))
        {
            return;
        }

        vmhook::hotspot::klass* const string_klass{ vmhook::find_class("java/lang/String") };
        if (!string_klass)
        {
            return;
        }

        const auto value_field{ vmhook::find_field(string_klass, "value") };
        if (!value_field)
        {
            return;
        }

        std::uint32_t compressed{};
        std::memcpy(&compressed, reinterpret_cast<const std::uint8_t*>(string_oop) + value_field->offset, sizeof(compressed));
        void* const array_oop{ vmhook::decode_array_oop(compressed) };
        if (!array_oop)
        {
            return;
        }

        const std::int32_t length{ vmhook::array_length(array_oop) };
        const std::int32_t writable_length{ (std::min)(length, static_cast<std::int32_t>(value.size())) };
        if (writable_length <= 0)
        {
            return;
        }

        if (value_field->signature == "[C")
        {
            for (std::int32_t index{ 0 }; index < writable_length; ++index)
            {
                vmhook::set_array_element<std::uint16_t>(array_oop, index, static_cast<std::uint16_t>(static_cast<unsigned char>(value[static_cast<std::size_t>(index)])));
            }
        }
        else
        {
            for (std::int32_t index{ 0 }; index < writable_length; ++index)
            {
                vmhook::set_array_element<std::uint8_t>(array_oop, index, static_cast<std::uint8_t>(value[static_cast<std::size_t>(index)]));
            }
        }
    }

    /*
        @brief Overwrites a String field's backing array content in-place.
        @details
        Decodes the field's OOP to obtain the backing java.lang.String object, then
        delegates to write_java_string().  Convenience wrapper for the common pattern
        of mutating a String-typed instance field without replacing the String reference.

        Complexity: O(N) where N = written character count.
        Exception safety: noexcept — returns silently on failure.

        @param field  A field_proxy whose underlying field is a java.lang.String reference.
        @param value  New string content; truncated to the existing array length.
    */
    inline auto set_str_field(const vmhook::field_proxy& field, const std::string_view value) noexcept
        -> void
    {
        vmhook::write_java_string(vmhook::field_oop(field), value);
    }

    /*
        @brief Writes a std::vector<bool> into a Java boolean[] field.
        @details
        Decodes the field's array OOP via field_oop(), then writes each element
        as uint8 (0 = false, 1 = true) into the backing boolean[] array.
        Writes only min(array_length, values.size()) elements.

        Complexity: O(N) where N = written element count.
        Exception safety: noexcept — returns silently if the OOP is null.

        @param field   A field_proxy whose field holds a boolean[] reference.
        @param values  Boolean values to write into the array.
    */
    inline auto set_bool_array(const vmhook::field_proxy& field, const std::vector<bool>& values) noexcept
        -> void
    {
        void* const array_oop{ vmhook::field_oop(field) };
        if (!array_oop)
        {
            return;
        }

        const std::int32_t length{ (std::min)(vmhook::array_length(array_oop), static_cast<std::int32_t>(values.size())) };
        for (std::int32_t index{ 0 }; index < length; ++index)
        {
            vmhook::set_array_element<std::uint8_t>(array_oop, index, values[static_cast<std::size_t>(index)] ? 1u : 0u);
        }
    }

    /*
        @brief Writes a std::vector<element_type> into a Java primitive array field.
        @details
        Decodes the field's array OOP, then writes each element into the backing Java
        primitive array.  Special-cases char/uint16 conversion for "[C" (Java char[])
        fields: narrows each char to uint16 to avoid sign-extension issues.
        Writes only min(array_length, values.size()) elements.

        Complexity: O(N) where N = written element count.
        Exception safety: noexcept — returns silently if the OOP is null.

        @tparam element_type  Trivially copyable C++ element type (int, float, etc.).
        @param field   A field_proxy whose field holds a primitive array reference.
        @param values  Values to write into the array.
    */
    template<typename element_type>
    inline auto set_prim_array(const vmhook::field_proxy& field, const std::vector<element_type>& values) noexcept
        -> void
    {
        void* const array_oop{ vmhook::field_oop(field) };
        if (!array_oop)
        {
            return;
        }

        const std::int32_t length{ (std::min)(vmhook::array_length(array_oop), static_cast<std::int32_t>(values.size())) };
        for (std::int32_t index{ 0 }; index < length; ++index)
        {
            const element_type& value{ values[static_cast<std::size_t>(index)] };
            if constexpr (std::is_same_v<element_type, char>)
            {
                if (field.signature() == "[C")
                {
                    vmhook::set_array_element<std::uint16_t>(array_oop, index, static_cast<std::uint16_t>(static_cast<unsigned char>(value)));
                }
                else
                {
                    vmhook::set_array_element<char>(array_oop, index, value);
                }
            }
            else
            {
                vmhook::set_array_element<element_type>(array_oop, index, value);
            }
        }
    }

    /*
        @brief Writes a std::vector<std::string> into a Java String[] field in-place.
        @details
        Decodes the field's array OOP, then for each slot reads the existing compressed
        OOP (the java.lang.String* in the array), decodes it, and overwrites the backing
        char[]/byte[] via write_java_string().  Does NOT replace the String references in
        the array; it mutates the content of each pre-existing String object.
        Writes only min(array_length, values.size()) elements.

        Complexity: O(N * S) where N = element count, S = average string length.
        Exception safety: noexcept — returns silently if the OOP is null.

        @param field   A field_proxy whose field holds a String[] reference.
        @param values  New string contents for each slot.
    */
    inline auto set_str_array(const vmhook::field_proxy& field, const std::vector<std::string>& values) noexcept
        -> void
    {
        void* const array_oop{ vmhook::field_oop(field) };
        if (!array_oop)
        {
            return;
        }

        const std::int32_t length{ (std::min)(vmhook::array_length(array_oop), static_cast<std::int32_t>(values.size())) };
        for (std::int32_t index{ 0 }; index < length; ++index)
        {
            const std::uint32_t compressed{ vmhook::get_array_element<std::uint32_t>(array_oop, index) };
            vmhook::write_java_string(vmhook::hotspot::decode_oop_pointer(compressed), values[static_cast<std::size_t>(index)]);
        }
    }

    // --- Helper: decode compressed array OOP ---------------------------------

    /*
        @brief Decodes a 32-bit compressed array OOP into a raw pointer.
        @param compressed  A 32-bit compressed OOP value.
        @return A valid pointer if decoding succeeded, nullptr otherwise.
    */
    auto decode_array_oop(const std::uint32_t compressed)
        -> void*
    {
        if (!compressed)
        {
            return nullptr;
        }
        void* const decoded_pointer{ vmhook::hotspot::decode_oop_pointer(compressed) };
        return (decoded_pointer && vmhook::hotspot::is_valid_pointer(decoded_pointer)) ? decoded_pointer : nullptr;
    }

    // -------------------------------------------------------------------------
    // Background watchers (definitions).  Declared above as part of the
    // public API; defined here so the lambda bodies see the complete
    // `vmhook::object_base` type.
    // -------------------------------------------------------------------------

    /*
        @brief Registry that owns hardware-data-breakpoint state for the
               trap-based watch_static_field path.  At most four
               simultaneous watches per process (the CPU exposes DR0-DR3).
    */
#if VMHOOK_HAS_HW_DATA_BREAKPOINTS
    namespace detail
    {
        struct dr_slot
        {
            void*                                                    address{ nullptr };
            std::function<void(const void*)>                         callback{};
            std::uint64_t                                            dr7_bits{ 0 };
            std::atomic_bool                                         in_use{ false };
        };

        inline std::mutex   dr_mutex{};
        inline dr_slot      dr_slots[4]{};
        inline PVOID        dr_veh_handle{ nullptr };
        // Refcount of currently-armed watches.  When this transitions
        // from 0 -> 1 we AddVectoredExceptionHandler; from 1 -> 0 we
        // RemoveVectoredExceptionHandler.  Previously the VEH was
        // installed once and never removed, so a process that briefly
        // used a watch then dropped it kept the handler in the kernel's
        // dispatch list for the rest of its lifetime.  Both reads and
        // writes are gated by dr_mutex so we don't need an atomic.
        inline int          dr_armed_count{ 0 };

        inline auto find_free_slot() -> int
        {
            for (int i{ 0 }; i < 4; ++i)
            {
                if (!dr_slots[i].in_use.load(std::memory_order_acquire))
                {
                    return i;
                }
            }
            return -1;
        }

        inline auto refresh_thread_drs(int slot, std::uint64_t address, std::uint64_t dr7_bits) -> void
        {
            vmhook::os::detail_dr::for_each_thread([&](HANDLE thread)
            {
                CONTEXT ctx{};
                ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                if (::GetThreadContext(thread, &ctx))
                {
                    switch (slot)
                    {
                        case 0: ctx.Dr0 = address; break;
                        case 1: ctx.Dr1 = address; break;
                        case 2: ctx.Dr2 = address; break;
                        case 3: ctx.Dr3 = address; break;
                        default: return;
                    }
                    // Set Dr7 with our slot's bits or'd in (don't clobber
                    // other slots that may be configured).
                    const std::uint64_t slot_mask_local{ std::uint64_t{ 0b11 } << (slot * 2) };
                    const std::uint64_t slot_mask_rwlen{ std::uint64_t{ 0xF }  << (16 + slot * 4) };
                    const std::uint64_t mask{ slot_mask_local | slot_mask_rwlen };
                    ctx.Dr7 = (ctx.Dr7 & ~mask) | (dr7_bits & mask);
                    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                    ::SetThreadContext(thread, &ctx);
                }
            });
        }

        inline auto clear_thread_drs(int slot) -> void
        {
            vmhook::os::detail_dr::for_each_thread([&](HANDLE thread)
            {
                CONTEXT ctx{};
                ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                if (::GetThreadContext(thread, &ctx))
                {
                    switch (slot)
                    {
                        case 0: ctx.Dr0 = 0; break;
                        case 1: ctx.Dr1 = 0; break;
                        case 2: ctx.Dr2 = 0; break;
                        case 3: ctx.Dr3 = 0; break;
                        default: return;
                    }
                    const std::uint64_t slot_mask_local{ std::uint64_t{ 0b11 } << (slot * 2) };
                    const std::uint64_t slot_mask_rwlen{ std::uint64_t{ 0xF }  << (16 + slot * 4) };
                    ctx.Dr7 &= ~(slot_mask_local | slot_mask_rwlen);
                    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                    ::SetThreadContext(thread, &ctx);
                }
            });
        }

        inline auto WINAPI dr_exception_handler(EXCEPTION_POINTERS* eptrs) -> LONG
        {
            if (eptrs->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
            {
                return EXCEPTION_CONTINUE_SEARCH;
            }
            const std::uint64_t dr6{ eptrs->ContextRecord->Dr6 };
            int slot{ -1 };
            if      (dr6 & 0x1) { slot = 0; }
            else if (dr6 & 0x2) { slot = 1; }
            else if (dr6 & 0x4) { slot = 2; }
            else if (dr6 & 0x8) { slot = 3; }
            if (slot < 0)
            {
                return EXCEPTION_CONTINUE_SEARCH;
            }
            // Take a LOCAL COPY of the callback and address under the lock.
            // Previously we kept slot_ptr and dereferenced it after releasing
            // the mutex - which raced with watch_handle's destructor clearing
            // dr_slots[slot].callback = nullptr.  Two failure modes:
            //   1. Destructor wins between our "if (slot_ptr->callback)"
            //      and "slot_ptr->callback(...)" - operator() on an empty
            //      std::function throws std::bad_function_call.
            //   2. Destructor wins MID-CALL - the callback's stored lambda
            //      target gets destructed while it's executing, taking out
            //      anything the lambda captured by value (use-after-free
            //      on any heap-allocated captures).
            // Copying the std::function bumps the target's refcount-equivalent
            // and keeps it alive until our local goes out of scope, no matter
            // what the destructor does to the slot.
            std::function<void(const void*)> callback_copy;
            void* address_copy{ nullptr };
            {
                std::lock_guard<std::mutex> guard{ dr_mutex };
                if (dr_slots[slot].in_use.load(std::memory_order_relaxed))
                {
                    callback_copy = dr_slots[slot].callback;
                    address_copy = dr_slots[slot].address;
                }
            }
            if (callback_copy)
            {
                try
                {
                    callback_copy(address_copy);
                }
                catch (...)
                {
                    // never let user exceptions escape into the kernel.
                }
            }
            // Clear DR6 status and set RF so we don't re-trigger on the
            // very instruction that just fired.
            eptrs->ContextRecord->Dr6   = 0;
            eptrs->ContextRecord->EFlags |= 0x10000;
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        /*
            @brief Increments the armed-watch refcount; installs the VEH on the
                   0 -> 1 transition.
            @details
            Must be called with dr_mutex held.  ensure_dr_handler_installed used
            to install the VEH on first call and never remove it; pair this
            with dr_unarm_one (called from the watch_handle destructor) so the
            handler is unregistered once the last watch goes away.
        */
        inline auto dr_arm_one() -> void
        {
            if (dr_armed_count == 0)
            {
                dr_veh_handle = ::AddVectoredExceptionHandler(1, &dr_exception_handler);
            }
            ++dr_armed_count;
        }

        /*
            @brief Decrements the armed-watch refcount; removes the VEH on the
                   1 -> 0 transition.
            @details
            Must be called with dr_mutex held.  Safe to call when the count
            is already 0 (no-op) so double-stop on a handle doesn't
            misbalance the count or crash on a stale VEH handle.
        */
        inline auto dr_unarm_one() -> void
        {
            if (dr_armed_count <= 0)
            {
                return;
            }
            --dr_armed_count;
            if (dr_armed_count == 0 && dr_veh_handle != nullptr)
            {
                ::RemoveVectoredExceptionHandler(dr_veh_handle);
                dr_veh_handle = nullptr;
            }
        }

        /*
            @brief Legacy alias retained for source compatibility.
            @details
            ensure_dr_handler_installed used to be the public entry point.
            New code should call dr_arm_one() so the unregister path
            stays balanced.  Kept here so external consumers that might
            have built on the old name don't break at upgrade time.
        */
        inline auto ensure_dr_handler_installed() -> void
        {
            dr_arm_one();
        }
    } // namespace detail
#endif // VMHOOK_HAS_HW_DATA_BREAKPOINTS

    /*
        @brief Watches a Java static field and invokes a callback when it changes.
        @details
        Installs a hardware data breakpoint (one of DR0-DR3) on the
        field's address.  The trap fires *instantly* on every write —
        no polling, no idle CPU.  The callback runs inside a vectored
        exception handler on whichever thread issued the write, so it
        must not allocate Java objects or call back into the JVM (those
        operations require a JavaThread and a safe-point window).

        Currently supported only on Windows x86_64.  On other platforms
        the function returns an empty `watch_handle` (its `running()`
        returns false) and logs an error — there is no polling fallback.

        Limits:
          - At most 4 simultaneous watches per process (hardware limit
            of DR0-DR3).
          - Threads that exist when the watch is installed get the trap
            armed; threads created later do NOT.  Hooking thread
            creation so they are caught too is a known improvement.
          - The callback receives (old_value, new_value) where
            old_value is a zero-initialised placeholder — the CPU does
            not save the pre-write value, so the "previous" argument
            cannot be reconstructed from the trap path alone.

        @tparam wrapper_type  Registered C++ wrapper for the Java class.
        @tparam field_type    Field value type (used to read the new
                              value at the moment the trap fires and to
                              pick the DR LEN field).
        @tparam callback_type Callable invoked as
                              `void(field_type old, field_type new)`.
    */
    template<class wrapper_type, typename field_type, typename callback_type>
    inline auto watch_static_field(
        std::string_view field_name,
        callback_type    on_change) -> watch_handle
    {
#if VMHOOK_HAS_HW_DATA_BREAKPOINTS
        const auto proxy{ vmhook::object_base::get_field(
            std::type_index{ typeid(wrapper_type) }, field_name) };
        if (!proxy.has_value())
        {
            VMHOOK_LOG("{} watch_static_field<{}>('{}'): field not found",
                       vmhook::error_tag, typeid(wrapper_type).name(), field_name);
            return watch_handle{};
        }
        void* const address{ proxy->raw_address() };
        if (!address)
        {
            VMHOOK_LOG("{} watch_static_field<{}>('{}'): null address",
                       vmhook::error_tag, typeid(wrapper_type).name(), field_name);
            return watch_handle{};
        }

        std::lock_guard<std::mutex> guard{ detail::dr_mutex };
        const int slot{ detail::find_free_slot() };
        if (slot < 0)
        {
            VMHOOK_LOG("{} watch_static_field: all 4 hardware breakpoint slots in use",
                       vmhook::error_tag);
            return watch_handle{};
        }
        // Refcount the install so the VEH is uninstalled when the last
        // watch_handle drops its on_stop callback.  Previously this called
        // ensure_dr_handler_installed() which installed the handler once
        // and never removed it.
        detail::dr_arm_one();

        constexpr auto length{
            sizeof(field_type) == 1 ? vmhook::os::data_breakpoint_length::one_byte    :
            sizeof(field_type) == 2 ? vmhook::os::data_breakpoint_length::two_bytes   :
            sizeof(field_type) == 4 ? vmhook::os::data_breakpoint_length::four_bytes  :
                                       vmhook::os::data_breakpoint_length::eight_bytes };
        const std::uint64_t dr7_bits{ vmhook::os::detail_dr::build_dr7(
            slot, vmhook::os::data_breakpoint_kind::write, length) };

        detail::dr_slots[slot].address  = address;
        detail::dr_slots[slot].dr7_bits = dr7_bits;
        detail::dr_slots[slot].callback = [on_change = std::move(on_change), address](const void*) mutable
        {
            // The trap arrives *during* the write instruction.  The
            // memory at `address` may or may not already hold the new
            // value depending on how the CPU pipelines the access; we
            // call the user callback with what we observe now.
            try
            {
                field_type current{};
                std::memcpy(&current, address, sizeof(field_type));
                on_change(field_type{}, current);
            }
            catch (const std::exception& ex)
            {
                VMHOOK_LOG("{} watch_static_field<{}>: user on_change callback threw: {} - "
                           "swallowed to keep the DR trap handler safe.",
                           vmhook::error_tag, typeid(field_type).name(), ex.what());
            }
            catch (...)
            {
                VMHOOK_LOG("{} watch_static_field<{}>: user on_change callback threw an unknown "
                           "exception - swallowed to keep the DR trap handler safe.",
                           vmhook::error_tag, typeid(field_type).name());
            }
        };
        detail::dr_slots[slot].in_use.store(true, std::memory_order_release);

        detail::refresh_thread_drs(slot, reinterpret_cast<std::uint64_t>(address), dr7_bits);

        auto block{ std::make_shared<watch_handle::control_block>() };
        block->on_stop = [slot]() noexcept
        {
            std::lock_guard<std::mutex> guard{ detail::dr_mutex };
            detail::clear_thread_drs(slot);
            detail::dr_slots[slot].in_use.store(false, std::memory_order_release);
            detail::dr_slots[slot].address  = nullptr;
            detail::dr_slots[slot].callback = nullptr;
            detail::dr_slots[slot].dr7_bits = 0;
            // Balance the dr_arm_one() in watch_static_field below.  When
            // the last live watch_handle goes away the VEH is uninstalled
            // by the dr_armed_count 1 -> 0 transition.
            detail::dr_unarm_one();
        };
        return watch_handle{ std::move(block) };
#else
        (void)field_name;
        (void)on_change;
        VMHOOK_LOG("{} watch_static_field<{}>('{}'): hardware data breakpoints "
                   "are unsupported on this platform; the watch was not armed",
                   vmhook::error_tag, typeid(wrapper_type).name(), field_name);
        return watch_handle{};
#endif
    }

    /*
        @brief Registry that owns the class-load callbacks installed by
               on_class_loaded.  Lives inside namespace detail because it
               is implementation-only; users get the class name through
               the on_class_loaded callback.
    */
    namespace detail
    {
        using class_load_callback_t = std::function<void(const std::string&)>;
        inline std::mutex                                          class_load_mutex{};
        inline std::vector<std::shared_ptr<class_load_callback_t>> class_load_callbacks{};
        inline bool                                                class_load_hook_installed{ false };

        // Internal C++ wrapper for java.lang.ClassLoader used only by the
        // class-load hook.  Not exposed publicly; users see the class
        // name string in the callback.
        class class_loader_wrapper : public vmhook::object<class_loader_wrapper>
        {
        public:
            explicit class_loader_wrapper(vmhook::oop_t oop) noexcept
                : vmhook::object<class_loader_wrapper>{ oop }
            {
            }
        };
    }

    /*
        @brief Registers a callback fired whenever java.lang.ClassLoader
               defines a new class.
        @details
        Installs an interpreter hook on the Java method
        `java.lang.ClassLoader::defineClass(String, byte[], int, int,
        ProtectionDomain)` and dispatches the callback with the internal
        class name read from the call's first argument.  This is
        event-driven: zero latency, zero idle cost — no polling.

        Limitations:
          - Only catches classes defined through ClassLoader.defineClass.
            Bootstrap-loaded classes (java.*, javax.*, sun.*) are not
            reported because they bypass the Java-side defineClass entry.
          - The (String, ByteBuffer, ProtectionDomain) overload of
            defineClass is not yet hooked.

        Exception safety: noexcept boundary — callback exceptions are caught
            and logged through VMHOOK_LOG.
        Thread safety: the callback runs on the Java thread that triggered
            the class definition.

        Example:
            auto handle{ vmhook::on_class_loaded(
                [](const std::string& internal_name)
                {
                    std::println("loaded: {}", internal_name);
                }) };

        @tparam callback_type Callable invoked as `void(const std::string&)`.
                              Argument is the JVM-style `/`-separated name.
    */
    template<typename callback_type>
    inline auto on_class_loaded(callback_type on_load) -> watch_handle
    {
        auto cb{ std::make_shared<detail::class_load_callback_t>(std::move(on_load)) };

        {
            std::lock_guard<std::mutex> guard{ detail::class_load_mutex };
            detail::class_load_callbacks.push_back(cb);

            if (!detail::class_load_hook_installed)
            {
                if (vmhook::type_to_class_map.find(std::type_index{ typeid(detail::class_loader_wrapper) })
                    == vmhook::type_to_class_map.end())
                {
                    vmhook::register_class<detail::class_loader_wrapper>("java/lang/ClassLoader");
                }

                auto define_class_detour = [](
                    vmhook::return_value& /*ret*/,
                    const std::unique_ptr<detail::class_loader_wrapper>& /*self*/,
                    const std::string& name,
                    vmhook::oop_t /*bytes*/,
                    std::int32_t /*offset*/,
                    std::int32_t /*length*/,
                    vmhook::oop_t /*protection_domain*/)
                {
                    std::vector<std::shared_ptr<detail::class_load_callback_t>> snapshot;
                    {
                        std::lock_guard<std::mutex> guard2{ detail::class_load_mutex };
                        snapshot = detail::class_load_callbacks;
                    }
                    std::string internal_name{ name };
                    std::replace(internal_name.begin(), internal_name.end(), '.', '/');
                    for (auto& callback : snapshot)
                    {
                        if (!callback) { continue; }
                        try
                        {
                            (*callback)(internal_name);
                        }
                        catch (const std::exception& ex)
                        {
                            VMHOOK_LOG("{} on_class_loaded callback: {}",
                                       vmhook::error_tag, ex.what());
                        }
                    }
                };
                const bool installed = vmhook::hook<detail::class_loader_wrapper>(
                    "defineClass",
                    "(Ljava/lang/String;[BIILjava/security/ProtectionDomain;)Ljava/lang/Class;",
                    define_class_detour);

                if (installed)
                {
                    detail::class_load_hook_installed = true;
                }
                else
                {
                    VMHOOK_LOG("{} on_class_loaded: ClassLoader.defineClass hook installation failed",
                               vmhook::error_tag);
                }
            }
        }

        // If the defineClass hook never armed (e.g. no JVM in-process, or the
        // method couldn't be resolved), this watcher can never fire.  Match
        // watch_static_field's contract: return an inert handle whose
        // running() == false, and drop the callback we optimistically pushed
        // so it doesn't linger in the registry.  Without this, a caller has no
        // way to tell a working watcher from a dead one — the exact
        // method-vs-field-style inconsistency we want to avoid.
        {
            std::lock_guard<std::mutex> guard{ detail::class_load_mutex };
            if (!detail::class_load_hook_installed)
            {
                detail::class_load_callbacks.erase(
                    std::remove(detail::class_load_callbacks.begin(),
                                detail::class_load_callbacks.end(), cb),
                    detail::class_load_callbacks.end());
                return watch_handle{};
            }
        }

        auto block{ std::make_shared<watch_handle::control_block>() };
        block->on_stop = [cb]() noexcept
        {
            try
            {
                std::lock_guard<std::mutex> guard{ detail::class_load_mutex };
                detail::class_load_callbacks.erase(
                    std::remove(detail::class_load_callbacks.begin(),
                                detail::class_load_callbacks.end(), cb),
                    detail::class_load_callbacks.end());
            }
            catch (const std::exception& ex)
            {
                VMHOOK_LOG("{} on_class_loaded on_stop: failed to erase callback: {} - swallowed.",
                           vmhook::error_tag, ex.what());
            }
            catch (...)
            {
                VMHOOK_LOG("{} on_class_loaded on_stop: unknown exception while erasing callback - "
                           "swallowed.", vmhook::error_tag);
            }
        };

        return watch_handle{ std::move(block) };
    }

    /*
        @brief Registry that owns the exception callbacks installed by
               on_exception.
    */
    namespace detail
    {
        using exception_callback_t = std::function<void(const std::string&)>;
        inline std::mutex                                          exception_mutex{};
        inline std::vector<std::shared_ptr<exception_callback_t>>  exception_callbacks{};
        inline bool                                                exception_hook_installed{ false };

        // Internal C++ wrapper for java.lang.Throwable used only by the
        // exception hook.  Not exposed publicly.
        class throwable_wrapper : public vmhook::object<throwable_wrapper>
        {
        public:
            explicit throwable_wrapper(vmhook::oop_t oop) noexcept
                : vmhook::object<throwable_wrapper>{ oop }
            {
            }
        };
    }

    /*
        @brief Registers a callback fired whenever a `java.lang.Throwable`
               (or any subclass) is constructed.
        @details
        Installs an interpreter hook on
        `java.lang.Throwable::fillInStackTrace()Ljava/lang/Throwable;`,
        which every public Throwable constructor calls before
        returning.  When the hook fires we read the dynamic klass off
        the receiver oop's narrow-klass header and dispatch the
        callback with the fully-qualified internal class name (`/`-
        separated).

        This catches NullPointerException, IOException,
        IllegalArgumentException, custom exceptions, and any other
        Throwable whose constructor goes through `fillInStackTrace`.
        It misses:
          - exceptions constructed with `writableStackTrace=false`
            (the 4-bool protected constructor with that flag set),
          - subclasses that override `fillInStackTrace` to a no-op
            for performance (rare; some preallocated VM errors do
            this).

        Event-driven: zero polling, zero idle cost.

        Exception safety: noexcept boundary — callback exceptions are
            caught and logged through VMHOOK_LOG.
        Thread safety: the callback runs on the Java thread that
            constructed the throwable.

        @tparam callback_type Callable invoked as
                              `void(const std::string&)`.  The string
                              is the throwable's internal class name
                              (e.g. `"java/lang/NullPointerException"`).
    */
    template<typename callback_type>
    inline auto on_exception(callback_type on_throw) -> watch_handle
    {
        auto cb{ std::make_shared<detail::exception_callback_t>(std::move(on_throw)) };

        {
            std::lock_guard<std::mutex> guard{ detail::exception_mutex };
            detail::exception_callbacks.push_back(cb);

            if (!detail::exception_hook_installed)
            {
                if (vmhook::type_to_class_map.find(std::type_index{ typeid(detail::throwable_wrapper) })
                    == vmhook::type_to_class_map.end())
                {
                    vmhook::register_class<detail::throwable_wrapper>("java/lang/Throwable");
                }

                auto fill_in_stack_trace_detour = [](
                    vmhook::return_value& /*ret*/,
                    const std::unique_ptr<detail::throwable_wrapper>& self)
                {
                    std::string internal_name{};
                    if (self)
                    {
                        if (auto* const dynamic_klass{ vmhook::klass_from_oop(self->get_instance()) })
                        {
                            if (auto* const sym{ dynamic_klass->get_name() })
                            {
                                internal_name = sym->to_string();
                            }
                        }
                    }
                    if (internal_name.empty())
                    {
                        internal_name = "java/lang/Throwable";
                    }

                    std::vector<std::shared_ptr<detail::exception_callback_t>> snapshot;
                    {
                        std::lock_guard<std::mutex> guard2{ detail::exception_mutex };
                        snapshot = detail::exception_callbacks;
                    }
                    for (auto& callback : snapshot)
                    {
                        if (!callback) { continue; }
                        try
                        {
                            (*callback)(internal_name);
                        }
                        catch (const std::exception& ex)
                        {
                            VMHOOK_LOG("{} on_exception callback: {}",
                                       vmhook::error_tag, ex.what());
                        }
                    }
                };

                const bool installed = vmhook::hook<detail::throwable_wrapper>(
                    "fillInStackTrace",
                    "()Ljava/lang/Throwable;",
                    fill_in_stack_trace_detour);

                if (installed)
                {
                    detail::exception_hook_installed = true;
                }
                else
                {
                    VMHOOK_LOG("{} on_exception: Throwable.fillInStackTrace hook installation failed",
                               vmhook::error_tag);
                }
            }
        }

        // Inert-watcher parity (see on_class_loaded): if the fillInStackTrace
        // hook never armed, return an empty handle (running() == false) and
        // drop the optimistically-pushed callback.
        {
            std::lock_guard<std::mutex> guard{ detail::exception_mutex };
            if (!detail::exception_hook_installed)
            {
                detail::exception_callbacks.erase(
                    std::remove(detail::exception_callbacks.begin(),
                                detail::exception_callbacks.end(), cb),
                    detail::exception_callbacks.end());
                return watch_handle{};
            }
        }

        auto block{ std::make_shared<watch_handle::control_block>() };
        block->on_stop = [cb]() noexcept
        {
            try
            {
                std::lock_guard<std::mutex> guard{ detail::exception_mutex };
                detail::exception_callbacks.erase(
                    std::remove(detail::exception_callbacks.begin(),
                                detail::exception_callbacks.end(), cb),
                    detail::exception_callbacks.end());
            }
            catch (const std::exception& ex)
            {
                VMHOOK_LOG("{} on_exception on_stop: failed to erase callback: {} - swallowed.",
                           vmhook::error_tag, ex.what());
            }
            catch (...)
            {
                VMHOOK_LOG("{} on_exception on_stop: unknown exception while erasing callback - "
                           "swallowed.", vmhook::error_tag);
            }
        };

        return watch_handle{ std::move(block) };
    }

    namespace detail
    {
        // Defined here (not next to shutdown_hooks) because it touches the
        // class_load_* / exception_* registries declared just above in this
        // same namespace.  Forward-declared near shutdown_hooks() so that
        // call site can reach it.  Clears both watcher install latches and
        // drops the stale callback lists, so the first on_class_loaded() /
        // on_exception() AFTER a shutdown_hooks() re-installs a live detour
        // instead of trusting a latch left true while the detour was torn down.
        static auto reset_watcher_latches() noexcept -> void
        {
            {
                std::lock_guard<std::mutex> guard{ class_load_mutex };
                class_load_hook_installed = false;
                class_load_callbacks.clear();
            }
            {
                std::lock_guard<std::mutex> guard{ exception_mutex };
                exception_hook_installed = false;
                exception_callbacks.clear();
            }
        }
    }

    // -------------------------------------------------------------------------
    // JNI global references: keep a Java object alive across GC.
    //
    // Everything else in this header hands you an oop_t / object_base / wrapper
    // whose address is valid only for the duration of the current hook
    // invocation: HotSpot relocates objects on every (relocating) GC, so an
    // address you captured this tick is a dangling pointer the moment a
    // collection runs.  That makes the common "compute Java objects on one
    // thread/tick, consume them on another" pattern a use-after-relocation by
    // construction.  global_ref is the missing lifetime primitive: it pins the
    // object via NewGlobalRef and its oop() always reads the object's CURRENT
    // (post-relocation) address out of the handle slot.
    // -------------------------------------------------------------------------
    namespace jni
    {
        /*
            @brief Move-only RAII pin around a JNI global reference.
            @details
            Construct from a raw decoded heap OOP (e.g. wrapper->get_instance(),
            field_proxy reads, method_proxy::call() results).  The constructor
            promotes it to a JNI global reference so the JVM cannot collect the
            object while this handle is alive; the destructor releases it exactly
            once.  oop() re-derives the live address every call, so it stays valid
            across relocating GCs.  Copying is disabled (a global ref is owned
            exactly once — double DeleteGlobalRef corrupts the handle table);
            moving transfers ownership and nulls the source.

            Typical use: cache the result of a method call across ticks, own Java
            objects inside a long-lived / cross-thread snapshot, or hold a handle
            on a thread other than the one that produced it.

                vmhook::jni::global_ref pinned{ wrapper->get_instance() };
                // ... ticks / GCs later, on any thread ...
                if (pinned) { use(pinned.oop()); }   // live, relocated address

            Requires a JVM (and an attached thread): with no JVM, construction is
            a no-op and the handle stays empty (operator bool == false).
        */
        class global_ref final
        {
        public:
            global_ref() noexcept = default;

            explicit global_ref(vmhook::oop_t const raw_oop) noexcept
            {
                if (!raw_oop)
                {
                    return;
                }
                void* storage{};
                void* const local_handle{ vmhook::detail::jni_oop_handle(raw_oop, storage) };
                this->handle_ = vmhook::detail::jni_new_global_ref(local_handle);
            }

            ~global_ref() noexcept
            {
                vmhook::detail::jni_delete_global_ref(this->handle_);
            }

            global_ref(const global_ref&)                    = delete;
            auto operator=(const global_ref&) -> global_ref& = delete;

            global_ref(global_ref&& other) noexcept
                : handle_{ other.handle_ }
            {
                other.handle_ = nullptr;
            }

            auto operator=(global_ref&& other) noexcept -> global_ref&
            {
                if (this != &other)
                {
                    vmhook::detail::jni_delete_global_ref(this->handle_);
                    this->handle_ = other.handle_;
                    other.handle_ = nullptr;
                }
                return *this;
            }

            /*
                @brief The pinned object's CURRENT heap OOP, or nullptr if empty.
                @details Dereferences the global-ref slot, so the returned address
                reflects any relocation the JVM performed since the pin was taken.
            */
            auto oop() const noexcept -> vmhook::oop_t
            {
                if (!this->handle_)
                {
                    return nullptr;
                }
                // HotSpot tags JNI handles in the low bits (JDK 9+: a small type
                // tag distinguishing local / global / weak-global; the underlying
                // OopStorage slot is 8-byte aligned).  A NewGlobalRef handle on a
                // modern JDK therefore is NOT directly dereferenceable — its low
                // bits hold the tag, so `*(void**)handle` reads a MISaligned,
                // garbage oop.  Masking the low 3 bits recovers the real slot
                // address.  This is a no-op on JDK 8 (untagged, already-aligned
                // handles), which is why the raw deref worked there but returned
                // garbage on the CI's JDK 11-25.
                const std::uintptr_t slot{
                    reinterpret_cast<std::uintptr_t>(this->handle_) & ~std::uintptr_t{ 0b111 } };
                return *reinterpret_cast<void**>(slot);
            }

            /*
                @brief Releases the pin early (idempotent).  oop() returns nullptr after.
            */
            auto reset() noexcept -> void
            {
                vmhook::detail::jni_delete_global_ref(this->handle_);
                this->handle_ = nullptr;
            }

            /*
                @brief The raw JNI global handle (for advanced JNI interop).  May be null.
            */
            auto handle() const noexcept -> void*
            {
                return this->handle_;
            }

            explicit operator bool() const noexcept
            {
                return this->handle_ != nullptr;
            }

        private:
            void* handle_{ nullptr };
        };
    }

    /*
        @brief Pins a raw OOP against GC, returning an owning vmhook::jni::global_ref.
        @details One-liner for the ubiquitous `global_ref{ oop }` pattern.
    */
    inline auto pin(vmhook::oop_t const oop) noexcept
        -> vmhook::jni::global_ref
    {
        return vmhook::jni::global_ref{ oop };
    }

    /*
        @brief Pins the Java object behind a wrapper unique_ptr against GC.
        @details `pin(wrapper)` instead of `vmhook::jni::global_ref{ wrapper->get_instance() }`.
        Returns an empty pin for a null wrapper.
    */
    template<typename wrapper_type>
    inline auto pin(const std::unique_ptr<wrapper_type>& wrapper) noexcept
        -> vmhook::jni::global_ref
    {
        static_assert(std::is_base_of_v<vmhook::object_base, wrapper_type>,
                      "vmhook::pin(unique_ptr<T>): T must derive from vmhook::object_base.");
        return wrapper
            ? vmhook::jni::global_ref{ wrapper->vmhook::object_base::get_instance() }
            : vmhook::jni::global_ref{};
    }
}
