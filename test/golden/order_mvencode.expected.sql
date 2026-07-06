CREATE SCHEMA IF NOT EXISTS dodo;
CREATE OR REPLACE TABLE dodo._current AS SELECT * FROM read_csv('sales.csv');
WITH
  -- [source] use "sales.csv"
  _s0 AS (
    SELECT *
    FROM dodo._current
  ),
  -- [source] order region id
  _s1 AS (
    SELECT region, id, * EXCLUDE (region, id)
    FROM _s0
  ),
  -- [source] mvencode revenue cost, mv(0)
  _s2 AS (
    SELECT * REPLACE (COALESCE(revenue, 0) AS revenue, COALESCE(cost, 0) AS cost)
    FROM _s1
  )
SELECT *
FROM _s2;
