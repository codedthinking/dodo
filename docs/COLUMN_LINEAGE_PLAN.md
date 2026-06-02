# Plan: Column Comment Propagation Along CTE Chain

## Context

When `use` loads a .dta file, dodo reads variable labels into `state.variable_labels`. These labels should be propagated as `COMMENT ON COLUMN` on materialized tables so downstream consumers (DTA writer, UI, etc.) can see them. After materialization checkpoints, the node table has no comments because `CREATE TABLE AS SELECT` doesn't carry comments.

## Design

`variable_labels` (existing `unordered_map<string, string>`) already maps current column name → label text. It becomes the single source of truth for column comments. No separate lineage map needed — `variable_labels` IS the lineage.

## Update Rules by Command

| Command | Rule |
|---------|------|
| `use` (with .dta) | Populate from .dta header (already implemented) |
| `rename old new` | Move entry: `labels[new] = labels[old]; erase(old)` |
| `keep vars` (column select) | Remove entries not in kept set |
| `keep if cond` | No change (row filter) |
| `drop vars` | Remove entries for dropped vars |
| `drop if cond` | No change (row filter) |
| `generate` / `egen` | No entry (new column, no label) |
| `replace` | No change (column identity preserved) |
| `label variable` | Update entry directly (already implemented) |
| `order` / `mvencode` / `compress` | No change |
| `collapse` | Clear all labels (new column set) |
| `reshape` | Clear all labels (too complex to track) |
| `merge` | Keep existing labels, no entries for merged-in columns |
| `append` / `duplicates drop` / `expand` | No change |

## Comment Emission

In `MaybeCheckpoint()`, after `CREATE TABLE IF NOT EXISTS`, emit for each label:
```cpp
for (auto &[col, label] : variable_labels) {
    pending_sql.push_back("COMMENT ON COLUMN " + node_name + "."
        + col + " IS '" + escape(label) + "'");
}
```

Same for root node table during `use`.

Comments are emitted optimistically — if a column was dropped by `keep`/`drop` but the label wasn't cleaned up, the COMMENT will fail. This is why the update rules above matter.

## Files to Modify

### `src/core/dodo_core.hpp`
- `MaybeCheckpoint()`: emit COMMENT ON after creating node table

### `src/core/dodo_core.cpp`
- `use`: emit COMMENT ON for root node table (via `pending_sql`)
- `rename`: move label entry
- `keep` (column select): filter labels
- `drop` (column variant): remove labels
- `collapse`: clear labels
- `reshape`: clear labels

## Verification

1. `use "auto.dta"; tabulate foreign` — root + checkpoint nodes have comments
2. `rename price cost; count` — comment on `cost` has `price`'s original label
3. `keep make price; count` — only `make` and `price` have comments
4. `collapse (mean) price` — no comments
5. `label variable price "New label"; count` — checkpoint has "New label"
6. Verify: `SELECT column_name, comment FROM duckdb_columns() WHERE table_schema = 'dodo'`
