CREATE SCHEMA IF NOT EXISTS dodo;
CREATE OR REPLACE TABLE dodo._current AS SELECT * FROM read_csv('sales.csv');
COPY (WITH
  -- [source] use "sales.csv"
  _s0 AS (
    SELECT *
    FROM dodo._current
  ),
  -- [source] keep if revenue > 0
  _s1 AS (
    SELECT *
    FROM _s0
    WHERE revenue > 0
  )
SELECT *
FROM _s1) TO 'clean.parquet' (FORMAT PARQUET);
COPY (WITH
  -- [source] use "sales.csv"
  _s0 AS (
    SELECT *
    FROM dodo._current
  ),
  -- [source] keep if revenue > 0
  _s1 AS (
    SELECT *
    FROM _s0
    WHERE revenue > 0
  )
SELECT *
FROM _s1) TO 'clean.csv' (FORMAT CSV, HEADER);
