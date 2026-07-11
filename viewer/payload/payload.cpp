// vmhook viewer payload DLL.
//
// Injected (LoadLibrary via CreateRemoteThread) into a live HotSpot JVM by the
// vmhook viewer.  It uses vmhook — pure-VM, no JNI/JVMTI — to walk every loaded
// Java class and, per class, enumerate its declared methods and fields, then
// streams the whole surface back to the viewer over a named pipe.
//
// Wire protocol (UTF-8, '\n'-delimited, '\t'-separated; Java identifiers and JVM
// descriptors never contain '\t' or '\n', so the framing is unambiguous):
//   C <TAB> internal/Class/Name
//   M <TAB> methodName <TAB> (descriptor)ret
//   F <TAB> fieldName  <TAB> descriptor <TAB> {0|1 static}
//   DONE <TAB> <classCount>
// Methods/fields belong to the most recently emitted C record.

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

    auto connect_pipe() -> HANDLE
    {
        // The viewer creates the pipe server before injecting, but injection and
        // process warm-up race, so retry for a few seconds.
        for (int attempt{ 0 }; attempt < 150; ++attempt)
        {
            HANDLE handle{ CreateFileW(k_pipe_name, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr) };
            if (handle != INVALID_HANDLE_VALUE)
            {
                return handle;
            }
            if (GetLastError() == ERROR_PIPE_BUSY)
            {
                WaitNamedPipeW(k_pipe_name, 2000);
            }
            else
            {
                Sleep(100);
            }
        }
        return INVALID_HANDLE_VALUE;
    }

    void enumerate_and_stream()
    {
        HANDLE pipe{ connect_pipe() };
        if (pipe == INVALID_HANDLE_VALUE)
        {
            return;
        }

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
                writer.put("\n");

                // Declared methods: (name, JVM descriptor).
                for (const auto& [method_name, descriptor] : vmhook::detail::collect_klass_methods(klass))
                {
                    writer.put("M\t");
                    writer.put(method_name);
                    writer.put("\t");
                    writer.put(descriptor);
                    writer.put("\n");
                }

                // Declared fields: (name, JVM descriptor, is_static).
                for (const auto& [field_name, field_descriptor, is_static] : klass->collect_fields())
                {
                    writer.put("F\t");
                    writer.put(field_name);
                    writer.put("\t");
                    writer.put(field_descriptor);
                    writer.put(is_static ? "\t1\n" : "\t0\n");
                }
            });

        writer.put("DONE\t");
        writer.put(std::to_string(class_count));
        writer.put("\n");
        writer.flush();

        FlushFileBuffers(pipe);
        CloseHandle(pipe);
    }
}

extern "C" BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);
        // Do the walk off the loader lock on a detached worker thread.
        std::thread(enumerate_and_stream).detach();
    }
    return TRUE;
}
