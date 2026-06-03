#include "regression.hpp"

#include <cmath>
#include <stdexcept>

namespace dodo {

//===--------------------------------------------------------------------===//
// Cholesky decomposition (row-major, lower triangle, in-place)
//===--------------------------------------------------------------------===//

static bool CholeskyDecompose(std::vector<double> &A, int k) {
	for (int j = 0; j < k; j++) {
		double sum = 0.0;
		for (int s = 0; s < j; s++) {
			sum += A[j * k + s] * A[j * k + s];
		}
		double diag = A[j * k + j] - sum;
		if (diag <= 0.0) {
			return false;
		}
		A[j * k + j] = std::sqrt(diag);
		for (int i = j + 1; i < k; i++) {
			double sum2 = 0.0;
			for (int s = 0; s < j; s++) {
				sum2 += A[i * k + s] * A[j * k + s];
			}
			A[i * k + j] = (A[i * k + j] - sum2) / A[j * k + j];
		}
	}
	return true;
}

// Solve L L^T x = b given Cholesky factor L (modifies b in-place)
static void CholeskySolveInPlace(const std::vector<double> &L, std::vector<double> &b, int k) {
	// Forward substitution: L y = b
	for (int i = 0; i < k; i++) {
		for (int j = 0; j < i; j++) {
			b[i] -= L[i * k + j] * b[j];
		}
		b[i] /= L[i * k + i];
	}
	// Back substitution: L^T x = y
	for (int i = k - 1; i >= 0; i--) {
		for (int j = i + 1; j < k; j++) {
			b[i] -= L[j * k + i] * b[j];
		}
		b[i] /= L[i * k + i];
	}
}

// Compute full inverse A^{-1} given Cholesky factor L
static std::vector<double> CholeskyInverse(const std::vector<double> &L, int k) {
	std::vector<double> inv(static_cast<size_t>(k) * k, 0.0);
	for (int col = 0; col < k; col++) {
		std::vector<double> e(k, 0.0);
		e[col] = 1.0;
		CholeskySolveInPlace(L, e, k);
		for (int row = 0; row < k; row++) {
			inv[row * k + col] = e[row];
		}
	}
	return inv;
}

// Factor X'X via Cholesky, throwing on failure
static std::vector<double> FactorXtX(const std::vector<double> &xtx, int k) {
	std::vector<double> A = xtx;
	if (!CholeskyDecompose(A, k)) {
		throw std::runtime_error("X'X is not positive definite (perfect collinearity?)");
	}
	return A;
}

//===--------------------------------------------------------------------===//
// Public API
//===--------------------------------------------------------------------===//

std::vector<double> OlsSolve(const std::vector<double> &xtx, const std::vector<double> &xty, int k) {
	auto L = FactorXtX(xtx, k);
	std::vector<double> b = xty;
	CholeskySolveInPlace(L, b, k);
	return b;
}

std::vector<double> OlsInvDiag(const std::vector<double> &xtx, int k) {
	auto L = FactorXtX(xtx, k);
	auto inv = CholeskyInverse(L, k);
	std::vector<double> diag(k);
	for (int i = 0; i < k; i++) {
		diag[i] = inv[i * k + i];
	}
	return diag;
}

std::vector<double> SandwichDiag(const std::vector<double> &xtx, const std::vector<double> &meat, int k) {
	auto L = FactorXtX(xtx, k);
	auto Ainv = CholeskyInverse(L, k);
	// V = Ainv * M * Ainv — only need diagonal
	std::vector<double> diag(k, 0.0);
	for (int i = 0; i < k; i++) {
		for (int j = 0; j < k; j++) {
			double am = 0.0;
			for (int s = 0; s < k; s++) {
				am += Ainv[i * k + s] * meat[s * k + j];
			}
			diag[i] += am * Ainv[i * k + j];
		}
	}
	return diag;
}

FitStats ComputeFitStats(double ess, double tss, int n, int k) {
	FitStats fs;
	fs.ess = ess;
	fs.tss = tss;
	fs.mss = tss - ess;
	fs.n = n;
	fs.k = k;
	fs.r2 = 1.0 - ess / tss;
	fs.r2_adj = 1.0 - (ess / (n - k)) / (tss / (n - 1));
	fs.f_stat = (fs.mss / (k - 1)) / (ess / (n - k));
	fs.rmse = std::sqrt(ess / (n - k));
	return fs;
}

double NormalPValue(double t_stat) {
	// Two-sided p-value using normal approximation: 2 * Phi(-|t|)
	// erfc(x/sqrt(2))/2 = Phi(-x) for x >= 0
	return std::erfc(std::abs(t_stat) / std::sqrt(2.0));
}

} // namespace dodo
