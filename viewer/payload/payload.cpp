// vmhook viewer payload DLL.
//
// Injected (LoadLibrary via CreateRemoteThread) into a live HotSpot JVM by the
// vmhook viewer.  It uses vmhook — pure-VM, no JNI/JVMTI — to walk every loaded
// Java class and, per class, enumerate its declared methods and fields, then
// streams the whole surface back to the viewer over a named pipe.
//
// Wire protocol (UTF-8, '\n'-delimited, '\t'-separated; Java identifiers and JVM
// descriptors never contain '\t' or '\n', so the framing is unambiguous):
//   C <TAB> internal/Class/Name <TAB> super/Internal/Name <TAB> <classAccessFlags>
//   M <TAB> methodName <TAB> (descriptor)ret <TAB> <methodAccessFlags>
//   F <TAB> fieldName  <TAB> descriptor <TAB> <fieldAccessFlags>
//   O <TAB> 0x<addr>                      (start of a live instance, its oop address)
//   V <TAB> fieldName <TAB> value <TAB> owner <TAB> refAddr
//        (a field value of the current instance; owner = declaring simple name when
//         inherited, else empty; refAddr = 0x<oop> of the pointee for a non-null
//         reference field, else empty — lets the viewer "grab" the object)
//   R <TAB> display <TAB> kind <TAB> refAddr <TAB> refClass   (a CALL result)
//   Z <TAB> <I|S> <TAB> Class <TAB> addr <TAB> field <TAB> value  (a FLST freeze entry)
//   E <TAB> message                      (an error, for the SET / CALL requests)
//   DONE <TAB> <count>
// Methods/fields belong to the most recently emitted C record; V records to the
// most recent O.  Which stream is produced depends on the request the viewer
// leaves in %TEMP%\vmhook_viewer_req.txt before it opens the pipe:
//   "ENUM"          (or missing)  -> the full class surface (C/M/F).
//   "INST" <TAB> internal/Class/Name [<TAB> max]  -> live instances (O/V).
//   "STAT" <TAB> internal/Class/Name              -> static fields (one O + V's).
//   "SETI" <TAB> Class <TAB> 0xADDR <TAB> field <TAB> value -> write an instance
//                                                   field, then stream back V (re-read).
//   "SETS" <TAB> Class <TAB> field <TAB> value    -> write a static field, then V.
//   "FRZ"  <TAB> <I|S> <TAB> Class <TAB> 0xADDR <TAB> field <TAB> value -> freeze a
//                    field at `value` (a background thread re-writes it ~50Hz until
//                    unfrozen); addr empty for statics.  Replies V (re-read) + DONE.
//   "UNF"  <TAB> <key>            -> unfreeze one entry (key "<I|S>|Class|addr|field")
//                                    or "*" to clear all.  Replies DONE.
//   "FLST"                        -> stream current freeze entries (Z records) + DONE.
//   "CALL" <TAB> Class <TAB> <0xADDR|-> <TAB> method <TAB> descriptor <TAB> n
//                    <TAB> arg0 <TAB> arg1 ...  -> invoke a method and reply R + DONE.
//                    Each arg is a tagged token: a bare literal (primitive parsed by
//                    the descriptor), "@null", "@0x<oop>" (existing reference), or
//                    "#<text>" (a freshly-allocated java.lang.String).
//   "HOOK"          -> arm the ClassLoader.defineClass hook; reply H <TAB> <1|0>.
//   "DRAIN"         -> stream (and clear) hook-captured class names as N records:
//                        N <TAB> internal/Class/Name
// Access-flag fields are decimal JVM class-file flag words.
//
// Method invocation and building a new java.lang.String both need a live
// JavaThread, but the payload's serve thread is native.  ensure_java_attached()
// promotes it to a HotSpot JavaThread (AttachCurrentThreadAsDaemon via the JVM's
// own invocation interface, resolved from jvm.dll) the first time it is needed,
// so CALL / new-String writes actually run instead of being refused.

#include <vmhook/vmhook.hpp>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <jni.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <typeindex>
#include <unordered_set>
#include <vector>

namespace
{
    constexpr const wchar_t* k_pipe_name{ L"\\\\.\\pipe\\vmhook_viewer" };

    // ── running Java from the native serve thread, SAFELY ─────────────────────
    // method_proxy::call() and make_java_string() only work on a real JavaThread
    // that is *inside an interpreter detour* — that is the library's documented
    // contract (current_java_thread set, a valid last_Java_frame anchor, a
    // safepoint-safe transition).  Calling the raw call_stub from a plain attached
    // thread crashes the VM (no anchor -> a GC stack-walk faults).
    //
    // So we (1) promote the serve thread to a JavaThread via the JVM's own JNI
    // (AttachCurrentThreadAsDaemon — the only safe native->Java entry, it sets up
    // all the machinery), and (2) run every VM-executing task INSIDE a hook detour
    // by hooking a tiny static JDK method (java.lang.Runtime.getRuntime) and
    // firing it with a real JNI call.  The detour runs on a genuine JavaThread in
    // a valid Java context, so the queued task (a method call, or a String alloc)
    // executes on the proven path.  The hook is gated by a pending-task flag, so
    // any collateral getRuntime() caller just no-ops.

    JavaVM*                g_jvm{ nullptr };
    std::once_flag         g_jvm_once;
    thread_local JNIEnv*   t_env{ nullptr };

    auto ensure_java_attached() -> JNIEnv*
    {
        if (t_env) { return t_env; }
        std::call_once(g_jvm_once, []
        {
            HMODULE jvm{ GetModuleHandleW(L"jvm.dll") };
            if (!jvm) { return; }
            using get_created_fn = jint (JNICALL*)(JavaVM**, jsize, jsize*);
            auto* const get_created{ reinterpret_cast<get_created_fn>(
                reinterpret_cast<void*>(GetProcAddress(jvm, "JNI_GetCreatedJavaVMs"))) };
            if (!get_created) { return; }
            JavaVM* vm{ nullptr };
            jsize   count{ 0 };
            if (get_created(&vm, 1, &count) == JNI_OK && count > 0 && vm) { g_jvm = vm; }
        });
        if (!g_jvm) { return nullptr; }
        JNIEnv* env{ nullptr };
        if (g_jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK && env) { t_env = env; return env; }
        JavaVMAttachArgs args{ JNI_VERSION_1_6, const_cast<char*>("vmhook-viewer"), nullptr };
        if (g_jvm->AttachCurrentThreadAsDaemon(reinterpret_cast<void**>(&env), &args) == JNI_OK && env) { t_env = env; return env; }
        return nullptr;
    }

    // Wrapper so vmhook::hook can target java.lang.Runtime.getRuntime() — our
    // detour trigger.  Not otherwise used.
    class runtime_trigger : public vmhook::object<runtime_trigger>
    {
    public:
        explicit runtime_trigger(vmhook::oop_t o) noexcept : vmhook::object<runtime_trigger>{ o } {}
    };

    std::mutex                          g_task_mtx;
    std::condition_variable             g_task_cv;
    std::function<void()>               g_task;
    std::atomic<bool>                   g_task_pending{ false };
    bool                                g_task_done{ false };  // guarded by g_task_mtx
    std::optional<vmhook::hook_handle>  g_trigger_hook;
    std::once_flag                      g_trigger_once;
    bool                                g_trigger_ok{ false };

    void ensure_trigger_hook()
    {
        std::call_once(g_trigger_once, []
        {
            if (vmhook::type_to_class_map.find(std::type_index{ typeid(runtime_trigger) }) == vmhook::type_to_class_map.end())
            {
                vmhook::register_class<runtime_trigger>("java/lang/Runtime");
            }
            // getRuntime() is hot/JIT-compiled; deopt so our i2i detour fires.
            vmhook::deoptimize_methods_if([](const std::string& cn, vmhook::hotspot::method* m)
            { return cn == "java/lang/Runtime" && m && m->get_name() == "getRuntime"; });

            auto detour = [](vmhook::return_value&)
            {
                std::function<void()> job;
                {
                    std::lock_guard<std::mutex> lk{ g_task_mtx };
                    if (g_task_pending.load()) { job = std::move(g_task); g_task = nullptr; g_task_pending.store(false); }
                }
                if (job)
                {
                    try { job(); } catch (...) { /* keep the VM alive */ }
                    { std::lock_guard<std::mutex> lk{ g_task_mtx }; g_task_done = true; }
                    g_task_cv.notify_all();
                }
            };
            auto handle{ vmhook::scoped_hook<runtime_trigger>("getRuntime", "()Ljava/lang/Runtime;", detour) };
            if (handle.installed())
            {
                g_trigger_ok = true;
                g_trigger_hook.emplace(std::move(handle));  // keep installed
            }
        });
    }

    // Run `fn` on a real JavaThread inside the trigger detour (the only safe
    // context for method_proxy::call / make_java_string).  Blocks until it runs
    // or times out.  Returns false if the JVM is unreachable or the detour never
    // fired.
    auto run_on_java_thread(std::function<void()> fn) -> bool
    {
        JNIEnv* const env{ ensure_java_attached() };
        if (!env) { return false; }
        ensure_trigger_hook();
        if (!g_trigger_ok) { return false; }

        {
            std::lock_guard<std::mutex> lk{ g_task_mtx };
            g_task = std::move(fn);
            g_task_done = false;
            g_task_pending.store(true);
        }

        static jclass    s_cls{ nullptr };
        static jmethodID s_mid{ nullptr };
        if (!s_cls)
        {
            if (jclass c{ env->FindClass("java/lang/Runtime") })
            { s_cls = static_cast<jclass>(env->NewGlobalRef(c)); env->DeleteLocalRef(c); }
        }
        if (s_cls && !s_mid) { s_mid = env->GetStaticMethodID(s_cls, "getRuntime", "()Ljava/lang/Runtime;"); }
        if (env->ExceptionCheck()) { env->ExceptionClear(); }
        if (s_cls && s_mid)
        {
            jobject r{ env->CallStaticObjectMethod(s_cls, s_mid) };  // fires the detour
            if (env->ExceptionCheck()) { env->ExceptionClear(); }
            if (r) { env->DeleteLocalRef(r); }
        }

        std::unique_lock<std::mutex> lk{ g_task_mtx };
        g_task_cv.wait_for(lk, std::chrono::seconds(5), [] { return g_task_done; });
        const bool ran{ g_task_done };
        if (!ran) { g_task = nullptr; g_task_pending.store(false); }
        return ran;
    }

    // make_java_string, but on a real JavaThread (via the trigger detour).
    auto alloc_java_string(const std::string& text) -> void*
    {
        void* out{ nullptr };
        run_on_java_thread([&] { out = vmhook::make_java_string(text); });
        return out;
    }

    // Buffered pipe writer: accumulates records and flushes in ~64 KB chunks so
    // enumerating tens of thousands of classes doesn't do a syscall per line.
    struct pipe_writer
    {
        HANDLE      pipe{ INVALID_HANDLE_VALUE };
        std::string buffer{};

        void put(std::string_view text)
        {
            buffer.append(text);
            if (buffer.size() >= 64u * 1024u)
            {
                flush();
            }
        }

        void flush()
        {
            if (pipe == INVALID_HANDLE_VALUE || buffer.empty())
            {
                return;
            }
            const char* cursor{ buffer.data() };
            std::size_t remaining{ buffer.size() };
            while (remaining > 0u)
            {
                DWORD written{ 0 };
                if (!WriteFile(pipe, cursor, static_cast<DWORD>(remaining), &written, nullptr) || written == 0u)
                {
                    break;
                }
                cursor    += written;
                remaining -= written;
            }
            buffer.clear();
        }
    };

    void stream_to(HANDLE pipe)
    {
        pipe_writer writer{ pipe };
        std::uint64_t class_count{ 0 };

        // The ClassLoaderDataGraph walk can surface the same class name more than
        // once (bootstrap dictionary + _klasses list, distinct Klass* per node).
        // Dedup by internal name for a clean, unique class list.
        std::unordered_set<std::string> seen{};

        vmhook::for_each_loaded_class(
            [&](const std::string& name, vmhook::hotspot::klass* const klass)
            {
                if (!klass || !vmhook::hotspot::is_valid_pointer(klass) || name.empty())
                {
                    return;
                }
                if (!seen.insert(name).second)
                {
                    return;
                }
                ++class_count;

                writer.put("C\t");
                writer.put(name);
                writer.put("\t");
                // Superclass internal name (empty for java/lang/Object / interfaces).
                if (vmhook::hotspot::klass* const super_klass{ klass->get_super() };
                    super_klass && vmhook::hotspot::is_valid_pointer(super_klass))
                {
                    if (const vmhook::hotspot::symbol* const super_name{ super_klass->get_name() };
                        super_name && vmhook::hotspot::is_valid_pointer(super_name))
                    {
                        writer.put(super_name->to_string());
                    }
                }
                writer.put("\t");
                // Class-file access flags (interface/enum/abstract/... kind + vis).
                writer.put(std::to_string(klass->get_class_access_flags()));
                writer.put("\n");

                // Declared methods: (name, descriptor, access flags).  Array
                // klasses have no _methods array, so skip them ('[' names).
                if (name.empty() || name.front() != '[')
                {
                    const std::int32_t method_count{ klass->get_methods_count() };
                    vmhook::hotspot::method** const methods{ klass->get_methods_ptr() };
                    if (methods && method_count > 0)
                    {
                        for (std::int32_t mi = 0; mi < method_count; ++mi)
                        {
                            vmhook::hotspot::method* const m{ methods[mi] };
                            if (!m || !vmhook::hotspot::is_valid_pointer(m))
                            {
                                continue;
                            }
                            std::uint32_t flags{ 0 };
                            if (std::uint32_t* const fp{ m->get_access_flags() })
                            {
                                vmhook::os::safe_read(&flags, fp, sizeof(flags));
                            }
                            writer.put("M\t");
                            writer.put(m->get_name());
                            writer.put("\t");
                            writer.put(m->get_signature());
                            writer.put("\t");
                            writer.put(std::to_string(flags));
                            writer.put("\n");
                        }
                    }
                }

                // Declared fields: (name, descriptor, access flags).
                for (const auto& [field_name, field_descriptor, access_flags] : klass->collect_fields())
                {
                    writer.put("F\t");
                    writer.put(field_name);
                    writer.put("\t");
                    writer.put(field_descriptor);
                    writer.put("\t");
                    writer.put(std::to_string(access_flags));
                    writer.put("\n");
                }
            });

        writer.put("DONE\t");
        writer.put(std::to_string(class_count));
        writer.put("\n");
        writer.flush();
        FlushFileBuffers(pipe);
    }

    // Read (and consume) the one-line request the viewer left in
    // %TEMP%\vmhook_viewer_req.txt.  Missing / empty -> "ENUM", so the CLI/MCP
    // (which never writes one) and every plain attach keep getting the class
    // surface.  The file is deleted after reading so a stale INST request can't
    // leak into the next enumerate.
    auto read_request() -> std::string
    {
        wchar_t tmp[MAX_PATH]{};
        const DWORD n{ GetTempPathW(MAX_PATH, tmp) };
        if (n == 0 || n >= MAX_PATH)
        {
            return "ENUM";
        }
        std::wstring path{ tmp, n };
        path += L"vmhook_viewer_req.txt";

        HANDLE f{ CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, 0, nullptr) };
        if (f == INVALID_HANDLE_VALUE)
        {
            return "ENUM";
        }
        std::string req{};
        char buf[1024];
        DWORD rd{ 0 };
        while (ReadFile(f, buf, sizeof(buf), &rd, nullptr) && rd > 0)
        {
            req.append(buf, rd);
        }
        CloseHandle(f);
        DeleteFileW(path.c_str());

        while (!req.empty() && (req.back() == '\n' || req.back() == '\r' || req.back() == ' '))
        {
            req.pop_back();
        }
        return req.empty() ? std::string{ "ENUM" } : req;
    }

    // Tab/newline are legal in some obfuscated identifiers and in String values;
    // they'd corrupt the tab/newline framing, so flatten them to spaces (this is
    // display data, so lossy sanitising is fine).
    auto sanitize(std::string s) -> std::string
    {
        for (char& c : s)
        {
            if (c == '\t' || c == '\n' || c == '\r')
            {
                c = ' ';
            }
        }
        return s;
    }

    // Read one instance field's value and format it for display.  All reads go
    // through the fault-safe vmhook accessors (get_field returns a default on any
    // failure), so a stale/garbage oop from the conservative scan can't fault.
    // ref_out (optional): for a non-null reference field, receives the pointee's
    // heap address as "0x..." — the viewer uses it to "grab" the object into its
    // clipboard; left empty for primitives / null.
    auto read_field_value(void* const oop, vmhook::hotspot::klass* const k,
                          const std::string& name, const std::string& desc,
                          std::string* const ref_out = nullptr) -> std::string
    {
        if (ref_out) { ref_out->clear(); }
        if (desc.empty())
        {
            return "?";
        }
        switch (desc[0])
        {
        case 'Z': return vmhook::get_field<std::uint8_t>(oop, k, name) != 0 ? "true" : "false";
        case 'B': return std::to_string(static_cast<int>(vmhook::get_field<std::int8_t>(oop, k, name)));
        case 'C':
        {
            const std::uint16_t ch{ vmhook::get_field<std::uint16_t>(oop, k, name) };
            if (ch >= 32u && ch < 127u) { return std::string{ "'" } + static_cast<char>(ch) + "'"; }
            return std::to_string(static_cast<unsigned>(ch));
        }
        case 'S': return std::to_string(static_cast<int>(vmhook::get_field<std::int16_t>(oop, k, name)));
        case 'I': return std::to_string(vmhook::get_field<std::int32_t>(oop, k, name));
        case 'F': return std::to_string(vmhook::get_field<float>(oop, k, name));
        case 'J': return std::to_string(vmhook::get_field<std::int64_t>(oop, k, name));
        case 'D': return std::to_string(vmhook::get_field<double>(oop, k, name));
        case 'L':
        case '[':
        {
            // Reference field: read the (compressed) oop, decode, and describe it.
            const std::uint32_t compressed{ vmhook::get_field<std::uint32_t>(oop, k, name) };
            if (compressed == 0u)
            {
                return "null";
            }
            void* const ref{ vmhook::hotspot::decode_oop_pointer(compressed) };
            if (!ref || !vmhook::hotspot::is_valid_pointer(ref))
            {
                return "null";
            }
            if (ref_out)
            {
                char rb[32];
                std::snprintf(rb, sizeof(rb), "0x%llX",
                              static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(ref)));
                *ref_out = rb;
            }
            if (desc == "Ljava/lang/String;")
            {
                return std::string{ "\"" } + vmhook::read_java_string(ref) + "\"";
            }
            if (vmhook::hotspot::klass* const rk{ vmhook::klass_from_oop(ref) };
                rk && vmhook::hotspot::is_valid_pointer(rk))
            {
                if (const vmhook::hotspot::symbol* const rn{ rk->get_name() };
                    rn && vmhook::hotspot::is_valid_pointer(rn))
                {
                    return std::string{ "<" } + rn->to_string() + ">";
                }
            }
            return "<object>";
        }
        default: return "?";
        }
    }

    // Stream up to `max` live instances of `arg` ("classname" or
    // "classname\t<max>"), each with its declared instance (non-static) field
    // values.  `max` is viewer-driven (the scan-cap control); default 200.
    void stream_instances(HANDLE pipe, const std::string& arg)
    {
        std::string classname{ arg };
        std::size_t max_scan{ 200 };
        if (const std::size_t tab{ arg.find('\t') }; tab != std::string::npos)
        {
            classname = arg.substr(0, tab);
            if (const unsigned long long m{ std::strtoull(arg.c_str() + tab + 1, nullptr, 10) }; m > 0ull)
            {
                max_scan = static_cast<std::size_t>(m);
            }
        }

        pipe_writer writer{ pipe };
        vmhook::hotspot::klass* const k{ vmhook::find_class(classname) };
        if (!k || !vmhook::hotspot::is_valid_pointer(k))
        {
            writer.put("DONE\t0\n");
            writer.flush();
            FlushFileBuffers(pipe);
            return;
        }

        // Declared instance (non-static) fields of k, then inherited ones by
        // walking the super chain — so the table shows an object's FULL state,
        // not just what its own class declares.  Each field remembers its
        // declaring klass so a shadowed name is read at the correct offset
        // (vmhook::find_field walks supers and would otherwise pick the subclass).
        // owner_tag = declaring class's simple name when the field is INHERITED
        // (declared by a super, not by k), else empty.  Precomputed once per
        // field (identical across instances).
        struct field_info { std::string name; std::string desc; vmhook::hotspot::klass* owner; std::string owner_tag; };
        std::vector<field_info> fields;
        int hops{ 0 };
        for (vmhook::hotspot::klass* kk{ k };
             kk && vmhook::hotspot::is_valid_pointer(kk) && hops < 64;
             kk = kk->get_super(), ++hops)
        {
            std::string tag;
            if (kk != k)
            {
                if (const vmhook::hotspot::symbol* const on{ kk->get_name() };
                    on && vmhook::hotspot::is_valid_pointer(on))
                {
                    const std::string full{ on->to_string() };
                    const std::size_t p{ full.find_last_of("/$") };
                    tag = (p == std::string::npos) ? full : full.substr(p + 1);
                }
            }
            for (const auto& [fname, fdesc, facc] : kk->collect_fields())
            {
                if ((facc & 0x0008u) == 0u)  // instance (non-static) fields only
                {
                    fields.push_back({ fname, fdesc, kk, tag });
                }
            }
        }

        std::uint64_t count{ 0 };
        vmhook::for_each_instance_of(k, [&](void* const oop)
        {
            // O <TAB> 0x<heap address of the instance oop>
            char addr[32];
            std::snprintf(addr, sizeof(addr), "0x%llX",
                          static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(oop)));
            writer.put("O\t");
            writer.put(addr);
            writer.put("\n");
            for (const field_info& f : fields)
            {
                std::string ref_addr;
                const std::string value{ read_field_value(oop, f.owner, f.name, f.desc, &ref_addr) };
                writer.put("V\t");
                writer.put(sanitize(f.name));
                writer.put("\t");
                writer.put(sanitize(value));
                writer.put("\t");
                writer.put(sanitize(f.owner_tag));  // empty = declared by the inspected class
                writer.put("\t");
                writer.put(ref_addr);               // 0x<oop> for a non-null reference, else empty
                writer.put("\n");
            }
            ++count;
        }, max_scan);

        writer.put("DONE\t");
        writer.put(std::to_string(count));
        writer.put("\n");
        writer.flush();
        FlushFileBuffers(pipe);
    }

    // Stream a class's STATIC field values.  Static fields live inside the class's
    // java.lang.Class mirror; the field offset is mirror-relative, so read_field_value
    // reads them by passing the mirror oop as the base (get_field adds the offset).
    // Streamed as one "instance" (the mirror) with a V record per static field.
    void stream_statics(HANDLE pipe, const std::string& classname)
    {
        pipe_writer writer{ pipe };
        vmhook::hotspot::klass* const k{ vmhook::find_class(classname) };
        if (!k || !vmhook::hotspot::is_valid_pointer(k))
        {
            writer.put("DONE\t0\n"); writer.flush(); FlushFileBuffers(pipe); return;
        }
        void* const mirror{ k->get_java_mirror() };
        if (!mirror || !vmhook::hotspot::is_valid_pointer(mirror))
        {
            writer.put("DONE\t0\n"); writer.flush(); FlushFileBuffers(pipe); return;
        }

        char addr[32];
        std::snprintf(addr, sizeof(addr), "0x%llX",
                      static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(mirror)));
        writer.put("O\t"); writer.put(addr); writer.put("\n");

        std::uint64_t count{ 0 };
        for (const auto& [fname, fdesc, facc] : k->collect_fields())
        {
            if ((facc & 0x0008u) != 0u)  // static fields only
            {
                std::string ref_addr;
                const std::string value{ read_field_value(mirror, k, fname, fdesc, &ref_addr) };
                writer.put("V\t");
                writer.put(sanitize(fname));
                writer.put("\t");
                writer.put(sanitize(value));
                writer.put("\t");                   // owner (always empty for statics)
                writer.put("\t");
                writer.put(ref_addr);               // 0x<oop> for a non-null reference, else empty
                writer.put("\n");
                ++count;
            }
        }

        writer.put("DONE\t");
        writer.put(std::to_string(count));
        writer.put("\n");
        writer.flush();
        FlushFileBuffers(pipe);
    }

    // Find a declared or inherited field's descriptor + static flag by name,
    // walking k and its superchain (like stream_instances).  Returns true and
    // fills desc/is_static when found.
    auto find_field_desc(vmhook::hotspot::klass* const k, const std::string& name,
                         std::string& desc, bool& is_static) -> bool
    {
        int hops{ 0 };
        for (vmhook::hotspot::klass* kk{ k };
             kk && vmhook::hotspot::is_valid_pointer(kk) && hops < 64;
             kk = kk->get_super(), ++hops)
        {
            for (const auto& [fname, fdesc, facc] : kk->collect_fields())
            {
                if (fname == name)
                {
                    desc      = fdesc;
                    is_static = (facc & 0x0008u) != 0u;
                    return true;
                }
            }
        }
        return false;
    }

    // Parse `value` per the field descriptor and write it through vmhook's
    // fault-safe set_field (which resolves static-vs-instance + offset itself, so
    // `base` is the instance oop for instance fields and is ignored for statics).
    // Returns "" on success, else a human error message.
    auto apply_set(vmhook::hotspot::klass* const k, void* const base,
                   const std::string& name, const std::string& desc, const std::string& value) -> std::string
    {
        if (desc.empty())
        {
            return "unknown field descriptor";
        }
        const auto to_ll{ [](const std::string& v) { return std::strtoll(v.c_str(), nullptr, 0); } };
        switch (desc[0])
        {
        case 'Z':
        {
            const bool b{ value == "true" || value == "TRUE" || value == "1" };
            vmhook::set_field<std::uint8_t>(base, k, name, static_cast<std::uint8_t>(b ? 1 : 0));
            break;
        }
        case 'B': vmhook::set_field<std::int8_t>(base, k, name, static_cast<std::int8_t>(to_ll(value))); break;
        case 'C':
        {
            const std::uint16_t ch{ value.size() == 1
                ? static_cast<std::uint16_t>(static_cast<unsigned char>(value[0]))
                : static_cast<std::uint16_t>(std::strtoul(value.c_str(), nullptr, 0)) };
            vmhook::set_field<std::uint16_t>(base, k, name, ch);
            break;
        }
        case 'S': vmhook::set_field<std::int16_t>(base, k, name, static_cast<std::int16_t>(to_ll(value))); break;
        case 'I': vmhook::set_field<std::int32_t>(base, k, name, static_cast<std::int32_t>(to_ll(value))); break;
        case 'J': vmhook::set_field<std::int64_t>(base, k, name, static_cast<std::int64_t>(to_ll(value))); break;
        case 'F': vmhook::set_field<float>(base, k, name, std::strtof(value.c_str(), nullptr)); break;
        case 'D': vmhook::set_field<double>(base, k, name, std::strtod(value.c_str(), nullptr)); break;
        case 'L':
        case '[':
        {
            if (value == "null")
            {
                vmhook::set_field<std::uint32_t>(base, k, name, 0u);
                break;
            }
            if (desc == "Ljava/lang/String;")
            {
                void* const s{ alloc_java_string(value) };  // runs on a real JavaThread
                if (!s || !vmhook::hotspot::is_valid_pointer(s))
                {
                    return "failed to allocate a java.lang.String for the new value";
                }
                vmhook::set_field<std::uint32_t>(base, k, name, vmhook::hotspot::encode_oop_pointer(s));
                break;
            }
            if (value.rfind("0x", 0) == 0 || value.rfind("0X", 0) == 0)
            {
                void* const ref{ reinterpret_cast<void*>(
                    static_cast<std::uintptr_t>(std::strtoull(value.c_str(), nullptr, 16))) };
                if (ref && !vmhook::hotspot::is_valid_pointer(ref))
                {
                    return "the target oop address is not a readable pointer";
                }
                vmhook::set_field<std::uint32_t>(base, k, name, vmhook::hotspot::encode_oop_pointer(ref));
                break;
            }
            return "reference field: pass 'null', a 0x<oop> address, or (for String) the literal text";
        }
        default: return "unsupported field type";
        }
        return "";
    }

    // Write one field on a live instance (SETI) or a class's statics (SETS), then
    // stream the re-read value back so the caller sees the applied result.
    //   SETI  arg = "<class>\t<0xADDR>\t<field>\t<value>"
    //   SETS  arg = "<class>\t<field>\t<value>"
    void stream_set(HANDLE pipe, const std::string& arg, const bool is_instance)
    {
        pipe_writer writer{ pipe };
        const auto bail{ [&](const std::string& msg)
        {
            writer.put("E\t"); writer.put(sanitize(msg)); writer.put("\n");
            writer.put("DONE\t0\n"); writer.flush(); FlushFileBuffers(pipe);
        } };

        // Split into fields; the value is everything after the last structural tab
        // so it may itself contain tabs.
        std::vector<std::string> parts;
        const std::size_t need{ is_instance ? 3u : 2u };  // structural tabs before the value
        std::size_t pos{ 0 };
        for (std::size_t i = 0; i < need; ++i)
        {
            const std::size_t tab{ arg.find('\t', pos) };
            if (tab == std::string::npos) { bail("malformed set request"); return; }
            parts.push_back(arg.substr(pos, tab - pos));
            pos = tab + 1;
        }
        parts.push_back(arg.substr(pos));  // the value (remainder)

        const std::string& classname{ parts[0] };
        const std::string& field{ is_instance ? parts[2] : parts[1] };
        const std::string& value{ is_instance ? parts[3] : parts[2] };

        vmhook::hotspot::klass* const k{ vmhook::find_class(classname) };
        if (!k || !vmhook::hotspot::is_valid_pointer(k)) { bail("class not found: " + classname); return; }

        std::string desc; bool is_static{ false };
        if (!find_field_desc(k, field, desc, is_static)) { bail("field not found: " + field); return; }

        void* base{ nullptr };
        if (is_instance)
        {
            if (is_static) { bail("'" + field + "' is a static field — use set-static"); return; }
            base = reinterpret_cast<void*>(
                static_cast<std::uintptr_t>(std::strtoull(parts[1].c_str(), nullptr, 16)));
            if (!base || !vmhook::hotspot::is_valid_pointer(base)) { bail("invalid instance address: " + parts[1]); return; }
        }
        else
        {
            if (!is_static) { bail("'" + field + "' is an instance field — use set-instance"); return; }
            base = k->get_java_mirror();
            if (!base || !vmhook::hotspot::is_valid_pointer(base)) { bail("could not resolve the class mirror"); return; }
        }

        const std::string err{ apply_set(k, base, field, desc, value) };
        if (!err.empty()) { bail(err); return; }

        // Re-read so the response reflects what actually landed (full 4-col V).
        std::string ref_addr;
        const std::string reread{ read_field_value(base, k, field, desc, &ref_addr) };
        writer.put("V\t");
        writer.put(sanitize(field));
        writer.put("\t");
        writer.put(sanitize(reread));
        writer.put("\t\t");
        writer.put(ref_addr);
        writer.put("\n");
        writer.put("DONE\t1\n");
        writer.flush();
        FlushFileBuffers(pipe);
    }

    // ── field freeze engine (FRZ / UNF / FLST) ────────────────────────────────
    // "Freeze" a field: hold it at a chosen value against the running program by
    // re-writing it continuously from a background thread (~50 Hz).  Nothing can
    // truly veto the JVM's own writes from outside, so — like a memory trainer —
    // we simply out-write it.  Each entry caches its resolved klass + descriptor
    // so a tick is a bare fault-safe set_field.  A String value is pre-resolved
    // ONCE to a 0x<oop> at registration (allocating a fresh String every tick
    // would churn the heap and need a JavaThread on the enforcer), so the tick
    // never allocates.  GC caveat: an instance freeze pins a raw oop address; a
    // relocating GC that moves the object leaves it stale (best-effort, like every
    // raw-address feature here).  Static freezes re-resolve the mirror each write.
    struct freeze_entry
    {
        char        scope{ 'I' };  // 'I' instance, 'S' static
        std::string cls;
        std::string addr;          // 0x<oop> for instance; empty for static
        std::string field;
        std::string value;         // applied each tick (0x<oop> for pre-resolved refs)
        vmhook::hotspot::klass* k{ nullptr };
        std::string desc;
        void*       base{ nullptr };  // instance oop; nullptr for statics (set_field resolves the mirror)
    };
    std::mutex                g_freeze_mtx;
    std::vector<freeze_entry> g_freezes;          // guarded by g_freeze_mtx
    std::once_flag            g_freeze_thread_once;

    auto freeze_key(char scope, const std::string& cls, const std::string& addr, const std::string& field) -> std::string
    {
        std::string k(1, scope);
        k += '|'; k += cls;
        k += '|'; if (scope == 'I') { k += addr; }
        k += '|'; k += field;
        return k;
    }

    void freeze_thread_loop()
    {
        for (;;)
        {
            {
                std::lock_guard<std::mutex> lock{ g_freeze_mtx };
                for (freeze_entry& e : g_freezes)
                {
                    if (!e.k || !vmhook::hotspot::is_valid_pointer(e.k)) { continue; }
                    if (e.scope == 'I' && (!e.base || !vmhook::hotspot::is_valid_pointer(e.base))) { continue; }
                    apply_set(e.k, e.base, e.field, e.desc, e.value);  // fault-safe write
                }
            }
            Sleep(20);
        }
    }

    void ensure_freeze_thread()
    {
        std::call_once(g_freeze_thread_once, [] { std::thread(freeze_thread_loop).detach(); });
    }

    // FRZ: register (or replace) a freeze and stream back the applied value.
    //   arg = "<I|S>\tClass\t0xADDR\tfield\tvalue"   (addr empty for statics)
    void stream_freeze(HANDLE pipe, const std::string& arg)
    {
        pipe_writer writer{ pipe };
        const auto bail{ [&](const std::string& msg)
        {
            writer.put("E\t"); writer.put(sanitize(msg)); writer.put("\n");
            writer.put("DONE\t0\n"); writer.flush(); FlushFileBuffers(pipe);
        } };

        std::vector<std::string> parts;
        std::size_t pos{ 0 };
        for (int i = 0; i < 4; ++i)
        {
            const std::size_t tab{ arg.find('\t', pos) };
            if (tab == std::string::npos) { bail("malformed freeze request"); return; }
            parts.push_back(arg.substr(pos, tab - pos));
            pos = tab + 1;
        }
        parts.push_back(arg.substr(pos));  // value (remainder)

        const char        scope{ parts[0].empty() ? 'I' : parts[0][0] };
        const std::string& cls{ parts[1] };
        const std::string& addr{ parts[2] };
        const std::string& field{ parts[3] };
        std::string        value{ parts[4] };

        vmhook::hotspot::klass* const k{ vmhook::find_class(cls) };
        if (!k || !vmhook::hotspot::is_valid_pointer(k)) { bail("class not found: " + cls); return; }
        std::string desc; bool is_static{ false };
        if (!find_field_desc(k, field, desc, is_static)) { bail("field not found: " + field); return; }
        if (scope == 'S' && !is_static) { bail("'" + field + "' is an instance field"); return; }
        if (scope == 'I' && is_static)  { bail("'" + field + "' is a static field"); return; }

        void* base{ nullptr };
        if (scope == 'I')
        {
            base = reinterpret_cast<void*>(static_cast<std::uintptr_t>(std::strtoull(addr.c_str(), nullptr, 16)));
            if (!base || !vmhook::hotspot::is_valid_pointer(base)) { bail("invalid instance address: " + addr); return; }
        }

        // Pre-resolve a literal-text String value to a stable 0x<oop> so the
        // enforcer never re-allocates (null / 0x / primitive pass through as-is).
        if (desc == "Ljava/lang/String;" && value != "null"
            && value.rfind("0x", 0) != 0 && value.rfind("0X", 0) != 0)
        {
            void* const s{ alloc_java_string(value) };
            if (!s || !vmhook::hotspot::is_valid_pointer(s)) { bail("failed to allocate the String to freeze"); return; }
            char sb[32];
            std::snprintf(sb, sizeof(sb), "0x%llX", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(s)));
            value = sb;
        }

        // Apply once so the caller sees it land, and validate the write path.
        if (const std::string err{ apply_set(k, base, field, desc, value) }; !err.empty()) { bail(err); return; }

        {
            std::lock_guard<std::mutex> lock{ g_freeze_mtx };
            const std::string key{ freeze_key(scope, cls, addr, field) };
            g_freezes.erase(std::remove_if(g_freezes.begin(), g_freezes.end(),
                [&](const freeze_entry& e) { return freeze_key(e.scope, e.cls, e.addr, e.field) == key; }),
                g_freezes.end());
            g_freezes.push_back(freeze_entry{ scope, cls, addr, field, value, k, desc, base });
        }
        ensure_freeze_thread();

        std::string ref_addr;
        const std::string reread{ read_field_value(scope == 'S' ? k->get_java_mirror() : base, k, field, desc, &ref_addr) };
        writer.put("V\t"); writer.put(sanitize(field)); writer.put("\t");
        writer.put(sanitize(reread)); writer.put("\t\t"); writer.put(ref_addr); writer.put("\n");
        writer.put("DONE\t1\n"); writer.flush(); FlushFileBuffers(pipe);
    }

    // UNF: remove one freeze by key ("<I|S>|Class|addr|field") or "*" for all.
    void stream_unfreeze(HANDLE pipe, const std::string& key)
    {
        {
            std::lock_guard<std::mutex> lock{ g_freeze_mtx };
            if (key == "*")
            {
                g_freezes.clear();
            }
            else
            {
                g_freezes.erase(std::remove_if(g_freezes.begin(), g_freezes.end(),
                    [&](const freeze_entry& e) { return freeze_key(e.scope, e.cls, e.addr, e.field) == key; }),
                    g_freezes.end());
            }
        }
        pipe_writer writer{ pipe };
        writer.put("DONE\t0\n"); writer.flush(); FlushFileBuffers(pipe);
    }

    // FLST: stream the current freeze table so the viewer can reconcile its lock
    // state after a re-attach (Z records) + DONE.
    void stream_freeze_list(HANDLE pipe)
    {
        pipe_writer writer{ pipe };
        std::lock_guard<std::mutex> lock{ g_freeze_mtx };
        for (const freeze_entry& e : g_freezes)
        {
            writer.put("Z\t"); writer.put(std::string(1, e.scope)); writer.put("\t");
            writer.put(sanitize(e.cls)); writer.put("\t"); writer.put(sanitize(e.addr)); writer.put("\t");
            writer.put(sanitize(e.field)); writer.put("\t"); writer.put(sanitize(e.value)); writer.put("\n");
        }
        writer.put("DONE\t"); writer.put(std::to_string(g_freezes.size())); writer.put("\n");
        writer.flush(); FlushFileBuffers(pipe);
    }

    // ── dynamic method invocation (CALL) ──────────────────────────────────────
    // Resolve a method by exact name + JVM descriptor on `k` or a superclass
    // (most-derived first, so an override wins over the inherited definition).
    auto resolve_method(vmhook::hotspot::klass* const k, const std::string& name, const std::string& desc)
        -> vmhook::hotspot::method*
    {
        int hops{ 0 };
        for (vmhook::hotspot::klass* kk{ k };
             kk && vmhook::hotspot::is_valid_pointer(kk) && hops < 64;
             kk = kk->get_super(), ++hops)
        {
            const std::int32_t mc{ kk->get_methods_count() };
            vmhook::hotspot::method** const ms{ kk->get_methods_ptr() };
            if (!ms || mc <= 0) { continue; }
            for (std::int32_t i = 0; i < mc; ++i)
            {
                vmhook::hotspot::method* const m{ ms[i] };
                if (!m || !vmhook::hotspot::is_valid_pointer(m)) { continue; }
                if (m->get_name() == name && m->get_signature() == desc) { return m; }
            }
        }
        return nullptr;
    }

    // Split a method descriptor's parameter list into individual descriptors,
    // e.g. "(ILjava/lang/String;[I)V" -> {"I","Ljava/lang/String;","[I"}.
    auto parse_param_descriptors(const std::string& sig) -> std::vector<std::string>
    {
        std::vector<std::string> out;
        std::size_t i{ sig.find('(') };
        if (i == std::string::npos) { return out; }
        ++i;
        while (i < sig.size() && sig[i] != ')')
        {
            const std::size_t start{ i };
            while (i < sig.size() && sig[i] == '[') { ++i; }
            if (i < sig.size() && sig[i] == 'L')
            {
                const std::size_t s{ sig.find(';', i) };
                i = (s == std::string::npos ? sig.size() : s + 1);
            }
            else if (i < sig.size()) { ++i; }
            out.push_back(sig.substr(start, i - start));
        }
        return out;
    }

    // Wrap a raw heap oop as a JNI local ref so it can be passed to JNI Call*
    // methods.  JNI has no oop->jobject, but NewObjectArray returns a real ref; we
    // store the oop into element 0 (a raw compressed-oop write) and read it back
    // out as a proper local ref.  Runs inside the trigger detour (valid env).
    auto oop_to_jobject(JNIEnv* const env, void* const oop) -> jobject
    {
        if (!oop) { return nullptr; }
        static jclass s_object{ nullptr };
        if (!s_object)
        {
            if (jclass c{ env->FindClass("java/lang/Object") })
            { s_object = static_cast<jclass>(env->NewGlobalRef(c)); env->DeleteLocalRef(c); }
        }
        if (!s_object) { return nullptr; }
        jobjectArray arr{ env->NewObjectArray(1, s_object, nullptr) };
        if (env->ExceptionCheck()) { env->ExceptionClear(); }
        if (!arr) { return nullptr; }
        if (void* const arr_oop{ *reinterpret_cast<void**>(arr) };
            arr_oop && vmhook::hotspot::is_valid_pointer(arr_oop))
        {
            vmhook::set_array_element<std::uint32_t>(arr_oop, 0, vmhook::hotspot::encode_oop_pointer(oop));
        }
        jobject r{ env->GetObjectArrayElement(arr, 0) };
        if (env->ExceptionCheck()) { env->ExceptionClear(); }
        env->DeleteLocalRef(arr);
        return r;
    }

    // Read the raw heap oop out of a JNI ref (local refs store the oop directly).
    auto jobject_to_oop(jobject o) -> void*
    {
        return o ? *reinterpret_cast<void**>(o) : nullptr;
    }

    // Invoke a method via JNI, from INSIDE the trigger detour (a real JavaThread).
    // This works on every JDK — unlike the pure-VM call_stub, which JDK 21+ no
    // longer exposes through VMStructs.  k = the target klass (its mirror gives the
    // jclass, and the method id); receiver is the raw oop (ignored for statics).
    auto invoke_jni(void* const receiver, vmhook::hotspot::klass* const k, const bool is_static,
                    const std::string& name, const std::string& sig, const std::vector<std::string>& argtoks,
                    std::string& kind, std::string& disp, std::string& raddr, std::string& rclass) -> std::string
    {
        JNIEnv* env{ nullptr };
        if (!g_jvm || g_jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK || !env)
        { return "no JNIEnv on the invoking thread"; }

        void* const mirror{ k->get_java_mirror() };
        if (!mirror || !vmhook::hotspot::is_valid_pointer(mirror)) { return "could not resolve the class mirror"; }
        jclass cls{ static_cast<jclass>(oop_to_jobject(env, mirror)) };
        if (!cls) { return "could not obtain a jclass for the target"; }

        std::vector<jobject> locals;  // JNI local refs to release
        const auto done{ [&](const std::string& e) -> std::string
        {
            for (jobject l : locals) if (l) env->DeleteLocalRef(l);
            if (cls) env->DeleteLocalRef(cls);
            return e;
        } };

        const jmethodID mid{ is_static ? env->GetStaticMethodID(cls, name.c_str(), sig.c_str())
                                       : env->GetMethodID(cls, name.c_str(), sig.c_str()) };
        if (env->ExceptionCheck()) { env->ExceptionClear(); }
        if (!mid) { return done("no method id for " + name + sig); }

        const std::vector<std::string> pds{ parse_param_descriptors(sig) };
        if (argtoks.size() != pds.size()) { return done("argument count mismatch (expected " + std::to_string(pds.size()) + ")"); }

        std::vector<jvalue> jargs(pds.size());
        for (std::size_t a = 0; a < pds.size(); ++a)
        {
            const std::string& pd{ pds[a] };
            const std::string& tok{ argtoks[a] };
            const auto to_ll{ [&] { return std::strtoll(tok.c_str(), nullptr, 0); } };
            switch (pd.empty() ? '?' : pd[0])
            {
            case 'Z': jargs[a].z = (tok == "true" || tok == "TRUE" || tok == "1") ? JNI_TRUE : JNI_FALSE; break;
            case 'B': jargs[a].b = static_cast<jbyte>(to_ll()); break;
            case 'S': jargs[a].s = static_cast<jshort>(to_ll()); break;
            case 'C': jargs[a].c = tok.size() == 1 ? static_cast<jchar>(static_cast<unsigned char>(tok[0]))
                                                   : static_cast<jchar>(std::strtoul(tok.c_str(), nullptr, 0)); break;
            case 'I': jargs[a].i = static_cast<jint>(to_ll()); break;
            case 'J': jargs[a].j = static_cast<jlong>(to_ll()); break;
            case 'F': jargs[a].f = std::strtof(tok.c_str(), nullptr); break;
            case 'D': jargs[a].d = std::strtod(tok.c_str(), nullptr); break;
            case 'L':
            case '[':
            {
                jobject o{ nullptr };
                if (tok == "@null" || tok == "null" || tok.empty()) { o = nullptr; }
                else if (tok.rfind("@0x", 0) == 0 || tok.rfind("@0X", 0) == 0 || tok.rfind("0x", 0) == 0 || tok.rfind("0X", 0) == 0)
                {
                    const char* hex{ tok.c_str() + (tok[0] == '@' ? 1 : 0) };
                    void* const ref{ reinterpret_cast<void*>(static_cast<std::uintptr_t>(std::strtoull(hex, nullptr, 16))) };
                    if (ref && !vmhook::hotspot::is_valid_pointer(ref)) { return done("argument oop is not readable: " + tok); }
                    o = oop_to_jobject(env, ref);
                    if (ref && !o) { return done("could not wrap the argument object"); }
                }
                else  // "#text" or a bare literal for a String/Object parameter -> new String
                {
                    const std::string text{ tok.rfind('#', 0) == 0 ? tok.substr(1) : tok };
                    o = env->NewStringUTF(text.c_str());
                    if (!o) { if (env->ExceptionCheck()) env->ExceptionClear(); return done("failed to allocate a String argument"); }
                }
                jargs[a].l = o;
                if (o) { locals.push_back(o); }
                break;
            }
            default: return done("unsupported parameter type: " + pd);
            }
        }

        jobject receiverObj{ nullptr };
        if (!is_static)
        {
            receiverObj = oop_to_jobject(env, receiver);
            if (!receiverObj) { return done("could not wrap the receiver object"); }
            locals.push_back(receiverObj);
        }

        const std::size_t rparen{ sig.rfind(')') };
        const char ret{ (rparen != std::string::npos && rparen + 1 < sig.size()) ? sig[rparen + 1] : 'V' };
        const jvalue* const av{ jargs.empty() ? nullptr : jargs.data() };

        const auto threw{ [&]() -> bool
        {
            if (!env->ExceptionCheck()) { return false; }
            env->ExceptionDescribe();  // to JVM stderr
            env->ExceptionClear();
            return true;
        } };

        switch (ret)
        {
        case 'V': is_static ? env->CallStaticVoidMethodA(cls, mid, av) : env->CallVoidMethodA(receiverObj, mid, av);
                  if (threw()) return done("the method threw an exception (see the JVM's stderr)");
                  kind = "void"; disp = "(void)"; break;
        case 'Z': { const jboolean v{ is_static ? env->CallStaticBooleanMethodA(cls, mid, av) : env->CallBooleanMethodA(receiverObj, mid, av) };
                    if (threw()) return done("the method threw an exception (see the JVM's stderr)");
                    kind = "bool"; disp = v ? "true" : "false"; break; }
        case 'B': { const jbyte v{ is_static ? env->CallStaticByteMethodA(cls, mid, av) : env->CallByteMethodA(receiverObj, mid, av) };
                    if (threw()) return done("the method threw an exception (see the JVM's stderr)");
                    kind = "byte"; disp = std::to_string(static_cast<int>(v)); break; }
        case 'S': { const jshort v{ is_static ? env->CallStaticShortMethodA(cls, mid, av) : env->CallShortMethodA(receiverObj, mid, av) };
                    if (threw()) return done("the method threw an exception (see the JVM's stderr)");
                    kind = "short"; disp = std::to_string(static_cast<int>(v)); break; }
        case 'C': { const jchar v{ is_static ? env->CallStaticCharMethodA(cls, mid, av) : env->CallCharMethodA(receiverObj, mid, av) };
                    if (threw()) return done("the method threw an exception (see the JVM's stderr)");
                    kind = "char"; disp = (v >= 32 && v < 127) ? (std::string{ "'" } + static_cast<char>(v) + "'") : std::to_string(static_cast<unsigned>(v)); break; }
        case 'I': { const jint v{ is_static ? env->CallStaticIntMethodA(cls, mid, av) : env->CallIntMethodA(receiverObj, mid, av) };
                    if (threw()) return done("the method threw an exception (see the JVM's stderr)");
                    kind = "int"; disp = std::to_string(v); break; }
        case 'J': { const jlong v{ is_static ? env->CallStaticLongMethodA(cls, mid, av) : env->CallLongMethodA(receiverObj, mid, av) };
                    if (threw()) return done("the method threw an exception (see the JVM's stderr)");
                    kind = "long"; disp = std::to_string(static_cast<long long>(v)); break; }
        case 'F': { const jfloat v{ is_static ? env->CallStaticFloatMethodA(cls, mid, av) : env->CallFloatMethodA(receiverObj, mid, av) };
                    if (threw()) return done("the method threw an exception (see the JVM's stderr)");
                    kind = "float"; disp = std::to_string(v); break; }
        case 'D': { const jdouble v{ is_static ? env->CallStaticDoubleMethodA(cls, mid, av) : env->CallDoubleMethodA(receiverObj, mid, av) };
                    if (threw()) return done("the method threw an exception (see the JVM's stderr)");
                    kind = "double"; disp = std::to_string(v); break; }
        default:  // 'L' / '[' — object / array
        {
            jobject v{ is_static ? env->CallStaticObjectMethodA(cls, mid, av) : env->CallObjectMethodA(receiverObj, mid, av) };
            if (threw()) return done("the method threw an exception (see the JVM's stderr)");
            if (!v) { kind = "null"; disp = "null"; break; }
            void* const oop{ jobject_to_oop(v) };
            char rb[32]; std::snprintf(rb, sizeof(rb), "0x%llX", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(oop)));
            raddr = rb;
            if (rparen != std::string::npos && sig.substr(rparen + 1) == "Ljava/lang/String;")
            { kind = "string"; disp = std::string{ "\"" } + vmhook::read_java_string(oop) + "\""; rclass = "java/lang/String"; }
            else if (vmhook::hotspot::klass* const rk{ vmhook::klass_from_oop(oop) }; rk && vmhook::hotspot::is_valid_pointer(rk))
            {
                if (const vmhook::hotspot::symbol* const rn{ rk->get_name() }; rn && vmhook::hotspot::is_valid_pointer(rn))
                { kind = "ref"; rclass = rn->to_string(); disp = std::string{ "<" } + rclass + ">"; }
                else { kind = "ref"; disp = "<object>"; }
            }
            else { kind = "ref"; disp = "<object>"; }
            env->DeleteLocalRef(v);
            break;
        }
        }
        return done("");
    }

    // CALL: invoke a method and reply R + DONE (or E).
    //   arg = "Class\t<0xADDR|->\tmethod\tdescriptor\tn\targ0\targ1..."
    void stream_call(HANDLE pipe, const std::string& arg)
    {
        pipe_writer writer{ pipe };
        const auto bail{ [&](const std::string& msg)
        {
            writer.put("E\t"); writer.put(sanitize(msg)); writer.put("\n");
            writer.put("DONE\t0\n"); writer.flush(); FlushFileBuffers(pipe);
        } };

        std::vector<std::string> parts;
        std::size_t p{ 0 };
        for (;;)
        {
            const std::size_t tab{ arg.find('\t', p) };
            if (tab == std::string::npos) { parts.push_back(arg.substr(p)); break; }
            parts.push_back(arg.substr(p, tab - p));
            p = tab + 1;
        }
        if (parts.size() < 5) { bail("malformed call request"); return; }
        const std::string& cls{ parts[0] };
        const std::string& addrs{ parts[1] };
        const std::string& mname{ parts[2] };
        const std::string& desc{ parts[3] };
        const long         n{ std::strtol(parts[4].c_str(), nullptr, 10) };
        std::vector<std::string> args{ parts.begin() + 5, parts.end() };
        if (n >= 0 && static_cast<std::size_t>(n) < args.size()) { args.resize(static_cast<std::size_t>(n)); }

        vmhook::hotspot::klass* const k{ vmhook::find_class(cls) };
        if (!k || !vmhook::hotspot::is_valid_pointer(k)) { bail("class not found: " + cls); return; }
        vmhook::hotspot::method* const m{ resolve_method(k, mname, desc) };
        if (!m) { bail("method not found: " + mname + desc); return; }

        std::uint32_t flags{ 0 };
        if (std::uint32_t* const fp{ m->get_access_flags() }) { vmhook::os::safe_read(&flags, fp, sizeof(flags)); }
        const bool is_static{ (flags & 0x0008u) != 0u };

        void* receiver{ nullptr };
        if (!is_static && !addrs.empty() && addrs != "-")
        {
            receiver = reinterpret_cast<void*>(static_cast<std::uintptr_t>(std::strtoull(addrs.c_str(), nullptr, 16)));
            if (receiver && !vmhook::hotspot::is_valid_pointer(receiver)) { bail("invalid instance address: " + addrs); return; }
        }
        if (!is_static && !receiver) { bail("this instance method needs a receiver address"); return; }

        // Invocation must happen INSIDE a detour on a real JavaThread (the JNI
        // Call* path sets up the proper Java frame there).  run_on_java_thread
        // fires the trigger hook and runs the task in its detour.
        std::string kind, disp, raddr, rclass, err;
        const bool ran{ run_on_java_thread([&] { err = invoke_jni(receiver, k, is_static, mname, desc, args, kind, disp, raddr, rclass); }) };
        if (!ran) { bail("could not enter a Java thread to invoke (trigger hook unavailable on this JVM)"); return; }
        if (!err.empty()) { bail(err); return; }

        writer.put("R\t"); writer.put(sanitize(disp)); writer.put("\t");
        writer.put(sanitize(kind)); writer.put("\t"); writer.put(sanitize(raddr)); writer.put("\t");
        writer.put(sanitize(rclass)); writer.put("\n");
        writer.put("DONE\t1\n"); writer.flush(); FlushFileBuffers(pipe);
    }

    // ── live class-load hook (vmhook::on_class_loaded) ───────────────────────
    // Event-driven detection of classes DEFINED AT RUNTIME through
    // java.lang.ClassLoader.defineClass (application / agent / custom-loader
    // classes — bootstrap java.*/sun.* bypass it).  Each definition appends the
    // internal name to g_hook_log; the viewer drains it via the DRAIN request.
    std::mutex                          g_hook_mtx;
    std::vector<std::string>            g_hook_log;    // names captured since last drain
    std::optional<vmhook::watch_handle> g_hook;        // keeps the hook installed
    bool                                g_hook_armed{ false };

    void arm_class_hook()
    {
        bool just_armed{ false };
        {
            std::lock_guard<std::mutex> lock{ g_hook_mtx };
            if (g_hook_armed)
            {
                return;
            }
            auto handle{ vmhook::on_class_loaded([](const std::string& name)
            {
                std::lock_guard<std::mutex> lk{ g_hook_mtx };
                if (g_hook_log.size() < 200000u) g_hook_log.push_back(name);  // bounded
            }) };
            if (handle.running())
            {
                g_hook.emplace(std::move(handle));  // keep alive → hook stays installed
                g_hook_armed = true;
                just_armed  = true;
            }
        }
        // ClassLoader.defineClass is hot and almost always already JIT-compiled,
        // so the i2i-interpreter-entry detour the hook installs is bypassed and
        // the hook silently misses (the documented i2i-vs-JIT gap).  Deoptimise
        // every ClassLoader.defineClass overload once so subsequent calls route
        // back through the interpreter entry the hook patched.
        if (just_armed)
        {
            vmhook::deoptimize_methods_if([](const std::string& class_name, vmhook::hotspot::method* m)
            {
                return class_name == "java/lang/ClassLoader" && m && m->get_name() == "defineClass";
            });
        }
    }

    // HOOK request: arm the class-load hook (once); reply H<TAB><1|0> + DONE.
    void stream_hook_arm(HANDLE pipe)
    {
        arm_class_hook();
        pipe_writer writer{ pipe };
        writer.put("H\t");
        writer.put(g_hook_armed ? "1" : "0");
        writer.put("\n");
        writer.put("DONE\t0\n");
        writer.flush();
        FlushFileBuffers(pipe);
    }

    // DRAIN request: stream (and clear) the hook-captured names as N records.
    void stream_hook_drain(HANDLE pipe)
    {
        std::vector<std::string> names;
        {
            std::lock_guard<std::mutex> lock{ g_hook_mtx };
            names.swap(g_hook_log);
        }
        pipe_writer writer{ pipe };
        for (const std::string& n : names)
        {
            writer.put("N\t");
            writer.put(sanitize(n));
            writer.put("\n");
        }
        writer.put("DONE\t");
        writer.put(std::to_string(names.size()));
        writer.put("\n");
        writer.flush();
        FlushFileBuffers(pipe);
    }

    // Serve every attach: the payload stays loaded and, each time a viewer/CLI
    // creates the pipe server, connects to it and streams whatever the current
    // request asks for (class surface by default, or live instances).  This makes
    // RE-ATTACH robust — a second attach just re-uses this already-running server
    // (LoadLibrary of an already-loaded DLL never re-runs DllMain, so relying on
    // re-injection would hang).  The payload only READS VM metadata (no hooks).
    void serve_forever()
    {
        for (;;)
        {
            HANDLE pipe{ CreateFileW(k_pipe_name, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr) };
            if (pipe == INVALID_HANDLE_VALUE)
            {
                if (GetLastError() == ERROR_PIPE_BUSY)
                {
                    WaitNamedPipeW(k_pipe_name, 2000);
                }
                else
                {
                    Sleep(250);  // no server up right now; wait for the next attach
                }
                continue;
            }
            const std::string req{ read_request() };
            if (req.rfind("INST\t", 0) == 0)
            {
                stream_instances(pipe, req.substr(5));
            }
            else if (req.rfind("STAT\t", 0) == 0)
            {
                stream_statics(pipe, req.substr(5));
            }
            else if (req.rfind("SETI\t", 0) == 0)
            {
                stream_set(pipe, req.substr(5), /*is_instance=*/true);
            }
            else if (req.rfind("SETS\t", 0) == 0)
            {
                stream_set(pipe, req.substr(5), /*is_instance=*/false);
            }
            else if (req.rfind("FRZ\t", 0) == 0)
            {
                stream_freeze(pipe, req.substr(4));
            }
            else if (req.rfind("UNF\t", 0) == 0)
            {
                stream_unfreeze(pipe, req.substr(4));
            }
            else if (req == "FLST")
            {
                stream_freeze_list(pipe);
            }
            else if (req.rfind("CALL\t", 0) == 0)
            {
                stream_call(pipe, req.substr(5));
            }
            else if (req == "HOOK")
            {
                stream_hook_arm(pipe);
            }
            else if (req == "DRAIN")
            {
                stream_hook_drain(pipe);
            }
            else
            {
                stream_to(pipe);
            }
            CloseHandle(pipe);
        }
    }
}

extern "C" BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);
        // One detached server thread handles this attach and every future one.
        std::thread(serve_forever).detach();
    }
    return TRUE;
}
