#pragma once
#include <QQuickWindow>
#include <cstring>

inline void configure_rendering(int argc, char **argv) {
    bool software = false, hardware = false;
    for (int i = 1; i < argc; ++i) {
        software = software || std::strcmp(argv[i], "--software-rendering") == 0;
        hardware = hardware || std::strcmp(argv[i], "--hardware-rendering") == 0;
    }
#ifdef Q_OS_WIN
    // This 2D workstation does not need a GPU. Avoid driver/render-thread startup
    // failures by default, while retaining explicit Qt environment overrides.
    software = software || (!hardware && !qEnvironmentVariableIsSet("QT_QUICK_BACKEND") &&
                            !qEnvironmentVariableIsSet("QSG_RHI_BACKEND"));
#else
    Q_UNUSED(hardware);
#endif
    if (software)
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
}
