CREATE SCHEMA IF NOT EXISTS dodo;
CREATE OR REPLACE TABLE dodo._current AS SELECT * FROM read_csv('sales.csv');
SET VARIABLE _dodo_l_i = _n + 1;
WITH
  -- [source] use "sales.csv"
  _s0 AS (
    SELECT *
    FROM dodo._current
  )
SELECT *
FROM _s0;
