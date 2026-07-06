CREATE SCHEMA IF NOT EXISTS dodo;
CREATE OR REPLACE TABLE dodo._current AS SELECT * FROM read_csv('sales.csv');
SELECT lpad('revenue', 61, ' ') || chr(10) || '-------------------------------------------------------------' || chr(10) || '      Percentiles      Smallest' || chr(10) || ' 1%    ' || lpad(printf('%g', p1), 9, ' ') || '       ' || lpad(printf('%g', s1), 12, ' ') || chr(10) || ' 5%    ' || lpad(printf('%g', p5), 9, ' ') || '       ' || lpad(printf('%g', s2), 12, ' ') || chr(10) || '10%    ' || lpad(printf('%g', p10), 9, ' ') || '       ' || lpad(printf('%g', s3), 12, ' ') || '       Obs         ' || lpad(printf('%g', N::DOUBLE), 12, ' ') || chr(10) || '25%    ' || lpad(printf('%g', p25), 9, ' ') || '       ' || lpad(printf('%g', s4), 12, ' ') || '       Sum of wgt. ' || lpad(printf('%g', N::DOUBLE), 12, ' ') || chr(10) || chr(10) || '50%    ' || lpad(printf('%g', p50), 9, ' ') || '                      Mean        ' || lpad(printf('%g', mean), 12, ' ') || chr(10) || '                        Largest       Std. dev.   ' || lpad(printf('%g', sd), 12, ' ') || chr(10) || '75%    ' || lpad(printf('%g', p75), 9, ' ') || '       ' || lpad(printf('%g', l4), 12, ' ') || chr(10) || '90%    ' || lpad(printf('%g', p90), 9, ' ') || '       ' || lpad(printf('%g', l3), 12, ' ') || '       Variance    ' || lpad(printf('%g', var), 12, ' ') || chr(10) || '95%    ' || lpad(printf('%g', p95), 9, ' ') || '       ' || lpad(printf('%g', l2), 12, ' ') || '       Skewness    ' || lpad(printf('%.4f', skew), 12, ' ') || chr(10) || '99%    ' || lpad(printf('%g', p99), 9, ' ') || '       ' || lpad(printf('%g', l1), 12, ' ') || '       Kurtosis    ' || lpad(printf('%.1f', kurt), 12, ' ') AS display FROM (SELECT COUNT(_val) AS N, AVG(_val) AS mean, COALESCE(STDDEV(_val), 0) AS sd, COALESCE(VARIANCE(_val), 0) AS var, COALESCE(SKEWNESS(_val), 0) AS skew, COALESCE(KURTOSIS(_val), 0) AS kurt, PERCENTILE_CONT(0.01) WITHIN GROUP (ORDER BY _val) AS p1, PERCENTILE_CONT(0.05) WITHIN GROUP (ORDER BY _val) AS p5, PERCENTILE_CONT(0.10) WITHIN GROUP (ORDER BY _val) AS p10, PERCENTILE_CONT(0.25) WITHIN GROUP (ORDER BY _val) AS p25, PERCENTILE_CONT(0.50) WITHIN GROUP (ORDER BY _val) AS p50, PERCENTILE_CONT(0.75) WITHIN GROUP (ORDER BY _val) AS p75, PERCENTILE_CONT(0.90) WITHIN GROUP (ORDER BY _val) AS p90, PERCENTILE_CONT(0.95) WITHIN GROUP (ORDER BY _val) AS p95, PERCENTILE_CONT(0.99) WITHIN GROUP (ORDER BY _val) AS p99 FROM (WITH
  -- [source] use "sales.csv"
  _s0 AS (
    SELECT *
    FROM dodo._current
  )
SELECT CAST(revenue AS DOUBLE) AS _val
FROM _s0) _d) _stats, LATERAL (SELECT list(_val::DOUBLE ORDER BY _val)[:4] AS vals FROM (WITH
  -- [source] use "sales.csv"
  _s0 AS (
    SELECT *
    FROM dodo._current
  )
SELECT CAST(revenue AS DOUBLE) AS _val
FROM _s0) _d2) _sm, LATERAL (SELECT list(_val::DOUBLE ORDER BY _val DESC)[:4] AS vals FROM (WITH
  -- [source] use "sales.csv"
  _s0 AS (
    SELECT *
    FROM dodo._current
  )
SELECT CAST(revenue AS DOUBLE) AS _val
FROM _s0) _d3) _lg, LATERAL (SELECT COALESCE(_sm.vals[1], 0) AS s1, COALESCE(_sm.vals[2], 0) AS s2, COALESCE(_sm.vals[3], 0) AS s3, COALESCE(_sm.vals[4], 0) AS s4) _s, LATERAL (SELECT COALESCE(_lg.vals[1], 0) AS l1, COALESCE(_lg.vals[2], 0) AS l2, COALESCE(_lg.vals[3], 0) AS l3, COALESCE(_lg.vals[4], 0) AS l4) _l;
WITH
  -- [source] use "sales.csv"
  _s0 AS (
    SELECT *
    FROM dodo._current
  )
SELECT *
FROM _s0;
