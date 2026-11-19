// 3D views, computation side: the orbit camera (qsig/app/orbit_camera.h) and the shapes of the sources
// in space (qsig/app/source_shapes.h).

#include "qsig_test.h"

#include "qsig/app/orbit_camera.h"
#include "qsig/app/source_shapes.h"
#include "qsig/constants.h"

#include <cmath>
#include <vector>

using qsig::Vec3;
using qsig::app::OrbitCamera;
using qsig::app::SourceShape3D;

namespace
{
OrbitCamera camera(double yaw, double pitch)
{
    OrbitCamera c;
    c.yaw = yaw;
    c.pitch = pitch;
    c.centre = {0.3, -0.2, 0.1};
    c.radius = 2.0;
    c.width = 800.0;
    c.height = 600.0;
    return c;
}

double pathLength(const SourceShape3D::Path &path)
{
    double total = 0.0;
    for (std::size_t i = 0; i + 1 < path.size(); ++i) total += qsig::norm(path[i + 1] - path[i]);
    return total;
}
} // namespace

TEST_CASE("orbit camera: right-handed screen axes, projection and its inverse", "[view3d]")
{
    for (const auto &[yaw, pitch] : {std::pair{35.0, 20.0}, std::pair{-120.0, -65.0}, std::pair{0.0, 90.0}, std::pair{90.0, 0.0}})
    {
        const OrbitCamera c = camera(yaw, pitch);
        const auto f = c.frame();
        CHECK_NEAR(qsig::norm(f.right), 1.0, 0.0, 1e-12);
        CHECK_NEAR(qsig::norm(f.up), 1.0, 0.0, 1e-12);
        CHECK_NEAR(qsig::dot(f.right, f.up), 0.0, 0.0, 1e-12);
        // right x up points at the viewer: against forward.
        CHECK_NEAR(qsig::norm(qsig::cross(f.right, f.up) + f.forward), 0.0, 0.0, 1e-12);
        const Vec3 p{0.7, 0.4, -0.9};
        const auto s = f.project(p);
        CHECK_NEAR(qsig::norm(f.unproject(s.x, s.y, s.depth) - p), 0.0, 0.0, 1e-12);
        // The centre of the scene is drawn at the centre of the viewport.
        const auto o = f.project(c.centre);
        CHECK_NEAR(o.x, 400.0, 0.0, 1e-9);
        CHECK_NEAR(o.y, 300.0, 0.0, 1e-9);
    }
    // 0.34 x the small side of the viewport for the radius of the scene.
    CHECK_NEAR(camera(0.0, 0.0).scale(), 0.34 * 600.0 / 2.0, 0.0, 1e-12);
}

TEST_CASE("orbit camera: arrows of the view cube step by 45 degrees on the grid, fit keeps the side", "[view3d]")
{
    // From the initial free view (35, 20): the nearest grid angles are 45 and 0.
    OrbitCamera c = camera(35.0, 20.0);
    c.step(+1, 0);
    CHECK_NEAR(c.yaw, 90.0, 0.0, 1e-12);
    CHECK_NEAR(c.pitch, 20.0, 0.0, 1e-12); // untouched
    c = camera(35.0, 20.0);
    c.step(-1, 0);
    CHECK_NEAR(c.yaw, 0.0, 0.0, 1e-12);
    c.step(0, +1);
    CHECK_NEAR(c.pitch, 45.0, 0.0, 1e-12);
    c.step(0, -1);
    CHECK_NEAR(c.pitch, 0.0, 0.0, 1e-12);
    CHECK(c.facingPlane() == 1); // the front view, plane XZ
    // Two steps to the right of the front view: the view from the right (the viewer at +x).
    c.step(+1, 0);
    CHECK(c.facingPlane() == -1);
    c.step(+1, 0);
    CHECK(c.facingPlane() == 2);
    CHECK_NEAR(c.frame().forward.x, -1.0, 0.0, 1e-12);
    // Two steps up: the view from above, which is an end.
    c.step(0, +1);
    c.step(0, +1);
    CHECK(c.facingPlane() == 0);
    CHECK_NEAR(c.frame().forward.z, -1.0, 0.0, 1e-12);
    c.step(0, +1);
    CHECK_NEAR(c.pitch, 90.0, 0.0, 1e-12);
    // All the way round: the yaw stays bounded and the view is the same.
    c = camera(0.0, 0.0);
    for (int i = 0; i < 8; ++i) c.step(+1, 0);
    CHECK(std::abs(c.yaw) < 360.0);
    CHECK_NEAR(qsig::norm(c.frame().right - Vec3{1, 0, 0}), 0.0, 0.0, 1e-12);
    // Fit: zoom and slide are forgotten, not the orientation.
    c = camera(-70.0, 33.0);
    c.zoomAt(120.0, 80.0, 3.0);
    c.pan(40.0, -25.0);
    c.fit();
    CHECK_NEAR(c.zoom, 1.0, 0.0, 0.0);
    CHECK_NEAR(c.panX, 0.0, 0.0, 0.0);
    CHECK_NEAR(c.panY, 0.0, 0.0, 0.0);
    CHECK_NEAR(c.yaw, -70.0, 0.0, 0.0);
    CHECK_NEAR(c.pitch, 33.0, 0.0, 0.0);
    const auto o = c.frame().project(c.centre);
    CHECK_NEAR(o.x, 400.0, 0.0, 1e-9);
    CHECK_NEAR(o.y, 300.0, 0.0, 1e-9);
}

TEST_CASE("orbit camera: exact views match the planes of the 2D views", "[view3d]")
{
    OrbitCamera c = camera(12.0, 34.0);
    CHECK(c.facingPlane() == -1);
    // From above: x to the right, y up (plane XY, seen from +z).
    c.lookFrom(2, +1);
    auto f = c.frame();
    CHECK(c.facingPlane() == 0);
    CHECK_NEAR(qsig::norm(f.right - Vec3{1, 0, 0}), 0.0, 0.0, 1e-12);
    CHECK_NEAR(qsig::norm(f.up - Vec3{0, 1, 0}), 0.0, 0.0, 1e-12);
    CHECK_NEAR(qsig::norm(f.forward - Vec3{0, 0, -1}), 0.0, 0.0, 1e-12);
    // From the front: x to the right, z up (plane XZ, the viewer at -y).
    c.lookFrom(1, -1);
    f = c.frame();
    CHECK(c.facingPlane() == 1);
    CHECK_NEAR(qsig::norm(f.right - Vec3{1, 0, 0}), 0.0, 0.0, 1e-12);
    CHECK_NEAR(qsig::norm(f.up - Vec3{0, 0, 1}), 0.0, 0.0, 1e-12);
    CHECK_NEAR(qsig::norm(f.forward - Vec3{0, 1, 0}), 0.0, 0.0, 1e-12);
    // From the right: y to the right, z up (plane YZ, the viewer at +x).
    c.lookFrom(0, +1);
    f = c.frame();
    CHECK(c.facingPlane() == 2);
    CHECK_NEAR(qsig::norm(f.right - Vec3{0, 1, 0}), 0.0, 0.0, 1e-12);
    CHECK_NEAR(qsig::norm(f.up - Vec3{0, 0, 1}), 0.0, 0.0, 1e-12);
    CHECK_NEAR(qsig::norm(f.forward - Vec3{-1, 0, 0}), 0.0, 0.0, 1e-12);
    // The opposite sides look the other way.
    c.lookFrom(2, -1);
    CHECK_NEAR(c.frame().forward.z, 1.0, 0.0, 1e-12);
    c.lookFrom(1, +1);
    CHECK_NEAR(c.frame().forward.y, -1.0, 0.0, 1e-12);
    c.lookFrom(0, -1);
    CHECK_NEAR(c.frame().forward.x, 1.0, 0.0, 1e-12);
    CHECK(c.facingPlane() == 2);
}

TEST_CASE("orbit camera: facing a plane of any orientation (the turned construction plane)", "[view3d]")
{
    // The XY plane turned by 30 degrees about x, then 20 about y, then 50 about z (the order of the
    // designer): its normal, and the view that faces it.
    auto turn = [](const Vec3 &v, double rxDeg, double ryDeg, double rzDeg)
    {
        const double rx = rxDeg * qsig::constants::pi / 180.0, ry = ryDeg * qsig::constants::pi / 180.0, rz = rzDeg * qsig::constants::pi / 180.0;
        const Vec3 x{v.x, v.y * std::cos(rx) - v.z * std::sin(rx), v.y * std::sin(rx) + v.z * std::cos(rx)};
        const Vec3 y{x.x * std::cos(ry) + x.z * std::sin(ry), x.y, -x.x * std::sin(ry) + x.z * std::cos(ry)};
        return Vec3{y.x * std::cos(rz) - y.y * std::sin(rz), y.x * std::sin(rz) + y.y * std::cos(rz), y.z};
    };
    for (const Vec3 &normal : {turn({0.0, 0.0, 1.0}, 30.0, 20.0, 50.0), turn({0.0, 1.0, 0.0}, -40.0, 0.0, 10.0), Vec3{0.6, -0.8, 0.0}, Vec3{0.0, 0.0, -1.0}})
    {
        OrbitCamera c = camera(35.0, 20.0);
        c.faceTowards(normal);
        const auto f = c.frame();
        CHECK_NEAR(f.forward.x, -normal.x, 0.0, 1e-12);
        CHECK_NEAR(f.forward.y, -normal.y, 0.0, 1e-12);
        CHECK_NEAR(f.forward.z, -normal.z, 0.0, 1e-12);
        // A point of the plane through the centre stays at the depth of the centre.
        const Vec3 inPlane = qsig::cross(normal, Vec3{0.3, 0.7, 0.2});
        CHECK_NEAR(f.project(c.centre + inPlane).depth, 0.0, 0.0, 1e-12);
    }
    // A turned plane is not a plane of the 2D views; facing one of those is an exact view.
    OrbitCamera c = camera(35.0, 20.0);
    c.faceTowards(turn({0.0, 0.0, 1.0}, 30.0, 0.0, 0.0));
    CHECK(c.facingPlane() == -1);
    c.faceTowards({1.0, 0.0, 0.0});
    CHECK(c.facingPlane() == 2);
    // Straight above: the yaw is kept, the pitch is 90 degrees.
    c.yaw = 12.0;
    c.faceTowards({0.0, 0.0, 2.0});
    CHECK(c.yaw == 12.0);
    CHECK_NEAR(c.pitch, 90.0, 0.0, 1e-12);
    CHECK(c.facingPlane() == 0);
}

TEST_CASE("orbit camera: picking a plane, zoom about the cursor, orbit limits", "[view3d]")
{
    OrbitCamera c = camera(35.0, 20.0);
    auto f = c.frame();
    // A point of the plane z = 0.4 comes back from its own pixel.
    const Vec3 onPlane{1.1, -0.6, 0.4};
    const auto s = f.project(onPlane);
    Vec3 hit;
    REQUIRE(f.pick(s.x, s.y, {0.0, 0.0, 0.4}, {0.0, 0.0, 1.0}, hit));
    CHECK_NEAR(qsig::norm(hit - onPlane), 0.0, 0.0, 1e-12);
    // The normal may have any length and sign; a plane seen edge-on is refused.
    REQUIRE(f.pick(s.x, s.y, {5.0, 5.0, 0.4}, {0.0, 0.0, -3.0}, hit));
    CHECK_NEAR(qsig::norm(hit - onPlane), 0.0, 0.0, 1e-12);
    c.lookFrom(1, -1);
    CHECK(!c.frame().pick(100.0, 100.0, {}, {0.0, 0.0, 1.0}, hit));
    CHECK(c.frame().pick(100.0, 100.0, {}, {0.0, 1.0, 0.0}, hit));
    CHECK_NEAR(hit.y, 0.0, 0.0, 1e-12);

    // Zoom: the world point under the cursor stays under it.
    c = camera(35.0, 20.0);
    const Vec3 under = c.frame().unproject(610.0, 140.0);
    c.zoomAt(610.0, 140.0, 1.7);
    const auto after = c.frame().project(under);
    CHECK_NEAR(after.x, 610.0, 0.0, 1e-9);
    CHECK_NEAR(after.y, 140.0, 0.0, 1e-9);
    CHECK_NEAR(c.zoom, 1.7, 0.0, 1e-12);
    c.zoomAt(0.0, 0.0, 1e9);
    CHECK(c.zoom == 80.0);
    // Pan moves the picture by as many pixels.
    const auto before = c.frame().project(under);
    c.pan(25.0, -40.0);
    CHECK_NEAR(c.frame().project(under).x - before.x, 25.0, 0.0, 1e-9);
    CHECK_NEAR(c.frame().project(under).y - before.y, -40.0, 0.0, 1e-9);
    // Dragging down tilts the scene towards the viewer, up to straight above; reset comes back.
    c.orbit(0.0, 1e4);
    CHECK(c.pitch == 90.0);
    c.orbit(0.0, -1e5);
    CHECK(c.pitch == -90.0);
    c.reset();
    CHECK(c.yaw == 35.0);
    CHECK(c.pitch == 20.0);
    CHECK(c.panX == 0.0);
}

TEST_CASE("source shapes: electrostatic sources in space", "[view3d]")
{
    using Type = Electrostatics::Source::Type;
    Electrostatics::Source s;
    s.position = {0.5, -0.25, 0.75};
    s.strength = -2e-9;
    const Vec3 view{0.0, 0.0, 0.0};

    s.type = Type::PointCharge;
    auto shape = qsig::app::sourceShape(s, view, 2.0);
    CHECK(shape.point);
    CHECK(shape.sign == -1);
    CHECK(qsig::app::sourceReach(s) == 0.0);

    // A ball: three great circles on the sphere.
    s.type = Type::SolidSphere;
    s.radius = 0.3;
    shape = qsig::app::sourceShape(s, view, 2.0);
    CHECK(shape.sphereRadius == 0.3);
    REQUIRE(shape.lines.size() == 3);
    for (const auto &line : shape.lines)
        for (const auto &p : line) CHECK_NEAR(qsig::norm(p - s.position), 0.3, 0.0, 1e-12);
    CHECK(qsig::app::sourceReach(s) == 0.3);

    // A box: two faces, twelve edges (two outlines and four uprights), turned about z.
    s.type = Type::RectangularVolume;
    s.width = 0.8;
    s.height = 0.4;
    s.thickness = 0.2;
    s.angleDeg = 90.0;
    shape = qsig::app::sourceShape(s, view, 2.0);
    REQUIRE(shape.faces.size() == 2);
    REQUIRE(shape.lines.size() == 6);
    double edges = 0.0;
    for (const auto &line : shape.lines) edges += pathLength(line);
    CHECK_NEAR(edges, 4.0 * (0.8 + 0.4 + 0.2), 0.0, 1e-12);
    // Turned by 90 degrees: the width now runs along y.
    CHECK_NEAR(std::abs(shape.faces[0][1].y - shape.faces[0][0].y), 0.8, 0.0, 1e-12);
    CHECK_NEAR(shape.faces[0][0].z, 0.75 - 0.1, 0.0, 1e-12);
    CHECK_NEAR(shape.faces[1][0].z, 0.75 + 0.1, 0.0, 1e-12);
    CHECK_NEAR(qsig::app::sourceReach(s), 0.5 * std::sqrt(0.64 + 0.16 + 0.04), 0.0, 1e-12);

    // Infinite line: drawn across the view, not framed.
    s.type = Type::InfiniteLine;
    shape = qsig::app::sourceShape(s, {0.0, 0.0, 1.0}, 2.0);
    CHECK(shape.infinite);
    REQUIRE(shape.lines.size() == 1);
    CHECK_NEAR(shape.lines[0].front().z, -1.0, 0.0, 1e-12);
    CHECK_NEAR(shape.lines[0].back().z, 3.0, 0.0, 1e-12);
    CHECK(shape.lines[0].front().x == 0.5);

    // Every type gives something to draw.
    for (int t = 0; t <= static_cast<int>(Type::RectangularVolume); ++t)
    {
        s.type = static_cast<Type>(t);
        shape = qsig::app::sourceShape(s, view, 2.0);
        CHECK((shape.point || !shape.lines.empty()));
        CHECK(std::isfinite(qsig::app::sourceReach(s)));
    }
}

TEST_CASE("source shapes: magnetostatic sources and the direction of their currents", "[view3d]")
{
    using Type = Magnetostatics::Source::Type;
    Magnetostatics::Source s;
    s.position = {0.1, 0.2, 0.3};
    s.strength = 2.0;
    s.radius = 0.5;
    s.azimuthDeg = 0.0;
    s.elevationDeg = 90.0; // axis along +z
    const Vec3 view{0.0, 0.0, 0.0};

    // Loop: on the circle, the arrows turn counterclockwise about +z for a positive current
    // (right-hand rule: B along +z at the centre).
    s.type = Type::CircularLoop;
    auto shape = qsig::app::sourceShape(s, view, 2.0);
    REQUIRE(shape.lines.size() == 1);
    for (const auto &p : shape.lines[0])
    {
        CHECK_NEAR(qsig::norm(p - s.position), 0.5, 0.0, 1e-12);
        CHECK_NEAR(p.z, 0.3, 0.0, 1e-12);
    }
    REQUIRE(shape.arrows.size() == 4);
    for (const auto &a : shape.arrows)
    {
        CHECK_NEAR(qsig::norm(a.direction), 1.0, 0.0, 1e-12);
        CHECK(qsig::cross(a.at - s.position, a.direction).z > 0.0);
    }
    const double bz = Magnetostatics::fieldAt({s}, s.position).magneticFluxDensity.z;
    CHECK(bz > 0.0);
    // A negative current turns the other way.
    s.strength = -2.0;
    shape = qsig::app::sourceShape(s, view, 2.0);
    for (const auto &a : shape.arrows) CHECK(qsig::cross(a.at - s.position, a.direction).z < 0.0);
    s.strength = 2.0;

    // Solenoid: a helix on the cylinder, between its two ends.
    s.type = Type::Solenoid;
    s.length = 1.2;
    s.turns = 200;
    shape = qsig::app::sourceShape(s, view, 2.0);
    REQUIRE(shape.lines.size() == 3);
    const auto &helix = shape.lines[0];
    CHECK_NEAR(helix.front().z, 0.3 - 0.6, 0.0, 1e-12);
    CHECK_NEAR(helix.back().z, 0.3 + 0.6, 0.0, 1e-12);
    for (const auto &p : helix) CHECK_NEAR(std::hypot(p.x - 0.1, p.y - 0.2), 0.5, 0.0, 1e-12);
    CHECK_NEAR(qsig::app::sourceReach(s), std::hypot(0.5, 0.6), 0.0, 1e-12);

    // Wire on z: across the view, the arrows along the current.
    s.type = Type::InfiniteWireZ;
    shape = qsig::app::sourceShape(s, {0.0, 0.0, 0.5}, 1.5);
    CHECK(shape.infinite);
    CHECK_NEAR(shape.lines[0].front().z, -1.0, 0.0, 1e-12);
    REQUIRE(!shape.arrows.empty());
    CHECK(shape.arrows[0].direction.z == 1.0);

    // Dipole: a point with its moment.
    s.type = Type::MagneticDipole;
    shape = qsig::app::sourceShape(s, view, 2.0);
    CHECK(shape.point);
    REQUIRE(shape.arrows.size() == 1);
    CHECK(shape.arrows[0].large);
    CHECK_NEAR(shape.arrows[0].direction.z, 1.0, 0.0, 1e-12);

    for (int t = 0; t <= static_cast<int>(Type::InfiniteCurrentSheet); ++t)
    {
        s.type = static_cast<Type>(t);
        shape = qsig::app::sourceShape(s, view, 2.0);
        CHECK((shape.point || !shape.lines.empty()));
        CHECK(!shape.arrows.empty()); // every magnetic source shows the direction of its current or moment
        CHECK(std::isfinite(qsig::app::sourceReach(s)));
    }
}
