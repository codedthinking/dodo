# Plan: Physical/Logical Separation with Materialization Checkpoints

## Context

dodo currently accumulates all transformations as a single CTE chain that grows unboundedly. Every query re-executes all prior steps. This plan introduces:
- **Physical tables** with content-addressed (Merkle hash) names in the `dodo` schema
- **Logical pointers** — `dodo._current` as a VIEW pointing to the latest materialized table
- **Checkpoint rules** — materialize on `use`, on side-effect/terminal commands, accumulate CTEs otherwise
- **Symbol table** — `dodo._objects` to track all materialized tables for future GC
- **CTE chain flushing** — after materialization, restart CTE chain from the VIEW base

No branching, no DAG — single linear chain only.

## Materialization Rules

| Trigger | Action |
|---------|--------|
| `use "file"` | Materialize as table, point `dodo._current` VIEW to it |
| Terminal command (tabulate, summarize, list, count, describe, codebook, display, head, tail, show, levelsof, assert) | Materialize CTE chain as table, point `dodo._current`, flush CTEs, then run the terminal query against the VIEW |
| Side-effect command (save, export) | Materialize CTE chain as table, point `dodo._current`, flush CTEs, then run COPY from the VIEW |
| Transformation command (keep, generate, replace, collapse, etc.) | Append to CTE chain only |

## Physical Naming

Tables are named `dodo.__node_<hash>` where hash is:

```
SHA-256(parent_table_hash + "\0" + canonical_sql_of_CTE_chain)
```

- For `use`: `SHA-256("\0" + source_description)` (root node)
- For checkpoints: `SHA-256(base_table_hash + "\0" + json_deserialize_sql(json_serialize_sql(full_CTE_query)))`

The hash encodes full provenance. Two identical transformation chains produce the same table name → natural deduplication (if the table already exists, skip CREATE and just rebind the VIEW).

We use DuckDB's `sha256()` and `json_serialize_sql()` for both operations.

## Schema

```sql
CREATE SCHEMA IF NOT EXISTS dodo;

-- Symbol table: tracks all materialized tables
CREATE TABLE IF NOT EXISTS dodo._objects (
    hash         VARCHAR PRIMARY KEY,   -- the sha256 hash (short hex prefix used in table name)
    table_name   VARCHAR NOT NULL,      -- full qualified name: dodo.__node_<hash12>
    parent_hash  VARCHAR,               -- hash of the base table (NULL for root)
    source_sql   VARCHAR,               -- the SQL that produced this table (CTE chain or file read)
    command_log  VARCHAR,               -- semicolon-separated original commands since last checkpoint
    row_count    BIGINT,                -- approximate row count
    created_at   TIMESTAMPTZ DEFAULT now(),
    is_current   BOOLEAN DEFAULT TRUE   -- TRUE for the table dodo._current points to
);

-- Logical pointer: always a VIEW on the latest materialized table
-- dodo._current is created as: CREATE OR REPLACE VIEW dodo._current AS SELECT * FROM dodo.__node_<hash>;
```

## Key Design Decisions

1. **Table names use 12-char hash prefix** (`dodo.__node_8f3a9c2b1d4e`) — short enough to read, long enough to avoid collisions in practice.

2. **`dodo._current` is a VIEW**, not a TABLE. Currently it's a TABLE created by `use`. Changing it to a VIEW is the core of this refactor. The VIEW always points to the latest `dodo.__node_*` table.

3. **Deduplication**: Before CREATE TABLE, check if `dodo.__node_<hash>` already exists. If so, just rebind the VIEW. This is free caching.

4. **CTE chain references `dodo._current`** as base. After materialization + flush, the next CTE step `_s0` is `SELECT * FROM dodo._current`. This means the CTE chain is always short (steps since last checkpoint).

5. **`is_current` flag**: When the VIEW moves to a new table, UPDATE the old row to `is_current = FALSE`, new row to `is_current = TRUE`. This enables future GC to identify droppable tables.

6. **Terminal commands need two-phase execution**: First materialize (CREATE TABLE + rebind VIEW + flush CTEs), then execute the terminal query. The materialization SQL goes into `pending_sql`, the terminal query is returned as the command result.

## Files to Modify

### `src/core/dodo_core.hpp`
- Add to `DodoState`:
  - `std::string current_node_hash` — hash of the table `dodo._current` points to
  - `bool objects_table_created = false` — whether `dodo._objects` exists
- Add new free functions:
  - `std::string ComputeNodeHash(const std::string &parent_hash, const std::string &sql)` — returns hash computation SQL
  - `std::string MaterializeCheckpoint(DodoState &state)` — generates SQL to materialize CTE chain as a new node table, rebind VIEW, register in symbol table, flush CTE chain
  - `std::string InitObjectsSchema(DodoState &state)` — idempotent schema + table creation
- Update `Clear()` and `BuildCleanupSQL()` to handle new state

### `src/core/dodo_core.cpp`
- **`use` command handler** (~line 1620): Instead of `CREATE TABLE dodo._current`, create `dodo.__node_<hash>` and point `dodo._current` VIEW to it. Register in `dodo._objects`.
- **`ProcessCommand` terminal commands** (tabulate, summarize, list, count, describe, codebook, display, head, tail, show, levelsof, assert): If CTE chain has steps beyond the base, materialize first (emit checkpoint SQL via `pending_sql`), then return the terminal query referencing `dodo._current`.
- **Side-effect commands** (save, export): Same materialization-first pattern.
- **`BuildCleanupSQL()`**: Drop all `dodo.__node_*` tables and `dodo._objects`.

### `src/extension/dodo_extension.cpp`
- **`BuildLiveViewSQL()`**: After materialization, the `_dodo_data` view should reference either the CTE chain (if steps exist) or `dodo._current` directly.
- **`BuildHistorySQL()`**: May need to read from `dodo._objects` command_log for full history across checkpoints.
- Pending SQL drainage already works — materialization SQL emitted via `state.core.pending_sql` will be picked up by the existing drain loop (line 239-246).

## Materialization Flow (step by step)

### On `use "data/large.dta", clear`:
1. `CREATE SCHEMA IF NOT EXISTS dodo`
2. `CREATE TABLE IF NOT EXISTS dodo._objects (...)`
3. Compute hash: `sha256('\0' || 'data/large.dta')` → e.g. `"8f3a9c2b1d4e..."`
4. `CREATE TABLE dodo.__node_8f3a9c2b1d4e AS SELECT * FROM read_dta('data/large.dta')`
5. `CREATE OR REPLACE VIEW dodo._current AS SELECT * FROM dodo.__node_8f3a9c2b1d4e`
6. `INSERT INTO dodo._objects VALUES (...)`
7. Set `state.current_node_hash = "8f3a9c2b1d4e..."`
8. `state.AddStep("SELECT * FROM dodo._current")` — first CTE step references the VIEW

### On `keep if year >= 2020`:
1. Normal CTE accumulation: `state.AddStep("SELECT * FROM _s0 WHERE year >= 2020")`
2. No materialization.

### On `tabulate sector` (terminal command):
1. **Checkpoint**: Materialize the CTE chain up to this point:
   - Build full CTE query: `WITH _s0 AS (SELECT * FROM dodo._current), _s1 AS (SELECT * FROM _s0 WHERE year >= 2020), ... SELECT * FROM _sN`
   - Compute hash: `sha256(parent_hash || '\0' || canonical_sql)`
   - `CREATE TABLE dodo.__node_<newhash> AS (full CTE query)`
   - `UPDATE dodo._objects SET is_current = FALSE WHERE is_current = TRUE`
   - `INSERT INTO dodo._objects VALUES (..., is_current = TRUE)`
   - `CREATE OR REPLACE VIEW dodo._current AS SELECT * FROM dodo.__node_<newhash>`
   - Flush CTE chain: `state.cte_steps.clear(); state.step_counter = 0`
   - `state.AddStep("SELECT * FROM dodo._current")` — restart from VIEW
   - All this goes into `pending_sql`
2. **Terminal query**: Return `SELECT sector, COUNT(*) AS freq FROM dodo._current GROUP BY sector ORDER BY sector`
   - Note: now queries `dodo._current` directly (no CTE chain needed since we just flushed)

### On `save "data/sectoral.dta", replace`:
1. Same checkpoint pattern as terminal commands
2. Then `COPY (SELECT * FROM dodo._current) TO 'data/sectoral.dta' (FORMAT DTA)`

## Implementation Order

1. **Add `dodo._objects` schema init** — `InitObjectsSchema()` function
2. **Add `ComputeNodeHash()`** — SQL expression returning sha256 hash
3. **Add `MaterializeCheckpoint()`** — the core materialization + flush logic
4. **Refactor `use` command** — create node table + VIEW instead of TABLE
5. **Add checkpoint calls for terminal commands** — detect CTE chain > base, materialize first
6. **Add checkpoint calls for side-effect commands** — same pattern
7. **Update `Clear()`, `BuildCleanupSQL()`** — handle new objects
8. **Update `BuildLiveViewSQL()`** — handle post-materialization state
9. **Update `BuildHistorySQL()`** — full history across checkpoints

## Verification

1. Run: `use "test/data/auto.dta"; keep if price > 5000; generate lprice = ln(price); tabulate foreign` — verify two `dodo.__node_*` tables exist, `dodo._current` is a VIEW, `dodo._objects` has 2 rows
2. Run: `summarize price` after the above — should query `dodo._current` directly (no CTE chain), no new materialization (chain is empty after tabulate checkpoint)
3. Run: `keep if mpg > 20; save "test/data/out.dta", replace` — verify third node table created before save
4. Run: `SELECT * FROM dodo._objects` — verify 3 rows with correct `is_current` flags
5. Run existing test suite: `make test`

## Hash Computation Detail

The hash must be computed **before** the CREATE TABLE executes, because the table name contains the hash. Two approaches:

**Option A: Compute in C++** — Use a C++ SHA-256 library. Simpler control flow, no round-trip to DuckDB.

**Option B: Two-phase SQL** — First execute `SELECT sha256(...)` to get the hash, then use it in `CREATE TABLE`. Requires an extra query round-trip.

**Recommendation: Option A** — DuckDB already links OpenSSL (used by httpfs). We can use `duckdb/common/crypto/mbedtls_wrapper.hpp` or similar. Alternatively, since the hash is for naming only (not cryptographic security), we could use a simpler hash. But SHA-256 is fine and matches the design doc.

**Decision: Option A with DuckDB's MbedTLS.**

DuckDB bundles MbedTLS at `duckdb/third_party/mbedtls/include/mbedtls_wrapper.hpp`:
```cpp
#include "mbedtls_wrapper.hpp"
// std::string hash = duckdb_mbedtls::MbedTlsWrapper::ComputeSha256Hash(input_string);
```

This returns a 32-byte binary hash. Use `MbedTlsWrapper::ToBase16()` or take the first 12 hex chars for the table name suffix.

Input string for hashing: `parent_hash + "\0" + canonical_sql` where canonical_sql is the full CTE query text (we skip `json_serialize_sql` round-tripping for now since it requires a DB connection — can add later for better deduplication).
