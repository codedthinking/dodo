#!/usr/bin/env bash
# E2E test: DuckDB CLI with dodo extension
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
DUCKDB="$PROJECT_DIR/build/release/duckdb"

if [ ! -x "$DUCKDB" ]; then
    echo "FAIL: DuckDB binary not found at $DUCKDB"
    exit 1
fi

FAILURES=0

run_test() {
    local name="$1"
    local input="$2"
    local expected="$3"

    actual=$(echo "$input" | "$DUCKDB" -noheader -list 2>&1) || true
    if echo "$actual" | grep -qF "$expected"; then
        echo "  PASS: $name"
    else
        echo "  FAIL: $name"
        echo "    expected to contain: $expected"
        echo "    actual: $actual"
        FAILURES=$((FAILURES + 1))
    fi
}

echo "=== DuckDB CLI e2e tests ==="

# Test 1: use + count
run_test "use + count" \
    "use \"$PROJECT_DIR/test/data/firms.csv\", clear;
count;" \
    "5"

# Test 2: use + keep if + count
run_test "use + keep if + count" \
    "use \"$PROJECT_DIR/test/data/firms.csv\", clear;
keep if year == 2018;
count;" \
    "3"

# Test 3: use + keep if + list returns expected rows
run_test "use + keep if + list shows Beta" \
    "use \"$PROJECT_DIR/test/data/firms.csv\", clear;
keep if year == 2018;
list;" \
    "Beta"

# Test 4: generate + list
run_test "generate new column" \
    "use \"$PROJECT_DIR/test/data/firms.csv\", clear;
generate double_revenue = revenue * 2;
keep if id == 1;
list;" \
    "2000"

# Test 5: describe
run_test "describe shows columns" \
    "use \"$PROJECT_DIR/test/data/firms.csv\", clear;
describe;" \
    "revenue"

# Test 6: summarize
run_test "summarize shows mean" \
    "use \"$PROJECT_DIR/test/data/firms.csv\", clear;
summarize revenue;" \
    "1600"

if [ "$FAILURES" -gt 0 ]; then
    echo "=== $FAILURES test(s) FAILED ==="
    exit 1
fi

echo "=== All CLI tests passed ==="
