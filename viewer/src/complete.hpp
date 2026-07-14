// vmhook viewer — code-completion index for the Scripts editor.
//
// No language server: the completion database is built from the SAME artifact
// the script #includes (the generated wrapper header), so every suggestion is
// exactly an identifier the user can type and that compiles.  Plus a static set
// of C++ keywords and the vmhook/script API.  Flat, sorted, prefix+substring
// filtered — simple and robust.

#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace complete
{
    enum class Kind { Keyword, Api, Namespace, Class, Member };

    struct Symbol
    {
        std::string name;    // the typeable identifier
        std::string detail;  // dim hint shown to the right (kind / signature)
        Kind        kind{ Kind::Member };
    };

    struct Index
    {
        std::vector<Symbol> all;  // sorted case-insensitively, deduped
    };

    namespace detail
    {
        inline std::string lower(std::string s)
        {
            for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }
        inline bool is_ident(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

        // The identifier that immediately follows `key` on the line (skipping one
        // space), read up to the first non-identifier char.  "" if not present.
        inline std::string ident_after(std::string_view line, std::string_view key)
        {
            const std::size_t k{ line.find(key) };
            if (k == std::string_view::npos) return {};
            std::size_t i{ k + key.size() };
            while (i < line.size() && line[i] == ' ') ++i;
            const std::size_t s{ i };
            while (i < line.size() && is_ident(line[i])) ++i;
            return std::string{ line.substr(s, i - s) };
        }
    }

    // Build the index from the generated wrapper header text (+ static sets).
    inline Index build_from_header(const std::string& header)
    {
        Index idx;
        std::unordered_set<std::string> seen;
        const auto add{ [&](std::string n, Kind k, const char* det)
        {
            if (n.empty() || !seen.insert(n).second) return;
            idx.all.push_back(Symbol{ std::move(n), det ? det : "", k });
        } };

        static const char* const kw[]{
            "auto", "void", "bool", "int", "float", "double", "const", "static", "return",
            "if", "else", "for", "while", "struct", "class", "namespace", "using", "nullptr",
            "true", "false", "unsigned", "char", "short", "long", "std", "string",
            "int32_t", "int64_t", "int16_t", "int8_t", "uint16_t", "size_t", "noexcept",
            "constexpr", "include", "unique_ptr", "make_unique", "move",
        };
        for (const char* k : kw) add(k, Kind::Keyword, "keyword");

        struct Api { const char* n; const char* d; };
        static const Api api[]{
            { "vmhook", "namespace" },
            { "hook", "hook<T>(name, sig, detour)" },
            { "return_value", "vmhook::return_value&" },
            { "object", "vmhook::object<T>" },
            { "oop_t", "vmhook::oop_t" },
            { "register_class", "register_class<T>(name)" },
            { "shutdown_hooks", "shutdown_hooks()" },
            { "find_class", "find_class(name)" },
            { "script", "namespace" },
            { "log", "script::log(msg)" },
            { "wait_for_vm", "script::wait_for_vm(ms)" },
            { "script_setup", "void script_setup()" },
            { "register_all", "register_all()" },
            { "set", "return_value::set(v)" },
            { "cancel", "return_value::cancel()" },
        };
        for (const Api& a : api) add(a.n, Kind::Api, a.d);

        // Scan the header for namespace segments, class leaves, member accessors.
        std::size_t p{ 0 };
        while (p < header.size())
        {
            const std::size_t nl{ header.find('\n', p) };
            const std::string_view line{ header.data() + p, (nl == std::string::npos ? header.size() : nl) - p };
            p = (nl == std::string::npos ? header.size() : nl + 1);

            std::string_view trimmed{ line };
            while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t')) trimmed.remove_prefix(1);

            // namespace a::b::c { ...   -> add each path segment
            if (trimmed.rfind("namespace ", 0) == 0)
            {
                std::size_t i{ 10 };
                std::string seg;
                for (; i < trimmed.size(); ++i)
                {
                    const char ch{ trimmed[i] };
                    if (detail::is_ident(ch)) seg.push_back(ch);
                    else { if (!seg.empty()) add(seg, Kind::Namespace, "namespace"); seg.clear(); if (ch == '{' || ch == ';') break; }
                }
            }

            // class Leaf   (forward decl "class Leaf;" or "class Leaf : public ...")
            if (trimmed.find("class ") != std::string_view::npos)
            {
                const std::string leaf{ detail::ident_after(trimmed, "class ") };
                if (!leaf.empty()) add(leaf, Kind::Class, "class");
            }

            // member accessor: "[static ]auto <name>("
            if (trimmed.rfind("auto ", 0) == 0 || trimmed.rfind("static auto ", 0) == 0)
            {
                const std::string name{ detail::ident_after(trimmed, "auto ") };
                if (!name.empty() && trimmed.find('(') != std::string_view::npos)
                    add(name, Kind::Member, "member");
            }
        }

        std::sort(idx.all.begin(), idx.all.end(),
                  [](const Symbol& a, const Symbol& b) { return detail::lower(a.name) < detail::lower(b.name); });
        return idx;
    }

    // Prefix matches (case-insensitive) first, then substring matches, up to max.
    inline std::vector<int> filter(const Index& idx, const std::string& token, int max)
    {
        std::vector<int> out;
        if (token.empty()) return out;
        const std::string t{ detail::lower(token) };
        for (int i = 0; i < static_cast<int>(idx.all.size()) && static_cast<int>(out.size()) < max; ++i)
            if (detail::lower(idx.all[i].name).rfind(t, 0) == 0) out.push_back(i);
        if (static_cast<int>(out.size()) < max)
            for (int i = 0; i < static_cast<int>(idx.all.size()) && static_cast<int>(out.size()) < max; ++i)
            {
                const std::string ln{ detail::lower(idx.all[i].name) };
                if (ln.rfind(t, 0) != 0 && ln.find(t) != std::string::npos) out.push_back(i);
            }
        return out;
    }
}
