#pragma once
#include "core/source.hpp"
#include <chrono>
#include <functional>
#include <libtorrent/session.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <memory>
#include <mutex>
#include <set>
namespace afd {
inline constexpr int max_torrent_files = 250000;
inline constexpr int max_torrent_piece_bytes = 16 * 1024 * 1024;
inline constexpr int max_torrent_metadata_bytes = 16 * 1024 * 1024;
struct TorrentFile {
    int id;
    std::string path;
    std::uint64_t size;
};
struct TransferStats {
    int peers{}, down{}, up{}, pieces{}, total_pieces{};
    std::int64_t uploaded{}, downloaded{};
    bool metadata{}, paused{};
    std::string error;
};
class TorrentEngine {
    libtorrent::session session_;
    libtorrent::torrent_handle handle_;
    std::filesystem::path root_;
    std::uint64_t quota_ = 2ULL * 1024 * 1024 * 1024;
    std::set<int> reserved_;
    std::uint64_t reserved_bytes_{};
    bool validated_{};
    mutable std::mutex operations_;
    std::function<void(const TransferStats &)> progress_;
    void validate_metadata();

  public:
    explicit TorrentEngine(const std::filesystem::path &, bool local_only = false);
    ~TorrentEngine();
    void open(const std::string &uri_or_file);
    bool metadata_ready();
    std::vector<TorrentFile> files();
    Bytes read_range(int file, std::uint64_t offset, std::size_t length, afd::StopToken = {},
                     std::chrono::seconds timeout = std::chrono::seconds(45));
    TransferStats stats() const;
    void set_progress_callback(std::function<void(const TransferStats &)> callback) {
        progress_ = std::move(callback);
    }
    std::string identity() const;
    void pause();
    void resume();
    void save(const std::filesystem::path &);
    void restore(const std::filesystem::path &);
    void remove(bool delete_files);
    void set_upload_limit(int bytes_per_second);
    void set_quota(std::uint64_t bytes);
    std::pair<std::uint64_t, std::uint64_t> cache_usage() const;
    void connect_local_peer(unsigned short port);
    libtorrent::torrent_handle handle() const {
        return handle_;
    }
};
class TorrentSource final : public Source {
    TorrentEngine &engine_;
    int file_;
    std::uint64_t size_;

  public:
    TorrentSource(TorrentEngine &e, int file, std::uint64_t size) : engine_(e), file_(file), size_(size) {}
    std::uint64_t size() const override {
        return size_;
    }
    Bytes read(std::uint64_t offset, std::size_t length, afd::StopToken stop = {}) override {
        return engine_.read_range(file_, offset, length, stop);
    }
};
} // namespace afd
