"""Validate the vmhook feature registry.

CI runs:
    python audit/features/validate.py
and expects exit code 0.  Exit non-zero on:

  1. Manifest missing a required field, or with an invalid value
     (status / risk / category not in the allowed sets;
     schema_version mismatch).
  2. `specialist` path does not exist.
  3. Any `test_modules` / `audit_docs` path does not exist.
  4. A `depends_on` or `related` slug does not resolve to another
     manifest.
  5. A dependency cycle exists in `depends_on`.
  6. A specialist agent file exists with no manifest (run `init`).

Local invocation:
    python audit/features/validate.py            # full check
    python audit/features/validate.py --quiet    # only print errors
"""

from __future__ import annotations

import argparse
import sys
from typing import Dict, List

from registry import (
    ALLOWED_RISK,
    ALLOWED_STATUS,
    AGENTS_DIR,
    CATEGORIES,
    FEATURES_DIR,
    REQUIRED_FIELDS,
    REPO_ROOT,
    SCHEMA_VERSION,
    cycle_in_depends_on,
    eprint,
    list_agent_slugs,
    list_manifest_slugs,
    load_all_manifests,
    repo_path_exists,
)


def _validate_one(slug: str, m: dict, all_slugs: set) -> List[str]:
    errs: List[str] = []
    where = f"audit/features/{slug}.yaml"

    # 1. required fields
    for f in REQUIRED_FIELDS:
        if f not in m:
            errs.append(f"{where}: missing required field `{f}`")

    # 2. value validity
    if m.get("schema_version") != SCHEMA_VERSION:
        errs.append(
            f"{where}: schema_version is {m.get('schema_version')!r}, "
            f"expected {SCHEMA_VERSION}"
        )
    if m.get("slug") != slug:
        errs.append(
            f"{where}: slug field is {m.get('slug')!r}, "
            f"expected {slug!r} (must match filename)"
        )
    status = m.get("status")
    if status not in ALLOWED_STATUS:
        errs.append(
            f"{where}: status `{status}` not in {sorted(ALLOWED_STATUS)}"
        )
    risk = m.get("risk")
    if risk not in ALLOWED_RISK:
        errs.append(
            f"{where}: risk `{risk}` not in {sorted(ALLOWED_RISK)}"
        )
    cat = m.get("category")
    if cat not in CATEGORIES:
        errs.append(
            f"{where}: category `{cat}` not in {sorted(CATEGORIES)}"
        )

    # 3. specialist exists
    spec = m.get("specialist")
    if isinstance(spec, str) and spec:
        if not repo_path_exists(spec):
            errs.append(f"{where}: specialist file missing: `{spec}`")
    else:
        errs.append(f"{where}: specialist must be a non-empty string")

    # 4. test_modules / audit_docs paths exist
    for field in ("test_modules", "audit_docs"):
        v = m.get(field) or []
        if not isinstance(v, list):
            errs.append(f"{where}: `{field}` must be a list")
            continue
        for path in v:
            if not isinstance(path, str):
                errs.append(f"{where}: `{field}` entries must be strings")
                continue
            if not repo_path_exists(path):
                errs.append(f"{where}: `{field}` entry missing: `{path}`")

    # 5. depends_on / related point to known slugs
    for field in ("depends_on", "related"):
        v = m.get(field) or []
        if not isinstance(v, list):
            errs.append(f"{where}: `{field}` must be a list")
            continue
        for ref in v:
            if not isinstance(ref, str):
                errs.append(f"{where}: `{field}` entries must be strings")
                continue
            if ref == slug:
                errs.append(f"{where}: `{field}` references self (`{ref}`)")
            elif ref not in all_slugs:
                errs.append(
                    f"{where}: `{field}` -> `{ref}` is not a registered slug"
                )

    # 6. hpp_anchors shape
    anchors = m.get("hpp_anchors") or []
    if not isinstance(anchors, list):
        errs.append(f"{where}: `hpp_anchors` must be a list")
    else:
        for i, a in enumerate(anchors):
            if not isinstance(a, dict):
                errs.append(f"{where}: hpp_anchors[{i}] must be a mapping")
                continue
            for key in ("symbol", "file"):
                if key not in a or not isinstance(a[key], str) or not a[key]:
                    errs.append(
                        f"{where}: hpp_anchors[{i}] missing/empty `{key}`"
                    )
            if "lines" in a:
                ln = a["lines"]
                if not (isinstance(ln, list) and len(ln) == 2
                        and all(isinstance(x, int) for x in ln)):
                    errs.append(
                        f"{where}: hpp_anchors[{i}].lines must be [start, end] ints"
                    )
            if "file" in a and isinstance(a["file"], str) and a["file"]:
                if not repo_path_exists(a["file"]):
                    errs.append(
                        f"{where}: hpp_anchors[{i}].file missing: `{a['file']}`"
                    )

    # 7. known_bugs shape
    bugs = m.get("known_bugs") or []
    if not isinstance(bugs, list):
        errs.append(f"{where}: `known_bugs` must be a list")
    else:
        for i, b in enumerate(bugs):
            if not isinstance(b, dict):
                continue  # tolerate plain strings as informal notes
            sev = b.get("severity")
            if sev not in ALLOWED_RISK:
                errs.append(
                    f"{where}: known_bugs[{i}].severity `{sev}` not in "
                    f"{sorted(ALLOWED_RISK)}"
                )
            if not isinstance(b.get("note", ""), str):
                errs.append(f"{where}: known_bugs[{i}].note must be a string")

    return errs


def main(argv: List[str]) -> int:
    p = argparse.ArgumentParser(prog="validate.py")
    p.add_argument("--quiet", action="store_true",
                   help="suppress the per-feature OK lines")
    ns = p.parse_args(argv)

    manifest_slugs = list_manifest_slugs()
    if not manifest_slugs:
        eprint("[validate] no manifests on disk")
        return 1

    manifests: Dict[str, dict] = load_all_manifests()
    all_slugs = set(manifests)
    total_errs: List[str] = []

    # 8. every agent has a manifest
    for slug in list_agent_slugs():
        if slug not in all_slugs:
            total_errs.append(
                f"specialist agent `{slug}` has no manifest "
                f"(create audit/features/{slug}.yaml or run "
                f"`python audit/features/generate.py init`)"
            )

    # per-manifest validation
    bad_slugs: set = set()
    for slug in sorted(all_slugs):
        errs = _validate_one(slug, manifests[slug], all_slugs)
        if errs:
            bad_slugs.add(slug)
            total_errs.extend(errs)
        elif not ns.quiet:
            print(f"  ok  {slug}")

    # 9. cycles
    cycles = cycle_in_depends_on(manifests)
    for c in cycles:
        total_errs.append("dependency cycle: " + " -> ".join(c))

    print()
    print(f"[validate] {len(manifests)} manifests, {len(total_errs)} error(s)")
    if total_errs:
        print()
        for e in total_errs:
            eprint(f"  E  {e}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
