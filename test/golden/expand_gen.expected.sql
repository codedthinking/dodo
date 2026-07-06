CREATE SCHEMA IF NOT EXISTS dodo;
CREATE OR REPLACE TABLE dodo._current AS SELECT * FROM read_csv('sales.csv');
WITH
  -- [source] use "sales.csv"
  _s0 AS (
    SELECT *
    FROM dodo._current
  ),
  -- [source] expand 3, generate(copy)
  _s1 AS (
    SELECT * EXCLUDE (_expand_idx), CASE WHEN _expand_idx = 1 THEN 0 ELSE 1 END AS copy
    FROM (SELECT t.*, g.generate_series AS _expand_idx FROM _s0 t, LATERAL GENERATE_SERIES(1, 3) g)
  )
SELECT *
FROM _s1;
