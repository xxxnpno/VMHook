// Viewer widget helpers.  The buttons / combos / inputs are the stock,
// battle-tested Dear ImGui widgets (a clean flat theme in apply_modern_style()
// does the styling — no bespoke skeuomorphic drawing); the toggle switches are
// the vendored, professional cmdwtf/imgui_toggle library (third_party/
// imgui_toggle, 0BSD) with an accent-matched palette.  Spinner is the community
// #1901 widget.  Same call sites as before — only the implementation changed.
#pragma once

#include "imgui.h"
#include "imgui_internal.h"

#include "imgui_toggle.h"
#include "imgui_toggle_palette.h"

namespace ui
{
    enum ButtonKind { BtnNeutral = 0, BtnPrimary = 1, BtnGhost = 2, BtnDanger = 3 };

    // A themed button.  Neutral = the frame-coloured default; Primary = solid
    // accent; Ghost = transparent chrome that tints on hover; Danger = red hover
    // (for the window close control).  Just stock ImGui::Button under themed colours.
    inline bool Button(const char* label, const ImVec2& size = ImVec2(0, 0), int kind = BtnNeutral)
    {
        const ImVec4* c{ ImGui::GetStyle().Colors };
        const ImVec4 accent{ c[ImGuiCol_SliderGrab] };
        const ImVec4 accentHi{ c[ImGuiCol_SliderGrabActive] };
        int colors{ 0 }, vars{ 0 };
        if (kind == BtnPrimary)
        {
            ImGui::PushStyleColor(ImGuiCol_Button,        accent);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, accentHi);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(accent.x * 0.86f, accent.y * 0.86f, accent.z * 0.86f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(1, 1, 1, 1));
            colors = 4;
        }
        else if (kind == BtnGhost)
        {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.09f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1, 1, 1, 0.15f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
            colors = 3; vars = 1;
        }
        else if (kind == BtnDanger)
        {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.86f, 0.26f, 0.26f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.70f, 0.17f, 0.17f, 1.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
            colors = 3; vars = 1;
        }
        const bool pressed{ ImGui::Button(label, size) };
        if (vars)   ImGui::PopStyleVar(vars);
        if (colors) ImGui::PopStyleColor(colors);
        return pressed;
    }

    // Dropdowns — stock ImGui, width set for the caller (negative widths follow
    // ImGui's "fill from the right" convention, same as before).
    inline bool BeginCombo(const char* id, const char* preview, float width)
    {
        ImGui::SetNextItemWidth(width);
        return ImGui::BeginCombo(id, preview, ImGuiComboFlags_None);
    }
    inline void EndCombo() { ImGui::EndCombo(); }

    inline bool Combo(const char* id, int* current, const char* items_sep, float width)
    {
        ImGui::SetNextItemWidth(width);
        return ImGui::Combo(id, current, items_sep);
    }
    inline bool Combo(const char* id, int* current, const char* const items[], int count, float width)
    {
        ImGui::SetNextItemWidth(width);
        return ImGui::Combo(id, current, items, count);
    }

    // Text input — stock ImGui, plus a crisp 1px accent border while focused
    // (the one custom touch; drawn on top so it never hides the text).
    inline bool InputText(const char* id, const char* hint, char* buf, std::size_t sz, float width)
    {
        ImGui::SetNextItemWidth(width);
        const bool changed{ ImGui::InputTextWithHint(id, hint, buf, sz) };
        if (ImGui::IsItemActive() || ImGui::IsItemFocused())
        {
            const ImVec4 accent{ ImGui::GetStyle().Colors[ImGuiCol_SliderGrab] };
            ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                ImGui::GetColorU32(accent), ImGui::GetStyle().FrameRounding, 0, 1.5f);
        }
        return changed;
    }

    // Animated spinner (ocornut/imgui#1901), adapted.
    inline void Spinner(const char* label, float radius, float thickness, ImU32 color)
    {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (window->SkipItems) return;

        ImGuiContext& g = *GImGui;
        const ImGuiStyle& style = g.Style;
        const ImGuiID id = window->GetID(label);

        const ImVec2 pos = window->DC.CursorPos;
        const ImVec2 size((radius) * 2.0f, (radius + style.FramePadding.y) * 2.0f);
        const ImRect bb(pos, ImVec2(pos.x + size.x, pos.y + size.y));
        ImGui::ItemSize(bb, style.FramePadding.y);
        if (!ImGui::ItemAdd(bb, id)) return;

        window->DrawList->PathClear();
        const int num_segments = 30;
        const int start = static_cast<int>(ImAbs(ImSin(static_cast<float>(g.Time) * 1.8f) * (num_segments - 5)));
        const float a_min = IM_PI * 2.0f * (static_cast<float>(start)) / static_cast<float>(num_segments);
        const float a_max = IM_PI * 2.0f * (static_cast<float>(num_segments) - 3) / static_cast<float>(num_segments);
        const ImVec2 centre = ImVec2(pos.x + radius, pos.y + radius + style.FramePadding.y);
        for (int i = 0; i < num_segments; i++)
        {
            const float a = a_min + (static_cast<float>(i) / static_cast<float>(num_segments)) * (a_max - a_min);
            window->DrawList->PathLineTo(ImVec2(centre.x + ImCos(a + static_cast<float>(g.Time) * 8.0f) * radius,
                                                centre.y + ImSin(a + static_cast<float>(g.Time) * 8.0f) * radius));
        }
        window->DrawList->PathStroke(color, false, thickness);
    }

    // Professional animated toggle switch — the vendored cmdwtf/imgui_toggle,
    // configured once with an accent-matched palette (theme blue when on, frame
    // when off, white knob, thin border, 120ms ease).  Draws its own trailing
    // label like ImGui::Checkbox, so call sites pass the label as usual.
    inline bool Toggle(const char* label, bool* v)
    {
        const ImVec4* col{ ImGui::GetStyle().Colors };
        ImGuiTogglePalette on{};
        on.Frame       = col[ImGuiCol_SliderGrab];
        on.FrameHover  = col[ImGuiCol_SliderGrabActive];
        on.FrameBorder = col[ImGuiCol_SliderGrabActive];
        on.Knob        = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
        on.KnobHover   = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
        ImGuiTogglePalette off{};
        off.Frame       = col[ImGuiCol_FrameBg];
        off.FrameHover  = col[ImGuiCol_FrameBgHovered];
        off.FrameBorder = col[ImGuiCol_Border];
        off.Knob        = ImVec4(0.72f, 0.75f, 0.82f, 1.0f);
        off.KnobHover   = ImVec4(0.86f, 0.88f, 0.93f, 1.0f);

        ImGuiToggleConfig cfg{};
        cfg.Flags            = ImGuiToggleFlags_Animated | ImGuiToggleFlags_BorderedFrame;
        cfg.AnimationDuration = 0.12f;
        cfg.FrameRounding    = 1.0f;   // pill frame — correct/expected for a switch
        cfg.KnobRounding     = 1.0f;
        cfg.On.Palette       = &on;
        cfg.Off.Palette      = &off;
        return ImGui::Toggle(label, v, cfg);
    }
}
