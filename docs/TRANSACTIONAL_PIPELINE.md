# Transactional Pipelines for dodo

## Overview

dodo is a content-addressable version control system for interactive SQL
work on columnar data. It follows the git object model -- commits, refs,
branches, tags -- but applied to dataframes: SQL transforms are commits,
the current dataframe is HEAD, undo is reset, alternatives are branches.

This document consolidates research on git internals, build system theory,
pipeline execution models, and external prior art (dbt, Bauplan) into a
single design reference for dodo's transactional pipeline architecture.

---

## Part 1: Foundations

### Content-Addressable Storage

Git is a content-addressable filesystem. Every piece of content is
identified by the SHA-1 hash of its content plus a header:

- Identical content always produces the same hash (deduplication for free).
- Objects are immutable: changing content creates a new object with a new hash.
- Any corruption is detectable by recomputing the hash.

Object storage format: `<type> <size>\0<content>`, compressed with zlib.

### The Four Object Types

**Blob.** Stores raw file content. No filename, no metadata -- just bytes.
Two files with identical content share the same blob object.

**Tree.** Stores a directory listing: a mapping from names to blobs or
subtrees. Each entry has a mode (`100644` regular, `040000` subdirectory,
etc.), type, SHA-1 hash, and name. Trees are recursive: a tree can point
to subtrees, forming a full directory hierarchy.

**Commit.** Points to a single root tree (the project snapshot) and records
metadata: parent pointer(s), author, committer, timestamp, message. The
parent pointers form the history chain. A merge commit has two or more
parents.

**Tag (annotated).** Points to any object (usually a commit) with metadata:
object hash, type, tag name, tagger, message. Lightweight tags are just
refs, not objects.

### The DAG (Directed Acyclic Graph)

Commits form a DAG through their parent pointers:

```
c1 <-- c2 <-- c3       (linear history)

c1 <-- c2 <-- c4       (branch + merge)
        \      ^
         c3 --/
```

Properties: directed (edges point child to parent), acyclic (no commit can
be its own ancestor), reachability-based garbage collection.

### Merkle Tree Properties

Git's object graph is a Merkle tree:

1. Each blob is hashed from its content.
2. Each tree is hashed from its entries (which include child hashes).
3. Each commit is hashed from its tree hash, parent hashes, and metadata.

Any change propagates upward: modifying a single file changes its blob
hash, which changes the tree hash, which changes the commit hash. To
verify a full snapshot, you only need to verify the root commit hash.

### References (Refs)

Refs are human-readable pointers stored as plain text containing a hash.

| Type | Mutable? | Purpose |
|------|----------|---------|
| Branch | Yes, advances on commit | Current line of work |
| Lightweight tag | No (by convention) | Named snapshot |
| Annotated tag | No | Named snapshot + metadata |
| HEAD | Yes | Current checkout |

HEAD is a symbolic reference: it usually contains `ref: refs/heads/master`,
not a hash directly. When HEAD contains a raw hash, the repo is in
"detached HEAD" state.

A branch is just a 41-byte file. Creating a branch = writing a new ref.
Switching branches = updating HEAD. This is why branching in git is nearly
instant regardless of repository size.

**Key insight: objects are the database, refs are the index into it.**

---

## Part 2: Build System Theory

### The Bipartite DAG Model

From Petri net theory: pipelines have two kinds of nodes:

- **Data nodes** -- tables, views, CTEs, scalar values.
- **Code nodes** -- transforms that consume input data and produce output data.

Edges always cross types (data -> code or code -> data). This enforces
separation of concerns: materialization strategy is a property of data
nodes, not code nodes. The user thinks about transformations; the system
decides how to store intermediates.

Petri net properties that apply:
- **Firing rule**: a code node can execute when all its input data nodes have values.
- **Boundedness**: each data node holds at most one value (one table/result).
- **Liveness**: every code node should eventually be fireable. A dead transition means unreachable code.

### Build Systems Taxonomy (Mokhov et al. 2018)

Every build system is a composition of two independent choices:

**Scheduler (execution order):**

| Scheduler | Dependencies | Systems |
|-----------|-------------|---------|
| Topological | Static only | Make, Buck |
| Restarting | Dynamic | Excel, Bazel |
| Suspending | Dynamic | Shake, Nix |

**Rebuilder (what to re-execute):**

| Rebuilder | What it stores | Early cutoff? |
|-----------|---------------|---------------|
| Dirty bit | One bit per key | No |
| Verifying traces | Hash of each dependency | Yes |
| Constructive traces | Hash + actual result | Yes |
| Deep constructive traces | Hash of terminal inputs only | No |

**Early cutoff**: if a code node re-executes but produces the same output
as before, downstream nodes don't need rebuilding. Verifying traces support
this; dirty bits don't.

The full taxonomy (Table 2 from the paper):

|                          | Topological | Restarting | Suspending |
|--------------------------|-------------|------------|------------|
| Dirty bit                | Make        | Excel      | --         |
| Verifying traces         | Ninja       | --         | Shake      |
| Constructive traces      | CloudBuild  | Bazel      | --         |
| Deep constructive traces | Buck        | --         | Nix        |

**Core abstractions:**
- **Store**: key -> value mapping (node identifiers -> computed results).
- **Task**: a function that fetches dependency values and computes a new value.
- **Build**: takes task description + target key + store; returns updated store.
- **Correctness**: every output equals what you'd get by recomputing from scratch.
- **Minimality**: tasks execute at most once per build, only if they transitively depend on changed inputs.

### Incremental Computing

The general problem: given a computation and a small change to its inputs,
recompute only the affected outputs.

- **Change propagation**: follow the transitive closure of dependencies from changed inputs.
- **Self-adjusting computation**: track dependencies at runtime, cache stable subcomputations.
- Spreadsheets are the canonical example: change a cell, recalculate only transitive dependents.

### Memoization and Caching

- Only valid for referentially transparent computations (same inputs -> same output).
- Side effects break memoization.
- Content-addressable storage (Bazel, Nix): index cached results by hash of inputs.
- Time-space tradeoff: storing intermediates costs storage but saves recomputation time.

### Error Handling in DAGs

When a code node errors:
- **Stop downstream**: all transitively dependent nodes are skipped.
- **Continue independent branches**: unrelated branches can still execute.
- **Retry with escalation**: more resources on retry (Snakemake, Airflow).

### Precious Intermediates

When a code node is expensive, its output becomes worth persisting:
- Automatic materialization if a node took > N seconds.
- Explicit checkpointing by the user.
- Protected outputs to prevent accidental deletion.

---

## Part 3: Prior Art

### dbt

- **Manifest**: full project snapshot -- all nodes, `parent_map` and `child_map` for DAG traversal.
- **Run results**: per-node execution results -- status, timing, compiled SQL.
- **State comparison**: `--defer` and `state:modified` compare current project against a previous manifest, enabling "slim CI" (run only modified models).
- **Retry**: `dbt retry` resumes from the point of failure using run results.
- **Materialization**: table, view, ephemeral (CTE), incremental.
- **Lineage**: `ref()` function creates explicit edges in the DAG.

### Bauplan (git-for-data)

Source: https://docs.bauplanlabs.com/concepts/git-for-data/transactional-pipelines

**Core model:** Every pipeline run is a database transaction -- atomic,
isolated, versioned.

**Execution architecture:**
1. Fork an isolated temporary branch for the run.
2. Materialize intermediate outputs on the temp branch.
3. On success: atomic merge of all outputs to the working branch as a new ref.
4. On failure: working branch untouched; temp artifacts kept for debugging.

**Branching:**
- Zero-copy: creating a branch is instantaneous, only metadata changes.
- Username-based namespaces (`user.branch_name`) with write isolation.
- **Branches** are mutable (advance with new runs). **Refs** are immutable (point to specific commits for reproducibility).

**Transactional semantics:** Each state change creates a new commit. Success
advances the branch pointer. Failure leaves it unchanged. Branch pointer
advancement is the commit mechanism.

### Make / Snakemake

- **Make**: target/prerequisites/recipe, timestamp-based invalidation, topological execution, parallel with `make -j N`.
- **Snakemake**: extends Make with input/output declarations, wildcard patterns, checksum-based invalidation, retries with escalating resources.

---

## Part 4: dodo's Design

### Current State

```
DodoState
  cte_steps[]       -- vector of inner SQL strings, one per transform
  cte_commands[]    -- original command text, parallel to cte_steps
  redo_stack[]      -- (command, sql) pairs popped by undo
  step_counter      -- next CTE index (_s0, _s1, ...)
  preserve/restore  -- checkpoint into cte_steps by index
```

Linear history. Undo pops from `cte_steps`, pushes to `redo_stack`. A new
command after undo clears the redo stack (destructive). No branches, no
tags, no persistence across sessions.

### Target State

```
DodoState
  cte_steps[]       -- unchanged: in-session CTE chain
  cte_commands[]    -- unchanged: parallel command text
  step_counter      -- unchanged: CTE naming (_sN)

  commits table     -- persistent DAG of all transforms
  branches table    -- named mutable pointers to commits
  tags table        -- named immutable pointers to commits
  head table        -- current branch (or detached commit)
  branch_stack      -- saved positions for /btw and /back
```

The CTE chain remains the live execution mechanism. The commit DAG is a
parallel persistent record. Every `AddStep()` also writes a commit.
Undo moves HEAD back along the parent chain instead of popping a stack.
Branching is automatic when the user diverges after undo.

### The Git Analogy

| Git | dodo |
|-----|------|
| Working tree | Current query result (the dataframe) |
| Commit | One command (SQL transform) |
| Commit message | Original command text |
| Commit tree | Output schema (column names + types) |
| Commit parent(s) | Input dataframe(s) -- one for most, two for merge |
| HEAD | Current position in the DAG |
| Branch | Named pointer, advances on new commands |
| Tag | Named immutable snapshot |
| `git reset --soft` | `/undo` -- move HEAD back, keep commits |
| `git checkout -b` | Auto-branch on divergence after undo |
| `git stash` + `pop` | `/btw` + `/back` |
| `git log` | `/log` |
| `__head__` view | `_dodo_data` live view |
| `.git/objects/` | `dodo.commits`, `dodo.blobs` |
| `.git/refs/` | `dodo.branches`, `dodo.tags`, `dodo.head` |

### Object Model

#### Commit

The core object. Each command that transforms data creates one commit.

```
commit
  hash          -- SHA-256 of (sorted parent hashes + canonical_sql)
  parents[]     -- hashes of parent commits (empty for root)
  sql_text      -- the SQL that was executed (pure function: parents -> sql -> output)
  command_text  -- original command text (for display)
  input_tree    -- schema hash before transform (deferred, may be NULL)
  output_tree   -- schema hash after transform (deferred, may be NULL)
  row_count     -- number of rows in result
  duration_ms   -- execution time
  created_at    -- timestamp
```

Each SQL transform is a pure function: it takes one or more input
dataframes (the parents) and produces one output dataframe. The commit
hash is `H(sorted_parent_hashes + sql)`, so it encodes the full lineage.
Two users who run the same commands on the same inputs get the same hashes
-- a Merkle DAG.

Parent commits are derived from the SQL AST. Parsing with
`json_serialize_sql()` exposes all table references in `from_table` nodes
(type `BASE_TABLE`, field `table_name`). Each referenced table that maps
to a known branch or CTE step becomes a parent edge. The DAG structure is
derived from the SQL itself, not from command type conventions.

#### Tree (deferred to Phase 4)

A schema snapshot: sorted list of (column_name, dtype) pairs, hashed for
identity. Two commits with the same output_tree have identical schemas.

#### Blob (deferred to Phase 4)

Content hash of column data or SQL text. Enables Merkle integrity and
deduplication.

#### Tag

An immutable named pointer to a commit: name, target hash, optional
message, timestamp.

#### Branch

A mutable named pointer to a commit: name, target hash, timestamps.
Advances when a new commit is made on this branch.

#### HEAD

Singleton. Points to either a branch name (attached) or a commit hash
(detached). Exactly one is set.

### Commit Hashing

```
SHA-256(sorted_parent_hashes + "\0" + canonical_sql)
```

Parent hashes are sorted lexicographically and concatenated with `\0`
separators before the SQL.

- **Zero parents** (root): `SHA-256("\0" + source_description)`.
- **One parent** (most commands): `SHA-256(parent_hash + "\0" + sql)`.
- **Two parents** (merge): `SHA-256(min(p1,p2) + "\0" + max(p1,p2) + "\0" + sql)`.

#### SQL Canonicalization

Round-tripping through `json_deserialize_sql(json_serialize_sql(sql))`
produces a canonical form: explicit keywords, consistent casing, normalized
whitespace. Use the round-tripped SQL before hashing:

```sql
SELECT sha256(
    parent_hash || E'\0' ||
    json_deserialize_sql(json_serialize_sql(sql_text))
) AS hash
```

#### Dependency Extraction

The same JSON AST exposes all table references:

```sql
SELECT DISTINCT json_extract_string(node, '$.table_name') AS table_name
FROM (
    SELECT unnest(
        json_extract(json_serialize_sql(sql_text), '$.statements[*].node.from_table')
    ) AS node
)
WHERE json_extract_string(node, '$.type') = 'BASE_TABLE';
```

Each referenced table that maps to a known dodo object becomes a parent
edge. For joins and subqueries, the AST contains nested `from_table` nodes
that must be extracted recursively.

### DuckDB Schema

All metadata lives in the `dodo` schema:

```sql
CREATE TABLE IF NOT EXISTS dodo.commits (
    hash         VARCHAR PRIMARY KEY,
    sql_text     VARCHAR,
    command_text VARCHAR,
    input_tree   VARCHAR,     -- NULL until Phase 4
    output_tree  VARCHAR,     -- NULL until Phase 4
    row_count    BIGINT,
    duration_ms  BIGINT,
    created_at   TIMESTAMPTZ DEFAULT now()
);

CREATE TABLE IF NOT EXISTS dodo.commit_parents (
    commit_hash  VARCHAR NOT NULL REFERENCES dodo.commits(hash),
    parent_hash  VARCHAR NOT NULL REFERENCES dodo.commits(hash),
    parent_index INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (commit_hash, parent_index)
);

CREATE TABLE IF NOT EXISTS dodo.tags (
    name         VARCHAR PRIMARY KEY,
    target       VARCHAR NOT NULL REFERENCES dodo.commits(hash),
    message      VARCHAR,
    created_at   TIMESTAMPTZ DEFAULT now()
);

CREATE TABLE IF NOT EXISTS dodo.branches (
    name         VARCHAR PRIMARY KEY,
    target       VARCHAR NOT NULL REFERENCES dodo.commits(hash),
    created_at   TIMESTAMPTZ DEFAULT now(),
    updated_at   TIMESTAMPTZ DEFAULT now()
);

CREATE TABLE IF NOT EXISTS dodo.head (
    id           INTEGER PRIMARY KEY DEFAULT 1,
    branch_name  VARCHAR REFERENCES dodo.branches(name),
    commit_hash  VARCHAR REFERENCES dodo.commits(hash),
    CHECK (
        (branch_name IS NOT NULL AND commit_hash IS NULL) OR
        (branch_name IS NULL     AND commit_hash IS NOT NULL)
    )
);

CREATE TABLE IF NOT EXISTS dodo.branch_stack (
    stack_depth  INTEGER PRIMARY KEY,
    branch_name  VARCHAR NOT NULL,
    head_at_push VARCHAR NOT NULL REFERENCES dodo.commits(hash),
    pushed_at    TIMESTAMPTZ DEFAULT now()
);
```

#### Views

```sql
-- Resolved HEAD: actual commit hash regardless of attached/detached.
CREATE OR REPLACE VIEW dodo.head_commit AS
SELECT
    COALESCE(b.target, h.commit_hash) AS commit_hash,
    h.branch_name,
    h.commit_hash IS NOT NULL AS is_detached
FROM dodo.head h
LEFT JOIN dodo.branches b ON b.name = h.branch_name;

-- Log: recursive walk from HEAD along primary parent chain.
CREATE OR REPLACE VIEW dodo.log AS
WITH RECURSIVE chain AS (
    SELECT c.*, 0 AS depth
    FROM dodo.commits c
    JOIN dodo.head_commit h ON h.commit_hash = c.hash
    UNION ALL
    SELECT c.*, chain.depth + 1
    FROM dodo.commits c
    JOIN dodo.commit_parents p ON p.parent_hash = c.hash
    JOIN chain ON p.commit_hash = chain.hash
    WHERE p.parent_index = 0
)
SELECT chain.*, t.name AS tag_name
FROM chain
LEFT JOIN dodo.tags t ON t.target = chain.hash
ORDER BY depth;

-- Children index: detect forks.
CREATE OR REPLACE VIEW dodo.children AS
SELECT parent_hash AS commit_hash, commit_hash AS child_hash
FROM dodo.commit_parents WHERE parent_index = 0;

-- Branch overview.
CREATE OR REPLACE VIEW dodo.branch_status AS
SELECT
    br.name,
    br.target,
    c.created_at AS last_commit_at,
    c.command_text AS last_command,
    c.row_count,
    br.name = h.branch_name AS is_current
FROM dodo.branches br
JOIN dodo.commits c ON c.hash = br.target
JOIN dodo.head h ON TRUE
ORDER BY is_current DESC, last_commit_at DESC;
```

### Data Flow: How a Command Becomes a Commit

Current flow (unchanged):
```
user types command
    -> TokenizeCommand()
    -> ProcessCommand()     -- generate SQL, call AddStep()
    -> AddStep(inner_sql)   -- append to cte_steps[], increment step_counter
    -> BuildQuery() + execute
    -> display result
```

New flow (additions marked with `*`):
```
user types command
    -> TokenizeCommand()
    -> ProcessCommand()
    -> AddStep(inner_sql)
    -> * parse SQL AST              -- json_serialize_sql(inner_sql)
    -> *   extract table refs       -- find BASE_TABLE nodes -> parent commits
    -> * head_has_children()?       -- does current HEAD have child commits?
    -> *   YES -> auto_branch()     -- create new branch before committing
    -> * compute commit hash        -- SHA-256(sorted parents + canonical sql)
    -> * INSERT INTO dodo.commits
    -> * INSERT INTO dodo.commit_parents
    -> * UPDATE dodo.branches       -- advance current branch target
    -> BuildQuery() + execute
    -> * UPDATE dodo.commits        -- record row_count, duration_ms
    -> display result
```

The CTE chain (`_s0`, `_s1`, ...) remains the execution mechanism. The
commit table is a parallel persistent log. The two are kept in sync but
serve different purposes: CTE chain for query execution, commit DAG for
history navigation.

### CTE Chain and Commit DAG Synchronization

**On session start (if commits exist):** Walk the commit DAG from HEAD back
to root, rebuild the CTE chain from each commit's `sql_text`.

**On each command:** `AddStep()` appends to the CTE chain *and* writes a
commit to `dodo.commits`.

**On undo:** Move HEAD back N commits along the parent chain. Truncate the
CTE chain to match. Old commits remain (they dangle).

**On branch switch:** Walk the target branch's commit chain from its target
back to root. Rebuild the CTE chain from scratch. O(chain length) but
chains are typically short.

### User-Facing Commands

#### New slash commands

| Command | Git equivalent | Action |
|---------|----------------|--------|
| `/undo [N]` | `git reset HEAD~N` | Move HEAD back N commits. Truncate CTE chain. |
| `/branch <name>` | `git checkout -b` | Create branch at HEAD, switch to it. |
| `/switch <name\|hash>` | `git checkout` | Switch to branch, tag, or commit hash prefix. Rebuild CTE chain. |
| `/tag <name> [message]` | `git tag -a` | Create immutable named pointer at HEAD. |
| `/btw [name]` | `git stash` + `checkout -b` | Push current branch to stack, start new branch. |
| `/back` | `git stash pop` | Pop branch stack, restore previous branch. |
| `/log [N]` | `git log` | Show commit history from HEAD. |
| `/branches` | `git branch` | List all branches with status. |
| `/diff [target]` | `git diff` | Column-level diff between HEAD and target. |
| `/export` | `git format-patch` | Emit self-contained SQL script from commit chain. |

#### Modified existing commands

| Command | Current behavior | New behavior |
|---------|-----------------|--------------|
| `undo [N]` | Pop cte_steps to redo_stack | Move HEAD back N commits. Old commits dangle. |
| `redo [N]` | Pop redo_stack to cte_steps | Replaced by `/switch` to dangling commit or branch. |
| `history` | Show linear step list | Show `/log` (DAG-aware). |
| `preserve` | Save index into cte_steps | Create anonymous tag at HEAD. |
| `restore` | Truncate cte_steps to index | Switch HEAD to the preserve tag. |

#### Automatic branching

When the user undoes and then types a new command, the current HEAD has
children. Before committing, dodo automatically:

1. Creates a new branch (`alt-1`, `alt-2`, ...).
2. Points the new branch at the current HEAD commit.
3. Switches HEAD to the new branch.
4. Commits the new command on the new branch.

The old branch still points to its tip. The user can `/switch` back.

```
main:    use -> keep -> generate -> collapse
                                       ^
                                       | HEAD was here, user did /undo 2
                        HEAD is here --+
                                       |
                                       v
alt-1:                  keep -> sort -> ...  (user typed new commands)
```

#### /btw and /back (branch stack)

`/btw aggregate_employment`:
1. Push current (branch_name, HEAD commit) onto `dodo.branch_stack`.
2. Create new branch `aggregate_employment` at HEAD.
3. Switch HEAD to the new branch.

`/back`:
1. Pop top of `dodo.branch_stack`.
2. Switch HEAD to the saved branch at the saved commit.
3. Rebuild CTE chain.

The stack supports nesting: `/btw` inside a `/btw` pushes another frame.

### Transactional Batch Execution

Inspired by Bauplan's model, `.do` file execution can use transactional
semantics:

1. Fork an isolated temporary branch before the run.
2. Execute all commands on the temp branch (each creating commits).
3. On success: advance the working branch to the temp branch's HEAD.
4. On failure: discard the temp branch; working branch is untouched.

This gives interactive use per-command commit granularity and batch use
per-pipeline-run atomicity. Both coexist naturally because the branching
mechanism is the same.

### Integration with Existing Architecture

The commit/branch/tag logic is purely data manipulation (INSERT, UPDATE,
SELECT on `dodo.*` tables). It belongs in `dodo_core.hpp` / `dodo_core.cpp`,
not in the extension layer. The extension layer only ensures the schema is
created on load.

New additions to `DodoState`:

```cpp
struct DodoState {
    // ... existing fields unchanged ...

    //! Current branch name (empty string = detached HEAD)
    std::string current_branch;

    //! Current HEAD commit hash (empty string = no commits yet)
    std::string head_commit;

    //! Whether the dodo.commits schema has been initialized
    bool vcs_initialized = false;
};
```

New free functions in `dodo_core.hpp`:

```cpp
std::string InitVcsSchema();
std::string CreateCommit(DodoState &state, const std::string &sql_text,
                         const std::string &command_text);
std::string CreateRootCommit(DodoState &state,
                             const std::string &source_description);
std::string UndoCommits(DodoState &state, int n);
std::string CreateBranch(DodoState &state, const std::string &name);
std::string SwitchTo(DodoState &state, const std::string &name_or_hash);
std::string CreateTag(DodoState &state, const std::string &name,
                      const std::string &message);
std::string Btw(DodoState &state, const std::string &new_branch);
std::string Back(DodoState &state);
std::string ShowLog(DodoState &state, int max_entries);
std::string ShowBranches(DodoState &state);
std::string ExportSql(DodoState &state);
std::string CheckForFork(DodoState &state);
std::string AutoBranchName(DodoState &state);
```

CTE naming (`_s0`, `_s1`, ...) stays as-is. The `_dodo_data` live view
stays as-is. History table becomes a view over `dodo.log`.

---

## Part 5: Implementation Phases

### Phase 1: Persist linear history (commits only)

Every command writes a commit. History survives session restart.

1. Add `dodo.commits` and `dodo.commit_parents` tables.
2. On `use` (root): INSERT with no parent edges.
3. On each `AddStep()`: parse SQL AST, extract table refs, resolve parents, INSERT commit + parent edges.
4. Compute hash via `sha256()` and `json_serialize_sql()`.
5. Store `head_commit` in `DodoState`.
6. On session start with existing commits: rebuild CTE chain from ancestor chain.
7. Replace `history` to read from `dodo.commits`.
8. Existing `undo`/`redo` unchanged.

**Test:** Run commands, close DuckDB, reopen, verify `history` shows same commands.

### Phase 2: Branches and HEAD

Named branches. HEAD tracks current position. Undo moves HEAD.

1. Add `dodo.branches`, `dodo.head` tables.
2. On `use`: create `main` branch, set HEAD to attached.
3. On each commit: advance branch target.
4. Rewrite `undo` to move HEAD back N commits. Rebuild CTE chain.
5. Remove `redo_stack`. Redo replaced by `/switch`.
6. Add `/branch`, `/switch`.
7. Add auto-branching on divergence.

**Test:** Undo 2, type new command, verify auto-branch. `/switch` back, verify CTE restored.

### Phase 3: Tags, branch stack, export

Full interactive workflow.

1. Add `dodo.tags`, `/tag`.
2. Rewrite `preserve` as auto-named tag, `restore` as `/switch`.
3. Add `dodo.branch_stack`, `/btw`, `/back`.
4. Add `/log`, `/branches`, `/export`.

**Test:** `/btw`, do work, `/back`, verify restored. Nested `/btw`. `/export` produces valid SQL.

### Phase 4: Trees, blobs, Merkle integrity (future)

Full content-addressable object model. Schema snapshots. Column-level diff.

1. Add `dodo.trees` (schema snapshots).
2. Derive tree from DuckDB result metadata on each commit.
3. Add `dodo.blobs` for SQL text deduplication.
4. Add column-level `/diff`.
5. Optionally: hash column data for full Merkle integrity.

### Phase 5: Transactional batch execution (future)

Atomic `.do` file execution using Bauplan-style fork/merge.

1. On `.do` file start: create temp branch from current HEAD.
2. Execute all commands on temp branch.
3. On success: fast-forward working branch to temp branch HEAD.
4. On failure: discard temp branch, report error, working branch untouched.

---

## Part 6: Open Questions

1. **Scheduler**: topological (static, like Make) or suspending (dynamic, like Shake)? Commands have mostly static dependencies, suggesting topological. But conditional logic could create data-dependent paths.

2. **Rebuilder**: dirty bits (simple) or verifying traces (hash-based, early cutoff)? Hash-based is more correct but requires hashing data nodes, expensive for large tables.

3. **Materialization**: which data nodes are CTEs (ephemeral), which are views, which are tables? Automatic (performance-driven) or user-controlled?

4. **Granularity**: each command = one code node, or group sequences?

5. **Side effect isolation**: model `save`, `export`, logging as separate sink nodes to keep the core graph pure and memoizable?

6. **Error recovery**: when a code node fails, can we resume from the last successful data node? Interacts with materialization -- ephemeral CTEs can't be checkpointed.

7. **Interactive vs batch**: the DAG must support both. Interactive wants fast feedback; batch wants minimal recomputation and atomicity.

8. **Multi-file pipelines**: how do `.do` files compose into a larger DAG? Each file could be one code node, or files could share data nodes via `save`/`use`.

---

## Part 7: Cross-Cutting Comparison

| Theme | Git | dodo | Bauplan | dbt |
|-------|-----|------|---------|-----|
| Unit of versioning | File tree snapshot | SQL transform | Pipeline run outputs | Model (SELECT) |
| Hash basis | Content + metadata | Parents + canonical SQL | Not documented | Not content-addressed |
| Branching cost | ~Free (pointer) | ~Free (INSERT) | Zero-copy (metadata) | N/A |
| Atomicity | Single commit | Single AddStep | Full pipeline run | Per-model |
| Failure model | Working tree dirty | CTE chain unchanged | Branch unchanged | Partial (retry) |
| Materialization | N/A (files) | CTE chain (ephemeral) | Iceberg tables | table/view/ephemeral |
| Lineage | Parent pointers | SQL AST table refs | Pipeline DAG | `ref()` graph |

---

## Non-Goals

- Remote sync / shared repos.
- Full SQL parser (use `json_serialize_sql()` canonicalization).
- GUI / TUI beyond the DuckDB shell.
- Arrow buffer hashing on every commit (Phase 4, deferred to `/tag`).
