CREATE SCHEMA IF NOT EXISTS dodo;
CREATE OR REPLACE TABLE dodo._current AS SELECT * FROM read_csv('sales.csv');
WITH
  -- [source] use "sales.csv"
  _s0 AS (
    SELECT *
    FROM dodo._current
  ),
  -- [source] generate url = "http://example.com"
  _s1 AS (
    SELECT *, ('http://example.com') AS url
    FROM _s0
  ),
  -- [source] generate note = "value"
  _s2 AS (
    SELECT *, ('value') AS note
    FROM _s1
  )
SELECT *
FROM _s2;
