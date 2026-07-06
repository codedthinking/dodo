CREATE SCHEMA IF NOT EXISTS dodo;
CREATE OR REPLACE TABLE dodo._current AS SELECT * FROM read_csv('sales.csv');
WITH
  -- [source] use "sales.csv"
  _s0 AS (
    SELECT *
    FROM dodo._current
  ),
  -- [source] keep id region revenue
  _s1 AS (
    SELECT id, region, revenue
    FROM _s0
  ),
  -- [source] drop if revenue < 0
  _s2 AS (
    SELECT *
    FROM _s1
    WHERE NOT (revenue < 0)
  )
SELECT *
FROM _s2;
