"""E2E test: Python duckdb client with dodo extension."""
import os
import sys

import duckdb

PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXT_PATH = os.path.join(PROJECT_DIR, "build", "release", "extension", "dodo", "dodo.duckdb_extension")

failures = 0


def run_test(name, fn):
    global failures
    try:
        fn()
        print(f"  PASS: {name}")
    except Exception as e:
        print(f"  FAIL: {name}")
        print(f"    {e}")
        failures += 1


def fresh_conn():
    con = duckdb.connect(":memory:", config={"allow_unsigned_extensions": "true"})
    con.execute(f"LOAD '{EXT_PATH}'")
    return con


def test_use_and_count():
    con = fresh_conn()
    con.execute(f'use "{PROJECT_DIR}/test/data/firms.csv", clear')
    result = con.execute("count").fetchall()
    assert result[0][0] == 5, f"expected 5, got {result[0][0]}"


def test_keep_if():
    con = fresh_conn()
    con.execute(f'use "{PROJECT_DIR}/test/data/firms.csv", clear')
    con.execute("keep if year == 2018")
    result = con.execute("count").fetchall()
    assert result[0][0] == 3, f"expected 3, got {result[0][0]}"


def test_list_after_keep():
    con = fresh_conn()
    con.execute(f'use "{PROJECT_DIR}/test/data/firms.csv", clear')
    con.execute("keep if year == 2018")
    rows = con.execute("list").fetchall()
    names = [row[1] for row in rows]
    assert "Beta" in names, f"expected Beta in {names}"
    assert "Acme" not in names, f"Acme should be filtered out, got {names}"


def test_generate():
    con = fresh_conn()
    con.execute(f'use "{PROJECT_DIR}/test/data/firms.csv", clear')
    con.execute("generate double_revenue = revenue * 2")
    con.execute("keep if id == 1")
    rows = con.execute("list").fetchall()
    assert rows[0][-1] == 2000, f"expected 2000, got {rows[0][-1]}"


def test_describe():
    con = fresh_conn()
    con.execute(f'use "{PROJECT_DIR}/test/data/firms.csv", clear')
    rows = con.execute("describe").fetchall()
    col_names = [row[0] for row in rows]
    assert "revenue" in col_names, f"expected revenue in {col_names}"


def test_summarize():
    con = fresh_conn()
    con.execute(f'use "{PROJECT_DIR}/test/data/firms.csv", clear')
    rows = con.execute("summarize revenue").fetchall()
    # summarize returns stats; check that mean is 1600
    # column order: variable, N, mean, sd, min, p25, p50, p75, max
    mean_val = rows[0][2]
    assert mean_val == 1600.0, f"expected mean 1600.0, got {mean_val}"


def test_inline_data():
    """Test with inline CREATE TABLE instead of CSV file."""
    con = fresh_conn()
    con.execute("""
        CREATE TABLE t AS
        SELECT 2020 AS year, 1 AS x
        UNION ALL
        SELECT 2021 AS year, 2 AS x
    """)
    con.execute("use t")
    con.execute("keep if year == 2020")
    rows = con.execute("list").fetchall()
    assert len(rows) == 1, f"expected 1 row, got {len(rows)}"
    assert rows[0][1] == 1, f"expected x=1, got {rows[0][1]}"


if __name__ == "__main__":
    if not os.path.exists(EXT_PATH):
        print(f"FAIL: Extension not found at {EXT_PATH}")
        sys.exit(1)

    print("=== Python e2e tests ===")
    run_test("use + count", test_use_and_count)
    run_test("keep if + count", test_keep_if)
    run_test("list after keep", test_list_after_keep)
    run_test("generate", test_generate)
    run_test("describe", test_describe)
    run_test("summarize", test_summarize)
    run_test("inline data + use table", test_inline_data)

    if failures > 0:
        print(f"=== {failures} test(s) FAILED ===")
        sys.exit(1)

    print("=== All Python tests passed ===")
