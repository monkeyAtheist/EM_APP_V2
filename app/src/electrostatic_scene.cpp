#include "qsig/app/electrostatic_scene.h"

#include "outline_builder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>

namespace qsig::app
{
namespace
{
using Source = Electrostatics::Source;
using Type = Source::Type;
using qsig::Vec3;
constexpr int kTypeCount = static_cast<int>(Type::RectangularVolume) + 1;

struct TypeInfo
{
    const char *key;       // stable identifier in project files
    const char *name;      // French name shown in the interface
    const char *shortName; // prefix of automatic source names
};

// Indexed by Source::Type.
constexpr std::array<TypeInfo, kTypeCount> kTypes{{
    {"point_charge", QT_TRANSLATE_NOOP("Electrostatics", "Charge ponctuelle"), "q"},
    {"solid_sphere", QT_TRANSLATE_NOOP("Electrostatics", "Sphère pleine uniforme"), "Sphère"},
    {"spherical_shell", QT_TRANSLATE_NOOP("Electrostatics", "Coquille sphérique"), "Coquille"},
    {"infinite_cylinder_z", QT_TRANSLATE_NOOP("Electrostatics", "Cylindre plein infini (axe z)"), "Cylindre"},
    {"finite_line", QT_TRANSLATE_NOOP("Electrostatics", "Fil chargé fini"), "Fil"},
    {"rectangular_plate", QT_TRANSLATE_NOOP("Electrostatics", "Plaque rectangulaire"), "Plaque"},
    {"thick_spherical_shell", QT_TRANSLATE_NOOP("Electrostatics", "Sphère creuse épaisse"), "Sphère creuse"},
    {"infinite_hollow_cylinder_z", QT_TRANSLATE_NOOP("Electrostatics", "Cylindre creux infini (axe z)"), "Tube"},
    {"infinite_line", QT_TRANSLATE_NOOP("Electrostatics", "Fil infini (axe z)"), "Fil infini"},
    {"disk", QT_TRANSLATE_NOOP("Electrostatics", "Disque"), "Disque"},
    {"annulus", QT_TRANSLATE_NOOP("Electrostatics", "Anneau plat"), "Anneau"},
    {"triangular_plate", QT_TRANSLATE_NOOP("Electrostatics", "Plaque triangulaire"), "Triangle"},
    {"infinite_plane", QT_TRANSLATE_NOOP("Electrostatics", "Plan infini (xy)"), "Plan"},
    {"rectangular_volume", QT_TRANSLATE_NOOP("Electrostatics", "Pavé chargé"), "Pavé"},
}};

Type typeFromInt(int type)
{
    return static_cast<Type>(std::clamp(type, 0, kTypeCount - 1));
}
const TypeInfo &info(Type type)
{
    return kTypes[static_cast<std::size_t>(type)];
}

bool isLinearDensity(Type t)
{
    return t == Type::InfiniteCylinderZ || t == Type::InfiniteHollowCylinderZ || t == Type::InfiniteLine;
}

constexpr const char *kDimensions[] = {"radius", "innerRadius", "length", "width", "height", "thickness"};
} // namespace

ElectrostaticScene::ElectrostaticScene(QObject *parent) : SourceListScene(parent) {}

QString ElectrostaticScene::title() const
{
    return tr("Électrostatique");
}

// ---- Module description -------------------------------------------------------------------------

const char *ElectrostaticScene::typeKey(Type type) const
{
    return info(type).key;
}
QString ElectrostaticScene::shortName(Type type) const
{
    return QString::fromUtf8(info(type).shortName);
}

QStringList ElectrostaticScene::typeNames() const
{
    QStringList names;
    for (const auto &t : kTypes) names << QCoreApplication::translate("Electrostatics", t.name);
    return names;
}

Electrostatics::Source ElectrostaticScene::makeSource(Type type) const
{
    Source s;
    s.type = type;
    if (isLinearDensity(type)) s.strength = 1e-10;
    if (type == Type::InfinitePlane) s.strength = 1e-11;
    return s;
}

QStringList ElectrostaticScene::editableFields(int type) const
{
    switch (typeFromInt(type))
    {
    case Type::PointCharge:
    case Type::InfiniteLine:
    case Type::InfinitePlane:
        return {};
    case Type::SolidSphere:
    case Type::SphericalShell:
    case Type::InfiniteCylinderZ:
    case Type::CircularPlate:
        return {"radius"};
    case Type::ThickSphericalShell:
    case Type::InfiniteHollowCylinderZ:
    case Type::AnnularPlate:
        return {"radius", "innerRadius"};
    case Type::FiniteLine:
        return {"length", "angleDeg"};
    case Type::RectangularPlate:
    case Type::TriangularPlate:
        return {"width", "height", "angleDeg"};
    case Type::RectangularVolume:
        return {"width", "height", "thickness", "angleDeg"};
    }
    return {};
}

QString ElectrostaticScene::fieldLabel(int, const QString &key) const
{
    if (key == QLatin1String("radius")) return tr("Rayon R");
    if (key == QLatin1String("innerRadius")) return tr("Rayon intérieur");
    if (key == QLatin1String("length")) return tr("Longueur L");
    if (key == QLatin1String("width")) return tr("Largeur");
    if (key == QLatin1String("height")) return tr("Hauteur");
    if (key == QLatin1String("thickness")) return tr("Épaisseur");
    if (key == QLatin1String("angleDeg")) return tr("Rotation dans le plan xy");
    return key;
}

bool ElectrostaticScene::setField(Source &s, const QString &key, double v, bool &dimension) const
{
    const double positive = std::max(v, 1e-6);
    dimension = std::any_of(std::begin(kDimensions), std::end(kDimensions),
                            [&key](const char *k) { return key == QLatin1String(k); });
    if (key == QLatin1String("radius"))
        s.radius = std::max(positive, s.innerRadius * 1.000001);
    else if (key == QLatin1String("innerRadius"))
        s.innerRadius = std::clamp(v, 0.0, s.radius * 0.999999);
    else if (key == QLatin1String("length"))
        s.length = positive;
    else if (key == QLatin1String("width"))
        s.width = positive;
    else if (key == QLatin1String("height"))
        s.height = positive;
    else if (key == QLatin1String("thickness"))
        s.thickness = positive;
    else if (key == QLatin1String("angleDeg"))
        s.angleDeg = std::remainder(v, 360.0);
    else
        return false;
    return true;
}

void ElectrostaticScene::addFields(QVariantMap &map, const Source &s) const
{
    map.insert(QStringLiteral("radius"), s.radius);
    map.insert(QStringLiteral("innerRadius"), s.innerRadius);
    map.insert(QStringLiteral("length"), s.length);
    map.insert(QStringLiteral("width"), s.width);
    map.insert(QStringLiteral("height"), s.height);
    map.insert(QStringLiteral("thickness"), s.thickness);
    map.insert(QStringLiteral("angleDeg"), s.angleDeg);
}

void ElectrostaticScene::writeFields(QJsonObject &o, const Source &s) const
{
    o.insert("radius", s.radius);
    o.insert("innerRadius", s.innerRadius);
    o.insert("length", s.length);
    o.insert("width", s.width);
    o.insert("height", s.height);
    o.insert("thickness", s.thickness);
    o.insert("angleDeg", s.angleDeg);
}

void ElectrostaticScene::readFields(const QJsonObject &o, Source &s) const
{
    s.radius = o.value("radius").toDouble(s.radius);
    s.innerRadius = o.value("innerRadius").toDouble(s.innerRadius);
    s.length = o.value("length").toDouble(s.length);
    s.width = o.value("width").toDouble(s.width);
    s.height = o.value("height").toDouble(s.height);
    s.thickness = o.value("thickness").toDouble(s.thickness);
    s.angleDeg = o.value("angleDeg").toDouble(s.angleDeg);
}

// Charge density of a shape: strength = density x measure (volume, surface, length, or section of an
// infinite cylinder, whose strength is already a linear density).
DensityInfo ElectrostaticScene::densityInfo(Type t) const
{
    switch (t)
    {
    case Type::SolidSphere:
    case Type::ThickSphericalShell:
    case Type::RectangularVolume:
        return {QStringLiteral("ρ"), QStringLiteral("C/m³"), tr("volumique"), tr("Volume"), QStringLiteral("m³")};
    case Type::InfiniteCylinderZ:
    case Type::InfiniteHollowCylinderZ:
        return {QStringLiteral("ρ"), QStringLiteral("C/m³"), tr("volumique"), tr("Section"), QStringLiteral("m²")};
    case Type::SphericalShell:
    case Type::RectangularPlate:
    case Type::TriangularPlate:
    case Type::CircularPlate:
    case Type::AnnularPlate:
        return {QStringLiteral("σ"), QStringLiteral("C/m²"), tr("surfacique"), tr("Surface"), QStringLiteral("m²")};
    case Type::FiniteLine:
        return {QStringLiteral("λ"), QStringLiteral("C/m"), tr("linéique"), tr("Longueur"), QStringLiteral("m")};
    case Type::PointCharge:
    case Type::InfiniteLine:
    case Type::InfinitePlane:
        break;
    }
    return {};
}

// Same conventions as the physics kernels (hollow shapes: charge in the material only).
double ElectrostaticScene::measure(const Source &s) const
{
    const double pi = qsig::constants::pi;
    const double r = std::max(s.radius, 0.0);
    const double ri = std::clamp(s.innerRadius, 0.0, r);
    switch (s.type)
    {
    case Type::SolidSphere:
        return 4.0 / 3.0 * pi * r * r * r;
    case Type::ThickSphericalShell:
        return 4.0 / 3.0 * pi * (r * r * r - ri * ri * ri);
    case Type::SphericalShell:
        return 4.0 * pi * r * r;
    case Type::InfiniteCylinderZ:
    case Type::CircularPlate:
        return pi * r * r;
    case Type::InfiniteHollowCylinderZ:
    case Type::AnnularPlate:
        return pi * (r * r - ri * ri);
    case Type::FiniteLine:
        return s.length;
    case Type::RectangularPlate:
        return s.width * s.height;
    case Type::TriangularPlate:
        return 0.5 * s.width * s.height;
    case Type::RectangularVolume:
        return s.width * s.height * s.thickness;
    case Type::PointCharge:
    case Type::InfiniteLine:
    case Type::InfinitePlane:
        break;
    }
    return 0.0;
}

QString ElectrostaticScene::strengthSymbol(int type) const
{
    const Type t = typeFromInt(type);
    if (isLinearDensity(t)) return QStringLiteral("λ");
    if (t == Type::InfinitePlane) return QStringLiteral("σ");
    return QStringLiteral("Q");
}

QString ElectrostaticScene::strengthUnit(int type) const
{
    const Type t = typeFromInt(type);
    if (isLinearDensity(t)) return QStringLiteral("C/m");
    if (t == Type::InfinitePlane) return QStringLiteral("C/m²");
    return QStringLiteral("C");
}

QString ElectrostaticScene::strengthLabel(int type) const
{
    const Type t = typeFromInt(type);
    const QString what = isLinearDensity(t)         ? tr("densité linéique")
                         : t == Type::InfinitePlane ? tr("densité surfacique")
                                                    : tr("charge totale");
    return QStringLiteral("%1 — %2").arg(strengthSymbol(type), what);
}

QString ElectrostaticScene::formula(int type) const
{
    switch (typeFromInt(type))
    {
    case Type::PointCharge:
        return tr("E = Q / (4π ε₀ r²) · r̂        V = Q / (4π ε₀ r)");
    case Type::SolidSphere:
        return tr("r ≥ R : comme une charge ponctuelle.\n"
                  "r < R : E = Q r / (4π ε₀ R³),  V = Q (3 − r²/R²) / (8π ε₀ R)");
    case Type::SphericalShell:
        return tr("r > R : comme une charge ponctuelle.\nr < R : E = 0,  V = Q / (4π ε₀ R)");
    case Type::ThickSphericalShell:
        return tr("Superposition d'une sphère pleine et d'une cavité de densité opposée (théorème de Gauss).");
    case Type::InfiniteCylinderZ:
        return tr("ρ ≥ R : E = λ / (2π ε₀ ρ).\nρ < R : E = λ ρ / (2π ε₀ R²).\nV relatif : V = 0 à 1 m de l'axe.");
    case Type::InfiniteHollowCylinderZ:
        return tr("Théorème de Gauss : E = λ_enfermée / (2π ε₀ ρ).\nV relatif : V = 0 à 1 m de l'axe.");
    case Type::InfiniteLine:
        return tr("E = λ / (2π ε₀ ρ), radial.\nV = −λ / (2π ε₀) · ln(ρ / 1 m) : référence V = 0 à 1 m de l'axe.");
    case Type::FiniteLine:
        return tr("Intégrale exacte : composantes axiale et radiale fermées, "
                  "V = kλ [asinh(t₊/ρ) − asinh(t₋/ρ)].");
    case Type::InfinitePlane:
        return tr("E = σ / (2 ε₀), uniforme et normal au plan de chaque côté.\n"
                  "V = −σ |d| / (2 ε₀) : référence V = 0 sur le plan.");
    case Type::RectangularPlate:
    case Type::TriangularPlate:
        return tr("Forme fermée : composante normale = kσ × angle solide de la plaque ; composantes dans le plan "
                  "= kσ Σ ln((R₊+l₊)/(R₋+l₋)) sur les arêtes.");
    case Type::CircularPlate:
    case Type::AnnularPlate:
        return tr("Intégrales de contour exactes sur les bords circulaires (théorème de la divergence), "
                  "évaluées par quadrature adaptative.");
    case Type::RectangularVolume:
        return tr("Feuillets rectangulaires exacts intégrés sur l'épaisseur (quadrature adaptative).");
    }
    return {};
}

QString ElectrostaticScene::accuracy(int type) const
{
    switch (typeFromInt(type))
    {
    case Type::CircularPlate:
    case Type::AnnularPlate:
        return tr("Exacte à 10⁻¹⁰ près (quadrature contrôlée)");
    case Type::RectangularVolume:
        return tr("Exacte à 10⁻⁹ près (quadrature contrôlée)");
    default:
        return tr("Exacte (formule fermée)");
    }
}

QString ElectrostaticScene::quantityTitle(int quantity) const
{
    return quantity == 1 ? tr("Potentiel V") : tr("|E| (échelle log)");
}
QString ElectrostaticScene::quantityUnit(int quantity) const
{
    return quantity == 1 ? QStringLiteral("V") : QStringLiteral("V/m");
}
QString ElectrostaticScene::contourName(int quantity) const
{
    return quantity == 1 ? tr("Équipotentielles") : tr("Iso-|E|");
}

// 'plane': the view where the example reads best (0 XY, 1 XZ, 2 YZ).
QVariantList ElectrostaticScene::examples() const
{
    return {QVariantMap{{"key", "dipole"}, {"name", tr("Dipôle")}, {"plane", 0}},
            QVariantMap{{"key", "quadrupole"}, {"name", tr("Quadripôle")}, {"plane", 0}},
            QVariantMap{{"key", "capacitor"}, {"name", tr("Condensateur plan (coupe XZ)")}, {"plane", 1}},
            QVariantMap{{"key", "sphere_and_plate"}, {"name", tr("Sphère et plaque")}, {"plane", 0}}};
}

std::vector<Electrostatics::Source> ElectrostaticScene::example(const QString &key) const
{
    std::vector<Source> scene;
    auto add = [&scene](Type type, const char *label, Vec3 at, double q)
    {
        Source s;
        s.type = type;
        s.name = label;
        s.position = at;
        s.strength = q;
        scene.push_back(s);
        return scene.size() - 1;
    };
    if (key == QLatin1String("dipole"))
    {
        add(Type::PointCharge, "+q", {-0.3, 0.0, 0.0}, 1e-9);
        add(Type::PointCharge, "−q", {0.3, 0.0, 0.0}, -1e-9);
    }
    else if (key == QLatin1String("capacitor"))
    {
        for (const auto i : {add(Type::RectangularPlate, "Armature +", {0.0, 0.0, 0.1}, 1e-9),
                             add(Type::RectangularPlate, "Armature −", {0.0, 0.0, -0.1}, -1e-9)})
        {
            scene[i].width = 1.0;
            scene[i].height = 1.0;
        }
    }
    else if (key == QLatin1String("quadrupole"))
    {
        add(Type::PointCharge, "+q 1", {-0.3, -0.3, 0.0}, 1e-9);
        add(Type::PointCharge, "−q 1", {0.3, -0.3, 0.0}, -1e-9);
        add(Type::PointCharge, "+q 2", {0.3, 0.3, 0.0}, 1e-9);
        add(Type::PointCharge, "−q 2", {-0.3, 0.3, 0.0}, -1e-9);
    }
    else if (key == QLatin1String("sphere_and_plate"))
    {
        scene[add(Type::SolidSphere, "Sphère", {-0.4, 0.0, 0.0}, 2e-9)].radius = 0.2;
        const auto plate = add(Type::RectangularPlate, "Plaque", {0.4, 0.0, 0.0}, -1e-9);
        scene[plate].width = 0.1;
        scene[plate].height = 0.8;
    }
    return scene;
}

// ---- Field --------------------------------------------------------------------------------------

FieldSampler ElectrostaticScene::sampler(int, ViewPlane) const
{
    auto sources = std::make_shared<const std::vector<Source>>(m_sources);
    return [sources](const Vec3 &p)
    {
        const auto f = Electrostatics::fieldAt(*sources, p);
        FieldSample s;
        s.field = f.electricField;
        s.fieldValid = !f.singular;
        s.scalar = f.relativePotential;
        s.scalarValid = f.relativePotentialDefined;
        return s;
    };
}

bool ElectrostaticScene::scalarReferenced() const
{
    return std::any_of(m_sources.begin(), m_sources.end(),
                       [](const Source &s) { return Electrostatics::extendsToInfinity(s.type); });
}

QVariantMap ElectrostaticScene::probe(double x, double y, double z) const
{
    const auto f = Electrostatics::fieldAt(m_sources, {x, y, z});
    // The physics notes are English diagnostics; the interface explains the same cases in French.
    QString note;
    if (f.singular)
        note = tr("Le point de mesure est sur une charge : le champ y est singulier.");
    else if (f.note.find("on the charged sheet") != std::string::npos)
        note = tr("Le point est sur une plaque chargée : la composante normale y saute de σ/ε₀ et n'est pas "
                  "définie.");
    else if (f.potentialReferenced)
        note = tr("Potentiel relatif : une source infinie n'a pas de potentiel nul à l'infini. Référence : V = 0 à "
                  "1 m de l'axe d'un fil ou d'un cylindre infini, et sur un plan infini.");
    const double v = f.relativePotentialDefined ? f.relativePotential : std::nan("");
    const QVariantList rows{
        QVariantMap{{"name", "|E|"}, {"value", f.electricField.norm()}, {"unit", "V/m"}, {"strong", true}},
        QVariantMap{{"name", "E<sub>x</sub>"}, {"value", f.electricField.x}, {"unit", "V/m"}, {"strong", false}},
        QVariantMap{{"name", "E<sub>y</sub>"}, {"value", f.electricField.y}, {"unit", "V/m"}, {"strong", false}},
        QVariantMap{{"name", "E<sub>z</sub>"}, {"value", f.electricField.z}, {"unit", "V/m"}, {"strong", false}},
        QVariantMap{
            {"name", f.potentialReferenced ? "V<sub>rel</sub>" : "V"}, {"value", v}, {"unit", "V"}, {"strong", true}}};
    QVariantMap reading{{"rows", rows},
                        {"ex", f.electricField.x},
                        {"ey", f.electricField.y},
                        {"ez", f.electricField.z},
                        {"magnitude", f.electricField.norm()},
                        {"potential", f.relativePotential},
                        {"potentialDefined", f.relativePotentialDefined},
                        {"potentialReferenced", f.potentialReferenced},
                        {"singular", f.singular}};
    if (m_useB)
    {
        const Vec3 a{x, y, z}, b = m_probeB;
        const auto fb = Electrostatics::fieldAt(m_sources, b);
        const double undefined = std::nan(""); // a quantity that has no value here
        const double vb = fb.relativePotentialDefined ? fb.relativePotential : undefined;
        const double distance = qsig::norm(b - a);
        const double voltage = v - vb; // NaN when one of the potentials is undefined
        auto row = [](const QString &name, double value, const QString &unit, bool strong)
        { return QVariantMap{{"name", name}, {"value", value}, {"unit", unit}, {"strong", strong}}; };
        const QVariantList pointB{
            row("|E|", fb.electricField.norm(), "V/m", true), row("E<sub>x</sub>", fb.electricField.x, "V/m", false),
            row("E<sub>y</sub>", fb.electricField.y, "V/m", false), row("E<sub>z</sub>", fb.electricField.z, "V/m", false),
            row(fb.potentialReferenced ? "V<sub>rel</sub>" : "V", vb, "V", true)};
        const double ea = f.electricField.norm(), eb = fb.electricField.norm();
        const QVariantList difference{row("U<sub>AB</sub>", voltage, "V", true), row("d", distance, "m", false),
                                      row("Ē<sub>AB</sub>", distance > 0.0 ? voltage / distance : undefined, "V/m", false),
                                      row("E<sub>B</sub>/E<sub>A</sub>", ea > 0.0 ? eb / ea : undefined, QString(), false)};
        reading.insert("groups", QVariantList{QVariantMap{{"title", tr("Point B")}, {"rows", pointB}},
                                               QVariantMap{{"title", tr("De A vers B")}, {"rows", difference}}});
        // Profiles along the segment, A on the left.
        constexpr int samples = 97;
        QVariantList potential, magnitude;
        for (int i = 0; i < samples; ++i)
        {
            const auto fp = Electrostatics::fieldAt(m_sources, a + (b - a) * (static_cast<double>(i) / (samples - 1)));
            potential << (fp.relativePotentialDefined && !fp.singular ? fp.relativePotential : undefined);
            magnitude << (fp.singular ? undefined : fp.electricField.norm());
        }
        reading.insert("profiles", QVariantList{QVariantMap{{"name", "V"}, {"unit", "V"}, {"values", potential}},
                                                 QVariantMap{{"name", "|E|"}, {"unit", "V/m"}, {"values", magnitude}}});
        reading.insert("profileLength", distance);
        if (!note.isEmpty()) note += QLatin1Char('\n');
        note += tr("U_AB = V_A − V_B : travail du champ sur une charge de 1 C portée de A à B. Ē_AB = U_AB / d : "
                   "moyenne, le long de AB, de la composante du champ dirigée de A vers B.");
    }
    reading.insert("note", note);
    return reading;
}

void ElectrostaticScene::setProbePoints(QVector3D, bool useB, QVector3D b)
{
    m_useB = useB;
    m_probeB = {static_cast<double>(b.x()), static_cast<double>(b.y()), static_cast<double>(b.z())};
}

SourceShape3D ElectrostaticScene::shape3d(int row, const Vec3 &viewCentre, double extent) const
{
    return row >= 0 && row < count() ? sourceShape(at(row), viewCentre, extent) : SourceShape3D{};
}

double ElectrostaticScene::sourceReach(int row) const
{
    return row >= 0 && row < count() ? qsig::app::sourceReach(at(row)) : 0.0;
}

// ---- Drawing ------------------------------------------------------------------------------------

SourceOutline ElectrostaticScene::outline(int row, ViewPlane plane, double offset) const
{
    const Source &s = at(row);
    detail::OutlineBuilder b(plane, offset);
    SourceOutline &o = b.o;
    o.centre = b.project(s.position);
    o.sign = s.strength > 0.0 ? 1 : s.strength < 0.0 ? -1 : 0;
    const double dw = b.distance(s.position);
    const double distance = std::abs(dw);
    const double angle = qsig::constants::degToRad(s.angleDeg);
    const double ca = std::cos(angle), sa = std::sin(angle);
    auto rotated = [&](double x, double y, double z)
    { return Vec3{s.position.x + x * ca - y * sa, s.position.y + x * sa + y * ca, s.position.z + z}; };
    bool polygonal = false;
    switch (s.type)
    {
    case Type::PointCharge:
        o.marker = s.strength >= 0.0 ? SourceOutline::Marker::Plus : SourceOutline::Marker::Minus;
        o.gap = distance;
        break;
    case Type::SolidSphere:
    case Type::SphericalShell:
    case Type::ThickSphericalShell:
    {
        // Cross-section of the sphere by the view plane (silhouette when it does not cut it).
        const double r = distance < s.radius ? std::sqrt(s.radius * s.radius - dw * dw) : s.radius;
        o.filled << detail::OutlineBuilder::planeCircle(o.centre, r);
        o.gap = std::max(0.0, distance - s.radius);
        if (s.type == Type::ThickSphericalShell && distance < s.innerRadius)
            o.lines << detail::OutlineBuilder::planeCircle(o.centre,
                                                           std::sqrt(s.innerRadius * s.innerRadius - dw * dw));
        break;
    }
    case Type::InfiniteCylinderZ:
    case Type::InfiniteHollowCylinderZ:
    case Type::InfiniteLine:
    {
        const double radius = s.type == Type::InfiniteLine ? 0.0 : s.radius;
        if (plane == ViewPlane::XY)
        {
            if (radius > 0.0) o.filled << detail::OutlineBuilder::planeCircle(o.centre, radius);
            if (s.type == Type::InfiniteHollowCylinderZ)
                o.lines << detail::OutlineBuilder::planeCircle(o.centre, s.innerRadius);
        }
        else
        {
            // The axis (z) is vertical on the XZ and YZ planes: the plane cuts a band of the cylinder.
            o.infinite = true;
            const double half = distance < radius ? std::sqrt(radius * radius - dw * dw) : 0.0;
            o.bands << SourceOutline::Band{o.centre.x() - half, o.centre.x() + half, true, 0, 0};
            o.gap = std::max(0.0, distance - radius);
        }
        break;
    }
    case Type::FiniteLine:
        o.lines << QPolygonF{b.track(rotated(-0.5 * s.length, 0, 0)), b.track(rotated(0.5 * s.length, 0, 0))};
        polygonal = true;
        break;
    case Type::RectangularPlate:
    case Type::RectangularVolume:
    {
        const double a = 0.5 * s.width, h2 = 0.5 * s.height;
        const double h = s.type == Type::RectangularVolume ? 0.5 * s.thickness : 0.0;
        const std::pair<double, double> corners[] = {{-a, -h2}, {a, -h2}, {a, h2}, {-a, h2}, {-a, -h2}};
        for (double z : {-h, h})
        {
            QPolygonF face;
            for (const auto &[x, y] : corners) face << b.track(rotated(x, y, z));
            o.filled << face;
            if (h == 0.0) break;
        }
        if (h > 0.0)
            for (int i = 0; i < 4; ++i)
                o.lines << QPolygonF{b.project(rotated(corners[i].first, corners[i].second, -h)),
                                     b.project(rotated(corners[i].first, corners[i].second, h))};
        polygonal = true;
        break;
    }
    case Type::TriangularPlate:
    {
        QPolygonF tri;
        for (const auto &[x, y] :
             {std::pair{-0.5 * s.width, -s.height / 3.0}, std::pair{0.5 * s.width, -s.height / 3.0},
              std::pair{0.0, 2.0 * s.height / 3.0}, std::pair{-0.5 * s.width, -s.height / 3.0}})
            tri << b.track(rotated(x, y, 0.0));
        o.filled << tri;
        polygonal = true;
        break;
    }
    case Type::CircularPlate:
    case Type::AnnularPlate:
    {
        // The disk lies in z = z0: parallel to the XY view, edge-on in the others.
        const Vec3 ex{1.0, 0.0, 0.0}, ey{0.0, 1.0, 0.0};
        auto projected = [&b](const std::vector<Vec3> &points)
        {
            QPolygonF poly;
            for (const auto &p : points) poly << b.project(p);
            return poly;
        };
        o.filled << projected(detail::OutlineBuilder::circle(s.position, ex, ey, s.radius));
        if (s.type == Type::AnnularPlate)
            o.lines << projected(detail::OutlineBuilder::circle(s.position, ex, ey, s.innerRadius));
        o.gap = plane == ViewPlane::XY ? distance : std::max(0.0, distance - s.radius);
        break;
    }
    case Type::InfinitePlane:
        // Plane z = z0: a horizontal line on the XZ and YZ planes, parallel to the XY plane.
        if (plane != ViewPlane::XY)
        {
            o.infinite = true;
            o.bands << SourceOutline::Band{o.centre.y(), o.centre.y(), false, 0, 0};
        }
        else
        {
            o.gap = distance;
        }
        break;
    }
    if (polygonal) b.gapFromTrackedPoints();
    if (o.gap < 1e-9) o.gap = 0.0;
    return o;
}
} // namespace qsig::app
