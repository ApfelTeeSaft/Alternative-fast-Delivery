#include "core/source.hpp"
#include "email/mbox.hpp"
#include "email/message.hpp"
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <map>

// Opt-in, read-only corpus check. Reports counts and offsets, never message bodies.
class PrefixSource final : public afd::Source {
    afd::FileSource source_;
    std::uint64_t limit_;

  public:
    PrefixSource(const std::filesystem::path &path, std::uint64_t cap)
        : source_(path), limit_(std::min(source_.size(), cap)) {}
    std::uint64_t size() const override {
        return limit_;
    }
    afd::Bytes read(std::uint64_t offset, std::size_t length, afd::StopToken stop = {}) override {
        afd::check_range(limit_, offset, length);
        return source_.read(offset, length, stop);
    }
};

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    auto args = app.arguments();
    if (args.size() < 3) {
        std::cerr << "Usage: afd_mail_audit <email directory> <report.json> [prefix MiB=2]\n";
        return 2;
    }
    auto root = std::filesystem::u8path(args[1].toUtf8().toStdString());
    auto cap = static_cast<std::uint64_t>(args.size() > 3 ? args[3].toUInt() : 2) * 1024 * 1024;
    if (!cap || cap > 1024ULL * 1024 * 1024 || !std::filesystem::is_directory(root))
        return 2;
    std::map<std::string, int> errors;
    std::map<std::string, int> warnings, example_count;
    QJsonArray examples;
    std::uint64_t files = 0, messages = 0, parsed = 0, recovered = 0, empty = 0, lf = 0, zero = 0,
                  skipped = 0;
    for (const auto &entry : std::filesystem::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".mbox")
            continue;
        ++files;
        PrefixSource source(entry.path(), cap);
        try {
            afd::scan_mbox(source, 0, [&](afd::MessageRange range) {
                if (range.end == source.size() && entry.file_size() > cap)
                    return true; // A prefix's incomplete final message is not a parser failure.
                if (range.end - range.begin > afd::max_range) {
                    ++skipped;
                    return true;
                }
                auto bytes = source.read(range.begin, static_cast<std::size_t>(range.end - range.begin));
                std::string raw(bytes.begin(), bytes.end());
                ++messages;
                if (raw.find('\0') != raw.npos)
                    ++zero;
                bool bare_lf = false;
                for (std::size_t i = 0; i < raw.size(); ++i)
                    bare_lf |= raw[i] == '\n' && (i == 0 || raw[i - 1] != '\r');
                lf += bare_lf;
                std::string error;
                try {
                    auto mail = afd::parse_message(raw);
                    ++parsed;
                    empty += mail.text.empty() && mail.html.empty();
                    if (!mail.warning.empty())
                        ++warnings[mail.warning];
                } catch (const std::exception &e) {
                    error = e.what();
                    ++errors[error];
                }
                if (!error.empty()) {
                    if (example_count[error]++ < 2) {
                        auto name = entry.path().u8string();
                        examples.append(QJsonObject{
                            {"file",
                             QString::fromUtf8(reinterpret_cast<const char *>(name.data()), name.size())},
                            {"offset", static_cast<qint64>(range.begin)},
                            {"length", static_cast<qint64>(range.end - range.begin)},
                            {"error", QString::fromStdString(error)}});
                    }
                    if (bare_lf) {
                        std::string normalized;
                        normalized.reserve(raw.size() + raw.size() / 32);
                        for (std::size_t i = 0; i < raw.size(); ++i) {
                            if (raw[i] == '\n' && (i == 0 || raw[i - 1] != '\r'))
                                normalized += '\r';
                            normalized += raw[i];
                        }
                        try {
                            afd::parse_message(normalized);
                            ++recovered;
                        } catch (const std::exception &) {
                        }
                    }
                }
                return true;
            });
        } catch (const std::exception &e) {
            ++errors[std::string("Archive: ") + e.what()];
        }
    }
    QJsonObject diagnostics;
    QJsonObject notes;
    for (const auto &[error, count] : errors)
        diagnostics[QString::fromStdString(error)] = count;
    for (const auto &[warning, count] : warnings)
        notes[QString::fromStdString(warning)] = count;
    QJsonObject report{{"files", static_cast<qint64>(files)},
                       {"messages", static_cast<qint64>(messages)},
                       {"parsed", static_cast<qint64>(parsed)},
                       {"bare_lf", static_cast<qint64>(lf)},
                       {"recovered_by_crlf", static_cast<qint64>(recovered)},
                       {"empty_bodies", static_cast<qint64>(empty)},
                       {"messages_with_nul", static_cast<qint64>(zero)},
                       {"oversized_skipped", static_cast<qint64>(skipped)},
                       {"prefix_bytes_per_file", static_cast<qint64>(cap)},
                       {"errors", diagnostics},
                       {"warnings", notes}};
    std::cout << QJsonDocument(report).toJson().toStdString();
    report["examples"] = examples;
    QFile output(args[2]);
    if (!output.open(QIODevice::WriteOnly))
        return 2;
    output.write(QJsonDocument(report).toJson());
    return 0;
}
