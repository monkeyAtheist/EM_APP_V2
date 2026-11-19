#pragma once

// "Antennes" module: wire antennas solved by the method of moments. The list of elements, undo, files
// and inspector are those of every module (SourceListScene); the settings (frequency, reference
// impedance, mesh, sweep) are undoable too. Every change is solved again in the background (debounced);
// frequency sweeps run on the thread pool with a progress value. The views read result() and sweep().
// Two tools run on a worker as well: the Yagi optimiser (its result is one undoable step) and the
// validation bench of the wire solver.

#include "qsig/app/antenna_bench.h"
#include "qsig/app/antenna_model.h"
#include "qsig/app/antenna_optimizer.h"
#include "qsig/app/scene_base.h"

#include <QFutureWatcher>
#include <QList>
#include <QTimer>

#include <atomic>
#include <memory>
#include <vector>

namespace qsig::app
{
// Not final: the QML engine instantiates a subclass (QQmlElement<AntennaScene>).
class AntennaScene : public SourceListScene<AntennaElement>
{
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY resultChanged)
    Q_PROPERTY(double frequency READ frequency NOTIFY sceneChanged)
    Q_PROPERTY(double referenceOhm READ referenceOhm NOTIFY sceneChanged)
    Q_PROPERTY(bool sweepRunning READ sweepRunning NOTIFY sweepRunningChanged)
    Q_PROPERTY(double sweepProgress READ sweepProgress NOTIFY sweepProgressChanged)
    // The scene changed since the last sweep.
    Q_PROPERTY(bool sweepStale READ sweepStale NOTIFY sweepChanged)
    Q_PROPERTY(bool hasSweep READ hasSweep NOTIFY sweepChanged)
    // Yagi optimiser: running state, progress 0..1, live best design, last outcome.
    Q_PROPERTY(bool optimising READ optimising NOTIFY optimiserChanged)
    Q_PROPERTY(double optimiseProgress READ optimiseProgress NOTIFY optimiserChanged)
    Q_PROPERTY(QString optimiserText READ optimiserText NOTIFY optimiserChanged)
    Q_PROPERTY(QVariantMap optimiserResult READ optimiserResult NOTIFY optimiserChanged)
    // Validation bench: running state, progress, the lines of the last report and its summary.
    Q_PROPERTY(bool benchRunning READ benchRunning NOTIFY benchChanged)
    Q_PROPERTY(double benchProgress READ benchProgress NOTIFY benchChanged)
    Q_PROPERTY(QVariantList benchCases READ benchCases NOTIFY benchChanged)
    Q_PROPERTY(QString benchSummary READ benchSummary NOTIFY benchChanged)
    // Sources: entities placed on the wire elements (see feedList).
    Q_PROPERTY(QVariantList feedList READ feedList NOTIFY sceneChanged)
    Q_PROPERTY(int feedTotal READ feedTotal NOTIFY sceneChanged)
    // Media: what the shapes are made of (see mediaList).
    Q_PROPERTY(QVariantList mediaList READ mediaList NOTIFY sceneChanged)

public:
    explicit AntennaScene(QObject *parent = nullptr);
    ~AntennaScene() override;

    QString title() const override;
    QString fileSuffix() const override { return QStringLiteral("qsig-ant.json"); }
    QStringList quantityNames() const override { return {tr("Gain"), tr("Gain")}; }
    QString viewKind() const override { return QStringLiteral("antenna"); }
    bool hasProbe() const override { return false; }

    SourceOutline outline(int row, ViewPlane plane, double offset) const override;
    FieldSampler sampler(int, ViewPlane) const override { return {}; }

    QStringList typeNames() const override;
    QStringList editableFields(int type) const override;
    QString fieldLabel(int type, const QString &key) const override;
    QString fieldUnit(int type, const QString &key) const override;
    QStringList fieldChoices(int type, const QString &key) const override;
    QString fieldsTitle(int) const override { return tr("Géométrie"); }
    QString strengthSymbol(int) const override { return QStringLiteral("V"); }
    QString strengthUnit(int) const override { return QStringLiteral("V"); }
    QString strengthLabel(int) const override { return {}; } // the voltage is among the fields
    QString formula(int type) const override;
    QString accuracy(int type) const override;
    // Results of the last solve: rows of the first source, one group per other source.
    QVariantMap probe(double x, double y, double z) const override;
    QString quantityTitle(int) const override { return tr("Gain"); }
    QString quantityUnit(int) const override { return QStringLiteral("dBi"); }
    QString contourName(int) const override { return {}; }
    QVariantList examples() const override;
    void loadExample(const QString &key) override;

    QStringList settingFields() const override;
    QString settingLabel(const QString &key) const override;
    QString settingUnit(const QString &key) const override;
    QVariantMap settingsData() const override;
    void setSetting(const QString &key, const QVariant &value) override;
    QString settingsNote() const override;

    const AntennaSettings &settings() const { return m_settings; }
    void applySettings(const AntennaSettings &settings); // used by undo
    std::shared_ptr<const AntennaResult> result() const { return m_result; }
    std::shared_ptr<const std::vector<SweepPoint>> sweep() const { return m_sweep; }

    bool busy() const { return m_solving; }
    QString statusText() const;
    double frequency() const { return m_settings.frequency; }
    double referenceOhm() const { return m_settings.referenceOhm; }
    bool sweepRunning() const { return m_sweeping; }
    double sweepProgress() const { return m_sweepProgress; }
    bool sweepStale() const { return m_sweepStale; }
    bool hasSweep() const { return m_sweep && !m_sweep->empty(); }

    Q_INVOKABLE void setFrequency(double frequency);
    // Designer tools of the 3D view (double precision: ends that meet must coincide to be joined).
    // A passive wire between two points; its row, or -1 (with errorOccurred) when it is too short.
    int addWire(const qsig::Vec3 &a, const qsig::Vec3 &b);
    // Sources are entities placed on the wire elements: one on a ready-made element, at the place its
    // type gives it; any number on a plain wire. The list, in the order of the scene, for the table and
    // the panels: [{row, index, name, host, place, text, fraction, voltage, phaseDeg, movable}].
    QVariantList feedList() const;
    // The media of the scene, for the tree: air, the perfect conductor, then each dielectric in use.
    // [{name, detail, kind}], kind 0 air, 1 conductor, 2 dielectric.
    QVariantList mediaList() const;
    int feedTotal() const;
    // Places a source on the element. On a plain wire it goes to the point nearest to 'point' (the
    // quarters and the middle attract it; near an end that stands on a surface, to that end), and a
    // wire that has sources already gets one more. The index of the source on its element (the one
    // already there when 'point' is on it), or -1 for a surface (with errorOccurred).
    int placeFeed(int row, const qsig::Vec3 &point);
    // One more source without a point: on a passive element, where its type or its last source had it;
    // on a plain wire with sources, in the middle of its longest free stretch. Its index, or -1 (with
    // errorOccurred) when the element takes no more.
    Q_INVOKABLE int addFeedTo(int row);
    // A source of a plain wire slid to the point of the wire nearest to 'point'. The calls of one drag
    // merge into a single undo step, closed by endDrag(). False when it cannot go there.
    bool moveFeed(int row, int index, const qsig::Vec3 &point);
    Q_INVOKABLE void deleteFeed(int row, int index);
    // 'key': "voltage" [V], "phaseDeg", or "fraction" (place along a plain wire or a helix, 0..1).
    Q_INVOKABLE void setFeedValue(int row, int index, const QString &key, double value);
    // One end of a plain wire (0: its first end, 1: the other) laid at 'point', the other end staying
    // where it is. The calls of one drag merge into a single undo step, closed by endDrag(). False when
    // the element is not a plain wire or would become too short.
    bool moveWireEnd(int row, int end, const qsig::Vec3 &point);
    // Cuts a plain wire at the point of its length nearest to 'point': two wires that touch. The row of
    // the second part, or -1 (with errorOccurred) when a part would be too short.
    int cutWire(int row, const qsig::Vec3 &point);
    // Elements dragged together: 'positions' are the new centres of the elements at 'rows'. The calls
    // of one drag merge into a single undo step, closed by endDrag().
    void dragElements(const QList<int> &rows, const std::vector<qsig::Vec3> &positions);
    // Transformations of a selection, each one undo step. They return the rows to select afterwards:
    // the elements themselves, or their copies (added at the end of the list) when copies are asked for.
    // 'copies' > 0: the elements stay and that many copies are laid at delta, 2 delta...
    QList<int> translateElements(const QList<int> &rows, const qsig::Vec3 &delta, int copies);
    QList<int> rotateElements(const QList<int> &rows, const qsig::Vec3 &pivot, const qsig::Vec3 &axis, double angleDeg,
                              bool copy);
    QList<int> mirrorElements(const QList<int> &rows, const qsig::Vec3 &point, const qsig::Vec3 &normal, bool copy);
    // Coordinate 'axis' (0 x, 1 y, 2 z) of the centres set to 'value'.
    void alignElements(const QList<int> &rows, int axis, double value);
    void removeElements(const QList<int> &rows);
    // Centre of the box that holds the wires of the elements at 'rows'.
    qsig::Vec3 centreOf(const QList<int> &rows) const;

    // Groups: elements with the same group name are selected and transformed together.
    // Makes one new group of the elements at 'rows' (two at least).
    void groupElements(const QList<int> &rows);
    // Dissolves the groups the elements at 'rows' belong to.
    void ungroupElements(const QList<int> &rows);
    void renameGroup(int row, const QString &name);
    Q_INVOKABLE QString groupOf(int row) const;
    // For the lists by kind: whether the element at 'row' is a conducting surface (a plate, a disc, or
    // the skin of a solid), whether it is a solid, whether it is fed.
    Q_INVOKABLE bool isSurface(int row) const
    {
        return row >= 0 && row < count() && elementIsSurface(sources()[static_cast<std::size_t>(row)].type);
    }
    Q_INVOKABLE bool isSolid(int row) const
    {
        return row >= 0 && row < count() && elementIsSolid(sources()[static_cast<std::size_t>(row)].type);
    }
    Q_INVOKABLE bool isDielectric(int row) const
    {
        return row >= 0 && row < count() && elementIsDielectric(sources()[static_cast<std::size_t>(row)]);
    }
    Q_INVOKABLE bool isFed(int row) const
    {
        return row >= 0 && row < count() && sources()[static_cast<std::size_t>(row)].fed != 0 && !isSurface(row);
    }
    // Rows of the group of 'row'; the row alone when it has no group.
    QList<int> groupRows(int row) const;

    // Constraints: the element at 'row' moves, turns or changes length to keep a relation with the
    // element at 'referenceRow' (or with itself); they are applied again at every edit of the scene.
    // 'type' is an AntennaConstraint::Type; 'value' a length or a distance [m] or an angle [deg];
    // anchors: 0 centre, 1 first end, 2 second end; 'axes': bit 0 x, bit 1 y, bit 2 z (fixed position).
    // False, with errorOccurred, when it cannot apply.
    bool addConstraint(int row, int type, int referenceRow, double value, int anchor, int referenceAnchor, int axes);
    void removeConstraint(int row, int index);
    void setConstraintEnabled(int row, int index, bool enabled);
    // For the panel: [{text, enabled, met}] of the constraints of the element.
    QVariantList constraintList(int row) const;
    Q_INVOKABLE void runSweep();
    Q_INVOKABLE void cancelSweep();
    // For the tests: waits for the solve in progress (and a pending one) to finish.
    void waitForResult();

    bool optimising() const { return m_optimising; }
    double optimiseProgress() const { return m_optimiseProgress; }
    QString optimiserText() const { return m_optimiserText; }
    QVariantMap optimiserResult() const { return m_optimiserResult; }
    QStringList optimiserGoals() const;
    Q_INVOKABLE QStringList goalNames() const { return optimiserGoals(); }
    // goal: index in goalNames(); budget: number of solves.
    Q_INVOKABLE void optimise(int goal, bool match, int budget);
    Q_INVOKABLE void cancelOptimise() { m_cancelOptimise = true; }

    bool benchRunning() const { return m_benchRunning; }
    double benchProgress() const { return m_benchProgress; }
    QVariantList benchCases() const { return m_benchCases; }
    QString benchSummary() const { return m_benchSummary; }
    Q_INVOKABLE void runBench();

signals:
    void busyChanged();
    void resultChanged();
    void sweepChanged();
    void sweepRunningChanged();
    void sweepProgressChanged();
    void optimiserChanged();
    void benchChanged();

protected:
    const char *typeKey(Type type) const override;
    QString shortName(Type type) const override;
    QString formatName() const override { return QStringLiteral("QTsignalApp.antennas"); }
    AntennaElement makeSource(Type type) const override;
    bool setField(AntennaElement &s, const QString &key, double value, bool &dimension) const override;
    void addFields(QVariantMap &map, const AntennaElement &s) const override;
    void writeFields(QJsonObject &o, const AntennaElement &s) const override;
    void readFields(const QJsonObject &o, AntennaElement &s) const override;
    std::vector<AntennaElement> example(const QString &key) const override;
    QString summary(const AntennaElement &s) const override;
    void writeExtra(QJsonObject &root) const override;
    bool readExtra(const QJsonObject &root, QString *error) override;
    void clearSources() override;

protected:
    // Ids, then the constraints: every edit leaves the scene with its constraints applied.
    void settleState(State &state) const override;

private:
    QString constraintText(const AntennaConstraint &constraint) const;
    // Rows of 'rows' that exist, each once, in increasing order.
    QList<int> validRows(const QList<int> &rows) const;
    // "Fil 3", or "4 éléments": what an undo text says of a selection.
    QString describe(const QList<int> &rows) const;
    // A name no element of 'sources' has: the prefix of the type and the first free number.
    std::string freshName(const std::vector<AntennaElement> &sources, Type type) const;
    // Applies 'change' to the elements at 'rows', or to copies of them added to the list ('copy').
    template <typename Change> QList<int> transformElements(const QList<int> &rows, bool copy, const QString &text, Change change);

    void solve();
    void onSolved();
    void onSweepFinished();
    void onOptimised();
    void onBenchFinished();
    bool computeSweepStale() const;
    void updateSweepStale();

    AntennaSettings m_settings;
    std::shared_ptr<const AntennaResult> m_result;
    std::shared_ptr<const std::vector<SweepPoint>> m_sweep;
    QFutureWatcher<std::shared_ptr<const AntennaResult>> m_watcher;
    QFutureWatcher<SweepPoint> m_sweepWatcher;
    QTimer m_debounce;
    bool m_solving = false, m_pending = false, m_sweeping = false;
    double m_sweepProgress = 0.0;
    bool m_sweepStale = false;
    // What the sweep (shown or running) was computed from: it is stale once the scene differs.
    std::vector<AntennaElement> m_sweepElements, m_runningElements;
    AntennaSettings m_sweepSettings, m_runningSettings;

    QFutureWatcher<YagiOptimizerResult> m_optimiseWatcher;
    std::atomic<bool> m_cancelOptimise{false};
    bool m_optimising = false;
    double m_optimiseProgress = 0.0;
    QString m_optimiserText;
    QVariantMap m_optimiserResult;
    std::vector<AntennaElement> m_optimiseStart; // applied only if the scene did not change meanwhile

    QFutureWatcher<AntennaBenchReport> m_benchWatcher;
    bool m_benchRunning = false;
    double m_benchProgress = 0.0;
    QVariantList m_benchCases;
    QString m_benchSummary;
};
} // namespace qsig::app
