// vmhook viewer — ImGui (Win32 + DirectX 11) front-end.
//
// Lists running HotSpot JVMs, injects the vmhook payload DLL into the chosen
// one, and shows every loaded Java class with its declared methods and fields
// in an MCPMappingViewer-style layout (class list on top, methods + fields
// below).  Everything is discovered dynamically at runtime — no mappings.

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"

#include <d3d11.h>
#include <tchar.h>

#include <algorithm>
#include <cctype>
#include <string>

#include "app.hpp"

#pragma comment(lib, "d3d11.lib")

// ── DirectX 11 device plumbing (standard ImGui example boilerplate) ───────────
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
        if (back_buffer)
        {
            g_device->CreateRenderTargetView(back_buffer, nullptr, &g_rtv);
            back_buffer->Release();
        }
    }

    void cleanup_rtv()
    {
        if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    }

    bool create_device(HWND hwnd)
    {
        DXGI_SWAP_CHAIN_DESC desc{};
        desc.BufferCount                        = 2;
        desc.BufferDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.BufferDesc.RefreshRate.Numerator   = 60;
        desc.BufferDesc.RefreshRate.Denominator = 1;
        desc.BufferUsage                        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.OutputWindow                       = hwnd;
        desc.SampleDesc.Count                   = 1;
        desc.Windowed                           = TRUE;
        desc.SwapEffect                         = DXGI_SWAP_EFFECT_DISCARD;

        UINT flags{ 0 };
        D3D_FEATURE_LEVEL levels[]{ D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
        D3D_FEATURE_LEVEL obtained{};
        if (D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                          levels, 2, D3D11_SDK_VERSION, &desc, &g_swap_chain,
                                          &g_device, &obtained, &g_context) != S_OK)
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
        if (slash != std::wstring::npos)
        {
            path.resize(slash + 1);
        }
        return path + L"vmhook_payload.dll";
    }

    auto icontains(const std::string& haystack, const std::string& needle) -> bool
    {
        if (needle.empty())
        {
            return true;
        }
        const auto it{ std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
            [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); }) };
        return it != haystack.end();
    }
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam))
    {
        return true;
    }
    switch (msg)
    {
    case WM_SIZE:
        if (g_device && wparam != SIZE_MINIMIZED)
        {
            cleanup_rtv();
            g_swap_chain->ResizeBuffers(0, LOWORD(lparam), HIWORD(lparam), DXGI_FORMAT_UNKNOWN, 0);
            create_rtv();
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

// ── UI ────────────────────────────────────────────────────────────────────────
namespace
{
    char        g_search[256]{};
    int         g_selected_class{ -1 };
    std::wstring g_dll_path{};

    const char* status_label(viewer::Status s)
    {
        switch (s)
        {
        case viewer::Status::Idle:      return "Idle";
        case viewer::Status::Injecting: return "Injecting";
        case viewer::Status::Receiving: return "Receiving";
        case viewer::Status::Done:      return "Done";
        case viewer::Status::Error:     return "Error";
        }
        return "?";
    }

    void draw_jvm_bar(viewer::App& app)
    {
        ImGui::TextUnformatted("Running JVMs");
        ImGui::SameLine();
        if (ImGui::SmallButton("Refresh") && !app.busy())
        {
            app.refresh_jvms();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(app.busy() || app.selected_jvm < 0);
        if (ImGui::SmallButton("Attach & Enumerate"))
        {
            app.attach_selected(g_dll_path);
            g_selected_class = -1;
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::Text("|  Status: %s", status_label(app.status.load()));
        if (app.status.load() == viewer::Status::Receiving)
        {
            ImGui::SameLine();
            ImGui::Text("(%llu classes...)", static_cast<unsigned long long>(app.classes_streamed.load()));
        }
        ImGui::TextWrapped("%s", app.status_message.c_str());

        if (ImGui::BeginTable("jvms", 3,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                ImVec2(0.0f, 120.0f)))
        {
            ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableSetupColumn("Process", ImGuiTableColumnFlags_WidthFixed, 130.0f);
            ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            for (int i = 0; i < static_cast<int>(app.jvms.size()); ++i)
            {
                const viewer::JvmProcess& p{ app.jvms[static_cast<std::size_t>(i)] };
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                char label[32];
                std::snprintf(label, sizeof(label), "%u", p.pid);
                if (ImGui::Selectable(label, app.selected_jvm == i, ImGuiSelectableFlags_SpanAllColumns))
                {
                    app.selected_jvm = i;
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(p.image_name.c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(p.image_path.empty() ? "<unknown>" : p.image_path.c_str());
            }
            ImGui::EndTable();
        }
    }

    void draw_members(const viewer::ClassInfo& cls)
    {
        const float half{ ImGui::GetContentRegionAvail().x * 0.5f - 4.0f };

        // Methods
        ImGui::BeginChild("methods_pane", ImVec2(half, 0), ImGuiChildFlags_Borders);
        ImGui::Text("Methods (%zu)", cls.methods.size());
        if (ImGui::BeginTable("methods", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable))
        {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 180.0f);
            ImGui::TableSetupColumn("Descriptor", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(cls.methods.size()));
            while (clipper.Step())
            {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(cls.methods[static_cast<std::size_t>(i)].name.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(cls.methods[static_cast<std::size_t>(i)].descriptor.c_str());
                }
            }
            ImGui::EndTable();
        }
        ImGui::EndChild();

        ImGui::SameLine();

        // Fields
        ImGui::BeginChild("fields_pane", ImVec2(0, 0), ImGuiChildFlags_Borders);
        ImGui::Text("Fields (%zu)", cls.fields.size());
        if (ImGui::BeginTable("fields", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable))
        {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 160.0f);
            ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Static", ImGuiTableColumnFlags_WidthFixed, 50.0f);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(cls.fields.size()));
            while (clipper.Step())
            {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                {
                    const viewer::FieldInfo& f{ cls.fields[static_cast<std::size_t>(i)] };
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(f.name.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(f.descriptor.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(f.is_static ? "yes" : "");
                }
            }
            ImGui::EndTable();
        }
        ImGui::EndChild();
    }

    void render_ui(viewer::App& app)
    {
        const ImGuiViewport* vp{ ImGui::GetMainViewport() };
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("vmhook viewer", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);

        draw_jvm_bar(app);
        ImGui::Separator();

        ImGui::SetNextItemWidth(320.0f);
        ImGui::InputTextWithHint("##search", "Search classes (substring)", g_search, sizeof(g_search));
        ImGui::SameLine();
        {
            std::lock_guard<std::mutex> lock{ app.data_mutex };
            ImGui::Text("%zu classes loaded", app.classes.size());
        }

        // Class list (top ~45%)
        const float class_h{ ImGui::GetContentRegionAvail().y * 0.45f };
        ImGui::BeginChild("classes_pane", ImVec2(0, class_h), ImGuiChildFlags_Borders);
        {
            std::lock_guard<std::mutex> lock{ app.data_mutex };
            const std::string needle{ g_search };
            if (ImGui::BeginTable("classes", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable))
            {
                ImGui::TableSetupColumn("Package", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Class", ImGuiTableColumnFlags_WidthFixed, 260.0f);
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableHeadersRow();
                for (int i = 0; i < static_cast<int>(app.classes.size()); ++i)
                {
                    const viewer::ClassInfo& c{ app.classes[static_cast<std::size_t>(i)] };
                    if (!needle.empty() && !icontains(c.internal_name, needle))
                    {
                        continue;
                    }
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    if (ImGui::Selectable(c.package.empty() ? "<default>" : c.package.c_str(),
                            g_selected_class == i, ImGuiSelectableFlags_SpanAllColumns))
                    {
                        g_selected_class = i;
                    }
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(c.simple_name.c_str());
                }
                ImGui::EndTable();
            }
        }
        ImGui::EndChild();

        // Members (bottom)
        {
            std::lock_guard<std::mutex> lock{ app.data_mutex };
            if (g_selected_class >= 0 && g_selected_class < static_cast<int>(app.classes.size()))
            {
                draw_members(app.classes[static_cast<std::size_t>(g_selected_class)]);
            }
            else
            {
                ImGui::TextDisabled("Select a class to see its methods and fields.");
            }
        }

        ImGui::End();
    }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
    WNDCLASSEXW wc{ sizeof(wc), CS_CLASSDC, WndProc, 0, 0, GetModuleHandleW(nullptr), nullptr, nullptr, nullptr, nullptr, L"vmhook_viewer", nullptr };
    RegisterClassExW(&wc);
    HWND hwnd{ CreateWindowW(wc.lpszClassName, L"vmhook viewer", WS_OVERLAPPEDWINDOW,
                             100, 100, 1280, 800, nullptr, nullptr, wc.hInstance, nullptr) };

    if (!create_device(hwnd))
    {
        cleanup_device();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io{ ImGui::GetIO() };
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
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
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT)
            {
                running = false;
            }
        }
        if (!running)
        {
            break;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        render_ui(app);

        ImGui::Render();
        const float clear[4]{ 0.10f, 0.10f, 0.12f, 1.0f };
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
