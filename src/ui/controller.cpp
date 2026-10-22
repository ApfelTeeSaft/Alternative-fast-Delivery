#include "ui/controller.hpp"
#include "core/source.hpp"
#include "dkim/dkim.hpp"
#include "email/mbox.hpp"
#include "email/message.hpp"
#include "indexing/index.hpp"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDnsLookup>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QTemporaryFile>
#include <QTextDocument>
#include <QUuid>
#include <QtConcurrent>
#include <algorithm>

namespace {
class InertDocument final : public QTextDocument {
    QVariant loadResource(int, const QUrl &) override {
        return {};
    }
};
QString q(const std::string &s) {
    return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}
std::filesystem::path path(const QString &s) {
    return std::filesystem::u8path(s.toUtf8().toStdString());
}
QString qs(const std::filesystem::path &p) {
    auto s = p.u8string();
    return QDir::fromNativeSeparators(
        QString::fromUtf8(reinterpret_cast<const char *>(s.data()), static_cast<qsizetype>(s.size())));
}
QString coverage_label(afd::Coverage c) {
    return QString("%1 emails · %2% scanned · %3%4")
        .arg(c.messages)
        .arg(c.total ? 100.0 * static_cast<double>(c.scanned) / static_cast<double>(c.total) : 0, 0, 'f', 1)
        .arg(c.complete ? "Search ready" : "Search is incomplete")
        .arg(c.omitted ? QString(" · %1 bodies not searched").arg(c.omitted) : "");
}
QString transfer_label(const afd::TransferStats &s) {
    return QString("%1 peers · ↓ %2 KiB/s · ↑ %3 KiB/s%4")
        .arg(s.peers)
        .arg(s.down / 1024)
        .arg(s.up / 1024)
        .arg(s.paused ? " · Paused" : "");
}
} // namespace
int Controller::uploadKiB() const {
    return QSettings().value("uploadKiB", 64).toInt();
}
int Controller::quotaMiB() const {
    return QSettings().value("quotaMiB", 2048).toInt();
}
Controller::Controller(QObject *parent, bool local_only) : QObject(parent), local_only_(local_only) {
    data_ = path(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation));
    std::filesystem::create_directories(data_);
    db_ = data_ / "index.sqlite";
    download_folder_ = QSettings().value("downloadFolder").toString();
#if defined(Q_OS_ANDROID) || defined(Q_OS_IOS)
    suggested_download_folder_ = qs(data_ / "downloads");
#else
    auto downloads = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    suggested_download_folder_ =
        downloads.isEmpty() ? qs(data_ / "downloads") : QDir(downloads).filePath("AFD");
#endif
    timer_.setInterval(2000);
    connect(&timer_, &QTimer::timeout, this, &Controller::refresh);
    timer_.start();
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this] {
        stop_.request_stop();
        future_.waitForFinished();
        browse_future_.waitForFinished();
        if (engine_)
            try {
                engine_->pause();
                engine_->save(resume_);
            } catch (...) {
            }
    });
}
void Controller::chooseDownloadFolder(QUrl url) {
    run([this, url] {
        try {
            if (!url.isLocalFile() || url.toLocalFile().isEmpty() || !QDir::isAbsolutePath(url.toLocalFile()))
                throw std::runtime_error("Choose a local folder. Shared-storage links are not supported; use "
                                         "the suggested folder instead.");
            auto folder = QDir::cleanPath(QFileInfo(url.toLocalFile()).absoluteFilePath());
            if (!QDir().mkpath(folder))
                throw std::runtime_error("Could not create that folder. Choose another folder.");
            QTemporaryFile check(QDir(folder).filePath(".afd-write-XXXXXX"));
            if (!check.open())
                throw std::runtime_error("AFD cannot write to that folder. Choose another folder.");
            check.close();
            QSettings settings;
            settings.setValue("downloadFolder", folder);
            settings.sync();
            if (settings.status() != QSettings::NoError)
                throw std::runtime_error("Could not save the download folder. Try again.");
            post([this, folder] {
                download_folder_ = folder;
                download_folder_error_.clear();
                notice_ = "Download folder saved. Existing files stay where they are.";
            });
        } catch (const std::exception &e) {
            auto message = q(e.what());
            post([this, message] {
                download_folder_error_ = message;
                notice_ = message;
            });
        }
    });
}
QUrl Controller::folderPickerStart() const {
    auto folder = download_folder_.isEmpty() ? suggested_download_folder_ : download_folder_;
    while (!QDir(folder).exists()) {
        auto parent = QFileInfo(folder).absolutePath();
        if (parent == folder || parent.isEmpty())
            return QUrl::fromLocalFile(QDir::homePath());
        folder = parent;
    }
    return QUrl::fromLocalFile(folder);
}
void Controller::clear_reader() {
    text_.clear();
    raw_.clear();
    headers_.clear();
    mime_.clear();
    signatures_.clear();
}
Controller::~Controller() {
    stop_.request_stop();
    future_.waitForFinished();
    browse_future_.waitForFinished();
}
void Controller::post(std::function<void()> fn) {
    QMetaObject::invokeMethod(
        this,
        [this, fn = std::move(fn)] {
            fn();
            emit changed();
        },
        Qt::QueuedConnection);
}
void Controller::run(std::function<void()> fn, bool indexing) {
    if (busy_ || browsing_) {
        if (refreshing_ && !after_refresh_) {
            after_refresh_ = [this, fn = std::move(fn), indexing]() mutable { run(std::move(fn), indexing); };
            emit changed();
        }
        return;
    }
    busy_ = true;
    indexing_ = indexing;
    stop_ = afd::StopSource{};
    emit changed();
    auto *watcher = new QFutureWatcher<void>(this);
    connect(watcher, &QFutureWatcher<void>::finished, this, [this, watcher] {
        busy_ = false;
        indexing_ = false;
        refreshing_ = false;
        watcher->deleteLater();
        auto next = std::move(after_refresh_);
        after_refresh_ = {};
        if (next)
            next();
        else
            emit changed();
    });
    future_ = QtConcurrent::run([this, fn = std::move(fn)] {
        try {
            fn();
        } catch (const std::exception &e) {
            auto msg = QString::fromUtf8(e.what());
            post([this, msg] { notice_ = msg; });
        }
        if (pause_requested_.exchange(false) && engine_) {
            engine_->pause();
            try {
                engine_->save(resume_);
            } catch (...) {
            }
        }
    });
    watcher->setFuture(future_);
}
void Controller::browse(std::function<void()> fn) {
    if (!busy_ || refreshing_) {
        run(std::move(fn));
        return;
    }
    if (!indexing_ || browsing_)
        return;
    browsing_ = true;
    emit changed();
    auto *watcher = new QFutureWatcher<void>(this);
    connect(watcher, &QFutureWatcher<void>::finished, this, [this, watcher] {
        browsing_ = false;
        watcher->deleteLater();
        emit changed();
    });
    browse_future_ = QtConcurrent::run([this, fn = std::move(fn)] {
        try {
            fn();
        } catch (const std::exception &e) {
            auto msg = q(e.what());
            post([this, msg] { notice_ = msg; });
        }
        if (pause_requested_.exchange(false) && engine_) {
            engine_->pause();
            try {
                engine_->save(resume_);
            } catch (...) {
            }
        }
    });
    watcher->setFuture(browse_future_);
}
std::unique_ptr<afd::Source> Controller::source() {
    if (selected_file_ >= 0 && engine_)
        return std::make_unique<afd::TorrentSource>(*engine_, selected_file_, size_);
    if (!local_.empty())
        return std::make_unique<afd::FileSource>(local_);
    throw std::runtime_error("Select a source file first");
}
void Controller::show_range(std::uint64_t offset, std::size_t length) {
    if (length > 65536)
        throw std::runtime_error("Preview limit is 64 KiB");
    auto src = source();
    auto bytes = src->read(offset, length, stop_.get_token());
    auto hex = q(afd::hex_preview(bytes, offset));
    auto text = QString::fromUtf8(bytes.data(), static_cast<qsizetype>(bytes.size()));
    auto type = q(afd::detect_type(bytes));
    post([this, hex, text, type, offset, length] {
        hex_ = hex;
        file_text_ = text;
        notice_ = type + QString(" · %1 bytes from byte %2 · preview").arg(length).arg(offset);
    });
}
void Controller::openLocal(QUrl url, bool emailOnly) {
    run([this, url, emailOnly] {
        auto local = url.toLocalFile();
        if (local.isEmpty()) {
            QFile input(url.toString());
            if (!input.open(QIODevice::ReadOnly))
                throw std::runtime_error("Document provider could not open this file");
            if (input.size() < 0 || input.size() > 2LL * 1024 * 1024 * 1024 ||
                QStorageInfo(qs(data_)).bytesAvailable() < input.size() + 64 * 1024 * 1024)
                throw std::runtime_error(
                    "Provider import exceeds 2 GiB or available storage; copy locally first");
            auto target =
                qs(data_ / ("import-" + QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString() +
                            (emailOnly ? ".eml" : ".bin")));
            QSaveFile output(target);
            if (!output.open(QIODevice::WriteOnly))
                throw std::runtime_error("Cannot create private document copy");
            std::int64_t copied = 0;
            while (!input.atEnd()) {
                if (stop_.stop_requested())
                    throw std::runtime_error("Cancelled");
                auto bytes = input.read(64 * 1024);
                if (bytes.isEmpty() && input.error() != QFile::NoError)
                    throw std::runtime_error("Document import failed");
                copied += bytes.size();
                if (copied > 2LL * 1024 * 1024 * 1024 || output.write(bytes) != bytes.size())
                    throw std::runtime_error("Document import storage limit");
            }
            if (!output.commit())
                throw std::runtime_error("Cannot commit private document copy");
            local = target;
        }
        QFileInfo info(local);
        if (!info.isFile())
            throw std::runtime_error("Select a regular file");
        local_ = path(info.absoluteFilePath());
        selected_file_ = -1;
        message_id_ = 0;
        message_raw_.clear();
        samples_.clear();
        size_ = static_cast<std::uint64_t>(info.size());
        identity_ =
            afd::sha256(info.canonicalFilePath().toUtf8().toStdString() + ":" + std::to_string(size_) + ":" +
                        std::to_string(info.lastModified().toMSecsSinceEpoch()));
        selected_name_ = info.fileName().toStdString();
        mbox_ = info.suffix().compare("mbox", Qt::CaseInsensitive) == 0;
        afd::FileSource src(local_);
        auto prefix = src.read(0, static_cast<std::size_t>(std::min<std::uint64_t>(65536, size_)));
        if (afd::detect_type(prefix) == "MBOX")
            mbox_ = true;
        auto name = info.fileName();
        QString kind = mbox_ ? "mbox" : "file";
        post([this, name, kind] {
            sourceKind_ = kind;
            sourceName_ = name;
            messages_.clear();
            text_.clear();
            raw_.clear();
            headers_.clear();
            mime_.clear();
            signatures_.clear();
            coverage_ = "Not scanned yet";
        });
        show_range(0, static_cast<std::size_t>(std::min<std::uint64_t>(size_, 65536)));
        if (!mbox_ &&
            (emailOnly || info.suffix().compare("eml", Qt::CaseInsensitive) == 0 ||
             afd::detect_type(prefix) == "RFC 5322 email") &&
            size_ <= afd::max_range) {
            auto bytes = src.read(0, static_cast<std::size_t>(size_));
            show_message(std::string(bytes.begin(), bytes.end()), true);
            kind = "email";
        }
        if (mbox_)
            sample_archive();
        QSettings settings;
        settings.setValue("lastLocal", local);
        post([this, kind] {
            sourceKind_ = kind;
            emit sourceOpened(kind);
        });
    });
}
void Controller::addTorrent(QString input) {
    if (needsDownloadFolder()) {
        notice_ = "Choose a download folder first.";
        emit changed();
        return;
    }
    auto downloads = download_folder_;
    run([this, input, downloads] {
        if (engine_)
            throw std::runtime_error("Close the current torrent before opening another");
        std::string value = input.trimmed().toUtf8().toStdString();
        if (value.starts_with("file:"))
            value = QUrl(input).toLocalFile().toUtf8().toStdString();
        else if (value.starts_with("content:")) {
            QFile document(input.trimmed());
            if (!document.open(QIODevice::ReadOnly))
                throw std::runtime_error("Document provider could not open torrent metadata");
            auto bytes = document.read(static_cast<qint64>(afd::max_range) + 1);
            if (document.error() != QFile::NoError || bytes.isEmpty() || bytes.size() > afd::max_range)
                throw std::runtime_error("Torrent metadata unreadable or exceeds 16 MiB");
            auto destination = data_ / ("metadata-" + afd::sha256(bytes.toStdString()) + ".torrent");
            QSaveFile copy(qs(destination));
            if (!copy.open(QIODevice::WriteOnly) || copy.write(bytes) != bytes.size() || !copy.commit())
                throw std::runtime_error("Cannot save private torrent metadata");
            value = qs(destination).toUtf8().toStdString();
        }
        auto id = afd::sha256(value);
        auto root = path(downloads) / id;
        engine_ = std::make_unique<afd::TorrentEngine>(root, local_only_);
        watch_progress();
        resume_ = root / "session.resume";
        files_loaded_ = false;
        try {
            engine_->set_quota(QSettings().value("quotaMiB", 2048).toULongLong() * 1024 * 1024);
            engine_->open(value);
            engine_->set_upload_limit(QSettings().value("uploadKiB", 64).toInt() * 1024);
        } catch (...) {
            engine_.reset();
            throw;
        }
        QSettings settings;
        settings.setValue("resume", qs(resume_));
        settings.setValue("torrentRoot", qs(root));
        auto folder = qs(root);
        post([this, folder] {
            torrent_open_ = true;
            torrent_folder_ = folder;
            notice_ = "Torrent added. Getting the file list…";
        });
        update_stats();
    });
}
void Controller::restoreTorrent() {
    if (needsDownloadFolder()) {
        notice_ = "Choose a download folder first.";
        emit changed();
        return;
    }
    run([this] {
        if (engine_)
            throw std::runtime_error("Close the current torrent first");
        QSettings settings;
        resume_ = path(settings.value("resume").toString());
        auto root = path(settings.value("torrentRoot").toString());
        if (resume_.empty() || !std::filesystem::exists(resume_))
            throw std::runtime_error("No saved torrent snapshot available");
        files_loaded_ = false;
        engine_ = std::make_unique<afd::TorrentEngine>(root, local_only_);
        watch_progress();
        try {
            engine_->restore(resume_);
        } catch (...) {
            engine_.reset();
            throw;
        }
        engine_->set_quota(QSettings().value("quotaMiB", 2048).toULongLong() * 1024 * 1024);
        engine_->set_upload_limit(QSettings().value("uploadKiB", 64).toInt() * 1024);
        auto folder = qs(root);
        post([this, folder] {
            torrent_open_ = true;
            torrent_folder_ = folder;
        });
        update_stats();
        auto selected = settings.value(q("selected/" + engine_->identity()), -1).toInt();
        if (selected >= 0 && engine_->metadata_ready())
            select_file(selected, false);
    });
}
void Controller::watch_progress() {
    engine_->set_progress_callback([this](const afd::TransferStats &stats) {
        auto label = transfer_label(stats);
        post([this, label] { stats_ = label; });
    });
}
void Controller::update_stats() {
    if (!engine_)
        return;
    auto s = engine_->stats();
    if (!files_loaded_ && s.metadata) {
        torrent_files_ = engine_->files();
        files_loaded_ = true;
        show_directory("");
        engine_->save(resume_);
        post([this] {
            notice_ = "File list ready. Choose a file to open.";
            emit torrentOpened();
        });
    }
    auto label = transfer_label(s);
    auto [used, limit] = engine_->cache_usage();
    label += QString(" · Cache %1/%2 MiB").arg(used / (1024 * 1024)).arg(limit / (1024 * 1024));
    post([this, label] { stats_ = label; });
}
void Controller::show_directory(std::string directory) {
    if (directory == "..") {
        directory = current_directory_;
        if (!directory.empty())
            directory.pop_back();
        auto slash = directory.rfind('/');
        directory = slash == directory.npos ? "" : directory.substr(0, slash + 1);
    }
    QVariantList rows;
    std::map<std::string, std::uint64_t> folders;
    for (const auto &file : torrent_files_) {
        auto name = file.path;
        std::replace(name.begin(), name.end(), '\\', '/');
        if (!name.starts_with(directory))
            continue;
        auto rest = name.substr(directory.size());
        auto slash = rest.find('/');
        if (slash != rest.npos) {
            folders[directory + rest.substr(0, slash + 1)] += file.size;
            continue;
        }
        rows.push_back(QVariantMap{{"id", file.id},
                                   {"folder", false},
                                   {"path", q(file.path)},
                                   {"label", q(rest)},
                                   {"size", QVariant::fromValue<qulonglong>(file.size)}});
    }
    QVariantList ordered;
    for (const auto &[folder, size] : folders)
        ordered.push_back(QVariantMap{{"id", -1},
                                      {"folder", true},
                                      {"path", q(folder)},
                                      {"label", q(folder.substr(directory.size()))},
                                      {"size", QVariant::fromValue<qulonglong>(size)}});
    ordered.append(rows);
    if (ordered.isEmpty() && !directory.empty())
        throw std::runtime_error("Directory is not present in torrent metadata");
    current_directory_ = directory;
    auto label = q(directory);
    post([this, ordered, label] {
        files_ = ordered;
        directory_label_ = label;
    });
}
void Controller::openDirectory(QString directory) {
    run([this, directory] {
        if (!engine_ || !files_loaded_)
            throw std::runtime_error("Waiting for torrent metadata");
        show_directory(directory.toUtf8().toStdString());
    });
}
void Controller::refresh() {
    if (!busy_ && !browsing_ && engine_) {
        refreshing_ = true;
        run([this] { update_stats(); });
    }
}
void Controller::selectFile(int id) {
    run([this, id] { select_file(id, true); });
}
void Controller::select_file(int id, bool preview) {
    if (!engine_)
        throw std::runtime_error("No torrent open");
    auto fs = engine_->files();
    auto it = std::find_if(fs.begin(), fs.end(), [id](const auto &f) { return f.id == id; });
    if (it == fs.end())
        throw std::runtime_error("File not found");
    selected_file_ = id;
    local_.clear();
    message_id_ = 0;
    message_raw_.clear();
    samples_.clear();
    size_ = it->size;
    selected_name_ = it->path;
    identity_ = afd::sha256(engine_->identity() + ":" + std::to_string(id) + ":" + std::to_string(size_));
    mbox_ = afd::lower(it->path).ends_with(".mbox");
    auto name = q(it->path);
    auto kind = mbox_ ? QString("mbox") : QString("file");
    post([this, name, kind] {
        sourceKind_ = kind;
        sourceName_ = name;
        messages_.clear();
        text_.clear();
        file_text_.clear();
        hex_.clear();
        raw_.clear();
        headers_.clear();
        mime_.clear();
        signatures_.clear();
        coverage_ = "Not scanned yet";
    });
    if (preview) {
        show_range(0, static_cast<std::size_t>(std::min<std::uint64_t>(size_, 65536)));
        if (mbox_) {
            sample_archive();
        } else if (afd::lower(it->path).ends_with(".eml") && size_ <= afd::max_range) {
            auto src = source();
            auto bytes = src->read(0, static_cast<std::size_t>(size_), stop_.get_token());
            show_message(std::string(bytes.begin(), bytes.end()), true);
            kind = "email";
        }
        post([this, kind] {
            sourceKind_ = kind;
            emit sourceOpened(kind);
        });
    }
    update_stats();
    QSettings().setValue(q("selected/" + engine_->identity()), id);
    engine_->save(resume_);
}

void Controller::inspectRange(QString offset, QString length) {
    run([this, offset, length] {
        bool ok1 = false, ok2 = false;
        auto o = offset.toULongLong(&ok1);
        auto n = length.toULongLong(&ok2);
        if (!ok1 || !ok2 || n > 65536)
            throw std::runtime_error("Enter a byte offset and length (maximum 65536)");
        if (o <= size_)
            n = std::min<qulonglong>(n, size_ - o);
        show_range(o, static_cast<std::size_t>(n));
        if (engine_ && selected_file_ >= 0)
            engine_->save(resume_);
    });
}
void Controller::indexArchive() {
    run(
        [this] {
            auto src = source();
            afd::Index index(db_, identity_, size_);
            auto last = std::chrono::steady_clock::now();
            index.build(*src, mbox_, stop_.get_token(), [this, &last](afd::Coverage c) {
                if (std::chrono::steady_clock::now() - last > std::chrono::milliseconds(250)) {
                    last = std::chrono::steady_clock::now();
                    auto label = coverage_label(c);
                    post([this, label] { coverage_ = label; });
                }
            });
            auto label = coverage_label(index.coverage());
            post([this, label] {
                coverage_ = label;
                notice_ = "Scan saved. Search covers the scanned email text and headers. Some bodies may be "
                          "skipped.";
            });
            load_page({}, 0, false);
            if (engine_ && selected_file_ >= 0)
                engine_->save(resume_);
        },
        true);
}
void Controller::quickInspect() {
    run([this] { sample_archive(); });
}
void Controller::sample_archive() {
    message_id_ = 0;
    message_raw_.clear();
    if (!mbox_)
        throw std::runtime_error(
            "Quick sample requires an MBOX source; import EML directly for a single message");
    auto src = source();
    class Window final : public afd::Source {
        afd::Bytes bytes_;

      public:
        explicit Window(afd::Bytes bytes) : bytes_(std::move(bytes)) {}
        std::uint64_t size() const override {
            return bytes_.size();
        }
        afd::Bytes read(std::uint64_t offset, std::size_t length, afd::StopToken) override {
            afd::check_range(size(), offset, length);
            return {bytes_.begin() + static_cast<std::ptrdiff_t>(offset),
                    bytes_.begin() + static_cast<std::ptrdiff_t>(offset + length)};
        }
    } sample(src->read(0, static_cast<std::size_t>(std::min<std::uint64_t>(size_, 1024 * 1024)),
                       stop_.get_token()));
    samples_.clear();
    QVariantList list;
    std::int64_t id = -1;
    afd::scan_mbox(
        sample, 0,
        [&](afd::MessageRange range) {
            if (range.end == sample.size() && sample.size() < size_)
                return true;
            try {
                auto bytes = sample.read(range.begin, static_cast<std::size_t>(range.end - range.begin), {});
                auto m = afd::parse_message(std::string_view(bytes.data(), bytes.size()));
                samples_[id] = {range.begin, range.end - range.begin};
                list.push_back(QVariantMap{{"id", QVariant::fromValue<qlonglong>(id--)},
                                           {"sender", q(m.sender)},
                                           {"recipient", q(m.recipient)},
                                           {"subject", q(m.subject)},
                                           {"date", q(m.date)},
                                           {"size", QVariant::fromValue<qulonglong>(range.end - range.begin)},
                                           {"dkim", "Not verified"},
                                           {"attachment", m.attachments}});
            } catch (const std::exception &) {
            }
            return list.size() < 100;
        },
        stop_.get_token());
    post([this, list] {
        messages_ = list;
        coverage_ = QString("Sample: %1 emails from the first 1 MiB · scan to search the full archive")
                        .arg(list.size());
        clear_reader();
        notice_ = "Sample loaded. Scan emails to search the full archive.";
        emit messagesLoaded();
    });
    if (engine_ && selected_file_ >= 0)
        engine_->save(resume_);
}
void Controller::load_page(const QString &query, int page, bool reset_reader) {
    afd::Index index(db_, identity_, size_);
    auto rows = index.query(query.toUtf8().toStdString(), 100, page);
    QVariantList list;
    for (const auto &m : rows)
        list.push_back(QVariantMap{{"id", QVariant::fromValue<qlonglong>(m.id)},
                                   {"sender", q(m.sender)},
                                   {"recipient", q(m.recipient)},
                                   {"subject", q(m.subject)},
                                   {"date", q(m.date)},
                                   {"size", QVariant::fromValue<qulonglong>(m.size)},
                                   {"dkim", q(m.dkim)},
                                   {"attachment", m.attachments}});
    auto label = coverage_label(index.coverage());
    if (reset_reader) {
        message_id_ = 0;
        message_raw_.clear();
    }
    post([this, list, label, reset_reader] {
        if (reset_reader)
            clear_reader();
        messages_ = list;
        coverage_ = label;
        if (reset_reader)
            emit messagesLoaded();
    });
}
void Controller::search(QString query, int page) {
    browse([this, query, page] {
        if (identity_.empty())
            throw std::runtime_error("Open and index an email source first");
        load_page(query, page);
    });
}
void Controller::checkIndex() {
    run([this] {
        afd::Index index(db_, identity_, size_);
        index.check_integrity();
        post([this] { notice_ = "SQLite integrity check passed."; });
    });
}
void Controller::resetIndex() {
    run([this] {
        if (identity_.empty())
            throw std::runtime_error("Open a source first");
        afd::Index::quarantine(db_);
        afd::Index fresh(db_, identity_, size_);
        message_id_ = 0;
        message_raw_.clear();
        auto label = coverage_label(fresh.coverage());
        post([this, label] {
            messages_.clear();
            clear_reader();
            coverage_ = label;
            notice_ = "Search data reset. A backup was kept. Choose Scan emails to start again.";
            emit messagesLoaded();
        });
    });
}
void Controller::show_message(std::string raw, bool standalone) {
    message_raw_ = std::move(raw);
    auto m = afd::parse_message(message_raw_);
    QVariantMap row;
    if (standalone) {
        message_id_ = -1;
        samples_[-1] = {0, size_};
        row = {{"id", -1},
               {"sender", q(m.sender)},
               {"recipient", q(m.recipient)},
               {"subject", q(m.subject)},
               {"date", q(m.date)},
               {"size", QVariant::fromValue<qulonglong>(size_)},
               {"dkim", "Not verified"},
               {"attachment", m.attachments}};
    }
    QString headers, mime;
    for (const auto &h : m.headers)
        headers += q(h.name + ": " + h.value + "\n");
    for (const auto &p : m.parts)
        mime += q(p.type + " · " + p.name + " · " + std::to_string(p.size) + " bytes · " + p.charset +
                  (p.attachment ? " · attachment (never opened automatically)" : "") + "\n");
    QString text = q(m.text.substr(0, 65536));
    if (m.text.empty() && !m.html.empty()) {
        InertDocument document;
        document.setHtml(q(m.html.substr(0, 65536)));
        text = "[HTML converted to inert text; original markup is in Raw.]\n\n" + document.toPlainText();
    }
    if (m.text.size() > 65536 || m.html.size() > 65536)
        text += "\n\n[Body preview limited to 64 KiB; indexed text and verification use the complete bounded "
                "message.]";
    auto original = q(message_raw_.substr(0, 65536));
    if (message_raw_.size() > 65536)
        original += "\n\n[Raw preview limited to 64 KiB. Use the byte inspector to read other ranges.]";
    if (headers.size() > 65536)
        headers = headers.left(65536) + "\n[Header preview truncated.]";
    auto results = afd::verify_dkim(
        message_raw_, [](const std::string &) { return afd::DnsKey{afd::DnsKey::State::Unavailable, {}}; });
    QString signatures;
    for (const auto &r : results)
        signatures += q(afd::status_name(r.status) + " · " + r.domain + " · " + r.detail + "\n");
    auto warning = q(m.warning).trimmed();
    post([this, text, original, headers, mime, signatures, row, warning] {
        text_ = text;
        raw_ = original;
        headers_ = headers;
        mime_ = mime;
        signatures_ = signatures;
        if (!row.isEmpty()) {
            messages_ = {row};
            coverage_ = "One email · ready to read";
        }
        notice_ = "Email opened. Remote images and attachments stay closed.";
        if (!warning.isEmpty())
            notice_ += " " + warning;
    });
}
void Controller::inspectMessage(qint64 id) {
    browse([this, id] {
        std::uint64_t offset = 0, length = 0;
        if (id < 0) {
            auto range = samples_.at(id);
            offset = range.first;
            length = range.second;
        } else {
            afd::Index index(db_, identity_, size_);
            auto m = index.get(id);
            offset = m.offset;
            length = m.size;
        }
        if (length > afd::max_range)
            throw std::runtime_error("Message exceeds inspection limit; use byte-range preview");
        auto src = source();
        auto bytes = src->read(offset, static_cast<std::size_t>(length), stop_.get_token());
        message_id_ = id;
        show_message(std::string(bytes.begin(), bytes.end()));
    });
}
void Controller::verify(bool dns) {
    run([this, dns] {
        if (message_raw_.empty())
            throw std::runtime_error("Open an EML or indexed message first");
        auto resolver = [this, dns](const std::string &name) {
            if (!dns || stop_.stop_requested())
                return afd::DnsKey{afd::DnsKey::State::Unavailable, {}};
            QDnsLookup lookup(QDnsLookup::TXT, q(name));
            QEventLoop loop;
            QTimer timeout;
            timeout.setSingleShot(true);
            connect(&lookup, &QDnsLookup::finished, &loop, &QEventLoop::quit);
            connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
            lookup.lookup();
            timeout.start(6000);
            loop.exec();
            if (stop_.stop_requested() || !lookup.isFinished()) {
                lookup.abort();
                return afd::DnsKey{afd::DnsKey::State::Unavailable, {}};
            }
            if (lookup.error() == QDnsLookup::NotFoundError)
                return afd::DnsKey{afd::DnsKey::State::Missing, {}};
            if (lookup.error() != QDnsLookup::NoError)
                return afd::DnsKey{afd::DnsKey::State::Unavailable, {}};
            std::string key;
            for (const auto &rr : lookup.textRecords()) {
                QByteArray joined;
                for (const auto &v : rr.values())
                    joined += v;
                if (joined.contains("p=")) {
                    if (!key.empty())
                        return afd::DnsKey{afd::DnsKey::State::Unavailable, {}};
                    key = joined.toStdString();
                }
            }
            return afd::DnsKey{key.empty() ? afd::DnsKey::State::Missing : afd::DnsKey::State::Found, key};
        };
        auto results = afd::verify_dkim(message_raw_, resolver);
        QString out;
        for (const auto &r : results)
            out += q(afd::status_name(r.status) + "\n" + r.domain + " / " + r.selector + "\n" + r.detail +
                     "\n\n");
        if (message_id_ > 0) {
            afd::Index index(db_, identity_, size_);
            std::string status = results.size() == 1 ? afd::status_name(results.front().status)
                                                     : "Multiple signatures · inspect details";
            index.set_dkim(message_id_, status);
        }
        post([this, out] {
            signatures_ = out;
            notice_ = "DKIM does not prove the sender's personal identity. Historical DNS keys may no longer "
                      "exist.";
        });
    });
}
void Controller::cancel() {
    if (after_refresh_) {
        after_refresh_ = {};
        notice_ = "Stopped.";
        emit changed();
        return;
    }
    stop_.request_stop();
    notice_ = "Stopping… Scan progress is saved.";
    emit changed();
}
void Controller::pauseTorrent(bool paused) {
    if (paused && busy()) {
        pause_requested_ = true;
        stop_.request_stop();
        return;
    }
    run([this, paused] {
        if (engine_) {
            if (paused)
                engine_->pause();
            else
                engine_->resume();
            engine_->save(resume_);
            update_stats();
        }
    });
}
void Controller::uploadLimit(int kib) {
    run([this, kib] {
        if (kib < 1 || kib > 1048576)
            throw std::runtime_error("Choose an upload cap from 1 to 1048576 KiB/s");
        if (engine_)
            engine_->set_upload_limit(kib * 1024);
        QSettings().setValue("uploadKiB", kib);
        post([this] { notice_ = "Upload limit saved. Pause the torrent to stop transfers."; });
    });
}
void Controller::storageLimit(int mib) {
    run([this, mib] {
        if (mib < 16 || mib > 1048576)
            throw std::runtime_error("Choose a piece quota between 16 and 1048576 MiB");
        if (engine_)
            engine_->set_quota(static_cast<std::uint64_t>(mib) * 1024 * 1024);
        QSettings().setValue("quotaMiB", mib);
        post([this] { notice_ = "Download limit saved."; });
    });
}
void Controller::clearTorrent() {
    close_torrent(true);
}
void Controller::closeTorrent() {
    close_torrent(false);
}
void Controller::close_torrent(bool delete_files) {
    run([this, delete_files] {
        if (!engine_)
            return;
        bool selected = selected_file_ >= 0;
        engine_->pause();
        if (!delete_files)
            engine_->save(resume_);
        engine_->remove(delete_files);
        engine_.reset();
        torrent_files_.clear();
        current_directory_.clear();
        if (delete_files && !resume_.empty())
            std::filesystem::remove(resume_);
        if (delete_files) {
            QSettings().remove("resume");
            QSettings().remove("torrentRoot");
        }
        files_loaded_ = false;
        if (selected) {
            selected_file_ = -1;
            identity_.clear();
            size_ = 0;
            message_id_ = 0;
            message_raw_.clear();
            samples_.clear();
        }
        post([this, selected, delete_files] {
            torrent_open_ = false;
            torrent_folder_.clear();
            files_.clear();
            directory_label_.clear();
            stats_ = "No transfers";
            if (selected) {
                sourceKind_ = "none";
                messages_.clear();
                sourceName_ = "No file open";
                text_.clear();
                file_text_.clear();
                hex_.clear();
                raw_.clear();
                headers_.clear();
                mime_.clear();
                signatures_.clear();
                coverage_ = "No email scanned";
            }
            notice_ = delete_files ? "Torrent deleted. Its downloaded files will be removed."
                                   : "Torrent closed. Downloads saved; use Reopen to continue.";
        });
    });
}
