# Variable Substitution — Macros, Scalars, and Stored Results

> Design for `local`, `global`, `scalar`, loops (`foreach`/`forvalues`), and
> stored results (`r()`, `e()`) in dodo. This is the plan for milestone **M14**.

## The model

dodo compiles Stata `.do` files to SQL. Every Stata "variable" (macro, scalar,
stored result) compiles to one of two SQL mechanisms:

| Stata construct | SQL mechanism | When evaluated |
|---|---|---|
| `local varlist revenue profit` | **Text substitution** — expanded by the compiler before SQL generation | Compile time |
| `scalar pi = 3.14159` | `SET VARIABLE pi = 3.14159` | Execution time (but value is a constant) |
| `scalar hi = r(max)` | `SET VARIABLE hi = (SELECT max(revenue) FROM _sN)` | Execution time |
| `local n = _N` | `SET VARIABLE n = (SELECT count(*) FROM _sN)` | Execution time |
| `` keep if revenue > `threshold' `` | `WHERE revenue > getvariable('threshold')` | Query time |
| `foreach i in a b c { ... }` | **Loop unrolling** — body emitted once per value | Compile time |

There is **no LITERAL/RUNTIME distinction** in the compiler. `SET VARIABLE`
handles both cases uniformly — the only difference is what appears on the RHS
(a constant or a subquery). DuckDB evaluates the RHS at the point the
`SET VARIABLE` statement executes and freezes the value.

**Text substitution** is reserved for syntactic operations that cannot be
expressed as SQL values: variable lists in command arguments (`` keep `vars' ``),
loop unrolling, `$global` path expansion.

---

## Category A — Text substitution (compile-time syntax)

These are pure text rewrites that happen **before** SQL generation. They expand
syntactic tokens, not data values.

### A.1 What stays as text substitution

| Pattern | Purpose | Example |
|---|---|---|
| `local name word1 word2 ...` (no `=`) | Variable lists for command arguments | `local vars revenue profit` → `` keep `vars' `` → `keep revenue profit` |
| `global name text` (no `=`) | Path prefixes, option strings | `global base "data/"` → `` use "$base/firms.csv" `` |
| `` `name' `` expansion | Insert local text into command | `` rename `old' `new' `` |
| `$name` / `${name}` expansion | Insert global text into command | `use "$datadir/file.csv"` |
| `foreach` / `forvalues` | Loop unrolling | Body emitted N times with substitution |
| Macro functions (`:word count`, etc.) | Text manipulation | `` local n `:word count `vars'' `` |

### A.2 Loops — unrolled at compile time

The compiler reads the loop header and `{ … }` body and emits the body once per
iteration, binding the loop local before each emission.

| Header | Iterates over |
|---|---|
| `forvalues i = 1/10` | 1, 2, …, 10 |
| `forvalues i = 0(2)10` | 0, 2, 4, …, 10 |
| `foreach v in a b c` | literal list tokens |
| `foreach v of local mylist` | tokens of a text-substitution local |
| `foreach v of numlist 1 3 5` | literal numlist |
| `foreach v of global g` | tokens of a global |

Loop bounds must be known at compile time. `forvalues i = 1/`n'` where `n` was
set via `local n = _N` (which compiles to `SET VARIABLE`) **cannot be unrolled** —
the compiler raises an error pointing to set-based alternatives.

### A.3 Status

**M14a: Done.** Text substitution, loop unrolling, `tempvar`/`tempname`,
`display`, macro functions. See `docs/M14.md`.

---

## Category B — `SET VARIABLE` / `getvariable()` (values)

Everything that carries a value — constants, computed results, observation
counts — compiles to DuckDB's `SET VARIABLE` + `getvariable()`.

### B.1 The mechanism

```sql
-- Assignment: evaluates RHS at execution time, stores the result
SET VARIABLE pi = 3.14159;
SET VARIABLE hi = (SELECT max(revenue) FROM _s3);
SET VARIABLE n = (SELECT count(*) FROM _s3);
SET VARIABLE floor = getvariable('hi') - 1;

-- Reference: retrieves stored value
SELECT * FROM _s4 WHERE revenue > getvariable('floor');
```

Properties:
- **Evaluated once at assignment.** Value is frozen when `SET VARIABLE` runs.
- **Session-scoped.** Variables persist across statements within a connection.
- **Composable.** `SET VARIABLE floor = getvariable('hi') - 1` works natively.
- **dodoc-safe.** Both `SET VARIABLE` and `getvariable()` are valid DuckDB SQL.
- **No compiler round-trip.** The compiler never observes query results.

### B.2 Scalars

Every `scalar` command emits `SET VARIABLE`:

```stata
scalar pi = 3.14159        // → SET VARIABLE pi = 3.14159
scalar hi = r(max)         // → SET VARIABLE hi = (SELECT max(revenue) FROM _sN)
scalar floor = hi - 1      // → SET VARIABLE floor = getvariable('hi') - 1
```

At use sites, bare scalar names compile to `getvariable('name')`:

```stata
keep if revenue > floor    // → WHERE revenue > getvariable('floor')
generate ratio = revenue / pi  // → (revenue / getvariable('pi')) AS ratio
```

### B.3 `local`/`global` with `=`

When `local` or `global` uses `=`, the RHS is an expression (not a word list).
These also compile to `SET VARIABLE`:

```stata
local n = _N               // → SET VARIABLE n = (SELECT count(*) FROM _sN)
local threshold = 1500     // → SET VARIABLE threshold = 1500
local hi = r(max)          // → SET VARIABLE hi = (SELECT max(revenue) FROM _sN)
```

At use sites, `` `n' `` compiles to `getvariable('n')`.

**Note:** `local varlist revenue profit` (no `=`) stays as text substitution
because it's a word list, not a value. The presence of `=` distinguishes the
two cases.

### B.4 Stored results `r()`

When the compiler processes a terminal command (`summarize`, `count`), it emits
`SET VARIABLE` statements that capture the results from the current CTE step:

```stata
summarize revenue          // latest step: _s3
```

emits:

```sql
SET VARIABLE _r_N    = (SELECT count(revenue)       FROM _s3);
SET VARIABLE _r_mean = (SELECT avg(revenue)         FROM _s3);
SET VARIABLE _r_min  = (SELECT min(revenue)         FROM _s3);
SET VARIABLE _r_max  = (SELECT max(revenue)         FROM _s3);
SET VARIABLE _r_sd   = (SELECT stddev_samp(revenue) FROM _s3);
```

`count` emits `SET VARIABLE _r_N = (SELECT count(*) FROM _sN)`.

References to `r(max)` compile to `getvariable('_r_max')`:

```stata
keep if revenue >= r(mean)     // → WHERE revenue >= getvariable('_r_mean')
generate hi = revenue == r(max)  // → (revenue = getvariable('_r_max')) AS hi
```

**Volatility:** Each new r-class command overwrites the `_r_*` variables
(matching Stata). A compile-time reference to a stale `r()` is an error.

### B.5 `_N` (observation count)

`_N` in a `SET VARIABLE` context compiles to a count subquery:

```stata
local n = _N               // → SET VARIABLE n = (SELECT count(*) FROM _sN)
scalar total = _N          // → SET VARIABLE total = (SELECT count(*) FROM _sN)
```

`_N` inside SQL expressions (e.g., `generate row = _n / _N`) continues to use
`COUNT(*) OVER ()` as a window function — that path is unchanged.

### B.6 `levelsof` and set-based rewrites

`levelsof x, local(L)` produces a list of unknown length. It cannot be
text-substituted or loop-unrolled. Common idioms compile to set-based SQL:

| Stata idiom | SQL rewrite |
|---|---|
| `levelsof x, local(L)` then `` keep if inlist(y, `L') `` | `WHERE y IN (SELECT DISTINCT x FROM _sK)` |
| `levelsof x` for membership | `IN (SELECT DISTINCT x FROM _sK)` |

Loops over `levelsof` lists that require an unknown number of iterations are
out of scope and produce an explicit error.

### B.7 `e()` (estimation results)

Deferred with `regress` (M9). Same `SET VARIABLE` mechanism as `r()`.

---

## Stretch goal: named result structs via `let`

```stata
let result = summarize employment
keep if employment > result.min
generate z = (employment - result.mean) / result.sd
```

`let` emits `SET VARIABLE` under a named prefix:

| Field | SQL |
|---|---|
| `result.min` | `SET VARIABLE result_min = (SELECT min(employment) FROM _s3)` |
| `result.mean` | `SET VARIABLE result_mean = (SELECT avg(employment) FROM _s3)` |

`result.min` in expressions → `getvariable('result_min')`.

Advantages over `r()`:
1. **Not volatile.** Multiple results coexist (`emp.min` and `rev.mean`).
2. **Validated fields.** Unknown fields are compile-time errors.

`r()` is reframed as the implicit, auto-reassigned `r` struct.
Sequenced after M14b.

---

## Why not round-trip? (Rejected)

A tempting alternative: execute `summarize`, read the result, substitute
the literal `42` into later commands. We reject this:

1. **Breaks dodoc.** The standalone compiler has no database.
2. **Breaks laziness.** Forces mid-script materialization.
3. **Impure.** Same `.do` file could compile to different SQL on different days.

`SET VARIABLE` avoids all three: the compiler emits SQL, DuckDB evaluates it.

---

## State changes (`DodoState`)

### Compiler state (M14a — done)

```cpp
// Text substitution (word lists, paths, loop variables)
std::unordered_map<std::string, std::string> local_macros;   // `x' → text
std::unordered_map<std::string, std::string> global_macros;  // $x  → text
```

### Value state (M14b — emitted as SQL)

```cpp
// Track names that have been SET VARIABLE'd
// The compiler needs to know these exist so it can emit getvariable('name')
// at use sites instead of treating them as column references
std::unordered_set<std::string> set_variables;

// Map from r() names to DuckDB variable names: "r(max)" → "_r_max"
std::unordered_map<std::string, std::string> stored_results;

// The command that last populated r(), for volatility enforcement
std::string last_rclass_command;
```

The compiler does not track the *values* of SET VARIABLE'd names — only their
*existence*, so it knows to emit `getvariable()` references.

---

## Pipeline placement

```
raw line(s)
  └─ join /// continuations, strip comments        (ProcessLines, done)
       └─ LOOP UNROLLING (forvalues/foreach)        ── done (M14a)
            └─ MACRO EXPANSION ($g, `l')             ── done (M14a)
                 └─ TokenizeCommand                  (exists)
                      └─ ProcessCommand              (exists)
                           ├─ scalar/local =         → emit SET VARIABLE         ── M14b
                           ├─ terminal cmd           → emit SET VARIABLE _r_*    ── M14b
                           └─ TranslateExpression    (exists)
                                └─ known SET VAR names → getvariable('name')     ── M14b
                                └─ r(max)              → getvariable('_r_max')   ── M14b
                                └─ unknown names       → column reference (as-is)
```

---

## Milestone split

- **M14a — Text substitution & loops. ✅ DONE.**
  See `docs/M14.md`.
- **M14b — `SET VARIABLE` for scalars, `r()`, `_N`.**
  `scalar`/`local =`/`global =` emit `SET VARIABLE`. Terminal commands emit
  `SET VARIABLE _r_*`. Expression translation emits `getvariable()`. `levelsof`
  for set membership. `e()` deferred with M9.
- **M14c — Named result structs via `let` (stretch).**
  `let name = <command>` emits prefixed `SET VARIABLE`. Dotted access
  `name.field` → `getvariable('name_field')`. Sequenced after M14b.

---

## Test plan

**M14a (done):** text substitution, loops, macro functions — 250 assertions.

**M14b:**
- `scalar pi = 3.14159; keep if revenue > pi` → `SET VARIABLE pi = 3.14159;
  WHERE revenue > getvariable('pi')`
- `summarize revenue; keep if revenue == r(max)` → `SET VARIABLE _r_max = ...;
  WHERE revenue = getvariable('_r_max')`
- `scalar hi = r(max); scalar floor = hi - 1; keep if revenue > floor` →
  chain of `SET VARIABLE` + `getvariable()`
- `local n = _N; display "`n'"` → `SET VARIABLE n = (SELECT count(*) ...);
  getvariable('n')`
- `levelsof year, local(yrs); keep if inlist(year, `yrs')` →
  `WHERE year IN (SELECT DISTINCT year FROM _sK)`
- Stale `r()`: `summarize a; count; keep if x > r(max)` → error
- `forvalues i = 1/`n'` where `n` was SET VARIABLE'd → error (can't unroll)
