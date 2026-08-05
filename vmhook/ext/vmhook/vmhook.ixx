// vmhook.ixx — the C++20 named-module interface for vmhook.
//
// ===========================================================================
// WHAT THIS IS, AND WHY IT IS SHAPED THIS WAY
// ===========================================================================
// `import vmhook;` instead of `#include <vmhook/vmhook.hpp>`.  Same library,
// same semantics; what changes is that the 24 000-line header is parsed once
// into a BMI rather than once per translation unit, and that vmhook's names no
// longer leak into consumers by textual inclusion.
//
// The header remains the single source of truth.  This file does NOT duplicate
// it — it includes it in the GLOBAL MODULE FRAGMENT and re-exports the public
// surface with `export using`.  That is the standard migration shape for a
// header-only library (fmt and Boost do the same), and it is the only shape
// that works here, for three concrete reasons:
//
//   1. The header must be included BEFORE `export module`, in the global module
//      fragment.  Everything it declares then keeps its normal external linkage
//      and stays attached to the global module, so a program that mixes
//      `import vmhook;` in one TU with `#include <vmhook/vmhook.hpp>` in
//      another sees ONE set of entities, not two.  Include it in the purview
//      instead and those become different entities: an ODR violation that
//      typically shows up as a linker error, or worse, as two copies of the
//      hook registry.
//
//   2. vmhook.hpp declares several functions `static` (register_class,
//      find_class, ...).  A `static` entity has internal linkage and CANNOT be
//      exported; wrapping the include in `export { }` is ill-formed the moment
//      the compiler reaches one.
//
//   3. The header specialises `std::hash` for its identity types.  A
//      specialisation of a template owned by another module cannot be exported,
//      and does not need to be — it is found by the specialisation lookup rules
//      through the global module, which is exactly where it is.
//
// MACROS ARE NOT EXPORTED.  Modules do not export macros; that is a language
// rule, not an omission.  A consumer needing VMHOOK_LOG, VMHOOK_DEBUG_LOGS, or
// any VMHOOK_HAS_* capability macro must `#include <vmhook/vmhook.hpp>` — which
// is fine and costs nothing extra, because both spellings name the same
// entities.  Everything else — every type, function, enum and constant a user
// touches — is below.
//
// BUILDING.  Needs a compiler with named-module support and a build system that
// scans for module dependencies: MSVC 19.28+, GCC 14+ with -fmodules-ts, or
// Clang 16+.  With CMake, that is CXX_SCAN_FOR_MODULES plus a FILE_SET of type
// CXX_MODULES; the header-only target is unaffected and both can coexist.
// ===========================================================================

module;

// ---------------------------------------------------------------------------
// Global module fragment.  vmhook.hpp pulls these in itself; including them
// here first is what keeps them attached to the global module rather than to
// `vmhook`, which is the whole point of the fragment.
// ---------------------------------------------------------------------------
#include <vmhook/vmhook.hpp>

export module vmhook;

// ---------------------------------------------------------------------------
// Re-exported public surface.
//
// Order mirrors the header's own sections so the two stay diffable.  A name
// missing here is not an error at build time — it simply is not reachable via
// `import`, and shows up as an "undeclared identifier" at the consumer.  When
// adding a public entity to the header, add it here too.
// ---------------------------------------------------------------------------
export namespace vmhook
{
    // ── diagnostics ────────────────────────────────────────────────────────
    using vmhook::exception;
    using vmhook::error_tag;
    using vmhook::warning_tag;
    using vmhook::info_tag;

    // ── the OOP alias and the object base ──────────────────────────────────
    using vmhook::oop_type_t;
    using vmhook::oop_t;
    using vmhook::object_base;
    using vmhook::object;
    using vmhook::oop_reflective_base;

    // ── proxies ────────────────────────────────────────────────────────────
    using vmhook::field_proxy;
    using vmhook::method_proxy;
    using vmhook::return_value;

    // ── hooking ────────────────────────────────────────────────────────────
    using vmhook::hook;
    using vmhook::hook_by_signature;
    using vmhook::scoped_hook;
    using vmhook::hook_handle;
    using vmhook::verify_hooks;
    using vmhook::shutdown_hooks;
    using vmhook::auto_repair_enabled;
    using vmhook::set_auto_repair_enabled;

    // ── watchers ───────────────────────────────────────────────────────────
    using vmhook::watch_handle;
    using vmhook::watch_static_field;
    using vmhook::on_class_loaded;
    using vmhook::on_exception;

    // ── class / method / field lookup ──────────────────────────────────────
    using vmhook::register_class;
    using vmhook::find_class;
    using vmhook::find_class_via_oop;
    using vmhook::override_class_lookup;
    using vmhook::evict_class_lookup;
    using vmhook::reanchor_classes_via_oop;
    using vmhook::resolve_array_klass;
    using vmhook::find_field;
    using vmhook::find_methods_by_signature;
    using vmhook::get_class_methods;
    using vmhook::log_class_methods;
    using vmhook::klass_from_oop;
    using vmhook::is_instance_of;

    // ── the typed field helpers ────────────────────────────────────────────
    using vmhook::get_field;
    using vmhook::set_field;
    using vmhook::field_oop;
    using vmhook::set_str_field;
    using vmhook::set_bool_array;
    using vmhook::set_prim_array;
    using vmhook::set_str_array;

    // ── arrays and strings ─────────────────────────────────────────────────
    using vmhook::array_length;
    using vmhook::get_array_element;
    using vmhook::set_array_element;
    using vmhook::decode_array_oop;
    using vmhook::read_java_string;
    using vmhook::write_java_string;
    using vmhook::read_java_string_max_units;
    using vmhook::clamp_safe_container_count;
    using vmhook::k_max_safe_container_elems;

    // ── allocation: raw and handle-returning ───────────────────────────────
    using vmhook::make_java_object;
    using vmhook::make_java_array;
    using vmhook::make_java_string;
    using vmhook::make_unique;
    using vmhook::new_object;
    using vmhook::new_array;
    using vmhook::new_string;

    // ── enumeration ────────────────────────────────────────────────────────
    using vmhook::for_each_loaded_class;
    using vmhook::for_each_instance;
    using vmhook::for_each_instance_of;
    using vmhook::for_each_thread;
    using vmhook::thread_info;

    // ── deoptimisation ─────────────────────────────────────────────────────
    using vmhook::deoptimize_methods_if;
    using vmhook::deoptimize_all_jit_compiled_methods;

    // ── collection wrappers ────────────────────────────────────────────────
    using vmhook::collection;
    using vmhook::list;
    using vmhook::set;
    using vmhook::linked_list;
    using vmhook::map;
    using vmhook::hash_map;

    // ── GC observation ─────────────────────────────────────────────────────
    using vmhook::gc_epoch;
    using vmhook::gc_epoch_t;
    using vmhook::gc_epoch_changed;
    using vmhook::gc_collector;
    using vmhook::gc_collector_name;
    using vmhook::gc_barrier_shape;
    using vmhook::vm_capabilities;
    using vmhook::vm_capabilities_t;

    // ── the anchored-reference model (Goal B) ──────────────────────────────
    using vmhook::ref;
    using vmhook::root;
    using vmhook::borrowed;
    using vmhook::ref_vector;
    using vmhook::object_id;
    using vmhook::anchor_kind;
    using vmhook::anchor_kind_name;
    using vmhook::borrow;
    using vmhook::ephemeral_ref;
    using vmhook::static_ref;
    using vmhook::elements_of;
    using vmhook::oop_pin;
    using vmhook::pin;

    // ── error reporting for the try_* accessors ────────────────────────────
    using vmhook::access_error;
    using vmhook::error_message;

    // ── C++26 reflection: the annotation that names a wrapper's Java class ──
    using vmhook::java_class;

    // ── the registry the descriptor builder consults ───────────────────────
    using vmhook::type_to_class_map;
}

// ---------------------------------------------------------------------------
// The HotSpot metadata layer.  Advanced surface: raw structure views, exposed
// because hooks legitimately need Method* / klass* / symbol*, and because every
// enumeration callback hands them out.
// ---------------------------------------------------------------------------
export namespace vmhook::hotspot
{
    using vmhook::hotspot::klass;
    using vmhook::hotspot::method;
    using vmhook::hotspot::symbol;
    using vmhook::hotspot::const_method;
    using vmhook::hotspot::constant_pool;
    using vmhook::hotspot::field_entry_t;
    using vmhook::hotspot::frame;
    using vmhook::hotspot::java_thread;
    using vmhook::hotspot::java_thread_state;
    using vmhook::hotspot::return_slot;
    using vmhook::hotspot::is_valid_pointer;
    using vmhook::hotspot::decode_oop_pointer;
    using vmhook::hotspot::encode_oop_pointer;
    using vmhook::hotspot::iterate_struct_entries;
    using vmhook::hotspot::iterate_type_entries;
    using vmhook::hotspot::current_java_thread;
}

// ---------------------------------------------------------------------------
// The OS abstraction.  Exported because the fault-safe primitives are the
// documented way for a consumer to read JVM memory without risking an AV, and
// because a payload frequently needs the module/thread helpers.
// ---------------------------------------------------------------------------
export namespace vmhook::os
{
    using vmhook::os::safe_read;
    using vmhook::os::safe_write;
    using vmhook::os::page_size;
    using vmhook::os::allocation_granularity;
    using vmhook::os::protect;
    using vmhook::os::query_region;
    using vmhook::os::region_info;
    using vmhook::os::memory_protection;
    using vmhook::os::find_loaded_module;
    using vmhook::os::find_jvm_module;
    using vmhook::os::get_proc_address;
    using vmhook::os::current_thread_id;
    using vmhook::os::module_handle;
    using vmhook::os::thread_id_t;
}
