#!/usr/bin/env bash
# E2E test: DuckDB CLI with dodo extension
# Test cases are defined in cases.yaml; converted to JSON for jq processing.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
DUCKDB="$PROJECT_DIR/build/release/duckdb"
CASES_YAML="$SCRIPT_DIR/cases.yaml"
DATA_DIR="$PROJECT_DIR/test/data"

if [ ! -x "$DUCKDB" ]; then
    echo "FAIL: DuckDB binary not found at $DUCKDB"
    exit 1
fi

# Convert YAML to JSON via Python (available via uv)
CASES_JSON=$(python3 -c "import sys,json,yaml; json.dump(yaml.safe_load(open(sys.argv[1])),sys.stdout)" "$CASES_YAML" 2>/dev/null) || \
CASES_JSON=$(uv run --python 3.13 --with pyyaml python3 -c "import sys,json,yaml; json.dump(yaml.safe_load(open(sys.argv[1])),sys.stdout)" "$CASES_YAML")

FAILURES=0
N_CASES=$(echo "$CASES_JSON" | jq length)

echo "=== DuckDB CLI e2e tests ==="

for i in $(seq 0 $((N_CASES - 1))); do
    name=$(echo "$CASES_JSON" | jq -r ".[$i].name")
    setup=$(echo "$CASES_JSON" | jq -r ".[$i].setup // empty")

    # Build input: setup SQL + commands (newline-separated string), each terminated with semicolon
    input=""
    if [ -n "$setup" ]; then
        input+="$setup;"$'\n'
    fi
    commands_str=$(echo "$CASES_JSON" | jq -r ".[$i].commands")
    while IFS= read -r cmd; do
        [ -z "$cmd" ] && continue
        cmd="${cmd//\{data\}/$DATA_DIR}"
        input+="$cmd;"$'\n'
    done <<< "$commands_str"

    actual=$(echo "$input" | "$DUCKDB" -noheader -list 2>&1) || true

    # Check expectations based on type
    expect_type=$(echo "$CASES_JSON" | jq -r ".[$i].expect.type")
    ok=true

    case "$expect_type" in
        scalar)
            expected=$(echo "$CASES_JSON" | jq -r ".[$i].expect.value")
            if ! echo "$actual" | grep -qF "$expected"; then ok=false; fi
            ;;
        contains_column)
            for inc in $(echo "$CASES_JSON" | jq -r ".[$i].expect.includes[]" 2>/dev/null); do
                if ! echo "$actual" | grep -qF "$inc"; then ok=false; fi
            done
            for exc in $(echo "$CASES_JSON" | jq -r ".[$i].expect.excludes[]" 2>/dev/null); do
                if echo "$actual" | grep -qF "$exc"; then ok=false; fi
            done
            ;;
        cell|row_count_and_cell)
            expected=$(echo "$CASES_JSON" | jq -r ".[$i].expect.value")
            if ! echo "$actual" | grep -qF "$expected"; then ok=false; fi
            ;;
    esac

    if $ok; then
        echo "  PASS: $name"
    else
        echo "  FAIL: $name"
        echo "    actual: $actual"
        FAILURES=$((FAILURES + 1))
    fi
done

if [ "$FAILURES" -gt 0 ]; then
    echo "=== $FAILURES test(s) FAILED ==="
    exit 1
fi

echo "=== All CLI tests passed ==="
