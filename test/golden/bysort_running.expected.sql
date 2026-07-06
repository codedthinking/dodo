CREATE SCHEMA IF NOT EXISTS dodo;
CREATE OR REPLACE TABLE dodo._current AS SELECT * FROM read_csv('panel.csv');
WITH
  -- [source] use "panel.csv"
  _s0 AS (
    SELECT *
    FROM dodo._current
  ),
  -- [source] bysort firm (year): generate cum_sales = sum(sales)
  _s1 AS (
    SELECT *, (SUM(sales) OVER (PARTITION BY firm ORDER BY year ROWS UNBOUNDED PRECEDING)) AS cum_sales
    FROM _s0
  )
SELECT *
FROM _s1;
