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

} // namespace dodo
