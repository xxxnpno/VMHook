// vmhook viewer payload DLL.
//
// Injected (LoadLibrary via CreateRemoteThread) into a live HotSpot JVM by the
// vmhook viewer.  It uses vmhook — pure-VM, no JNI/JVMTI — to walk every loaded
// Java class and, per class, enumerate its declared methods and fields, then
// streams the whole surface back to the viewer over a named pipe.
//
// Wire protocol (UTF-8, '\n'-delimited, '\t'-separated; Java identifiers and JVM
// descriptors never contain '\t' or '\n', so the framing is unambiguous):
//   C <TAB> internal/Class/Name <TAB> super/Internal/Name <TAB> <classAccessFlags>
//   M <TAB> methodName <TAB> (descriptor)ret <TAB> <methodAccessFlags>
//   F <TAB> fieldName  <TAB> descriptor <TAB> <fieldAccessFlags>
//   O <TAB> 0x<addr>                      (start of a live instance, its oop address)
//   V <TAB> fieldName <TAB> value        (a field value of the current instance)
//   DONE <TAB> <count>
// Methods/fields belong to the most recently emitted C record; V records to the
// most recent O.  Which stream is produced depends on the request the viewer
// leaves in %TEMP%\vmhook_viewer_req.txt before it opens the pipe:
//   "ENUM"          (or missing)  -> the full class surface (C/M/F).
//   "INST" <TAB> internal/Class/Name  -> live instances of that class (O/V).
// Access-flag fields are decimal JVM class-file flag words.

#include <vmhook/vmhook.hpp>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

namespace
{
    constexpr const wchar_t* k_pipe_name{ L"\\\\.\\pipe\\vmhook_viewer" };

    // Buffered pipe writer: accumulates records and flushes in ~64 KB chunks so
    // enumerating tens of thousands of classes doesn't do a syscall per line.
    struct pipe_writer
    {
        HANDLE      pipe{ INVALID_HANDLE_VALUE };
        std::string buffer{};

        void put(std::string_view text)
        {
            buffer.append(text);
            if (buffer.size() >= 64u * 1024u)
            {
                flush();
            }
        }

        void flush()
        {
            if (pipe == INVALID_HANDLE_VALUE || buffer.empty())
            {
                return;
            }
            const char* cursor{ buffer.data() };
            std::size_t remaining{ buffer.size() };
            while (remaining > 0u)
            {
                DWORD written{ 0 };
                if (!WriteFile(pipe, cursor, static_cast<DWORD>(remaining), &written, nullptr) || written == 0u)
                {
                    break;
                }
                cursor    += written;
                remaining -= written;
            }
            buffer.clear();
        }
    };

    void stream_to(HANDLE pipe)
    {
        pipe_writer writer{ pipe };
        std::uint64_t class_count{ 0 };

        // The ClassLoaderDataGraph walk can surface the same class name more than
        // once (bootstrap dictionary + _klasses list, distinct Klass* per node).
        // Dedup by internal name for a clean, unique class list.
        std::unordered_set<std::string> seen{};

        vmhook::for_each_loaded_class(
            [&](const std::string& name, vmhook::hotspot::klass* const klass)
            {
                if (!klass || !vmhook::hotspot::is_valid_pointer(klass) || name.empty())
                {
                    return;
                }
                if (!seen.insert(name).second)
                {
                    return;
                }
                ++class_count;

                writer.put("C\t");
                writer.put(name);
                writer.put("\t");
                // Superclass internal name (empty for java/lang/Object / interfaces).
                if (vmhook::hotspot::klass* const super_klass{ klass->get_super() };
                    super_klass && vmhook::hotspot::is_valid_pointer(super_klass))
                {
                    if (const vmhook::hotspot::symbol* const super_name{ super_klass->get_name() };
                        super_name && vmhook::hotspot::is_valid_pointer(super_name))
                    {
                        writer.put(super_name->to_string());
                    }
                }
                writer.put("\t");
                // Class-file access flags (interface/enum/abstract/... kind + vis).
                writer.put(std::to_string(klass->get_class_access_flags()));
                writer.put("\n");

                // Declared methods: (name, descriptor, access flags).  Array
                // klasses have no _methods array, so skip them ('[' names).
                if (name.empty() || name.front() != '[')
                {
                    const std::int32_t method_count{ klass->get_methods_count() };
                    vmhook::hotspot::method** const methods{ klass->get_methods_ptr() };
                    if (methods && method_count > 0)
                    {
                        for (std::int32_t mi = 0; mi < method_count; ++mi)
                        {
                            vmhook::hotspot::method* const m{ methods[mi] };
                            if (!m || !vmhook::hotspot::is_valid_pointer(m))
                            {
                                continue;
                            }
                            std::uint32_t flags{ 0 };
                            if (std::uint32_t* const fp{ m->get_access_flags() })
                            {
                                vmhook::os::safe_read(&flags, fp, sizeof(flags));
                            }
                            writer.put("M\t");
                            writer.put(m->get_name());
                            writer.put("\t");
                            writer.put(m->get_signature());
                            writer.put("\t");
                            writer.put(std::to_string(flags));
                            writer.put("\n");
                        }
                    }
                }

                // Declared fields: (name, descriptor, access flags).
                for (const auto& [field_name, field_descriptor, access_flags] : klass->collect_fields())
                {
                    writer.put("F\t");
                    writer.put(field_name);
                    writer.put("\t");
                    writer.put(field_descriptor);
                    writer.put("\t");
                    writer.put(std::to_string(access_flags));
                    writer.put("\n");
                }
            });

        writer.put("DONE\t");
        writer.put(std::to_string(class_count));
        writer.put("\n");
        writer.flush();
        FlushFileBuffers(pipe);
    }

    // Read (and consume) the one-line request the viewer left in
    // %TEMP%\vmhook_viewer_req.txt.  Missing / empty -> "ENUM", so the CLI/MCP
    // (which never writes one) and every plain attach keep getting the class
    // surface.  The file is deleted after reading so a stale INST request can't
    // leak into the next enumerate.
    auto read_request() -> std::string
    {
        wchar_t tmp[MAX_PATH]{};
        const DWORD n{ GetTempPathW(MAX_PATH, tmp) };
        if (n == 0 || n >= MAX_PATH)
        {
            return "ENUM";
        }
        std::wstring path{ tmp, n };
        path += L"vmhook_viewer_req.txt";

        HANDLE f{ CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, 0, nullptr) };
        if (f == INVALID_HANDLE_VALUE)
        {
            return "ENUM";
        }
        std::string req{};
        char buf[1024];
        DWORD rd{ 0 };
        while (ReadFile(f, buf, sizeof(buf), &rd, nullptr) && rd > 0)
        {
            req.append(buf, rd);
        }
        CloseHandle(f);
        DeleteFileW(path.c_str());

        while (!req.empty() && (req.back() == '\n' || req.back() == '\r' || req.back() == ' '))
        {
            req.pop_back();
        }
        return req.empty() ? std::string{ "ENUM" } : req;
    }

    // Tab/newline are legal in some obfuscated identifiers and in String values;
    // they'd corrupt the tab/newline framing, so flatten them to spaces (this is
    // display data, so lossy sanitising is fine).
    auto sanitize(std::string s) -> std::string
    {
        for (char& c : s)
        {
            if (c == '\t' || c == '\n' || c == '\r')
            {
                c = ' ';
            }
        }
        return s;
    }

    // Read one instance field's value and format it for display.  All reads go
    // through the fault-safe vmhook accessors (get_field returns a default on any
    // failure), so a stale/garbage oop from the conservative scan can't fault.
    auto read_field_value(void* const oop, vmhook::hotspot::klass* const k,
                          const std::string& name, const std::string& desc) -> std::string
    {
        if (desc.empty())
        {
            return "?";
        }
        switch (desc[0])
        {
        case 'Z': return vmhook::get_field<std::uint8_t>(oop, k, name) != 0 ? "true" : "false";
        case 'B': return std::to_string(static_cast<int>(vmhook::get_field<std::int8_t>(oop, k, name)));
        case 'C':
        {
            const std::uint16_t ch{ vmhook::get_field<std::uint16_t>(oop, k, name) };
            if (ch >= 32u && ch < 127u) { return std::string{ "'" } + static_cast<char>(ch) + "'"; }
            return std::to_string(static_cast<unsigned>(ch));
        }
        case 'S': return std::to_string(static_cast<int>(vmhook::get_field<std::int16_t>(oop, k, name)));
        case 'I': return std::to_string(vmhook::get_field<std::int32_t>(oop, k, name));
        case 'F': return std::to_string(vmhook::get_field<float>(oop, k, name));
        case 'J': return std::to_string(vmhook::get_field<std::int64_t>(oop, k, name));
        case 'D': return std::to_string(vmhook::get_field<double>(oop, k, name));
        case 'L':
        case '[':
        {
            // Reference field: read the (compressed) oop, decode, and describe it.
            const std::uint32_t compressed{ vmhook::get_field<std::uint32_t>(oop, k, name) };
            if (compressed == 0u)
            {
                return "null";
            }
            void* const ref{ vmhook::hotspot::decode_oop_pointer(compressed) };
            if (!ref || !vmhook::hotspot::is_valid_pointer(ref))
            {
                return "null";
            }
            if (desc == "Ljava/lang/String;")
            {
                return std::string{ "\"" } + vmhook::read_java_string(ref) + "\"";
            }
            if (vmhook::hotspot::klass* const rk{ vmhook::klass_from_oop(ref) };
                rk && vmhook::hotspot::is_valid_pointer(rk))
            {
                if (const vmhook::hotspot::symbol* const rn{ rk->get_name() };
                    rn && vmhook::hotspot::is_valid_pointer(rn))
                {
                    return std::string{ "<" } + rn->to_string() + ">";
                }
            }
            return "<object>";
        }
        default: return "?";
        }
    }

    // Stream up to `max` live instances of `arg` ("classname" or
    // "classname\t<max>"), each with its declared instance (non-static) field
    // values.  `max` is viewer-driven (the scan-cap control); default 200.
    void stream_instances(HANDLE pipe, const std::string& arg)
    {
        std::string classname{ arg };
        std::size_t max_scan{ 200 };
        if (const std::size_t tab{ arg.find('\t') }; tab != std::string::npos)
        {
            classname = arg.substr(0, tab);
            if (const unsigned long long m{ std::strtoull(arg.c_str() + tab + 1, nullptr, 10) }; m > 0ull)
            {
                max_scan = static_cast<std::size_t>(m);
            }
        }

        pipe_writer writer{ pipe };
        vmhook::hotspot::klass* const k{ vmhook::find_class(classname) };
        if (!k || !vmhook::hotspot::is_valid_pointer(k))
        {
            writer.put("DONE\t0\n");
            writer.flush();
            FlushFileBuffers(pipe);
            return;
        }

        struct field_info { std::string name; std::string desc; };
        std::vector<field_info> fields;
        for (const auto& [fname, fdesc, facc] : k->collect_fields())
        {
            if ((facc & 0x0008u) == 0u)  // instance (non-static) fields only
            {
                fields.push_back({ fname, fdesc });
            }
        }

        std::uint64_t count{ 0 };
        vmhook::for_each_instance_of(k, [&](void* const oop)
        {
            // O <TAB> 0x<heap address of the instance oop>
            char addr[32];
            std::snprintf(addr, sizeof(addr), "0x%llX",
                          static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(oop)));
            writer.put("O\t");
            writer.put(addr);
            writer.put("\n");
            for (const field_info& f : fields)
            {
                writer.put("V\t");
                writer.put(sanitize(f.name));
                writer.put("\t");
                writer.put(sanitize(read_field_value(oop, k, f.name, f.desc)));
                writer.put("\n");
            }
            ++count;
        }, max_scan);

        writer.put("DONE\t");
        writer.put(std::to_string(count));
        writer.put("\n");
        writer.flush();
        FlushFileBuffers(pipe);
    }

    // Serve every attach: the payload stays loaded and, each time a viewer/CLI
    // creates the pipe server, connects to it and streams whatever the current
    // request asks for (class surface by default, or live instances).  This makes
    // RE-ATTACH robust — a second attach just re-uses this already-running server
    // (LoadLibrary of an already-loaded DLL never re-runs DllMain, so relying on
    // re-injection would hang).  The payload only READS VM metadata (no hooks).
    void serve_forever()
    {
        for (;;)
        {
            HANDLE pipe{ CreateFileW(k_pipe_name, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr) };
            if (pipe == INVALID_HANDLE_VALUE)
            {
                if (GetLastError() == ERROR_PIPE_BUSY)
                {
                    WaitNamedPipeW(k_pipe_name, 2000);
                }
                else
                {
                    Sleep(250);  // no server up right now; wait for the next attach
                }
                continue;
            }
            const std::string req{ read_request() };
            if (req.rfind("INST\t", 0) == 0)
            {
                stream_instances(pipe, req.substr(5));
            }
            else
            {
                stream_to(pipe);
            }
            CloseHandle(pipe);
        }
    }
}

extern "C" BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);
        // One detached server thread handles this attach and every future one.
        std::thread(serve_forever).detach();
    }
    return TRUE;
}
