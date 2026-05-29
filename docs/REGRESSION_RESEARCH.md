# Regression in DuckDB — Research Notes

Research for implementing `regress` in dodo. Sources: duckreg (py-econometrics), pyfixest, FixedEffectModels.jl, DuckDB built-in aggregates, Lal et al. (2024). No decisions made yet.

---

## 1. DuckDB built-in regression aggregates

DuckDB implements the full SQL standard set of regression aggregate functions:

| Function | What it computes |
|----------|-----------------|
| `regr_slope(y, x)` | OLS slope coefficient |
| `regr_intercept(y, x)` | OLS intercept |
| `regr_r2(y, x)` | R-squared |
| `regr_count(y, x)` | Number of non-null pairs |
| `regr_avgx(y, x)` | Mean of x |
| `regr_avgy(y, x)` | Mean of y |
| `regr_sxx(y, x)` | Sum of squares of x deviations |
| `regr_syy(y, x)` | Sum of squares of y deviations |
| `regr_sxy(y, x)` | Sum of cross-products |
| `corr(y, x)` | Correlation coefficient |
| `covar_pop(y, x)` | Population covariance |
| `covar_samp(y, x)` | Sample covariance |

These are single-pass streaming aggregates. They handle the simple bivariate case natively. For multivariate OLS, they are insufficient — you need matrix operations.

### What DuckDB can compute natively via SQL

For a regression `y ~ x1 + x2 + ... + xk`, DuckDB can compute in a single pass:
- All pairwise `regr_sxy(xi, xj)` → builds X'X matrix
- All `regr_sxy(y, xi)` → builds X'y vector
- `regr_count`, means, variances

What DuckDB **cannot** do natively: matrix inversion (`(X'X)^{-1}`). This requires either a UDF, an extension, or client-side computation.

---

## 2. duckreg approach: compress then solve

### Core insight (Lal, Fischer, Wardrop 2024)

> "Every big regression is a small regression with weights."

If all regressors are discrete (or discretized), you can compress millions of rows into a few hundred by grouping on the unique combinations of regressor values and storing sufficient statistics (count, sum_y, sum_y_sq per group). Then run weighted least squares on the compressed data.

### How duckreg works

1. **Parse formula**: `Y ~ D + X1 + X2` → outcome vars, covariate vars
2. **Compress via SQL**: group by all covariate combinations, compute count and outcome sums
   ```sql
   SELECT D, X1, X2, COUNT(*) AS count,
          SUM(Y) AS sum_Y, SUM(POW(Y, 2)) AS sum_Y_sq
   FROM data
   GROUP BY D, X1, X2
   ```
3. **Build design matrix**: from compressed data, compute group means (`mean_Y = sum_Y / count`), add intercept column
4. **Solve WLS**: `beta = (X'WX)^{-1} X'Wy` where W = diag(count) — uses `numpy.linalg.lstsq`
5. **Standard errors**:
   - **Robust (HC1)**: reconstruct residual sum of squares per group from sufficient statistics: `rss_g = yhat^2 * n - 2 * yhat * sum_y + sum_y_sq`. Then sandwich formula.
   - **Clustered**: cluster bootstrap — resample clusters with replacement, recompute compressed data with multiplied counts, re-estimate coefficients. Empirical covariance of bootstrap estimates.

### Compression ratio

10 million rows with 20 discrete x-values each → ~800 groups. 99.99% compression. WLS on 800 rows is instant.

### Performance

| Method | Time (10M rows) | Speedup |
|--------|-----------------|---------|
| duckreg | 145 ms | 1x |
| pyfixest | 7 s | ~48x slower |
| statsmodels OLS | 36 s | ~250x slower |

### Limitations

- All regressors must be discrete (or binned). Continuous regressors need discretization, which introduces approximation.
- No formula-level fixed effects in `DuckRegression`. Must use `DuckMundlak` or `DuckDoubleDemeaning` for panel FE.
- Standard errors via bootstrap are approximate (not exact analytical).
- Python-only — the matrix inversion happens in numpy, not in DuckDB.

---

## 3. duckreg panel data estimators

### DuckMundlak

Implements Mundlak (1978): instead of N unit dummies, include unit means of regressors as additional controls. Eliminates unit fixed effects without explicit dummies.

Y_it = alpha + beta * X_it + gamma * X_bar_i + epsilon_it

SQL steps:
1. Compute unit means: `SELECT unit, AVG(X) AS avg_X FROM data GROUP BY unit`
2. Optionally compute time means
3. Join means back to data
4. Compress the augmented dataset (group by X, avg_X_unit, avg_X_time)
5. Solve WLS on compressed data

### DuckDoubleDemeaning

Implements Frisch-Waugh-Lovell for two-way FE:

X_dd = X_it - X_bar_i - X_bar_t + X_bar

SQL steps:
1. Compute overall mean, unit means, time means (three separate queries)
2. Double-demean: `X_dd = X - unit_mean - time_mean + overall_mean`
3. Compress the demeaned data
4. Solve WLS on compressed data

### DuckMundlakEventStudy

Two-way Mundlak with dynamic treatment effects (cohort x time interactions). For difference-in-differences and event study designs.

---

## 4. Alternative approaches

### pyfixest / FixedEffectModels.jl approach

These packages use iterative demeaning (alternating projections) to absorb high-dimensional fixed effects:

1. Demean y and X with respect to all FE dimensions (iterate until convergence)
2. Run OLS on demeaned data
3. Compute clustered SE via analytical formulas

**Algorithm**: MAP (Method of Alternating Projections) or LSMR for the demeaning step. Converges in ~10-20 iterations typically.

**Pros**: handles continuous regressors, exact analytical SE, very fast for moderate-dimensional FE.
**Cons**: requires all data in memory (no out-of-memory support), iterative (not single-pass).

### Pure SQL approach (no external solver)

For simple OLS with a small number of regressors, you can solve the normal equations in SQL using DuckDB's aggregate functions:

For `y ~ x1 + x2` (2 regressors + intercept):
```sql
WITH stats AS (
  SELECT
    COUNT(*) AS n,
    SUM(x1) AS sx1, SUM(x2) AS sx2, SUM(y) AS sy,
    SUM(x1*x1) AS sx1x1, SUM(x1*x2) AS sx1x2, SUM(x2*x2) AS sx2x2,
    SUM(x1*y) AS sx1y, SUM(x2*y) AS sx2y
  FROM data
)
-- Then solve the 3x3 system using Cramer's rule or explicit inverse
```

This works for k <= ~5 regressors (manageable closed-form inverse). Beyond that, you need a general matrix solver.

**Pros**: pure SQL, no external dependencies, single-pass, works with DuckDB's query optimizer.
**Cons**: doesn't scale to many regressors, no FE support, verbose SQL for the inverse.

### DuckDB extension approach

Write the matrix inversion as a C++ DuckDB extension (table function or scalar function). The regression becomes:

1. Compute X'X and X'y via SQL aggregates (efficient, streaming)
2. Call `ols_solve(XtX, Xty)` extension function for the matrix inverse
3. Compute residuals and SE via SQL

This keeps the heavy lifting (aggregation) in DuckDB and only uses C++ for the small matrix algebra step.

---

## 5. What dodo needs for `regress`

### Minimum viable: `regress y x1 x2`

Output: coefficient table with estimates, standard errors, t-statistics, p-values, R-squared, N.

### Desirable: `regress y x1 x2, robust`

Heteroscedasticity-robust (HC1) standard errors.

### Desirable: `regress y x1 x2, cluster(id)`

Cluster-robust standard errors.

### Stretch: `regress y x1 x2, absorb(fe1 fe2)`

High-dimensional fixed effects (like `reghdfe` / `areg`).

---

## 6. Implementation options (no decision yet)

### Option A: Pure SQL (small k)

Generate the normal equations in SQL using DuckDB aggregates. Solve the matrix inverse with explicit formulas (Cramer's rule up to ~4x4, or Cholesky via a small C++ UDF). Everything stays in the DuckDB process.

**Pros**: no external dependencies, works in dodoc compiler output, pure SQL is inspectable.
**Cons**: doesn't scale beyond ~5 regressors without a matrix solver, verbose generated SQL.

### Option B: duckreg-style compress + external solve

Compress data via SQL GROUP BY, then solve WLS externally (in Python via numpy, or in C++ within the extension).

**Pros**: handles large data, leverages DuckDB for the expensive part.
**Cons**: requires discrete regressors or discretization, bootstrap SE is approximate.

### Option C: C++ extension with LAPACK

Add matrix algebra functions to the dodo extension (using LAPACK or Eigen). Compute sufficient statistics via SQL, invert the matrix in C++.

**Pros**: exact solution, handles any number of regressors, robust/clustered SE can be exact.
**Cons**: adds C++ dependency (LAPACK/Eigen), more complex build.

### Option D: Delegate to pyfixest/duckreg

Don't implement regression in dodo at all. Instead, document how to use pyfixest or duckreg with dodo's data pipeline. The `save` command can write to a DuckDB table that pyfixest reads.

**Pros**: zero implementation cost, users get battle-tested econometrics.
**Cons**: breaks the single-tool workflow, requires Python.

### Option E: Hybrid — SQL aggregates + small solver UDF

Compute X'X and X'y as a single SQL query using `regr_sxx`, `regr_sxy`, etc. Pass the resulting small matrix to a `dodo_ols_solve()` scalar UDF that returns coefficients. Compute residuals and SE in a follow-up SQL query.

**Pros**: most computation in SQL (optimizer-friendly), small C++ footprint, exact solution.
**Cons**: still needs a C++ matrix solver (but very small — just Cholesky on a k×k matrix).

---

## 7. Open questions

1. **How many regressors is realistic?** If k <= 5 (common in applied work), pure SQL with explicit formulas may suffice. If k can be 20+, need a general solver.

2. **Fixed effects**: are they in scope for dodo, or should users use pyfixest/duckreg? FE absorption is a separate algorithm (iterative demeaning) that's hard to do in pure SQL.

3. **Standard errors**: robust SE requires residuals (another pass over data). Clustered SE requires either analytical formulas (complex) or bootstrap (approximate). What's the right tradeoff?

4. **Output format**: should `regress` return a table (coefficient, se, t, p per row), or print a formatted summary like other terminal commands?

5. **Interaction with the CTE chain**: `regress` is a terminal command — it materializes the chain and runs the regression. But should it also store results (e.g., predicted values, residuals) that can be used in subsequent commands?

6. **dodoc compatibility**: can the regression be expressed as pure SQL (option A/E), or does it require runtime computation that dodoc can't emit?

7. **IV/2SLS**: is instrumental variables estimation in scope? This is common in applied econometrics but adds significant complexity.

---

## References

- Lal, A., Fischer, N., & Wardrop, M. (2024). Large Scale Longitudinal Experiments: Estimation and Inference. arXiv:2410.09952.
- Mundlak, Y. (1978). On the pooling of time series and cross section data. Econometrica 46(1), 69-85.
- Correia, S. (2016). A feasible estimator for linear models with multi-way fixed effects. Working paper. (reghdfe)
- Berge, L. (2018). Efficient estimation of maximum likelihood models with multiple fixed-effects: the R package fixest.
