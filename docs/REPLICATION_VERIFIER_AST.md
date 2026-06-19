# dodo dna — Core Compiler AST for the Replication Verifier

> What should the dodo core compiler emit so that the Replication Verifier
> (claim verification by static analysis) can be built on top of it?

This document answers one question: **what AST/IR should `dodo` core produce**,
and **where in the existing compiler does that emission hook in**. It is scoped
to the deterministic layer only (steps 1–6 in the design doc's cost table). The
LLM layers consume the artifacts defined here; they are out of scope.

## Naming

The module that emits this AST is **dodo dna** — it sequences each variable and
result back through its ancestors to the raw source data. "dna" is the brand and
the user-facing CLI verb (`dodoc --dna`); the verdict layer that consumes it is
the **Replication Verifier** product. *Lineage* and *provenance* are the
underlying technical concepts (and the PROV-O serialization vocabulary), used in
this doc and the code, but never on the buyer-facing surface — the audience
doesn't speak that language.

---

## 1. What the compiler does today

The current pipeline is a **single-pass, side-effecting CTE-chain builder**:

```
ProcessLines(reader, state, skip_terminal)         src/core/dodo_core.cpp:3746
  ├─ strips comments (//, *, /* */), joins /// continuations
  ├─ ExpandMacros(line, state)        ← eager macro substitution (line 618)
  ├─ foreach/forvalues loop unrolling
  └─ process_command(trimmed)
        ├─ TokenizeCommand(query)     → DodoCommand {command, arguments,
        │                                condition, options, bysort_*}  (886)
        └─ ProcessCommand(cmd, state) → mutates DodoState, returns SQL   (1544)
```

`DodoState` (`src/core/dodo_core.hpp:39`) is the only thing that survives between
commands. What it tracks:

| Tracked today                              | Field                                |
|--------------------------------------------|--------------------------------------|
| CTE step SQL + original command text       | `cte_steps`, `cte_commands`          |
| Step counter (≈ a rowset version)          | `step_counter`                       |
| Current source file                        | `current_source`                     |
| Macros: local / global / scalar            | `local_symbols`, `global_symbols`, `scalar_symbols` |
| Variable + value labels                    | `variable_labels`, `value_label_defs`, `column_labels` |
| Panel/time keys                            | `panel_var`, `time_var`              |
| tempvars / tempfiles / preserve checkpoint | `tempvar_columns`, `tempfile_*`, `preserve_checkpoint` |

Each handler in `ProcessCommand` already knows, structurally, **exactly the facts
the lineage log needs** — `generate` (2562) knows the target var and the RHS
expression; `merge` (2796) knows the keys, the using file, and whether `_merge`
is created; `collapse`, `egen`, `sort`, `keep/drop` all parse their targets and
sources. We are not re-parsing Stata; we are **tapping facts the parser already
computes** and serializing them.

### The four gaps

What the verifier needs that the compiler does **not** produce today:

1. **No location.** Lines are read off a `LineReader` with no line counter;
   `DodoCommand` has no `loc`. The lineage DSL is keyed entirely on `script:line`.
2. **No dataframe symbol table.** `DodoState` tracks *macros*, but the columns
   living in the frame (`main.wage`, `main.lnwage`) are never modeled. SQL column
   resolution is delegated wholesale to DuckDB. The product's symbol table and
   the `frame.variable` namespace do not exist yet.
3. **No certainty / lossy macro handling.** `ExpandMacros` (618) substitutes
   eagerly and, on an unknown local, **expands to the empty string** (line 696).
   A runtime-unknown macro in a regression spec is exactly the `~out` / `unknown`
   case the verifier must flag — but that information is currently destroyed
   before any handler sees it.
4. **No terminal statistical nodes.** `regress`, `ivregress`, `xtreg`, `outreg2`
   are not dodo commands at all (`DODO_COMMANDS`, 834). These are the ~12% of
   terminal ops and the *single most important* node type for claim verification
   (`emit`). They need to be recognized and emitted, even though they produce no
   SQL.

The plan below closes those four gaps and defines the emitted AST.

---

## 2. The emission target

The compiler emits **three coordinated artifacts** per do-file (or per package).
All three are derivable in one pass; none requires data.

```
                     ┌─────────────────────────────────────┐
                     │   dodoc --dna                        │
   .do file(s) ────► │   (existing parse, instrumented)     │
                     └───────────────┬─────────────────────┘
                                     │
        ┌────────────────────────────┼────────────────────────────┐
        ▼                            ▼                            ▼
  ① dna.jsonl                ② symbols.jsonl              ③ clean.sql
  (one node per stmt,        (one row per symbol,          (88% data-transform
   the AST proper)            the resolved env)             commands, already
                                                            emitted today)
        │                            │                            │
        └──────────► openlineage / PROV-O / DuckDB ◄──────────────┘
                     openlineage-sql consumes ③ for column lineage
```

The **AST is artifact ①** — a stream of typed lineage nodes. Artifacts ② and ③
are projections/byproducts of the same walk. The lineage log and symbol table in
the design doc are the *human-readable renderings*; JSON-LD is the on-disk form.

### 2.1 Node schema (artifact ①)

One JSON object per emitted statement. This is a direct, typed serialization of
the six-column lineage DSL plus the structured payload each verb needs.

```jsonc
{
  "verb":   "assign",                       // load|merge|assign|scalar|filter|
                                            //   reorder|lag|byop|emit
  "loc":    { "script": "table3", "line": 47 },
  "cert":   "resolved",                     // resolved | abbrev | unknown
  "targets": ["main.lnwage"],               // frame.var | scalar | RS#N | >file
  "sources": ["main.wage"],
  "rowset_in":  "RS#2",                     // rowset consumed (across-row ops)
  "rowset_out": null,                       // rowset produced (new version)
  "expression": "gen lnwage = log(wage)",   // raw, comment-stripped, NOT macro-expanded
  "sql_ref":    "_s4",                       // CTE step this node corresponds to (links to ③)
  "payload":  { /* verb-specific, see §4 */ },
  "warnings": ["log of variable with unknown sign"]
}
```

Field rationale:
- `loc` is the only identity. **No node IDs** — matches the design rule.
- `expression` is the cleaned-but-unexpanded text. Critically, this is
  pre-`ExpandMacros` (see §3.2), so `egen \`out' = rowmean(\`controls')` survives
  verbatim and is taggable as `unknown`.
- `sql_ref` is the bridge to `clean.sql`. It lets openlineage-sql's column
  lineage be joined back to the statement that produced it — the design doc's
  "custom nodes referencing SQL outputs."
- `payload` carries the structured, verb-specific detail an LLM needs without
  re-parsing Stata (e.g. for `emit`: the estimator, dependent var, regressors,
  vce). This is the part that makes claim verification a lookup, not an inference.

### 2.2 Symbol schema (artifact ②)

One object per unique symbol, mirroring the seven-column symbol table:

```jsonc
{
  "kind":  "variable",            // variable|scalar|local|global|rowset|table|output
  "name":  "main.lnwage",
  "declared":   { "script": "table3", "line": 47 },
  "first_seen": { "script": "table3", "line": 47 },
  "dtype": "float",
  "dtype_cert": "inferred",       // known | inferred | unknown | conflict
  "note":  "log(float)->float"
}
```

### 2.3 SQL (artifact ③)

**Already produced.** `state.BuildQuery(...)` and the per-step CTEs are exactly
the clean SQL openlineage-sql ingests for free column lineage. The only change is
to **tag each CTE with its `sql_ref`** so column lineage rejoins the AST (the
`--annotate` comment machinery in `dodoc.cpp:132` is the seam for this).

---

## 3. Compiler changes to produce the AST

Ordered by dependency. Each is small and local; none rewrites the engine.

### 3.1 Thread location through the parser  *(unblocks everything)*

- Add `int line` and `std::string script` to `DodoCommand`
  (`dodo_core.hpp:27`).
- `ProcessLines` (3746) already owns the read loop — add a `line_no` counter
  incremented per `reader()` call, and a `state.current_script` set by the CLI /
  `do` handler. Stamp both onto `pending_command` context.
- Continuation lines (`///`) and loop unrolling: record the **physical line of
  the statement head** so unrolled `foreach` bodies map back to their source line
  (the design doc keys everything on `script:line`, and one source line may emit
  many nodes — that is fine, nodes share a `loc`).

### 3.2 Make macro expansion lineage-aware  *(closes gap #3)*

`ExpandMacros` (618) is currently lossy. Two minimal changes:

- Capture the **raw statement** before expansion and store it as the node's
  `expression`. (Today only the post-expansion `trimmed` survives.)
- When a local/global is **not found**, instead of silently emitting `""`,
  record an `unknown` marker for that span so the handler can set
  `cert = "unknown"` and emit the `~out` symbol notation. A thin
  `ExpandResult { text, certainty, unresolved[] }` return type carries this
  without changing call sites that ignore it.

This is the difference between the verifier saying *"macro expanded at runtime in
regression spec"* (a real deterministic warning in the design doc) and silently
dropping the regressor.

### 3.3 Build the dataframe symbol table  *(closes gap #2)*

Add to `DodoState`:

```cpp
struct VarSymbol { std::string frame, name, dtype, dtype_cert, note;
                   Loc declared, first_seen; };
std::map<std::string, VarSymbol> frame_vars;     // key "frame.var"
std::vector<LineageNode> lineage;                // artifact ①, accumulates
int rowset_version = 1;                          // RS#N, distinct from step_counter
```

Population is **incremental, inside the handlers that already parse the facts**:

- `use` / `import` (1620): seed `frame_vars` from the source. For `.dta` we
  already open a `DtaReader` and read columns/labels (1649) — extend that to
  populate `frame_vars` with `dtype_cert = "known"`. For CSV/Parquet, columns
  are `unknown` until a `describe`/`codebook` resolves them.
- `generate` (2562) / `egen` (2617): add the LHS var; infer `dtype` from the RHS
  (`log(float)→float`) → `dtype_cert = "inferred"`; note expression.
- `merge` (2796): bump rowset, add using-file vars + `main._merge`
  (`variable` kind, per the rule that `_merge` etc. are tracked as variables).
  Honor the rule: **merge does not update existing vars** unless `update` present.
- `rename`, `drop`, `keep`, `collapse`, `reshape`: mutate `frame_vars` to match
  the SQL they already build.

Abbreviation handling (`?sal` / `cert=abbrev`): when a referenced name is a
unique prefix of one known var → resolve; when it matches several → emit
`abbrev` with the candidate list in `note`. The candidate set is exactly
`frame_vars` filtered by prefix.

### 3.4 Rowset versioning  *(design rule)*

`step_counter` ≈ rowset today but is incremented on *every* CTE step. The design
doc's rule is narrower: **a new rowset version only on `sort`, `merge`, `filter`
(drop/keep if), `collapse`, `append`, `reshape`, `duplicates`**, and a rowset
appears as a *source* only for across-row ops (`lag`, window/`by:`, cumulative
`sum()` outside egen). So:

- Keep `rowset_version` separate from `step_counter`.
- Bump it in exactly those handlers; record `rowset_out`.
- For `lag`/`byop`/window nodes, set `rowset_in` to the current version, with
  the `RS#N@script:line` provenance (the lag example in the design doc).

### 3.5 Recognize terminal statistical commands  *(closes gap #4)*

Add `regress`, `ivregress`, `xtreg`/`xtfe`, `areg`, `logit`/`probit`,
`tabulate`, `summarize`, `outreg2`, `esttab`/`estout` to a new **terminal-emit**
classification. These do **not** join `IsTransformationCommand` (no CTE step) and
do **not** need SQL. They emit a single `emit` node:

```jsonc
{
  "verb": "emit", "loc": {"script":"table3","line":112}, "cert":"resolved",
  "targets": [">table3.tex"],
  "sources": ["main.lnwage","main.edu","main.exp"],
  "expression": "regress lnwage edu exp, vce(cluster firmid)",
  "payload": {
    "estimator": "ols",
    "depvar": "main.lnwage",
    "regressors": ["main.edu","main.exp"],
    "vce": {"type":"cluster","by":"main.firmid"},
    "output": "table3.tex"
  }
}
```

`tabulate`/`summarize` already exist as side-effect handlers (3145, 3249) — wrap
their existing parse to *also* push an `emit` node. New estimators need a small
spec parser (depvar, regressors, `vce()`, `absorb()`, `if/in`). This parser is
~150 lines and is the highest-leverage new code, because **`emit` payloads are
what the smart LLM verifies specification claims against**.

### 3.6 Deterministic warnings

Emit at node-construction time, attached to `node.warnings[]`. Each maps to facts
already on hand:

| Warning                                   | Source of truth                          |
|-------------------------------------------|------------------------------------------|
| log of variable with unknown sign         | `assign` RHS is `log(x)` & `x.dtype_cert != known-nonneg` |
| merge on key with abbrev uncertainty      | `merge` key resolves to `cert=abbrev`    |
| variable used before assignment           | source var absent from `frame_vars`      |
| macro expanded at runtime in regression   | `emit` node with `cert=unknown` (§3.2)   |
| rowset modified between sort and lag       | `rowset_version` changed between a `reorder` and a dependent `lag` |

---

## 4. Verb ↔ command mapping

The walk classifies every recognized statement into one of nine verbs. Mapping
to existing handlers:

| Verb      | Stata / dodo commands                          | Handler (line)        | New rowset? |
|-----------|------------------------------------------------|-----------------------|-------------|
| `load`    | `use`, `import`                                | 1620, 1938            | yes (RS#1…) |
| `merge`   | `merge`, `append`                              | 2796, 3322            | yes         |
| `assign`  | `generate`, `replace`, `egen` (within-row)     | 2562, 2581, 2617      | no          |
| `scalar`  | `scalar`                                       | 2232                  | no          |
| `filter`  | `drop if`, `keep if`                           | 2490, 2452            | yes         |
| `reorder` | `sort` (`order` = column reorder, not rowset)  | 2600, 2776            | yes (sort)  |
| `lag`     | `L.`/`F.`/`S.`, cumulative `sum()` outside egen| in `TranslateExpression` (1480) | no |
| `byop`    | `by:` / `bysort` prefixed expressions          | 1555                  | depends     |
| `emit`    | `regress`, `tabulate`, `summarize`, `outreg2`… | 3145/3249 + **new**   | no          |

`lag`/`byop` are special: they are detected **inside expression translation**
(the `L.`→`LAG()` rewrite already lives at 1480–1503), so the node's `rowset_in`
and the `RS#N@loc` provenance must be set where that rewrite fires, not just at
the command level.

---

## 5. Where it plugs into `dodoc`

The CLI is the natural driver (`src/cli/dodoc.cpp`). Add flags alongside
`--annotate`:

- `--dna[=DIR]`        → emit the full dodo dna bundle: artifacts ① + ②, and
                         tag CTEs in ③ with `sql_ref`. This is the headline verb.
- `--symbols[=FILE]`   → artifact ② only.
- `--lineage[=FILE]`   → artifact ① only; quiet alias for tooling that wants just
                         the AST stream. Not advertised to end users.

Implementation: after `ProcessLines` returns, `state.lineage` and
`state.frame_vars` are fully populated; serialize them. SQL emission
(`dodoc.cpp:131`) is unchanged except for the `sql_ref` CTE tags. The extension
build can expose the same via a `dodo_dna()` table function later, but the
**CLI is sufficient for the product** — the verifier runs as a batch tool over a
replication package, not interactively.

A second pass over a whole package (multiple do-files sharing macro/scalar state)
is just calling `ProcessLines` repeatedly against one `DodoState`, which already
persists macros across `do` (handler at 1780) — so cross-file lineage
(*"which scripts use the firm-level merge?"*) falls out for free once `loc`
carries the script name.

---

## 6. Phased delivery

| Phase | Deliverable                                                        | Unblocks |
|-------|--------------------------------------------------------------------|----------|
| **1** | Location threading (§3.1) + `LineageNode` struct + `--lineage` emits `load/assign/filter/reorder` with `resolved` cert only | End-to-end pipeline on simple scripts; openlineage-sql on ③ |
| **2** | Dataframe symbol table (§3.3) + `--symbols`; dtype inference; abbrev resolution | Symbol table artifact; `abbrev` cert; "used before assignment" warning |
| **3** | Lineage-aware macros (§3.2) → `unknown` cert; rowset versioning (§3.4) for `lag`/`byop`/`merge` | Runtime-macro warning; across-row provenance |
| **4** | Terminal `emit` nodes + estimator spec parser (§3.5)               | **Specification-claim verification** — the core product |
| **5** | PROV-O / JSON-LD serialization + DuckDB sink; OpenLineage envelope | Journal-system integration, recursive-CTE provenance queries |

Phases 1–4 are the deterministic engine the LLM layers sit on. Phase 4 is the
revenue-critical one (specification + sample + construction claims are the three
*statically verifiable* rows of the claim taxonomy). Phase 5 is packaging for the
publisher/journal channel and can trail the first author-facing release.

---

## 7. Key principle

The compiler should treat lineage emission as a **non-destructive tap on facts it
already computes**, never a second parser. Every node field above is something a
`ProcessCommand` handler already has in a local variable at emit time. The work is
(a) giving statements a location, (b) keeping a column-level symbol table the SQL
layer currently delegates to DuckDB, (c) preserving—rather than discarding—macro
uncertainty, and (d) recognizing the terminal estimators that never needed SQL.
Everything else is serialization.
