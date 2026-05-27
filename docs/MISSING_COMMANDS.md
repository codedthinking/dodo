# Missing Stata Commands in Dodo

Inventory of Stata commands used in [korenmiklos/ceo-value](https://github.com/korenmiklos/ceo-value/tree/main/lib/create) but not yet implemented in the Dodo extension.

## Critical (used in nearly every file)

| Gap | Usage | SQL mapping |
|---|---|---|
| `merge` (1:1, m:1, m:m) | Joins between datasets | DuckDB JOIN |
| Local macros (`local x = ...`, `` `x' ``) | Variable substitution | String replacement in parser |
| `foreach` / `forvalues` loops | Repeated operations | Unroll in parser |
| `tempfile` + `preserve`/`restore` | Intermediate data stacks | Stack of CTE chains in state |
| `bysort var1 (var2):` prefix | Sort-within-group for generate | `OVER (PARTITION BY var1 ORDER BY var2)` |
| `xtset` + `L.`/`F.` lag/lead | Panel data operators | `LAG()/LEAD() OVER (PARTITION BY id ORDER BY t)` |

## Important (used in multiple files)

| Gap | Usage | SQL mapping |
|---|---|---|
| `duplicates drop` | Dedup rows | `SELECT DISTINCT` |
| `expand N` | Replicate rows | `GENERATE_SERIES` + cross join |
| `joinby` | Many-to-many merge | `CROSS JOIN` or unrestricted `JOIN` |
| `export delimited` | Write CSV | `COPY ... TO` (like `save`) |
| `import delimited` | Read CSV | Already works via `use` |
| `inrange()`, `inlist()` | Filter expressions | `BETWEEN` and `IN` |
| `cond()` | Ternary expression | `CASE WHEN` |
| `substr()`, `real()` | String functions | `SUBSTRING`, `CAST` |
| Running `sum()` in `bysort:` | Cumulative sum | `SUM() OVER (ORDER BY ...)` |

## Nice to have (specialized)

| Gap | Usage |
|---|---|
| `reghdfe` | High-dimensional FE regression (third-party) |
| `scalar`, `display`, `assert` | Scripting/debugging |
| `recode` | Value recoding |
| `set seed`, `set obs` | Simulation setup |
| `args`, `confirm` | Script arguments |
| `compress` | No-op in DuckDB (no storage types to optimize) |

## Already partially working

- `cond()` — planned but not yet in expression translator
- `collapse (firstnm)/(count)/(min)/(max)` — count/min/max work, firstnm needs adding
- `label define` with many pairs — already works
- `reshape wide` with `j()` — already works
