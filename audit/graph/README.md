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
