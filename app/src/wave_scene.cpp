#include "qsig/app/wave_scene.h"

#include "qsig/constants.h"

#include <QLocale>
#include <QLineF>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace qsig::app
{
namespace
{
using Type = WaveObject::Type;
constexpr int kTypeCount = static_cast<int>(Type::ConductorDrawing) + 1;
constexpr double kPi = 3.14159265358979323846;

struct TypeInfo
{
    const char *key;       // stable identifier in project files
    const char *name;      // French name shown in the interface
    const char *shortName; // prefix of automatic names
};

// Indexed by WaveObject::Type.
constexpr std::array<TypeInfo, kTypeCount> kTypes{{
    {"point_source", QT_TRANSLATE_NOOP("Waves", "Source ponctuelle"), "Source"},
    {"line_source", QT_TRANSLATE_NOOP("Waves", "Source linéaire (front plan)"), "Ligne"},
    {"dielectric_block", QT_TRANSLATE_NOOP("Waves", "Bloc diélectrique"), "Bloc"},
    {"dielectric_disk", QT_TRANSLATE_NOOP("Waves", "Disque diélectrique"), "Disque"},
    {"conductor_block", QT_TRANSLATE_NOOP("Waves", "Bloc conducteur (PEC)"), "Conducteur"},
    {"conductor_disk", QT_TRANSLATE_NOOP("Waves", "Cylindre conducteur (PEC)"), "Cylindre"},
    {"slit_wall", QT_TRANSLATE_NOOP("Waves", "Écran à fentes (PEC)"), "Écran"},
    {"dielectric_drawing", QT_TRANSLATE_NOOP("Waves", "Zone dessinée diélectrique"), "Dessin"},
    {"conductor_drawing", QT_TRANSLATE_NOOP("Waves", "Zone dessinée conductrice (PEC)"), "Dessin"},
}};

Type typeFromInt(int type)
{
    return static_cast<Type>(std::clamp(type, 0, kTypeCount - 1));
}
const TypeInfo &info(Type type)
{
    return kTypes[static_cast<std::size_t>(type)];
}
bool isDielectric(Type t)
{
    return t == Type::DielectricBlock || t == Type::DielectricDisk || t == Type::DielectricDrawing;
}
// Bounding box of a drawing in world coordinates, strokes widened by their half width.
QRectF drawingBounds(const WaveObject &o)
{
    double x0 = 1e300, x1 = -1e300, y0 = 1e300, y1 = -1e300;
    for (const auto &st : o.strokes)
        for (std::size_t k = 0; k + 1 < st.points.size(); k += 2)
        {
            const double x = o.position.x + st.points[k], y = o.position.y + st.points[k + 1], h = 0.5 * st.width;
            x0 = std::min(x0, x - h);
            x1 = std::max(x1, x + h);
            y0 = std::min(y0, y - h);
            y1 = std::max(y1, y + h);
        }
    return x0 <= x1 ? QRectF(QPointF(x0, y0), QPointF(x1, y1)) : QRectF();
}

QString number(double v, int decimals)
{
    return QString::number(v, 'f', decimals).replace(QLatin1Char('.'), QLatin1Char(','));
}

// Every number of an object, in files and in the inspector map.
struct NumberField
{
    const char *key;
    double WaveObject::*member;
};
constexpr std::array<NumberField, 13> kNumbers{{
    {"frequency", &WaveObject::frequency},
    {"phaseDeg", &WaveObject::phaseDeg},
    {"width", &WaveObject::width},
    {"height", &WaveObject::height},
    {"radius", &WaveObject::radius},
    {"length", &WaveObject::length},
    {"thickness", &WaveObject::thickness},
    {"angleDeg", &WaveObject::angleDeg},
    {"slitWidth", &WaveObject::slitWidth},
    {"slitSpacing", &WaveObject::slitSpacing},
    {"epsilonR", &WaveObject::epsilonR},
    {"muR", &WaveObject::muR},
    {"conductivity", &WaveObject::conductivity},
}};

// Settings changes share the undo stack of the objects.
class SettingsCommand final : public QUndoCommand
{
public:
    SettingsCommand(WaveScene *scene, WaveSettings before, WaveSettings after, const QString &text)
        : QUndoCommand(text), m_scene(scene), m_before(before), m_after(after)
    {
    }
    void undo() override { m_scene->applySettings(m_before); }
    void redo() override { m_scene->applySettings(m_after); }

private:
    WaveScene *m_scene;
    WaveSettings m_before, m_after;
};

QPolygonF rectangle(double cx, double cy, double w, double h, double angleDeg)
{
    const double a = angleDeg * kPi / 180.0, c = std::cos(a), s = std::sin(a);
    QPolygonF p;
    for (const auto &corner : {QPointF(-0.5, -0.5), QPointF(0.5, -0.5), QPointF(0.5, 0.5), QPointF(-0.5, 0.5)})
    {
        const double u = corner.x() * w, v = corner.y() * h;
        p << QPointF(cx + u * c - v * s, cy + u * s + v * c);
    }
    p << p.front();
    return p;
}

QPolygonF disk(double cx, double cy, double r)
{
    QPolygonF p;
    constexpr int segments = 72;
    for (int k = 0; k <= segments; ++k)
    {
        const double a = 2.0 * kPi * k / segments;
        p << QPointF(cx + r * std::cos(a), cy + r * std::sin(a));
    }
    return p;
}

WaveSettings exampleSettings(const QString &key)
{
    WaveSettings s;
    if (key == QLatin1String("scattering"))
    {
        s.width = 2.4;
        s.height = 1.6;
    }
    else if (key == QLatin1String("cavity"))
    {
        s.width = 1.0;
        s.height = 0.7;
        s.cellsPerWavelength = 40.0;
        s.boundary = 2;
    }
    return s;
}
} // namespace

WaveScene::WaveScene(QObject *parent) : SourceListScene(parent), m_simulation(std::make_shared<WaveSimulation>())
{
    m_tick.setInterval(16);
    m_tick.setTimerType(Qt::PreciseTimer);
    connect(&m_tick, &QTimer::timeout, this, &WaveScene::tick);
    connect(&m_watcher, &QFutureWatcher<std::shared_ptr<const WaveFrame>>::finished, this, &WaveScene::onFrame);
    connect(this, &SceneBase::sceneChanged, this,
            [this]
            {
                m_dirty = true;
                watchPulses();
            });
    m_readingsClock.start();
}

WaveScene::~WaveScene()
{
    m_tick.stop();
    m_watcher.waitForFinished();
}

QString WaveScene::title() const
{
    return tr("Ondes FDTD 2D");
}

QString WaveScene::fieldName() const
{
    return m_settings.polarization == 1 ? QStringLiteral("Hz") : QStringLiteral("Ez");
}

QString WaveScene::fieldUnitName() const
{
    return m_settings.polarization == 1 ? QStringLiteral("A/m") : QStringLiteral("V/m");
}

QStringList WaveScene::quantityNames() const
{
    return {QStringLiteral("|%1|").arg(fieldName()), fieldName()};
}

std::shared_ptr<const LiveFrame> WaveScene::liveFrame() const
{
    return m_frame ? m_frame->picture : nullptr;
}

QRectF WaveScene::viewBounds(ViewPlane) const
{
    return {-0.5 * m_settings.width, -0.5 * m_settings.height, m_settings.width, m_settings.height};
}

// ---- Description of the objects ----

QStringList WaveScene::typeNames() const
{
    QStringList names;
    for (const auto &t : kTypes) names << QCoreApplication::translate("Waves", t.name);
    return names;
}

const char *WaveScene::typeKey(Type type) const
{
    return info(type).key;
}

QString WaveScene::shortName(Type type) const
{
    return QString::fromUtf8(info(type).shortName);
}

WaveObject WaveScene::makeSource(Type type) const
{
    WaveObject o;
    o.type = type;
    o.strength = 0.0; // obstacles: grey in the list
    switch (type)
    {
    case Type::PointSource:
        o.strength = 1.0;
        break;
    case Type::LineSource:
        o.strength = 1.0;
        o.length = 1.0;
        o.angleDeg = 90.0;
        break;
    case Type::DielectricBlock:
        o.width = 0.4;
        o.height = 0.3;
        break;
    case Type::DielectricDisk:
        o.radius = 0.2;
        break;
    case Type::ConductorBlock:
        o.width = 0.3;
        o.height = 0.3;
        break;
    case Type::ConductorDisk:
        o.radius = 0.15;
        break;
    case Type::SlitWall:
        o.length = 1.6;
        o.thickness = 0.04;
        o.angleDeg = 90.0;
        o.slits = 2;
        o.slitWidth = 0.06;
        o.slitSpacing = 0.3;
        break;
    case Type::DielectricDrawing:
    case Type::ConductorDrawing:
    {
        // Added from the list: a round blot, to be extended with the brush.
        WaveStroke blot;
        blot.points = {0.0, 0.0};
        blot.width = 0.15;
        o.strokes = {blot};
        break;
    }
    }
    return o;
}

QStringList WaveScene::editableFields(int typeValue) const
{
    const QStringList excitation{"frequency", "phaseDeg", "waveform", "injection"};
    const QStringList material{"epsilonR", "muR", "conductivity"};
    switch (typeFromInt(typeValue))
    {
    case Type::PointSource:
        return excitation;
    case Type::LineSource:
        return excitation + QStringList{"length", "angleDeg"};
    case Type::DielectricBlock:
        return material + QStringList{"width", "height", "angleDeg"};
    case Type::DielectricDisk:
        return material + QStringList{"radius"};
    case Type::ConductorBlock:
        return {"width", "height", "angleDeg"};
    case Type::ConductorDisk:
        return {"radius"};
    case Type::SlitWall:
        return {"length", "thickness", "slits", "slitWidth", "slitSpacing", "angleDeg"};
    case Type::DielectricDrawing:
        return material;
    case Type::ConductorDrawing:
        return {};
    }
    return {};
}

QString WaveScene::fieldLabel(int typeValue, const QString &key) const
{
    const Type type = typeFromInt(typeValue);
    if (key == QLatin1String("frequency")) return tr("f — fréquence");
    if (key == QLatin1String("phaseDeg")) return tr("φ — phase");
    if (key == QLatin1String("waveform")) return tr("Forme du signal");
    if (key == QLatin1String("injection")) return tr("Injection");
    if (key == QLatin1String("length")) return tr("Longueur");
    if (key == QLatin1String("width")) return tr("Largeur");
    if (key == QLatin1String("height")) return tr("Hauteur");
    if (key == QLatin1String("radius")) return tr("Rayon");
    if (key == QLatin1String("thickness")) return tr("Épaisseur");
    if (key == QLatin1String("slits")) return tr("Nombre de fentes");
    if (key == QLatin1String("slitWidth")) return tr("a — largeur d'une fente");
    if (key == QLatin1String("slitSpacing")) return tr("d — distance entre les fentes (axe à axe)");
    if (key == QLatin1String("epsilonR")) return tr("εr — permittivité relative");
    if (key == QLatin1String("muR")) return tr("μr — perméabilité relative");
    if (key == QLatin1String("conductivity")) return tr("σ — conductivité (pertes)");
    if (key == QLatin1String("angleDeg"))
        return type == Type::LineSource || type == Type::SlitWall ? tr("Orientation (90° : verticale)") : tr("Rotation");
    return key;
}

QString WaveScene::fieldUnit(int, const QString &key) const
{
    if (key == QLatin1String("frequency")) return QStringLiteral("Hz");
    if (key == QLatin1String("phaseDeg") || key == QLatin1String("angleDeg")) return QStringLiteral("°");
    if (key == QLatin1String("conductivity")) return QStringLiteral("S/m");
    if (key == QLatin1String("waveform") || key == QLatin1String("injection") || key == QLatin1String("slits") ||
        key == QLatin1String("epsilonR") || key == QLatin1String("muR"))
        return {};
    return QStringLiteral("m");
}

QStringList WaveScene::fieldChoices(int, const QString &key) const
{
    if (key == QLatin1String("waveform"))
        return {tr("Sinus continu"), tr("Impulsion gaussienne"), tr("Ondelette de Ricker")};
    if (key == QLatin1String("injection")) return {tr("Douce (ajoutée au champ)"), tr("Forcée (champ imposé)")};
    return {};
}

QString WaveScene::fieldsTitle(int typeValue) const
{
    const Type type = typeFromInt(typeValue);
    if (type == Type::PointSource || type == Type::LineSource) return tr("Excitation");
    if (type == Type::DielectricDrawing) return tr("Matériau");
    if (isDielectric(type)) return tr("Matériau et forme");
    return tr("Forme");
}

QString WaveScene::strengthSymbol(int typeValue) const
{
    const Type type = typeFromInt(typeValue);
    return type == Type::PointSource || type == Type::LineSource ? QStringLiteral("A") : QString();
}

QString WaveScene::strengthUnit(int typeValue) const
{
    return strengthSymbol(typeValue).isEmpty() ? QString() : fieldUnitName();
}

QString WaveScene::strengthLabel(int typeValue) const
{
    return strengthSymbol(typeValue).isEmpty() ? QString() : tr("A — amplitude (sur %1)").arg(fieldName());
}

QString WaveScene::summary(const WaveObject &s) const
{
    switch (s.type)
    {
    case Type::PointSource:
    case Type::LineSource:
    {
        QString text = Units::formatQuantity(s.frequency, QStringLiteral("Hz"), 3);
        if (s.waveform == 1) text += tr(" impulsion");
        if (s.waveform == 2) text += tr(" Ricker");
        return text;
    }
    case Type::DielectricBlock:
    case Type::DielectricDisk:
    case Type::DielectricDrawing:
        return QStringLiteral("εr = %1").arg(Units::formatQuantity(s.epsilonR, QString(), 3)) +
               (s.conductivity > 0.0 ? QStringLiteral(" · σ") : QString());
    case Type::SlitWall:
        if (s.slits == 0) return tr("paroi");
        return s.slits == 1 ? tr("1 fente") : tr("%1 fentes").arg(s.slits);
    default:
        return QStringLiteral("PEC");
    }
}

bool WaveScene::setField(WaveObject &s, const QString &key, double v, bool &dimension) const
{
    dimension = false;
    auto positive = [&](double &member)
    {
        if (!(v > 0.0)) return false;
        member = v;
        return true;
    };
    if (key == QLatin1String("frequency")) return positive(s.frequency);
    if (key == QLatin1String("phaseDeg"))
    {
        s.phaseDeg = v;
        return true;
    }
    if (key == QLatin1String("angleDeg"))
    {
        s.angleDeg = v;
        return true;
    }
    if (key == QLatin1String("waveform"))
    {
        s.waveform = std::clamp(static_cast<int>(std::lround(v)), 0, 2);
        return true;
    }
    if (key == QLatin1String("injection"))
    {
        s.injection = std::clamp(static_cast<int>(std::lround(v)), 0, 1);
        return true;
    }
    if (key == QLatin1String("slits"))
    {
        s.slits = std::clamp(static_cast<int>(std::lround(v)), 0, 20);
        return true;
    }
    // A medium slower than vacuum keeps the time step stable: eps_r, mu_r >= 1.
    if (key == QLatin1String("epsilonR"))
    {
        s.epsilonR = std::max(1.0, v);
        return true;
    }
    if (key == QLatin1String("muR"))
    {
        s.muR = std::max(1.0, v);
        return true;
    }
    if (key == QLatin1String("conductivity"))
    {
        s.conductivity = std::max(0.0, v);
        return true;
    }
    if (key == QLatin1String("width")) return positive(s.width);
    if (key == QLatin1String("height")) return positive(s.height);
    if (key == QLatin1String("radius")) return positive(s.radius);
    if (key == QLatin1String("length")) return positive(s.length);
    if (key == QLatin1String("thickness")) return positive(s.thickness);
    if (key == QLatin1String("slitWidth")) return positive(s.slitWidth);
    if (key == QLatin1String("slitSpacing")) return positive(s.slitSpacing);
    return false;
}

void WaveScene::addFields(QVariantMap &map, const WaveObject &s) const
{
    for (const auto &f : kNumbers) map.insert(QString::fromLatin1(f.key), s.*(f.member));
    map.insert(QStringLiteral("waveform"), s.waveform);
    map.insert(QStringLiteral("injection"), s.injection);
    map.insert(QStringLiteral("slits"), s.slits);
}

void WaveScene::writeFields(QJsonObject &o, const WaveObject &s) const
{
    for (const auto &f : kNumbers) o.insert(QString::fromLatin1(f.key), s.*(f.member));
    o.insert(QStringLiteral("waveform"), s.waveform);
    o.insert(QStringLiteral("injection"), s.injection);
    o.insert(QStringLiteral("slits"), s.slits);
    if (s.strokes.empty()) return;
    QJsonArray strokes;
    for (const auto &st : s.strokes)
    {
        QJsonArray points;
        for (double v : st.points) points.append(v);
        strokes.append(QJsonObject{{"width", st.width}, {"erase", st.erase}, {"points", points}});
    }
    o.insert(QStringLiteral("strokes"), strokes);
}

void WaveScene::readFields(const QJsonObject &o, WaveObject &s) const
{
    for (const auto &f : kNumbers) s.*(f.member) = o.value(QString::fromLatin1(f.key)).toDouble(s.*(f.member));
    s.waveform = std::clamp(o.value(QStringLiteral("waveform")).toInt(s.waveform), 0, 2);
    s.injection = std::clamp(o.value(QStringLiteral("injection")).toInt(s.injection), 0, 1);
    s.slits = std::clamp(o.value(QStringLiteral("slits")).toInt(s.slits), 0, 20);
    s.epsilonR = std::max(1.0, s.epsilonR);
    s.muR = std::max(1.0, s.muR);
    s.conductivity = std::max(0.0, s.conductivity);
    if (o.contains(QStringLiteral("strokes")))
    {
        s.strokes.clear();
        for (const auto &value : o.value(QStringLiteral("strokes")).toArray())
        {
            const QJsonObject so = value.toObject();
            WaveStroke st;
            st.width = std::max(1e-6, so.value(QStringLiteral("width")).toDouble(0.04));
            st.erase = so.value(QStringLiteral("erase")).toBool(false);
            const QJsonArray points = so.value(QStringLiteral("points")).toArray();
            for (qsizetype k = 0; k + 1 < points.size(); k += 2)
            {
                const double x = points.at(k).toDouble(), y = points.at(k + 1).toDouble();
                if (!std::isfinite(x) || !std::isfinite(y)) continue;
                st.points.push_back(x);
                st.points.push_back(y);
            }
            if (!st.points.empty()) s.strokes.push_back(std::move(st));
        }
    }
}

QString WaveScene::formula(int typeValue) const
{
    const QString f = fieldName();
    switch (typeFromInt(typeValue))
    {
    case Type::PointSource:
        return tr("Source ponctuelle : le signal s(t) est appliqué à %1 sur un nœud de la grille ; elle rayonne une onde "
                  "cylindrique dont l'amplitude décroît en 1/√r.\n"
                  "Sinus : s = A sin(2πft + φ). Impulsion gaussienne : A e^(−((t−t₀)/τ)²) sin(2πf(t−t₀)), τ = 1,25/f, "
                  "t₀ = 4/f. Ricker : A (1 − 2π²f²(t−t₀)²) e^(−π²f²(t−t₀)²), t₀ = 3/f.\n"
                  "Douce : s est ajouté au champ, la source laisse passer les ondes qui reviennent. Forcée : le champ est "
                  "imposé, la source réfléchit comme un petit obstacle.")
            .arg(f);
    case Type::LineSource:
        return tr("Source linéaire : le même signal sur tous les nœuds d'un segment. Elle émet de part et d'autre un "
                  "front quasi plan, diffracté à ses extrémités (prolongez-la dans la couche absorbante pour une onde "
                  "plane sur toute la zone).\nSignaux et injection : comme la source ponctuelle.");
    case Type::DielectricBlock:
    case Type::DielectricDisk:
        return tr("Milieu linéaire : vitesse v = c/n avec n = √(εr μr), longueur d'onde λ = λ₀/n, impédance "
                  "η = η₀ √(μr/εr). Aux interfaces : réflexion et réfraction (Snell-Descartes n₁ sin θ₁ = n₂ sin θ₂).\n"
                  "σ ajoute des pertes ohmiques : l'onde s'atténue en traversant le milieu.");
    case Type::ConductorBlock:
    case Type::ConductorDisk:
        return tr("Conducteur électrique parfait : le champ électrique tangent est nul à sa surface "
                  "(Ez = 0 en TMz ; Ex = Ey = 0 en TEz). Réflexion totale, ombre et diffraction derrière l'obstacle.");
    case Type::SlitWall:
        return tr("Écran conducteur percé de fentes. Chaque fente étroite (a ≲ λ) rayonne comme une source "
                  "(principe de Huygens) ; deux fentes d'axe à axe d donnent des franges brillantes dans les directions "
                  "sin θ = mλ/d. Sans fente, c'est une paroi conductrice.");
    case Type::DielectricDrawing:
        return tr("Zone peinte au pinceau, dans un milieu linéaire : vitesse v = c/n avec n = √(εr μr), "
                  "longueur d'onde λ = λ₀/n. Chaque trait du pinceau s'ajoute à la zone, la gomme en retire ; "
                  "la zone se déplace d'un seul bloc.");
    case Type::ConductorDrawing:
        return tr("Zone conductrice parfaite peinte au pinceau : champ électrique tangent nul, réflexion "
                  "totale. Chaque trait s'ajoute à la zone, la gomme en retire ; la zone se déplace d'un seul bloc. "
                  "Un trait fin reste continu (au moins une cellule et demie).");
    }
    return {};
}

QString WaveScene::accuracy(int typeValue) const
{
    const Type type = typeFromInt(typeValue);
    if (type == Type::PointSource || type == Type::LineSource) return tr("Schéma de Yee, ordre 2");
    if (isDielectric(type)) return tr("Interface en marches d'une cellule");
    if (type == Type::ConductorDrawing) return tr("Conducteur parfait, contour en marches");
    return tr("Conducteur parfait, contour en marches");
}

// ---- Outline ----

SourceOutline WaveScene::outline(int row, ViewPlane, double) const
{
    SourceOutline o;
    if (row < 0 || row >= count()) return o;
    const WaveObject &s = at(row);
    const double cx = s.position.x, cy = s.position.y;
    o.centre = {cx, cy};
    const QColor dielectric(84, 204, 232), metal(178, 184, 194);
    switch (s.type)
    {
    case Type::PointSource:
        o.sign = 1;
        break;
    case Type::LineSource:
    {
        o.sign = 1;
        const double a = s.angleDeg * kPi / 180.0;
        const QPointF d(0.5 * s.length * std::cos(a), 0.5 * s.length * std::sin(a));
        QPolygonF segment;
        segment << o.centre - d << o.centre + d;
        o.lines << segment;
        break;
    }
    case Type::DielectricBlock:
        o.colour = dielectric;
        o.filled << rectangle(cx, cy, s.width, s.height, s.angleDeg);
        break;
    case Type::DielectricDisk:
        o.colour = dielectric;
        o.filled << disk(cx, cy, s.radius);
        break;
    case Type::ConductorBlock:
        o.colour = metal;
        o.solid = true;
        o.filled << rectangle(cx, cy, s.width, s.height, s.angleDeg);
        break;
    case Type::ConductorDisk:
        o.colour = metal;
        o.solid = true;
        o.filled << disk(cx, cy, s.radius);
        break;
    case Type::SlitWall:
    {
        o.colour = metal;
        o.solid = true;
        const double a = s.angleDeg * kPi / 180.0, c = std::cos(a), sn = std::sin(a);
        // The pieces of wall between the slits, along the wall.
        const int n = std::clamp(s.slits, 0, 20);
        double from = -0.5 * s.length;
        auto piece = [&](double begin, double end)
        {
            if (end - begin <= 0.0) return;
            const double mid = 0.5 * (begin + end);
            o.filled << rectangle(cx + mid * c, cy + mid * sn, end - begin, s.thickness, s.angleDeg);
        };
        for (int k = 0; k < n; ++k)
        {
            const double centre = (k - 0.5 * (n - 1)) * s.slitSpacing;
            piece(from, std::min(centre - 0.5 * s.slitWidth, 0.5 * s.length));
            from = std::max(from, centre + 0.5 * s.slitWidth);
        }
        piece(from, 0.5 * s.length);
        break;
    }
    case Type::DielectricDrawing:
    case Type::ConductorDrawing:
        o.colour = s.type == Type::ConductorDrawing ? metal : dielectric;
        o.solid = s.type == Type::ConductorDrawing;
        for (const auto &st : s.strokes)
        {
            SourceOutline::Stroke stroke;
            stroke.width = st.width;
            stroke.erase = st.erase;
            for (std::size_t k = 0; k + 1 < st.points.size(); k += 2)
                stroke.points << QPointF(cx + st.points[k], cy + st.points[k + 1]);
            o.strokes << stroke;
        }
        break;
    }
    return o;
}

// ---- Probe ----

QVariantMap WaveScene::probe(double, double, double) const
{
    const QString f = fieldName(), unit = fieldUnitName();
    const bool tm = m_settings.polarization != 1;
    const QString inPlaneUnit = tm ? QStringLiteral("A/m") : QStringLiteral("V/m");
    static const WaveFrame none;
    const WaveFrame &frame = m_frame ? *m_frame : none;
    const WaveProbe &a = frame.probe;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    auto row = [](const QString &name, double value, const QString &u, bool strong)
    { return QVariantMap{{"name", name}, {"value", value}, {"unit", u}, {"strong", strong}}; };
    const QVariantList rows{
        row(f, a.valid ? a.primary : nan, unit, true),
        row(QStringLiteral("|%1|").arg(f), a.valid ? a.amplitude : nan, unit, false),
        row(tm ? QStringLiteral("Hx") : QStringLiteral("Ex"), a.valid ? a.inPlaneU : nan, inPlaneUnit, false),
        row(tm ? QStringLiteral("Hy") : QStringLiteral("Ey"), a.valid ? a.inPlaneV : nan, inPlaneUnit, false),
    };
    auto values = [](const WaveProbe &p)
    {
        QVariantList list;
        for (double v : p.trace) list << v;
        return list;
    };
    QString note = a.valid ? tr("A : nœud (%1, %2) de la grille. |%3| : amplitude, √2 × valeur efficace sur "
                                "environ deux périodes.")
                                 .arg(a.i)
                                 .arg(a.j)
                                 .arg(f)
                           : tr("Le point A est hors de la zone simulée.");
    QVariantList traces;
    if (a.valid) traces << QVariantMap{{"name", QStringLiteral("A")}, {"values", values(a)}};
    QVariantList groups;
    const bool hasB = m_useB && frame.hasProbeB;
    if (hasB)
    {
        const WaveProbe &b = frame.probeB;
        const bool both = a.valid && b.valid;
        groups << QVariantMap{{"title", tr("Point B")},
                              {"rows", QVariantList{row(f, b.valid ? b.primary : nan, unit, true),
                                                    row(QStringLiteral("|%1|").arg(f), b.valid ? b.amplitude : nan,
                                                        unit, false)}}};
        // Phase of B minus phase of A at the frequency of the sinusoidal source; a positive delay means
        // that B lags behind A (defined to within a period).
        const double fp = frame.phaseFrequency;
        const bool phase = both && fp > 0.0 && std::abs(a.phasor) > 0.0 && std::abs(b.phasor) > 0.0;
        const double dphi = phase ? std::arg(b.phasor / a.phasor) * 180.0 / kPi : nan;
        const double ratio = both && a.amplitude > 0.0 && b.amplitude > 0.0 ? 20.0 * std::log10(b.amplitude / a.amplitude) : nan;
        groups << QVariantMap{{"title", tr("Différence A − B")},
                              {"rows", QVariantList{row(QStringLiteral("A − B"), both ? a.primary - b.primary : nan, unit, true),
                                                    row(QStringLiteral("|B|/|A|"), ratio, QStringLiteral("dB"), false),
                                                    row(QStringLiteral("Δφ"), dphi, QStringLiteral("°"), false),
                                                    row(QStringLiteral("Δt"), phase ? -dphi / (360.0 * fp) : nan,
                                                        QStringLiteral("s"), false)}}};
        if (b.valid) traces << QVariantMap{{"name", QStringLiteral("B")}, {"values", values(b)}};
        if (!b.valid) note += QLatin1Char('\n') + tr("Le point B est hors de la zone simulée.");
        else if (fp > 0.0)
            note += QLatin1Char('\n') + tr("Δφ et Δt à %1 (première source sinusoïdale) : phase de B moins celle de A, "
                                           "Δt > 0 quand B est en retard ; définis à une période près.")
                                            .arg(Units::formatQuantity(fp, QStringLiteral("Hz"), 4));
        else
            note += QLatin1Char('\n') + tr("Δφ et Δt demandent une source sinusoïdale.");
    }
    const WaveProbe &shown = a.valid ? a : frame.probeB;
    return {{"rows", rows},
            {"groups", groups},
            {"note", note},
            {"traces", traces},
            {"traceName", f},
            {"traceUnit", unit},
            {"traceSpan", shown.traceStep * static_cast<double>(shown.trace.size())}};
}

void WaveScene::setProbePoints(QVector3D a, bool useB, QVector3D b)
{
    const double ax = a.x(), ay = a.y(), bx = b.x(), by = b.y();
    if (ax == m_probeX && ay == m_probeY && useB == m_useB && bx == m_probeBX && by == m_probeBY) return;
    m_probeX = ax;
    m_probeY = ay;
    m_useB = useB;
    m_probeBX = bx;
    m_probeBY = by;
    m_probeDirty = true;
}

QString WaveScene::quantityTitle(int quantity) const
{
    return quantity == 0 ? tr("Amplitude de %1").arg(fieldName()) : tr("%1 instantané").arg(fieldName());
}

QString WaveScene::quantityUnit(int) const
{
    return fieldUnitName();
}

// ---- Examples ----

QVariantList WaveScene::examples() const
{
    auto e = [](const char *key, const QString &name) { return QVariantMap{{"key", key}, {"name", name}, {"plane", 0}}; };
    return {e("point_source", tr("Source ponctuelle : onde cylindrique")),
            e("two_sources", tr("Deux sources en phase : interférences")),
            e("double_slit", tr("Fentes d'Young : diffraction et franges")),
            e("refraction", tr("Dioptre incliné : réflexion et réfraction")),
            e("lens", tr("Lentille boule : focalisation")),
            e("waveguide", tr("Guide à plaques parallèles")),
            e("scattering", tr("Impulsion diffractée par un cylindre")),
            e("cavity", tr("Cavité résonante : mode TM₃₂"))};
}

std::vector<WaveObject> WaveScene::example(const QString &key) const
{
    auto object = [this](Type type, const char *name, double x, double y)
    {
        WaveObject o = makeSource(type);
        o.name = name;
        o.position = {x, y, 0.0};
        return o;
    };
    // A vertical line source crossing the whole area and its absorbing layers: a plane wave.
    auto planeWave = [&](double x)
    {
        WaveObject o = object(Type::LineSource, "Onde plane", x, 0.0);
        o.length = 2.6;
        return o;
    };
    if (key == QLatin1String("point_source")) return {object(Type::PointSource, "Source", 0.0, 0.0)};
    if (key == QLatin1String("two_sources"))
        return {object(Type::PointSource, "Source 1", -1.0, 0.3), object(Type::PointSource, "Source 2", -1.0, -0.3)};
    if (key == QLatin1String("double_slit"))
    {
        WaveObject wall = object(Type::SlitWall, "Écran", -0.8, 0.0);
        wall.length = 2.6;
        wall.slitSpacing = 0.6;
        return {planeWave(-1.25), wall};
    }
    if (key == QLatin1String("refraction"))
    {
        WaveObject glass = object(Type::DielectricBlock, "Milieu n = 2", 1.0, 0.0);
        glass.width = 2.0;
        glass.height = 3.6;
        glass.angleDeg = 25.0;
        glass.epsilonR = 4.0;
        return {planeWave(-1.25), glass};
    }
    if (key == QLatin1String("lens"))
    {
        WaveObject ball = object(Type::DielectricDisk, "Lentille", -0.3, 0.0);
        ball.radius = 0.35;
        ball.epsilonR = 2.5;
        return {planeWave(-1.25), ball};
    }
    if (key == QLatin1String("waveguide"))
    {
        WaveObject top = object(Type::ConductorBlock, "Plaque haute", -0.75, 0.24);
        top.width = 1.9;
        top.height = 0.04;
        WaveObject bottom = top;
        bottom.name = "Plaque basse";
        bottom.position.y = -0.24;
        return {top, bottom, object(Type::PointSource, "Source", -1.1, 0.0)};
    }
    if (key == QLatin1String("scattering"))
    {
        WaveObject pulse = object(Type::PointSource, "Impulsion", -0.8, 0.0);
        pulse.waveform = 2;
        WaveObject cylinder = object(Type::ConductorDisk, "Cylindre", 0.3, 0.0);
        cylinder.radius = 0.2;
        return {pulse, cylinder};
    }
    if (key == QLatin1String("cavity"))
    {
        // TM32 mode of the cavity: Ez = sin(3 pi x'/a) sin(2 pi y'/b), the walls being the outer nodes
        // of the grid (a = width + dx, b = height + dy). Driven on an antinode at its frequency.
        const WaveSettings s = exampleSettings(key);
        WaveObject source = object(Type::PointSource, "Source", 0.0, 0.0);
        for (int pass = 0; pass < 3; ++pass)
        {
            const WaveGrid g = waveGrid(s, {source});
            const double a = s.width + g.dx, b = s.height + g.dy;
            source.frequency = 0.5 * qsig::constants::c0 * std::sqrt(9.0 / (a * a) + 4.0 / (b * b));
            source.position = {-0.5 * a + a / 6.0, -0.5 * b + b / 4.0, 0.0};
        }
        return {source};
    }
    return {};
}

void WaveScene::loadExample(const QString &key)
{
    if (example(key).empty()) return;
    m_settings = exampleSettings(key);
    m_resetRequested = true;
    SourceListScene::loadExample(key);
}

// ---- Settings ----

QStringList WaveScene::settingFields() const
{
    return {"width", "height", "cellsPerWavelength", "polarization", "boundary"};
}

QString WaveScene::settingLabel(const QString &key) const
{
    if (key == QLatin1String("width")) return tr("Largeur de la zone");
    if (key == QLatin1String("height")) return tr("Hauteur de la zone");
    if (key == QLatin1String("cellsPerWavelength")) return tr("Résolution : cellules par longueur d'onde");
    if (key == QLatin1String("polarization")) return tr("Polarisation");
    if (key == QLatin1String("boundary")) return tr("Bord de la zone");
    return key;
}

QString WaveScene::settingUnit(const QString &key) const
{
    return key == QLatin1String("width") || key == QLatin1String("height") ? QStringLiteral("m") : QString();
}

QStringList WaveScene::settingChoices(const QString &key) const
{
    if (key == QLatin1String("polarization"))
        return {tr("TMz — Ez hors du plan (Hx, Hy)"), tr("TEz — Hz hors du plan (Ex, Ey)")};
    if (key == QLatin1String("boundary"))
        return {tr("Absorbant CPML (espace libre)"), tr("Absorbant de Mur (1er ordre)"), tr("Conducteur parfait (cavité)")};
    return {};
}

QVariantMap WaveScene::settingsData() const
{
    return {{"width", m_settings.width},
            {"height", m_settings.height},
            {"cellsPerWavelength", m_settings.cellsPerWavelength},
            {"polarization", m_settings.polarization},
            {"boundary", m_settings.boundary}};
}

void WaveScene::setSetting(const QString &key, const QVariant &value)
{
    bool ok = false;
    const double v = value.toDouble(&ok);
    if (!ok || !std::isfinite(v)) return;
    WaveSettings next = m_settings;
    if (key == QLatin1String("width"))
        next.width = std::clamp(v, 0.01, 1000.0);
    else if (key == QLatin1String("height"))
        next.height = std::clamp(v, 0.01, 1000.0);
    else if (key == QLatin1String("cellsPerWavelength"))
        next.cellsPerWavelength = std::clamp(v, 4.0, 100.0);
    else if (key == QLatin1String("polarization"))
        next.polarization = std::clamp(static_cast<int>(std::lround(v)), 0, 1);
    else if (key == QLatin1String("boundary"))
        next.boundary = std::clamp(static_cast<int>(std::lround(v)), 0, 2);
    else
        return;
    if (next == m_settings) return;
    m_undo.push(new SettingsCommand(this, m_settings, next, tr("Modifier la simulation")));
}

void WaveScene::applySettings(const WaveSettings &settings)
{
    m_settings = settings;
    emit sceneChanged();
}

QString WaveScene::settingsNote() const
{
    const WaveGrid g = grid();
    const double dt = 0.95 / (qsig::constants::c0 * std::sqrt(1.0 / (g.dx * g.dx) + 1.0 / (g.dy * g.dy)));
    QString text = tr("Grille de %1 × %2 cellules").arg(g.areaNx).arg(g.areaNy);
    if (g.pad > 1) text += tr(", plus %1 de couche absorbante sur chaque bord").arg(g.pad);
    text += QLatin1Char('\n') + tr("Δx = %1, soit λ/%2 à %3")
                                    .arg(Units::formatQuantity(g.dx, QStringLiteral("m"), 3))
                                    .arg(number(g.cellsPerWavelength, 1))
                                    .arg(Units::formatQuantity(g.frequency, QStringLiteral("Hz"), 3));
    text += QLatin1Char('\n') + tr("Δt = %1 (nombre de Courant 0,95)").arg(Units::formatQuantity(dt, QStringLiteral("s"), 3));
    const double n = maxRefractiveIndex(m_sources);
    if (n > 1.0001)
    {
        const double inMedium = g.cellsPerWavelength / n;
        text += QLatin1Char('\n') + tr("Dans le milieu le plus lent (n = %1) : λ/%2").arg(number(n, 2)).arg(number(inMedium, 1));
        if (inMedium < 10.0) text += tr(" — sous 10 cellules par λ, la dispersion numérique se voit");
    }
    if (g.limited) text += QLatin1Char('\n') + tr("Résolution réduite : limite de %1 cellules.").arg(kWaveMaxCells);
    return text;
}

// ---- Files ----

void WaveScene::writeExtra(QJsonObject &root) const
{
    root.insert(QStringLiteral("settings"), QJsonObject{{"width", m_settings.width},
                                                        {"height", m_settings.height},
                                                        {"cellsPerWavelength", m_settings.cellsPerWavelength},
                                                        {"polarization", m_settings.polarization},
                                                        {"boundary", m_settings.boundary}});
}

bool WaveScene::readExtra(const QJsonObject &root, QString *)
{
    const QJsonObject o = root.value(QStringLiteral("settings")).toObject();
    const WaveSettings defaults;
    WaveSettings s;
    s.width = std::clamp(o.value(QStringLiteral("width")).toDouble(defaults.width), 0.01, 1000.0);
    s.height = std::clamp(o.value(QStringLiteral("height")).toDouble(defaults.height), 0.01, 1000.0);
    s.cellsPerWavelength =
        std::clamp(o.value(QStringLiteral("cellsPerWavelength")).toDouble(defaults.cellsPerWavelength), 4.0, 100.0);
    s.polarization = std::clamp(o.value(QStringLiteral("polarization")).toInt(defaults.polarization), 0, 1);
    s.boundary = std::clamp(o.value(QStringLiteral("boundary")).toInt(defaults.boundary), 0, 2);
    m_settings = s;
    m_resetRequested = true;
    return true;
}

void WaveScene::clearSources()
{
    m_settings = WaveSettings{};
    m_resetRequested = true;
    SourceListScene::clearSources();
}

// ---- Drawing ----

QString WaveScene::automaticName(const State &state, Type type, const QString &prefix) const
{
    int same = 1;
    for (const auto &o : state.sources) same += o.type == type ? 1 : 0;
    return QStringLiteral("%1 %2").arg(prefix).arg(same);
}

int WaveScene::addStroke(int row, const QVariantList &points, double width, bool erase, int material, double epsilonR)
{
    if (!(width > 0.0) || !std::isfinite(width)) return -1;
    // Points closer than a quarter of the width add nothing to the shape of the stroke.
    std::vector<QPointF> path;
    for (const QVariant &v : points)
    {
        const QPointF p = v.toPointF();
        if (!std::isfinite(p.x()) || !std::isfinite(p.y())) continue;
        if (!path.empty() && QLineF(path.back(), p).length() < 0.25 * width) continue;
        path.push_back(p);
    }
    if (path.empty()) return -1;
    const QPointF last = points.isEmpty() ? path.back() : points.back().toPointF();
    if (path.size() > 1 && path.back() != last && std::isfinite(last.x()) && std::isfinite(last.y())) path.push_back(last);
    auto strokeFor = [&](const WaveObject &o)
    {
        WaveStroke st;
        st.width = width;
        st.erase = erase;
        for (const QPointF &p : path)
        {
            st.points.push_back(p.x() - o.position.x);
            st.points.push_back(p.y() - o.position.y);
        }
        return st;
    };
    double x0 = 1e300, x1 = -1e300, y0 = 1e300, y1 = -1e300;
    for (const QPointF &p : path)
    {
        x0 = std::min(x0, p.x());
        x1 = std::max(x1, p.x());
        y0 = std::min(y0, p.y());
        y1 = std::max(y1, p.y());
    }
    const QRectF reach = QRectF(QPointF(x0, y0), QPointF(x1, y1)).adjusted(-0.5 * width, -0.5 * width, 0.5 * width, 0.5 * width);

    State next = current();
    if (erase)
    {
        // The eraser takes paint off every drawing it crosses (blocks and discs are deleted instead).
        bool touched = false;
        for (auto &o : next.sources)
            if (o.isDrawing() && drawingBounds(o).intersects(reach))
            {
                o.strokes.push_back(strokeFor(o));
                touched = true;
            }
        if (!touched) return -1;
        pushState(std::move(next), tr("Gommer"));
        return row;
    }
    const Type wanted = material == 1 ? Type::ConductorDrawing : Type::DielectricDrawing;
    const double permittivity = std::isfinite(epsilonR) ? std::max(1.0, epsilonR) : 1.0;
    int target = -1;
    if (row >= 0 && row < count())
    {
        const WaveObject &selected = next.sources[static_cast<std::size_t>(row)];
        if (selected.type == wanted && (wanted == Type::ConductorDrawing || std::abs(selected.epsilonR - permittivity) < 1e-9))
            target = row;
    }
    if (target < 0)
    {
        WaveObject o = makeSource(wanted);
        o.strokes.clear();
        o.epsilonR = permittivity;
        o.position = {0.5 * (x0 + x1), 0.5 * (y0 + y1), 0.0};
        o.name = automaticName(next, wanted, tr("Dessin")).toStdString();
        next.sources.push_back(o);
        next.keepDensity.push_back(0);
        target = static_cast<int>(next.sources.size()) - 1;
    }
    WaveObject &drawing = next.sources[static_cast<std::size_t>(target)];
    drawing.strokes.push_back(strokeFor(drawing));
    const QString text = tr("Peindre %1").arg(QString::fromStdString(drawing.name));
    pushState(std::move(next), text);
    return target;
}

int WaveScene::addDrawnShape(const QString &shape, double x0, double y0, double x1, double y1, int material,
                             double epsilonR, double width)
{
    if (!std::isfinite(x0) || !std::isfinite(y0) || !std::isfinite(x1) || !std::isfinite(y1)) return -1;
    const bool conductor = material == 1;
    const double dx = x1 - x0, dy = y1 - y0, length = std::hypot(dx, dy);
    constexpr double tiny = 1e-4; // a click without a drag draws nothing
    State next = current();
    WaveObject o;
    QString prefix;
    if (shape == QLatin1String("rectangle"))
    {
        if (std::abs(dx) < tiny || std::abs(dy) < tiny) return -1;
        o = makeSource(conductor ? Type::ConductorBlock : Type::DielectricBlock);
        o.width = std::abs(dx);
        o.height = std::abs(dy);
        o.angleDeg = 0.0;
        o.position = {0.5 * (x0 + x1), 0.5 * (y0 + y1), 0.0};
    }
    else if (shape == QLatin1String("disk"))
    {
        if (length < tiny) return -1;
        o = makeSource(conductor ? Type::ConductorDisk : Type::DielectricDisk);
        o.radius = length;
        o.position = {x0, y0, 0.0};
    }
    else if (shape == QLatin1String("line"))
    {
        if (length < tiny || !(width > 0.0)) return -1;
        // A conducting line is a wall without slits; a dielectric one, a thin rotated block.
        o = makeSource(conductor ? Type::SlitWall : Type::DielectricBlock);
        o.angleDeg = std::atan2(dy, dx) * 180.0 / kPi;
        o.position = {0.5 * (x0 + x1), 0.5 * (y0 + y1), 0.0};
        if (conductor)
        {
            o.slits = 0;
            o.length = length;
            o.thickness = width;
            prefix = tr("Paroi");
        }
        else
        {
            o.width = length;
            o.height = width;
        }
    }
    else
        return -1;
    if (!conductor) o.epsilonR = std::isfinite(epsilonR) ? std::max(1.0, epsilonR) : 1.0;
    if (prefix.isEmpty()) prefix = shortName(o.type);
    o.name = automaticName(next, o.type, prefix).toStdString();
    next.sources.push_back(o);
    next.keepDensity.push_back(0);
    const QString text = tr("Dessiner %1").arg(QString::fromStdString(o.name));
    pushState(std::move(next), text);
    return count() - 1;
}

// ---- Live simulation ----

void WaveScene::setRunning(bool running)
{
    if (running == m_running) return;
    m_running = running;
    emit runningChanged();
    emit statsChanged();
}

void WaveScene::setSpeed(int speed)
{
    speed = std::clamp(speed, 1, 64);
    if (speed == m_speed) return;
    m_speed = speed;
    emit speedChanged();
}

void WaveScene::setActive(bool active)
{
    if (active == m_active) return;
    m_active = active;
    if (m_active)
        m_tick.start();
    else
        m_tick.stop();
    emit activeChanged();
}

void WaveScene::stepOnce()
{
    setRunning(false);
    m_stepPending = true;
}

void WaveScene::resetSimulation()
{
    m_resetRequested = true;
}

double WaveScene::time() const
{
    return m_frame ? m_frame->time : 0.0;
}

QString WaveScene::statusText() const
{
    if (!m_frame) return tr("Préparation de la simulation…");
    const WaveFrame &f = *m_frame;
    QString text = tr("t = %1 · %2 pas · grille %3 × %4")
                       .arg(Units::formatQuantity(f.time, QStringLiteral("s"), 4))
                       .arg(QLocale().toString(static_cast<qulonglong>(f.steps)))
                       .arg(f.grid.areaNx)
                       .arg(f.grid.areaNy);
    if (f.msPerStep > 0.0) text += tr(" · %1 ms par pas").arg(number(f.msPerStep, 2));
    if (!m_running) text += tr(" · en pause");
    return text;
}

// A pulse leaves its source once, from t = 0: a new or changed pulse source restarts the simulation.
void WaveScene::watchPulses()
{
    QString signature;
    for (const auto &s : m_sources)
        if (s.isSource() && s.waveform != 0)
            signature += QStringLiteral("%1:%2:%3;").arg(static_cast<int>(s.type)).arg(s.waveform).arg(s.frequency, 0, 'g', 12);
    if (signature != m_pulseSignature && !signature.isEmpty()) m_resetRequested = true;
    m_pulseSignature = signature;
}

void WaveScene::tick()
{
    if (!m_active || m_watcher.isRunning()) return;
    const bool stepping = m_running || m_stepPending;
    if (!stepping && !m_dirty && !m_resetRequested && !m_probeDirty && m_frame) return;

    struct Job
    {
        bool update = false;
        WaveSettings settings;
        std::vector<WaveObject> objects;
        bool reset = false;
        bool probe = false;
        double probeX = 0.0, probeY = 0.0;
        bool useB = false;
        double probeBX = 0.0, probeBY = 0.0;
        int steps = 0;
    } job;
    job.update = m_dirty;
    if (m_dirty)
    {
        job.settings = m_settings;
        job.objects = m_sources;
    }
    job.reset = m_resetRequested;
    job.probe = m_probeDirty;
    job.probeX = m_probeX;
    job.probeY = m_probeY;
    job.useB = m_useB;
    job.probeBX = m_probeBX;
    job.probeBY = m_probeBY;
    job.steps = m_running ? m_speed : (m_stepPending ? 1 : 0);
    m_dirty = m_resetRequested = m_probeDirty = m_stepPending = false;

    // The simulation is only ever touched by one job at a time.
    std::shared_ptr<WaveSimulation> simulation = m_simulation;
    m_watcher.setFuture(QtConcurrent::run(
        [simulation, job]() -> std::shared_ptr<const WaveFrame>
        {
            if (job.update) simulation->update(job.settings, job.objects);
            if (job.reset) simulation->reset();
            if (job.probe) simulation->setProbes(job.probeX, job.probeY, job.useB, job.probeBX, job.probeBY);
            simulation->advance(job.steps);
            return std::make_shared<const WaveFrame>(simulation->frame());
        }));
}

void WaveScene::onFrame()
{
    m_frame = m_watcher.result();
    emit frameReady();
    emit statsChanged();
    // The inspector reads the probe about ten times a second while running, at once otherwise.
    if (!m_running || m_readingsClock.elapsed() >= 100)
    {
        m_readingsClock.restart();
        emit readingsChanged();
    }
}
} // namespace qsig::app
