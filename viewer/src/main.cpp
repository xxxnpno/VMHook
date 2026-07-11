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
    void apply_modern_style()
    {
        ImGuiStyle& s{ ImGui::GetStyle() };
        // Rounding — consistent, soft.
        s.WindowRounding = 8.0f; s.ChildRounding = 8.0f; s.FrameRounding = 6.0f;
        s.PopupRounding = 8.0f; s.GrabRounding = 6.0f; s.ScrollbarRounding = 10.0f; s.TabRounding = 6.0f;
        // Spacing / padding — roomier so controls breathe and line up.
        s.WindowPadding = ImVec2(14, 12); s.FramePadding = ImVec2(11, 7);
        s.ItemSpacing = ImVec2(9, 8); s.ItemInnerSpacing = ImVec2(8, 6); s.CellPadding = ImVec2(9, 5);
        s.ScrollbarSize = 14.0f; s.GrabMinSize = 12.0f;
        s.WindowBorderSize = 0.0f; s.FrameBorderSize = 1.0f; s.ChildBorderSize = 1.0f; s.PopupBorderSize = 1.0f;
        s.SeparatorTextBorderSize = 2.0f; s.SeparatorTextPadding = ImVec2(20, 6);
        s.WindowTitleAlign = ImVec2(0.0f, 0.5f);

        ImVec4* c{ s.Colors };
        const ImVec4 accent  { 0.26f, 0.56f, 0.96f, 1.00f };
        const ImVec4 accentHi{ 0.34f, 0.63f, 1.00f, 1.00f };
        c[ImGuiCol_Text]                 = ImVec4(0.92f, 0.93f, 0.96f, 1.00f);
        c[ImGuiCol_TextDisabled]         = ImVec4(0.46f, 0.49f, 0.56f, 1.00f);
        c[ImGuiCol_WindowBg]             = ImVec4(0.070f, 0.074f, 0.090f, 1.00f);
        c[ImGuiCol_ChildBg]              = ImVec4(0.100f, 0.106f, 0.126f, 1.00f);
        c[ImGuiCol_PopupBg]              = ImVec4(0.115f, 0.122f, 0.145f, 0.99f);
        c[ImGuiCol_Border]               = ImVec4(1.00f, 1.00f, 1.00f, 0.090f);
        c[ImGuiCol_BorderShadow]         = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
        c[ImGuiCol_FrameBg]              = ImVec4(0.155f, 0.165f, 0.200f, 1.00f);
        c[ImGuiCol_FrameBgHovered]       = ImVec4(0.200f, 0.215f, 0.260f, 1.00f);
        c[ImGuiCol_FrameBgActive]        = ImVec4(0.230f, 0.250f, 0.310f, 1.00f);
        c[ImGuiCol_TitleBg]              = ImVec4(0.08f, 0.09f, 0.11f, 1.00f);
        c[ImGuiCol_TitleBgActive]        = ImVec4(0.10f, 0.14f, 0.24f, 1.00f);
        c[ImGuiCol_MenuBarBg]            = ImVec4(0.11f, 0.12f, 0.14f, 1.00f);
        c[ImGuiCol_ScrollbarBg]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
        c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.28f, 0.30f, 0.37f, 1.00f);
        c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.36f, 0.39f, 0.47f, 1.00f);
        c[ImGuiCol_ScrollbarGrabActive]  = accent;
        c[ImGuiCol_CheckMark]            = accentHi;
        c[ImGuiCol_SliderGrab]           = accent;
        c[ImGuiCol_SliderGrabActive]     = accentHi;
        c[ImGuiCol_Button]               = ImVec4(0.22f, 0.43f, 0.80f, 1.00f);
        c[ImGuiCol_ButtonHovered]        = ImVec4(0.28f, 0.53f, 0.94f, 1.00f);
        c[ImGuiCol_ButtonActive]         = ImVec4(0.19f, 0.37f, 0.70f, 1.00f);
        c[ImGuiCol_Header]               = ImVec4(0.22f, 0.42f, 0.74f, 0.55f);
        c[ImGuiCol_HeaderHovered]        = ImVec4(0.25f, 0.47f, 0.82f, 0.75f);
        c[ImGuiCol_HeaderActive]         = ImVec4(0.27f, 0.52f, 0.90f, 0.92f);
        c[ImGuiCol_Separator]            = ImVec4(1.00f, 1.00f, 1.00f, 0.090f);
        c[ImGuiCol_SeparatorHovered]     = accent;
        c[ImGuiCol_SeparatorActive]      = accentHi;
        c[ImGuiCol_ResizeGrip]           = ImVec4(1.00f, 1.00f, 1.00f, 0.06f);
        c[ImGuiCol_ResizeGripHovered]    = accent;
        c[ImGuiCol_ResizeGripActive]     = accentHi;
        c[ImGuiCol_TableHeaderBg]        = ImVec4(0.140f, 0.150f, 0.185f, 1.00f);
        c[ImGuiCol_TableBorderStrong]    = ImVec4(1.00f, 1.00f, 1.00f, 0.100f);
        c[ImGuiCol_TableBorderLight]     = ImVec4(1.00f, 1.00f, 1.00f, 0.050f);
        c[ImGuiCol_TableRowBg]           = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
        c[ImGuiCol_TableRowBgAlt]        = ImVec4(1.00f, 1.00f, 1.00f, 0.022f);
        c[ImGuiCol_TextSelectedBg]       = ImVec4(accent.x, accent.y, accent.z, 0.35f);
        c[ImGuiCol_TextLink]             = accentHi;  // the extends / field-type jump links
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
                0xF00D, 0xF00D,  // xmark
                0xF059, 0xF059,  // circle-question
                0xF065, 0xF066,  // expand / compress
                0xF068, 0xF068,  // minus
                0xF1E6, 0xF1E6,  // plug
                0xF7B6, 0xF7B6,  // mug-hot
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
    bool         g_focus_search{ false };
    int          g_kind_filter{ 0 };   // 0=all; else index into k_kind_names
    int          g_search_scope{ 0 };  // 0=Classes, 1=Methods, 2=Fields
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
            << "font_scale="     << ImGui::GetIO().FontGlobalScale << "\n";
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

    // Make a combo's dropdown-arrow box blend into its frame instead of showing
    // as a bright accent button — cleaner, more integrated look.
    inline void push_combo_style()
    {
        const ImVec4* col{ ImGui::GetStyle().Colors };
        ImGui::PushStyleColor(ImGuiCol_Button,        col[ImGuiCol_FrameBg]);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col[ImGuiCol_FrameBgHovered]);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  col[ImGuiCol_FrameBgActive]);
    }
    inline void pop_combo_style() { ImGui::PopStyleColor(3); }

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
        // brand — all toolbar text is frame-aligned so it sits centered against
        // the combo/buttons on this row.
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.72f, 1.0f, 1.0f));
        ImGui::TextUnformatted("vmhook");
        ImGui::PopStyleColor();
        ImGui::SameLine(0.0f, em(0.4f));
        row_label_disabled("viewer");
        row_divider();

        // JVM label + adaptive combo (list auto-refreshes every 2s; no Refresh btn)
        row_label(ICON_FA_MUG_HOT "  JVM");
        ImGui::SameLine();
        const float combo_w{ std::clamp(ImGui::GetContentRegionMax().x - em(34.0f), em(16.0f), em(64.0f)) };
        ImGui::SetNextItemWidth(combo_w);
        std::string preview{ "Select a running JVM..." };
        if (app.selected_jvm >= 0 && app.selected_jvm < (int)app.jvms.size())
        {
            const auto& p{ app.jvms[(std::size_t)app.selected_jvm] };
            preview = std::to_string(p.pid) + " — " + (p.command_line.empty() ? p.image_name : p.command_line);
        }
        push_combo_style();
        if (ImGui::BeginCombo("##jvm", preview.c_str()))
        {
            for (int i = 0; i < (int)app.jvms.size(); ++i)
            {
                const auto& p{ app.jvms[(std::size_t)i] };
                std::string item{ std::to_string(p.pid) + " — " + (p.command_line.empty() ? p.image_path.empty() ? p.image_name : p.image_path : p.command_line) };
                if (ImGui::Selectable(item.c_str(), app.selected_jvm == i)) app.selected_jvm = i;
            }
            ImGui::EndCombo();
        }
        pop_combo_style();
        ImGui::SameLine(0.0f, em(0.5f));
        ImGui::BeginDisabled(app.busy() || app.selected_jvm < 0);
        if (ImGui::Button(ICON_FA_PLUG "  Attach")) { app.attach_selected(g_dll_path); g_selected_class = -1; g_nav_back.clear(); g_nav_fwd.clear(); }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered() && !ImGui::IsItemActive())
            ImGui::SetTooltip("Inject vmhook and enumerate every class, method and field");

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
        // Square buttons (width = row height) sized/spaced in font-relative units.
        const float bw{ ImGui::GetFrameHeight() };
        const float gap{ em(0.35f) };
        ImGui::SameLine(ImGui::GetContentRegionMax().x - (bw * 4.0f + gap * 3.0f));
        // Ghost (transparent) window controls that only tint on hover — subtle
        // chrome rather than loud primary buttons.
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.10f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1.0f, 1.0f, 1.0f, 0.16f));
        if (ImGui::Button(ICON_FA_CIRCLE_Q "##help", ImVec2(bw, bw))) ImGui::OpenPopup("shortcuts");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Keyboard shortcuts (F1)");
        if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) ImGui::OpenPopup("shortcuts");
        ImGui::SameLine(0.0f, gap);
        if (ImGui::Button(ICON_FA_MINUS "##min", ImVec2(bw, bw)) && g_hwnd) ShowWindow(g_hwnd, SW_MINIMIZE);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Minimize");
        ImGui::SameLine(0.0f, gap);
        const bool maximized{ g_hwnd && IsZoomed(g_hwnd) };
        char maxlbl[24];
        std::snprintf(maxlbl, sizeof(maxlbl), "%s##maxrestore", maximized ? ICON_FA_COMPRESS : ICON_FA_EXPAND);
        if (ImGui::Button(maxlbl, ImVec2(bw, bw)) && g_hwnd)
            ShowWindow(g_hwnd, maximized ? SW_RESTORE : SW_MAXIMIZE);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(maximized ? "Restore" : "Maximize");
        ImGui::PopStyleColor(3);
        ImGui::SameLine(0.0f, gap);
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.86f, 0.26f, 0.26f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.70f, 0.17f, 0.17f, 1.0f));
        if (ImGui::Button(ICON_FA_XMARK "##close", ImVec2(bw, bw)) && g_hwnd) PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
        ImGui::PopStyleColor(3);
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
            ImGui::TextDisabled("Tip: click a field's type or 'extends' to jump to that class.");
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
        ImGui::SetNextItemWidth(has_query ? -em(1.9f) : -1.0f);
        const char* hint{ g_search_scope == 1 ? "Search methods across all classes" : g_search_scope == 2 ? "Search fields across all classes" : "Search classes  (Ctrl+F)" };
        ImGui::InputTextWithHint("##search", hint, g_search, sizeof(g_search));
        if (has_query)
        {
            ImGui::SameLine(0.0f, em(0.3f));
            if (ImGui::Button(ICON_FA_XMARK "##clear", ImVec2(em(1.6f), 0.0f))) g_search[0] = '\0';
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Clear (Esc)");
        }

        std::lock_guard<std::mutex> lock{ app.data_mutex };

        // ── scope selector — search Classes / Methods / Fields ──────────────
        ImGui::AlignTextToFramePadding();
        ImGui::SetNextItemWidth(em(7.5f));
        push_combo_style();
        ImGui::Combo("##scope", &g_search_scope, "Classes\0Methods\0Fields\0");
        pop_combo_style();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Search across every class's methods or fields");
        ImGui::SameLine(0.0f, em(0.7f));

        if (g_search_scope != 0)
        {
            draw_member_results(app, g_search_scope == 2);
            return;
        }

        // Match either slash- or dot-qualified queries ("java.lang" == "java/lang").
        std::string needle{ g_search };
        for (char& ch : needle) if (ch == '.') ch = '/';

        static const char* k_kind_names[]{ "All kinds", "class", "interface", "enum", "abstract", "annotation", "record" };
        const char* want_kind{ g_kind_filter > 0 ? k_kind_names[g_kind_filter] : nullptr };

        g_filtered.clear();
        g_filtered.reserve(app.classes.size());
        for (int i = 0; i < (int)app.classes.size(); ++i)
        {
            const viewer::ClassInfo& ci{ app.classes[(std::size_t)i] };
            if (!icontains(ci.internal_name, needle)) continue;
            if (want_kind && std::strcmp(class_kind(ci).label, want_kind) != 0) continue;
            g_filtered.push_back(i);
        }

        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%d / %zu classes", (int)g_filtered.size(), app.classes.size());
        const float kind_w{ em(8.5f) };
        ImGui::SameLine((std::max)(ImGui::GetContentRegionMax().x - kind_w, ImGui::GetCursorPosX() + em(0.5f)));
        ImGui::SetNextItemWidth(kind_w);
        push_combo_style();
        ImGui::Combo("##kind", &g_kind_filter, k_kind_names, IM_ARRAYSIZE(k_kind_names));
        pop_combo_style();

        if (ImGui::BeginTable("classes", 3,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable | ImGuiTableFlags_SortTristate))
        {
            ImGui::TableSetupColumn("Package", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Class",   ImGuiTableColumnFlags_WidthStretch, 1.15f);
            ImGui::TableSetupColumn("m/f",     ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort, em(4.2f));
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();

            if (ImGuiTableSortSpecs* sort = ImGui::TableGetSortSpecs(); sort && sort->SpecsCount > 0)
            {
                const bool by_pkg{ sort->Specs[0].ColumnIndex == 0 };
                const bool asc{ sort->Specs[0].SortDirection == ImGuiSortDirection_Ascending };
                std::sort(g_filtered.begin(), g_filtered.end(), [&](int a, int b)
                {
                    const auto& ca{ app.classes[(std::size_t)a] };
                    const auto& cb{ app.classes[(std::size_t)b] };
                    const std::string& ka{ by_pkg ? ca.package : ca.simple_name };
                    const std::string& kb{ by_pkg ? cb.package : cb.simple_name };
                    int cmp{ ka.compare(kb) };
                    if (cmp == 0) cmp = ca.simple_name.compare(cb.simple_name);
                    return asc ? cmp < 0 : cmp > 0;
                });
            }

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
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::PushID(idx);
                    if (ImGui::Selectable(c.package.empty() ? "(default)" : c.package.c_str(),
                            g_selected_class == idx, ImGuiSelectableFlags_SpanAllColumns))
                    {
                        navigate_to(idx, false);  // clicked row already visible
                    }
                    copy_menu("cls", c.internal_name);
                    ImGui::TableSetColumnIndex(1);
                    ImGui::PushStyleColor(ImGuiCol_Text, class_kind(c).color);
                    ImGui::TextUnformatted(c.simple_name.c_str());
                    ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered() && (c.access || !c.super_name.empty()))
                        ImGui::SetTooltip("%s", class_kind(c).label);
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextDisabled("%zu/%zu", c.methods.size(), c.fields.size());
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
                        const bool clicked{ ImGui::Selectable(f.name.c_str(), false, ImGuiSelectableFlags_SpanAllColumns) };
                        ImGui::PopStyleColor();
                        if (clicked) { navigate_to(ci); std::snprintf(g_field_filter, sizeof(g_field_filter), "%s", g_search); }
                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextUnformatted((g_pretty ? viewer::pretty_field(f.descriptor, g_full_names) : f.descriptor).c_str());
                    }
                    else
                    {
                        const viewer::MethodInfo& m{ c.methods[(std::size_t)mi] };
                        ImGui::PushStyleColor(ImGuiCol_Text, vis_color(m.access));
                        const bool clicked{ ImGui::Selectable(m.name.c_str(), false, ImGuiSelectableFlags_SpanAllColumns) };
                        ImGui::PopStyleColor();
                        if (clicked) { navigate_to(ci); std::snprintf(g_method_filter, sizeof(g_method_filter), "%s", g_search); }
                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextUnformatted((g_pretty ? viewer::pretty_method(m.descriptor, g_full_names) : m.descriptor).c_str());
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
        // Subtle frame-coloured buttons so they don't compete with Attach.
        push_combo_style();
        ImGui::BeginDisabled(g_nav_back.empty());
        if (ImGui::ArrowButton("##nav_back", ImGuiDir_Left)) nav_back();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered() && !g_nav_back.empty()) ImGui::SetTooltip("Back (Alt+Left)");
        ImGui::SameLine(0.0f, em(0.25f));
        ImGui::BeginDisabled(g_nav_fwd.empty());
        if (ImGui::ArrowButton("##nav_fwd", ImGuiDir_Right)) nav_forward();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered() && !g_nav_fwd.empty()) ImGui::SetTooltip("Forward (Alt+Right)");
        pop_combo_style();
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
        if (ImGui::SmallButton("Copy name"))
        {
            ImGui::SetClipboardText(c.internal_name.c_str());
            app.status_message = "Copied class name to clipboard.";
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy all"))
        {
            std::string all{ class_decl(c) + " " + dotted + " {\n" };
            for (const auto& f : c.fields)
                all += "  " + viewer::access_modifiers(f.access, false) + " " + viewer::pretty_field(f.descriptor) + " " + f.name + ";\n";
            all += "\n";
            for (const auto& m : c.methods)
                all += "  " + viewer::access_modifiers(m.access, true) + " " + m.name + viewer::pretty_method(m.descriptor) + "\n";
            all += "}\n";
            ImGui::SetClipboardText(all.c_str());
            app.status_message = "Copied class listing to clipboard.";
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Export .txt"))
            app.status_message = export_class(c) ? "Exported to vmhook_export.txt (next to the viewer)."
                                                 : "Export failed — is the viewer's folder writable?";
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
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##mf", "Filter methods", g_method_filter, sizeof(g_method_filter));
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
                        const std::string sig{ g_pretty ? viewer::pretty_method(m.descriptor, g_full_names) : m.descriptor };
                        ImGui::TextUnformatted(sig.c_str());
                        if (g_pretty && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", m.descriptor.c_str());
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
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##ff", "Filter fields", g_field_filter, sizeof(g_field_filter));
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

    void render_ui(viewer::App& app)
    {
        // Ctrl+F focuses the class search; Esc clears all filters.
        if (ImGui::IsKeyDown(ImGuiKey_LeftCtrl) && ImGui::IsKeyPressed(ImGuiKey_F, false)) g_focus_search = true;
        if (ImGui::IsKeyPressed(ImGuiKey_Escape) && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId))
        {
            g_search[0] = 0; g_method_filter[0] = 0; g_field_filter[0] = 0;
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

        const ImGuiViewport* vp{ ImGui::GetMainViewport() };
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("root", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);

        draw_toolbar(app);
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
            std::size_t nclasses{ 0 }, nmethods{ 0 }, nfields{ 0 };
            {
                std::lock_guard<std::mutex> lock{ app.data_mutex };
                nclasses = app.classes.size();
                for (const auto& c : app.classes) { nmethods += c.methods.size(); nfields += c.fields.size(); }
            }
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%zu classes  \xC2\xB7  %zu methods  \xC2\xB7  %zu fields", nclasses, nmethods, nfields);
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

        ImGui::End();
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
    g_left_width *= g_dpi_scale;  // scale the default split (load_settings may override)

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
