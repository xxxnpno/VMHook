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
//   DONE <TAB> <classCount>
// Methods/fields belong to the most recently emitted C record.  Access-flag
// fields are decimal JVM class-file flag words (older readers ignore extra
// trailing tab-separated fields, so appending the class flags stays compatible).

#include <vmhook/vmhook.hpp>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>

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

    // Serve every attach: the payload stays loaded and, each time a viewer/CLI
    // creates the pipe server, connects to it and re-streams the current class
    // surface.  This makes RE-ATTACH robust — a second attach just re-uses this
    // already-running server (LoadLibrary of an already-loaded DLL never re-runs
    // DllMain, so relying on re-injection to re-enumerate would hang).  Low
    // overhead: it sleeps between connect attempts and blocks in the OS while a
    // client is reading.  The payload only READS VM metadata (no hooks).
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
            stream_to(pipe);
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
