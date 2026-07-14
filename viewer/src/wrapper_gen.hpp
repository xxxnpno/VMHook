// vmhook viewer — JVM-to-C++ wrapper generator.
//
// Turns the live JVM's enumerated class surface (viewer::ClassInfo) into a
// compile-ready, single-header C++ wrapper in the same style as the hand-written
// npnoqol SDK:  each Java class becomes a `class X : public vmhook::object<X>`
// with `get_field(...)->get()` / `get_method(...,sig)->call(...)` accessors.
//
// Pure string logic — no ImGui, no Win32 — so it can be unit-tested standalone.
// The only dependency is the plain-data viewer::ClassInfo/MethodInfo/FieldInfo.
//
// Mutual recursion between wrappers (A returns unique_ptr<B>, B returns
// unique_ptr<A>) is handled by splitting each class into (1) a forward
// declaration, (2) a body of member DECLARATIONS, and (3) out-of-line inline
// member DEFINITIONS emitted after every class is complete.

#pragma once

#include "app.hpp"  // viewer::ClassInfo / MethodInfo / FieldInfo

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wrapper
{
    enum class NameCase { Original, Snake, Camel, Pascal };
    enum class NsLayout { Nested, Flat };

    struct Options
    {
        std::string root_namespace{ "jvm" };
        NsLayout    ns_layout{ NsLayout::Nested };
        NameCase    type_case{ NameCase::Pascal };
        NameCase    member_case{ NameCase::Snake };
        std::string getter_prefix{ "get_" };
        std::string setter_prefix{ "set_" };
        bool        emit_setters{ true };
        bool        include_methods{ true };
        bool        include_fields{ true };
        bool        include_jdk{ true };     // java*/javax*/sun*/jdk*/com.sun*
        bool        public_only{ false };
        std::string include_prefixes{};      // whitespace/comma-separated internal-name prefixes; empty = all
        std::string exclude_prefixes{};
        int         max_call_slots{ 8 };     // method_proxy::call packs at most params[8]
    };

    struct Stats
    {
        int classes_emitted{ 0 };
        int methods_emitted{ 0 };
        int fields_emitted{ 0 };
        int classes_skipped{ 0 };
        int members_skipped{ 0 };
    };

    struct Result
    {
        std::string              header;
        Stats                    stats;
        std::vector<std::string> notes;
    };

    namespace detail
    {
        inline auto is_cpp_keyword(std::string_view s) -> bool
        {
            static const std::unordered_set<std::string_view> kw{
                "alignas","alignof","and","and_eq","asm","atomic_cancel","atomic_commit",
                "atomic_noexcept","auto","bitand","bitor","bool","break","case","catch",
                "char","char8_t","char16_t","char32_t","class","compl","concept","const",
                "consteval","constexpr","constinit","const_cast","continue","co_await",
                "co_return","co_yield","decltype","default","delete","do","double",
                "dynamic_cast","else","enum","explicit","export","extern","false","float",
                "for","friend","goto","if","inline","int","long","mutable","namespace",
                "new","noexcept","not","not_eq","nullptr","operator","or","or_eq","private",
                "protected","public","register","reinterpret_cast","requires","return",
                "short","signed","sizeof","static","static_assert","static_cast","struct",
                "switch","template","this","thread_local","throw","true","try","typedef",
                "typeid","typename","union","unsigned","using","virtual","void","volatile",
                "wchar_t","while","xor","xor_eq",
            };
            return kw.count(s) != 0;
        }

        // Make a valid, non-keyword C++ identifier out of an arbitrary string.
        inline auto sanitize(std::string s) -> std::string
        {
            if (s.empty()) return "_";
            for (char& c : s)
            {
                const unsigned char u{ static_cast<unsigned char>(c) };
                if (!(std::isalnum(u) || c == '_')) c = '_';
            }
            if (std::isdigit(static_cast<unsigned char>(s.front()))) s.insert(s.begin(), '_');
            if (is_cpp_keyword(s)) s += '_';
            return s;
        }

        // Split a Java identifier into words on separators and case transitions:
        // "getHTTPResponseCode" -> [get, HTTP, Response, Code]; "pos_x" -> [pos, x].
        inline auto tokenize(std::string_view name) -> std::vector<std::string>
        {
            std::vector<std::string> out;
            std::string cur;
            const auto flush{ [&] { if (!cur.empty()) { out.push_back(cur); cur.clear(); } } };
            for (std::size_t i = 0; i < name.size(); ++i)
            {
                const unsigned char c{ static_cast<unsigned char>(name[i]) };
                if (!std::isalnum(c)) { flush(); continue; }
                if (!cur.empty())
                {
                    const unsigned char prev{ static_cast<unsigned char>(cur.back()) };
                    const bool camel{ (std::islower(prev) || std::isdigit(prev)) && std::isupper(c) };
                    const bool acronym{ std::isupper(prev) && std::isupper(c)
                                        && i + 1 < name.size()
                                        && std::islower(static_cast<unsigned char>(name[i + 1])) };
                    if (camel || acronym) flush();
                }
                cur.push_back(static_cast<char>(c));
            }
            flush();
            return out;
        }

        inline auto lower(std::string s) -> std::string
        {
            for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        inline auto cap(std::string s) -> std::string
        {
            s = lower(std::move(s));
            if (!s.empty()) s.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(s.front())));
            return s;
        }

        inline auto apply_case(std::string_view name, NameCase mode) -> std::string
        {
            if (mode == NameCase::Original) return sanitize(std::string{ name });
            const std::vector<std::string> words{ tokenize(name) };
            if (words.empty()) return sanitize(std::string{ name });
            std::string out;
            switch (mode)
            {
            case NameCase::Snake:
                for (std::size_t i = 0; i < words.size(); ++i) { if (i) out += '_'; out += lower(words[i]); }
                break;
            case NameCase::Camel:
                for (std::size_t i = 0; i < words.size(); ++i) out += (i == 0 ? lower(words[i]) : cap(words[i]));
                break;
            case NameCase::Pascal:
                for (const auto& w : words) out += cap(w);
                break;
            default:
                out.assign(name);
                break;
            }
            return sanitize(out);
        }

        // object_base / object<> members our generated bodies call unqualified
        // (get_field/get_method/…).  A Java member whose C++ name collides would
        // hide them and break `get_field("x")` inside the same class — suffix '_'.
        inline auto avoid_reserved(std::string name) -> std::string
        {
            static const std::unordered_set<std::string_view> reserved{
                "get_instance", "get_field", "get_method", "static_field", "static_method",
            };
            if (reserved.count(name)) name += '_';
            return name;
        }

        inline auto cpp_primitive(char c) -> std::string
        {
            switch (c)
            {
            case 'Z': return "bool";
            case 'B': return "std::int8_t";
            case 'C': return "std::uint16_t";
            case 'S': return "std::int16_t";
            case 'I': return "std::int32_t";
            case 'J': return "std::int64_t";
            case 'F': return "float";
            case 'D': return "double";
            case 'V': return "void";
            default:  return {};
            }
        }

        // A parsed JVM type from a descriptor position.
        struct JType
        {
            enum Kind { Void, Prim, Str, Ref, Array, Bad } kind{ Bad };
            std::string prim;  // for Prim
            std::string ref;   // internal name for Ref ("net/minecraft/.../Foo")
            int         slots{ 1 };
        };

        // Consume one type starting at d[i], advancing i past it.
        inline auto parse_one(std::string_view d, std::size_t& i) -> JType
        {
            int arrays{ 0 };
            while (i < d.size() && d[i] == '[') { ++arrays; ++i; }
            if (i >= d.size()) return JType{ JType::Bad };
            const char c{ d[i] };
            if (arrays > 0)
            {
                // Skip past the element type; arrays are not typed by the wrapper.
                if (c == 'L') { const std::size_t semi{ d.find(';', i) }; i = (semi == std::string_view::npos ? d.size() : semi + 1); }
                else ++i;
                return JType{ JType::Array };
            }
            switch (c)
            {
            case 'V': ++i; return JType{ JType::Void, {}, {}, 0 };
            case 'Z': case 'B': case 'C': case 'S': case 'I': case 'F':
                ++i; return JType{ JType::Prim, cpp_primitive(c), {}, 1 };
            case 'J': case 'D':
                ++i; return JType{ JType::Prim, cpp_primitive(c), {}, 2 };
            case 'L':
            {
                const std::size_t semi{ d.find(';', i) };
                if (semi == std::string_view::npos) { i = d.size(); return JType{ JType::Bad }; }
                std::string ref{ d.substr(i + 1, semi - i - 1) };
                i = semi + 1;
                if (ref == "java/lang/String") return JType{ JType::Str, {}, {}, 1 };
                return JType{ JType::Ref, {}, std::move(ref), 1 };
            }
            default: ++i; return JType{ JType::Bad };
            }
        }
    }

    // Build the fully-qualified C++ wrapper name for every class that will be
    // emitted, so cross-references resolve.  Returns internal-name -> FQN and the
    // per-class namespace path + leaf, keyed by internal name.
    struct NameTable
    {
        std::unordered_map<std::string, std::string> fqn;         // "net/.../Foo" -> "jvm::net::...::Foo"
        std::unordered_map<std::string, std::string> leaf;        // -> "Foo"
        std::unordered_map<std::string, std::vector<std::string>> ns;  // -> ["net","minecraft",...]
    };

    namespace detail
    {
        inline auto passes_filter(const std::string& internal, const Options& opt) -> bool
        {
            const auto starts_any{ [&](const std::string& list) -> bool
            {
                bool any{ false };
                std::string tok;
                const auto check{ [&](const std::string& t) -> bool
                {
                    if (t.empty()) return false;
                    std::string norm{ t };
                    for (char& c : norm) if (c == '.') c = '/';
                    return internal.rfind(norm, 0) == 0;
                } };
                for (char c : list)
                {
                    if (c == ',' || c == ' ' || c == '\n' || c == '\r' || c == '\t')
                    { if (check(tok)) return true; if (!tok.empty()) any = true; tok.clear(); }
                    else tok.push_back(c);
                }
                if (check(tok)) return true;
                (void)any;
                return false;
            } };

            if (!opt.include_jdk)
            {
                static const char* jdk[]{ "java/", "javax/", "sun/", "jdk/", "com/sun/", "jdk.internal", "com/oracle/" };
                for (const char* p : jdk) if (internal.rfind(p, 0) == 0) return false;
            }
            const bool has_inc{ opt.include_prefixes.find_first_not_of(" ,\t\r\n") != std::string::npos };
            if (has_inc && !starts_any(opt.include_prefixes)) return false;
            const bool has_exc{ opt.exclude_prefixes.find_first_not_of(" ,\t\r\n") != std::string::npos };
            if (has_exc && starts_any(opt.exclude_prefixes)) return false;
            return true;
        }

        inline auto split_internal(const std::string& internal, std::string& pkg, std::string& simple) -> void
        {
            const std::size_t slash{ internal.find_last_of('/') };
            if (slash == std::string::npos) { pkg.clear(); simple = internal; }
            else { pkg = internal.substr(0, slash); simple = internal.substr(slash + 1); }
        }
    }

    inline auto build_name_table(const std::vector<viewer::ClassInfo>& classes, const Options& opt) -> NameTable
    {
        NameTable t;
        // Deterministic order so disambiguation suffixes are stable.
        std::vector<const viewer::ClassInfo*> ptrs;
        ptrs.reserve(classes.size());
        for (const auto& c : classes)
            if (!c.internal_name.empty() && detail::passes_filter(c.internal_name, opt)) ptrs.push_back(&c);
        std::sort(ptrs.begin(), ptrs.end(), [](auto* a, auto* b) { return a->internal_name < b->internal_name; });

        // Track used leaf names per namespace path to disambiguate collisions.
        std::unordered_map<std::string, std::unordered_set<std::string>> used;

        for (const auto* c : ptrs)
        {
            std::string pkg, simple;
            detail::split_internal(c->internal_name, pkg, simple);

            std::vector<std::string> nspath;
            std::string ns_key;
            if (opt.ns_layout == NsLayout::Nested && !pkg.empty())
            {
                std::string seg;
                for (char ch : pkg)
                {
                    if (ch == '/') { nspath.push_back(detail::sanitize(detail::lower(seg))); seg.clear(); }
                    else seg.push_back(ch);
                }
                if (!seg.empty()) nspath.push_back(detail::sanitize(detail::lower(seg)));
            }
            for (const auto& s : nspath) { ns_key += s; ns_key += "::"; }

            std::string leaf{ detail::apply_case(simple, opt.type_case) };
            auto& taken{ used[ns_key] };
            if (taken.count(leaf))
            {
                int n{ 2 };
                std::string cand;
                do { cand = leaf + "_" + std::to_string(n++); } while (taken.count(cand));
                leaf = cand;
            }
            taken.insert(leaf);

            std::string fqn{ opt.root_namespace };
            fqn += "::";
            for (const auto& s : nspath) { fqn += s; fqn += "::"; }
            fqn += leaf;

            t.fqn.emplace(c->internal_name, fqn);
            t.leaf.emplace(c->internal_name, leaf);
            t.ns.emplace(c->internal_name, std::move(nspath));
        }
        return t;
    }

    namespace detail
    {
        // One emitted member: its in-class declaration and out-of-line definition.
        struct Member
        {
            std::string decl;
            std::string def;
            std::string sig_key;  // for de-duplication within a class
        };

        inline auto escape(const std::string& s) -> std::string
        {
            std::string out;
            for (char c : s) { if (c == '\\' || c == '"') out += '\\'; out += c; }
            return out;
        }

        // Build a method member, or return false to skip it.
        inline auto build_method(const viewer::MethodInfo& m, const std::string& class_fqn,
                                 const NameTable& t, const Options& opt, Member& out) -> bool
        {
            if (m.name.empty() || m.name.front() == '<') return false;               // <init>/<clinit>
            if (opt.public_only && !(m.access & 0x0001u)) return false;
            if (m.access & 0x1000u) return false;                                     // ACC_SYNTHETIC
            const bool is_static{ (m.access & 0x0008u) != 0 };

            const std::string_view d{ m.descriptor };
            if (d.empty() || d.front() != '(') return false;
            std::size_t i{ 1 };
            std::vector<JType> params;
            while (i < d.size() && d[i] != ')')
            {
                JType p{ parse_one(d, i) };
                if (p.kind == JType::Bad || p.kind == JType::Void) return false;
                if (p.kind == JType::Array) return false;                            // arrays unsupported
                if (p.kind == JType::Ref && !t.fqn.count(p.ref)) return false;        // ref to unwrapped class
                params.push_back(std::move(p));
            }
            if (i >= d.size()) return false;
            ++i;  // ')'
            JType ret{ parse_one(d, i) };
            if (ret.kind == JType::Bad || ret.kind == JType::Array) return false;
            if (ret.kind == JType::Ref && !t.fqn.count(ret.ref)) return false;

            int slots{ is_static ? 0 : 1 };
            for (const auto& p : params) slots += p.slots;
            if (slots > opt.max_call_slots) return false;

            const auto cpp_ret{ [&]() -> std::string
            {
                switch (ret.kind)
                {
                case JType::Void: return "void";
                case JType::Prim: return ret.prim;
                case JType::Str:  return "std::string";
                case JType::Ref:  return "std::unique_ptr<" + t.fqn.at(ret.ref) + ">";
                default:          return "void";
                }
            }() };

            std::string decl_params, call_args, sig_key;
            for (std::size_t k = 0; k < params.size(); ++k)
            {
                const JType& p{ params[k] };
                std::string ty;
                switch (p.kind)
                {
                case JType::Prim: ty = p.prim; break;
                case JType::Str:  ty = "const std::string&"; break;
                case JType::Ref:  ty = "const std::unique_ptr<" + t.fqn.at(p.ref) + ">&"; break;
                default:          return false;
                }
                if (k) { decl_params += ", "; call_args += ", "; sig_key += ","; }
                const std::string argn{ "arg" + std::to_string(k) };
                decl_params += ty + " " + argn;
                call_args += argn;
                sig_key += ty;
            }

            const std::string name{ avoid_reserved(apply_case(m.name, opt.member_case)) };
            // Unified dedup key: C++ member name + parameter types.  A method and a
            // field accessor that collapse to the same C++ signature (e.g. method
            // getName() and field `name` -> get_name()) share this key, so the
            // second is skipped instead of emitting a duplicate declaration.
            out.sig_key = name + "(" + sig_key + ")";

            const std::string qual{ is_static ? "static " : "" };
            const std::string cst{ is_static ? "" : " const" };
            out.decl = qual + "auto " + name + "(" + decl_params + ")" + cst + " noexcept -> " + cpp_ret + ";";

            const std::string getter{ is_static ? "static_method" : "get_method" };
            std::string body{ getter + "(\"" + escape(m.name) + "\", \"" + escape(m.descriptor) + "\")->call(" + call_args + ")" };
            std::string def{ "inline auto " + class_fqn + "::" + name + "(" + decl_params + ")" + cst + " noexcept -> " + cpp_ret + "\n    { " };
            def += (ret.kind == JType::Void ? "" : "return ");
            def += body + "; }";
            out.def = def;
            return true;
        }

        // Build the getter (+ optional setter) for one field, or return false.
        inline auto build_field(const viewer::FieldInfo& f, const std::string& class_fqn,
                                const NameTable& t, const Options& opt,
                                Member& getter, Member& setter, bool& has_setter) -> bool
        {
            has_setter = false;
            if (f.name.empty()) return false;
            if (opt.public_only && !(f.access & 0x0001u)) return false;
            if (f.access & 0x1000u) return false;  // ACC_SYNTHETIC

            std::size_t i{ 0 };
            JType ty{ parse_one(f.descriptor, i) };
            if (ty.kind == JType::Bad || ty.kind == JType::Void || ty.kind == JType::Array) return false;
            if (ty.kind == JType::Ref && !t.fqn.count(ty.ref)) return false;

            const bool is_static{ f.is_static };
            const auto cpp_ty{ [&]() -> std::string
            {
                switch (ty.kind)
                {
                case JType::Prim: return ty.prim;
                case JType::Str:  return "std::string";
                case JType::Ref:  return "std::unique_ptr<" + t.fqn.at(ty.ref) + ">";
                default:          return "void";
                }
            }() };

            const std::string base{ apply_case(f.name, opt.member_case) };
            const std::string gname{ avoid_reserved(opt.getter_prefix + base) };
            const std::string qual{ is_static ? "static " : "" };
            const std::string cst{ is_static ? "" : " const" };
            const std::string accessor{ is_static ? "static_field" : "get_field" };

            getter.sig_key = gname + "()";
            getter.decl = qual + "auto " + gname + "()" + cst + " noexcept -> " + cpp_ty + ";";
            getter.def  = "inline auto " + class_fqn + "::" + gname + "()" + cst + " noexcept -> " + cpp_ty
                        + "\n    { return " + accessor + "(\"" + escape(f.name) + "\")->get(); }";

            if (opt.emit_setters)
            {
                const std::string sname{ avoid_reserved(opt.setter_prefix + base) };
                std::string pty;
                switch (ty.kind)
                {
                case JType::Prim: pty = ty.prim; break;
                case JType::Str:  pty = "const std::string&"; break;
                case JType::Ref:  pty = "const " + cpp_ty + "&"; break;
                default: pty.clear(); break;
                }
                if (!pty.empty())
                {
                    setter.sig_key = sname + "(" + pty + ")";
                    setter.decl = qual + "auto " + sname + "(" + pty + " value)" + cst + " noexcept -> void;";
                    setter.def  = "inline auto " + class_fqn + "::" + sname + "(" + pty + " value)" + cst
                                + " noexcept -> void\n    { " + accessor + "(\"" + escape(f.name) + "\")->set(value); }";
                    has_setter = true;
                }
            }
            return true;
        }
    }

    // The whole generation pass.  Produces one amalgamated header.
    inline auto generate(const std::vector<viewer::ClassInfo>& classes, const Options& opt) -> Result
    {
        Result r;
        const NameTable t{ build_name_table(classes, opt) };

        // Deterministic emission order.
        std::vector<const viewer::ClassInfo*> ptrs;
        ptrs.reserve(classes.size());
        for (const auto& c : classes)
            if (t.fqn.count(c.internal_name)) ptrs.push_back(&c);
        std::sort(ptrs.begin(), ptrs.end(), [](auto* a, auto* b) { return a->internal_name < b->internal_name; });

        std::string& h{ r.header };
        h.reserve(1u << 20);
        h += "// Generated by vmhook viewer — do not edit by hand.\n";
        h += "// A C++ wrapper over the live JVM's class surface, built on vmhook.\n";
        h += "#pragma once\n\n";
        h += "#include <vmhook/vmhook.hpp>\n";
        h += "#include <cstdint>\n#include <memory>\n#include <string>\n\n";

        const std::string& root{ opt.root_namespace };

        // Helper to open/close a class's namespace path.
        const auto open_ns{ [&](const std::vector<std::string>& ns) -> std::string
        {
            std::string s{ "namespace " + root };
            for (const auto& p : ns) { s += "::"; s += p; }
            s += " { ";
            return s;
        } };

        // (1) Forward declarations.
        h += "// ── forward declarations ───────────────────────────────────────────\n";
        for (const auto* c : ptrs)
            h += open_ns(t.ns.at(c->internal_name)) + "class " + t.leaf.at(c->internal_name) + "; }\n";
        h += "\n";

        // (2) Class bodies (member declarations only) + collect (3) definitions.
        std::string defs;
        defs += "// ── out-of-line member definitions (all types complete here) ───────\n";

        h += "// ── class bodies ───────────────────────────────────────────────────\n";
        for (const auto* c : ptrs)
        {
            const std::string& fqn{ t.fqn.at(c->internal_name) };
            const std::string& leaf{ t.leaf.at(c->internal_name) };

            h += open_ns(t.ns.at(c->internal_name)) + "\n";
            h += "class " + leaf + " : public vmhook::object<" + leaf + ">\n{\npublic:\n";
            h += "    explicit " + leaf + "(vmhook::oop_t instance) noexcept : vmhook::object<" + leaf + ">{ instance } {}\n";

            std::unordered_set<std::string> seen;
            int emitted_here{ 0 };

            if (opt.include_methods)
            {
                for (const auto& m : c->methods)
                {
                    detail::Member mem;
                    if (!detail::build_method(m, fqn, t, opt, mem)) { ++r.stats.members_skipped; continue; }
                    if (!seen.insert(mem.sig_key).second) { ++r.stats.members_skipped; continue; }
                    h += "    " + mem.decl + "\n";
                    defs += mem.def + "\n";
                    ++r.stats.methods_emitted; ++emitted_here;
                }
            }
            if (opt.include_fields)
            {
                for (const auto& f : c->fields)
                {
                    detail::Member g, s;
                    bool has_setter{ false };
                    if (!detail::build_field(f, fqn, t, opt, g, s, has_setter)) { ++r.stats.members_skipped; continue; }
                    if (seen.insert(g.sig_key).second)
                    {
                        h += "    " + g.decl + "\n";
                        defs += g.def + "\n";
                        ++r.stats.fields_emitted; ++emitted_here;
                    }
                    if (has_setter && seen.insert(s.sig_key).second)
                    {
                        h += "    " + s.decl + "\n";
                        defs += s.def + "\n";
                    }
                }
            }

            (void)emitted_here;
            h += "};\n} // namespace\n\n";
            ++r.stats.classes_emitted;
        }

        // (3) The out-of-line definitions.
        h += "\n";
        h += defs;
        h += "\n";

        // (4) register_all() — associates every wrapper type with its Java class.
        h += "// ── registration ───────────────────────────────────────────────────\n";
        h += "namespace " + root + " {\ninline void register_all() noexcept\n{\n";
        for (const auto* c : ptrs)
            h += "    vmhook::register_class<" + t.fqn.at(c->internal_name) + ">(\"" + detail::escape(c->internal_name) + "\");\n";
        h += "}\n} // namespace " + root + "\n";

        r.stats.classes_skipped = static_cast<int>(classes.size()) - r.stats.classes_emitted;
        return r;
    }
}
