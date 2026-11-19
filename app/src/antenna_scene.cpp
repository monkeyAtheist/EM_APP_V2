#include "qsig/app/antenna_scene.h"

#include "qsig/app/units.h"
#include "qsig/constants.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonObject>
#include <QtConcurrent/QtConcurrentMap>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <array>
#include <cmath>

namespace qsig::app
{
namespace
{
using Type = AntennaElement::Type;
constexpr int kTypeCount = static_cast<int>(Type::Spiral) + 1;
constexpr double kPi = qsig::constants::pi;

struct TypeInfo
{
    const char *key;       // stable identifier in project files
    const char *name;      // French name shown in the interface
    const char *shortName; // prefix of automatic names
};

// Indexed by AntennaElement::Type.
constexpr std::array<TypeInfo, kTypeCount> kTypes{{
    {"wire", QT_TRANSLATE_NOOP("Antennas", "Fil droit"), "Fil"},
    {"dipole", QT_TRANSLATE_NOOP("Antennas", "Dipôle"), "Dipôle"},
    {"folded_dipole", QT_TRANSLATE_NOOP("Antennas", "Dipôle replié"), "Replié"},
    {"circular_loop", QT_TRANSLATE_NOOP("Antennas", "Boucle circulaire"), "Boucle"},
    {"rectangular_loop", QT_TRANSLATE_NOOP("Antennas", "Cadre rectangulaire"), "Cadre"},
    {"helix", QT_TRANSLATE_NOOP("Antennas", "Hélice"), "Hélice"},
    {"plate", QT_TRANSLATE_NOOP("Antennas", "Plaque conductrice"), "Plaque"},
    {"disc", QT_TRANSLATE_NOOP("Antennas", "Disque conducteur"), "Disque"},
    {"box", QT_TRANSLATE_NOOP("Antennas", "Pavé conducteur"), "Pavé"},
    {"cylinder", QT_TRANSLATE_NOOP("Antennas", "Cylindre conducteur"), "Cylindre"},
    {"cone", QT_TRANSLATE_NOOP("Antennas", "Cône conducteur"), "Cône"},
    {"sphere", QT_TRANSLATE_NOOP("Antennas", "Sphère conductrice"), "Sphère"},
    {"paraboloid", QT_TRANSLATE_NOOP("Antennas", "Parabole (réflecteur)"), "Parabole"},
    {"parabolic_cylinder", QT_TRANSLATE_NOOP("Antennas", "Cylindre parabolique"), "Cylindre parabolique"},
    {"arc", QT_TRANSLATE_NOOP("Antennas", "Arc de fil"), "Arc"},
    {"spiral", QT_TRANSLATE_NOOP("Antennas", "Spirale à deux bras"), "Spirale"},
}};

Type typeFromInt(int type)
{
    return static_cast<Type>(std::clamp(type, 0, kTypeCount - 1));
}
const TypeInfo &info(Type type)
{
    return kTypes[static_cast<std::size_t>(type)];
}

QString number(double v, int decimals)
{
    return QString::number(v, 'f', decimals).replace(QLatin1Char('.'), QLatin1Char(','));
}

// Every number of an element, in files and in the inspector map.
struct NumberField
{
    const char *key;
    double AntennaElement::*member;
};
constexpr std::array<NumberField, 18> kNumbers{{
    {"length", &AntennaElement::length},
    {"width", &AntennaElement::width},
    {"radius", &AntennaElement::radius},
    {"height", &AntennaElement::height},
    {"permittivity", &AntennaElement::permittivity},
    {"lossTangent", &AntennaElement::lossTangent},
    {"focalLength", &AntennaElement::focalLength},
    {"sweepDeg", &AntennaElement::sweepDeg},
    {"innerRadius", &AntennaElement::innerRadius},
    {"apexDeg", &AntennaElement::apexDeg},
    {"pitch", &AntennaElement::pitch},
    {"turns", &AntennaElement::turns},
    {"wireRadius", &AntennaElement::wireRadius},
    {"azimuthDeg", &AntennaElement::azimuthDeg},
    {"elevationDeg", &AntennaElement::elevationDeg},
    {"rollDeg", &AntennaElement::rollDeg},
    {"phaseDeg", &AntennaElement::phaseDeg},
    {"feedFraction", &AntennaElement::feedFraction},
}};

// Settings changes share the undo stack of the elements.
class SettingsCommand final : public QUndoCommand
{
public:
    SettingsCommand(AntennaScene *scene, AntennaSettings before, AntennaSettings after, const QString &text)
        : QUndoCommand(text), m_scene(scene), m_before(before), m_after(after)
    {
    }
    void undo() override { m_scene->applySettings(m_before); }
    void redo() override { m_scene->applySettings(m_after); }

private:
    AntennaScene *m_scene;
    AntennaSettings m_before, m_after;
};

QString complexText(std::complex<double> z, const QString &unit)
{
    const QString sign = z.imag() < 0.0 ? QStringLiteral(" − j") : QStringLiteral(" + j");
    return Units::formatQuantity(z.real(), unit, 4) + sign + Units::formatQuantity(std::abs(z.imag()), unit, 4);
}

// Two significant digits, French decimal comma ("0,0036", "1,1e-11").
QString significant(double v, int digits = 2)
{
    QString s = QString::number(v, 'g', digits);
    s.replace(QLatin1Char('.'), QLatin1Char(','));
    s.replace(QLatin1Char('-'), QStringLiteral("−"));
    return s;
}

// A value of the bench in its unit ("λ" is a length in wavelengths: no prefix).
QString benchValue(double v, const std::string &unit)
{
    const QString u = QString::fromStdString(unit);
    if (u == QStringLiteral("λ")) return significant(v, 4) + QStringLiteral(" λ");
    return Units::formatQuantity(v, u, 5);
}

// A deviation or a threshold: % of the reference, or absolute (dB for ratios in dB).
QString benchDeviation(double v, bool relative, const std::string &unit)
{
    if (!std::isfinite(v)) return QStringLiteral("—");
    if (relative) return significant(v) + QStringLiteral(" %");
    const QString u = QString::fromStdString(unit);
    if (u.startsWith(QStringLiteral("dB"))) return significant(v) + QStringLiteral(" dB");
    return Units::formatQuantity(v, u, 3);
}

QString pointText(const SweepPoint &p)
{
    if (!p.valid) return QCoreApplication::translate("Antennas", "pas de solution");
    return QCoreApplication::translate("Antennas", "G %1 · Av/Ar %2 · TOS %3")
        .arg(Units::formatQuantity(p.gainDbi, QStringLiteral("dBi")), Units::formatQuantity(p.frontToBackDb, QStringLiteral("dB")),
             significant(p.vswr, 3));
}
} // namespace

AntennaScene::AntennaScene(QObject *parent) : SourceListScene(parent)
{
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(120);
    connect(&m_debounce, &QTimer::timeout, this, &AntennaScene::solve);
    connect(&m_watcher, &QFutureWatcher<std::shared_ptr<const AntennaResult>>::finished, this, &AntennaScene::onSolved);
    connect(&m_sweepWatcher, &QFutureWatcher<SweepPoint>::finished, this, &AntennaScene::onSweepFinished);
    connect(&m_optimiseWatcher, &QFutureWatcher<YagiOptimizerResult>::finished, this, &AntennaScene::onOptimised);
    connect(&m_benchWatcher, &QFutureWatcher<AntennaBenchReport>::finished, this, &AntennaScene::onBenchFinished);
    connect(&m_sweepWatcher, &QFutureWatcher<SweepPoint>::progressValueChanged, this,
            [this](int value)
            {
                const int maximum = std::max(1, m_sweepWatcher.progressMaximum());
                m_sweepProgress = double(value) / double(maximum);
                emit sweepProgressChanged();
            });
    connect(this, &SceneBase::sceneChanged, this,
            [this]
            {
                updateSweepStale();
                m_debounce.start();
            });
}

AntennaScene::~AntennaScene()
{
    m_debounce.stop();
    m_sweepWatcher.cancel();
    m_cancelOptimise = true;
    m_sweepWatcher.waitForFinished();
    m_optimiseWatcher.waitForFinished();
    m_benchWatcher.waitForFinished();
    m_watcher.waitForFinished();
}

QString AntennaScene::title() const
{
    return tr("Antennes");
}

// ---- Description of the elements ----

QStringList AntennaScene::typeNames() const
{
    QStringList names;
    for (const auto &t : kTypes) names << QCoreApplication::translate("Antennas", t.name);
    return names;
}

const char *AntennaScene::typeKey(Type type) const
{
    return info(type).key;
}

QString AntennaScene::shortName(Type type) const
{
    return QString::fromUtf8(info(type).shortName);
}

AntennaElement AntennaScene::makeSource(Type type) const
{
    // Dimensioned for the current frequency, as the presets.
    const double lambda = qsig::constants::c0 / std::max(1.0, m_settings.frequency);
    AntennaElement e;
    e.type = type;
    e.wireRadius = 1e-3 * lambda;
    e.length = 0.475 * lambda;
    switch (type)
    {
    case Type::Wire:
        e.fed = 0; // a parasitic element by default
        break;
    case Type::Dipole:
        break;
    case Type::FoldedDipole:
        e.length = 0.47 * lambda;
        e.width = 0.02 * lambda;
        break;
    case Type::CircularLoop:
        e.radius = 1.07 * lambda / (2.0 * kPi);
        break;
    case Type::RectangularLoop:
        e.length = e.width = 0.275 * lambda;
        break;
    case Type::Helix:
        e.radius = 0.02 * lambda;
        e.pitch = 0.02 * lambda;
        e.turns = 4.28; // first resonance, as the preset
        e.sides = 16;
        e.wireRadius = 5e-4 * lambda;
        break;
    case Type::Plate:
        // A horizontal ground plane of one wavelength.
        e.fed = 0;
        e.length = e.width = lambda;
        break;
    case Type::Disc:
        e.fed = 0;
        e.radius = 0.5 * lambda;
        break;
    // Conducting solids, of a size that a monopole of a quarter wave can stand on.
    case Type::Box:
        e.fed = 0;
        e.length = e.width = 0.4 * lambda;
        e.height = 0.2 * lambda;
        break;
    case Type::Cylinder:
        e.fed = 0;
        e.radius = 0.15 * lambda;
        e.height = 0.3 * lambda;
        break;
    case Type::Cone:
        e.fed = 0;
        e.radius = 0.2 * lambda;
        e.height = 0.3 * lambda;
        break;
    case Type::Sphere:
        e.fed = 0;
        e.radius = 0.2 * lambda;
        break;
    // Reflectors, to put a dipole at their focus; curved wires.
    case Type::Paraboloid:
        e.fed = 0;
        e.radius = lambda;
        e.focalLength = 0.8 * lambda;
        break;
    case Type::ParabolicCylinder:
        e.fed = 0;
        e.width = 2.0 * lambda;
        e.length = lambda;
        e.focalLength = 0.5 * lambda;
        break;
    case Type::Arc:
        e.fed = 0; // a shape: passive until a source is placed on it
        e.radius = 0.25 * lambda;
        e.sweepDeg = 180.0;
        e.sides = 48;
        break;
    case Type::Spiral:
        e.radius = 0.25 * lambda;
        e.innerRadius = 0.02 * lambda;
        e.turns = 4.0;
        e.sides = 48;
        e.wireRadius = 5e-4 * lambda;
        break;
    }
    return e;
}

QStringList AntennaScene::editableFields(int typeValue) const
{
    const QStringList orientation{"azimuthDeg", "elevationDeg"};
    // The sources are entities of their own (feedList, setFeedValue): an element shows its geometry.
    const QStringList source{"wireRadius"};
    switch (typeFromInt(typeValue))
    {
    case Type::Wire:
        return QStringList{"length"} + orientation + source;
    case Type::Dipole:
        return QStringList{"length", "apexDeg"} + orientation + QStringList{"rollDeg"} + source;
    case Type::FoldedDipole:
        return QStringList{"length", "width"} + orientation + QStringList{"rollDeg"} + source;
    case Type::CircularLoop:
        return QStringList{"radius", "sides"} + orientation + QStringList{"rollDeg"} + source;
    case Type::RectangularLoop:
        return QStringList{"width", "length"} + orientation + QStringList{"rollDeg"} + source;
    case Type::Helix:
        return QStringList{"radius", "pitch", "turns", "sides"} + orientation + source;
    case Type::Plate:
        return QStringList{"width", "length"} + orientation + QStringList{"rollDeg"};
    case Type::Disc:
        return QStringList{"radius"} + orientation;
    // A solid is made of a medium: the perfect conductor, or a dielectric with its permittivity and its losses.
    case Type::Box:
        return QStringList{"width", "length", "height"} + orientation + QStringList{"rollDeg", "medium", "permittivity", "lossTangent"};
    case Type::Cylinder:
    case Type::Cone:
        return QStringList{"radius", "height"} + orientation + QStringList{"medium", "permittivity", "lossTangent"};
    case Type::Sphere:
        return QStringList{"radius", "medium", "permittivity", "lossTangent"};
    case Type::Paraboloid:
        return QStringList{"radius", "focalLength"} + orientation;
    case Type::ParabolicCylinder:
        return QStringList{"width", "length", "focalLength"} + orientation + QStringList{"rollDeg"};
    case Type::Arc:
        return QStringList{"radius", "sweepDeg", "sides"} + orientation + QStringList{"rollDeg"} + source;
    case Type::Spiral:
        return QStringList{"radius", "innerRadius", "turns", "sides"} + orientation + QStringList{"rollDeg"} + source;
    }
    return {};
}

QString AntennaScene::fieldLabel(int typeValue, const QString &key) const
{
    const Type type = typeFromInt(typeValue);
    // A solid is oriented by its axis, as a loop; a plate or a disc by its normal.
    const bool solid = elementIsSolid(type);
    // Reflectors open along their axis; arcs and spirals lie across it.
    const bool opening = type == Type::Paraboloid || type == Type::ParabolicCylinder;
    const bool loop = type == Type::CircularLoop || type == Type::RectangularLoop || type == Type::Helix || type == Type::Arc ||
                      type == Type::Spiral || solid || opening;
    const bool surface = elementIsSurface(type) && !solid && !opening;
    if (key == QLatin1String("focalLength")) return tr("f — distance focale (du sommet au foyer)");
    if (key == QLatin1String("sweepDeg")) return tr("Angle couvert par l'arc");
    if (key == QLatin1String("innerRadius")) return tr("Rayon intérieur (départ des bras)");
    if (key == QLatin1String("radius") && type == Type::Paraboloid) return tr("Rayon de l'ouverture (D / 2)");
    if (key == QLatin1String("radius") && type == Type::Spiral) return tr("Rayon extérieur");
    if (key == QLatin1String("width") && type == Type::ParabolicCylinder) return tr("Largeur de l'ouverture");
    if (key == QLatin1String("length") && type == Type::ParabolicCylinder) return tr("Longueur (le long de la ligne focale)");
    if (key == QLatin1String("turns") && type == Type::Spiral) return tr("Nombre de tours de chaque bras");
    if (key == QLatin1String("sides") && type == Type::Arc) return tr("Segments par tour complet");
    if (key == QLatin1String("sides") && type == Type::Spiral) return tr("Segments par tour (au bord)");
    if ((key == QLatin1String("azimuthDeg") || key == QLatin1String("elevationDeg")) && opening)
        return key == QLatin1String("azimuthDeg") ? tr("Ouverture : azimut (depuis x)") : tr("Ouverture : élévation (90° : tournée vers le haut)");
    if (key == QLatin1String("height")) return type == Type::Cone ? tr("Hauteur (de la base à la pointe)") : tr("Hauteur (le long de l'axe)");
    if (key == QLatin1String("medium")) return tr("Milieu");
    if (key == QLatin1String("permittivity")) return tr("εr — permittivité relative (diélectrique)");
    if (key == QLatin1String("lossTangent")) return tr("tan δ — pertes (diélectrique)");
    if (key == QLatin1String("length"))
        return type == Type::RectangularLoop ? tr("Hauteur")
               : type == Type::Wire || surface || solid ? tr("Longueur")
                                               : tr("Longueur totale");
    if (key == QLatin1String("width")) return type == Type::FoldedDipole ? tr("Écartement des deux fils") : tr("Largeur");
    if (key == QLatin1String("radius")) return tr("Rayon");
    if (key == QLatin1String("apexDeg")) return tr("Angle entre les bras (180° : droit, moins : en V)");
    if (key == QLatin1String("pitch")) return tr("Pas (avance par tour)");
    if (key == QLatin1String("turns")) return tr("Nombre de tours");
    if (key == QLatin1String("sides"))
        return type == Type::Helix ? tr("Segments par tour") : tr("Côtés du polygone");
    if (key == QLatin1String("wireRadius")) return tr("a — rayon du fil");
    if (key == QLatin1String("azimuthDeg"))
        return surface ? tr("Normale : azimut (depuis x)") : loop ? tr("Axe : azimut (depuis x)") : tr("Direction : azimut (depuis x)");
    if (key == QLatin1String("elevationDeg"))
        return surface ? tr("Normale : élévation (90° : surface horizontale)")
               : loop  ? tr("Axe : élévation (90° : vertical)")
                       : tr("Direction : élévation (90° : vertical)");
    if (key == QLatin1String("rollDeg")) return surface ? tr("Rotation dans son plan") : tr("Rotation autour de l'axe");
    if (key == QLatin1String("fed")) return tr("Rôle");
    if (key == QLatin1String("voltage")) return tr("V — tension de la source");
    if (key == QLatin1String("phaseDeg")) return tr("φ — phase de la source");
    if (key == QLatin1String("feedFraction"))
        return type == Type::Helix ? tr("Position de la source le long du fil (0 : au pied, sur un plan de masse ; 0,5 : au milieu)")
                                   : tr("Position de la source le long du fil (0 à 1 ; 0 ou 1 : au bout, posé sur une surface)");
    return key;
}

QString AntennaScene::fieldUnit(int, const QString &key) const
{
    if (key.endsWith(QLatin1String("Deg"))) return QStringLiteral("°");
    if (key == QLatin1String("voltage")) return QStringLiteral("V");
    if (key == QLatin1String("turns") || key == QLatin1String("sides") || key == QLatin1String("fed") ||
        key == QLatin1String("feedFraction") || key == QLatin1String("medium") || key == QLatin1String("permittivity") ||
        key == QLatin1String("lossTangent"))
        return {};
    return QStringLiteral("m");
}

QStringList AntennaScene::fieldChoices(int, const QString &key) const
{
    if (key == QLatin1String("fed")) return {tr("Passif (élément parasite)"), tr("Alimenté (source)")};
    if (key == QLatin1String("medium")) return {tr("Conducteur parfait"), tr("Diélectrique")};
    return {};
}

namespace
{
// What a solid is made of, in a few words.
QString mediumText(const AntennaElement &s)
{
    if (!elementIsDielectric(s)) return QCoreApplication::translate("Antennas", "conducteur");
    return QCoreApplication::translate("Antennas", "diélectrique εr %1").arg(number(s.permittivity, 2));
}
} // namespace

QString AntennaScene::summary(const AntennaElement &s) const
{
    const double lambda = qsig::constants::c0 / std::max(1.0, m_settings.frequency);
    QString size;
    switch (s.type)
    {
    case Type::CircularLoop:
        size = tr("C = %1 λ").arg(number(2.0 * kPi * s.radius / lambda, 3));
        break;
    case Type::RectangularLoop:
        size = tr("P = %1 λ").arg(number(2.0 * (s.width + s.length) / lambda, 3));
        break;
    case Type::Helix:
        size = tr("%1 tours").arg(number(s.turns, 1));
        break;
    case Type::Plate:
        return tr("%1 × %2 λ · surface").arg(number(s.width / lambda, 2), number(s.length / lambda, 2));
    case Type::Disc:
        return tr("r = %1 λ · surface").arg(number(s.radius / lambda, 3));
    case Type::Box:
        return tr("%1 × %2 × %3 λ · %4").arg(number(s.width / lambda, 2), number(s.length / lambda, 2), number(s.height / lambda, 2), mediumText(s));
    case Type::Cylinder:
    case Type::Cone:
        return tr("r = %1 λ, h = %2 λ · %3").arg(number(s.radius / lambda, 3), number(s.height / lambda, 3), mediumText(s));
    case Type::Sphere:
        return tr("r = %1 λ · %2").arg(number(s.radius / lambda, 3), mediumText(s));
    case Type::Paraboloid:
        return tr("D = %1 λ, f/D = %2 · surface").arg(number(2.0 * s.radius / lambda, 2), number(s.focalLength / std::max(1e-12, 2.0 * s.radius), 2));
    case Type::ParabolicCylinder:
        return tr("%1 × %2 λ, f = %3 λ · surface").arg(number(s.width / lambda, 2), number(s.length / lambda, 2), number(s.focalLength / lambda, 3));
    case Type::Arc:
        size = tr("r = %1 λ, %2° · L = %3 λ").arg(number(s.radius / lambda, 3), number(s.sweepDeg, 0), number(s.radius * s.sweepDeg * kPi / 180.0 / lambda, 3));
        break;
    case Type::Spiral:
        size = tr("%1 tours, Ø %2 λ").arg(number(s.turns, 1), number(2.0 * s.radius / lambda, 2));
        break;
    default:
        size = tr("%1 λ").arg(number(s.length / lambda, 3));
        break;
    }
    const int feeds = qsig::app::feedCount(s);
    return feeds == 0 ? size : feeds == 1 ? size + tr(" · 1 source") : size + tr(" · %1 sources").arg(feeds);
}

bool AntennaScene::setField(AntennaElement &s, const QString &key, double v, bool &dimension) const
{
    dimension = false;
    auto positive = [&](double &member)
    {
        if (!(v > 0.0)) return false;
        member = v;
        return true;
    };
    if (key == QLatin1String("length")) return positive(s.length);
    if (key == QLatin1String("width")) return positive(s.width);
    if (key == QLatin1String("radius")) return positive(s.radius);
    if (key == QLatin1String("height")) return positive(s.height);
    if (key == QLatin1String("focalLength")) return positive(s.focalLength);
    if (key == QLatin1String("innerRadius")) return positive(s.innerRadius);
    if (key == QLatin1String("sweepDeg"))
    {
        s.sweepDeg = std::clamp(v, 1.0, 359.0);
        return true;
    }
    if (key == QLatin1String("medium"))
    {
        s.medium = std::clamp(static_cast<int>(std::lround(v)), 0, 1);
        return true;
    }
    if (key == QLatin1String("permittivity"))
    {
        if (!(v >= 1.0) || v > 1000.0) return false; // a dielectric: no less than the vacuum
        s.permittivity = v;
        return true;
    }
    if (key == QLatin1String("lossTangent"))
    {
        if (v < 0.0 || v > 10.0) return false;
        s.lossTangent = v;
        return true;
    }
    if (key == QLatin1String("pitch"))
    {
        if (v < 0.0) return false;
        s.pitch = v;
        return true;
    }
    if (key == QLatin1String("turns"))
    {
        s.turns = std::clamp(v, 0.25, 60.0);
        return true;
    }
    if (key == QLatin1String("sides"))
    {
        s.sides = std::clamp(static_cast<int>(std::lround(v)), s.type == Type::Helix ? 6 : 3, 200);
        return true;
    }
    if (key == QLatin1String("apexDeg"))
    {
        s.apexDeg = std::clamp(v, 10.0, 180.0);
        return true;
    }
    if (key == QLatin1String("wireRadius")) return positive(s.wireRadius);
    if (key == QLatin1String("fed"))
    {
        s.fed = std::clamp(static_cast<int>(std::lround(v)), 0, 1);
        return true;
    }
    if (key == QLatin1String("voltage"))
    {
        s.strength = v;
        return true;
    }
    if (key == QLatin1String("feedFraction"))
    {
        // 0 and 1: at the end itself, for a wire that stands on a surface.
        s.feedFraction = v < 0.02 ? 0.0 : v > 0.98 ? 1.0 : v;
        return true;
    }
    if (key == QLatin1String("azimuthDeg") || key == QLatin1String("elevationDeg") || key == QLatin1String("rollDeg") ||
        key == QLatin1String("phaseDeg"))
    {
        for (const auto &f : kNumbers)
            if (key == QLatin1String(f.key)) s.*(f.member) = v;
        if (key == QLatin1String("elevationDeg")) s.elevationDeg = std::clamp(v, -90.0, 90.0);
        return true;
    }
    return false;
}

void AntennaScene::addFields(QVariantMap &map, const AntennaElement &s) const
{
    for (const auto &f : kNumbers) map.insert(QString::fromLatin1(f.key), s.*(f.member));
    map.insert(QStringLiteral("sides"), s.sides);
    map.insert(QStringLiteral("fed"), s.fed);
    map.insert(QStringLiteral("medium"), s.medium);
    map.insert(QStringLiteral("voltage"), s.strength);
}

void AntennaScene::writeFields(QJsonObject &o, const AntennaElement &s) const
{
    for (const auto &f : kNumbers) o.insert(QString::fromLatin1(f.key), s.*(f.member));
    o.insert(QStringLiteral("sides"), s.sides);
    o.insert(QStringLiteral("medium"), s.medium);
    o.insert(QStringLiteral("fed"), s.fed);
    // Further sources of a plain wire: only when it has some, so that the other files stay as they were.
    if (!s.moreFeeds.empty())
    {
        QJsonArray feeds;
        for (const AntennaFeed &feed : s.moreFeeds)
            feeds.append(QJsonObject{{"name", QString::fromStdString(feed.name)},
                                     {"fraction", feed.fraction},
                                     {"voltage", feed.strength},
                                     {"phaseDeg", feed.phaseDeg}});
        o.insert(QStringLiteral("sources"), feeds);
    }
    // Designer: only what an element has, so that files without groups or constraints stay as they were.
    if (s.id > 0) o.insert(QStringLiteral("id"), s.id);
    if (!s.group.empty()) o.insert(QStringLiteral("group"), QString::fromStdString(s.group));
    if (s.constraints.empty()) return;
    QJsonArray list;
    for (const AntennaConstraint &c : s.constraints)
        list.append(QJsonObject{{"type", static_cast<int>(c.type)},
                                {"reference", c.reference},
                                {"value", c.value},
                                {"anchor", c.anchor},
                                {"referenceAnchor", c.referenceAnchor},
                                {"axes", c.axes},
                                {"fixedX", c.fixed.x},
                                {"fixedY", c.fixed.y},
                                {"fixedZ", c.fixed.z},
                                {"enabled", c.enabled}});
    o.insert(QStringLiteral("constraints"), list);
}

void AntennaScene::readFields(const QJsonObject &o, AntennaElement &s) const
{
    for (const auto &f : kNumbers) s.*(f.member) = o.value(QString::fromLatin1(f.key)).toDouble(s.*(f.member));
    s.sides = std::clamp(o.value(QStringLiteral("sides")).toInt(s.sides), 3, 200);
    s.medium = std::clamp(o.value(QStringLiteral("medium")).toInt(s.medium), 0, 1);
    s.permittivity = std::clamp(s.permittivity, 1.0, 1000.0);
    s.lossTangent = std::clamp(s.lossTangent, 0.0, 10.0);
    s.fed = std::clamp(o.value(QStringLiteral("fed")).toInt(s.fed), 0, 1);
    s.moreFeeds.clear();
    const QJsonArray feeds = o.value(QStringLiteral("sources")).toArray();
    for (const QJsonValue &item : feeds)
    {
        const QJsonObject j = item.toObject();
        AntennaFeed feed;
        feed.name = j.value(QStringLiteral("name")).toString().toStdString();
        feed.fraction = std::clamp(j.value(QStringLiteral("fraction")).toDouble(0.5), 0.0, 1.0);
        feed.strength = j.value(QStringLiteral("voltage")).toDouble(1.0);
        feed.phaseDeg = j.value(QStringLiteral("phaseDeg")).toDouble(0.0);
        s.moreFeeds.push_back(feed);
    }
    s.id = std::max(0, o.value(QStringLiteral("id")).toInt(0));
    s.group = o.value(QStringLiteral("group")).toString().toStdString();
    s.constraints.clear();
    const QJsonArray list = o.value(QStringLiteral("constraints")).toArray();
    for (const QJsonValue &item : list)
    {
        const QJsonObject j = item.toObject();
        AntennaConstraint c;
        c.type = static_cast<AntennaConstraint::Type>(std::clamp(j.value(QStringLiteral("type")).toInt(0), 0, 7));
        c.reference = j.value(QStringLiteral("reference")).toInt(0);
        c.value = j.value(QStringLiteral("value")).toDouble(0.0);
        c.anchor = std::clamp(j.value(QStringLiteral("anchor")).toInt(0), 0, 2);
        c.referenceAnchor = std::clamp(j.value(QStringLiteral("referenceAnchor")).toInt(0), 0, 2);
        c.axes = j.value(QStringLiteral("axes")).toInt(7) & 7;
        c.fixed = {j.value(QStringLiteral("fixedX")).toDouble(0.0), j.value(QStringLiteral("fixedY")).toDouble(0.0),
                   j.value(QStringLiteral("fixedZ")).toDouble(0.0)};
        c.enabled = j.value(QStringLiteral("enabled")).toBool(true);
        s.constraints.push_back(c);
    }
}

QString AntennaScene::formula(int typeValue) const
{
    const QString common = tr("\nMéthode des moments, fils minces (noyau réduit), courants en fonctions triangulaires "
                              "(Galerkin) ; source « delta-gap » : une tension appliquée en un point du fil.");
    switch (typeFromInt(typeValue))
    {
    case Type::Wire:
        return tr("Fil droit. Passif, il est excité par le champ des autres éléments (élément parasite : réflecteur "
                  "plus long, directeur plus court que le radiateur) ; alimenté, il porte une source à la position "
                  "choisie.") +
               common;
    case Type::Dipole:
        return tr("Dipôle alimenté au centre. Vers L ≈ 0,47 à 0,48 λ, il résonne (X = 0) vers 70 Ω ; diagramme en "
                  "tore, 2,15 dBi. En V, les bras s'inclinent vers l'avant : impédance et directivité changent.") +
               common;
    case Type::FoldedDipole:
        return tr("Dipôle replié : deux fils parallèles reliés aux extrémités, alimenté au centre de l'un d'eux. Même "
                  "diagramme que le dipôle, impédance environ quatre fois plus grande (≈ 4 × 73 Ω).") +
               common;
    case Type::CircularLoop:
    case Type::RectangularLoop:
        return tr("Boucle alimentée en un point. Petite (périmètre ≪ λ) : dipôle magnétique, diagramme en tore autour de "
                  "son axe et résistance de rayonnement minuscule, R ≈ 20π² (C/λ)⁴. D'une longueur d'onde : rayonnement "
                  "maximal le long de l'axe, résonance vers 1,07 à 1,10 λ de périmètre, 130 à 145 Ω.") +
               common;
    case Type::Helix:
        return tr("Hélice. Alimentée au milieu, en mode normal (diamètre et hauteur petits devant λ) : un dipôle "
                  "enroulé, plus court à la même résonance, de faible résistance de rayonnement. Alimentée au pied "
                  "(position de la source : 0), le pied posé sur un plan de masse, avec une circonférence d'environ "
                  "λ : mode axial, faisceau le long de l'axe en polarisation circulaire, R ≈ 140 C/λ ohms (Kraus).") +
               common;
    case Type::Plate:
    case Type::Disc:
        return tr("Surface parfaitement conductrice : plan de masse, réflecteur. Elle ne s'alimente pas elle-même ; un "
                  "fil dont un bout est posé dessus y est relié, et une source placée à ce bout (position 0 ou 1) agit "
                  "entre le fil et la surface, comme au pied d'un monopôle. Un plan fini n'est pas un plan infini : "
                  "le courant se réfléchit sur ses bords, l'impédance oscille autour de la valeur de la théorie des "
                  "images quand sa taille varie, et le lobe d'un monopôle se relève au-dessus de l'horizon.\n"
                  "Méthode des moments : courants de surface en fonctions de Rao-Wilton-Glisson sur des triangles, "
                  "courant du fil réparti autour de son pied par la fonction de jonction de Hwu et Wilton.");
    case Type::Box:
    case Type::Cylinder:
    case Type::Cone:
    case Type::Sphere:
        return tr("Solide parfaitement conducteur : un boîtier, un mât, un corps. Seule sa peau porte des courants ; "
                  "elle est fermée, le champ est nul à l'intérieur. Il ne s'alimente pas lui-même : un fil dont un bout "
                  "est posé sur sa peau y est relié, et une source placée à ce bout agit entre le fil et le solide. "
                  "Le solide est alors la masse de l'antenne : les courants descendent le long de ses faces et il "
                  "rayonne avec le fil, l'impédance et le diagramme dépendent de sa taille et de l'endroit où le fil "
                  "est posé.\n"
                  "Méthode des moments : équation du champ électrique sur la peau, en triangles (fonctions de "
                  "Rao-Wilton-Glisson). Un pavé est fait de ses six faces jointes ; cylindre, cône et sphère sont des "
                  "surfaces de révolution à facettes, dont un sommet vient sous le pied du fil. Limites : près des "
                  "fréquences de résonance de la cavité d'un solide de plus d'une demi-longueur d'onde, cette équation "
                  "perd en précision ; les solides courbes ne sont pas reliés aux plaques ni aux solides qu'ils touchent.\n"
                  "Solide diélectrique (milieu : diélectrique, εr, tan δ) : le champ le traverse, plus lentement que l'air ; "
                  "il dévie et concentre l'onde, et il en absorbe une part s'il a des pertes. Calcul par équivalence de "
                  "surface : sa peau porte un courant électrique et un courant magnétique équivalents, qui rayonnent dans "
                  "l'air d'un côté et dans le milieu de l'autre ; ses triangles sont plus fins (la longueur d'onde y est "
                  "plus courte) et comptent double. Limites : un conducteur ne peut pas encore le toucher ni le traverser, "
                  "aucun fil ne s'y attache.");
    case Type::Paraboloid:
    case Type::ParabolicCylinder:
        return tr("Réflecteur parabolique, parfaitement conducteur. Une onde partie du foyer (repéré par une croix) "
                  "repart parallèle à l'axe : un dipôle au foyer donne un faisceau étroit le long de l'axe de "
                  "l'ouverture. Parabole de révolution : directivité de l'ordre de η (π D / λ)², η ≈ 0,5 à 0,7 selon "
                  "le rapport f / D et le diagramme de la source (2 λ d'ouverture : environ 12 dBi). Cylindre "
                  "parabolique : il concentre dans un seul plan, la source s'étend le long de la ligne focale.\n"
                  "Méthode des moments : surface ouverte en triangles (fonctions de Rao-Wilton-Glisson), le courant "
                  "s'annule au bord ; un fil posé dessus y est relié. Vérifié contre l'optique physique (à 0,2 dB) et, "
                  "aplati (focale très longue), contre un disque et une plaque. Une parabole se maille vite en "
                  "beaucoup de triangles : au-delà de 2 à 3 λ d'ouverture, la limite d'inconnues est atteinte.");
    case Type::Arc:
        return tr("Arc de cercle en fil, dans le plan perpendiculaire à son axe. Passif, ou alimenté en un point qui "
                  "se glisse le long de l'arc ; ses bouts s'attachent comme ceux d'un fil droit. Très ouvert (grand rayon), "
                  "c'est un fil droit ; refermé, une boucle.") +
               common;
    case Type::Spiral:
        return tr("Spirale d'Archimède à deux bras, r = r0 + a φ, le second bras tourné d'un demi-tour, alimentée au "
                  "centre entre les deux. Elle rayonne là où sa circonférence vaut une longueur d'onde : polarisation "
                  "circulaire le long de l'axe, de sens opposés des deux côtés, sur une large bande tant que cette zone "
                  "est entre le rayon intérieur et le rayon extérieur. Ici les bras s'arrêtent sans charge : le courant "
                  "qui arrive au bout revient, le rapport axial varie avec la fréquence.") +
               common;
    }
    return {};
}

QString AntennaScene::accuracy(int) const
{
    return tr("Convergé à ~1 % (100 segments par λ ; surfaces : ~1 % sur R, ~1 Ω sur X à 10 triangles par λ)");
}

SourceOutline AntennaScene::outline(int row, ViewPlane, double) const
{
    // Seen from above (plane XY): the antenna views draw in 3D from antennaShape().
    SourceOutline o;
    if (row < 0 || row >= count()) return o;
    const AntennaShape shape = antennaShape(at(row));
    for (const auto &line : shape.polylines)
    {
        QPolygonF poly;
        for (const auto &p : line) poly << QPointF(p.x, p.y);
        o.lines << poly;
    }
    o.centre = QPointF(at(row).position.x, at(row).position.y);
    o.sign = at(row).fed ? 1 : 0;
    return o;
}

// ---- Results ----

QVariantMap AntennaScene::probe(double, double, double) const
{
    auto row = [](const QString &name, double value, const QString &u, bool strong)
    { return QVariantMap{{"name", name}, {"value", value}, {"unit", u}, {"strong", strong}}; };
    if (!m_result) return {{"rows", QVariantList{}}, {"note", tr("Calcul en cours…")}};
    const AntennaResult &r = *m_result;
    if (!r.valid) return {{"rows", QVariantList{}}, {"note", QString::fromStdString(r.error)}};
    auto feedRows = [&](const AntennaFeedResult &f)
    {
        return QVariantList{row(QStringLiteral("R"), f.impedance.real(), QStringLiteral("Ω"), true),
                            row(QStringLiteral("X"), f.impedance.imag(), QStringLiteral("Ω"), true),
                            row(QStringLiteral("|S11|"), -f.returnLossDb, QStringLiteral("dB"), false),
                            row(tr("TOS"), f.vswr, QString(), false),
                            row(QStringLiteral("|I|"), std::abs(f.current), QStringLiteral("A"), false),
                            row(QStringLiteral("P"), f.powerW, QStringLiteral("W"), false)};
    };
    QVariantList rows = r.feeds.empty() ? QVariantList{} : feedRows(r.feeds.front());
    QVariantList groups;
    for (std::size_t i = 1; i < r.feeds.size(); ++i)
        groups << QVariantMap{{"title", tr("Source « %1 »").arg(QString::fromStdString(r.feeds[i].name))},
                              {"rows", feedRows(r.feeds[i])}};
    if (r.hasPattern)
        groups << QVariantMap{{"title", tr("Rayonnement")},
                              {"rows", QVariantList{row(tr("Gain"), r.directivityDbi, QStringLiteral("dBi"), true),
                                                    row(tr("Av/Ar"), r.frontToBackDb, QStringLiteral("dB"), false),
                                                    row(QStringLiteral("θ max"), r.mainThetaDeg, QStringLiteral("°"), false),
                                                    row(QStringLiteral("φ max"), r.mainPhiDeg, QStringLiteral("°"), false),
                                                    row(QStringLiteral("P ray."), r.radiatedPowerW, QStringLiteral("W"), false)}}};
    if (r.hasPattern && r.polarisationSense != 0)
        groups << QVariantMap{{"title", r.polarisationSense > 0 ? tr("Polarisation elliptique droite") : tr("Polarisation elliptique gauche")},
                              {"rows", QVariantList{row(tr("Rapport axial"), r.axialRatioDb, QStringLiteral("dB"), true)}}};
    if (!r.facets.empty())
        groups << QVariantMap{{"title", tr("Surfaces")},
                              {"rows", QVariantList{row(tr("|J| max"), r.maxSurfaceCurrent, QStringLiteral("A/m"), false),
                                                    row(tr("Triangles"), double(r.facets.size()), QString(), false)}}};
    QString note;
    if (!r.feeds.empty())
        note = tr("Source « %1 » : Zin = %2, référence %3. ")
                   .arg(QString::fromStdString(r.feeds.front().name), complexText(r.feeds.front().impedance, QStringLiteral("Ω")),
                        Units::formatQuantity(m_settings.referenceOhm, QStringLiteral("Ω"), 3));
    note += tr("Conducteurs parfaits : gain = directivité. Av/Ar : rapport avant/arrière dans l'axe du lobe principal ; θ depuis +z, "
               "φ depuis +x.");
    if (r.hasPattern && r.polarisationSense != 0)
        note += tr(" Rapport axial dans l'axe du lobe principal : 0 dB pour une polarisation circulaire, au-delà de 40 dB elle "
                   "est linéaire.");
    return {{"rows", rows}, {"groups", groups}, {"note", note}};
}

QString AntennaScene::statusText() const
{
    if (!m_result) return busy() ? tr("Calcul…") : QString();
    const AntennaResult &r = *m_result;
    if (!r.valid) return tr("Pas de résultat : %1").arg(QString::fromStdString(r.error));
    return tr("%1 · λ = %2 · %3 inconnues · %4 ms")
        .arg(Units::formatQuantity(r.frequency, QStringLiteral("Hz"), 4), Units::formatQuantity(r.wavelength, QStringLiteral("m"), 4))
        .arg(r.unknowns)
        .arg(QString::number(r.milliseconds, 'f', 0));
}

// ---- Examples ----

QVariantList AntennaScene::examples() const
{
    QVariantList list;
    for (const auto &p : antennaPresets())
        list << QVariantMap{{"key", QString::fromLatin1(p.key)}, {"name", QString::fromUtf8(p.name)}, {"plane", 0}};
    return list;
}

std::vector<AntennaElement> AntennaScene::example(const QString &key) const
{
    return antennaPreset(key.toStdString(), m_settings.frequency);
}

void AntennaScene::loadExample(const QString &key)
{
    // Built once: some presets solve to set their sources (the phased pair).
    std::vector<AntennaElement> elements = example(key);
    if (elements.empty()) return;
    // A sweep around the frequency the preset is dimensioned for.
    m_settings.sweepStart = 0.8 * m_settings.frequency;
    m_settings.sweepStop = 1.2 * m_settings.frequency;
    m_sweep.reset();
    m_sweepStale = false;
    emit sweepChanged();
    m_undo.clear();
    applyState(State{std::move(elements), {}});
    forgetFile();
}

// ---- Settings ----

QStringList AntennaScene::settingFields() const
{
    return {"frequency", "referenceOhm", "segmentsPerWavelength", "facetsPerWavelength", "sweepStart", "sweepStop", "sweepPoints"};
}

QString AntennaScene::settingLabel(const QString &key) const
{
    if (key == QLatin1String("frequency")) return tr("f — fréquence d'analyse");
    if (key == QLatin1String("referenceOhm")) return tr("Z₀ — impédance de référence (S11, TOS)");
    if (key == QLatin1String("segmentsPerWavelength")) return tr("Maillage des fils : segments par longueur d'onde");
    if (key == QLatin1String("facetsPerWavelength")) return tr("Maillage des surfaces : triangles par longueur d'onde");
    if (key == QLatin1String("sweepStart")) return tr("Balayage : de");
    if (key == QLatin1String("sweepStop")) return tr("Balayage : à");
    if (key == QLatin1String("sweepPoints")) return tr("Balayage : nombre de points");
    return key;
}

QString AntennaScene::settingUnit(const QString &key) const
{
    if (key == QLatin1String("referenceOhm")) return QStringLiteral("Ω");
    if (key == QLatin1String("segmentsPerWavelength") || key == QLatin1String("facetsPerWavelength") ||
        key == QLatin1String("sweepPoints"))
        return {};
    return QStringLiteral("Hz");
}

QVariantMap AntennaScene::settingsData() const
{
    return {{"frequency", m_settings.frequency},
            {"referenceOhm", m_settings.referenceOhm},
            {"segmentsPerWavelength", m_settings.segmentsPerWavelength},
            {"facetsPerWavelength", m_settings.facetsPerWavelength},
            {"sweepStart", m_settings.sweepStart},
            {"sweepStop", m_settings.sweepStop},
            {"sweepPoints", m_settings.sweepPoints}};
}

void AntennaScene::setSetting(const QString &key, const QVariant &value)
{
    bool ok = false;
    const double v = value.toDouble(&ok);
    if (!ok || !std::isfinite(v)) return;
    AntennaSettings next = m_settings;
    if (key == QLatin1String("frequency"))
        next.frequency = std::clamp(v, 1e3, 1e12);
    else if (key == QLatin1String("referenceOhm"))
        next.referenceOhm = std::clamp(v, 0.1, 1e5);
    else if (key == QLatin1String("segmentsPerWavelength"))
        next.segmentsPerWavelength = std::clamp(v, 10.0, 240.0);
    else if (key == QLatin1String("facetsPerWavelength"))
        next.facetsPerWavelength = std::clamp(v, 4.0, 40.0);
    else if (key == QLatin1String("sweepStart"))
        next.sweepStart = std::clamp(v, 1e3, 1e12);
    else if (key == QLatin1String("sweepStop"))
        next.sweepStop = std::clamp(v, 1e3, 1e12);
    else if (key == QLatin1String("sweepPoints"))
        next.sweepPoints = std::clamp(static_cast<int>(std::lround(v)), 2, 401);
    else
        return;
    if (next == m_settings) return;
    m_undo.push(new SettingsCommand(this, m_settings, next, tr("Modifier les réglages")));
}

void AntennaScene::setFrequency(double frequency)
{
    setSetting(QStringLiteral("frequency"), frequency);
}

int AntennaScene::addWire(const qsig::Vec3 &a, const qsig::Vec3 &b)
{
    AntennaElement e = makeSource(Type::Wire);
    if (!setWireEndpoints(e, a, b))
    {
        emit errorOccurred(tr("Fil trop court : il faut au moins dix fois son rayon (%1).")
                               .arg(Units::formatQuantity(10.0 * e.wireRadius, QStringLiteral("m"), 3)));
        return -1;
    }
    e.name = freshName(m_sources, Type::Wire);
    State next = current();
    next.sources.push_back(e);
    next.keepDensity.push_back(0);
    pushState(std::move(next), tr("Tracer %1").arg(QString::fromStdString(e.name)));
    return count() - 1;
}

std::string AntennaScene::freshName(const std::vector<AntennaElement> &sources, Type type) const
{
    const std::string prefix = shortName(type).toStdString() + " ";
    for (int n = 1;; ++n)
    {
        const std::string name = prefix + std::to_string(n);
        if (std::none_of(sources.begin(), sources.end(), [&name](const AntennaElement &e) { return e.name == name; }))
            return name;
    }
}

QList<int> AntennaScene::validRows(const QList<int> &rows) const
{
    QList<int> valid;
    for (const int row : rows)
        if (row >= 0 && row < count() && !valid.contains(row)) valid.append(row);
    std::sort(valid.begin(), valid.end());
    return valid;
}

QString AntennaScene::describe(const QList<int> &rows) const
{
    return rows.size() == 1 ? sourceName(rows.first()) : tr("%1 éléments").arg(rows.size());
}

qsig::Vec3 AntennaScene::centreOf(const QList<int> &rows) const
{
    return elementsCentre(m_sources, std::vector<int>(rows.begin(), rows.end()));
}

bool AntennaScene::moveWireEnd(int row, int end, const qsig::Vec3 &point)
{
    if (row < 0 || row >= count() || at(row).type != Type::Wire) return false;
    State next = current();
    AntennaElement &e = next.sources[static_cast<std::size_t>(row)];
    qsig::Vec3 a, b;
    wireEndpoints(e, a, b);
    // The source keeps its place counted as a fraction of the wire.
    if (!setWireEndpoints(e, end == 0 ? point : a, end == 0 ? b : point)) return false;
    const QString text = tr("Modifier %1").arg(QString::fromStdString(e.name));
    pushState(std::move(next), text, dragMergeId(row));
    return true;
}

int AntennaScene::cutWire(int row, const qsig::Vec3 &point)
{
    if (row < 0 || row >= count() || at(row).type != Type::Wire) return -1;
    AntennaElement first, second;
    if (!splitWire(at(row), wireFraction(at(row), point), first, second))
    {
        emit errorOccurred(tr("Coupe trop près d'un bout : chaque partie doit faire au moins dix fois le rayon du fil (%1).")
                               .arg(Units::formatQuantity(10.0 * at(row).wireRadius, QStringLiteral("m"), 3)));
        return -1;
    }
    State next = current();
    second.name = freshName(next.sources, Type::Wire);
    next.sources[static_cast<std::size_t>(row)] = first;
    next.sources.push_back(second);
    next.keepDensity.push_back(0);
    pushState(std::move(next), tr("Couper %1").arg(QString::fromStdString(first.name)));
    return count() - 1;
}

void AntennaScene::dragElements(const QList<int> &rows, const std::vector<qsig::Vec3> &positions)
{
    if (rows.isEmpty() || static_cast<std::size_t>(rows.size()) != positions.size()) return;
    State next = current();
    for (qsizetype i = 0; i < rows.size(); ++i)
    {
        const int row = rows[i];
        if (row < 0 || row >= count()) return;
        next.sources[static_cast<std::size_t>(row)].position = positions[static_cast<std::size_t>(i)];
    }
    pushState(std::move(next), tr("Déplacer %1").arg(describe(rows)), dragMergeId(rows.first()));
}

template <typename Change>
QList<int> AntennaScene::transformElements(const QList<int> &rows, bool copy, const QString &text, Change change)
{
    const QList<int> valid = validRows(rows);
    if (valid.isEmpty()) return {};
    State next = current();
    QList<int> selection;
    for (const int row : valid)
    {
        if (!copy)
        {
            change(next.sources[static_cast<std::size_t>(row)]);
            selection.append(row);
            continue;
        }
        AntennaElement image = at(row);
        change(image);
        image.name = freshName(next.sources, image.type);
        selection.append(static_cast<int>(next.sources.size()));
        next.sources.push_back(image);
        next.keepDensity.push_back(0);
    }
    pushState(std::move(next), text.arg(describe(valid)));
    return selection;
}

QList<int> AntennaScene::translateElements(const QList<int> &rows, const qsig::Vec3 &delta, int copies)
{
    if (copies <= 0)
        return transformElements(rows, false, tr("Déplacer %1"), [&delta](AntennaElement &e) { e.position += delta; });
    // An array: the copies of all the elements at delta, then at 2 delta... in one undo step.
    const QList<int> valid = validRows(rows);
    if (valid.isEmpty()) return {};
    State next = current();
    QList<int> selection;
    for (int k = 1; k <= std::min(copies, 50); ++k)
        for (const int row : valid)
        {
            AntennaElement image = at(row);
            image.position += delta * static_cast<double>(k);
            image.name = freshName(next.sources, image.type);
            selection.append(static_cast<int>(next.sources.size()));
            next.sources.push_back(image);
            next.keepDensity.push_back(0);
        }
    pushState(std::move(next), tr("Dupliquer %1").arg(describe(valid)));
    return selection;
}

QList<int> AntennaScene::rotateElements(const QList<int> &rows, const qsig::Vec3 &pivot, const qsig::Vec3 &axis,
                                        double angleDeg, bool copy)
{
    return transformElements(rows, copy, copy ? tr("Copier en tournant %1") : tr("Tourner %1"),
                             [&](AntennaElement &e) { rotateElement(e, pivot, axis, angleDeg); });
}

QList<int> AntennaScene::mirrorElements(const QList<int> &rows, const qsig::Vec3 &point, const qsig::Vec3 &normal,
                                        bool copy)
{
    return transformElements(rows, copy, copy ? tr("Copier en miroir %1") : tr("Miroir de %1"),
                             [&](AntennaElement &e) { mirrorElement(e, point, normal); });
}

void AntennaScene::alignElements(const QList<int> &rows, int axis, double value)
{
    if (!std::isfinite(value)) return;
    transformElements(rows, false, tr("Aligner %1"),
                      [axis, value](AntennaElement &e)
                      {
                          if (axis == 0)
                              e.position.x = value;
                          else if (axis == 1)
                              e.position.y = value;
                          else
                              e.position.z = value;
                      });
}

// ---- Groups and constraints ----

void AntennaScene::settleState(State &state) const
{
    ensureElementIds(state.sources);
    pruneConstraints(state.sources);
    applyConstraints(state.sources);
}

QString AntennaScene::groupOf(int row) const
{
    return row >= 0 && row < count() ? QString::fromStdString(at(row).group) : QString();
}

QList<int> AntennaScene::groupRows(int row) const
{
    QList<int> rows;
    if (row < 0 || row >= count()) return rows;
    const std::string &group = at(row).group;
    if (group.empty()) return QList<int>{row};
    for (int i = 0; i < count(); ++i)
        if (at(i).group == group) rows.append(i);
    return rows;
}

void AntennaScene::groupElements(const QList<int> &rows)
{
    const QList<int> valid = validRows(rows);
    if (valid.size() < 2) return;
    State next = current();
    // "Groupe 1", "Groupe 2"...: the first name no element carries.
    std::string name;
    for (int n = 1; name.empty(); ++n)
    {
        const std::string candidate = tr("Groupe %1").arg(n).toStdString();
        if (std::none_of(next.sources.begin(), next.sources.end(), [&](const AntennaElement &e) { return e.group == candidate; }))
            name = candidate;
    }
    for (const int row : valid) next.sources[static_cast<std::size_t>(row)].group = name;
    pushState(std::move(next), tr("Grouper %1").arg(describe(valid)));
}

void AntennaScene::ungroupElements(const QList<int> &rows)
{
    std::vector<std::string> names;
    for (const int row : validRows(rows))
        if (!at(row).group.empty()) names.push_back(at(row).group);
    if (names.empty()) return;
    State next = current();
    for (AntennaElement &e : next.sources)
        if (std::find(names.begin(), names.end(), e.group) != names.end()) e.group.clear();
    pushState(std::move(next), tr("Dégrouper"));
}

void AntennaScene::renameGroup(int row, const QString &name)
{
    if (row < 0 || row >= count()) return;
    const std::string before = at(row).group, after = name.trimmed().toStdString();
    if (before.empty() || after.empty() || after == before) return;
    if (std::any_of(m_sources.begin(), m_sources.end(), [&](const AntennaElement &e) { return e.group == after; }))
    {
        emit errorOccurred(tr("« %1 » est déjà le nom d'un autre groupe.").arg(name.trimmed()));
        return;
    }
    State next = current();
    for (AntennaElement &e : next.sources)
        if (e.group == before) e.group = after;
    pushState(std::move(next), tr("Renommer le groupe en %1").arg(name.trimmed()));
}

bool AntennaScene::addConstraint(int row, int type, int referenceRow, double value, int anchor, int referenceAnchor, int axes)
{
    using CType = AntennaConstraint::Type;
    if (row < 0 || row >= count()) return false;
    auto refuse = [this](const QString &message)
    {
        emit errorOccurred(message);
        return false;
    };
    State next = current();
    ensureElementIds(next.sources);
    AntennaElement &e = next.sources[static_cast<std::size_t>(row)];
    AntennaConstraint c;
    c.type = static_cast<CType>(std::clamp(type, 0, 7));
    c.anchor = std::clamp(anchor, 0, 2);
    c.referenceAnchor = std::clamp(referenceAnchor, 0, 2);
    const AntennaElement *reference = nullptr;
    if (constraintNeedsReference(c.type))
    {
        if (referenceRow < 0 || referenceRow >= count() || referenceRow == row)
            return refuse(tr("Cette contrainte lie deux éléments : cliquez l'élément de référence, puis Ctrl + clic sur "
                             "celui qui doit le suivre."));
        reference = &next.sources[static_cast<std::size_t>(referenceRow)];
        c.reference = reference->id;
    }
    const QString lengthless = tr("Une contrainte de longueur s'applique aux fils, dipôles, dipôles repliés et cadres.");
    switch (c.type)
    {
    case CType::FixPosition:
        c.axes = axes & 7;
        if (c.axes == 0) return refuse(tr("Cochez au moins un axe à fixer."));
        c.fixed = e.position;
        break;
    case CType::Length:
        if (!elementHasLength(e.type)) return refuse(lengthless);
        if (!std::isfinite(value) || !(value > 10.0 * e.wireRadius))
            return refuse(tr("Longueur trop courte : il faut au moins dix fois le rayon du fil."));
        c.value = value;
        break;
    case CType::EqualLength:
        if (!reference || !elementHasLength(e.type) || !elementHasLength(reference->type)) return refuse(lengthless);
        break;
    case CType::Angle:
        if (!std::isfinite(value) || value < 0.0 || value > 180.0) return refuse(tr("L'angle va de 0° à 180°."));
        c.value = value;
        break;
    case CType::Distance:
        if (!std::isfinite(value) || value < 0.0) return refuse(tr("La distance ne peut pas être négative."));
        c.value = value;
        break;
    case CType::Parallel:
    case CType::Perpendicular:
    case CType::Coincident:
        break;
    }
    // A new constraint replaces the one it would contradict outright: the same kind of relation
    // (direction, length, place of the same point, fixed position) with the same reference.
    auto family = [](CType t)
    {
        if (t == CType::Parallel || t == CType::Perpendicular || t == CType::Angle) return 1;
        if (t == CType::Length || t == CType::EqualLength) return 2;
        if (t == CType::Coincident || t == CType::Distance) return 3;
        return 0;
    };
    std::erase_if(e.constraints,
                  [&](const AntennaConstraint &other)
                  {
                      if (family(other.type) != family(c.type)) return false;
                      if (family(c.type) == 0 || family(c.type) == 2) return true;
                      return other.reference == c.reference && (family(c.type) != 3 || other.anchor == c.anchor);
                  });
    e.constraints.push_back(c);
    const QString text = tr("Contraindre %1").arg(QString::fromStdString(e.name));
    pushState(std::move(next), text);
    return true;
}

void AntennaScene::removeConstraint(int row, int index)
{
    if (row < 0 || row >= count() || index < 0 || index >= static_cast<int>(at(row).constraints.size())) return;
    State next = current();
    auto &list = next.sources[static_cast<std::size_t>(row)].constraints;
    list.erase(list.begin() + index);
    pushState(std::move(next), tr("Retirer une contrainte de %1").arg(sourceName(row)));
}

void AntennaScene::setConstraintEnabled(int row, int index, bool enabled)
{
    if (row < 0 || row >= count() || index < 0 || index >= static_cast<int>(at(row).constraints.size())) return;
    if (at(row).constraints[static_cast<std::size_t>(index)].enabled == enabled) return;
    State next = current();
    next.sources[static_cast<std::size_t>(row)].constraints[static_cast<std::size_t>(index)].enabled = enabled;
    pushState(std::move(next), enabled ? tr("Activer une contrainte de %1").arg(sourceName(row))
                                       : tr("Suspendre une contrainte de %1").arg(sourceName(row)));
}

QString AntennaScene::constraintText(const AntennaConstraint &c) const
{
    using CType = AntennaConstraint::Type;
    QString reference = tr("un élément supprimé");
    for (const AntennaElement &other : m_sources)
        if (other.id == c.reference && c.reference > 0) reference = QString::fromStdString(other.name);
    const QString points[3] = {tr("centre"), tr("bout 1"), tr("bout 2")};
    const QString own = points[std::clamp(c.anchor, 0, 2)], theirs = points[std::clamp(c.referenceAnchor, 0, 2)];
    switch (c.type)
    {
    case CType::FixPosition:
    {
        QStringList axes;
        if (c.axes & 1) axes << QStringLiteral("x");
        if (c.axes & 2) axes << QStringLiteral("y");
        if (c.axes & 4) axes << QStringLiteral("z");
        return tr("Position fixée (%1)").arg(axes.join(QStringLiteral(", ")));
    }
    case CType::Length:
        return tr("Longueur imposée : %1").arg(Units::formatQuantity(c.value, QStringLiteral("m"), 4));
    case CType::EqualLength:
        return tr("Même longueur que %1").arg(reference);
    case CType::Parallel:
        return tr("Parallèle à %1").arg(reference);
    case CType::Perpendicular:
        return tr("Perpendiculaire à %1").arg(reference);
    case CType::Angle:
        return tr("À %1 de %2").arg(Units::formatQuantity(c.value, QStringLiteral("°"), 4), reference);
    case CType::Coincident:
        // "Bout 1 sur le bout 2 de Fil 3"
        return tr("%1 sur le %2 de %3").arg(own.left(1).toUpper() + own.mid(1), theirs, reference);
    case CType::Distance:
        return tr("%1 à %2 du %3 de %4")
            .arg(own.left(1).toUpper() + own.mid(1), Units::formatQuantity(c.value, QStringLiteral("m"), 4), theirs, reference);
    }
    return {};
}

QVariantList AntennaScene::constraintList(int row) const
{
    QVariantList list;
    if (row < 0 || row >= count()) return list;
    const AntennaElement &e = at(row);
    for (const AntennaConstraint &c : e.constraints)
        list.append(QVariantMap{{"text", constraintText(c)}, {"enabled", c.enabled}, {"met", !c.enabled || constraintMet(m_sources, e, c)}});
    return list;
}

void AntennaScene::removeElements(const QList<int> &rows)
{
    const QList<int> valid = validRows(rows);
    if (valid.isEmpty()) return;
    const QString text = tr("Supprimer %1").arg(describe(valid));
    State next = current();
    // From the last row down: the rows before it keep their place.
    for (qsizetype i = valid.size() - 1; i >= 0; --i)
    {
        next.sources.erase(next.sources.begin() + valid[i]);
        next.keepDensity.erase(next.keepDensity.begin() + valid[i]);
    }
    pushState(std::move(next), text);
}

namespace
{
// Place of a source on a plain wire for a point picked on it: the quarters and the middle attract it;
// near an end that stands on a surface the source goes to the end itself, between the wire and the
// surface.
double feedFractionAt(const std::vector<AntennaElement> &elements, const AntennaElement &wire, const qsig::Vec3 &point)
{
    double fraction = curveFraction(wire, point);
    for (const double mark : {0.25, 0.5, 0.75})
        if (std::abs(fraction - mark) < 0.03) fraction = mark;
    double placed = std::clamp(fraction, 0.02, 0.98);
    // Its ends: those of a straight wire, or of the curve.
    qsig::Vec3 a, b;
    wireEndpoints(wire, a, b);
    if (wire.type != Type::Wire)
    {
        const auto shape = antennaShape(wire);
        if (!shape.polylines.empty() && !shape.polylines.front().empty())
        {
            a = shape.polylines.front().front();
            b = shape.polylines.front().back();
        }
    }
    if (fraction < 0.2 && surfaceAt(elements, a) >= 0) placed = 0.0;
    if (fraction > 0.8 && surfaceAt(elements, b) >= 0) placed = 1.0;
    return placed;
}
} // namespace

int AntennaScene::feedTotal() const
{
    int total = 0;
    for (const AntennaElement &e : sources()) total += qsig::app::feedCount(e);
    return total;
}

QVariantList AntennaScene::mediaList() const
{
    // Air around everything; the perfect conductor of the wires, the surfaces and the conducting solids;
    // then each dielectric in use (a permittivity and a loss tangent), with the solids made of it.
    int wires = 0, surfaces = 0, solids = 0;
    struct Entry
    {
        double permittivity, lossTangent;
        int count;
        QStringList names;
    };
    std::vector<Entry> dielectrics;
    for (const AntennaElement &e : sources())
    {
        if (elementIsDielectric(e))
        {
            auto same = [&e](const Entry &entry) { return entry.permittivity == e.permittivity && entry.lossTangent == e.lossTangent; };
            auto found = std::find_if(dielectrics.begin(), dielectrics.end(), same);
            if (found == dielectrics.end())
            {
                dielectrics.push_back({e.permittivity, e.lossTangent, 0, {}});
                found = dielectrics.end() - 1;
            }
            ++found->count;
            found->names << QString::fromStdString(e.name);
        }
        else if (elementIsSolid(e.type))
            ++solids;
        else if (elementIsSurface(e.type))
            ++surfaces;
        else
            ++wires;
    }
    QVariantList list;
    list.append(QVariantMap{{"name", tr("Air")}, {"detail", tr("Milieu ambiant")}, {"kind", 0}});
    list.append(QVariantMap{{"name", tr("Conducteur parfait")},
                            {"detail", tr("%1 fil(s), %2 surface(s), %3 solide(s)").arg(wires).arg(surfaces).arg(solids)},
                            {"kind", 1}});
    for (const Entry &entry : dielectrics)
    {
        QString name = tr("Diélectrique εr %1").arg(number(entry.permittivity, 2));
        if (entry.lossTangent > 0.0) name += tr(", tan δ %1").arg(number(entry.lossTangent, 3));
        list.append(QVariantMap{{"name", name}, {"detail", entry.names.join(QStringLiteral(", "))}, {"kind", 2}});
    }
    return list;
}

QVariantList AntennaScene::feedList() const
{
    QVariantList list;
    int running = 0;
    for (int row = 0; row < count(); ++row)
    {
        const AntennaElement &e = at(row);
        const int feeds = qsig::app::feedCount(e);
        for (int k = 0; k < feeds; ++k)
        {
            const AntennaFeed feed = qsig::app::feedAt(e, k);
            const bool along = e.type == Type::Wire || e.type == Type::Helix || e.type == Type::Arc;
            QString place;
            if (!along)
                place = e.type == Type::Dipole || e.type == Type::FoldedDipole ? tr("au centre") : tr("au point d'alimentation");
            else if (feed.fraction < 0.02)
                place = e.type == Type::Helix ? tr("au pied") : tr("au premier bout");
            else if (feed.fraction > 0.98)
                place = tr("au second bout");
            else
                place = tr("à %1 % du fil").arg(number(100.0 * feed.fraction, 0));
            QString text = Units::formatQuantity(feed.strength, QStringLiteral("V"), 3);
            if (feed.phaseDeg != 0.0) text += QStringLiteral(" ∠ %1°").arg(number(feed.phaseDeg, 0));
            ++running;
            list.append(QVariantMap{{"row", row},
                                    {"index", k},
                                    {"name", tr("Source %1").arg(running)},
                                    {"host", QString::fromStdString(e.name)},
                                    {"place", place},
                                    {"text", text},
                                    {"fraction", feed.fraction},
                                    {"voltage", feed.strength},
                                    {"phaseDeg", feed.phaseDeg},
                                    {"along", along},
                                    {"movable", qsig::app::feedSlides(e.type)}});
        }
    }
    return list;
}

int AntennaScene::placeFeed(int row, const qsig::Vec3 &point)
{
    if (row < 0 || row >= count()) return -1;
    if (elementIsSurface(at(row).type))
    {
        emit errorOccurred(tr("Une surface ou un solide ne s'alimente pas : placez la source au bout d'un fil posé dessus."));
        return -1;
    }
    State next = current();
    AntennaElement &e = next.sources[static_cast<std::size_t>(row)];
    int index = 0;
    if (e.fed != 0)
    {
        // A ready-made element has its source, at the place its type gives it.
        if (e.type != Type::Wire) return 0; // one source on the other elements
        // A plain wire takes one more, unless the point is on a source that is already there.
        AntennaFeed feed;
        feed.fraction = feedFractionAt(next.sources, e, point);
        const int feeds = qsig::app::feedCount(e);
        for (int k = 0; k < feeds; ++k)
            if (std::abs(qsig::app::feedAt(e, k).fraction - feed.fraction) < 0.03) return k;
        index = qsig::app::addFeed(e, feed);
    }
    else
    {
        if (qsig::app::feedSlides(e.type)) e.feedFraction = feedFractionAt(next.sources, e, point);
        e.fed = 1;
        e.moreFeeds.clear();
        if (e.strength == 0.0) e.strength = 1.0;
    }
    const QString text = tr("Placer une source sur %1").arg(QString::fromStdString(e.name));
    pushState(std::move(next), text);
    return index;
}

int AntennaScene::addFeedTo(int row)
{
    if (row < 0 || row >= count()) return -1;
    if (!qsig::app::canAddFeed(at(row)))
    {
        emit errorOccurred(elementIsSurface(at(row).type)
                               ? tr("Une surface ou un solide ne s'alimente pas : placez la source au bout d'un fil posé dessus.")
                               : tr("%1 a déjà sa source, à la place que lui donne ce type d'élément. Seul un fil droit en porte plusieurs.")
                                     .arg(QString::fromStdString(at(row).name)));
        return -1;
    }
    State next = current();
    AntennaElement &e = next.sources[static_cast<std::size_t>(row)];
    AntennaFeed feed = qsig::app::feedAt(e, 0); // on a passive element: the place and the voltage it last had
    if (e.fed != 0)
    {
        // A plain wire with sources: the middle of its longest free stretch.
        std::vector<double> marks{0.0, 1.0};
        const int feeds = qsig::app::feedCount(e);
        for (int k = 0; k < feeds; ++k) marks.push_back(std::clamp(qsig::app::feedAt(e, k).fraction, 0.0, 1.0));
        std::sort(marks.begin(), marks.end());
        double widest = 0.0;
        feed = AntennaFeed{};
        for (std::size_t k = 0; k + 1 < marks.size(); ++k)
            if (marks[k + 1] - marks[k] > widest)
            {
                widest = marks[k + 1] - marks[k];
                feed.fraction = 0.5 * (marks[k] + marks[k + 1]);
            }
    }
    if (feed.strength == 0.0) feed.strength = 1.0;
    const int index = qsig::app::addFeed(e, feed);
    if (index < 0) return -1;
    const QString text = tr("Placer une source sur %1").arg(QString::fromStdString(e.name));
    pushState(std::move(next), text);
    return index;
}

bool AntennaScene::moveFeed(int row, int index, const qsig::Vec3 &point)
{
    if (row < 0 || row >= count() || !qsig::app::feedSlides(at(row).type)) return false;
    State next = current();
    AntennaElement &e = next.sources[static_cast<std::size_t>(row)];
    const int feeds = qsig::app::feedCount(e);
    if (index < 0 || index >= feeds) return false;
    AntennaFeed feed = qsig::app::feedAt(e, index);
    const double fraction = feedFractionAt(next.sources, e, point);
    // Two sources do not share a place.
    for (int k = 0; k < feeds; ++k)
        if (k != index && std::abs(qsig::app::feedAt(e, k).fraction - fraction) < 0.02) return false;
    if (fraction == feed.fraction) return true;
    feed.fraction = fraction;
    qsig::app::setFeedAt(e, index, feed);
    const QString text = tr("Déplacer une source de %1").arg(QString::fromStdString(e.name));
    pushState(std::move(next), text, dragMergeId(row));
    return true;
}

void AntennaScene::deleteFeed(int row, int index)
{
    if (row < 0 || row >= count()) return;
    State next = current();
    AntennaElement &e = next.sources[static_cast<std::size_t>(row)];
    if (!qsig::app::removeFeed(e, index)) return;
    const QString text = tr("Retirer une source de %1").arg(QString::fromStdString(e.name));
    pushState(std::move(next), text);
}

void AntennaScene::setFeedValue(int row, int index, const QString &key, double value)
{
    if (row < 0 || row >= count() || !std::isfinite(value)) return;
    State next = current();
    AntennaElement &e = next.sources[static_cast<std::size_t>(row)];
    if (index < 0 || index >= qsig::app::feedCount(e)) return;
    AntennaFeed feed = qsig::app::feedAt(e, index);
    if (key == QLatin1String("voltage"))
        feed.strength = value;
    else if (key == QLatin1String("phaseDeg"))
        feed.phaseDeg = value;
    else if (key == QLatin1String("fraction") && (e.type == Type::Wire || e.type == Type::Helix || e.type == Type::Arc))
        feed.fraction = value < 0.02 ? 0.0 : value > 0.98 ? 1.0 : value; // the ends themselves, as on a surface
    else
        return;
    if (feed == qsig::app::feedAt(e, index)) return;
    qsig::app::setFeedAt(e, index, feed);
    const QString text = tr("Modifier une source de %1").arg(QString::fromStdString(e.name));
    pushState(std::move(next), text);
}

void AntennaScene::applySettings(const AntennaSettings &settings)
{
    m_settings = settings;
    emit sceneChanged();
}

QString AntennaScene::settingsNote() const
{
    const double lambda = qsig::constants::c0 / std::max(1.0, m_settings.frequency);
    QString text = tr("λ = %1 à %2").arg(Units::formatQuantity(lambda, QStringLiteral("m"), 4),
                                         Units::formatQuantity(m_settings.frequency, QStringLiteral("Hz"), 4));
    if (m_result && m_result->valid)
    {
        text += QLatin1Char('\n') + tr("%1 inconnues, %2 segments par λ").arg(m_result->unknowns).arg(number(m_result->segmentsPerWavelength, 0));
        if (m_result->segmentsPerWavelength < m_settings.segmentsPerWavelength - 0.5)
            text += tr(" (réduit : un segment ne descend pas sous deux rayons de fil)");
    }
    return text;
}

// ---- Files ----

void AntennaScene::writeExtra(QJsonObject &root) const
{
    root.insert(QStringLiteral("settings"), QJsonObject{{"frequency", m_settings.frequency},
                                                        {"referenceOhm", m_settings.referenceOhm},
                                                        {"segmentsPerWavelength", m_settings.segmentsPerWavelength},
                                                        {"facetsPerWavelength", m_settings.facetsPerWavelength},
                                                        {"sweepStart", m_settings.sweepStart},
                                                        {"sweepStop", m_settings.sweepStop},
                                                        {"sweepPoints", m_settings.sweepPoints}});
}

bool AntennaScene::readExtra(const QJsonObject &root, QString *)
{
    const QJsonObject o = root.value(QStringLiteral("settings")).toObject();
    const AntennaSettings d;
    AntennaSettings s;
    s.frequency = std::clamp(o.value(QStringLiteral("frequency")).toDouble(d.frequency), 1e3, 1e12);
    s.referenceOhm = std::clamp(o.value(QStringLiteral("referenceOhm")).toDouble(d.referenceOhm), 0.1, 1e5);
    s.segmentsPerWavelength =
        std::clamp(o.value(QStringLiteral("segmentsPerWavelength")).toDouble(d.segmentsPerWavelength), 10.0, 240.0);
    s.facetsPerWavelength = std::clamp(o.value(QStringLiteral("facetsPerWavelength")).toDouble(d.facetsPerWavelength), 4.0, 40.0);
    s.sweepStart = std::clamp(o.value(QStringLiteral("sweepStart")).toDouble(d.sweepStart), 1e3, 1e12);
    s.sweepStop = std::clamp(o.value(QStringLiteral("sweepStop")).toDouble(d.sweepStop), 1e3, 1e12);
    s.sweepPoints = std::clamp(o.value(QStringLiteral("sweepPoints")).toInt(d.sweepPoints), 2, 401);
    m_settings = s;
    m_sweep.reset();
    m_sweepStale = false;
    emit sweepChanged();
    return true;
}

void AntennaScene::clearSources()
{
    m_settings = AntennaSettings{};
    m_sweep.reset();
    m_sweepStale = false;
    emit sweepChanged();
    SourceListScene::clearSources();
}

// ---- Background computation ----

void AntennaScene::solve()
{
    if (m_solving)
    {
        m_pending = true; // solved again as soon as the current one ends
        return;
    }
    const std::vector<AntennaElement> elements = m_sources;
    const AntennaSettings settings = m_settings;
    m_solving = true;
    m_watcher.setFuture(QtConcurrent::run([elements, settings]() -> std::shared_ptr<const AntennaResult>
                                          { return std::make_shared<const AntennaResult>(solveAntenna(elements, settings)); }));
    emit busyChanged();
}

void AntennaScene::onSolved()
{
    m_result = m_watcher.result();
    m_solving = false;
    emit busyChanged();
    emit resultChanged();
    emit readingsChanged();
    if (m_pending)
    {
        m_pending = false;
        solve();
    }
}

void AntennaScene::waitForResult()
{
    if (m_debounce.isActive())
    {
        m_debounce.stop();
        solve();
    }
    // Until the result is delivered (onSolved), not only computed.
    while (m_solving || m_pending)
    {
        m_watcher.waitForFinished();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
}

void AntennaScene::runSweep()
{
    if (m_sweeping || m_sources.empty()) return;
    const std::vector<AntennaElement> elements = m_sources;
    const AntennaSettings settings = m_settings;
    std::vector<double> frequencies = sweepFrequencies(settings);
    m_sweepProgress = 0.0;
    emit sweepProgressChanged();
    m_runningElements = elements;
    m_runningSettings = settings;
    m_sweeping = true;
    m_sweepWatcher.setFuture(QtConcurrent::mapped(
        std::move(frequencies), [elements, settings](double f) { return solveSweepPoint(elements, settings, f); }));
    emit sweepRunningChanged();
}

// The sweep depends on the elements and on every setting but the analysis frequency.
bool AntennaScene::computeSweepStale() const
{
    if (!m_sweep) return false;
    AntennaSettings now = m_settings, then = m_sweepSettings;
    now.frequency = then.frequency = 0.0;
    return !(m_sources == m_sweepElements) || !(now == then);
}

void AntennaScene::updateSweepStale()
{
    const bool stale = computeSweepStale();
    if (stale == m_sweepStale) return;
    m_sweepStale = stale;
    emit sweepChanged();
}

void AntennaScene::cancelSweep()
{
    m_sweepWatcher.cancel();
}

void AntennaScene::onSweepFinished()
{
    if (!m_sweepWatcher.isCanceled())
    {
        const QList<SweepPoint> points = m_sweepWatcher.future().results();
        m_sweep = std::make_shared<const std::vector<SweepPoint>>(points.begin(), points.end());
        m_sweepElements = m_runningElements;
        m_sweepSettings = m_runningSettings;
        m_sweepProgress = 1.0;
    }
    m_sweepStale = computeSweepStale();
    m_sweeping = false;
    emit sweepRunningChanged();
    emit sweepProgressChanged();
    emit sweepChanged();
}

// ---- Yagi optimiser ----

QStringList AntennaScene::optimiserGoals() const
{
    return {tr("Gain maximal"), tr("Rapport avant/arrière maximal"), tr("Compromis gain / avant-arrière")};
}

void AntennaScene::optimise(int goal, bool match, int budget)
{
    if (m_optimising || m_sources.size() < 2) return;
    const YagiGoal::Kind kinds[3] = {YagiGoal::Kind::Gain, YagiGoal::Kind::FrontToBack, YagiGoal::Kind::Balanced};
    YagiOptimizerOptions options;
    options.goal = yagiGoal(kinds[std::clamp(goal, 0, 2)], match);
    options.maxEvaluations = std::clamp(budget, 20, 2000);
    const std::vector<AntennaElement> elements = m_sources;
    const AntennaSettings settings = m_settings;
    m_optimiseStart = elements;
    m_cancelOptimise = false;
    m_optimising = true;
    m_optimiseProgress = 0.0;
    m_optimiserText = tr("Démarrage…");
    m_optimiserResult.clear();
    emit optimiserChanged();
    m_optimiseWatcher.setFuture(QtConcurrent::run(
        [this, elements, settings, options]
        {
            return optimiseYagi(elements, settings, options,
                                [this](int evaluation, int total, const SweepPoint &best)
                                {
                                    // Worker thread: the scene takes the news on its own thread.
                                    QMetaObject::invokeMethod(
                                        this,
                                        [this, evaluation, total, best]
                                        {
                                            if (!m_optimising) return;
                                            m_optimiseProgress = double(evaluation) / double(std::max(1, total));
                                            m_optimiserText = tr("Essai %1 / %2 · meilleur : %3")
                                                                  .arg(evaluation)
                                                                  .arg(total)
                                                                  .arg(pointText(best));
                                            emit optimiserChanged();
                                        },
                                        Qt::QueuedConnection);
                                    return !m_cancelOptimise.load();
                                });
        }));
}

void AntennaScene::onOptimised()
{
    const YagiOptimizerResult r = m_optimiseWatcher.result();
    m_optimising = false;
    m_optimiseProgress = 1.0;
    auto row = [](const QString &name, double before, double after, const QString &unit)
    { return QVariantMap{{"name", name}, {"before", before}, {"after", after}, {"unit", unit}}; };
    QVariantMap map{{"valid", r.valid},
                    {"cancelled", r.cancelled},
                    {"improved", r.improved},
                    {"evaluations", r.evaluations},
                    {"variables", r.variables},
                    {"seconds", r.milliseconds / 1000.0}};
    QString text;
    if (!r.valid)
        text = QString::fromStdString(r.error);
    else
    {
        map.insert(QStringLiteral("rows"),
                   QVariantList{row(tr("Gain"), r.before.gainDbi, r.after.gainDbi, QStringLiteral("dBi")),
                                row(tr("Av/Ar"), r.before.frontToBackDb, r.after.frontToBackDb, QStringLiteral("dB")),
                                row(QStringLiteral("|S11|"), -r.before.returnLossDb, -r.after.returnLossDb, QStringLiteral("dB")),
                                row(tr("TOS"), r.before.vswr, r.after.vswr, QString()),
                                row(QStringLiteral("R"), r.before.impedance.real(), r.after.impedance.real(), QStringLiteral("Ω")),
                                row(QStringLiteral("X"), r.before.impedance.imag(), r.after.impedance.imag(), QStringLiteral("Ω"))});
        bool applied = false;
        if (r.improved && m_sources == m_optimiseStart)
        {
            State next = current();
            next.sources = r.elements;
            pushState(std::move(next), tr("Optimiser la Yagi"));
            applied = true;
        }
        map.insert(QStringLiteral("applied"), applied);
        const QString how = tr("%1 essais sur %2 variables, %3 s").arg(r.evaluations).arg(r.variables).arg(significant(r.milliseconds / 1000.0, 3));
        if (applied)
            text = tr("Appliqué (Ctrl+Z pour revenir) — %1%2.").arg(how, r.cancelled ? tr(", arrêté avant la fin") : QString());
        else if (!r.improved)
            text = tr("Pas de meilleure solution trouvée — %1.").arg(how);
        else
            text = tr("La scène a changé pendant la recherche : résultat non appliqué — %1.").arg(how);
    }
    m_optimiserText = text;
    m_optimiserResult = map;
    emit optimiserChanged();
}

// ---- Validation bench ----

void AntennaScene::runBench()
{
    if (m_benchRunning) return;
    const AntennaSettings settings = m_settings;
    m_benchRunning = true;
    m_benchProgress = 0.0;
    emit benchChanged();
    m_benchWatcher.setFuture(QtConcurrent::run(
        [this, settings]
        {
            return runAntennaBench(settings,
                                   [this](int done, int total)
                                   {
                                       QMetaObject::invokeMethod(
                                           this,
                                           [this, done, total]
                                           {
                                               m_benchProgress = double(done) / double(std::max(1, total));
                                               emit benchChanged();
                                           },
                                           Qt::QueuedConnection);
                                   });
        }));
}

void AntennaScene::onBenchFinished()
{
    const AntennaBenchReport report = m_benchWatcher.result();
    m_benchRunning = false;
    m_benchProgress = 1.0;
    m_benchCases.clear();
    for (const auto &c : report.cases)
        m_benchCases << QVariantMap{{"group", QString::fromStdString(c.group)},
                                    {"quantity", QString::fromStdString(c.quantity)},
                                    {"value", benchValue(c.value, c.unit)},
                                    {"reference", benchValue(c.reference, c.unit)},
                                    {"deviation", benchDeviation(c.deviation, c.relative, c.unit)},
                                    {"passLimit", benchDeviation(c.passLimit, c.relative, c.unit)},
                                    {"warnLimit", benchDeviation(c.warnLimit, c.relative, c.unit)},
                                    {"source", QString::fromStdString(c.source)},
                                    {"note", QString::fromStdString(c.note)},
                                    {"verdict", static_cast<int>(c.verdict)},
                                    {"verdictName", QString::fromUtf8(benchVerdictName(c.verdict))}};
    m_benchSummary = tr("%1 conformes · %2 écarts limites · %3 non conformes — maillage %4 segments par λ, %5 ms")
                         .arg(report.passed)
                         .arg(report.warnings)
                         .arg(report.failed)
                         .arg(significant(report.segmentsPerWavelength, 4))
                         .arg(QString::number(report.milliseconds, 'f', 0));
    emit benchChanged();
}
} // namespace qsig::app
