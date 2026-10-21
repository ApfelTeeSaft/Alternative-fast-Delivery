#include "torrent/engine.hpp"
#include <algorithm>
#include <fstream>
#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/download_priority.hpp>
#include <libtorrent/load_torrent.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/read_resume_data.hpp>
#include <libtorrent/session_params.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/torrent_flags.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/torrent_status.hpp>
#include <libtorrent/write_resume_data.hpp>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif
namespace afd {
namespace lt = libtorrent;
static lt::session_params settings(bool local) {
    lt::settings_pack p;
    p.set_str(lt::settings_pack::listen_interfaces, local ? "127.0.0.1:0" : "0.0.0.0:0,[::]:0");
    p.set_bool(lt::settings_pack::enable_dht, !local);
    p.set_bool(lt::settings_pack::enable_lsd, false);
    p.set_bool(lt::settings_pack::enable_upnp, false);
    p.set_bool(lt::settings_pack::enable_natpmp, false);
    p.set_bool(lt::settings_pack::allow_multiple_connections_per_ip, local);
    p.set_bool(lt::settings_pack::close_redundant_connections, false);
    p.set_int(lt::settings_pack::min_reconnect_time, 5);
    p.set_bool(lt::settings_pack::enable_incoming_utp, !local);
    p.set_bool(lt::settings_pack::enable_outgoing_utp, !local);
    p.set_int(lt::settings_pack::connections_limit, 100);
    p.set_int(lt::settings_pack::max_metadata_size, max_torrent_metadata_bytes);
    p.set_int(lt::settings_pack::active_downloads, 4);
    p.set_int(lt::settings_pack::upload_rate_limit, 64 * 1024);
    p.set_int(lt::settings_pack::alert_mask,
              lt::alert_category::error | lt::alert_category::storage | lt::alert_category::status);
    return lt::session_params(p);
}
static std::string path_string(const std::filesystem::path &p) {
    auto s = p.u8string();
    return {reinterpret_cast<const char *>(s.data()), s.size()};
}
static bool linked_path(const std::filesystem::path &path) {
#ifdef _WIN32
    auto attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT);
#else
    return std::filesystem::is_symlink(path);
#endif
}
TorrentEngine::TorrentEngine(const std::filesystem::path &root, bool local)
    : session_(settings(local)), root_(std::filesystem::absolute(root)) {
    std::filesystem::create_directories(root_);
    if (linked_path(root_))
        throw std::runtime_error("Torrent root must not be a symlink or reparse point");
}
TorrentEngine::~TorrentEngine() = default;
void TorrentEngine::open(const std::string &input) {
    if (handle_.is_valid())
        throw std::runtime_error("Close the current torrent first");
    lt::add_torrent_params p;
    if (input.starts_with("magnet:"))
        p = lt::parse_magnet_uri(input);
    else {
        auto path = std::filesystem::u8path(input);
        if (std::filesystem::file_size(path) > max_torrent_metadata_bytes)
            throw std::runtime_error("Torrent metadata exceeds 16 MiB limit");
        lt::load_torrent_limits limits;
        limits.max_buffer_size = max_torrent_metadata_bytes;
        p = lt::load_torrent_file(input, limits);
    }
    p.save_path = path_string(root_);
    p.flags &= ~(lt::torrent_flags::auto_managed | lt::torrent_flags::paused);
    p.flags |= lt::torrent_flags::upload_mode | lt::torrent_flags::default_dont_download;
    if (p.ti)
        p.file_priorities.assign(static_cast<std::size_t>(p.ti->num_files()), lt::dont_download);
    else
        p.file_priorities = {lt::dont_download};
    handle_ = session_.add_torrent(p);
    if (handle_.torrent_file())
        validate_metadata();
}
void TorrentEngine::validate_metadata() {
    auto ti = handle_.torrent_file();
    if (!ti)
        throw std::runtime_error("Metadata not available");
    const auto &fs = ti->layout();
    if (fs.num_files() > max_torrent_files) {
        pause();
        throw std::runtime_error("Torrent contains " + std::to_string(fs.num_files()) +
                                 " files; this build supports at most " + std::to_string(max_torrent_files));
    }
    if (ti->piece_length() > max_torrent_piece_bytes) {
        pause();
        throw std::runtime_error("Torrent piece size is " + std::to_string(ti->piece_length()) +
                                 " bytes; the inspection limit is " +
                                 std::to_string(max_torrent_piece_bytes) + " bytes (16 MiB)");
    }
    if (ti->piece_length() < 16384) {
        pause();
        throw std::runtime_error("Torrent piece size is below the 16 KiB minimum");
    }
    for (auto f : fs.file_range()) {
        if (fs.pad_file_at(f))
            continue;
        if (!safe_relative_path(fs.file_path(f)) || (fs.file_flags(f) & lt::file_storage::flag_symlink)) {
            pause();
            throw std::runtime_error("Unsafe torrent path or symlink rejected");
        }
        auto target = root_ / std::filesystem::u8path(fs.file_path(f));
        for (auto parent = target; parent != root_ && !parent.empty(); parent = parent.parent_path())
            if (linked_path(parent)) {
                pause();
                throw std::runtime_error("Existing symlink or reparse point in torrent storage path");
            }
    }
    handle_.prioritize_pieces(
        std::vector<lt::download_priority_t>(static_cast<std::size_t>(ti->num_pieces()), lt::dont_download));
    validated_ = true;
}
bool TorrentEngine::metadata_ready() {
    if (!handle_.is_valid() || !handle_.torrent_file())
        return false;
    // upload_mode prevents payload requests while magnet metadata is being validated.
    if (!validated_)
        validate_metadata();
    return true;
}
std::vector<TorrentFile> TorrentEngine::files() {
    if (!metadata_ready())
        return {};
    auto ti = handle_.torrent_file();
    std::vector<TorrentFile> out;
    for (auto f : ti->layout().file_range())
        if (!ti->layout().pad_file_at(f))
            out.push_back({static_cast<int>(f), ti->layout().file_path(f),
                           static_cast<std::uint64_t>(ti->layout().file_size(f))});
    return out;
}
Bytes TorrentEngine::read_range(int file, std::uint64_t offset, std::size_t length, afd::StopToken stop,
                                std::chrono::seconds timeout) {
    std::lock_guard lock(operations_);
    if (!metadata_ready())
        throw std::runtime_error("Waiting for metadata; availability is unknown");
    auto ti = handle_.torrent_file();
    if (file < 0 || file >= ti->num_files())
        throw std::out_of_range("Invalid torrent file");
    auto fi = lt::file_index_t(file);
    check_range(static_cast<std::uint64_t>(ti->layout().file_size(fi)), offset, length);
    if (!length)
        return {};
    if (stop.stop_requested())
        throw std::runtime_error("Cancelled");
    auto absolute = static_cast<std::uint64_t>(ti->layout().file_offset(fi)) + offset;
    auto piece_len = static_cast<std::uint64_t>(ti->piece_length());
    auto first = static_cast<int>(absolute / piece_len),
         last = static_cast<int>((absolute + length - 1) / piece_len);
    std::uint64_t additional = 0;
    for (int p = first; p <= last; ++p)
        if (!reserved_.contains(p))
            additional += static_cast<std::uint64_t>(ti->piece_size(lt::piece_index_t(p)));
    if (additional > quota_ - std::min(quota_, reserved_bytes_))
        throw std::runtime_error("Torrent storage quota reached; clean up session or raise quota");
    for (int p = first; p <= last; ++p)
        reserved_.insert(p);
    reserved_bytes_ += additional;
    struct Reset {
        lt::torrent_handle h;
        int first, last;
        ~Reset() {
            if (h.is_valid())
                for (int p = first; p <= last; ++p) {
                    h.reset_piece_deadline(lt::piece_index_t(p));
                    h.piece_priority(lt::piece_index_t(p), lt::dont_download);
                }
        }
    } reset{handle_, first, last};
    handle_.unset_flags(lt::torrent_flags::upload_mode);
    for (int p = first; p <= last; ++p) {
        handle_.piece_priority(lt::piece_index_t(p), lt::top_priority);
        handle_.set_piece_deadline(lt::piece_index_t(p), (p - first) * 100);
    }
    auto deadline = std::chrono::steady_clock::now() + timeout;
    auto last_progress = std::chrono::steady_clock::now();
    Bytes out;
    out.reserve(length);
    for (int p = first; p <= last; ++p) {
        auto pi = lt::piece_index_t(p);
        while (!handle_.have_piece(pi)) {
            if (progress_ &&
                std::chrono::steady_clock::now() - last_progress >= std::chrono::milliseconds(500)) {
                last_progress = std::chrono::steady_clock::now();
                progress_(stats());
            }
            if (stop.stop_requested())
                throw std::runtime_error("Cancelled");
            if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error(
                    "Requested pieces unavailable: timed out waiting for compatible peers");
            if (handle_.status().errc)
                throw std::runtime_error("Torrent disk or transfer error");
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        handle_.read_piece(pi);
        bool received = false;
        while (!received) {
            if (stop.stop_requested())
                throw std::runtime_error("Cancelled");
            if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error("Verified piece read timed out");
            session_.wait_for_alert(std::chrono::milliseconds(50));
            std::vector<lt::alert *> alerts;
            session_.pop_alerts(&alerts);
            for (auto *a : alerts)
                if (auto *r = lt::alert_cast<lt::read_piece_alert>(a);
                    r && r->handle == handle_ && r->piece == pi) {
                    if (r->error || !r->buffer || r->size != ti->piece_size(pi))
                        throw std::runtime_error("Verified piece read failed");
                    auto begin = std::max(absolute, static_cast<std::uint64_t>(p) * piece_len) -
                                 static_cast<std::uint64_t>(p) * piece_len;
                    auto end = std::min(absolute + length, static_cast<std::uint64_t>(p) * piece_len +
                                                               static_cast<std::uint64_t>(r->size)) -
                               static_cast<std::uint64_t>(p) * piece_len;
                    out.insert(out.end(), r->buffer.get() + begin, r->buffer.get() + end);
                    received = true;
                }
        }
    }
    if (progress_)
        progress_(stats());
    return out;
}
TransferStats TorrentEngine::stats() const {
    if (!handle_.is_valid())
        return {};
    auto s = handle_.status();
    auto ti = handle_.torrent_file();
    return {s.num_peers,
            s.download_payload_rate,
            s.upload_payload_rate,
            s.num_pieces,
            ti ? ti->num_pieces() : 0,
            s.total_payload_upload,
            s.total_payload_download,
            bool(ti),
            bool(handle_.flags() & lt::torrent_flags::paused),
            s.errc ? "Transfer error" : ""};
}
std::string TorrentEngine::identity() const {
    if (!handle_.is_valid())
        throw std::runtime_error("No torrent open");
    auto hashes = handle_.info_hashes();
    return sha256(hashes.v1.to_string() + hashes.v2.to_string());
}
void TorrentEngine::pause() {
    if (handle_.is_valid())
        handle_.pause();
}
void TorrentEngine::resume() {
    if (handle_.is_valid())
        handle_.resume();
}
void TorrentEngine::set_upload_limit(int bps) {
    if (bps < 1024)
        throw std::runtime_error("Upload limit must be >= 1 KiB/s; pause to stop network activity");
    if (handle_.is_valid())
        handle_.set_upload_limit(bps);
}
void TorrentEngine::set_quota(std::uint64_t bytes) {
    std::lock_guard lock(operations_);
    if (bytes < reserved_bytes_)
        throw std::runtime_error("Quota below reserved storage");
    quota_ = bytes;
}
std::pair<std::uint64_t, std::uint64_t> TorrentEngine::cache_usage() const {
    std::lock_guard lock(operations_);
    return {reserved_bytes_, quota_};
}
void TorrentEngine::connect_local_peer(unsigned short port) {
    handle_.connect_peer(lt::tcp::endpoint(lt::address_v4::loopback(), port));
}
void TorrentEngine::save(const std::filesystem::path &path) {
    std::lock_guard lock(operations_);
    if (!handle_.is_valid())
        return;
    handle_.save_resume_data(lt::torrent_handle::save_info_dict | lt::torrent_handle::flush_disk_cache);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        session_.wait_for_alert(std::chrono::milliseconds(100));
        std::vector<lt::alert *> alerts;
        session_.pop_alerts(&alerts);
        for (auto *a : alerts) {
            if (auto *r = lt::alert_cast<lt::save_resume_data_alert>(a); r && r->handle == handle_) {
                auto bytes = lt::write_resume_data_buf(r->params);
                auto tmp = path;
                tmp += ".tmp";
                {
                    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
                    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                    if (!f)
                        throw std::runtime_error("Cannot persist torrent state");
                }
                if (std::filesystem::exists(path))
                    std::filesystem::remove(path);
                std::filesystem::rename(tmp, path);
                return;
            }
            if (lt::alert_cast<lt::save_resume_data_failed_alert>(a))
                throw std::runtime_error("Resume snapshot failed");
        }
    }
    throw std::runtime_error("Resume snapshot timed out");
}
void TorrentEngine::restore(const std::filesystem::path &path) {
    if (handle_.is_valid())
        throw std::runtime_error("Close the current torrent first");
    FileSource f(path);
    if (f.size() > max_range)
        throw std::runtime_error("Resume metadata too large");
    auto bytes = f.read(0, static_cast<std::size_t>(f.size()));
    auto p = lt::read_resume_data(bytes);
    p.save_path = path_string(root_);
    p.flags &= ~(lt::torrent_flags::auto_managed | lt::torrent_flags::paused);
    p.flags |= lt::torrent_flags::upload_mode | lt::torrent_flags::default_dont_download;
    p.file_priorities.assign(p.ti ? static_cast<std::size_t>(p.ti->num_files()) : 1, lt::dont_download);
    p.piece_priorities.clear();
    handle_ = session_.add_torrent(p);
    if (p.ti) {
        validate_metadata();
        // A restored session may already occupy the entire quota; account conservatively.
        for (int i = 0; i < p.ti->num_pieces(); ++i)
            if (i < p.have_pieces.size() && p.have_pieces[lt::piece_index_t(i)]) {
                reserved_.insert(i);
                reserved_bytes_ += static_cast<std::uint64_t>(p.ti->piece_size(lt::piece_index_t(i)));
            }
    }
}
void TorrentEngine::remove(bool delete_files) {
    if (handle_.is_valid())
        session_.remove_torrent(handle_, delete_files ? lt::session::delete_files : lt::remove_flags_t{});
    handle_ = {};
    reserved_.clear();
    reserved_bytes_ = 0;
    validated_ = false;
}
} // namespace afd
