// Application layer (qsig_app): units, scene editing and undo, persistence, background field map.
#include "qsig_test.h"

#include "qsig/app/antenna_scene.h"
#include "qsig/app/electrostatic_scene.h"
#include "qsig/app/field_lines.h"
#include "qsig/app/magnetostatic_scene.h"
#include "qsig/app/field_map.h"
#include "qsig/app/units.h"
#include "qsig/app/wave_scene.h"
#include "qsig/constants.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTemporaryDir>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <complex>
#include <memory>
#include <vector>

using qsig::app::AntennaScene;
using qsig::app::ElectrostaticScene;
using qsig::app::MagnetostaticScene;
using qsig::app::Units;
using qsig::app::WaveScene;

namespace
{
double parsed(const QString &text, const QString &unit, const QString &defaultUnit, bool *ok = nullptr)
{
    const QVariantMap r = Units::parseQuantity(text, unit, defaultUnit);
    if (ok) *ok = r.value(QStringLiteral("ok")).toBool();
    return r.value(QStringLiteral("value")).toDouble();
}
} // namespace

TEST_CASE("units: parse accepts prefixes, French decimal comma and bare numbers", "[app]")
{
    bool ok = false;
    CHECK_REL(parsed(QStringLiteral("1,5 nC"), QStringLiteral("C"), QStringLiteral("C"), &ok), 1.5e-9, 1e-12);
    CHECK(ok);
    CHECK_REL(parsed(QStringLiteral("2"), QStringLiteral("C"), QStringLiteral("nC"), &ok), 2e-9, 1e-12);
    CHECK(ok);
    CHECK_REL(parsed(QStringLiteral("10 cm"), QStringLiteral("m"), QStringLiteral("m"), &ok), 0.1, 1e-12);
    CHECK(ok);
    CHECK_REL(parsed(QStringLiteral("−2 µC"), QStringLiteral("C"), QStringLiteral("C"), &ok), -2e-6, 1e-12);
    CHECK(ok);
    CHECK_REL(parsed(QStringLiteral("3"), QStringLiteral("C/m²"), QStringLiteral("nC/m²"), &ok), 3e-9, 1e-12);
    CHECK(ok);
    CHECK_REL(parsed(QStringLiteral("2+3"), QStringLiteral("m"), QStringLiteral("cm"), &ok), 0.05, 1e-12);
    CHECK(ok);
}

TEST_CASE("units: angles are degrees, with or without a unit", "[app]")
{
    bool ok = false;
    CHECK_REL(parsed(QStringLiteral("45"), QStringLiteral("°"), QStringLiteral("°"), &ok), 45.0, 1e-12);
    CHECK(ok);
    CHECK_REL(parsed(QStringLiteral("30°"), QStringLiteral("°"), QStringLiteral("°"), &ok), 30.0, 1e-12);
    CHECK(ok);
    CHECK_REL(parsed(QStringLiteral("1 rad"), QStringLiteral("°"), QStringLiteral("°"), &ok),
              180.0 / qsig::constants::pi, 1e-12);
    CHECK(ok);
}

TEST_CASE("units: wrong dimension and garbage are refused with a message", "[app]")
{
    QVariantMap r = Units::parseQuantity(QStringLiteral("3 V"), QStringLiteral("m"), QStringLiteral("m"));
    CHECK(!r.value(QStringLiteral("ok")).toBool());
    CHECK(!r.value(QStringLiteral("error")).toString().isEmpty());
    r = Units::parseQuantity(QStringLiteral("abc"), QStringLiteral("m"), QStringLiteral("m"));
    CHECK(!r.value(QStringLiteral("ok")).toBool());
    r = Units::parseQuantity(QStringLiteral("  "), QStringLiteral("m"), QStringLiteral("m"));
    CHECK(!r.value(QStringLiteral("ok")).toBool());
}

TEST_CASE("units: format picks an engineering prefix and round-trips through parse", "[app]")
{
    CHECK(Units::formatQuantity(1.5e-9, QStringLiteral("C")) == QStringLiteral("1,5 nC"));
    CHECK(Units::formatQuantity(-0.25, QStringLiteral("m")) == QStringLiteral("−250 mm"));
    CHECK(Units::formatQuantity(0.0, QStringLiteral("V")) == QStringLiteral("0 V"));
    CHECK(Units::formatQuantity(30.0, QStringLiteral("°")) == QStringLiteral("30°"));
    CHECK(Units::preferredUnitFor(2e-9, QStringLiteral("C")) == QStringLiteral("nC"));
    for (double v : {1.2345e-12, 3.3e-6, 0.047, 12.5, 4.7e3, -8.2e7})
    {
        const QString text = Units::formatQuantity(v, QStringLiteral("V/m"), 6);
        bool ok = false;
        CHECK_REL(parsed(text, QStringLiteral("V/m"), QStringLiteral("V/m"), &ok), v, 1e-9);
        CHECK(ok);
    }
}

TEST_CASE("scene: each edit is one undo step and undo restores the previous value", "[app]")
{
    ElectrostaticScene scene;
    const int row = scene.addSource(0);
    REQUIRE(row == 0);
    REQUIRE(scene.count() == 1);
    CHECK(scene.isModified());

    scene.setSourceValue(row, QStringLiteral("strength"), 4e-9);
    CHECK_REL(scene.sources()[0].strength, 4e-9, 1e-15);
    scene.setSourceValue(row, QStringLiteral("x"), 0.3);
    CHECK_REL(scene.sources()[0].position.x, 0.3, 1e-15);

    scene.undo();
    CHECK_NEAR(scene.sources()[0].position.x, 0.0, 1e-15, 1e-15);
    CHECK_REL(scene.sources()[0].strength, 4e-9, 1e-15);
    scene.undo();
    CHECK_REL(scene.sources()[0].strength, 1e-9, 1e-15);
    scene.undo();
    CHECK(scene.count() == 0);
    CHECK(!scene.canUndo());
    scene.redo();
    scene.redo();
    CHECK(scene.count() == 1);
    CHECK_REL(scene.sources()[0].strength, 4e-9, 1e-15);
}

TEST_CASE("scene: a drag is a single undo step, and two drags are two", "[app]")
{
    ElectrostaticScene scene;
    const int row = scene.addSource(0);
    for (int i = 1; i <= 20; ++i) scene.moveSource(row, 0.01 * i, 0.0, 0.0);
    scene.endDrag();
    for (int i = 1; i <= 10; ++i) scene.moveSource(row, 0.2, 0.01 * i, 0.0);
    scene.endDrag();
    CHECK_REL(scene.sources()[0].position.y, 0.1, 1e-12);

    scene.undo(); // second drag
    CHECK_REL(scene.sources()[0].position.x, 0.2, 1e-12);
    CHECK_NEAR(scene.sources()[0].position.y, 0.0, 1e-15, 1e-15);
    scene.undo(); // first drag
    CHECK_NEAR(scene.sources()[0].position.x, 0.0, 1e-15, 1e-15);
    scene.undo(); // creation
    CHECK(scene.count() == 0);
    CHECK(scene.canRedo());
}

TEST_CASE("scene: removing and duplicating keep rows consistent", "[app]")
{
    ElectrostaticScene scene;
    scene.addSource(0);
    scene.addSource(1);
    const int copy = scene.duplicateSource(1);
    REQUIRE(copy == 2);
    CHECK(scene.count() == 3);
    CHECK(scene.sources()[2].type == scene.sources()[1].type);
    CHECK(scene.sources()[2].name != scene.sources()[1].name);
    scene.removeSource(0);
    CHECK(scene.count() == 2);
    scene.removeSource(42); // out of range: ignored
    CHECK(scene.count() == 2);
}

TEST_CASE("scene: JSON round-trip preserves every source exactly", "[app]")
{
    ElectrostaticScene a;
    a.loadExample(QStringLiteral("sphere_and_plate"));
    REQUIRE(a.count() > 0);
    a.setSourceValue(0, QStringLiteral("name"), QStringLiteral("Électrode α"));
    const QByteArray json = a.toJson();

    ElectrostaticScene b;
    QString error;
    REQUIRE(b.fromJson(json, &error));
    REQUIRE(b.count() == a.count());
    for (int i = 0; i < a.count(); ++i)
    {
        const auto &s = a.sources()[static_cast<std::size_t>(i)];
        const auto &t = b.sources()[static_cast<std::size_t>(i)];
        CHECK(s.name == t.name);
        CHECK(s.type == t.type);
        CHECK(s.position.x == t.position.x && s.position.y == t.position.y && s.position.z == t.position.z);
        CHECK(s.strength == t.strength);
        CHECK(s.radius == t.radius && s.innerRadius == t.innerRadius);
        CHECK(s.width == t.width && s.height == t.height && s.length == t.length);
    }
    CHECK(!b.fromJson(QByteArrayLiteral("{\"format\":\"autre chose\"}"), &error));
    CHECK(!error.isEmpty());
    CHECK(!b.fromJson(QByteArrayLiteral("pas du json"), &error));
}

TEST_CASE("scene: save then load from disk, clean state after loading", "[app]")
{
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QUrl url = QUrl::fromLocalFile(dir.filePath(QStringLiteral("scène.qsig-es.json")));
    ElectrostaticScene a;
    a.loadExample(QStringLiteral("quadrupole"));
    REQUIRE(a.save(url));
    CHECK(!a.isModified());
    CHECK(a.fileUrl() == url);

    ElectrostaticScene b;
    REQUIRE(b.load(url));
    CHECK(b.count() == a.count());
    CHECK(!b.isModified());
    CHECK(!b.canUndo());
}

TEST_CASE("scene: probe of a point charge is Coulomb's law", "[app]")
{
    ElectrostaticScene scene;
    const int row = scene.addSource(0);
    scene.setSourceValue(row, QStringLiteral("strength"), 2e-9);
    const QVariantMap p = scene.probe(0.3, 0.4, 0.0);
    const double k = 1.0 / (4.0 * qsig::constants::pi * qsig::constants::epsilon0);
    CHECK_REL(p.value(QStringLiteral("magnitude")).toDouble(), k * 2e-9 / 0.25, 1e-12);
    CHECK_REL(p.value(QStringLiteral("potential")).toDouble(), k * 2e-9 / 0.5, 1e-12);
    CHECK_REL(p.value(QStringLiteral("ex")).toDouble(), k * 2e-9 / 0.25 * 0.6, 1e-12);
    CHECK(p.value(QStringLiteral("potentialDefined")).toBool());
}

TEST_CASE("field map: background computation returns a coherent map", "[app]")
{
    ElectrostaticScene scene;
    scene.loadExample(QStringLiteral("dipole"));

    qsig::app::FieldMapRequest request;
    request.sampler = scene.sampler(1, qsig::app::ViewPlane::XY);
    request.plane = qsig::app::ViewPlane::XY;
    request.quantity = qsig::app::FieldQuantity::Scalar;
    request.u0 = -1.0;
    request.v0 = -1.0;
    request.du = 2.0 / 119.0;
    request.columns = 120;
    request.rows = 80;
    request.contourLevels = 12;

    qsig::app::FieldMapComputer computer;
    std::shared_ptr<const qsig::app::FieldMapResult> result;
    QEventLoop loop;
    QObject::connect(&computer, &qsig::app::FieldMapComputer::ready, &loop,
                     [&](std::shared_ptr<const qsig::app::FieldMapResult> r)
                     {
                         result = std::move(r);
                         loop.quit();
                     });
    QTimer::singleShot(20000, &loop, &QEventLoop::quit);
    computer.request(request);
    loop.exec();

    REQUIRE(result != nullptr);
    CHECK(result->image.width() == 120);
    CHECK(result->image.height() == 80);
    CHECK(result->validSamples > 120 * 80 - 10);
    CHECK(result->displayMin < result->displayMax);
    CHECK(std::abs(result->displayMin + result->displayMax) < 1e-9 * result->displayMax); // symmetric for V
    CHECK(!result->contours.empty());
    CHECK(!result->arrows.empty());
}

TEST_CASE("field map: a newer request supersedes the running one", "[app]")
{
    ElectrostaticScene scene;
    scene.loadExample(QStringLiteral("quadrupole"));
    qsig::app::FieldMapRequest request;
    request.sampler = scene.sampler(0, qsig::app::ViewPlane::XY);
    request.u0 = -1.0;
    request.v0 = -1.0;
    request.du = 2.0 / 399.0;
    request.columns = 400;
    request.rows = 300;

    qsig::app::FieldMapComputer computer;
    int delivered = 0;
    int lastColumns = 0;
    QEventLoop loop;
    QObject::connect(&computer, &qsig::app::FieldMapComputer::ready, &loop,
                     [&](std::shared_ptr<const qsig::app::FieldMapResult> r)
                     {
                         ++delivered;
                         lastColumns = r->request.columns;
                         if (lastColumns == 64) loop.quit();
                     });
    QTimer::singleShot(20000, &loop, &QEventLoop::quit);
    computer.request(request);
    request.columns = 64;
    request.rows = 48;
    request.du = 2.0 / 63.0;
    computer.request(request);
    loop.exec();
    CHECK(lastColumns == 64);
    CHECK(delivered >= 1);
    CHECK(delivered <= 2);
}

TEST_CASE("scene: an infinite source gives a relative potential instead of none", "[app]")
{
    ElectrostaticScene scene;
    scene.loadExample(QStringLiteral("dipole"));
    const int row = scene.addSource(8); // infinite line
    REQUIRE(scene.sources()[static_cast<std::size_t>(row)].type == Electrostatics::Source::Type::InfiniteLine);
    const QVariantMap p = scene.probe(0.1, 0.5, 0.0);
    CHECK(p.value(QStringLiteral("potentialDefined")).toBool());
    CHECK(p.value(QStringLiteral("potentialReferenced")).toBool());
    CHECK(std::isfinite(p.value(QStringLiteral("potential")).toDouble()));
    CHECK(!p.value(QStringLiteral("note")).toString().isEmpty());

    qsig::app::FieldMapRequest request;
    request.sampler = scene.sampler(1, qsig::app::ViewPlane::XY);
    request.scalarReferenced = scene.scalarReferenced();
    request.quantity = qsig::app::FieldQuantity::Scalar;
    request.u0 = -1.0;
    request.v0 = 1.0;
    request.du = 2.0 / 79.0;
    request.columns = 80;
    request.rows = 80;
    qsig::app::FieldMapComputer computer;
    std::shared_ptr<const qsig::app::FieldMapResult> result;
    QEventLoop loop;
    QObject::connect(&computer, &qsig::app::FieldMapComputer::ready, &loop,
                     [&](std::shared_ptr<const qsig::app::FieldMapResult> r)
                     {
                         result = std::move(r);
                         loop.quit();
                     });
    QTimer::singleShot(20000, &loop, &QEventLoop::quit);
    computer.request(request);
    loop.exec();
    REQUIRE(result != nullptr);
    CHECK(result->scalarReferenced);
    CHECK(result->validSamples > 80 * 80 - 90); // only the samples on the line itself are undefined
    CHECK(!result->contours.empty());
}

TEST_CASE("scene: the charge can be given as a density of the shape", "[app]")
{
    using T = Electrostatics::Source::Type;
    const double pi = qsig::constants::pi;
    ElectrostaticScene scene;
    const int sphere = scene.addSource(static_cast<int>(T::SolidSphere));
    scene.setSourceValue(sphere, QStringLiteral("radius"), 0.2);
    scene.setSourceValue(sphere, QStringLiteral("density"), 3e-6); // C/m^3
    const double volume = 4.0 / 3.0 * pi * 0.008;
    CHECK_REL(scene.sources()[0].strength, 3e-6 * volume, 1e-12);
    QVariantMap d = scene.sourceData(sphere);
    CHECK(d.value(QStringLiteral("hasDensity")).toBool());
    CHECK_REL(d.value(QStringLiteral("density")).toDouble(), 3e-6, 1e-12);
    CHECK_REL(d.value(QStringLiteral("measure")).toDouble(), volume, 1e-12);
    CHECK(d.value(QStringLiteral("densityUnit")).toString() == QStringLiteral("C/m³"));

    // By default Q is kept: doubling the radius divides the density by 8.
    scene.setSourceValue(sphere, QStringLiteral("radius"), 0.4);
    CHECK_REL(scene.sources()[0].strength, 3e-6 * volume, 1e-12);
    // Keeping the density instead: the charge follows the volume.
    scene.setSourceValue(sphere, QStringLiteral("radius"), 0.2);
    scene.setSourceValue(sphere, QStringLiteral("keepDensity"), true);
    scene.setSourceValue(sphere, QStringLiteral("radius"), 0.4);
    CHECK_REL(scene.sources()[0].strength, 8.0 * 3e-6 * volume, 1e-12);
    scene.undo();
    CHECK_REL(scene.sources()[0].strength, 3e-6 * volume, 1e-12);
    scene.undo();
    CHECK(!scene.sourceData(sphere).value(QStringLiteral("keepDensity")).toBool());

    // Other shapes: surface, length, hollow shapes and the section of an infinite cylinder.
    struct Case
    {
        T type;
        double expectedMeasure;
    };
    const Case cases[] = {{T::SphericalShell, 4.0 * pi * 0.25 * 0.25},
                          {T::RectangularPlate, 1.0 * 0.6},
                          {T::TriangularPlate, 0.5 * 1.0 * 0.6},
                          {T::CircularPlate, pi * 0.25 * 0.25},
                          {T::AnnularPlate, pi * (0.25 * 0.25 - 0.12 * 0.12)},
                          {T::FiniteLine, 1.0},
                          {T::RectangularVolume, 1.0 * 0.6 * 0.1},
                          {T::ThickSphericalShell, 4.0 / 3.0 * pi * (0.25 * 0.25 * 0.25 - 0.12 * 0.12 * 0.12)},
                          {T::InfiniteCylinderZ, pi * 0.25 * 0.25}};
    for (const Case &c : cases)
    {
        const int row = scene.addSource(static_cast<int>(c.type));
        CHECK_REL(scene.sourceData(row).value(QStringLiteral("measure")).toDouble(), c.expectedMeasure, 1e-12);
        scene.setSourceValue(row, QStringLiteral("density"), 2e-9);
        CHECK_REL(scene.sources()[static_cast<std::size_t>(row)].strength, 2e-9 * c.expectedMeasure, 1e-12);
    }
    for (T type : {T::PointCharge, T::InfiniteLine, T::InfinitePlane})
    {
        const int row = scene.addSource(static_cast<int>(type));
        CHECK(!scene.sourceData(row).value(QStringLiteral("hasDensity")).toBool());
    }
}

TEST_CASE("scene: the density choice is saved and restored", "[app]")
{
    ElectrostaticScene a;
    const int row = a.addSource(static_cast<int>(Electrostatics::Source::Type::SolidSphere));
    a.setSourceValue(row, QStringLiteral("keepDensity"), true);
    ElectrostaticScene b;
    REQUIRE(b.fromJson(a.toJson()));
    CHECK(b.sourceData(0).value(QStringLiteral("keepDensity")).toBool());
}

TEST_CASE("units: areas and volumes never take a prefix on the metre", "[app]")
{
    CHECK(Units::formatQuantity(0.0654, QStringLiteral("m³")) == QStringLiteral("0,0654 m³"));
    CHECK(Units::formatQuantity(5e-6, QStringLiteral("m³")) == QStringLiteral("5 cm³"));
    CHECK(Units::formatQuantity(2e-9, QStringLiteral("m³")) == QStringLiteral("2 mm³"));
    CHECK(Units::formatQuantity(0.6, QStringLiteral("m²")) == QStringLiteral("0,6 m²"));
    CHECK(Units::formatQuantity(2e-4, QStringLiteral("m²")) == QStringLiteral("2 cm²"));
    CHECK(Units::formatQuantity(3e-9, QStringLiteral("C/m³")) == QStringLiteral("3 nC/m³"));
    bool ok = false;
    CHECK_REL(parsed(QStringLiteral("3 nC/m³"), QStringLiteral("C/m³"), QStringLiteral("C/m³"), &ok), 3e-9, 1e-12);
    CHECK(ok);
    CHECK_REL(parsed(QStringLiteral("4"), QStringLiteral("C/m³"), QStringLiteral("µC/m³"), &ok), 4e-6, 1e-12);
    CHECK(ok);
}

namespace
{
std::shared_ptr<const qsig::app::FieldMapResult> computeMap(qsig::app::FieldMapRequest request)
{
    qsig::app::FieldMapComputer computer;
    std::shared_ptr<const qsig::app::FieldMapResult> result;
    QEventLoop loop;
    QObject::connect(&computer, &qsig::app::FieldMapComputer::ready, &loop,
                     [&](std::shared_ptr<const qsig::app::FieldMapResult> r)
                     {
                         result = std::move(r);
                         loop.quit();
                     });
    QTimer::singleShot(20000, &loop, &QEventLoop::quit);
    computer.request(std::move(request));
    loop.exec();
    return result;
}
} // namespace

TEST_CASE("field lines: evenly spaced circles around a wire, open lines for a dipole", "[app]")
{
    const int w = 120, h = 100;
    std::vector<float> fc(w * h), fr(w * h);
    std::vector<unsigned char> valid(w * h, 1);
    for (int r = 0; r < h; ++r)
        for (int c = 0; c < w; ++c)
        {
            const double x = c - 60.0, y = 50.0 - r, d = x * x + y * y + 1e-9; // wire at the centre
            fc[static_cast<std::size_t>(r * w + c)] = static_cast<float>(-y / d);
            fr[static_cast<std::size_t>(r * w + c)] = static_cast<float>(-x / d); // rows go down
        }
    qsig::app::FieldLineSettings settings;
    settings.separation = 8.0;
    const auto lines = qsig::app::traceFieldLines(w, h, fc, fr, valid, settings);
    REQUIRE(lines.size() >= 5);
    int closed = 0;
    for (const auto &line : lines)
    {
        if (!line.closed) continue;
        ++closed;
        // A closed line around the wire stays at a constant distance from it.
        const auto &p0 = line.points.front();
        const double r0 = std::hypot(p0.c - 60.0, p0.r - 50.0);
        for (const auto &p : line.points) CHECK_NEAR(std::hypot(p.c - 60.0, p.r - 50.0), r0, 0.0, 0.05 * r0 + 0.3);
        CHECK(p0.c == line.points.back().c);
    }
    CHECK(closed >= 3);
}

TEST_CASE("magnetostatics scene: probe, examples, outlines and files", "[app]")
{
    MagnetostaticScene scene;
    const int wire = scene.addSource(0); // infinite wire along z, at the origin
    scene.setSourceValue(wire, QStringLiteral("strength"), 2.0);
    const QVariantMap p = scene.probe(0.3, 0.4, 0.0);
    const double expected = qsig::constants::mu0 * 2.0 / (2.0 * qsig::constants::pi * 0.5);
    CHECK_REL(p.value(QStringLiteral("magnitude")).toDouble(), expected, 1e-12);
    CHECK_REL(p.value(QStringLiteral("bx")).toDouble(), -expected * 0.8, 1e-12); // counterclockwise seen from +z
    CHECK(p.value(QStringLiteral("rows")).toList().size() == 5);
    CHECK(scene.quantityNames().size() == 2);

    // Every example loads, every source type has an outline on every plane.
    for (const QVariant &e : scene.examples())
    {
        scene.loadExample(e.toMap().value(QStringLiteral("key")).toString());
        CHECK(scene.count() > 0);
    }
    scene.newScene();
    for (int t = 0; t < scene.typeNames().size(); ++t)
    {
        const int row = scene.addSource(t);
        for (int plane = 0; plane < 3; ++plane) (void)scene.outline(row, static_cast<qsig::app::ViewPlane>(plane), 0.0);
    }
    // A loop cut through its axis (plane XZ) crosses the plane twice: one current out, one in.
    scene.newScene();
    const int loop = scene.addSource(2);
    scene.setSourceValue(loop, QStringLiteral("x"), 0.0);
    const auto o = scene.outline(loop, qsig::app::ViewPlane::XZ, 0.0);
    REQUIRE(o.marks.size() == 2);
    CHECK(o.marks[0].out != o.marks[1].out);
    CHECK(o.gap == 0.0);
    CHECK(scene.outline(loop, qsig::app::ViewPlane::XZ, 1.0).gap > 0.69);

    // Turns are whole numbers, the elevation is clamped, files keep everything.
    const int sol = scene.addSource(3);
    scene.setSourceValue(sol, QStringLiteral("turns"), 12.6);
    scene.setSourceValue(sol, QStringLiteral("elevationDeg"), 120.0);
    CHECK(scene.sources()[static_cast<std::size_t>(sol)].turns == 13);
    CHECK(scene.sources()[static_cast<std::size_t>(sol)].elevationDeg == 90.0);
    MagnetostaticScene copy;
    REQUIRE(copy.fromJson(scene.toJson()));
    CHECK(copy.count() == scene.count());
    CHECK(copy.sources()[static_cast<std::size_t>(sol)].turns == 13);
    ElectrostaticScene wrongModule;
    CHECK(!wrongModule.fromJson(scene.toJson()));
}

TEST_CASE("magnetostatics scene: current density of a conductor and the normal component", "[app]")
{
    MagnetostaticScene scene;
    const int conductor = scene.addSource(5);                        // solid current cylinder, R = 0.1 m
    scene.setSourceValue(conductor, QStringLiteral("density"), 1e6); // A/m^2
    CHECK_REL(scene.sources()[0].strength, 1e6 * qsig::constants::pi * 0.01, 1e-12);
    CHECK(scene.sourceData(conductor).value(QStringLiteral("densityUnit")).toString() == QStringLiteral("A/m²"));

    // B-perp on the XZ plane is the component towards the viewer, i.e. along -y.
    const auto sampler = scene.sampler(1, qsig::app::ViewPlane::XZ);
    const auto sample = sampler({0.5, 0.0, 0.2});
    const auto probe = scene.probe(0.5, 0.0, 0.2);
    CHECK_REL(sample.scalar, -probe.value(QStringLiteral("by")).toDouble(), 1e-12);
}

TEST_CASE("field map: field lines around two wires, through the background computer", "[app]")
{
    MagnetostaticScene scene;
    scene.loadExample(QStringLiteral("two_wires_opposite"));
    qsig::app::FieldMapRequest request;
    request.sampler = scene.sampler(0, qsig::app::ViewPlane::XY);
    request.u0 = -1.0;
    request.v0 = 1.0;
    request.du = 2.0 / 159.0;
    request.columns = 160;
    request.rows = 160;
    request.lineSeparation = 9.0;
    const auto result = computeMap(request);
    REQUIRE(result != nullptr);
    CHECK(result->fieldLines.size() >= 10);
    const auto closed = std::count_if(result->fieldLines.begin(), result->fieldLines.end(),
                                      [](const qsig::app::FieldLine &l) { return l.closed; });
    CHECK(closed >= 4); // loops around each wire
}

TEST_CASE("units: pure numbers and the middle dot", "[app]")
{
    CHECK(Units::formatQuantity(100.0, QString()) == QStringLiteral("100"));
    CHECK(Units::preferredUnitFor(100.0, QString()).isEmpty());
    bool ok = false;
    CHECK_REL(parsed(QStringLiteral("2*12"), QString(), QString(), &ok), 24.0, 1e-15);
    CHECK(ok);
    CHECK(!Units::parseQuantity(QStringLiteral("3 m"), QString(), QString()).value(QStringLiteral("ok")).toBool());
    CHECK_REL(parsed(QStringLiteral("2 mA·m²"), QStringLiteral("A·m²"), QStringLiteral("A·m²"), &ok), 2e-3, 1e-12);
    CHECK(ok);
    CHECK(Units::formatQuantity(2e-3, QStringLiteral("A·m²")) == QStringLiteral("2 mA·m²"));
}

TEST_CASE("waves scene: objects, choices, undoable settings, files and examples", "[app]")
{
    WaveScene scene;
    CHECK(scene.isLive());
    CHECK(scene.isPlanar());
    CHECK(scene.defaultQuantity() == 1);
    // Every type: an outline (shapes, lines or brush strokes), labelled fields; sources have an
    // amplitude, obstacles none.
    for (int t = 0; t < scene.typeNames().size(); ++t)
    {
        const int row = scene.addSource(t);
        const auto o = scene.outline(row, qsig::app::ViewPlane::XY, 0.0);
        CHECK(!o.filled.isEmpty() || !o.lines.isEmpty() || !o.strokes.isEmpty() || t == 0);
        for (const QString &key : scene.editableFields(t)) CHECK(scene.fieldLabel(t, key) != key);
        CHECK(scene.strengthLabel(t).isEmpty() == (t >= 2));
    }
    CHECK(scene.count() == scene.typeNames().size());
    CHECK(scene.count() == 9);
    // Choices are indices, clamped; media are never faster than vacuum.
    CHECK(scene.fieldChoices(0, QStringLiteral("waveform")).size() == 3);
    CHECK(scene.fieldChoices(0, QStringLiteral("frequency")).isEmpty());
    scene.setSourceValue(0, QStringLiteral("waveform"), 1.6);
    CHECK(scene.sources()[0].waveform == 2);
    scene.setSourceValue(0, QStringLiteral("injection"), 5);
    CHECK(scene.sources()[0].injection == 1);
    scene.setSourceValue(2, QStringLiteral("epsilonR"), 0.5);
    CHECK(scene.sources()[2].epsilonR == 1.0);
    scene.setSourceValue(2, QStringLiteral("width"), -1.0); // refused
    CHECK(scene.sources()[2].width > 0.0);
    // The slit wall is drawn as the pieces between its slits.
    CHECK(scene.outline(6, qsig::app::ViewPlane::XY, 0.0).filled.size() == 3);

    // Settings are undoable; the polarisation changes the field and the units.
    scene.setSetting(QStringLiteral("polarization"), 1);
    CHECK(scene.settings().polarization == 1);
    CHECK(scene.strengthUnit(0) == QStringLiteral("A/m"));
    CHECK(scene.quantityNames().value(1) == QStringLiteral("Hz"));
    scene.setSetting(QStringLiteral("width"), 2.5);
    scene.undo();
    CHECK(scene.settings().width == 3.0);
    scene.undo();
    CHECK(scene.settings().polarization == 0);
    CHECK(scene.strengthUnit(0) == QStringLiteral("V/m"));
    scene.redo();
    CHECK(scene.settings().polarization == 1);
    CHECK(scene.settingsData().value(QStringLiteral("polarization")).toInt() == 1);
    CHECK(scene.settingsNote().contains(QStringLiteral("Δx")));

    // Files keep the objects and the settings; another module refuses them.
    WaveScene copy;
    REQUIRE(copy.fromJson(scene.toJson()));
    CHECK(copy.count() == scene.count());
    CHECK(copy.settings() == scene.settings());
    CHECK(copy.sources()[0].waveform == 2);
    CHECK(copy.sources()[6].slits == scene.sources()[6].slits);
    MagnetostaticScene wrongModule;
    CHECK(!wrongModule.fromJson(scene.toJson()));
    // New scene: default settings again.
    scene.newScene();
    CHECK(scene.count() == 0);
    CHECK(scene.settings() == qsig::app::WaveSettings{});

    // Every example loads, within the cell budget.
    for (const QVariant &e : scene.examples())
    {
        scene.loadExample(e.toMap().value(QStringLiteral("key")).toString());
        CHECK(scene.count() > 0);
        CHECK(!scene.grid().limited);
        CHECK(!scene.canUndo());
    }
}

TEST_CASE("waves scene: the cavity example is driven at the resonance of its TM32 mode", "[app]")
{
    WaveScene scene;
    scene.loadExample(QStringLiteral("cavity"));
    REQUIRE(scene.count() == 1);
    qsig::app::WaveSimulation sim;
    sim.update(scene.settings(), scene.sources());
    sim.advance(3000);
    // Amplitude map against |sin(3 pi x'/a) sin(2 pi y'/b)|, walls on the outer nodes.
    const qsig::app::WaveGrid g = sim.grid();
    const double a = scene.settings().width + g.dx, b = scene.settings().height + g.dy;
    double cross = 0.0, mode = 0.0, field = 0.0;
    for (int j = g.pad; j < g.pad + g.areaNy; ++j)
        for (int i = g.pad; i < g.pad + g.areaNx; ++i)
        {
            const double x = g.x0 + i * g.dx, y = g.y0 + j * g.dy;
            const double m = std::abs(std::sin(3.0 * qsig::constants::pi * (x + 0.5 * a) / a) *
                                      std::sin(2.0 * qsig::constants::pi * (y + 0.5 * b) / b));
            const double v = sim.amplitudeAt(x, y);
            cross += m * v;
            mode += m * m;
            field += v * v;
        }
    REQUIRE(field > 0.0);
    CHECK(cross / std::sqrt(mode * field) > 0.99);
}

TEST_CASE("waves scene: the live simulation runs on its worker while active", "[app]")
{
    WaveScene scene;
    scene.loadExample(QStringLiteral("point_source"));
    scene.setProbePoints(QVector3D(0.3f, 0.0f, 0.0f), true, QVector3D(0.5f, 0.0f, 0.0f));
    scene.setSpeed(8);
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&scene, &qsig::app::SceneBase::frameReady, &loop,
                     [&]
                     {
                         if (scene.lastFrame() && scene.lastFrame()->steps >= 120) loop.quit();
                     });
    scene.setActive(true);
    timeout.start(20000);
    loop.exec();
    scene.setActive(false);
    REQUIRE(scene.lastFrame() != nullptr);
    CHECK(scene.lastFrame()->steps >= 120);
    REQUIRE(scene.liveFrame() != nullptr);
    CHECK(scene.liveFrame()->images[1].width() == 200);
    const QVariantMap reading = scene.probe(0.3, 0.0, 0.0);
    CHECK(reading.value(QStringLiteral("rows")).toList().size() == 4);
    const QVariantList traces = reading.value(QStringLiteral("traces")).toList();
    REQUIRE(traces.size() == 2); // A and B
    CHECK(!traces[0].toMap().value(QStringLiteral("values")).toList().isEmpty());
    CHECK(traces[0].toMap().value(QStringLiteral("values")).toList().size() ==
          traces[1].toMap().value(QStringLiteral("values")).toList().size());
    // Point B and the differences: amplitude ratio, phase and delay at the frequency of the source.
    const QVariantList groups = reading.value(QStringLiteral("groups")).toList();
    REQUIRE(groups.size() == 2);
    const QVariantList difference = groups[1].toMap().value(QStringLiteral("rows")).toList();
    REQUIRE(difference.size() == 4);
    CHECK(difference[1].toMap().value(QStringLiteral("unit")).toString() == QStringLiteral("dB"));
    CHECK(std::isfinite(difference[2].toMap().value(QStringLiteral("value")).toDouble())); // phase
    CHECK(!std::isnan(reading.value(QStringLiteral("rows")).toList().value(0).toMap().value("value").toDouble()));
    CHECK(scene.statusText().contains(QStringLiteral("pas")));
    // Paused and inactive: no more steps.
    const std::size_t steps = scene.lastFrame()->steps;
    scene.setRunning(false);
    QTimer::singleShot(150, &loop, &QEventLoop::quit);
    loop.exec();
    CHECK(scene.lastFrame()->steps == steps);
}

TEST_CASE("waves scene: drawing with the brush, the eraser and the shape tools", "[app]")
{
    WaveScene scene;
    CHECK(scene.canDraw());
    CHECK(scene.hasDifferentialProbe());
    auto points = [](std::initializer_list<QPointF> list)
    {
        QVariantList v;
        for (const QPointF &p : list) v << p;
        return v;
    };
    // A conductor stroke makes a new drawing; the next one with the same material joins it.
    const int drawing = scene.addStroke(-1, points({{-0.5, 0.0}, {0.5, 0.0}}), 0.04, false, 1, 4.0);
    REQUIRE(drawing == 0);
    CHECK(scene.sources()[0].type == qsig::app::WaveObject::Type::ConductorDrawing);
    CHECK(scene.addStroke(drawing, points({{0.0, 0.3}}), 0.1, false, 1, 4.0) == drawing);
    CHECK(scene.sources()[0].strokes.size() == 2);
    // Another material: another drawing, whatever the selection.
    const int glass = scene.addStroke(drawing, points({{0.0, -0.4}, {0.2, -0.4}}), 0.05, false, 0, 2.5);
    CHECK(glass == 1);
    CHECK(scene.sources()[1].epsilonR == 2.5);
    // Points closer than a quarter of the width are dropped, the last one is kept.
    const int dense = scene.addStroke(-1, points({{1.0, 0.0}, {1.001, 0.0}, {1.002, 0.0}, {1.2, 0.0}}), 0.04, false, 1, 1.0);
    CHECK(scene.sources()[static_cast<std::size_t>(dense)].strokes[0].points.size() == 4);
    // The eraser goes into every drawing it crosses, as one undo step.
    const int steps = scene.count();
    scene.addStroke(-1, points({{0.0, -0.5}, {0.0, 0.5}}), 0.1, true, 1, 1.0);
    CHECK(scene.count() == steps);
    CHECK(scene.sources()[0].strokes.back().erase);
    CHECK(scene.sources()[1].strokes.back().erase);
    CHECK(!scene.sources()[2].strokes.back().erase); // not crossed
    scene.undo();
    CHECK(!scene.sources()[0].strokes.back().erase);
    // Strokes are relative to the drawing: it moves in one piece.
    scene.moveSource(0, 0.3, 0.2, 0.0);
    scene.endDrag();
    const auto o = scene.outline(0, qsig::app::ViewPlane::XY, 0.0);
    REQUIRE(o.strokes.size() == 2);
    CHECK_REL(o.strokes[0].points[0].x(), -0.5 + 0.3 + 0.0, 1e-12);

    // Shapes dragged with the mouse.
    const int box = scene.addDrawnShape(QStringLiteral("rectangle"), 0.4, 0.1, 0.1, 0.5, 0, 3.0, 0.04);
    REQUIRE(box >= 0);
    const auto &block = scene.sources()[static_cast<std::size_t>(box)];
    CHECK(block.type == qsig::app::WaveObject::Type::DielectricBlock);
    CHECK_REL(block.width, 0.3, 1e-12);
    CHECK_REL(block.height, 0.4, 1e-12);
    CHECK_REL(block.position.x, 0.25, 1e-12);
    CHECK(block.epsilonR == 3.0);
    const int disc = scene.addDrawnShape(QStringLiteral("disk"), 0.0, 0.0, 0.3, 0.4, 1, 1.0, 0.04);
    CHECK_REL(scene.sources()[static_cast<std::size_t>(disc)].radius, 0.5, 1e-12);
    const int wall = scene.addDrawnShape(QStringLiteral("line"), 0.0, 0.0, 0.0, 1.0, 1, 1.0, 0.03);
    const auto &paroi = scene.sources()[static_cast<std::size_t>(wall)];
    CHECK(paroi.type == qsig::app::WaveObject::Type::SlitWall);
    CHECK(paroi.slits == 0);
    CHECK_REL(paroi.angleDeg, 90.0, 1e-12);
    CHECK_REL(paroi.thickness, 0.03, 1e-12);
    CHECK(scene.addDrawnShape(QStringLiteral("rectangle"), 0.1, 0.1, 0.1, 0.1, 1, 1.0, 0.04) == -1); // a click

    // Files keep the strokes.
    WaveScene copy;
    REQUIRE(copy.fromJson(scene.toJson()));
    REQUIRE(copy.count() == scene.count());
    CHECK(copy.sources()[0].strokes.size() == scene.sources()[0].strokes.size());
    CHECK(copy.sources()[0].strokes[1].points == scene.sources()[0].strokes[1].points);
    CHECK(copy.sources()[1].type == qsig::app::WaveObject::Type::DielectricDrawing);
}

TEST_CASE("antennas scene: elements, choices, undoable settings, files and examples", "[app]")
{
    AntennaScene scene;
    CHECK(scene.viewKind() == QStringLiteral("antenna"));
    CHECK(!scene.hasProbe());
    CHECK(!scene.isLive());
    // Every type: a wire outline seen from above, labelled fields with their unit.
    for (int t = 0; t < scene.typeNames().size(); ++t)
    {
        const int row = scene.addSource(t);
        CHECK(!scene.outline(row, qsig::app::ViewPlane::XY, 0.0).lines.isEmpty());
        for (const QString &key : scene.editableFields(t)) CHECK(scene.fieldLabel(t, key) != key);
        // The sources are entities of their own: an element shows its geometry, not a voltage. A ready-made
        // wire element comes with its source; a plain wire (type 0) and an arc (type 14) are passive;
        // plates, discs, solids and reflectors never carry one.
        const bool surface = qsig::app::elementIsSurface(scene.sources()[static_cast<std::size_t>(row)].type);
        CHECK(!scene.editableFields(t).contains(QStringLiteral("voltage")));
        CHECK(qsig::app::feedCount(scene.sources()[static_cast<std::size_t>(row)]) == (surface || t == 0 || t == 14 ? 0 : 1));
        CHECK(!scene.formula(t).isEmpty());
    }
    CHECK(scene.count() == 16); // six wire elements, two surfaces, four solids, two reflectors, two curved wires
    // A plain wire is a parasitic element, every other element is fed; sized for the frequency.
    CHECK(scene.sources()[0].fed == 0);
    CHECK(scene.sources()[1].fed == 1);
    CHECK_REL(scene.sources()[1].length, 0.475 * qsig::constants::c0 / 300e6, 1e-12);
    CHECK(scene.fieldChoices(0, QStringLiteral("fed")).size() == 2);
    CHECK(scene.fieldChoices(0, QStringLiteral("length")).isEmpty());
    scene.setSourceValue(1, QStringLiteral("apexDeg"), 500.0);
    CHECK(scene.sources()[1].apexDeg == 180.0);
    scene.setSourceValue(1, QStringLiteral("length"), -1.0); // refused
    CHECK(scene.sources()[1].length > 0.0);
    scene.setSourceValue(0, QStringLiteral("fed"), 1.0);
    CHECK(scene.sources()[0].fed == 1);
    scene.setSourceValue(0, QStringLiteral("voltage"), 2.0);
    CHECK(scene.sources()[0].strength == 2.0);

    // Settings are undoable, clamped, and shown with a note on the wavelength.
    scene.setSetting(QStringLiteral("frequency"), 150e6);
    scene.setSetting(QStringLiteral("sweepPoints"), 1e6);
    CHECK(scene.settings().sweepPoints == 401);
    scene.undo();
    CHECK(scene.settings().sweepPoints == 61);
    scene.undo();
    CHECK(scene.frequency() == 300e6);
    scene.redo();
    CHECK(scene.frequency() == 150e6);
    CHECK(scene.settingsData().value(QStringLiteral("frequency")).toDouble() == 150e6);
    CHECK(scene.settingsNote().contains(QStringLiteral("λ")));

    // Files keep the elements and the settings; another module refuses them.
    AntennaScene copy;
    REQUIRE(copy.fromJson(scene.toJson()));
    CHECK(copy.sources() == scene.sources());
    CHECK(copy.settings() == scene.settings());
    WaveScene wrongModule;
    CHECK(!wrongModule.fromJson(scene.toJson()));
    scene.newScene();
    CHECK(scene.count() == 0);
    CHECK(scene.settings() == qsig::app::AntennaSettings{});

    // Every example loads, with a sweep band around its frequency.
    for (const QVariant &e : scene.examples())
    {
        scene.loadExample(e.toMap().value(QStringLiteral("key")).toString());
        CHECK(scene.count() > 0);
        CHECK(!scene.canUndo());
        CHECK(scene.settings().sweepStart < scene.frequency());
        CHECK(scene.settings().sweepStop > scene.frequency());
    }
}

TEST_CASE("antennas scene: background solve, results and frequency sweep", "[app]")
{
    AntennaScene scene;
    scene.loadExample(QStringLiteral("dipole"));
    scene.waitForResult();
    REQUIRE(scene.result() != nullptr);
    REQUIRE(scene.result()->valid);
    CHECK(!scene.busy());
    REQUIRE(scene.result()->feeds.size() == 1);
    CHECK_NEAR(scene.result()->feeds[0].impedance.real(), 71.5, 0.0, 3.0);
    CHECK_NEAR(scene.result()->directivityDbi, 2.14, 0.0, 0.05);
    CHECK(scene.statusText().contains(QStringLiteral("inconnues")));
    // Readings: the feed (R, X, |S11|, VSWR, |I|, P) and the radiation group.
    const QVariantMap reading = scene.probe(0.0, 0.0, 0.0);
    const QVariantList rows = reading.value(QStringLiteral("rows")).toList();
    REQUIRE(rows.size() == 6);
    CHECK_NEAR(rows[0].toMap().value(QStringLiteral("value")).toDouble(), scene.result()->feeds[0].impedance.real(), 0.0, 1e-9);
    const QVariantList groups = reading.value(QStringLiteral("groups")).toList();
    REQUIRE(groups.size() == 1);
    CHECK(groups[0].toMap().value(QStringLiteral("rows")).toList().size() == 5);

    // An edit is solved again (debounced).
    scene.setSourceValue(0, QStringLiteral("length"), 0.6);
    scene.waitForResult();
    REQUIRE(scene.result()->valid);
    CHECK(scene.result()->feeds[0].impedance.real() > 100.0); // longer than resonant: R rises
    scene.undo();
    scene.waitForResult();
    CHECK_NEAR(scene.result()->feeds[0].impedance.real(), 71.5, 0.0, 3.0);

    // Sweep on the thread pool: points in order, current until the scene changes (the analysis
    // frequency alone does not change it).
    scene.setSetting(QStringLiteral("sweepPoints"), 5);
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&scene, &AntennaScene::sweepRunningChanged, &loop,
                     [&]
                     {
                         if (!scene.sweepRunning()) loop.quit();
                     });
    scene.runSweep();
    CHECK(scene.sweepRunning());
    timeout.start(30000);
    loop.exec();
    CHECK(!scene.sweepRunning());
    REQUIRE(scene.hasSweep());
    const auto sweep = scene.sweep();
    REQUIRE(sweep->size() == 5);
    CHECK_REL(sweep->front().frequency, 0.8 * 300e6, 1e-12);
    CHECK_REL(sweep->back().frequency, 1.2 * 300e6, 1e-12);
    for (std::size_t i = 0; i < sweep->size(); ++i)
    {
        CHECK((*sweep)[i].valid);
        if (i > 0) CHECK((*sweep)[i].frequency > (*sweep)[i - 1].frequency);
    }
    CHECK_NEAR(scene.sweepProgress(), 1.0, 0.0, 1e-12);
    CHECK(!scene.sweepStale());
    scene.setFrequency(310e6);
    CHECK(!scene.sweepStale());
    scene.setSourceValue(0, QStringLiteral("length"), 0.4);
    CHECK(scene.sweepStale());
    scene.undo();
    CHECK(!scene.sweepStale());
    scene.waitForResult();
}

TEST_CASE("antennas scene: the Yagi optimiser is one undoable step, the bench reports its lines", "[app]")
{
    AntennaScene scene;
    scene.loadExample(QStringLiteral("yagi3"));
    const auto start = scene.sources();
    CHECK(scene.goalNames().size() == 3);
    auto waitFor = [&](auto signal, auto done)
    {
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        QObject::connect(&scene, signal, &loop,
                         [&]
                         {
                             if (done()) loop.quit();
                         });
        timeout.start(60000);
        if (!done()) loop.exec();
    };

    scene.optimise(2, true, 30);
    CHECK(scene.optimising());
    waitFor(&AntennaScene::optimiserChanged, [&] { return !scene.optimising(); });
    REQUIRE(!scene.optimising());
    const QVariantMap result = scene.optimiserResult();
    CHECK(result.value(QStringLiteral("valid")).toBool());
    CHECK(result.value(QStringLiteral("evaluations")).toInt() <= 30);
    REQUIRE(result.value(QStringLiteral("rows")).toList().size() == 6);
    if (result.value(QStringLiteral("applied")).toBool())
    {
        CHECK(!(scene.sources() == start));
        CHECK(scene.canUndo());
        CHECK(scene.undoText() == QStringLiteral("Optimiser la Yagi"));
        scene.undo();
        CHECK(scene.sources() == start);
    }
    CHECK(!scene.optimiserText().isEmpty());

    scene.runBench();
    CHECK(scene.benchRunning());
    waitFor(&AntennaScene::benchChanged, [&] { return !scene.benchRunning(); });
    REQUIRE(scene.benchCases().size() == 14);
    const QVariantMap first = scene.benchCases().front().toMap();
    CHECK(first.value(QStringLiteral("value")).toString().endsWith(QStringLiteral("Ω")));
    CHECK(first.value(QStringLiteral("deviation")).toString().endsWith(QStringLiteral("%")));
    CHECK(first.value(QStringLiteral("verdict")).toInt() == 0);
    CHECK(scene.benchSummary().startsWith(QStringLiteral("14 conformes")));
    CHECK(Units::formatQuantity(0.5, QStringLiteral("dBi")) == QStringLiteral("0,50 dBi"));
    scene.waitForResult();
}

TEST_CASE("static scenes: point B gives the voltage, the differences and the profiles from A to B", "[app]")
{
    auto rowsOf = [](const QVariant &group) { return group.toMap().value(QStringLiteral("rows")).toList(); };
    auto valueOf = [](const QVariant &row) { return row.toMap().value(QStringLiteral("value")).toDouble(); };

    ElectrostaticScene electro;
    electro.addSource(0); // point charge at the origin
    CHECK(electro.hasDifferentialProbe());
    CHECK(electro.hasSpaceView());
    CHECK(electro.shape3d(0, {}, 1.0).point);
    CHECK(electro.sourceReach(0) == 0.0);
    CHECK(!electro.probe(1.0, 0.0, 0.0).contains(QStringLiteral("groups")));
    electro.setProbePoints(QVector3D(1.0f, 0.0f, 0.0f), true, QVector3D(2.0f, 0.0f, 0.0f));
    QVariantMap reading = electro.probe(1.0, 0.0, 0.0);
    QVariantList groups = reading.value(QStringLiteral("groups")).toList();
    REQUIRE(groups.size() == 2);
    const auto fa = Electrostatics::fieldAt(electro.sources(), {1.0, 0.0, 0.0});
    const auto fb = Electrostatics::fieldAt(electro.sources(), {2.0, 0.0, 0.0});
    QVariantList rows = rowsOf(groups[0]);
    REQUIRE(rows.size() == 5);
    CHECK_REL(valueOf(rows[0]), fb.electricField.norm(), 1e-12);
    CHECK_REL(valueOf(rows[4]), fb.potential, 1e-12);
    rows = rowsOf(groups[1]);
    REQUIRE(rows.size() == 4);
    CHECK_REL(valueOf(rows[0]), fa.potential - fb.potential, 1e-12); // U_AB = V_A - V_B
    CHECK_REL(valueOf(rows[0]), fa.potential / 2.0, 1e-12);         // 1/r: half the potential is lost from 1 m to 2 m
    CHECK_REL(valueOf(rows[1]), 1.0, 1e-12);                        // distance
    CHECK_REL(valueOf(rows[2]), fa.potential - fb.potential, 1e-12); // mean field along AB = U / d
    CHECK_REL(valueOf(rows[3]), 0.25, 1e-12);                       // 1/r^2
    const QVariantList profiles = reading.value(QStringLiteral("profiles")).toList();
    REQUIRE(profiles.size() == 2);
    const QVariantList potential = profiles[0].toMap().value(QStringLiteral("values")).toList();
    REQUIRE(potential.size() == 97);
    CHECK_REL(potential.front().toDouble(), fa.potential, 1e-12);
    CHECK_REL(potential.back().toDouble(), fb.potential, 1e-12);
    CHECK_REL(reading.value(QStringLiteral("profileLength")).toDouble(), 1.0, 1e-12);
    electro.setProbePoints(QVector3D(), false, QVector3D());
    CHECK(!electro.probe(1.0, 0.0, 0.0).contains(QStringLiteral("groups")));

    MagnetostaticScene magneto;
    magneto.addSource(0); // infinite wire on z through the origin
    CHECK(magneto.hasDifferentialProbe());
    CHECK(magneto.shape3d(0, {}, 1.5).infinite);
    const double current = magneto.sources()[0].strength;
    magneto.setProbePoints(QVector3D(1.0f, 0.0f, 0.0f), true, QVector3D(0.0f, 1.0f, 0.0f));
    reading = magneto.probe(1.0, 0.0, 0.0);
    groups = reading.value(QStringLiteral("groups")).toList();
    REQUIRE(groups.size() == 2);
    rows = rowsOf(groups[1]);
    REQUIRE(rows.size() == 5);
    CHECK_NEAR(valueOf(rows[0]), 0.0, 0.0, 1e-18);            // same |B| at the same distance from the wire
    CHECK_NEAR(valueOf(rows[1]), 0.0, 0.0, 1e-9);             // 0 %
    CHECK_NEAR(valueOf(rows[2]), 90.0, 0.0, 1e-9);            // B turns with the azimuth
    CHECK_REL(valueOf(rows[3]), std::sqrt(2.0), 1e-12);       // distance
    CHECK_REL(valueOf(rows[4]), current / 4.0, 1e-9);         // quarter of a turn around the wire: I / 4
    CHECK(reading.value(QStringLiteral("profiles")).toList().size() == 1);
    CHECK(Units::formatQuantity(0.5, QStringLiteral("%")) == QStringLiteral("0,5 %"));
}

TEST_CASE("antennas scene: wires drawn in the designer are joined, sources placed on them", "[app]")
{
    using Type = qsig::app::AntennaElement::Type;
    AntennaScene scene;
    int errors = 0;
    QObject::connect(&scene, &AntennaScene::errorOccurred, &scene, [&errors] { ++errors; });
    // Two wires end to end along z: 0.5 m in all, half a wavelength at 300 MHz.
    REQUIRE(scene.addWire({0.0, 0.0, -0.2}, {0.0, 0.0, 0.1}) == 0);
    REQUIRE(scene.addWire({0.0, 0.0, 0.1}, {0.0, 0.0, 0.3}) == 1);
    CHECK(scene.sources()[0].type == Type::Wire);
    CHECK(scene.sources()[0].fed == 0);
    CHECK(scene.sources()[0].name != scene.sources()[1].name);
    CHECK_NEAR(scene.sources()[1].length, 0.2, 0.0, 1e-15);
    CHECK_NEAR(scene.sources()[1].position.z, 0.2, 0.0, 1e-15);
    CHECK_NEAR(scene.sources()[1].elevationDeg, 90.0, 0.0, 1e-12);
    // Too short for its radius: refused with a message, nothing added.
    CHECK(scene.addWire({0.0, 0.0, 0.0}, {0.0, 0.0, 1e-4}) == -1);
    CHECK(errors == 1);
    CHECK(scene.count() == 2);
    // The source goes to the point of the wire nearest to the click; the middle attracts it.
    CHECK(scene.placeFeed(0, {0.05, 0.0, -0.11}) == 0);
    CHECK(scene.sources()[0].fed == 1);
    CHECK_NEAR(scene.sources()[0].feedFraction, 0.3, 0.0, 1e-12);
    CHECK(scene.undoText().startsWith(QStringLiteral("Placer une source")));
    CHECK(scene.feedTotal() == 1);
    // The source is an entity: slid along its wire, one undo step for the drag; the middle attracts it.
    CHECK(scene.moveFeed(0, 0, {0.0, 0.3, -0.045}));
    scene.endDrag();
    CHECK_NEAR(scene.sources()[0].feedFraction, 0.5, 0.0, 1e-12);
    // The two wires make one antenna: a half-wave dipole fed 5 cm below its centre.
    scene.waitForResult();
    REQUIRE(scene.result() != nullptr);
    REQUIRE(scene.result()->valid);
    REQUIRE(scene.result()->feeds.size() == 1);
    // Fed off its centre, where the current is lower: a higher resistance than the 86 ohm of the centre.
    CHECK_NEAR(scene.result()->feeds[0].impedance.real(), 137.7, 0.0, 5.0);
    CHECK_NEAR(scene.result()->directivityDbi, 2.18, 0.0, 0.1);
    // Each gesture is one undo step.
    scene.undo();
    scene.undo();
    CHECK(scene.sources()[0].fed == 0);
    scene.undo();
    CHECK(scene.count() == 1);

    // Sources are entities placed on the wires: a plain wire (0.3 m, from z = -0.2 to 0.1) carries
    // several, listed in the order of the scene; each one is edited, slid and removed on its own.
    CHECK(scene.placeFeed(0, {0.0, 0.0, -0.125}) == 0); // at a quarter
    CHECK(scene.placeFeed(0, {0.0, 0.0, 0.025}) == 1);  // one more, at three quarters
    CHECK(scene.placeFeed(0, {0.0, 0.0, 0.03}) == 1);   // on the one already there: nothing is added
    CHECK(scene.feedTotal() == 2);
    const QVariantList feedEntries = scene.feedList();
    REQUIRE(feedEntries.size() == 2);
    const QVariantMap secondFeed = feedEntries[1].toMap();
    CHECK(secondFeed.value(QStringLiteral("row")).toInt() == 0);
    CHECK(secondFeed.value(QStringLiteral("index")).toInt() == 1);
    CHECK(secondFeed.value(QStringLiteral("name")).toString() == QStringLiteral("Source 2"));
    CHECK(secondFeed.value(QStringLiteral("movable")).toBool());
    CHECK_NEAR(secondFeed.value(QStringLiteral("fraction")).toDouble(), 0.75, 0.0, 1e-12);
    scene.setFeedValue(0, 1, QStringLiteral("voltage"), 2.0);
    scene.setFeedValue(0, 1, QStringLiteral("phaseDeg"), 90.0);
    REQUIRE(scene.sources()[0].moreFeeds.size() == 1);
    CHECK(scene.sources()[0].moreFeeds[0].strength == 2.0);
    CHECK(scene.sources()[0].moreFeeds[0].phaseDeg == 90.0);
    CHECK(scene.sources()[0].strength == 1.0); // the first source keeps its own voltage
    // Two sources do not share a place; a slide is one undo step.
    CHECK(!scene.moveFeed(0, 1, {0.0, 0.0, -0.125}));
    CHECK(scene.moveFeed(0, 1, {0.0, 0.0, -0.05}));
    scene.endDrag();
    CHECK_NEAR(scene.sources()[0].moreFeeds[0].fraction, 0.5, 0.0, 1e-12);
    scene.undo();
    CHECK_NEAR(scene.sources()[0].moreFeeds[0].fraction, 0.75, 0.0, 1e-12);
    // Saved with their wire and read back.
    AntennaScene reread;
    REQUIRE(reread.fromJson(scene.toJson()));
    CHECK(reread.feedTotal() == 2);
    CHECK(reread.sources()[0].moreFeeds == scene.sources()[0].moreFeeds);
    // Both drive the antenna, each with its own result.
    scene.waitForResult();
    REQUIRE(scene.result() != nullptr);
    REQUIRE(scene.result()->valid);
    REQUIRE(scene.result()->feeds.size() == 2);
    CHECK(scene.result()->feeds[1].element == 0);
    CHECK(scene.result()->feeds[1].feedIndex == 1);
    // Removing the first one: the other becomes the only source of the wire, then the wire is passive.
    scene.deleteFeed(0, 0);
    CHECK(scene.feedTotal() == 1);
    CHECK(scene.sources()[0].fed == 1);
    CHECK_NEAR(scene.sources()[0].feedFraction, 0.75, 0.0, 1e-12);
    CHECK(scene.sources()[0].strength == 2.0);
    scene.deleteFeed(0, 0);
    CHECK(scene.sources()[0].fed == 0);
    CHECK(scene.feedTotal() == 0);
    scene.deleteFeed(0, 0); // nothing left to remove: no undo step
    scene.undo();
    CHECK(scene.feedTotal() == 1);
    // A ready-made element comes with its source, at the place its type gives it, and takes no more;
    // a plain wire takes one more, in the middle of its longest free stretch.
    const int readyMade = scene.addSource(static_cast<int>(Type::Dipole));
    CHECK(scene.feedTotal() == 2);
    CHECK(scene.placeFeed(readyMade, {0.0, 0.0, 0.0}) == 0);
    const int errorsBefore = errors;
    CHECK(scene.addFeedTo(readyMade) == -1);
    CHECK(errors == errorsBefore + 1);
    scene.deleteFeed(readyMade, 0);
    CHECK(scene.sources()[static_cast<std::size_t>(readyMade)].fed == 0);
    CHECK(scene.addFeedTo(readyMade) == 0);
    CHECK(scene.addFeedTo(0) == 1);
    REQUIRE(scene.sources()[0].moreFeeds.size() == 1);
    CHECK_NEAR(scene.sources()[0].moreFeeds[0].fraction, 0.375, 0.0, 1e-12);
    scene.waitForResult();
}

TEST_CASE("field map: the colour scales start from the tone of the ground", "[app]")
{
    using qsig::app::FieldMapComputer;
    using qsig::app::FieldQuantity;
    // A magnitude: the weak end merges with the ground, the strong end stands out against it.
    const QColor weakOnDark = FieldMapComputer::mapColour(FieldQuantity::Magnitude, 0.0, false);
    const QColor weakOnLight = FieldMapComputer::mapColour(FieldQuantity::Magnitude, 0.0, true);
    CHECK(weakOnDark.lightness() < 12);
    CHECK(weakOnLight.lightness() > 244);
    CHECK(FieldMapComputer::mapColour(FieldQuantity::Magnitude, 1.0, false).lightness() > 180);
    CHECK(FieldMapComputer::mapColour(FieldQuantity::Magnitude, 1.0, true).lightness() < 80);
    // The dark-ground scale is the historical one, shared with the wave module.
    CHECK(FieldMapComputer::mapColour(FieldQuantity::Magnitude, 0.6, false) == FieldMapComputer::sequentialColour(0.6));
    // A signed quantity: the ground at zero, blue below and red above on both grounds.
    for (const bool light : {false, true})
    {
        const QColor zero = FieldMapComputer::mapColour(FieldQuantity::Scalar, 0.5, light);
        const QColor negative = FieldMapComputer::mapColour(FieldQuantity::Scalar, 0.1, light);
        const QColor positive = FieldMapComputer::mapColour(FieldQuantity::Scalar, 0.9, light);
        CHECK((light ? zero.lightness() > 235 : zero.lightness() < 30));
        CHECK(negative.blue() > negative.red());
        CHECK(positive.red() > positive.blue());
    }
    // Out of range values are clamped to the ends.
    CHECK(FieldMapComputer::mapColour(FieldQuantity::Magnitude, -3.0, true) == weakOnLight);
}

TEST_CASE("field map: requests coming faster than the maps are computed do not starve the display", "[app]")
{
    ElectrostaticScene scene;
    scene.loadExample(QStringLiteral("dipole"));
    auto make = [&scene](double u0, int columns, int quantity)
    {
        qsig::app::FieldMapRequest q;
        q.sampler = scene.sampler(quantity, qsig::app::ViewPlane::XY);
        q.quantity = quantity == 1 ? qsig::app::FieldQuantity::Scalar : qsig::app::FieldQuantity::Magnitude;
        q.u0 = u0;
        q.v0 = 1.0;
        q.du = 2.0 / (columns - 1);
        q.columns = columns;
        q.rows = columns;
        return q;
    };
    qsig::app::FieldMapComputer computer;
    std::vector<double> delivered; // u0 of the maps received, in order
    int busyChanges = 0;
    QObject::connect(&computer, &qsig::app::FieldMapComputer::ready, &computer,
                     [&delivered](std::shared_ptr<const qsig::app::FieldMapResult> r) { delivered.push_back(r->request.u0); });
    QObject::connect(&computer, &qsig::app::FieldMapComputer::busyChanged, &computer, [&busyChanges] { ++busyChanges; });
    auto wait = [&computer]
    {
        QElapsedTimer clock;
        clock.start();
        while (computer.busy() && clock.elapsed() < 20000) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    };

    // A source being dragged: a request at every mouse move, for the same view.
    computer.request(make(-1.0, 120, 0));
    computer.request(make(-1.1, 120, 0));
    computer.request(make(-1.2, 120, 0));
    CHECK(computer.busy());
    wait();
    // The running map is delivered, then the latest request; the one in between was replaced.
    REQUIRE(delivered.size() == 2);
    CHECK(delivered[0] == -1.0);
    CHECK(delivered[1] == -1.2);
    CHECK(busyChanges == 2); // busy once, from the first request to the last map

    // Another quantity: the running map shows what is no longer asked for, it is dropped.
    delivered.clear();
    computer.request(make(-1.0, 120, 0));
    computer.request(make(-1.3, 120, 1));
    wait();
    REQUIRE(delivered.size() == 1);
    CHECK(delivered[0] == -1.3);

    // cancel(): neither the running map nor the waiting request is delivered.
    delivered.clear();
    computer.request(make(-1.0, 120, 0));
    computer.request(make(-1.4, 120, 0));
    computer.cancel();
    wait();
    CHECK(delivered.empty());
    CHECK(!computer.busy());
    // And the computer serves the next request as usual.
    computer.request(make(-1.5, 120, 0));
    wait();
    REQUIRE(delivered.size() == 1);
    CHECK(delivered[0] == -1.5);
}

TEST_CASE("antennas scene: designer transformations of a selection, each one undo step", "[app]")
{
    using Type = qsig::app::AntennaElement::Type;
    using qsig::Vec3;
    AntennaScene scene;
    REQUIRE(scene.addWire({0.0, 0.0, -0.2}, {0.0, 0.0, 0.2}) == 0);
    REQUIRE(scene.addWire({0.3, 0.0, -0.2}, {0.3, 0.0, 0.2}) == 1);
    const std::string firstName = scene.sources()[0].name;

    // Displacement of both, then an array of two copies of the first one.
    QList<int> rows = scene.translateElements({0, 1}, {0.0, 0.1, 0.0}, 0);
    CHECK(rows == QList<int>({0, 1}));
    CHECK_NEAR(scene.sources()[0].position.y, 0.1, 0.0, 1e-15);
    CHECK_NEAR(scene.sources()[1].position.y, 0.1, 0.0, 1e-15);
    rows = scene.translateElements({0}, {-0.3, 0.0, 0.0}, 2);
    CHECK(rows == QList<int>({2, 3}));
    REQUIRE(scene.count() == 4);
    CHECK_NEAR(scene.sources()[2].position.x, -0.3, 0.0, 1e-15);
    CHECK_NEAR(scene.sources()[3].position.x, -0.6, 0.0, 1e-15);
    CHECK(scene.sources()[0].position.x == 0.0); // the original stays
    // Every element has its own name.
    for (int i = 0; i < scene.count(); ++i)
        for (int j = i + 1; j < scene.count(); ++j) CHECK(scene.sources()[i].name != scene.sources()[j].name);
    scene.undo();
    CHECK(scene.count() == 2);
    scene.undo();
    CHECK(scene.sources()[0].position.y == 0.0);
    CHECK(scene.sources()[1].position.y == 0.0);

    // Quarter turn of both about z through the centre of the pair: the pair now lies along y.
    const Vec3 centre = scene.centreOf({0, 1});
    CHECK_NEAR(qsig::norm(centre - Vec3{0.15, 0.0, 0.0}), 0.0, 0.0, 1e-12);
    scene.rotateElements({0, 1}, centre, {0.0, 0.0, 1.0}, 90.0, false);
    CHECK_NEAR(qsig::norm(scene.sources()[0].position - Vec3{0.15, -0.15, 0.0}), 0.0, 0.0, 1e-12);
    CHECK_NEAR(qsig::norm(scene.sources()[1].position - Vec3{0.15, 0.15, 0.0}), 0.0, 0.0, 1e-12);
    CHECK(scene.sources()[0].elevationDeg == 90.0); // vertical wires stay vertical
    scene.undo();
    CHECK(scene.sources()[1].position.x == 0.3);

    // Mirror copy of the second wire in the plane x = 0, alignment, removal of several rows.
    rows = scene.mirrorElements({1}, {0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, true);
    REQUIRE(rows == QList<int>({2}));
    CHECK_NEAR(qsig::norm(scene.sources()[2].position - Vec3{-0.3, 0.0, 0.0}), 0.0, 0.0, 1e-12);
    scene.alignElements({0, 1, 2}, 2, 0.05);
    for (int i = 0; i < 3; ++i) CHECK(scene.sources()[i].position.z == 0.05);
    scene.undo();
    CHECK(scene.sources()[0].position.z == 0.0);
    scene.removeElements({2, 0, 2, 7}); // twice the same row and a row that does not exist
    REQUIRE(scene.count() == 1);
    CHECK(scene.sources()[0].position.x == 0.3);
    scene.undo();
    REQUIRE(scene.count() == 3);
    CHECK(scene.sources()[0].name == firstName);

    // Elements dragged together: one undo step for the whole drag.
    scene.dragElements({0, 1}, {Vec3{0.0, 0.01, 0.0}, Vec3{0.3, 0.01, 0.0}});
    scene.dragElements({0, 1}, {Vec3{0.0, 0.02, 0.0}, Vec3{0.3, 0.02, 0.0}});
    scene.endDrag();
    CHECK(scene.sources()[1].position.y == 0.02);
    scene.undo();
    CHECK(scene.sources()[0].position.y == 0.0);
    CHECK(scene.sources()[1].position.y == 0.0);

    // An end of a wire dragged: the other end stays, the source keeps its fraction; refused when the
    // wire would be too short.
    scene.placeFeed(0, {0.0, 0.0, 0.0});
    CHECK(scene.moveWireEnd(0, 1, {0.0, 0.0, 0.3}));
    CHECK(scene.moveWireEnd(0, 1, {0.1, 0.0, 0.3}));
    scene.endDrag();
    Vec3 a, b;
    qsig::app::wireEndpoints(scene.sources()[0], a, b);
    CHECK_NEAR(qsig::norm(a - Vec3{0.0, 0.0, -0.2}), 0.0, 0.0, 1e-15);
    CHECK_NEAR(qsig::norm(b - Vec3{0.1, 0.0, 0.3}), 0.0, 0.0, 1e-15);
    CHECK(scene.sources()[0].feedFraction == 0.5);
    CHECK(!scene.moveWireEnd(0, 1, {0.0, 0.0, -0.2 + 1e-4}));
    CHECK(!scene.moveWireEnd(7, 0, {0.0, 0.0, 0.0}));
    scene.endDrag();
    scene.undo(); // the two moves of the end are one step
    qsig::app::wireEndpoints(scene.sources()[0], a, b);
    CHECK_NEAR(qsig::norm(b - Vec3{0.0, 0.0, 0.2}), 0.0, 0.0, 1e-15);

    // A wire cut in two: the parts touch, the source stays on its part, one undo step.
    const int count = scene.count();
    const int second = scene.cutWire(0, {0.0, 0.0, 0.1});
    REQUIRE(second == count);
    Vec3 c, d;
    qsig::app::wireEndpoints(scene.sources()[0], a, b);
    qsig::app::wireEndpoints(scene.sources()[static_cast<std::size_t>(second)], c, d);
    CHECK_NEAR(qsig::norm(b - Vec3{0.0, 0.0, 0.1}), 0.0, 0.0, 1e-15);
    CHECK_NEAR(qsig::norm(c - b), 0.0, 0.0, 1e-15);
    CHECK_NEAR(qsig::norm(d - Vec3{0.0, 0.0, 0.2}), 0.0, 0.0, 1e-15);
    CHECK(scene.sources()[0].fed == 1);
    CHECK(scene.sources()[static_cast<std::size_t>(second)].fed == 0);
    CHECK(scene.sources()[static_cast<std::size_t>(second)].type == Type::Wire);
    int errors = 0;
    QObject::connect(&scene, &AntennaScene::errorOccurred, &scene, [&errors] { ++errors; });
    CHECK(scene.cutWire(0, {0.0, 0.0, -0.2}) == -1); // at an end
    CHECK(errors == 1);
    scene.undo();
    CHECK(scene.count() == count);
    scene.waitForResult();
}

TEST_CASE("antennas scene: groups and constraints are kept by every edit, undone and saved", "[app]")
{
    using qsig::Vec3;
    using CType = qsig::app::AntennaConstraint::Type;
    AntennaScene scene;
    int errors = 0;
    QObject::connect(&scene, &AntennaScene::errorOccurred, &scene, [&errors] { ++errors; });
    REQUIRE(scene.addWire({0.0, 0.0, 0.0}, {0.0, 0.0, 0.4}) == 0); // the reference
    REQUIRE(scene.addWire({0.3, 0.1, 0.0}, {0.5, 0.3, 0.2}) == 1); // the one that follows
    REQUIRE(scene.addWire({0.8, 0.0, 0.0}, {0.8, 0.0, 0.3}) == 2);
    // Every element has its own id, kept from one edit to the next.
    const int id0 = scene.sources()[0].id, id1 = scene.sources()[1].id;
    CHECK(id0 > 0);
    CHECK(id1 > 0);
    CHECK(id0 != id1);

    // The first end of wire 1 on the second end of wire 0, at a right angle to it.
    REQUIRE(scene.addConstraint(1, static_cast<int>(CType::Coincident), 0, 0.0, 1, 2, 0));
    REQUIRE(scene.addConstraint(1, static_cast<int>(CType::Perpendicular), 0, 0.0, 0, 0, 0));
    auto joined = [&scene]
    {
        Vec3 a, b, c, d;
        qsig::app::wireEndpoints(scene.sources()[0], a, b);
        qsig::app::wireEndpoints(scene.sources()[1], c, d);
        return qsig::norm(c - b) < 1e-9 && std::abs(qsig::dot(qsig::normalized(b - a), qsig::normalized(d - c))) < 1e-6;
    };
    CHECK(joined());
    QVariantList list = scene.constraintList(1);
    REQUIRE(list.size() == 2);
    CHECK(list[0].toMap().value(QStringLiteral("met")).toBool());
    CHECK(list[1].toMap().value(QStringLiteral("met")).toBool());
    CHECK(list[1].toMap().value(QStringLiteral("text")).toString().contains(QString::fromStdString(scene.sources()[0].name)));
    // The reference is moved, turned, resized: the other one follows at every edit.
    scene.moveSource(0, 0.2, -0.1, 0.05);
    scene.endDrag();
    CHECK(joined());
    scene.rotateElements({0}, scene.sources()[0].position, {1.0, 0.0, 0.0}, 40.0, false);
    CHECK(joined());
    CHECK(scene.moveWireEnd(0, 1, {0.3, 0.3, 0.6}));
    scene.endDrag();
    CHECK(joined());
    // Dragging the constrained wire itself does not break the constraint.
    scene.moveSource(1, 5.0, 5.0, 5.0);
    scene.endDrag();
    CHECK(joined());
    CHECK(scene.sources()[0].id == id0);
    CHECK(scene.sources()[1].id == id1);

    // A new direction with the same reference replaces the previous one; a second kind adds up.
    REQUIRE(scene.addConstraint(1, static_cast<int>(CType::Parallel), 0, 0.0, 0, 0, 0));
    CHECK(scene.constraintList(1).size() == 2);
    REQUIRE(scene.addConstraint(1, static_cast<int>(CType::EqualLength), 0, 0.0, 0, 0, 0));
    CHECK(scene.constraintList(1).size() == 3);
    CHECK_NEAR(scene.sources()[1].length, scene.sources()[0].length, 0.0, 1e-12);
    scene.undo();
    scene.undo();
    CHECK(joined()); // perpendicular again
    // Suspended, a constraint lets go; enabled again, it is applied at once.
    scene.setConstraintEnabled(1, 0, false);
    scene.moveSource(1, 1.0, 1.0, 1.0);
    scene.endDrag();
    CHECK(!joined());
    scene.setConstraintEnabled(1, 0, true);
    CHECK(joined());

    // Refused: no reference, the element itself, a length for a loop, no axis to fix, a bad angle.
    const int before = errors;
    CHECK(!scene.addConstraint(1, static_cast<int>(CType::Parallel), -1, 0.0, 0, 0, 0));
    CHECK(!scene.addConstraint(1, static_cast<int>(CType::Parallel), 1, 0.0, 0, 0, 0));
    CHECK(!scene.addConstraint(1, static_cast<int>(CType::FixPosition), -1, 0.0, 0, 0, 0));
    CHECK(!scene.addConstraint(1, static_cast<int>(CType::Angle), 0, 200.0, 0, 0, 0));
    const int loop = scene.addSource(static_cast<int>(qsig::app::AntennaElement::Type::CircularLoop));
    CHECK(!scene.addConstraint(loop, static_cast<int>(CType::Length), -1, 0.3, 0, 0, 0));
    CHECK(errors == before + 5);
    // A fixed position that contradicts the junction: both stay listed, one is reported unmet.
    REQUIRE(scene.addConstraint(1, static_cast<int>(CType::FixPosition), -1, 0.0, 0, 0, 7));
    scene.moveSource(0, 2.0, 2.0, 2.0);
    scene.endDrag();
    list = scene.constraintList(1);
    int unmet = 0;
    for (const QVariant &item : list) unmet += item.toMap().value(QStringLiteral("met")).toBool() ? 0 : 1;
    CHECK(unmet >= 1);
    scene.undo(); // the move
    scene.removeConstraint(1, static_cast<int>(scene.constraintList(1).size()) - 1);
    CHECK(joined());

    // A copy keeps neither the constraints nor the group of its original, and gets its own id.
    scene.groupElements({0, 1});
    const QString group = scene.groupOf(0);
    CHECK(!group.isEmpty());
    CHECK(scene.groupOf(1) == group);
    CHECK(scene.groupOf(2).isEmpty());
    CHECK(scene.groupRows(1) == QList<int>({0, 1}));
    CHECK(scene.groupRows(2) == QList<int>({2}));
    const QList<int> copies = scene.translateElements({1}, {0.0, 0.5, 0.0}, 1);
    REQUIRE(copies.size() == 1);
    const auto &copy = scene.sources()[static_cast<std::size_t>(copies.first())];
    CHECK(copy.constraints.empty());
    CHECK(copy.group.empty());
    CHECK(copy.id != id1);
    CHECK(copy.id > 0);
    scene.undo();
    // Renaming: a name of its own, not that of another group.
    scene.groupElements({2, loop});
    scene.renameGroup(0, QStringLiteral(" Brins "));
    CHECK(scene.groupOf(1) == QStringLiteral("Brins"));
    const int refused = errors;
    scene.renameGroup(2, QStringLiteral("Brins"));
    CHECK(errors == refused + 1);
    CHECK(scene.groupOf(2) != QStringLiteral("Brins"));

    // Files keep ids, groups and constraints: the loaded scene is the same, and still constrained.
    AntennaScene loaded;
    REQUIRE(loaded.fromJson(scene.toJson()));
    CHECK(loaded.sources() == scene.sources());
    CHECK(loaded.constraintList(1).size() == scene.constraintList(1).size());
    loaded.moveSource(0, -0.4, 0.2, 0.1);
    loaded.endDrag();
    Vec3 a, b, c, d;
    qsig::app::wireEndpoints(loaded.sources()[0], a, b);
    qsig::app::wireEndpoints(loaded.sources()[1], c, d);
    CHECK_NEAR(qsig::norm(c - b), 0.0, 0.0, 1e-9);

    // The reference is deleted: the constraints that needed it go with it. Ungrouping frees the group.
    scene.ungroupElements({1});
    CHECK(scene.groupOf(0).isEmpty());
    CHECK(scene.groupOf(1).isEmpty());
    CHECK(!scene.groupOf(2).isEmpty()); // the other group stays
    scene.removeElements({0});
    CHECK(scene.constraintList(0).isEmpty()); // wire 1 is now row 0
    scene.undo();
    CHECK(scene.constraintList(1).size() == 2);
    scene.waitForResult();
}

TEST_CASE("antennas scene: surfaces - the source at the foot of a wire, currents of the plate, mesh setting, files", "[app]")
{
    using Type = qsig::app::AntennaElement::Type;
    const double lambda = qsig::constants::c0 / 300e6;
    AntennaScene scene;
    int errors = 0;
    QObject::connect(&scene, &AntennaScene::errorOccurred, &scene, [&errors] { ++errors; });
    // A plate: horizontal, one wavelength across, no source of its own.
    const int plate = scene.addSource(static_cast<int>(Type::Plate));
    REQUIRE(plate == 0);
    CHECK(scene.sources()[0].fed == 0);
    CHECK_REL(scene.sources()[0].width, lambda, 1e-12);
    CHECK_REL(scene.sources()[0].length, lambda, 1e-12);
    CHECK(scene.editableFields(static_cast<int>(Type::Plate)) ==
          QStringList({"width", "length", "azimuthDeg", "elevationDeg", "rollDeg"}));
    CHECK(scene.editableFields(static_cast<int>(Type::Disc)) == QStringList({"radius", "azimuthDeg", "elevationDeg"}));
    // The place of a source belongs to the source (setFeedValue), not to the fields of its element.
    CHECK(!scene.editableFields(static_cast<int>(Type::Helix)).contains(QStringLiteral("feedFraction")));

    // A wire drawn from the plate upwards, then the source tool near its foot: the source goes to the
    // foot itself, between the wire and the plate.
    const int wire = scene.addWire({0.0, 0.0, 0.0}, {0.0, 0.0, 0.24 * lambda});
    REQUIRE(wire == 1);
    scene.placeFeed(wire, {0.01, 0.0, 0.02 * lambda});
    CHECK(scene.sources()[1].fed == 1);
    CHECK(scene.sources()[1].feedFraction == 0.0);
    // Slid near its top, which stands on nothing: the source stays on the wire.
    CHECK(scene.moveFeed(wire, 0, {0.0, 0.0, 0.24 * lambda}));
    scene.endDrag();
    CHECK(scene.sources()[1].feedFraction == 0.98);
    scene.undo();
    CHECK(scene.sources()[1].feedFraction == 0.0);
    // A surface takes no source.
    scene.placeFeed(plate, {0.1, 0.1, 0.0});
    CHECK(errors == 1);
    CHECK(scene.sources()[0].fed == 0);

    scene.waitForResult();
    REQUIRE(scene.result() != nullptr);
    REQUIRE(scene.result()->valid);
    CHECK_NEAR(scene.result()->feeds[0].impedance.real(), 39.5, 0.0, 1.5);
    CHECK(scene.result()->facets.size() == 200);
    CHECK(scene.result()->maxSurfaceCurrent > 0.0);
    // Readings: the radiation, then the surfaces (a linear polarisation has no group of its own).
    QVariantList groups = scene.probe(0.0, 0.0, 0.0).value(QStringLiteral("groups")).toList();
    REQUIRE(groups.size() == 2);
    CHECK(groups[1].toMap().value(QStringLiteral("title")).toString() == QStringLiteral("Surfaces"));

    // The mesh of the surfaces: a setting, undoable, clamped, solved again, saved.
    CHECK(scene.settingFields().contains(QStringLiteral("facetsPerWavelength")));
    CHECK(scene.settingLabel(QStringLiteral("facetsPerWavelength")) != QStringLiteral("facetsPerWavelength"));
    CHECK(scene.settingUnit(QStringLiteral("facetsPerWavelength")).isEmpty());
    scene.setSetting(QStringLiteral("facetsPerWavelength"), 100.0);
    CHECK(scene.settings().facetsPerWavelength == 40.0);
    scene.undo();
    CHECK(scene.settings().facetsPerWavelength == 10.0);
    scene.setSetting(QStringLiteral("facetsPerWavelength"), 8.0);
    scene.waitForResult();
    REQUIRE(scene.result()->valid);
    CHECK(scene.result()->facets.size() == 128);
    CHECK_NEAR(scene.result()->feeds[0].impedance.real(), 39.5, 0.0, 2.0);
    AntennaScene copy;
    REQUIRE(copy.fromJson(scene.toJson()));
    CHECK(copy.sources() == scene.sources());
    CHECK(copy.settings() == scene.settings());
    CHECK(copy.settings().facetsPerWavelength == 8.0);

    // The position of the source as a field: the ends themselves below 0.02 and above 0.98. At the free
    // end the source drives nothing, and the status says why.
    scene.setSourceValue(wire, QStringLiteral("feedFraction"), 0.3);
    CHECK(scene.sources()[1].feedFraction == 0.3);
    scene.setSourceValue(wire, QStringLiteral("feedFraction"), 0.001);
    CHECK(scene.sources()[1].feedFraction == 0.0);
    scene.setSourceValue(wire, QStringLiteral("feedFraction"), 0.995);
    CHECK(scene.sources()[1].feedFraction == 1.0);
    scene.waitForResult();
    REQUIRE(scene.result() != nullptr);
    CHECK(!scene.result()->valid);
    CHECK(scene.statusText().contains(QStringLiteral("bout libre")));

    // The axial helix of the examples: right-hand circular polarisation among the readings.
    scene.loadExample(QStringLiteral("helix_axial"));
    REQUIRE(scene.count() == 2);
    CHECK(scene.sources()[0].type == Type::Disc);
    scene.waitForResult();
    REQUIRE(scene.result()->valid);
    groups = scene.probe(0.0, 0.0, 0.0).value(QStringLiteral("groups")).toList();
    REQUIRE(groups.size() == 3);
    CHECK(groups[1].toMap().value(QStringLiteral("title")).toString().contains(QStringLiteral("droite")));
    const QVariantList polarisation = groups[1].toMap().value(QStringLiteral("rows")).toList();
    REQUIRE(polarisation.size() == 1);
    CHECK(polarisation[0].toMap().value(QStringLiteral("value")).toDouble() < 1.5); // axial ratio, dB
}

TEST_CASE("antennas scene: an element created at a point (context menu of the view) is one undo step", "[app]")
{
    using Type = qsig::app::AntennaElement::Type;
    AntennaScene scene;
    const int row = scene.addSourceAt(static_cast<int>(Type::Dipole), {0.3, -0.2, 0.1});
    REQUIRE(row == 0);
    CHECK(scene.sources()[0].type == Type::Dipole);
    CHECK((scene.sources()[0].position == qsig::Vec3{0.3, -0.2, 0.1})); // a comma in braces splits a macro argument
    CHECK(scene.sources()[0].name == "Dipôle 1");
    CHECK(scene.undoText().startsWith(QStringLiteral("Ajouter")));
    // A second one of the same type takes the next name; the list button still spreads its elements along x.
    CHECK(scene.addSourceAt(static_cast<int>(Type::Dipole), {0.0, 0.0, 0.0}) == 1);
    CHECK(scene.sources()[1].name == "Dipôle 2");
    CHECK(scene.addSource(static_cast<int>(Type::Plate)) == 2);
    CHECK((scene.sources()[2].position == qsig::Vec3{-0.4, 0.0, 0.0}));
    scene.undo();
    scene.undo();
    scene.undo();
    CHECK(scene.count() == 0);
    CHECK(!scene.canUndo());
}

TEST_CASE("antennas scene: conducting solids - fields, a wire standing on a box, files", "[app]")
{
    using Type = qsig::app::AntennaElement::Type;
    AntennaScene scene;
    int errors = 0;
    QObject::connect(&scene, &AntennaScene::errorOccurred, &scene, [&errors] { ++errors; });
    const double lambda = qsig::constants::c0 / 300e6;

    // A box: a solid, which is a surface for everything said of surfaces; its three sizes are fields.
    const int box = scene.addSourceAt(static_cast<int>(Type::Box), {0.0, 0.0, 0.0});
    REQUIRE(box == 0);
    CHECK(scene.isSolid(box));
    CHECK(scene.isSurface(box));
    CHECK(!scene.isFed(box));
    CHECK(scene.sources()[0].name == "Pavé 1");
    // Its sizes, its axis, then what it is made of: the perfect conductor, or a dielectric.
    CHECK(scene.editableFields(static_cast<int>(Type::Box)) ==
          QStringList({"width", "length", "height", "azimuthDeg", "elevationDeg", "rollDeg", "medium", "permittivity", "lossTangent"}));
    CHECK(scene.editableFields(static_cast<int>(Type::Cylinder)) ==
          QStringList({"radius", "height", "azimuthDeg", "elevationDeg", "medium", "permittivity", "lossTangent"}));
    CHECK(scene.editableFields(static_cast<int>(Type::Sphere)) == QStringList({"radius", "medium", "permittivity", "lossTangent"}));
    CHECK(scene.fieldChoices(static_cast<int>(Type::Box), QStringLiteral("medium")).size() == 2);
    CHECK(!scene.isDielectric(box));
    CHECK_REL(scene.sources()[0].height, 0.2 * lambda, 1e-12);
    scene.setSourceValue(box, QStringLiteral("height"), -1.0); // refused
    CHECK_REL(scene.sources()[0].height, 0.2 * lambda, 1e-12);
    scene.setSourceValue(box, QStringLiteral("height"), 0.3 * lambda);
    CHECK_REL(scene.sources()[0].height, 0.3 * lambda, 1e-12);
    scene.undo();
    CHECK_REL(scene.sources()[0].height, 0.2 * lambda, 1e-12);
    // It takes no source itself.
    CHECK(scene.placeFeed(box, {0.0, 0.0, 0.1 * lambda}) == -1);
    CHECK(scene.addFeedTo(box) == -1);
    CHECK(errors == 2);
    CHECK(scene.feedTotal() == 0);

    // A wire standing on its top: the source placed near its foot goes to the foot itself, between the
    // wire and the box, and the box is the ground of the monopole.
    const int wire = scene.addWire({0.0, 0.0, 0.1 * lambda}, {0.0, 0.0, 0.34 * lambda});
    REQUIRE(wire == 1);
    CHECK(!scene.isSolid(wire));
    CHECK(scene.placeFeed(wire, {0.0, 0.0, 0.11 * lambda}) == 0);
    CHECK(scene.sources()[1].feedFraction == 0.0);
    scene.waitForResult();
    REQUIRE(scene.result() != nullptr);
    REQUIRE(scene.result()->valid);
    REQUIRE(scene.result()->feeds.size() == 1);
    // The value of the six joined plates (27.27 - j4.30 ohm), currents on the skin of the box.
    CHECK(std::abs(scene.result()->feeds[0].impedance - std::complex<double>(27.27, -4.30)) < 0.3);
    CHECK(!scene.result()->facets.empty());
    CHECK(scene.result()->facets.front().element == box);

    // Saved and read back, with the other solids.
    scene.addSourceAt(static_cast<int>(Type::Cylinder), {1.0, 0.0, 0.0});
    scene.addSourceAt(static_cast<int>(Type::Cone), {2.0, 0.0, 0.0});
    scene.addSourceAt(static_cast<int>(Type::Sphere), {3.0, 0.0, 0.0});
    AntennaScene copy;
    REQUIRE(copy.fromJson(scene.toJson()));
    CHECK(copy.sources() == scene.sources());
    CHECK(copy.isSolid(4));

    // The two examples with a solid load and solve.
    for (const QString &key : {QStringLiteral("monopole_box"), QStringLiteral("monopole_sphere")})
    {
        scene.loadExample(key);
        REQUIRE(scene.count() == 2);
        CHECK(scene.isSolid(0));
        scene.waitForResult();
        REQUIRE(scene.result() != nullptr);
        CHECK(scene.result()->valid);
    }

    // A dielectric solid: the example of the dipole beside a sphere of permittivity 4. The media of the
    // scene are the air, the perfect conductor and that dielectric.
    scene.loadExample(QStringLiteral("dipole_dielectric"));
    REQUIRE(scene.count() == 2);
    CHECK(scene.isSolid(1));
    CHECK(scene.isDielectric(1));
    CHECK(!scene.isDielectric(0));
    REQUIRE(scene.mediaList().size() == 3);
    CHECK(scene.mediaList()[2].toMap().value(QStringLiteral("kind")).toInt() == 2);
    CHECK(scene.mediaList()[2].toMap().value(QStringLiteral("name")).toString().contains(QStringLiteral("4")));
    scene.waitForResult();
    REQUIRE(scene.result() != nullptr);
    REQUIRE(scene.result()->valid);
    // All the power fed is radiated (no losses), and the sphere has currents on its skin.
    CHECK(std::abs(scene.result()->radiatedPowerW / scene.result()->acceptedPowerW - 1.0) < 5e-3);
    CHECK(!scene.result()->facets.empty());
    // Its medium is a choice, its permittivity and its losses are numbers; each change is undone.
    scene.setSourceValue(1, QStringLiteral("permittivity"), 0.5); // refused: less than the vacuum
    CHECK(scene.sources()[1].permittivity == 4.0);
    scene.setSourceValue(1, QStringLiteral("lossTangent"), 0.02);
    CHECK(scene.sources()[1].lossTangent == 0.02);
    CHECK(scene.mediaList()[2].toMap().value(QStringLiteral("name")).toString().contains(QStringLiteral("tan")));
    scene.setSourceValue(1, QStringLiteral("medium"), 0.0);
    CHECK(!scene.isDielectric(1));
    CHECK(scene.mediaList().size() == 2);
    scene.undo();
    CHECK(scene.isDielectric(1));
    AntennaScene dielectricCopy;
    REQUIRE(dielectricCopy.fromJson(scene.toJson()));
    CHECK(dielectricCopy.sources() == scene.sources());
    CHECK(dielectricCopy.isDielectric(1));
    scene.waitForResult();
}

TEST_CASE("antennas scene: reflectors and curved wires - a dish with its feed, a source slid along an arc", "[app]")
{
    using Type = qsig::app::AntennaElement::Type;
    AntennaScene scene;
    const double lambda = qsig::constants::c0 / 300e6;

    // A dish: a surface, its aperture and focal length as fields, nothing to feed on it.
    const int dish = scene.addSourceAt(static_cast<int>(Type::Paraboloid), {0.0, 0.0, 0.0});
    REQUIRE(dish == 0);
    CHECK(scene.isSurface(dish));
    CHECK(!scene.isSolid(dish));
    CHECK(scene.editableFields(static_cast<int>(Type::Paraboloid)) == QStringList({"radius", "focalLength", "azimuthDeg", "elevationDeg"}));
    CHECK(scene.fieldUnit(static_cast<int>(Type::Paraboloid), QStringLiteral("focalLength")) == QStringLiteral("m"));
    scene.setSourceValue(dish, QStringLiteral("focalLength"), -1.0); // refused
    CHECK_REL(scene.sources()[0].focalLength, 0.8 * lambda, 1e-12);
    scene.setSourceValue(dish, QStringLiteral("focalLength"), 0.6 * lambda);
    CHECK_REL(scene.sources()[0].focalLength, 0.6 * lambda, 1e-12);
    scene.undo();
    CHECK(scene.placeFeed(dish, {0.0, 0.0, 0.0}) == -1);

    // An arc: passive; a source placed on it goes where it is clicked, and slides along it.
    const int arc = scene.addSourceAt(static_cast<int>(Type::Arc), {2.0, 0.0, 0.0});
    REQUIRE(arc == 1);
    CHECK(scene.feedTotal() == 0);
    CHECK(scene.editableFields(static_cast<int>(Type::Arc)).contains(QStringLiteral("sweepDeg")));
    CHECK(scene.fieldUnit(static_cast<int>(Type::Arc), QStringLiteral("sweepDeg")) == QStringLiteral("°"));
    // The arc lies across the vertical axis, from -90 to +90 degrees about e2 = -y, towards e3 = +x.
    const qsig::Vec3 centre = scene.sources()[1].position;
    const double r = scene.sources()[1].radius;
    CHECK(scene.placeFeed(arc, centre + qsig::Vec3{r * std::sin(-45.0 * qsig::constants::pi / 180.0), -r * std::cos(45.0 * qsig::constants::pi / 180.0), 0.0}) == 0);
    CHECK_NEAR(scene.sources()[1].feedFraction, 0.25, 0.0, 1e-9); // a quarter: the quarters attract it
    const QVariantMap entry = scene.feedList().front().toMap();
    CHECK(entry.value(QStringLiteral("movable")).toBool());
    CHECK(entry.value(QStringLiteral("along")).toBool());
    CHECK(scene.moveFeed(arc, 0, centre + qsig::Vec3{0.0, -r, 0.0})); // the middle of the arc
    scene.endDrag();
    CHECK_NEAR(scene.sources()[1].feedFraction, 0.5, 0.0, 1e-9);
    scene.setFeedValue(arc, 0, QStringLiteral("fraction"), 0.75);
    CHECK_NEAR(scene.sources()[1].feedFraction, 0.75, 0.0, 1e-12);

    // A spiral comes with its source at the centre, which does not slide.
    const int spiral = scene.addSourceAt(static_cast<int>(Type::Spiral), {-2.0, 0.0, 0.0});
    CHECK(scene.feedTotal() == 2);
    CHECK(!scene.moveFeed(spiral, 0, {-2.0, 0.1, 0.0}));
    CHECK(scene.sources()[static_cast<std::size_t>(spiral)].innerRadius > 0.0);

    // Saved and read back.
    AntennaScene copy;
    REQUIRE(copy.fromJson(scene.toJson()));
    CHECK(copy.sources() == scene.sources());

    // The examples load and solve: the dish with its dipole at the focus, the parabolic cylinder, the spiral.
    for (const QString &key : {QStringLiteral("parabola_dipole"), QStringLiteral("parabolic_cylinder"), QStringLiteral("spiral")})
    {
        scene.loadExample(key);
        scene.waitForResult();
        REQUIRE(scene.result() != nullptr);
        CHECK(scene.result()->valid);
    }
    scene.loadExample(QStringLiteral("parabola_dipole"));
    scene.waitForResult();
    REQUIRE(scene.result() != nullptr);
    REQUIRE(scene.result()->valid);
    CHECK(scene.result()->directivityDbi > 11.5);
}

