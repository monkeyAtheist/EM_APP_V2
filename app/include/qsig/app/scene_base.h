#pragma once

// Common ground of the field modules (electrostatics, magnetostatics): a list model of sources with
// undo/redo, files and the API the interface uses. Each module only describes its sources, its
// formulas, its drawing (SourceOutline) and its field (FieldSampler).
//
//   SceneBase                    QObject side: properties, signals, undo stack, files, drag merging
//   SourceListScene<SourceT>     the generic list of sources (no Q_OBJECT: templates cannot have one)
//   ElectrostaticScene, ...      Q_OBJECT, registered in QML

#include "qsig/app/live_frame.h"
#include "qsig/app/source_outline.h"
#include "qsig/app/source_shapes.h"
#include "qsig/app/units.h"
#include "qsig/app/view_plane.h"
#include "qsig/vec3.h"

#include <QAbstractListModel>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRectF>
#include <QUndoCommand>
#include <QUndoStack>
#include <QUrl>
#include <QVariantMap>
#include <QVector3D>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace qsig::app
{
// One sample of a field map. 'scalar' is the second displayed quantity (potential, normal component...).
struct FieldSample
{
    qsig::Vec3 field;
    double scalar = 0.0;
    bool fieldValid = false;
    bool scalarValid = false;
};
// Pure function of the point, safe to call from worker threads (it owns a copy of the sources).
using FieldSampler = std::function<FieldSample(const qsig::Vec3 &)>;

// Density form of a source strength: strength = density x measure.
struct DensityInfo
{
    QString symbol; // empty: the strength is the only way to give it
    QString unit;
    QString name;        // "volumique", "surfacique"...
    QString measureName; // "Volume", "Surface", "Section"...
    QString measureUnit;
};

class SceneBase : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY undoStateChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY undoStateChanged)
    Q_PROPERTY(QString undoText READ undoText NOTIFY undoStateChanged)
    Q_PROPERTY(QString redoText READ redoText NOTIFY undoStateChanged)
    Q_PROPERTY(QUrl fileUrl READ fileUrl NOTIFY fileUrlChanged)
    Q_PROPERTY(bool modified READ isModified NOTIFY modifiedChanged)
    // Description of the module, for the interface.
    Q_PROPERTY(QString title READ title CONSTANT)
    Q_PROPERTY(QString fileSuffix READ fileSuffix CONSTANT)
    Q_PROPERTY(QStringList quantityNames READ quantityNames NOTIFY sceneChanged)
    // Time-domain module: the view paints liveFrame() as it comes (frameReady) instead of a map.
    Q_PROPERTY(bool live READ isLive CONSTANT)
    // Two-dimensional module: no z coordinate, always seen on the XY plane.
    Q_PROPERTY(bool planar READ isPlanar CONSTANT)
    Q_PROPERTY(int defaultQuantity READ defaultQuantity CONSTANT)
    // The view offers its drawing tools (brush, eraser, shapes drawn with the mouse).
    Q_PROPERTY(bool canDraw READ canDraw CONSTANT)
    // A second measurement point B, for differential readings.
    Q_PROPERTY(bool differentialProbe READ hasDifferentialProbe CONSTANT)
    // Centre view of the module: "field" (field maps, FieldView) or "antenna" (3D, patterns, sweeps).
    Q_PROPERTY(QString viewKind READ viewKind CONSTANT)
    // The inspector has a measurement point; otherwise probe() gives the results of the module.
    Q_PROPERTY(bool hasProbe READ hasProbe CONSTANT)
    // The field view can show the scene in space (3D), besides its cutting planes.
    Q_PROPERTY(bool spaceView READ hasSpaceView CONSTANT)

public:
    enum Role
    {
        NameRole = Qt::UserRole + 1,
        TypeRole,
        TypeNameRole,
        StrengthRole,
        StrengthTextRole,
        XRole,
        YRole,
        ZRole,
    };
    Q_ENUM(Role)

    explicit SceneBase(QObject *parent = nullptr);

    virtual int count() const = 0;
    bool canUndo() const { return m_undo.canUndo(); }
    bool canRedo() const { return m_undo.canRedo(); }
    QString undoText() const { return m_undo.undoText(); }
    QString redoText() const { return m_undo.redoText(); }
    QUrl fileUrl() const { return m_fileUrl; }
    bool isModified() const { return !m_undo.isClean(); }

    virtual QString title() const = 0;
    virtual QString fileSuffix() const = 0;
    virtual QStringList quantityNames() const = 0; // [magnitude, scalar], e.g. "|E|", "V"
    virtual bool isLive() const { return false; }
    virtual bool isPlanar() const { return false; }
    virtual int defaultQuantity() const { return 0; }
    virtual bool canDraw() const { return false; }
    virtual bool hasDifferentialProbe() const { return false; }
    virtual QString viewKind() const { return QStringLiteral("field"); }
    virtual bool hasProbe() const { return true; }
    virtual std::shared_ptr<const LiveFrame> liveFrame() const { return {}; }
    // Area to frame in the view; empty: the outlines of the sources.
    virtual QRectF viewBounds(ViewPlane) const { return {}; }

    // For the field view.
    virtual qsig::Vec3 sourcePosition(int row) const = 0;
    virtual QString sourceName(int row) const = 0;
    virtual SourceOutline outline(int row, ViewPlane plane, double offset) const = 0;
    // For the 3D field view: the source in space, and the radius it occupies around its position.
    // 'viewCentre' and 'extent' bound the sources that extend to infinity.
    virtual SourceShape3D shape3d(int, const qsig::Vec3 &, double) const { return {}; }
    virtual double sourceReach(int) const { return 0.0; }
    // The module has a 3D field view (static fields in space).
    virtual bool hasSpaceView() const { return false; }
    // quantity 0: magnitude of the field (log colour scale); 1: the scalar (diverging scale).
    virtual FieldSampler sampler(int quantity, ViewPlane plane) const = 0;
    // The scalar is relative to an arbitrary reference (infinite sources): said in the legend.
    virtual bool scalarReferenced() const { return false; }

    // Editing (every call is one undoable step).
    Q_INVOKABLE virtual int addSource(int type) = 0;
    Q_INVOKABLE virtual void removeSource(int row) = 0;
    Q_INVOKABLE virtual int duplicateSource(int row) = 0;
    Q_INVOKABLE virtual void setSourceValue(int row, const QString &key, const QVariant &value) = 0;
    // Dragging: consecutive calls with the same row merge into a single undo step until endDrag().
    Q_INVOKABLE virtual void moveSource(int row, double x, double y, double z) = 0;
    Q_INVOKABLE void endDrag();
    Q_INVOKABLE void undo() { m_undo.undo(); }
    Q_INVOKABLE void redo() { m_undo.redo(); }

    // Read access for the inspector.
    Q_INVOKABLE virtual QVariantMap sourceData(int row) const = 0;
    Q_INVOKABLE virtual QStringList typeNames() const = 0;
    Q_INVOKABLE virtual QStringList editableFields(int type) const = 0;
    Q_INVOKABLE virtual QString fieldLabel(int type, const QString &key) const = 0;
    Q_INVOKABLE virtual QString fieldUnit(int type, const QString &key) const;
    // A field with choices is edited with a list; its value is the index of the choice.
    Q_INVOKABLE virtual QStringList fieldChoices(int, const QString &) const { return {}; }
    Q_INVOKABLE virtual QString fieldsTitle(int) const { return tr("Dimensions"); }
    Q_INVOKABLE virtual QString strengthSymbol(int type) const = 0;
    Q_INVOKABLE virtual QString strengthUnit(int type) const = 0;
    Q_INVOKABLE virtual QString strengthLabel(int type) const = 0; // "Q — charge totale"
    Q_INVOKABLE virtual QString formula(int type) const = 0;
    Q_INVOKABLE virtual QString accuracy(int type) const = 0;
    // Field at a point: {rows: [{name, value, unit, strong}], note}.
    Q_INVOKABLE virtual QVariantMap probe(double x, double y, double z) const = 0;
    // Where the measurement points are (B only when 'useB'), for modules that record them over time
    // or read B. probe() then answers for A, with B and the differences in its extra groups:
    // {groups: [{title, rows}], profiles: [{name, unit, values}] along A -> B, profileLength [m]}.
    Q_INVOKABLE virtual void setProbePoints(QVector3D, bool, QVector3D) {}
    Q_INVOKABLE virtual QString quantityTitle(int quantity) const = 0; // legend title
    Q_INVOKABLE virtual QString quantityUnit(int quantity) const = 0;
    Q_INVOKABLE virtual QString contourName(int quantity) const = 0;
    Q_INVOKABLE virtual QVariantList examples() const = 0; // [{key, name}]
    Q_INVOKABLE virtual void loadExample(const QString &key) = 0;

    // Settings of the whole scene (simulation parameters), shown in the inspector when listed.
    Q_INVOKABLE virtual QStringList settingFields() const { return {}; }
    Q_INVOKABLE virtual QString settingLabel(const QString &) const { return {}; }
    Q_INVOKABLE virtual QString settingUnit(const QString &) const { return {}; }
    Q_INVOKABLE virtual QStringList settingChoices(const QString &) const { return {}; }
    Q_INVOKABLE virtual QVariantMap settingsData() const { return {}; }
    Q_INVOKABLE virtual void setSetting(const QString &, const QVariant &) {}
    Q_INVOKABLE virtual QString settingsNote() const { return {}; }

    // Files.
    Q_INVOKABLE void newScene();
    Q_INVOKABLE bool save(const QUrl &url);
    Q_INVOKABLE bool load(const QUrl &url);
    virtual QByteArray toJson() const = 0;
    virtual bool fromJson(const QByteArray &json, QString *error = nullptr) = 0;

signals:
    void countChanged();
    void undoStateChanged();
    void fileUrlChanged();
    void modifiedChanged();
    void sceneChanged();
    void errorOccurred(const QString &message);
    // Live modules: a new liveFrame() is available; readingsChanged: the probe reading moved.
    void frameReady();
    void readingsChanged();

protected:
    virtual void clearSources() = 0;
    // Merge id of the current drag of 'row' (a new drag never merges into the previous one).
    int dragMergeId(int row);
    void forgetFile();

    QUndoStack m_undo;
    QUrl m_fileUrl;

private:
    bool m_dragging = false;
    int m_dragRow = -1;
    int m_dragSession = 0;
};

// Whole-scene snapshots: scenes hold a few dozen sources at most, so this keeps undo trivially correct.
template <typename Scene, typename State> class SnapshotCommand final : public QUndoCommand
{
public:
    SnapshotCommand(Scene *scene, State before, State after, const QString &text, int mergeId)
        : QUndoCommand(text), m_scene(scene), m_before(std::move(before)), m_after(std::move(after)), m_id(mergeId)
    {
    }
    void undo() override { m_scene->applyState(m_before); }
    void redo() override { m_scene->applyState(m_after); }
    int id() const override { return m_id; }
    bool mergeWith(const QUndoCommand *other) override
    {
        if (other->id() != m_id || m_id < 0) return false;
        m_after = static_cast<const SnapshotCommand *>(other)->m_after;
        return true;
    }

private:
    Scene *m_scene;
    State m_before, m_after;
    int m_id;
};

// Generic list of sources. SourceT has: name (std::string), type (enum), position (Vec3), strength.
template <typename SourceT> class SourceListScene : public SceneBase
{
public:
    using Source = SourceT;
    using Type = typename SourceT::Type;

    // Snapshot of the scene, as kept by undo/redo.
    struct State
    {
        std::vector<SourceT> sources;
        // Per source: the strength was given as a density, so the density (not the total) is kept
        // when the dimensions change. An editing choice only; the physics receives the total.
        std::vector<char> keepDensity;
    };

    using SceneBase::SceneBase;

    int rowCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : count(); }
    QVariant data(const QModelIndex &index, int role) const override
    {
        if (!index.isValid() || index.row() < 0 || index.row() >= count()) return {};
        const SourceT &s = m_sources[static_cast<std::size_t>(index.row())];
        const int type = static_cast<int>(s.type);
        switch (role)
        {
        case NameRole:
            return QString::fromStdString(s.name);
        case TypeRole:
            return type;
        case TypeNameRole:
            return typeNames().value(type);
        case StrengthRole:
            return s.strength;
        case StrengthTextRole:
            return summary(s);
        case XRole:
            return s.position.x;
        case YRole:
            return s.position.y;
        case ZRole:
            return s.position.z;
        default:
            return {};
        }
    }
    QHash<int, QByteArray> roleNames() const override
    {
        return {{NameRole, "name"},
                {TypeRole, "type"},
                {TypeNameRole, "typeName"},
                {StrengthRole, "strength"},
                {StrengthTextRole, "strengthText"},
                {XRole, "x"},
                {YRole, "y"},
                {ZRole, "z"}};
    }

    int count() const override { return static_cast<int>(m_sources.size()); }
    const std::vector<SourceT> &sources() const { return m_sources; }
    qsig::Vec3 sourcePosition(int row) const override { return at(row).position; }
    QString sourceName(int row) const override { return QString::fromStdString(at(row).name); }

    int addSource(int typeValue) override
    {
        const double offsets[] = {0.0, 0.4, -0.4, 0.8, -0.8};
        return addSourceAt(typeValue, {offsets[m_sources.size() % 5], 0.0, 0.0});
    }

    // A new source of the type at 'position', in one undo step; its row.
    int addSourceAt(int typeValue, const qsig::Vec3 &position)
    {
        const int type = std::clamp(typeValue, 0, static_cast<int>(typeNames().size()) - 1);
        SourceT s = makeSource(static_cast<Type>(type));
        int sameType = 1;
        for (const auto &other : m_sources) sameType += static_cast<int>(other.type) == type ? 1 : 0;
        s.name = shortName(static_cast<Type>(type)).toStdString() + " " + std::to_string(sameType);
        s.position = position;
        State next = current();
        next.sources.push_back(s);
        next.keepDensity.push_back(0);
        const QString text = SceneBase::tr("Ajouter %1").arg(QString::fromStdString(s.name));
        pushState(std::move(next), text);
        return count() - 1;
    }

    void removeSource(int row) override
    {
        if (row < 0 || row >= count()) return;
        State next = current();
        const QString text = SceneBase::tr("Supprimer %1").arg(sourceName(row));
        next.sources.erase(next.sources.begin() + row);
        next.keepDensity.erase(next.keepDensity.begin() + row);
        pushState(std::move(next), text);
    }

    int duplicateSource(int row) override
    {
        if (row < 0 || row >= count()) return -1;
        State next = current();
        SourceT copy = at(row);
        copy.name += " (copie)";
        copy.position.x += 0.2;
        next.sources.push_back(copy);
        next.keepDensity.push_back(next.keepDensity[static_cast<std::size_t>(row)]);
        const QString text = SceneBase::tr("Dupliquer %1").arg(QString::fromStdString(copy.name));
        pushState(std::move(next), text);
        return count() - 1;
    }

    // Keys: name, x, y, z, strength, density, keepDensity, and the module's dimensions.
    void setSourceValue(int row, const QString &key, const QVariant &value) override
    {
        if (row < 0 || row >= count()) return;
        State next = current();
        const auto i = static_cast<std::size_t>(row);
        SourceT &s = next.sources[i];
        if (key == QLatin1String("name"))
        {
            s.name = value.toString().trimmed().toStdString();
        }
        else if (key == QLatin1String("keepDensity"))
        {
            next.keepDensity[i] = value.toBool() ? 1 : 0;
        }
        else
        {
            bool ok = false;
            const double v = value.toDouble(&ok);
            if (!ok || !std::isfinite(v)) return;
            const double measureBefore = measure(s);
            bool dimension = false;
            if (key == QLatin1String("x"))
                s.position.x = v;
            else if (key == QLatin1String("y"))
                s.position.y = v;
            else if (key == QLatin1String("z"))
                s.position.z = v;
            else if (key == QLatin1String("strength"))
                s.strength = v;
            else if (key == QLatin1String("density"))
            {
                if (measureBefore <= 0.0) return;
                s.strength = v * measureBefore;
            }
            else if (setField(s, key, v, dimension))
            {
            }
            else
                return;
            // A density given by the user stays the same when the shape grows or shrinks.
            const double measureAfter = measure(s);
            if (dimension && next.keepDensity[i] && measureBefore > 0.0 && measureAfter > 0.0)
                s.strength *= measureAfter / measureBefore;
        }
        const QString text = SceneBase::tr("Modifier %1").arg(QString::fromStdString(s.name));
        pushState(std::move(next), text);
    }

    void moveSource(int row, double x, double y, double z) override
    {
        if (row < 0 || row >= count()) return;
        const int mergeId = dragMergeId(row);
        State next = current();
        SourceT &s = next.sources[static_cast<std::size_t>(row)];
        s.position = {x, y, z};
        const QString text = SceneBase::tr("Déplacer %1").arg(QString::fromStdString(s.name));
        pushState(std::move(next), text, mergeId);
    }

    QVariantMap sourceData(int row) const override
    {
        if (row < 0 || row >= count()) return {};
        const SourceT &s = at(row);
        const int type = static_cast<int>(s.type);
        const DensityInfo d = densityInfo(s.type);
        const double m = d.symbol.isEmpty() ? 0.0 : measure(s);
        QVariantMap map{{"name", QString::fromStdString(s.name)},
                        {"type", type},
                        {"typeName", typeNames().value(type)},
                        {"x", s.position.x},
                        {"y", s.position.y},
                        {"z", s.position.z},
                        {"strength", s.strength},
                        {"hasDensity", !d.symbol.isEmpty() && m > 0.0},
                        {"density", m > 0.0 ? s.strength / m : 0.0},
                        {"densitySymbol", d.symbol},
                        {"densityUnit", d.unit},
                        {"densityName", d.name},
                        {"measure", m},
                        {"measureName", d.measureName},
                        {"measureUnit", d.measureUnit},
                        {"keepDensity", m_keepDensity[static_cast<std::size_t>(row)] != 0}};
        addFields(map, s);
        return map;
    }

    void loadExample(const QString &key) override
    {
        std::vector<SourceT> sources = example(key);
        if (sources.empty()) return;
        m_undo.clear();
        applyState(State{std::move(sources), {}});
        forgetFile();
    }

    QByteArray toJson() const override
    {
        QJsonArray list;
        for (std::size_t i = 0; i < m_sources.size(); ++i)
        {
            const SourceT &s = m_sources[i];
            QJsonObject o{{"name", QString::fromStdString(s.name)},
                          {"type", QString::fromLatin1(typeKey(s.type))},
                          {"x", s.position.x},
                          {"y", s.position.y},
                          {"z", s.position.z},
                          {"strength", s.strength},
                          {"keepDensity", m_keepDensity[i] != 0}};
            writeFields(o, s);
            list.append(o);
        }
        QJsonObject root{{"format", formatName()}, {"schema", 1}, {"sources", list}};
        writeExtra(root);
        return QJsonDocument(root).toJson(QJsonDocument::Indented);
    }

    bool fromJson(const QByteArray &json, QString *error = nullptr) override
    {
        auto fail = [error](const QString &message)
        {
            if (error) *error = message;
            return false;
        };
        QJsonParseError parse{};
        const QJsonDocument doc = QJsonDocument::fromJson(json, &parse);
        if (parse.error != QJsonParseError::NoError) return fail(parse.errorString());
        const QJsonObject root = doc.object();
        if (root.value("format").toString() != formatName())
            return fail(SceneBase::tr("Ce fichier n'est pas une scène « %1 ».").arg(title()));
        if (root.value("schema").toInt() > 1) return fail(SceneBase::tr("Scène créée par une version plus récente."));
        State loaded;
        const int types = static_cast<int>(typeNames().size());
        for (const auto &value : root.value("sources").toArray())
        {
            const QJsonObject o = value.toObject();
            const QString key = o.value("type").toString();
            int type = -1;
            for (int t = 0; t < types && type < 0; ++t)
                if (key == QLatin1String(typeKey(static_cast<Type>(t)))) type = t;
            if (type < 0) return fail(SceneBase::tr("Type de source inconnu : %1").arg(key));
            SourceT s = makeSource(static_cast<Type>(type));
            s.name = o.value("name").toString().toStdString();
            s.position = {o.value("x").toDouble(), o.value("y").toDouble(), o.value("z").toDouble()};
            s.strength = o.value("strength").toDouble(s.strength);
            readFields(o, s);
            loaded.sources.push_back(s);
            loaded.keepDensity.push_back(o.value("keepDensity").toBool() ? 1 : 0);
        }
        if (!readExtra(root, error)) return false;
        m_undo.clear();
        applyState(loaded);
        emit modifiedChanged();
        return true;
    }

    // Used by undo commands.
    void applyState(const State &state)
    {
        if (state.sources.size() == m_sources.size())
        {
            m_sources = state.sources;
            if (!m_sources.empty()) emit dataChanged(index(0), index(count() - 1));
        }
        else
        {
            beginResetModel();
            m_sources = state.sources;
            endResetModel();
            emit countChanged();
        }
        m_keepDensity = state.keepDensity;
        m_keepDensity.resize(m_sources.size(), 0);
        emit sceneChanged();
    }

protected:
    // Module description.
    virtual const char *typeKey(Type type) const = 0; // stable identifier in files
    virtual QString shortName(Type type) const = 0;   // prefix of automatic names
    virtual QString formatName() const = 0;           // "QTsignalApp.electrostatics"
    virtual SourceT makeSource(Type type) const = 0;  // defaults of a new source
    // Sets a module-specific field; 'dimension' tells whether it changes the measure of the shape.
    virtual bool setField(SourceT &s, const QString &key, double value, bool &dimension) const = 0;
    virtual void addFields(QVariantMap &map, const SourceT &s) const = 0;
    virtual void writeFields(QJsonObject &o, const SourceT &s) const = 0;
    virtual void readFields(const QJsonObject &o, SourceT &s) const = 0;
    virtual DensityInfo densityInfo(Type) const { return {}; }
    virtual double measure(const SourceT &) const { return 0.0; }
    virtual std::vector<SourceT> example(const QString &key) const = 0;
    // Text of the list entry (default: the strength with its unit).
    virtual QString summary(const SourceT &s) const
    {
        return Units::formatQuantity(s.strength, strengthUnit(static_cast<int>(s.type)));
    }
    // Scene-wide data of the file (settings), next to the sources.
    virtual void writeExtra(QJsonObject &) const {}
    virtual bool readExtra(const QJsonObject &, QString *) { return true; }

    void clearSources() override { applyState({}); }
    const SourceT &at(int row) const { return m_sources[static_cast<std::size_t>(row)]; }
    State current() const { return {m_sources, m_keepDensity}; }
    // Last word of the module on a state about to be applied by an edit: relations it keeps between
    // its sources (the constraints of the antenna designer). Not called by undo, redo or loading.
    virtual void settleState(State &) const {}
    void pushState(State next, const QString &text, int mergeId = -1)
    {
        next.keepDensity.resize(next.sources.size(), 0);
        settleState(next);
        m_undo.push(new SnapshotCommand<SourceListScene, State>(this, current(), std::move(next), text, mergeId));
    }

    std::vector<SourceT> m_sources;
    std::vector<char> m_keepDensity;
};
} // namespace qsig::app
