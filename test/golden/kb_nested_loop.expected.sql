CREATE SCHEMA IF NOT EXISTS dodo;
CREATE OR REPLACE TABLE dodo._current AS SELECT * FROM read_csv('sales.csv');
WITH
  -- [source] use "sales.csv"
  _s0 AS (
    SELECT *
    FROM dodo._current
  ),
  -- [source] generate v1 = 1*
  _s1 AS (
    SELECT *, (1*) AS v1
    FROM _s0
  ),
  -- [source] generate v2 = 2*
  _s2 AS (
    SELECT *, (2*) AS v2
    FROM _s1
  )
SELECT *
FROM _s2;
