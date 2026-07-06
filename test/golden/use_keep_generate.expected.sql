CREATE SCHEMA IF NOT EXISTS dodo;
CREATE OR REPLACE TABLE dodo._current AS SELECT * FROM read_csv('sales.csv');
WITH
  -- [source] use "sales.csv"
  _s0 AS (
    SELECT *
    FROM dodo._current
  ),
  -- [source] keep if region == "north"
  _s1 AS (
    SELECT *
    FROM _s0
    WHERE region == 'north'
  ),
  -- [source] generate margin = revenue - cost
  _s2 AS (
    SELECT *, (revenue - cost) AS margin
    FROM _s1
  )
SELECT COUNT(*) AS n
FROM _s2;
WITH
  -- [source] use "sales.csv"
  _s0 AS (
    SELECT *
    FROM dodo._current
  ),
  -- [source] keep if region == "north"
  _s1 AS (
    SELECT *
    FROM _s0
    WHERE region == 'north'
  ),
  -- [source] generate margin = revenue - cost
  _s2 AS (
    SELECT *, (revenue - cost) AS margin
    FROM _s1
  )
SELECT *
FROM _s2;
