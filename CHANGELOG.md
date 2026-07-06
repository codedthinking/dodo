# Changelog

## Unreleased

### Fixed — SQL correctness

- Filenames and every other user-supplied value pasted into generated SQL are
  now escaped as proper SQL string literals (`use "it's data.csv"` →
  `read_csv('it''s data.csv')`). Previously an apostrophe broke — or injected
  into — the generated query.
- Double-quoted strings in expressions convert to SQL literals with embedded
  apostrophes doubled: `keep if name == "it's"` → `'it''s'` (was the invalid
  `'it's'`).
- `//` inside a string literal (e.g. `"http://example.com"`) is no longer
  treated as a comment. Matching Stata, `//` only starts a comment when
  preceded by whitespace.
- Comparisons against the missing sentinel `.` translate to NULL tests instead
  of arithmetic against NULL (which silently never matched): `x >= .` →
  `x IS NULL`, `x < .` → `x IS NOT NULL`, `x > .` → `FALSE`, `x <= .` → `TRUE`.
  Works with identifier, parenthesized, function-call, subscripted, and numeric
  operands on either side. A bare `.` inside a string literal is left alone.
- Nested multi-line `foreach`/`forvalues` loops expand correctly; previously
  the inner body was silently dropped, emitting broken SQL.
- `display 1 + 1` compiles as one expression (`CAST(1 + 1 AS VARCHAR)`) instead
  of splitting on whitespace. Note: consecutive bare tokens are now parsed as a
  single expression, so `display a b` is one token, not two concatenated casts.
- `_n` (observation index) and `_N` (observation count) are no longer
  conflated: `local i = _n + 1` raises a clear error (no row context) instead
  of emitting invalid `SET VARIABLE` SQL; `local i = _N + 1` still compiles to
  a count subquery.
- Malformed numbers (`undo abc`, `forvalues i = a/b`, `label define l x "t"`)
  raise a clean dodo error instead of aborting the process with an unhandled
  `std::invalid_argument`.
- `local x =` (empty right-hand side) clears the macro instead of emitting
  invalid SQL; `scalar s =` errors.
- `undo` may not cross an active `preserve` checkpoint (previously `restore`
  could then grow the chain with empty CTE steps, i.e. broken SQL).
- `keep/drop/list ... in <range>` raises "not supported" instead of silently
  mistranslating the range into a column list.
- Scalar names are no longer substituted inside string literals
  (`scalar region = 5` no longer turns `"region is here"` into `"5 is here"`).
- The explicit `scalar(name)` form resolves the scalar even when a data column
  shares its name (see docs/VARIABLE_SUBSTITUTION.md for the compile-time
  scalar-vs-column precedence deviation).
- DuckDB reserved words (`qualify`, `pivot`, `unpivot`, `lateral`, `semi`,
  `anti`, `asof`, …) used as column names are now quoted.

### Changed — extension behavior

- **Plain SQL lines in a mixed dodo/SQL batch are no longer macro-expanded.**
  When the parser override processes a batch, lines that are not dodo commands
  are parsed as the original SQL text, so `$ident`, `${...}`, and backticks
  inside SQL (including string literals) pass through untouched. Previously
  they were substituted or deleted. Scripts that deliberately relied on dodo
  globals inside plain SQL (e.g. `SELECT * FROM $mytable`) must now assign via
  a dodo command first or use `getvariable()` directly.
- Statement splitting on `;` (and line splitting) is quote-aware: a `;` inside
  a string literal no longer splits the statement.

### Build / CI

- `MainDistributionPipeline.yml` builds/ships `dodo` (was the template
  leftover name `waddle`, which broke artifact discovery).
- The dodoc release workflow compiles the dta reader sources and initializes
  the `duckdb-dta` submodule (previously every release job failed to compile);
  release binaries take their version from the git tag.
- New fast CI job (`core-tests.yml`): builds dodoc and runs the golden-SQL
  suite plus lexer unit tests on every push, no DuckDB build required
  (`make test-core`).
- `src/include/dodo_core.hpp` is a proxy header again (was a stale forked copy
  of the real header — a latent ODR violation).

### Known issues

- `scalar list` output is dropped by dodoc (command classification; fix
  planned with the command registry). Tracked by the
  `kb_scalar_list_dropped` golden case.
