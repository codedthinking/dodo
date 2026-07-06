CREATE SCHEMA IF NOT EXISTS dodo;
CREATE OR REPLACE TABLE dodo._current AS SELECT * FROM read_csv('sales.csv');
WITH
  -- [source] use "sales.csv"
  _s0 AS (
    SELECT *
    FROM dodo._current
  ),
  -- [source] generate v13 = 1*3
  _s1 AS (
    SELECT *, (1*3) AS v13
    FROM _s0
  ),
  -- [source] generate v14 = 1*4
  _s2 AS (
    SELECT *, (1*4) AS v14
    FROM _s1
  ),
  -- [source] generate v23 = 2*3
  _s3 AS (
    SELECT *, (2*3) AS v23
    FROM _s2
  ),
  -- [source] generate v24 = 2*4
  _s4 AS (
    SELECT *, (2*4) AS v24
    FROM _s3
  )
SELECT *
FROM _s4;
