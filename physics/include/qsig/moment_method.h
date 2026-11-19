#pragma once

// Method of moments for thin wires and perfectly conducting surfaces in free space, in one system.
//
// Electric field integral equation in its mixed-potential form, tested by the basis functions
// themselves (Galerkin):
//     Z[m][n] = j w mu  ∫∫ f_m . f_n G  +  1 / (j w eps)  ∫∫ (div f_m)(div f_n) G,
//     G = exp(-j k R) / (4 pi R)
// - wires: piecewise-linear ("triangle") currents on straight segments, reduced kernel
//   R = sqrt(|r - r'|^2 + a^2); wires that touch are joined (end to end, T, X);
// - surfaces: Rao-Wilton-Glisson functions on flat triangles, the 1/R part integrated exactly;
// - a wire that ends on a surface is attached to it by the junction function of Hwu and Wilton:
//   the current of the wire spreads radially in the triangles around the mesh vertex where it lands.
// Sources are delta gaps: at a point of a wire, at the foot of a wire attached to a surface (between
// the wire and the surface), or across an edge of a surface mesh.
//
// Dielectric bodies (homogeneous, each with a closed skin of triangles): surface equivalence, in the
// formulation of Poggio, Miller, Chang, Harrington, Wu and Tsai. The skin carries an electric current
// J = n x H and a magnetic current M = -n x E, both in RWG functions; outside, they radiate in air with
// the wires and the conductors; inside, their opposites radiate in the medium of the body. The
// tangential E and H are continuous across the skin:
//     E-rows:  (Z1 + Z2) J - (K1 + K2) M = <f, E inc>,     H-rows:  (K1 + K2) J + (Z1 / eta1^2 + Z2 / eta2^2) M = <f, H inc>
// with Z the operator above in each medium and K[m][n] = ∫∫ f_m . (f_n x grad G) (principal value).
// A conductor may not touch a dielectric yet, nor two dielectrics each other.
//
// The solver replaces the hybrid wire + RWG solver inherited from 6.14 (hybrid_wire_surface_mom),
// measured on a quarter-wave monopole over a one-wavelength plate: impedance drifting with the
// mesh (65 + j35 to 56 + j9 ohms), power balance 0.78 to 0.92, 42 s for 431 unknowns.

#include "qsig/vec3.h"

#include <complex>
#include <cstddef>
#include <string>
#include <vector>

namespace qsig::mom
{
using Complex = std::complex<double>;

struct ComplexVec3
{
    Complex x, y, z;
};

struct Wire
{
    Vec3 a, b;
    double radius = 1e-3;
    int tag = -1; // returned with the segments made from this wire
};

struct Triangle
{
    Vec3 a, b, c;
    int tag = -1;
    // -1: a conducting sheet. Otherwise the index, in Input::dielectrics, of the body whose skin this
    // triangle belongs to.
    int body = -1;
};

// A homogeneous dielectric body. Its skin is the closed set of the triangles that name it.
struct Dielectric
{
    // eps' - j eps'': the imaginary part is negative in a lossy medium (eps'' = eps' tan delta).
    Complex relativePermittivity{1.0, 0.0};
};

// A plane wave E = field exp(-j k direction . r) falling on the scene (the tests against the exact
// solutions of scattering); 'field' is perpendicular to 'direction'. Off when the field is null.
struct PlaneWave
{
    Vec3 direction{0.0, 0.0, 1.0};
    Vec3 field{0.0, 0.0, 0.0};
};

struct Feed
{
    Vec3 position;
    Complex voltage{1.0, 0.0};
    // false: on a wire, at 'position' (the solver puts a node there); at the end of a wire that lies
    // on a surface, the gap is between the wire and the surface.
    // true: across the edge of the surface mesh nearest to 'position'.
    bool onSurface = false;
};

struct Input
{
    double frequency = 300e6;
    std::vector<Wire> wires;
    std::vector<Triangle> triangles; // conforming mesh: neighbours share whole edges
    std::vector<Feed> feeds;
    std::vector<Dielectric> dielectrics;
    PlaneWave incident;
    double segmentsPerWavelength = 40.0;
    double tolerance = 1e-6; // points closer than this are one point [m]
    int maxUnknowns = 2500;
};

struct Segment
{
    Vec3 p0, p1;
    double radius = 0.0;
    int tag = -1;
    Complex i0, i1; // current at both ends, along p0 -> p1 [A]
};

struct Facet
{
    Vec3 a, b, c;
    int tag = -1;
    // Surface current density at three inner points, each two thirds of the way to a corner (a, b, c)
    // [A/m]: at the corner where a wire lands the density of its current grows as 1 / distance.
    ComplexVec3 ja, jb, jc;
    // On the skin of a dielectric body ('body' as in Triangle): the magnetic current density at the same
    // points [V/m], M = -n x E.
    int body = -1;
    ComplexVec3 ma, mb, mc;
};

// Current moment of a small piece of surface, for the far field [A m].
struct Moment
{
    Vec3 position;
    ComplexVec3 moment;
};

struct FeedResult
{
    Vec3 position; // where the gap is
    Complex voltage, current, impedance;
};

struct Result
{
    bool valid = false;
    std::string error;
    int unknowns = 0, wireUnknowns = 0, surfaceUnknowns = 0, attachments = 0;
    int magneticUnknowns = 0; // magnetic currents on the skins of the dielectric bodies (counted in 'unknowns')
    std::vector<Segment> segments;
    std::vector<Facet> facets;
    std::vector<Moment> moments; // all the surface currents (junction currents included)
    std::vector<Moment> magneticMoments; // magnetic currents of the dielectric skins [V m]
    std::vector<FeedResult> feeds;
};

Result solve(const Input &input);

// Far field of solved currents: F(r^) = sum of the current moments times exp(j k r^.r), so that
// E = -j k eta exp(-j k r) / (4 pi r) F_transverse.
// Magnetic currents (dielectric skins) radiate too: with L the same sum of their moments,
// F_transverse becomes F_transverse - (r^ x L) / eta.
class Radiator
{
public:
    explicit Radiator(double k) : m_k(k) {}
    Radiator(const Result &result, double k);
    // Linear current i0 -> i1 along p0 -> p1: 2 Gauss points while the phase turns by less than
    // 0.8 rad along the segment, 4 beyond.
    void addSegment(const Vec3 &p0, const Vec3 &p1, const Complex &i0, const Complex &i1);
    void addMoment(const Vec3 &position, const ComplexVec3 &moment);
    void addMagneticMoment(const Vec3 &position, const ComplexVec3 &moment);
    std::size_t points() const { return m_px.size(); }
    // Transverse part of F in the direction 'dir' (unit vector), and its squared modulus.
    ComplexVec3 field(const Vec3 &dir) const;
    double intensity(const Vec3 &dir) const;
    // Radiated power: eta k^2 / (32 pi^2) times the integral of the intensity over the sphere.
    double power(double intensityIntegral) const;

private:
    double m_k;
    std::vector<double> m_px, m_py, m_pz;
    std::vector<Complex> m_mx, m_my, m_mz;
    std::vector<double> m_qx, m_qy, m_qz; // magnetic moments: positions, values
    std::vector<Complex> m_lx, m_ly, m_lz;
};

// ---- Surface meshes ----

// Rectangle centred at 'centre', 'width' along u and 'height' along v (unit, perpendicular), in
// cells of 'step' at most, each cut in two triangles. The mesh has a vertex at each of 'points'
// that lies on the rectangle (where a wire lands): the grid lines go through them.
std::vector<Triangle> meshRectangle(const Vec3 &centre, const Vec3 &u, const Vec3 &v, double width, double height,
                                    double step, const std::vector<Vec3> &points = {}, int tag = -1);

// Disc: rings of triangles around the centre, 6 n^2 triangles for n rings. A point at the centre is
// the centre vertex; for another point of the disc, the nearest inner vertex is moved there.
std::vector<Triangle> meshDisc(const Vec3 &centre, const Vec3 &u, const Vec3 &v, double radius, double step,
                               const std::vector<Vec3> &points = {}, int tag = -1);

// Surface of revolution about the axis w through 'centre' (u, v, w: unit, perpendicular): the skin
// of a cylinder, a cone, a sphere. The profile is a broken line of points (radius, height along w),
// in order; a radius of zero puts the point on the axis (apex, pole, centre of a flat end). Each
// stretch of the profile is cut in rings 'step' apart at most, each ring in sides of 'step' at most
// (a multiple of four of them, eight at least), and the rings are sewn with triangles. The surface
// is closed when the profile starts and ends on the axis. For each of 'points' (a wire landing on
// the surface), the nearest vertex within one step is moved there.
std::vector<Triangle> meshRevolution(const Vec3 &centre, const Vec3 &u, const Vec3 &v, const Vec3 &w,
                                     const std::vector<std::pair<double, double>> &profile, double step,
                                     const std::vector<Vec3> &points = {}, int tag = -1);

// Surface swept by a broken line (the profile, points in space) along the unit direction 'along', over
// 'length' centred on the profile: a parabolic cylinder, a bent sheet. Each stretch of the profile and
// the length are cut in pieces of 'step' at most, each cell in two triangles. For each of 'points'
// (a wire landing on the surface), the nearest vertex within one step is moved there.
std::vector<Triangle> meshSweep(const std::vector<Vec3> &profile, const Vec3 &along, double length, double step,
                                const std::vector<Vec3> &points = {}, int tag = -1);

// ---- Integrals over a flat triangle, exposed for the tests ----

// ∫ dS'/R and ∫ (r' - rho)/R dS' over the triangle (a, b, c), rho being the projection of r on its
// plane (Wilton, Rao, Glisson, Schaubert, Al-Bundak, Butler, 1984).
void staticTriangleIntegrals(const Vec3 &a, const Vec3 &b, const Vec3 &c, const Vec3 &r, double &inverseR,
                             Vec3 &vectorOverR);

// Curl operator of the triangle (a, b, c), seen from r, in a medium of wavenumber k:
//     out[j] = (r - p_j) x ∫ (r' - p_j) (1 + j k R) exp(-j k R) / (4 pi R^3) dS',   p_0 = a, p_1 = b, p_2 = c,
// its singular part integrated exactly (principal value in the plane of the triangle).
void curlTriangleIntegrals(const Vec3 &a, const Vec3 &b, const Vec3 &c, const Vec3 &r, Complex k, ComplexVec3 out[3]);
} // namespace qsig::mom
