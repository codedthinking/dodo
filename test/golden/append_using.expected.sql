CREATE SCHEMA IF NOT EXISTS dodo;
CREATE OR REPLACE TABLE dodo._current AS SELECT * FROM read_csv('sales.csv');
WITH
  -- [source] use "sales.csv"
  _s0 AS (
    SELECT *
    FROM dodo._current
  ),
  -- [source] append using "sales_2019.csv"
  _s1 AS (
    SELECT *
    FROM _s0
    UNION ALL BY NAME
    SELECT *
    FROM read_csv('sales_2019.csv')
  )
SELECT *
FROM _s1;
