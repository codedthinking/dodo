CREATE SCHEMA IF NOT EXISTS dodo;
CREATE OR REPLACE TABLE dodo._current AS SELECT * FROM read_csv('sales.csv');
WITH
  -- [source] use "sales.csv"
  _s0 AS (
    SELECT *
    FROM dodo._current
  ),
  -- [source] egen region_avg = mean(revenue), by(region)
  _s1 AS (
    SELECT *, AVG(revenue) OVER (PARTITION BY region) AS region_avg
    FROM _s0
  )
SELECT *
FROM _s1;
