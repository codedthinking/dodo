CREATE SCHEMA IF NOT EXISTS dodo;
CREATE OR REPLACE TABLE dodo._current AS SELECT * FROM read_csv('sales.csv');
WITH
  -- [source] use "sales.csv"
  _s0 AS (
    SELECT *
    FROM dodo._current
  ),
  -- [source] collapse (mean) revenue (sum) cost, by(region)
  _s1 AS (
    SELECT region, AVG(revenue) AS revenue, SUM(cost) AS cost
    FROM _s0
    GROUP BY region
    ORDER BY region
  )
SELECT *
FROM _s1;
