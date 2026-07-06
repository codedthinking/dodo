CREATE SCHEMA IF NOT EXISTS dodo;
CREATE OR REPLACE TABLE dodo._current AS SELECT * FROM read_csv('sales.csv');
WITH
  -- [source] use "sales.csv"
  _s0 AS (
    SELECT *
    FROM dodo._current
  ),
  -- [source] generate log_revenue = log(revenue)
  _s1 AS (
    SELECT *, (LN(revenue)) AS log_revenue
    FROM _s0
  ),
  -- [source] generate log_cost = log(cost)
  _s2 AS (
    SELECT *, (LN(cost)) AS log_cost
    FROM _s1
  )
SELECT *
FROM _s2;
