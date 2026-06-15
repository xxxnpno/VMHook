# Feature manifest schema (v1)

Every file in `audit/features/<slug>.yaml` follows this shape.
`validate.py` enforces it in CI.  See `schema.example.yaml` for a
full annotated example.

| field           | type                | required | description |
|-----------------|---------------------|----------|-------------|
| `schema_version`| integer (== `1`)    | yes      | bumped when the schema changes incompatibly. |
| `slug`          | string              | yes      | unique identifier; must match the filename and the agent slug (`[a-z0-9_]+`). |
| `title`         | string              | yes      | human-friendly title shown in the vault. |
| `category`      | string              | yes      | one of the categories in `registry.CATEGORIES`. |
| `status`        | string              | yes      | one of `queued`, `seeded`, `in_progress`, `audited`, `perfected`. |
| `risk`          | string              | yes      | one of `low`, `medium`, `high`, `critical`. |
| `specialist`    | repo-relative path  | yes      | the `.claude/agents/*.md` file that owns this feature. |
| `description`   | string (multiline)  | yes      | one paragraph; what the feature does, contract, blast radius. |
| `java_versions` | list of ints        | yes      | which Java majors this feature claims to support. |
| `tags`          | list of strings     | yes      | freeform tags (e.g. `safety`, `performance`, `x86_64`). |
| `hpp_anchors`   | list of objects     | yes      | locations in `vmhook.hpp` — see below. |
| `test_modules`  | list of paths       | yes      | repo-relative test files covering this feature. |
| `depends_on`    | list of slugs       | yes      | features this one **requires** to function. |
| `related`       | list of slugs       | yes      | features that share a code path / fixture / concept but are not strict deps. |
| `audit_docs`    | list of paths       | yes      | repo-relative `.md` files under `audit/` that discuss this feature. |
| `known_bugs`    | list of objects     | yes      | open bugs — see below. |
| `notes`         | string (multiline)  | yes      | freeform; anything that does not fit above. |

A required list may be empty (`[]`), and a required string may be
empty (`""`) but it must still be present.

## `hpp_anchors[*]`

Pin where the implementation actually lives.  A spawned specialist
should be able to read these and jump straight to the right window
of the 17k-line header.

```yaml
hpp_anchors:
  - symbol: vmhook::hook<T>          # required
    file: vmhook/ext/vmhook/vmhook.hpp  # required, must exist
    lines: [7850, 8125]              # optional [start, end] (ints)
    note: "install routine"          # optional
```

## `known_bugs[*]`

Each entry can be a free-form string (informal) or an object:

```yaml
known_bugs:
  - severity: high            # one of low/medium/high/critical
    note: "half-installed method permanently poisons re-install"
    ref: "audit/LIBRARY_BUGS.md#hook_basic-1"   # optional anchor
```

## Status semantics

| status         | meaning |
|----------------|---------|
| `queued`       | exists in plan, no work yet. |
| `seeded`       | stub manifest written; description / anchors are placeholders. |
| `in_progress`  | specialist actively iterating; tests / fixes landing. |
| `audited`      | per-file audit consolidated; no high-severity bugs remain. |
| `perfected`    | exhaustive tests across all supported Java versions; zero known bugs. |

## Risk semantics

The **blast radius** of breaking this feature, not the difficulty.

| risk        | meaning |
|-------------|---------|
| `low`       | localised; failure surfaces in a single contained call. |
| `medium`    | typical feature-level breakage; the immediate user notices. |
| `high`      | corrupts neighbouring state, leaks references, or breaks JVM stability for related code paths. |
| `critical`  | crashes the JVM, corrupts memory across the process, or breaks the hooking machinery globally. |

## Categories

See `registry.py::CATEGORIES`.  Each category has a fixed display
title and an ordered slug list.  A feature must belong to exactly one.
