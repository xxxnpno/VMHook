// vmhook_cli — headless JSON front-end to the vmhook viewer, so an MCP server
// (or any script) can drive it.
//
//   vmhook_cli list                     -> [{pid,image,cmdline}, ...]
//   vmhook_cli enumerate <pid>          -> inject + read; cache; {pid,classes,methods,fields}
//   vmhook_cli classes  <pid> [substr]  -> [class internal names] (from cache)
//   vmhook_cli class    <pid> <name>    -> {name, methods:[...], fields:[...]} (from cache)
//   vmhook_cli instances <pid> <class> [cap] -> {pid,class,instances:[{address,fields:{...}}]} (live heap)
//   vmhook_cli statics  <pid> <class>   -> {pid,class,statics:{name:value, ...}} (from the class mirror)
//
// `enumerate` caches the raw stream to %TEMP%\vmhook_mcp_<pid>.txt so the class/
// classes queries are cheap and don't re-inject.  `instances` reads the live heap
// (no cache).  All output is JSON on stdout.

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

    // Leave a one-line request for the payload (read + deleted on its next
    // connect): "ENUM" (class surface) or "INST\t<class>\t<max>" (live instances).
    void write_request(const std::string& req)
    {
        wchar_t tmp[MAX_PATH]{};
        const DWORD n{ GetTempPathW(MAX_PATH, tmp) };
        if (n == 0 || n >= MAX_PATH) return;
        std::wstring path{ tmp, n }; path += L"vmhook_viewer_req.txt";
        HANDLE f{ CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr) };
        if (f == INVALID_HANDLE_VALUE) return;
        DWORD wn{ 0 }; WriteFile(f, req.data(), static_cast<DWORD>(req.size()), &wn, nullptr); CloseHandle(f);
    }

    // Inject the payload and read the raw stream into `raw`.  Overlapped I/O with
    // timeouts so a failed attach reports an error instead of hanging.  `request`
    // selects what the payload streams (the class surface by default).
    auto enumerate_raw(std::uint32_t pid, const std::wstring& dll, std::string& raw, std::string& err,
                       const std::string& request = "ENUM") -> bool
    {
        write_request(request);
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
                    if (parts.size() >= 2) c.super_name.assign(parts[1]);
                    if (parts.size() >= 3)
                        c.access = (std::uint16_t)std::strtoul(std::string{ parts[2] }.c_str(), nullptr, 10);
                    const std::size_t s{ c.internal_name.find_last_of('/') };
                    c.package     = (s == std::string::npos) ? "" : c.internal_name.substr(0, s);
                    c.simple_name = (s == std::string::npos) ? c.internal_name : c.internal_name.substr(s + 1);
                    out.push_back(std::move(c));
                }
                break;
            case 'M':
                if (parts.size() >= 2 && !out.empty())
                {
                    const std::uint16_t acc{ parts.size() >= 3
                        ? (std::uint16_t)std::strtoul(std::string{ parts[2] }.c_str(), nullptr, 10) : (std::uint16_t)0 };
                    out.back().methods.push_back({ std::string{ parts[0] }, std::string{ parts[1] }, acc });
                }
                break;
            case 'F':
                if (parts.size() >= 3 && !out.empty())
                {
                    const std::uint16_t acc{ (std::uint16_t)std::strtoul(std::string{ parts[2] }.c_str(), nullptr, 10) };
                    out.back().fields.push_back({ std::string{ parts[0] }, std::string{ parts[1] }, acc, (acc & 0x0008u) != 0 });
                }
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

    if (cmd == "instances" && argc >= 4)
    {
        const std::uint32_t pid{ (std::uint32_t)std::strtoul(argv[2], nullptr, 10) };
        const std::string cls{ argv[3] };                        // internal name, e.g. com/example/demo/Foo
        const std::string cap{ argc >= 5 ? argv[4] : "1000" };
        wchar_t exe[MAX_PATH]{}; GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring w{ exe }; const std::size_t s{ w.find_last_of(L"\\/") };
        if (s != std::wstring::npos) w.resize(s + 1);
        const std::wstring dll{ w + L"vmhook_payload.dll" };
        std::string raw, err;
        if (!enumerate_raw(pid, dll, raw, err, "INST\t" + cls + "\t" + cap))
        { std::printf("{\"error\":\"%s\"}\n", json_escape(err).c_str()); return 1; }

        // O <TAB> 0x<addr> starts an instance; V <TAB> name <TAB> value <TAB> owner <TAB> refAddr.
        // A `refs` map exposes each reference field's pointee address, so callers can
        // feed it back to set-instance / call / array without re-deriving it.
        std::printf("{\"pid\":%u,\"class\":\"%s\",\"instances\":[", pid, json_escape(cls).c_str());
        std::string fields, refs, addr; bool have_inst{ false }, first_inst{ true };
        const auto flush_inst{ [&]
        {
            if (!have_inst) return;
            std::printf("%s{\"address\":\"%s\",\"fields\":{%s},\"refs\":{%s}}",
                first_inst ? "" : ",", json_escape(addr).c_str(), fields.c_str(), refs.c_str());
            first_inst = false; fields.clear(); refs.clear();
        } };
        std::size_t p{ 0 };
        while (p < raw.size())
        {
            std::size_t nl{ raw.find('\n', p) };
            if (nl == std::string::npos) nl = raw.size();
            const std::string_view line{ raw.data() + p, nl - p };
            p = nl + 1;
            if (line.empty()) continue;
            if (line[0] == 'O')
            {
                flush_inst();
                addr = (line.size() > 2 && line[1] == '\t') ? std::string{ line.substr(2) } : std::string{};
                have_inst = true;
            }
            else if (line.size() >= 2 && line[0] == 'V' && line[1] == '\t' && have_inst)
            {
                const std::size_t t2{ line.find('\t', 2) };
                if (t2 != std::string_view::npos)
                {
                    const std::size_t t3{ line.find('\t', t2 + 1) };
                    const std::string name{ line.substr(2, t2 - 2) };
                    const std::string val{ line.substr(t2 + 1, (t3 == std::string_view::npos ? line.size() : t3) - (t2 + 1)) };
                    fields += (fields.empty() ? "" : ",");
                    fields += "\"" + json_escape(name) + "\":\"" + json_escape(val) + "\"";
                    if (t3 != std::string_view::npos)  // owner <TAB> refAddr may follow
                    {
                        const std::size_t t4{ line.find('\t', t3 + 1) };
                        if (t4 != std::string_view::npos)
                        {
                            const std::string ref{ line.substr(t4 + 1) };
                            if (!ref.empty())
                            {
                                refs += (refs.empty() ? "" : ",");
                                refs += "\"" + json_escape(name) + "\":\"" + json_escape(ref) + "\"";
                            }
                        }
                    }
                }
            }
        }
        flush_inst();
        std::printf("]}\n");
        return 0;
    }

    if (cmd == "statics" && argc >= 4)
    {
        const std::uint32_t pid{ (std::uint32_t)std::strtoul(argv[2], nullptr, 10) };
        const std::string cls{ argv[3] };
        wchar_t exe[MAX_PATH]{}; GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring w{ exe }; const std::size_t s{ w.find_last_of(L"\\/") };
        if (s != std::wstring::npos) w.resize(s + 1);
        const std::wstring dll{ w + L"vmhook_payload.dll" };
        std::string raw, err;
        if (!enumerate_raw(pid, dll, raw, err, "STAT\t" + cls))
        { std::printf("{\"error\":\"%s\"}\n", json_escape(err).c_str()); return 1; }

        std::printf("{\"pid\":%u,\"class\":\"%s\",\"statics\":{", pid, json_escape(cls).c_str());
        std::string refs; bool first{ true };
        std::size_t p{ 0 };
        while (p < raw.size())
        {
            std::size_t nl{ raw.find('\n', p) };
            if (nl == std::string::npos) nl = raw.size();
            const std::string_view line{ raw.data() + p, nl - p };
            p = nl + 1;
            if (line.size() >= 2 && line[0] == 'V' && line[1] == '\t')
            {
                const std::size_t t2{ line.find('\t', 2) };
                if (t2 != std::string_view::npos)
                {
                    const std::size_t t3{ line.find('\t', t2 + 1) };
                    const std::string name{ line.substr(2, t2 - 2) };
                    const std::string val{ line.substr(t2 + 1, (t3 == std::string_view::npos ? line.size() : t3) - (t2 + 1)) };
                    std::printf("%s\"%s\":\"%s\"", first ? "" : ",", json_escape(name).c_str(), json_escape(val).c_str());
                    first = false;
                    if (t3 != std::string_view::npos)
                    {
                        const std::size_t t4{ line.find('\t', t3 + 1) };
                        if (t4 != std::string_view::npos)
                        {
                            const std::string ref{ line.substr(t4 + 1) };
                            if (!ref.empty())
                            {
                                refs += (refs.empty() ? "" : ",");
                                refs += "\"" + json_escape(name) + "\":\"" + json_escape(ref) + "\"";
                            }
                        }
                    }
                }
            }
        }
        std::printf("},\"refs\":{%s}}\n", refs.c_str());
        return 0;
    }

    if (cmd == "array" && argc >= 5)
    {
        // array <pid> <0xaddr> <elemDescriptor> [max]  -> the elements of a Java array
        // at that heap address.  elemDescriptor is a JVM element type: I, J, F, D, Z,
        // B, C, S, or Ljava/lang/String; / L<class>; for object arrays.
        const std::uint32_t pid{ (std::uint32_t)std::strtoul(argv[2], nullptr, 10) };
        const std::string addr{ argv[3] }, elem{ argv[4] };
        const std::string max{ argc >= 6 ? argv[5] : "4096" };
        wchar_t exe[MAX_PATH]{}; GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring w{ exe }; const std::size_t s{ w.find_last_of(L"\\/") };
        if (s != std::wstring::npos) w.resize(s + 1);
        const std::wstring dll{ w + L"vmhook_payload.dll" };
        std::string raw, err;
        if (!enumerate_raw(pid, dll, raw, err, "ARR\t" + addr + "\t" + elem + "\t" + max))
        { std::printf("{\"error\":\"%s\"}\n", json_escape(err).c_str()); return 1; }

        std::string length{ "0" };
        std::string elems; bool first{ true };
        std::size_t p{ 0 };
        while (p < raw.size())
        {
            std::size_t nl{ raw.find('\n', p) };
            if (nl == std::string::npos) nl = raw.size();
            const std::string_view line{ raw.data() + p, nl - p };
            p = nl + 1;
            if (line.size() >= 2 && line[0] == 'A' && line[1] == '\t') length.assign(line.substr(2));
            else if (line.size() >= 2 && line[0] == 'V' && line[1] == '\t')
            {
                const std::size_t t2{ line.find('\t', 2) };
                if (t2 != std::string_view::npos)
                {
                    const std::size_t t3{ line.find('\t', t2 + 1) };
                    const std::string_view val{ line.substr(t2 + 1, (t3 == std::string_view::npos ? line.size() : t3) - (t2 + 1)) };
                    elems += (first ? "" : ","); elems += "\""; elems += json_escape(val); elems += "\"";
                    first = false;
                }
            }
        }
        std::printf("{\"pid\":%u,\"address\":\"%s\",\"length\":%s,\"elements\":[%s]}\n",
            pid, json_escape(addr).c_str(), length.c_str(), elems.c_str());
        return 0;
    }

    if (cmd == "array-set" && argc >= 7)
    {
        // array-set <pid> <0xaddr> <elemDescriptor> <index> <value>
        const std::uint32_t pid{ (std::uint32_t)std::strtoul(argv[2], nullptr, 10) };
        const std::string addr{ argv[3] }, elem{ argv[4] }, index{ argv[5] }, value{ argv[6] };
        wchar_t exe[MAX_PATH]{}; GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring w{ exe }; const std::size_t s{ w.find_last_of(L"\\/") };
        if (s != std::wstring::npos) w.resize(s + 1);
        const std::wstring dll{ w + L"vmhook_payload.dll" };
        std::string raw, err;
        if (!enumerate_raw(pid, dll, raw, err, "ARRSET\t" + addr + "\t" + elem + "\t" + index + "\t" + value))
        { std::printf("{\"error\":\"%s\"}\n", json_escape(err).c_str()); return 1; }
        std::string emsg, newval; bool ok{ false };
        std::size_t p{ 0 };
        while (p < raw.size())
        {
            std::size_t nl{ raw.find('\n', p) };
            if (nl == std::string::npos) nl = raw.size();
            const std::string_view line{ raw.data() + p, nl - p };
            p = nl + 1;
            if (line.size() >= 2 && line[0] == 'E' && line[1] == '\t') emsg.assign(line.substr(2));
            else if (line.size() >= 2 && line[0] == 'V' && line[1] == '\t')
            { const std::size_t t2{ line.find('\t', 2) }; if (t2 != std::string_view::npos)
              { const std::size_t t3{ line.find('\t', t2 + 1) }; newval.assign(line.substr(t2 + 1, (t3 == std::string_view::npos ? line.size() : t3) - (t2 + 1))); ok = true; } }
        }
        if (!ok) { std::printf("{\"error\":\"%s\"}\n", json_escape(emsg.empty() ? "array-set failed" : emsg).c_str()); return 1; }
        std::printf("{\"pid\":%u,\"address\":\"%s\",\"index\":%s,\"value\":\"%s\",\"ok\":true}\n",
            pid, json_escape(addr).c_str(), index.c_str(), json_escape(newval).c_str());
        return 0;
    }

    if (cmd == "watch" && argc >= 3)
    {
        // Arm vmhook's on_class_loaded hook, wait, then drain the names of every
        // class DEFINED AT RUNTIME via ClassLoader.defineClass during the window.
        const std::uint32_t pid{ (std::uint32_t)std::strtoul(argv[2], nullptr, 10) };
        const int seconds{ argc >= 4 ? std::atoi(argv[3]) : 5 };
        wchar_t exe[MAX_PATH]{}; GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring w{ exe }; const std::size_t s{ w.find_last_of(L"\\/") };
        if (s != std::wstring::npos) w.resize(s + 1);
        const std::wstring dll{ w + L"vmhook_payload.dll" };

        std::string raw, err;
        if (!enumerate_raw(pid, dll, raw, err, "HOOK"))
        { std::printf("{\"error\":\"%s\"}\n", json_escape(err).c_str()); return 1; }
        const bool armed{ raw.find("H\t1") != std::string::npos };
        if (!armed)
        { std::printf("{\"pid\":%u,\"armed\":false,\"error\":\"on_class_loaded hook failed to install\"}\n", pid); return 1; }

        Sleep((seconds > 0 ? seconds : 5) * 1000);

        raw.clear();
        if (!enumerate_raw(pid, dll, raw, err, "DRAIN"))
        { std::printf("{\"error\":\"%s\"}\n", json_escape(err).c_str()); return 1; }

        std::printf("{\"pid\":%u,\"armed\":true,\"seconds\":%d,\"loaded\":[", pid, seconds);
        std::size_t p{ 0 }; bool first{ true };
        while (p < raw.size())
        {
            std::size_t nl{ raw.find('\n', p) };
            if (nl == std::string::npos) nl = raw.size();
            const std::string_view line{ raw.data() + p, nl - p };
            p = nl + 1;
            if (line.size() >= 2 && line[0] == 'N' && line[1] == '\t')
            {
                std::printf("%s\"%s\"", first ? "" : ",", json_escape(line.substr(2)).c_str());
                first = false;
            }
        }
        std::printf("]}\n");
        return 0;
    }

    if (cmd == "watchf" && argc >= 3)
    {
        // Like `watch`, but drains via DRAINF (each new class's FULL surface) — the
        // hook-driven path the viewer's Auto uses.  Prints the surfaced class names
        // (the C records) + a method/field count so the payload path is verifiable.
        const std::uint32_t pid{ (std::uint32_t)std::strtoul(argv[2], nullptr, 10) };
        const int seconds{ argc >= 4 ? std::atoi(argv[3]) : 5 };
        wchar_t exe[MAX_PATH]{}; GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring w{ exe }; const std::size_t s{ w.find_last_of(L"\\/") };
        if (s != std::wstring::npos) w.resize(s + 1);
        const std::wstring dll{ w + L"vmhook_payload.dll" };

        std::string raw, err;
        if (!enumerate_raw(pid, dll, raw, err, "HOOK"))
        { std::printf("{\"error\":\"%s\"}\n", json_escape(err).c_str()); return 1; }
        if (raw.find("H\t1") == std::string::npos)
        { std::printf("{\"pid\":%u,\"armed\":false}\n", pid); return 1; }

        Sleep((seconds > 0 ? seconds : 5) * 1000);
        raw.clear();
        if (!enumerate_raw(pid, dll, raw, err, "DRAINF"))
        { std::printf("{\"error\":\"%s\"}\n", json_escape(err).c_str()); return 1; }

        auto classes{ parse(raw) };
        std::printf("{\"pid\":%u,\"armed\":true,\"seconds\":%d,\"new_classes\":[", pid, seconds);
        for (std::size_t i = 0; i < classes.size(); ++i)
            std::printf("%s{\"name\":\"%s\",\"methods\":%zu,\"fields\":%zu}", i ? "," : "",
                        json_escape(classes[i].internal_name).c_str(),
                        classes[i].methods.size(), classes[i].fields.size());
        std::printf("]}\n");
        return 0;
    }

    if ((cmd == "set-instance" || cmd == "set-static") && argc >= 5)
    {
        const bool is_instance{ cmd == "set-instance" };
        // set-instance <pid> <class> <address> <field> <value>
        // set-static   <pid> <class> <field> <value>
        if (is_instance && argc < 7) { std::printf("{\"error\":\"usage: set-instance <pid> <class> <address> <field> <value>\"}\n"); return 2; }
        if (!is_instance && argc < 6) { std::printf("{\"error\":\"usage: set-static <pid> <class> <field> <value>\"}\n"); return 2; }
        const std::uint32_t pid{ (std::uint32_t)std::strtoul(argv[2], nullptr, 10) };
        const std::string cls{ argv[3] };
        std::string request, field, value;
        if (is_instance)
        {
            const std::string addr{ argv[4] };
            field = argv[5]; value = argv[6];
            request = "SETI\t" + cls + "\t" + addr + "\t" + field + "\t" + value;
        }
        else
        {
            field = argv[4]; value = argv[5];
            request = "SETS\t" + cls + "\t" + field + "\t" + value;
        }
        wchar_t exe[MAX_PATH]{}; GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring w{ exe }; const std::size_t s{ w.find_last_of(L"\\/") };
        if (s != std::wstring::npos) w.resize(s + 1);
        const std::wstring dll{ w + L"vmhook_payload.dll" };
        std::string raw, err;
        if (!enumerate_raw(pid, dll, raw, err, request))
        { std::printf("{\"error\":\"%s\"}\n", json_escape(err).c_str()); return 1; }

        // Response: "E<TAB>message" on failure, "V<TAB>name<TAB>value" on success.
        std::string emsg, newval; bool ok{ false };
        std::size_t p{ 0 };
        while (p < raw.size())
        {
            std::size_t nl{ raw.find('\n', p) };
            if (nl == std::string::npos) nl = raw.size();
            const std::string_view line{ raw.data() + p, nl - p };
            p = nl + 1;
            if (line.size() >= 2 && line[0] == 'E' && line[1] == '\t') emsg.assign(line.substr(2));
            else if (line.size() >= 2 && line[0] == 'V' && line[1] == '\t')
            {
                // V <TAB> field <TAB> value [<TAB> owner <TAB> refAddr] — take just value.
                const std::size_t t2{ line.find('\t', 2) };
                if (t2 != std::string_view::npos)
                {
                    const std::size_t t3{ line.find('\t', t2 + 1) };
                    newval.assign(line.substr(t2 + 1, (t3 == std::string_view::npos ? line.size() : t3) - (t2 + 1)));
                    ok = true;
                }
            }
        }
        if (!ok)
        { std::printf("{\"error\":\"%s\"}\n", json_escape(emsg.empty() ? "set failed (no response)" : emsg).c_str()); return 1; }
        std::printf("{\"pid\":%u,\"class\":\"%s\",\"field\":\"%s\",\"value\":\"%s\",\"ok\":true}\n",
            pid, json_escape(cls).c_str(), json_escape(field).c_str(), json_escape(newval).c_str());
        return 0;
    }

    if (cmd == "search" && argc >= 4)
    {
        const std::uint32_t pid{ (std::uint32_t)std::strtoul(argv[2], nullptr, 10) };
        const std::string query{ argv[3] };
        const std::string scope{ argc >= 5 ? argv[4] : "all" };
        const bool wantM{ scope == "all" || scope == "methods" };
        const bool wantF{ scope == "all" || scope == "fields" };
        const std::string raw{ load_cache(pid) };
        if (raw.empty()) { std::printf("{\"error\":\"no cache for pid %u; run enumerate first\"}\n", pid); return 1; }
        auto classes{ parse(raw) };
        constexpr int k_cap{ 5000 };
        std::printf("[");
        bool first{ true }; int emitted{ 0 };
        for (const auto& c : classes)
        {
            if (emitted >= k_cap) break;
            if (wantM)
                for (const auto& m : c.methods)
                {
                    if (emitted >= k_cap) break;
                    if (!icontains(m.name, query)) continue;
                    std::printf("%s{\"class\":\"%s\",\"kind\":\"method\",\"name\":\"%s\",\"descriptor\":\"%s\",\"signature\":\"%s\"}",
                        first ? "" : ",", json_escape(c.internal_name).c_str(), json_escape(m.name).c_str(),
                        json_escape(m.descriptor).c_str(), json_escape(viewer::pretty_method(m.descriptor)).c_str());
                    first = false; ++emitted;
                }
            if (wantF)
                for (const auto& f : c.fields)
                {
                    if (emitted >= k_cap) break;
                    if (!icontains(f.name, query)) continue;
                    std::printf("%s{\"class\":\"%s\",\"kind\":\"field\",\"name\":\"%s\",\"descriptor\":\"%s\",\"type\":\"%s\",\"static\":%s}",
                        first ? "" : ",", json_escape(c.internal_name).c_str(), json_escape(f.name).c_str(),
                        json_escape(f.descriptor).c_str(), json_escape(viewer::pretty_field(f.descriptor)).c_str(),
                        f.is_static ? "true" : "false");
                    first = false; ++emitted;
                }
        }
        std::printf("]\n");
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
            const char* kind{
                (c.access & 0x2000u) ? "annotation" :
                (c.access & 0x0200u) ? "interface"  :
                (c.access & 0x4000u) ? "enum"       :
                (c.super_name == "java/lang/Record") ? "record" :
                (c.access == 0u && c.super_name == "java/lang/Enum") ? "enum" :
                (c.access & 0x0400u) ? "abstract" : "class" };
            std::printf("{\"name\":\"%s\",\"super\":\"%s\",\"kind\":\"%s\",\"access\":%u,\"methods\":[",
                json_escape(c.internal_name).c_str(), json_escape(c.super_name).c_str(),
                kind, (unsigned)c.access);
            for (std::size_t i = 0; i < c.methods.size(); ++i)
                std::printf("%s{\"name\":\"%s\",\"descriptor\":\"%s\",\"signature\":\"%s\",\"modifiers\":\"%s\",\"access\":%u}",
                    i ? "," : "", json_escape(c.methods[i].name).c_str(),
                    json_escape(c.methods[i].descriptor).c_str(),
                    json_escape(viewer::pretty_method(c.methods[i].descriptor)).c_str(),
                    json_escape(viewer::access_modifiers(c.methods[i].access, true)).c_str(),
                    (unsigned)c.methods[i].access);
            std::printf("],\"fields\":[");
            for (std::size_t i = 0; i < c.fields.size(); ++i)
                std::printf("%s{\"name\":\"%s\",\"descriptor\":\"%s\",\"type\":\"%s\",\"modifiers\":\"%s\",\"access\":%u,\"static\":%s}",
                    i ? "," : "", json_escape(c.fields[i].name).c_str(),
                    json_escape(c.fields[i].descriptor).c_str(),
                    json_escape(viewer::pretty_field(c.fields[i].descriptor)).c_str(),
                    json_escape(viewer::access_modifiers(c.fields[i].access, false)).c_str(),
                    (unsigned)c.fields[i].access,
                    c.fields[i].is_static ? "true" : "false");
            std::printf("]}\n");
            return 0;
        }
        std::printf("{\"error\":\"class not found: %s\"}\n", json_escape(name).c_str());
        return 1;
    }

    if (cmd == "call" && argc >= 7)
    {
        // call <pid> <class> <addr|-> <method> <descriptor> [arg ...]
        // Each arg is a tagged token: a bare literal (primitive), "@null",
        // "@0x<oop>" (existing reference) or "#<text>" (a new java.lang.String).
        const std::uint32_t pid{ (std::uint32_t)std::strtoul(argv[2], nullptr, 10) };
        const std::string cls{ argv[3] }, addr{ argv[4] }, method{ argv[5] }, desc{ argv[6] };
        const int nargs{ argc - 7 };
        std::string request{ "CALL\t" + cls + "\t" + (addr.empty() ? "-" : addr) + "\t" + method + "\t" + desc
                             + "\t" + std::to_string(nargs) };
        // Flatten any control char in an argument so a tab/newline can't split it
        // into extra tab-separated fields and mis-align the argument list.
        for (int i = 7; i < argc; ++i)
        {
            std::string a{ argv[i] };
            for (char& c : a) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
            request += "\t"; request += a;
        }
        wchar_t exe[MAX_PATH]{}; GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring w{ exe }; const std::size_t s{ w.find_last_of(L"\\/") };
        if (s != std::wstring::npos) w.resize(s + 1);
        const std::wstring dll{ w + L"vmhook_payload.dll" };
        std::string raw, err;
        if (!enumerate_raw(pid, dll, raw, err, request))
        { std::printf("{\"error\":\"%s\"}\n", json_escape(err).c_str()); return 1; }

        // Reply: "E<TAB>msg" or "R<TAB>display<TAB>kind<TAB>refAddr<TAB>refClass".
        std::string emsg, disp, kind, raddr, rclass; bool ok{ false };
        std::size_t p{ 0 };
        while (p < raw.size())
        {
            std::size_t nl{ raw.find('\n', p) };
            if (nl == std::string::npos) nl = raw.size();
            const std::string_view line{ raw.data() + p, nl - p };
            p = nl + 1;
            if (line.size() >= 2 && line[0] == 'E' && line[1] == '\t') emsg.assign(line.substr(2));
            else if (line.size() >= 2 && line[0] == 'R' && line[1] == '\t')
            {
                std::vector<std::string_view> f; std::size_t q{ 2 };
                while (q <= line.size())
                {
                    const std::size_t tab{ line.find('\t', q) };
                    if (tab == std::string_view::npos) { f.push_back(line.substr(q)); break; }
                    f.push_back(line.substr(q, tab - q)); q = tab + 1;
                }
                if (f.size() >= 1) disp.assign(f[0]);
                if (f.size() >= 2) kind.assign(f[1]);
                if (f.size() >= 3) raddr.assign(f[2]);
                if (f.size() >= 4) rclass.assign(f[3]);
                ok = true;
            }
        }
        if (!ok)
        { std::printf("{\"error\":\"%s\"}\n", json_escape(emsg.empty() ? "call failed (no response)" : emsg).c_str()); return 1; }
        std::printf("{\"pid\":%u,\"class\":\"%s\",\"method\":\"%s\",\"result\":\"%s\",\"kind\":\"%s\",\"ref\":\"%s\",\"refClass\":\"%s\",\"ok\":true}\n",
            pid, json_escape(cls).c_str(), json_escape(method).c_str(), json_escape(disp).c_str(),
            json_escape(kind).c_str(), json_escape(raddr).c_str(), json_escape(rclass).c_str());
        return 0;
    }

    if (cmd == "freeze" && argc >= 7)
    {
        // freeze <pid> <I|S> <class> <addr|-> <field> <value>
        const std::uint32_t pid{ (std::uint32_t)std::strtoul(argv[2], nullptr, 10) };
        const std::string scope{ argv[3] }, cls{ argv[4] };
        std::string addr{ argv[5] }; if (addr == "-") addr.clear();
        const std::string field{ argv[6] }, value{ argc >= 8 ? argv[7] : "" };
        const std::string request{ "FRZ\t" + scope + "\t" + cls + "\t" + addr + "\t" + field + "\t" + value };
        wchar_t exe[MAX_PATH]{}; GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring w{ exe }; const std::size_t s{ w.find_last_of(L"\\/") };
        if (s != std::wstring::npos) w.resize(s + 1);
        const std::wstring dll{ w + L"vmhook_payload.dll" };
        std::string raw, err;
        if (!enumerate_raw(pid, dll, raw, err, request))
        { std::printf("{\"error\":\"%s\"}\n", json_escape(err).c_str()); return 1; }
        std::string emsg, newval; bool ok{ false };
        std::size_t p{ 0 };
        while (p < raw.size())
        {
            std::size_t nl{ raw.find('\n', p) };
            if (nl == std::string::npos) nl = raw.size();
            const std::string_view line{ raw.data() + p, nl - p };
            p = nl + 1;
            if (line.size() >= 2 && line[0] == 'E' && line[1] == '\t') emsg.assign(line.substr(2));
            else if (line.size() >= 2 && line[0] == 'V' && line[1] == '\t')
            { const std::size_t t2{ line.find('\t', 2) }; if (t2 != std::string_view::npos)
              { const std::size_t t3{ line.find('\t', t2 + 1) }; newval.assign(line.substr(t2 + 1, (t3 == std::string_view::npos ? line.size() : t3) - (t2 + 1))); ok = true; } }
        }
        if (!ok) { std::printf("{\"error\":\"%s\"}\n", json_escape(emsg.empty() ? "freeze failed" : emsg).c_str()); return 1; }
        std::printf("{\"pid\":%u,\"class\":\"%s\",\"field\":\"%s\",\"value\":\"%s\",\"frozen\":true}\n",
            pid, json_escape(cls).c_str(), json_escape(field).c_str(), json_escape(newval).c_str());
        return 0;
    }

    if (cmd == "unfreeze" && argc >= 4)
    {
        // unfreeze <pid> <key|*>   (key = "<I|S>|Class|addr|field")
        const std::uint32_t pid{ (std::uint32_t)std::strtoul(argv[2], nullptr, 10) };
        const std::string key{ argv[3] };
        wchar_t exe[MAX_PATH]{}; GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring w{ exe }; const std::size_t s{ w.find_last_of(L"\\/") };
        if (s != std::wstring::npos) w.resize(s + 1);
        const std::wstring dll{ w + L"vmhook_payload.dll" };
        std::string raw, err;
        if (!enumerate_raw(pid, dll, raw, err, "UNF\t" + key))
        { std::printf("{\"error\":\"%s\"}\n", json_escape(err).c_str()); return 1; }
        std::printf("{\"pid\":%u,\"unfroze\":\"%s\",\"ok\":true}\n", pid, json_escape(key).c_str());
        return 0;
    }

    std::printf("{\"error\":\"usage: vmhook_cli list | enumerate <pid> | classes <pid> [substr] | class <pid> <name> | "
                "search <pid> <query> [methods|fields|all] | instances <pid> <class> [cap] | statics <pid> <class> | "
                "array <pid> <0xaddr> <elemDescriptor> [max] | array-set <pid> <0xaddr> <elemDescriptor> <index> <value> | "
                "watch <pid> [seconds] | watchf <pid> [seconds] | set-instance <pid> <class> <address> <field> <value> | "
                "set-static <pid> <class> <field> <value> | "
                "call <pid> <class> <addr|-> <method> <descriptor> [arg...] | "
                "freeze <pid> <I|S> <class> <addr|-> <field> <value> | unfreeze <pid> <key|*>\"}\n");
    return 2;
}
