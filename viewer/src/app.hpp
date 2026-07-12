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
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
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
        bool                    is_new{ false }; // loaded since the previous scan (runtime-added)
        double                  seen_epoch{ 0.0 }; // secs-since-app-start when first observed (age source)
    };

    // One instance field: name, formatted value, and (if inherited) the simple
    // name of the class that declares it — empty when declared by the inspected
    // class itself.
    struct InstField
    {
        std::string name;
        std::string value;
        std::string owner;
        std::string ref_addr;  // "0x..." pointee of a non-null reference field, else "" (for grab/place)
    };

    // One live heap instance of a class: its heap address + its instance fields
    // (declared then inherited), each with a formatted value (as read by the payload).
    struct InstanceInfo
    {
        std::string address;  // "0x..." oop address
        std::vector<InstField> fields;
        double seen_epoch{ 0.0 };  // secs-since-app-start when this address was first observed
    };

    // An object stashed in the viewer's "clipboard": a raw heap address + the
    // class it belongs to + a user-facing label.  Used to place a reference into
    // a field, or pass an object as a method argument.  Best-effort: the address
    // is captured at grab time and a relocating GC can leave it stale (same caveat
    // as every raw-oop feature here — pin nothing survives GC pure-VM).
    struct SavedObject
    {
        std::string label;       // e.g. "Worker.inner" or "call result"
        std::string class_name;  // internal name ("com/example/.../Inner")
        std::string address;     // "0x..." decoded heap oop
        double      saved_epoch{ 0.0 };
    };

    // Result of a one-shot mutating op (set / freeze / call), published to the UI.
    struct OpResult
    {
        bool        ok{ false };
        std::string disp;    // display value (call result / re-read field value)
        std::string kind;    // "int"/"ref"/"string"/... for calls; "" for sets
        std::string raddr;   // "0x..." result object address (ref/string calls)
        std::string rclass;  // internal name of the result object
        std::string error;   // human message on failure
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

        // Runtime class-load tracking: a re-scan diffs against the previous
        // snapshot to flag newly-loaded classes (ClassInfo::is_new) and list
        // ones that vanished (unloaded).  Guarded by data_mutex.
        std::atomic<int>          last_added{ 0 };    // # classes new since the previous scan
        std::vector<std::string>  last_removed;       // names present before but gone now
        std::atomic<bool>         has_baseline{ false };

        // Event-driven class-load hook (vmhook::on_class_loaded in the payload):
        // armed when live tracking is on; the payload logs each defineClass and
        // the viewer drains the names, which drive an immediate re-scan.
        std::atomic<bool>          hook_armed{ false };
        std::atomic<std::uint64_t> hook_total{ 0 };   // cumulative hook-captured loads
        std::vector<std::string>   hook_recent;        // last N names the hook captured (guarded)

        // Monotonic clock shared by the worker (stamping seen_epoch) and the UI
        // (computing "age" = now_s() - seen_epoch).  Seconds since App creation.
        auto now_s() const -> double
        {
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
        }

        // Live-instance inspection (guarded by data_mutex, driven on `worker`).
        std::atomic<Status>       inst_status{ Status::Idle };
        std::string               inst_message{};
        std::string               inst_class{};    // internal name being inspected
        std::vector<InstanceInfo> instances;
        int                       inst_cap{ 1000 }; // max instances the payload scans

        // Live static-field inspection (STAT): the class mirror's static fields,
        // shown/refreshed like instances but on their own channel so both windows
        // can auto-refresh independently.  statics_obj holds the single "instance"
        // (the mirror) the payload streams.
        std::atomic<Status>       stat_status{ Status::Idle };
        std::string               stat_message{};
        std::string               stat_class{};    // internal name whose statics are shown
        InstanceInfo              statics_obj;      // mirror + its static field values

        // One-shot mutating op (set / freeze / unfreeze / call): result published
        // for the UI to consume when op_seq changes.  Serialised with the other
        // channels through the single `worker` thread.
        std::atomic<Status>        op_status{ Status::Idle };
        std::atomic<std::uint64_t> op_seq{ 0 };     // bumps once per completed op
        OpResult                   op_result;       // guarded by data_mutex

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

        bool stat_busy() const { return stat_status.load() == Status::Receiving; }
        bool op_busy()   const { return op_status.load()   == Status::Receiving; }

        // Any pipe operation in flight — every dispatcher checks this so the four
        // channels (enumerate / instances / statics / op) never overlap on the
        // single-instance named pipe or the shared worker thread.
        bool any_busy() const { return busy() || inst_busy() || stat_busy() || op_busy(); }

        // Scan the attached JVM's heap for live instances of `internal_class` and
        // read their instance-field values.  The payload must already be loaded
        // (i.e. a class enumeration has happened) — this reuses that server via a
        // request file, no re-injection.
        void request_instances(const std::string& internal_class)
        {
            if (any_busy() || selected_jvm < 0 || selected_jvm >= static_cast<int>(jvms.size()))
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
                    inst_seen_.clear();  // fresh class = fresh age baseline (worker idle here)
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
            if (any_busy() || selected_jvm < 0 || selected_jvm >= static_cast<int>(jvms.size()))
            {
                return;
            }
            if (worker.joinable())
            {
                worker.join();
            }
            const std::uint32_t pid{ jvms[static_cast<std::size_t>(selected_jvm)].pid };
            attached_pid_ = pid;
            attached_dll_ = dll_path;
            classes_streamed.store(0);
            {
                std::lock_guard<std::mutex> lock{ data_mutex };
                classes.clear();
                prev_names_.clear();           // fresh attach = fresh baseline
                last_removed.clear();
            }
            last_added.store(0);
            has_baseline.store(false);
            status.store(Status::Injecting);
            set_status("Creating pipe + injecting...");
            worker = std::thread([this, pid, dll_path] { run_enumerate(pid, dll_path, /*inject=*/true); });
        }

        // Re-enumerate the ALREADY-attached JVM (no re-injection — the loaded
        // payload's serve loop reconnects), diffing against the previous snapshot
        // to flag runtime-loaded/unloaded classes.  Cheap enough to auto-repeat.
        void rescan()
        {
            if (any_busy() || attached_pid_ == 0 || !has_baseline.load())
            {
                return;
            }
            if (worker.joinable())
            {
                worker.join();
            }
            const std::uint32_t pid{ attached_pid_ };
            const std::wstring  dll{ attached_dll_ };
            status.store(Status::Receiving);
            set_status("Re-scanning loaded classes...");
            worker = std::thread([this, pid, dll] { run_enumerate(pid, dll, /*inject=*/false); });
        }

        // Live tracking: arm the on_class_loaded hook (once) and drain the classes
        // it captured, APPENDING just those new classes (with their full surface) to
        // the model — no full re-enumeration/diff.  Purely event-driven: a class
        // appears the moment ClassLoader.defineClass defines it.  Driven from
        // render_ui while "Auto" is on.
        void auto_track()
        {
            if (any_busy() || attached_pid_ == 0 || !has_baseline.load())
            {
                return;
            }
            if (worker.joinable())
            {
                worker.join();
            }
            status.store(Status::Receiving);
            worker = std::thread([this] { run_auto_tracking(); });
        }

        // Fetch the class mirror's live static-field values (STAT).  Same reuse
        // model as request_instances: the loaded payload's serve loop answers.
        void request_statics(const std::string& internal_class)
        {
            if (any_busy() || selected_jvm < 0 || selected_jvm >= static_cast<int>(jvms.size()))
            {
                return;
            }
            if (worker.joinable()) { worker.join(); }
            stat_status.store(Status::Receiving);
            {
                std::lock_guard<std::mutex> lock{ data_mutex };
                if (stat_class != internal_class)
                {
                    statics_obj = InstanceInfo{};
                    stat_message = "Reading static fields...";
                }
                stat_class = internal_class;
            }
            worker = std::thread([this, internal_class] { run_statics(internal_class); });
        }

        // ── one-shot mutating operations (set / freeze / unfreeze / call) ─────
        // Each builds a request line, runs it on the worker, and publishes an
        // OpResult the UI consumes when op_seq changes.  scope = 'I' instance,
        // 'S' static.  All are no-ops while any channel is busy.
        void set_field_value(char scope, const std::string& cls, const std::string& addr,
                             const std::string& field, const std::string& value)
        {
            const std::string tag{ scope == 'S' ? "SETS\t" : "SETI\t" };
            const std::string req{ scope == 'S'
                ? tag + cls + "\t" + field + "\t" + value
                : tag + cls + "\t" + addr + "\t" + field + "\t" + value };
            dispatch_op(req, /*want_call=*/false);
        }

        void freeze_field(char scope, const std::string& cls, const std::string& addr,
                          const std::string& field, const std::string& value)
        {
            std::string req{ "FRZ\t" };
            req += (scope == 'S' ? "S" : "I");
            req += "\t" + cls + "\t" + (scope == 'S' ? std::string{} : addr) + "\t" + field + "\t" + value;
            dispatch_op(req, /*want_call=*/false);
        }

        void unfreeze_field(char scope, const std::string& cls, const std::string& addr, const std::string& field)
        {
            std::string key(1, scope);
            key += '|'; key += cls;
            key += '|'; if (scope == 'I') { key += addr; }
            key += '|'; key += field;
            dispatch_op("UNF\t" + key, /*want_call=*/false);
        }

        void unfreeze_all() { dispatch_op("UNF\t*", /*want_call=*/false); }

        // args: already-tagged tokens (bare literal / @null / @0x<oop> / #text).
        void call_method(const std::string& cls, const std::string& addr, const std::string& method,
                         const std::string& descriptor, const std::vector<std::string>& args)
        {
            std::string req{ "CALL\t" + cls + "\t" + (addr.empty() ? "-" : addr) + "\t"
                             + method + "\t" + descriptor + "\t" + std::to_string(args.size()) };
            for (const std::string& a : args) { req += "\t"; req += a; }
            dispatch_op(req, /*want_call=*/true);
        }

    private:
        void dispatch_op(const std::string& request, bool want_call)
        {
            if (any_busy() || attached_pid_ == 0) { return; }
            if (worker.joinable()) { worker.join(); }
            op_status.store(Status::Receiving);
            worker = std::thread([this, request, want_call] { run_op(request, want_call); });
        }

        // Run a mutating request over the pipe and parse its V / R / E reply into
        // op_result, then bump op_seq so the UI picks it up exactly once.
        void run_op(const std::string& request, bool want_call)
        {
            OpResult r{};
            std::string raw;
            if (!pipe_exchange(request, raw))
            {
                r.ok = false;
                r.error = "Could not reach the payload (is the JVM still attached?).";
            }
            else
            {
                r.ok = true;  // assume success unless an E record says otherwise
                std::size_t pos{ 0 };
                while (pos < raw.size())
                {
                    const std::size_t nl{ raw.find('\n', pos) };
                    const std::size_t end{ nl == std::string::npos ? raw.size() : nl };
                    const std::string_view line{ raw.data() + pos, end - pos };
                    pos = (nl == std::string::npos ? raw.size() : nl + 1);
                    if (line.size() >= 2 && line[0] == 'E' && line[1] == '\t')
                    {
                        r.ok = false;
                        r.error = std::string{ line.substr(2) };
                    }
                    else if (line.size() >= 2 && line[0] == 'V' && line[1] == '\t')
                    {
                        // V <TAB> field <TAB> value [<TAB> owner <TAB> refAddr]
                        const std::size_t t2{ line.find('\t', 2) };
                        if (t2 != std::string_view::npos)
                        {
                            const std::size_t t3{ line.find('\t', t2 + 1) };
                            r.disp = std::string{ line.substr(t2 + 1, (t3 == std::string_view::npos ? line.size() : t3) - (t2 + 1)) };
                        }
                    }
                    else if (line.size() >= 2 && line[0] == 'R' && line[1] == '\t' && want_call)
                    {
                        // R <TAB> display <TAB> kind <TAB> refAddr <TAB> refClass
                        std::vector<std::string_view> f;
                        std::size_t q{ 2 };
                        while (q <= line.size())
                        {
                            const std::size_t tab{ line.find('\t', q) };
                            if (tab == std::string_view::npos) { f.push_back(line.substr(q)); break; }
                            f.push_back(line.substr(q, tab - q));
                            q = tab + 1;
                        }
                        if (f.size() >= 1) r.disp   = std::string{ f[0] };
                        if (f.size() >= 2) r.kind   = std::string{ f[1] };
                        if (f.size() >= 3) r.raddr  = std::string{ f[2] };
                        if (f.size() >= 4) r.rclass = std::string{ f[3] };
                    }
                }
            }
            {
                std::lock_guard<std::mutex> lock{ data_mutex };
                op_result = std::move(r);
            }
            op_seq.fetch_add(1);
            op_status.store(Status::Done);
        }

        void run_statics(const std::string& classname)
        {
            std::string raw;
            const bool ok{ pipe_exchange("STAT\t" + classname, raw) };
            std::vector<InstanceInfo> parsed;
            if (ok) { parse_instances(raw, parsed); }
            {
                std::lock_guard<std::mutex> lock{ data_mutex };
                statics_obj = parsed.empty() ? InstanceInfo{} : std::move(parsed.front());
                stat_message = ok
                    ? ("Read " + std::to_string(statics_obj.fields.size()) + " static field(s).")
                    : "Could not read statics — is the JVM still attached?";
            }
            stat_status.store(ok ? Status::Done : Status::Error);
        }

        std::thread   worker;
        std::uint32_t attached_pid_{ 0 };   // the JVM a successful attach latched onto
        std::wstring  attached_dll_{};      // payload path used for that attach
        std::unordered_set<std::string> prev_names_;  // class names from the previous scan (diff)
        std::chrono::steady_clock::time_point start_{ std::chrono::steady_clock::now() };
        std::unordered_map<std::string, double> class_seen_;  // class name -> first-seen epoch (age)
        std::unordered_map<std::string, double> inst_seen_;   // instance address -> first-seen epoch

        // Send a one-line request to the loaded payload and read its (short)
        // reply.  Reuses the loaded payload's serve loop (no injection) — the
        // same overlapped connect/read pattern as the enumerate/instance paths.
        auto pipe_exchange(const std::string& request, std::string& raw) -> bool
        {
            write_request(request);
            HANDLE pipe{ CreateNamedPipeW(k_pipe_name, PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 0, 1u << 20, 0, nullptr) };
            if (pipe == INVALID_HANDLE_VALUE) return false;
            HANDLE ev{ CreateEventW(nullptr, TRUE, FALSE, nullptr) };
            OVERLAPPED ov{}; ov.hEvent = ev;
            const BOOL connected{ ConnectNamedPipe(pipe, &ov) };
            const DWORD ce{ GetLastError() };
            if (!connected && ce == ERROR_IO_PENDING)
            {
                if (WaitForSingleObject(ev, 10000) != WAIT_OBJECT_0) { CloseHandle(ev); CloseHandle(pipe); return false; }
            }
            else if (!connected && ce != ERROR_PIPE_CONNECTED) { CloseHandle(ev); CloseHandle(pipe); return false; }
            std::vector<char> buffer(64u * 1024u);
            for (;;)
            {
                ResetEvent(ev);
                OVERLAPPED rov{}; rov.hEvent = ev;
                DWORD read_bytes{ 0 };
                const BOOL ok{ ReadFile(pipe, buffer.data(), (DWORD)buffer.size(), &read_bytes, &rov) };
                if (!ok && GetLastError() == ERROR_IO_PENDING)
                {
                    if (WaitForSingleObject(ev, 15000) != WAIT_OBJECT_0) break;
                    if (!GetOverlappedResult(pipe, &rov, &read_bytes, FALSE)) break;
                }
                else if (!ok) break;
                if (read_bytes == 0) break;
                raw.append(buffer.data(), read_bytes);
            }
            CloseHandle(ev); CloseHandle(pipe);
            return true;
        }

        void run_auto_tracking()
        {
            // 1) Arm the class-load hook once (payload replies "H\t1" on success).
            if (!hook_armed.load())
            {
                std::string raw;
                if (pipe_exchange("HOOK", raw))
                    hook_armed.store(raw.find("H\t1") != std::string::npos);
            }
            if (!hook_armed.load()) { status.store(Status::Done); return; }

            // 2) Drain the hook — DRAINF streams each newly-defined class's full
            //    surface (C/M/F), so we can ADD those classes without re-listing
            //    everything.  Most polls drain nothing (a cheap no-op).
            std::string raw;
            if (!pipe_exchange("DRAINF", raw)) { status.store(Status::Done); return; }

            std::vector<ClassInfo> parsed;
            bool done{ false };
            std::size_t p{ 0 };
            while (p < raw.size())
            {
                const std::size_t nl{ raw.find('\n', p) };
                const std::size_t end{ nl == std::string::npos ? raw.size() : nl };
                parse_line(std::string_view{ raw }.substr(p, end - p), parsed, done);
                p = (nl == std::string::npos ? raw.size() : nl + 1);
            }
            for (ClassInfo& c : parsed) split_name(c);

            // 3) Append only the genuinely-new classes (dedup by name), each marked
            //    new + stamped with its REAL arrival time.
            const double t{ now_s() };
            int added{ 0 };
            {
                std::lock_guard<std::mutex> lock{ data_mutex };
                for (ClassInfo& c : parsed)
                {
                    if (c.internal_name.empty() || name_to_index.count(c.internal_name)) continue;
                    c.is_new = true;
                    c.seen_epoch = t;
                    name_to_index.emplace(c.internal_name, static_cast<int>(classes.size()));
                    prev_names_.insert(c.internal_name);   // a later full Rescan won't re-flag it
                    class_seen_[c.internal_name] = t;
                    hook_recent.push_back(c.internal_name);
                    classes.push_back(std::move(c));
                    ++added;
                }
                if (hook_recent.size() > 500)
                    hook_recent.erase(hook_recent.begin(), hook_recent.end() - 500);
                if (added > 0)
                {
                    classes_streamed.store(classes.size());
                    status_message = "+" + std::to_string(added) + " new class(es) via hook  ("
                                   + std::to_string(classes.size()) + " total)";
                }
            }
            if (added > 0)
            {
                last_added.fetch_add(added);
                hook_total.fetch_add(static_cast<std::uint64_t>(added));
            }
            status.store(Status::Done);
        }

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
                    // V <TAB> name <TAB> value [<TAB> owner [<TAB> refAddr]]  (later
                    // columns absent on older payloads — parse defensively).
                    const std::size_t t2{ line.find('\t', 2) };
                    if (t2 != std::string_view::npos)
                    {
                        InstField f;
                        f.name = std::string{ line.substr(2, t2 - 2) };
                        const std::size_t t3{ line.find('\t', t2 + 1) };
                        if (t3 == std::string_view::npos)
                        {
                            f.value = std::string{ line.substr(t2 + 1) };
                        }
                        else
                        {
                            f.value = std::string{ line.substr(t2 + 1, t3 - (t2 + 1)) };
                            const std::size_t t4{ line.find('\t', t3 + 1) };
                            if (t4 == std::string_view::npos)
                            {
                                f.owner = std::string{ line.substr(t3 + 1) };
                            }
                            else
                            {
                                f.owner    = std::string{ line.substr(t3 + 1, t4 - (t3 + 1)) };
                                f.ref_addr = std::string{ line.substr(t4 + 1) };
                            }
                        }
                        out.back().fields.push_back(std::move(f));
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

            // Stamp each instance's first-seen time (age source), keyed by heap
            // address.  NOTE: a moving GC relocates objects, so an address can
            // vanish + a new one appear for the same object — this is "first
            // OBSERVED by the viewer", best-effort, not a true creation time.
            {
                const double t{ now_s() };
                std::unordered_map<std::string, double> seen_next;
                seen_next.reserve(parsed.size());
                for (InstanceInfo& in : parsed)
                {
                    const auto it{ inst_seen_.find(in.address) };
                    const double e{ it != inst_seen_.end() ? it->second : t };
                    in.seen_epoch = e;
                    seen_next.emplace(in.address, e);
                }
                inst_seen_ = std::move(seen_next);
            }
            {
                std::lock_guard<std::mutex> lock{ data_mutex };
                instances = std::move(parsed);
                inst_message = "Found " + std::to_string(instances.size()) + " live instance(s).";
            }
            inst_status.store(Status::Done);
        }

        // Enumerate the JVM's class surface.  `inject` is true for the first
        // attach (loads the payload); false for a re-scan, which reuses the
        // already-loaded payload's serve loop (it reconnects to a fresh pipe) —
        // no CreateRemoteThread, so it's cheap enough to auto-repeat.
        void run_enumerate(std::uint32_t pid, std::wstring dll_path, bool inject)
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

            // 2) Inject the payload DLL (first attach only).
            if (inject)
            {
                std::string inject_error{};
                if (!inject_dll(pid, dll_path, inject_error))
                {
                    CloseHandle(pipe);
                    fail(inject_error);
                    return;
                }
            }

            status.store(Status::Receiving);
            set_status(inject ? "Waiting for the JVM to stream its class surface..."
                              : "Re-scanning loaded classes...");

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

            // Diff against the previous snapshot: flag runtime-loaded classes
            // (is_new) and collect ones that vanished (unloaded).  The first
            // attach has no baseline, so nothing is flagged then.
            const bool had_baseline{ has_baseline.load() };
            std::unordered_set<std::string> new_names;
            new_names.reserve(parsed.size());
            for (ClassInfo& c : parsed) new_names.insert(c.internal_name);
            int added{ 0 };
            std::vector<std::string> removed;
            if (had_baseline)
            {
                for (ClassInfo& c : parsed)
                    if (!prev_names_.count(c.internal_name)) { c.is_new = true; ++added; }
                for (const auto& old : prev_names_)
                    if (!new_names.count(old)) removed.push_back(old);
            }

            // Stamp each class's first-seen time (the age source): keep the prior
            // time if we've seen the name before, else now.  Baseline classes all
            // get the attach time (= "age since attach").  class_seen_ is touched
            // only on this worker thread, so it needs no lock.
            {
                const double t{ now_s() };
                std::unordered_map<std::string, double> seen_next;
                seen_next.reserve(parsed.size());
                for (ClassInfo& c : parsed)
                {
                    const auto it{ class_seen_.find(c.internal_name) };
                    const double e{ it != class_seen_.end() ? it->second : t };
                    c.seen_epoch = e;
                    seen_next.emplace(c.internal_name, e);
                }
                class_seen_ = std::move(seen_next);
            }

            {
                std::lock_guard<std::mutex> lock{ data_mutex };
                classes = std::move(parsed);
                name_to_index.clear();
                name_to_index.reserve(classes.size());
                for (int i = 0; i < (int)classes.size(); ++i)
                    name_to_index.emplace(classes[(std::size_t)i].internal_name, i);
                classes_streamed.store(classes.size());
                prev_names_ = std::move(new_names);
                last_removed = std::move(removed);
                if (had_baseline && (added > 0 || !last_removed.empty()))
                    status_message = "Re-scan: " + std::to_string(classes.size()) + " classes  (+"
                                   + std::to_string(added) + " new, -" + std::to_string(last_removed.size()) + ")";
                else
                    status_message = "Loaded " + std::to_string(classes.size()) + " classes.";
            }
            last_added.store(added);
            has_baseline.store(true);
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
