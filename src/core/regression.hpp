#pragma once

#include <vector>

namespace dodo {

struct FitStats {
	double ess;     // error (residual) sum of squares
	double tss;     // total sum of squares
	double mss;     // model sum of squares
	double r2;
	double r2_adj;
	double f_stat;
	double rmse;
	int n;
	int k;          // number of regressors including intercept
};

// Solve b = (X'X)^{-1} X'y via Cholesky decomposition.
// xtx: k*k row-major, xty: k-vector. Returns k-vector of coefficients.
// Throws on singular matrix.
std::vector<double> OlsSolve(const std::vector<double> &xtx, const std::vector<double> &xty, int k);

// Diagonal of (X'X)^{-1}. Returns k-vector.
std::vector<double> OlsInvDiag(const std::vector<double> &xtx, int k);

// Diagonal of (X'X)^{-1} M (X'X)^{-1} (sandwich variance).
// xtx, meat: k*k row-major. Returns k-vector.
std::vector<double> SandwichDiag(const std::vector<double> &xtx, const std::vector<double> &meat, int k);

// Compute fit statistics from ESS, TSS, n, k.
FitStats ComputeFitStats(double ess, double tss, int n, int k);

// Compute p-value from t-statistic using normal approximation.
double NormalPValue(double t_stat);

//===--------------------------------------------------------------------===//
// MAP demeaning for high-dimensional fixed effects
//===--------------------------------------------------------------------===//

// Demean variables with respect to multiple FE dimensions using
// Method of Alternating Projections (symmetric Kaczmarz).
// vars: n_obs x n_vars column-major matrix (modified in-place).
// fe_groups: n_obs x n_fe matrix of integer group IDs (0-based).
// n_fe_levels: number of unique levels per FE dimension.
// tolerance: convergence threshold (default 1e-8).
// max_iter: maximum iterations (default 16000).
// Returns number of iterations used.
int MapDemean(std::vector<double> &vars, int n_obs, int n_vars,
              const std::vector<std::vector<int>> &fe_groups,
              const std::vector<int> &n_fe_levels,
              double tolerance = 1e-8, int max_iter = 16000);

// Count connected components in bipartite graph between two FE dimensions.
// Returns number of connected components (= number of redundant parameters).
int CountConnectedComponents(const std::vector<int> &fe1, const std::vector<int> &fe2,
                             int n_levels1, int n_levels2, int n_obs);

} // namespace dodo
