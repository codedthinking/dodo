CREATE SCHEMA IF NOT EXISTS dodo;
CREATE OR REPLACE TABLE dodo._current AS SELECT * FROM read_csv('panel_wide.csv');
WITH
  -- [source] use "panel_wide.csv"
  _s0 AS (
    SELECT *
    FROM dodo._current
  ),
  -- [source] reshape long revenue, i(id) j(year)
  _s1 AS (
    SELECT * REPLACE (REPLACE(year, 'revenue_', '') AS year)
    FROM (UNPIVOT _s0 ON COLUMNS('revenue_.*') INTO NAME year VALUE revenue)
  )
SELECT *
FROM _s1;
