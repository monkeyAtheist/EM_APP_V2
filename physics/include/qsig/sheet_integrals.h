#pragma once

// Exact (closed form or controlled-error quadrature) integrals for uniformly charged
// surfaces and volumes, used by the electrostatic kernels.
//
// Every function returns geometric integrals for a UNIT density, without 1/(4 pi eps0):
//     potential = integral of 1 / R,      field = integral of (P - r') / R^3
// over the source (dS' for sheets, dV' for slabs). Multiply by density * coulombK.
//
// Coordinates are local: sheets lie in the plane z = 0, the observation point is p.

#include "qsig/vec3.h"

#include <vector>

namespace qsig::sheet
{
struct Point2
{
    double x = 0.0;
    double y = 0.0;
};

struct Integrals
{
    Vec3 field;
    double potential = 0.0;
    bool onSheet = false; // p lies on the charged surface: the normal field jumps, it is set to 0
    bool onEdge = false;  // p lies on a boundary edge or ring: logarithmic singularity
};

// Uniformly charged convex polygon, vertices counter-clockwise seen from +z.
// Closed form: edge logarithms for the in-plane field and the potential,
// solid angle (Van Oosterom & Strackee) for the normal field.
Integrals convexPolygon(const std::vector<Point2> &ccwVertices, const Vec3 &p);

// Uniformly charged annulus (innerRadius may be 0 for a full disk), centred on the origin.
// Exact ring fields (complete elliptic integrals) integrated over the radius with adaptive
// Gauss-Kronrod quadrature, split at the observation radius. Relative accuracy ~1e-10.
Integrals annulus(double innerRadius, double outerRadius, const Vec3 &p);

// Uniformly charged prism: the convex polygon extruded over z in [-thickness/2, thickness/2].
// Exact polygon sheets integrated over the thickness (adaptive, split at p.z).
Integrals polygonSlab(const std::vector<Point2> &ccwVertices, double thickness, const Vec3 &p);

// Complete elliptic integrals K(k), E(k) of modulus k, by the arithmetic-geometric mean.
void ellipticKE(double k, double &K, double &E);
// Same, with the complementary modulus kc = sqrt(1 - k^2) supplied exactly by the caller
// (needed when k is within rounding of 1).
void ellipticKEc(double k, double kc, double &K, double &E);
} // namespace qsig::sheet
