#include "torrent/engine.hpp"
#include <QCoreApplication>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/bencode.hpp>
#include <libtorrent/create_torrent.hpp>
#include <libtorrent/error_code.hpp>
#include <libtorrent/load_torrent.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/socket.hpp>
#include <libtorrent/torrent_flags.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/torrent_status.hpp>
#include <stdexcept>
#include <thread>
namespace lt = libtorrent;
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        if (!(x))                                                                                            \
            throw std::runtime_error(std::string("Failed: ") + #x);                                          \
    } while (false)
static unsigned short block_udp_port(boost::asio::ip::udp::socket &udp) {
    // Reserve UDP while checking that TCP is available on the same port. This
    // reproduces hosts where a UDP bind is denied even though TCP can listen.
    auto loopback = boost::asio::ip::address_v4::loopback();
    for (int attempt = 0; attempt < 32; ++attempt) {
        if (udp.is_open())
            udp.close();
        udp.open(boost::asio::ip::udp::v4());
#ifdef _WIN32
        int exclusive = 1;
        CHECK(::setsockopt(udp.native_handle(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                           reinterpret_cast<const char *>(&exclusive), sizeof(exclusive)) == 0);
#endif
        udp.bind({loopback, 0});
        auto port = udp.local_endpoint().port();
        boost::asio::ip::tcp::acceptor tcp(udp.get_executor());
        tcp.open(boost::asio::ip::tcp::v4());
        lt::error_code ec;
        tcp.bind({loopback, port}, ec);
        if (!ec)
            return port;
        if (ec != boost::asio::error::address_in_use && ec != boost::asio::error::access_denied)
            throw boost::system::system_error(ec);
    }
    throw std::runtime_error("Could not reserve a UDP-only port for the loopback test");
}
static void many_file_metadata(const std::filesystem::path &path, int count, int piece_bytes,
                               std::size_t padding = 0) {
    std::ofstream out(path, std::ios::binary);
    out << 'd';
    if (padding)
        out << "7:comment" << padding << ':' << std::string(padding, 'x');
    out << "4:infod5:filesl";
    for (int i = 0; i < count; ++i) {
        auto name = "f" + std::to_string(i);
        out << "d6:lengthi1e4:pathl" << name.size() << ':' << name << "ee";
    }
    auto hashes = static_cast<std::size_t>((count + piece_bytes - 1) / piece_bytes) * 20;
    out << "e4:name9:manyfiles12:piece lengthi" << piece_bytes << "e6:pieces" << hashes << ':'
        << std::string(hashes, '\0') << "ee";
    CHECK(out.good());
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    auto root =
        std::filesystem::temp_directory_path() /
        ("afd-torrent-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::string stage = "fixture creation";
    try {
        if (argc == 3 && std::string(argv[1]) == "--inspect-metadata") {
            // Supply tracker/web-seed-free metadata for an offline compatibility check.
            afd::TorrentEngine metadata(root / "metadata-only", true);
            metadata.open(argv[2]);
            auto ti = metadata.handle().torrent_file();
            auto files = metadata.files();
            CHECK(metadata.stats().downloaded == 0);
            auto priorities = metadata.handle().get_piece_priorities();
            CHECK(std::all_of(priorities.begin(), priorities.end(),
                              [](auto priority) { return priority == lt::dont_download; }));
            std::cout << "PASS metadata-only: " << files.size() << " files, " << ti->piece_length()
                      << " bytes per piece, " << ti->total_size()
                      << " total bytes, zero payload downloaded\n";
            return 0;
        }
        std::filesystem::create_directories(root / "seed" / "fixture");
        std::string a(16375, 'a'), b(65536, 'b'), c(1024 * 1024, 'c');
        for (std::size_t i = 0; i < b.size(); ++i)
            b[i] = static_cast<char>('A' + i % 26);
        auto write = [&](const char *name, const std::string &data) {
            std::ofstream f(root / "seed" / "fixture" / name, std::ios::binary);
            f.write(data.data(), static_cast<std::streamsize>(data.size()));
        };
        write("a.txt", a);
        write("b.txt", b);
        write("c.txt", c);
        std::vector<lt::create_file_entry> fs{{"fixture/a.txt", static_cast<std::int64_t>(a.size())},
                                              {"fixture/b.txt", static_cast<std::int64_t>(b.size())},
                                              {"fixture/c.txt", static_cast<std::int64_t>(c.size())}};
        lt::create_torrent creator(fs, 16384, lt::create_torrent::v1_only);
        lt::set_piece_hashes(creator, (root / "seed").string());
        std::vector<char> metainfo;
        lt::bencode(std::back_inserter(metainfo), creator.generate());
        {
            std::ofstream f(root / "fixture.torrent", std::ios::binary);
            f.write(metainfo.data(), static_cast<std::streamsize>(metainfo.size()));
        }
        auto loaded = lt::load_torrent_buffer(metainfo);
        auto ti = loaded.ti;
        stage = "UDP bind restriction fixture";
        boost::asio::io_context socket_context;
        boost::asio::ip::udp::socket blocked_udp(socket_context);
        auto listen_port = block_udp_port(blocked_udp);
        lt::settings_pack settings;
        settings.set_str(lt::settings_pack::listen_interfaces, "127.0.0.1:" + std::to_string(listen_port));
        settings.set_int(lt::settings_pack::max_retry_port_bind, 0);
        settings.set_bool(lt::settings_pack::listen_system_port_fallback, false);
        settings.set_bool(lt::settings_pack::enable_dht, false);
        settings.set_bool(lt::settings_pack::enable_lsd, false);
        settings.set_bool(lt::settings_pack::enable_upnp, false);
        settings.set_bool(lt::settings_pack::enable_natpmp, false);
        settings.set_bool(lt::settings_pack::allow_multiple_connections_per_ip, true);
        settings.set_bool(lt::settings_pack::enable_incoming_utp, false);
        settings.set_bool(lt::settings_pack::enable_outgoing_utp, false);
        settings.set_int(lt::settings_pack::alert_mask,
                         lt::alert_category::error | lt::alert_category::status);
        {
            lt::session seed(settings);
            lt::add_torrent_params p;
            p.ti = ti;
            p.save_path = (root / "seed").string();
            p.flags &= ~(lt::torrent_flags::paused | lt::torrent_flags::auto_managed);
            auto seeder = seed.add_torrent(p);
            stage = "seeder verification";
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
            while (!seeder.status().is_seeding) {
                CHECK(std::chrono::steady_clock::now() < deadline);
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }
            stage = "loopback TCP listen";
            unsigned short peer_port = 0;
            bool udp_bind_failed = false;
            int listen_retries = 0;
            while (!peer_port) {
                CHECK(std::chrono::steady_clock::now() < deadline);
                seed.wait_for_alert(std::chrono::milliseconds(50));
                std::vector<lt::alert *> alerts;
                seed.pop_alerts(&alerts);
                for (auto *alert : alerts) {
                    if (auto *ready = lt::alert_cast<lt::listen_succeeded_alert>(alert);
                        ready && ready->socket_type == lt::socket_type_t::tcp)
                        peer_port = static_cast<unsigned short>(ready->port);
                    if (auto *failure = lt::alert_cast<lt::listen_failed_alert>(alert)) {
                        // A failed UDP bind discards the TCP listener too.
                        // Release the deliberate conflict and reopen the
                        // prechecked port. Other errors must still fail.
                        if (failure->socket_type == lt::socket_type_t::utp &&
                            failure->op == lt::operation_t::sock_bind &&
                            (failure->error == boost::asio::error::address_in_use ||
                             failure->error == boost::asio::error::access_denied) &&
                            listen_retries++ < 8) {
                            udp_bind_failed = true;
                            if (blocked_udp.is_open())
                                blocked_udp.close();
                            seed.reopen_network_sockets();
                        } else {
                            throw std::runtime_error(failure->message());
                        }
                    }
                }
            }
            CHECK(udp_bind_failed);
            afd::TorrentEngine engine(root / "leech", true);
            engine.open((root / "fixture.torrent").string());
            CHECK(engine.files().size() == 3);
            engine.connect_local_peer(peer_port);
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            CHECK(engine.stats().downloaded == 0);
            stage = "v1 range";
            auto bytes = engine.read_range(1, 0, 40);
            CHECK(std::string(bytes.begin(), bytes.end()) == b.substr(0, 40));
            auto s = engine.stats();
            CHECK(s.pieces == 2);
            CHECK(s.downloaded < 65536);
            CHECK(!engine.handle().have_piece(lt::piece_index_t(3)));
            engine.pause();
            engine.save(root / "saved.resume");
            engine.remove(false);
            afd::TorrentEngine restored(root / "leech", true);
            restored.restore(root / "saved.resume");
            restored.connect_local_peer(peer_port);
            stage = "restored range";
            auto again = restored.read_range(1, 0, 40);
            CHECK(again == bytes);
            afd::StopSource cancelled;
            cancelled.request_stop();
            bool stopped = false;
            try {
                restored.read_range(2, 0, 32, cancelled.get_token());
            } catch (...) {
                stopped = true;
            }
            CHECK(stopped);
            afd::TorrentEngine absent(root / "absent", true);
            absent.open((root / "fixture.torrent").string());
            bool timeout = false;
            try {
                absent.read_range(2, 0, 32, {}, std::chrono::seconds(1));
            } catch (...) {
                timeout = true;
            }
            CHECK(timeout);
            CHECK(absent.stats().downloaded == 0);
            absent.handle().add_piece(lt::piece_index_t(0), std::vector<char>(16384, 'X'));
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            CHECK(!absent.handle().have_piece(lt::piece_index_t(0)));
            bool malformed = false;
            try {
                lt::parse_magnet_uri("magnet:?xt=invalid");
            } catch (...) {
                malformed = true;
            }
            CHECK(malformed);
            auto magnet = lt::make_magnet_uri(loaded);
            auto parsed = lt::parse_magnet_uri(magnet);
            CHECK(parsed.info_hashes == ti->info_hashes());
            for (auto flags : {lt::create_flags_t{}, lt::create_torrent::v2_only}) {
                lt::create_torrent modern(fs, 16384, flags);
                lt::set_piece_hashes(modern, (root / "seed").string());
                std::vector<char> data;
                lt::bencode(std::back_inserter(data), modern.generate());
                auto params = lt::load_torrent_buffer(data);
                CHECK(params.ti->info_hashes().has_v2());
                CHECK(params.ti->info_hashes().has_v1() == (flags == lt::create_flags_t{}));
                auto metadata = root / (flags == lt::create_flags_t{} ? "hybrid.torrent" : "v2.torrent");
                {
                    std::ofstream f(metadata, std::ios::binary);
                    f.write(data.data(), static_cast<std::streamsize>(data.size()));
                }
                params.save_path = (root / "seed").string();
                params.flags &= ~(lt::torrent_flags::paused | lt::torrent_flags::auto_managed);
                auto modern_seed = seed.add_torrent(params);
                auto ready = std::chrono::steady_clock::now() + std::chrono::seconds(15);
                while (!modern_seed.status().is_seeding) {
                    CHECK(std::chrono::steady_clock::now() < ready);
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                afd::TorrentEngine modern_client(root / (flags == lt::create_flags_t{} ? "hybrid" : "v2"),
                                                 true);
                modern_client.open(metadata.string());
                modern_client.connect_local_peer(peer_port);
                auto modern_files = modern_client.files();
                auto found = std::find_if(modern_files.begin(), modern_files.end(),
                                          [](const auto &f) { return f.path.ends_with("b.txt"); });
                CHECK(found != modern_files.end());
                stage = flags == lt::create_flags_t{} ? "hybrid range" : "v2 range";
                auto piece = modern_client.read_range(found->id, 10, 40);
                CHECK(std::string(piece.begin(), piece.end()) == b.substr(10, 40));
                seed.remove_torrent(modern_seed);
            }
            {
                std::ofstream f(root / "bad.torrent");
                f << "not bencoded metadata";
            }
            afd::TorrentEngine bad(root / "bad", true);
            bool rejected = false;
            try {
                bad.open((root / "bad.torrent").string());
            } catch (...) {
                rejected = true;
            }
            CHECK(rejected);
            stage = "large file-list metadata";
            auto many = root / "many.torrent";
            // A valid file list above the former 100,000-file cap, also above
            // libtorrent's default 10 MB local metadata-file buffer limit.
            many_file_metadata(many, 100001, 16384, 10 * 1024 * 1024);
            CHECK(std::filesystem::file_size(many) > 10000000);
            CHECK(std::filesystem::file_size(many) <= afd::max_torrent_metadata_bytes);
            {
                afd::TorrentEngine large(root / "many", true);
                large.open(many.string());
                CHECK(large.files().size() == 100001);
                CHECK(large.stats().downloaded == 0);
            }
            auto rejected_message = [&](const std::filesystem::path &file, const char *storage) {
                afd::TorrentEngine limited(root / storage, true);
                try {
                    limited.open(file.string());
                } catch (const std::exception &e) {
                    return std::string(e.what());
                }
                return std::string{};
            };
            stage = "file count diagnostic";
            many_file_metadata(many, afd::max_torrent_files + 1, 16384);
            auto file_error = rejected_message(many, "too-many");
            CHECK(file_error.find("250001 files") != std::string::npos);
            CHECK(file_error.find("250000") != std::string::npos);
            CHECK(file_error.find("piece") == std::string::npos);
            stage = "piece size diagnostic";
            many_file_metadata(many, 1, 32 * 1024 * 1024);
            auto piece_error = rejected_message(many, "big-piece");
            CHECK(piece_error.find("33554432") != std::string::npos);
            CHECK(piece_error.find("16 MiB") != std::string::npos);
            std::cout << "PASS loopback: recovery from blocked UDP bind; exact range, cross-file pieces, "
                         "no full download, verified bytes, "
                         "pause/resume, cancellation, no peers, magnet parsing; 100001-file metadata, "
                         "local metadata above 10 MB, separate file/piece limits\n";
        }
        std::filesystem::remove_all(root);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << stage << ": " << e.what() << '\n';
        std::filesystem::remove_all(root);
        return 1;
    }
}
