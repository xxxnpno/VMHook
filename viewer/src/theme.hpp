// vmhook viewer — centralized design tokens (Tokyo Night, Night).
//
// Single source of truth for the GUI's colors so the whole app reads as ONE
// designed system.  The base ImGuiStyle palette/geometry is applied in
// apply_modern_style() (main.cpp) from these tokens; this header also owns the
// SEMANTIC colors that were previously hardcoded ad-hoc across the per-window
// code (status, values, access, class kinds, headers, success/danger/warning).
//
// Discipline: exactly ONE chromatic UI accent (blue #7aa2f7) drives interactive
// state.  Green/amber/red/cyan/orange/purple are reserved for MEANING (state,
// value type, access, kind) — colour always carries information, never decorates.
//
// Every accessor returns `const ImVec4&` into a function-local static: these run
// per-row inside table clippers (thousands of calls/frame), so they must not
// construct an ImVec4 each call.

#pragma once

#include "imgui.h"

#include <cstdint>
#include <string>

namespace theme
{
    // ── raw palette (Tokyo Night, hex-verified) ───────────────────────────────
    inline const ImVec4& accent()    { static const ImVec4 v{ 0.478f, 0.635f, 0.969f, 1.00f }; return v; }  // #7aa2f7
    inline const ImVec4& accent_hi() { static const ImVec4 v{ 0.596f, 0.722f, 1.000f, 1.00f }; return v; }  // #98b8ff
    inline const ImVec4& green()     { static const ImVec4 v{ 0.620f, 0.808f, 0.416f, 1.00f }; return v; }  // #9ece6a
    inline const ImVec4& amber()     { static const ImVec4 v{ 0.878f, 0.686f, 0.408f, 1.00f }; return v; }  // #e0af68
    inline const ImVec4& red()       { static const ImVec4 v{ 0.969f, 0.463f, 0.557f, 1.00f }; return v; }  // #f7768e
    inline const ImVec4& cyan()      { static const ImVec4 v{ 0.490f, 0.812f, 1.000f, 1.00f }; return v; }  // #7dcfff
    inline const ImVec4& orange()    { static const ImVec4 v{ 1.000f, 0.620f, 0.392f, 1.00f }; return v; }  // #ff9e64
    inline const ImVec4& purple()    { static const ImVec4 v{ 0.733f, 0.604f, 0.969f, 1.00f }; return v; }  // #bb9af7
    inline const ImVec4& text()      { static const ImVec4 v{ 0.753f, 0.792f, 0.961f, 1.00f }; return v; }  // #c0caf5
    inline const ImVec4& muted()     { static const ImVec4 v{ 0.451f, 0.478f, 0.635f, 1.00f }; return v; }  // #737aa2
    inline const ImVec4& muted_dim() { static const ImVec4 v{ 0.337f, 0.373f, 0.537f, 1.00f }; return v; }  // #565f89

    // ── semantic aliases (use these in UI code) ───────────────────────────────
    inline const ImVec4& heading() { return text(); }       // window / section titles = bright text, hierarchy from position
    inline const ImVec4& link()    { return accent_hi(); }  // clickable type / address
    inline const ImVec4& success() { return green(); }
    inline const ImVec4& danger()  { return red(); }
    inline const ImVec4& warning() { return amber(); }
    inline const ImVec4& info()    { return cyan(); }
    inline const ImVec4& faint()   { return muted_dim(); }
    // kept for the many existing call sites that meant "section/window title".
    inline const ImVec4& header()  { return heading(); }

    // ── field / call-result value color, by content ───────────────────────────
    inline const ImVec4& value(const std::string& v)
    {
        if (v == "null")                    return muted();     // dim
        if (v == "true" || v == "false")    return amber();     // bool
        if (!v.empty() && v.front() == '"') return green();     // string
        if (!v.empty() && v.front() == '<') return cyan();      // ref
        return ImGui::GetStyleColorVec4(ImGuiCol_Text);         // number / default
    }

    // ── member access-visibility color ────────────────────────────────────────
    inline const ImVec4& visibility(std::uint16_t f)
    {
        if (f & 0x0001u) return green();   // public
        if (f & 0x0002u) return red();     // private
        if (f & 0x0004u) return amber();   // protected
        return muted();                    // package-private
    }

    // ── class-kind badge color ────────────────────────────────────────────────
    inline const ImVec4& kind_annotation() { return purple(); }
    inline const ImVec4& kind_interface()  { return cyan(); }
    inline const ImVec4& kind_enum()       { return amber(); }
    inline const ImVec4& kind_record()     { return green(); }
    inline const ImVec4& kind_abstract()   { return orange(); }
    inline const ImVec4& kind_class()      { static const ImVec4 v{ 0.560f, 0.640f, 0.820f, 1.00f }; return v; }  // calm slate-blue

    // ── status pill colors ────────────────────────────────────────────────────
    inline const ImVec4& status_idle()      { return muted(); }
    inline const ImVec4& status_injecting() { return orange(); }
    inline const ImVec4& status_receiving() { return accent(); }
    inline const ImVec4& status_done()      { return green(); }
    inline const ImVec4& status_error()     { return red(); }
}
