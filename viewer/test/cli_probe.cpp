// Headless end-to-end probe: create the pipe, inject the payload into <pid>,
// read the streamed class surface, print a summary.  Validates the payload +
// pipe + vmhook pure-VM enumeration without the GUI.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "app.hpp"
#include <cstdio>
#include <string>
#include <string_view>

int main(int argc, char** argv)
{
    if (argc < 3) { std::printf("usage: cli_probe <pid> <payload_dll_path>\n"); return 2; }
    const std::uint32_t pid{ std::strtoul(argv[1], nullptr, 10) };
    const int wn{ MultiByteToWideChar(CP_UTF8, 0, argv[2], -1, nullptr, 0) };
    std::wstring dll(static_cast<std::size_t>(wn), 0);
    MultiByteToWideChar(CP_UTF8, 0, argv[2], -1, dll.data(), wn);
    if (!dll.empty() && dll.back() == 0) dll.pop_back();

    HANDLE pipe{ CreateNamedPipeW(viewer::k_pipe_name, PIPE_ACCESS_INBOUND,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 0, 1u << 20, 0, nullptr) };
    if (pipe == INVALID_HANDLE_VALUE) { std::printf("CreateNamedPipe fail %lu\n", GetLastError()); return 1; }

    std::printf("target cmdline: %s\n", viewer::resolve_command_line(pid).c_str());

    std::string err;
    if (!viewer::inject_dll(pid, dll, err)) { std::printf("inject fail: %s\n", err.c_str()); return 1; }
    std::printf("injected into pid %u; waiting for stream...\n", pid);

    if (!ConnectNamedPipe(pipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED)
    { std::printf("ConnectNamedPipe fail %lu\n", GetLastError()); return 1; }

    std::string acc; char buf[65536]; DWORD rd{ 0 };
    std::uint64_t classes{ 0 }, methods{ 0 }, fields{ 0 }; std::string sample;
    while (ReadFile(pipe, buf, sizeof(buf), &rd, nullptr) && rd > 0)
    {
        acc.append(buf, rd);
        std::size_t p{ 0 }, nl;
        while ((nl = acc.find('\n', p)) != std::string::npos)
        {
            std::string_view line{ acc.data() + p, nl - p };
            if (!line.empty())
            {
                if (line[0] == 'C') { ++classes; if (classes <= 8) sample += std::string(line.substr(2)) + "\n"; }
                else if (line[0] == 'M') ++methods;
                else if (line[0] == 'F') ++fields;
            }
            p = nl + 1;
        }
        acc.erase(0, p);
    }
    std::printf("RESULT classes=%llu methods=%llu fields=%llu\nfirst classes:\n%s",
        (unsigned long long)classes, (unsigned long long)methods, (unsigned long long)fields, sample.c_str());
    CloseHandle(pipe);
    return classes > 0 ? 0 : 3;
}
