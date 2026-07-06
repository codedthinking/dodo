CREATE SCHEMA IF NOT EXISTS dodo;
CREATE OR REPLACE TABLE dodo._current AS SELECT * FROM read_csv('sales.csv');
WITH
  -- [source] use "sales.csv"
  _s0 AS (
    SELECT *
    FROM dodo._current
  ),
  -- [source] generate note = "paid in  today"
  _s1 AS (
    SELECT *, ('paid in  today') AS note
    FROM _s0
  )
SELECT *
FROM _s1;
