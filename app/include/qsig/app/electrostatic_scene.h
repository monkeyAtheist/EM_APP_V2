#pragma once

// Electrostatics module: charges, charged shapes and their exact fields (qsig/electrostatics.h).

#include "qsig/app/scene_base.h"
#include "qsig/electrostatics.h"

namespace qsig::app
{
// Not final: the QML engine instantiates a subclass (QQmlElement<ElectrostaticScene>).
class ElectrostaticScene : public SourceListScene<Electrostatics::Source>
{
    Q_OBJECT

public:
    explicit ElectrostaticScene(QObject *parent = nullptr);

    QString title() const override;
    QString fileSuffix() const override { return QStringLiteral("qsig-es.json"); }
    QStringList quantityNames() const override { return {QStringLiteral("|E|"), QStringLiteral("V")}; }

    SourceOutline outline(int row, ViewPlane plane, double offset) const override;
    SourceShape3D shape3d(int row, const qsig::Vec3 &viewCentre, double extent) const override;
    double sourceReach(int row) const override;
    bool hasSpaceView() const override { return true; }
    FieldSampler sampler(int quantity, ViewPlane plane) const override;
    bool scalarReferenced() const override;
    // Point B: its field, the voltage U_AB = V_A - V_B, the mean field along AB and the profiles of V
    // and |E| from A to B.
    bool hasDifferentialProbe() const override { return true; }
    void setProbePoints(QVector3D a, bool useB, QVector3D b) override;

    QStringList typeNames() const override;
    QStringList editableFields(int type) const override;
    QString fieldLabel(int type, const QString &key) const override;
    QString strengthSymbol(int type) const override;
    QString strengthUnit(int type) const override;
    QString strengthLabel(int type) const override;
    QString formula(int type) const override;
    QString accuracy(int type) const override;
    // rows |E|, Ex, Ey, Ez, V (relative when an infinite source is present).
    QVariantMap probe(double x, double y, double z) const override;
    QString quantityTitle(int quantity) const override;
    QString quantityUnit(int quantity) const override;
    QString contourName(int quantity) const override;
    QVariantList examples() const override;

protected:
    const char *typeKey(Type type) const override;
    QString shortName(Type type) const override;
    QString formatName() const override { return QStringLiteral("QTsignalApp.electrostatics"); }
    Source makeSource(Type type) const override;
    bool setField(Source &s, const QString &key, double value, bool &dimension) const override;
    void addFields(QVariantMap &map, const Source &s) const override;
    void writeFields(QJsonObject &o, const Source &s) const override;
    void readFields(const QJsonObject &o, Source &s) const override;
    DensityInfo densityInfo(Type type) const override;
    double measure(const Source &s) const override;
    std::vector<Source> example(const QString &key) const override;

private:
    bool m_useB = false;
    qsig::Vec3 m_probeB;
};
} // namespace qsig::app
