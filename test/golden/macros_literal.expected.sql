CREATE SCHEMA IF NOT EXISTS dodo;
CREATE OR REPLACE TABLE dodo._current AS SELECT * FROM read_csv('sales.csv');
SELECT 'cutoff is 100' AS display;
WITH
  -- [source] use "sales.csv"
  _s0 AS (
    SELECT *
    FROM dodo._current
  ),
  -- [source] keep if revenue > 100
  _s1 AS (
    SELECT *
    FROM _s0
    WHERE revenue > 100
  )
SELECT *
FROM _s1;
