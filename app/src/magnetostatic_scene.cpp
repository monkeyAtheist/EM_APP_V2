#include "qsig/app/magnetostatic_scene.h"

#include "outline_builder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>

namespace qsig::app
{
namespace
{
using Source = Magnetostatics::Source;
using Type = Source::Type;
using qsig::Vec3;
constexpr int kTypeCount = static_cast<int>(Type::InfiniteCurrentSheet) + 1;

struct TypeInfo
{
    const char *key;       // stable identifier in project files
    const char *name;      // French name shown in the interface
    const char *shortName; // prefix of automatic source names
};

// Indexed by Source::Type.
constexpr std::array<TypeInfo, kTypeCount> kTypes{{
    {"infinite_wire_z", QT_TRANSLATE_NOOP("Magnetostatics", "Fil infini (axe z)"), "Fil infini"},
    {"finite_wire", QT_TRANSLATE_NOOP("Magnetostatics", "Fil rectiligne fini"), "Fil"},
    {"circular_loop", QT_TRANSLATE_NOOP("Magnetostatics", "Spire circulaire"), "Spire"},
    {"solenoid", QT_TRANSLATE_NOOP("Magnetostatics", "Solénoïde"), "Solénoïde"},
    {"magnetic_dipole", QT_TRANSLATE_NOOP("Magnetostatics", "Dipôle magnétique"), "Dipôle"},
    {"solid_current_cylinder_z", QT_TRANSLATE_NOOP("Magnetostatics", "Conducteur cylindrique plein (axe z)"),
     "Conducteur"},
    {"hollow_current_cylinder_z", QT_TRANSLATE_NOOP("Magnetostatics", "Conducteur cylindrique creux (axe z)"), "Tube"},
    {"rectangular_loop", QT_TRANSLATE_NOOP("Magnetostatics", "Spire rectangulaire"), "Cadre"},
    {"triangular_loop", QT_TRANSLATE_NOOP("Magnetostatics", "Spire triangulaire"), "Triangle"},
    {"helmholtz_pair", QT_TRANSLATE_NOOP("Magnetostatics", "Bobines de Helmholtz"), "Helmholtz"},
    {"infinite_current_sheet", QT_TRANSLATE_NOOP("Magnetostatics", "Nappe de courant infinie (xy)"), "Nappe"},
}};

Type typeFromInt(int type)
{
    return static_cast<Type>(std::clamp(type, 0, kTypeCount - 1));
}
const TypeInfo &info(Type type)
{
    return kTypes[static_cast<std::size_t>(type)];
}

// Turns drawn for a solenoid: all of them up to the exact-loop limit of the physics, a sample above.
constexpr int kDrawnTurns = 24;
} // namespace

MagnetostaticScene::MagnetostaticScene(QObject *parent) : SourceListScene(parent) {}

QString MagnetostaticScene::title() const
{
    return tr("Magnétostatique");
}

// ---- Module description -------------------------------------------------------------------------

const char *MagnetostaticScene::typeKey(Type type) const
{
    return info(type).key;
}
QString MagnetostaticScene::shortName(Type type) const
{
    return QString::fromUtf8(info(type).shortName);
}

QStringList MagnetostaticScene::typeNames() const
{
    QStringList names;
    for (const auto &t : kTypes) names << QCoreApplication::translate("Magnetostatics", t.name);
    return names;
}

Magnetostatics::Source MagnetostaticScene::makeSource(Type type) const
{
    Source s;
    s.type = type;
    switch (type)
    {
    case Type::FiniteWire:
        s.length = 1.0;
        s.azimuthDeg = 90.0; // along y: lies in the XY view
        s.elevationDeg = 0.0;
        break;
    case Type::CircularLoop:
        s.radius = 0.3;
        break;
    case Type::Solenoid:
        s.radius = 0.2;
        s.length = 1.0;
        s.turns = 100;
        break;
    case Type::HelmholtzPair:
        s.radius = 0.3;
        s.length = 0.3;
        break;
    case Type::SolidCurrentCylinderZ:
        s.radius = 0.1;
        break;
    case Type::HollowCurrentCylinderZ:
        s.radius = 0.1;
        s.innerRadius = 0.06;
        break;
    case Type::RectangularLoop:
        s.width = 0.6;
        s.height = 0.4;
        break;
    case Type::TriangularLoop:
        s.width = 0.6;
        s.height = 0.5;
        break;
    case Type::InfiniteWireZ:
    case Type::MagneticDipole:
    case Type::InfiniteCurrentSheet:
        break;
    }
    return s;
}

QStringList MagnetostaticScene::editableFields(int type) const
{
    switch (typeFromInt(type))
    {
    case Type::InfiniteWireZ:
        return {};
    case Type::FiniteWire:
        return {"length", "azimuthDeg", "elevationDeg"};
    case Type::CircularLoop:
        return {"radius", "azimuthDeg", "elevationDeg"};
    case Type::Solenoid:
        return {"radius", "length", "turns", "azimuthDeg", "elevationDeg"};
    case Type::MagneticDipole:
        return {"azimuthDeg", "elevationDeg"};
    case Type::SolidCurrentCylinderZ:
        return {"radius"};
    case Type::HollowCurrentCylinderZ:
        return {"radius", "innerRadius"};
    case Type::RectangularLoop:
    case Type::TriangularLoop:
        return {"width", "height", "azimuthDeg", "elevationDeg"};
    case Type::HelmholtzPair:
        return {"radius", "length", "azimuthDeg", "elevationDeg"};
    case Type::InfiniteCurrentSheet:
        return {"azimuthDeg"};
    }
    return {};
}

QString MagnetostaticScene::fieldLabel(int type, const QString &key) const
{
    const Type t = typeFromInt(type);
    if (key == QLatin1String("radius")) return tr("Rayon R");
    if (key == QLatin1String("innerRadius")) return tr("Rayon intérieur");
    if (key == QLatin1String("length"))
        return t == Type::HelmholtzPair ? tr("Écartement des bobines") : tr("Longueur L");
    if (key == QLatin1String("width")) return tr("Largeur");
    if (key == QLatin1String("height")) return tr("Hauteur");
    if (key == QLatin1String("turns")) return tr("Nombre de spires N");
    if (key == QLatin1String("azimuthDeg"))
    {
        if (t == Type::InfiniteCurrentSheet) return tr("Direction du courant (azimut dans xy)");
        return t == Type::FiniteWire ? tr("Azimut du fil") : tr("Azimut de l'axe");
    }
    if (key == QLatin1String("elevationDeg"))
        return t == Type::FiniteWire ? tr("Élévation du fil") : tr("Élévation de l'axe");
    return key;
}

bool MagnetostaticScene::setField(Source &s, const QString &key, double v, bool &dimension) const
{
    const double positive = std::max(v, 1e-6);
    dimension = key == QLatin1String("radius") || key == QLatin1String("innerRadius");
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
    else if (key == QLatin1String("turns"))
        s.turns = static_cast<int>(std::clamp(std::round(v), 1.0, 100000.0));
    else if (key == QLatin1String("azimuthDeg"))
        s.azimuthDeg = std::remainder(v, 360.0);
    else if (key == QLatin1String("elevationDeg"))
        s.elevationDeg = std::clamp(v, -90.0, 90.0);
    else
        return false;
    return true;
}

void MagnetostaticScene::addFields(QVariantMap &map, const Source &s) const
{
    map.insert(QStringLiteral("radius"), s.radius);
    map.insert(QStringLiteral("innerRadius"), s.innerRadius);
    map.insert(QStringLiteral("length"), s.length);
    map.insert(QStringLiteral("width"), s.width);
    map.insert(QStringLiteral("height"), s.height);
    map.insert(QStringLiteral("turns"), s.turns);
    map.insert(QStringLiteral("azimuthDeg"), s.azimuthDeg);
    map.insert(QStringLiteral("elevationDeg"), s.elevationDeg);
}

void MagnetostaticScene::writeFields(QJsonObject &o, const Source &s) const
{
    o.insert("radius", s.radius);
    o.insert("innerRadius", s.innerRadius);
    o.insert("length", s.length);
    o.insert("width", s.width);
    o.insert("height", s.height);
    o.insert("turns", s.turns);
    o.insert("azimuthDeg", s.azimuthDeg);
    o.insert("elevationDeg", s.elevationDeg);
}

void MagnetostaticScene::readFields(const QJsonObject &o, Source &s) const
{
    s.radius = o.value("radius").toDouble(s.radius);
    s.innerRadius = o.value("innerRadius").toDouble(s.innerRadius);
    s.length = o.value("length").toDouble(s.length);
    s.width = o.value("width").toDouble(s.width);
    s.height = o.value("height").toDouble(s.height);
    s.turns = o.value("turns").toInt(s.turns);
    s.azimuthDeg = o.value("azimuthDeg").toDouble(s.azimuthDeg);
    s.elevationDeg = o.value("elevationDeg").toDouble(s.elevationDeg);
}

// Current density of a conductor: I = J x section.
DensityInfo MagnetostaticScene::densityInfo(Type t) const
{
    if (t == Type::SolidCurrentCylinderZ || t == Type::HollowCurrentCylinderZ)
        return {QStringLiteral("J"), QStringLiteral("A/m²"), tr("de courant"), tr("Section"), QStringLiteral("m²")};
    return {};
}

double MagnetostaticScene::measure(const Source &s) const
{
    const double pi = qsig::constants::pi;
    const double r = std::max(s.radius, 0.0);
    const double ri = std::clamp(s.innerRadius, 0.0, r);
    if (s.type == Type::SolidCurrentCylinderZ) return pi * r * r;
    if (s.type == Type::HollowCurrentCylinderZ) return pi * (r * r - ri * ri);
    return 0.0;
}

QString MagnetostaticScene::strengthSymbol(int type) const
{
    const Type t = typeFromInt(type);
    if (t == Type::MagneticDipole) return QStringLiteral("m");
    if (t == Type::InfiniteCurrentSheet) return QStringLiteral("K");
    return QStringLiteral("I");
}

QString MagnetostaticScene::strengthUnit(int type) const
{
    const Type t = typeFromInt(type);
    if (t == Type::MagneticDipole) return QStringLiteral("A·m²");
    if (t == Type::InfiniteCurrentSheet) return QStringLiteral("A/m");
    return QStringLiteral("A");
}

QString MagnetostaticScene::strengthLabel(int type) const
{
    switch (typeFromInt(type))
    {
    case Type::MagneticDipole:
        return tr("m — moment magnétique");
    case Type::InfiniteCurrentSheet:
        return tr("K — courant par mètre de largeur");
    case Type::Solenoid:
        return tr("I — courant dans chaque spire");
    case Type::HelmholtzPair:
        return tr("I — courant dans chaque bobine");
    default:
        return tr("I — courant");
    }
}

QString MagnetostaticScene::formula(int type) const
{
    switch (typeFromInt(type))
    {
    case Type::InfiniteWireZ:
        return tr("B = μ₀ I / (2π ρ), orthoradial (règle de la main droite).");
    case Type::FiniteWire:
        return tr("Biot et Savart intégré sur le segment : B = μ₀ I / (4π ρ) · (sin θ₂ − sin θ₁).\n"
                  "Un fil seul n'est pas un circuit fermé : à combiner avec d'autres pour un vrai circuit.");
    case Type::CircularLoop:
        return tr("Intégrales elliptiques complètes K et E.\nSur l'axe : B = μ₀ I R² / (2 (R² + z²)^3/2).");
    case Type::Solenoid:
        return tr("N > 24 : nappe de courant n = N / L (Derby et Olbert) ; N ≤ 24 : N spires exactes.\n"
                  "Au centre d'un solénoïde long : B ≈ μ₀ N I / L.");
    case Type::MagneticDipole:
        return tr("B = μ₀ / (4π r³) · (3 (m·r̂) r̂ − m).");
    case Type::SolidCurrentCylinderZ:
        return tr("Théorème d'Ampère : ρ ≥ R : B = μ₀ I / (2π ρ) ; ρ < R : B = μ₀ I ρ / (2π R²).");
    case Type::HollowCurrentCylinderZ:
        return tr("Théorème d'Ampère avec le courant enlacé ; B = 0 dans le creux.");
    case Type::RectangularLoop:
    case Type::TriangularLoop:
        return tr("Somme exacte des côtés rectilignes (Biot et Savart).");
    case Type::HelmholtzPair:
        return tr("Deux spires exactes. Écartement = R : champ uniforme au centre, B = (4/5)^3/2 μ₀ I / R.");
    case Type::InfiniteCurrentSheet:
        return tr("B = μ₀ K / 2, parallèle à la nappe et perpendiculaire au courant, de sens opposé de part "
                  "et d'autre.");
    }
    return {};
}

QString MagnetostaticScene::accuracy(int type) const
{
    switch (typeFromInt(type))
    {
    case Type::Solenoid:
        return tr("Exacte (nappe idéale ou spires)");
    case Type::MagneticDipole:
        return tr("Exacte (dipôle ponctuel)");
    default:
        return tr("Exacte (formule fermée)");
    }
}

QString MagnetostaticScene::quantityTitle(int quantity) const
{
    return quantity == 1 ? tr("B⊥ (vers vous : +)") : tr("|B| (échelle log)");
}
QString MagnetostaticScene::quantityUnit(int) const
{
    return QStringLiteral("T");
}
QString MagnetostaticScene::contourName(int quantity) const
{
    return quantity == 1 ? tr("Iso-B⊥") : tr("Iso-|B|");
}

// 'plane': the view where the example reads best (0 XY, 1 XZ, 2 YZ).
QVariantList MagnetostaticScene::examples() const
{
    return {QVariantMap{{"key", "wire"}, {"name", tr("Fil infini")}, {"plane", 0}},
            QVariantMap{{"key", "two_wires_opposite"}, {"name", tr("Deux fils, courants opposés")}, {"plane", 0}},
            QVariantMap{{"key", "two_wires_same"}, {"name", tr("Deux fils, même sens")}, {"plane", 0}},
            QVariantMap{{"key", "loop"}, {"name", tr("Spire circulaire (coupe XZ)")}, {"plane", 1}},
            QVariantMap{{"key", "helmholtz"}, {"name", tr("Bobines de Helmholtz (coupe XZ)")}, {"plane", 1}},
            QVariantMap{{"key", "solenoid"}, {"name", tr("Solénoïde (coupe XZ)")}, {"plane", 1}},
            QVariantMap{{"key", "dipole"}, {"name", tr("Dipôle magnétique (coupe XZ)")}, {"plane", 1}}};
}

std::vector<Magnetostatics::Source> MagnetostaticScene::example(const QString &key) const
{
    std::vector<Source> scene;
    auto add = [this, &scene](Type type, const char *label, Vec3 at, double strength)
    {
        Source s = makeSource(type);
        s.name = label;
        s.position = at;
        s.strength = strength;
        scene.push_back(s);
        return scene.size() - 1;
    };
    if (key == QLatin1String("wire"))
        add(Type::InfiniteWireZ, "I", {0.0, 0.0, 0.0}, 1.0);
    else if (key == QLatin1String("two_wires_opposite"))
    {
        add(Type::InfiniteWireZ, "Aller", {-0.3, 0.0, 0.0}, 1.0);
        add(Type::InfiniteWireZ, "Retour", {0.3, 0.0, 0.0}, -1.0);
    }
    else if (key == QLatin1String("two_wires_same"))
    {
        add(Type::InfiniteWireZ, "Fil 1", {-0.3, 0.0, 0.0}, 1.0);
        add(Type::InfiniteWireZ, "Fil 2", {0.3, 0.0, 0.0}, 1.0);
    }
    else if (key == QLatin1String("loop"))
        add(Type::CircularLoop, "Spire", {0.0, 0.0, 0.0}, 1.0);
    else if (key == QLatin1String("helmholtz"))
        add(Type::HelmholtzPair, "Helmholtz", {0.0, 0.0, 0.0}, 1.0);
    else if (key == QLatin1String("solenoid"))
        add(Type::Solenoid, "Solénoïde", {0.0, 0.0, 0.0}, 1.0);
    else if (key == QLatin1String("dipole"))
        add(Type::MagneticDipole, "m", {0.0, 0.0, 0.0}, 1.0);
    return scene;
}

// ---- Field --------------------------------------------------------------------------------------

FieldSampler MagnetostaticScene::sampler(int, ViewPlane plane) const
{
    auto sources = std::make_shared<const std::vector<Source>>(m_sources);
    const Vec3 out = outOfScreen(plane);
    return [sources, out](const Vec3 &p)
    {
        const auto f = Magnetostatics::fieldAt(*sources, p);
        FieldSample s;
        s.field = f.magneticFluxDensity;
        s.fieldValid = !f.singular;
        s.scalar = qsig::dot(f.magneticFluxDensity, out);
        s.scalarValid = !f.singular;
        return s;
    };
}

QVariantMap MagnetostaticScene::probe(double x, double y, double z) const
{
    const auto f = Magnetostatics::fieldAt(m_sources, {x, y, z});
    const Vec3 &b = f.magneticFluxDensity;
    const QString note =
        f.singular ? tr("Le point de mesure est sur un conducteur : le champ y est singulier.") : QString();
    const QVariantList rows{
        QVariantMap{{"name", "|B|"}, {"value", b.norm()}, {"unit", "T"}, {"strong", true}},
        QVariantMap{{"name", "B<sub>x</sub>"}, {"value", b.x}, {"unit", "T"}, {"strong", false}},
        QVariantMap{{"name", "B<sub>y</sub>"}, {"value", b.y}, {"unit", "T"}, {"strong", false}},
        QVariantMap{{"name", "B<sub>z</sub>"}, {"value", b.z}, {"unit", "T"}, {"strong", false}},
        QVariantMap{{"name", "|H|"}, {"value", f.magneticField.norm()}, {"unit", "A/m"}, {"strong", true}}};
    QVariantMap reading{{"rows", rows}, {"bx", b.x}, {"by", b.y}, {"bz", b.z}, {"magnitude", b.norm()}, {"singular", f.singular}};
    QString text = note;
    if (m_useB)
    {
        const Vec3 pa{x, y, z}, pb = m_probeB;
        const auto fb = Magnetostatics::fieldAt(m_sources, pb);
        const Vec3 &bb = fb.magneticFluxDensity;
        const double undefined = std::nan(""); // a quantity that has no value here
        const double na = b.norm(), nb = bb.norm();
        const double distance = qsig::norm(pb - pa);
        auto row = [](const QString &name, double value, const QString &unit, bool strong)
        { return QVariantMap{{"name", name}, {"value", value}, {"unit", unit}, {"strong", strong}}; };
        const QVariantList pointB{row("|B|", nb, "T", true), row("B<sub>x</sub>", bb.x, "T", false),
                                  row("B<sub>y</sub>", bb.y, "T", false), row("B<sub>z</sub>", bb.z, "T", false),
                                  row("|H|", fb.magneticField.norm(), "A/m", true)};
        const double angle =
            na > 0.0 && nb > 0.0 ? qsig::constants::radToDeg(std::acos(std::clamp(qsig::dot(b, bb) / (na * nb), -1.0, 1.0)))
                                 : undefined;
        const bool singular = f.singular || fb.singular;
        const QVariantList difference{
            row("Δ|B|", nb - na, "T", true), row(tr("Écart"), na > 0.0 ? 100.0 * (nb - na) / na : undefined, "%", true),
            row(tr("Angle"), angle, "°", false), row("d", distance, "m", false),
            row("∫H·dl", singular ? undefined : Magnetostatics::magnetomotiveForce(m_sources, pa, pb), "A", false)};
        reading.insert("groups", QVariantList{QVariantMap{{"title", tr("Point B")}, {"rows", pointB}},
                                               QVariantMap{{"title", tr("De A vers B")}, {"rows", difference}}});
        constexpr int samples = 97;
        QVariantList magnitude;
        for (int i = 0; i < samples; ++i)
        {
            const auto fp = Magnetostatics::fieldAt(m_sources, pa + (pb - pa) * (static_cast<double>(i) / (samples - 1)));
            magnitude << (fp.singular ? undefined : fp.magneticFluxDensity.norm());
        }
        reading.insert("profiles", QVariantList{QVariantMap{{"name", "|B|"}, {"unit", "T"}, {"values", magnitude}}});
        reading.insert("profileLength", distance);
        if (!text.isEmpty()) text += QLatin1Char('\n');
        text += tr("Écart : variation relative de |B| de A à B (uniformité du champ). Angle : entre les champs en A et "
                   "en B. ∫H·dl : circulation de H le long du segment AB ; sur un contour fermé elle vaut le courant "
                   "enlacé (théorème d'Ampère).");
    }
    reading.insert("note", text);
    return reading;
}

void MagnetostaticScene::setProbePoints(QVector3D, bool useB, QVector3D b)
{
    m_useB = useB;
    m_probeB = {static_cast<double>(b.x()), static_cast<double>(b.y()), static_cast<double>(b.z())};
}

SourceShape3D MagnetostaticScene::shape3d(int row, const Vec3 &viewCentre, double extent) const
{
    return row >= 0 && row < count() ? sourceShape(at(row), viewCentre, extent) : SourceShape3D{};
}

double MagnetostaticScene::sourceReach(int row) const
{
    return row >= 0 && row < count() ? qsig::app::sourceReach(at(row)) : 0.0;
}

// ---- Drawing ------------------------------------------------------------------------------------

SourceOutline MagnetostaticScene::outline(int row, ViewPlane plane, double offset) const
{
    using Marker = SourceOutline::Marker;
    const Source &s = at(row);
    detail::OutlineBuilder b(plane, offset);
    SourceOutline &o = b.o;
    o.centre = b.project(s.position);
    o.sign = s.strength > 0.0 ? 1 : s.strength < 0.0 ? -1 : 0;
    const double sense = s.strength >= 0.0 ? 1.0 : -1.0;
    const double dw = b.distance(s.position);
    const double distance = std::abs(dw);
    const Vec3 out = outOfScreen(plane);
    const Vec3 axis = Magnetostatics::axisUnit(s);
    Vec3 u, v;
    Magnetostatics::perpendicularBasis(axis, u, v);
    // Something seen end-on (a current, a moment): dot or cross depending on its direction.
    auto endOn = [&out](const Vec3 &direction)
    { return qsig::dot(direction, out) >= 0.0 ? Marker::CurrentOut : Marker::CurrentIn; };
    bool tracked = false;
    switch (s.type)
    {
    case Type::InfiniteWireZ:
    case Type::SolidCurrentCylinderZ:
    case Type::HollowCurrentCylinderZ:
    {
        const double radius = s.type == Type::InfiniteWireZ ? 0.0 : s.radius;
        const Vec3 current{0.0, 0.0, sense};
        if (plane == ViewPlane::XY)
        {
            o.marker = endOn(current);
            if (radius > 0.0) o.filled << detail::OutlineBuilder::planeCircle(o.centre, radius);
            if (s.type == Type::HollowCurrentCylinderZ)
                o.lines << detail::OutlineBuilder::planeCircle(o.centre, s.innerRadius);
        }
        else
        {
            // Axis along z, vertical on XZ and YZ: the current runs up (I > 0) or down the band.
            o.infinite = true;
            const double half = distance < radius ? std::sqrt(radius * radius - dw * dw) : 0.0;
            o.bands << SourceOutline::Band{o.centre.x() - half, o.centre.x() + half, true, static_cast<int>(sense), 0};
            o.gap = std::max(0.0, distance - radius);
        }
        break;
    }
    case Type::FiniteWire:
    {
        const double half = 0.5 * std::max(s.length, 1e-9);
        b.addCurrentPath({s.position - axis * half, s.position + axis * half}, s.strength, {0.5});
        if (std::abs(qsig::dot(axis, out)) > 0.95) o.marker = endOn(axis * sense);
        tracked = true;
        break;
    }
    case Type::CircularLoop:
        b.addCurrentPath(detail::OutlineBuilder::circle(s.position, u, v, s.radius), s.strength,
                         {0.125, 0.375, 0.625, 0.875});
        tracked = true;
        break;
    case Type::HelmholtzPair:
    {
        const double half = 0.5 * (s.length > 1e-9 ? s.length : s.radius);
        for (const double side : {-1.0, 1.0})
            b.addCurrentPath(detail::OutlineBuilder::circle(s.position + axis * (side * half), u, v, s.radius),
                             s.strength, {0.125, 0.625});
        tracked = true;
        break;
    }
    case Type::Solenoid:
    {
        const int turns = std::max(s.turns, 1);
        const int drawn = std::min(turns, kDrawnTurns);
        const double length = std::max(s.length, 1e-9);
        for (int i = 0; i < drawn; ++i)
        {
            const double along = turns <= kDrawnTurns ? (turns == 1 ? 0.0 : -0.5 * length + (i + 0.5) * length / turns)
                                                      : -0.5 * length + i * length / (drawn - 1);
            b.addCurrentPath(detail::OutlineBuilder::circle(s.position + axis * along, u, v, s.radius, 72), s.strength,
                             i == 0 ? std::initializer_list<double>{0.125, 0.625} : std::initializer_list<double>{});
        }
        tracked = true;
        break;
    }
    case Type::RectangularLoop:
    case Type::TriangularLoop:
    {
        const double w = std::max(s.width, 1e-9), h = std::max(s.height, 1e-9);
        const Vec3 &c = s.position;
        if (s.type == Type::RectangularLoop)
            b.addCurrentPath({c - u * (0.5 * w) - v * (0.5 * h), c + u * (0.5 * w) - v * (0.5 * h),
                              c + u * (0.5 * w) + v * (0.5 * h), c - u * (0.5 * w) + v * (0.5 * h),
                              c - u * (0.5 * w) - v * (0.5 * h)},
                             s.strength, {0.125, 0.375, 0.625, 0.875});
        else
            b.addCurrentPath({c - u * (0.5 * w) - v * (h / 3.0), c + u * (0.5 * w) - v * (h / 3.0),
                              c + v * (2.0 * h / 3.0), c - u * (0.5 * w) - v * (h / 3.0)},
                             s.strength, {1.0 / 6.0, 0.5, 5.0 / 6.0});
        tracked = true;
        break;
    }
    case Type::MagneticDipole:
    {
        const Vec3 moment = axis * sense;
        const QPointF direction = b.project(s.position + moment) - o.centre;
        if (std::hypot(direction.x(), direction.y()) > 0.3)
            o.arrows << SourceOutline::Arrow{o.centre, direction, true};
        else
            o.marker = endOn(moment);
        o.gap = distance;
        break;
    }
    case Type::InfiniteCurrentSheet:
    {
        const double az = qsig::constants::degToRad(s.azimuthDeg);
        const Vec3 k{std::cos(az) * sense, std::sin(az) * sense, 0.0};
        const QPointF along = b.project(s.position + k) - o.centre;
        if (plane == ViewPlane::XY)
        {
            o.arrows << SourceOutline::Arrow{o.centre, along, true};
            o.gap = distance;
        }
        else
        {
            // Plane z = z0: a horizontal line on XZ and YZ, with the current along it or through it.
            o.infinite = true;
            const double toward = qsig::dot(k, out);
            o.bands << SourceOutline::Band{o.centre.y(), o.centre.y(), false,
                                           std::abs(along.x()) > 0.3 ? (along.x() > 0.0 ? 1 : -1) : 0,
                                           std::abs(toward) > 0.3 ? (toward > 0.0 ? 1 : -1) : 0};
        }
        break;
    }
    }
    if (tracked) b.gapFromTrackedPoints();
    if (o.gap < 1e-9) o.gap = 0.0;
    return o;
}
} // namespace qsig::app
