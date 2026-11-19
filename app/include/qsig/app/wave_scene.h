#pragma once

// "Ondes" module: sources and obstacles of a 2D FDTD simulation, run live on a worker thread.
// The list of objects, undo, files and inspector are those of every module (SourceListScene); the
// simulation settings are undoable too. The view paints liveFrame() as frames come.

#include "qsig/app/scene_base.h"
#include "qsig/app/wave_simulation.h"

#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QTimer>

#include <memory>

namespace qsig::app
{
// Not final: the QML engine instantiates a subclass (QQmlElement<WaveScene>).
class WaveScene : public SourceListScene<WaveObject>
{
    Q_OBJECT
    Q_PROPERTY(bool running READ running WRITE setRunning NOTIFY runningChanged)
    // Time steps per frame (about 60 frames per second).
    Q_PROPERTY(int speed READ speed WRITE setSpeed NOTIFY speedChanged)
    // The module is on screen: the simulation only runs then.
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statsChanged)
    Q_PROPERTY(double time READ time NOTIFY statsChanged)

public:
    explicit WaveScene(QObject *parent = nullptr);
    ~WaveScene() override;

    QString title() const override;
    QString fileSuffix() const override { return QStringLiteral("qsig-wave.json"); }
    // [amplitude, instantaneous value] of the field out of the plane.
    QStringList quantityNames() const override;
    bool isLive() const override { return true; }
    bool isPlanar() const override { return true; }
    int defaultQuantity() const override { return 1; }
    bool canDraw() const override { return true; }
    bool hasDifferentialProbe() const override { return true; }
    std::shared_ptr<const LiveFrame> liveFrame() const override;
    QRectF viewBounds(ViewPlane plane) const override;

    SourceOutline outline(int row, ViewPlane plane, double offset) const override;
    FieldSampler sampler(int, ViewPlane) const override { return {}; }

    QStringList typeNames() const override;
    QStringList editableFields(int type) const override;
    QString fieldLabel(int type, const QString &key) const override;
    QString fieldUnit(int type, const QString &key) const override;
    QStringList fieldChoices(int type, const QString &key) const override;
    QString fieldsTitle(int type) const override;
    QString strengthSymbol(int type) const override;
    QString strengthUnit(int type) const override;
    QString strengthLabel(int type) const override;
    QString formula(int type) const override;
    QString accuracy(int type) const override;
    // Rows at A: primary field, its amplitude, the two in-plane components. With B: B, and A - B,
    // amplitude ratio, phase difference and delay at the frequency of the sinusoidal source. Traces of
    // both points.
    QVariantMap probe(double x, double y, double z) const override;
    void setProbePoints(QVector3D a, bool useB, QVector3D b) override;
    QString quantityTitle(int quantity) const override;
    QString quantityUnit(int quantity) const override;
    QString contourName(int) const override { return {}; }
    QVariantList examples() const override;
    void loadExample(const QString &key) override;

    QStringList settingFields() const override;
    QString settingLabel(const QString &key) const override;
    QString settingUnit(const QString &key) const override;
    QStringList settingChoices(const QString &key) const override;
    QVariantMap settingsData() const override;
    void setSetting(const QString &key, const QVariant &value) override;
    QString settingsNote() const override;

    const WaveSettings &settings() const { return m_settings; }
    void applySettings(const WaveSettings &settings); // used by undo
    WaveGrid grid() const { return waveGrid(m_settings, m_sources); }

    bool running() const { return m_running; }
    void setRunning(bool running);
    int speed() const { return m_speed; }
    void setSpeed(int speed);
    bool active() const { return m_active; }
    void setActive(bool active);
    QString statusText() const;
    double time() const;
    std::shared_ptr<const WaveFrame> lastFrame() const { return m_frame; }

    Q_INVOKABLE void stepOnce();
    Q_INVOKABLE void resetSimulation();

    // Drawing in the plane (one undo step each). 'material': 0 dielectric of permittivity
    // 'epsilonR', 1 conductor. 'points': world positions (QPointF) of the gesture.
    // A brush stroke goes into the selected drawing when it has the same material, else into a new
    // one; an eraser stroke removes paint from every drawing it crosses. Returns the drawing's row
    // (-1 when nothing changed).
    Q_INVOKABLE int addStroke(int row, const QVariantList &points, double width, bool erase, int material,
                              double epsilonR);
    // Shape dragged from (x0, y0) to (x1, y1): "rectangle" (corners), "disk" (centre, then a point of
    // the rim) or "line" (a wall of thickness 'width'). Returns the new row, or -1 if too small.
    Q_INVOKABLE int addDrawnShape(const QString &shape, double x0, double y0, double x1, double y1, int material,
                                  double epsilonR, double width);

signals:
    void runningChanged();
    void speedChanged();
    void activeChanged();
    void statsChanged();

protected:
    const char *typeKey(Type type) const override;
    QString shortName(Type type) const override;
    QString formatName() const override { return QStringLiteral("QTsignalApp.waves"); }
    WaveObject makeSource(Type type) const override;
    bool setField(WaveObject &s, const QString &key, double value, bool &dimension) const override;
    void addFields(QVariantMap &map, const WaveObject &s) const override;
    void writeFields(QJsonObject &o, const WaveObject &s) const override;
    void readFields(const QJsonObject &o, WaveObject &s) const override;
    std::vector<WaveObject> example(const QString &key) const override;
    QString summary(const WaveObject &s) const override;
    void writeExtra(QJsonObject &root) const override;
    bool readExtra(const QJsonObject &root, QString *error) override;
    void clearSources() override;

private:
    void tick();
    void onFrame();
    void watchPulses();
    QString automaticName(const State &state, Type type, const QString &prefix) const;
    QString fieldName() const; // "Ez" or "Hz"
    QString fieldUnitName() const;

    WaveSettings m_settings;
    std::shared_ptr<WaveSimulation> m_simulation;
    std::shared_ptr<const WaveFrame> m_frame;
    QFutureWatcher<std::shared_ptr<const WaveFrame>> m_watcher;
    QTimer m_tick;
    QElapsedTimer m_readingsClock;
    bool m_running = true;
    bool m_active = false;
    int m_speed = 2;
    // Work for the next job.
    bool m_dirty = true, m_resetRequested = false, m_probeDirty = true, m_stepPending = false;
    double m_probeX = 0.15, m_probeY = 0.2;
    bool m_useB = false;
    double m_probeBX = 0.0, m_probeBY = 0.0;
    QString m_pulseSignature;
};
} // namespace qsig::app
