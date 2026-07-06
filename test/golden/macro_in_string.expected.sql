CREATE SCHEMA IF NOT EXISTS dodo;
CREATE OR REPLACE TABLE dodo._current AS SELECT * FROM read_csv('sales.csv');
WITH
  -- [source] use "sales.csv"
  _s0 AS (
    SELECT *
    FROM dodo._current
  ),
  -- [source] generate paid = "amount in USD"
  _s1 AS (
    SELECT *, ('amount in USD') AS paid
    FROM _s0
  ),
  -- [source] generate note = "region is here"
  _s2 AS (
    SELECT *, ('region is here') AS note
    FROM _s1
  )
SELECT *
FROM _s2;
