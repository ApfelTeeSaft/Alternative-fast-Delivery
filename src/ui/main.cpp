#include "ui/controller.hpp"
#include "ui/startup.hpp"
#include "ui/startup_probe.hpp"
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSvgRenderer>
#include <QSysInfo>
#include <QTimer>
#include <QUrl>
int main(int argc, char **argv) {
    configure_rendering(argc, argv);
    QGuiApplication app(argc, argv);
    app.setApplicationName("AlternativeFastDelivery");
    app.setOrganizationName("AlternativeFastDelivery");
    app.setApplicationVersion(AFD_VERSION);
    QSvgRenderer brand(QStringLiteral(":/qt/qml/AFD/resources/afd.svg"));
    if (!brand.isValid())
        return 2;
    QQuickStyle::setStyle("Basic");
    Controller controller;
    QObject::connect(&app, &QGuiApplication::applicationStateChanged, &controller,
                     [&](Qt::ApplicationState state) {
                         if ((QSysInfo::productType() == "android" || QSysInfo::productType() == "ios") &&
                             state != Qt::ApplicationActive)
                             controller.pauseTorrent(true);
                     });
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("workstation", &controller);
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app, [] { QCoreApplication::exit(1); },
        Qt::QueuedConnection);
    engine.loadFromModule("AFD", "Main");
    app.setWindowIcon(QIcon(":/qt/qml/AFD/resources/afd.svg"));
    auto args = app.arguments();
    if (args.contains("--interaction-smoke-test"))
        QTimer::singleShot(500, &app, [&engine] {
            afd::startup_probe(qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)));
        });
    for (const auto &arg : args)
        if (arg.startsWith("--export-icon="))
            return QIcon(":/qt/qml/AFD/resources/afd.svg").pixmap(1024, 1024).save(arg.mid(14)) ? 0 : 2;
    if (args.contains("--smoke-test"))
        QTimer::singleShot(1500, &app, &QCoreApplication::quit);
    for (const auto &arg : args)
        if (arg.startsWith("--capture=")) {
            if (args.contains("--mobile"))
                if (auto *w = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0))) {
                    w->resize(390, 844);
                }
            QTimer::singleShot(1000, &app, [&engine, arg] {
                if (auto *w = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0))) {
                    if (!w->grabWindow().save(arg.mid(10)))
                        QCoreApplication::exit(2);
                    else
                        QCoreApplication::quit();
                } else
                    QCoreApplication::exit(3);
            });
        }
    if (args.size() > 1 && !args[1].startsWith("--"))
        QTimer::singleShot(50, &controller,
                           [&controller, args] { controller.openLocal(QUrl::fromLocalFile(args[1])); });
    return app.exec();
}
