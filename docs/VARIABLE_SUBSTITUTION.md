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

**Scoping:** Loop index variables are scoped to the loop body. They are set as
LITERAL entries in `local_symbols` during each iteration and **erased** when
the loop ends. Variables created inside the loop body (via `local`, `scalar`,
etc.) persist normally — only the index variable is destroyed.

### Status

**M14a: Done.** See `docs/M14.md`.

---

## Value position — `SET VARIABLE` (M14b)

For `=` assignments, the compiler first tries to evaluate the expression at
compile time (after macro expansion). If the expression is fully resolvable
to a literal, the symbol is stored as **LITERAL**. Otherwise, the compiler
emits `SET VARIABLE` and stores the symbol as **VARIABLE**.

### The mechanism

```stata
scalar pi = 3.14159        // parser evaluates → LITERAL "3.14159"
local a = 300              // parser evaluates → LITERAL "300"
local b = `a' * 5         // `a' expands to "300", parser evaluates → LITERAL "1500"
local n = _N               // _N is runtime → VARIABLE, emits SET VARIABLE _dodo_l_n = (SELECT count(*) FROM ...)
scalar floor = `n' * 2    // `n' expands to getvariable(...), can't evaluate → VARIABLE
```

When falling back to VARIABLE:

```sql
SET VARIABLE _dodo_l_n = (SELECT count(*) FROM _s3);
SET VARIABLE _dodo_s_floor = getvariable('_dodo_l_n') * 2;
```

Properties of `SET VARIABLE`:
- **Evaluated once at assignment.** Value is frozen when `SET VARIABLE` runs.
- **Session-scoped.** Variables persist across statements within a connection.
- **Composable.** `getvariable()` references work inside `SET VARIABLE` RHS.
- **dodoc-safe.** `SET VARIABLE` is valid DuckDB SQL.
- **No compiler round-trip.** The compiler emits SQL; DuckDB evaluates it.

### Symbol resolution at use sites

When a symbol is encountered during macro expansion (`ExpandMacros`):
- **LITERAL** entry → substitute the stored text directly
- **VARIABLE** entry → substitute `getvariable('<uname>')`

```stata
local vars revenue profit     // LITERAL → `vars' expands to "revenue profit"
local a = 300                 // LITERAL → `a' expands to "300"
local n = _N                  // VARIABLE → `n' expands to getvariable('_dodo_l_n')
```

This means LITERAL symbols propagate: if `a` is LITERAL and `b = `a' * 5`,
macro expansion produces `b = 300 * 5`, the parser evaluates to 1500, and `b`
is also LITERAL. Only when a VARIABLE symbol contaminates an expression does
the result become VARIABLE.

### Scalars

```stata
scalar pi = 3.14159        // LITERAL "3.14159" — parser evaluates
scalar floor = pi * 2      // pi is LITERAL, expands to "3.14159", evaluates → LITERAL "6.28318"
scalar n = _N              // VARIABLE → SET VARIABLE _dodo_s_n = (SELECT count(*) FROM ...)
scalar msg = "hello"       // LITERAL "hello" — quoted string
```

### `local`/`global` with `=`

When `local` or `global` uses `=`, it defines a value (not a word list for
identifier substitution):

```stata
local n = _N               // VARIABLE → SET VARIABLE _dodo_l_n = (SELECT count(*) FROM _sN)
local threshold = 1500     // LITERAL "1500" — parser evaluates
local msg = "hello"        // LITERAL "hello" — quoted string
```

**Note:** The same local can appear in both positions. `local x 5` stores `"5"`
as LITERAL. `local x = 5` also stores `"5"` as LITERAL (parser evaluates).
Both expand identically via `` `x' `` → `"5"`. The difference only arises
when the RHS contains runtime references.

### `_N` (observation count)

`_N` in a `SET VARIABLE` context compiles to a count subquery:

```stata
local n = _N               // → SET VARIABLE _dodo_l_n = (SELECT count(*) FROM _sN)
scalar total = _N          // → SET VARIABLE _dodo_s_total = (SELECT count(*) FROM _sN)
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

### Named result structs via `scalar`

```stata
scalar result = summarize employment
keep if employment > result.min
generate z = (employment - result.mean) / result.sd
```

`scalar name = command` stores the result table under a named alias.
`result.min` compiles to `(SELECT min FROM _result)`.

**Why `scalar`?** In Stata, "scalar" means any named value that is not a
column — it can hold a number, a string, or (in our extension) a whole row of
results. This is a misnomer from the SQL perspective, where "scalar" means a
single value. We keep the keyword because Stata users expect it, and because
it already exists in the language. The compiler distinguishes the two forms by
the RHS: `scalar x = expr` stores a single value via `SET VARIABLE` (M14b),
while `scalar x = command` stores a result table (M14c).

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

### Unified symbol table (M14b)

The runtime carries a single symbol table with four namespaces: local macro,
global macro, scalar, and table. The same name can exist in all namespaces
independently. Each symbol is either **LITERAL** (text value stored directly)
or **VARIABLE** (backed by a DuckDB `SET VARIABLE`, resolved at runtime via
`getvariable()`).

```cpp
enum class SymbolKind { LITERAL, VARIABLE };

struct SymbolEntry {
    SymbolKind kind;
    std::string value;  // LITERAL: the text; VARIABLE: the DuckDB variable name
};

// Four distinct namespaces — same name can exist in each
std::unordered_map<std::string, SymbolEntry> local_symbols;
std::unordered_map<std::string, SymbolEntry> global_symbols;
std::unordered_map<std::string, SymbolEntry> scalar_symbols;
// table_symbols reserved for M14c

std::vector<std::string> pending_sql;  // queued SET VARIABLE statements
```

**LITERAL** symbols have a text value stored directly. A symbol is LITERAL when:
- Declared without `=` (word lists): `local vars revenue profit`
- Declared with `=` and a quoted string: `scalar msg = "hello"`
- Declared with `=` and an expression that the parser can fully evaluate at
  compile time (all operands are literals or resolve to LITERAL symbols):

```stata
local vars revenue profit      // LITERAL: "revenue profit"
scalar msg = "hello"           // LITERAL: "hello"
local a = 300                  // LITERAL: "300" (parser evaluates 300 → "300")
scalar pi = 3.14159            // LITERAL: "3.14159"
local b = `a' * 5             // LITERAL: "1500" (a is LITERAL "300", expands, evaluates)
```

**VARIABLE** symbols are backed by a DuckDB `SET VARIABLE`. A symbol becomes
VARIABLE when the expression parser cannot fully evaluate the RHS — i.e., after
macro expansion, the expression still contains runtime references (`_N`,
`getvariable()` from other VARIABLE symbols, etc.):

```stata
local n = _N                   // VARIABLE: _dodo_l_n → SET VARIABLE _dodo_l_n = (SELECT count(*) FROM ...)
scalar hi = _N - 1             // VARIABLE: _dodo_s_hi → SET VARIABLE _dodo_s_hi = (SELECT count(*) - 1 FROM ...)
local m = `n' + 1             // VARIABLE: n is VARIABLE, expands to getvariable(...), can't evaluate
```

The decision is made by the expression parser: **try to evaluate → if it
succeeds, LITERAL; if it fails, VARIABLE with SET VARIABLE**. This keeps
compile-time evaluation as the fast path and only falls back to runtime when
necessary.

Unique DuckDB variable names follow the pattern `_dodo_<prefix>_<name>` where
prefix is `l` (local), `g` (global), or `s` (scalar). This ensures namespace
isolation: `local x` and `scalar x` produce `_dodo_l_x` and `_dodo_s_x`.

### Result table state (M14c)

```cpp
// Named result tables: "r" → "_r", "result" → "_result" (from scalar result = ...)
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
                 │  LITERAL symbols → substitute text directly
                 │  VARIABLE symbols → substitute getvariable('<uname>')
                 └─ TokenizeCommand                  (exists)
                      └─ ProcessCommand              (exists)
                           ├─ local/global/scalar =  → emit SET VARIABLE         ── M14b
                           ├─ terminal cmd           → emit result table          ── M14c
                           ├─ scalar name = cmd      → emit named result table    ── M14c
                           └─ TranslateExpression    (exists, no symbol lookup)
                                └─ r(max)            → (SELECT max FROM _r)       ── M14c
                                └─ result.field      → (SELECT field FROM _name)  ── M14c
                                └─ unknown names     → column reference (as-is)
```

---

## Milestone split

- **M14a — Text substitution & loops. ✅ DONE.** See `docs/M14.md`.
- **M14b — Unified symbol table & `SET VARIABLE`.** Replace separate macro/scalar
  maps with `SymbolEntry` (LITERAL/VARIABLE) in four namespaces. `=` assignments
  try compile-time evaluation first (LITERAL if successful), fall back to
  `SET VARIABLE` with namespaced unique names (`_dodo_l_x`, `_dodo_s_x`).
  `ExpandMacros` resolves VARIABLE symbols to `getvariable()`.
  `_N` emits count subquery. `levelsof` for set membership.
- **M14c — Stored results as single-row tables.** Terminal commands emit
  `CREATE OR REPLACE TEMP TABLE _r AS (SELECT ...)`. `r(field)` compiles to
  `(SELECT field FROM _r)`. `scalar name = cmd` stores named result tables.
  `name.field` compiles to `(SELECT field FROM _name)`.

---

## Test plan

**M14a (done):** text substitution, loops, macro functions — 1359 assertions.

**M14b:**
- `scalar pi = 3.14159` → LITERAL `"3.14159"` (parser evaluates, no SET VARIABLE)
- `scalar floor = pi * 2` → LITERAL `"6.28318"` (pi is LITERAL, expands, evaluates)
- `local n = _N` → VARIABLE, `SET VARIABLE _dodo_l_n = (SELECT count(*) FROM _sN)`
- `local a = 300` → LITERAL `"300"` (parser evaluates)
- `local b = `a' * 5` → LITERAL `"1500"` (a is LITERAL, expands to 300, evaluates)
- `local m = `n' + 1` → VARIABLE (n is VARIABLE, expands to getvariable, can't evaluate)
- `local x 5` then `` `x' `` → `"5"` (LITERAL, text substitution)
- `local x = 5` then `` `x' `` → `"5"` (also LITERAL — parser evaluates successfully)
- Namespace isolation: `local x = _N` and `scalar x = _N` → `_dodo_l_x` and `_dodo_s_x`
- `foreach v of local mylist` where mylist is VARIABLE → error (cannot iterate runtime value)
- Loop accumulation: `local total = `total' + `i'` stays LITERAL when both are LITERAL
- `levelsof year, local(yrs); keep if inlist(year, `yrs')` →
  `WHERE year IN (SELECT DISTINCT year FROM _sK)`

**M14c:**
- `summarize revenue` → `CREATE OR REPLACE TEMP TABLE _r AS (SELECT count(...) AS N, avg(...) AS mean, ...)`
- `keep if revenue == r(max)` → `WHERE revenue = (SELECT max FROM _r)`
- `scalar result = summarize employment; keep if employment > result.min` →
  result table + `WHERE employment > (SELECT min FROM _result)`
- Stale `r()`: `summarize a; count; keep if x > r(max)` → error (volatility)
- `r(nonexistent)` → compile-time error (field validation)

## Scalars vs. data columns (known deviation)

dodo translates commands to SQL without reading the dataset's schema, so at
compile time it cannot tell whether a bare identifier is a data column or a
scalar. When a scalar and a column share a name, dodo resolves the **bare name
to the scalar** — Stata resolves it to the column (variables take precedence
over scalars).

```stata
scalar revenue = 5
generate doubled = revenue * 2   // dodo: (5 * 2);  Stata: (<column revenue> * 2)
```

To be explicit and portable, use the `scalar(name)` function form, which always
resolves to the scalar regardless of any same-named column:

```stata
generate scaled = scalar(factor) * revenue   // scalar(factor) -> the scalar; revenue -> the column
```

Scalar substitution never occurs inside string literals:

```stata
scalar region = 5
generate note = "region is here"   // -> 'region is here' (not '5 is here')
```

Global (`$name`, `${name}`) and local (`` `name' ``) macros do expand inside
double-quoted strings (matching Stata). A `$` followed by a non-letter (e.g.
`$20`) is left literal, since Stata macro names cannot begin with a digit.
