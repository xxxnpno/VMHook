# vmhook feature registry

This folder is the **machine-readable per-feature registry** for the
vmhook library.  It is the **single source of truth** for:

- Which features the library ships, who owns them (the specialist
  agent), and where they live in the single-header `vmhook.hpp`.
- Which test modules cover each feature, and which audit docs and
  known bugs are open against it.
- The **dependency graph** between features — so an agent or a
  human can answer "if I touch X, what else is at risk?" in
  constant time.

The Obsidian vault under [`audit/graph/`](../graph/) is a derived
view, regenerated from these manifests.

## Why this exists

vmhook is a large project: ~17,000 lines of single-header C++,
80+ JVM test modules, 100+ specialist agents.  Without a registry,
every spawned agent re-discovers the same metadata by grepping
the header and the test tree.  A tiny structured manifest per
feature lets an agent (or a CI script, or a docs build) read
~200 tokens of metadata instead of megabytes of source.

This registry is therefore primarily a **token-efficiency lever**.
The Obsidian graph view is a free bonus on top of the same data.

## Layout

```
audit/features/
  README.md              <- this file
  schema.md              <- field-by-field schema reference
  schema.example.yaml    <- annotated example manifest
  registry.py            <- shared Python helpers (categories, deps)
  generate.py            <- CLI: init + vault
  validate.py            <- CLI: CI lint
  <slug>.yaml ...        <- one manifest per feature (source of truth)

audit/graph/             <- Obsidian vault (auto-generated)
  README.md
  INDEX.md               <- flat index, GitHub-renderable
  features/<slug>.md     <- one note per feature
  categories/<cat>.md    <- one note per category
  _data/graph.json       <- machine-readable graph
  _data/graph.mmd        <- full Mermaid source
```

## How to use it

### As a spawned agent / contributor

Before grepping the header for a feature, **read its manifest**:

```bash
cat audit/features/<feature_slug>.yaml
```

It tells you the specialist agent, the test modules, the
direct dependencies, and any known bugs — usually enough to
decide whether you even need to open `vmhook.hpp`.

For the visual view, open `audit/graph/` as an Obsidian vault and
inspect the graph (the `[[depends_on]]` wikilinks render as edges).

### Adding a new feature

1. Add a specialist agent under `.claude/agents/<slug>-specialist.md`.
2. Run `python audit/features/generate.py init` — this writes a
   stub `audit/features/<slug>.yaml` (filling in category, default
   tests, and any seeded deps).
3. Edit the stub to fill in the description, `hpp_anchors`, and
   anything else you know.
4. Run `python audit/features/generate.py vault` to refresh the
   Obsidian vault.
5. Run `python audit/features/validate.py` to confirm the
   registry passes CI.

### Editing an existing feature

Edit `audit/features/<slug>.yaml` directly.  Then:

```bash
python audit/features/generate.py vault    # rebuild audit/graph/
python audit/features/validate.py          # confirm CI-clean
```

The vault is intentionally **derived**: any direct edit there is
overwritten on the next `vault` run.  Treat `audit/graph/` as
build output.

## How CI enforces it

`.github/workflows/registry.yml` runs `validate.py` on every push
touching `audit/features/` or `audit/graph/`.  The validator fails
if any manifest:

- is missing a required field or has an invalid status / risk / category
- points to a non-existent specialist, test module, audit doc, or
  `hpp_anchors[*].file`
- declares a `depends_on` / `related` slug that is not a registered feature
- introduces a cycle in `depends_on`
- describes a feature whose specialist agent does not exist

…or if a specialist agent exists with no manifest.

See `schema.md` for the full field reference.

## How it relates to the rest of `audit/`

- `audit/PERFECTION_PROGRAM.md` — the durable program spine
  ("what are we trying to do, in what order").  This registry
  is not a substitute; it is the metadata index.
- `audit/LIBRARY_BUGS.md` — bug catalogue.  Manifests link in via
  the `known_bugs` field (free-form `ref:` text).
- `audit/AUDIT_FINDINGS.md` — per-file audit consolidation.
  Manifests link in via the `audit_docs` field.
- `audit/findings/`, `audit/npnoqol/`, `audit/patches/` — the
  source-of-truth folders these manifests reference.

The registry intentionally **does not** duplicate prose in those
files.  It indexes them so a reader can find the right doc without
opening every one.
