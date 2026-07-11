// vmhook viewer — non-UI application layer.
//
// Owns the data model (enumerated Java classes / methods / fields), discovers
// running HotSpot JVMs, injects the vmhook payload DLL into a chosen one, and
// receives the streamed class surface over a named pipe on a worker thread.
// The UI (main.cpp) only reads the published snapshot + a few atomics.

#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace viewer
{
    inline constexpr const wchar_t* k_pipe_name{ L"\\\\.\\pipe\\vmhook_viewer" };

    struct MethodInfo
    {
        std::string name;
        std::string descriptor;
    };

    struct FieldInfo
    {
        std::string name;
        std::string descriptor;
        bool        is_static{ false };
    };

    struct ClassInfo
    {
        std::string             internal_name;  // "net/minecraft/client/Minecraft"
        std::string             package;        // "net/minecraft/client"
        std::string             simple_name;    // "Minecraft"
        std::vector<MethodInfo> methods;
        std::vector<FieldInfo>  fields;
    };

    struct JvmProcess
    {
        std::uint32_t pid{ 0 };
        std::string   image_name;    // "javaw.exe"
        std::string   image_path;    // full path if resolvable
        std::string   command_line;  // e.g. "java -cp out com.example.demo.ExampleApp"
    };

    enum class Status
    {
        Idle,
        Injecting,
        Receiving,
        Done,
        Error,
    };

    // ── small helpers ─────────────────────────────────────────────────────────

    inline auto to_utf8(const wchar_t* wide, int wide_len = -1) -> std::string
    {
        std::string out{};
        const int bytes{ WideCharToMultiByte(CP_UTF8, 0, wide, wide_len, nullptr, 0, nullptr, nullptr) };
        if (bytes > 0)
        {
            out.resize(static_cast<std::size_t>(wide_len < 0 ? bytes - 1 : bytes));
            WideCharToMultiByte(CP_UTF8, 0, wide, wide_len, out.data(), bytes, nullptr, nullptr);
        }
        return out;
    }

    // ── process discovery ─────────────────────────────────────────────────────

    inline auto process_has_jvm(std::uint32_t pid, const std::string& image_name) -> bool
    {
        HANDLE snapshot{ CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid) };
        if (snapshot != INVALID_HANDLE_VALUE)
        {
            MODULEENTRY32W entry{};
            entry.dwSize = sizeof(entry);
            bool found{ false };
            if (Module32FirstW(snapshot, &entry))
            {
                do
                {
                    if (_wcsicmp(entry.szModule, L"jvm.dll") == 0)
                    {
                        found = true;
                        break;
                    }
                } while (Module32NextW(snapshot, &entry));
            }
            CloseHandle(snapshot);
            if (found)
            {
                return true;
            }
        }
        return _stricmp(image_name.c_str(), "java.exe") == 0 ||
               _stricmp(image_name.c_str(), "javaw.exe") == 0;
    }

    inline auto resolve_image_path(std::uint32_t pid) -> std::string
    {
        HANDLE process{ OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) };
        if (!process)
        {
            return {};
        }
        wchar_t buffer[MAX_PATH]{};
        DWORD   size{ MAX_PATH };
        std::string result{};
        if (QueryFullProcessImageNameW(process, 0, buffer, &size))
        {
            result = to_utf8(buffer, static_cast<int>(size));
        }
        CloseHandle(process);
        return result;
    }

    // Reads a target process's command line by walking its PEB (x64 offsets).
    // Lets the viewer show "java ... com.example.demo.ExampleApp" instead of just
    // "java.exe".  Best-effort: returns "" for protected / unreadable processes.
    inline auto resolve_command_line(std::uint32_t pid) -> std::string
    {
        HANDLE process{ OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid) };
        if (!process)
        {
            return {};
        }
        using NtQueryInformationProcessFn = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
        static NtQueryInformationProcessFn nt_query{
            reinterpret_cast<NtQueryInformationProcessFn>(
                GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess")) };
        if (!nt_query)
        {
            CloseHandle(process);
            return {};
        }

        struct BasicInfo
        {
            PVOID     reserved1;
            PVOID     peb_base;         // PROCESS_BASIC_INFORMATION::PebBaseAddress
            PVOID     reserved2[2];
            ULONG_PTR unique_process_id;
            PVOID     reserved3;
        } info{};
        if (nt_query(process, 0 /*ProcessBasicInformation*/, &info, sizeof(info), nullptr) != 0 || !info.peb_base)
        {
            CloseHandle(process);
            return {};
        }

        // x64: PEB.ProcessParameters @ +0x20; RTL_USER_PROCESS_PARAMETERS.CommandLine @ +0x70.
        PVOID params{ nullptr };
        if (!ReadProcessMemory(process, static_cast<char*>(info.peb_base) + 0x20, &params, sizeof(params), nullptr) || !params)
        {
            CloseHandle(process);
            return {};
        }
        struct UnicodeString
        {
            USHORT length;
            USHORT maximum_length;
            PWSTR  buffer;
        } command{};
        if (!ReadProcessMemory(process, static_cast<char*>(params) + 0x70, &command, sizeof(command), nullptr) ||
            command.length == 0 || !command.buffer)
        {
            CloseHandle(process);
            return {};
        }
        std::wstring wide(command.length / sizeof(wchar_t), L'\0');
        if (!ReadProcessMemory(process, command.buffer, wide.data(), command.length, nullptr))
        {
            CloseHandle(process);
            return {};
        }
        CloseHandle(process);
        return to_utf8(wide.c_str(), static_cast<int>(wide.size()));
    }

    inline auto enumerate_jvms() -> std::vector<JvmProcess>
    {
        std::vector<JvmProcess> result{};
        HANDLE snapshot{ CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0) };
        if (snapshot == INVALID_HANDLE_VALUE)
        {
            return result;
        }
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry))
        {
            do
            {
                const std::string   image_name{ to_utf8(entry.szExeFile) };
                const std::uint32_t pid{ entry.th32ProcessID };
                if (pid != 0 && pid != GetCurrentProcessId() && process_has_jvm(pid, image_name))
                {
                    result.push_back(JvmProcess{ pid, image_name, resolve_image_path(pid), resolve_command_line(pid) });
                }
            } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
        return result;
    }

    // ── injection (CreateRemoteThread → LoadLibraryW) ─────────────────────────

    inline auto inject_dll(std::uint32_t pid, const std::wstring& dll_path, std::string& error) -> bool
    {
        HANDLE process{ OpenProcess(
            PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
            FALSE, pid) };
        if (!process)
        {
            error = "OpenProcess failed (error " + std::to_string(GetLastError()) + "). Try running as administrator.";
            return false;
        }

        const std::size_t path_bytes{ (dll_path.size() + 1) * sizeof(wchar_t) };
        void* const remote{ VirtualAllocEx(process, nullptr, path_bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE) };
        if (!remote)
        {
            error = "VirtualAllocEx failed (error " + std::to_string(GetLastError()) + ").";
            CloseHandle(process);
            return false;
        }

        if (!WriteProcessMemory(process, remote, dll_path.c_str(), path_bytes, nullptr))
        {
            error = "WriteProcessMemory failed (error " + std::to_string(GetLastError()) + ").";
            VirtualFreeEx(process, remote, 0, MEM_RELEASE);
            CloseHandle(process);
            return false;
        }

        HMODULE kernel32{ GetModuleHandleW(L"kernel32.dll") };
        auto* const load_library{ reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(kernel32, "LoadLibraryW")) };
        HANDLE thread{ CreateRemoteThread(process, nullptr, 0, load_library, remote, 0, nullptr) };
        if (!thread)
        {
            error = "CreateRemoteThread failed (error " + std::to_string(GetLastError()) + ").";
            VirtualFreeEx(process, remote, 0, MEM_RELEASE);
            CloseHandle(process);
            return false;
        }

        WaitForSingleObject(thread, 15000);
        CloseHandle(thread);
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        CloseHandle(process);
        return true;
    }

    // ── the viewer application controller ─────────────────────────────────────

    class App
    {
    public:
        // UI-read snapshot (published atomically on completion).
        std::vector<JvmProcess> jvms;
        int                     selected_jvm{ -1 };

        std::mutex              data_mutex;   // guards `classes`
        std::vector<ClassInfo>  classes;      // published result

        std::atomic<Status>       status{ Status::Idle };
        std::atomic<std::uint64_t> classes_streamed{ 0 };
        std::string               status_message{ "Idle." };

        App()
        {
            refresh_jvms();
        }

        ~App()
        {
            if (worker.joinable())
            {
                worker.join();
            }
        }

        void refresh_jvms()
        {
            jvms = enumerate_jvms();
            if (selected_jvm >= static_cast<int>(jvms.size()))
            {
                selected_jvm = jvms.empty() ? -1 : 0;
            }
            else if (selected_jvm < 0 && !jvms.empty())
            {
                selected_jvm = 0;
            }
        }

        bool busy() const
        {
            const Status s{ status.load() };
            return s == Status::Injecting || s == Status::Receiving;
        }

        // Launch: create the pipe, inject, receive, parse, publish. Runs on a
        // detached-ish worker thread (joined in the dtor / before relaunch).
        void attach_selected(const std::wstring& dll_path)
        {
            if (busy() || selected_jvm < 0 || selected_jvm >= static_cast<int>(jvms.size()))
            {
                return;
            }
            if (worker.joinable())
            {
                worker.join();
            }
            const std::uint32_t pid{ jvms[static_cast<std::size_t>(selected_jvm)].pid };
            classes_streamed.store(0);
            {
                std::lock_guard<std::mutex> lock{ data_mutex };
                classes.clear();
            }
            status.store(Status::Injecting);
            set_status("Creating pipe + injecting...");
            worker = std::thread([this, pid, dll_path] { run_attach(pid, dll_path); });
        }

    private:
        std::thread worker;

        // status_message is read by the UI thread under data_mutex; every write
        // from the worker must take the same lock.
        void set_status(std::string message)
        {
            std::lock_guard<std::mutex> lock{ data_mutex };
            status_message = std::move(message);
        }

        void run_attach(std::uint32_t pid, std::wstring dll_path)
        {
            // 1) Create the pipe server BEFORE injecting so the payload can connect.
            HANDLE pipe{ CreateNamedPipeW(
                k_pipe_name,
                PIPE_ACCESS_INBOUND,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                1,                       // one instance
                0, 1u << 20,             // out / in buffer sizes
                0, nullptr) };
            if (pipe == INVALID_HANDLE_VALUE)
            {
                fail("CreateNamedPipe failed (error " + std::to_string(GetLastError()) + ").");
                return;
            }

            // 2) Inject the payload DLL.
            std::string inject_error{};
            if (!inject_dll(pid, dll_path, inject_error))
            {
                CloseHandle(pipe);
                fail(inject_error);
                return;
            }

            status.store(Status::Receiving);
            set_status("Waiting for the JVM to stream its class surface...");

            // 3) Wait for the payload to connect (bounded).
            OVERLAPPED overlapped{};
            overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            const BOOL connected_now{ ConnectNamedPipe(pipe, &overlapped) };
            if (!connected_now && GetLastError() == ERROR_IO_PENDING)
            {
                if (WaitForSingleObject(overlapped.hEvent, 30000) != WAIT_OBJECT_0)
                {
                    CloseHandle(overlapped.hEvent);
                    CloseHandle(pipe);
                    fail("Timed out waiting for the payload to connect (is jvm.dll present / is the JVM alive?).");
                    return;
                }
            }
            else if (!connected_now && GetLastError() != ERROR_PIPE_CONNECTED)
            {
                CloseHandle(overlapped.hEvent);
                CloseHandle(pipe);
                fail("ConnectNamedPipe failed (error " + std::to_string(GetLastError()) + ").");
                return;
            }
            CloseHandle(overlapped.hEvent);

            // 4) Read + parse the stream into a fresh model, then publish.
            std::vector<ClassInfo> parsed{};
            std::string            leftover{};
            std::vector<char>      buffer(256u * 1024u);
            bool                   done{ false };

            while (!done)
            {
                DWORD read_bytes{ 0 };
                const BOOL ok{ ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read_bytes, nullptr) };
                if (!ok || read_bytes == 0)
                {
                    break;  // pipe closed by the payload (EOF)
                }
                leftover.append(buffer.data(), read_bytes);

                std::size_t line_start{ 0 };
                while (true)
                {
                    const std::size_t nl{ leftover.find('\n', line_start) };
                    if (nl == std::string::npos)
                    {
                        break;
                    }
                    parse_line(std::string_view{ leftover }.substr(line_start, nl - line_start), parsed, done);
                    line_start = nl + 1;
                }
                leftover.erase(0, line_start);
                classes_streamed.store(parsed.size());
            }

            CloseHandle(pipe);

            for (ClassInfo& c : parsed)
            {
                split_name(c);
            }

            {
                std::lock_guard<std::mutex> lock{ data_mutex };
                classes = std::move(parsed);
                classes_streamed.store(classes.size());
                status_message = "Loaded " + std::to_string(classes.size()) + " classes.";
            }
            status.store(Status::Done);
        }

        void fail(std::string message)
        {
            set_status(std::move(message));
            status.store(Status::Error);
        }

        static void parse_line(std::string_view line, std::vector<ClassInfo>& out, bool& done)
        {
            if (line.empty())
            {
                return;
            }
            const char tag{ line[0] };
            // records are "<tag>\t<field>\t<field>..."
            std::vector<std::string_view> parts{};
            std::size_t pos{ 2 };  // skip "<tag>\t"
            if (line.size() < 2 || line[1] != '\t')
            {
                if (tag == 'D')  // "DONE" (possibly with a trailing count)
                {
                    done = true;
                }
                return;
            }
            while (pos <= line.size())
            {
                const std::size_t tab{ line.find('\t', pos) };
                if (tab == std::string_view::npos)
                {
                    parts.push_back(line.substr(pos));
                    break;
                }
                parts.push_back(line.substr(pos, tab - pos));
                pos = tab + 1;
            }

            switch (tag)
            {
            case 'C':
                if (!parts.empty())
                {
                    ClassInfo ci{};
                    ci.internal_name = std::string{ parts[0] };
                    out.push_back(std::move(ci));
                }
                break;
            case 'M':
                if (parts.size() >= 2 && !out.empty())
                {
                    out.back().methods.push_back(MethodInfo{ std::string{ parts[0] }, std::string{ parts[1] } });
                }
                break;
            case 'F':
                if (parts.size() >= 3 && !out.empty())
                {
                    out.back().fields.push_back(
                        FieldInfo{ std::string{ parts[0] }, std::string{ parts[1] }, parts[2] == "1" });
                }
                break;
            case 'D':
                done = true;
                break;
            default:
                break;
            }
        }

        static void split_name(ClassInfo& c)
        {
            const std::size_t slash{ c.internal_name.find_last_of('/') };
            if (slash == std::string::npos)
            {
                c.package     = "";
                c.simple_name = c.internal_name;
            }
            else
            {
                c.package     = c.internal_name.substr(0, slash);
                c.simple_name = c.internal_name.substr(slash + 1);
            }
        }
    };
}
