// JVM type-descriptor pretty-printer: turns "(ID)D" into "(int, double) : double"
// and "Ljava/lang/String;" into "String".  Pure string logic, no dependencies.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace viewer
{
    // JVM access flags -> "public static final" etc.
    inline auto access_modifiers(std::uint16_t f, bool method) -> std::string
    {
        std::string s;
        if      (f & 0x0001u) s += "public ";
        else if (f & 0x0002u) s += "private ";
        else if (f & 0x0004u) s += "protected ";
        if (f & 0x0008u) s += "static ";
        if (f & 0x0010u) s += "final ";
        if (method)
        {
            if (f & 0x0400u) s += "abstract ";
            if (f & 0x0100u) s += "native ";
            if (f & 0x0020u) s += "synchronized ";
        }
        else
        {
            if (f & 0x0040u) s += "volatile ";
            if (f & 0x0080u) s += "transient ";
        }
        if (!s.empty()) s.pop_back();
        return s;
    }

    inline auto pretty_one_type(std::string_view d, std::size_t& i, bool full = false) -> std::string
    {
        int arrays{ 0 };
        while (i < d.size() && d[i] == '[') { ++arrays; ++i; }
        std::string base;
        if (i >= d.size())
        {
            base = "?";
        }
        else
        {
            switch (d[i])
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
                const std::size_t semi{ d.find(';', i) };
                const std::string_view cls{ d.substr(i + 1, (semi == std::string_view::npos ? d.size() : semi) - i - 1) };
                if (full)
                {
                    base.assign(cls);
                    for (char& ch : base) { if (ch == '/') ch = '.'; }
                }
                else
                {
                    const std::size_t slash{ cls.find_last_of('/') };
                    base.assign(slash == std::string_view::npos ? cls : cls.substr(slash + 1));
                    for (char& ch : base) { if (ch == '$') ch = '.'; }
                }
                i = (semi == std::string_view::npos ? d.size() : semi + 1);
                break;
            }
            default: base = std::string(1, d[i]); ++i; break;
            }
        }
        for (int a = 0; a < arrays; ++a) { base += "[]"; }
        return base;
    }

    // "(ID)D" -> "(int, double) : double"
    inline auto pretty_method(std::string_view d, bool full = false) -> std::string
    {
        if (d.empty() || d[0] != '(')
        {
            return std::string{ d };
        }
        std::size_t i{ 1 };
        std::string params;
        bool first{ true };
        while (i < d.size() && d[i] != ')')
        {
            if (!first) { params += ", "; }
            params += pretty_one_type(d, i, full);
            first = false;
        }
        if (i < d.size()) { ++i; }  // skip ')'
        const std::string ret{ pretty_one_type(d, i, full) };
        return "(" + params + ") : " + ret;
    }

    // "Ljava/lang/String;" -> "String" ; "[I" -> "int[]"
    inline auto pretty_field(std::string_view d, bool full = false) -> std::string
    {
        std::size_t i{ 0 };
        return pretty_one_type(d, i, full);
    }
}
