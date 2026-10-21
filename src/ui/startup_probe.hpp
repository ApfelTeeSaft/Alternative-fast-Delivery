#pragma once
#include <QCoreApplication>
#include <QDebug>
#include <QImage>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTimer>
#include <memory>

namespace afd {
inline QQuickItem *probe_item(QQuickItem *parent, const QString &name) {
    if (parent->objectName() == name)
        return parent;
    for (auto *child : parent->childItems())
        if (auto *found = probe_item(child, name))
            return found;
    return nullptr;
}
// Exercise the packaged QML module and native window, rather than a source-only
// test view. The external test runner supplies a timeout if rendering deadlocks.
inline void startup_probe(QQuickWindow *window) {
    if (!window) {
        QCoreApplication::exit(4);
        return;
    }
    struct State {
        int step = 0;
        int expected = 0;
        QImage frame;
        bool setup_dismissed = false;
    };
    auto state = std::make_shared<State>();
    window->requestActivate();
    auto *timer = new QTimer(window);
    timer->setInterval(120);
    QObject::connect(timer, &QTimer::timeout, window, [window, timer, state] {
        auto fail = [timer](const char *reason) {
            timer->stop();
            qCritical() << "FAIL startup interaction:" << reason;
            QCoreApplication::exit(4);
        };
        if (!state->setup_dismissed) {
            auto *later = probe_item(window->contentItem(), "downloadSetupLaterButton");
            state->setup_dismissed = true;
            if (later && later->isVisible()) {
                QMetaObject::invokeMethod(later, "click");
                return;
            }
        }
        if (state->step == 6) {
            window->showMinimized();
            ++state->step;
            return;
        }
        if (state->step == 7) {
            window->showNormal();
            window->requestActivate();
            ++state->step;
            return;
        }
        if (window->property("section").toInt() != state->expected) {
            fail("navigation did not respond to button activation");
            return;
        }
        auto frame = window->grabWindow();
        if (frame.isNull() ||
            (((state->step > 0 && state->step < 6) || state->step == 9) && frame == state->frame)) {
            fail("the window did not repaint after navigation");
            return;
        }
        state->frame = std::move(frame);
        if (state->step == 5) {
            ++state->step;
            return;
        }
        if (state->step == 9) {
            timer->stop();
            qInfo() << "PASS packaged startup: six navigation pages, repaint, minimize/restore; renderer"
                    << window->rendererInterface()->graphicsApi();
            QCoreApplication::quit();
            return;
        }
        const int section = state->step < 6 ? state->step + 1 : 0;
        auto *control = probe_item(window->contentItem(), "desktopNav" + QString::number(section));
        if (!control || !control->isEnabled() || !control->isVisible()) {
            fail("navigation control is unavailable");
            return;
        }
        // Use the public button action here. QPA-level mouse/keyboard delivery
        // is covered separately by the Qt Test native workflow suite.
        if (!QMetaObject::invokeMethod(control, "click")) {
            fail("navigation control has no activation action");
            return;
        }
        state->expected = section;
        ++state->step;
    });
    timer->start();
}
} // namespace afd
