// Constants, vector type, fast sine/cosine and dense linear solver.

#include "qsig_test.h"

#include "qsig/constants.h"
#include "qsig/em_calculator_model.h"
#include "qsig/em_engineering_model.h"
#include "qsig/fast_math.h"
#include "qsig/fdtd_solver.h"
#include "qsig/linalg.h"
#include "qsig/numerical_em_solvers.h"
#include "qsig/vec3.h"

#include <algorithm>
#include <chrono>
#include <complex>
#include <cstdio>
#include <random>

namespace k = qsig::constants;

TEST_CASE("constants: CODATA 2022 values and Maxwell consistency", "[constants]")
{
    CHECK_REL(k::c0 * k::c0 * k::mu0 * k::epsilon0, 1.0, 1e-15);
    CHECK_REL(k::epsilon0, 8.8541878188e-12, 1e-10);
    CHECK_REL(k::coulombK, 8.9875517862e9, 1e-10);
    CHECK_REL(k::eta0, 376.730313412, 1e-10);
    CHECK_REL(k::biotSavartK, 0.99999999987e-7, 1e-10); // mu0 / 4 pi, CODATA 2022
}

TEST_CASE("constants: every solver aliases the single definition", "[constants]")
{
    CHECK(NumericalEM::Mu0 == k::mu0);
    CHECK(NumericalEM::Epsilon0 == k::epsilon0);
    CHECK(FDTD::mu0 == k::mu0);
    CHECK(FDTD::epsilon0 == k::epsilon0);
    CHECK(EmCalculator::mu0 == k::mu0);
    CHECK(EmCalculator::epsilon0 == k::epsilon0);
    CHECK(EmEngineering::Mu0 == k::mu0);
    CHECK(EmEngineering::Eta0 == k::eta0);
}

TEST_CASE("vec3: algebra and the +Z fallback of normalized()", "[core]")
{
    const qsig::Vec3 a{1.0, 2.0, 3.0}, b{-2.0, 0.5, 4.0};
    CHECK(dot(a, b) == 11.0);
    const auto c = cross(a, b);
    CHECK(dot(c, a) == 0.0);
    CHECK(dot(c, b) == 0.0);
    CHECK_REL(normalized(a).norm(), 1.0, 1e-15);
    const auto z = normalized(qsig::Vec3{});
    CHECK(z.x == 0.0 && z.y == 0.0 && z.z == 1.0);
}

namespace
{
template <class T>
double residual(const std::vector<std::vector<T>> &a, const std::vector<T> &x, const std::vector<T> &b)
{
    double worst = 0.0, scale = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        T s{};
        for (std::size_t j = 0; j < x.size(); ++j) s += a[i][j] * x[j];
        worst = std::max(worst, std::abs(s - b[i]));
        scale = std::max(scale, std::abs(b[i]));
    }
    return worst / scale;
}
} // namespace

TEST_CASE("sinCos: within 5e-16 of the library up to 1e5 rad, the library beyond", "[core]")
{
    std::mt19937_64 random(7);
    double worst = 0.0;
    for (double range : {1.0, 50.0, 1e4, 0.999e5})
    {
        std::uniform_real_distribution<double> uniform(-range, range);
        for (int i = 0; i < 200000; ++i)
        {
            const double x = uniform(random);
            double s = 0.0, c = 0.0;
            qsig::sinCos(x, s, c);
            worst = std::max({worst, std::abs(s - std::sin(x)), std::abs(c - std::cos(x))});
        }
    }
    CHECK(worst < 5e-16);
    // Quadrant boundaries and exact points.
    for (double x : {0.0, 1e-300, -1e-300})
    {
        double s = 1.0, c = 0.0;
        qsig::sinCos(x, s, c);
        CHECK(s == x);
        CHECK(c == 1.0);
    }
    for (int q = -8; q <= 8; ++q)
    {
        const double x = q * 0.25 * k::pi;
        double s = 0.0, c = 0.0;
        qsig::sinCos(x, s, c);
        CHECK(std::abs(s - std::sin(x)) < 5e-16);
        CHECK(std::abs(c - std::cos(x)) < 5e-16);
    }
    double s = 0.0, c = 0.0;
    qsig::sinCos(3e7, s, c);
    CHECK(s == std::sin(3e7));
    CHECK(c == std::cos(3e7));
}

TEST_CASE("linalg: complex system with pivoting", "[linalg]")
{
    using C = std::complex<double>;
    std::mt19937 rng(42);
    std::uniform_real_distribution<double> u(-1.0, 1.0);
    const std::size_t n = 120;
    std::vector<std::vector<C>> a(n, std::vector<C>(n));
    std::vector<C> b(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        for (std::size_t j = 0; j < n; ++j) a[i][j] = {u(rng), u(rng)};
        a[i][i] = 0.0; // forces row exchanges
        b[i] = {u(rng), u(rng)};
    }
    std::vector<C> x;
    qsig::linalg::PivotStats stats;
    REQUIRE(qsig::linalg::solve(a, b, x, stats));
    CHECK(residual(a, x, b) < 1e-12);
    CHECK(stats.minPivot > 0.0);
    CHECK(stats.maxPivot >= stats.minPivot);
}

TEST_CASE("linalg: the blocked complex solver gives the solution of the plain one", "[linalg]")
{
    using C = std::complex<double>;
    std::mt19937 rng(11);
    std::uniform_real_distribution<double> u(-1.0, 1.0);
    // 450 unknowns: more than one block, and enough rows for the trailing update to be shared between threads.
    for (const std::size_t n : {std::size_t{1}, std::size_t{37}, std::size_t{450}})
    {
        qsig::linalg::DenseMatrix<C> plain(n, n), blocked(n, n);
        std::vector<std::vector<C>> rows(n, std::vector<C>(n));
        std::vector<C> b(n);
        for (std::size_t i = 0; i < n; ++i)
        {
            for (std::size_t j = 0; j < n; ++j) rows[i][j] = {u(rng), u(rng)};
            if (n > 1) rows[i][i] = 0.0; // forces row exchanges
            b[i] = {u(rng), u(rng)};
            for (std::size_t j = 0; j < n; ++j) plain(i, j) = blocked(i, j) = rows[i][j];
        }
        std::vector<C> b1 = b, b2 = b, x1, x2;
        qsig::linalg::PivotStats s1, s2;
        REQUIRE(qsig::linalg::solveInPlace(plain, b1, x1, s1));
        REQUIRE(qsig::linalg::solveComplexBlocked(blocked, b2, x2, s2));
        CHECK(residual(rows, x2, b) < 1e-11);
        double difference = 0.0, scale = 0.0;
        for (std::size_t i = 0; i < n; ++i)
        {
            difference = std::max(difference, std::abs(x1[i] - x2[i]));
            scale = std::max(scale, std::abs(x1[i]));
        }
        CHECK(difference < 1e-9 * scale);
        CHECK_REL(s2.maxPivot, s1.maxPivot, 1e-9);
    }
    // A singular system is refused.
    qsig::linalg::DenseMatrix<C> singular(3, 3);
    const double values[3][3] = {{1.0, 2.0, 3.0}, {2.0, 4.0, 6.0}, {0.0, 1.0, 1.0}};
    for (std::size_t i = 0; i < 3; ++i)
        for (std::size_t j = 0; j < 3; ++j) singular(i, j) = values[i][j];
    std::vector<C> b{1.0, 2.0, 3.0}, x;
    qsig::linalg::PivotStats stats;
    CHECK(!qsig::linalg::solveComplexBlocked(singular, b, x, stats));
}

TEST_CASE("linalg: singular matrix is rejected", "[linalg]")
{
    std::vector<std::vector<double>> a{{1.0, 2.0, 3.0}, {2.0, 4.0, 6.0}, {0.0, 1.0, 1.0}};
    std::vector<double> b{1.0, 2.0, 3.0}, x;
    qsig::linalg::PivotStats stats;
    CHECK(!qsig::linalg::solve(a, b, x, stats));
}

TEST_CASE("linalg: timing of a 600 x 600 complex solve (informative)", "[linalg][bench]")
{
    using C = std::complex<double>;
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> u(-1.0, 1.0);
    const std::size_t n = 600;
    qsig::linalg::DenseMatrix<C> m(n, n);
    std::vector<C> b(n), x;
    for (std::size_t i = 0; i < n; ++i)
    {
        for (std::size_t j = 0; j < n; ++j) m(i, j) = {u(rng), u(rng)};
        m(i, i) += C{double(n), 0.0};
        b[i] = {u(rng), u(rng)};
    }
    qsig::linalg::PivotStats stats;
    const auto t0 = std::chrono::steady_clock::now();
    REQUIRE(qsig::linalg::solveInPlace(m, b, x, stats));
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("    600x600 complex LU: %.0f ms\n", ms);
}
