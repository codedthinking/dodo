# dodo refactor plan — implementation guide

*Companion to `docs/CODE_REVIEW.md` (same branch). That document is the evidence; this one is the
work order. It is written to be executed top-to-bottom by a coding agent without additional
context. Every task lists the files to touch, the concrete steps, and a "done when" check.*

**Deliverables kept green throughout:**
1. **dodo core** — `src/core/` (DuckDB-independent translation engine)
2. **dodoc** — `src/cli/dodoc.cpp` (standalone .do → SQL compiler)
3. **dodo extension** — `src/extension/` (DuckDB parser/operator extension)

**Ground rules for the implementer**
- Work phase by phase; each phase ends with all tests passing and a commit.
- Never change generated-SQL behavior without adding/updating a golden test in the same commit.
- The core must stay DuckDB-free: nothing under `src/core/` may include a `duckdb/` header.
- Out of scope for this plan: `rhistory-corpus/` and `scripts/bootstrap-template.py` fixes,
  implementing `in` ranges, `r()`/`e()` stored results (M14c), and website/docs work.

---

## Phase 0 — Safety net and build hygiene

Everything later depends on 0.1. Tasks 0.2–0.6 are independent one-file fixes; do them in any
order.

### 0.1 Golden-SQL test harness for the core
**Goal:** fast, DuckDB-free regression tests: feed `.do` text into the core, snapshot the SQL.
**Files:** new `test/golden/` (cases + expected output), new `test/golden/run_golden.sh`,
`Makefile` (new `test-core` target), `.github/workflows/` (new small job or step).
**Steps:**
1. Build `dodoc` (`make dodoc`) — it already exposes the whole pipeline
   (`ProcessLines` → side-effect SQL → final chain query).
2. Each case is a pair: `test/golden/NAME.do` and `test/golden/NAME.expected.sql`. Runner compiles
   with `dodoc --no-terminal` off (default) and `diff`s output. Normalize by disabling nothing —
   dodoc output is already deterministic text.
3. Error cases: `test/golden/NAME.do` + `NAME.expected.err`; runner asserts non-zero exit and that
   stderr contains the expected substring.
4. Seed with ~25 cases covering: `use`/`keep`/`drop`/`generate`/`replace`/`rename`/`sort`/`egen`/
   `collapse`/`merge`/`append`/`reshape long`/`duplicates`/`expand`/`tabulate`/`summarize`/
   `count`/`save`/`export`/`local`/`global`/`scalar`/`foreach`/`forvalues`/`bysort`/`display`,
   plus every confirmed bug in CODE_REVIEW §2 as a (currently failing or currently-wrong-output)
   case marked `# KNOWN-BAD` in a header comment. As phases land, flip the expected files to the
   corrected output and remove the marker.
5. `make test-core` runs the golden suite; add it to CI (a plain ubuntu job: checkout with
   submodules, `make dodoc`, `make test-core` — no DuckDB build needed).
**Done when:** `make test-core` passes locally and in CI; intentionally breaking a translation
makes it fail.

### 0.2 Replace the stale header with a proxy
**Files:** `src/include/dodo_core.hpp`.
**Steps:** Replace the entire file content with the same two-line proxy pattern used by its
siblings:
```cpp
// Proxy header — forward to actual location
#include "../core/dodo_core.hpp"
```
**Done when:** extension build (`make`) and `make dodoc` both compile; `grep -c SymbolEntry
src/include/dodo_core.hpp` returns 0.

### 0.3 Fix the extension name in the distribution pipeline
**Files:** `.github/workflows/MainDistributionPipeline.yml`.
**Steps:** change both `extension_name: waddle` occurrences (lines ~21, ~29) to `dodo`.
**Done when:** the distribution workflow's test step finds `dodo.duckdb_extension`.

### 0.4 Make the dodoc release workflow build via make, from a real checkout
**Files:** `.github/workflows/dodoc-release.yml`.
**Steps:**
1. Add `submodules: recursive` to every `actions/checkout` step (only the `duckdb-dta` submodule
   is actually needed for dodoc; `submodules: true` + explicit
   `git submodule update --init duckdb-dta` is faster than recursive — prefer that).
2. Replace the raw compiler invocations with `make dodoc CXX=...` on unix; on Windows keep a
   direct compile line but add `/Iduckdb-dta\src\include` and `duckdb-dta\src\dta_reader.cpp`
   (the Makefile is not usable under MSVC).
3. Version single-source: pass `-DDODOC_VERSION_OVERRIDE=\"${TAG#dodoc-v}\"` and change
   `src/cli/dodoc.cpp` to
   `#ifdef DODOC_VERSION_OVERRIDE / static const char *DODOC_VERSION = DODOC_VERSION_OVERRIDE; /
   #else ... #endif`.
4. Security hardening in the same file: move every `${{ steps.version.outputs.tag }}` /
   `${{ github.ref* }}` used inside `run:` into `env:` variables and reference `"$TAG"`; pin
   `softprops/action-gh-release` and `ilammy/msvc-dev-cmd` to full commit SHAs; add
   `persist-credentials: false` to checkouts that don't push, and run the cross-compiled
   macos-x86_64 binary under Rosetta-less sanity (`file` check + `--version` via `arch -x86_64`
   on the arm64 runner if available; otherwise at minimum assert the artifact is non-empty and
   Mach-O x86_64).
**Done when:** a dry-run of the workflow's build steps succeeds locally (`act` not required —
replicate the exact commands in a shell) and the workflow lints clean (`actionlint` if available).

### 0.5 CMake hygiene
**Files:** `CMakeLists.txt`.
**Steps:** wrap the `add_executable(dodoc ...)` block in `if(NOT EMSCRIPTEN AND NOT MINGW)`;
convert the global `include_directories(...)` to `target_include_directories` on the extension
targets if `build_static_extension`/`build_loadable_extension` expose the target names (they
create `${EXTENSION_NAME}` and `${LOADABLE_EXTENSION_NAME}`); otherwise leave the global includes
but scope them with a comment explaining why.
**Done when:** normal extension build still compiles; a wasm configure (if run) no longer creates
the dodoc target.

### 0.6 Delete dead CI
**Files:** `.github/workflows/ExtensionTemplate.yml` (delete). Leave `scripts/extension-upload.sh`
but add a header comment noting it is currently unused (deploy job wiring is a product decision,
not part of this plan).

---

## Phase 1 — Point correctness fixes in the core

Each task is independent, small, and must add golden cases. All file references are
`src/core/dodo_core.cpp` unless noted.

### 1.1 SQL string escaping everywhere
**Files:** `src/core/string_utils.hpp`, `src/core/dodo_core.cpp`,
`src/extension/dodo_extension.cpp`, `src/cli/dodoc.cpp` (no change expected, verify).
**Steps:**
1. Add to `str::`:
```cpp
inline std::string SqlString(const std::string &s) {   // -> 'escaped'
    std::string out = "'";
    for (char c : s) { out += c; if (c == '\'') out += '\''; }
    out += "'";
    return out;
}
```
2. Replace every hand-rolled quote-doubling loop and every raw `"'" + x + "'"` paste with
   `str::SqlString(x)`. Known sites: `FileReadFunction` (all four read functions), `import`
   (`read_csv('...')`), `merge` (via `FileReadFunction`), `save`/`export`
   (`TO '<file>'`), `label list` (labels **and** column names), `history` VALUES,
   `describe`/`codebook` CASE arms (labels and column names), `summarize` header label,
   `assert` message, `scalar list` values, `reshape long` stub literals, and in the extension
   `BuildHistorySQL`. Grep check: `grep -n "find('\\\\''" ` and `grep -n '"'"'"'" + '` style
   pastes should be gone.
3. `FileReadFunction`: unknown extensions still return the raw name (table reference) — keep, but
   `Trim` it and route through `QuoteIdent` when it is a bare identifier is **not** desired
   (schema-qualified names); leave as-is with a comment.
**Golden:** `use "it's data.csv"` now emits `read_csv('it''s data.csv')`;
`label variable x "it's"` + `label list` round-trips.

### 1.2 Checked number parsing
**Steps:**
1. Add file-local helpers:
```cpp
static int ParseIntStrict(const string &s, const string &ctx);
static double ParseDoubleStrict(const string &s, const string &ctx);
```
   Both `Trim`, reject empty, use `std::from_chars`/`stod` with full-consumption check, and throw
   `DodoException(ctx + ": expected a number, got '" + s + "'")`.
2. Replace every `std::stoi`/`std::stod`: `undo`, `redo`, `ParseNumlist` (both patterns),
   `label define` value, `EvaluateMacroFunction` (`word #`, `label # maxlen` — keep their existing
   catch-to-default behavior where present), `TokenizeExpr` number literal, the `L/F/D` and
   subscript regex handlers.
**Golden:** `undo abc` → error `expected a number`; `forvalues i = a/b { ... }` → clean error;
`label define l x "t"` → clean error.

### 1.3 Missing-value comparison semantics
**Location:** `TranslateExpression`, insert a pass **before** the bare-`.` → `NULL` rewrite.
**Steps:** detect `<expr-atom> <op> .` and `. <op> <expr-atom>` where `.` is the bare missing
token (same boundary rules as the existing pass). Rewrites (Stata: missing sorts above every
number):
| pattern | SQL |
|---|---|
| `x == .`, `x = .` | `x IS NULL` |
| `x != .`, `x ~= .` | `x IS NOT NULL` |
| `x >= .` | `x IS NULL` |
| `x > .`  | `FALSE` |
| `x < .`  | `x IS NOT NULL` |
| `x <= .` | `TRUE` |
Left-hand side: capture the immediately preceding operand (identifier, closing paren group, or
number) — a token scan, not regex, since parens must balance. Mirror for `. <op> x`.
**Golden:** `replace x = 0 if x >= .` → `CASE WHEN x IS NULL THEN 0 ELSE x END`;
`keep if x < .` → `WHERE x IS NOT NULL`.

### 1.4 Stop conflating `_n` and `_N` at compile time
**Location:** `FindRuntimeToken`.
**Steps:** make the `_N` scan case-sensitive on the original string (scan `expr`, not `lower`).
Add a separate case-sensitive `_n` scan that returns a distinct token `"_n"`; in the three
callers (`local`, `global`, `scalar`), on `"_n"` throw
`"... contains _n (observation index), which has no value outside a row context; use 'generate'"`.
**Golden:** `local i = _N + 1` (with data) → `SET VARIABLE ... count(*) ... + 1`;
`local i = _n + 1` → clean error.

### 1.5 Empty right-hand sides and `.front()` UB
**Location:** `local` and `global` handlers.
**Steps:** after computing `value`, if empty: store `{LITERAL, ""}` and return (Stata: assigning
nothing clears the macro). Guard every `value.front()`/`value.back()` with `!value.empty()`
(there are two sites per handler). Audit the whole file for other unguarded `.front()`/`.back()`:
`ExtractQuotedString` and `expr.front()` in `scalar` are already size-guarded — verify.
**Golden:** `local x =` then `display "[`x']"` → `SELECT '[]' AS display`, no `SET VARIABLE`.

### 1.6 `restore` after `undo` must not grow the chain
**Location:** `undo` and `restore` handlers.
**Steps:** in `undo`, if `preserve_checkpoint >= 0` and the undo would shrink
`cte_steps.size()` below `preserve_checkpoint`, throw
`"cannot undo past an active 'preserve'; run 'restore' first"`. (Keep `restore` as-is; add an
internal `assert`-style check that `cte_steps.size() >= preserve_checkpoint`.)
**Golden:** `preserve` → `undo 2` (crossing the checkpoint) → error.

### 1.7 Reject `in` ranges instead of mistranslating
**Location:** `TokenizeCommand` or per-command; simplest at `TokenizeCommand` consumer level.
**Steps:** in `keep`, `drop`, `list`: after splitting arguments, if the first token is `in`
(case-insensitive) or arguments match `in <numlist>` shape, throw
`"'in' row ranges are not supported; use 'if' with _n"`.
**Golden:** `keep in 1/10` → error, not `SELECT "in", "1/10"`.

### 1.8 Nested multi-line loops
**Location:** `ProcessLines` / `execute_loop` / `AccumulateBraceBlock`.
**Steps:** restructure loop-body execution to reuse one mechanism:
1. Add `static vector<string> AccumulateBraceBlockFromVector(const vector<string> &lines, idx_t
   &pos)` — same logic as `AccumulateBraceBlock` but reading from a vector (or better: make
   `AccumulateBraceBlock` take a `LineReader` and build a vector-backed reader; the function
   already takes a `LineReader` — reuse it directly with a lambda over the body vector).
2. In `execute_loop`, when a body line starts a `foreach`/`forvalues` **without** an inline `}`:
   build a vector-backed `LineReader` over the remaining body lines, call
   `AccumulateBraceBlock(reader)` to get the inner body, advance the outer index by the number of
   consumed lines, and recurse.
3. If the inner body cannot be closed (unbalanced), the existing "Unterminated brace block" error
   fires — that replaces today's silent garbage.
**Golden:** the nested two-level loop from CODE_REVIEW §2.5 produces four steps
`v13, v14, v23, v24` with correct expressions.

### 1.9 `display` expression tokenization
**Location:** `display` handler.
**Steps:** replace the whitespace-delimited "bare expression" scan: accumulate a bare-expression
token until (at paren depth 0) the next `"` or a `%fmt` token that *starts* a whitespace-separated
word — i.e., expressions may contain spaces and operators. Concretely: scan forward; a `%` only
terminates the expression if preceded by whitespace; a `"` always terminates it.
**Golden:** `display 1 + 1` → `SELECT CAST(1 + 1 AS VARCHAR) AS display`;
`display "n=" _N %5.2f 1.5` keeps working.

### 1.10 Re-entrant expression parser
**Location:** the `expr_pos` static and the four `ParseExpr*` functions.
**Steps:** introduce `struct ExprParser { const vector<ExprToken> &tokens; idx_t pos = 0; ... }`
with the parse functions as members (or pass `idx_t &pos` through). Delete the file-scope static.
**Done when:** no function-scope/file-scope mutable statics remain in `src/core/`
(`grep -n "^static idx_t\|^static int\|^static bool" src/core/dodo_core.cpp` shows only constants).

### 1.11 DuckDB keyword list
**Files:** `src/core/string_utils.hpp`.
**Steps:** extend `KEYWORDS` with DuckDB reserved words missing today — at minimum: `qualify`,
`semi`, `anti`, `asof`, `positional`, `pivot`, `unpivot`, `describe`, `summarize`, `show`,
`install`, `load`, `macro`, `columns`, `lateral`, `fetch`, `interval`, `grouping`. (Source of
truth: `duckdb_keywords()` — dump once from a DuckDB shell and reconcile; keep the list sorted.)
**Golden:** `keep pivot lateral` quotes both identifiers.

---

## Phase 2 — One lexer (highest-leverage change)

### 2.1 `do_lexer` module
**Files:** new `src/core/do_lexer.hpp` + `src/core/do_lexer.cpp`; add to
`CMakeLists.txt` (both targets), `Makefile` (`DODOC_SOURCES`/`HEADERS`).
**API (keep it small):**
```cpp
namespace dodo::lex {
// Strip * line, // (only when at start or preceded by whitespace), and /* */ comments.
// Quote-aware: nothing inside "..." or `"..."' is a comment. Tracks block-comment state.
std::string StripComments(const std::string &line, bool &in_block_comment,
                          bool &line_continued /* trailing /// */);

// Split on a delimiter, ignoring delimiters inside "..." strings and (...) groups.
std::vector<std::string> SplitOutsideQuotes(const std::string &s, char delim);

// Find a keyword (e.g. " if ", ":") outside quotes/parens; npos if absent.
idx_t FindKeywordOutsideQuotes(const std::string &s, const std::string &kw);

// Tokenize into words / quoted strings / operators, preserving quotes on STRING tokens.
struct Token { enum Kind { WORD, STRING, OP, LPAREN, RPAREN, LBRACE, RBRACE } kind; std::string text; };
std::vector<Token> Tokenize(const std::string &s);

// Quote-aware brace scan used for loop accumulation: net brace depth of a line.
int BraceDelta(const std::string &line);
}
```
Implement each with a single character-walk state machine (states: normal, in `"..."`, in
compound `` `"..."' ``). Unit-test the module directly (small `test/golden/lexer_*.do` cases plus
a dedicated C++ test main under `test/unit/` run by `make test-core`).

### 2.2 Rewire call sites
Replace, in order, each ad-hoc implementation (golden suite green after every one):
1. `ProcessLines` comment stripping (fixes `//` in strings — CODE_REVIEW §2.1) → `StripComments`.
2. `AccumulateBraceBlock` comment stripping and brace counting → `StripComments` + `BraceDelta`.
3. `TokenizeCommand` option-comma and `if` detection → `SplitOutsideQuotes` /
   `FindKeywordOutsideQuotes` (behavior is already close; unify to one implementation).
4. `SplitTokens` → `Tokenize` (WORD/STRING projection).
5. `dodo_parser_override` statement split on `;` and `\n` → `SplitOutsideQuotes` (fixes §3.4).
6. `display` handler scanning (finish 1.9 on top of the lexer).

### 2.3 Macro expansion correctness
**Location:** `ExpandMacros` + the scalar substitution pass + `dodo_parser_override`.
**Steps:**
1. Keep backtick/`$` expansion active inside `"..."` strings — that is Stata behavior — but make
   the **bare-scalar substitution pass skip string literals** (walk with the lexer's quote state).
   Bare scalar names in strings are not substituted by Stata.
2. Document the remaining known deviation in `docs/VARIABLE_SUBSTITUTION.md`: when a scalar and a
   column share a name, dodo resolves to the scalar (Stata resolves to the column). Emit the
   `scalar(name)` explicit form in the docs as the disambiguator (already supported via macro
   functions? verify; if not, add `scalar(name)` recognition in `TranslateExpression` that maps to
   the symbol table and bypasses column resolution).
3. In `dodo_parser_override`: macro-expand a line, test `IsDodoCommand` on the expanded text — but
   if it is **not** a dodo command, parse the **original unexpanded** line as SQL (fixes plain-SQL
   corruption, CODE_REVIEW §2.2/§3.1, while preserving Stata-style command construction like
   `` `cmd' price``).
**Golden:** `generate s = "don`t"` (backtick, no closing quote-mark) survives; scalar named like a
string word is not replaced inside literals; mixed batch `local x 1; SELECT '$foo' AS c` leaves
the SQL literal intact.

---

## Phase 3 — State model and execution paths (extension + core API)

### 3.1 Typed command results (replaces string markers)
**Files:** `src/core/dodo_core.hpp/.cpp`, `src/extension/dodo_extension.cpp`,
`src/cli/dodoc.cpp`.
**Steps:**
1. Introduce:
```cpp
struct CommandResult {
    std::vector<std::string> statements;   // SQL to run, in order
    enum class Kind { OK_STATUS, RESULT_QUERY, SIDE_EFFECT, PIVOT_RESTART } kind;
    std::string pivot_table;               // Kind::PIVOT_RESTART only
};
CommandResult ProcessCommandV2(const DodoCommand &cmd, DodoState &state);
```
   Implement `ProcessCommandV2` as the real function; keep `ProcessCommand` as a thin
   compatibility shim that joins `statements` with `"; "` (and re-encodes the pivot marker) until
   all callers are migrated, then delete the shim and the `__PIVOT__` / `||STATE||` protocol.
2. Migrate `ProcessLines`: collect `RESULT_QUERY` and `SIDE_EFFECT` statements structurally; drop
   the `find("SELECT 'OK' AS status")` filter.
3. Migrate `dodoc.cpp`: filter on `kind == OK_STATUS` instead of substring; track "the final chain
   was consumed" only when the **last** emitted statement was a `save`/`export` of the current
   chain (the core can set a flag on the result: `bool consumes_chain`). This fixes both dodoc
   output bugs (CODE_REVIEW §4.1) — `clear` cleanup DDL is emitted, and a mid-script `save` no
   longer suppresses the final query.
4. Migrate `dodo_parser_override` and `dodo_plan`.
**Golden:** `use a.csv / generate x = 1 / clear / use b.csv / keep if x` compiled by dodoc emits
the `DROP TABLE`/`DROP SCHEMA` cleanup between datasets; `use / save backup.csv / keep if ...`
emits both the COPY and the final chain query.

### 3.2 Remove the global; make shared state thread-safe
**Files:** `src/extension/dodo_extension.cpp`, `src/extension/dodo_extension.hpp`.
**Steps:**
1. Delete `g_dodo_state`. The three option callbacks capture
   `std::weak_ptr<DodoStateInfo>(shared_state)` by value; lock and no-op if expired.
2. Add `std::mutex state_mutex` to `DodoStateInfo`; take `std::lock_guard` at the top of
   `dodo_parser_override`, `dodo_plan`, and inside each option callback. (Per-`ClientContext`
   session state is a product decision — the DuckDB-UI live-view feature depends on one shared
   session per database — so shared-but-locked is the correct scope here. Leave a comment saying
   so.)
**Done when:** `grep -n g_dodo_state src/` is empty; a two-connection stress test (see 3.5) runs
clean under TSan if a sanitizer build is available, otherwise passes functionally.

### 3.3 Unify the two execution paths
**Files:** `src/extension/dodo_extension.cpp`.
**Steps:**
1. Extract the per-command translation block of `dodo_parser_override` (macro expansion →
   `IsDodoCommand` → `TokenizeCommand` → `ProcessCommandV2` → pivot handling → pending_sql drain →
   history/live-view injection) into one function
   `TranslateDodoLine(const string &line, DodoStateInfo &state, vector<unique_ptr<SQLStatement>> &out)`.
2. Rewrite `dodo_plan` to call the same function and — because the plan path can bind only one
   statement — throw a clear `BinderException` telling the user to run the command through the
   normal path **if** translation yields more than one statement. Then instrument: add a temporary
   test (`test/sql/dodo_paths.test`) issuing every command category and assert behavior; if the
   override handles 100% of commands in practice (expected, since
   `allow_parser_override_extension=fallback` is set and every dodo command fails the standard
   parser), delete `dodo_parse`/`dodo_plan`/`DodoOperatorExtension`/`DodoBindState` entirely and
   keep only the override. Prefer deletion — one path, one behavior.
**Done when:** every `test/sql/*.test` passes with the parse/plan path removed (or, if any command
provably needs it, the plan path shares `TranslateDodoLine` and a comment documents which commands
use it).

### 3.4 Stop destroying state before the source is known
**Files:** `src/core/dodo_core.cpp` (`use`, `import`).
**Steps:** before `state.Clear()`, if the source resolves to a file-read function
(`is_file == true`) and the path has no glob characters (`*?[`) and no URL scheme (`://`), check
`std::ifstream(source).good()`; on failure throw `Cannot open file: <source>` **before** any state
mutation. (Full transactional parse-time state is not achievable — execution happens later — but
this closes the common data-loss case: a typo'd filename wiping the session.)
**Golden:** `use good.csv` → `use missing.csv` errors and a subsequent `count` still queries
`good.csv`'s chain.

### 3.5 Concurrency smoke test
**Files:** new `test/sql/dodo_concurrent.test` if the sqllogictest runner supports
`concurrentloop` (it does: `concurrentloop i 0 8`), issuing interleaved `generate`/`count` on
separate connections. Accept coarse behavior (commands serialized by the mutex); the assertion is
"no crash, no interleaved-corrupt chain".

---

## Phase 4 — Command registry (mechanical, do after Phase 3)

**Files:** `src/core/dodo_core.cpp` → split into `src/core/commands/` (new files:
`cmd_data.cpp` — use/import/save/export/append/merge; `cmd_transform.cpp` —
keep/drop/generate/replace/rename/sort/order/egen/collapse/reshape/duplicates/expand/mvencode;
`cmd_session.cpp` — clear/undo/redo/history/preserve/restore/xtset/show/compress;
`cmd_macros.cpp` — local/global/scalar/macro/tempvar/tempname/tempfile/display/label;
`cmd_terminal.cpp` — list/count/head/tail/describe/summarize/tabulate/levelsof/assert), plus
`src/core/command_registry.hpp`.
**Steps:**
1. Define:
```cpp
struct CommandDef {
    const char *name;
    CommandKind kind;                 // TRANSFORMATION, TERMINAL, SIDE_EFFECT, SESSION, MACRO
    bool needs_data;                  // replaces the ad-hoc HasData() gate
    bool conflicts_with_sql;          // drives parser-override detection
    CommandResult (*handler)(const DodoCommand &, DodoState &);
};
const std::vector<CommandDef> &CommandTable();
```
2. Derive `DODO_COMMANDS`, `IsTransformationCommand`, `IsSideEffectCommand`, **and** the
   keyword checks inside `dodo_parser_override` (`has_conflict_commands`, `has_macro_commands`)
   from this single table. Delete the three hand-maintained lists.
3. Move each `if (cmd.command == ...)` block verbatim into its handler function — no behavior
   changes in this phase; the golden suite is the proof.
4. Update `CMakeLists.txt` (`EXTENSION_SOURCES`) and `Makefile` (`DODOC_SOURCES`).
**Done when:** golden suite and `test/sql/` unchanged; `dodo_core.cpp` is < 800 lines
(tokenizer/expression/loop machinery only); adding a command touches exactly one file plus the
table.

---

## Phase 5 — Deterministic row order

The largest semantic change; land behind golden-test updates in one PR.

**Design:** a hidden `BIGINT` column `_dodo_ord` threaded through the chain.
1. **Creation:** `use`/`import` append `row_number() OVER () AS _dodo_ord` in the *materialization*
   statement (`CREATE TABLE dodo._current AS SELECT *, row_number() OVER () AS _dodo_ord FROM ...`)
   — deterministic because it is evaluated once. Lazy mode gets it on the first chain step.
2. **`sort`:** becomes
   `SELECT * REPLACE (row_number() OVER (ORDER BY <keys> <dir>, _dodo_ord) AS _dodo_ord) FROM prev`
   (stable sort; no ORDER BY step in the chain at all — order is now data, which also removes the
   current false dependence on CTE order preservation).
3. **Consumers:** every window the translator emits gains a default order:
   `_n` → `ROW_NUMBER() OVER (PARTITION BY ... ORDER BY _dodo_ord)`; `_N` unchanged; subscripts
   `var[_n±k]` → LAG/LEAD ordered by `_dodo_ord` (bysort sort keys, when given, come first, then
   `_dodo_ord` as tiebreak); `tail`, `duplicates drop varlist` (keep-first), running aggregates —
   all `ORDER BY _dodo_ord`.
4. **Producers of new shapes:** `collapse` groups drop `_dodo_ord` and re-emit
   `row_number() OVER (ORDER BY <by_cols>)`; `merge`/`append`/`reshape` re-emit it the same way
   after the operation (master order first for merge: order by master `_dodo_ord` NULLS LAST, then
   using order).
5. **Terminals:** every terminal SELECT and every `save`/`export`/`live view` adds
   `EXCLUDE (_dodo_ord)`; `list`/`head`/`tail` add `ORDER BY _dodo_ord` so displayed order is the
   Stata order.
6. **Column ops:** `keep <vars>` must append `_dodo_ord` to the projection; `drop`/`rename` must
   refuse to touch `_dodo_ord` (name is reserved; error if user data contains it at `use` time —
   detect via the read's DESCRIBE? Not available at compile time; instead pick a
   collision-improbable name and document it).
**Steps for the implementer:** implement behind a state flag `bool ordered = true` set at `use`;
update all goldens (this rewrites most expected SQL — mechanical); add determinism goldens:
`sort price / generate id = _n / sort name / list` yields `id` values pinned to the price order.
**Done when:** `test/sql/dodo_panel.test` and expression tests pass with windows ordered; a new
`dodo_order.test` asserts `sort`+`_n` stability on `test/data/generate_large.sql` data across
`PRAGMA threads=8`.

---

## Phase 6 — Performance and polish

1. **Static regexes:** in `TranslateExpression` and `ExpressionUsesWindowFunctions`, hoist every
   `std::regex` into `static const std::regex` (they are all fixed patterns). Verify with a quick
   10k-line synthetic do-file compile (`time dodoc big.do` before/after; expect >5× on that path).
2. **Incremental history:** replace `BuildHistorySQL`'s full rebuild with:
   `CREATE TABLE IF NOT EXISTS dodo._history(step_id INT, command VARCHAR, undone BOOL)` at
   materialization; after each new step `INSERT INTO dodo._history VALUES (...)`; on
   `undo`/`redo` `UPDATE dodo._history SET undone = ...  WHERE step_id = ...`; on `clear` drop it
   (already handled). Track in `DodoState` how many steps are persisted.
3. **Live view:** skip `CREATE OR REPLACE VIEW` when the chain didn't change (macro-only
   commands).
4. **`summarize, detail`:** build the query with a single named CTE for the data
   (`WITH _d AS (<chain>) SELECT ... FROM _stats, LATERAL(... FROM _d) ...`) instead of inlining
   the chain three times.
5. **`ExpandMacros`:** single-pass rescan of only the substituted region (keep the fixpoint loop
   as a safety net with `MAX_DEPTH`), and skip the scalar pass when `scalar_symbols.empty()`.
6. **README version badge** → 0.3.0 (or wire it to the release workflow).

---

## Suggested commit sequence

| # | Contents | Risk |
|---|---|---|
| 1 | Phase 0 (harness + header proxy + CI names/build) | none — infra |
| 2 | 1.1–1.2 (escaping, checked parsing) | low |
| 3 | 1.3–1.7 (missing values, `_n/_N`, guards, `in`) | low, semantics documented |
| 4 | 1.8–1.11 (loops, display, reentrancy, keywords) | low |
| 5 | Phase 2 (lexer + rewires + macro fixes) | medium — biggest review surface |
| 6 | 3.1 (CommandResult) + dodoc fixes | medium |
| 7 | 3.2–3.5 (global removal, single path, use-guard) | medium |
| 8 | Phase 4 (registry split) | low — mechanical, golden-pinned |
| 9 | Phase 5 (`_dodo_ord`) | high — own PR, all goldens updated |
| 10 | Phase 6 (perf) | low |

Every commit: `make dodoc && make test-core` green; commits 5–10 also `make` (extension) +
`test/sql/` green.
