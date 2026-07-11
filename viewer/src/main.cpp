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
#include <fstream>
#include <string>
#include <vector>

#include "app.hpp"
#include "descriptor.hpp"

#pragma comment(lib, "d3d11.lib")

// ── DirectX 11 plumbing (standard ImGui example boilerplate) ──────────────────
namespace
{
    ID3D11Device*           g_device{ nullptr };
    ID3D11DeviceContext*    g_context{ nullptr };
    IDXGISwapChain*         g_swap_chain{ nullptr };
    ID3D11RenderTargetView* g_rtv{ nullptr };

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
        s.WindowRounding = 6.0f; s.ChildRounding = 6.0f; s.FrameRounding = 5.0f;
        s.PopupRounding = 5.0f; s.GrabRounding = 5.0f; s.ScrollbarRounding = 9.0f; s.TabRounding = 5.0f;
        s.WindowPadding = ImVec2(12, 12); s.FramePadding = ImVec2(9, 6);
        s.ItemSpacing = ImVec2(8, 7); s.ItemInnerSpacing = ImVec2(7, 5); s.CellPadding = ImVec2(7, 4);
        s.ScrollbarSize = 13.0f; s.GrabMinSize = 11.0f;
        s.WindowBorderSize = 0.0f; s.FrameBorderSize = 0.0f; s.ChildBorderSize = 1.0f; s.PopupBorderSize = 1.0f;

        ImVec4* c{ s.Colors };
        const ImVec4 accent{ 0.24f, 0.52f, 0.92f, 1.0f };
        c[ImGuiCol_Text]                 = ImVec4(0.92f, 0.93f, 0.95f, 1.00f);
        c[ImGuiCol_TextDisabled]         = ImVec4(0.48f, 0.50f, 0.55f, 1.00f);
        c[ImGuiCol_WindowBg]             = ImVec4(0.075f, 0.078f, 0.094f, 1.00f);
        c[ImGuiCol_ChildBg]              = ImVec4(0.105f, 0.110f, 0.130f, 1.00f);
        c[ImGuiCol_PopupBg]              = ImVec4(0.11f, 0.115f, 0.135f, 0.98f);
        c[ImGuiCol_Border]               = ImVec4(1.00f, 1.00f, 1.00f, 0.075f);
        c[ImGuiCol_FrameBg]              = ImVec4(0.16f, 0.17f, 0.20f, 1.00f);
        c[ImGuiCol_FrameBgHovered]       = ImVec4(0.21f, 0.22f, 0.26f, 1.00f);
        c[ImGuiCol_FrameBgActive]        = ImVec4(0.24f, 0.26f, 0.31f, 1.00f);
        c[ImGuiCol_TitleBg]              = ImVec4(0.08f, 0.09f, 0.11f, 1.00f);
        c[ImGuiCol_TitleBgActive]        = ImVec4(0.10f, 0.14f, 0.24f, 1.00f);
        c[ImGuiCol_MenuBarBg]            = ImVec4(0.11f, 0.12f, 0.14f, 1.00f);
        c[ImGuiCol_ScrollbarBg]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
        c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.30f, 0.32f, 0.38f, 1.00f);
        c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.38f, 0.40f, 0.47f, 1.00f);
        c[ImGuiCol_CheckMark]            = accent;
        c[ImGuiCol_SliderGrab]           = accent;
        c[ImGuiCol_Button]               = ImVec4(0.20f, 0.34f, 0.58f, 0.70f);
        c[ImGuiCol_ButtonHovered]        = ImVec4(0.24f, 0.44f, 0.76f, 0.95f);
        c[ImGuiCol_ButtonActive]         = accent;
        c[ImGuiCol_Header]               = ImVec4(0.22f, 0.40f, 0.70f, 0.55f);
        c[ImGuiCol_HeaderHovered]        = ImVec4(0.24f, 0.44f, 0.78f, 0.70f);
        c[ImGuiCol_HeaderActive]         = ImVec4(0.26f, 0.50f, 0.86f, 0.90f);
        c[ImGuiCol_Separator]            = ImVec4(1.00f, 1.00f, 1.00f, 0.08f);
        c[ImGuiCol_TableHeaderBg]        = ImVec4(0.145f, 0.155f, 0.185f, 1.00f);
        c[ImGuiCol_TableBorderStrong]    = ImVec4(1.00f, 1.00f, 1.00f, 0.10f);
        c[ImGuiCol_TableBorderLight]     = ImVec4(1.00f, 1.00f, 1.00f, 0.05f);
        c[ImGuiCol_TableRowBg]           = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
        c[ImGuiCol_TableRowBgAlt]        = ImVec4(1.00f, 1.00f, 1.00f, 0.022f);
        c[ImGuiCol_NavHighlight]         = accent;
    }

    void load_fonts()
    {
        ImGuiIO& io{ ImGui::GetIO() };
        io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 18.0f);
        if (io.Fonts->Fonts.empty())
        {
            io.Fonts->AddFontDefault();
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
    float        g_left_width{ 420.0f };
    bool         g_pretty{ true };
    bool         g_full_names{ false };
    bool         g_focus_search{ false };
    std::wstring g_dll_path{};
    std::vector<int> g_filtered;  // rebuilt each frame from the search box

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
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::Text("%s %s", "\xE2\x97\x8f", text);  // ● dot
        ImGui::PopStyleColor();
    }

    ImVec4 vis_color(std::uint16_t f)
    {
        if (f & 0x0001u) return ImVec4(0.52f, 0.82f, 0.56f, 1.0f);  // public
        if (f & 0x0002u) return ImVec4(0.90f, 0.52f, 0.52f, 1.0f);  // private
        if (f & 0x0004u) return ImVec4(0.92f, 0.74f, 0.42f, 1.0f);  // protected
        return ImVec4(0.62f, 0.63f, 0.70f, 1.0f);                   // package-private
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

    void export_class(const viewer::ClassInfo& c)
    {
        std::string path{ exe_dir() + "vmhook_export.txt" };
        std::ofstream out{ path, std::ios::trunc };
        out << "class " << c.internal_name << "\n\nMETHODS (" << c.methods.size() << ")\n";
        for (const auto& m : c.methods) out << "  " << m.name << "  " << m.descriptor << "\n";
        out << "\nFIELDS (" << c.fields.size() << ")\n";
        for (const auto& f : c.fields) out << "  " << (f.is_static ? "static " : "") << f.name << "  " << f.descriptor << "\n";
    }
}

// ── panes ─────────────────────────────────────────────────────────────────────
namespace
{
    void draw_toolbar(viewer::App& app)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.72f, 1.0f, 1.0f));
        ImGui::Text("vmhook");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextDisabled("viewer");
        ImGui::SameLine(); ImGui::TextDisabled("|"); ImGui::SameLine();

        // JVM combo
        ImGui::TextUnformatted("JVM");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(560.0f);
        std::string preview{ "Select a running JVM..." };
        if (app.selected_jvm >= 0 && app.selected_jvm < (int)app.jvms.size())
        {
            const auto& p{ app.jvms[(std::size_t)app.selected_jvm] };
            preview = std::to_string(p.pid) + " — " + (p.command_line.empty() ? p.image_name : p.command_line);
        }
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
        ImGui::SameLine();
        ImGui::BeginDisabled(app.busy());
        if (ImGui::Button("Refresh")) app.refresh_jvms();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(app.busy() || app.selected_jvm < 0);
        if (ImGui::Button("Attach & Enumerate")) { app.attach_selected(g_dll_path); g_selected_class = -1; }
        ImGui::EndDisabled();

        ImGui::SameLine(); ImGui::TextDisabled("|"); ImGui::SameLine();
        status_pill(app.status.load());
        if (app.status.load() == viewer::Status::Receiving)
        {
            ImGui::SameLine();
            ImGui::Text("%llu classes...", (unsigned long long)app.classes_streamed.load());
        }
    }

    void draw_class_list(viewer::App& app)
    {
        // search box (Ctrl+F focuses it)
        if (g_focus_search) { ImGui::SetKeyboardFocusHere(); g_focus_search = false; }
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##search", "Search classes  (Ctrl+F)", g_search, sizeof(g_search));

        std::lock_guard<std::mutex> lock{ app.data_mutex };
        const std::string needle{ g_search };

        g_filtered.clear();
        g_filtered.reserve(app.classes.size());
        for (int i = 0; i < (int)app.classes.size(); ++i)
            if (icontains(app.classes[(std::size_t)i].internal_name, needle))
                g_filtered.push_back(i);

        ImGui::TextDisabled("%d / %zu classes", (int)g_filtered.size(), app.classes.size());

        if (ImGui::BeginTable("classes", 2,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable | ImGuiTableFlags_SortTristate))
        {
            ImGui::TableSetupColumn("Package", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Class", ImGuiTableColumnFlags_WidthFixed, 150.0f);
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
                        g_selected_class = idx;
                        g_method_filter[0] = 0; g_field_filter[0] = 0;
                    }
                    copy_menu("cls", c.internal_name);
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(c.simple_name.c_str());
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
            ImGui::Dummy(ImVec2(0, 40));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f,0.5f,0.55f,1));
            const char* hint{ app.classes.empty()
                ? "Pick a JVM above and click \"Attach & Enumerate\" to load its classes."
                : "Select a class on the left to see its methods and fields." };
            const float w{ ImGui::CalcTextSize(hint).x };
            ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - w) * 0.5f);
            ImGui::TextUnformatted(hint);
            ImGui::PopStyleColor();
            return;
        }
        const viewer::ClassInfo& c{ app.classes[(std::size_t)g_selected_class] };

        // header
        std::string dotted{ c.internal_name };
        for (char& ch : dotted) if (ch == '/') ch = '.';
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f,0.82f,1.0f,1));
        ImGui::TextUnformatted(dotted.c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 168.0f);
        if (ImGui::SmallButton("Copy name")) ImGui::SetClipboardText(c.internal_name.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Export .txt")) export_class(c);
        ImGui::Separator();

        const float half{ ImGui::GetContentRegionAvail().x * 0.5f - 4.0f };

        // Methods
        ImGui::BeginChild("methods", ImVec2(half, 0), ImGuiChildFlags_Borders);
        ImGui::Text("Methods (%zu)", c.methods.size());
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##mf", "filter methods", g_method_filter, sizeof(g_method_filter));
        if (ImGui::BeginTable("mt", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable))
        {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 150.0f);
            ImGui::TableSetupColumn("Access", ImGuiTableColumnFlags_WidthFixed, 118.0f);
            ImGui::TableSetupColumn(g_pretty ? "Signature" : "Descriptor", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();
            const std::string mf{ g_method_filter };
            for (const auto& m : c.methods)
            {
                if (!icontains(m.name, mf) && !icontains(m.descriptor, mf)) continue;
                ImGui::TableNextRow(); ImGui::TableNextColumn();
                ImGui::PushID(&m);
                ImGui::TextUnformatted(m.name.c_str());
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
        ImGui::EndChild();

        ImGui::SameLine();

        // Fields
        ImGui::BeginChild("fields", ImVec2(0, 0), ImGuiChildFlags_Borders);
        ImGui::Text("Fields (%zu)", c.fields.size());
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##ff", "filter fields", g_field_filter, sizeof(g_field_filter));
        if (ImGui::BeginTable("ft", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable))
        {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 140.0f);
            ImGui::TableSetupColumn("Access", ImGuiTableColumnFlags_WidthFixed, 118.0f);
            ImGui::TableSetupColumn(g_pretty ? "Type" : "Descriptor", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();
            const std::string ff{ g_field_filter };
            for (const auto& f : c.fields)
            {
                if (!icontains(f.name, ff) && !icontains(f.descriptor, ff)) continue;
                ImGui::TableNextRow(); ImGui::TableNextColumn();
                ImGui::PushID(&f);
                ImGui::TextUnformatted(f.name.c_str());
                copy_menu("f", f.name, f.descriptor);
                ImGui::TableNextColumn();
                ImGui::PushStyleColor(ImGuiCol_Text, vis_color(f.access));
                ImGui::TextUnformatted(viewer::access_modifiers(f.access, false).c_str());
                ImGui::PopStyleColor();
                ImGui::TableNextColumn();
                const std::string ty{ g_pretty ? viewer::pretty_field(f.descriptor, g_full_names) : f.descriptor };
                ImGui::TextUnformatted(ty.c_str());
                if (g_pretty && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", f.descriptor.c_str());
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::EndChild();
    }

    void render_ui(viewer::App& app)
    {
        // Ctrl+F focuses the class search.
        if (ImGui::IsKeyDown(ImGuiKey_LeftCtrl) && ImGui::IsKeyPressed(ImGuiKey_F, false)) g_focus_search = true;

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
        ImGui::InvisibleButton("splitter", ImVec2(7.0f, avail_y));
        if (ImGui::IsItemActive()) g_left_width += ImGui::GetIO().MouseDelta.x;
        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        g_left_width = std::clamp(g_left_width, 240.0f, vp->WorkSize.x - 360.0f);
        {
            const ImVec2 p0{ ImGui::GetItemRectMin() }, p1{ ImGui::GetItemRectMax() };
            const float x{ (p0.x + p1.x) * 0.5f };
            const ImU32 col{ ImGui::GetColorU32(ImGui::IsItemActive() ? ImGuiCol_ButtonActive : ImGuiCol_Border) };
            ImGui::GetWindowDrawList()->AddLine(ImVec2(x, p0.y + 2), ImVec2(x, p1.y - 2), col, 2.0f);
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
            ImGui::Text("%zu classes  ·  %zu methods  ·  %zu fields", nclasses, nmethods, nfields);
            ImGui::SameLine();
            {
                std::lock_guard<std::mutex> lock{ app.data_mutex };
                ImGui::TextDisabled("   %s", app.status_message.c_str());
            }
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 230.0f);
            ImGui::Checkbox("Pretty signatures", &g_pretty);
            ImGui::SameLine();
            ImGui::BeginDisabled(!g_pretty);
            ImGui::Checkbox("Full names", &g_full_names);
            ImGui::EndDisabled();
        }

        ImGui::End();
    }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
    WNDCLASSEXW wc{ sizeof(wc), CS_CLASSDC, WndProc, 0, 0, GetModuleHandleW(nullptr), nullptr, LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW)), nullptr, nullptr, L"vmhook_viewer", nullptr };
    RegisterClassExW(&wc);
    HWND hwnd{ CreateWindowW(wc.lpszClassName, L"vmhook viewer", WS_OVERLAPPEDWINDOW, 80, 80, 1400, 880, nullptr, nullptr, wc.hInstance, nullptr) };

    if (!create_device(hwnd)) { cleanup_device(); UnregisterClassW(wc.lpszClassName, wc.hInstance); return 1; }
    ShowWindow(hwnd, SW_SHOWMAXIMIZED);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::GetIO().IniFilename = nullptr;
    apply_modern_style();
    load_fonts();
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
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    cleanup_device();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}
