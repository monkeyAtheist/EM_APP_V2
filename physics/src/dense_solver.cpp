#include "qsig/linalg.h"

#include <thread>

namespace qsig::linalg
{
namespace
{
// row[j] -= f * source[j] for the complex entries first..last, on interleaved (re, im) doubles.
inline void subtractScaled(double *row, const double *source, double fr, double fi, std::size_t first, std::size_t last)
{
    for (std::size_t j = 2 * first; j < 2 * last; j += 2)
    {
        const double sr = source[j], si = source[j + 1];
        row[j] -= fr * sr - fi * si;
        row[j + 1] -= fr * si + fi * sr;
    }
}
} // namespace

bool solveComplexBlocked(DenseMatrix<std::complex<double>> &a, std::vector<std::complex<double>> &b,
                         std::vector<std::complex<double>> &x, PivotStats &stats)
{
    using Complex = std::complex<double>;
    const std::size_t n = b.size();
    if (a.rows() != n || a.cols() != n) return false;
    // std::complex<double> is layout-compatible with double[2] (real, imaginary).
    auto row = [&a](std::size_t r) { return reinterpret_cast<double *>(a.row(r)); };
    const std::size_t blockSize = 32;
    const unsigned cores = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));

    for (std::size_t k0 = 0; k0 < n; k0 += blockSize)
    {
        const std::size_t k1 = std::min(n, k0 + blockSize);
        // Panel: the columns of the block, with partial pivoting; the multipliers stay in place.
        for (std::size_t k = k0; k < k1; ++k)
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
            const Complex inverse = 1.0 / a(k, k);
            const double *rowK = row(k);
            for (std::size_t i = k + 1; i < n; ++i)
            {
                const Complex f = a(i, k) * inverse;
                a(i, k) = f;
                if (f == Complex{}) continue;
                subtractScaled(row(i), rowK, f.real(), f.imag(), k + 1, k1);
                b[i] -= f * b[k];
            }
        }
        if (k1 == n) break;
        // The rows of the block, right of it: U12 = L11^-1 A12.
        for (std::size_t k = k0; k < k1; ++k)
            for (std::size_t i = k + 1; i < k1; ++i)
            {
                const Complex f = a(i, k);
                if (f != Complex{}) subtractScaled(row(i), row(k), f.real(), f.imag(), k1, n);
            }
        // Trailing matrix: A22 -= L21 U12, row by row (each row stays in the cache for the whole block).
        auto update = [&](std::size_t first, std::size_t last)
        {
            for (std::size_t i = first; i < last; ++i)
            {
                double *rowI = row(i);
                for (std::size_t k = k0; k < k1; ++k)
                {
                    const double fr = rowI[2 * k], fi = rowI[2 * k + 1];
                    if (fr != 0.0 || fi != 0.0) subtractScaled(rowI, row(k), fr, fi, k1, n);
                }
            }
        };
        const std::size_t rows = n - k1;
        const unsigned threads = rows * rows * (k1 - k0) > 4'000'000 ? std::min<unsigned>(cores, static_cast<unsigned>(rows)) : 1u;
        if (threads <= 1)
            update(k1, n);
        else
        {
            std::vector<std::thread> pool;
            for (unsigned t = 0; t < threads; ++t)
                pool.emplace_back(update, k1 + rows * t / threads, k1 + rows * (t + 1) / threads);
            for (std::thread &thread : pool) thread.join();
        }
    }

    x.assign(n, Complex{});
    for (std::size_t i = n; i-- > 0;)
    {
        const double *rowI = row(i);
        double sr = b[i].real(), si = b[i].imag();
        for (std::size_t j = i + 1; j < n; ++j)
        {
            const double ar = rowI[2 * j], ai = rowI[2 * j + 1];
            sr -= ar * x[j].real() - ai * x[j].imag();
            si -= ar * x[j].imag() + ai * x[j].real();
        }
        if (std::abs(a(i, i)) < kSingularPivot) return false;
        x[i] = Complex(sr, si) / a(i, i);
    }
    return true;
}
} // namespace qsig::linalg
