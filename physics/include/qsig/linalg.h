#pragma once

// Dense linear solver shared by every MoM / BEM solver.
//
// Replaces four hand-copied Gaussian eliminations that took std::vector<std::vector<T>>
// by value. Semantics are kept identical so results do not move:
//  - LU with partial (row) pivoting on |a_ik|,
//  - failure when a pivot magnitude is <= 1e-18 or not finite,
//  - statistics = min/max pivot magnitude seen during elimination.
// Storage is one contiguous row-major block. The backend can later be swapped
// (Eigen, LAPACK) behind this interface without touching the solvers.

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <limits>
#include <vector>

namespace qsig::linalg
{
struct PivotStats
{
    double minPivot = std::numeric_limits<double>::infinity();
    double maxPivot = 0.0;
};

template <class T> class DenseMatrix
{
public:
    DenseMatrix() = default;
    DenseMatrix(std::size_t rows, std::size_t cols, T value = T{})
        : m_rows(rows), m_cols(cols), m_data(rows * cols, value)
    {
    }

    std::size_t rows() const noexcept { return m_rows; }
    std::size_t cols() const noexcept { return m_cols; }
    T &operator()(std::size_t r, std::size_t c) noexcept { return m_data[r * m_cols + c]; }
    const T &operator()(std::size_t r, std::size_t c) const noexcept { return m_data[r * m_cols + c]; }
    T *row(std::size_t r) noexcept { return m_data.data() + r * m_cols; }
    const T *row(std::size_t r) const noexcept { return m_data.data() + r * m_cols; }

    static DenseMatrix fromRows(const std::vector<std::vector<T>> &rows)
    {
        const std::size_t n = rows.size();
        const std::size_t m = n ? rows.front().size() : 0;
        DenseMatrix out(n, m);
        for (std::size_t i = 0; i < n; ++i) std::copy_n(rows[i].begin(), std::min(m, rows[i].size()), out.row(i));
        return out;
    }

private:
    std::size_t m_rows = 0;
    std::size_t m_cols = 0;
    std::vector<T> m_data;
};

inline constexpr double kSingularPivot = 1e-18;

// Solves a x = b in place: 'a' is overwritten by its LU factors, 'b' by intermediate values.
template <class T> bool solveInPlace(DenseMatrix<T> &a, std::vector<T> &b, std::vector<T> &x, PivotStats &stats)
{
    const std::size_t n = b.size();
    if (a.rows() != n || a.cols() != n) return false;
    for (std::size_t k = 0; k < n; ++k)
    {
        std::size_t pivot = k;
        double best = std::abs(a(k, k));
        for (std::size_t i = k + 1; i < n; ++i)
        {
            const double candidate = std::abs(a(i, k));
            if (candidate > best)
            {
                best = candidate;
                pivot = i;
            }
        }
        if (!(best > kSingularPivot) || !std::isfinite(best)) return false;
        if (pivot != k)
        {
            std::swap_ranges(a.row(pivot), a.row(pivot) + n, a.row(k));
            std::swap(b[pivot], b[k]);
        }
        stats.minPivot = std::min(stats.minPivot, best);
        stats.maxPivot = std::max(stats.maxPivot, best);
        const T diag = a(k, k);
        const T *rowK = a.row(k);
        for (std::size_t i = k + 1; i < n; ++i)
        {
            T *rowI = a.row(i);
            const T f = rowI[k] / diag;
            if (std::abs(f) == 0.0) continue;
            rowI[k] = T{};
            for (std::size_t j = k + 1; j < n; ++j) rowI[j] -= f * rowK[j];
            b[i] -= f * b[k];
        }
    }
    x.assign(n, T{});
    for (std::size_t ii = n; ii-- > 0;)
    {
        const T *rowI = a.row(ii);
        T s = b[ii];
        for (std::size_t j = ii + 1; j < n; ++j) s -= rowI[j] * x[j];
        if (std::abs(rowI[ii]) < kSingularPivot) return false;
        x[ii] = s / rowI[ii];
    }
    return true;
}

// Convenience overload for the legacy row-of-vectors representation (copies once).
template <class T>
bool solve(const std::vector<std::vector<T>> &a, std::vector<T> b, std::vector<T> &x, PivotStats &stats)
{
    if (a.size() != b.size()) return false;
    auto m = DenseMatrix<T>::fromRows(a);
    return solveInPlace(m, b, x, stats);
}
} // namespace qsig::linalg
