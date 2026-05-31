# Plan: Value Labels and ENUM Types

## Problem

Stata value labels are display-only: `keep if gender == 2` works because the data is always numeric. DuckDB ENUMs are type-safe: `gender = 2` fails because ENUM != INTEGER.

## Current Behavior

- `read_dta('file.dta')` — returns numeric types (default, `value_labels=false`)
- `read_dta('file.dta', value_labels=true)` — returns ENUM types
- `use "file.dta"` — uses `read_dta` without `value_labels=true`, so data stays numeric
- Value label metadata is stored in `state.column_labels` and `state.value_label_defs` for codebook/describe display

This is correct: the dodo layer never converts to ENUM, preserving Stata's numeric semantics. Commands like `keep if sector == 4` work because `sector` remains an integer.

## Future Enhancement: `enum_code()` Wrapping

If we ever want dodo to use ENUMs (for nicer display in `list`), we'd need to wrap ENUM columns with `enum_code()` when they appear in expressions:

### Approach

In `TranslateExpression()`, for each column that has a value label:
1. Parse the expression to identify COLUMN_REF nodes
2. If the COLUMN_REF is an operand of a comparison (`==`, `!=`, `>`, `<`, etc.), arithmetic (`+`, `-`, `*`, `/`), or function call — wrap with `enum_code(col)`
3. If the COLUMN_REF is "naked" (bare select, GROUP BY key, ORDER BY) — leave as-is

### Complexity

This requires either:
- **C++ AST walking** via DuckDB's `Parser::ParseExpressionList()` and tree traversal
- **Regex heuristic** (fragile): detect `col ==`, `col !=`, `col >`, `col +` patterns

The AST approach is robust but heavy. The regex approach breaks on nested expressions.

### Recommendation

**Don't use ENUMs in the dodo layer.** Keep numeric types for data, store labels as metadata. This matches Stata semantics exactly. The `value_labels=true` option remains available for direct SQL users who want ENUM types and understand the tradeoffs.

If Stata-style label display is desired in `list` output, implement it as a formatting step in the `list` command rather than as a type change.
