// vmhook viewer — ImGui (Win32 + DirectX 11) front-end.
//
// Lists running HotSpot JVMs, injects the vmhook payload DLL into the chosen
// one, and shows every loaded Java class with its methods and fields in a
// modern, IDE-style layout.  Everything is discovered dynamically — no mappings.

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"

#include <d3d11.h>
#include <tchar.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "app.hpp"
#include "descriptor.hpp"
#include "icons.hpp"
#include "widgets.hpp"

#pragma comment(lib, "d3d11.lib")

// ── DirectX 11 plumbing (standard ImGui example boilerplate) ──────────────────
namespace
{
    ID3D11Device*           g_device{ nullptr };
    ID3D11DeviceContext*    g_context{ nullptr };
    IDXGISwapChain*         g_swap_chain{ nullptr };
    ID3D11RenderTargetView* g_rtv{ nullptr };
    HWND                    g_hwnd{ nullptr };  // for the custom min/close controls
    float                   g_dpi_scale{ 1.0f };  // physical DPI / 96 (set at startup)

    void create_rtv()
    {
        ID3D11Texture2D* back_buffer{ nullptr };
        g_swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
        if (back_buffer) { g_device->CreateRenderTargetView(back_buffer, nullptr, &g_rtv); back_buffer->Release(); }
    }
    void cleanup_rtv() { if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; } }

    bool create_device(HWND hwnd)
    {
        DXGI_SWAP_CHAIN_DESC desc{};
        desc.BufferCount = 2;
        desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.BufferDesc.RefreshRate.Numerator = 60;
        desc.BufferDesc.RefreshRate.Denominator = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.OutputWindow = hwnd;
        desc.SampleDesc.Count = 1;
        desc.Windowed = TRUE;
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        D3D_FEATURE_LEVEL levels[]{ D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
        D3D_FEATURE_LEVEL obtained{};
        if (D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                D3D11_SDK_VERSION, &desc, &g_swap_chain, &g_device, &obtained, &g_context) != S_OK)
        {
            return false;
        }
        create_rtv();
        return true;
    }
    void cleanup_device()
    {
        cleanup_rtv();
        if (g_swap_chain) { g_swap_chain->Release(); g_swap_chain = nullptr; }
        if (g_context)    { g_context->Release();    g_context = nullptr; }
        if (g_device)     { g_device->Release();     g_device = nullptr; }
    }

    auto payload_dll_path() -> std::wstring
    {
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring path{ exe };
        const std::size_t slash{ path.find_last_of(L"\\/") };
        if (slash != std::wstring::npos) { path.resize(slash + 1); }
        return path + L"vmhook_payload.dll";
    }
    auto exe_dir() -> std::string
    {
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring w{ exe };
        const std::size_t slash{ w.find_last_of(L"\\/") };
        if (slash != std::wstring::npos) { w.resize(slash + 1); }
        return viewer::to_utf8(w.c_str());
    }

    auto icontains(const std::string& haystack, const std::string& needle) -> bool
    {
        if (needle.empty()) return true;
        const auto it{ std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
            [](char a, char b){ return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); }) };
        return it != haystack.end();
    }
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l)
{
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, w, l)) return true;
    switch (msg)
    {
    case WM_SIZE:
        if (g_device && w != SIZE_MINIMIZED) { cleanup_rtv(); g_swap_chain->ResizeBuffers(0, LOWORD(l), HIWORD(l), DXGI_FORMAT_UNKNOWN, 0); create_rtv(); }
        return 0;
    case WM_GETMINMAXINFO:
    {
        // Borderless (WS_POPUP) window: constrain "maximize" to the monitor work
        // area so it fills the screen without covering the taskbar.
        MONITORINFO mi{ sizeof(mi) };
        if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi))
        {
            auto* const mmi{ reinterpret_cast<MINMAXINFO*>(l) };
            mmi->ptMaxPosition.x = mi.rcWork.left - mi.rcMonitor.left;
            mmi->ptMaxPosition.y = mi.rcWork.top  - mi.rcMonitor.top;
            mmi->ptMaxSize.x     = mi.rcWork.right  - mi.rcWork.left;
            mmi->ptMaxSize.y     = mi.rcWork.bottom - mi.rcWork.top;
            mmi->ptMinTrackSize.x = 900;
            mmi->ptMinTrackSize.y = 600;
        }
        return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, w, l);
}

// ── style + fonts ─────────────────────────────────────────────────────────────
namespace
{
    // Theme: a clean, flat, modern-IDE dark palette (VS Code / Linear family) —
    // moderate rounding, crisp 1px frame borders, a single blue accent for
    // interactive state.  No gloss/gradients/halos; the widgets are stock ImGui,
    // so the polish comes entirely from restraint here.
    void apply_modern_style()
    {
        ImGuiStyle& s{ ImGui::GetStyle() };
        // Rounding — modern-but-restrained (not pill-shaped).
        s.WindowRounding = 8.0f; s.ChildRounding = 6.0f; s.FrameRounding = 5.0f;
        s.PopupRounding = 6.0f; s.GrabRounding = 4.0f; s.ScrollbarRounding = 9.0f; s.TabRounding = 6.0f;
        // Spacing / padding.
        s.WindowPadding = ImVec2(14, 12); s.FramePadding = ImVec2(12, 5);
        s.ItemSpacing = ImVec2(8, 7); s.ItemInnerSpacing = ImVec2(7, 5); s.CellPadding = ImVec2(10, 6);
        s.ScrollbarSize = 12.0f; s.GrabMinSize = 10.0f;
        // Crisp 1px borders on frames — the signature of a clean flat tool UI.
        s.WindowBorderSize = 0.0f; s.FrameBorderSize = 1.0f; s.ChildBorderSize = 1.0f; s.PopupBorderSize = 1.0f;
        s.SeparatorTextBorderSize = 2.0f; s.SeparatorTextPadding = ImVec2(20, 6);
        s.WindowTitleAlign = ImVec2(0.0f, 0.5f);
        s.ButtonTextAlign = ImVec2(0.5f, 0.5f);

        ImVec4* c{ s.Colors };
        const ImVec4 accent   { 0.29f, 0.56f, 0.96f, 1.00f };  // clean modern blue
        const ImVec4 accentHi { 0.40f, 0.66f, 1.00f, 1.00f };
        c[ImGuiCol_Text]                 = ImVec4(0.90f, 0.91f, 0.94f, 1.00f);
        c[ImGuiCol_TextDisabled]         = ImVec4(0.44f, 0.47f, 0.54f, 1.00f);
        c[ImGuiCol_WindowBg]             = ImVec4(0.086f, 0.092f, 0.106f, 1.00f);
        c[ImGuiCol_ChildBg]              = ImVec4(0.104f, 0.111f, 0.128f, 1.00f);
        c[ImGuiCol_PopupBg]              = ImVec4(0.094f, 0.100f, 0.116f, 0.98f);
        c[ImGuiCol_Border]               = ImVec4(0.196f, 0.208f, 0.235f, 1.00f);
        c[ImGuiCol_BorderShadow]         = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
        c[ImGuiCol_FrameBg]              = ImVec4(0.140f, 0.150f, 0.172f, 1.00f);
        c[ImGuiCol_FrameBgHovered]       = ImVec4(0.176f, 0.190f, 0.220f, 1.00f);
        c[ImGuiCol_FrameBgActive]        = ImVec4(0.205f, 0.222f, 0.256f, 1.00f);
        c[ImGuiCol_TitleBg]              = ImVec4(0.066f, 0.072f, 0.084f, 1.00f);
        c[ImGuiCol_TitleBgActive]        = ImVec4(0.066f, 0.072f, 0.084f, 1.00f);
        c[ImGuiCol_MenuBarBg]            = ImVec4(0.104f, 0.111f, 0.128f, 1.00f);
        c[ImGuiCol_ScrollbarBg]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
        c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.22f, 0.24f, 0.28f, 1.00f);
        c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.30f, 0.33f, 0.38f, 1.00f);
        c[ImGuiCol_ScrollbarGrabActive]  = accent;
        c[ImGuiCol_CheckMark]            = accentHi;
        c[ImGuiCol_SliderGrab]           = accent;
        c[ImGuiCol_SliderGrabActive]     = accentHi;
        // Neutral secondary button (Primary/Ghost/Danger are set per-call in ui::Button).
        c[ImGuiCol_Button]               = ImVec4(0.170f, 0.182f, 0.210f, 1.00f);
        c[ImGuiCol_ButtonHovered]        = ImVec4(0.215f, 0.232f, 0.268f, 1.00f);
        c[ImGuiCol_ButtonActive]         = ImVec4(0.135f, 0.145f, 0.168f, 1.00f);
        c[ImGuiCol_Header]               = ImVec4(accent.x, accent.y, accent.z, 0.32f);
        c[ImGuiCol_HeaderHovered]        = ImVec4(accent.x, accent.y, accent.z, 0.48f);
        c[ImGuiCol_HeaderActive]         = ImVec4(accent.x, accent.y, accent.z, 0.64f);
        c[ImGuiCol_Separator]            = ImVec4(0.196f, 0.208f, 0.235f, 1.00f);
        c[ImGuiCol_SeparatorHovered]     = accent;
        c[ImGuiCol_SeparatorActive]      = accentHi;
        c[ImGuiCol_ResizeGrip]           = ImVec4(1.00f, 1.00f, 1.00f, 0.06f);
        c[ImGuiCol_ResizeGripHovered]    = accent;
        c[ImGuiCol_ResizeGripActive]     = accentHi;
        // Header sits in the same flat frame family as the combos/inputs (not a
        // dark block) so it reads as part of the UI chrome.
        c[ImGuiCol_TableHeaderBg]        = ImVec4(0.135f, 0.145f, 0.168f, 1.00f);
        c[ImGuiCol_TableBorderStrong]    = ImVec4(1.00f, 1.00f, 1.00f, 0.060f);
        c[ImGuiCol_TableBorderLight]     = ImVec4(1.00f, 1.00f, 1.00f, 0.028f);
        c[ImGuiCol_TableRowBg]           = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
        c[ImGuiCol_TableRowBgAlt]        = ImVec4(1.00f, 1.00f, 1.00f, 0.018f);
        c[ImGuiCol_TextSelectedBg]       = ImVec4(accent.x, accent.y, accent.z, 0.35f);
        c[ImGuiCol_TextLink]             = accentHi;  // the extends / field-type jump links
        c[ImGuiCol_DragDropTarget]       = accentHi;
        c[ImGuiCol_NavHighlight]         = accent;
    }

    void load_fonts(float dpi_scale)
    {
        ImGuiIO& io{ ImGui::GetIO() };
        const float base_px{ 18.0f * dpi_scale };
        io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", base_px);
        if (io.Fonts->Fonts.empty())
        {
            io.Fonts->AddFontDefault();
        }

        // Merge Font Awesome icons into the base font (shared baseline), if the
        // bundled font is present next to the exe.  Only the glyphs actually used
        // are rasterized (tight codepoint ranges) to keep the atlas small.
        const std::string icon_ttf{ exe_dir() + "fa-solid-900.ttf" };
        if (std::ifstream{ icon_ttf, std::ios::binary }.good())
        {
            static const ImWchar icon_ranges[]{
                0xF002, 0xF002,  // magnifying-glass
                0xF021, 0xF021,  // arrows-rotate (rescan)
                0xF00D, 0xF00D,  // xmark
                0xF059, 0xF059,  // circle-question
                0xF060, 0xF061,  // arrow-left / arrow-right
                0xF065, 0xF066,  // expand / compress
                0xF068, 0xF068,  // minus
                0xF1E6, 0xF1E6,  // plug
                0 };
            ImFontConfig cfg{};
            cfg.MergeMode        = true;
            cfg.PixelSnapH       = true;
            cfg.GlyphMinAdvanceX = 16.0f * dpi_scale;  // keep icons a consistent width
            cfg.GlyphOffset      = ImVec2(0.0f, 2.0f * dpi_scale);  // sit on the text baseline
            io.Fonts->AddFontFromFileTTF(icon_ttf.c_str(), 15.0f * dpi_scale, &cfg, icon_ranges);
        }
    }
}

// ── UI state ──────────────────────────────────────────────────────────────────
namespace
{
    char         g_search[256]{};
    char         g_method_filter[128]{};
    char         g_field_filter[128]{};
    int          g_selected_class{ -1 };
    float        g_left_width{ 500.0f };  // default; DPI-scaled + persisted
    bool         g_pretty{ true };
    bool         g_full_names{ false };
    bool         g_show_inherited{ false };  // details: include super-chain members
    bool         g_show_instances{ false };  // live-instances window open
    bool         g_instances_live{ true };   // auto-refresh the instances window
    bool         g_instances_refresh_now{ false };  // force an immediate re-scan
    std::string  g_instances_class;          // internal name shown in the window
    char         g_instance_filter[128]{};   // substring filter over the instance rows
    bool         g_open_instance_detail{ false };  // request the per-instance detail popup
    std::string  g_detail_addr;              // address of the instance the popup shows
    bool         g_copy_instance_table{ false };   // request a TSV copy of the instance table
    int          g_instance_cap{ 1000 };     // persisted mirror of App::inst_cap
    bool         g_inst_show_inherited{ true };  // show inherited columns in the instance table
    bool         g_focus_search{ false };
    int          g_kind_filter{ 0 };   // 0=all; else index into k_kind_names
    int          g_search_scope{ 0 };  // 0=Classes, 1=Methods, 2=Fields
    int          g_class_sort{ 0 };    // class list: 0=natural, 1=A→Z, 2=Z→A
    bool         g_auto_rescan{ false };  // periodically re-scan loaded classes
    bool         g_new_only{ false };     // class list: show only runtime-loaded classes
    bool         g_show_removed{ false }; // request the unloaded-classes popup
    std::wstring g_dll_path{};
    std::vector<int> g_filtered;  // rebuilt each frame from the search box
    // Global member-search results: (class index, member index) pairs.
    std::vector<std::pair<int,int>> g_member_results;

    // Back/forward navigation history (paired with the clickable `extends` jump
    // and class-list clicks) so browsing the class graph feels like a browser.
    std::vector<int> g_nav_back;
    std::vector<int> g_nav_fwd;
    bool             g_scroll_to_selected{ false };  // sync the list to a jump

    // Select a class, recording the previous selection for Back.  Clears the
    // per-pane member filters (a fresh class shouldn't inherit stale filters).
    // scroll_into_view syncs the class list to the target — set for link/history
    // jumps, cleared for list clicks (the clicked row is already visible).
    void navigate_to(int idx, bool scroll_into_view = true)
    {
        if (idx == g_selected_class) return;
        if (g_selected_class >= 0) g_nav_back.push_back(g_selected_class);
        g_nav_fwd.clear();
        g_selected_class = idx;
        g_method_filter[0] = 0; g_field_filter[0] = 0;
        if (scroll_into_view) g_scroll_to_selected = true;
    }

    void nav_back()
    {
        if (g_nav_back.empty()) return;
        if (g_selected_class >= 0) g_nav_fwd.push_back(g_selected_class);
        g_selected_class = g_nav_back.back(); g_nav_back.pop_back();
        g_method_filter[0] = 0; g_field_filter[0] = 0;
        g_scroll_to_selected = true;
    }

    void nav_forward()
    {
        if (g_nav_fwd.empty()) return;
        if (g_selected_class >= 0) g_nav_back.push_back(g_selected_class);
        g_selected_class = g_nav_fwd.back(); g_nav_fwd.pop_back();
        g_method_filter[0] = 0; g_field_filter[0] = 0;
        g_scroll_to_selected = true;
    }

    // Persist a few UI preferences next to the exe so the tool remembers how the
    // user likes it.  Best-effort: any I/O or parse error is silently ignored.
    auto settings_path() -> std::string { return exe_dir() + "vmhook_viewer.ini"; }

    void save_settings()
    {
        std::ofstream out{ settings_path(), std::ios::trunc };
        if (!out) return;
        out << "pretty="        << (g_pretty ? 1 : 0)        << "\n"
            << "full_names="     << (g_full_names ? 1 : 0)    << "\n"
            << "inherited="      << (g_show_inherited ? 1 : 0)<< "\n"
            << "kind_filter="    << g_kind_filter             << "\n"
            << "left_width="     << g_left_width              << "\n"
            << "font_scale="     << ImGui::GetIO().FontGlobalScale << "\n"
            << "inst_cap="       << g_instance_cap            << "\n"
            << "inst_live="      << (g_instances_live ? 1 : 0)<< "\n"
            << "inst_inherited=" << (g_inst_show_inherited ? 1 : 0) << "\n";
    }

    void load_settings()
    {
        std::ifstream in{ settings_path() };
        if (!in) return;
        std::string line;
        while (std::getline(in, line))
        {
            const std::size_t eq{ line.find('=') };
            if (eq == std::string::npos) continue;
            const std::string key{ line.substr(0, eq) };
            const std::string val{ line.substr(eq + 1) };
            try
            {
                if      (key == "pretty")     g_pretty        = (std::stoi(val) != 0);
                else if (key == "full_names") g_full_names    = (std::stoi(val) != 0);
                else if (key == "inherited")  g_show_inherited= (std::stoi(val) != 0);
                else if (key == "kind_filter"){ int k{ std::stoi(val) }; if (k >= 0 && k <= 6) g_kind_filter = k; }
                else if (key == "left_width") g_left_width    = std::clamp(std::stof(val), 240.0f, 2000.0f);
                else if (key == "font_scale") ImGui::GetIO().FontGlobalScale = std::clamp(std::stof(val), 0.7f, 2.0f);
                else if (key == "inst_cap")   g_instance_cap  = std::clamp(std::stoi(val), 20, 200000);
                else if (key == "inst_live")  g_instances_live = (std::stoi(val) != 0);
                else if (key == "inst_inherited") g_inst_show_inherited = (std::stoi(val) != 0);
            }
            catch (...) { /* ignore a malformed value, keep the default */ }
        }
    }

    float em(float n);  // fwd (defined in the panes namespace)

    void status_pill(viewer::Status st)
    {
        ImVec4 col; const char* text;
        switch (st)
        {
        case viewer::Status::Idle:      col = ImVec4(0.5f,0.5f,0.55f,1); text = "Idle";      break;
        case viewer::Status::Injecting: col = ImVec4(0.95f,0.7f,0.2f,1); text = "Injecting"; break;
        case viewer::Status::Receiving: col = ImVec4(0.3f,0.6f,0.95f,1); text = "Receiving"; break;
        case viewer::Status::Done:      col = ImVec4(0.35f,0.8f,0.45f,1);text = "Done";      break;
        default:                        col = ImVec4(0.9f,0.35f,0.35f,1);text = "Error";     break;
        }
        // Status dot drawn as a filled circle (no font-glyph dependency), text
        // vertically centered against the toolbar's framed widgets.
        ImGui::AlignTextToFramePadding();
        const ImVec2 p{ ImGui::GetCursorScreenPos() };
        const float  r{ em(0.28f) };
        const float  cy{ p.y + ImGui::GetFrameHeight() * 0.5f };
        ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + r, cy), r, ImGui::GetColorU32(col));
        ImGui::Dummy(ImVec2(r * 2.0f + em(0.35f), ImGui::GetFrameHeight()));
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::TextUnformatted(text);
        ImGui::PopStyleColor();
    }

    ImVec4 vis_color(std::uint16_t f)
    {
        if (f & 0x0001u) return ImVec4(0.52f, 0.82f, 0.56f, 1.0f);  // public
        if (f & 0x0002u) return ImVec4(0.90f, 0.52f, 0.52f, 1.0f);  // private
        if (f & 0x0004u) return ImVec4(0.92f, 0.74f, 0.42f, 1.0f);  // protected
        return ImVec4(0.62f, 0.63f, 0.70f, 1.0f);                   // package-private
    }

    // Primary class kind from the class-file access flags (+ super for records),
    // with a distinct badge colour.  ANNOTATION implies INTERFACE, and ENUM
    // implies FINAL, so the checks are ordered most-specific first.  Returns a
    // stable label even when flags are 0 (older payloads) — falls back to super.
    struct ClassKind { const char* label; ImVec4 color; };
    ClassKind class_kind(const viewer::ClassInfo& c)
    {
        const std::uint16_t f{ c.access };
        if (f & 0x2000u) return { "annotation", ImVec4(0.80f, 0.68f, 0.95f, 1.0f) };
        if (f & 0x0200u) return { "interface",  ImVec4(0.52f, 0.82f, 0.92f, 1.0f) };
        if (f & 0x4000u) return { "enum",       ImVec4(0.95f, 0.80f, 0.45f, 1.0f) };
        if (c.super_name == "java/lang/Record") return { "record", ImVec4(0.60f, 0.86f, 0.66f, 1.0f) };
        if (f & 0x0400u) return { "abstract",   ImVec4(0.82f, 0.66f, 0.60f, 1.0f) };
        // Flags unavailable but super says enum → still label it.
        if (f == 0u && c.super_name == "java/lang/Enum") return { "enum", ImVec4(0.95f, 0.80f, 0.45f, 1.0f) };
        return { "class", ImVec4(0.62f, 0.72f, 0.85f, 1.0f) };
    }

    // The Java source keyword(s) that would declare this class, for export.
    std::string class_decl(const viewer::ClassInfo& c)
    {
        const std::uint16_t f{ c.access };
        if (f & 0x2000u) return "@interface";
        if (f & 0x0200u) return "interface";
        if (f & 0x4000u) return "enum";
        if (c.super_name == "java/lang/Record") return "record";
        if (f == 0u && c.super_name == "java/lang/Enum") return "enum";
        std::string kw;
        if (f & 0x0400u) kw += "abstract ";
        else if (f & 0x0010u) kw += "final ";
        kw += "class";
        return kw;
    }

    // The internal class name a (possibly array) field descriptor refers to, or
    // "" for a primitive/void.  "[[Lcom/foo/Bar;" -> "com/foo/Bar", "I" -> "".
    std::string ref_internal_name(const std::string& desc)
    {
        std::size_t i{ 0 };
        while (i < desc.size() && desc[i] == '[') ++i;
        if (i < desc.size() && desc[i] == 'L')
        {
            const std::size_t semi{ desc.find(';', i) };
            if (semi != std::string::npos) return desc.substr(i + 1, semi - i - 1);
        }
        return {};
    }

    // Render a method's pretty signature inline, with each CLASS type (params +
    // return) drawn as a clickable link that navigates to that class.  Falls
    // back to plain text in raw-descriptor mode or on a malformed descriptor.
    void render_method_signature(viewer::App& app, const std::string& desc)
    {
        if (!g_pretty || desc.size() < 2 || desc[0] != '(')
        {
            ImGui::TextUnformatted((g_pretty ? viewer::pretty_method(desc, g_full_names) : desc).c_str());
            return;
        }

        auto render_type = [&](std::size_t& i)
        {
            const std::size_t start{ i };
            int arrays{ 0 };
            while (i < desc.size() && desc[i] == '[') { ++arrays; ++i; }
            std::string base, internal;
            if (i >= desc.size()) { base = "?"; }
            else
            {
                switch (desc[i])
                {
                case 'V': base = "void";    ++i; break;
                case 'Z': base = "boolean"; ++i; break;
                case 'B': base = "byte";    ++i; break;
                case 'C': base = "char";    ++i; break;
                case 'S': base = "short";   ++i; break;
                case 'I': base = "int";     ++i; break;
                case 'J': base = "long";    ++i; break;
                case 'F': base = "float";   ++i; break;
                case 'D': base = "double";  ++i; break;
                case 'L':
                {
                    const std::size_t semi{ desc.find(';', i) };
                    const std::size_t end{ semi == std::string::npos ? desc.size() : semi };
                    internal = desc.substr(i + 1, end - i - 1);
                    if (g_full_names) { base = internal; for (char& ch : base) if (ch == '/') ch = '.'; }
                    else { const std::size_t sl{ internal.find_last_of('/') }; base = (sl == std::string::npos ? internal : internal.substr(sl + 1)); for (char& ch : base) if (ch == '$') ch = '.'; }
                    i = (semi == std::string::npos ? desc.size() : semi + 1);
                    break;
                }
                default: base = std::string(1, desc[i]); ++i; break;
                }
            }
            for (int a = 0; a < arrays; ++a) base += "[]";

            ImGui::PushID((int)start);
            const auto it{ internal.empty() ? app.name_to_index.end() : app.name_to_index.find(internal) };
            if (it != app.name_to_index.end())
            {
                if (ImGui::TextLink(base.c_str())) navigate_to(it->second);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s  (click to open)", internal.c_str());
            }
            else
            {
                ImGui::TextUnformatted(base.c_str());
            }
            ImGui::PopID();
        };

        std::size_t i{ 1 };
        ImGui::TextUnformatted("(");
        bool first{ true };
        while (i < desc.size() && desc[i] != ')')
        {
            ImGui::SameLine(0.0f, 0.0f);
            if (!first) { ImGui::TextUnformatted(", "); ImGui::SameLine(0.0f, 0.0f); }
            render_type(i);
            first = false;
        }
        if (i < desc.size()) ++i;  // skip ')'
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::TextUnformatted(") : ");
        ImGui::SameLine(0.0f, 0.0f);
        render_type(i);
    }

    void copy_menu(const char* id, const std::string& primary, const std::string& secondary = {})
    {
        if (ImGui::BeginPopupContextItem(id))
        {
            if (ImGui::MenuItem("Copy name"))            ImGui::SetClipboardText(primary.c_str());
            if (!secondary.empty() && ImGui::MenuItem("Copy descriptor")) ImGui::SetClipboardText(secondary.c_str());
            if (!secondary.empty() && ImGui::MenuItem("Copy name + descriptor"))
                ImGui::SetClipboardText((primary + " " + secondary).c_str());
            ImGui::EndPopup();
        }
    }

    bool export_class(const viewer::ClassInfo& c)
    {
        std::string path{ exe_dir() + "vmhook_export.txt" };
        std::ofstream out{ path, std::ios::trunc };
        if (!out) return false;  // directory not writable, etc.
        out << class_decl(c) << " " << c.internal_name;
        if (!c.super_name.empty() && c.super_name != "java/lang/Object")
            out << " extends " << c.super_name;
        out << "\n\nMETHODS (" << c.methods.size() << ")\n";
        for (const auto& m : c.methods) out << "  " << m.name << "  " << m.descriptor << "\n";
        out << "\nFIELDS (" << c.fields.size() << ")\n";
        for (const auto& f : c.fields) out << "  " << (f.is_static ? "static " : "") << f.name << "  " << f.descriptor << "\n";
        return static_cast<bool>(out);
    }
}

// ── panes ─────────────────────────────────────────────────────────────────────
namespace
{
    // Font-relative unit ("em"): scales with both DPI and the user's Ctrl+/- zoom,
    // so layout math stays adaptive instead of using magic pixel constants.
    inline float em(float n) { return ImGui::GetFontSize() * n; }

    // Text vertically centered against framed widgets on the same row.
    inline void row_label(const char* text)
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(text);
    }
    inline void row_label_disabled(const char* text)
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", text);
    }

    // A thin vertical divider that lines up with the row's framed widgets.
    inline void row_divider()
    {
        ImGui::SameLine(0.0f, em(0.6f));
        const ImVec2 p{ ImGui::GetCursorScreenPos() };
        const float  h{ ImGui::GetFrameHeight() };
        ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y + h * 0.18f), ImVec2(p.x, p.y + h * 0.82f),
                                            ImGui::GetColorU32(ImGuiCol_Border), 1.0f);
        ImGui::Dummy(ImVec2(em(0.06f), h));
        ImGui::SameLine(0.0f, em(0.6f));
    }

    void draw_toolbar(viewer::App& app)
    {
        // Adaptive JVM picker combo (list auto-refreshes every 2s; no Refresh btn).
        // No leading label/icon — the "Select a running JVM..." preview is self-explanatory.
        const float combo_w{ std::clamp(ImGui::GetContentRegionMax().x - em(30.0f), em(16.0f), em(64.0f)) };
        std::string preview{ "Select a running JVM..." };
        if (app.selected_jvm >= 0 && app.selected_jvm < (int)app.jvms.size())
        {
            const auto& p{ app.jvms[(std::size_t)app.selected_jvm] };
            preview = p.display_name + "  -  " + std::to_string(p.pid);
        }
        ImGui::BeginDisabled(app.busy());  // don't swap JVM mid-attach
        const bool combo_open{ ui::BeginCombo("##jvm", preview.c_str(), combo_w) };
        // Full command line on hover (the name/pid preview hides it).
        if (!combo_open && app.selected_jvm >= 0 && app.selected_jvm < (int)app.jvms.size() && ImGui::IsItemHovered())
        {
            const auto& p{ app.jvms[(std::size_t)app.selected_jvm] };
            ImGui::SetTooltip("%s", p.command_line.empty() ? p.image_name.c_str() : p.command_line.c_str());
        }
        if (combo_open)
        {
            for (int i = 0; i < (int)app.jvms.size(); ++i)
            {
                const auto& p{ app.jvms[(std::size_t)i] };
                std::string item{ p.display_name + "  -  " + std::to_string(p.pid) };
                if (ImGui::Selectable(item.c_str(), app.selected_jvm == i)) app.selected_jvm = i;
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", p.command_line.empty() ? p.image_name.c_str() : p.command_line.c_str());
            }
            ui::EndCombo();
        }
        ImGui::EndDisabled();
        ImGui::SameLine(0.0f, em(0.6f));
        ImGui::BeginDisabled(app.busy() || app.selected_jvm < 0);
        if (ui::Button(ICON_FA_PLUG "  Attach", ImVec2(0, 0), ui::BtnPrimary)) { app.attach_selected(g_dll_path); g_selected_class = -1; g_nav_back.clear(); g_nav_fwd.clear(); }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered() && !ImGui::IsItemActive())
            ImGui::SetTooltip("Inject vmhook and enumerate every class, method and field");

        // Re-scan the already-attached JVM's classes (no re-injection) to catch
        // ones loaded / unloaded at runtime; + an auto toggle for live tracking.
        ImGui::SameLine(0.0f, em(0.4f));
        ImGui::BeginDisabled(app.busy() || !app.has_baseline.load());
        if (ui::IconButton(ICON_FA_ROTATE, "rescan")) app.rescan();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered() && app.has_baseline.load())
            ImGui::SetTooltip("Re-scan loaded classes — new ones are marked, unloaded ones listed");
        ImGui::SameLine(0.0f, em(0.5f));
        // The switch is shorter than the buttons on this row — nudge it down so
        // it sits vertically centred against Attach / Rescan.
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ImGui::GetFrameHeight() * 0.13f);
        ImGui::BeginDisabled(!app.has_baseline.load());
        ui::Toggle("Auto", &g_auto_rescan);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Live class-load tracking: arms the on_class_loaded hook + re-scans on load");

        row_divider();
        const viewer::Status st{ app.status.load() };
        status_pill(st);
        if (st == viewer::Status::Injecting || st == viewer::Status::Receiving)
        {
            ImGui::SameLine(0.0f, em(0.6f));
            const float r{ ImGui::GetFrameHeight() * 0.32f };
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (ImGui::GetFrameHeight() * 0.5f - r) - ImGui::GetStyle().FramePadding.y);
            ui::Spinner("##spin", r, (std::max)(r * 0.35f, em(0.12f)), ImGui::GetColorU32(ImVec4(0.34f, 0.63f, 1.0f, 1.0f)));
            ImGui::SameLine(0.0f, em(0.45f));
            ImGui::AlignTextToFramePadding();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.60f, 0.74f, 1.0f, 1.0f));
            if (st == viewer::Status::Receiving)
                ImGui::Text("%llu classes...", (unsigned long long)app.classes_streamed.load());
            else
                ImGui::TextUnformatted("injecting...");
            ImGui::PopStyleColor();
        }

        // Right-aligned window controls: help (?), minimize, close — the app is
        // borderless (no native title bar), so these replace the caption buttons.
        // Compact square icon buttons (glyph pixel-centred by ui::IconButton),
        // spaced in font-relative units.
        const float bw{ ImGui::GetFrameHeight() * 0.78f };
        const float gap{ em(0.35f) };
        ImGui::SameLine(ImGui::GetContentRegionMax().x - (bw * 4.0f + gap * 3.0f));
        // Ghost (transparent) window controls that only tint on hover — subtle
        // chrome rather than loud primary buttons.
        if (ui::IconButton(ICON_FA_CIRCLE_Q, "help", bw, ui::BtnGhost)) ImGui::OpenPopup("shortcuts");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Keyboard shortcuts (F1)");
        if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) ImGui::OpenPopup("shortcuts");
        ImGui::SameLine(0.0f, gap);
        if (ui::IconButton(ICON_FA_MINUS, "min", bw, ui::BtnGhost) && g_hwnd) ShowWindow(g_hwnd, SW_MINIMIZE);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Minimize");
        ImGui::SameLine(0.0f, gap);
        const bool maximized{ g_hwnd && IsZoomed(g_hwnd) };
        if (ui::IconButton(maximized ? ICON_FA_COMPRESS : ICON_FA_EXPAND, "maxrestore", bw, ui::BtnGhost) && g_hwnd)
            ShowWindow(g_hwnd, maximized ? SW_RESTORE : SW_MAXIMIZE);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(maximized ? "Restore" : "Maximize");
        ImGui::SameLine(0.0f, gap);
        if (ui::IconButton(ICON_FA_XMARK, "close", bw, ui::BtnDanger) && g_hwnd) PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Close");
        if (ImGui::BeginPopup("shortcuts"))
        {
            ImGui::SeparatorText("Keyboard shortcuts");
            const std::pair<const char*, const char*> keys[]{
                { "Ctrl+F",           "Focus the class search" },
                { "Esc",              "Clear all filters" },
                { "Alt+Left / Right", "Navigate back / forward" },
                { "Ctrl+= / Ctrl+-",  "Zoom the UI font in / out" },
                { "Ctrl+0",           "Reset the font zoom" },
            };
            if (ImGui::BeginTable("keys", 2, ImGuiTableFlags_SizingFixedFit))
            {
                for (const auto& [k, desc] : keys)
                {
                    ImGui::TableNextRow(); ImGui::TableNextColumn();
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.72f, 1.0f, 1.0f));
                    ImGui::TextUnformatted(k);
                    ImGui::PopStyleColor();
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(desc);
                }
                ImGui::EndTable();
            }
            ImGui::Separator();
            ImGui::TextDisabled("Tip: click any field or method type (or 'extends') to jump to that class.");
            ImGui::TextDisabled("Tip: use the Methods/Fields scope to search members across all classes.");
            ImGui::TextDisabled("Tip: 'Live instances' scans the heap for live objects of a class and shows");
            ImGui::TextDisabled("      their field values — sortable, filterable, live; click a row for full detail.");
            ImGui::TextDisabled("Tip: right-click a class/method/field row to copy its name.");
            ImGui::EndPopup();
        }
    }

    // Details-pane methods/fields tables are drawn by draw_details; declared here
    // so a member-search click can pre-focus the right filter.
    void draw_member_results(viewer::App& app, bool fields);

    void draw_class_list(viewer::App& app)
    {
        // ── search row: magnifier + input + clear button ────────────────────
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(ICON_FA_SEARCH);
        ImGui::SameLine(0.0f, em(0.4f));
        if (g_focus_search) { ImGui::SetKeyboardFocusHere(); g_focus_search = false; }
        const bool has_query{ g_search[0] != '\0' };
        const char* hint{ g_search_scope == 1 ? "Search methods across all classes" : g_search_scope == 2 ? "Search fields across all classes" : "Search classes  (Ctrl+F)" };
        ui::InputText("##search", hint, g_search, sizeof(g_search), has_query ? -em(1.9f) : -1.0f);
        if (has_query)
        {
            ImGui::SameLine(0.0f, em(0.3f));
            if (ui::IconButton(ICON_FA_XMARK, "clear")) g_search[0] = '\0';
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Clear (Esc)");
        }

        std::lock_guard<std::mutex> lock{ app.data_mutex };

        // ── scope selector (+ kind filter for Classes) on one compact row ───
        static const char* k_kind_names[]{ "All kinds", "class", "interface", "enum", "abstract", "annotation", "record" };
        ui::Combo("##scope", &g_search_scope, "Classes\0Methods\0Fields\0", em(7.5f));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Search across every class's methods or fields");
        if (g_search_scope == 0)
        {
            ImGui::SameLine(0.0f, em(0.5f));
            ui::Combo("##kind", &g_kind_filter, k_kind_names, IM_ARRAYSIZE(k_kind_names), -1.0f);
        }

        if (g_search_scope != 0)
        {
            draw_member_results(app, g_search_scope == 2);
            return;
        }

        // Match either slash- or dot-qualified queries ("java.lang" == "java/lang").
        std::string needle{ g_search };
        for (char& ch : needle) if (ch == '.') ch = '/';

        const char* want_kind{ g_kind_filter > 0 ? k_kind_names[g_kind_filter] : nullptr };

        const int n_new{ app.last_added.load() };
        if (g_new_only && n_new == 0) g_new_only = false;  // nothing new left to show

        g_filtered.clear();
        g_filtered.reserve(app.classes.size());
        for (int i = 0; i < (int)app.classes.size(); ++i)
        {
            const viewer::ClassInfo& ci{ app.classes[(std::size_t)i] };
            if (g_new_only && !ci.is_new) continue;
            if (!icontains(ci.internal_name, needle)) continue;
            if (want_kind && std::strcmp(class_kind(ci).label, want_kind) != 0) continue;
            g_filtered.push_back(i);
        }

        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%d / %zu classes", (int)g_filtered.size(), app.classes.size());
        // Runtime-loaded classes: a green "+N new" chip that filters to them.
        if (n_new > 0)
        {
            ImGui::SameLine(0.0f, em(0.6f));
            const ImVec4 green{ g_new_only ? ImVec4(0.55f, 0.95f, 0.66f, 1.0f) : ImVec4(0.42f, 0.82f, 0.52f, 1.0f) };
            ImGui::PushStyleColor(ImGuiCol_Text, green);
            ImGui::PushStyleColor(ImGuiCol_TextLink, green);
            char lbl[40];
            std::snprintf(lbl, sizeof(lbl), "+%d new%s", n_new, g_new_only ? "  (shown)" : "");
            if (ImGui::TextLink(lbl)) g_new_only = !g_new_only;
            ImGui::PopStyleColor(2);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Classes loaded since the previous scan — click to filter to them");
        }

        // Custom "Class" header — a rounded frame matching the combo/input cells
        // (ImGui table headers can't be rounded).  Click to cycle sort: natural →
        // A→Z → Z→A, shown by a little triangle on the right.
        {
            const ImGuiStyle& st{ ImGui::GetStyle() };
            // Match the combo/input text inset (FramePadding.x) so "Class" lines
            // up exactly under "Classes".
            const float   hpad{ st.FramePadding.x };
            const ImVec2  p0{ ImGui::GetCursorScreenPos() };
            const float   bar_w{ ImGui::GetContentRegionAvail().x };
            const float   bar_h{ ImGui::GetFrameHeight() };
            const ImVec2  p1{ p0.x + bar_w, p0.y + bar_h };
            if (ImGui::InvisibleButton("##classhdr", ImVec2(bar_w, bar_h))) g_class_sort = (g_class_sort + 1) % 3;
            const bool    hov{ ImGui::IsItemHovered() };
            if (hov) ImGui::SetTooltip("Sort by name (%s)", g_class_sort == 0 ? "click to A→Z" : g_class_sort == 1 ? "A→Z" : "Z→A");
            ImDrawList* dl{ ImGui::GetWindowDrawList() };
            dl->AddRectFilled(p0, p1, ImGui::GetColorU32(hov ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), st.FrameRounding);
            if (st.FrameBorderSize > 0.0f)
                dl->AddRect(p0, p1, ImGui::GetColorU32(ImGuiCol_Border), st.FrameRounding, 0, st.FrameBorderSize);
            dl->AddText(ImVec2(p0.x + hpad, p0.y + (bar_h - ImGui::GetFontSize()) * 0.5f),
                        ImGui::GetColorU32(ImGuiCol_Text), "Class");
            if (g_class_sort != 0)
            {
                const float  cx{ p1.x - hpad }, cy{ p0.y + bar_h * 0.5f }, tr{ em(0.22f) };
                const ImU32  ac{ ImGui::GetColorU32(ImGuiCol_Text) };
                if (g_class_sort == 1)  // A→Z, triangle up
                    dl->AddTriangleFilled(ImVec2(cx, cy - tr), ImVec2(cx - tr, cy + tr), ImVec2(cx + tr, cy + tr), ac);
                else                    // Z→A, triangle down
                    dl->AddTriangleFilled(ImVec2(cx - tr, cy - tr), ImVec2(cx + tr, cy - tr), ImVec2(cx, cy + tr), ac);
            }
        }
        if (g_class_sort != 0)
        {
            const bool asc{ g_class_sort == 1 };
            std::sort(g_filtered.begin(), g_filtered.end(), [&](int a, int b)
            {
                const int cmp{ app.classes[(std::size_t)a].internal_name.compare(app.classes[(std::size_t)b].internal_name) };
                return asc ? cmp < 0 : cmp > 0;
            });
        }

        // Headerless table (the rounded header above replaces the built-in one).
        // PadOuterX: without borders ImGui defaults to NoPadOuterX, so the column
        // would sit flush against the edge — force the outer padding + a touch more
        // CellPadding so rows are inset comfortably and align with the header label.
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(ImGui::GetStyle().FramePadding.x, ImGui::GetStyle().CellPadding.y));
        const bool table_open{ ImGui::BeginTable("classes", 1,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_PadOuterX) };
        if (table_open)
        {
            ImGui::TableSetupColumn("Class", ImGuiTableColumnFlags_WidthStretch);

            ImGuiListClipper clipper;
            clipper.Begin((int)g_filtered.size());
            // A link/history jump changed the selection off-screen: force that row
            // to be submitted so we can scroll it into view this frame.
            int scroll_row{ -1 };
            if (g_scroll_to_selected && g_selected_class >= 0)
            {
                for (int i = 0; i < (int)g_filtered.size(); ++i)
                    if (g_filtered[(std::size_t)i] == g_selected_class) { scroll_row = i; break; }
                if (scroll_row >= 0) clipper.IncludeItemByIndex(scroll_row);
                else g_scroll_to_selected = false;  // target filtered out — nothing to sync
            }
            while (clipper.Step())
            {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
                {
                    const int idx{ g_filtered[(std::size_t)row] };
                    const viewer::ClassInfo& c{ app.classes[(std::size_t)idx] };
                    const bool sel{ g_selected_class == idx };
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::PushID(idx);
                    // Full-row selectable (empty label); the two-tone path is
                    // drawn over it via SetCursorScreenPos so package + name can
                    // carry distinct colours without losing click/selection.
                    const ImVec2 rp{ ImGui::GetCursorScreenPos() };
                    if (ImGui::Selectable("##row", sel, ImGuiSelectableFlags_SpanAllColumns))
                        navigate_to(idx, false);  // clicked row already visible
                    copy_menu("cls", c.internal_name);
                    std::string dotted{ c.internal_name };
                    for (char& ch : dotted) if (ch == '/') ch = '.';
                    if (ImGui::IsItemHovered() && (c.access || !c.super_name.empty()))
                        ImGui::SetTooltip("%s\n%s  ·  %zu methods / %zu fields",
                            dotted.c_str(), class_kind(c).label, c.methods.size(), c.fields.size());

                    ImGui::SetCursorScreenPos(rp);
                    std::string pkg{ c.package };
                    for (char& ch : pkg) if (ch == '/') ch = '.';
                    if (!pkg.empty())
                    {
                        ImGui::PushStyleColor(ImGuiCol_Text, sel ? ImVec4(0.80f, 0.85f, 0.95f, 1.0f)
                                                                 : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                        ImGui::TextUnformatted((pkg + ".").c_str());
                        ImGui::PopStyleColor();
                        ImGui::SameLine(0.0f, 0.0f);
                    }
                    std::string sname{ c.simple_name };
                    for (char& ch : sname) if (ch == '$') ch = '.';  // nested → dotted for display
                    ImGui::PushStyleColor(ImGuiCol_Text, sel ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : class_kind(c).color);
                    ImGui::TextUnformatted(sname.c_str());
                    ImGui::PopStyleColor();
                    // runtime-loaded classes get a small green dot in the left gutter
                    if (c.is_new)
                        ImGui::GetWindowDrawList()->AddCircleFilled(
                            ImVec2(rp.x - em(0.52f), rp.y + ImGui::GetFontSize() * 0.5f),
                            em(0.17f), ImGui::GetColorU32(ImVec4(0.42f, 0.85f, 0.52f, 1.0f)));
                    ImGui::PopID();
                    if (row == scroll_row && g_scroll_to_selected)
                    {
                        ImGui::SetScrollHereY(0.5f);
                        g_scroll_to_selected = false;
                    }
                }
            }
            ImGui::EndTable();
        }
        ImGui::PopStyleVar();  // cell padding kept live through the whole table
    }

    // Global search across every class's methods or fields.  Called by
    // draw_class_list with app.data_mutex ALREADY HELD (must not re-lock).
    void draw_member_results(viewer::App& app, bool fields)
    {
        const std::string needle{ g_search };  // raw: member names have no . or /
        g_member_results.clear();
        if (!needle.empty())
        {
            for (int ci = 0; ci < (int)app.classes.size(); ++ci)
            {
                const viewer::ClassInfo& c{ app.classes[(std::size_t)ci] };
                const int n{ fields ? (int)c.fields.size() : (int)c.methods.size() };
                for (int mi = 0; mi < n; ++mi)
                {
                    const std::string& nm{ fields ? c.fields[(std::size_t)mi].name : c.methods[(std::size_t)mi].name };
                    if (icontains(nm, needle)) g_member_results.emplace_back(ci, mi);
                }
            }
        }

        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%zu %s", g_member_results.size(), fields ? "fields" : "methods");

        if (needle.empty())
        {
            ImGui::Dummy(ImVec2(0.0f, em(1.5f)));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.55f, 1.0f));
            const std::string hint{ std::string("Type above to search ") + (fields ? "fields" : "methods") + " across every loaded class." };
            const float w{ ImGui::CalcTextSize(hint.c_str()).x };
            ImGui::SetCursorPosX((std::max)((ImGui::GetContentRegionAvail().x - w) * 0.5f, 0.0f));
            ImGui::TextUnformatted(hint.c_str());
            ImGui::PopStyleColor();
            return;
        }

        if (ImGui::BeginTable("members", 3,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable))
        {
            ImGui::TableSetupColumn(fields ? "Field" : "Method", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn(fields ? "Type" : "Signature", ImGuiTableColumnFlags_WidthStretch, 1.1f);
            ImGui::TableSetupColumn("Class", ImGuiTableColumnFlags_WidthStretch, 1.2f);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();

            ImGuiListClipper clip;
            clip.Begin((int)g_member_results.size());
            while (clip.Step())
            {
                for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r)
                {
                    const auto [ci, mi] = g_member_results[(std::size_t)r];
                    const viewer::ClassInfo& c{ app.classes[(std::size_t)ci] };
                    std::string dotted{ c.internal_name };
                    for (char& ch : dotted) if (ch == '/') ch = '.';

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::PushID(r);
                    if (fields)
                    {
                        const viewer::FieldInfo& f{ c.fields[(std::size_t)mi] };
                        ImGui::PushStyleColor(ImGuiCol_Text, vis_color(f.access));
                        const bool clicked{ ImGui::Selectable(f.name.c_str(), false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap) };
                        ImGui::PopStyleColor();
                        if (clicked) { navigate_to(ci); std::snprintf(g_field_filter, sizeof(g_field_filter), "%s", g_search); }
                        ImGui::TableSetColumnIndex(1);
                        {
                            const std::string ty{ g_pretty ? viewer::pretty_field(f.descriptor, g_full_names) : f.descriptor };
                            const std::string refn{ ref_internal_name(f.descriptor) };
                            const auto rit{ refn.empty() ? app.name_to_index.end() : app.name_to_index.find(refn) };
                            if (rit != app.name_to_index.end()) { if (ImGui::TextLink(ty.c_str())) navigate_to(rit->second); }
                            else ImGui::TextUnformatted(ty.c_str());
                        }
                    }
                    else
                    {
                        const viewer::MethodInfo& m{ c.methods[(std::size_t)mi] };
                        ImGui::PushStyleColor(ImGuiCol_Text, vis_color(m.access));
                        const bool clicked{ ImGui::Selectable(m.name.c_str(), false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap) };
                        ImGui::PopStyleColor();
                        if (clicked) { navigate_to(ci); std::snprintf(g_method_filter, sizeof(g_method_filter), "%s", g_search); }
                        ImGui::TableSetColumnIndex(1);
                        render_method_signature(app, m.descriptor);
                    }
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextDisabled("%s", dotted.c_str());
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", dotted.c_str());
                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }
    }

    void draw_details(viewer::App& app)
    {
        std::lock_guard<std::mutex> lock{ app.data_mutex };
        if (g_selected_class < 0 || g_selected_class >= (int)app.classes.size())
        {
            ImGui::Dummy(ImVec2(0, em(2.2f)));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f,0.5f,0.55f,1));
            const char* hint{ app.classes.empty()
                ? "Pick a JVM above and click \"Attach\" to load its classes."
                : "Select a class on the left to see its methods and fields." };
            const float w{ ImGui::CalcTextSize(hint).x };
            ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - w) * 0.5f);
            ImGui::TextUnformatted(hint);
            ImGui::PopStyleColor();
            return;
        }
        const viewer::ClassInfo& c{ app.classes[(std::size_t)g_selected_class] };

        // back / forward navigation (browser-style, pairs with `extends` jumps).
        // Subtle frame-coloured square buttons with Font Awesome arrows.
        ImGui::BeginDisabled(g_nav_back.empty());
        if (ui::IconButton(ICON_FA_ARROW_L, "nav_back")) nav_back();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered() && !g_nav_back.empty()) ImGui::SetTooltip("Back (Alt+Left)");
        ImGui::SameLine(0.0f, em(0.3f));
        ImGui::BeginDisabled(g_nav_fwd.empty());
        if (ui::IconButton(ICON_FA_ARROW_R, "nav_fwd")) nav_forward();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered() && !g_nav_fwd.empty()) ImGui::SetTooltip("Forward (Alt+Right)");
        ImGui::SameLine(0.0f, em(0.6f));

        // header — one frame-align sets the baseline for the whole line (name + badges)
        std::string dotted{ c.internal_name };
        for (char& ch : dotted) if (ch == '/') ch = '.';
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f,0.82f,1.0f,1));
        ImGui::TextUnformatted(dotted.c_str());
        ImGui::PopStyleColor();
        // kind badge from the class-file access flags (interface/enum/abstract/...)
        const ClassKind kind{ class_kind(c) };
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, kind.color);
        ImGui::Text("[%s]", kind.label);
        ImGui::PopStyleColor();
        if (c.access & 0x0010u && !(c.access & 0x4000u))  // final (enums are implicitly final)
        {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.60f, 0.62f, 0.70f, 1.0f));
            ImGui::TextUnformatted("[final]");
            ImGui::PopStyleColor();
        }
        if (c.internal_name.find('$') != std::string::npos)
        {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.60f, 0.62f, 0.70f, 1.0f));
            ImGui::TextUnformatted("[nested]");
            ImGui::PopStyleColor();
        }
        if (!c.super_name.empty() && c.super_name != "java/lang/Object")
        {
            ImGui::SameLine(); ImGui::TextDisabled("extends"); ImGui::SameLine();
            std::string sd{ c.super_name };
            for (char& ch : sd) if (ch == '/') ch = '.';
            const auto it{ app.name_to_index.find(c.super_name) };
            if (it != app.name_to_index.end())
            {
                if (ImGui::TextLink(sd.c_str())) { navigate_to(it->second); }
            }
            else
            {
                ImGui::TextDisabled("%s", sd.c_str());
            }
        }
        // Action buttons on their own row so a long class name / extends link can
        // never push them off the right edge (adaptive, no overflow).
        ImGui::Spacing();
        // These run while draw_details holds data_mutex, so status_message
        // (also guarded by it) can be written directly for lightweight feedback.
        if (ui::Button("Copy name"))
        {
            ImGui::SetClipboardText(c.internal_name.c_str());
            app.status_message = "Copied class name to clipboard.";
        }
        ImGui::SameLine(0.0f, em(0.4f));
        if (ui::Button("Copy all"))
        {
            // WYSIWYG with the tables: include inherited members (grouped under a
            // "// inherited from X" comment) when the Show-inherited toggle is on.
            std::vector<const viewer::ClassInfo*> ch{ &c };
            if (g_show_inherited)
            {
                const viewer::ClassInfo* cur{ &c };
                for (int hops = 0; hops < 200; ++hops)
                {
                    if (cur->super_name.empty() || cur->super_name == "java/lang/Object") break;
                    const auto it{ app.name_to_index.find(cur->super_name) };
                    if (it == app.name_to_index.end()) break;
                    cur = &app.classes[(std::size_t)it->second];
                    if (cur == &c) break;
                    ch.push_back(cur);
                }
            }
            std::string all{ class_decl(c) + " " + dotted + " {\n" };
            for (const viewer::ClassInfo* oc : ch)
            {
                if (oc != &c)
                {
                    std::string od{ oc->internal_name };
                    for (char& x : od) if (x == '/') x = '.';
                    all += "\n  // inherited from " + od + "\n";
                }
                for (const auto& f : oc->fields)
                    all += "  " + viewer::access_modifiers(f.access, false) + " " + viewer::pretty_field(f.descriptor) + " " + f.name + ";\n";
                for (const auto& m : oc->methods)
                    all += "  " + viewer::access_modifiers(m.access, true) + " " + m.name + viewer::pretty_method(m.descriptor) + "\n";
            }
            all += "}\n";
            ImGui::SetClipboardText(all.c_str());
            app.status_message = g_show_inherited ? "Copied class listing (with inherited members) to clipboard."
                                                  : "Copied class listing to clipboard.";
        }
        ImGui::SameLine(0.0f, em(0.4f));
        if (ui::Button("Export .txt"))
            app.status_message = export_class(c) ? "Exported to vmhook_export.txt (next to the viewer)."
                                                 : "Export failed — is the viewer's folder writable?";
        ImGui::SameLine(0.0f, em(0.4f));
        // Live heap instances — deferred (request_instances re-locks data_mutex
        // which this function already holds).
        if (ui::Button(ICON_FA_SEARCH " Live instances", ImVec2(0, 0), ui::BtnPrimary))
        {
            g_instances_class = c.internal_name;
            g_instances_refresh_now = true;  // immediate first scan
            g_show_instances = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Scan the heap for live objects of this class and show their field values");
        ImGui::Spacing();
        ui::Toggle("Show inherited members", &g_show_inherited);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Include methods & fields from superclasses (shown dimmed)");
        ImGui::Separator();

        // Member source chain: this class, plus (optionally) its superclasses.
        // The hop cap is a cycle/corruption guard — real Java hierarchies are shallow.
        std::vector<const viewer::ClassInfo*> chain{ &c };
        if (g_show_inherited)
        {
            const viewer::ClassInfo* cur{ &c };
            for (int hops = 0; hops < 200; ++hops)
            {
                if (cur->super_name.empty() || cur->super_name == "java/lang/Object") break;
                const auto it{ app.name_to_index.find(cur->super_name) };
                if (it == app.name_to_index.end()) break;
                cur = &app.classes[(std::size_t)it->second];
                if (cur == &c) break;  // defensive: never loop back to the start
                chain.push_back(cur);
            }
        }
        struct MRow { const viewer::MethodInfo* m; const viewer::ClassInfo* owner; };
        struct FRow { const viewer::FieldInfo*  f; const viewer::ClassInfo* owner; };
        std::vector<MRow> meths; std::vector<FRow> flds;
        for (const viewer::ClassInfo* kc : chain)
        {
            for (const auto& m : kc->methods) meths.push_back({ &m, kc });
            for (const auto& f : kc->fields)  flds.push_back({ &f, kc });
        }
        const auto owner_tooltip{ [&](const viewer::ClassInfo* owner)
        {
            if (owner == &c || !ImGui::IsItemHovered()) return;
            std::string od{ owner->internal_name };
            for (char& ch : od) if (ch == '/') ch = '.';
            ImGui::SetTooltip("inherited from %s", od.c_str());
        } };

        const float half{ ImGui::GetContentRegionAvail().x * 0.5f - em(0.25f) };

        // Methods
        ImGui::BeginChild("methods", ImVec2(half, 0), ImGuiChildFlags_Borders);
        ImGui::Text("Methods (%zu)", meths.size());
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(ICON_FA_SEARCH);
        ImGui::SameLine(0.0f, em(0.4f));
        ui::InputText("##mf", "Filter methods", g_method_filter, sizeof(g_method_filter), -1.0f);
        {
            const std::string mf{ g_method_filter };
            static std::vector<int> mrows;
            mrows.clear();
            for (int i = 0; i < (int)meths.size(); ++i)
                if (icontains(meths[(std::size_t)i].m->name, mf) || icontains(meths[(std::size_t)i].m->descriptor, mf))
                    mrows.push_back(i);
            if (ImGui::BeginTable("mt", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable))
            {
                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, em(9.0f));
                ImGui::TableSetupColumn("Access", ImGuiTableColumnFlags_WidthFixed, em(9.5f));
                ImGui::TableSetupColumn(g_pretty ? "Signature" : "Descriptor", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableHeadersRow();
                ImGuiListClipper clip; clip.Begin((int)mrows.size());
                while (clip.Step())
                    for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r)
                    {
                        const MRow& row{ meths[(std::size_t)mrows[(std::size_t)r]] };
                        const viewer::MethodInfo& m{ *row.m };
                        const bool inh{ row.owner != &c };
                        ImGui::TableNextRow(); ImGui::TableNextColumn();
                        ImGui::PushID(r);
                        if (inh) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.60f, 0.62f, 0.70f, 1.0f));
                        ImGui::TextUnformatted(m.name.c_str());
                        if (inh) ImGui::PopStyleColor();
                        owner_tooltip(row.owner);
                        copy_menu("m", m.name, m.descriptor);
                        ImGui::TableNextColumn();
                        ImGui::PushStyleColor(ImGuiCol_Text, vis_color(m.access));
                        ImGui::TextUnformatted(viewer::access_modifiers(m.access, true).c_str());
                        ImGui::PopStyleColor();
                        ImGui::TableNextColumn();
                        render_method_signature(app, m.descriptor);
                        ImGui::PopID();
                    }
                ImGui::EndTable();
            }
        }
        ImGui::EndChild();

        ImGui::SameLine();

        // Fields
        ImGui::BeginChild("fields", ImVec2(0, 0), ImGuiChildFlags_Borders);
        ImGui::Text("Fields (%zu)", flds.size());
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(ICON_FA_SEARCH);
        ImGui::SameLine(0.0f, em(0.4f));
        ui::InputText("##ff", "Filter fields", g_field_filter, sizeof(g_field_filter), -1.0f);
        {
            const std::string ff{ g_field_filter };
            static std::vector<int> frows;
            frows.clear();
            for (int i = 0; i < (int)flds.size(); ++i)
                if (icontains(flds[(std::size_t)i].f->name, ff) || icontains(flds[(std::size_t)i].f->descriptor, ff))
                    frows.push_back(i);
            if (ImGui::BeginTable("ft", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable))
            {
                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, em(8.5f));
                ImGui::TableSetupColumn("Access", ImGuiTableColumnFlags_WidthFixed, em(9.5f));
                ImGui::TableSetupColumn(g_pretty ? "Type" : "Descriptor", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableHeadersRow();
                ImGuiListClipper clip; clip.Begin((int)frows.size());
                while (clip.Step())
                    for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r)
                    {
                        const FRow& row{ flds[(std::size_t)frows[(std::size_t)r]] };
                        const viewer::FieldInfo& f{ *row.f };
                        const bool inh{ row.owner != &c };
                        ImGui::TableNextRow(); ImGui::TableNextColumn();
                        ImGui::PushID(r);
                        if (inh) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.60f, 0.62f, 0.70f, 1.0f));
                        ImGui::TextUnformatted(f.name.c_str());
                        if (inh) ImGui::PopStyleColor();
                        owner_tooltip(row.owner);
                        copy_menu("f", f.name, f.descriptor);
                        ImGui::TableNextColumn();
                        ImGui::PushStyleColor(ImGuiCol_Text, vis_color(f.access));
                        ImGui::TextUnformatted(viewer::access_modifiers(f.access, false).c_str());
                        ImGui::PopStyleColor();
                        ImGui::TableNextColumn();
                        const std::string ty{ g_pretty ? viewer::pretty_field(f.descriptor, g_full_names) : f.descriptor };
                        const std::string ref{ ref_internal_name(f.descriptor) };
                        const auto rit{ ref.empty() ? app.name_to_index.end() : app.name_to_index.find(ref) };
                        if (rit != app.name_to_index.end())
                        {
                            if (ImGui::TextLink(ty.c_str())) navigate_to(rit->second);
                        }
                        else
                        {
                            ImGui::TextUnformatted(ty.c_str());
                        }
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("%s%s", f.descriptor.c_str(),
                                rit != app.name_to_index.end() ? "  (click to open)" : "");
                        ImGui::PopID();
                    }
                ImGui::EndTable();
            }
        }
        ImGui::EndChild();
    }

    // Floating window listing the live heap instances of a class (one row per
    // instance: heap address + one column per declared instance field, cells =
    // field values).  Auto-refreshes while "Live" so values update in real time.
    // Case-insensitive substring test (needle must already be lower-cased).
    bool contains_ci(const std::string& hay, const std::string& lower_needle)
    {
        if (lower_needle.empty())            return true;
        if (lower_needle.size() > hay.size()) return false;
        std::string h;
        h.reserve(hay.size());
        for (char c : hay) h.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        return h.find(lower_needle) != std::string::npos;
    }

    // Compare two field-value strings numerically when BOTH parse as a full
    // number (so ticks 9 < 10, not "10" < "9"), else lexicographically.
    int cmp_num_or_str(const std::string& x, const std::string& y)
    {
        auto as_num = [](const std::string& s, double& out) -> bool
        {
            if (s.empty() || s == "null") return false;
            char* end{ nullptr };
            out = std::strtod(s.c_str(), &end);
            return end != nullptr && *end == '\0';
        };
        double nx{}, ny{};
        if (as_num(x, nx) && as_num(y, ny)) return nx < ny ? -1 : nx > ny ? 1 : 0;
        const int c{ x.compare(y) };
        return c < 0 ? -1 : c > 0 ? 1 : 0;
    }

    // Order two instances by table column `col` (0=#, 1=Address, 2+=field).
    int cmp_instance(const viewer::InstanceInfo& A, const viewer::InstanceInfo& B,
                     int col, int ia, int ib)
    {
        if (col <= 0) return ia < ib ? -1 : ia > ib ? 1 : 0;
        if (col == 1)
        {
            const unsigned long long x{ std::strtoull(A.address.c_str(), nullptr, 16) };
            const unsigned long long y{ std::strtoull(B.address.c_str(), nullptr, 16) };
            return x < y ? -1 : x > y ? 1 : 0;
        }
        static const std::string empty{};
        const int fi{ col - 2 };
        const std::string& xs{ fi < (int)A.fields.size() ? A.fields[(std::size_t)fi].value : empty };
        const std::string& ys{ fi < (int)B.fields.size() ? B.fields[(std::size_t)fi].value : empty };
        return cmp_num_or_str(xs, ys);
    }

    // Colour a formatted field value by its kind so the table scans easily:
    // strings, object refs (<...>), booleans, and null each get their own tint.
    ImVec4 value_color(const std::string& v)
    {
        if (v == "null")                 return ImVec4(0.55f, 0.57f, 0.62f, 1.0f);  // dim grey
        if (v == "true" || v == "false") return ImVec4(0.90f, 0.66f, 0.40f, 1.0f);  // amber
        if (!v.empty() && v.front() == '"') return ImVec4(0.56f, 0.81f, 0.58f, 1.0f);  // string green
        if (!v.empty() && v.front() == '<') return ImVec4(0.56f, 0.71f, 0.96f, 1.0f);  // ref blue
        return ImGui::GetStyleColorVec4(ImGuiCol_Text);                             // number/default
    }

    void render_field_value(const std::string& v)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, value_color(v));
        ImGui::TextUnformatted(v.c_str());
        ImGui::PopStyleColor();
    }

    void draw_instances_window(viewer::App& app)
    {
        ImGui::SetNextWindowSize(ImVec2(em(58.0f), em(32.0f)), ImGuiCond_FirstUseEver);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em(0.85f), em(0.7f)));
        const bool open{ ImGui::Begin("Live instances", &g_show_instances, ImGuiWindowFlags_NoCollapse) };
        ImGui::PopStyleVar();
        if (!open) { ImGui::End(); return; }

        std::lock_guard<std::mutex> lock{ app.data_mutex };
        const viewer::Status st{ app.inst_status.load() };

        // Header: class name (+ tiny spinner while scanning) ... Live | Refresh.
        std::string dotted{ app.inst_class };
        for (char& ch : dotted) if (ch == '/') ch = '.';
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.70f, 0.82f, 1.0f, 1.0f));
        ImGui::TextUnformatted(dotted.empty() ? "(no class selected)" : dotted.c_str());
        ImGui::PopStyleColor();
        if (st == viewer::Status::Receiving)
        {
            ImGui::SameLine(0.0f, em(0.5f));
            const float r{ ImGui::GetFrameHeight() * 0.28f };
            ui::Spinner("##iscan", r, (std::max)(r * 0.35f, em(0.12f)), ImGui::GetColorU32(ImVec4(0.34f, 0.63f, 1.0f, 1.0f)));
        }
        ImGui::SameLine((std::max)(ImGui::GetContentRegionMax().x - em(16.5f), ImGui::GetCursorPosX() + em(1.0f)));
        ui::Toggle("Live", &g_instances_live);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Re-scan the heap ~every 1.5s so field values update live");
        ImGui::SameLine(0.0f, em(0.7f));
        if (ui::Button("Refresh")) g_instances_refresh_now = true;
        ImGui::SameLine(0.0f, em(0.4f));
        if (ui::Button("Copy table")) g_copy_instance_table = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Copy the filtered + sorted rows to the clipboard as TSV");

        // Columns = indices into the streamed fields (declared then inherited),
        // filtered by the "Inherited" toggle.  Carrying the field index (rather
        // than name/owner) keeps value lookup + sorting correct when inherited
        // columns are hidden.  Taken from the first instance — every instance of
        // the same class streams the same fields.
        std::vector<int> cols;
        if (!app.instances.empty())
            for (int fi = 0; fi < (int)app.instances.front().fields.size(); ++fi)
                if (g_inst_show_inherited || app.instances.front().fields[(std::size_t)fi].owner.empty())
                    cols.push_back(fi);

        // Filter order: keep the rows whose address or any value matches (the
        // InputText below updates g_instance_filter, so this is last frame's
        // text — a 1-frame lag that's imperceptible).
        std::string needle{ g_instance_filter };
        for (char& ch : needle) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        std::vector<int> view;
        view.reserve(app.instances.size());
        for (int i = 0; i < (int)app.instances.size(); ++i)
        {
            if (needle.empty()) { view.push_back(i); continue; }
            const viewer::InstanceInfo& in{ app.instances[(std::size_t)i] };
            bool hit{ contains_ci(in.address, needle) };
            for (const auto& fv : in.fields) if (!hit && contains_ci(fv.value, needle)) hit = true;
            if (hit) view.push_back(i);
        }

        // "Found N" (+ filtered count + cap warning) on one line.
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", app.inst_message.c_str());
        if (!needle.empty())
        {
            ImGui::SameLine(0.0f, em(0.5f));
            ImGui::TextDisabled("· %d shown", (int)view.size());
        }
        if (app.inst_cap > 0 && (int)app.instances.size() >= app.inst_cap)
        {
            ImGui::SameLine(0.0f, em(0.6f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.75f, 0.35f, 1.0f));
            ImGui::TextUnformatted("scan cap reached — raise it to see more");
            ImGui::PopStyleColor();
        }

        // Row: substring filter (address or any value) + inherited toggle + cap.
        ui::InputText("##ifilter", ICON_FA_SEARCH "  Filter instances",
                      g_instance_filter, sizeof(g_instance_filter),
                      (std::max)(ImGui::GetContentRegionAvail().x - em(18.5f), em(6.0f)));
        ImGui::SameLine(0.0f, em(0.6f));
        ImGui::Checkbox("Inherited", &g_inst_show_inherited);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show columns for fields inherited from a superclass");
        ImGui::SameLine(0.0f, em(0.6f));
        ImGui::SetNextItemWidth(em(8.5f));
        int cap{ app.inst_cap };
        if (ImGui::DragInt("##icap", &cap, 10.0f, 20, 200000, "cap %d"))
        {
            app.inst_cap = std::clamp(cap, 20, 200000);
            g_instance_cap = app.inst_cap;   // remember for next launch
            g_instances_refresh_now = true;  // re-scan with the new cap
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Max instances to scan for on the heap (drag or double-click to edit)");

        ImGui::Separator();
        if (app.instances.empty())
        {
            // Show guidance once a scan has actually completed with 0 hits (the
            // worker sets "Found 0…"); it persists across live re-scans, unlike a
            // status check which a 0-instance class spends re-Receiving.
            if (app.inst_message.rfind("Found 0", 0) == 0)
            {
                ImGui::TextDisabled("No live instances of this exact class on the heap.");
                ImGui::TextDisabled("The scan matches this class only — if it is abstract, or just its");
                ImGui::TextDisabled("subclasses are instantiated, inspect a subclass instead.");
            }
        }
        else if (ImGui::BeginTable("instances", 2 + (int)cols.size(),
                     ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                     ImGuiTableFlags_ScrollX | ImGuiTableFlags_Resizable |
                     ImGuiTableFlags_Sortable | ImGuiTableFlags_SortMulti))
        {
            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort, em(2.8f));
            ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, em(11.0f));
            for (const int fi : cols)
                ImGui::TableSetupColumn(app.instances.front().fields[(std::size_t)fi].name.c_str(), ImGuiTableColumnFlags_WidthFixed, em(11.0f));
            ImGui::TableSetupScrollFreeze(2, 1);

            // Header row (manual, so inherited columns render dimmed + tooltipped).
            ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
            for (int c = 0; c < 2 + (int)cols.size(); ++c)
            {
                if (!ImGui::TableSetColumnIndex(c)) continue;
                const std::string* owner{ c >= 2 ? &app.instances.front().fields[(std::size_t)cols[(std::size_t)(c - 2)]].owner : nullptr };
                const bool inherited{ owner != nullptr && !owner->empty() };
                if (inherited) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.60f, 0.72f, 1.0f));
                ImGui::TableHeader(ImGui::TableGetColumnName(c));
                if (inherited) ImGui::PopStyleColor();
                if (inherited && ImGui::IsItemHovered())
                    ImGui::SetTooltip("inherited from %s", owner->c_str());
            }

            if (ImGuiTableSortSpecs* ss{ ImGui::TableGetSortSpecs() }; ss && ss->SpecsCount > 0)
            {
                std::stable_sort(view.begin(), view.end(), [&](int a, int b)
                {
                    for (int s = 0; s < ss->SpecsCount; ++s)
                    {
                        const ImGuiTableColumnSortSpecs& sp{ ss->Specs[s] };
                        // Map the (possibly filtered) table column to the real field
                        // index, biased by +2 so cmp_instance's (col-2) recovers it.
                        const int vcol{ (sp.ColumnIndex >= 2 && sp.ColumnIndex - 2 < (int)cols.size())
                                            ? cols[(std::size_t)(sp.ColumnIndex - 2)] + 2 : (int)sp.ColumnIndex };
                        const int c{ cmp_instance(app.instances[(std::size_t)a], app.instances[(std::size_t)b],
                                                  vcol, a, b) };
                        if (c != 0)
                            return sp.SortDirection == ImGuiSortDirection_Ascending ? c < 0 : c > 0;
                    }
                    return a < b;
                });
            }

            ImGuiListClipper clip;
            clip.Begin((int)view.size());
            while (clip.Step())
                for (int vr = clip.DisplayStart; vr < clip.DisplayEnd; ++vr)
                {
                    const int r{ view[(std::size_t)vr] };
                    const viewer::InstanceInfo& inst{ app.instances[(std::size_t)r] };
                    ImGui::TableNextRow();
                    ImGui::PushID(r);
                    ImGui::TableSetColumnIndex(0);
                    char idxlbl[24];
                    std::snprintf(idxlbl, sizeof(idxlbl), "%d", r);
                    if (ImGui::Selectable(idxlbl, g_detail_addr == inst.address,
                            ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
                    {
                        g_detail_addr = inst.address;
                        g_open_instance_detail = true;
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click for full detail");
                    ImGui::TableSetColumnIndex(1);
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.58f, 0.68f, 0.82f, 1.0f));
                    ImGui::TextUnformatted(inst.address.c_str());
                    ImGui::PopStyleColor();
                    copy_menu("addr", inst.address);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s  (right-click to copy)", inst.address.c_str());
                    for (int cidx = 0; cidx < (int)cols.size(); ++cidx)
                    {
                        ImGui::TableSetColumnIndex(cidx + 2);
                        const int fi{ cols[(std::size_t)cidx] };
                        if (fi >= (int)inst.fields.size()) continue;
                        const std::string& v{ inst.fields[(std::size_t)fi].value };
                        render_field_value(v);
                        if (ImGui::IsItemHovered() && v.size() > 18) ImGui::SetTooltip("%s", v.c_str());
                    }
                    ImGui::PopID();
                }
            ImGui::EndTable();
        }

        // Copy the filtered + sorted table to the clipboard as TSV (view holds
        // the current display order after the sort above).
        if (g_copy_instance_table)
        {
            g_copy_instance_table = false;
            std::string tsv{ "#\tAddress" };
            for (const int fi : cols) { tsv += '\t'; tsv += app.instances.front().fields[(std::size_t)fi].name; }
            tsv += '\n';
            for (const int rr : view)
            {
                const viewer::InstanceInfo& in{ app.instances[(std::size_t)rr] };
                tsv += std::to_string(rr);
                tsv += '\t';
                tsv += in.address;
                for (const int fi : cols) { tsv += '\t'; if (fi < (int)in.fields.size()) tsv += in.fields[(std::size_t)fi].value; }
                tsv += '\n';
            }
            ImGui::SetClipboardText(tsv.c_str());
        }

        // Per-instance detail popup (click a row): a vertical Field/Value/From
        // view — readable for objects with many columns, and live (re-looked-up
        // by address each frame so its values keep updating).
        if (g_open_instance_detail) { ImGui::OpenPopup("Instance detail"); g_open_instance_detail = false; }
        if (ImGui::BeginPopup("Instance detail"))
        {
            const viewer::InstanceInfo* sel{ nullptr };
            for (const auto& in : app.instances) if (in.address == g_detail_addr) { sel = &in; break; }
            if (!sel)
            {
                ImGui::TextDisabled("(instance no longer on the heap)");
            }
            else
            {
                // class name @ address, so the (movable) popup is self-describing.
                std::string dcls{ app.inst_class };
                for (char& ch : dcls) if (ch == '/') ch = '.';
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.70f, 0.82f, 1.0f, 1.0f));
                ImGui::TextUnformatted(dcls.c_str());
                ImGui::PopStyleColor();
                ImGui::SameLine(0.0f, em(0.35f));
                ImGui::TextDisabled("@");
                ImGui::SameLine(0.0f, em(0.35f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.58f, 0.68f, 0.82f, 1.0f));
                ImGui::TextUnformatted(sel->address.c_str());
                ImGui::PopStyleColor();
                ImGui::SameLine(0.0f, em(0.8f));
                if (ui::Button("Copy addr")) ImGui::SetClipboardText(sel->address.c_str());
                ImGui::SameLine(0.0f, em(0.4f));
                if (ui::Button("Copy all"))
                {
                    std::string tsv{ dcls + " @ " + sel->address + "\nField\tValue\tFrom\n" };
                    for (const viewer::InstField& f : sel->fields)
                    {
                        tsv += f.name; tsv += '\t'; tsv += f.value; tsv += '\t'; tsv += f.owner; tsv += '\n';
                    }
                    ImGui::SetClipboardText(tsv.c_str());
                }
                ImGui::Separator();
                if (ImGui::BeginTable("idetail", 3,
                        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                        ImVec2(em(32.0f), em(18.0f))))
                {
                    ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthFixed, em(9.0f));
                    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("From",  ImGuiTableColumnFlags_WidthFixed, em(7.0f));
                    ImGui::TableHeadersRow();
                    for (const viewer::InstField& f : sel->fields)
                    {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextUnformatted(f.name.c_str());
                        ImGui::TableSetColumnIndex(1);
                        // A "<internal/name>" ref value whose class is loaded becomes a
                        // link that navigates to it (jump from a field to its type).
                        bool linked{ false };
                        if (f.value.size() > 2 && f.value.front() == '<' && f.value.back() == '>')
                        {
                            const std::string internal{ f.value.substr(1, f.value.size() - 2) };
                            if (const auto it{ app.name_to_index.find(internal) }; it != app.name_to_index.end())
                            {
                                if (ImGui::TextLink(f.value.c_str())) navigate_to(it->second);
                                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s  (click to open)", internal.c_str());
                                linked = true;
                            }
                        }
                        if (!linked)
                        {
                            ImGui::PushStyleColor(ImGuiCol_Text, value_color(f.value));
                            ImGui::TextWrapped("%s", f.value.c_str());
                            ImGui::PopStyleColor();
                        }
                        ImGui::TableSetColumnIndex(2);
                        if (!f.owner.empty()) ImGui::TextDisabled("%s", f.owner.c_str());
                    }
                    ImGui::EndTable();
                }
            }
            ImGui::EndPopup();
        }
        ImGui::End();
    }

    void render_ui(viewer::App& app)
    {
        // Ctrl+F focuses the class search; Esc clears all filters.
        if (ImGui::IsKeyDown(ImGuiKey_LeftCtrl) && ImGui::IsKeyPressed(ImGuiKey_F, false)) g_focus_search = true;
        if (ImGui::IsKeyPressed(ImGuiKey_Escape) && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId))
        {
            g_search[0] = 0; g_method_filter[0] = 0; g_field_filter[0] = 0; g_instance_filter[0] = 0;
        }
        // Alt+Left / Alt+Right walk the class navigation history (browser-style).
        if (ImGui::IsKeyDown(ImGuiKey_LeftAlt) || ImGui::IsKeyDown(ImGuiKey_RightAlt))
        {
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow,  false)) nav_back();
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, false)) nav_forward();
        }
        // Ctrl +/-/0 scales the UI font (accessibility / dense-listing zoom).
        if (ImGui::GetIO().KeyCtrl)
        {
            ImGuiIO& kio{ ImGui::GetIO() };
            if (ImGui::IsKeyPressed(ImGuiKey_Equal, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd, false))
                kio.FontGlobalScale = std::clamp(kio.FontGlobalScale + 0.1f, 0.7f, 2.0f);
            if (ImGui::IsKeyPressed(ImGuiKey_Minus, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract, false))
                kio.FontGlobalScale = std::clamp(kio.FontGlobalScale - 0.1f, 0.7f, 2.0f);
            if (ImGui::IsKeyPressed(ImGuiKey_0, false) || ImGui::IsKeyPressed(ImGuiKey_Keypad0, false))
                kio.FontGlobalScale = 1.0f;
        }

        // Auto-refresh the JVM list every 2s so new/closed JVMs appear without a
        // manual Refresh (selection is preserved by pid).
        static double last_refresh{ 0.0 };
        if (!app.busy() && (ImGui::GetTime() - last_refresh) > 2.0)
        {
            app.refresh_jvms();
            last_refresh = ImGui::GetTime();
        }

        // Live class-load tracking: arm the on_class_loaded hook and poll it
        // ~every 1.5s (cheap; an immediate re-scan fires the moment the hook sees
        // a class defined), with a full re-scan every ~3s as a safety net that
        // also catches bootstrap classes the hook can't see.
        static double last_poll{ 0.0 }, last_full{ 0.0 };
        if (g_auto_rescan && app.has_baseline.load() && !app.busy() && !app.inst_busy() &&
            (ImGui::GetTime() - last_poll) > 1.5)
        {
            const bool full{ (ImGui::GetTime() - last_full) > 3.0 };
            app.auto_track(full);
            last_poll = ImGui::GetTime();
            if (full) last_full = ImGui::GetTime();
        }

        const ImGuiViewport* vp{ ImGui::GetMainViewport() };
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("root", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        draw_toolbar(app);
        // Drag the borderless window by the empty part of the top toolbar strip.
        const float toolbar_bottom{ ImGui::GetCursorScreenPos().y };
        if (g_hwnd && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
            !ImGui::IsAnyItemHovered() && !ImGui::IsAnyItemActive() &&
            ImGui::GetIO().MousePos.y >= vp->WorkPos.y && ImGui::GetIO().MousePos.y < toolbar_bottom)
        {
            ReleaseCapture();
            SendMessageW(g_hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            ImGui::GetIO().MouseDown[0] = false;  // the native move loop ate the button-up
        }
        ImGui::Separator();

        // main split: left classes / right details, with a draggable splitter
        const float avail_y{ ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() };
        ImGui::BeginChild("left", ImVec2(g_left_width, avail_y), ImGuiChildFlags_Borders);
        draw_class_list(app);
        ImGui::EndChild();

        ImGui::SameLine();
        ImGui::InvisibleButton("splitter", ImVec2(em(0.45f), avail_y));
        if (ImGui::IsItemActive()) g_left_width += ImGui::GetIO().MouseDelta.x;
        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        g_left_width = std::clamp(g_left_width, em(14.0f), vp->WorkSize.x - em(20.0f));
        {
            const ImVec2 p0{ ImGui::GetItemRectMin() }, p1{ ImGui::GetItemRectMax() };
            const float x{ (p0.x + p1.x) * 0.5f };
            const ImU32 col{ ImGui::GetColorU32(ImGui::IsItemActive() ? ImGuiCol_ButtonActive : ImGuiCol_Border) };
            ImGui::GetWindowDrawList()->AddLine(ImVec2(x, p0.y + em(0.15f)), ImVec2(x, p1.y - em(0.15f)), col, (std::max)(em(0.12f), 1.0f));
        }
        ImGui::SameLine();
        ImGui::BeginChild("right", ImVec2(0, avail_y), ImGuiChildFlags_Borders);
        draw_details(app);
        ImGui::EndChild();

        // status bar
        ImGui::Separator();
        {
            std::size_t nclasses{ 0 }, nmethods{ 0 }, nfields{ 0 }, nremoved{ 0 };
            {
                std::lock_guard<std::mutex> lock{ app.data_mutex };
                nclasses = app.classes.size();
                for (const auto& c : app.classes) { nmethods += c.methods.size(); nfields += c.fields.size(); }
                nremoved = app.last_removed.size();
            }
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%zu classes  \xC2\xB7  %zu methods  \xC2\xB7  %zu fields", nclasses, nmethods, nfields);
            if (nremoved > 0)
            {
                ImGui::SameLine(0.0f, em(0.6f));
                const ImVec4 red{ 0.92f, 0.56f, 0.46f, 1.0f };
                ImGui::PushStyleColor(ImGuiCol_Text, red);
                ImGui::PushStyleColor(ImGuiCol_TextLink, red);
                char lbl[40];
                std::snprintf(lbl, sizeof(lbl), "-%zu unloaded", nremoved);
                if (ImGui::TextLink(lbl)) g_show_removed = true;
                ImGui::PopStyleColor(2);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Classes unloaded since the previous scan — click to list");
            }
            if (app.hook_armed.load())
            {
                ImGui::SameLine(0.0f, em(0.6f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.72f, 1.0f, 1.0f));
                ImGui::Text("\xef\x80\xa1 hook: %llu", (unsigned long long)app.hook_total.load());
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("on_class_loaded hook armed — %llu class(es) defined via ClassLoader.defineClass so far",
                                      (unsigned long long)app.hook_total.load());
            }
            ImGui::SameLine(0.0f, em(0.8f));

            // Status message — coloured by severity and ellipsized so a long error
            // can never run under / push the toggles off the right edge.
            const auto ellipsize{ [](const std::string& s, float max_w) -> std::string
            {
                if (ImGui::CalcTextSize(s.c_str()).x <= max_w) return s;
                std::string out{ s };
                while (!out.empty() && ImGui::CalcTextSize((out + "...").c_str()).x > max_w) out.pop_back();
                return out + "...";
            } };
            const float toggles_w{ em(23.0f) };
            const float msg_max{ (std::max)(ImGui::GetContentRegionMax().x - toggles_w - ImGui::GetCursorPosX() - em(0.8f), em(3.0f)) };
            {
                std::lock_guard<std::mutex> lock{ app.data_mutex };
                const viewer::Status st{ app.status.load() };
                const ImVec4 mcol{ st == viewer::Status::Error ? ImVec4(0.95f, 0.46f, 0.46f, 1.0f)
                                 : st == viewer::Status::Done  ? ImVec4(0.55f, 0.78f, 0.60f, 1.0f)
                                 : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled) };
                const std::string shown{ ellipsize(app.status_message, msg_max) };
                ImGui::AlignTextToFramePadding();
                ImGui::PushStyleColor(ImGuiCol_Text, mcol);
                ImGui::TextUnformatted(shown.c_str());
                ImGui::PopStyleColor();
                if (shown.size() != app.status_message.size() && ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", app.status_message.c_str());
            }

            ImGui::SameLine((std::max)(ImGui::GetContentRegionMax().x - toggles_w, ImGui::GetCursorPosX() + em(0.5f)));
            ui::Toggle("Pretty signatures", &g_pretty);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show human-readable signatures/types instead of raw JVM descriptors");
            ImGui::SameLine(0.0f, em(0.9f));
            ImGui::BeginDisabled(!g_pretty);
            ui::Toggle("Full names", &g_full_names);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show fully-qualified type names (java.lang.String) instead of simple ones (String)");
        }

        // Popup listing the classes unloaded at the last re-scan (they're gone
        // from the list, so this is the only place to see them).
        if (g_show_removed) { ImGui::OpenPopup("Unloaded classes"); g_show_removed = false; }
        if (ImGui::BeginPopup("Unloaded classes"))
        {
            ImGui::SeparatorText("Unloaded since the previous scan");
            std::lock_guard<std::mutex> lock{ app.data_mutex };
            if (app.last_removed.empty())
                ImGui::TextDisabled("(none)");
            else
            {
                ImGui::BeginChild("rmlist", ImVec2(em(26.0f), (std::min)((float)app.last_removed.size() + 0.5f, 14.0f) * em(1.3f)));
                for (const std::string& nm : app.last_removed)
                {
                    std::string d{ nm };
                    for (char& ch : d) if (ch == '/') ch = '.';
                    ImGui::TextUnformatted(d.c_str());
                }
                ImGui::EndChild();
            }
            ImGui::EndPopup();
        }

        ImGui::End();

        // Live instances: (re)scan the heap outside the root window's Begin/End
        // and outside draw_details' data_mutex lock (request_instances re-locks
        // it).  The next auto-scan is scheduled a fixed delay AFTER the previous
        // one COMPLETES (not after it starts) — otherwise a heap scan slower than
        // the interval would re-fire back-to-back and never show a result.
        if (g_show_instances && !g_instances_class.empty())
        {
            static bool   was_busy{ false };
            static double next_scan{ 0.0 };
            const bool now_busy{ app.inst_busy() };
            if (was_busy && !now_busy) next_scan = ImGui::GetTime() + 1.2;  // just finished
            was_busy = now_busy;

            const bool due{ g_instances_refresh_now || (g_instances_live && ImGui::GetTime() >= next_scan) };
            if (due && !now_busy)
            {
                app.request_instances(g_instances_class);
                g_instances_refresh_now = false;
                next_scan = ImGui::GetTime() + 1.0e9;  // reset on completion above
            }
        }
        if (g_show_instances)
        {
            draw_instances_window(app);
        }
    }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
    // Per-monitor DPI awareness (V2) so the window renders crisp at native
    // resolution on high-DPI displays instead of being bitmap-stretched.
    // Loaded dynamically to stay independent of the SDK header version.
    if (HMODULE user32{ GetModuleHandleW(L"user32.dll") })
    {
        using set_ctx_fn = BOOL(WINAPI*)(HANDLE);
        if (auto set_ctx{ reinterpret_cast<set_ctx_fn>(GetProcAddress(user32, "SetProcessDpiAwarenessContext")) })
            set_ctx(reinterpret_cast<HANDLE>(-4));  // PER_MONITOR_AWARE_V2
    }

    WNDCLASSEXW wc{ sizeof(wc), CS_CLASSDC, WndProc, 0, 0, GetModuleHandleW(nullptr), nullptr, LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW)), nullptr, nullptr, L"vmhook_viewer", nullptr };
    // App icon (resource id 1 from assets/vmhook.rc) for the taskbar / alt-tab.
    wc.hIcon   = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);
    // Borderless (no native title bar) — a custom in-app toolbar provides the
    // minimize/close controls.  WS_MAXIMIZEBOX + WM_GETMINMAXINFO make it fill
    // the monitor work area (respecting the taskbar); WS_MINIMIZEBOX/WS_SYSMENU
    // keep taskbar minimize/restore working.
    HWND hwnd{ CreateWindowW(wc.lpszClassName, L"vmhook viewer",
                             WS_POPUP | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU | WS_CLIPCHILDREN,
                             80, 80, 1400, 880, nullptr, nullptr, wc.hInstance, nullptr) };
    g_hwnd = hwnd;

    if (!create_device(hwnd)) { cleanup_device(); UnregisterClassW(wc.lpszClassName, wc.hInstance); return 1; }
    ShowWindow(hwnd, SW_SHOWMAXIMIZED);
    UpdateWindow(hwnd);

    // Resolve the window's DPI scale (physical dpi / 96) for font + style scaling.
    if (HMODULE user32{ GetModuleHandleW(L"user32.dll") })
    {
        using get_dpi_fn = UINT(WINAPI*)(HWND);
        if (auto get_dpi{ reinterpret_cast<get_dpi_fn>(GetProcAddress(user32, "GetDpiForWindow")) })
        {
            const UINT dpi{ get_dpi(hwnd) };
            if (dpi >= 48u) g_dpi_scale = static_cast<float>(dpi) / 96.0f;
        }
    }
    // Default the class list to a quarter of the screen (load_settings may
    // override with a previously dragged width).
    {
        MONITORINFO mi{ sizeof(mi) };
        if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi))
            g_left_width = static_cast<float>(mi.rcWork.right - mi.rcWork.left) * 0.25f;
        else
            g_left_width *= g_dpi_scale;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::GetIO().IniFilename = nullptr;
    apply_modern_style();
    ImGui::GetStyle().ScaleAllSizes(g_dpi_scale);  // scale padding/rounding to DPI
    load_fonts(g_dpi_scale);
    load_settings();  // restore the user's remembered UI preferences
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    g_dll_path = payload_dll_path();
    viewer::App app{};
    app.inst_cap = g_instance_cap;  // apply the remembered heap-scan cap

    bool running{ true };
    while (running)
    {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg); DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) running = false;
        }
        if (!running) break;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        render_ui(app);
        ImGui::Render();
        const float clear[4]{ 0.055f, 0.06f, 0.072f, 1.0f };
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_context->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swap_chain->Present(1, 0);

        // Reflect the loaded class count in the window title (only on change).
        {
            static std::string last_title;
            const unsigned long long n{ app.classes_streamed.load() };
            std::string title{ n ? "vmhook viewer  —  " + std::to_string(n) + " classes" : "vmhook viewer" };
            if (title != last_title) { last_title = title; SetWindowTextA(hwnd, title.c_str()); }
        }
    }

    save_settings();  // remember preferences for next launch
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    cleanup_device();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}
