CREATE SCHEMA IF NOT EXISTS dodo;
CREATE OR REPLACE TABLE dodo._current AS SELECT * FROM read_csv('sales.csv');
WITH
  -- [source] use "sales.csv"
  _s0 AS (
    SELECT *
    FROM dodo._current
  ),
  -- [source] generate bare = 3 * 2
  _s1 AS (
    SELECT *, (3 * 2) AS bare
    FROM _s0
  ),
  -- [source] generate explicit = 3 * revenue
  _s2 AS (
    SELECT *, (3 * revenue) AS explicit
    FROM _s1
  )
SELECT *
FROM _s2;
