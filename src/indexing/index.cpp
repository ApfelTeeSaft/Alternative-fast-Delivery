#include "indexing/index.hpp"
#include "dkim/dkim.hpp"
#include "email/mbox.hpp"
#include <algorithm>
#include <chrono>
#include <memory>
#include <sqlite3.h>
#include <stdexcept>
namespace afd {
namespace {
void exec(sqlite3 *db, const char *sql) {
    char *error = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &error) != SQLITE_OK) {
        std::string msg = error ? error : "SQLite failure";
        sqlite3_free(error);
        throw std::runtime_error(msg);
    }
}
class Statement {
    sqlite3 *db_;

  public:
    sqlite3_stmt *p{};
    Statement(sqlite3 *db, const char *sql) : db_(db) {
        if (sqlite3_prepare_v2(db, sql, -1, &p, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db));
    }
    ~Statement() {
        sqlite3_finalize(p);
    }
    void text(int i, const std::string &s) {
        if (sqlite3_bind_text(p, i, s.data(), static_cast<int>(s.size()), SQLITE_TRANSIENT) != SQLITE_OK)
            throw std::runtime_error("SQLite bind failed");
    }
    void num(int i, std::uint64_t n) {
        if (n > INT64_MAX || sqlite3_bind_int64(p, i, static_cast<sqlite3_int64>(n)) != SQLITE_OK)
            throw std::runtime_error("SQLite integer overflow");
    }
    bool step() {
        auto r = sqlite3_step(p);
        if (r != SQLITE_ROW && r != SQLITE_DONE)
            throw std::runtime_error(sqlite3_errmsg(db_));
        return r == SQLITE_ROW;
    }
    std::string str(int i) const {
        auto pstr = sqlite3_column_text(p, i);
        return pstr ? reinterpret_cast<const char *>(pstr) : "";
    }
    std::uint64_t num(int i) const {
        return static_cast<std::uint64_t>(sqlite3_column_int64(p, i));
    }
};
IndexedMessage row(const Statement &s) {
    return {static_cast<std::int64_t>(s.num(0)),
            s.num(1),
            s.num(2),
            s.str(3),
            s.str(4),
            s.str(5),
            s.str(6),
            s.str(7),
            s.str(8),
            s.num(9) != 0};
}
} // namespace
Index::Index(const std::filesystem::path &path, std::string identity, std::uint64_t size)
    : source_(std::move(identity)) {
    auto u8 = path.u8string();
    if (sqlite3_open_v2(reinterpret_cast<const char *>(u8.c_str()), &db_,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
        auto err = db_ ? sqlite3_errmsg(db_) : "Cannot open index";
        std::string copy(err);
        sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error(copy);
    }
    try {
        sqlite3_busy_timeout(db_, 5000);
        sqlite3_limit(db_, SQLITE_LIMIT_LENGTH, 32 * 1024 * 1024);
        exec(db_, "PRAGMA trusted_schema=OFF; PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;");
        exec(db_, "CREATE TABLE IF NOT EXISTS sources(identity TEXT PRIMARY KEY,total INTEGER NOT "
                  "NULL,checkpoint INTEGER NOT NULL DEFAULT 0,complete INTEGER NOT NULL DEFAULT 0,omitted "
                  "INTEGER NOT NULL DEFAULT 0);"
                  "CREATE TABLE IF NOT EXISTS messages(id INTEGER PRIMARY KEY,source TEXT NOT NULL,offset "
                  "INTEGER NOT NULL,size INTEGER NOT NULL,sender TEXT,recipient TEXT,subject TEXT,date "
                  "TEXT,dkim TEXT,warning TEXT,attachment INTEGER,timestamp INTEGER,UNIQUE(source,offset));"
                  "CREATE INDEX IF NOT EXISTS by_source ON messages(source,offset);"
                  "CREATE VIRTUAL TABLE IF NOT EXISTS search USING "
                  "fts5(sender,recipient,subject,body,tokenize='unicode61');");
        Statement add(db_, "INSERT OR IGNORE INTO sources(identity,total) VALUES(?,?)");
        add.text(1, source_);
        add.num(2, size);
        add.step();
        if (coverage().total != size)
            throw std::runtime_error("Source size changed; create a new index identity");
    } catch (...) {
        sqlite3_close(db_);
        db_ = nullptr;
        throw;
    }
}
Index::~Index() {
    sqlite3_close(db_);
}
void Index::check_integrity() const {
    Statement check(db_, "PRAGMA quick_check");
    if (!check.step() || check.str(0) != "ok")
        throw std::runtime_error("Corrupt index: preserve it and rebuild into a new database");
}
std::filesystem::path Index::quarantine(const std::filesystem::path &path) {
    auto backup = path;
    backup += ".backup-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    if (std::filesystem::exists(backup))
        throw std::runtime_error("Index backup path already exists");
    if (std::filesystem::exists(path))
        std::filesystem::rename(path, backup);
    for (const auto *suffix : {"-wal", "-shm"}) {
        auto sidecar = path;
        sidecar += suffix;
        auto destination = backup;
        destination += suffix;
        if (std::filesystem::exists(sidecar))
            std::filesystem::rename(sidecar, destination);
    }
    return backup;
}
Coverage Index::coverage() const {
    Statement s(db_, "SELECT checkpoint,total,complete,omitted,(SELECT count(*) FROM messages WHERE "
                     "source=?) FROM sources WHERE identity=?");
    s.text(1, source_);
    s.text(2, source_);
    if (!s.step())
        throw std::runtime_error("Index source missing");
    return {s.num(0), s.num(1), s.num(4), s.num(3), s.num(2) != 0};
}
void Index::build(Source &src, bool mbox, afd::StopToken stop,
                  const std::function<void(Coverage)> &progress) {
    auto state = coverage();
    if (state.total != src.size())
        throw std::runtime_error("Index source size mismatch");
    if (state.complete)
        return;
    if (src.size() == 0) {
        Statement empty(db_, "UPDATE sources SET complete=1 WHERE identity=?");
        empty.text(1, source_);
        empty.step();
        state.complete = true;
        if (progress)
            progress(state);
        return;
    }
    auto visit = [&](MessageRange range) {
        if (stop.stop_requested())
            return false;
        Message m;
        std::string dkim = "Not verified";
        bool omitted = false;
        if (range.end - range.begin > max_range) {
            m.subject = "[Oversized message]";
            m.warning = "Body not indexed: 16 MiB per-message limit";
            omitted = true;
        } else
            try {
                auto bytes = src.read(range.begin, static_cast<std::size_t>(range.end - range.begin), stop);
                m = parse_message(std::string_view(bytes.data(), bytes.size()));
                if (header_value(m.headers, "dkim-signature").empty())
                    dkim = "Missing signature";
            } catch (const std::exception &) {
                if (stop.stop_requested())
                    return false;
                m.subject = "[Unparsed message]";
                m.warning = "Malformed or unsupported MIME; inspect raw bytes";
                omitted = true;
            }
        exec(db_, "BEGIN IMMEDIATE");
        try {
            Statement insert(db_, "INSERT INTO "
                                  "messages(source,offset,size,sender,recipient,subject,date,dkim,warning,"
                                  "attachment,timestamp) VALUES(?,?,?,?,?,?,?,?,?,?,?)");
            insert.text(1, source_);
            insert.num(2, range.begin);
            insert.num(3, range.end - range.begin);
            insert.text(4, m.sender);
            insert.text(5, m.recipient);
            insert.text(6, m.subject);
            insert.text(7, m.date);
            insert.text(8, dkim);
            insert.text(9, m.warning);
            insert.num(10, m.attachments);
            sqlite3_bind_int64(insert.p, 11, m.timestamp);
            insert.step();
            auto id = sqlite3_last_insert_rowid(db_);
            Statement fts(db_, "INSERT INTO search(rowid,sender,recipient,subject,body) VALUES(?,?,?,?,?)");
            fts.num(1, static_cast<std::uint64_t>(id));
            fts.text(2, m.sender);
            fts.text(3, m.recipient);
            fts.text(4, m.subject);
            fts.text(5, m.text);
            fts.step();
            Statement update(db_,
                             "UPDATE sources SET checkpoint=?,complete=?,omitted=omitted+? WHERE identity=?");
            update.num(1, range.checkpoint);
            update.num(2, range.checkpoint == src.size());
            update.num(3, omitted || (!m.html.empty() && m.text.empty()));
            update.text(4, source_);
            update.step();
            exec(db_, "COMMIT");
        } catch (...) {
            exec(db_, "ROLLBACK");
            throw;
        }
        state.scanned = range.checkpoint;
        state.complete = range.checkpoint == src.size();
        ++state.messages;
        state.omitted += omitted || (!m.html.empty() && m.text.empty());
        if (progress)
            progress(state);
        return !stop.stop_requested();
    };
    if (mbox)
        scan_mbox(src, state.scanned, visit, stop);
    else
        visit({0, src.size(), src.size()});
}
std::vector<IndexedMessage> Index::query(const std::string &text, int limit, int page) const {
    if (limit < 1 || limit > 500 || page < 0 || page > 1000000)
        throw std::runtime_error("Invalid result page");
    auto sql = text.empty() ? "SELECT id,offset,size,sender,recipient,subject,date,dkim,warning,attachment "
                              "FROM messages WHERE source=? ORDER BY offset LIMIT ? OFFSET ?"
                            : "SELECT "
                              "m.id,m.offset,m.size,m.sender,m.recipient,m.subject,m.date,m.dkim,m.warning,m."
                              "attachment FROM messages m JOIN search s ON m.id=s.rowid WHERE m.source=? AND "
                              "search MATCH ? ORDER BY m.offset LIMIT ? OFFSET ?";
    Statement s(db_, sql);
    s.text(1, source_);
    int n = 2;
    if (!text.empty()) {
        std::string literal = "\"";
        for (char c : text)
            literal += c == '"' ? "\"\"" : std::string(1, c);
        literal += '"';
        s.text(n++, literal);
    }
    s.num(n++, static_cast<std::uint64_t>(limit));
    s.num(n, static_cast<std::uint64_t>(page) * static_cast<unsigned>(limit));
    std::vector<IndexedMessage> out;
    while (s.step())
        out.push_back(row(s));
    return out;
}
IndexedMessage Index::get(std::int64_t id) const {
    Statement s(db_, "SELECT id,offset,size,sender,recipient,subject,date,dkim,warning,attachment FROM "
                     "messages WHERE source=? AND id=?");
    s.text(1, source_);
    s.num(2, static_cast<std::uint64_t>(id));
    if (!s.step())
        throw std::runtime_error("Message not found");
    return row(s);
}
void Index::set_dkim(std::int64_t id, const std::string &status) {
    Statement s(db_, "UPDATE messages SET dkim=? WHERE source=? AND id=?");
    s.text(1, status);
    s.text(2, source_);
    s.num(3, static_cast<std::uint64_t>(id));
    s.step();
}
} // namespace afd
