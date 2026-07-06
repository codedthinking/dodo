CREATE SCHEMA IF NOT EXISTS dodo;
CREATE OR REPLACE TABLE dodo._current AS SELECT * FROM read_csv('sales.csv');
WITH
  -- [source] use "sales.csv"
  _s0 AS (
    SELECT *
    FROM dodo._current
  ),
  -- [source] generate lag1 = revenue
  _s1 AS (
    SELECT *, (revenue) AS lag1
    FROM _s0
  ),
  -- [source] generate lag2 = revenue
  _s2 AS (
    SELECT *, (revenue) AS lag2
    FROM _s1
  ),
  -- [source] generate lag3 = revenue
  _s3 AS (
    SELECT *, (revenue) AS lag3
    FROM _s2
  )
SELECT *
FROM _s3;
