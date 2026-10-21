#include "ui/controller.hpp"
#include "ui/startup.hpp"
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTimer>
#include <iostream>
#include <libtorrent/bencode.hpp>
#include <libtorrent/entry.hpp>
#include <stdexcept>
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        if (!(x))                                                                                            \
            throw std::runtime_error(std::string("Failed: ") + #x);                                          \
    } while (false)
static void wait(Controller &c) {
    QElapsedTimer timer;
    timer.start();
    while (c.busy()) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
        if (timer.elapsed() > 20000)
            throw std::runtime_error("Controller timed out");
    }
    QCoreApplication::processEvents();
}
static void until(const std::function<bool()> &condition) {
    QElapsedTimer timer;
    timer.start();
    while (!condition()) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
        if (timer.elapsed() > 20000)
            throw std::runtime_error("UI transition timed out");
    }
    QCoreApplication::processEvents();
}
static QQuickItem *item(QQuickItem *parent, const QString &name) {
    if (parent->objectName() == name)
        return parent;
    for (auto *child : parent->childItems())
        if (auto *found = item(child, name))
            return found;
    return nullptr;
}
static void click(QQuickWindow *window, const QString &name) {
    QTest::qWait(60);
    auto *control = item(window->contentItem(), name);
    if (!control || !control->isVisible() || !control->isEnabled() || control->width() <= 0 ||
        control->height() <= 0)
        throw std::runtime_error("Control is not clickable: " + name.toStdString());
    const auto center = control->mapToScene(QPointF(control->width() / 2, control->height() / 2));
    if (!QRectF(0, 0, window->width(), window->height()).contains(center))
        throw std::runtime_error("Control is outside the window: " + name.toStdString());
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center.toPoint());
    QCoreApplication::processEvents();
}
int main(int argc, char **argv) {
    configure_rendering(argc, argv);
    QQuickStyle::setStyle("Basic");
    QGuiApplication app(argc, argv);
    QStandardPaths::setTestModeEnabled(true);
    app.setApplicationName("AFD-Test-" + QString::number(QCoreApplication::applicationPid()));
    app.setOrganizationName("AlternativeFastDeliveryTests");
    app.setApplicationVersion("0.1.0");
    try {
        QTemporaryDir temp;
        CHECK(temp.isValid());
        const QByteArray eml =
            "From: Alice <alice@example.invalid>\r\nTo: Bob <bob@example.invalid>\r\nSubject: Native "
            "workflow\r\nMIME-Version: 1.0\r\nContent-Type: text/plain\r\n\r\nhello copper lantern\r\n";
        auto write = [&](QString name, QByteArray data) {
            QFile f(temp.path() + "/" + name);
            CHECK(f.open(QIODevice::WriteOnly));
            CHECK(f.write(data) == data.size());
            return QUrl::fromLocalFile(f.fileName());
        };
        auto message = write("message.eml", eml);
        auto archive = write("archive.mbox", "From alice Tue Oct 20 12:00:00 2026\n" + eml +
                                                 "From bob Tue Oct 20 12:01:00 2026\n" + eml);
        libtorrent::entry metainfo;
        auto &info = metainfo["info"];
        info["name"] = "fixture";
        info["piece length"] = 16384;
        info["pieces"] = std::string(20, '\0');
        for (auto name : {"empty.txt", "second.txt", "payload.bin"}) {
            libtorrent::entry file;
            file["length"] = name == std::string("payload.bin") ? 1 : 0;
            auto &parts = file["path"].list();
            if (name != std::string("payload.bin"))
                parts.emplace_back("folder");
            parts.emplace_back(name);
            info["files"].list().push_back(std::move(file));
        }
        for (int i = 0; i < 25; ++i) {
            libtorrent::entry file;
            file["length"] = 0;
            file["path"].list().emplace_back("folder");
            file["path"].list().emplace_back("entry" + std::to_string(i) + ".txt");
            info["files"].list().push_back(std::move(file));
        }
        std::vector<char> encoded;
        libtorrent::bencode(std::back_inserter(encoded), metainfo);
        auto torrent = write("fixture.torrent", QByteArray(encoded.data(), encoded.size()));
        {
            Controller c(nullptr, true);
            CHECK(c.needsDownloadFolder());
            c.addTorrent(torrent.toString());
            CHECK(!c.torrentOpen());
            CHECK(c.notice().contains("Choose a download folder"));
            c.uploadLimit(128);
            wait(c);
            CHECK(c.uploadKiB() == 128);
            c.storageLimit(4096);
            wait(c);
            CHECK(c.quotaMiB() == 4096);
            c.openLocal(message);
            wait(c);
            CHECK(c.text().contains("copper lantern"));
            CHECK(c.sourceKind() == "email");
            CHECK(c.messages().size() == 1);
            CHECK(c.headers().contains("Native workflow"));
            CHECK(c.signatures().contains("Missing signature"));
            c.verify(false);
            wait(c);
            CHECK(c.signatures().contains("Missing signature"));
            auto html = eml;
            html.replace("Content-Type: text/plain", "Content-Type: text/html");
            html.replace(
                "hello copper lantern",
                "<p>Inert HTML</p><img src=\"https://example.invalid/tracker\"><script>alert(1)</script>");
            c.openLocal(write("html.data", html), true);
            wait(c);
            CHECK(c.text().contains("Inert HTML"));
            c.openLocal(archive);
            wait(c);
            CHECK(c.sourceKind() == "mbox");
            CHECK(c.messages().size() == 2);
            c.quickInspect();
            wait(c);
            CHECK(c.messages().size() == 2);
            CHECK(c.coverage().contains("Sample:"));
            c.inspectMessage(c.messages()[0].toMap()["id"].toLongLong());
            wait(c);
            CHECK(c.text().contains("copper lantern"));
            auto sample_id = c.messages()[0].toMap()["id"].toLongLong();
            c.indexArchive();
            CHECK(c.canBrowse());
            c.inspectMessage(sample_id);
            wait(c);
            CHECK(c.text().contains("copper lantern"));
            CHECK(c.messages().size() == 2);
            CHECK(c.coverage().contains("Search ready"));
            c.search("lantern");
            wait(c);
            CHECK(c.messages().size() == 2);
            c.inspectMessage(c.messages()[0].toMap()["id"].toLongLong());
            wait(c);
            CHECK(c.raw().startsWith("From:"));
            c.inspectRange("0", "32");
            wait(c);
            CHECK(c.hex().startsWith("000000000000"));
            c.inspectRange("18446744073709551615", "32");
            wait(c);
            CHECK(c.notice().contains("exceeds"));
            c.search("lantern");
            wait(c);
            c.inspectMessage(c.messages()[0].toMap()["id"].toLongLong());
            wait(c);
            QQmlApplicationEngine view;
            view.rootContext()->setContextProperty("workstation", &c);
            view.load(QUrl::fromLocalFile(QStringLiteral(AFD_SOURCE_DIR "/src/ui/Main.qml")));
            CHECK(!view.rootObjects().isEmpty());
            auto *window = qobject_cast<QQuickWindow *>(view.rootObjects()[0]);
            CHECK(window);
            CHECK(QTest::qWaitForWindowExposed(window));
            window->requestActivate();
            CHECK(QTest::qWaitForWindowActive(window));
            auto *setup = window->findChild<QObject *>("downloadSetup");
            CHECK(setup);
            until([&] { return setup->property("visible").toBool(); });
            click(window, "downloadSetupLaterButton");
            QTest::qWait(200);
            CHECK(c.needsDownloadFolder());
            CHECK(!setup->property("visible").toBool());
            // Adding a torrent reopens setup and waits for a confirmed folder.
            click(window, "desktopNav1");
            item(window->contentItem(), "magnetField")->setProperty("text", torrent.toString());
            click(window, "addTorrentButton");
            until([&] { return setup->property("visible").toBool(); });
            CHECK(!c.torrentOpen());
            c.chooseDownloadFolder(message);
            wait(c);
            CHECK(c.needsDownloadFolder());
            CHECK(!c.downloadFolderError().isEmpty());
            CHECK(setup->property("visible").toBool());
            auto *folderPicker = window->findChild<QObject *>("downloadFolderPicker");
            CHECK(folderPicker);
            // Qt Test drives the Qt picker, not the operating system's dialog.
            CHECK(folderPicker->setProperty("options", 8)); // DontUseNativeDialog
            CHECK(QFileInfo(c.folderPickerStart().toLocalFile()).isDir());
            click(window, "chooseDownloadFolderButton");
            CHECK(folderPicker->property("visible").toBool());
            auto chosen = temp.path() + "/chosen-downloads";
            CHECK(QDir().mkpath(chosen));
            CHECK(folderPicker->setProperty("selectedFolder", QUrl::fromLocalFile(chosen)));
            CHECK(QMetaObject::invokeMethod(folderPicker, "accept"));
            until([&] { return c.torrentOpen() && !c.busy() && !c.files().isEmpty(); });
            QTest::qWait(200);
            window->requestActivate();
            CHECK(QTest::qWaitForWindowActive(window));
            CHECK(!c.needsDownloadFolder());
            CHECK(c.downloadFolderError().isEmpty());
            CHECK(c.downloadFolder() == chosen);
            c.chooseDownloadFolder(QUrl("content://example.invalid/tree/downloads"));
            wait(c);
            CHECK(c.downloadFolder() == chosen);
            CHECK(c.downloadFolderError().contains("local folder"));
            CHECK(!setup->property("visible").toBool());
            CHECK(c.torrentFolder().startsWith(chosen + "/"));
            const auto savedFolder = c.torrentFolder();
            CHECK(QFileInfo::exists(savedFolder + "/session.resume"));
            CHECK(window->property("section").toInt() == 1);
            click(window, "torrentFolder:fixture/");
            wait(c);
            click(window, "torrentFolder:fixture/folder/");
            wait(c);
            CHECK(c.directory() == "fixture/folder/");
            CHECK(c.files().size() > 8);
            const auto emptyId = c.files()[8].toMap()["id"].toLongLong();
            CHECK(emptyId >= 0);
            auto *catalog = item(window->contentItem(), "torrentFilesList");
            CHECK(catalog);
            catalog->setProperty("contentY", 6 * 62);
            QTest::qWait(100);
            const auto listPosition = catalog->property("contentY").toReal();
            CHECK(listPosition > 0);
            click(window, "torrentFile:" + QString::number(emptyId));
            wait(c);
            CHECK(window->property("section").toInt() == 2);
            click(window, "backToFilesButton");
            CHECK(window->property("section").toInt() == 1);
            CHECK(c.directory() == "fixture/folder/");
            CHECK(qAbs(catalog->property("contentY").toReal() - listPosition) < 1);
            click(window, "torrentFile:" + QString::number(emptyId));
            wait(c);
            QTest::keySequence(window, QKeySequence::Back);
            QCoreApplication::processEvents();
            CHECK(window->property("section").toInt() == 1);
            CHECK(c.directory() == "fixture/folder/");
            window->resize(390, 844);
            QTest::qWait(100);
            click(window, "torrentFile:" + QString::number(emptyId));
            wait(c);
            click(window, "backToFilesButton");
            CHECK(window->property("section").toInt() == 1);
            CHECK(c.directory() == "fixture/folder/");
            window->resize(1380, 870);
            QTest::qWait(100);
            QFile keep(savedFolder + "/keep.txt");
            CHECK(keep.open(QIODevice::WriteOnly));
            keep.write("keep");
            keep.close();
            click(window, "closeTorrentButton");
            wait(c);
            CHECK(!c.torrentOpen());
            CHECK(QFileInfo::exists(keep.fileName()));
            c.restoreTorrent();
            wait(c);
            CHECK(c.torrentOpen());
            CHECK(c.torrentFolder() == savedFolder);
            click(window, "desktopNav5");
            click(window, "changeDownloadFolderButton");
            CHECK(folderPicker->property("visible").toBool());
            CHECK(QMetaObject::invokeMethod(folderPicker, "close"));
            window->requestActivate();
            CHECK(QTest::qWaitForWindowActive(window));
            CHECK(c.downloadFolder() == chosen);
            auto futureFolder = temp.path() + "/future-downloads";
            c.refresh();
            CHECK(!c.busy());
            c.chooseDownloadFolder(QUrl::fromLocalFile(futureFolder));
            wait(c);
            CHECK(c.downloadFolder() == futureFolder);
            CHECK(c.torrentFolder() == savedFolder);
            c.closeTorrent();
            wait(c);
            c.addTorrent(torrent.toString());
            wait(c);
            CHECK(c.torrentFolder().startsWith(futureFolder + "/"));
            CHECK(QFileInfo::exists(keep.fileName()));
            CHECK(!QFileInfo::exists(c.torrentFolder() + "/fixture/payload.bin"));
            c.closeTorrent();
            wait(c);
            {
                Controller reopened(nullptr, true);
                CHECK(!reopened.needsDownloadFolder());
                CHECK(reopened.downloadFolder() == futureFolder);
            }
            c.openLocal(archive);
            wait(c);
            c.indexArchive();
            wait(c);
            c.inspectMessage(c.messages()[0].toMap()["id"].toLongLong());
            wait(c);
            for (int section = 0; section < 6; ++section) {
                click(window, "desktopNav" + QString::number(section));
                CHECK(window->property("section").toInt() == section);
            }
            click(window, "desktopNav0");
            click(window, "openTorrentWorkspaceButton");
            CHECK(window->property("section").toInt() == 1);
            // Keyboard activation must work independently of pointer activation.
            click(window, "desktopNav0");
            auto *torrentButton = item(window->contentItem(), "openTorrentWorkspaceButton");
            CHECK(torrentButton);
            torrentButton->forceActiveFocus();
            QTest::keyClick(window, Qt::Key_Space);
            CHECK(window->property("section").toInt() == 1);
            click(window, "openFileButton");
            auto *picker = window->findChild<QObject *>("localPicker");
            CHECK(picker);
            CHECK(picker->property("visible").toBool());
            CHECK(QMetaObject::invokeMethod(picker, "close"));
            window->requestActivate();
            CHECK(QTest::qWaitForWindowActive(window));
            click(window, "desktopNav2");
            click(window, "readRangeButton");
            const auto bodyBeforeRange = c.text();
            wait(c);
            CHECK(c.hex().startsWith("000000000000"));
            CHECK(c.text() == bodyBeforeRange);
            CHECK(c.notice().contains("preview"));
            click(window, "rangeTextButton");
            CHECK(window->property("fileTab").toInt() == 1);
            click(window, "desktopNav3");
            click(window, "quickSampleButton");
            wait(c);
            CHECK(c.coverage().contains("Sample:"));
            c.inspectMessage(c.messages()[0].toMap()["id"].toLongLong());
            wait(c);
            CHECK(!c.raw().isEmpty());
            c.resetIndex();
            wait(c);
            CHECK(c.messages().isEmpty());
            CHECK(c.raw().isEmpty());
            CHECK(c.text().isEmpty());
            CHECK(!window->property("reading").toBool());
            click(window, "indexArchiveButton");
            wait(c);
            CHECK(c.coverage().contains("Search ready"));
            click(window, "desktopNav5");
            QTest::keySequence(window, QKeySequence::Find);
            QCoreApplication::processEvents();
            CHECK(window->property("section").toInt() == 3);
            CHECK(item(window->contentItem(), "searchField")->hasActiveFocus());
            click(window, "searchField");
            QTest::keyClick(window, Qt::Key_Z);
            QTest::keyClick(window, Qt::Key_Z);
            QTest::keyClick(window, Qt::Key_Z);
            CHECK(item(window->contentItem(), "searchField")->property("text").toString() == "zzz");
            click(window, "findButton");
            wait(c);
            CHECK(c.messages().isEmpty());
            click(window, "searchField");
            QTest::keySequence(window, QKeySequence::SelectAll);
            QTest::keyClick(window, Qt::Key_Backspace);
            QTest::keyClick(window, Qt::Key_Return);
            wait(c);
            CHECK(c.messages().size() == 2);
            click(window, "messageRow" + c.messages()[0].toMap()["id"].toString());
            wait(c);
            CHECK(window->property("reading").toBool());
            CHECK(c.text().contains("copper lantern"));
            click(window, "readerTab2");
            CHECK(window->property("readerTab").toInt() == 2);
            click(window, "readerTab0");
            for (int i = 0; i < 10; ++i) {
                QCoreApplication::processEvents();
                QThread::msleep(20);
            }
            auto *list = window->findChild<QObject *>("messageList");
            CHECK(list);
            CHECK(list->property("count").toInt() == 2);
            window->setProperty("reading", false);
            auto screenshots = qEnvironmentVariable("AFD_SCREENSHOT_DIR");
            if (!screenshots.isEmpty()) {
                CHECK(window->grabWindow().save(screenshots + "/desktop-emails.png"));
            }
            window->resize(390, 844);
            QTest::qWait(100);
            click(window, "mobileNav0");
            CHECK(window->property("section").toInt() == 0);
            click(window, "mobileNav1");
            CHECK(window->property("section").toInt() == 1);
            // Bring the remaining navigation buttons into the horizontal viewport.
            auto *mobile = item(window->contentItem(), "mobileNav3");
            CHECK(mobile);
            mobile->parentItem()->parentItem()->parentItem()->setProperty("contentX", 240);
            click(window, "mobileNav3");
            CHECK(window->property("section").toInt() == 3);
            if (!screenshots.isEmpty()) {
                for (int i = 0; i < 10; ++i) {
                    QCoreApplication::processEvents();
                    QThread::msleep(20);
                }
                CHECK(window->grabWindow().save(screenshots + "/mobile-emails.png"));
            }
            click(window, "messageRow" + c.messages()[0].toMap()["id"].toString());
            wait(c);
            CHECK(window->property("reading").toBool());
            CHECK(c.text().contains("copper lantern"));
            if (!screenshots.isEmpty()) {
                for (int i = 0; i < 10; ++i) {
                    QCoreApplication::processEvents();
                    QThread::msleep(20);
                }
                CHECK(window->grabWindow().save(screenshots + "/mobile-reader.png"));
            }
            click(window, "readerTab1");
            CHECK(window->property("readerTab").toInt() == 1);
            click(window, "backToListButton");
            CHECK(!window->property("reading").toBool());
            c.openLocal(message);
            wait(c);
            CHECK(window->property("section").toInt() == 3);
            CHECK(window->property("reading").toBool());
            CHECK(c.messages().size() == 1);
            click(window, "readerTab2");
            CHECK(window->property("readerTab").toInt() == 2);
            c.openLocal(archive);
            wait(c);
            CHECK(window->property("section").toInt() == 3);
            CHECK(!window->property("reading").toBool());
            CHECK(c.messages().size() == 2);
            CHECK(c.coverage().contains("Sample:"));
        }
        QSettings().clear();
        QDir state(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation));
        CHECK(state.dirName() == app.applicationName());
        state.removeRecursively();
        std::cout << "PASS native controller: EML, DKIM inspection, MBOX sample, full index, search, message "
                     "selection, safe preview bounds; real desktop/mobile mouse clicks and keyboard search\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
