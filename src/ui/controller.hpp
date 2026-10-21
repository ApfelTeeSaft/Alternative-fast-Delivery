#pragma once
#include "core/cancel.hpp"
#include "torrent/engine.hpp"
#include <QFuture>
#include <QObject>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <atomic>
#include <functional>
#include <map>
#include <memory>

class Controller final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool canBrowse READ canBrowse NOTIFY changed)
    Q_PROPERTY(bool needsDownloadFolder READ needsDownloadFolder NOTIFY changed)
    Q_PROPERTY(QString downloadFolder READ downloadFolder NOTIFY changed)
    Q_PROPERTY(QString downloadFolderError READ downloadFolderError NOTIFY changed)
    Q_PROPERTY(QUrl downloadFolderUrl READ downloadFolderUrl NOTIFY changed)
    Q_PROPERTY(QUrl folderPickerStart READ folderPickerStart NOTIFY changed)
    Q_PROPERTY(QUrl suggestedDownloadFolder READ suggestedDownloadFolder CONSTANT)
    Q_PROPERTY(QString suggestedDownloadPath READ suggestedDownloadPath CONSTANT)
    Q_PROPERTY(QString torrentFolder READ torrentFolder NOTIFY changed)
    Q_PROPERTY(bool torrentOpen READ torrentOpen NOTIFY changed)
    Q_PROPERTY(int uploadKiB READ uploadKiB NOTIFY changed)
    Q_PROPERTY(int quotaMiB READ quotaMiB NOTIFY changed)
    Q_PROPERTY(QString sourceName READ sourceName NOTIFY changed)
    Q_PROPERTY(QString sourceKind READ sourceKind NOTIFY changed)
    Q_PROPERTY(QString notice READ notice NOTIFY changed)
    Q_PROPERTY(QString coverage READ coverage NOTIFY changed)
    Q_PROPERTY(QString stats READ stats NOTIFY changed)
    Q_PROPERTY(QString text READ text NOTIFY changed)
    Q_PROPERTY(QString fileText READ fileText NOTIFY changed)
    Q_PROPERTY(QString hex READ hex NOTIFY changed)
    Q_PROPERTY(QString raw READ raw NOTIFY changed)
    Q_PROPERTY(QString headers READ headers NOTIFY changed)
    Q_PROPERTY(QString mime READ mime NOTIFY changed)
    Q_PROPERTY(QString signatures READ signatures NOTIFY changed)
    Q_PROPERTY(QVariantList files READ files NOTIFY changed)
    Q_PROPERTY(QString directory READ directory NOTIFY changed)
    Q_PROPERTY(QVariantList messages READ messages NOTIFY changed)
    bool busy_{};
    bool browsing_{}, indexing_{}, files_loaded_{}, refreshing_{};
    std::function<void()> after_refresh_;
    QString sourceName_ = "No file open", notice_ = "Open a file or add a torrent to start.",
            coverage_ = "No email scanned", stats_ = "No transfers";
    QString download_folder_, download_folder_error_, suggested_download_folder_, torrent_folder_;
    bool torrent_open_{}, local_only_{};
    QString text_, file_text_, hex_, raw_, headers_, mime_, signatures_;
    QString sourceKind_ = "none";
    QVariantList files_, messages_;
    QString directory_label_;
    std::string current_directory_;
    std::vector<afd::TorrentFile> torrent_files_;
    QFuture<void> future_;
    QFuture<void> browse_future_;
    afd::StopSource stop_;
    std::unique_ptr<afd::TorrentEngine> engine_;
    std::filesystem::path local_, db_, data_, resume_;
    std::string identity_;
    std::string selected_name_;
    int selected_file_ = -1;
    std::uint64_t size_{};
    std::int64_t message_id_{};
    std::string message_raw_;
    std::atomic_bool pause_requested_{false};
    std::map<std::int64_t, std::pair<std::uint64_t, std::uint64_t>> samples_;
    bool mbox_{};
    QTimer timer_;
    void run(std::function<void()>, bool indexing = false);
    void browse(std::function<void()>);
    void post(std::function<void()>);
    std::unique_ptr<afd::Source> source();
    void update_stats();
    void watch_progress();
    void show_directory(std::string);
    void select_file(int, bool preview);
    void load_page(const QString &, int, bool reset_reader = true);
    void show_range(std::uint64_t, std::size_t);
    void show_message(std::string, bool standalone = false);
    void sample_archive();
    void close_torrent(bool delete_files);
    void clear_reader();

  public:
    int uploadKiB() const;
    int quotaMiB() const;
    explicit Controller(QObject *parent = nullptr, bool local_only = false);
    ~Controller() override;
    bool busy() const {
        return (busy_ && !refreshing_) || browsing_ || static_cast<bool>(after_refresh_);
    }
    bool canBrowse() const {
        return !browsing_ && (!busy_ || indexing_ || (refreshing_ && !after_refresh_));
    }
    bool needsDownloadFolder() const {
        return download_folder_.isEmpty();
    }
    QString downloadFolder() const {
        return download_folder_;
    }
    QString downloadFolderError() const {
        return download_folder_error_;
    }
    QUrl downloadFolderUrl() const {
        return QUrl::fromLocalFile(download_folder_);
    }
    QUrl folderPickerStart() const;
    QUrl suggestedDownloadFolder() const {
        return QUrl::fromLocalFile(suggested_download_folder_);
    }
    QString suggestedDownloadPath() const {
        return suggested_download_folder_;
    }
    QString torrentFolder() const {
        return torrent_folder_;
    }
    bool torrentOpen() const {
        return torrent_open_;
    }
    QString fileText() const {
        return file_text_;
    }
    QString sourceName() const {
        return sourceName_;
    }
    QString sourceKind() const {
        return sourceKind_;
    }
    QString notice() const {
        return notice_;
    }
    QString coverage() const {
        return coverage_;
    }
    QString stats() const {
        return stats_;
    }
    QString text() const {
        return text_;
    }
    QString hex() const {
        return hex_;
    }
    QString raw() const {
        return raw_;
    }
    QString headers() const {
        return headers_;
    }
    QString mime() const {
        return mime_;
    }
    QString signatures() const {
        return signatures_;
    }
    QVariantList files() const {
        return files_;
    }
    QString directory() const {
        return directory_label_;
    }
    QVariantList messages() const {
        return messages_;
    }
    Q_INVOKABLE void openLocal(QUrl, bool emailOnly = false);
    Q_INVOKABLE void chooseDownloadFolder(QUrl);
    Q_INVOKABLE void addTorrent(QString);
    Q_INVOKABLE void restoreTorrent();
    Q_INVOKABLE void selectFile(int);
    Q_INVOKABLE void openDirectory(QString);
    Q_INVOKABLE void inspectRange(QString, QString);
    Q_INVOKABLE void indexArchive();
    Q_INVOKABLE void quickInspect();
    Q_INVOKABLE void checkIndex();
    Q_INVOKABLE void resetIndex();
    Q_INVOKABLE void search(QString, int page = 0);
    Q_INVOKABLE void inspectMessage(qint64);
    Q_INVOKABLE void verify(bool dns);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void pauseTorrent(bool);
    Q_INVOKABLE void uploadLimit(int);
    Q_INVOKABLE void storageLimit(int);
    Q_INVOKABLE void clearTorrent();
    Q_INVOKABLE void closeTorrent();
    Q_INVOKABLE void refresh();
  signals:
    void changed();
    void sourceOpened(QString kind);
    void torrentOpened();
    void messagesLoaded();
};
