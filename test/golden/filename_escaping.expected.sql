CREATE SCHEMA IF NOT EXISTS dodo;
CREATE OR REPLACE TABLE dodo._current AS SELECT * FROM read_csv('it''s data.csv');
WITH
  -- [source] use "it's data.csv"
  _s0 AS (
    SELECT *
    FROM dodo._current
  )
SELECT *
FROM _s0;
