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
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "app.hpp"
#include "descriptor.hpp"
#include "icons.hpp"
#include "theme.hpp"
#include "widgets.hpp"
#include "wrapper_gen.hpp"
#include "script_host.hpp"
#include "complete.hpp"
#include "TextEditor.h"

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
    ImFont*                 g_font_mono{ nullptr };  // monospace font for the code editor

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
    // Theme: Tokyo Night (Night) — a deep, desaturated navy-black dark palette
    // (Linear / Vercel / VS Code family) with a single blue accent for every
    // interactive state.  Flat frames (no frame borders); panels are separated by
    // navy-tinted background steps + a hairline Border, not by luminance jumps or
    // shadows.  Semantic hues (green/amber/red/cyan/...) live in theme.hpp and are
    // reserved for MEANING.  See [[theme.hpp]].
    void apply_modern_style()
    {
        ImGuiStyle& s{ ImGui::GetStyle() };
        // Concentric, restrained rounding (inner <= outer - padding).
        s.WindowRounding = 8.0f; s.ChildRounding = 6.0f; s.FrameRounding = 5.0f;
        s.PopupRounding = 6.0f; s.GrabRounding = 4.0f; s.ScrollbarRounding = 5.5f; s.TabRounding = 5.0f;
        // Spacing / padding — breathable, uniform.
        s.WindowPadding = ImVec2(14, 12); s.FramePadding = ImVec2(12, 7);
        s.ItemSpacing = ImVec2(10, 8); s.ItemInnerSpacing = ImVec2(8, 6); s.CellPadding = ImVec2(10, 7);
        s.IndentSpacing = 22.0f; s.ScrollbarSize = 11.0f; s.GrabMinSize = 12.0f;
        // Crisp 1px frame borders; hairline chrome on children/popups; the
        // borderless host keeps WindowBorderSize 0 (no seam against the custom titlebar).
        s.WindowBorderSize = 0.0f; s.FrameBorderSize = 1.0f; s.ChildBorderSize = 1.0f;
        s.PopupBorderSize = 1.0f; s.TabBorderSize = 0.0f;
        s.SeparatorTextBorderSize = 1.0f; s.SeparatorTextPadding = ImVec2(20, 8);
        s.WindowTitleAlign = ImVec2(0.0f, 0.5f);
        s.ButtonTextAlign = ImVec2(0.5f, 0.5f);
        s.SelectableTextAlign = ImVec2(0.0f, 0.5f);
        s.WindowMenuButtonPosition = ImGuiDir_None;  // no collapse arrow — cleaner titlebars
        s.DisabledAlpha = 0.45f;
        // Snappy-but-flicker-free tooltips (uses ImGui's stationary + short-delay).
        s.HoverStationaryDelay = 0.12f;
        s.AntiAliasedLines = true; s.AntiAliasedLinesUseTex = true; s.AntiAliasedFill = true;
        s.CurveTessellationTol = 1.10f; s.CircleTessellationMaxError = 0.20f;

        ImVec4* c{ s.Colors };
        const ImVec4 accent   { theme::accent() };     // #7aa2f7
        const ImVec4 accentHi { theme::accent_hi() };  // #98b8ff
        c[ImGuiCol_Text]                 = theme::text();                          // #c0caf5
        c[ImGuiCol_TextDisabled]         = theme::muted_dim();                     // #565f89
        c[ImGuiCol_WindowBg]             = ImVec4(0.102f, 0.106f, 0.149f, 1.00f);  // #1a1b26
        c[ImGuiCol_ChildBg]              = ImVec4(0.118f, 0.122f, 0.169f, 1.00f);  // #1e1f2b
        c[ImGuiCol_PopupBg]              = ImVec4(0.086f, 0.086f, 0.118f, 0.98f);  // #16161e
        c[ImGuiCol_Border]               = ImVec4(0.180f, 0.200f, 0.314f, 1.00f);  // #2e3350 hairline
        c[ImGuiCol_BorderShadow]         = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
        c[ImGuiCol_FrameBg]              = ImVec4(0.137f, 0.149f, 0.227f, 1.00f);  // #23263a
        c[ImGuiCol_FrameBgHovered]       = ImVec4(0.173f, 0.188f, 0.278f, 1.00f);  // #2c3047
        c[ImGuiCol_FrameBgActive]        = ImVec4(0.208f, 0.231f, 0.341f, 1.00f);  // #353b57
        c[ImGuiCol_TitleBg]              = ImVec4(0.086f, 0.086f, 0.118f, 1.00f);  // #16161e
        c[ImGuiCol_TitleBgActive]        = ImVec4(0.102f, 0.106f, 0.149f, 1.00f);  // #1a1b26
        c[ImGuiCol_TitleBgCollapsed]     = ImVec4(0.086f, 0.086f, 0.118f, 1.00f);
        c[ImGuiCol_MenuBarBg]            = ImVec4(0.118f, 0.122f, 0.169f, 1.00f);  // #1e1f2b
        c[ImGuiCol_ScrollbarBg]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
        c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.180f, 0.200f, 0.314f, 1.00f);  // #2e3350
        c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.239f, 0.267f, 0.408f, 1.00f);  // #3d4468
        c[ImGuiCol_ScrollbarGrabActive]  = accent;
        c[ImGuiCol_CheckMark]            = accentHi;
        c[ImGuiCol_SliderGrab]           = accent;
        c[ImGuiCol_SliderGrabActive]     = accentHi;
        // Neutral secondary button (Primary/Ghost/Danger are set per-call in ui::Button).
        c[ImGuiCol_Button]               = ImVec4(0.149f, 0.165f, 0.251f, 1.00f);  // #262a40
        c[ImGuiCol_ButtonHovered]        = ImVec4(0.200f, 0.220f, 0.333f, 1.00f);  // #333855
        c[ImGuiCol_ButtonActive]         = ImVec4(0.118f, 0.133f, 0.200f, 1.00f);  // #1e2233 (pressed = darker)
        c[ImGuiCol_Header]               = ImVec4(accent.x, accent.y, accent.z, 0.30f);
        c[ImGuiCol_HeaderHovered]        = ImVec4(accent.x, accent.y, accent.z, 0.45f);
        c[ImGuiCol_HeaderActive]         = ImVec4(accent.x, accent.y, accent.z, 0.60f);
        c[ImGuiCol_Separator]            = ImVec4(0.180f, 0.200f, 0.314f, 1.00f);  // = Border
        c[ImGuiCol_SeparatorHovered]     = accent;
        c[ImGuiCol_SeparatorActive]      = accentHi;
        c[ImGuiCol_ResizeGrip]           = ImVec4(1.00f, 1.00f, 1.00f, 0.05f);
        c[ImGuiCol_ResizeGripHovered]    = ImVec4(accent.x, accent.y, accent.z, 0.60f);
        c[ImGuiCol_ResizeGripActive]     = accentHi;
        c[ImGuiCol_TableHeaderBg]        = ImVec4(0.137f, 0.149f, 0.227f, 1.00f);  // = FrameBg
        c[ImGuiCol_TableBorderStrong]    = ImVec4(1.00f, 1.00f, 1.00f, 0.060f);
        c[ImGuiCol_TableBorderLight]     = ImVec4(1.00f, 1.00f, 1.00f, 0.028f);
        c[ImGuiCol_TableRowBg]           = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
        c[ImGuiCol_TableRowBgAlt]        = ImVec4(1.00f, 1.00f, 1.00f, 0.020f);
        c[ImGuiCol_TextSelectedBg]       = ImVec4(accent.x, accent.y, accent.z, 0.35f);
        c[ImGuiCol_TextLink]             = accentHi;  // the extends / field-type jump links
        c[ImGuiCol_DragDropTarget]       = accentHi;
        c[ImGuiCol_NavCursor]            = accent;
        // Tabs (Scripts window) — dim when unselected, accent-tinted selected.
        c[ImGuiCol_Tab]                  = ImVec4(0.118f, 0.122f, 0.169f, 1.00f);
        c[ImGuiCol_TabHovered]           = ImVec4(accent.x, accent.y, accent.z, 0.40f);
        c[ImGuiCol_TabSelected]          = ImVec4(0.137f, 0.149f, 0.227f, 1.00f);
        c[ImGuiCol_TabSelectedOverline]  = accent;
        c[ImGuiCol_TabDimmed]            = ImVec4(0.086f, 0.086f, 0.118f, 1.00f);
        c[ImGuiCol_TabDimmedSelected]    = ImVec4(0.118f, 0.122f, 0.169f, 1.00f);
        c[ImGuiCol_NavWindowingHighlight]= ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
        c[ImGuiCol_NavWindowingDimBg]    = ImVec4(0.086f, 0.086f, 0.118f, 0.60f);
        c[ImGuiCol_ModalWindowDimBg]     = ImVec4(0.055f, 0.055f, 0.078f, 0.55f);
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
                0xF04B, 0xF04B,  // play (call method)
                0xF08D, 0xF08D,  // thumbtack (grab object)
                0xF09C, 0xF09C,  // lock-open (unfrozen)
                0xF023, 0xF023,  // lock (frozen)
                0xF1E6, 0xF1E6,  // plug
                0xF1F8, 0xF1F8,  // trash (remove saved object)
                0xF304, 0xF304,  // pen (edit value)
                0xF121, 0xF121,  // code (generate wrapper)
                0xF70E, 0xF70E,  // scroll (scripts)
                0xF0AD, 0xF0AD,  // wrench (build)
                0 };
            ImFontConfig cfg{};
            cfg.MergeMode        = true;
            cfg.PixelSnapH       = true;
            cfg.GlyphMinAdvanceX = 16.0f * dpi_scale;  // keep icons a consistent width
            cfg.GlyphOffset      = ImVec2(0.0f, 2.0f * dpi_scale);  // sit on the text baseline
            io.Fonts->AddFontFromFileTTF(icon_ttf.c_str(), 15.0f * dpi_scale, &cfg, icon_ranges);
        }

        // Monospace font for the Scripts code editor (Consolas ships with Windows).
        if (std::ifstream{ "C:\\Windows\\Fonts\\consola.ttf", std::ios::binary }.good())
        {
            g_font_mono = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\consola.ttf", base_px);
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
    // The selected class's internal name, tracked alongside the index so the
    // selection can self-heal when a full Rescan replaces the class vector and
    // shifts indices (HotSpot prepends newly-loaded klasses) — without it, the
    // details pane would silently show a different class.
    std::string  g_selected_name;
    float        g_left_width{ 500.0f };  // default; DPI-scaled + persisted
    bool         g_pretty{ true };
    bool         g_full_names{ false };
    bool         g_show_inherited{ false };  // details: include super-chain members
    bool         g_show_instances{ false };  // live-instances window open
    bool         g_instances_live{ true };   // auto-refresh the instances window
    bool         g_instances_refresh_now{ false };  // force an immediate re-scan
    std::string  g_instances_class;          // internal name shown in the window
    char         g_instance_filter[128]{};   // substring filter over the instance rows
    std::string  g_detail_addr;              // address of the instance the popup shows
    bool         g_copy_instance_table{ false };   // request a TSV copy of the instance table
    int          g_instance_cap{ 1000 };     // persisted mirror of App::inst_cap
    bool         g_inst_show_inherited{ true };  // show inherited columns in the instance table
    bool         g_focus_search{ false };
    int          g_kind_filter{ 0 };   // 0=all; else index into k_kind_names
    int          g_search_scope{ 0 };  // 0=Classes, 1=Methods, 2=Fields
    // class list sort: key (0=Natural 1=Name 2=Package 3=Age 4=Kind 5=Members) + direction.
    int          g_class_sort_key{ 0 };
    bool         g_class_sort_desc{ false };
    bool         g_auto_rescan{ false };  // periodically re-scan loaded classes
    bool         g_new_only{ false };     // class list: show only runtime-loaded classes
    bool         g_show_removed{ false }; // request the unloaded-classes popup
    bool         g_show_age{ false };     // class list: show "age" + sort newest-first
    bool         g_inst_show_age{ false };// instance list: show "age" + sort newest-first
    int          g_inst_sort{ 1 };     // instance list: 0=natural, 1=addr↑, 2=addr↓
    float        g_inst_left_width{ 300.0f };  // Live-instances master/detail split
    std::wstring g_dll_path{};

    // ── object clipboard + field editing + method-call UI state ──
    std::vector<viewer::SavedObject> g_clipboard;          // stashed objects (grab / drag-drop)
    bool          g_show_clipboard{ false };               // the clipboard strip is visible
    // Frozen fields: key "scope|cls|addr|field" -> its details (so we can list +
    // manage every freeze from one place, not just per-instance).
    struct FrozenField { char scope{ 'I' }; std::string cls, addr, field, value; };
    std::map<std::string, FrozenField> g_frozen;
    bool         g_show_frozen{ false };                   // open the frozen-fields popup
    // Array inspector (click an array-typed value to open it).
    bool         g_show_array{ false };
    std::string  g_array_addr, g_array_elemdesc, g_array_label;
    bool         g_array_refresh{ false };
    char          g_edit_buf[512]{};                       // shared edit-popup input buffer
    // Static-fields window (per class, async STAT channel).
    bool          g_show_statics{ false };
    std::string   g_statics_class;                         // internal name whose statics show
    bool          g_statics_live{ true };
    bool          g_statics_refresh_now{ false };
    char          g_statics_filter[128]{};
    float         g_statics_left_width{ 300.0f };
    // Method-call panel state — one per receiver context (instance detail vs the
    // statics window) so the two panels never clobber each other's selection.
    struct CallState
    {
        std::string      ctx;                // "cls@addr" the buffers belong to
        int              method_idx{ -1 };   // selected row in the method list
        char             filter[96]{};       // method-picker filter
        char             args[8][256]{};     // per-argument token buffers
        bool             pending{ false };   // an op it dispatched is in flight
        bool             has_result{ false };
        viewer::OpResult result;             // last call result (shown in the panel)
    };
    CallState     g_call_inst;                             // instance-detail call panel
    CallState     g_call_stat;                             // statics-window call panel
    // Transient toast for set / freeze feedback + op-result de-dup.
    std::string   g_op_toast;
    double        g_op_toast_until{ 0.0 };
    std::uint64_t g_op_seen_seq{ 0 };
    std::vector<int> g_filtered;  // rebuilt each frame from the search box
    // Global member-search results: (class index, member index) pairs.
    std::vector<std::pair<int,int>> g_member_results;

    // ── Generate Wrapper window state ─────────────────────────────────────────
    bool  g_show_wrapper{ false };
    char  g_w_ns[64]{ "jvm" };
    char  g_w_getter[24]{ "get_" };
    char  g_w_setter[24]{ "set_" };
    char  g_w_include[512]{};
    char  g_w_exclude[512]{};
    char  g_w_outdir[512]{};
    int   g_w_layout{ 0 };        // 0 = nested namespaces, 1 = flat
    int   g_w_type_case{ 3 };     // 0 orig / 1 snake / 2 camel / 3 pascal
    int   g_w_member_case{ 1 };   // default snake
    bool  g_w_setters{ true };
    bool  g_w_methods{ true };
    bool  g_w_fields{ true };
    bool  g_w_jdk{ false };       // default: skip the (huge) JDK
    bool  g_w_public_only{ false };
    std::atomic<int> g_w_state{ 0 };   // 0 idle / 1 running / 2 done / 3 error
    std::string      g_w_msg;          // result summary (read after join)
    std::string      g_w_path;         // written header path
    std::string      g_w_notes;        // generator notes
    std::thread      g_w_thread;

    // ── Scripts window state ──────────────────────────────────────────────────
    bool  g_show_scripts{ false };
    TextEditor g_editor;                   // syntax-highlighted code editor (BalazsJako)
    bool  g_editor_seeded{ false };        // language def + starter applied once
    char  g_script_hdr[512]{};             // generated-wrapper header path (for #include)
    // Completion (built from the generated wrapper header + C++/vmhook API).
    complete::Index  g_complete_index;
    bool             g_cmpl_open{ false };
    std::vector<int> g_cmpl_items;
    int              g_cmpl_sel{ 0 };
    std::string      g_cmpl_token;
    bool             g_cmpl_reindex{ false };  // rebuild the index next frame
    std::atomic<int>  g_build_state{ 0 };  // 0 idle / 1 building / 2 ok / 3 fail
    std::string       g_build_log;         // compiler output (read after join)
    std::string       g_build_dll;         // built DLL path
    std::thread       g_build_thread;
    bool  g_build_inject{ false };         // auto-inject once a build succeeds
    double g_script_log_poll{ 0.0 };
    std::string g_script_log_text;         // tail of %TEMP%\vmhook_script.log

    // Back/forward navigation history (paired with the clickable `extends` jump
    // and class-list clicks) so browsing the class graph feels like a browser.
    // Each entry carries the class's internal name too, so history survives a
    // Rescan that reorders the class vector (the index is re-resolved by name).
    std::vector<std::pair<int, std::string>> g_nav_back;
    std::vector<std::pair<int, std::string>> g_nav_fwd;
    bool             g_scroll_to_selected{ false };  // sync the list to a jump

    // Select a class (by index + its internal name), recording the previous
    // selection for Back.  Clears the per-pane member filters (a fresh class
    // shouldn't inherit stale filters).  scroll_into_view syncs the class list to
    // the target — set for link/history jumps, cleared for list clicks (the
    // clicked row is already visible).
    void navigate_to(int idx, std::string name, bool scroll_into_view = true)
    {
        if (idx == g_selected_class) return;
        if (g_selected_class >= 0) g_nav_back.push_back({ g_selected_class, g_selected_name });
        g_nav_fwd.clear();
        g_selected_class = idx;
        g_selected_name = std::move(name);
        g_method_filter[0] = 0; g_field_filter[0] = 0;
        if (scroll_into_view) g_scroll_to_selected = true;
    }

    void nav_back()
    {
        if (g_nav_back.empty()) return;
        if (g_selected_class >= 0) g_nav_fwd.push_back({ g_selected_class, g_selected_name });
        g_selected_class = g_nav_back.back().first;
        g_selected_name  = std::move(g_nav_back.back().second);
        g_nav_back.pop_back();
        g_method_filter[0] = 0; g_field_filter[0] = 0;
        g_scroll_to_selected = true;
    }

    void nav_forward()
    {
        if (g_nav_fwd.empty()) return;
        if (g_selected_class >= 0) g_nav_back.push_back({ g_selected_class, g_selected_name });
        g_selected_class = g_nav_fwd.back().first;
        g_selected_name  = std::move(g_nav_fwd.back().second);
        g_nav_fwd.pop_back();
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
            << "inst_inherited=" << (g_inst_show_inherited ? 1 : 0) << "\n"
            << "sort_key="       << g_class_sort_key          << "\n"
            << "sort_desc="      << (g_class_sort_desc ? 1 : 0) << "\n"
            << "show_age="       << (g_show_age ? 1 : 0)      << "\n"
            << "clipboard="      << (g_show_clipboard ? 1 : 0)<< "\n"
            << "statics_live="   << (g_statics_live ? 1 : 0)  << "\n";
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
                else if (key == "sort_key")   { int k{ std::stoi(val) }; if (k >= 0 && k <= 5) g_class_sort_key = k; }
                else if (key == "sort_desc")  g_class_sort_desc = (std::stoi(val) != 0);
                else if (key == "show_age")   g_show_age       = (std::stoi(val) != 0);
                else if (key == "clipboard")  g_show_clipboard = (std::stoi(val) != 0);
                else if (key == "statics_live") g_statics_live = (std::stoi(val) != 0);
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
        case viewer::Status::Idle:      col = theme::status_idle();      text = "Idle";      break;
        case viewer::Status::Injecting: col = theme::status_injecting(); text = "Injecting"; break;
        case viewer::Status::Receiving: col = theme::status_receiving(); text = "Receiving"; break;
        case viewer::Status::Done:      col = theme::status_done();      text = "Done";      break;
        default:                        col = theme::status_error();     text = "Error";     break;
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

    ImVec4 vis_color(std::uint16_t f) { return theme::visibility(f); }

    // Primary class kind from the class-file access flags (+ super for records),
    // with a distinct badge colour.  ANNOTATION implies INTERFACE, and ENUM
    // implies FINAL, so the checks are ordered most-specific first.  Returns a
    // stable label even when flags are 0 (older payloads) — falls back to super.
    struct ClassKind { const char* label; ImVec4 color; };
    ClassKind class_kind(const viewer::ClassInfo& c)
    {
        const std::uint16_t f{ c.access };
        if (f & 0x2000u) return { "annotation", theme::kind_annotation() };
        if (f & 0x0200u) return { "interface",  theme::kind_interface() };
        if (f & 0x4000u) return { "enum",       theme::kind_enum() };
        if (c.super_name == "java/lang/Record") return { "record", theme::kind_record() };
        if (f & 0x0400u) return { "abstract",   theme::kind_abstract() };
        // Flags unavailable but super says enum → still label it.
        if (f == 0u && c.super_name == "java/lang/Enum") return { "enum", theme::kind_enum() };
        return { "class", theme::kind_class() };
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
                if (ImGui::TextLink(base.c_str())) navigate_to(it->second, it->first);
                ImGui::SetItemTooltip("%s  (click to open)", internal.c_str());
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

    // Compact "how long ago" label from a seconds delta (e.g. 12s / 3m / 1h).
    inline std::string fmt_age(double secs)
    {
        if (secs < 0.0) return "?";
        const int s{ static_cast<int>(secs) };
        if (s < 60)   return std::to_string(s) + "s";
        if (s < 3600) return std::to_string(s / 60) + "m";
        return std::to_string(s / 3600) + "h";
    }

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
        if (ui::Button(ICON_FA_PLUG "  Attach", ImVec2(0, 0), ui::BtnPrimary)) { app.attach_selected(g_dll_path); g_selected_class = -1; g_selected_name.clear(); g_nav_back.clear(); g_nav_fwd.clear(); }
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
        ImGui::SetItemTooltip("Live class-load tracking: arms the on_class_loaded hook and adds each new class\nthe moment ClassLoader.defineClass defines it — no full re-scan. (Rescan button = full list + diff.)");

        // Object clipboard toggle (shows the count so it reads at a glance).
        ImGui::SameLine(0.0f, em(0.7f));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ImGui::GetFrameHeight() * 0.13f);
        ui::Toggle("Clipboard", &g_show_clipboard);
        ImGui::SetItemTooltip("Show the saved-objects strip — stash objects, then place them into fields / method args");
        if (!g_clipboard.empty())
        {
            ImGui::SameLine(0.0f, em(0.3f));
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("(%d)", (int)g_clipboard.size());
        }

        // Generate Wrapper + Scripts — enabled once a class surface is loaded.
        ImGui::SameLine(0.0f, em(0.7f));
        ImGui::BeginDisabled(!app.has_baseline.load());
        if (ui::Button(ICON_FA_CODE "  Wrapper")) g_show_wrapper = true;
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Generate a C++ wrapper of the JVM's classes (customizable naming / namespaces)");
        ImGui::SameLine(0.0f, em(0.4f));
        ImGui::BeginDisabled(!app.has_baseline.load());
        if (ui::Button(ICON_FA_SCROLL "  Scripts")) g_show_scripts = true;
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Write, compile and inject a C++ script built on the generated wrapper");

        row_divider();
        const viewer::Status st{ app.status.load() };
        status_pill(st);
        if (st == viewer::Status::Injecting || st == viewer::Status::Receiving)
        {
            ImGui::SameLine(0.0f, em(0.6f));
            const float r{ ImGui::GetFrameHeight() * 0.32f };
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (ImGui::GetFrameHeight() * 0.5f - r) - ImGui::GetStyle().FramePadding.y);
            ui::Spinner("##spin", r, (std::max)(r * 0.35f, em(0.12f)), ImGui::GetColorU32(theme::accent()));
            ImGui::SameLine(0.0f, em(0.45f));
            ImGui::AlignTextToFramePadding();
            ImGui::PushStyleColor(ImGuiCol_Text, theme::link());
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
        ImGui::SetItemTooltip("Keyboard shortcuts (F1)");
        if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) ImGui::OpenPopup("shortcuts");
        ImGui::SameLine(0.0f, gap);
        if (ui::IconButton(ICON_FA_MINUS, "min", bw, ui::BtnGhost) && g_hwnd) ShowWindow(g_hwnd, SW_MINIMIZE);
        ImGui::SetItemTooltip("Minimize");
        ImGui::SameLine(0.0f, gap);
        const bool maximized{ g_hwnd && IsZoomed(g_hwnd) };
        if (ui::IconButton(maximized ? ICON_FA_COMPRESS : ICON_FA_EXPAND, "maxrestore", bw, ui::BtnGhost) && g_hwnd)
            ShowWindow(g_hwnd, maximized ? SW_RESTORE : SW_MAXIMIZE);
        ImGui::SetItemTooltip(maximized ? "Restore" : "Maximize");
        ImGui::SameLine(0.0f, gap);
        if (ui::IconButton(ICON_FA_XMARK, "close", bw, ui::BtnDanger) && g_hwnd) PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
        ImGui::SetItemTooltip("Close");
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
                    ImGui::PushStyleColor(ImGuiCol_Text, theme::link());
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
            ImGui::SetItemTooltip("Clear (Esc)");
        }

        std::lock_guard<std::mutex> lock{ app.data_mutex };

        // ── scope selector (+ kind filter for Classes) on one compact row ───
        static const char* k_kind_names[]{ "All kinds", "class", "interface", "enum", "abstract", "annotation", "record" };
        ui::Combo("##scope", &g_search_scope, "Classes\0Methods\0Fields\0", em(7.5f));
        ImGui::SetItemTooltip("Search across every class's methods or fields");
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
            const ImVec4 green{ theme::success() };
            ImGui::PushStyleColor(ImGuiCol_Text, green);
            ImGui::PushStyleColor(ImGuiCol_TextLink, green);
            char lbl[40];
            std::snprintf(lbl, sizeof(lbl), "+%d new%s", n_new, g_new_only ? "  (shown)" : "");
            if (ImGui::TextLink(lbl)) g_new_only = !g_new_only;
            ImGui::PopStyleColor(2);
            ImGui::SetItemTooltip("Classes loaded since the previous scan — click to filter to them");
        }
        // Sort key + Age display, right-aligned on the count row.  The combo picks
        // WHAT to sort by; clicking the "Class" header below reverses the direction.
        {
            const float sort_w{ em(8.4f) }, age_w{ em(3.4f) };
            ImGui::SameLine((std::max)(ImGui::GetContentRegionMax().x - sort_w - age_w - em(0.6f), ImGui::GetCursorPosX() + em(0.5f)));
            const int prev_key{ g_class_sort_key };
            ui::Combo("##classsort", &g_class_sort_key,
                      "Sort: Natural\0Sort: Name\0Sort: Package\0Sort: Age\0Sort: Kind\0Sort: Members\0", sort_w);
            ImGui::SetItemTooltip("Sort the class list. Natural = load order.\nClick the \"Class\" header to reverse the direction.");
            if (g_class_sort_key != prev_key)  // sensible default direction per key
                g_class_sort_desc = (g_class_sort_key == 3 || g_class_sort_key == 5);  // Age / Members -> most first
            ImGui::SameLine(0.0f, em(0.5f));
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ImGui::GetFrameHeight() * 0.13f);
            ui::Toggle("Age", &g_show_age);
            ImGui::SetItemTooltip("Show how long ago each class was first observed (\"· 12s\").\nBaseline classes share the attach time (loaded before the viewer attached).");
        }
        // Show the age suffix whenever it's toggled on OR we're sorting by age.
        const bool show_age_col{ g_show_age || g_class_sort_key == 3 };

        // Custom "Class" header — a rounded frame matching the combo/input cells
        // (ImGui table headers can't be rounded).  Shows a direction triangle for
        // the active sort; clicking it reverses the direction (or starts a Name
        // sort when the list is in Natural order).
        static const char* const k_sort_names[]{ "Natural", "Name", "Package", "Age", "Kind", "Members" };
        {
            const ImGuiStyle& st{ ImGui::GetStyle() };
            // Match the combo/input text inset (FramePadding.x) so "Class" lines
            // up exactly under "Classes".
            const float   hpad{ st.FramePadding.x };
            const ImVec2  p0{ ImGui::GetCursorScreenPos() };
            const float   bar_w{ ImGui::GetContentRegionAvail().x };
            const float   bar_h{ ImGui::GetFrameHeight() };
            const ImVec2  p1{ p0.x + bar_w, p0.y + bar_h };
            if (ImGui::InvisibleButton("##classhdr", ImVec2(bar_w, bar_h)))
            {
                if (g_class_sort_key == 0) g_class_sort_key = 1;   // Natural -> sort by Name on first click
                else                       g_class_sort_desc = !g_class_sort_desc;  // otherwise reverse
            }
            const bool    hov{ ImGui::IsItemHovered() };
            if (hov) ImGui::SetTooltip("Sorted by %s (%s) — click to %s", k_sort_names[g_class_sort_key],
                g_class_sort_key == 0 ? "load order" : (g_class_sort_desc ? "descending" : "ascending"),
                g_class_sort_key == 0 ? "sort by name" : "reverse");
            ImDrawList* dl{ ImGui::GetWindowDrawList() };
            dl->AddRectFilled(p0, p1, ImGui::GetColorU32(hov ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), st.FrameRounding);
            if (st.FrameBorderSize > 0.0f)
                dl->AddRect(p0, p1, ImGui::GetColorU32(ImGuiCol_Border), st.FrameRounding, 0, st.FrameBorderSize);
            dl->AddText(ImVec2(p0.x + hpad, p0.y + (bar_h - ImGui::GetFontSize()) * 0.5f),
                        ImGui::GetColorU32(ImGuiCol_Text), "Class");
            if (g_class_sort_key != 0)  // direction triangle (up = ascending, down = descending)
            {
                const float  cx{ p1.x - hpad }, cy{ p0.y + bar_h * 0.5f }, tr{ em(0.22f) };
                const ImU32  ac{ ImGui::GetColorU32(ImGuiCol_Text) };
                if (!g_class_sort_desc)
                    dl->AddTriangleFilled(ImVec2(cx, cy - tr), ImVec2(cx - tr, cy + tr), ImVec2(cx + tr, cy + tr), ac);
                else
                    dl->AddTriangleFilled(ImVec2(cx - tr, cy - tr), ImVec2(cx + tr, cy - tr), ImVec2(cx, cy + tr), ac);
            }
        }
        // Apply the chosen sort (Natural keeps the enumeration/filter order).  Every
        // key breaks ties on the full name so the order is stable + deterministic.
        if (g_class_sort_key != 0)
        {
            const auto key_cmp{ [&](int a, int b) -> int
            {
                const viewer::ClassInfo& ca{ app.classes[(std::size_t)a] };
                const viewer::ClassInfo& cb{ app.classes[(std::size_t)b] };
                switch (g_class_sort_key)
                {
                case 1: return ca.internal_name.compare(cb.internal_name);                                   // Name (full path)
                case 2: { const int c{ ca.package.compare(cb.package) }; return c ? c : ca.simple_name.compare(cb.simple_name); }  // Package, then simple name
                case 3: { if (ca.seen_epoch != cb.seen_epoch) return ca.seen_epoch < cb.seen_epoch ? -1 : 1; return ca.internal_name.compare(cb.internal_name); }  // Age (first-observed)
                case 4: { const int c{ std::strcmp(class_kind(ca).label, class_kind(cb).label) }; return c ? c : ca.internal_name.compare(cb.internal_name); }  // Kind
                case 5: { const std::size_t ma{ ca.methods.size() + ca.fields.size() }, mb{ cb.methods.size() + cb.fields.size() };
                          if (ma != mb) return ma < mb ? -1 : 1; return ca.internal_name.compare(cb.internal_name); }  // Members (methods+fields)
                default: return 0;
                }
            } };
            std::stable_sort(g_filtered.begin(), g_filtered.end(), [&](int a, int b)
            {
                const int c{ key_cmp(a, b) };
                return g_class_sort_desc ? c > 0 : c < 0;
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
                        navigate_to(idx, c.internal_name, false);  // clicked row already visible
                    // Left accent bar on the selected row (VS Code-style active marker).
                    if (sel)
                    {
                        const float h{ ImGui::GetFontSize() };
                        ImGui::GetWindowDrawList()->AddRectFilled(
                            ImVec2(rp.x - em(0.62f), rp.y + h * 0.08f),
                            ImVec2(rp.x - em(0.62f) + em(0.16f), rp.y + h * 0.92f),
                            ImGui::GetColorU32(theme::accent()), em(0.06f));
                    }
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
                        ImGui::PushStyleColor(ImGuiCol_Text, sel ? theme::text()
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
                    if (show_age_col)
                    {
                        ImGui::SameLine(0.0f, em(0.5f));
                        ImGui::TextDisabled("· %s", fmt_age(app.now_s() - c.seen_epoch).c_str());
                    }
                    // runtime-loaded classes get a small green dot in the left gutter
                    if (c.is_new)
                        ImGui::GetWindowDrawList()->AddCircleFilled(
                            ImVec2(rp.x - em(0.52f), rp.y + ImGui::GetFontSize() * 0.5f),
                            em(0.17f), ImGui::GetColorU32(theme::success()));
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
            ImGui::PushStyleColor(ImGuiCol_Text, theme::muted());
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
                        if (clicked) { navigate_to(ci, c.internal_name); std::snprintf(g_field_filter, sizeof(g_field_filter), "%s", g_search); }
                        ImGui::TableSetColumnIndex(1);
                        {
                            const std::string ty{ g_pretty ? viewer::pretty_field(f.descriptor, g_full_names) : f.descriptor };
                            const std::string refn{ ref_internal_name(f.descriptor) };
                            const auto rit{ refn.empty() ? app.name_to_index.end() : app.name_to_index.find(refn) };
                            if (rit != app.name_to_index.end()) { if (ImGui::TextLink(ty.c_str())) navigate_to(rit->second, rit->first); }
                            else ImGui::TextUnformatted(ty.c_str());
                        }
                    }
                    else
                    {
                        const viewer::MethodInfo& m{ c.methods[(std::size_t)mi] };
                        ImGui::PushStyleColor(ImGuiCol_Text, vis_color(m.access));
                        const bool clicked{ ImGui::Selectable(m.name.c_str(), false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap) };
                        ImGui::PopStyleColor();
                        if (clicked) { navigate_to(ci, c.internal_name); std::snprintf(g_method_filter, sizeof(g_method_filter), "%s", g_search); }
                        ImGui::TableSetColumnIndex(1);
                        render_method_signature(app, m.descriptor);
                    }
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextDisabled("%s", dotted.c_str());
                    ImGui::SetItemTooltip("%s", dotted.c_str());
                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }
    }

    void draw_details(viewer::App& app)
    {
        std::lock_guard<std::mutex> lock{ app.data_mutex };
        // Self-heal the selection: a Rescan can replace the class vector and shift
        // indices, so if the tracked index no longer names the selected class,
        // re-resolve it by name (dropping to "none" if it was unloaded).
        if (g_selected_class >= 0 &&
            (g_selected_class >= (int)app.classes.size() ||
             app.classes[(std::size_t)g_selected_class].internal_name != g_selected_name))
        {
            const auto it{ app.name_to_index.find(g_selected_name) };
            g_selected_class = (it != app.name_to_index.end()) ? it->second : -1;
        }
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
            ImGui::PushStyleColor(ImGuiCol_Text, theme::muted());
            ImGui::TextUnformatted("[final]");
            ImGui::PopStyleColor();
        }
        if (c.internal_name.find('$') != std::string::npos)
        {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, theme::muted());
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
                if (ImGui::TextLink(sd.c_str())) { navigate_to(it->second, it->first); }
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
        ImGui::SameLine(0.0f, em(0.4f));
        if (ui::Button("Static fields"))
        {
            g_statics_class = c.internal_name;
            g_statics_refresh_now = true;
            g_show_statics = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Read this class's live static-field values (editable, freezable) + call static methods");
        ImGui::Spacing();
        ui::Toggle("Show inherited members", &g_show_inherited);
        ImGui::SetItemTooltip("Include methods & fields from superclasses (shown dimmed)");
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
                        if (inh) ImGui::PushStyleColor(ImGuiCol_Text, theme::muted());
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
                        if (inh) ImGui::PushStyleColor(ImGuiCol_Text, theme::muted());
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
                            if (ImGui::TextLink(ty.c_str())) navigate_to(rit->second, rit->first);
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

    // Colour a formatted field value by its kind so the table scans easily:
    // strings, object refs (<...>), booleans, and null each get their own tint.
    ImVec4 value_color(const std::string& v) { return theme::value(v); }

    // ── object clipboard + field editing + method invocation ──────────────────

    // Cross-window drag-and-drop payload: a heap object (address + class + label).
    struct DragObj { char address[48]{}; char class_name[200]{}; char label[128]{}; };

    inline std::string cls_short(const std::string& internal)
    {
        const std::size_t p{ internal.find_last_of("/$") };
        return p == std::string::npos ? internal : internal.substr(p + 1);
    }
    inline std::string dotted_name(std::string s) { for (char& c : s) if (c == '/') c = '.'; return s; }

    // The internal class name a formatted reference value refers to: "<a/b/C>" ->
    // "a/b/C"; a "..." string -> java/lang/String; else "".
    inline std::string class_of_ref_value(const std::string& v)
    {
        if (v.size() >= 2 && v.front() == '<' && v.back() == '>') return v.substr(1, v.size() - 2);
        if (!v.empty() && v.front() == '"')                        return "java/lang/String";
        return {};
    }
    inline std::string frozen_key(char scope, const std::string& cls, const std::string& addr, const std::string& field)
    {
        std::string k(1, scope);
        k += '|'; k += cls;
        k += '|'; if (scope == 'I') k += addr;
        k += '|'; k += field;
        return k;
    }
    inline bool desc_is_ref(const std::string& d) { return !d.empty() && (d[0] == 'L' || d[0] == '['); }
    inline bool desc_is_array(const std::string& d) { return !d.empty() && d[0] == '['; }

    // Open the array inspector on the array at `addr` (elements typed by the field
    // descriptor with one leading '[' stripped).
    void open_array(const std::string& addr, const std::string& field_desc, const std::string& label)
    {
        if (addr.empty()) return;
        g_array_addr     = addr;
        g_array_elemdesc = field_desc.size() > 1 ? field_desc.substr(1) : std::string{};
        g_array_label    = label;
        g_show_array     = true;
        g_array_refresh  = true;
    }

    // Freeze / unfreeze through the app AND keep the UI's freeze registry in sync.
    // Only record the change if the request was ACTUALLY dispatched — otherwise
    // (a channel was busy so the op was dropped) the registry would claim a freeze
    // that never reached the VM, and the lock icon / overview would lie.
    void ui_freeze(viewer::App& app, char scope, const std::string& cls, const std::string& addr,
                   const std::string& field, const std::string& value)
    {
        if (app.freeze_field(scope, cls, addr, field, value))
            g_frozen[frozen_key(scope, cls, addr, field)] = FrozenField{ scope, cls, addr, field, value };
    }
    void ui_unfreeze(viewer::App& app, char scope, const std::string& cls, const std::string& addr, const std::string& field)
    {
        if (app.unfreeze_field(scope, cls, addr, field))
            g_frozen.erase(frozen_key(scope, cls, addr, field));
    }

    // Turn a displayed field value into a token accepted by the payload's writer:
    // strings lose their quotes (payload re-allocates), non-string refs use their
    // pointee address (or null), chars lose their quotes, everything else is verbatim.
    inline std::string writeable_value(const std::string& desc, const std::string& value, const std::string& ref_addr)
    {
        if (desc == "Ljava/lang/String;")
        {
            if (value == "null") return "null";
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"') return value.substr(1, value.size() - 2);
            return value;
        }
        if (desc_is_ref(desc)) return ref_addr.empty() ? std::string{ "null" } : ref_addr;
        if (value.size() >= 3 && value.front() == '\'' && value.back() == '\'') return value.substr(1, value.size() - 2);
        return value;
    }

    void add_saved_object(viewer::App& app, const std::string& label, const std::string& cls, const std::string& addr)
    {
        if (addr.empty() || addr == "null") return;
        for (const auto& s : g_clipboard) if (s.address == addr) return;  // dedup by address
        g_clipboard.push_back(viewer::SavedObject{ label, cls, addr, app.now_s() });
        g_show_clipboard = true;
    }

    // Make the last-submitted item a drag source carrying a heap object.
    void obj_drag_source(const std::string& addr, const std::string& cls, const std::string& label)
    {
        if (addr.empty() || addr == "null") return;
        // SourceAllowNullID: a reference VALUE is often rendered with TextWrapped
        // (item ID 0) when its class isn't a resolvable link, and BeginDragDropSource
        // refuses a null-ID source without this flag (asserts in a debug build).
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
        {
            DragObj d{};
            std::snprintf(d.address, sizeof(d.address), "%s", addr.c_str());
            std::snprintf(d.class_name, sizeof(d.class_name), "%s", cls.c_str());
            std::snprintf(d.label, sizeof(d.label), "%s", label.c_str());
            ImGui::SetDragDropPayload("VMHOOK_OBJ", &d, sizeof(d));
            ImGui::TextUnformatted(label.empty() ? addr.c_str() : label.c_str());
            if (!cls.empty()) ImGui::TextDisabled("%s", dotted_name(cls).c_str());
            ImGui::TextDisabled("%s", addr.c_str());
            ImGui::EndDragDropSource();
        }
    }
    // Accept a heap-object drop on the last-submitted item.
    bool obj_drop_target(DragObj& out)
    {
        bool got{ false };
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* p{ ImGui::AcceptDragDropPayload("VMHOOK_OBJ") })
                if (p->DataSize == static_cast<int>(sizeof(DragObj))) { std::memcpy(&out, p->Data, sizeof(DragObj)); got = true; }
            ImGui::EndDragDropTarget();
        }
        return got;
    }

    // name -> JVM descriptor for a class's fields, walking the super chain (so a
    // detail/statics row can know each field's real type without re-querying).
    std::unordered_map<std::string, std::string> field_desc_map(viewer::App& app, const std::string& cls, bool want_static)
    {
        std::unordered_map<std::string, std::string> out;
        const auto it0{ app.name_to_index.find(cls) };
        if (it0 == app.name_to_index.end()) return out;
        const viewer::ClassInfo* cur{ &app.classes[(std::size_t)it0->second] };
        for (int hops = 0; hops < 200 && cur; ++hops)
        {
            for (const auto& f : cur->fields)
                if (((f.access & 0x0008u) != 0u) == want_static)
                    out.emplace(f.name, f.descriptor);  // first (most-derived) wins
            if (cur->super_name.empty() || cur->super_name == "java/lang/Object") break;
            const auto it{ app.name_to_index.find(cur->super_name) };
            if (it == app.name_to_index.end()) break;
            cur = &app.classes[(std::size_t)it->second];
        }
        return out;
    }

    // Render the lock / edit / grab action icons for one field row (+ the edit
    // popup) and apply the user's choices.  scope 'I' instance, 'S' static.
    void field_actions(viewer::App& app, int uid, char scope, const std::string& cls, const std::string& addr,
                       const std::string& field, const std::string& value, const std::string& desc,
                       const std::string& ref_addr)
    {
        ImGui::PushID(uid);
        const std::string fkey{ frozen_key(scope, cls, addr, field) };
        const bool frozen{ g_frozen.count(fkey) != 0 };
        const bool is_ref{ desc_is_ref(desc) };
        const bool is_string{ desc == "Ljava/lang/String;" };
        const auto refreeze_if{ [&](const std::string& v) { if (frozen) ui_freeze(app, scope, cls, addr, field, v); } };

        // Freeze lock
        ImGui::PushStyleColor(ImGuiCol_Text, frozen ? theme::accent() : ImVec4(0.52f, 0.54f, 0.60f, 1.0f));
        const bool lk{ ui::IconButton(frozen ? ICON_FA_LOCK : ICON_FA_UNLOCK, "frz") };
        ImGui::PopStyleColor();
        ImGui::SetItemTooltip(frozen ? "Frozen — click to unlock" : "Freeze at the current value");
        if (lk)
        {
            if (frozen) ui_unfreeze(app, scope, cls, addr, field);
            else        ui_freeze(app, scope, cls, addr, field, writeable_value(desc, value, ref_addr));
        }

        // Edit
        ImGui::SameLine(0.0f, em(0.1f));
        if (ui::IconButton(ICON_FA_PEN, "edit"))
        {
            std::snprintf(g_edit_buf, sizeof(g_edit_buf), "%s", writeable_value(desc, value, ref_addr).c_str());
            ImGui::OpenPopup("editfield");
        }
        ImGui::SetItemTooltip("Edit value");

        // Grab (references only, non-null)
        if (is_ref && !ref_addr.empty())
        {
            ImGui::SameLine(0.0f, em(0.1f));
            if (ui::IconButton(ICON_FA_THUMBTACK, "grab"))
                add_saved_object(app, cls_short(cls) + "." + field, class_of_ref_value(value), ref_addr);
            ImGui::SetItemTooltip("Grab this object into the clipboard");
            obj_drag_source(ref_addr, class_of_ref_value(value), cls_short(cls) + "." + field);
        }

        if (ImGui::BeginPopup("editfield"))
        {
            ImGui::TextDisabled("%s : %s", field.c_str(), desc.c_str());
            ImGui::Separator();
            if (is_ref)
            {
                if (!g_clipboard.empty())
                {
                    ImGui::TextUnformatted("Place a saved object:");
                    for (int i = 0; i < (int)g_clipboard.size(); ++i)
                    {
                        const viewer::SavedObject& so{ g_clipboard[(std::size_t)i] };
                        ImGui::PushID(i);
                        const std::string lbl{ so.label + "   " + dotted_name(cls_short(so.class_name)) + "  " + so.address };
                        if (ImGui::Selectable(lbl.c_str()))
                        { app.set_field_value(scope, cls, addr, field, so.address); refreeze_if(so.address); ImGui::CloseCurrentPopup(); }
                        ImGui::PopID();
                    }
                    ImGui::Separator();
                }
                if (ImGui::SmallButton("Set null")) { app.set_field_value(scope, cls, addr, field, "null"); refreeze_if("null"); ImGui::CloseCurrentPopup(); }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(em(15.0f));
                const bool ent{ ImGui::InputText("##ev", g_edit_buf, sizeof(g_edit_buf), ImGuiInputTextFlags_EnterReturnsTrue) };
                ImGui::SameLine();
                if (ImGui::SmallButton(is_string ? "Set text" : "Set 0x") || ent)
                { app.set_field_value(scope, cls, addr, field, g_edit_buf); refreeze_if(g_edit_buf); ImGui::CloseCurrentPopup(); }
                if (is_string) ImGui::TextDisabled("text = new String; or 0x<oop> / null");
                else           ImGui::TextDisabled("0x<oop> address, or null");
            }
            else if (desc == "Z")
            {
                if (ImGui::SmallButton("true"))  { app.set_field_value(scope, cls, addr, field, "true");  refreeze_if("true");  ImGui::CloseCurrentPopup(); }
                ImGui::SameLine();
                if (ImGui::SmallButton("false")) { app.set_field_value(scope, cls, addr, field, "false"); refreeze_if("false"); ImGui::CloseCurrentPopup(); }
            }
            else
            {
                ImGui::SetNextItemWidth(em(12.0f));
                const bool ent{ ImGui::InputText("##ev", g_edit_buf, sizeof(g_edit_buf), ImGuiInputTextFlags_EnterReturnsTrue) };
                ImGui::SameLine();
                if (ImGui::SmallButton("Set") || ent) { app.set_field_value(scope, cls, addr, field, g_edit_buf); refreeze_if(g_edit_buf); ImGui::CloseCurrentPopup(); }
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }

    // Build the CALL argument token for one parameter from the user's text box.
    std::string call_token(const std::string& pd, const char* raw)
    {
        std::string s{ raw ? raw : "" };
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))   s.pop_back();
        if (!desc_is_ref(pd)) return s.empty() ? std::string{ "0" } : s;   // primitive: verbatim
        if (s.empty() || s == "null" || s == "@null") return "@null";
        if (s[0] == '@' || s[0] == '#') return s;
        if (s.rfind("0x", 0) == 0 || s.rfind("0X", 0) == 0) return "@" + s;
        const bool stringy{ pd == "Ljava/lang/String;" || pd == "Ljava/lang/CharSequence;" || pd == "Ljava/lang/Object;" };
        return stringy ? ("#" + s) : ("@" + s);
    }

    // A method-call panel: pick a method (of `cls` + supers), fill its arguments
    // (type a primitive, or drop / pick a saved object, or #text for a String),
    // Call, and see the result — which itself can be grabbed into the clipboard.
    // addr = the receiver ("" for a static-only context; static methods ignore it).
    void draw_call_panel(viewer::App& app, const std::string& cls, const std::string& addr, CallState& cs)
    {
        struct CallM { const viewer::MethodInfo* m; std::string owner; };
        std::vector<CallM> methods;
        {
            const auto it0{ app.name_to_index.find(cls) };
            const viewer::ClassInfo* cur{ it0 == app.name_to_index.end() ? nullptr : &app.classes[(std::size_t)it0->second] };
            for (int hops = 0; hops < 200 && cur; ++hops)
            {
                for (const auto& m : cur->methods)
                {
                    if (m.name == "<clinit>") continue;                            // static initialiser: not callable
                    // Constructors are only offered on the class itself (its own <init>s,
                    // not a superclass's), and create a new object rather than taking a receiver.
                    if (m.name == "<init>" && cur != (it0 == app.name_to_index.end() ? nullptr : &app.classes[(std::size_t)it0->second])) continue;
                    if (!addr.empty() || (m.access & 0x0008u) || m.name == "<init>")  // static ctx -> statics + ctors
                        methods.push_back({ &m, cls_short(cur->internal_name) });
                }
                if (cur->super_name.empty() || cur->super_name == "java/lang/Object") break;
                const auto it{ app.name_to_index.find(cur->super_name) };
                if (it == app.name_to_index.end()) break;
                cur = &app.classes[(std::size_t)it->second];
            }
        }

        // Reset the picker when the receiver context changes.
        const std::string ctx{ cls + "@" + addr };
        if (ctx != cs.ctx) { cs.ctx = ctx; cs.method_idx = -1; cs.has_result = false; for (auto& b : cs.args) b[0] = '\0'; }

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Call");
        ImGui::SameLine(0.0f, em(0.5f));
        ui::InputText("##callfilter", "filter methods", cs.filter, sizeof(cs.filter), em(12.0f));
        ImGui::SameLine(0.0f, em(0.4f));

        const std::string flt{ cs.filter };
        const auto label_of{ [&](const CallM& c) -> std::string
        {
            if (c.m->name == "<init>")  // constructor -> "new Class(params)"
                return "new " + c.owner + viewer::pretty_method(c.m->descriptor, false).substr(0, viewer::pretty_method(c.m->descriptor, false).rfind(" :"));
            std::string s{ (c.m->access & 0x0008u) ? "[S] " : "" };
            s += c.m->name;
            s += g_pretty ? viewer::pretty_method(c.m->descriptor, false) : c.m->descriptor;
            return s;
        } };
        const char* preview{ (cs.method_idx >= 0 && cs.method_idx < (int)methods.size())
                             ? nullptr : "select a method" };
        std::string preview_s;
        if (!preview) { preview_s = label_of(methods[(std::size_t)cs.method_idx]); preview = preview_s.c_str(); }
        if (ui::BeginCombo("##callm", preview, em(24.0f)))
        {
            for (int i = 0; i < (int)methods.size(); ++i)
            {
                const std::string lbl{ label_of(methods[(std::size_t)i]) };
                if (!flt.empty() && !icontains(lbl, flt)) continue;
                ImGui::PushID(i);
                if (ImGui::Selectable(lbl.c_str(), i == cs.method_idx))
                { cs.method_idx = i; cs.has_result = false; for (auto& b : cs.args) b[0] = '\0'; }
                ImGui::PopID();
            }
            ui::EndCombo();
        }

        if (cs.method_idx < 0 || cs.method_idx >= (int)methods.size()) return;
        const viewer::MethodInfo& m{ *methods[(std::size_t)cs.method_idx].m };
        const bool is_static{ (m.access & 0x0008u) != 0u };
        // Parse parameter descriptors.
        std::vector<std::string> pds;
        {
            const std::string& d{ m.descriptor };
            std::size_t i{ d.find('(') };
            if (i != std::string::npos) { ++i; while (i < d.size() && d[i] != ')') { const std::size_t s{ i }; while (i < d.size() && d[i] == '[') ++i; if (i < d.size() && d[i] == 'L') { const std::size_t sc{ d.find(';', i) }; i = (sc == std::string::npos ? d.size() : sc + 1); } else if (i < d.size()) ++i; pds.push_back(d.substr(s, i - s)); } }
        }
        if ((int)pds.size() > 8) { ImGui::TextDisabled("(more than 8 arguments — not callable)"); return; }

        for (int a = 0; a < (int)pds.size(); ++a)
        {
            ImGui::PushID(a);
            const std::string ptxt{ g_pretty ? viewer::pretty_field(pds[(std::size_t)a], false) : pds[(std::size_t)a] };
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("arg%d", a); ImGui::SameLine(0.0f, em(0.3f));
            ImGui::PushStyleColor(ImGuiCol_Text, theme::link());
            ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(ptxt.c_str());
            ImGui::PopStyleColor();
            ImGui::SameLine(em(10.0f));
            ui::InputText("##arg", desc_is_ref(pds[(std::size_t)a]) ? "@null / @0x.. / #text / drop object" : "value",
                          cs.args[a], sizeof(cs.args[a]), desc_is_ref(pds[(std::size_t)a]) ? em(16.0f) : em(10.0f));
            if (desc_is_ref(pds[(std::size_t)a]))
            {
                DragObj d{};
                if (obj_drop_target(d)) std::snprintf(cs.args[a], sizeof(cs.args[a]), "@%s", d.address);
            }
            ImGui::PopID();
        }

        const bool can_call{ !app.any_busy() };
        ImGui::BeginDisabled(!can_call);
        if (ui::Button(ICON_FA_PLAY "  Call", ImVec2(0, 0), ui::BtnPrimary))
        {
            std::vector<std::string> toks;
            for (int a = 0; a < (int)pds.size(); ++a) toks.push_back(call_token(pds[(std::size_t)a], cs.args[a]));
            app.call_method(cls, (is_static || m.name == "<init>") ? std::string{} : addr, m.name, m.descriptor, toks);
            cs.pending = true; cs.has_result = false;
        }
        ImGui::EndDisabled();
        if (is_static) { ImGui::SameLine(); ImGui::TextDisabled("(static)"); }

        if (cs.has_result)
        {
            ImGui::SameLine(0.0f, em(0.8f));
            ImGui::AlignTextToFramePadding();
            if (cs.result.ok)
            {
                ImGui::TextDisabled("->"); ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, value_color(cs.result.disp));
                ImGui::TextUnformatted(cs.result.disp.c_str());
                ImGui::PopStyleColor();
                if (!cs.result.raddr.empty())
                {
                    ImGui::SameLine(0.0f, em(0.5f));
                    if (ui::Button(ICON_FA_THUMBTACK " Grab result"))
                        add_saved_object(app, m.name + "()", cs.result.rclass, cs.result.raddr);
                    obj_drag_source(cs.result.raddr, cs.result.rclass, m.name + "()");
                }
            }
            else
            {
                ImGui::PushStyleColor(ImGuiCol_Text, theme::danger());
                ImGui::TextWrapped("%s", cs.result.error.c_str());
                ImGui::PopStyleColor();
            }
        }
    }

    // The floating "Saved objects" clipboard: a drop target (drag an instance /
    // reference here to stash it) whose chips are themselves drag sources (drag one
    // onto a field or method argument to place it).
    void draw_clipboard_panel(viewer::App& app)
    {
        ImGui::BeginChild("clipboard", ImVec2(0, em(6.6f)), ImGuiChildFlags_Borders);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(ICON_FA_THUMBTACK "  Saved objects");
        ImGui::SameLine(0.0f, em(0.5f));
        ImGui::TextDisabled("(%d)", (int)g_clipboard.size());
        ImGui::SameLine(0.0f, em(0.6f));
        ImGui::TextDisabled("drag an object here to stash it · drag a chip onto a field / arg to place it");
        ImGui::SameLine((std::max)(ImGui::GetContentRegionMax().x - em(5.0f), ImGui::GetCursorPosX() + em(1.0f)));
        ImGui::BeginDisabled(g_clipboard.empty());
        if (ui::Button("Clear")) g_clipboard.clear();
        ImGui::EndDisabled();
        ImGui::Separator();

        ImGui::BeginChild("clipscroll", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
        if (g_clipboard.empty())
            ImGui::TextDisabled("empty — grab a reference field (thumbtack) or drag an object here");
        int remove{ -1 };
        for (int i = 0; i < (int)g_clipboard.size(); ++i)
        {
            const viewer::SavedObject& so{ g_clipboard[(std::size_t)i] };
            ImGui::PushID(i);
            ImGui::BeginGroup();
            const ImVec2 p0{ ImGui::GetCursorScreenPos() };
            ImGui::Dummy(ImVec2(em(13.0f), 0));
            ImGui::PushStyleColor(ImGuiCol_Text, theme::heading());
            ImGui::TextUnformatted(so.label.empty() ? "object" : so.label.c_str());
            ImGui::PopStyleColor();
            ImGui::PushStyleColor(ImGuiCol_Text, theme::link());
            ImGui::TextUnformatted(dotted_name(cls_short(so.class_name)).c_str());
            ImGui::PopStyleColor();
            ImGui::TextDisabled("%s", so.address.c_str());
            ImGui::EndGroup();
            // Chip frame + hover highlight.
            const ImVec2 p1{ ImGui::GetItemRectMax() };
            const bool hov{ ImGui::IsMouseHoveringRect(p0, p1) };
            ImGui::GetWindowDrawList()->AddRect(ImVec2(p0.x - em(0.3f), p0.y - em(0.2f)), ImVec2(p1.x + em(0.3f), p1.y + em(0.2f)),
                ImGui::GetColorU32(hov ? ImGuiCol_SliderGrab : ImGuiCol_Border), ImGui::GetStyle().FrameRounding);
            // Whole chip is a drag source.
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
            {
                DragObj d{};
                std::snprintf(d.address, sizeof(d.address), "%s", so.address.c_str());
                std::snprintf(d.class_name, sizeof(d.class_name), "%s", so.class_name.c_str());
                std::snprintf(d.label, sizeof(d.label), "%s", so.label.c_str());
                ImGui::SetDragDropPayload("VMHOOK_OBJ", &d, sizeof(d));
                ImGui::TextUnformatted(so.label.c_str());
                ImGui::TextDisabled("%s", so.address.c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginPopupContextItem("chipctx"))
            {
                if (ImGui::MenuItem("Copy address")) ImGui::SetClipboardText(so.address.c_str());
                if (ImGui::MenuItem("Remove"))       remove = i;
                ImGui::EndPopup();
            }
            ImGui::PopID();
            ImGui::SameLine(0.0f, em(0.9f));
        }
        if (remove >= 0) g_clipboard.erase(g_clipboard.begin() + remove);
        ImGui::EndChild();
        // Dropping anywhere in the panel stashes the object.
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* p{ ImGui::AcceptDragDropPayload("VMHOOK_OBJ") })
                if (p->DataSize == (int)sizeof(DragObj)) { DragObj d{}; std::memcpy(&d, p->Data, sizeof(d)); add_saved_object(app, d.label, d.class_name, d.address); }
            ImGui::EndDragDropTarget();
        }
        ImGui::EndChild();
    }

    void draw_instances_window(viewer::App& app)
    {
        ImGui::SetNextWindowSize(ImVec2(em(64.0f), em(34.0f)), ImGuiCond_FirstUseEver);
        const bool open{ ImGui::Begin("Live instances", &g_show_instances, ImGuiWindowFlags_NoCollapse) };
        if (!open) { ImGui::End(); return; }

        std::lock_guard<std::mutex> lock{ app.data_mutex };
        const viewer::Status st{ app.inst_status.load() };
        const double now{ app.now_s() };
        const float  toggle_dy{ ImGui::GetFrameHeight() * 0.13f };  // vertical-centre small switches

        // ── header: class name (+ spinner) .............. Live | Refresh | Copy ──
        std::string dotted{ app.inst_class };
        for (char& ch : dotted) if (ch == '/') ch = '.';
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, theme::header());
        ImGui::TextUnformatted(dotted.empty() ? "(no class selected)" : dotted.c_str());
        ImGui::PopStyleColor();
        if (st == viewer::Status::Receiving)
        {
            ImGui::SameLine(0.0f, em(0.5f));
            const float r{ ImGui::GetFrameHeight() * 0.28f };
            ui::Spinner("##iscan", r, (std::max)(r * 0.35f, em(0.12f)), ImGui::GetColorU32(theme::accent()));
        }
        ImGui::SameLine((std::max)(ImGui::GetContentRegionMax().x - em(17.0f), ImGui::GetCursorPosX() + em(1.0f)));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + toggle_dy);
        ui::Toggle("Live", &g_instances_live);
        ImGui::SetItemTooltip("Re-scan the heap ~every 1.5s so field values update live");
        ImGui::SameLine(0.0f, em(0.7f));
        if (ui::Button("Refresh")) g_instances_refresh_now = true;
        ImGui::SameLine(0.0f, em(0.4f));
        if (ui::Button("Copy table")) g_copy_instance_table = true;
        ImGui::SetItemTooltip("Copy every instance's fields to the clipboard as TSV");

        // ── filter row: filter + Age + Inherited + cap ──
        ui::InputText("##ifilter", ICON_FA_SEARCH "  Filter instances",
                      g_instance_filter, sizeof(g_instance_filter),
                      (std::max)(ImGui::GetContentRegionAvail().x - em(23.0f), em(6.0f)));
        ImGui::SameLine(0.0f, em(0.6f));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + toggle_dy);
        ui::Toggle("Age", &g_inst_show_age);
        ImGui::SetItemTooltip("Show how long ago each instance was first observed + sort newest-first.\nHotSpot has no per-object creation time and a moving GC can reset this — best-effort.");
        ImGui::SameLine(0.0f, em(0.6f));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + toggle_dy);
        ui::Toggle("Inherited", &g_inst_show_inherited);
        ImGui::SetItemTooltip("Include inherited fields in the detail pane");
        ImGui::SameLine(0.0f, em(0.6f));
        ImGui::SetNextItemWidth(em(7.5f));
        int cap{ app.inst_cap };
        if (ImGui::DragInt("##icap", &cap, 10.0f, 20, 200000, "cap %d"))
        {
            app.inst_cap = std::clamp(cap, 20, 200000);
            g_instance_cap = app.inst_cap;
            g_instances_refresh_now = true;
        }
        ImGui::SetItemTooltip("Max instances to scan for on the heap");

        // filter the rows (address or any field value)
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

        // "Found N" (+ shown + cap warning)
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", app.inst_message.c_str());
        if (!needle.empty()) { ImGui::SameLine(0.0f, em(0.5f)); ImGui::TextDisabled("· %d shown", (int)view.size()); }
        if (app.inst_cap > 0 && (int)app.instances.size() >= app.inst_cap)
        {
            ImGui::SameLine(0.0f, em(0.6f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.75f, 0.35f, 1.0f));
            ImGui::TextUnformatted("scan cap reached — raise it to see more");
            ImGui::PopStyleColor();
        }

        ImGui::Separator();

        if (app.instances.empty())
        {
            if (app.inst_message.rfind("Found 0", 0) == 0)
            {
                ImGui::TextDisabled("No live instances of this exact class on the heap.");
                ImGui::TextDisabled("The scan matches this class only — if it is abstract, or just its");
                ImGui::TextDisabled("subclasses are instantiated, inspect a subclass instead.");
            }
            ImGui::End();
            return;
        }

        // sort the view — Age on → newest first; else the header's address sort.
        if (g_inst_show_age)
            std::stable_sort(view.begin(), view.end(), [&](int a, int b)
            {
                const double ea{ app.instances[(std::size_t)a].seen_epoch }, eb{ app.instances[(std::size_t)b].seen_epoch };
                if (ea != eb) return ea > eb;
                return app.instances[(std::size_t)a].address < app.instances[(std::size_t)b].address;
            });
        else if (g_inst_sort != 0)
            std::stable_sort(view.begin(), view.end(), [&](int a, int b)
            {
                const unsigned long long x{ std::strtoull(app.instances[(std::size_t)a].address.c_str(), nullptr, 16) };
                const unsigned long long y{ std::strtoull(app.instances[(std::size_t)b].address.c_str(), nullptr, 16) };
                return g_inst_sort == 1 ? x < y : x > y;
            });

        // keep a valid selection (default to the first row) so the detail is filled
        bool sel_present{ false };
        for (const auto& in : app.instances) if (in.address == g_detail_addr) { sel_present = true; break; }
        if (!sel_present && !view.empty()) g_detail_addr = app.instances[(std::size_t)view.front()].address;

        // ── master / detail split (mirrors the main window) ──
        const float avail_y{ ImGui::GetContentRegionAvail().y };
        ImGui::BeginChild("ileft", ImVec2(g_inst_left_width, avail_y), ImGuiChildFlags_Borders);
        {
            const ImGuiStyle& stl{ ImGui::GetStyle() };
            // Rounded "Instances (N)" header (matches the combo/input cells); click
            // cycles the address sort (ignored while Age sort is active).
            const ImVec2 hp0{ ImGui::GetCursorScreenPos() };
            const float  hw{ ImGui::GetContentRegionAvail().x }, hh{ ImGui::GetFrameHeight() };
            const ImVec2 hp1{ hp0.x + hw, hp0.y + hh };
            if (ImGui::InvisibleButton("##ihdr", ImVec2(hw, hh)) && !g_inst_show_age)
                g_inst_sort = (g_inst_sort == 1) ? 2 : 1;
            const bool hhov{ ImGui::IsItemHovered() };
            ImDrawList* hdl{ ImGui::GetWindowDrawList() };
            hdl->AddRectFilled(hp0, hp1, ImGui::GetColorU32(hhov ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), stl.FrameRounding);
            if (stl.FrameBorderSize > 0.0f) hdl->AddRect(hp0, hp1, ImGui::GetColorU32(ImGuiCol_Border), stl.FrameRounding, 0, stl.FrameBorderSize);
            char hlbl[40]; std::snprintf(hlbl, sizeof(hlbl), "Instances  (%d)", (int)view.size());
            hdl->AddText(ImVec2(hp0.x + stl.FramePadding.x, hp0.y + (hh - ImGui::GetFontSize()) * 0.5f),
                         ImGui::GetColorU32(ImGuiCol_Text), hlbl);
            {
                const float cx{ hp1.x - stl.FramePadding.x }, cy{ hp0.y + hh * 0.5f }, tr{ em(0.22f) };
                const ImU32 ac{ ImGui::GetColorU32(ImGuiCol_Text) };
                const bool up{ !g_inst_show_age && g_inst_sort == 1 };  // Age sort = newest-first (down)
                if (up) hdl->AddTriangleFilled(ImVec2(cx, cy - tr), ImVec2(cx - tr, cy + tr), ImVec2(cx + tr, cy + tr), ac);
                else    hdl->AddTriangleFilled(ImVec2(cx - tr, cy - tr), ImVec2(cx + tr, cy - tr), ImVec2(cx, cy + tr), ac);
            }

            ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(stl.FramePadding.x, ImGui::GetStyle().CellPadding.y));
            if (ImGui::BeginTable("ilist", 1, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_PadOuterX))
            {
                ImGui::TableSetupColumn("addr", ImGuiTableColumnFlags_WidthStretch);
                ImGuiListClipper clip; clip.Begin((int)view.size());
                while (clip.Step())
                    for (int vr = clip.DisplayStart; vr < clip.DisplayEnd; ++vr)
                    {
                        const int r{ view[(std::size_t)vr] };
                        const viewer::InstanceInfo& inst{ app.instances[(std::size_t)r] };
                        const bool sel{ g_detail_addr == inst.address };
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::PushID(r);
                        const ImVec2 rp{ ImGui::GetCursorScreenPos() };
                        if (ImGui::Selectable("##irow", sel, ImGuiSelectableFlags_SpanAllColumns))
                            g_detail_addr = inst.address;
                        copy_menu("addr", inst.address);
                        obj_drag_source(inst.address, app.inst_class, cls_short(app.inst_class));
                        ImGui::SetCursorScreenPos(rp);
                        ImGui::PushStyleColor(ImGuiCol_Text, sel ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : theme::muted());
                        ImGui::TextUnformatted(inst.address.c_str());
                        ImGui::PopStyleColor();
                        if (g_inst_show_age)
                        {
                            ImGui::SameLine(0.0f, em(0.5f));
                            ImGui::TextDisabled("· %s", fmt_age(now - inst.seen_epoch).c_str());
                        }
                        ImGui::PopID();
                    }
                ImGui::EndTable();
            }
            ImGui::PopStyleVar();
        }
        ImGui::EndChild();

        ImGui::SameLine();
        ImGui::InvisibleButton("isplit", ImVec2(em(0.4f), avail_y));
        if (ImGui::IsItemActive()) g_inst_left_width += ImGui::GetIO().MouseDelta.x;
        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        g_inst_left_width = std::clamp(g_inst_left_width, em(9.0f), (std::max)(ImGui::GetWindowSize().x - em(14.0f), em(10.0f)));
        {
            const ImVec2 sp0{ ImGui::GetItemRectMin() }, sp1{ ImGui::GetItemRectMax() };
            const float  sx{ (sp0.x + sp1.x) * 0.5f };
            ImGui::GetWindowDrawList()->AddLine(ImVec2(sx, sp0.y + em(0.15f)), ImVec2(sx, sp1.y - em(0.15f)),
                ImGui::GetColorU32(ImGui::IsItemActive() ? ImGuiCol_ButtonActive : ImGuiCol_Border), (std::max)(em(0.12f), 1.0f));
        }
        ImGui::SameLine();

        // ── right: the selected instance's fields (live, re-looked-up by address) ──
        ImGui::BeginChild("iright", ImVec2(0, avail_y), ImGuiChildFlags_Borders);
        {
            const viewer::InstanceInfo* selp{ nullptr };
            for (const auto& in : app.instances) if (in.address == g_detail_addr) { selp = &in; break; }
            if (!selp)
            {
                ImGui::Dummy(ImVec2(0, em(1.0f)));
                ImGui::PushStyleColor(ImGuiCol_Text, theme::muted());
                const char* hint{ "Select an instance on the left to see its fields." };
                const float w{ ImGui::CalcTextSize(hint).x };
                ImGui::SetCursorPosX((std::max)((ImGui::GetContentRegionAvail().x - w) * 0.5f, 0.0f));
                ImGui::TextUnformatted(hint);
                ImGui::PopStyleColor();
            }
            else
            {
                std::string dcls{ app.inst_class };
                for (char& ch : dcls) if (ch == '/') ch = '.';
                ImGui::AlignTextToFramePadding();
                ImGui::PushStyleColor(ImGuiCol_Text, theme::header());
                ImGui::TextUnformatted(dcls.c_str());
                ImGui::PopStyleColor();
                ImGui::SameLine(0.0f, em(0.35f)); ImGui::TextDisabled("@"); ImGui::SameLine(0.0f, em(0.35f));
                ImGui::PushStyleColor(ImGuiCol_Text, theme::muted());
                ImGui::TextUnformatted(selp->address.c_str());
                ImGui::PopStyleColor();
                if (g_inst_show_age)
                {
                    ImGui::SameLine(0.0f, em(0.6f));
                    ImGui::TextDisabled("· seen %s ago", fmt_age(now - selp->seen_epoch).c_str());
                }
                ImGui::SameLine(0.0f, em(0.8f));
                if (ui::Button(ICON_FA_THUMBTACK " Grab"))
                    add_saved_object(app, cls_short(app.inst_class), app.inst_class, selp->address);
                ImGui::SetItemTooltip("Stash this instance in the clipboard");
                obj_drag_source(selp->address, app.inst_class, cls_short(app.inst_class));
                ImGui::SameLine(0.0f, em(0.4f));
                if (ui::Button("Statics")) { g_statics_class = app.inst_class; g_statics_refresh_now = true; g_show_statics = true; }
                ImGui::SetItemTooltip("Open this class's static fields");
                ImGui::SameLine(0.0f, em(0.4f));
                if (ui::Button("Copy addr")) ImGui::SetClipboardText(selp->address.c_str());
                ImGui::SameLine(0.0f, em(0.4f));
                if (ui::Button("Copy all"))
                {
                    std::string tsv{ dcls + " @ " + selp->address + "\nField\tValue\tFrom\n" };
                    for (const viewer::InstField& f : selp->fields)
                    {
                        tsv += f.name; tsv += '\t'; tsv += f.value; tsv += '\t'; tsv += f.owner; tsv += '\n';
                    }
                    ImGui::SetClipboardText(tsv.c_str());
                }
                ImGui::Separator();

                const std::unordered_map<std::string, std::string> descOf{ field_desc_map(app, app.inst_class, false) };
                const auto eff_desc{ [&](const viewer::InstField& f) -> std::string
                {
                    if (const auto it{ descOf.find(f.name) }; it != descOf.end()) return it->second;
                    if (!f.value.empty() && f.value.front() == '"') return "Ljava/lang/String;";
                    if (!f.ref_addr.empty() || f.value == "null" || (f.value.size() > 1 && f.value.front() == '<')) return "Ljava/lang/Object;";
                    return {};  // primitive of unknown width — writer still parses the text
                } };

                // Field | Value (editable / droppable) | Actions
                const float detail_h{ (std::max)(ImGui::GetContentRegionAvail().y - em(11.0f), em(6.0f)) };
                if (ImGui::BeginTable("idetail", 3,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH, ImVec2(0, detail_h)))
                {
                    ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthFixed, em(9.0f));
                    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("",      ImGuiTableColumnFlags_WidthFixed, em(4.6f));
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableHeadersRow();
                    int uid{ 0 };
                    for (const viewer::InstField& f : selp->fields)
                    {
                        if (!g_inst_show_inherited && !f.owner.empty()) continue;
                        const std::string desc{ eff_desc(f) };
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        if (!f.owner.empty()) ImGui::PushStyleColor(ImGuiCol_Text, theme::muted());
                        ImGui::TextUnformatted(f.name.c_str());
                        if (!f.owner.empty()) { ImGui::PopStyleColor(); ImGui::SetItemTooltip("inherited from %s", f.owner.c_str()); }

                        ImGui::TableSetColumnIndex(1);
                        bool linked{ false };
                        if (desc_is_array(desc) && !f.ref_addr.empty() && f.value != "null")
                        {
                            if (ImGui::TextLink(f.value.c_str()))
                                open_array(f.ref_addr, desc, dotted_name(cls_short(app.inst_class)) + "." + f.name);
                            ImGui::SetItemTooltip("%s  (click to view the array's elements)", f.ref_addr.c_str());
                            linked = true;
                        }
                        else if (f.value.size() > 2 && f.value.front() == '<' && f.value.back() == '>')
                        {
                            const std::string internal{ f.value.substr(1, f.value.size() - 2) };
                            if (const auto it{ app.name_to_index.find(internal) }; it != app.name_to_index.end())
                            {
                                if (ImGui::TextLink(f.value.c_str())) navigate_to(it->second, it->first);
                                ImGui::SetItemTooltip("%s  (click to open · drag to grab)", internal.c_str());
                                linked = true;
                            }
                        }
                        if (!linked)
                        {
                            ImGui::PushStyleColor(ImGuiCol_Text, value_color(f.value));
                            ImGui::TextWrapped("%s", f.value.c_str());
                            ImGui::PopStyleColor();
                        }
                        // A reference value is a drag source (grab) and a drop target (place).
                        if (desc_is_ref(desc))
                        {
                            if (!f.ref_addr.empty()) obj_drag_source(f.ref_addr, class_of_ref_value(f.value), cls_short(app.inst_class) + "." + f.name);
                            DragObj d{};
                            if (obj_drop_target(d))
                            {
                                app.set_field_value('I', app.inst_class, selp->address, f.name, d.address);
                                if (g_frozen.count(frozen_key('I', app.inst_class, selp->address, f.name))) ui_freeze(app, 'I', app.inst_class, selp->address, f.name, d.address);
                            }
                        }

                        ImGui::TableSetColumnIndex(2);
                        field_actions(app, uid++, 'I', app.inst_class, selp->address, f.name, f.value, desc, f.ref_addr);
                    }
                    ImGui::EndTable();
                }

                // Method invocation on this instance.
                ImGui::Separator();
                draw_call_panel(app, app.inst_class, selp->address, g_call_inst);
            }
        }
        ImGui::EndChild();

        // Copy table → every instance's fields as TSV (current view order).
        if (g_copy_instance_table)
        {
            g_copy_instance_table = false;
            const std::vector<viewer::InstField>& f0{ app.instances.front().fields };
            std::string tsv{ "Address" };
            for (const auto& f : f0) { tsv += '\t'; tsv += f.name; }
            tsv += '\n';
            for (const int rr : view)
            {
                const viewer::InstanceInfo& in{ app.instances[(std::size_t)rr] };
                tsv += in.address;
                for (std::size_t k = 0; k < f0.size(); ++k) { tsv += '\t'; if (k < in.fields.size()) tsv += in.fields[k].value; }
                tsv += '\n';
            }
            ImGui::SetClipboardText(tsv.c_str());
        }

        ImGui::End();
    }

    // The class's live STATIC fields (mirror state the instance view can't show):
    // editable + freezable like instance fields, plus a static-method call panel.
    void draw_statics_window(viewer::App& app)
    {
        ImGui::SetNextWindowSize(ImVec2(em(46.0f), em(30.0f)), ImGuiCond_FirstUseEver);
        const bool open{ ImGui::Begin("Static fields", &g_show_statics, ImGuiWindowFlags_NoCollapse) };
        if (!open) { ImGui::End(); return; }

        std::lock_guard<std::mutex> lock{ app.data_mutex };
        const viewer::Status st{ app.stat_status.load() };
        const float toggle_dy{ ImGui::GetFrameHeight() * 0.13f };

        std::string dotted{ app.stat_class };
        for (char& ch : dotted) if (ch == '/') ch = '.';
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, theme::header());
        ImGui::TextUnformatted(dotted.empty() ? "(no class)" : dotted.c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine(0.0f, em(0.35f));
        ImGui::TextDisabled("· statics");
        if (st == viewer::Status::Receiving)
        {
            ImGui::SameLine(0.0f, em(0.5f));
            const float r{ ImGui::GetFrameHeight() * 0.28f };
            ui::Spinner("##sscan", r, (std::max)(r * 0.35f, em(0.12f)), ImGui::GetColorU32(theme::accent()));
        }
        ImGui::SameLine((std::max)(ImGui::GetContentRegionMax().x - em(11.0f), ImGui::GetCursorPosX() + em(1.0f)));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + toggle_dy);
        ui::Toggle("Live", &g_statics_live);
        ImGui::SetItemTooltip("Re-read the static fields ~every 1.5s");
        ImGui::SameLine(0.0f, em(0.6f));
        if (ui::Button("Refresh")) g_statics_refresh_now = true;

        ui::InputText("##sfilter", ICON_FA_SEARCH "  Filter statics", g_statics_filter, sizeof(g_statics_filter),
                      (std::max)(ImGui::GetContentRegionAvail().x - em(0.5f), em(6.0f)));
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", app.stat_message.c_str());
        ImGui::Separator();

        if (app.stat_class.empty())
        {
            ImGui::TextDisabled("Open statics from a class's Live-instances detail (Statics button).");
            ImGui::End();
            return;
        }

        const std::unordered_map<std::string, std::string> descOf{ field_desc_map(app, app.stat_class, true) };
        std::string needle{ g_statics_filter };
        for (char& c : needle) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        const float table_h{ (std::max)(ImGui::GetContentRegionAvail().y - em(11.0f), em(6.0f)) };
        if (ImGui::BeginTable("sdetail", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH, ImVec2(0, table_h)))
        {
            ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthFixed, em(10.0f));
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("",      ImGuiTableColumnFlags_WidthFixed, em(4.6f));
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();
            int uid{ 0 };
            for (const viewer::InstField& f : app.statics_obj.fields)
            {
                if (!needle.empty() && !contains_ci(f.name, needle) && !contains_ci(f.value, needle)) continue;
                std::string desc;
                if (const auto it{ descOf.find(f.name) }; it != descOf.end()) desc = it->second;
                else if (!f.value.empty() && f.value.front() == '"') desc = "Ljava/lang/String;";
                else if (!f.ref_addr.empty() || f.value == "null" || (f.value.size() > 1 && f.value.front() == '<')) desc = "Ljava/lang/Object;";

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(f.name.c_str());
                ImGui::TableSetColumnIndex(1);
                bool linked{ false };
                if (desc_is_array(desc) && !f.ref_addr.empty() && f.value != "null")
                {
                    if (ImGui::TextLink(f.value.c_str()))
                        open_array(f.ref_addr, desc, dotted_name(cls_short(app.stat_class)) + "." + f.name);
                    ImGui::SetItemTooltip("%s  (click to view the array's elements)", f.ref_addr.c_str());
                    linked = true;
                }
                else if (f.value.size() > 2 && f.value.front() == '<' && f.value.back() == '>')
                {
                    const std::string internal{ f.value.substr(1, f.value.size() - 2) };
                    if (const auto it{ app.name_to_index.find(internal) }; it != app.name_to_index.end())
                    {
                        if (ImGui::TextLink(f.value.c_str())) navigate_to(it->second, it->first);
                        ImGui::SetItemTooltip("%s  (click to open · drag to grab)", internal.c_str());
                        linked = true;
                    }
                }
                if (!linked)
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, value_color(f.value));
                    ImGui::TextWrapped("%s", f.value.c_str());
                    ImGui::PopStyleColor();
                }
                if (desc_is_ref(desc))
                {
                    if (!f.ref_addr.empty()) obj_drag_source(f.ref_addr, class_of_ref_value(f.value), cls_short(app.stat_class) + "." + f.name);
                    DragObj d{};
                    if (obj_drop_target(d))
                    {
                        app.set_field_value('S', app.stat_class, {}, f.name, d.address);
                        if (g_frozen.count(frozen_key('S', app.stat_class, {}, f.name))) ui_freeze(app, 'S', app.stat_class, {}, f.name, d.address);
                    }
                }
                ImGui::TableSetColumnIndex(2);
                field_actions(app, uid++, 'S', app.stat_class, {}, f.name, f.value, desc, f.ref_addr);
            }
            ImGui::EndTable();
        }

        // Static-method invocation (addr empty -> the panel lists statics only).
        ImGui::Separator();
        draw_call_panel(app, app.stat_class, {}, g_call_stat);

        ImGui::End();
    }

    // The array inspector: the elements of the array a user clicked.  Read-only
    // element list; reference elements can be grabbed / dragged into the clipboard.
    void draw_array_window(viewer::App& app)
    {
        ImGui::SetNextWindowSize(ImVec2(em(30.0f), em(28.0f)), ImGuiCond_FirstUseEver);
        const bool open{ ImGui::Begin("Array", &g_show_array, ImGuiWindowFlags_NoCollapse) };
        if (!open) { ImGui::End(); return; }

        std::lock_guard<std::mutex> lock{ app.data_mutex };
        const viewer::Status st{ app.arr_status.load() };

        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, theme::header());
        ImGui::TextUnformatted(g_array_label.empty() ? "array" : g_array_label.c_str());
        ImGui::PopStyleColor();
        if (st == viewer::Status::Receiving)
        {
            ImGui::SameLine(0.0f, em(0.5f));
            const float r{ ImGui::GetFrameHeight() * 0.28f };
            ui::Spinner("##ascan", r, (std::max)(r * 0.35f, em(0.12f)), ImGui::GetColorU32(theme::accent()));
        }
        ImGui::SameLine((std::max)(ImGui::GetContentRegionMax().x - em(5.0f), ImGui::GetCursorPosX() + em(1.0f)));
        if (ui::Button("Refresh")) g_array_refresh = true;
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", g_array_addr.c_str());
        ImGui::SameLine(0.0f, em(0.6f));
        ImGui::TextDisabled("%s", app.arr_message.c_str());
        ImGui::Separator();

        if (app.array_elems.empty())
        {
            ImGui::TextDisabled(app.arr_length == 0 ? "(empty array)" : "Reading...");
            ImGui::End();
            return;
        }

        if (ImGui::BeginTable("arrt", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH))
        {
            ImGui::TableSetupColumn("#",     ImGuiTableColumnFlags_WidthFixed, em(4.0f));
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("",      ImGuiTableColumnFlags_WidthFixed, em(2.4f));
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();
            int uid{ 0 };
            for (const viewer::InstField& e : app.array_elems)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("%s", e.name.c_str());
                ImGui::TableSetColumnIndex(1);
                bool linked{ false };
                if (e.value.size() > 2 && e.value.front() == '<' && e.value.back() == '>')
                {
                    const std::string internal{ e.value.substr(1, e.value.size() - 2) };
                    if (const auto it{ app.name_to_index.find(internal) }; it != app.name_to_index.end())
                    { if (ImGui::TextLink(e.value.c_str())) navigate_to(it->second, it->first); linked = true; }
                }
                if (!linked)
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, value_color(e.value));
                    ImGui::TextUnformatted(e.value.c_str());
                    ImGui::PopStyleColor();
                }
                if (!e.ref_addr.empty())
                    obj_drag_source(e.ref_addr, class_of_ref_value(e.value), "elem[" + e.name + "]");
                ImGui::TableSetColumnIndex(2);
                ImGui::PushID(uid++);
                const int idx{ std::atoi(e.name.c_str()) };
                const bool is_ref{ desc_is_ref(g_array_elemdesc) };
                if (ui::IconButton(ICON_FA_PEN, "edit"))
                {
                    std::snprintf(g_edit_buf, sizeof(g_edit_buf), "%s", writeable_value(g_array_elemdesc, e.value, e.ref_addr).c_str());
                    ImGui::OpenPopup("editelem");
                }
                ImGui::SetItemTooltip("Edit element");
                if (!e.ref_addr.empty())
                {
                    ImGui::SameLine(0.0f, em(0.1f));
                    if (ui::IconButton(ICON_FA_THUMBTACK, "grab"))
                        add_saved_object(app, "elem[" + e.name + "]", class_of_ref_value(e.value), e.ref_addr);
                    ImGui::SetItemTooltip("Grab this element into the clipboard");
                }
                if (ImGui::BeginPopup("editelem"))
                {
                    ImGui::TextDisabled("[%d] : %s", idx, g_array_elemdesc.c_str());
                    ImGui::Separator();
                    const auto set_elem{ [&](const std::string& v) { app.set_array_element(g_array_addr, g_array_elemdesc, idx, v); g_array_refresh = true; } };
                    if (is_ref)
                    {
                        for (int ci = 0; ci < (int)g_clipboard.size(); ++ci)
                        {
                            const viewer::SavedObject& so{ g_clipboard[(std::size_t)ci] };
                            ImGui::PushID(ci);
                            if (ImGui::Selectable((so.label + "   " + so.address).c_str())) { set_elem(so.address); ImGui::CloseCurrentPopup(); }
                            ImGui::PopID();
                        }
                        if (!g_clipboard.empty()) ImGui::Separator();
                        if (ImGui::SmallButton("Set null")) { set_elem("null"); ImGui::CloseCurrentPopup(); }
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(em(14.0f));
                        const bool ent{ ImGui::InputText("##ee", g_edit_buf, sizeof(g_edit_buf), ImGuiInputTextFlags_EnterReturnsTrue) };
                        ImGui::SameLine();
                        if (ImGui::SmallButton(g_array_elemdesc == "Ljava/lang/String;" ? "Set text" : "Set 0x") || ent) { set_elem(g_edit_buf); ImGui::CloseCurrentPopup(); }
                    }
                    else if (g_array_elemdesc == "Z")
                    {
                        if (ImGui::SmallButton("true"))  { set_elem("true");  ImGui::CloseCurrentPopup(); }
                        ImGui::SameLine();
                        if (ImGui::SmallButton("false")) { set_elem("false"); ImGui::CloseCurrentPopup(); }
                    }
                    else
                    {
                        ImGui::SetNextItemWidth(em(12.0f));
                        const bool ent{ ImGui::InputText("##ee", g_edit_buf, sizeof(g_edit_buf), ImGuiInputTextFlags_EnterReturnsTrue) };
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Set") || ent) { set_elem(g_edit_buf); ImGui::CloseCurrentPopup(); }
                    }
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::End();
    }

    // ── Generate Wrapper + Scripts ────────────────────────────────────────────

    inline std::string vmhook_include_dir() { return exe_dir(); }  // CMake copies vmhook/ next to the exe
    inline std::string default_wrapper_dir() { return exe_dir() + "generated"; }

    wrapper::NameCase case_from_index(int v)
    {
        switch (v)
        {
        case 0:  return wrapper::NameCase::Original;
        case 1:  return wrapper::NameCase::Snake;
        case 2:  return wrapper::NameCase::Camel;
        default: return wrapper::NameCase::Pascal;
        }
    }

    wrapper::Options current_wrapper_options()
    {
        wrapper::Options o;
        o.root_namespace  = g_w_ns[0] ? std::string{ g_w_ns } : std::string{ "jvm" };
        o.ns_layout       = g_w_layout == 1 ? wrapper::NsLayout::Flat : wrapper::NsLayout::Nested;
        o.type_case       = case_from_index(g_w_type_case);
        o.member_case     = case_from_index(g_w_member_case);
        o.getter_prefix   = g_w_getter;
        o.setter_prefix   = g_w_setter;
        o.emit_setters    = g_w_setters;
        o.include_methods = g_w_methods;
        o.include_fields  = g_w_fields;
        o.include_jdk     = g_w_jdk;
        o.public_only     = g_w_public_only;
        o.include_prefixes = g_w_include;
        o.exclude_prefixes = g_w_exclude;
        return o;
    }

    void start_wrapper_generation(viewer::App& app)
    {
        if (g_w_state.load() == 1) return;
        auto classes{ std::make_shared<std::vector<viewer::ClassInfo>>() };
        { std::lock_guard<std::mutex> lock{ app.data_mutex }; *classes = app.classes; }
        const wrapper::Options opt{ current_wrapper_options() };
        const std::string outdir{ g_w_outdir[0] ? std::string{ g_w_outdir } : default_wrapper_dir() };
        if (g_w_thread.joinable()) g_w_thread.join();
        g_w_msg.clear(); g_w_path.clear(); g_w_notes.clear();
        g_w_state.store(1);
        g_w_thread = std::thread([classes, opt, outdir]
        {
            const wrapper::Result r{ wrapper::generate(*classes, opt) };
            CreateDirectoryA(outdir.c_str(), nullptr);
            const std::string path{ outdir + "\\wrapper.hpp" };
            bool ok{ false };
            { std::ofstream f{ path, std::ios::trunc }; f << r.header; ok = static_cast<bool>(f); }
            g_w_path = ok ? path : std::string{};
            g_w_msg  = ok
                ? (std::to_string(r.stats.classes_emitted) + " classes, "
                   + std::to_string(r.stats.methods_emitted) + " methods, "
                   + std::to_string(r.stats.fields_emitted) + " fields  ("
                   + std::to_string(r.header.size() / 1024) + " KB, "
                   + std::to_string(r.stats.classes_skipped) + " classes filtered out).")
                : ("Could not write " + path + " — is the folder writable?");
            g_w_state.store(ok ? 2 : 3);
        });
    }

    void reseed_editor();  // defined with the Scripts editor below; used by "Use in a script"

    void draw_wrapper_window(viewer::App& app)
    {
        ImGui::SetNextWindowSize(ImVec2(em(36.0f), em(38.0f)), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin(ICON_FA_CODE "  Generate Wrapper", &g_show_wrapper))
        {
            ImGui::End();
            return;
        }

        ImGui::TextWrapped("Turn the attached JVM's classes into a compile-ready C++ wrapper "
                           "(vmhook object<> style). Choose how names and namespaces are formed.");
        ImGui::Spacing();

        const float lw{ em(10.0f) };
        static const char* const cases[]{ "Original", "snake_case", "camelCase", "PascalCase" };

        row_label("Root namespace"); ImGui::SameLine(lw); ui::InputText("##wns", "jvm", g_w_ns, sizeof g_w_ns, em(12.0f));
        row_label("Namespaces");     ImGui::SameLine(lw); ui::Combo("##wlay", &g_w_layout, "Nested (mirror packages)\0Flat (one namespace)\0", em(18.0f));
        ImGui::SetItemTooltip("Nested mirrors Java packages as nested C++ namespaces (jvm::net::minecraft::client::Minecraft).\nFlat puts every class in one namespace and disambiguates leaf collisions.");
        row_label("Class names");    ImGui::SameLine(lw); ui::Combo("##wtc", &g_w_type_case, cases, 4, em(12.0f));
        row_label("Member names");   ImGui::SameLine(lw); ui::Combo("##wmc", &g_w_member_case, cases, 4, em(12.0f));
        ImGui::SetItemTooltip("Applied to method accessors and field getter/setter names.");
        row_label("Getter prefix");  ImGui::SameLine(lw); ui::InputText("##wg", "get_", g_w_getter, sizeof g_w_getter, em(6.0f));
        ImGui::SameLine(0.0f, em(0.8f)); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Setter");
        ImGui::SameLine(0.0f, em(0.4f)); ui::InputText("##ws", "set_", g_w_setter, sizeof g_w_setter, em(6.0f));

        ImGui::Spacing();
        ui::Toggle("Setters", &g_w_setters);      ImGui::SameLine(em(9.0f));
        ui::Toggle("Methods", &g_w_methods);      ImGui::SameLine(em(18.0f));
        ui::Toggle("Fields",  &g_w_fields);
        ui::Toggle("Public only", &g_w_public_only); ImGui::SameLine(em(9.0f));
        ui::Toggle("Include JDK", &g_w_jdk);
        ImGui::SetItemTooltip("Include java/*, javax/*, sun/*, jdk/* classes. The full JDK is large and slow to compile — leave off to wrap just the app's own classes.");

        ImGui::Spacing();
        row_label("Include only"); ImGui::SameLine(lw); ui::InputText("##winc", "all packages (e.g. net/minecraft, com/example)", g_w_include, sizeof g_w_include, -1.0f);
        row_label("Exclude");      ImGui::SameLine(lw); ui::InputText("##wexc", "none (e.g. com/sun, org/objectweb)", g_w_exclude, sizeof g_w_exclude, -1.0f);
        row_label("Output folder"); ImGui::SameLine(lw); ui::InputText("##wout", default_wrapper_dir().c_str(), g_w_outdir, sizeof g_w_outdir, -1.0f);

        // Live count of classes that pass the current filter.
        int matched{ 0 }, total{ 0 };
        {
            const wrapper::Options opt{ current_wrapper_options() };
            std::lock_guard<std::mutex> lock{ app.data_mutex };
            total = static_cast<int>(app.classes.size());
            for (const auto& c : app.classes)
                if (!c.internal_name.empty() && wrapper::detail::passes_filter(c.internal_name, opt)) ++matched;
        }
        ImGui::Spacing();
        ImGui::TextDisabled("%d of %d classes match the filter.", matched, total);
        if (matched > 4000)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::warning());
            ImGui::TextWrapped("Heads up: wrapping this many classes makes a large header that is slow to compile. Consider an 'Include only' filter.");
            ImGui::PopStyleColor();
        }

        ImGui::Separator();
        const int st{ g_w_state.load() };
        ImGui::BeginDisabled(st == 1 || matched == 0);
        if (ui::Button(ICON_FA_CODE "  Generate", ImVec2(em(10.0f), 0), ui::BtnPrimary)) start_wrapper_generation(app);
        ImGui::EndDisabled();
        if (st == 1)
        {
            ImGui::SameLine(0.0f, em(0.6f));
            ui::Spinner("##wspin", ImGui::GetFrameHeight() * 0.32f, em(0.14f), ImGui::GetColorU32(theme::accent()));
            ImGui::SameLine(0.0f, em(0.5f)); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Generating...");
        }
        else if (st == 2)
        {
            if (g_w_thread.joinable()) g_w_thread.join();
            ImGui::PushStyleColor(ImGuiCol_Text, theme::success());
            ImGui::TextWrapped("%s", g_w_msg.c_str());
            ImGui::PopStyleColor();
            if (!g_w_path.empty())
            {
                ImGui::TextDisabled("Wrote %s", g_w_path.c_str());
                if (ui::Button("Copy path")) ImGui::SetClipboardText(g_w_path.c_str());
                ImGui::SameLine();
                if (ui::Button(ICON_FA_SCROLL "  Use in a script"))
                {
                    std::snprintf(g_script_hdr, sizeof g_script_hdr, "%s", g_w_path.c_str());
                    reseed_editor();  // re-seed the editor with this header's include + reindex
                    g_show_scripts = true;
                }
            }
        }
        else if (st == 3)
        {
            if (g_w_thread.joinable()) g_w_thread.join();
            ImGui::PushStyleColor(ImGuiCol_Text, theme::danger());
            ImGui::TextWrapped("%s", g_w_msg.c_str());
            ImGui::PopStyleColor();
        }

        ImGui::End();
    }

    std::string script_starter_for(const std::string& header_path)
    {
        std::string inc;
        if (!header_path.empty())
        {
            std::string base{ header_path };
            const std::size_t slash{ base.find_last_of("\\/") };
            if (slash != std::string::npos) base = base.substr(slash + 1);
            inc = "#include \"" + base + "\"";
        }
        std::string example;
        example += "    // Example (uncomment + edit): hook a method and log each call.\n";
        example += "    // Its detour runs on the real Java thread, so calling Java here is safe.\n";
        example += "    // vmhook::hook<jvm::net::example::Player>(\"getHealth\", \"()I\",\n";
        example += "    //     [](vmhook::return_value& ret, std::unique_ptr<jvm::net::example::Player> self)\n";
        example += "    //     {\n";
        example += "    //         script::log(\"getHealth() called\");\n";
        example += "    //         // ret.set(999); // force a return value\n";
        example += "    //     });\n";
        return script_host::starter_body(inc, example);
    }

    // A dark editor palette tuned to the Tokyo Night theme.
    const TextEditor::Palette& editor_palette()
    {
        using PI = TextEditor::PaletteIndex;
        static TextEditor::Palette pal{};
        static bool init{ false };
        if (!init)
        {
            const auto u32{ [](const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); } };
            pal = TextEditor::GetDarkPalette();  // sensible base
            pal[(int)PI::Default]        = u32(theme::text());
            pal[(int)PI::Keyword]        = u32(theme::accent_hi());
            pal[(int)PI::Number]         = u32(theme::orange());
            pal[(int)PI::String]         = u32(theme::green());
            pal[(int)PI::CharLiteral]    = u32(theme::green());
            pal[(int)PI::Punctuation]    = u32(theme::text());
            pal[(int)PI::Preprocessor]   = u32(theme::purple());
            pal[(int)PI::Identifier]     = u32(theme::text());
            pal[(int)PI::KnownIdentifier]= u32(theme::cyan());
            pal[(int)PI::Comment]        = u32(theme::muted());
            pal[(int)PI::MultiLineComment]= u32(theme::muted());
            pal[(int)PI::Background]      = u32(ImVec4(0.086f, 0.086f, 0.118f, 1.0f));  // #16161e
            pal[(int)PI::Cursor]         = u32(theme::accent_hi());
            pal[(int)PI::LineNumber]     = u32(theme::muted_dim());
            pal[(int)PI::CurrentLineFill]= u32(ImVec4(1, 1, 1, 0.03f));
            pal[(int)PI::CurrentLineFillInactive] = u32(ImVec4(1, 1, 1, 0.015f));
            pal[(int)PI::CurrentLineEdge]= u32(ImVec4(0, 0, 0, 0));
            init = true;
        }
        return pal;
    }

    void reseed_editor()
    {
        const std::string starter{ script_starter_for(g_script_hdr[0] ? std::string{ g_script_hdr } : g_w_path) };
        g_editor.SetLanguageDefinition(TextEditor::LanguageDefinition::CPlusPlus());
        g_editor.SetPalette(editor_palette());
        g_editor.SetShowWhitespaces(false);
        g_editor.SetText(starter);
        g_editor_seeded = true;
        g_cmpl_reindex = true;
    }

    void seed_editor_if_empty()
    {
        if (!g_editor_seeded) reseed_editor();
    }

    // Parse cl.exe diagnostics ("vmhook_script(N): error ...") into 1-based line
    // markers.  compose_source() emits `#line 1 "vmhook_script"` before the user
    // body, so N is already the editor line.
    TextEditor::ErrorMarkers build_error_markers(const std::string& log)
    {
        TextEditor::ErrorMarkers markers;
        std::size_t p{ 0 };
        while (p < log.size())
        {
            const std::size_t nl{ log.find('\n', p) };
            const std::string_view line{ log.data() + p, (nl == std::string::npos ? log.size() : nl) - p };
            p = (nl == std::string::npos ? log.size() : nl + 1);
            const std::size_t k{ line.find("vmhook_script(") };
            if (k == std::string_view::npos) continue;
            std::size_t o{ k + 14 };
            int ln{ 0 }; bool any{ false };
            while (o < line.size() && std::isdigit(static_cast<unsigned char>(line[o]))) { ln = ln * 10 + (line[o] - '0'); ++o; any = true; }
            if (!any || ln <= 0) continue;
            const std::size_t mp{ line.find("): ", o) };
            std::string msg{ mp != std::string_view::npos ? std::string{ line.substr(mp + 3) } : std::string{ line.substr(o) } };
            auto it{ markers.find(ln) };
            if (it == markers.end()) markers[ln] = msg;
            else if (it->second.size() < 400) it->second += "\n" + msg;
        }
        return markers;
    }

    void start_script_build(viewer::App& app, bool inject)
    {
        if (g_build_state.load() == 1) return;
        std::string body{ g_editor.GetText() };
        std::string wrapper_dir;
        if (g_script_hdr[0])
        {
            std::string h{ g_script_hdr };
            const std::size_t slash{ h.find_last_of("\\/") };
            wrapper_dir = (slash != std::string::npos) ? h.substr(0, slash) : default_wrapper_dir();
        }
        else
        {
            wrapper_dir = default_wrapper_dir();
        }
        const std::string work{ exe_dir() + "script_build" };
        const std::string vmh{ vmhook_include_dir() };
        if (g_build_thread.joinable()) g_build_thread.join();
        g_build_log.clear(); g_build_dll.clear();
        g_build_inject = inject;
        g_build_state.store(1);
        g_build_thread = std::thread([body, work, vmh, wrapper_dir]
        {
            const script_host::BuildResult r{ script_host::build(body, work, vmh, wrapper_dir) };
            g_build_log = r.log;
            g_build_dll = r.dll_path;
            g_build_state.store(r.ok ? 2 : 3);
        });
    }

    // Replace the caret's partial identifier with `chosen`.
    void insert_completion(const TextEditor::Coordinates& caret, int token_len, const std::string& chosen)
    {
        g_editor.SetSelection(TextEditor::Coordinates(caret.mLine, (std::max)(0, caret.mColumn - token_len)), caret);
        g_editor.Delete();  // removes the current selection (DeleteSelection is private)
        g_editor.InsertText(chosen);
        g_cmpl_open = false;
    }

    // The syntax-highlighted code editor + an as-you-type completion popup built
    // from the generated wrapper's symbols (namespaces / classes / accessors) plus
    // the C++ / vmhook API.  Nav/accept keys are locked away from the editor for
    // the frame so Up/Down/Tab/Enter drive the popup, not the caret.
    void draw_code_editor(float height, bool read_only)
    {
        if (g_cmpl_reindex)
        {
            g_cmpl_reindex = false;
            const std::string path{ g_script_hdr[0] ? std::string{ g_script_hdr } : g_w_path };
            std::string hdr;
            if (!path.empty()) { std::ifstream f{ path, std::ios::binary }; std::stringstream ss; ss << f.rdbuf(); hdr = ss.str(); }
            g_complete_index = complete::build_from_header(hdr);
        }

        g_editor.SetReadOnly(read_only);

        // Current identifier token at the caret (computed before Render).
        const TextEditor::Coordinates cur{ g_editor.GetCursorPosition() };
        const std::string lineText{ g_editor.GetCurrentLineText() };
        const auto col_to_index{ [](const std::string& s, int col) -> int
        {
            int rc{ 0 }, i{ 0 };
            for (; i < static_cast<int>(s.size()) && rc < col; ++i) rc += (s[i] == '\t') ? (4 - (rc % 4)) : 1;
            return i;
        } };
        const int caret_i{ col_to_index(lineText, cur.mColumn) };
        int tok_start{ caret_i };
        while (tok_start > 0 && (std::isalnum(static_cast<unsigned char>(lineText[tok_start - 1])) || lineText[tok_start - 1] == '_')) --tok_start;
        const std::string token{ lineText.substr(tok_start, caret_i - tok_start) };

        if (!read_only && token.size() >= 2 && (std::isalpha(static_cast<unsigned char>(token[0])) || token[0] == '_'))
        {
            if (token != g_cmpl_token || !g_cmpl_open)
            {
                g_cmpl_token = token;
                g_cmpl_items = complete::filter(g_complete_index, token, 40);
                g_cmpl_sel = 0;
            }
            g_cmpl_open = !g_cmpl_items.empty();
        }
        else g_cmpl_open = false;

        bool accept{ false };
        if (g_cmpl_open)
        {
            const ImGuiID owner{ ImGui::GetID("##cmpl") };
            const int n{ static_cast<int>(g_cmpl_items.size()) };
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) { g_cmpl_sel = (g_cmpl_sel + 1) % n; ImGui::SetKeyOwner(ImGuiKey_DownArrow, owner, ImGuiInputFlags_LockThisFrame); }
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true))   { g_cmpl_sel = (g_cmpl_sel - 1 + n) % n; ImGui::SetKeyOwner(ImGuiKey_UpArrow, owner, ImGuiInputFlags_LockThisFrame); }
            if (ImGui::IsKeyPressed(ImGuiKey_Tab, false) || ImGui::IsKeyPressed(ImGuiKey_Enter, false))
            {
                accept = true;
                ImGui::SetKeyOwner(ImGuiKey_Tab, owner, ImGuiInputFlags_LockThisFrame);
                ImGui::SetKeyOwner(ImGuiKey_Enter, owner, ImGuiInputFlags_LockThisFrame);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) g_cmpl_open = false;
        }
        if (accept && g_cmpl_sel >= 0 && g_cmpl_sel < static_cast<int>(g_cmpl_items.size()))
            insert_completion(cur, static_cast<int>(token.size()), g_complete_index.all[g_cmpl_items[g_cmpl_sel]].name);

        // Render the editor in the monospace font.
        const char* const title{ "##scripteditor" };
        if (g_font_mono) ImGui::PushFont(g_font_mono, 0.0f);
        const float charW{ ImGui::CalcTextSize("m").x };
        const float lineH{ ImGui::GetTextLineHeightWithSpacing() };
        g_editor.Render(title, ImVec2(-1.0f, height), true);
        if (g_font_mono) ImGui::PopFont();

        // Completion popup near the caret.
        if (g_cmpl_open && !g_cmpl_items.empty())
        {
            ImVec2 pos{ ImGui::GetItemRectMin() };
            if (ImGuiWindow* ew{ ImGui::FindWindowByName(title) })
            {
                const int digits{ (std::max)(2, static_cast<int>(std::to_string((std::max)(1, g_editor.GetTotalLines())).size())) };
                const float gutter{ (digits + 2) * charW };
                pos = ImVec2(ew->Pos.x + gutter + cur.mColumn * charW - ew->Scroll.x,
                             ew->Pos.y + (cur.mLine + 1) * lineH - ew->Scroll.y + em(0.2f));
            }
            const ImGuiViewport* vp{ ImGui::GetMainViewport() };
            pos.x = std::clamp(pos.x, vp->WorkPos.x, vp->WorkPos.x + vp->WorkSize.x - em(20.0f));
            pos.y = std::clamp(pos.y, vp->WorkPos.y, vp->WorkPos.y + vp->WorkSize.y - em(12.0f));
            ImGui::SetNextWindowPos(pos);
            ImGui::SetNextWindowSize(ImVec2(em(24.0f), 0.0f));
            ImGui::SetNextWindowBgAlpha(0.98f);
            const ImGuiWindowFlags fl{ ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize };
            if (ImGui::Begin("##cmplpopup", nullptr, fl))
            {
                const int shown{ (std::min)(12, static_cast<int>(g_cmpl_items.size())) };
                for (int i = 0; i < shown; ++i)
                {
                    const complete::Symbol& sym{ g_complete_index.all[g_cmpl_items[i]] };
                    ImGui::PushID(i);
                    if (ImGui::Selectable("##ci", i == g_cmpl_sel, ImGuiSelectableFlags_AllowOverlap))
                        insert_completion(cur, static_cast<int>(token.size()), sym.name);
                    ImGui::SameLine(em(0.4f));
                    ImGui::TextUnformatted(sym.name.c_str());
                    if (!sym.detail.empty())
                    {
                        ImGui::SameLine(em(12.0f));
                        ImGui::PushStyleColor(ImGuiCol_Text, theme::muted());
                        ImGui::TextUnformatted(sym.detail.c_str());
                        ImGui::PopStyleColor();
                    }
                    ImGui::PopID();
                }
                if (static_cast<int>(g_cmpl_items.size()) > shown)
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, theme::muted_dim());
                    ImGui::Text("   ... %d more", static_cast<int>(g_cmpl_items.size()) - shown);
                    ImGui::PopStyleColor();
                }
            }
            ImGui::End();
        }
    }

    void draw_scripts_window(viewer::App& app)
    {
        ImGui::SetNextWindowSize(ImVec2(em(54.0f), em(42.0f)), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin(ICON_FA_SCROLL "  Scripts", &g_show_scripts))
        {
            ImGui::End();
            return;
        }
        seed_editor_if_empty();

        ImGui::TextWrapped("Write a C++ script over the generated wrapper. It compiles to a DLL "
                           "that is injected into the attached JVM; install hooks in script_setup().");
        ImGui::Spacing();

        row_label("Wrapper header"); ImGui::SameLine(em(9.0f));
        ui::InputText("##shdr", "path to the generated wrapper.hpp (optional)", g_script_hdr, sizeof g_script_hdr, em(28.0f));
        ImGui::SameLine();
        ImGui::BeginDisabled(g_w_path.empty());
        if (ui::Button("Use last")) { std::snprintf(g_script_hdr, sizeof g_script_hdr, "%s", g_w_path.c_str()); g_cmpl_reindex = true; }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ui::Button("Reset template")) reseed_editor();

        const int bst{ g_build_state.load() };
        const bool building{ bst == 1 };

        // Editor (syntax-highlighted, line numbers, error markers, completion).
        const float log_h{ em(11.0f) };
        const float editor_h{ (std::max)(em(8.0f), ImGui::GetContentRegionAvail().y - log_h - em(4.0f)) };
        draw_code_editor(editor_h, building);

        // Build / inject controls.
        const bool can_inject{ app.selected_jvm >= 0 && app.selected_jvm < (int)app.jvms.size() };
        ImGui::BeginDisabled(building);
        if (ui::Button(ICON_FA_WRENCH "  Compile", ImVec2(0, 0), ui::BtnPrimary)) start_script_build(app, /*inject=*/false);
        ImGui::SameLine();
        ImGui::BeginDisabled(!can_inject);
        if (ui::Button(ICON_FA_PLAY "  Compile & Inject", ImVec2(0, 0), ui::BtnPrimary)) start_script_build(app, /*inject=*/true);
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        if (building)
        {
            ImGui::SameLine(0.0f, em(0.6f));
            ui::Spinner("##bspin", ImGui::GetFrameHeight() * 0.32f, em(0.14f), ImGui::GetColorU32(theme::accent()));
            ImGui::SameLine(0.0f, em(0.5f)); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Compiling (first build takes a while)...");
        }
        ImGui::SameLine();
        ImGui::TextDisabled("|"); ImGui::SameLine();
        if (ui::Button("Clear log")) { g_script_log_text.clear(); std::ofstream{ script_host::log_path(), std::ios::trunc }; }

        // Compiler output (on failure) + the script's live log.
        ImGui::Separator();
        if (bst == 3 && g_build_thread.joinable()) g_build_thread.join();
        if (bst == 2 && g_build_thread.joinable()) g_build_thread.join();

        // On a build completing, mark (or clear) the editor's error lines once.
        static int prev_bst{ 0 };
        if (bst != prev_bst)
        {
            prev_bst = bst;
            if (bst == 3)      g_editor.SetErrorMarkers(build_error_markers(g_build_log));
            else if (bst == 2) g_editor.SetErrorMarkers(TextEditor::ErrorMarkers{});
        }

        if (ImGui::BeginTabBar("scripttabs"))
        {
            if (ImGui::BeginTabItem("Script log"))
            {
                ImGui::BeginChild("slog", ImVec2(0, 0), ImGuiChildFlags_Borders);
                ImGui::TextUnformatted(g_script_log_text.empty() ? "(no output yet)" : g_script_log_text.c_str());
                if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - em(2.0f)) ImGui::SetScrollHereY(1.0f);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Compiler output"))
            {
                ImGui::BeginChild("clog", ImVec2(0, 0), ImGuiChildFlags_Borders);
                if (bst == 3)
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, theme::danger());
                    ImGui::TextUnformatted("Build failed:");
                    ImGui::PopStyleColor();
                }
                else if (bst == 2)
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, theme::success());
                    ImGui::TextUnformatted("Build succeeded.");
                    ImGui::PopStyleColor();
                }
                ImGui::TextUnformatted(g_build_log.empty() ? "(no build yet)" : g_build_log.c_str());
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        ImGui::End();
    }

    // After a successful build, inject the DLL into the selected JVM (once).
    void maybe_inject_script(viewer::App& app)
    {
        if (g_build_state.load() != 2 || !g_build_inject || g_build_dll.empty()) return;
        g_build_inject = false;
        if (app.selected_jvm < 0 || app.selected_jvm >= (int)app.jvms.size())
        {
            g_script_log_text += "\n[viewer] No JVM selected — cannot inject.\n";
            return;
        }
        const std::uint32_t pid{ app.jvms[(std::size_t)app.selected_jvm].pid };
        std::wstring wpath(g_build_dll.size() + 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, g_build_dll.c_str(), -1, wpath.data(), (int)wpath.size());
        wpath.resize(wcslen(wpath.c_str()));
        std::string err;
        if (viewer::inject_dll(pid, wpath, err))
            g_script_log_text += "\n[viewer] Injected script into pid " + std::to_string(pid) + ".\n";
        else
            g_script_log_text += "\n[viewer] Injection failed: " + err + "\n";
    }

    // Poll the script log file (written by the injected DLL) ~4x/sec.
    void poll_script_log()
    {
        if (ImGui::GetTime() - g_script_log_poll < 0.25) return;
        g_script_log_poll = ImGui::GetTime();
        std::ifstream f{ script_host::log_path(), std::ios::binary };
        if (!f) return;
        std::stringstream ss; ss << f.rdbuf();
        g_script_log_text = ss.str();
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
        // manual Refresh (selection is preserved by pid).  The enumeration runs on
        // a background thread — it's too heavy (a module snapshot per process +
        // EnumWindows per JVM) to block the render thread with — and its result is
        // swapped in here when ready.
        app.apply_refreshed_jvms();
        static double last_refresh{ 0.0 };
        if ((ImGui::GetTime() - last_refresh) > 2.0)
        {
            app.refresh_jvms_async();
            last_refresh = ImGui::GetTime();
        }

        // Live class-load tracking: purely hook-driven — poll the on_class_loaded
        // hook ~every second and append just the classes it captured (with their
        // full surface).  No full re-enumeration/diff; a class shows up the moment
        // ClassLoader.defineClass defines it.  (A manual Rescan still does the full
        // list + diff, which additionally catches unloads and bootstrap classes.)
        static double last_poll{ 0.0 };
        if (g_auto_rescan && app.has_baseline.load() && !app.any_busy() &&
            (ImGui::GetTime() - last_poll) > 1.0)
        {
            app.auto_track();
            last_poll = ImGui::GetTime();
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

        // main split: left classes / right details, with a draggable splitter.
        // Reserve a strip at the bottom for the object clipboard when it's shown.
        const float clip_h{ g_show_clipboard ? em(7.4f) : 0.0f };
        const float avail_y{ ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() - clip_h };
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

        // Object clipboard strip (full width, below the split).
        if (g_show_clipboard) draw_clipboard_panel(app);

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
                ImGui::SetItemTooltip("Classes unloaded since the previous scan — click to list");
            }
            if (app.hook_armed.load())
            {
                ImGui::SameLine(0.0f, em(0.6f));
                ImGui::PushStyleColor(ImGuiCol_Text, theme::link());
                ImGui::Text("\xef\x80\xa1 hook: %llu", (unsigned long long)app.hook_total.load());
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("on_class_loaded hook armed — %llu class(es) defined via ClassLoader.defineClass so far",
                                      (unsigned long long)app.hook_total.load());
            }
            // Frozen-fields indicator: click to review + unfreeze every active freeze.
            if (!g_frozen.empty())
            {
                ImGui::SameLine(0.0f, em(0.6f));
                const ImVec4 ice{ 0.40f, 0.66f, 1.0f, 1.0f };
                ImGui::PushStyleColor(ImGuiCol_Text, ice);
                ImGui::PushStyleColor(ImGuiCol_TextLink, ice);
                char lbl[48];
                std::snprintf(lbl, sizeof(lbl), ICON_FA_LOCK " frozen: %d", (int)g_frozen.size());
                if (ImGui::TextLink(lbl)) g_show_frozen = true;
                ImGui::PopStyleColor(2);
                ImGui::SetItemTooltip("Fields held at a fixed value — click to review / unfreeze");
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
                const ImVec4 mcol{ st == viewer::Status::Error ? theme::danger()
                                 : st == viewer::Status::Done  ? theme::success()
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
            ImGui::SetItemTooltip("Show human-readable signatures/types instead of raw JVM descriptors");
            ImGui::SameLine(0.0f, em(0.9f));
            ImGui::BeginDisabled(!g_pretty);
            ui::Toggle("Full names", &g_full_names);
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("Show fully-qualified type names (java.lang.String) instead of simple ones (String)");
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

        // Frozen-fields overview: every active freeze, with per-row + bulk unfreeze.
        if (g_show_frozen) { ImGui::OpenPopup("Frozen fields"); g_show_frozen = false; }
        if (ImGui::BeginPopup("Frozen fields"))
        {
            ImGui::SeparatorText("Frozen fields");
            if (g_frozen.empty())
                ImGui::TextDisabled("(none)");
            else
            {
                if (ui::Button("Unfreeze all", ImVec2(0, 0), ui::BtnDanger)) { if (app.unfreeze_all()) g_frozen.clear(); }
                ImGui::Spacing();
                ImGui::BeginChild("frzlist", ImVec2(em(34.0f), (std::min)((float)g_frozen.size() + 0.5f, 14.0f) * em(1.7f)));
                if (ImGui::BeginTable("frztbl", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH))
                {
                    ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, em(9.0f));
                    ImGui::TableSetupColumn("",      ImGuiTableColumnFlags_WidthFixed, em(2.4f));
                    std::string remove_key;
                    int uid{ 0 };
                    for (const auto& [key, fz] : g_frozen)
                    {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        std::string label{ dotted_name(cls_short(fz.cls)) + "." + fz.field };
                        if (fz.scope == 'S') label += "  (static)";
                        ImGui::TextUnformatted(label.c_str());
                        if (fz.scope == 'I' && ImGui::IsItemHovered()) ImGui::SetTooltip("%s @ %s", dotted_name(fz.cls).c_str(), fz.addr.c_str());
                        ImGui::TableSetColumnIndex(1);
                        ImGui::PushStyleColor(ImGuiCol_Text, value_color(fz.value));
                        ImGui::TextUnformatted(fz.value.c_str());
                        ImGui::PopStyleColor();
                        ImGui::TableSetColumnIndex(2);
                        ImGui::PushID(uid++);
                        if (ui::IconButton(ICON_FA_UNLOCK, "unf")) remove_key = key;
                        ImGui::SetItemTooltip("Unfreeze");
                        ImGui::PopID();
                    }
                    if (!remove_key.empty())
                    {
                        if (const auto it{ g_frozen.find(remove_key) }; it != g_frozen.end())
                        { const FrozenField fz{ it->second }; ui_unfreeze(app, fz.scope, fz.cls, fz.addr, fz.field); }
                    }
                    ImGui::EndTable();
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
            if (due && !app.any_busy())
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

        // Static fields window — same cadence-after-completion scheduling.
        if (g_show_statics && !g_statics_class.empty())
        {
            if (app.stat_class != g_statics_class) g_statics_refresh_now = true;  // class switched
            static bool   was_busy{ false };
            static double next_scan{ 0.0 };
            const bool now_busy{ app.stat_busy() };
            if (was_busy && !now_busy) next_scan = ImGui::GetTime() + 1.2;
            was_busy = now_busy;

            const bool due{ g_statics_refresh_now || (g_statics_live && ImGui::GetTime() >= next_scan) };
            if (due && !app.any_busy())
            {
                app.request_statics(g_statics_class);
                g_statics_refresh_now = false;
                next_scan = ImGui::GetTime() + 1.0e9;
            }
        }
        if (g_show_statics)
        {
            draw_statics_window(app);
        }

        // Array inspector — fetch on open / Refresh (one-shot; arrays don't auto-poll).
        if (g_show_array && !g_array_addr.empty() && g_array_refresh && !app.any_busy())
        {
            app.request_array(g_array_addr, g_array_elemdesc);
            g_array_refresh = false;
        }
        if (g_show_array)
        {
            draw_array_window(app);
        }

        // Generate Wrapper + Scripts windows (+ their async plumbing).
        if (g_show_wrapper) draw_wrapper_window(app);
        if (g_show_scripts) { poll_script_log(); draw_scripts_window(app); }
        maybe_inject_script(app);

        // Consume a completed mutating op (set / freeze / call) exactly once:
        // route a call result to whichever panel dispatched it, otherwise show a
        // transient toast; a successful write triggers an immediate re-read so the
        // new value surfaces without waiting for the next auto-scan.
        if (const std::uint64_t seq{ app.op_seq.load() }; seq != g_op_seen_seq)
        {
            g_op_seen_seq = seq;
            viewer::OpResult r;
            { std::lock_guard<std::mutex> lock{ app.data_mutex }; r = app.op_result; }
            if (g_call_inst.pending)      { g_call_inst.pending = false; g_call_inst.result = r; g_call_inst.has_result = true; }
            else if (g_call_stat.pending) { g_call_stat.pending = false; g_call_stat.result = r; g_call_stat.has_result = true; }
            else
            {
                g_op_toast = r.ok ? (r.disp.empty() ? std::string{ "Done." } : ("Set -> " + r.disp)) : ("Error: " + r.error);
                g_op_toast_until = ImGui::GetTime() + 3.0;
            }
            if (r.ok) { g_instances_refresh_now = true; g_statics_refresh_now = true; }
        }
        // Floating toast for set / freeze feedback — fades + slides in/out.
        if (!g_op_toast.empty() && ImGui::GetTime() < g_op_toast_until)
        {
            const double remaining{ g_op_toast_until - ImGui::GetTime() };
            const double elapsed{ 3.0 - remaining };
            float a{ 1.0f };
            if (elapsed  < 0.18) a = (std::min)(a, static_cast<float>(elapsed  / 0.18));   // fade in
            if (remaining < 0.35) a = (std::min)(a, static_cast<float>(remaining / 0.35));  // fade out
            a = std::clamp(a, 0.0f, 1.0f);
            a = a * a * (3.0f - 2.0f * a);  // smoothstep
            const float dy{ (1.0f - a) * em(0.9f) };  // slides up as it appears

            const ImGuiViewport* v{ ImGui::GetMainViewport() };
            ImGui::SetNextWindowPos(ImVec2(v->WorkPos.x + v->WorkSize.x * 0.5f, v->WorkPos.y + v->WorkSize.y - em(3.0f) + dy), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
            ImGui::SetNextWindowBgAlpha(0.92f * a);
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, a);
            if (ImGui::Begin("##optoast", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav))
            {
                const bool err{ g_op_toast.rfind("Error", 0) == 0 };
                ImGui::PushStyleColor(ImGuiCol_Text, err ? theme::danger() : theme::success());
                ImGui::TextUnformatted(g_op_toast.c_str());
                ImGui::PopStyleColor();
            }
            ImGui::End();
            ImGui::PopStyleVar();
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
        const float clear[4]{ 0.102f, 0.106f, 0.149f, 1.0f };  // = WindowBg #1a1b26
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
    // Join background workers (wrapper generation / script build) so their
    // std::thread destructors don't std::terminate on a still-joinable thread.
    if (g_w_thread.joinable())     g_w_thread.join();
    if (g_build_thread.joinable()) g_build_thread.join();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    cleanup_device();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}
