CREATE SCHEMA IF NOT EXISTS dodo;
CREATE OR REPLACE TABLE dodo._current AS SELECT * FROM read_csv('panel.csv');
WITH
  -- [source] use "panel.csv"
  _s0 AS (
    SELECT *
    FROM dodo._current
  ),
  -- [source] generate growth = sales - L.sales
  _s1 AS (
    SELECT *, (sales - CASE WHEN (year - LAG(year, 1) OVER (PARTITION BY firm ORDER BY year)) = 1 THEN LAG(sales, 1) OVER (PARTITION BY firm ORDER BY year) ELSE NULL END) AS growth
    FROM _s0
  )
SELECT *
FROM _s1;
