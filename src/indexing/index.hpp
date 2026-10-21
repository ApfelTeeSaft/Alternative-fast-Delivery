#pragma once
#include "core/source.hpp"
#include "email/message.hpp"
#include <functional>
#include <string>
#include <vector>
struct sqlite3;
namespace afd {
struct IndexedMessage {
    std::int64_t id{};
    std::uint64_t offset{}, size{};
    std::string sender, recipient, subject, date, dkim, warning;
    bool attachments{};
};
struct Coverage {
    std::uint64_t scanned{}, total{}, messages{}, omitted{};
    bool complete{};
};
class Index {
    sqlite3 *db_{};
    std::string source_;

  public:
    Index(const std::filesystem::path &db, std::string identity, std::uint64_t size);
    ~Index();
    Index(const Index &) = delete;
    Index &operator=(const Index &) = delete;
    Coverage coverage() const;
    void build(Source &, bool mbox, afd::StopToken = {}, const std::function<void(Coverage)> & = {});
    std::vector<IndexedMessage> query(const std::string &text, int limit = 100, int page = 0) const;
    IndexedMessage get(std::int64_t id) const;
    void set_dkim(std::int64_t id, const std::string &status);
    void check_integrity() const;
    static std::filesystem::path quarantine(const std::filesystem::path &db);
};
} // namespace afd
