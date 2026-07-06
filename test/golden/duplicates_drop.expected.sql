CREATE SCHEMA IF NOT EXISTS dodo;
CREATE OR REPLACE TABLE dodo._current AS SELECT * FROM read_csv('sales.csv');
WITH
  -- [source] use "sales.csv"
  _s0 AS (
    SELECT *
    FROM dodo._current
  ),
  -- [source] duplicates drop id
  _s1 AS (
    SELECT * EXCLUDE (_dedup_rn)
    FROM (SELECT *, ROW_NUMBER() OVER (PARTITION BY id) AS _dedup_rn FROM _s0)
    WHERE _dedup_rn = 1
  )
SELECT *
FROM _s1;
