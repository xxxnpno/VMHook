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
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace viewer
{
    inline constexpr const wchar_t* k_pipe_name{ L"\\\\.\\pipe\\vmhook_viewer" };

    struct MethodInfo
    {
        std::string   name;
        std::string   descriptor;
        std::uint16_t access{ 0 };  // JVM access flags (ACC_PUBLIC, ACC_STATIC, ...)
    };

    struct FieldInfo
    {
        std::string   name;
        std::string   descriptor;
        std::uint16_t access{ 0 };
        bool          is_static{ false };
    };

    struct ClassInfo
    {
        std::string             internal_name;  // "net/minecraft/client/Minecraft"
        std::string             package;        // "net/minecraft/client"
        std::string             simple_name;    // "Minecraft"
        std::string             super_name;     // superclass internal name ("" for Object/interfaces)
        std::uint16_t           access{ 0 };    // class-file access flags (ACC_INTERFACE/ENUM/ABSTRACT/...)
        std::vector<MethodInfo> methods;
        std::vector<FieldInfo>  fields;
    };

    // One live heap instance of a class: its heap address + its instance fields,
    // each with a formatted value string (as read by the payload).
    struct InstanceInfo
    {
        std::string address;  // "0x..." oop address
        std::vector<std::pair<std::string, std::string>> fields;  // (name, value)
    };

    struct JvmProcess
    {
        std::uint32_t pid{ 0 };
        std::string   image_name;    // "javaw.exe"
        std::string   image_path;    // full path if resolvable
        std::string   command_line;  // e.g. "java -cp out com.example.demo.ExampleApp"
        std::string   display_name;  // window title, else main class, else image name
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

    // Title of the process's first visible top-level window (empty for console
    // apps, whose console window belongs to conhost, not the JVM).
    inline auto resolve_window_title(std::uint32_t pid) -> std::string
    {
        struct ctx_t { std::uint32_t pid; std::string title; } ctx{ pid, {} };
        EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL
        {
            auto* const c{ reinterpret_cast<ctx_t*>(lp) };
            DWORD wpid{ 0 };
            GetWindowThreadProcessId(hwnd, &wpid);
            if (wpid == c->pid && IsWindowVisible(hwnd))
            {
                const int len{ GetWindowTextLengthW(hwnd) };
                if (len > 0)
                {
                    std::wstring buf(static_cast<std::size_t>(len) + 1, L'\0');
                    GetWindowTextW(hwnd, buf.data(), len + 1);
                    buf.resize(static_cast<std::size_t>(len));
                    std::string t{ to_utf8(buf.c_str()) };
                    if (!t.empty()) { c->title = std::move(t); return FALSE; }
                }
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&ctx));
        return ctx.title;
    }

    // A short, friendly name for a JVM: its window title if any, else the main
    // class / jar (last command-line token that looks like one), else the image.
    inline auto friendly_name(const JvmProcess& p) -> std::string
    {
        // Prefer a real window title, but skip console "titles" that are just the
        // launching exe path (Windows sets those) — the main class reads cleaner.
        if (std::string title{ resolve_window_title(p.pid) }; !title.empty())
        {
            const bool exe_path{ (title.find('\\') != std::string::npos || title.find('/') != std::string::npos)
                                 && (title.ends_with(".exe") || title.ends_with(".EXE")) };
            if (!exe_path) return title;
        }
        const std::string& cmd{ p.command_line };
        std::size_t end{ cmd.size() };
        while (end > 0)
        {
            const std::size_t sp{ cmd.find_last_of(' ', end - 1) };
            const std::size_t start{ sp == std::string::npos ? 0 : sp + 1 };
            std::string tok{ cmd.substr(start, end - start) };
            while (!tok.empty() && tok.front() == '"') tok.erase(tok.begin());
            while (!tok.empty() && tok.back() == '"') tok.pop_back();
            if (!tok.empty() && tok.front() != '-' && tok.find('.') != std::string::npos)
            {
                return tok;
            }
            if (sp == std::string::npos) break;
            end = sp;
        }
        return p.image_name;
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
                    JvmProcess jp{ pid, image_name, resolve_image_path(pid), resolve_command_line(pid), {} };
                    jp.display_name = friendly_name(jp);
                    result.push_back(std::move(jp));
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

        // Verify LoadLibraryW actually loaded the DLL: the remote thread's exit
        // code is the low 32 bits of the HMODULE it returned (0 == load failed).
        // Without this, a missing / wrong-bitness / broken payload is reported as
        // a successful injection and the caller then hangs waiting for a pipe
        // connection that never comes.
        const DWORD wait_result{ WaitForSingleObject(thread, 15000) };
        DWORD load_result{ 0 };
        GetExitCodeThread(thread, &load_result);
        CloseHandle(thread);
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        CloseHandle(process);
        if (wait_result != WAIT_OBJECT_0)
        {
            error = "The injected LoadLibrary thread did not finish within 15s.";
            return false;
        }
        if (load_result == 0)
        {
            error = "LoadLibraryW failed inside the target JVM — is vmhook_payload.dll "
                    "present next to the viewer and 64-bit (matching the JVM)?";
            return false;
        }
        return true;
    }

    // ── the viewer application controller ─────────────────────────────────────

    class App
    {
    public:
        // UI-read snapshot (published atomically on completion).
        std::vector<JvmProcess> jvms;
        int                     selected_jvm{ -1 };

        std::mutex              data_mutex;      // guards `classes` + `name_to_index`
        std::vector<ClassInfo>  classes;         // published result
        std::unordered_map<std::string, int> name_to_index;  // internal name -> index (for jump-to-class)

        std::atomic<Status>       status{ Status::Idle };
        std::atomic<std::uint64_t> classes_streamed{ 0 };
        std::string               status_message{ "Idle." };

        // Live-instance inspection (guarded by data_mutex, driven on `worker`).
        std::atomic<Status>       inst_status{ Status::Idle };
        std::string               inst_message{};
        std::string               inst_class{};    // internal name being inspected
        std::vector<InstanceInfo> instances;
        int                       inst_cap{ 1000 }; // max instances the payload scans

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
            // Preserve the current selection across refreshes by pid (the list
            // order/size can change as JVMs come and go).
            const std::uint32_t previous{ (selected_jvm >= 0 && selected_jvm < (int)jvms.size())
                                              ? jvms[(std::size_t)selected_jvm].pid : 0 };
            jvms = enumerate_jvms();
            selected_jvm = -1;
            for (int i = 0; i < (int)jvms.size(); ++i)
            {
                if (jvms[(std::size_t)i].pid == previous) { selected_jvm = i; break; }
            }
            if (selected_jvm < 0 && !jvms.empty())
            {
                selected_jvm = 0;
            }
        }

        bool busy() const
        {
            const Status s{ status.load() };
            return s == Status::Injecting || s == Status::Receiving;
        }

        bool inst_busy() const
        {
            const Status s{ inst_status.load() };
            return s == Status::Injecting || s == Status::Receiving;
        }

        // Scan the attached JVM's heap for live instances of `internal_class` and
        // read their instance-field values.  The payload must already be loaded
        // (i.e. a class enumeration has happened) — this reuses that server via a
        // request file, no re-injection.
        void request_instances(const std::string& internal_class)
        {
            if (busy() || inst_busy() || selected_jvm < 0 || selected_jvm >= static_cast<int>(jvms.size()))
            {
                return;
            }
            if (worker.joinable())
            {
                worker.join();
            }
            inst_status.store(Status::Receiving);
            {
                std::lock_guard<std::mutex> lock{ data_mutex };
                // Only clear when switching classes — a live re-scan keeps the
                // current rows visible until the new results replace them.
                if (inst_class != internal_class)
                {
                    instances.clear();
                    inst_message = "Scanning heap for live instances...";
                }
                inst_class = internal_class;
            }
            worker = std::thread([this, internal_class] { run_instances(internal_class); });
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

        void inst_fail(std::string message)
        {
            {
                std::lock_guard<std::mutex> lock{ data_mutex };
                inst_message = std::move(message);
            }
            inst_status.store(Status::Error);
        }

        // Leave a one-line request for the payload in %TEMP%\vmhook_viewer_req.txt
        // (read + deleted by the payload on its next pipe connect).
        void write_request(const std::string& req)
        {
            wchar_t tmp[MAX_PATH]{};
            const DWORD n{ GetTempPathW(MAX_PATH, tmp) };
            if (n == 0 || n >= MAX_PATH) return;
            std::wstring path{ tmp, n };
            path += L"vmhook_viewer_req.txt";
            HANDLE f{ CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                  nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr) };
            if (f == INVALID_HANDLE_VALUE) return;
            DWORD written{ 0 };
            WriteFile(f, req.data(), static_cast<DWORD>(req.size()), &written, nullptr);
            CloseHandle(f);
        }

        static void parse_instances(const std::string& raw, std::vector<InstanceInfo>& out)
        {
            std::size_t line_start{ 0 };
            while (line_start < raw.size())
            {
                const std::size_t nl{ raw.find('\n', line_start) };
                const std::size_t end{ nl == std::string::npos ? raw.size() : nl };
                const std::string_view line{ raw.data() + line_start, end - line_start };
                line_start = (nl == std::string::npos ? raw.size() : nl + 1);
                if (line.empty()) continue;

                if (line[0] == 'O')
                {
                    out.emplace_back();
                    if (line.size() > 2 && line[1] == '\t')  // O <TAB> 0x<addr>
                    {
                        out.back().address = std::string{ line.substr(2) };
                    }
                }
                else if (line.size() >= 2 && line[0] == 'V' && line[1] == '\t' && !out.empty())
                {
                    const std::size_t t2{ line.find('\t', 2) };
                    if (t2 != std::string_view::npos)
                    {
                        out.back().fields.emplace_back(std::string{ line.substr(2, t2 - 2) },
                                                       std::string{ line.substr(t2 + 1) });
                    }
                }
            }
        }

        void run_instances(const std::string& classname)
        {
            const int cap{ inst_cap > 0 ? inst_cap : 1000 };
            write_request("INST\t" + classname + "\t" + std::to_string(cap));

            HANDLE pipe{ CreateNamedPipeW(
                k_pipe_name, PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 0, 1u << 20, 0, nullptr) };
            if (pipe == INVALID_HANDLE_VALUE)
            {
                inst_fail("CreateNamedPipe failed (error " + std::to_string(GetLastError()) + ").");
                return;
            }

            // No injection: the payload's serve_forever (from the initial attach)
            // connects and streams instances.
            HANDLE ev{ CreateEventW(nullptr, TRUE, FALSE, nullptr) };
            OVERLAPPED ov{};
            ov.hEvent = ev;
            const BOOL connected{ ConnectNamedPipe(pipe, &ov) };
            const DWORD ce{ GetLastError() };
            if (!connected && ce == ERROR_IO_PENDING)
            {
                if (WaitForSingleObject(ev, 15000) != WAIT_OBJECT_0)
                {
                    CloseHandle(ev); CloseHandle(pipe);
                    inst_fail("Timed out — is the payload still attached? Try Attach again.");
                    return;
                }
            }
            else if (!connected && ce != ERROR_PIPE_CONNECTED)
            {
                CloseHandle(ev); CloseHandle(pipe);
                inst_fail("ConnectNamedPipe failed (error " + std::to_string(ce) + ").");
                return;
            }

            std::string raw{};
            std::vector<char> buffer(64u * 1024u);
            for (;;)
            {
                ResetEvent(ev);
                OVERLAPPED rov{};
                rov.hEvent = ev;
                DWORD read_bytes{ 0 };
                const BOOL ok{ ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read_bytes, &rov) };
                if (!ok && GetLastError() == ERROR_IO_PENDING)
                {
                    if (WaitForSingleObject(ev, 30000) != WAIT_OBJECT_0) break;
                    if (!GetOverlappedResult(pipe, &rov, &read_bytes, FALSE)) break;
                }
                else if (!ok)
                {
                    break;
                }
                if (read_bytes == 0) break;
                raw.append(buffer.data(), read_bytes);
            }
            CloseHandle(ev);
            CloseHandle(pipe);

            std::vector<InstanceInfo> parsed{};
            parse_instances(raw, parsed);
            {
                std::lock_guard<std::mutex> lock{ data_mutex };
                instances = std::move(parsed);
                inst_message = "Found " + std::to_string(instances.size()) + " live instance(s).";
            }
            inst_status.store(Status::Done);
        }

        void run_attach(std::uint32_t pid, std::wstring dll_path)
        {
            write_request("ENUM");  // ensure a stale INST request can't leak in
            // 1) Create the pipe server BEFORE injecting so the payload can connect.
            HANDLE pipe{ CreateNamedPipeW(
                k_pipe_name,
                PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,  // async so the waits below are real
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

            // 3) Wait for the payload to connect — genuinely bounded now that the
            //    pipe is FILE_FLAG_OVERLAPPED (ConnectNamedPipe/ReadFile are async,
            //    so a failed attach reports an error instead of hanging forever and
            //    wedging the worker thread the destructor join()s).
            HANDLE ev{ CreateEventW(nullptr, TRUE, FALSE, nullptr) };
            OVERLAPPED overlapped{};
            overlapped.hEvent = ev;
            const BOOL connected_now{ ConnectNamedPipe(pipe, &overlapped) };
            const DWORD connect_err{ GetLastError() };
            if (!connected_now && connect_err == ERROR_IO_PENDING)
            {
                if (WaitForSingleObject(ev, 30000) != WAIT_OBJECT_0)
                {
                    CloseHandle(ev);
                    CloseHandle(pipe);
                    fail("Timed out waiting for the payload to connect (did the DLL load / is the JVM alive?).");
                    return;
                }
            }
            else if (!connected_now && connect_err != ERROR_PIPE_CONNECTED)
            {
                CloseHandle(ev);
                CloseHandle(pipe);
                fail("ConnectNamedPipe failed (error " + std::to_string(connect_err) + ").");
                return;
            }

            // 4) Read + parse the stream into a fresh model, then publish.  Each
            //    read is overlapped with a 30s ceiling so a stalled payload can't
            //    wedge the worker thread.
            std::vector<ClassInfo> parsed{};
            std::string            leftover{};
            std::vector<char>      buffer(256u * 1024u);
            bool                   done{ false };

            while (!done)
            {
                ResetEvent(ev);
                OVERLAPPED rov{};
                rov.hEvent = ev;
                DWORD read_bytes{ 0 };
                const BOOL ok{ ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read_bytes, &rov) };
                if (!ok && GetLastError() == ERROR_IO_PENDING)
                {
                    if (WaitForSingleObject(ev, 30000) != WAIT_OBJECT_0)
                    {
                        break;  // stalled payload — stop rather than hang
                    }
                    if (!GetOverlappedResult(pipe, &rov, &read_bytes, FALSE))
                    {
                        break;  // pipe closed / error
                    }
                }
                else if (!ok)
                {
                    break;  // pipe closed by the payload (EOF) or error
                }
                if (read_bytes == 0)
                {
                    break;
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

            CloseHandle(ev);
            CloseHandle(pipe);

            for (ClassInfo& c : parsed)
            {
                split_name(c);
            }

            {
                std::lock_guard<std::mutex> lock{ data_mutex };
                classes = std::move(parsed);
                name_to_index.clear();
                name_to_index.reserve(classes.size());
                for (int i = 0; i < (int)classes.size(); ++i)
                    name_to_index.emplace(classes[(std::size_t)i].internal_name, i);
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
                    if (parts.size() >= 2) ci.super_name = std::string{ parts[1] };
                    if (parts.size() >= 3)
                        ci.access = (std::uint16_t)std::strtoul(std::string{ parts[2] }.c_str(), nullptr, 10);
                    out.push_back(std::move(ci));
                }
                break;
            case 'M':
                if (parts.size() >= 2 && !out.empty())
                {
                    const std::uint16_t acc{ parts.size() >= 3
                        ? (std::uint16_t)std::strtoul(std::string{ parts[2] }.c_str(), nullptr, 10) : (std::uint16_t)0 };
                    out.back().methods.push_back(MethodInfo{ std::string{ parts[0] }, std::string{ parts[1] }, acc });
                }
                break;
            case 'F':
                if (parts.size() >= 3 && !out.empty())
                {
                    const std::uint16_t acc{ (std::uint16_t)std::strtoul(std::string{ parts[2] }.c_str(), nullptr, 10) };
                    out.back().fields.push_back(
                        FieldInfo{ std::string{ parts[0] }, std::string{ parts[1] }, acc, (acc & 0x0008u) != 0 });
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
