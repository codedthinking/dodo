# Variable Substitution — Macros, Scalars, and Stored Results

> Design for `local`, `global`, `scalar`, loops (`foreach`/`forvalues`), and
> stored results (`r()`, `e()`) in dodo. This is the plan for milestone **M14**.

## The model: identifier position vs value position

dodo compiles Stata `.do` files to SQL. Every Stata macro, scalar, and stored
result appears in one of two positions in the generated SQL, and the position
determines the mechanism:

| Position in SQL | Mechanism | Why |
|---|---|---|
| **Identifier** — column name, table name, file path, command keyword | **Text substitution** before SQL generation | SQL requires identifiers at parse time; you can't write `SELECT getvariable('cols')` and have DuckDB interpret it as column names |
| **Value** — in expressions, WHERE conditions, arithmetic | **`SET VARIABLE`** + **`getvariable()`** | SQL handles scalar values natively at runtime |

Both `local vars revenue profit` and `scalar pi = 3.14159` store values. The
difference is how they're consumed by SQL:

```stata
local vars revenue profit
keep `vars'                 // → SELECT revenue, profit FROM _s0
                            //   (identifiers — must be text-substituted)

scalar threshold = 1500
keep if revenue > threshold // → WHERE revenue > getvariable('threshold')
                            //   (value — SQL resolves it at runtime)
```

This is the only distinction. There is no compile-time vs runtime split, no
taint propagation, no symbol table complexity. The compiler just needs to know:
**is this name used as an identifier or as a value?**

---

## Identifier position — text substitution

Text substitution rewrites the command string **before** SQL generation. The
compiler pastes the stored text directly into the command, then tokenizes and
translates as usual.

### What uses text substitution

| Usage | Example |
|---|---|
| Column lists in commands | `` keep `vars' `` → `keep revenue profit` |
| Variable names in rename/generate | `` generate `v'_sq = `v' * `v' `` |
| File paths | `` use "$datadir/firms.csv" `` |
| Loop unrolling | `foreach v in a b c { ... }` — body emitted per value, `v` appears in column names |
| Macro functions | `` local n `:word count `vars'' `` |

### Loops

The compiler reads the loop header and `{ … }` body and emits the body once per
iteration, binding the loop variable before each emission. The loop variable
typically appears in identifier position (column names), so it must be text-
substituted.

| Header | Iterates over |
|---|---|
| `forvalues i = 1/10` | 1, 2, …, 10 |
| `forvalues i = 0(2)10` | 0, 2, 4, …, 10 |
| `foreach v in a b c` | literal list tokens |
| `foreach v of local mylist` | tokens of a local |
| `foreach v of numlist 1 3 5` | literal numlist |
| `foreach v of global g` | tokens of a global |

Loop bounds must be compile-time known. A loop whose bound comes from
`getvariable()` cannot be unrolled — the compiler raises an error.

### Status

**M14a: Done.** See `docs/M14.md`.

---

## Value position — `SET VARIABLE` (M14b)

Everything that appears as a **value** (not an identifier) compiles to DuckDB's
`SET VARIABLE`. This covers constants, computed results, and observation counts.

### The mechanism

```sql
-- Assignment: evaluates RHS when the statement executes, freezes the value
SET VARIABLE pi = 3.14159;
SET VARIABLE n = (SELECT count(*) FROM _s3);
SET VARIABLE floor = getvariable('pi') * 2;
```

Properties:
- **Evaluated once at assignment.** Value is frozen when `SET VARIABLE` runs.
- **Session-scoped.** Variables persist across statements within a connection.
- **Composable.** `SET VARIABLE floor = getvariable('pi') * 2` works natively.
- **dodoc-safe.** `SET VARIABLE` is valid DuckDB SQL.
- **No compiler round-trip.** The compiler emits SQL; DuckDB evaluates it.

M14b emits `SET VARIABLE` for assignments only. Use-site resolution (emitting
`getvariable()` in expressions like `WHERE revenue > floor`) comes in a later
milestone.

### Scalars

Every `scalar` command emits `SET VARIABLE`:

```stata
scalar pi = 3.14159        // → SET VARIABLE pi = 3.14159
scalar floor = pi * 2      // → SET VARIABLE floor = getvariable('pi') * 2
```

### `local`/`global` with `=`

When `local` or `global` uses `=`, it defines a value (not a word list for
identifier substitution). These also compile to `SET VARIABLE`:

```stata
local n = _N               // → SET VARIABLE n = (SELECT count(*) FROM _sN)
local threshold = 1500     // → SET VARIABLE threshold = 1500
```

**Note:** The same local can appear in both positions. `local x 5` stores `"5"`.
If used as `` generate col`x' = 1 `` (identifier), it's text-substituted to
`generate col5 = 1`. The compiler determines which mechanism to use based on
where in the SQL the reference lands. Use-site `getvariable()` emission is a
later milestone.

### `_N` (observation count)

`_N` in a `SET VARIABLE` context compiles to a count subquery:

```stata
local n = _N               // → SET VARIABLE n = (SELECT count(*) FROM _sN)
scalar total = _N          // → SET VARIABLE total = (SELECT count(*) FROM _sN)
```

`_N` inside SQL expressions (e.g., `generate pct = _n / _N`) continues to use
`COUNT(*) OVER ()` as a window function — that path is unchanged.

### `levelsof` and set-based rewrites

`levelsof x, local(L)` produces a set of unknown size. Common idioms compile
to set-based SQL:

| Stata idiom | SQL rewrite |
|---|---|
| `levelsof x, local(L)` then `` keep if inlist(y, `L') `` | `WHERE y IN (SELECT DISTINCT x FROM _sK)` |

Loops over `levelsof` lists that require an unknown number of iterations are
out of scope and produce an explicit error.

### `e()` (estimation results)

Deferred with `regress` (M9). Same `SET VARIABLE` mechanism.

---

## Stored results as single-row tables (M14c)

Terminal commands (`summarize`, `count`) produce multiple named results. Rather
than emitting one `SET VARIABLE` per field, M14c stores all results in a
**single-row table**. This is more natural in SQL: the result is a relation,
not a bag of scalars.

### The mechanism

```stata
summarize revenue          // latest step: _s3
```

emits a CTE that produces one row with all statistics as columns:

```sql
CREATE OR REPLACE TEMP TABLE _r AS
  SELECT
    count(revenue)       AS N,
    avg(revenue)         AS mean,
    min(revenue)         AS min,
    max(revenue)         AS max,
    stddev_samp(revenue) AS sd
  FROM _s3;
```

`count` emits:

```sql
CREATE OR REPLACE TEMP TABLE _r AS
  SELECT count(*) AS N FROM _sN;
```

References to `r(max)` compile to a scalar subquery:

```stata
keep if revenue >= r(mean)       // → WHERE revenue >= (SELECT mean FROM _r)
generate hi = revenue == r(max)  // → (revenue = (SELECT max FROM _r)) AS hi
```

**Volatility:** Each new r-class command replaces the `_r` table.

### Named result structs via `let`

```stata
let result = summarize employment
keep if employment > result.min
generate z = (employment - result.mean) / result.sd
```

`let` stores the result table under a named alias. `result.min` compiles to
`(SELECT min FROM _result)`.

Advantages over `r()`:
1. **Not volatile.** Multiple result tables coexist.
2. **Validated fields.** Unknown fields are compile-time errors.

`r()` is reframed as the implicit, auto-reassigned `_r` table.

---

## Why not round-trip? (Rejected)

Execute `summarize`, read the result, substitute the literal into later commands.

1. **Breaks dodoc.** The standalone compiler has no database.
2. **Breaks laziness.** Forces mid-script materialization.
3. **Impure.** Same `.do` file could compile to different SQL on different days.

`SET VARIABLE` avoids all three: the compiler emits SQL, DuckDB evaluates it.

---

## State changes (`DodoState`)

### Compiler state (M14a — done)

```cpp
// Text substitution (identifier position)
std::unordered_map<std::string, std::string> local_macros;   // `x' → text
std::unordered_map<std::string, std::string> global_macros;  // $x  → text
```

### Value state (M14b)

```cpp
// Names that have been SET VARIABLE'd — so the compiler knows which
// names are session variables (vs column references)
std::unordered_set<std::string> set_variables;
```

### Result table state (M14c)

```cpp
// Named result tables: "r" → "_r", "result" → "_result"
std::unordered_map<std::string, std::string> result_tables;

// Known columns per result table, for compile-time field validation
std::unordered_map<std::string, std::vector<std::string>> result_table_fields;

// The command that last populated _r, for volatility enforcement
std::string last_rclass_command;
```

---

## Pipeline placement

```
raw line(s)
  └─ join /// continuations, strip comments        (ProcessLines, done)
       └─ LOOP UNROLLING (forvalues/foreach)        ── done (M14a)
            └─ TEXT SUBSTITUTION ($g, `l')           ── done (M14a)
                 └─ TokenizeCommand                  (exists)
                      └─ ProcessCommand              (exists)
                           ├─ scalar / local =       → emit SET VARIABLE         ── M14b
                           ├─ terminal cmd           → emit result table          ── M14c
                           ├─ let name = cmd         → emit named result table    ── M14c
                           └─ TranslateExpression    (exists)
                                └─ r(max)            → (SELECT max FROM _r)       ── M14c
                                └─ result.field      → (SELECT field FROM _name)  ── M14c
                                └─ known SET VAR     → getvariable('name')       ── future
                                └─ unknown names     → column reference (as-is)
```

---

## Milestone split

- **M14a — Text substitution & loops. ✅ DONE.** See `docs/M14.md`.
- **M14b — `SET VARIABLE` for assignments.** `scalar`/`local =`/`global =`
  emit `SET VARIABLE`. `_N` emits count subquery. `levelsof` for set membership.
  No use-site `getvariable()` emission yet.
- **M14c — Stored results as single-row tables.** Terminal commands emit
  `CREATE OR REPLACE TEMP TABLE _r AS (SELECT ...)`. `r(field)` compiles to
  `(SELECT field FROM _r)`. `let name = cmd` stores named result tables.
  `name.field` compiles to `(SELECT field FROM _name)`.

---

## Test plan

**M14a (done):** text substitution, loops, macro functions — 1359 assertions.

**M14b:**
- `scalar pi = 3.14159` → `SET VARIABLE pi = 3.14159`
- `scalar floor = pi * 2` → `SET VARIABLE floor = getvariable('pi') * 2`
- `local n = _N` → `SET VARIABLE n = (SELECT count(*) FROM _sN)`
- `levelsof year, local(yrs); keep if inlist(year, `yrs')` →
  `WHERE year IN (SELECT DISTINCT year FROM _sK)`

**M14c:**
- `summarize revenue` → `CREATE OR REPLACE TEMP TABLE _r AS (SELECT count(...) AS N, avg(...) AS mean, ...)`
- `keep if revenue == r(max)` → `WHERE revenue = (SELECT max FROM _r)`
- `let result = summarize employment; keep if employment > result.min` →
  result table + `WHERE employment > (SELECT min FROM _result)`
- Stale `r()`: `summarize a; count; keep if x > r(max)` → error (volatility)
- `r(nonexistent)` → compile-time error (field validation)
