"""Generate the vmhook feature registry artefacts.

Commands
--------
    python generate.py init        # create stub manifests for any
                                   # specialist agent that does not
                                   # yet have a manifest.  Idempotent.

    python generate.py vault       # rebuild audit/graph/ (Obsidian
                                   # vault + graph.json + Mermaid)
                                   # from the manifests on disk.

    python generate.py all         # init + vault   (default)

    python generate.py check       # delegate to validate.py

The output under audit/graph/ is FULLY DERIVED from the manifests
under audit/features/.  Hand-editing the vault is pointless; edit
the manifests and re-run `python generate.py vault`.
"""

from __future__ import annotations

import argparse
import datetime as _dt
import json
import os
import sys
from pathlib import Path
from typing import Dict, Iterable, List, Set, Tuple

from registry import (
    AUDIT_DIR,
    CATEGORIES,
    DEPS_SEED,
    FEATURES_DIR,
    GRAPH_DIR,
    REPO_ROOT,
    SLUG_TO_CATEGORY,
    category_title,
    dump_manifest,
    eprint,
    list_agent_slugs,
    list_manifest_slugs,
    load_all_manifests,
    make_stub,
    manifest_path,
)


# ----------------------------------------------------------------------
# auto-related: fill empty `related` fields from same-category siblings
# that share at least one `depends_on` slug.  Hand-edited / non-empty
# `related` lists are NEVER overwritten — only `[]` is replaced.
# ----------------------------------------------------------------------

# Cap so the inferred list cannot explode for dense clusters.
AUTO_RELATED_MAX = 6


def cmd_auto_related() -> int:
    manifests = load_all_manifests()
    if not manifests:
        eprint("[auto-related] no manifests on disk — run `init` first")
        return 1

    # group slugs by category
    by_cat: Dict[str, List[str]] = {}
    for slug, m in manifests.items():
        by_cat.setdefault(m.get("category", "infra"), []).append(slug)

    updated: List[Tuple[str, List[str]]] = []
    for slug, m in manifests.items():
        # Only fill when explicitly empty.  Preserves any hand-curated value.
        if m.get("related"):
            continue
        my_deps = set(m.get("depends_on") or [])
        if not my_deps:
            continue
        cat = m.get("category", "infra")
        sibs = [s for s in by_cat.get(cat, []) if s != slug]
        scored: List[Tuple[int, str]] = []
        for sib in sibs:
            sib_deps = set(manifests[sib].get("depends_on") or [])
            shared = my_deps & sib_deps
            if shared:
                scored.append((len(shared), sib))
        # most overlap first, alpha as tie-breaker; cap the list length.
        scored.sort(key=lambda x: (-x[0], x[1]))
        new_related = [s for _, s in scored[:AUTO_RELATED_MAX]]
        if new_related:
            m["related"] = new_related
            dump_manifest(slug, m)
            updated.append((slug, new_related))

    if not updated:
        print("[auto-related] no empty `related` fields needed seeding")
        return 0
    print(f"[auto-related] seeded `related` on {len(updated)} manifest(s):")
    for slug, rel in updated:
        print(f"  + {slug:42} -> {', '.join(rel)}")
    return 0


# ----------------------------------------------------------------------
# init: stub any missing manifests
# ----------------------------------------------------------------------

def cmd_init() -> int:
    agents = list_agent_slugs()
    existing = set(list_manifest_slugs())
    created: List[str] = []
    for slug in agents:
        if slug in existing:
            continue
        data = make_stub(slug)
        dump_manifest(slug, data)
        created.append(slug)
    if created:
        print(f"[init] created {len(created)} stub manifests:")
        for s in created:
            print(f"  + {s}")
    else:
        print("[init] all specialist agents already have manifests")
    # Report unmapped: slugs with no category.
    unmapped = [s for s in agents if s not in SLUG_TO_CATEGORY]
    if unmapped:
        eprint(f"[init] WARNING: {len(unmapped)} slugs have no category mapping:")
        for s in unmapped:
            eprint(f"  ? {s}")
    return 0


# ----------------------------------------------------------------------
# vault: regenerate the Obsidian vault
# ----------------------------------------------------------------------

VAULT_README = """\
# vmhook feature graph (Obsidian vault)

This folder is **auto-generated** from the YAML manifests under
`audit/features/`.  Editing files here directly does nothing — your
changes will be overwritten the next time `python audit/features/generate.py vault`
runs.

## What is in this vault?

- `features/<slug>.md` — one note per vmhook feature, with
  frontmatter (status / risk / category), description, dependency
  links (`[[depends_on]]`), back-links (who depends on me), the
  matching specialist agent file, test modules, audit docs, and
  known bugs.
- `categories/<cat>.md` — one note per category, listing every
  feature in it plus a per-category Mermaid graph.
- `INDEX.md` — flat index, grouped by category, suitable for
  reading on GitHub (renders the Mermaid blocks).
- `_data/graph.json` — the same graph in machine-readable form,
  for tooling that wants to query it without parsing YAML.
- `_data/graph.mmd` — the full Mermaid source, useful for
  embedding in slides / docs.

## Opening in Obsidian

Open `audit/graph/` as an Obsidian vault.  The graph view (sidebar
→ Open graph view) renders the dependency relationships natively;
filter by `tag:#status/<state>` or `path:features/` to scope the
view.  Wikilinks resolve across the vault.

## Where the source of truth lives

The **manifests** under `audit/features/<slug>.yaml` are the source
of truth.  See `audit/features/schema.md` for the schema reference
and `audit/features/README.md` for how the registry is used.
"""


def _wikilink(slug: str, target_folder: str = "features") -> str:
    """Build an Obsidian wikilink that resolves to `target_folder/<slug>`."""
    return f"[[{target_folder}/{slug}|{slug}]]"


def _frontmatter(d: Dict) -> str:
    """Render a small block of YAML-style frontmatter."""
    lines = ["---"]
    for k, v in d.items():
        if isinstance(v, list):
            inner = ", ".join(str(x) for x in v)
            lines.append(f"{k}: [{inner}]")
        else:
            lines.append(f"{k}: {v}")
    lines.append("---")
    return "\n".join(lines)


def _back_links(manifests: Dict[str, dict]) -> Dict[str, Set[str]]:
    """slug -> set of slugs that depend on it (incoming edges)."""
    rev: Dict[str, Set[str]] = {s: set() for s in manifests}
    for s, m in manifests.items():
        for dep in m.get("depends_on") or []:
            if dep in rev:
                rev[dep].add(s)
    return rev


def _related_links(manifests: Dict[str, dict]) -> Dict[str, Set[str]]:
    """slug -> set of slugs that list it under `related` (incoming, symmetric)."""
    rev: Dict[str, Set[str]] = {s: set() for s in manifests}
    for s, m in manifests.items():
        for r in m.get("related") or []:
            if r in rev:
                rev[r].add(s)
    return rev


def _render_feature_note(
    slug: str,
    m: dict,
    back_deps: Set[str],
    back_related: Set[str],
) -> str:
    cat = m.get("category", "infra")
    tags = [
        f"status/{m.get('status', 'seeded')}",
        f"risk/{m.get('risk', 'medium')}",
        f"category/{cat}",
    ]
    for t in (m.get("tags") or []):
        tags.append(f"tag/{t}")

    fm = {
        "slug": slug,
        "title": m.get("title", slug),
        "category": cat,
        "status": m.get("status", "seeded"),
        "risk": m.get("risk", "medium"),
        "java_versions": m.get("java_versions", []),
        "tags": tags,
    }

    parts: List[str] = []
    parts.append(_frontmatter(fm))
    parts.append("")
    parts.append(f"# {m.get('title', slug)}")
    parts.append("")
    parts.append(
        f"> **Category:** [[categories/{cat}|{category_title(cat)}]]  ·  "
        f"**Status:** `{m.get('status', 'seeded')}`  ·  "
        f"**Risk:** `{m.get('risk', 'medium')}`  ·  "
        f"**Specialist:** `{m.get('specialist', '')}`"
    )
    parts.append("")

    desc = (m.get("description") or "").strip()
    if desc:
        parts.append("## Description")
        parts.append("")
        parts.append(desc)
        parts.append("")

    # ----- relationships -----
    deps = m.get("depends_on") or []
    if deps:
        parts.append("## Depends on")
        parts.append("")
        for d in deps:
            parts.append(f"- {_wikilink(d)}")
        parts.append("")

    related = m.get("related") or []
    if related:
        parts.append("## Related")
        parts.append("")
        for r in related:
            parts.append(f"- {_wikilink(r)}")
        parts.append("")

    if back_deps:
        parts.append("## Depended on by")
        parts.append("")
        for d in sorted(back_deps):
            parts.append(f"- {_wikilink(d)}")
        parts.append("")

    if back_related and not deps and not related:
        # only show "Related from" when there's nothing else to anchor it
        parts.append("## Referenced from")
        parts.append("")
        for d in sorted(back_related):
            parts.append(f"- {_wikilink(d)}")
        parts.append("")

    # ----- implementation anchors -----
    anchors = m.get("hpp_anchors") or []
    if anchors:
        parts.append("## Implementation anchors")
        parts.append("")
        for a in anchors:
            sym = a.get("symbol", "(unnamed)")
            file = a.get("file", "")
            lines = a.get("lines")
            note = a.get("note", "")
            loc = file
            if lines and isinstance(lines, list) and len(lines) == 2:
                loc = f"{file}:{lines[0]}-{lines[1]}"
            elif lines:
                loc = f"{file}:{lines}"
            line = f"- `{sym}` — `{loc}`"
            if note:
                line += f" — {note}"
            parts.append(line)
        parts.append("")

    # ----- tests -----
    tests = m.get("test_modules") or []
    if tests:
        parts.append("## Tests")
        parts.append("")
        for t in tests:
            parts.append(f"- `{t}`")
        parts.append("")

    # ----- audit -----
    audits = m.get("audit_docs") or []
    if audits:
        parts.append("## Audit docs")
        parts.append("")
        for a in audits:
            parts.append(f"- `{a}`")
        parts.append("")

    # ----- known bugs -----
    bugs = m.get("known_bugs") or []
    if bugs:
        parts.append("## Known bugs")
        parts.append("")
        for b in bugs:
            if not isinstance(b, dict):
                parts.append(f"- {b}")
                continue
            sev = b.get("severity", "?")
            note = b.get("note", "")
            ref = b.get("ref", "")
            line = f"- **[{sev}]** {note}"
            if ref:
                line += f"  (`{ref}`)"
            parts.append(line)
        parts.append("")

    notes = (m.get("notes") or "").strip()
    if notes:
        parts.append("## Notes")
        parts.append("")
        parts.append(notes)
        parts.append("")

    return "\n".join(parts).rstrip() + "\n"


def _render_category_note(
    cat: str,
    manifests: Dict[str, dict],
) -> str:
    feats = sorted(s for s, m in manifests.items() if m.get("category") == cat)

    fm = {
        "category": cat,
        "title": category_title(cat),
        "feature_count": len(feats),
        "tags": [f"category/{cat}"],
    }

    parts: List[str] = []
    parts.append(_frontmatter(fm))
    parts.append("")
    parts.append(f"# {category_title(cat)}")
    parts.append("")
    parts.append(f"**{len(feats)} feature(s) in this category.**")
    parts.append("")
    parts.append("## Features")
    parts.append("")
    for s in feats:
        m = manifests[s]
        risk = m.get("risk", "medium")
        status = m.get("status", "seeded")
        parts.append(
            f"- {_wikilink(s)} — `{status}` / `{risk}` — "
            f"{(m.get('title') or s)}"
        )
    parts.append("")

    # Mermaid graph of intra-category edges + cross-cat targets (as
    # boxed nodes outside the subgraph).
    mmd = _category_mermaid(cat, manifests)
    parts.append("## Dependency graph")
    parts.append("")
    parts.append("```mermaid")
    parts.append(mmd)
    parts.append("```")
    parts.append("")
    return "\n".join(parts).rstrip() + "\n"


def _category_mermaid(cat: str, manifests: Dict[str, dict]) -> str:
    feats = [s for s, m in manifests.items() if m.get("category") == cat]
    feats_sorted = sorted(feats)
    lines: List[str] = ["flowchart LR"]
    # Intra-cat subgraph
    safe_cat_id = cat.replace("-", "_")
    lines.append(f'  subgraph {safe_cat_id}["{category_title(cat)}"]')
    for s in feats_sorted:
        lines.append(f"    {s}([{s}])")
    lines.append("  end")

    # External (cross-category) targets
    externals: Set[str] = set()
    for s in feats_sorted:
        for dep in manifests[s].get("depends_on") or []:
            if dep not in manifests:
                continue
            other_cat = manifests[dep].get("category")
            if other_cat != cat:
                externals.add(dep)
    if externals:
        lines.append('  subgraph external["(external deps)"]')
        for e in sorted(externals):
            lines.append(f"    {e}[/{e}/]")
        lines.append("  end")

    # Edges
    for s in feats_sorted:
        for dep in manifests[s].get("depends_on") or []:
            if dep in manifests:
                lines.append(f"  {s} --> {dep}")
    return "\n".join(lines)


def _render_full_mermaid(manifests: Dict[str, dict]) -> str:
    """Full graph with one subgraph per category — for `_data/graph.mmd`."""
    by_cat: Dict[str, List[str]] = {cat: [] for cat in CATEGORIES}
    for s, m in manifests.items():
        cat = m.get("category", "infra")
        by_cat.setdefault(cat, []).append(s)
    lines: List[str] = ["flowchart LR"]
    for cat, slugs in by_cat.items():
        if not slugs:
            continue
        safe_cat_id = cat.replace("-", "_")
        lines.append(f'  subgraph {safe_cat_id}["{category_title(cat)}"]')
        for s in sorted(slugs):
            lines.append(f"    {s}([{s}])")
        lines.append("  end")
    for s, m in manifests.items():
        for dep in m.get("depends_on") or []:
            if dep in manifests:
                lines.append(f"  {s} --> {dep}")
    return "\n".join(lines)


def _render_graph_json(manifests: Dict[str, dict]) -> str:
    features = []
    edges = []
    for slug in sorted(manifests):
        m = manifests[slug]
        features.append({
            "slug": slug,
            "title": m.get("title", slug),
            "category": m.get("category"),
            "status": m.get("status"),
            "risk": m.get("risk"),
            "specialist": m.get("specialist"),
            "depends_on": list(m.get("depends_on") or []),
            "related": list(m.get("related") or []),
            "tags": list(m.get("tags") or []),
            "test_modules": list(m.get("test_modules") or []),
            "audit_docs": list(m.get("audit_docs") or []),
            "java_versions": list(m.get("java_versions") or []),
        })
        for dep in m.get("depends_on") or []:
            if dep in manifests:
                edges.append({"from": slug, "to": dep, "kind": "depends_on"})
        for rel in m.get("related") or []:
            if rel in manifests:
                edges.append({"from": slug, "to": rel, "kind": "related"})
    cats = []
    for cat_slug, (title, _slugs) in CATEGORIES.items():
        feats = sorted(s for s, m in manifests.items() if m.get("category") == cat_slug)
        cats.append({
            "slug": cat_slug,
            "title": title,
            "features": feats,
        })
    return json.dumps(
        {
            "schema_version": 1,
            # Deterministic per SOURCE_DATE_EPOCH (reproducible-builds standard): a
            # wall-clock now() here made graph.json differ on every regen, so the
            # registry "regenerate-check" (regen + git diff) failed on essentially every
            # commit (only this line moved; counts/content matched). Default epoch 0 when
            # unset keeps the regen reproducible so the check is stable.
            "generated_at": _dt.datetime.fromtimestamp(
                int(os.environ.get("SOURCE_DATE_EPOCH", "0")), _dt.timezone.utc
            ).strftime("%Y-%m-%dT%H:%M:%SZ"),
            "feature_count": len(features),
            "edge_count": len(edges),
            "categories": cats,
            "features": features,
            "edges": edges,
        },
        indent=2,
        sort_keys=False,
    )


def _render_index(manifests: Dict[str, dict]) -> str:
    parts: List[str] = []
    parts.append("# vmhook feature index (auto-generated)")
    parts.append("")
    parts.append(
        "This index is rebuilt from `audit/features/*.yaml`.  Edit the "
        "manifests, not this file."
    )
    parts.append("")
    parts.append(f"- **Features:** {len(manifests)}")
    parts.append(f"- **Categories:** {len(CATEGORIES)}")
    edge_count = sum(
        1 for m in manifests.values() for d in (m.get("depends_on") or []) if d in manifests
    )
    parts.append(f"- **`depends_on` edges:** {edge_count}")
    parts.append("")
    # status histogram
    hist: Dict[str, int] = {}
    for m in manifests.values():
        hist[m.get("status", "seeded")] = hist.get(m.get("status", "seeded"), 0) + 1
    parts.append("## Status")
    parts.append("")
    for k in ("queued", "seeded", "in_progress", "audited", "perfected"):
        parts.append(f"- `{k}`: {hist.get(k, 0)}")
    parts.append("")

    # by category
    parts.append("## By category")
    parts.append("")
    for cat in CATEGORIES:
        feats = sorted(s for s, m in manifests.items() if m.get("category") == cat)
        if not feats:
            continue
        parts.append(f"### [[categories/{cat}|{category_title(cat)}]] · {len(feats)} feature(s)")
        parts.append("")
        for s in feats:
            m = manifests[s]
            parts.append(
                f"- {_wikilink(s)} — `{m.get('status', 'seeded')}` / "
                f"`{m.get('risk', 'medium')}` — {m.get('title') or s}"
            )
        parts.append("")
    return "\n".join(parts).rstrip() + "\n"


def _ensure_dirs() -> None:
    (GRAPH_DIR / "features").mkdir(parents=True, exist_ok=True)
    (GRAPH_DIR / "categories").mkdir(parents=True, exist_ok=True)
    (GRAPH_DIR / "_data").mkdir(parents=True, exist_ok=True)


def _wipe_dir(p: Path, suffix: str) -> None:
    """Remove every file with the given suffix from a directory.
    Used so deleting a manifest also drops its derived note."""
    if not p.is_dir():
        return
    for f in p.iterdir():
        if f.is_file() and f.name.endswith(suffix):
            f.unlink()


def cmd_vault() -> int:
    manifests = load_all_manifests()
    if not manifests:
        eprint("[vault] no manifests on disk — run `init` first")
        return 1

    _ensure_dirs()
    # Wipe stale outputs so deleted manifests drop their notes.
    _wipe_dir(GRAPH_DIR / "features", ".md")
    _wipe_dir(GRAPH_DIR / "categories", ".md")

    back_deps = _back_links(manifests)
    back_related = _related_links(manifests)

    # feature notes
    for slug in sorted(manifests):
        out = GRAPH_DIR / "features" / f"{slug}.md"
        out.write_text(
            _render_feature_note(
                slug, manifests[slug],
                back_deps.get(slug, set()),
                back_related.get(slug, set()),
            ),
            encoding="utf-8", newline="\n",
        )

    # category notes
    for cat in CATEGORIES:
        out = GRAPH_DIR / "categories" / f"{cat}.md"
        out.write_text(
            _render_category_note(cat, manifests),
            encoding="utf-8", newline="\n",
        )

    # index, vault README, full graph artefacts
    (GRAPH_DIR / "INDEX.md").write_text(
        _render_index(manifests), encoding="utf-8", newline="\n"
    )
    (GRAPH_DIR / "README.md").write_text(
        VAULT_README, encoding="utf-8", newline="\n"
    )
    (GRAPH_DIR / "_data" / "graph.json").write_text(
        _render_graph_json(manifests), encoding="utf-8", newline="\n"
    )
    (GRAPH_DIR / "_data" / "graph.mmd").write_text(
        _render_full_mermaid(manifests) + "\n",
        encoding="utf-8", newline="\n",
    )

    feat_count = len(manifests)
    edge_count = sum(
        1 for m in manifests.values() for d in (m.get("depends_on") or []) if d in manifests
    )
    print(f"[vault] regenerated audit/graph/ — {feat_count} features, {edge_count} edges")
    return 0


# ----------------------------------------------------------------------
# main
# ----------------------------------------------------------------------

def main(argv: List[str]) -> int:
    p = argparse.ArgumentParser(prog="generate.py")
    p.add_argument(
        "command",
        nargs="?",
        default="all",
        choices=("init", "auto-related", "vault", "all", "check"),
        help="default: all (init + auto-related + vault)",
    )
    ns = p.parse_args(argv)
    if ns.command == "init":
        return cmd_init()
    if ns.command == "auto-related":
        return cmd_auto_related()
    if ns.command == "vault":
        return cmd_vault()
    if ns.command == "all":
        rc = cmd_init()
        if rc != 0:
            return rc
        rc = cmd_auto_related()
        if rc != 0:
            return rc
        return cmd_vault()
    if ns.command == "check":
        # local import so this module is usable without validate.py
        import validate
        return validate.main([])
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
