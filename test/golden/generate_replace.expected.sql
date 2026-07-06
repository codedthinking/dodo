CREATE SCHEMA IF NOT EXISTS dodo;
CREATE OR REPLACE TABLE dodo._current AS SELECT * FROM read_csv('sales.csv');
WITH
  -- [source] use "sales.csv"
  _s0 AS (
    SELECT *
    FROM dodo._current
  ),
  -- [source] generate flag = 0
  _s1 AS (
    SELECT *, (0) AS flag
    FROM _s0
  ),
  -- [source] replace flag = 1 if revenue > 100
  _s2 AS (
    SELECT * REPLACE (CASE WHEN revenue > 100 THEN 1 ELSE flag END AS flag)
    FROM _s1
  )
SELECT *
FROM _s2;
