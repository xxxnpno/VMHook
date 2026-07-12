// Custom "Moonlight" widget set for the viewer — buttons, combos, text inputs,
// toggle and spinner, all drawn by hand on the ImGui draw list so they carry a
// distinct look (glossy pill fills, hover halos, accent focus rings) instead of
// stock ImGui frames.  Palette is read live from the active style, so these
// follow apply_modern_style()'s Moonlight colours automatically.
//
// Community seeds: Spinner from ocornut/imgui#1901, ToggleSwitch from
// ocornut/imgui#1537 — both recoloured/adapted here.  Needs imgui_internal.h
// for GetCurrentWindow / FindRenderedTextEnd / GImGui.
#pragma once

#include "imgui.h"
#include "imgui_internal.h"

namespace ui
{
    enum ButtonKind { BtnNeutral = 0, BtnPrimary = 1, BtnGhost = 2, BtnDanger = 3 };

    namespace detail
    {
        inline ImU32  u32(const ImVec4& c) { return ImGui::GetColorU32(c); }
        inline ImVec4 sty(ImGuiCol i)      { return ImGui::GetStyle().Colors[i]; }
        inline ImVec4 accent()             { return sty(ImGuiCol_SliderGrab); }   // theme accent
        inline ImVec4 a(ImVec4 c, float na){ c.w = na; return c; }
        inline ImVec4 mul_a(ImVec4 c, float m){ c.w *= m; return c; }

        // A glossy, rounded, bordered fill with an optional outer halo — the
        // shared body of every custom control.  `glow` in [0,1] scales the halo.
        inline void panel(ImDrawList* dl, const ImVec2& p0, const ImVec2& p1,
                          ImVec4 fill, ImVec4 border, float rounding,
                          bool gloss, float glow, ImVec4 glow_col, float ga)
        {
            fill.w *= ga; border.w *= ga;
            // soft drop shadow
            dl->AddRectFilled(ImVec2(p0.x, p0.y + 1.5f), ImVec2(p1.x, p1.y + 2.5f),
                              ImGui::GetColorU32(ImVec4(0, 0, 0, 0.22f * ga)), rounding);
            // outer halo (hover / focus)
            if (glow > 0.0f)
            {
                dl->AddRect(ImVec2(p0.x - 1.0f, p0.y - 1.0f), ImVec2(p1.x + 1.0f, p1.y + 1.0f),
                            u32(a(glow_col, 0.55f * glow * ga)), rounding + 1.0f, 0, 2.0f);
                dl->AddRect(ImVec2(p0.x - 2.5f, p0.y - 2.5f), ImVec2(p1.x + 2.5f, p1.y + 2.5f),
                            u32(a(glow_col, 0.20f * glow * ga)), rounding + 2.5f, 0, 2.5f);
            }
            // base fill
            if (fill.w > 0.0f) dl->AddRectFilled(p0, p1, u32(fill), rounding);
            // top sheen + hairline highlight for a glassy edge
            if (gloss && fill.w > 0.0f)
            {
                const float midy{ p0.y + (p1.y - p0.y) * 0.5f };
                const ImU32 top{ u32(ImVec4(1, 1, 1, 0.055f * ga)) };
                const ImU32 bot{ u32(ImVec4(1, 1, 1, 0.0f)) };
                dl->AddRectFilledMultiColor(ImVec2(p0.x + rounding * 0.5f, p0.y + 1.0f),
                                            ImVec2(p1.x - rounding * 0.5f, midy), top, top, bot, bot);
                dl->AddLine(ImVec2(p0.x + rounding, p0.y + 1.0f), ImVec2(p1.x - rounding, p0.y + 1.0f),
                            u32(ImVec4(1, 1, 1, 0.10f * ga)), 1.0f);
            }
            // crisp border
            if (border.w > 0.0f) dl->AddRect(p0, p1, u32(border), rounding, 0, 1.0f);
        }
    }

    // A hand-drawn button.  Pill-rounded (rounding = height/2), glossy, with a
    // hover halo.  Kinds: Neutral (frame), Primary (accent), Ghost (chrome),
    // Danger (red on hover).  Returns true on click; layout/IsItem* work as usual.
    inline bool Button(const char* label, const ImVec2& size_arg = ImVec2(0, 0), int kind = BtnNeutral)
    {
        ImGuiStyle& st{ ImGui::GetStyle() };
        const ImVec2 lsz{ ImGui::CalcTextSize(label, nullptr, true) };
        const float  h{ size_arg.y > 0.0f ? size_arg.y : ImGui::GetFrameHeight() };
        const float  w{ size_arg.x > 0.0f ? size_arg.x : (lsz.x + st.FramePadding.x * 2.0f) };
        const ImVec2 p0{ ImGui::GetCursorScreenPos() };
        const ImVec2 p1{ ImVec2(p0.x + w, p0.y + h) };

        ImGui::PushID(label);
        const bool pressed{ ImGui::InvisibleButton("##btn", ImVec2(w, h)) };
        const bool hov{ ImGui::IsItemHovered() };
        const bool act{ ImGui::IsItemActive() };
        ImGui::PopID();

        const float   ga{ st.Alpha };
        const float   rounding{ h * 0.5f };
        const ImVec4  accent{ detail::accent() };
        ImVec4 fill, border{ detail::sty(ImGuiCol_Border) }, glowc{ accent };
        bool   gloss{ true };
        float  glow{ 0.0f };

        if (kind == BtnGhost)
        {
            gloss = false; border = ImVec4(0, 0, 0, 0);
            fill = act ? ImVec4(1, 1, 1, 0.14f) : hov ? ImVec4(1, 1, 1, 0.08f) : ImVec4(0, 0, 0, 0);
        }
        else if (kind == BtnDanger)
        {
            gloss = false; border = ImVec4(0, 0, 0, 0);
            fill = act ? ImVec4(0.70f, 0.17f, 0.17f, 1) : hov ? ImVec4(0.86f, 0.26f, 0.26f, 1) : ImVec4(0, 0, 0, 0);
        }
        else if (kind == BtnPrimary)
        {
            fill = act ? detail::sty(ImGuiCol_ButtonActive)
                 : hov ? detail::sty(ImGuiCol_ButtonHovered) : detail::sty(ImGuiCol_Button);
            border = detail::a(accent, 0.60f);
            glow = hov ? 1.0f : 0.35f;   // primary always carries a faint halo
        }
        else  // BtnNeutral
        {
            fill = act ? detail::sty(ImGuiCol_FrameBgActive)
                 : hov ? detail::sty(ImGuiCol_FrameBgHovered) : detail::sty(ImGuiCol_FrameBg);
            glow = hov ? 0.6f : 0.0f;
        }

        ImDrawList* dl{ ImGui::GetWindowDrawList() };
        detail::panel(dl, p0, p1, fill, border, rounding, gloss, glow, glowc, ga);

        const char* end{ ImGui::FindRenderedTextEnd(label) };
        ImVec2 tp{ p0.x + (w - lsz.x) * 0.5f, p0.y + (h - lsz.y) * 0.5f };
        if (act) tp.y += 1.0f;  // press nudge
        dl->AddText(tp, ImGui::GetColorU32(detail::mul_a(detail::sty(ImGuiCol_Text), ga)), label, end);
        return pressed;
    }

    // Draw the custom frame that sits behind a combo / input, using the current
    // hover state.  Returns the resolved pixel width (handles ImGui's negative
    // "fill from right" widths so the frame rect matches the widget).
    namespace detail
    {
        inline float resolve_w(float width)
        {
            if (width > 0.0f) return width;
            const float avail{ ImGui::GetContentRegionAvail().x };
            const float w{ avail + width };            // width is negative here
            return w > 1.0f ? w : avail;
        }
        inline void field_bg(float pxw, bool gloss, bool& hov_out, ImVec2& p0_out, ImVec2& p1_out)
        {
            ImGuiStyle& st{ ImGui::GetStyle() };
            const ImVec2 p0{ ImGui::GetCursorScreenPos() };
            const ImVec2 p1{ p0.x + pxw, p0.y + ImGui::GetFrameHeight() };
            const bool   hov{ ImGui::IsMouseHoveringRect(p0, p1) };
            panel(ImGui::GetWindowDrawList(), p0, p1,
                  hov ? sty(ImGuiCol_FrameBgHovered) : sty(ImGuiCol_FrameBg),
                  sty(ImGuiCol_Border), st.FrameRounding, gloss, hov ? 0.5f : 0.0f, accent(), st.Alpha);
            hov_out = hov; p0_out = p0; p1_out = p1;
        }
        inline void push_transparent_frame()
        {
            ImGui::PushStyleColor(ImGuiCol_FrameBg,        ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_FrameBgActive,  ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_Button,         ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   ImVec4(0, 0, 0, 0));
        }
        inline void pop_transparent_frame() { ImGui::PopStyleColor(6); }
    }

    // Custom-framed dropdowns.  We draw our glossy frame, then let ImGui render
    // the preview text + arrow + popup over a transparent frame — full control of
    // the closed look (what the user sees most) with ImGui's popup for the list.
    inline bool BeginCombo(const char* id, const char* preview, float width)
    {
        const float pxw{ detail::resolve_w(width) };
        bool hov; ImVec2 p0, p1; detail::field_bg(pxw, true, hov, p0, p1);
        detail::push_transparent_frame();
        ImGui::SetNextItemWidth(pxw);
        const bool open{ ImGui::BeginCombo(id, preview, ImGuiComboFlags_None) };
        detail::pop_transparent_frame();
        return open;
    }
    inline void EndCombo() { ImGui::EndCombo(); }

    inline bool Combo(const char* id, int* current, const char* items_sep, float width)
    {
        const float pxw{ detail::resolve_w(width) };
        bool hov; ImVec2 p0, p1; detail::field_bg(pxw, true, hov, p0, p1);
        detail::push_transparent_frame();
        ImGui::SetNextItemWidth(pxw);
        const bool ch{ ImGui::Combo(id, current, items_sep) };
        detail::pop_transparent_frame();
        return ch;
    }
    inline bool Combo(const char* id, int* current, const char* const items[], int count, float width)
    {
        const float pxw{ detail::resolve_w(width) };
        bool hov; ImVec2 p0, p1; detail::field_bg(pxw, true, hov, p0, p1);
        detail::push_transparent_frame();
        ImGui::SetNextItemWidth(pxw);
        const bool ch{ ImGui::Combo(id, current, items, count) };
        detail::pop_transparent_frame();
        return ch;
    }

    // Custom-framed text input with an accent focus ring.  `width` follows ImGui's
    // convention (negative = fill from the right).
    inline bool InputText(const char* id, const char* hint, char* buf, std::size_t sz, float width)
    {
        const float pxw{ detail::resolve_w(width) };
        bool hov; ImVec2 p0, p1; detail::field_bg(pxw, false, hov, p0, p1);
        detail::push_transparent_frame();
        ImGui::SetNextItemWidth(pxw);
        const bool ch{ ImGui::InputTextWithHint(id, hint, buf, sz) };
        detail::pop_transparent_frame();
        if (ImGui::IsItemActive())
        {
            ImDrawList* dl{ ImGui::GetWindowDrawList() };
            const ImVec4 ac{ detail::accent() };
            const float  r{ ImGui::GetStyle().FrameRounding };
            dl->AddRect(ImVec2(p0.x - 1.5f, p0.y - 1.5f), ImVec2(p1.x + 1.5f, p1.y + 1.5f),
                        detail::u32(detail::a(ac, 0.25f)), r + 1.5f, 0, 2.0f);
            dl->AddRect(p0, p1, detail::u32(ac), r, 0, 1.6f);
        }
        return ch;
    }

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

    // Animated on/off switch with a soft accent glow when on. Draws just the
    // switch (width ≈ 1.7*frame height); the caller adds a SameLine label.
    // Source: ocornut/imgui#1537, recoloured to the theme accent.
    inline bool ToggleSwitch(const char* str_id, bool* v)
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* draw_list = ImGui::GetWindowDrawList();

        const float height = ImGui::GetFrameHeight() * 0.92f;
        const float width  = height * 1.75f;
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
        const ImVec4 track = ImLerp(hov ? offH : off, hov ? onH : on, t);
        // glow when engaged
        if (t > 0.01f)
            draw_list->AddRectFilled(ImVec2(p.x - 1.0f, p.y - 1.0f), ImVec2(p.x + width + 1.0f, p.y + height + 1.0f),
                                     ImGui::GetColorU32(detail::a(detail::accent(), 0.28f * t)), radius + 1.0f);
        draw_list->AddRectFilled(p, ImVec2(p.x + width, p.y + height), ImGui::GetColorU32(track), radius);
        draw_list->AddRect(p, ImVec2(p.x + width, p.y + height), ImGui::GetColorU32(ImGui::GetStyleColorVec4(ImGuiCol_Border)), radius);
        draw_list->AddCircleFilled(ImVec2(p.x + radius + t * (width - radius * 2.0f), p.y + radius),
                                   radius - height * 0.14f, IM_COL32(255, 255, 255, 255));
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
