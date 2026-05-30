# dodo — Columnar Version Control for Interactive SQL
## Implementation Plan

---

## Overview

`dodo` is a content-addressable version control system for interactive SQL
work on columnar data. It follows the git object model (blob, tree, commit,
ref, tag) but applied to dataframes: columns are files, schemas are trees,
SQL transforms are commits. The backing store is DuckDB (C API), which
provides both the metadata store and the live query engine in a single
in-process library. Persistence is free — switching from `:memory:` to a
file path is a one-line change.

The REPL always shows a live `__head__` view. Every SQL the user types is a
commit. Undo, branch, tag, and switch are first-class operations.

---

## Repository Layout

```
dodo/
  src/
    db.h          -- DuckDB C API RAII wrappers (connection, appender, chunk)
    db.c
    hash.h        -- SHA-256 over arbitrary bytes; hex encoding
    hash.c
    objects.h     -- Blob, Tree, TreeEntry, Commit, Tag structs
    objects.c     -- put_blob, put_tree, put_commit, put_tag (write to DuckDB)
    refs.h        -- Head, Branch, BranchStack structs
    refs.c        -- resolve_head, advance_branch, checkout, push/pop stack
    repo.h        -- Repo struct; aggregates db + all object/ref operations
    repo.c
    session.h     -- Session struct; owns Repo + display params + CTE registry
    session.c     -- run, load_file, undo, tag, btw, back, export_sql
    repl.h        -- REPL command parser and dispatch
    repl.c
    main.c        -- entry point
  sql/
    schema.sql    -- CREATE SCHEMA dodo + all tables (run once on init)
    views.sql     -- dodo.log, dodo.tree_diff, dodo.branch_status, etc.
    macros.sql    -- dodo.resolve_head(), dodo.advance_head(), etc.
  test/
    test_hash.c
    test_objects.c
    test_repo.c
    test_session.c
  CMakeLists.txt
  PLAN.md         -- this file
```

---

## Layer 1: db.h — DuckDB C API Wrappers

Thin RAII wrappers. No logic, just lifetime management and typed accessors.
All DuckDB interaction in upper layers goes through these types.

```c
/* db.h */

typedef struct {
    duckdb_database  db;
    duckdb_connection con;
} Db;

/* Opens ":memory:" if path is NULL. Exits on failure. */
Db   db_open(const char *path);
void db_close(Db *db);

/* Execute SQL with no result (DDL, INSERT, UPDATE). Exits on error. */
void db_exec(Db *db, const char *sql);

/* Execute SQL with a bound VARCHAR parameter. */
void db_exec1(Db *db, const char *sql, const char *param);

/* Execute SQL and return a materialized result. Caller must db_result_free(). */
duckdb_result db_query(Db *db, const char *sql);
duckdb_result db_query1(Db *db, const char *sql, const char *param);
void          db_result_free(duckdb_result *r);

/* Chunk-based typed accessors (preferred over deprecated column_data). */
typedef struct {
    duckdb_data_chunk chunk;
    idx_t             row_count;
    idx_t             col_count;
} Chunk;

/* Fetch next chunk from a result. Returns chunk with row_count==0 at end. */
Chunk chunk_next(duckdb_result *r);
void  chunk_free(Chunk *c);

const char *chunk_str(Chunk *c, idx_t col, idx_t row);   /* VARCHAR */
int64_t     chunk_i64(Chunk *c, idx_t col, idx_t row);   /* BIGINT  */
int         chunk_null(Chunk *c, idx_t col, idx_t row);  /* 1 if NULL */

/* Appender helpers — for bulk INSERT without SQL string building. */
typedef struct { duckdb_appender app; } Appender;

Appender appender_open(Db *db, const char *schema, const char *table);
void     appender_str(Appender *a, const char *val);   /* VARCHAR or NULL if val==NULL */
void     appender_i64(Appender *a, int64_t val);
void     appender_null(Appender *a);
void     appender_end_row(Appender *a);
void     appender_flush(Appender *a);
void     appender_close(Appender *a);
```

---

## Layer 2: hash.h — Content Hashing

SHA-256 over raw bytes. Returns a 64-char lowercase hex string (null-terminated,
65 bytes). Callers own the returned buffer.

```c
/* hash.h */

#define HASH_HEX_LEN 64

typedef struct { char hex[HASH_HEX_LEN + 1]; } Hash;

Hash hash_bytes(const uint8_t *data, size_t len);
Hash hash_str(const char *s);        /* convenience: hash a C string */
Hash hash_file(const char *path);    /* stream file from disk, no full read */

int  hash_eq(Hash a, Hash b);        /* 1 if equal */
int  hash_null(Hash h);              /* 1 if all-zero (sentinel) */
Hash hash_zero(void);

/* Short prefix for display, like git's 8-char abbreviation. */
/* Returns pointer into h.hex — no allocation. */
const char *hash_short(const Hash *h);  /* first 8 chars */
```

---

## Layer 3: objects.h — The Four Object Types

Direct mapping to git's object model. Each struct is what lives in C memory.
Each `put_*` function writes to DuckDB and returns the hash. `put_*` is
idempotent: if the hash already exists in the table, it is a no-op.

```c
/* objects.h */

/* ── Blob ─────────────────────────────────────────────────────────────────── */
/* Raw content. kind='sql': data is canonical SQL text (stored in DB).        */
/* kind='column': data is NULL; the Arrow buffer lives on disk by hash.       */

typedef enum { BLOB_SQL, BLOB_COLUMN } BlobKind;

typedef struct {
    Hash      hash;
    BlobKind  kind;
    char     *data;       /* heap-allocated; NULL for BLOB_COLUMN */
    int64_t   byte_count;
} Blob;

Hash blob_put_sql(Db *db, const char *canonical_sql);
Hash blob_put_column(Db *db, Hash arrow_buffer_hash, int64_t byte_count);
Blob blob_get(Db *db, Hash hash);
void blob_free(Blob *b);

/* ── Tree ─────────────────────────────────────────────────────────────────── */
/* Schema snapshot. Entries sorted by col_name for canonical hashing.         */

typedef struct {
    char  col_name[256];
    char  dtype[64];       /* "INT64", "VARCHAR", "DOUBLE", etc. */
    Hash  blob_hash;
    int   col_index;
} TreeEntry;

typedef struct {
    Hash        hash;
    TreeEntry  *entries;
    int         n_entries;
} Tree;

/* Compute tree hash from entries, write to dodo.trees. Returns tree hash. */
Hash tree_put(Db *db, TreeEntry *entries, int n);
Tree tree_get(Db *db, Hash hash);
void tree_free(Tree *t);

/* Derive a Tree from a live DuckDB query result (reads column names+types). */
/* blob_hashes must be pre-computed for each column; pass NULL to use zeros. */
Tree tree_from_result(Db *db, duckdb_result *r, Hash *blob_hashes);

/* ── Commit ───────────────────────────────────────────────────────────────── */

typedef struct {
    Hash     hash;
    Hash     parent;        /* hash_zero() for root commits (file load) */
    Hash     sql_hash;      /* blob hash of canonical SQL; hash_zero() for root */
    Hash     input_tree;    /* tree hash before transform */
    Hash     output_tree;   /* tree hash after transform */
    char     message[512];
    int64_t  duration_ms;
    int64_t  row_count;
    int64_t  created_at;   /* Unix timestamp */
} Commit;

Hash   commit_put(Db *db, Commit *c);
Commit commit_get(Db *db, Hash hash);

/* Walk parent chain. Calls fn(commit, userdata) until fn returns 0 or root. */
void commit_walk(Db *db, Hash from, int (*fn)(Commit, void*), void *userdata);

/* Return the hash N steps up the parent chain. Returns hash_zero if N > depth. */
Hash commit_ancestor(Db *db, Hash from, int n);

/* ── Tag ──────────────────────────────────────────────────────────────────── */

typedef struct {
    char name[256];
    Hash target;      /* commit hash */
    char message[512];
    int64_t created_at;
} Tag;

void tag_put(Db *db, Tag *t);   /* INSERT OR REPLACE */
Tag  tag_get(Db *db, const char *name);
int  tag_exists(Db *db, const char *name);
```

---

## Layer 4: refs.h — Mutable Pointers

HEAD, branches, and the branch stack for `/btw` / `/back`.

```c
/* refs.h */

/* ── Branch ───────────────────────────────────────────────────────────────── */

void branch_create(Db *db, const char *name, Hash target);
void branch_advance(Db *db, const char *name, Hash target);
Hash branch_resolve(Db *db, const char *name);  /* exits if not found */
int  branch_exists(Db *db, const char *name);
void branch_delete(Db *db, const char *name);

/* ── HEAD ─────────────────────────────────────────────────────────────────── */
/* HEAD row always exists (id=1). One of branch_name or commit_hash is set.   */

typedef struct {
    char branch_name[256];  /* empty string if detached */
    Hash commit_hash;       /* hash_zero if attached (resolve via branch) */
    int  is_detached;
} Head;

Head head_get(Db *db);
Hash head_resolve(Db *db);               /* → actual commit hash */
void head_set_branch(Db *db, const char *branch_name);
void head_set_detached(Db *db, Hash commit_hash);
void head_checkout(Db *db, const char *branch_name);  /* switches + sets HEAD */

/* True if HEAD's commit has any children (i.e. undo was used and a new       */
/* commit would diverge). Used to trigger auto-branch before commit.          */
int  head_has_children(Db *db);

/* ── Branch Stack (/btw, /back) ──────────────────────────────────────────── */

typedef struct {
    int   stack_depth;
    char  branch_name[256];
    Hash  head_at_push;
    int64_t pushed_at;
} BranchFrame;

void branch_stack_push(Db *db, const char *new_branch_name);
void branch_stack_pop(Db *db);   /* restores HEAD to top frame, removes row */
int  branch_stack_empty(Db *db);
BranchFrame branch_stack_top(Db *db);
```

---

## Layer 5: repo.h — Aggregate Operations

The Repo owns a Db and exposes combined operations that span multiple tables.
This is the only layer above it that should call into objects.h and refs.h.

```c
/* repo.h */

typedef struct {
    Db db;
} Repo;

/* Open or create a dodo repo. Runs schema.sql + views.sql + macros.sql      */
/* if tables don't exist yet. path=NULL for in-memory.                        */
Repo repo_open(const char *path);
void repo_close(Repo *r);

/* Core commit operation. Hashes sql, inserts blob + commit, advances HEAD.  */
/* Auto-branches if head_has_children() is true before committing.           */
/* Returns new commit hash. */
Hash repo_commit(Repo *r,
                 const char *canonical_sql,
                 Hash input_tree,
                 Hash output_tree,
                 int64_t duration_ms,
                 int64_t row_count,
                 const char *message);

/* Root commit for a file load. No parent, no SQL. */
Hash repo_commit_root(Repo *r, Hash output_tree,
                      const char *message, int64_t row_count);

/* Move HEAD back n steps. Old commits dangle (not deleted). */
void repo_reset(Repo *r, int n);

/* Create a new branch at HEAD and check it out. */
void repo_branch(Repo *r, const char *name);

/* Switch to an existing branch or tag by name, or a commit hash prefix. */
void repo_checkout(Repo *r, const char *name_or_hash);

/* Create annotated tag at HEAD. */
void repo_tag(Repo *r, const char *name, const char *message);

/* /btw: push current branch, create new branch from HEAD (or from scratch). */
void repo_btw(Repo *r, const char *new_branch);  /* new_branch may be NULL → auto-named */

/* /back: pop branch stack, restore HEAD. */
void repo_back(Repo *r);

/* Auto-generate a branch name when user diverges without naming. */
/* Returns heap-allocated string, caller frees. */
char *repo_auto_branch_name(Repo *r);

/* Column-level diff between two commit hashes (via their output_trees).     */
/* Prints to stdout. For programmatic use, query dodo.tree_diff directly.    */
void repo_diff(Repo *r, Hash commit_a, Hash commit_b);

/* Print git-log style history from HEAD. */
void repo_log(Repo *r, int max_entries);

/* Print branch overview (dodo.branch_status). */
void repo_branches(Repo *r);
```

---

## Layer 6: session.h — REPL Session State

Owns the Repo and DuckDB's live query state (CTE registry, HEAD view).
This is the layer the REPL talks to.

```c
/* session.h */

#define SESSION_DEFAULT_LIMIT 20

typedef struct {
    Repo  repo;
    int   display_limit;
    int   display_offset;
    /* CTE registry is stored in dodo.session_ctes (TEMP TABLE).            */
    /* The __head__ VIEW is always kept up to date in DuckDB.               */
} Session;

Session session_open(const char *db_path);   /* NULL = in-memory */
void    session_close(Session *s);

/* ── Core user actions ───────────────────────────────────────────────────── */

/* Parse, canonicalize, execute SQL. Commits result. Refreshes __head__.     */
/* Returns new commit hash. Prints result preview to stdout.                 */
Hash session_run(Session *s, const char *sql);

/* Load a parquet or CSV file as a root commit. */
Hash session_load(Session *s, const char *file_path);

void session_undo(Session *s, int n);
void session_branch(Session *s, const char *name);
void session_switch(Session *s, const char *name);
void session_tag(Session *s, const char *name, const char *message);
void session_btw(Session *s, const char *branch);
void session_back(Session *s);

/* ── Display ─────────────────────────────────────────────────────────────── */

void session_set_limit(Session *s, int n);
void session_set_offset(Session *s, int n);
void session_show(Session *s);        /* print current __head__ to stdout */

/* ── CTE management ──────────────────────────────────────────────────────── */

/* Ensure a CTE named df_<hash8> exists for this commit.                     */
/* Builds the full CTE chain back to root if needed.                         */
/* Returns the CTE name (points into internal buffer, do not free).          */
const char *session_ensure_cte(Session *s, Hash commit_hash);

/* Repoint __head__ VIEW to the current HEAD's CTE.                         */
void session_refresh_head(Session *s);

/* ── SQL canonicalization ────────────────────────────────────────────────── */

/* Run EXPLAIN on the SQL to get DuckDB's canonical parse tree string.       */
/* Returns heap-allocated string, caller frees. NULL on parse error.         */
char *session_canonicalize_sql(Session *s, const char *sql);

/* ── Export ──────────────────────────────────────────────────────────────── */

/* Walk DAG from HEAD (or given hash), emit a self-contained SQL script.     */
/* Script is a chain of CTEs + a final SELECT, with dodo hash comments.      */
/* Returns heap-allocated string, caller frees.                              */
char *session_export_sql(Session *s, Hash target);
void  session_export_sql_print(Session *s);

/* Flush in-memory DuckDB to a file (only useful in :memory: mode). */
void session_flush(Session *s, const char *path);
```

---

## Layer 7: repl.h — Command Parser and Dispatch

Parses lines typed by the user. SQL goes to `session_run()`. Slash commands
dispatch to the appropriate session function.

```c
/* repl.h */

/* Commands:
 *   /undo [n]             → session_undo(n)
 *   /branch <name>        → session_branch(name)
 *   /switch <name>        → session_switch(name)
 *   /tag <name> [message] → session_tag(name, message)
 *   /btw [name]           → session_btw(name)
 *   /back                 → session_back()
 *   /log [n]              → repo_log(n)
 *   /diff [hash|tag]      → repo_diff(HEAD, target)
 *   /branches             → repo_branches()
 *   /show                 → session_show()
 *   /limit <n>            → session_set_limit(n)
 *   /offset <n>           → session_set_offset(n)
 *   /load <path>          → session_load(path)
 *   /export               → session_export_sql_print()
 *   /save <path>          → session_flush(path)
 *   /quit or /exit        → clean shutdown
 *   anything else         → session_run(line) as SQL
 */

void repl_run(Session *s);         /* blocking REPL loop */
int  repl_dispatch(Session *s, const char *line);  /* 0 = quit, 1 = continue */
```

---

## DuckDB Schema

Run once on `repo_open()`. Split across three files in `sql/`.

### sql/schema.sql

```sql
CREATE SCHEMA IF NOT EXISTS dodo;

CREATE TABLE IF NOT EXISTS dodo.blobs (
    hash        VARCHAR PRIMARY KEY,
    kind        VARCHAR NOT NULL CHECK (kind IN ('sql', 'column')),
    data        VARCHAR,
    byte_count  BIGINT,
    created_at  TIMESTAMPTZ DEFAULT now()
);

CREATE TABLE IF NOT EXISTS dodo.trees (
    tree_hash   VARCHAR NOT NULL,
    col_name    VARCHAR NOT NULL,
    dtype       VARCHAR NOT NULL,
    blob_hash   VARCHAR NOT NULL REFERENCES dodo.blobs(hash),
    col_index   INTEGER NOT NULL,
    PRIMARY KEY (tree_hash, col_name)
);

CREATE TABLE IF NOT EXISTS dodo.commits (
    hash        VARCHAR PRIMARY KEY,
    parent      VARCHAR REFERENCES dodo.commits(hash),
    sql_hash    VARCHAR REFERENCES dodo.blobs(hash),
    input_tree  VARCHAR NOT NULL,
    output_tree VARCHAR NOT NULL,
    message     VARCHAR,
    duration_ms BIGINT,
    row_count   BIGINT,
    created_at  TIMESTAMPTZ DEFAULT now()
);

CREATE TABLE IF NOT EXISTS dodo.tags (
    name        VARCHAR PRIMARY KEY,
    target      VARCHAR NOT NULL REFERENCES dodo.commits(hash),
    message     VARCHAR,
    created_at  TIMESTAMPTZ DEFAULT now()
);

CREATE TABLE IF NOT EXISTS dodo.branches (
    name        VARCHAR PRIMARY KEY,
    target      VARCHAR NOT NULL REFERENCES dodo.commits(hash),
    created_at  TIMESTAMPTZ DEFAULT now(),
    updated_at  TIMESTAMPTZ DEFAULT now()
);

CREATE TABLE IF NOT EXISTS dodo.head (
    id            INTEGER PRIMARY KEY DEFAULT 1,
    branch_name   VARCHAR REFERENCES dodo.branches(name),
    commit_hash   VARCHAR REFERENCES dodo.commits(hash),
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

-- Session-local: CTE registry and display params.
-- TEMP tables are per-connection, auto-dropped on disconnect.
CREATE TEMPORARY TABLE IF NOT EXISTS dodo_session_ctes (
    commit_hash  VARCHAR PRIMARY KEY,
    cte_name     VARCHAR NOT NULL UNIQUE,
    registered_at TIMESTAMPTZ DEFAULT now()
);

CREATE TEMPORARY TABLE IF NOT EXISTS dodo_session_display (
    id       INTEGER PRIMARY KEY DEFAULT 1,
    lim      INTEGER DEFAULT 20,
    offset_  INTEGER DEFAULT 0
);

INSERT OR IGNORE INTO dodo_session_display VALUES (1, 20, 0);
```

### sql/views.sql

```sql
-- Resolved HEAD: actual commit hash regardless of attached/detached state.
CREATE OR REPLACE VIEW dodo.head_commit AS
SELECT
    COALESCE(b.target, h.commit_hash) AS commit_hash,
    h.branch_name,
    h.commit_hash IS NOT NULL          AS is_detached
FROM dodo.head h
LEFT JOIN dodo.branches b ON b.name = h.branch_name;

-- git log: recursive walk from HEAD.
CREATE OR REPLACE VIEW dodo.log AS
WITH RECURSIVE chain AS (
    SELECT c.hash, c.parent, c.message, c.duration_ms,
           c.row_count, c.created_at, c.output_tree, c.sql_hash, 0 AS depth
    FROM dodo.commits c
    JOIN dodo.head_commit h ON h.commit_hash = c.hash
    UNION ALL
    SELECT c.hash, c.parent, c.message, c.duration_ms,
           c.row_count, c.created_at, c.output_tree, c.sql_hash, chain.depth + 1
    FROM dodo.commits c
    JOIN chain ON chain.parent = c.hash
)
SELECT chain.*, b.data AS sql_text, t.name AS tag_name
FROM chain
LEFT JOIN dodo.blobs b ON b.hash = chain.sql_hash
LEFT JOIN dodo.tags  t ON t.target = chain.hash
ORDER BY depth;

-- Reverse parent index: used to detect dangling commits.
CREATE OR REPLACE VIEW dodo.children AS
SELECT parent AS commit_hash, hash AS child_hash
FROM dodo.commits WHERE parent IS NOT NULL;

-- Column-level diff between any two trees.
-- Filter: WHERE tree_a = ? AND tree_b = ?
CREATE OR REPLACE VIEW dodo.tree_diff AS
SELECT
    COALESCE(a.tree_hash, b.tree_hash) AS tree_a,
    COALESCE(b.tree_hash, a.tree_hash) AS tree_b,
    COALESCE(a.col_name,  b.col_name)  AS col_name,
    CASE
        WHEN a.col_name  IS NULL        THEN 'added'
        WHEN b.col_name  IS NULL        THEN 'removed'
        WHEN a.blob_hash != b.blob_hash THEN 'changed'
    END AS change_kind,
    a.blob_hash AS blob_before,
    b.blob_hash AS blob_after
FROM      dodo.trees a
FULL JOIN dodo.trees b ON a.col_name = b.col_name
WHERE a.blob_hash IS DISTINCT FROM b.blob_hash;

-- Branch overview.
CREATE OR REPLACE VIEW dodo.branch_status AS
SELECT
    br.name,
    br.target,
    c.created_at  AS last_commit_at,
    c.message     AS last_message,
    c.row_count,
    br.name = h.branch_name AS is_head
FROM dodo.branches br
JOIN dodo.commits c ON c.hash = br.target
JOIN dodo.head    h ON TRUE
ORDER BY is_head DESC, last_commit_at DESC;
```

### sql/macros.sql

```sql
CREATE OR REPLACE MACRO dodo.resolve_head() AS (
    SELECT commit_hash FROM dodo.head_commit
);

CREATE OR REPLACE MACRO dodo.head_has_children() AS (
    SELECT EXISTS (
        SELECT 1 FROM dodo.children
        WHERE commit_hash = (SELECT commit_hash FROM dodo.head_commit)
    )
);

CREATE OR REPLACE MACRO dodo.ancestor(start_hash VARCHAR, n INTEGER) AS (
    WITH RECURSIVE walk AS (
        SELECT hash, parent, 0 AS step FROM dodo.commits WHERE hash = start_hash
        UNION ALL
        SELECT c.hash, c.parent, walk.step + 1
        FROM dodo.commits c JOIN walk ON walk.parent = c.hash
        WHERE walk.step < n
    )
    SELECT hash FROM walk ORDER BY step DESC LIMIT 1
);
```

---

## Data Flow: session_run()

```
user types SQL
    │
    ▼
session_canonicalize_sql()          -- EXPLAIN → canonical form → hash
    │
    ▼
head_has_children()?
    YES → repo_auto_branch_name() → branch_create() → head_set_branch()
    │
    ▼
get current HEAD tree hash          -- this is input_tree
    │
    ▼
session_ensure_cte(HEAD)            -- build CTE chain if not cached
    │
    ▼
execute SQL against head CTE        -- measure duration, row_count
    │
    ▼
tree_from_result()                  -- derive output Tree from result schema
    │
    ▼
blob_put_column() per column        -- hash Arrow buffers (or defer)
    │
    ▼
tree_put(output_entries)            -- write Tree to dodo.trees
    │
    ▼
blob_put_sql(canonical_sql)         -- write SQL blob to dodo.blobs
    │
    ▼
repo_commit(sql_hash, input_tree,   -- write Commit, advance branch
            output_tree, ...)
    │
    ▼
session_ensure_cte(new_commit)      -- register new CTE
    │
    ▼
session_refresh_head()              -- repoint __head__ VIEW
    │
    ▼
session_show()                      -- print LIMIT/OFFSET preview
```

---

## CTE Chain Construction

Given a commit chain `root → c1 → c2 → HEAD`, the CTE chain is:

```sql
CREATE OR REPLACE VIEW __head__ AS
WITH
  df_<root8> AS (SELECT * FROM read_parquet('<path>')),
  df_<c1_8>  AS (<user sql 1 referencing df_<root8>>),
  df_<c2_8>  AS (<user sql 2 referencing df_<c1_8>>)
SELECT * FROM df_<c2_8>
LIMIT 20 OFFSET 0;
```

User SQL references the current HEAD by the name `df` (or the short hash).
`session_run()` rewrites the user's SQL to substitute the correct CTE name
before executing.

CTEs are accumulated in `dodo_session_ctes` during the session. Once built,
they are never dropped. Branch switching is O(1): just repoint `__head__`
to a different already-registered CTE. If a CTE is not yet registered
(e.g. after loading a saved session), `session_ensure_cte()` rebuilds the
chain by walking commit parents.

---

## Column Hashing Strategy

Column hashes are computed lazily to avoid blocking the REPL:

- On `session_load()`: hash all columns immediately (file is small or the
  cost is paid once at load time).
- On `session_run()`: record `hash_zero()` as the blob hash for new columns.
  The tree is still written and the commit is valid — the hash just doesn't
  cover column data yet.
- On `/tag` or `/save`: flush deferred hashes (read the DuckDB result,
  compute Arrow IPC buffer hash, update `dodo.blobs`).

This keeps the REPL responsive while ensuring tagged checkpoints have full
Merkle integrity.

---

## SQL Canonicalization

```c
char *session_canonicalize_sql(Session *s, const char *sql) {
    /* Prepend EXPLAIN, run against DuckDB, read first column of first row. */
    /* DuckDB's EXPLAIN output is the logical plan string — stable for the  */
    /* same query regardless of whitespace or alias differences.            */
    /* Hash that string to get sql_hash.                                    */
}
```

`EXPLAIN` is cheap (no execution). The plan string is deterministic for
semantically identical queries. This means `SELECT a, b FROM df` and
`SELECT  a,b  FROM df` hash identically. Users can also store the raw
user-typed SQL in the `message` field for display purposes.

---

## Build

```cmake
cmake_minimum_required(VERSION 3.20)
project(dodo C)
set(CMAKE_C_STANDARD 11)

find_library(DUCKDB_LIB duckdb REQUIRED)
find_path(DUCKDB_INCLUDE duckdb.h REQUIRED)

add_executable(dodo
    src/main.c
    src/db.c
    src/hash.c
    src/objects.c
    src/refs.c
    src/repo.c
    src/session.c
    src/repl.c
)

target_include_directories(dodo PRIVATE ${DUCKDB_INCLUDE} src)
target_link_libraries(dodo PRIVATE ${DUCKDB_LIB} ssl crypto)
```

SHA-256 via OpenSSL (`EVP_DigestUpdate`). No other external dependencies
beyond DuckDB and OpenSSL.

---

## Implementation Order

Build and test each layer before the next. Each layer only depends on layers
above it in this list.

1. **hash.c** — unit test: hash a known string, check hex output
2. **db.c** — unit test: open `:memory:`, exec DDL, appender round-trip, chunk read
3. **schema.sql / views.sql / macros.sql** — load into test DB, verify tables exist
4. **objects.c** — unit test: put/get blob, tree, commit, tag; verify idempotency
5. **refs.c** — unit test: branch create/advance, HEAD attach/detach, stack push/pop
6. **repo.c** — unit test: commit sequence, reset, auto-branch, diff, log
7. **session.c** — integration test: load a parquet, run 3 SQL transforms, undo 1,
   run alternative, check CTE names, export SQL
8. **repl.c + main.c** — manual smoke test in terminal

---

## Non-Goals (explicitly deferred)

- Multi-input joins across two independent df lineages (commit_parents table
  is in the schema but not wired in session_run — add later)
- Arrow buffer hashing on every commit (deferred until /tag or /save)
- Remote sync / shared repos
- Full SQL parser (use EXPLAIN canonicalization, not a custom parser)
- GUI / TUI (REPL only for now; IDE layer is a later consumer of session.h)