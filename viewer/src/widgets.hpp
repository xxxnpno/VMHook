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

#include <cmath>

namespace ui
{
    // ── motion: one frame-rate-independent exponential smoother, keyed by widget
    // ID, so every hover / selection / focus / toast animation settles with the
    // SAME timing.  cur = lerp(cur, target, 1 - e^(-rate*dt)) — NOT cur += (t-cur)*k
    // (which is FPS-dependent).  State lives in ImGui's per-window storage (no
    // allocation, no external lib).  One motion constant everywhere = perceived
    // quality; matches the toggle's 0.12s.
    namespace anim
    {
        inline constexpr float kRate     = 14.0f;  // ~120ms settle (hover/focus/fill)
        inline constexpr float kRateSnap = 22.0f;  // selection / keyboard-nav: near-instant
        inline constexpr float kRateSoft = 10.0f;  // toast fade

        inline float Approach(ImGuiID id, ImU32 key, float target, float rate = kRate)
        {
            ImGuiStorage* st{ ImGui::GetStateStorage() };
            const ImGuiID k{ id ^ (key * 2654435761u) };
            float cur{ st->GetFloat(k, target) };  // seed = target on first frame (no pop-in)
            cur = ImLerp(cur, target, 1.0f - std::exp(-rate * ImGui::GetIO().DeltaTime));
            if (ImFabs(cur - target) < 0.001f) cur = target;
            st->SetFloat(k, cur);
            return cur;
        }
    }

    // Delayed, flicker-free tooltip (ImGui 1.92 stationary + short-delay for free).
    // Replaces the raw `if (IsItemHovered()) SetTooltip(...)` pattern.
    inline void Tooltip(const char* text)
    {
        if (ImGui::BeginItemTooltip())
        {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
            ImGui::TextUnformatted(text);
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }

    enum ButtonKind { BtnNeutral = 0, BtnPrimary = 1, BtnGhost = 2, BtnDanger = 3 };

    // A themed button.  Neutral = the frame-coloured default; Primary = solid
    // accent; Ghost = transparent chrome that tints on hover; Danger = red hover
    // (for the window close control).  Just stock ImGui::Button under themed colours.
    inline bool Button(const char* label, const ImVec2& size = ImVec2(0, 0), int kind = BtnNeutral)
    {
        const ImVec4* c{ ImGui::GetStyle().Colors };
        const ImVec4 accent{ c[ImGuiCol_SliderGrab] };
        const ImVec4 accentHi{ c[ImGuiCol_SliderGrabActive] };
        // Compact buttons that sit in the page's flat-frame family: narrower
        // horizontal padding (height kept = frame height so they stay aligned
        // with combos/inputs on a shared row).
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
            ImVec2(ImGui::GetStyle().FramePadding.x * 0.72f, ImGui::GetStyle().FramePadding.y));
        int colors{ 0 }, vars{ 1 };
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
            colors = 3; vars = 2;
        }
        else if (kind == BtnDanger)
        {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.86f, 0.26f, 0.26f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.70f, 0.17f, 0.17f, 1.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
            colors = 3; vars = 2;
        }
        else  // BtnNeutral — look like a flat frame cell (matches the combos/inputs)
        {
            ImGui::PushStyleColor(ImGuiCol_Button,        c[ImGuiCol_FrameBg]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, c[ImGuiCol_FrameBgHovered]);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  c[ImGuiCol_FrameBgActive]);
            colors = 3;
        }
        const bool pressed{ ImGui::Button(label, size) };
        ImGui::PopStyleVar(vars);
        if (colors) ImGui::PopStyleColor(colors);
        return pressed;
    }

    namespace detail
    {
        // First Unicode codepoint of a UTF-8 string (FA glyphs are 3-byte).
        inline unsigned first_cp(const char* s)
        {
            const unsigned char* u{ reinterpret_cast<const unsigned char*>(s) };
            const unsigned c{ u[0] };
            if (c < 0x80)          return c;
            if ((c >> 5) == 0x6)   return ((c & 0x1Fu) << 6) | (u[1] & 0x3Fu);
            if ((c >> 4) == 0xE)   return ((c & 0x0Fu) << 12) | ((u[1] & 0x3Fu) << 6) | (u[2] & 0x3Fu);
            if ((c >> 3) == 0x1E)  return ((c & 0x07u) << 18) | ((u[1] & 0x3Fu) << 12) | ((u[2] & 0x3Fu) << 6) | (u[3] & 0x3Fu);
            return c;
        }
    }

    // A compact square icon-only button.  The hit box reserves the full frame
    // height (so it aligns with combos/inputs on the same row), but the visible
    // square is `size` (default 0.78*frame-height — noticeably lighter than a
    // full-height square) and vertically centred in that cell.  The glyph is
    // placed by its ACTUAL visual bounds (ImFont::FindGlyph), so Font Awesome's
    // inline baseline offset can't push it off-centre — it's pixel-centred.
    inline bool IconButton(const char* icon, const char* str_id, float size = 0.0f, int kind = BtnNeutral)
    {
        ImGuiStyle& st{ ImGui::GetStyle() };
        const ImVec4* col{ st.Colors };
        const float fh{ ImGui::GetFrameHeight() };
        if (size <= 0.0f) size = fh * 0.78f;

        const ImVec2 p0{ ImGui::GetCursorScreenPos() };
        ImGui::PushID(str_id);
        const bool pressed{ ImGui::InvisibleButton("##ib", ImVec2(size, fh)) };
        const ImGuiID aid{ ImGui::GetItemID() };
        const bool hov{ ImGui::IsItemHovered() };
        const bool act{ ImGui::IsItemActive() };
        ImGui::PopID();

        // Visible square, vertically centred in the frame-height cell.
        const float  vy{ p0.y + (fh - size) * 0.5f };
        const ImVec2 q0{ p0.x, vy }, q1{ p0.x + size, vy + size };

        // Idle / hover / active fills — the state fill is LERPED between them so
        // hover/press fade in smoothly instead of snapping.
        ImVec4 idleFill{ 0, 0, 0, 0 }, hovFill{ 0, 0, 0, 0 }, actFill{ 0, 0, 0, 0 };
        ImVec4 border{ 0, 0, 0, 0 }, text{ col[ImGuiCol_Text] };
        bool drawBorder{ false };
        if (kind == BtnGhost)
        {
            hovFill = ImVec4(1, 1, 1, 0.09f); actFill = ImVec4(1, 1, 1, 0.15f);
        }
        else if (kind == BtnDanger)
        {
            hovFill = ImVec4(0.86f, 0.26f, 0.26f, 1); actFill = ImVec4(0.70f, 0.17f, 0.17f, 1);
        }
        else if (kind == BtnPrimary)
        {
            idleFill = col[ImGuiCol_SliderGrab];
            hovFill  = col[ImGuiCol_SliderGrabActive];
            actFill  = ImVec4(col[ImGuiCol_SliderGrab].x * 0.86f, col[ImGuiCol_SliderGrab].y * 0.86f, col[ImGuiCol_SliderGrab].z * 0.86f, 1.0f);
            text = ImVec4(1, 1, 1, 1);
        }
        else  // Neutral
        {
            idleFill = col[ImGuiCol_Button]; hovFill = col[ImGuiCol_ButtonHovered]; actFill = col[ImGuiCol_ButtonActive];
            border = col[ImGuiCol_Border]; drawBorder = true;
        }
        const float th{ anim::Approach(aid, ImHashStr("hov"), hov ? 1.0f : 0.0f) };
        const float ta{ anim::Approach(aid, ImHashStr("act"), act ? 1.0f : 0.0f, anim::kRateSnap) };
        const ImVec4 fill{ ImLerp(ImLerp(idleFill, hovFill, th), actFill, ta) };

        ImDrawList* dl{ ImGui::GetWindowDrawList() };
        const float r{ st.FrameRounding };
        if (fill.w > 0.0f) dl->AddRectFilled(q0, q1, ImGui::GetColorU32(fill), r);
        if (drawBorder && st.FrameBorderSize > 0.0f)
            dl->AddRect(q0, q1, ImGui::GetColorU32(border), r, 0, st.FrameBorderSize);

        const ImVec2 ctr{ p0.x + size * 0.5f, vy + size * 0.5f };
        const ImU32  tcol{ ImGui::GetColorU32(text) };
        // ImGui 1.92+ moved FindGlyph onto the size-baked font.  Centre by the
        // glyph's actual visual quad so the FA baseline offset can't shift it.
        ImFontBaked* baked{ ImGui::GetFontBaked() };
        const ImFontGlyph* g{ baked ? baked->FindGlyphNoFallback(static_cast<ImWchar>(detail::first_cp(icon))) : nullptr };
        if (g)
            dl->AddText(ImVec2(ctr.x - (g->X0 + g->X1) * 0.5f, ctr.y - (g->Y0 + g->Y1) * 0.5f), tcol, icon);
        else
        {
            const ImVec2 ts{ ImGui::CalcTextSize(icon) };
            dl->AddText(ImVec2(ctr.x - ts.x * 0.5f, ctr.y - ts.y * 0.5f), tcol, icon);
        }
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
        // Accent focus ring that fades in/out with focus (smoother than a hard toggle).
        const bool focused{ ImGui::IsItemActive() || ImGui::IsItemFocused() };
        const float a{ anim::Approach(ImGui::GetItemID(), ImHashStr("ring"), focused ? 1.0f : 0.0f, 18.0f) };
        if (a > 0.01f)
        {
            ImVec4 accent{ ImGui::GetStyle().Colors[ImGuiCol_SliderGrab] };
            accent.w = a;
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
        // A touch smaller than a full frame-height switch so it sits quietly in
        // the flat page style; width follows the golden-ratio default.
        cfg.Size             = ImVec2(0.0f, ImGui::GetFrameHeight() * 0.74f);
        cfg.On.Palette       = &on;
        cfg.Off.Palette      = &off;
        return ImGui::Toggle(label, v, cfg);
    }
}
