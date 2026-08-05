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
//   "ARR"  <TAB> 0xADDR <TAB> elemDescriptor <TAB> max -> a Java array's elements:
//                    A <TAB> length ; then V <TAB> index <TAB> value <TAB> <TAB> refAddr.
//   "ARRSET" <TAB> 0xADDR <TAB> elemDescriptor <TAB> index <TAB> value -> write one
//                    array element, then V (re-read).
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
//   "DRAINF"        -> drain the hook and stream each newly-defined class's FULL
//                        surface (C/M/F), so the viewer can ADD just those classes
//                        without re-enumerating everything.
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
    // that is *inside an interpreter detour* — the library's documented contract
    // (current_java_thread set, a valid last_Java_frame anchor, a safepoint-safe
    // transition).  Calling the raw call stub from a plain native thread crashes
    // the VM (no anchor -> a GC stack-walk faults).
    //
    // THE PUMP.  So we do not enter Java from native at all.  We hook methods the
    // JVM's own threads already call, and let the detour drain a work queue:
    //
    //     serve thread            some Java thread
    //     ------------            ----------------
    //     enqueue(task)   ---->   ...calls Thread.currentThread()...
    //     wait                    detour fires, dequeues, RUNS THE TASK on a
    //                             genuine JavaThread inside a valid Java frame
    //     <-- signalled            signals completion
    //
    // This is why the payload no longer needs <jni.h>.  The previous design
    // attached the serve thread with AttachCurrentThreadAsDaemon and then FIRED
    // the detour with a JNI CallStaticObjectMethod — JNI used purely as a
    // doorbell.  The doorbell is unnecessary: a running JVM rings it constantly
    // on its own.  What we pay instead is latency — the task runs on the next
    // natural call to one of the pump methods rather than immediately — and a
    // viewer does not care.
    //
    // WHY SEVERAL PUMP METHODS.  Any single choice is a bet on the target app's
    // behaviour.  Thread.currentThread() and System.nanoTime() are called by
    // essentially every non-trivial workload (and by the JDK's own machinery);
    // Object.hashCode() and Runtime.getRuntime() are the backstops.  We install
    // on all of them and any one firing drains the queue, so an idle-ish app is
    // far less likely to leave a task stranded than with a single hook.
    //
    // The detour body is deliberately trivial and gated on a pending-task flag,
    // so the overwhelmingly common case — a collateral caller with no work
    // queued — is one relaxed atomic load and a return.

    struct pump_target
    {
        const char* class_name;
        const char* method_name;
        const char* descriptor;
    };

    // Ordered most- to least-frequently-called in a typical workload.
    constexpr pump_target k_pump_targets[]{
        { "java/lang/Thread", "currentThread", "()Ljava/lang/Thread;" },
        { "java/lang/System", "nanoTime",      "()J"                  },
        { "java/lang/System", "currentTimeMillis", "()J"              },
        { "java/lang/Object", "hashCode",      "()I"                  },
        { "java/lang/Runtime", "getRuntime",   "()Ljava/lang/Runtime;" },
    };

    // One wrapper per pump class.  vmhook::hook<T> keys the target class off the
    // registered wrapper type, so each distinct class needs its own type.
    class pump_thread  : public vmhook::object<pump_thread>
    { public: explicit pump_thread(vmhook::oop_t o) noexcept  : vmhook::object<pump_thread>{ o } {} };
    class pump_system  : public vmhook::object<pump_system>
    { public: explicit pump_system(vmhook::oop_t o) noexcept  : vmhook::object<pump_system>{ o } {} };
    class pump_object  : public vmhook::object<pump_object>
    { public: explicit pump_object(vmhook::oop_t o) noexcept  : vmhook::object<pump_object>{ o } {} };
    class pump_runtime : public vmhook::object<pump_runtime>
    { public: explicit pump_runtime(vmhook::oop_t o) noexcept : vmhook::object<pump_runtime>{ o } {} };

    std::mutex                          g_task_mtx;
    std::condition_variable             g_task_cv;
    std::function<void()>               g_task;
    std::atomic<bool>                   g_task_pending{ false };
    bool                                g_task_done{ false };  // guarded by g_task_mtx
    std::vector<vmhook::hook_handle>    g_pump_hooks;
    std::once_flag                      g_pump_once;
    std::atomic<int>                    g_pump_installed{ 0 };

    // Drain one queued task.  Runs INSIDE a detour, on a real JavaThread, in a
    // valid Java frame — the only context where call() / make_java_string() are
    // defined to work.
    void drain_one_task() noexcept
    {
        if (!g_task_pending.load(std::memory_order_relaxed))
        {
            return;   // the common case: a collateral caller, no work queued
        }
        std::function<void()> job;
        {
            std::lock_guard<std::mutex> lk{ g_task_mtx };
            if (!g_task_pending.load()) { return; }
            job = std::move(g_task);
            g_task = nullptr;
            g_task_pending.store(false);
        }
        if (!job) { return; }
        try { job(); } catch (...) { /* never let a task kill the VM */ }
        { std::lock_guard<std::mutex> lk{ g_task_mtx }; g_task_done = true; }
        g_task_cv.notify_all();
    }

    // Install the pump on `target` using wrapper W.  Returns true if armed.
    template<typename wrapper_type>
    auto install_pump(const pump_target& target) -> bool
    {
        if (vmhook::type_to_class_map.find(std::type_index{ typeid(wrapper_type) })
            == vmhook::type_to_class_map.end())
        {
            if (!vmhook::register_class<wrapper_type>(target.class_name)) { return false; }
        }

        // These are the hottest methods in the JVM and are certainly JIT-compiled
        // (and inlined) already, so the i2i interpreter detour we install would be
        // bypassed.  Deoptimise the specific method back to the interpreter first
        // — the same discipline the JVM test suite uses — and accept that HotSpot
        // will recompile it; vmhook holds NO_COMPILE on a hooked Method, so the
        // route stays put once established.
        vmhook::deoptimize_methods_if(
            [&target](const std::string& class_name, vmhook::hotspot::method* m)
            {
                return class_name == target.class_name
                       && m && m->get_name() == target.method_name;
            });

        // scoped_hook, not hook: hook() returns a bare bool and leaves the detour
        // installed forever with no way to reach it.  The handle is what lets the
        // payload keep the pump alive deliberately (moved into g_pump_hooks) and
        // uninstall it on teardown.
        auto handle{ vmhook::scoped_hook<wrapper_type>(target.method_name, target.descriptor,
                                                       [](vmhook::return_value&) { drain_one_task(); }) };
        if (!handle.installed()) { return false; }
        g_pump_hooks.emplace_back(std::move(handle));   // keep installed
        return true;
    }

    void ensure_pump()
    {
        std::call_once(g_pump_once, []
        {
            int armed{ 0 };
            armed += install_pump<pump_thread>(k_pump_targets[0]) ? 1 : 0;
            armed += install_pump<pump_system>(k_pump_targets[1]) ? 1 : 0;
            // System has two pump methods; the wrapper is already registered, so
            // this second install reuses it.
            armed += install_pump<pump_system>(k_pump_targets[2]) ? 1 : 0;
            armed += install_pump<pump_object>(k_pump_targets[3]) ? 1 : 0;
            armed += install_pump<pump_runtime>(k_pump_targets[4]) ? 1 : 0;
            g_pump_installed.store(armed, std::memory_order_release);
        });
    }

    // Run `fn` on a real JavaThread inside a pump detour — the only safe context
    // for method_proxy::call / make_java_string.  Blocks until it runs or times
    // out.  Returns false if no pump could be armed or nothing rang it in time.
    auto run_on_java_thread(std::function<void()> fn) -> bool
    {
        ensure_pump();
        if (g_pump_installed.load(std::memory_order_acquire) == 0) { return false; }

        {
            std::lock_guard<std::mutex> lk{ g_task_mtx };
            g_task = std::move(fn);
            g_task_done = false;
            g_task_pending.store(true);
        }

        // We no longer ring the doorbell ourselves, so the wait is longer than the
        // old JNI-triggered 5 s: we are waiting for the target app to call one of
        // the pump methods on its own.  Any live workload does so within
        // milliseconds; a fully idle JVM may take longer, and a completely
        // quiescent one will time out, which is the honest answer.
        std::unique_lock<std::mutex> lk{ g_task_mtx };
        if (!g_task_cv.wait_for(lk, std::chrono::seconds(15), [] { return g_task_done; }))
        {
            // Timed out.  If the job was never dequeued, nobody holds our closure —
            // abandon it safely.  But if it was ALREADY dequeued (pending cleared),
            // some JavaThread is running it right now with references into THIS
            // caller's stack; we must NOT return and let the caller unwind, or the
            // late write faults.  Wait it out (bounded) so the closure outlives the
            // job.
            if (g_task_pending.load())
            {
                g_task = nullptr;
                g_task_pending.store(false);
                return false;
            }
            g_task_cv.wait_for(lk, std::chrono::seconds(30), [] { return g_task_done; });
        }
        return g_task_done;
    }

    // Allocate a java.lang.String on a real JavaThread (inside a pump detour) and
    // return its heap oop.  Pure VM — vmhook::make_java_string allocates from the
    // running thread's TLAB, which is exactly why it has to run in this context.
    //
    // GC caveat, unchanged from the JNI version and inherent to handing back a raw
    // address: the String is NOT rooted by anything here, so a relocating
    // collection between this returning and the caller storing it invalidates the
    // address.  The window is short (the caller stores it into a field
    // immediately) and this is best-effort, like every raw-oop path in the viewer.
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

    // Emit one class's full surface (C + M + F records) — shared by the full
    // enumeration and the hook-driven "new class" stream so both are byte-identical.
    void emit_class_surface(pipe_writer& writer, const std::string& name, vmhook::hotspot::klass* const klass)
    {
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

        // Declared methods: (name, descriptor, access flags).  Array klasses have
        // no _methods array, so skip them ('[' names).
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
    }

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
                emit_class_surface(writer, name, klass);
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

    // Parse a user-typed integer the way a field editor should: DECIMAL by
    // default, so "010" is ten and "09" is nine.  std::strtoll(.., 0) auto-detects
    // the base, which silently reads a leading-zero decimal as OCTAL ("010" -> 8,
    // "09" -> 0) — a real footgun for anyone typing a normal number.  An explicit
    // "0x"/"0X" prefix is still honoured as hex.  Leading sign / spaces tolerated.
    auto parse_ll(const std::string& v) -> long long
    {
        const char* s{ v.c_str() };
        while (*s == ' ' || *s == '\t') { ++s; }
        long long sign{ 1 };
        if (*s == '+') { ++s; }
        else if (*s == '-') { sign = -1; ++s; }
        int base{ 10 };
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
        return sign * static_cast<long long>(std::strtoull(s, nullptr, base));
    }

    auto parse_ull(const std::string& v) -> unsigned long long
    {
        const char* s{ v.c_str() };
        while (*s == ' ' || *s == '\t') { ++s; }
        int base{ 10 };
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
        return std::strtoull(s, nullptr, base);
    }

    // Clean float/double formatting for display.  std::to_string uses "%f" — a
    // fixed 6 decimal PLACES — so 1.5 prints "1.500000" and large/small magnitudes
    // lose or pad digits.  "%.*g" gives significant digits and strips trailing
    // zeros: 1.5 -> "1.5", 3.14159 -> "3.14159".  sig = enough to round-trip the
    // type without the noise of full 17-digit precision.
    auto fmt_g(double d, int sig) -> std::string
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.*g", sig, d);
        return buf;
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
        case 'F': return fmt_g(static_cast<double>(vmhook::get_field<float>(oop, k, name)), 7);
        case 'J': return std::to_string(vmhook::get_field<std::int64_t>(oop, k, name));
        case 'D': return fmt_g(vmhook::get_field<double>(oop, k, name), 15);
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

    // Read one array element and format it for display (mirrors read_field_value
    // but through the bounds-checked vmhook::get_array_element<T>).  ref_out gets a
    // non-null reference element's pointee address (for grab), else empty.
    auto read_array_element(void* const arr, const std::string& elem_desc, std::int32_t i,
                            std::string* const ref_out) -> std::string
    {
        if (ref_out) { ref_out->clear(); }
        if (elem_desc.empty()) { return "?"; }
        switch (elem_desc[0])
        {
        case 'Z': return vmhook::get_array_element<std::uint8_t>(arr, i) != 0 ? "true" : "false";
        case 'B': return std::to_string(static_cast<int>(vmhook::get_array_element<std::int8_t>(arr, i)));
        case 'C':
        {
            const std::uint16_t ch{ vmhook::get_array_element<std::uint16_t>(arr, i) };
            if (ch >= 32u && ch < 127u) { return std::string{ "'" } + static_cast<char>(ch) + "'"; }
            return std::to_string(static_cast<unsigned>(ch));
        }
        case 'S': return std::to_string(static_cast<int>(vmhook::get_array_element<std::int16_t>(arr, i)));
        case 'I': return std::to_string(vmhook::get_array_element<std::int32_t>(arr, i));
        case 'F': return std::to_string(vmhook::get_array_element<float>(arr, i));
        case 'J': return std::to_string(vmhook::get_array_element<std::int64_t>(arr, i));
        case 'D': return std::to_string(vmhook::get_array_element<double>(arr, i));
        case 'L':
        case '[':
        {
            const std::uint32_t compressed{ vmhook::get_array_element<std::uint32_t>(arr, i) };
            if (compressed == 0u) { return "null"; }
            void* const ref{ vmhook::hotspot::decode_oop_pointer(compressed) };
            if (!ref || !vmhook::hotspot::is_valid_pointer(ref)) { return "null"; }
            if (ref_out)
            {
                char rb[32];
                std::snprintf(rb, sizeof(rb), "0x%llX", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(ref)));
                *ref_out = rb;
            }
            if (elem_desc == "Ljava/lang/String;") { return std::string{ "\"" } + vmhook::read_java_string(ref) + "\""; }
            if (vmhook::hotspot::klass* const rk{ vmhook::klass_from_oop(ref) }; rk && vmhook::hotspot::is_valid_pointer(rk))
                if (const vmhook::hotspot::symbol* const rn{ rk->get_name() }; rn && vmhook::hotspot::is_valid_pointer(rn))
                    return std::string{ "<" } + rn->to_string() + ">";
            return "<object>";
        }
        default: return "?";
        }
    }

    // Stream a Java array's elements.  arg = "<0xADDR>\t<elemDescriptor>\t<max>".
    // Reply: A <TAB> length ; then per element  V <TAB> index <TAB> value <TAB> <TAB> refAddr ; DONE.
    void stream_array(HANDLE pipe, const std::string& arg)
    {
        pipe_writer writer{ pipe };
        std::vector<std::string> parts;
        std::size_t pos{ 0 };
        for (int i = 0; i < 2; ++i)
        {
            const std::size_t tab{ arg.find('\t', pos) };
            if (tab == std::string::npos) { writer.put("DONE\t0\n"); writer.flush(); FlushFileBuffers(pipe); return; }
            parts.push_back(arg.substr(pos, tab - pos));
            pos = tab + 1;
        }
        parts.push_back(arg.substr(pos));  // max

        void* const arr{ reinterpret_cast<void*>(static_cast<std::uintptr_t>(std::strtoull(parts[0].c_str(), nullptr, 16))) };
        const std::string elem_desc{ parts[1] };
        std::int32_t max{ static_cast<std::int32_t>(std::strtol(parts[2].c_str(), nullptr, 10)) };
        if (max <= 0) { max = 4096; }
        if (!arr || !vmhook::hotspot::is_valid_pointer(arr)) { writer.put("DONE\t0\n"); writer.flush(); FlushFileBuffers(pipe); return; }

        const std::int32_t length{ vmhook::array_length(arr) };
        writer.put("A\t"); writer.put(std::to_string(length)); writer.put("\n");
        const std::int32_t n{ length < max ? length : max };
        for (std::int32_t i = 0; i < n; ++i)
        {
            std::string ref_addr;
            const std::string value{ read_array_element(arr, elem_desc, i, &ref_addr) };
            writer.put("V\t"); writer.put(std::to_string(i)); writer.put("\t");
            writer.put(sanitize(value)); writer.put("\t\t"); writer.put(ref_addr); writer.put("\n");
        }
        writer.put("DONE\t"); writer.put(std::to_string(n)); writer.put("\n");
        writer.flush(); FlushFileBuffers(pipe);
    }

    // Write one array element (bounds-checked by vmhook::set_array_element).  Same
    // value grammar as apply_set: primitives from the literal; a reference element
    // takes null / 0x<oop> / (for String[]) the literal text.  Returns "" or error.
    auto apply_array_set(void* const arr, const std::string& elem_desc, std::int32_t i, const std::string& value) -> std::string
    {
        if (elem_desc.empty()) { return "unknown element type"; }
        const auto to_ll{ [&] { return std::strtoll(value.c_str(), nullptr, 0); } };
        switch (elem_desc[0])
        {
        case 'Z': vmhook::set_array_element<std::uint8_t>(arr, i, (value == "true" || value == "TRUE" || value == "1") ? 1u : 0u); break;
        case 'B': vmhook::set_array_element<std::int8_t>(arr, i, static_cast<std::int8_t>(to_ll())); break;
        case 'S': vmhook::set_array_element<std::int16_t>(arr, i, static_cast<std::int16_t>(to_ll())); break;
        case 'C': vmhook::set_array_element<std::uint16_t>(arr, i, value.size() == 1
                      ? static_cast<std::uint16_t>(static_cast<unsigned char>(value[0]))
                      : static_cast<std::uint16_t>(std::strtoul(value.c_str(), nullptr, 0))); break;
        case 'I': vmhook::set_array_element<std::int32_t>(arr, i, static_cast<std::int32_t>(to_ll())); break;
        case 'J': vmhook::set_array_element<std::int64_t>(arr, i, static_cast<std::int64_t>(to_ll())); break;
        case 'F': vmhook::set_array_element<float>(arr, i, std::strtof(value.c_str(), nullptr)); break;
        case 'D': vmhook::set_array_element<double>(arr, i, std::strtod(value.c_str(), nullptr)); break;
        case 'L':
        case '[':
        {
            if (value == "null") { vmhook::set_array_element<std::uint32_t>(arr, i, 0u); break; }
            if (value.rfind("0x", 0) == 0 || value.rfind("0X", 0) == 0)
            {
                void* const ref{ reinterpret_cast<void*>(static_cast<std::uintptr_t>(std::strtoull(value.c_str(), nullptr, 16))) };
                if (ref && !vmhook::hotspot::is_valid_pointer(ref)) { return "the element oop address is not readable"; }
                vmhook::set_array_element<std::uint32_t>(arr, i, vmhook::hotspot::encode_oop_pointer(ref));
                break;
            }
            if (elem_desc == "Ljava/lang/String;")
            {
                void* const s{ alloc_java_string(value) };
                if (!s || !vmhook::hotspot::is_valid_pointer(s)) { return "failed to allocate the String element"; }
                vmhook::set_array_element<std::uint32_t>(arr, i, vmhook::hotspot::encode_oop_pointer(s));
                break;
            }
            return "reference element: pass 'null', a 0x<oop> address, or (for String[]) the literal text";
        }
        default: return "unsupported element type";
        }
        return "";
    }

    // ARRSET: write one array element and stream the re-read value back.
    //   arg = "<0xADDR>\t<elemDescriptor>\t<index>\t<value>"
    void stream_array_set(HANDLE pipe, const std::string& arg)
    {
        pipe_writer writer{ pipe };
        const auto bail{ [&](const std::string& msg)
        { writer.put("E\t"); writer.put(sanitize(msg)); writer.put("\n"); writer.put("DONE\t0\n"); writer.flush(); FlushFileBuffers(pipe); } };

        std::vector<std::string> parts;
        std::size_t pos{ 0 };
        for (int i = 0; i < 3; ++i)
        {
            const std::size_t tab{ arg.find('\t', pos) };
            if (tab == std::string::npos) { bail("malformed array-set request"); return; }
            parts.push_back(arg.substr(pos, tab - pos));
            pos = tab + 1;
        }
        parts.push_back(arg.substr(pos));  // value

        void* const arr{ reinterpret_cast<void*>(static_cast<std::uintptr_t>(std::strtoull(parts[0].c_str(), nullptr, 16))) };
        const std::string elem_desc{ parts[1] };
        const std::int32_t index{ static_cast<std::int32_t>(std::strtol(parts[2].c_str(), nullptr, 10)) };
        if (!arr || !vmhook::hotspot::is_valid_pointer(arr)) { bail("invalid array address"); return; }
        if (index < 0 || index >= vmhook::array_length(arr)) { bail("index out of bounds"); return; }

        if (const std::string err{ apply_array_set(arr, elem_desc, index, parts[3]) }; !err.empty()) { bail(err); return; }

        std::string ref_addr;
        const std::string reread{ read_array_element(arr, elem_desc, index, &ref_addr) };
        writer.put("V\t"); writer.put(std::to_string(index)); writer.put("\t");
        writer.put(sanitize(reread)); writer.put("\t\t"); writer.put(ref_addr); writer.put("\n");
        writer.put("DONE\t1\n"); writer.flush(); FlushFileBuffers(pipe);
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
            // Repoint by raw oop FIRST — this is the pre-resolved form the freeze
            // thread re-applies, and it needs no JavaThread (a plain compressed-oop
            // write).  Only a NON-0x value for a String field allocates a new String
            // (which requires a JavaThread — off the freeze thread's hot path).
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

    // Locate a Method* by name+descriptor on a klass, walking its InstanceKlass
    // _methods array.  Pointer-validated throughout; nullptr means "no such
    // method", which the caller reports rather than guessing.
    auto find_method_on(vmhook::hotspot::klass* const k,
                        const std::string& name, const std::string& desc)
        -> vmhook::hotspot::method*
    {
        if (!k || !vmhook::hotspot::is_valid_pointer(k)) { return nullptr; }
        const std::int32_t count{ k->get_methods_count() };
        vmhook::hotspot::method** const methods{ k->get_methods_ptr() };
        if (!methods || count <= 0) { return nullptr; }
        for (std::int32_t i{ 0 }; i < count; ++i)
        {
            vmhook::hotspot::method* const m{ methods[i] };
            if (!m || !vmhook::hotspot::is_valid_pointer(m)) { continue; }
            const std::string mn = m->get_name();
            const std::string ms = m->get_signature();
            if (mn == name && ms == desc) { return m; }
        }
        return nullptr;
    }

    // Invoke a method PURE-VM, from INSIDE a pump detour (a real JavaThread).
    //
    // This replaced ~145 lines of JNI (GetMethodID + a Call*MethodA switch over
    // every return type + local-ref bookkeeping + a NewObjectArray trick to
    // manufacture jobjects from raw oops).  All of it existed for one reason:
    // method_proxy::call() used to be a silent no-op, because
    // find_call_stub_entry() looked for a VMStructs entry that no JDK has ever
    // published.  It is now derived from _call_stub_return_address and works on
    // 8 / 21 / 26 alike, so the library does the whole job and the descriptor
    // switch collapses into value_t.
    //
    // k = target klass; receiver is the raw oop (ignored for statics).
    auto invoke_vm(void* const receiver, vmhook::hotspot::klass* const k, const bool is_static,
                   const std::string& name, const std::string& sig, const std::vector<std::string>& argtoks,
                   std::string& kind, std::string& disp, std::string& raddr, std::string& rclass) -> std::string
    {
        vmhook::hotspot::method* const m{ find_method_on(k, name, sig) };
        if (!m) { return "no method " + name + sig + " on the target class"; }

        const std::vector<std::string> pds{ parse_param_descriptors(sig) };
        if (argtoks.size() != pds.size())
        {
            return "argument count mismatch (expected " + std::to_string(pds.size()) + ")";
        }

        // Build the runtime argument list.  A reference parameter accepts either
        // "@0xADDR" / "0xADDR" (an object the user already has, e.g. dragged from
        // the clipboard) or "#text" / bare text, which becomes a new String.
        std::vector<vmhook::method_proxy::java_arg> args;
        args.reserve(pds.size());
        for (std::size_t a = 0; a < pds.size(); ++a)
        {
            const std::string& pd{ pds[a] };
            const std::string& tok{ argtoks[a] };
            const char letter{ pd.empty() ? '?' : pd[0] };

            if (letter == 'L' || letter == '[')
            {
                if (tok == "@null" || tok == "null" || tok.empty())
                {
                    args.push_back(vmhook::method_proxy::java_arg::of_object(nullptr));
                }
                else if (tok.rfind("@0x", 0) == 0 || tok.rfind("@0X", 0) == 0
                         || tok.rfind("0x", 0) == 0 || tok.rfind("0X", 0) == 0)
                {
                    const char* hex{ tok.c_str() + (tok[0] == '@' ? 1 : 0) };
                    void* const ref{ reinterpret_cast<void*>(
                        static_cast<std::uintptr_t>(std::strtoull(hex, nullptr, 16))) };
                    if (ref && !vmhook::hotspot::is_valid_pointer(ref))
                    {
                        return "argument oop is not readable: " + tok;
                    }
                    args.push_back(vmhook::method_proxy::java_arg::of_object(ref));
                }
                else
                {
                    args.push_back(vmhook::method_proxy::java_arg::of_string(
                        tok.rfind('#', 0) == 0 ? tok.substr(1) : tok));
                }
                continue;
            }

            auto built{ vmhook::method_proxy::java_arg::from_descriptor(letter, tok) };
            if (!built) { return std::string{ "unsupported parameter type: " } + pd; }
            args.push_back(*std::move(built));
        }

        // A constructor is dispatched on a freshly allocated, UNINITIALISED
        // object: allocate, then run <init> on it as an instance call.  This is
        // what JNI's NewObjectA does internally, and doing it explicitly keeps
        // the whole path inside the library's one dispatcher.
        const bool is_ctor{ name == "<init>" };
        void* target_receiver{ receiver };
        if (is_ctor)
        {
            target_receiver = vmhook::make_java_object(k, k->get_instance_size());
            if (!target_receiver) { return "failed to allocate the object to construct"; }
        }

        const vmhook::method_proxy proxy{ is_static ? nullptr : target_receiver, m, sig };
        const auto result{ proxy.call_packed(m, args) };

        if (result.threw())
        {
            return "the method threw "
                   + (result.exception_class.empty() ? std::string{ "an exception" }
                                                     : result.exception_class);
        }

        const std::size_t rparen{ sig.rfind(')') };
        const char ret{ (rparen != std::string::npos && rparen + 1 < sig.size())
                            ? sig[rparen + 1] : 'V' };

        // A constructor "returns" the object it initialised.
        if (is_ctor)
        {
            char rb[32];
            std::snprintf(rb, sizeof(rb), "0x%llX",
                          static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(target_receiver)));
            raddr = rb; kind = "ref";
            if (const vmhook::hotspot::symbol* const kn{ k->get_name() };
                kn && vmhook::hotspot::is_valid_pointer(kn))
            { rclass = kn->to_string(); disp = std::string{ "<" } + rclass + ">"; }
            else { disp = "<object>"; }
            return "";
        }

        switch (ret)
        {
        case 'V': kind = "void";   disp = "(void)"; break;
        case 'Z': kind = "bool";   disp = static_cast<bool>(result) ? "true" : "false"; break;
        case 'B': kind = "byte";   disp = std::to_string(static_cast<int>(static_cast<std::int8_t>(result))); break;
        case 'S': kind = "short";  disp = std::to_string(static_cast<int>(static_cast<std::int16_t>(result))); break;
        case 'C':
        {
            const auto c{ static_cast<std::uint16_t>(result) };
            kind = "char";
            disp = (c >= 32 && c < 127) ? (std::string{ "'" } + static_cast<char>(c) + "'")
                                        : std::to_string(static_cast<unsigned>(c));
            break;
        }
        case 'I': kind = "int";    disp = std::to_string(static_cast<std::int32_t>(result)); break;
        case 'J': kind = "long";   disp = std::to_string(static_cast<long long>(static_cast<std::int64_t>(result))); break;
        case 'F': kind = "float";  disp = fmt_g(static_cast<double>(static_cast<float>(result)), 7); break;
        case 'D': kind = "double"; disp = fmt_g(static_cast<double>(result), 15); break;
        default:  // 'L' / '[' — object / array
        {
            if (result.is_string())
            {
                kind = "string"; rclass = "java/lang/String";
                disp = std::string{ "\"" } + result.as_string() + "\"";
                break;
            }
            const auto handle{ result.to_borrowed<>() };
            void* const oop{ handle.raw_unsafe() };
            if (!oop) { kind = "null"; disp = "null"; break; }
            char rb[32];
            std::snprintf(rb, sizeof(rb), "0x%llX",
                          static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(oop)));
            raddr = rb; kind = "ref";
            if (vmhook::hotspot::klass* const rk{ vmhook::klass_from_oop(oop) };
                rk && vmhook::hotspot::is_valid_pointer(rk))
            {
                if (const vmhook::hotspot::symbol* const rn{ rk->get_name() };
                    rn && vmhook::hotspot::is_valid_pointer(rn))
                { rclass = rn->to_string(); disp = std::string{ "<" } + rclass + ">"; }
                else { disp = "<object>"; }
            }
            else { disp = "<object>"; }
            break;
        }
        }
        return "";
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
        if (!is_static && !receiver && mname != "<init>") { bail("this instance method needs a receiver address"); return; }

        // Invocation must happen INSIDE a detour on a real JavaThread (the JNI
        // Call* path sets up the proper Java frame there).  run_on_java_thread
        // fires the trigger hook and runs the task in its detour.
        std::string kind, disp, raddr, rclass, err;
        const bool ran{ run_on_java_thread([&] { err = invoke_vm(receiver, k, is_static, mname, desc, args, kind, disp, raddr, rclass); }) };
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

    // DRAINF request: drain the hook and stream each newly-defined class's FULL
    // surface (C/M/F), so the viewer can add just those classes to its list
    // WITHOUT re-enumerating everything.  The class is fully defined by drain time
    // (defineClass has returned), so its methods/fields resolve.  Deduped per batch;
    // a name whose klass can't be resolved (unloaded / not yet linked) is skipped.
    void stream_hook_drain_full(HANDLE pipe)
    {
        std::vector<std::string> names;
        {
            std::lock_guard<std::mutex> lock{ g_hook_mtx };
            names.swap(g_hook_log);
        }
        pipe_writer writer{ pipe };
        std::unordered_set<std::string> seen;
        std::uint64_t count{ 0 };
        for (const std::string& n : names)
        {
            if (n.empty() || !seen.insert(n).second) { continue; }
            vmhook::hotspot::klass* const k{ vmhook::find_class(n) };
            if (!k || !vmhook::hotspot::is_valid_pointer(k)) { continue; }
            emit_class_surface(writer, n, k);
            ++count;
        }
        writer.put("DONE\t");
        writer.put(std::to_string(count));
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
            else if (req.rfind("ARRSET\t", 0) == 0)
            {
                stream_array_set(pipe, req.substr(7));
            }
            else if (req.rfind("ARR\t", 0) == 0)
            {
                stream_array(pipe, req.substr(4));
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
            else if (req == "DRAINF")
            {
                stream_hook_drain_full(pipe);
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
