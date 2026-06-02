# Missing Commands in dodo

Inventory based on [korenmiklos/ceo-value](https://github.com/korenmiklos/ceo-value/tree/main/lib/create). Updated June 2026 for v0.3.0.

## Implemented (v0.3.0)

These were previously listed as missing but are now complete:

- `merge` (1:1, m:1, 1:m, m:m) with `keep()`, `keepusing()`, `nogenerate`
- `bysort`/`by` prefix with partition and sort vars
- `xtset`/`tsset` + `L.`/`F.`/`D.` gap-aware lag/lead operators
- `tempfile`, `preserve`/`restore`
- `duplicates drop` (all columns or by varlist)
- `expand` with optional `generate()`
- `import delimited`, `export delimited`
- `inrange()`, `inlist()`, `cond()`, `substr()`, `real()`, `int()`
- `strlower()`, `strupper()`, `strtrim()`, `strlen()`
- Running `sum()` in `bysort:` context
- `label variable`, `label define`, `label values`, `label list`
- `reshape long` and `reshape wide`
- `mvencode`
- `undo`/`redo`, `history`
- `show sql`
- `var[_n-1]` subscript syntax (positional lag/lead)
- `local`/`global`/`scalar` macros with compile-time and runtime (`SET VARIABLE`) evaluation
- `foreach`/`forvalues` loops with compile-time unrolling
- `display`, `assert`, `compress`, `levelsof`
- `tempvar`, `tempname`
- `!` as NOT, `.` as NULL, multi-arg `missing()`
- Bulk `rename` syntax
- `total` as alias for `sum` in `egen`

## Still missing

### Critical

| Gap | Usage | SQL mapping |
|---|---|---|
| Runtime stored results (`r(max)`, `r(N)`, `e()`) | Reuse of computed values | Planned as single-row tables (M14c) |
| `levelsof ... , local()` option | Store distinct values in macro | Planned via set-based SQL rewrites |

### Nice to have

| Gap | Usage | Notes |
|---|---|---|
| `joinby` | Many-to-many merge | Could use `CROSS JOIN` or unrestricted `JOIN` |
| `reghdfe` / `regress` | Regression | Needs stats extension or custom implementation |
| `recode` | Value recoding | `CASE WHEN` chains |
| `set seed` | Simulation setup | `SELECT setseed(N)` |
| `mvencode _all` | Replace all missing | Needs column introspection at runtime |
| `reshape wide` with multiple value vars | Multi-var pivot | Currently supports one value variable |
| `program define` | User-defined programs | No SQL equivalent |
| `if`/`else` control flow | Branching | No SQL equivalent |
