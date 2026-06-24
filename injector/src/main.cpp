// injector — Windows-only helper that walks the process list, finds a running
// JVM, and uses LoadLibraryW via CreateRemoteThread to inject vmhook.dll into
// it.  The injection mechanism is intrinsically tied to the Windows process
// model; on Linux the supported workflow is to launch the JVM with
// `-Djava.library.path=...` and call System.loadLibrary("vmhook") from Java
// (or use LD_PRELOAD with libvmhook.so).  See the README.

#include <filesystem>
#include <iostream>
#include <string>
#include <sstream>
#include <vector>

#if !defined(_WIN32) && !defined(_WIN64)
#  error "injector targets Windows only; on Linux use System.loadLibrary or LD_PRELOAD"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>   // K32EnumProcessModulesEx / K32GetModuleBaseNameW (kernel32-exported; no psapi.lib link)
#include <algorithm>
#include <cwchar>

// Replacement for std::println / std::print that works on toolchains that
// have not yet shipped <print> (MinGW-w64 GCC 13, libc++ < 18, MSVC < 19.37).
template <typename... args_t>
static auto log_line(args_t&&... parts) -> void
{
    std::ostringstream stream{};
    ((stream << parts), ...);
    std::cout << stream.str() << '\n';
    std::cout.flush();
}

// std::println has no formatter<wchar_t> for narrow output; convert manually.
// Use WideCharToMultiByte with CP_UTF8 so paths containing non-ASCII characters
// (very common on Windows - any user with a non-Latin-1 username, any program
// installed under "Program Files\...\<accented dirname>\...") render correctly
// in the diagnostic logs.  The previous `static_cast<char>(c)` truncation
// silently corrupted every wide character above U+007F.
static auto wstr_to_str(const std::wstring& ws)
    -> std::string
{
    if (ws.empty())
    {
        return {};
    }
    const int needed{
        ::WideCharToMultiByte(CP_UTF8, 0,
                              ws.data(), static_cast<int>(ws.size()),
                              nullptr, 0, nullptr, nullptr) };
    if (needed <= 0)
    {
        return {};
    }
    std::string result(static_cast<std::size_t>(needed), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0,
                          ws.data(), static_cast<int>(ws.size()),
                          result.data(), needed, nullptr, nullptr);
    return result;
}

static auto resolve_dll_path()
    -> std::wstring
{
    // GetModuleFileNameW can fail (returns 0) or truncate when the path
    // exceeds MAX_PATH (returns MAX_PATH with no null terminator AND sets
    // GetLastError() to ERROR_INSUFFICIENT_BUFFER).  Either case leaves the
    // buffer's tail in an indeterminate state; using it as a wide string
    // gives us a path that's missing characters or has garbage at the end.
    // Return an empty path so main() reports "vmhook.dll not found" instead
    // of trying to inject a bogus path.
    wchar_t executable_path_buffer[MAX_PATH]{};
    const DWORD len{ ::GetModuleFileNameW(nullptr, executable_path_buffer, MAX_PATH) };
    if (len == 0 || len >= MAX_PATH)
    {
        return {};
    }
    return (std::filesystem::path{ executable_path_buffer }.parent_path() / L"vmhook.dll").wstring();
}

struct process_entry
{
    DWORD pid;
    std::wstring name;
};

static auto find_processes(const std::wstring& exe_name)
    -> std::vector<process_entry>
{
    std::vector<process_entry> result{};

    const HANDLE snapshot_handle{ CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0) };

    if (snapshot_handle == INVALID_HANDLE_VALUE)
    {
        return result;
    }

    PROCESSENTRY32W entry{};

    entry.dwSize = sizeof(entry);

    if (Process32FirstW(snapshot_handle, &entry))
    {
        do
        {
            if (!_wcsicmp(entry.szExeFile, exe_name.c_str()))
            {
                result.push_back({ entry.th32ProcessID, entry.szExeFile });
            }

        } while (Process32NextW(snapshot_handle, &entry));
    }

    CloseHandle(snapshot_handle);

    return result;
}

static auto inject_dll(const DWORD pid, const std::wstring& dll_path)
    -> bool
{
    HANDLE process_handle{ OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid) };

    if (!process_handle)
    {
        log_line("[ERROR] Failed to open process with PID ", pid, ", error ", GetLastError(), ".");

        return false;
    }

    const std::size_t path_bytes{ (dll_path.size() + 1) * sizeof(wchar_t) };

    void* const remote_buffer{ VirtualAllocEx(process_handle, nullptr, path_bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE) };

    if (!remote_buffer)
    {
        log_line("[ERROR] VirtualAllocEx failed, error ", GetLastError());

        CloseHandle(process_handle);

        return false;
    }

    if (!WriteProcessMemory(process_handle, remote_buffer, dll_path.c_str(), path_bytes, nullptr))
    {
        log_line("[ERROR] WriteProcessMemory failed, error ", GetLastError());

        VirtualFreeEx(process_handle, remote_buffer, 0, MEM_RELEASE);

        CloseHandle(process_handle);

        return false;
    }

    const HMODULE kernel32{ GetModuleHandleW(L"kernel32.dll") };

    const auto load_library_address{ reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(kernel32, "LoadLibraryW")) };

    HANDLE remote_thread{ CreateRemoteThread(process_handle, nullptr, 0, load_library_address, remote_buffer, 0, nullptr) };

    if (!remote_thread)
    {
        log_line("[ERROR] CreateRemoteThread failed, error ", GetLastError());

        VirtualFreeEx(process_handle, remote_buffer, 0, MEM_RELEASE);

        CloseHandle(process_handle);

        return false;
    }

    WaitForSingleObject(remote_thread, INFINITE);

    // GetExitCodeThread only returns the LOW 32 bits of LoadLibraryW's HMODULE
    // (the thread's exit code is a DWORD).  A module whose load base happens to
    // be 4 GiB-aligned has a low dword of 0, so a SUCCESSFUL injection would be
    // misreported as a failure by `exit_code != 0`.  Kept only as a fallback for
    // the rare case the module enumeration below is unavailable (e.g. denied).
    DWORD exit_code{ 0 };

    GetExitCodeThread(remote_thread, &exit_code);

    CloseHandle(remote_thread);

    VirtualFreeEx(process_handle, remote_buffer, 0, MEM_RELEASE);

    // Authoritative success check: confirm the DLL is actually mapped into the
    // target by scanning its module list for our base name.  K32EnumProcessModulesEx
    // / K32GetModuleBaseNameW are exported directly by kernel32 (the K32* psapi
    // forwarders), so this needs no psapi.lib and keeps injector.exe statically
    // self-contained (KERNEL32 + msvcrt only).
    const std::wstring target_module{ std::filesystem::path(dll_path).filename().wstring() };

    bool enumerated{ false };
    bool module_present{ false };
    std::vector<HMODULE> modules(256);
    DWORD bytes_returned{ 0 };

    for (int attempt{ 0 }; attempt < 2; ++attempt)
    {
        if (!K32EnumProcessModulesEx(process_handle,
                                     modules.data(),
                                     static_cast<DWORD>(modules.size() * sizeof(HMODULE)),
                                     &bytes_returned,
                                     LIST_MODULES_ALL))
        {
            enumerated = false;   // enumeration failed -> fall back to exit_code
            break;
        }

        const std::size_t modules_needed{ bytes_returned / sizeof(HMODULE) };
        if (modules_needed <= modules.size())
        {
            enumerated = true;
            break;
        }

        // Buffer was too small (modules loaded between the two calls); grow once.
        modules.resize(modules_needed);
    }

    if (enumerated)
    {
        const std::size_t module_count{
            (std::min)(static_cast<std::size_t>(bytes_returned / sizeof(HMODULE)), modules.size()) };

        for (std::size_t i{ 0 }; i < module_count && !module_present; ++i)
        {
            wchar_t base_name[MAX_PATH]{};
            if (K32GetModuleBaseNameW(process_handle, modules[i], base_name, MAX_PATH) != 0 &&
                _wcsicmp(base_name, target_module.c_str()) == 0)
            {
                module_present = true;
            }
        }
    }

    CloseHandle(process_handle);

    // Prefer the module-list result; if enumeration was unavailable, fall back to
    // the legacy exit-code heuristic so behaviour is never worse than before.
    const bool succeeded{ enumerated ? module_present : (exit_code != 0) };

    if (!succeeded)
    {
        log_line("[ERROR] injection of ", wstr_to_str(target_module),
                 " not confirmed (module-list-checked=", enumerated ? "yes" : "no",
                 ", thread exit_code=", exit_code, ").");
    }

    return succeeded;
}

auto main(std::int32_t argc, char** argv)
    -> std::int32_t
{
    const std::wstring dll_path{ resolve_dll_path() };

    log_line("[INFO] dll_path : ", wstr_to_str(dll_path), ".");

    if (dll_path.empty() || !std::filesystem::exists(dll_path))
    {
        log_line("[ERROR] vmhook.dll not found.");

        return EXIT_FAILURE;
    }

    log_line("[OK] dll found.");

    DWORD target_pid{ 0 };

    if (argc >= 2)
    {
        // Explicit PID path: skip the JVM process scan and inject directly.
        // The previous flow parsed argv[1] into target_pid and THEN scanned
        // and unconditionally overrode target_pid with the single-JVM
        // result (or aborted on multi-JVM) - which both ignored the user's
        // explicit choice and refused valid invocations when more than one
        // JVM was running.  Honor the user's PID without any further
        // discovery.
        try
        {
            target_pid = static_cast<DWORD>(std::stoul(argv[1]));
        }
        catch (...)
        {
            log_line("[ERROR] Invalid PID provided.");
            return EXIT_FAILURE;
        }

        log_line("[INFO] Using provided PID ", target_pid, ".");
    }
    else
    {
        log_line("[INFO] No PID provided. Scanning for JVM processes...");

        std::vector<process_entry> processes{ find_processes(L"javaw.exe") };

        if (processes.empty())
        {
            processes = find_processes(L"java.exe");
        }

        if (processes.empty())
        {
            log_line("[ERROR] No running JVM processes found.");
            return EXIT_FAILURE;
        }

        if (processes.size() == 1)
        {
            target_pid = processes[0].pid;
            log_line("[INFO] Found 1 process: ", target_pid, ", injecting automatically.");
        }
        else
        {
            log_line("[ERROR] Multiple JVM processes found, aborting automatic injection. ",
                     "Re-run with an explicit PID: injector.exe <pid>");
            return EXIT_FAILURE;
        }
    }

    log_line("[INFO] Injecting into process ", target_pid);

    if (inject_dll(target_pid, dll_path))
    {
        log_line("[OK] Injection successful.");

        return EXIT_SUCCESS;
    }
    else
    {
        log_line("[ERROR] Injection failed.");

        return EXIT_FAILURE;
    }
}
