#include "regression.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
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

//===--------------------------------------------------------------------===//
// MAP demeaning (Method of Alternating Projections)
//===--------------------------------------------------------------------===//

int MapDemean(std::vector<double> &vars, int n_obs, int n_vars,
              const std::vector<std::vector<int>> &fe_groups,
              const std::vector<int> &n_fe_levels,
              double tolerance, int max_iter) {
	int n_fe = static_cast<int>(fe_groups.size());
	if (n_fe == 0 || n_obs == 0) {
		return 0;
	}

	// Scratch: group sums and counts per FE dimension
	// We recompute per iteration (counts are constant but sums change)
	std::vector<std::vector<double>> group_sums; // n_fe x max_levels
	std::vector<std::vector<int>> group_counts;  // n_fe x max_levels

	for (int g = 0; g < n_fe; g++) {
		group_sums.emplace_back(n_fe_levels[g], 0.0);
		group_counts.emplace_back(n_fe_levels[g], 0);
	}

	// Precompute counts (constant across iterations)
	for (int g = 0; g < n_fe; g++) {
		auto &counts = group_counts[g];
		auto &groups = fe_groups[g];
		for (int i = 0; i < n_obs; i++) {
			counts[groups[i]]++;
		}
	}

	// Store previous values for convergence check
	std::vector<double> prev(vars.size());

	int iter;
	for (iter = 0; iter < max_iter; iter++) {
		// Save current state
		prev = vars;

		// Symmetric Kaczmarz: forward sweep 0..n_fe-1, backward n_fe-2..0
		// Forward sweep
		for (int g = 0; g < n_fe; g++) {
			auto &sums = group_sums[g];
			auto &counts = group_counts[g];
			auto &groups = fe_groups[g];

			for (int v = 0; v < n_vars; v++) {
				double *col = &vars[static_cast<size_t>(v) * n_obs];
				// Compute group sums
				std::fill(sums.begin(), sums.end(), 0.0);
				for (int i = 0; i < n_obs; i++) {
					sums[groups[i]] += col[i];
				}
				// Subtract group means
				for (int i = 0; i < n_obs; i++) {
					col[i] -= sums[groups[i]] / counts[groups[i]];
				}
			}
		}

		// Backward sweep (for symmetric Kaczmarz, needed for CG later)
		for (int g = n_fe - 2; g >= 0; g--) {
			auto &sums = group_sums[g];
			auto &counts = group_counts[g];
			auto &groups = fe_groups[g];

			for (int v = 0; v < n_vars; v++) {
				double *col = &vars[static_cast<size_t>(v) * n_obs];
				std::fill(sums.begin(), sums.end(), 0.0);
				for (int i = 0; i < n_obs; i++) {
					sums[groups[i]] += col[i];
				}
				for (int i = 0; i < n_obs; i++) {
					col[i] -= sums[groups[i]] / counts[groups[i]];
				}
			}
		}

		// Convergence: max relative difference across all variables
		double max_reldif = 0.0;
		for (size_t j = 0; j < vars.size(); j++) {
			double denom = std::max(std::abs(prev[j]), 1e-15);
			double rd = std::abs(vars[j] - prev[j]) / denom;
			if (rd > max_reldif) {
				max_reldif = rd;
			}
		}

		if (max_reldif < tolerance) {
			break;
		}
	}

	return iter + 1;
}

//===--------------------------------------------------------------------===//
// Connected components (for df_a redundancy in 2+ FE)
//===--------------------------------------------------------------------===//

int CountConnectedComponents(const std::vector<int> &fe1, const std::vector<int> &fe2,
                             int n_levels1, int n_levels2, int n_obs) {
	// Union-Find on the bipartite graph
	int total = n_levels1 + n_levels2;
	std::vector<int> parent(total);
	std::vector<int> rank(total, 0);
	for (int i = 0; i < total; i++) {
		parent[i] = i;
	}

	// Find with path compression
	std::function<int(int)> find = [&](int x) -> int {
		if (parent[x] != x) {
			parent[x] = find(parent[x]);
		}
		return parent[x];
	};

	// Union by rank
	auto unite = [&](int a, int b) {
		a = find(a);
		b = find(b);
		if (a == b) {
			return;
		}
		if (rank[a] < rank[b]) {
			std::swap(a, b);
		}
		parent[b] = a;
		if (rank[a] == rank[b]) {
			rank[a]++;
		}
	};

	// Connect fe1[i] with fe2[i] + n_levels1
	for (int i = 0; i < n_obs; i++) {
		unite(fe1[i], fe2[i] + n_levels1);
	}

	// Count unique roots among used levels
	std::vector<bool> used(total, false);
	for (int i = 0; i < n_obs; i++) {
		used[fe1[i]] = true;
		used[fe2[i] + n_levels1] = true;
	}

	std::vector<bool> root_seen(total, false);
	int components = 0;
	for (int i = 0; i < total; i++) {
		if (used[i]) {
			int r = find(i);
			if (!root_seen[r]) {
				root_seen[r] = true;
				components++;
			}
		}
	}

	return components;
}

} // namespace dodo
