CREATE SCHEMA IF NOT EXISTS dodo;
CREATE OR REPLACE TABLE dodo._current AS SELECT * FROM read_csv('sales.csv');
WITH
  -- [source] use "sales.csv"
  _s0 AS (
    SELECT *
    FROM dodo._current
  ),
  -- [source] replace revenue = 0 if revenue >= .
  _s1 AS (
    SELECT * REPLACE (CASE WHEN (revenue IS NULL) THEN 0 ELSE revenue END AS revenue)
    FROM _s0
  ),
  -- [source] keep if price < .
  _s2 AS (
    SELECT *
    FROM _s1
    WHERE (price IS NOT NULL)
  ),
  -- [source] keep if (revenue + cost) >= .
  _s3 AS (
    SELECT *
    FROM _s2
    WHERE ((revenue + cost) IS NULL)
  ),
  -- [source] keep if substr(region,1,1) != .
  _s4 AS (
    SELECT *
    FROM _s3
    WHERE (SUBSTRING(region,1,1) IS NOT NULL)
  ),
  -- [source] keep if . > price
  _s5 AS (
    SELECT *
    FROM _s4
    WHERE (price IS NOT NULL)
  )
SELECT *
FROM _s5;
