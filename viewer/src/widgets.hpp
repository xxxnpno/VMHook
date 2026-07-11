// Community Dear ImGui widgets, adapted to the viewer's theme.
//   - Spinner()      — animated circular loading indicator, from ocornut/imgui
//                      issue #1901 (progress indicators).
//   - ToggleSwitch() — iOS-style animated on/off switch, from ocornut/imgui
//                      issue #1537 (toggle button), recoloured to the accent.
// Both need imgui_internal.h for GetCurrentWindow/ItemAdd/GImGui/animation state.
#pragma once

#include "imgui.h"
#include "imgui_internal.h"

namespace ui
{
    // Animated spinner. Draws at the current cursor and advances the layout by
    // (radius*2). Source: ocornut/imgui#1901, adapted (float thickness).
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

    // Animated on/off switch. Draws just the switch (width ≈ 1.6*frame height);
    // the caller adds a SameLine label. Returns true on the frame it toggled.
    // Source: ocornut/imgui#1537, recoloured (FrameBg -> Button accent on).
    inline bool ToggleSwitch(const char* str_id, bool* v)
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* draw_list = ImGui::GetWindowDrawList();

        const float height = ImGui::GetFrameHeight();
        const float width  = height * 1.6f;
        const float radius = height * 0.5f;

        const bool clicked = ImGui::InvisibleButton(str_id, ImVec2(width, height));
        if (clicked) *v = !*v;

        float t = *v ? 1.0f : 0.0f;
        ImGuiContext& g = *GImGui;
        const float ANIM_SPEED = 0.09f;
        if (g.LastActiveId == g.CurrentWindow->GetID(str_id))
        {
            const float t_anim = ImSaturate(g.LastActiveIdTimer / ANIM_SPEED);
            t = *v ? t_anim : (1.0f - t_anim);
        }

        const bool hov = ImGui::IsItemHovered();
        const ImVec4 off  = ImGui::GetStyleColorVec4(ImGuiCol_FrameBg);
        const ImVec4 offH = ImGui::GetStyleColorVec4(ImGuiCol_FrameBgHovered);
        const ImVec4 on   = ImGui::GetStyleColorVec4(ImGuiCol_Button);
        const ImVec4 onH  = ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered);
        const ImU32 col_bg = ImGui::GetColorU32(ImLerp(hov ? offH : off, hov ? onH : on, t));

        draw_list->AddRectFilled(p, ImVec2(p.x + width, p.y + height), col_bg, radius);
        draw_list->AddCircleFilled(ImVec2(p.x + radius + t * (width - radius * 2.0f), p.y + radius),
                                   radius - height * 0.13f, IM_COL32(255, 255, 255, 255));
        return clicked;
    }

    // ToggleSwitch + a clickable text label on the same row (label also toggles).
    inline bool Toggle(const char* label, bool* v)
    {
        ImGui::PushID(label);
        bool changed = ToggleSwitch("##t", v);
        ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        if (ImGui::IsItemClicked()) { *v = !*v; changed = true; }
        ImGui::PopID();
        return changed;
    }
}
