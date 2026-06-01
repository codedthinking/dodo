# Variable Substitution — Macros, Scalars, and Stored Results

> Design for `local`, `global`, `scalar`, loops (`foreach`/`forvalues`), and
> stored results (`r()`, `e()`) in dodo. This is the plan for milestone **M14**.

## TL;DR — the one distinction that drives everything

dodo is a **compiler/transpiler**, not an interpreter like Stata. A `.do` file
becomes SQL once, and the SQL runs later (lazily, as a CTE chain). The standalone
`dodoc` front-end never touches DuckDB at all — it is pure text → SQL.

Because of that, every Stata "variable" splits into one of two categories, and the
two are handled by completely different machinery:

| | **Compile-time-known** | **Runtime-dependent** |
|---|---|---|
| Value is knowable | while compiling, from the source text alone | only after data is read and a query runs |
| Examples | `local years 2018 2019 2020`, `forvalues i = 1/10`, `foreach v of varlist x y z`, `global base "s3://bucket"`, `scalar pi = 3.14159` | `r(max)` after `summarize`, `r(N)` after `count`, `e(b)` after `regress`, `levelsof year, local(years)`, `keep if revenue == r(max)` |
| Mechanism | **textual substitution + loop unrolling** in a preprocessing pass | **compiled into the SQL** as a scalar subquery / set membership — never substituted as a literal value |
| Works in `dodoc` (no DuckDB)? | Yes, fully | Yes — the subquery is *emitted*; DuckDB evaluates it at run time |
| Requires executing a query mid-compile? | No | **No** (this is the whole point — see "Rejected: the round-trip" below) |

**The rule:** a runtime value such as `r(max)` is never resolved to a number by
dodo. It is rewritten into the generated SQL as a subquery, e.g.

```stata
summarize revenue
keep if revenue == r(max)
```

compiles to (schematically)

```sql
-- summarize records, but adds no CTE step; the chain's latest step is _s3
... WHERE revenue = (SELECT max(revenue) FROM _s3)
```

The lazy CTE chain stays intact, `dodoc` keeps working without a database, and the
value is computed exactly once, by DuckDB, at the moment the chain materializes.

---

## Background: how dodo compiles today

(See `docs/PLAN.md` for the full architecture.)

- Each transformation command appends one CTE step `_sN AS (SELECT … FROM _sN-1)`
  to a per-connection chain held in `DodoState`.
- Terminal commands (`list`, `summarize`, `count`, `tabulate`, …) prepend the
  full `WITH …` chain and add their own final `SELECT`. They **materialize but do
  not extend** the chain. In Stata these are exactly the commands that populate
  `r()`.
- `ProcessCommand(const DodoCommand&, DodoState&) -> std::string` is pure
  translation. The DuckDB extension hands the returned SQL to DuckDB; `dodoc`
  prints it. Neither path lets the compiler observe query *results*.

This last point is the hard constraint. Anything that needs a result value must be
expressed *as SQL*, not fetched and pasted back.

---

## Category A — Compile-time macros (textual substitution)

These behave like Stata's macro processor: pure text expansion that happens
**before** a command is tokenized. They never reach the data.

### A.1 Definitions

| Command | State effect | Notes |
|---|---|---|
| `local name text` | `locals[name] = "text"` | value is the literal remainder of the line |
| `local name = exp` | `locals[name] = fold(exp)` | `=` means "evaluate"; only **constant-foldable** expressions (numeric/string literals, arithmetic, `_N` is *not* constant — see A.4) |
| `global name text` | `globals[name] = "text"` | session-wide |
| `global name = exp` | `globals[name] = fold(exp)` | |
| `scalar name = exp` | constant → `scalars[name]` as literal | runtime case handled in Category B |

### A.2 References and the expansion pass

A preprocessing pass runs on each *logical* command line (after `///`
continuation joining, before `TokenizeCommand`) in this order:

1. **Globals** — `$name` and `${name}` → `globals[name]`.
2. **Locals** — `` `name' `` → `locals[name]`, expanded **innermost-first** so
   nested refs like `` `x`i'' `` work.
3. **Inline expression macros** — `` `=exp' `` → `fold(exp)` when constant-foldable.

Expansion is recursive (an expanded value may itself contain macro refs) with a
depth guard. Unknown locals expand to empty string (Stata semantics); unknown
globals likewise. Expansion happens inside double-quoted strings too, matching
Stata.

Runtime tokens (`r(...)`, `e(...)`, runtime scalars) are **deliberately left
untouched** by this pass — they are sentinels resolved later in Category B.

### A.3 Loops — unrolled at compile time

Loops are a programming construct, not a data command. The preprocessor reads the
loop header and `{ … }` body (possibly spanning lines) and emits the body once per
iteration, binding the loop local before each emission. The bound iterations feed
straight back through the macro-expansion pass.

| Header | Iterates over |
|---|---|
| `forvalues i = 1/10` | `1,2,…,10` |
| `forvalues i = 0(2)10` | `0,2,4,…,10` (start, step, stop) |
| `foreach v of varlist x y z` | literal varlist tokens |
| `foreach v in a b c` | literal list tokens |
| `foreach v of local mylist` | tokens of an **already-known** local |
| `foreach v of numlist 1 3 5` | literal numlist |
| `foreach v of global g` | tokens of a known global |

All of these are knowable from the source, so the loop is fully unrolled into
concrete commands. This is the only place loops are supported by the pure
compiler. **A loop over a runtime-populated list (e.g. the local that
`levelsof … local()` filled) cannot be unrolled** — see B.3.

### A.4 What is *not* compile-time

`local n = _N`, `local m = r(mean)`, `local lvls : levelsof …` and similar are
**not** constant-foldable, because the right-hand side depends on data. These are
Category B: the macro becomes an alias for a SQL fragment, not a literal. If a
non-foldable value is used in a position that requires a literal at compile time
(e.g. a loop bound, `forvalues i = 1/`n'` where `` `n' `` came from `_N`), the
compiler raises a clear error: *"loop bound depends on a runtime value; unroll is
impossible — rewrite set-based (see VARIABLE_SUBSTITUTION.md §B.3)."*

---

## Category B — Runtime-dependent values (compiled to `SET VARIABLE`)

These cannot be known while compiling. dodo compiles them to DuckDB's native
`SET VARIABLE` / `getvariable()` mechanism. **No execution round-trip needed** —
the emitted SQL is self-contained and dodoc-safe.

### B.0 The DuckDB mechanism: `SET VARIABLE` + `getvariable()`

DuckDB's `SET VARIABLE name = expr` evaluates `expr` **at execution time** and
stores the result in a session-scoped variable. Later, `getvariable('name')`
retrieves the stored value. Key properties:

- **Evaluated once at assignment.** The value is frozen when the `SET VARIABLE`
  statement runs. Later data changes do not affect it. This matches Stata's `r()`
  semantics: `r(max)` holds the value *as summarize saw it*.
- **Session-scoped.** Variables persist across statements within a connection,
  exactly like Stata scalars.
- **Composable.** `SET VARIABLE floor = getvariable('hi') - 1` works natively.
- **dodoc-safe.** `SET VARIABLE` and `getvariable()` are valid DuckDB SQL that
  dodoc emits and downstream consumers execute.
- **Can be set from subqueries.** `SET VARIABLE x = (SELECT max(col) FROM tbl)`
  evaluates the subquery and stores the scalar result.

### B.1 Stored results `r()` → `SET VARIABLE`

When the compiler processes a terminal command that populates `r()`, it emits
`SET VARIABLE` statements that capture each result from the CTE chain at that
point.

`summarize revenue` (latest step `_s3`) emits:

```sql
SET VARIABLE _r_N    = (SELECT count(revenue)       FROM _s3);
SET VARIABLE _r_mean = (SELECT avg(revenue)         FROM _s3);
SET VARIABLE _r_sum  = (SELECT sum(revenue)         FROM _s3);
SET VARIABLE _r_min  = (SELECT min(revenue)         FROM _s3);
SET VARIABLE _r_max  = (SELECT max(revenue)         FROM _s3);
SET VARIABLE _r_sd   = (SELECT stddev_samp(revenue) FROM _s3);
```

(The `_s3` CTE definition is included in the `WITH` prefix of the query that
contains these statements, so the subqueries resolve correctly.)

Likewise `count` emits `SET VARIABLE _r_N = (SELECT count(*) FROM _sK)`.

A later reference to `r(max)` in an expression is translated to
`getvariable('_r_max')`:

```stata
summarize revenue          // emits SET VARIABLE _r_max = ...
keep if revenue >= r(mean) // → WHERE revenue >= getvariable('_r_mean')
generate hi = revenue == r(max)  // → (revenue = getvariable('_r_max')) AS hi
```

**`r()` volatility.** Every new r-class command emits fresh `SET VARIABLE`
statements that overwrite the previous `_r_*` variables (matching Stata, where
`r()` is overwritten by the next r-class command). A reference to a stale
`r(...)` at compile time produces an error: *"r(max) is not set; the most recent
r-class command was `count`."*

### B.2 Scalars and macros bound to runtime values

```stata
summarize revenue
scalar hi = r(max)           // → SET VARIABLE hi = getvariable('_r_max')
scalar floor = hi - 1        // → SET VARIABLE floor = getvariable('hi') - 1
keep if revenue > floor      // → WHERE revenue > getvariable('floor')
```

The compiler tracks which names are `LITERAL` (compile-time text) vs `RUNTIME`
(backed by a DuckDB variable). The kind is determined at definition:

| | `LITERAL` | `RUNTIME` |
|---|---|---|
| stored as | raw text in compiler state | DuckDB variable via `SET VARIABLE` |
| resolved by | textual substitution (Category A) | `getvariable('name')` in generated SQL |
| set by | `local x text`, `scalar x = const` | RHS references `r()`, `_N`, or another RUNTIME name |

**Taint propagation.** If any part of the RHS is runtime-dependent, the whole
definition is `RUNTIME`, and the compiler emits `SET VARIABLE` with the
appropriate `getvariable()` references composed into the expression.

### B.3 Name resolution at a use site

When translating an expression, a bare identifier is looked up:

1. If it is a `RUNTIME` scalar → emit `getvariable('name')`
2. If it is a `LITERAL` scalar → text-substitute the value (Category A)
3. Otherwise → treat as a **column reference** (leave for DuckDB to resolve)

This means `keep if employment > min_employment` compiles to
`WHERE employment > getvariable('min_employment')` if `min_employment` was
defined as `scalar min_employment = r(min)`, but would be a column reference
if no such scalar exists.

### B.1 Stored results `r()` become SQL subqueries

When the compiler processes an r-class terminal command, it records, in state, a
map from each `r(...)` symbol to a **SQL subquery string** evaluated against the
**latest CTE step at that point** (the snapshot the command saw — matching Stata,
where `r()` holds the value as of the command).

`summarize revenue` (latest step `_s3`) records, schematically:

| Symbol | Recorded SQL fragment |
|---|---|
| `r(N)` | `(SELECT count(revenue) FROM _s3)` |
| `r(mean)` | `(SELECT avg(revenue) FROM _s3)` |
| `r(sum)` | `(SELECT sum(revenue) FROM _s3)` |
| `r(min)` | `(SELECT min(revenue) FROM _s3)` |
| `r(max)` | `(SELECT max(revenue) FROM _s3)` |
| `r(sd)` | `(SELECT stddev_samp(revenue) FROM _s3)` |
| `r(Var)` | `(SELECT var_samp(revenue) FROM _s3)` |

Likewise `count` records `r(N) ↦ (SELECT count(*) FROM _sK)`.

A later reference to `r(max)` (recognized during expression translation, not the
textual pass) is replaced by its recorded fragment:

```stata
summarize revenue          // records r(...) against _s3
generate hi = revenue == r(max)
keep if revenue >= r(mean)
```

→ each `r(...)` becomes its frozen subquery. Because `_s3` is an earlier link in
the same chain, any later step's `WITH` prefix still contains it, so the subquery
resolves correctly — and it references the data *as summarize saw it*, even if
later commands drop columns or rows.

**`r()` volatility.** Every new r-class command clears the recorded map before
recording its own (Stata's `r()` is overwritten by the next r-class command). A
reference to a stale `r(...)` is an error: *"r(max) is not set; the most recent
r-class command was `count`."*

### B.2 Scalars and macros bound to runtime values

`scalar hi = r(max)` or `local hi = r(max)` does not store a number — it records a
`RUNTIME` symbol (§B.0) whose value is the **SQL fragment** that `r(max)` currently
maps to, snapshotted at definition time. A later bare reference to `hi` (scalar) or
`` `hi' `` (macro) expands to that fragment. Scalars and macros thus share one
symbol-table mechanism; the only difference from Category A is the `kind` flag —
`RUNTIME` symbols carry a subquery, `LITERAL` symbols carry text — and the
resolution site (expression translation vs the textual pass).

### B.4 `levelsof` and set-based rewrites

`levelsof x, local(L)` produces a list whose **length and contents are unknown at
compile time**. It therefore cannot fill a compile-time local, and a
`foreach … of local L` over it cannot be unrolled. dodo handles the common idioms
by mapping them to **set-based SQL** instead of loops:

| Stata idiom | SQL rewrite |
|---|---|
| `levelsof x, local(L)` then `keep if inlist(y, `L')` | `… WHERE y IN (SELECT DISTINCT x FROM _sK)` |
| `levelsof x` used only for membership | subquery `IN (SELECT DISTINCT x FROM _sK)` |
| `foreach v of local L { gen d_`v' = x==`v' }` (one column per value) | `PIVOT`-style generation — **only if** the value set can be reified; otherwise rejected |
| `foreach … { append/stack }` | `GROUP BY` / `UNPIVOT` where expressible |

`levelsof x, local(L)` records `L` as a runtime-list symbol. The list subquery
`(SELECT DISTINCT x FROM _sK ORDER BY x)` is used inline for membership tests.
Loops over a runtime list that require iterating an unknown number of times are
**out of scope for the pure compiler** and produce an explicit error.

### B.4 `e()` (estimation results)

`e(...)` follows the same "compile to SQL fragment" rule as `r()`, but depends on
`regress`/`reghdfe`, which are themselves stretch goals (M9). Deferred; the
mechanism is identical to B.1 once estimation exists.

---

## Stretch goal: named result structs via `let`

`r(min)` is legacy: it is **anonymous** (one shared bucket), **volatile** (the next
r-class command wipes it), and **opaque** (you must know Stata's per-command result
list). We keep it for compatibility with existing `.do` files, but offer a
dodo-native alternative that is named, stable, and self-documenting.

### Syntax and why `let`

```stata
let result = summarize employment
keep if employment > result.min
generate z = (employment - result.mean) / result.sd
```

`let` binds the **stored results of a command to a named struct**, and the fields
(`result.min`, `result.mean`, `result.N`, …) are referenced by dotted access.

Why a keyword: dodo's parser dispatches on the **first token of every line** —
that is the invariant that lets the extension distinguish a dodo command from raw
SQL. A bare `result = summarize employment` would begin with an identifier and
break that invariant (and collide visually with SQL). `let` joins `DODO_COMMANDS`
like any other verb, so the line routes cleanly: the handler strips `let`, reads
`<name> =`, and runs the remainder as a captured command. This mirrors the
keyword-led design of `local`, `scalar`, `egen`, etc.

### Semantics — named, non-volatile SET VARIABLE namespace

`let NAME = <r-class command>` runs the command and emits `SET VARIABLE`
statements under a named prefix:

| `let result = summarize employment` (step `_s3`) | emitted SQL |
|---|---|
| `result.N` | `SET VARIABLE result_N = (SELECT count(employment) FROM _s3)` |
| `result.mean` | `SET VARIABLE result_mean = (SELECT avg(employment) FROM _s3)` |
| `result.min` / `result.max` | `SET VARIABLE result_min/max = (SELECT ...)` |
| `result.sd` | `SET VARIABLE result_sd = (SELECT stddev_samp(employment) FROM _s3)` |

`result.min` in an expression compiles to `getvariable('result_min')`.
Taint propagates normally — `scalar lo = result.min - 1` emits
`SET VARIABLE lo = getvariable('result_min') - 1`.

Two properties make it strictly better than `r()`:

1. **Not volatile.** A later `summarize`/`count` does **not** disturb `result`.
   You can hold several at once:
   ```stata
   let emp = summarize employment
   let rev = summarize revenue
   keep if employment > emp.min & revenue > rev.mean
   ```
   This is impossible with `r()`, where the second `summarize` overwrites the first.
2. **Validated fields.** `result.median` when `summarize` did not compute it is a
   **compile-time error** listing the available fields — no silent NULLs.

### Unifying `r()` with `let`

`r()` becomes the special case of an **implicit struct named `r`** that every
r-class command reassigns: `summarize x` is sugar for `let r = summarize x`, and
`r(min)` is just `r.min` with legacy `()` access. One recording mechanism backs
both surfaces; the only difference is that `r` is auto-reassigned (hence volatile)
while a user-named `let` struct is stable. This keeps the implementation single and
makes the legacy/modern relationship obvious in the docs.

### Scope, state, parity

- Named structs are tracked in compiler state as `RUNTIME` symbols with a
  `name.field` → DuckDB variable name mapping. The variables themselves live in
  DuckDB's session via `SET VARIABLE`.
- **Scope:** session-scoped like `scalar`/`global`, cleared by `clear`.
- **dodoc-safe.** `let` emits `SET VARIABLE` statements and `.field` access
  emits `getvariable()` — valid SQL that dodoc outputs and DuckDB executes.
- **Sequencing after M14b**, since it reuses the `SET VARIABLE` recording.

---

## Why not just run the query and paste the value back? (Rejected: the round-trip)

A tempting alternative: when the extension hits `summarize revenue`, actually
execute it, read `max`, and substitute the literal `42` into later commands.

We reject this:

1. **It breaks `dodoc`.** `dodoc` has no database and never executes anything. A
   round-trip would make the standalone compiler strictly less capable than the
   extension, splitting the language in two.
2. **It breaks laziness.** Materializing mid-script to read a scalar forces
   execution of the whole chain early, defeating the lazy-CTE design.
3. **It is impure.** Compilation would depend on live data, so the same `.do` file
   could compile to different SQL on different days.

Instead, we use DuckDB's `SET VARIABLE` / `getvariable()`:
- The compiler emits `SET VARIABLE _r_max = (SELECT max(x) FROM _sN)` — valid SQL
  that dodoc can output and DuckDB evaluates at runtime.
- The value is frozen at the point the `SET VARIABLE` executes, matching Stata's
  assignment-time semantics.
- No compiler round-trip needed — the SQL is self-contained and deterministic.

The only thing dodo *cannot* do is unroll a loop over a runtime list (B.4). That
is a deliberate, documented boundary, not a bug.

---

## State changes (`DodoState`)

### Compile-time state (in-memory, M14a — done)

```cpp
std::unordered_map<std::string, std::string> local_macros;   // `x' → text
std::unordered_map<std::string, std::string> global_macros;  // $x  → text
std::unordered_map<std::string, std::string> scalars;        // bare x → text
```

### Runtime state (M14b — emitted as SQL)

Runtime values live in DuckDB session variables, not in compiler state. The
compiler only tracks **which names are RUNTIME** (so it knows to emit
`getvariable('name')` instead of text-substituting):

```cpp
enum class BindingKind { LITERAL, RUNTIME };

// For each scalar/local, track whether it was set from a runtime expression
std::unordered_map<std::string, BindingKind> symbol_kinds;

// Map from r() names to DuckDB variable names: "r(max)" → "_r_max"
std::unordered_map<std::string, std::string> stored_results;

// The command that last populated r(), for volatility enforcement
std::string last_rclass_command;
```

**How it works:**
- `scalar pi = 3.14159` → `LITERAL`, text-substituted (Category A)
- `scalar pi = 3.14159` also emits `SET VARIABLE pi = 3.14159` for SQL-level access
- `summarize revenue` → emits `SET VARIABLE _r_max = (SELECT max(revenue) FROM _sN)` etc.
- `scalar hi = r(max)` → `RUNTIME`, emits `SET VARIABLE hi = getvariable('_r_max')`
- `keep if revenue > hi` → `WHERE revenue > getvariable('hi')`

**Taint rule:** if the RHS of a definition references any `r()` token, `_N`, or
a name already marked `RUNTIME`, the new name is `RUNTIME`. Otherwise `LITERAL`.

**Scoping:** `locals` are do-file/loop scoped (push frame on entry, pop on exit).
`globals`, `scalars`, `stored_results` are session-scoped. `clear` resets all.

---

## Pipeline placement

```
raw line(s)
  └─ join /// continuations, strip comments        (ProcessLines, done)
       └─ LOOP UNROLLING (forvalues/foreach)        ── done (M14a)
            └─ MACRO EXPANSION ($g, `l', `=exp')     ── done (M14a)
                 └─ TokenizeCommand                  (exists)
                      └─ ProcessCommand              (exists)
                           ├─ terminal cmd (summarize/count)
                           │    └─ emit SET VARIABLE _r_* = (SELECT ... FROM _sN)  ── M14b
                           └─ TranslateExpression    (exists)
                                └─ RUNTIME scalars → getvariable('name')           ── M14b
                                └─ r(max) → getvariable('_r_max')                  ── M14b
```

Category A (textual preprocessing) is done. Category B (runtime) adds SQL
generation in terminal command handlers and `getvariable()` emission in expression
translation. Both paths emit valid SQL; dodoc and the extension share the same
compiler.

---

## Milestone split

- **M14a — Compile-time macros & loops (Category A). ✅ DONE.**
  `local`, `global`, `scalar` (as locals), `` `x' ``/`$x`/`` `=…' `` expansion,
  `forvalues`, `foreach` over in/local/global/numlist, `tempvar`/`tempname`,
  `display`, macro functions. Scalars use bare-name expansion (Option B: no
  column-name tracking). 250 test assertions. See `docs/M14.md` for details.
- **M14b — Runtime stored results (Category B).** `r()` → frozen subqueries from
  `summarize`/`count`, runtime-bound scalars/locals, `levelsof` → set-membership
  rewrites, error paths for impossible unrolls. `e()` deferred with M9.
- **M14c — Named result structs via `let` (stretch).** `let name = <command>`
  capture, dotted `name.field` access, field validation, and unifying `r()` as the
  implicit `r` struct. Thin re-skin of M14b's recording; sequenced after it.

---

## Test plan

Compile-time (M14a), checkable purely with `dodoc` (text → SQL):

- `local controls age educ` → `regress`/`keep `controls'` expands to the varlist.
- `forvalues i = 1/3 { gen x`i' = `i' }` → three `generate` steps.
- `foreach v of varlist a b { gen ln_`v' = log(`v') }` → two steps.
- `global base "data"; use "`$base'/firms.csv"` → path expansion.
- Nested `` `x`i'' `` resolves innermost-first.
- `forvalues i = 1/`n'` where `` `n' `` is runtime → **error** (A.4).

Runtime (M14b):

- `summarize revenue; keep if revenue == r(max)` → subquery against the summarize
  step; verify the kept rows.
- `count if year==2020; gen share = _N / r(N)` — note `count`'s `r(N)`.
- `scalar hi = r(max)` then `keep if x == hi` → same subquery via the scalar.
- **Indirection (§B.0):** `summarize employment; scalar min_employment = r(min);
  keep if employment > min_employment` → `min_employment` resolves to the
  `(SELECT min(employment) FROM _sN)` subquery even though the use site looks like
  a plain column comparison.
- **Transitive taint:** `scalar floor = min_employment - 1` is RUNTIME and nests
  the inner subquery; `scalar pi = 3.14; keep if x > pi` stays LITERAL.
- Name resolution: a bare name that is *not* a defined scalar is left as a column
  reference, not mistaken for a macro.
- Stale `r()`: `summarize a; count; di r(max)` → error mentioning `count`.
- `levelsof year, local(yrs); keep if inlist(year, `yrs')` →
  `year IN (SELECT DISTINCT year FROM _sK)`.
- Round-trip determinism: the same `.do` compiles to identical SQL regardless of
  data (guards against accidental round-tripping).
</content>
</invoke>
