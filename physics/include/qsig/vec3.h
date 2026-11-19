#pragma once

// The one 3D vector type of the physics core. Plain aggregate (Vec3{x, y, z}),
// free arithmetic operators found by argument-dependent lookup.

#include <cmath>

namespace qsig
{
struct Vec3
{
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;

    constexpr double normSquared() const noexcept { return x * x + y * y + z * z; }
    double norm() const noexcept { return std::sqrt(normSquared()); }

    constexpr Vec3 &operator+=(const Vec3 &o) noexcept
    {
        x += o.x;
        y += o.y;
        z += o.z;
        return *this;
    }
    constexpr Vec3 &operator-=(const Vec3 &o) noexcept
    {
        x -= o.x;
        y -= o.y;
        z -= o.z;
        return *this;
    }
    constexpr Vec3 &operator*=(double s) noexcept
    {
        x *= s;
        y *= s;
        z *= s;
        return *this;
    }
    constexpr Vec3 &operator/=(double s) noexcept
    {
        x /= s;
        y /= s;
        z /= s;
        return *this;
    }
    // Exact comparison: for "is this the same data" (undo, cached results), not for geometry.
    constexpr bool operator==(const Vec3 &) const noexcept = default;
};

constexpr Vec3 operator+(const Vec3 &a, const Vec3 &b) noexcept
{
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
constexpr Vec3 operator-(const Vec3 &a, const Vec3 &b) noexcept
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
constexpr Vec3 operator-(const Vec3 &a) noexcept
{
    return {-a.x, -a.y, -a.z};
}
constexpr Vec3 operator*(const Vec3 &a, double s) noexcept
{
    return {a.x * s, a.y * s, a.z * s};
}
constexpr Vec3 operator*(double s, const Vec3 &a) noexcept
{
    return a * s;
}
constexpr Vec3 operator/(const Vec3 &a, double s) noexcept
{
    return {a.x / s, a.y / s, a.z / s};
}

constexpr double dot(const Vec3 &a, const Vec3 &b) noexcept
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
constexpr Vec3 cross(const Vec3 &a, const Vec3 &b) noexcept
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline double norm(const Vec3 &a) noexcept
{
    return a.norm();
}

// Unit vector. A (near) zero vector returns +Z: the legacy convention of every solver
// that used to define its own normalized(); callers rely on it for degenerate axes.
inline Vec3 normalized(const Vec3 &a) noexcept
{
    const double n = a.norm();
    return n > 1e-18 ? a * (1.0 / n) : Vec3{0.0, 0.0, 1.0};
}
} // namespace qsig
