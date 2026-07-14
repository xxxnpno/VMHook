// vmhook viewer — C++ script host.
//
// Compiles a user-written C++ script (that #includes a generated wrapper and
// installs vmhook hooks) into a self-contained DLL, which the viewer then
// injects into the attached JVM.  The script's hooks fire on real Java threads,
// so method calls inside a detour are safe (see the invocation rule in vmhook).
//
// Pure system logic (locate MSVC, compose the translation unit, drive cl.exe,
// tail the script log) — no ImGui.  The UI orchestration (editor, async build
// state) lives in main.cpp.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "app.hpp"  // viewer::to_utf8

namespace script_host
{
    // %TEMP%\vmhook_script.log — the script appends here; the viewer tails it.
    inline auto log_path() -> std::string
    {
        wchar_t tmp[MAX_PATH]{};
        const DWORD n{ GetTempPathW(MAX_PATH, tmp) };
        std::wstring p{ n ? std::wstring{ tmp, n } : L"" };
        p += L"vmhook_script.log";
        return viewer::to_utf8(p.c_str());
    }

    inline auto log_path_w() -> std::wstring
    {
        wchar_t tmp[MAX_PATH]{};
        const DWORD n{ GetTempPathW(MAX_PATH, tmp) };
        std::wstring p{ n ? std::wstring{ tmp, n } : L"" };
        p += L"vmhook_script.log";
        return p;
    }

    // Locate a vcvars64.bat from any installed VS 2022 edition (Build Tools /
    // Community / Professional / Enterprise), preferring vswhere's answer.
    inline auto find_vcvars() -> std::string
    {
        const auto exists{ [](const std::wstring& p) -> bool
        {
            const DWORD a{ GetFileAttributesW(p.c_str()) };
            return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
        } };

        // 1) Probe the standard install roots (fast, no process spawn — covers the
        //    common case; vswhere below is the fallback for non-default installs).
        {
            const wchar_t* roots[]{
                L"C:\\Program Files\\Microsoft Visual Studio\\2022",
                L"C:\\Program Files (x86)\\Microsoft Visual Studio\\2022",
            };
            const wchar_t* editions[]{ L"BuildTools", L"Community", L"Professional", L"Enterprise" };
            for (const wchar_t* r : roots)
                for (const wchar_t* e : editions)
                {
                    const std::wstring cand{ std::wstring{ r } + L"\\" + e + L"\\VC\\Auxiliary\\Build\\vcvars64.bat" };
                    if (exists(cand)) return viewer::to_utf8(cand.c_str());
                }
        }

        // 2) vswhere (authoritative for non-default install locations).
        {
            wchar_t pf86[MAX_PATH]{};
            if (GetEnvironmentVariableW(L"ProgramFiles(x86)", pf86, MAX_PATH))
            {
                const std::wstring vswhere{ std::wstring{ pf86 } + L"\\Microsoft Visual Studio\\Installer\\vswhere.exe" };
                if (exists(vswhere))
                {
                    // Run vswhere, capture its installationPath, append the vcvars path.
                    const std::wstring cmd{ L"\"" + vswhere +
                        L"\" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath" };
                    // Redirect to a temp file (simplest reliable capture).
                    wchar_t tmp[MAX_PATH]{};
                    GetTempPathW(MAX_PATH, tmp);
                    const std::wstring out{ std::wstring{ tmp } + L"vmhook_vswhere.txt" };
                    // Wrap the whole command in an extra pair of quotes so cmd /c
                    // keeps the quoted vswhere path intact (its quote-stripping rule).
                    const std::wstring full{ L"cmd.exe /c \"" + cmd + L" > \"" + out + L"\" 2>nul\"" };
                    std::vector<wchar_t> mut{ full.begin(), full.end() }; mut.push_back(L'\0');
                    STARTUPINFOW si{ sizeof(si) };
                    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
                    PROCESS_INFORMATION pi{};
                    if (CreateProcessW(nullptr, mut.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                                       nullptr, nullptr, &si, &pi))
                    {
                        WaitForSingleObject(pi.hProcess, 8000);
                        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
                        std::ifstream f{ viewer::to_utf8(out.c_str()) };
                        std::string line; std::getline(f, line);
                        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) line.pop_back();
                        if (!line.empty())
                        {
                            std::wstring wline(line.size(), L'\0');
                            MultiByteToWideChar(CP_UTF8, 0, line.c_str(), -1, wline.data(), (int)wline.size() + 1);
                            wline.resize(wcslen(wline.c_str()));
                            const std::wstring cand{ wline + L"\\VC\\Auxiliary\\Build\\vcvars64.bat" };
                            if (exists(cand)) return viewer::to_utf8(cand.c_str());
                        }
                    }
                }
            }
        }

        return {};
    }

    // The fixed harness wrapped around the user's script body.  The user body
    // must define `void script_setup();` and may #include the generated wrapper.
    inline auto compose_source(const std::string& user_body) -> std::string
    {
        // Bake the absolute log path (backslash-escaped) into the source so the
        // injected DLL writes to the same file the viewer tails.
        std::string log_lit;
        for (char c : log_path()) { if (c == '\\') log_lit += '\\'; log_lit += c; }

        std::string s;
        s += "// Generated by vmhook viewer — script translation unit.\n";
        s += "#ifndef WIN32_LEAN_AND_MEAN\n#define WIN32_LEAN_AND_MEAN\n#endif\n";
        s += "#ifndef NOMINMAX\n#define NOMINMAX\n#endif\n";  // vmhook uses std::...::max(); the windows min/max macros would break it
        s += "#include <windows.h>\n";
        s += "#include <atomic>\n#include <chrono>\n#include <fstream>\n#include <mutex>\n";
        s += "#include <string>\n#include <string_view>\n#include <thread>\n";
        s += "#include <vmhook/vmhook.hpp>\n\n";
        s += "namespace script\n{\n";
        s += "    inline std::mutex& log_mutex() { static std::mutex m; return m; }\n";
        s += "    inline void log(std::string_view msg)\n    {\n";
        s += "        std::lock_guard<std::mutex> lk{ log_mutex() };\n";
        s += "        std::ofstream f{ \"" + log_lit + "\", std::ios::app };\n";
        s += "        f << msg << '\\n';\n    }\n";
        s += "    // Blocks until the JVM's core classes resolve (or timeout).  Returns true if ready.\n";
        s += "    inline bool wait_for_vm(int timeout_ms = 30000)\n    {\n";
        s += "        const auto deadline{ std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms) };\n";
        s += "        for (;;)\n        {\n";
        s += "            if (vmhook::find_class(\"java/lang/Object\")) return true;\n";
        s += "            if (std::chrono::steady_clock::now() >= deadline) return false;\n";
        s += "            std::this_thread::sleep_for(std::chrono::milliseconds(100));\n        }\n    }\n";
        s += "}\n\n";
        s += "// ─────────────────────────────── user script ───────────────────────────────\n";
        s += user_body;
        s += "\n// ─────────────────────────────── harness epilogue ──────────────────────────\n";
        s += "void script_setup();\n";
        s += "static void script_boot()\n{\n";
        s += "    if (!script::wait_for_vm()) { script::log(\"[harness] JVM not ready — aborting.\"); return; }\n";
        s += "    script::log(\"[harness] JVM ready — running script_setup().\");\n";
        s += "    try { script_setup(); script::log(\"[harness] script_setup() returned.\"); }\n";
        s += "    catch (const std::exception& e) { script::log(std::string{\"[harness] script_setup() threw: \"} + e.what()); }\n";
        s += "    catch (...) { script::log(\"[harness] script_setup() threw (unknown).\"); }\n";
        s += "}\n\n";
        s += "extern \"C\" BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)\n{\n";
        s += "    if (reason == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(module); std::thread(script_boot).detach(); }\n";
        s += "    else if (reason == DLL_PROCESS_DETACH) { vmhook::shutdown_hooks(); }\n";
        s += "    return TRUE;\n}\n";
        return s;
    }

    struct BuildResult
    {
        bool        ok{ false };
        std::string dll_path;
        std::string log;
    };

    // Write the composed TU, drive cl.exe via vcvars64, and produce a DLL.
    //  work_dir       : scratch directory (created if missing)
    //  vmhook_include : dir containing vmhook/ (so <vmhook/vmhook.hpp> resolves)
    //  wrapper_dir    : dir containing the generated wrapper header (for the user's #include)
    inline auto build(const std::string& user_body, const std::string& work_dir,
                      const std::string& vmhook_include, const std::string& wrapper_dir) -> BuildResult
    {
        BuildResult r;
        const std::string vcvars{ find_vcvars() };
        if (vcvars.empty())
        {
            r.log = "Could not locate vcvars64.bat (Visual Studio 2022 with the C++ workload / Build Tools).\n"
                    "Install the MSVC toolset, then retry.";
            return r;
        }

        CreateDirectoryA(work_dir.c_str(), nullptr);
        const std::string src{ work_dir + "\\script_tu.cpp" };
        // Unique DLL name per build: an already-injected script holds a lock on its
        // module, so a fixed name would make the next rebuild's link fail.
        const std::string dll{ work_dir + "\\vmhook_script_" + std::to_string(GetTickCount64()) + ".dll" };
        const std::string bat{ work_dir + "\\build.bat" };
        const std::string blog{ work_dir + "\\build.log" };

        { std::ofstream f{ src, std::ios::trunc }; f << compose_source(user_body); }

        {
            std::ofstream f{ bat, std::ios::trunc };
            f << "@echo off\n";
            f << "call \"" << vcvars << "\" >nul\n";
            f << "cd /d \"" << work_dir << "\"\n";
            // No /O2: compiling vmhook.hpp dominates, and a script wants fast turnaround.
            f << "cl /nologo /LD /std:c++latest /EHa /bigobj /Zc:preprocessor /Zc:__cplusplus /MT ^\n";
            f << "  /I\"" << vmhook_include << "\" /I\"" << wrapper_dir << "\" ^\n";
            f << "  script_tu.cpp /link /OUT:\"" << dll << "\"\n";
            f << "exit /b %errorlevel%\n";
        }

        // Run the batch, redirecting all output to build.log, no console window.
        std::wstring wbat(bat.size(), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, bat.c_str(), -1, wbat.data(), (int)wbat.size() + 1);
        wbat.resize(wcslen(wbat.c_str()));
        std::wstring wlog(blog.size(), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, blog.c_str(), -1, wlog.data(), (int)wlog.size() + 1);
        wlog.resize(wcslen(wlog.c_str()));
        const std::wstring cmd{ L"cmd.exe /c \"\"" + wbat + L"\" > \"" + wlog + L"\" 2>&1\"" };

        std::vector<wchar_t> mut{ cmd.begin(), cmd.end() }; mut.push_back(L'\0');
        STARTUPINFOW si{ sizeof(si) };
        si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};
        DWORD exit_code{ 1 };
        if (CreateProcessW(nullptr, mut.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                           nullptr, nullptr, &si, &pi))
        {
            WaitForSingleObject(pi.hProcess, 600000);  // 10 min ceiling
            GetExitCodeProcess(pi.hProcess, &exit_code);
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        }
        else
        {
            r.log = "Failed to launch the compiler process.";
            return r;
        }

        { std::ifstream f{ blog }; std::stringstream ss; ss << f.rdbuf(); r.log = ss.str(); }
        const DWORD a{ GetFileAttributesA(dll.c_str()) };
        r.ok = (exit_code == 0) && a != INVALID_FILE_ATTRIBUTES;
        if (r.ok) r.dll_path = dll;
        return r;
    }

    // A starter script body shown in the editor.  {WRAPPER_INCLUDE} is replaced
    // with the actual generated-header include line by the caller.
    inline auto starter_body(const std::string& wrapper_include_line, const std::string& example_hook) -> std::string
    {
        std::string s;
        s += wrapper_include_line + "\n\n";
        s += "// script_setup() runs once on a background thread after the JVM is ready.\n";
        s += "// Install hooks here; their detours fire on real Java threads, so calling\n";
        s += "// Java methods from inside a detour is safe.  Use script::log(\"...\") to\n";
        s += "// print to the viewer's live log.\n";
        s += "void script_setup()\n{\n";
        s += "    script::log(\"hello from the script\");\n\n";
        if (!example_hook.empty()) s += example_hook;
        s += "}\n";
        return s;
    }
}
