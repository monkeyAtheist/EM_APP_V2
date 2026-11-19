#include "qsig/app/antenna_scene.h"
#include "qsig/app/electrostatic_scene.h"
#include "qsig/app/magnetostatic_scene.h"
#include "qsig/app/units.h"
#include "qsig/app/wave_scene.h"

#include <QGuiApplication>
#include <QLoggingCategory>
#include <QQmlApplicationEngine>
#include <QQmlEngine>
#include <QtQml/qqml.h>
#include <QQuickStyle>

int main(int argc, char *argv[])
{
    // Compact Material controls, suited to a desktop application (unless the user chose otherwise).
    if (!qEnvironmentVariableIsSet("QT_QUICK_CONTROLS_MATERIAL_VARIANT"))
        qputenv("QT_QUICK_CONTROLS_MATERIAL_VARIANT", "Dense");
    QGuiApplication app(argc, argv);
    // Windows: "Unable to obtain clipboard" whenever another program holds the clipboard (a screenshot
    // tool...). Harmless, and it would drown real messages in the log.
    QLoggingCategory::setFilterRules(QStringLiteral("qt.qpa.mime.warning=false"));
    QGuiApplication::setOrganizationName(QStringLiteral("QTsignalApp"));
    QGuiApplication::setApplicationName(QStringLiteral("QTsignalApp"));
    QGuiApplication::setApplicationVersion(QStringLiteral(QSIG_VERSION));
    QQuickStyle::setStyle(QStringLiteral("Material"));

    // Application-layer types (library qsig_app) exposed to QML.
    qmlRegisterType<qsig::app::ElectrostaticScene>("Qsig.App", 1, 0, "ElectrostaticScene");
    qmlRegisterType<qsig::app::MagnetostaticScene>("Qsig.App", 1, 0, "MagnetostaticScene");
    qmlRegisterType<qsig::app::WaveScene>("Qsig.App", 1, 0, "WaveScene");
    qmlRegisterType<qsig::app::AntennaScene>("Qsig.App", 1, 0, "AntennaScene");
    qmlRegisterSingletonType<qsig::app::Units>("Qsig.App", 1, 0, "Units", [](QQmlEngine *, QJSEngine *) -> QObject *
                                               { return new qsig::app::Units; });

    QQmlApplicationEngine engine;
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app, [] { QCoreApplication::exit(1); },
        Qt::QueuedConnection);
    engine.loadFromModule("Qsig", "Main");
    return QGuiApplication::exec();
}
