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
//   E <TAB> message                      (an error, for the SET requests)
//   DONE <TAB> <count>
// Methods/fields belong to the most recently emitted C record; V records to the
// most recent O.  Which stream is produced depends on the request the viewer
// leaves in %TEMP%\vmhook_viewer_req.txt before it opens the pipe:
//   "ENUM"          (or missing)  -> the full class surface (C/M/F).
//   "INST" <TAB> internal/Class/Name [<TAB> max]  -> live instances (O/V).
//   "STAT" <TAB> internal/Class/Name              -> static fields (one O + V's).
//   "SETI" <TAB> Class <TAB> 0xADDR <TAB> field <TAB> value -> write an instance
//                                                   field, then stream back V (re-read).
//   "SETS" <TAB> Class <TAB> field <TAB> value    -> write a static field, then V.
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

        // Declared instance (non-static) fields of k, then inherited ones by
        // walking the super chain — so the table shows an object's FULL state,
        // not just what its own class declares.  Each field remembers its
        // declaring klass so a shadowed name is read at the correct offset
        // (vmhook::find_field walks supers and would otherwise pick the subclass).
        // owner_tag = declaring class's simple name when the field is INHERITED
        // (declared by a super, not by k), else empty.  Precomputed once per
        // field (identical across instances).
        struct field_info { std::string name; std::string desc; vmhook::hotspot::klass* owner; std::string owner_tag; };
        std::vector<field_info> fields;
        int hops{ 0 };
        for (vmhook::hotspot::klass* kk{ k };
             kk && vmhook::hotspot::is_valid_pointer(kk) && hops < 64;
             kk = kk->get_super(), ++hops)
        {
            std::string tag;
            if (kk != k)
            {
                if (const vmhook::hotspot::symbol* const on{ kk->get_name() };
                    on && vmhook::hotspot::is_valid_pointer(on))
                {
                    const std::string full{ on->to_string() };
                    const std::size_t p{ full.find_last_of("/$") };
                    tag = (p == std::string::npos) ? full : full.substr(p + 1);
                }
            }
            for (const auto& [fname, fdesc, facc] : kk->collect_fields())
            {
                if ((facc & 0x0008u) == 0u)  // instance (non-static) fields only
                {
                    fields.push_back({ fname, fdesc, kk, tag });
                }
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
                writer.put(sanitize(read_field_value(oop, f.owner, f.name, f.desc)));
                writer.put("\t");
                writer.put(sanitize(f.owner_tag));  // empty = declared by the inspected class
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

    // Stream a class's STATIC field values.  Static fields live inside the class's
    // java.lang.Class mirror; the field offset is mirror-relative, so read_field_value
    // reads them by passing the mirror oop as the base (get_field adds the offset).
    // Streamed as one "instance" (the mirror) with a V record per static field.
    void stream_statics(HANDLE pipe, const std::string& classname)
    {
        pipe_writer writer{ pipe };
        vmhook::hotspot::klass* const k{ vmhook::find_class(classname) };
        if (!k || !vmhook::hotspot::is_valid_pointer(k))
        {
            writer.put("DONE\t0\n"); writer.flush(); FlushFileBuffers(pipe); return;
        }
        void* const mirror{ k->get_java_mirror() };
        if (!mirror || !vmhook::hotspot::is_valid_pointer(mirror))
        {
            writer.put("DONE\t0\n"); writer.flush(); FlushFileBuffers(pipe); return;
        }

        char addr[32];
        std::snprintf(addr, sizeof(addr), "0x%llX",
                      static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(mirror)));
        writer.put("O\t"); writer.put(addr); writer.put("\n");

        std::uint64_t count{ 0 };
        for (const auto& [fname, fdesc, facc] : k->collect_fields())
        {
            if ((facc & 0x0008u) != 0u)  // static fields only
            {
                writer.put("V\t");
                writer.put(sanitize(fname));
                writer.put("\t");
                writer.put(sanitize(read_field_value(mirror, k, fname, fdesc)));
                writer.put("\t");
                writer.put("\n");
                ++count;
            }
        }

        writer.put("DONE\t");
        writer.put(std::to_string(count));
        writer.put("\n");
        writer.flush();
        FlushFileBuffers(pipe);
    }

    // Find a declared or inherited field's descriptor + static flag by name,
    // walking k and its superchain (like stream_instances).  Returns true and
    // fills desc/is_static when found.
    auto find_field_desc(vmhook::hotspot::klass* const k, const std::string& name,
                         std::string& desc, bool& is_static) -> bool
    {
        int hops{ 0 };
        for (vmhook::hotspot::klass* kk{ k };
             kk && vmhook::hotspot::is_valid_pointer(kk) && hops < 64;
             kk = kk->get_super(), ++hops)
        {
            for (const auto& [fname, fdesc, facc] : kk->collect_fields())
            {
                if (fname == name)
                {
                    desc      = fdesc;
                    is_static = (facc & 0x0008u) != 0u;
                    return true;
                }
            }
        }
        return false;
    }

    // Parse `value` per the field descriptor and write it through vmhook's
    // fault-safe set_field (which resolves static-vs-instance + offset itself, so
    // `base` is the instance oop for instance fields and is ignored for statics).
    // Returns "" on success, else a human error message.
    auto apply_set(vmhook::hotspot::klass* const k, void* const base,
                   const std::string& name, const std::string& desc, const std::string& value) -> std::string
    {
        if (desc.empty())
        {
            return "unknown field descriptor";
        }
        const auto to_ll{ [](const std::string& v) { return std::strtoll(v.c_str(), nullptr, 0); } };
        switch (desc[0])
        {
        case 'Z':
        {
            const bool b{ value == "true" || value == "TRUE" || value == "1" };
            vmhook::set_field<std::uint8_t>(base, k, name, static_cast<std::uint8_t>(b ? 1 : 0));
            break;
        }
        case 'B': vmhook::set_field<std::int8_t>(base, k, name, static_cast<std::int8_t>(to_ll(value))); break;
        case 'C':
        {
            const std::uint16_t ch{ value.size() == 1
                ? static_cast<std::uint16_t>(static_cast<unsigned char>(value[0]))
                : static_cast<std::uint16_t>(std::strtoul(value.c_str(), nullptr, 0)) };
            vmhook::set_field<std::uint16_t>(base, k, name, ch);
            break;
        }
        case 'S': vmhook::set_field<std::int16_t>(base, k, name, static_cast<std::int16_t>(to_ll(value))); break;
        case 'I': vmhook::set_field<std::int32_t>(base, k, name, static_cast<std::int32_t>(to_ll(value))); break;
        case 'J': vmhook::set_field<std::int64_t>(base, k, name, static_cast<std::int64_t>(to_ll(value))); break;
        case 'F': vmhook::set_field<float>(base, k, name, std::strtof(value.c_str(), nullptr)); break;
        case 'D': vmhook::set_field<double>(base, k, name, std::strtod(value.c_str(), nullptr)); break;
        case 'L':
        case '[':
        {
            if (value == "null")
            {
                vmhook::set_field<std::uint32_t>(base, k, name, 0u);
                break;
            }
            if (desc == "Ljava/lang/String;")
            {
                void* const s{ vmhook::make_java_string(value) };
                if (!s || !vmhook::hotspot::is_valid_pointer(s))
                {
                    return "failed to allocate a java.lang.String for the new value";
                }
                vmhook::set_field<std::uint32_t>(base, k, name, vmhook::hotspot::encode_oop_pointer(s));
                break;
            }
            if (value.rfind("0x", 0) == 0 || value.rfind("0X", 0) == 0)
            {
                void* const ref{ reinterpret_cast<void*>(
                    static_cast<std::uintptr_t>(std::strtoull(value.c_str(), nullptr, 16))) };
                if (ref && !vmhook::hotspot::is_valid_pointer(ref))
                {
                    return "the target oop address is not a readable pointer";
                }
                vmhook::set_field<std::uint32_t>(base, k, name, vmhook::hotspot::encode_oop_pointer(ref));
                break;
            }
            return "reference field: pass 'null', a 0x<oop> address, or (for String) the literal text";
        }
        default: return "unsupported field type";
        }
        return "";
    }

    // Write one field on a live instance (SETI) or a class's statics (SETS), then
    // stream the re-read value back so the caller sees the applied result.
    //   SETI  arg = "<class>\t<0xADDR>\t<field>\t<value>"
    //   SETS  arg = "<class>\t<field>\t<value>"
    void stream_set(HANDLE pipe, const std::string& arg, const bool is_instance)
    {
        pipe_writer writer{ pipe };
        const auto bail{ [&](const std::string& msg)
        {
            writer.put("E\t"); writer.put(sanitize(msg)); writer.put("\n");
            writer.put("DONE\t0\n"); writer.flush(); FlushFileBuffers(pipe);
        } };

        // Split into fields; the value is everything after the last structural tab
        // so it may itself contain tabs.
        std::vector<std::string> parts;
        const std::size_t need{ is_instance ? 3u : 2u };  // structural tabs before the value
        std::size_t pos{ 0 };
        for (std::size_t i = 0; i < need; ++i)
        {
            const std::size_t tab{ arg.find('\t', pos) };
            if (tab == std::string::npos) { bail("malformed set request"); return; }
            parts.push_back(arg.substr(pos, tab - pos));
            pos = tab + 1;
        }
        parts.push_back(arg.substr(pos));  // the value (remainder)

        const std::string& classname{ parts[0] };
        const std::string& field{ is_instance ? parts[2] : parts[1] };
        const std::string& value{ is_instance ? parts[3] : parts[2] };

        vmhook::hotspot::klass* const k{ vmhook::find_class(classname) };
        if (!k || !vmhook::hotspot::is_valid_pointer(k)) { bail("class not found: " + classname); return; }

        std::string desc; bool is_static{ false };
        if (!find_field_desc(k, field, desc, is_static)) { bail("field not found: " + field); return; }

        void* base{ nullptr };
        if (is_instance)
        {
            if (is_static) { bail("'" + field + "' is a static field — use set-static"); return; }
            base = reinterpret_cast<void*>(
                static_cast<std::uintptr_t>(std::strtoull(parts[1].c_str(), nullptr, 16)));
            if (!base || !vmhook::hotspot::is_valid_pointer(base)) { bail("invalid instance address: " + parts[1]); return; }
        }
        else
        {
            if (!is_static) { bail("'" + field + "' is an instance field — use set-instance"); return; }
            base = k->get_java_mirror();
            if (!base || !vmhook::hotspot::is_valid_pointer(base)) { bail("could not resolve the class mirror"); return; }
        }

        const std::string err{ apply_set(k, base, field, desc, value) };
        if (!err.empty()) { bail(err); return; }

        // Re-read so the response reflects what actually landed.
        writer.put("V\t");
        writer.put(sanitize(field));
        writer.put("\t");
        writer.put(sanitize(read_field_value(base, k, field, desc)));
        writer.put("\n");
        writer.put("DONE\t1\n");
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
            else if (req.rfind("STAT\t", 0) == 0)
            {
                stream_statics(pipe, req.substr(5));
            }
            else if (req.rfind("SETI\t", 0) == 0)
            {
                stream_set(pipe, req.substr(5), /*is_instance=*/true);
            }
            else if (req.rfind("SETS\t", 0) == 0)
            {
                stream_set(pipe, req.substr(5), /*is_instance=*/false);
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
