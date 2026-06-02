# Plan: Column Comment Propagation Along CTE Chain

## Context

When `use` loads a .dta file, dodo reads variable labels into `state.variable_labels`. These labels should be propagated as `COMMENT ON COLUMN` on materialized tables so downstream consumers (DTA writer, UI, etc.) can see them. After materialization checkpoints, the node table has no comments because `CREATE TABLE AS SELECT` doesn't carry comments. We need to track which output columns descend from which original columns, and re-apply comments on each materialized table.

## Data Structure

Add to `DodoState`:
```cpp
// Maps current column name → root column name (for comment propagation)
std::unordered_map<std::string, std::string> column_lineage;
```

## Lineage Rules by Command

| Command | Rule |
|---------|------|
| `use` (with .dta) | Initialize: `column_lineage[col] = col` for each column with a label |
| `rename old new` | `lineage[new] = lineage[old]; erase(old)` |
| `keep vars` (column select) | Remove entries not in kept set |
| `keep if cond` | No change (row filter) |
| `drop vars` | Remove entries for dropped vars |
| `drop if cond` | No change (row filter) |
| `generate` | No entry (new column) |
| `replace` | No change (column identity preserved) |
| `order` | No change |
| `mvencode` | No change |
| `egen` | No entry (new column) |
| `collapse` | Clear all lineage (new column set) |
| `reshape` | Clear all lineage (too complex) |
| `merge` | Keep existing lineage, no entries for merged-in columns |
| `append` | No change |
| `duplicates drop` | No change |
| `expand` | No change |
| `compress` | No change |

## Comment Emission

In `MaybeCheckpoint()`, after creating the node table, emit `COMMENT ON COLUMN` for each column in `column_lineage` that has a label in `variable_labels`:

```cpp
for (auto &[current_name, root_name] : column_lineage) {
    auto it = variable_labels.find(root_name);
    if (it != variable_labels.end()) {
        pending_sql.push_back("COMMENT ON COLUMN " + node_name + "." +
                              current_name + " IS '" + escape(it->second) + "'");
    }
}
```

Also emit comments on the root node table during `use`.

## Files to Modify

### `src/core/dodo_core.hpp`
- Add `column_lineage` field to `DodoState`
- Clear in `Clear()`

### `src/core/dodo_core.cpp`
- `use`: initialize `column_lineage` from .dta column list (all columns, not just labeled ones — so `keep` can remove correctly)
- `rename`: update lineage mapping
- `keep` (column select variant): filter lineage to kept columns
- `drop` (column variant): remove from lineage
- `collapse`: clear lineage
- `reshape`: clear lineage
- `MaybeCheckpoint()` in hpp: emit COMMENT ON for tracked columns
- `use` materialization: emit COMMENT ON for root node table

## Verification

1. `use "test/data/auto.dta"; keep id name; tabulate id` — root node has all comments, checkpoint node has comments only for `id` and `name`
2. `use "test/data/auto.dta"; rename revenue sales; count` — checkpoint node has comment on `sales` mapped from `revenue`'s original label
3. `use "test/data/auto.dta"; collapse (mean) revenue, by(year)` — no comments on checkpoint (lineage cleared)
4. Verify with `SELECT comment FROM duckdb_columns() WHERE table_schema = 'dodo'`
