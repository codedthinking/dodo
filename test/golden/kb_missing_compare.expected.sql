CREATE SCHEMA IF NOT EXISTS dodo;
CREATE OR REPLACE TABLE dodo._current AS SELECT * FROM read_csv('sales.csv');
WITH
  -- [source] use "sales.csv"
  _s0 AS (
    SELECT *
    FROM dodo._current
  ),
  -- [source] replace revenue = 0 if revenue >= .
  _s1 AS (
    SELECT * REPLACE (CASE WHEN revenue >= NULL THEN 0 ELSE revenue END AS revenue)
    FROM _s0
  )
SELECT *
FROM _s1;
