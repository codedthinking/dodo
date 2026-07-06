CREATE SCHEMA IF NOT EXISTS dodo;
CREATE OR REPLACE TABLE dodo._current AS SELECT * FROM read_csv('sales.csv');
WITH
  -- [source] use "sales.csv"
  _s0 AS (
    SELECT *
    FROM dodo._current
  )
SELECT region, COUNT(*) AS freq
FROM _s0
GROUP BY region
ORDER BY region;
WITH
  -- [source] use "sales.csv"
  _s0 AS (
    SELECT *
    FROM dodo._current
  )
SELECT COUNT(revenue) AS N, AVG(revenue) AS mean, STDDEV(revenue) AS sd, MIN(revenue) AS min, PERCENTILE_CONT(0.25) WITHIN GROUP (ORDER BY revenue) AS p25, PERCENTILE_CONT(0.50) WITHIN GROUP (ORDER BY revenue) AS p50, PERCENTILE_CONT(0.75) WITHIN GROUP (ORDER BY revenue) AS p75, MAX(revenue) AS max
FROM _s0;
WITH
  -- [source] use "sales.csv"
  _s0 AS (
    SELECT *
    FROM dodo._current
  )
SELECT *
FROM _s0;
