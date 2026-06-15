"""Shared registry helpers for vmhook feature manifests.

The "feature registry" is the set of YAML files in this folder
(`audit/features/<slug>.yaml`), one per vmhook feature.  Each
manifest is the *single source of truth* for that feature's
metadata: where it lives in the header, what tests cover it,
which other features it depends on, which specialist agent
owns it, and what is known to be broken.

The Obsidian vault under `audit/graph/` and the machine-readable
`audit/graph/_data/graph.json` are *derived* from the manifests
by `generate.py`.  `validate.py` enforces invariants on the
manifests in CI so the registry cannot silently rot.

This module is plain Python 3.8 + PyYAML.  Do not import anything
from the rest of the repo here; the registry must stay self-contained.
"""

from __future__ import annotations

import sys
from pathlib import Path
from typing import Dict, Iterable, List, Set, Tuple

import yaml  # type: ignore


# ----------------------------------------------------------------------
# Paths
# ----------------------------------------------------------------------

THIS_FILE = Path(__file__).resolve()
FEATURES_DIR = THIS_FILE.parent                   # audit/features
AUDIT_DIR = FEATURES_DIR.parent                   # audit
REPO_ROOT = AUDIT_DIR.parent                      # repo root
GRAPH_DIR = AUDIT_DIR / "graph"
AGENTS_DIR = REPO_ROOT / ".claude" / "agents"
HPP_PATH = REPO_ROOT / "vmhook" / "ext" / "vmhook" / "vmhook.hpp"
TESTS_DIR = REPO_ROOT / "tests"
JVM_MODULES_DIR = TESTS_DIR / "jvm" / "modules"


# ----------------------------------------------------------------------
# Schema
# ----------------------------------------------------------------------

SCHEMA_VERSION = 1

REQUIRED_FIELDS = (
    "schema_version",
    "slug",
    "title",
    "category",
    "status",
    "risk",
    "specialist",
    "description",
    "java_versions",
    "tags",
    "hpp_anchors",
    "test_modules",
    "depends_on",
    "related",
    "audit_docs",
    "known_bugs",
    "notes",
)

ALLOWED_STATUS = {"queued", "seeded", "in_progress", "audited", "perfected"}
ALLOWED_RISK = {"low", "medium", "high", "critical"}
DEFAULT_JAVA_VERSIONS = [8, 11, 17, 21, 24, 25, 26]


# ----------------------------------------------------------------------
# Categories — every feature belongs to exactly one.
# ----------------------------------------------------------------------

CATEGORIES: Dict[str, Tuple[str, List[str]]] = {
    # slug -> (display_title, [feature slugs])
    "bootstrap":   ("Bootstrap / DLL entry", [
        "dllmain_bootstrap",
    ]),
    "hook":        ("Hooking machinery (install / dispatch / trampolines)", [
        "hook_basic", "hook_chaining", "hook_common_detour_dispatch",
        "hook_install_after_jit", "hook_reinstall_after_shutdown",
        "hook_signature", "hook_unhook_double_free", "hook_verify_repair",
        "midi2i_trampoline_alloc", "seh_invoke_detour",
        "dont_inline_dont_compile", "adapter_recovery_c2i",
        "method_entry_points_i2i_i2c",
    ]),
    "deopt":       ("De-optimisation", [
        "deoptimize_methods",
    ]),
    "klass":       ("Class / Klass introspection", [
        "klass_introspection", "compressed_klass_decode",
        "compressed_oops_decode", "vmstructs_offset_resolution",
        "instanceklass_methods_walk", "classloader_reanchor",
        "find_class_context_loader", "find_class_fallback",
        "decode_oop_and_pointers", "poly_inherited_oop",
        "nested_classes", "interface_polymorphism",
        "register_class", "const_method_bounds", "constantpool_access",
    ]),
    "method":      ("Method proxies (resolve / call / dispatch)", [
        "method_call_jni_fallback", "method_call_object",
        "method_call_primitives", "method_call_return_void",
        "method_call_string", "method_call_wide_args",
        "method_enumeration", "method_explicit_signature",
        "method_flags_width", "method_is_reference",
        "method_overload", "method_overload_java_dispatch",
        "method_proxy_value_t", "method_return_types",
        "method_static", "method_static_portability",
        "method_throwing_call_site", "find_methods_by_signature",
        "signature_parsing",
    ]),
    "field":       ("Field proxies (get / set / introspection)", [
        "field_arrays_object", "field_arrays_primitive",
        "field_inherited", "field_introspection", "field_null_safety",
        "field_object_ref", "field_primitives_get", "field_primitives_set",
        "field_proxy_set_guards", "field_proxy_value_t",
        "field_set_size_guard", "field_static", "field_string",
        "watch_static_field",
    ]),
    "collection":  ("Collection wrappers + element helpers", [
        "collection_hash_tree_map", "collection_iteration_safety",
        "collection_linked_list", "collection_list", "collection_map",
        "collection_set", "collection_type_tags", "array_element_helpers",
    ]),
    "return":      ("return_value (detour-side return manipulation)", [
        "return_caller", "return_frame_raw_access", "return_set_arg",
        "return_set_primitives", "return_set_wrapper_null",
        "return_stack_trace_depth", "return_value_cancel",
        "interpreter_frame_walk",
    ]),
    "jni":         ("JNI plumbing (arg packing / refs / java values)", [
        "jni_arg_packing", "jni_local_ref_hygiene", "make_java_array",
        "make_java_string", "read_java_string", "global_ref",
    ]),
    "enumeration": ("Live-VM enumeration (heap / classes / threads)", [
        "for_each_instance", "for_each_loaded_class", "for_each_thread",
        "iterate_entries_safety",
    ]),
    "os":          ("OS abstraction (memory / signals / breakpoints)", [
        "os_allocate_release", "os_page_size_granularity", "os_protect",
        "os_query_region", "os_safe_read", "os_signal_handler",
        "hw_breakpoint_dr7",
    ]),
    "lifecycle":   ("Lifecycle hooks (shutdown / class-load / exception / enum)", [
        "shutdown_hooks_teardown", "on_class_loaded", "on_exception",
        "enum_singleton",
    ]),
    "infra":       ("Infrastructure (wrappers, traits, macros, logging)", [
        "api_surface_no_jvm", "wrapper_pattern", "make_unique",
        "logging_format", "unified_call_syntax", "traits_function_traits",
        "version_macros", "platform_capability_macros", "decode_u5_unsigned5",
    ]),
}

# slug -> category slug
SLUG_TO_CATEGORY: Dict[str, str] = {
    slug: cat
    for cat, (_title, slugs) in CATEGORIES.items()
    for slug in slugs
}


def category_title(cat: str) -> str:
    """Display title of a category."""
    if cat in CATEGORIES:
        return CATEGORIES[cat][0]
    return cat


# ----------------------------------------------------------------------
# Dependency seed.  Each entry is a best-effort starting set of
# `depends_on` for a feature, used only when bootstrapping a stub
# manifest.  Hand-edits to a manifest take precedence.
# ----------------------------------------------------------------------

DEPS_SEED: Dict[str, List[str]] = {
    # --- hook cluster --------------------------------------------------
    "hook_basic": [
        "midi2i_trampoline_alloc", "hook_common_detour_dispatch",
        "method_entry_points_i2i_i2c", "find_class_fallback",
        "klass_introspection", "decode_oop_and_pointers",
        "interpreter_frame_walk", "signature_parsing",
    ],
    "hook_chaining": [
        "hook_basic", "midi2i_trampoline_alloc",
        "hook_common_detour_dispatch",
    ],
    "hook_install_after_jit": [
        "hook_basic", "deoptimize_methods",
        "method_entry_points_i2i_i2c", "adapter_recovery_c2i",
    ],
    "hook_reinstall_after_shutdown": [
        "shutdown_hooks_teardown", "hook_basic",
    ],
    "hook_signature": [
        "hook_basic", "signature_parsing", "method_explicit_signature",
    ],
    "hook_unhook_double_free": [
        "hook_basic", "shutdown_hooks_teardown",
    ],
    "hook_verify_repair": [
        "hook_basic", "midi2i_trampoline_alloc",
    ],
    "hook_common_detour_dispatch": [
        "seh_invoke_detour", "interpreter_frame_walk",
        "method_entry_points_i2i_i2c",
    ],
    "midi2i_trampoline_alloc": [
        "os_allocate_release", "os_protect",
        "os_query_region", "os_page_size_granularity",
    ],
    "seh_invoke_detour": ["os_signal_handler"],
    "dont_inline_dont_compile": ["method_flags_width", "hook_basic"],
    "adapter_recovery_c2i": [
        "method_entry_points_i2i_i2c", "vmstructs_offset_resolution",
    ],
    "method_entry_points_i2i_i2c": ["vmstructs_offset_resolution"],
    "deoptimize_methods": [
        "method_enumeration", "adapter_recovery_c2i",
        "method_entry_points_i2i_i2c",
    ],

    # --- klass cluster -------------------------------------------------
    "klass_introspection": [
        "vmstructs_offset_resolution", "instanceklass_methods_walk",
        "compressed_klass_decode",
    ],
    "compressed_klass_decode": ["vmstructs_offset_resolution"],
    "compressed_oops_decode": ["vmstructs_offset_resolution"],
    "vmstructs_offset_resolution": ["os_safe_read"],
    "instanceklass_methods_walk": [
        "vmstructs_offset_resolution",
    ],
    "classloader_reanchor": ["find_class_fallback"],
    "find_class_context_loader": ["find_class_fallback"],
    "find_class_fallback": [
        "vmstructs_offset_resolution", "klass_introspection",
    ],
    "decode_oop_and_pointers": [
        "compressed_oops_decode", "os_safe_read",
    ],
    "poly_inherited_oop": ["klass_introspection", "field_inherited"],
    "nested_classes": ["klass_introspection"],
    "interface_polymorphism": ["klass_introspection", "method_overload"],
    "register_class": ["wrapper_pattern", "find_class_fallback"],
    "const_method_bounds": ["constantpool_access"],
    "constantpool_access": ["vmstructs_offset_resolution"],

    # --- method cluster ------------------------------------------------
    "method_enumeration": ["instanceklass_methods_walk"],
    "method_explicit_signature": [
        "signature_parsing", "method_enumeration",
    ],
    "method_overload": [
        "signature_parsing", "method_explicit_signature",
    ],
    "method_overload_java_dispatch": ["method_overload"],
    "method_flags_width": ["vmstructs_offset_resolution"],
    "method_call_primitives": [
        "method_proxy_value_t", "method_enumeration",
    ],
    "method_call_string": [
        "method_call_primitives", "make_java_string", "read_java_string",
    ],
    "method_call_object": [
        "method_call_primitives", "wrapper_pattern",
    ],
    "method_call_return_void": ["method_call_primitives"],
    "method_call_wide_args": [
        "method_call_primitives", "jni_arg_packing",
    ],
    "method_call_jni_fallback": [
        "method_call_primitives", "jni_arg_packing", "jni_local_ref_hygiene",
    ],
    "method_return_types": ["method_call_primitives"],
    "method_static": ["method_call_primitives"],
    "method_static_portability": ["method_static"],
    "method_is_reference": ["signature_parsing"],
    "method_throwing_call_site": [
        "method_call_primitives", "on_exception",
    ],
    "method_proxy_value_t": ["wrapper_pattern"],
    "find_methods_by_signature": [
        "signature_parsing", "method_enumeration",
    ],
    "signature_parsing": [],

    # --- field cluster -------------------------------------------------
    "field_arrays_object": [
        "field_object_ref", "array_element_helpers", "wrapper_pattern",
    ],
    "field_arrays_primitive": [
        "field_primitives_get", "array_element_helpers",
    ],
    "field_inherited": ["field_introspection", "klass_introspection"],
    "field_introspection": [
        "constantpool_access", "vmstructs_offset_resolution",
    ],
    "field_null_safety": ["field_introspection"],
    "field_object_ref": [
        "wrapper_pattern", "compressed_oops_decode",
    ],
    "field_primitives_get": [
        "field_introspection", "field_proxy_value_t",
    ],
    "field_primitives_set": [
        "field_proxy_set_guards", "field_proxy_value_t",
    ],
    "field_proxy_set_guards": [
        "field_set_size_guard", "field_proxy_value_t",
    ],
    "field_proxy_value_t": ["field_introspection"],
    "field_set_size_guard": ["field_proxy_value_t"],
    "field_static": ["field_introspection"],
    "field_string": [
        "field_object_ref", "read_java_string", "make_java_string",
    ],
    "watch_static_field": ["field_static", "hw_breakpoint_dr7"],

    # --- collection cluster --------------------------------------------
    "collection_hash_tree_map": [
        "collection_map", "collection_iteration_safety",
    ],
    "collection_iteration_safety": [
        "collection_list", "collection_set", "collection_map",
    ],
    "collection_linked_list": [
        "collection_list", "collection_iteration_safety",
    ],
    "collection_list": [
        "collection_type_tags", "array_element_helpers", "wrapper_pattern",
    ],
    "collection_map": ["collection_type_tags", "wrapper_pattern"],
    "collection_set": ["collection_type_tags", "wrapper_pattern"],
    "collection_type_tags": ["klass_introspection"],
    "array_element_helpers": [],

    # --- return_value cluster ------------------------------------------
    "return_caller": ["interpreter_frame_walk"],
    "return_frame_raw_access": ["interpreter_frame_walk"],
    "return_set_arg": ["interpreter_frame_walk"],
    "return_set_primitives": ["interpreter_frame_walk"],
    "return_set_wrapper_null": [
        "interpreter_frame_walk", "wrapper_pattern",
    ],
    "return_stack_trace_depth": [
        "interpreter_frame_walk", "return_caller",
    ],
    "return_value_cancel": ["interpreter_frame_walk"],
    "interpreter_frame_walk": [],

    # --- JNI -----------------------------------------------------------
    "jni_arg_packing": ["signature_parsing"],
    "jni_local_ref_hygiene": [],
    "make_java_array": ["jni_arg_packing"],
    "make_java_string": ["jni_local_ref_hygiene"],
    "read_java_string": ["compressed_oops_decode"],
    "global_ref": ["jni_local_ref_hygiene"],

    # --- enumeration ---------------------------------------------------
    "for_each_instance": [
        "klass_introspection", "iterate_entries_safety",
    ],
    "for_each_loaded_class": [
        "iterate_entries_safety", "klass_introspection",
    ],
    "for_each_thread": ["iterate_entries_safety"],
    "iterate_entries_safety": [],

    # --- OS ------------------------------------------------------------
    "os_allocate_release": [],
    "os_page_size_granularity": [],
    "os_protect": [],
    "os_query_region": [],
    "os_safe_read": ["os_signal_handler"],
    "os_signal_handler": [],
    "hw_breakpoint_dr7": [],

    # --- lifecycle -----------------------------------------------------
    "shutdown_hooks_teardown": ["hook_basic"],
    "on_class_loaded": ["hook_basic"],
    "on_exception": ["hook_basic"],
    "enum_singleton": ["wrapper_pattern", "klass_introspection"],

    # --- infrastructure ------------------------------------------------
    "api_surface_no_jvm": [],
    "wrapper_pattern": [],
    "make_unique": ["wrapper_pattern"],
    "logging_format": [],
    "unified_call_syntax": ["method_overload"],
    "traits_function_traits": [],
    "version_macros": [],
    "platform_capability_macros": [],
    "decode_u5_unsigned5": [],
    "dllmain_bootstrap": [],
}


# ----------------------------------------------------------------------
# Loading / discovery helpers
# ----------------------------------------------------------------------

def list_agent_slugs() -> List[str]:
    """Slugs of every specialist agent that currently has a file in
    .claude/agents/*-specialist.md."""
    if not AGENTS_DIR.is_dir():
        return []
    slugs: List[str] = []
    for p in sorted(AGENTS_DIR.glob("*-specialist.md")):
        slugs.append(p.stem.removesuffix("-specialist"))
    return slugs


def list_manifest_slugs() -> List[str]:
    """Slugs of every manifest currently on disk.

    Filenames whose stem contains a dot (e.g. ``schema.example``) or
    begins with ``_`` are reserved for templates / docs and are
    skipped here so they are not loaded as real manifests.
    """
    out: List[str] = []
    for p in sorted(FEATURES_DIR.glob("*.yaml")):
        stem = p.stem
        if "." in stem or stem.startswith("_"):
            continue
        out.append(stem)
    return out


def manifest_path(slug: str) -> Path:
    return FEATURES_DIR / f"{slug}.yaml"


def load_manifest(slug: str) -> dict:
    path = manifest_path(slug)
    with path.open("r", encoding="utf-8") as f:
        data = yaml.safe_load(f)
    if not isinstance(data, dict):
        raise ValueError(f"{path}: top-level YAML must be a mapping")
    return data


def dump_manifest(slug: str, data: dict) -> None:
    """Serialize a manifest in our canonical YAML style."""
    path = manifest_path(slug)
    text = yaml.safe_dump(
        data,
        sort_keys=False,
        allow_unicode=True,
        default_flow_style=False,
        width=100,
    )
    path.write_text(text, encoding="utf-8", newline="\n")


def find_default_test_modules(slug: str) -> List[str]:
    """Best-effort: collect existing tests whose filename matches the
    slug.  Looks in tests/jvm/modules/<slug>.cpp and tests/test_<slug>.cpp.
    Returns repo-relative POSIX paths."""
    found: List[str] = []
    candidates = [
        JVM_MODULES_DIR / f"{slug}.cpp",
        JVM_MODULES_DIR / f"{slug}_exhaustive.cpp",
        JVM_MODULES_DIR / f"{slug}_shapes.cpp",
        TESTS_DIR / f"test_{slug}.cpp",
        TESTS_DIR / f"test_{slug}_extended.cpp",
        # tail-shaped slugs (e.g. decode_u5_unsigned5 -> decode_u5)
        TESTS_DIR / f"test_{slug.split('_')[0]}.cpp",
    ]
    seen: Set[Path] = set()
    for c in candidates:
        if c.exists() and c not in seen:
            seen.add(c)
            found.append(c.relative_to(REPO_ROOT).as_posix())
    return found


def specialist_relpath(slug: str) -> str:
    p = AGENTS_DIR / f"{slug}-specialist.md"
    return p.relative_to(REPO_ROOT).as_posix()


# ----------------------------------------------------------------------
# Stub builder
# ----------------------------------------------------------------------

def make_stub(slug: str) -> dict:
    """Build a minimal-but-valid manifest dict for a brand new feature."""
    category = SLUG_TO_CATEGORY.get(slug, "infra")
    title = slug.replace("_", " ").title()
    return {
        "schema_version": SCHEMA_VERSION,
        "slug": slug,
        "title": title,
        "category": category,
        "status": "seeded",
        "risk": "medium",
        "specialist": specialist_relpath(slug),
        "description": (
            "TODO: one-paragraph summary of what this feature does and what its "
            "input/output contract is.  Replace this with a real description so a "
            "spawned specialist can decide if the feature is relevant in ~200 tokens."
        ),
        "java_versions": list(DEFAULT_JAVA_VERSIONS),
        "tags": [],
        "hpp_anchors": [],
        "test_modules": find_default_test_modules(slug),
        "depends_on": list(DEPS_SEED.get(slug, [])),
        "related": [],
        "audit_docs": [],
        "known_bugs": [],
        "notes": (
            "Stub manifest — populate hpp_anchors, depends_on, known_bugs as they "
            "become known.  See audit/features/schema.md for the field reference."
        ),
    }


# ----------------------------------------------------------------------
# Loader / cross-validation
# ----------------------------------------------------------------------

def load_all_manifests() -> Dict[str, dict]:
    out: Dict[str, dict] = {}
    for slug in list_manifest_slugs():
        out[slug] = load_manifest(slug)
    return out


def cycle_in_depends_on(manifests: Dict[str, dict]) -> List[List[str]]:
    """Return any dependency cycles as lists of slugs (each list ends
    where the cycle closes)."""
    cycles: List[List[str]] = []
    WHITE, GREY, BLACK = 0, 1, 2
    colour: Dict[str, int] = {s: WHITE for s in manifests}
    path: List[str] = []

    def visit(node: str) -> None:
        colour[node] = GREY
        path.append(node)
        for dep in manifests.get(node, {}).get("depends_on", []) or []:
            if dep not in manifests:
                continue
            if colour[dep] == GREY:
                # found a back-edge; slice path from dep onwards
                try:
                    start = path.index(dep)
                    cycles.append(path[start:] + [dep])
                except ValueError:
                    cycles.append([dep])
            elif colour[dep] == WHITE:
                visit(dep)
        path.pop()
        colour[node] = BLACK

    for slug in manifests:
        if colour[slug] == WHITE:
            visit(slug)
    return cycles


def repo_path_exists(rel: str) -> bool:
    return (REPO_ROOT / rel).exists()


# ----------------------------------------------------------------------
# Small util used by generator + validator
# ----------------------------------------------------------------------

def iter_features_by_category() -> Iterable[Tuple[str, List[str]]]:
    """Yield (category_slug, sorted feature slugs in that category) for
    every category, in CATEGORIES order."""
    manifests = load_all_manifests()
    for cat in CATEGORIES:
        slugs = sorted(s for s, m in manifests.items() if m.get("category") == cat)
        yield cat, slugs


def eprint(*args, **kwargs) -> None:
    print(*args, file=sys.stderr, **kwargs)
