// vmhook_cli — headless JSON front-end to the vmhook viewer, so an MCP server
// (or any script) can drive it.
//
//   vmhook_cli list                     -> [{pid,image,cmdline}, ...]
//   vmhook_cli enumerate <pid>          -> inject + read; cache; {pid,classes,methods,fields}
//   vmhook_cli classes  <pid> [substr]  -> [class internal names] (from cache)
//   vmhook_cli class    <pid> <name>    -> {name, methods:[...], fields:[...]} (from cache)
//
// `enumerate` caches the raw stream to %TEMP%\vmhook_mcp_<pid>.txt so the other
// queries are cheap and don't re-inject.  All output is JSON on stdout.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "app.hpp"
#include "descriptor.hpp"

namespace
{
    auto json_escape(std::string_view s) -> std::string
    {
        std::string out;
        out.reserve(s.size() + 8);
        for (const char ch : s)
        {
            switch (ch)
            {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20)
                {
                    char buf[8]; std::snprintf(buf, sizeof(buf), "\\u%04x", ch); out += buf;
                }
                else { out += ch; }
            }
        }
        return out;
    }

    auto cache_path(std::uint32_t pid) -> std::string
    {
        const char* tmp{ std::getenv("TEMP") };
        return std::string{ tmp ? tmp : "." } + "\\vmhook_mcp_" + std::to_string(pid) + ".txt";
    }

    auto widen(const char* utf8) -> std::wstring
    {
        const int n{ MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0) };
        std::wstring w(static_cast<std::size_t>(n), 0);
        MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w.data(), n);
        if (!w.empty() && w.back() == 0) w.pop_back();
        return w;
    }

    // Inject the payload and read the raw C/M/F stream into `raw`.  Overlapped
    // I/O with timeouts so a failed attach reports an error instead of hanging.
    auto enumerate_raw(std::uint32_t pid, const std::wstring& dll, std::string& raw, std::string& err) -> bool
    {
        HANDLE pipe{ CreateNamedPipeW(viewer::k_pipe_name, PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 0, 1u << 20, 0, nullptr) };
        if (pipe == INVALID_HANDLE_VALUE) { err = "CreateNamedPipe failed"; return false; }
        if (!viewer::inject_dll(pid, dll, err)) { CloseHandle(pipe); return false; }

        HANDLE ev{ CreateEventW(nullptr, TRUE, FALSE, nullptr) };
        OVERLAPPED ov{}; ov.hEvent = ev;
        const BOOL connected{ ConnectNamedPipe(pipe, &ov) };
        const DWORD ce{ GetLastError() };
        if (!connected && ce == ERROR_IO_PENDING)
        {
            if (WaitForSingleObject(ev, 30000) != WAIT_OBJECT_0)
            { err = "timed out waiting for the payload to connect"; CloseHandle(ev); CloseHandle(pipe); return false; }
        }
        else if (!connected && ce != ERROR_PIPE_CONNECTED)
        { err = "ConnectNamedPipe failed (" + std::to_string(ce) + ")"; CloseHandle(ev); CloseHandle(pipe); return false; }

        char buf[65536];
        for (;;)
        {
            ResetEvent(ev);
            OVERLAPPED rov{}; rov.hEvent = ev;
            DWORD rd{ 0 };
            const BOOL ok{ ReadFile(pipe, buf, sizeof(buf), &rd, &rov) };
            if (!ok && GetLastError() == ERROR_IO_PENDING)
            {
                if (WaitForSingleObject(ev, 30000) != WAIT_OBJECT_0) break;
                if (!GetOverlappedResult(pipe, &rov, &rd, FALSE)) break;
            }
            else if (!ok) break;
            if (rd == 0) break;
            raw.append(buf, rd);
        }
        CloseHandle(ev);
        CloseHandle(pipe);
        return true;
    }

    // Parse the raw stream into the viewer model.
    auto parse(const std::string& raw) -> std::vector<viewer::ClassInfo>
    {
        std::vector<viewer::ClassInfo> out;
        std::size_t p{ 0 };
        while (p < raw.size())
        {
            std::size_t nl{ raw.find('\n', p) };
            if (nl == std::string::npos) nl = raw.size();
            std::string_view line{ raw.data() + p, nl - p };
            p = nl + 1;
            if (line.size() < 2 || line[1] != '\t') continue;
            std::vector<std::string_view> parts;
            std::size_t q{ 2 };
            while (q <= line.size())
            {
                const std::size_t tab{ line.find('\t', q) };
                if (tab == std::string_view::npos) { parts.push_back(line.substr(q)); break; }
                parts.push_back(line.substr(q, tab - q)); q = tab + 1;
            }
            switch (line[0])
            {
            case 'C':
                if (!parts.empty())
                {
                    viewer::ClassInfo c;
                    c.internal_name.assign(parts[0]);
                    const std::size_t s{ c.internal_name.find_last_of('/') };
                    c.package     = (s == std::string::npos) ? "" : c.internal_name.substr(0, s);
                    c.simple_name = (s == std::string::npos) ? c.internal_name : c.internal_name.substr(s + 1);
                    out.push_back(std::move(c));
                }
                break;
            case 'M':
                if (parts.size() >= 2 && !out.empty())
                    out.back().methods.push_back({ std::string{ parts[0] }, std::string{ parts[1] } });
                break;
            case 'F':
                if (parts.size() >= 3 && !out.empty())
                    out.back().fields.push_back({ std::string{ parts[0] }, std::string{ parts[1] }, parts[2] == "1" });
                break;
            default: break;
            }
        }
        return out;
    }

    auto load_cache(std::uint32_t pid) -> std::string
    {
        std::ifstream in{ cache_path(pid), std::ios::binary };
        std::stringstream ss; ss << in.rdbuf();
        return ss.str();
    }

    auto icontains(const std::string& h, const std::string& n) -> bool
    {
        if (n.empty()) return true;
        std::string hl{ h }, nl{ n };
        for (char& c : hl) c = (char)std::tolower((unsigned char)c);
        for (char& c : nl) c = (char)std::tolower((unsigned char)c);
        return hl.find(nl) != std::string::npos;
    }
}

int main(int argc, char** argv)
{
    const std::string cmd{ argc > 1 ? argv[1] : "" };

    if (cmd == "list")
    {
        auto jvms{ viewer::enumerate_jvms() };
        std::printf("[");
        for (std::size_t i = 0; i < jvms.size(); ++i)
        {
            std::printf("%s{\"pid\":%u,\"image\":\"%s\",\"cmdline\":\"%s\"}",
                i ? "," : "", jvms[i].pid,
                json_escape(jvms[i].image_name).c_str(),
                json_escape(jvms[i].command_line.empty() ? jvms[i].image_path : jvms[i].command_line).c_str());
        }
        std::printf("]\n");
        return 0;
    }

    if (cmd == "enumerate" && argc >= 3)
    {
        const std::uint32_t pid{ (std::uint32_t)std::strtoul(argv[2], nullptr, 10) };
        std::wstring dll;
        if (argc >= 4) dll = widen(argv[3]);
        else
        {
            wchar_t exe[MAX_PATH]{}; GetModuleFileNameW(nullptr, exe, MAX_PATH);
            std::wstring w{ exe }; const std::size_t s{ w.find_last_of(L"\\/") };
            if (s != std::wstring::npos) w.resize(s + 1);
            dll = w + L"vmhook_payload.dll";
        }
        std::string raw, err;
        if (!enumerate_raw(pid, dll, raw, err))
        { std::printf("{\"error\":\"%s\"}\n", json_escape(err).c_str()); return 1; }
        { std::ofstream out{ cache_path(pid), std::ios::binary | std::ios::trunc }; out << raw; }
        auto classes{ parse(raw) };
        std::size_t nm{ 0 }, nf{ 0 };
        for (const auto& c : classes) { nm += c.methods.size(); nf += c.fields.size(); }
        std::printf("{\"pid\":%u,\"classes\":%zu,\"methods\":%zu,\"fields\":%zu}\n", pid, classes.size(), nm, nf);
        return 0;
    }

    if (cmd == "classes" && argc >= 3)
    {
        const std::uint32_t pid{ (std::uint32_t)std::strtoul(argv[2], nullptr, 10) };
        const std::string filter{ argc >= 4 ? argv[3] : "" };
        const std::string raw{ load_cache(pid) };
        if (raw.empty()) { std::printf("{\"error\":\"no cache for pid %u; run enumerate first\"}\n", pid); return 1; }
        auto classes{ parse(raw) };
        std::printf("[");
        bool first{ true };
        for (const auto& c : classes)
        {
            if (!icontains(c.internal_name, filter)) continue;
            std::printf("%s\"%s\"", first ? "" : ",", json_escape(c.internal_name).c_str());
            first = false;
        }
        std::printf("]\n");
        return 0;
    }

    if (cmd == "class" && argc >= 4)
    {
        const std::uint32_t pid{ (std::uint32_t)std::strtoul(argv[2], nullptr, 10) };
        const std::string name{ argv[3] };
        const std::string raw{ load_cache(pid) };
        if (raw.empty()) { std::printf("{\"error\":\"no cache for pid %u; run enumerate first\"}\n", pid); return 1; }
        auto classes{ parse(raw) };
        for (const auto& c : classes)
        {
            if (c.internal_name != name) continue;
            std::printf("{\"name\":\"%s\",\"methods\":[", json_escape(c.internal_name).c_str());
            for (std::size_t i = 0; i < c.methods.size(); ++i)
                std::printf("%s{\"name\":\"%s\",\"descriptor\":\"%s\",\"signature\":\"%s\"}",
                    i ? "," : "", json_escape(c.methods[i].name).c_str(),
                    json_escape(c.methods[i].descriptor).c_str(),
                    json_escape(viewer::pretty_method(c.methods[i].descriptor)).c_str());
            std::printf("],\"fields\":[");
            for (std::size_t i = 0; i < c.fields.size(); ++i)
                std::printf("%s{\"name\":\"%s\",\"descriptor\":\"%s\",\"type\":\"%s\",\"static\":%s}",
                    i ? "," : "", json_escape(c.fields[i].name).c_str(),
                    json_escape(c.fields[i].descriptor).c_str(),
                    json_escape(viewer::pretty_field(c.fields[i].descriptor)).c_str(),
                    c.fields[i].is_static ? "true" : "false");
            std::printf("]}\n");
            return 0;
        }
        std::printf("{\"error\":\"class not found: %s\"}\n", json_escape(name).c_str());
        return 1;
    }

    std::printf("{\"error\":\"usage: vmhook_cli list | enumerate <pid> | classes <pid> [substr] | class <pid> <name>\"}\n");
    return 2;
}
