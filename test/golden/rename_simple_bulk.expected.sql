CREATE SCHEMA IF NOT EXISTS dodo;
CREATE OR REPLACE TABLE dodo._current AS SELECT * FROM read_csv('sales.csv');
WITH
  -- [source] use "sales.csv"
  _s0 AS (
    SELECT *
    FROM dodo._current
  ),
  -- [source] rename revenue sales
  _s1 AS (
    SELECT * EXCLUDE (revenue), revenue AS sales
    FROM _s0
  ),
  -- [source] rename (cost id) (expense key)
  _s2 AS (
    SELECT cost AS expense, id AS "key", * EXCLUDE (cost, id)
    FROM _s1
  )
SELECT *
FROM _s2;
