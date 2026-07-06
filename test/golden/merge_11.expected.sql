CREATE SCHEMA IF NOT EXISTS dodo;
CREATE OR REPLACE TABLE dodo._current AS SELECT * FROM read_csv('sales.csv');
WITH
  -- [source] use "sales.csv"
  _s0 AS (
    SELECT *
    FROM dodo._current
  ),
  -- [source] merge 1:1 id using "regions.csv", keep(match master)
  _s1 AS (
    SELECT *
    FROM (SELECT * EXCLUDE (_m_tag, _u_tag), CASE WHEN _m_tag IS NOT NULL AND _u_tag IS NOT NULL THEN 3 WHEN _m_tag IS NOT NULL THEN 1 ELSE 2 END AS _merge FROM (SELECT *, 1 AS _m_tag FROM _s0) AS _master LEFT JOIN (SELECT *, 1 AS _u_tag FROM (SELECT * FROM read_csv('regions.csv'))) AS _using USING (id))
    WHERE (_merge = 1 OR _merge = 3)
  )
SELECT *
FROM _s1;
