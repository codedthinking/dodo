#!/usr/bin/env bash
# E2E test: dodoc compiler output
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
DODOC="$PROJECT_DIR/build/dodoc/dodoc"
TMPDIR="${TMPDIR:-/tmp}"
FAILURES=0

if [ ! -x "$DODOC" ]; then
    echo "FAIL: dodoc binary not found at $DODOC"
    exit 1
fi

echo "=== dodoc compiler e2e tests ==="

# Helper: run dodoc on a .do string, check output
check() {
    local name="$1"
    local input="$2"
    local pattern="$3"
    local anti_pattern="${4:-}"

    local dofile="$TMPDIR/dodoc_test_$$.do"
    echo "$input" > "$dofile"
    local output
    output=$("$DODOC" "$dofile" 2>&1) || true
    rm -f "$dofile"

    local ok=true
    if ! echo "$output" | grep -qF -- "$pattern"; then
        ok=false
        echo "  FAIL: $name — expected pattern not found: $pattern"
        echo "    output: $output"
    fi
    if [ -n "$anti_pattern" ] && echo "$output" | grep -qF -- "$anti_pattern"; then
        ok=false
        echo "  FAIL: $name — unexpected pattern found: $anti_pattern"
        echo "    output: $output"
    fi
    if $ok; then
        echo "  PASS: $name"
    else
        FAILURES=$((FAILURES + 1))
    fi
}

# --- use emits CREATE TABLE before CTE ---
check "use emits CREATE TABLE" \
    'use "test/data/firms.csv", clear
keep if year == 2018' \
    "CREATE OR REPLACE TABLE dodo._current"

check "CTE references dodo._current" \
    'use "test/data/firms.csv", clear
keep if year == 2018' \
    "FROM dodo._current"

# --- lazy use does NOT create table ---
check "lazy use inlines read" \
    'use "test/data/firms.csv", clear lazy
keep if year == 2018' \
    "read_csv('test/data/firms.csv')" \
    "CREATE OR REPLACE TABLE"

# --- save: no duplicate CTE ---
check "save emits single COPY TO" \
    'use "test/data/firms.csv", clear
keep if year == 2018
save "out.csv"' \
    "COPY ("

# save should NOT have a second statement after COPY TO
# Count semicolons: CREATE SCHEMA (1) + CREATE TABLE (2) + COPY TO (3) = 3 total
output=$("$DODOC" <(echo 'use "test/data/firms.csv", clear
keep if year == 2018
save "out.csv"') 2>&1) || true
stmt_count=$(echo "$output" | grep -c ';')
if [ "$stmt_count" -eq 3 ]; then
    echo "  PASS: save: exactly 3 statements (no duplicate CTE)"
else
    echo "  FAIL: save: expected 3 statements, got $stmt_count"
    echo "    output: $output"
    FAILURES=$((FAILURES + 1))
fi

# --- CTE comments match commands ---
check "CTE comment for keep" \
    'use "test/data/firms.csv", clear
keep if year == 2018
generate rev2 = revenue * 2' \
    "-- [source] keep if year == 2018"

check "CTE comment for generate" \
    'use "test/data/firms.csv", clear
keep if year == 2018
generate rev2 = revenue * 2' \
    "-- [source] generate rev2 = revenue * 2"

# Comments should not be off-by-one: _s1 should be keep, not use
output=$("$DODOC" <(echo 'use "test/data/firms.csv", clear
keep if year == 2018
generate rev2 = revenue * 2') 2>&1) || true
# Extract the comment for _s1
s1_comment=$(echo "$output" | grep -B1 "_s1 AS" | head -1)
if echo "$s1_comment" | grep -qF "keep if year == 2018"; then
    echo "  PASS: _s1 comment is keep (not use)"
else
    echo "  FAIL: _s1 comment off-by-one: $s1_comment"
    FAILURES=$((FAILURES + 1))
fi

if [ "$FAILURES" -gt 0 ]; then
    echo "=== $FAILURES test(s) FAILED ==="
    exit 1
fi

echo "=== All dodoc tests passed ==="
