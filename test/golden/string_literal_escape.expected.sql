CREATE SCHEMA IF NOT EXISTS dodo;
CREATE OR REPLACE TABLE dodo._current AS SELECT * FROM read_csv('sales.csv');
WITH
  -- [source] use "sales.csv"
  _s0 AS (
    SELECT *
    FROM dodo._current
  ),
  -- [source] keep if name == "it's"
  _s1 AS (
    SELECT *
    FROM _s0
    WHERE name == 'it''s'
  )
SELECT *
FROM _s1;
