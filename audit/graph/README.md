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
- `.obsidian/` — a curated, committed Obsidian config (graph view
  enabled, nodes coloured by status) so the vault opens ready to use.
  It is regenerated too; per-user pane state (`workspace.json`) is
  git-ignored and never committed.

## Opening in Obsidian

Open the `audit/graph/` folder **itself** as an Obsidian vault — do
*not* open a subfolder such as `categories/`, which would create a
stray nested vault.  It is pre-configured: the graph view (sidebar →
Open graph view) renders the dependency relationships natively, with
nodes coloured by status (`in_progress` amber / `seeded` slate) and
high-risk features highlighted red.  Filter by `tag:#status/<state>`
or `path:features/` to scope the view.  Wikilinks resolve across the
vault.

## Where the source of truth lives

The **manifests** under `audit/features/<slug>.yaml` are the source
of truth.  See `audit/features/schema.md` for the schema reference
and `audit/features/README.md` for how the registry is used.
