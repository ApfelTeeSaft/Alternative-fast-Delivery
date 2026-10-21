#include "core/source.hpp"
#include "dkim/dkim.hpp"
#include "email/mbox.hpp"
#include "email/message.hpp"
#include "indexing/index.hpp"
#include <QCoreApplication>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <stdexcept>
using namespace afd;
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        if (!(x))                                                                                            \
            throw std::runtime_error(std::string("Check failed: ") + #x + " at " +                           \
                                     std::to_string(__LINE__));                                              \
    } while (false)
template <class F> void rejects(F f) {
    bool failed = false;
    try {
        f();
    } catch (const std::exception &) {
        failed = true;
    }
    CHECK(failed);
}
class Memory final : public Source {
  public:
    std::string bytes;
    explicit Memory(std::string s) : bytes(std::move(s)) {}
    std::uint64_t size() const override {
        return bytes.size();
    }
    Bytes read(std::uint64_t o, std::size_t n, afd::StopToken = {}) override {
        check_range(size(), o, n);
        return {bytes.begin() + static_cast<std::ptrdiff_t>(o),
                bytes.begin() + static_cast<std::ptrdiff_t>(o + n)};
    }
};
static const std::string eml =
    "From: Alice <alice@example.invalid>\r\nTo: Bob <bob@example.invalid>\r\nSubject: Synthetic archive "
    "test\r\nDate: Tue, 20 Oct 2026 12:00:00 +0000\r\nMIME-Version: 1.0\r\nContent-Type: text/plain; "
    "charset=utf-8\r\n\r\nsearchable copper lantern\r\n";
static const std::string envelope = "From alice@example.invalid Tue Oct 20 12:00:00 2026\n";
static std::string rfc_vector() {
    return "DKIM-Signature: v=1; a=ed25519-sha256; c=relaxed/relaxed;\r\n"
           " d=football.example.com; i=@football.example.com;\r\n"
           " q=dns/txt; s=brisbane; t=1528637909; h=from : to :\r\n"
           " subject : date : message-id : from : subject : date;\r\n"
           " bh=2jUSOH9NhtVGCQWNr9BrIAPreKQjO6Sn7XIkfJVOzv8=;\r\n"
           " b=/gCrinpcQOoIfuHNQIbq4pgh9kyIK3AQUdt9OdqQehSwhEIug4D11Bus\r\n"
           " Fa3bT3FY5OsU7ZbnKELq+eXdp1Q1Dw==\r\n"
           "DKIM-Signature: v=1; a=rsa-sha256; c=relaxed/relaxed;\r\n"
           " d=football.example.com; i=@football.example.com;\r\n"
           " q=dns/txt; s=test; t=1528637909; h=from : to : subject :\r\n"
           " date : message-id : from : subject : date;\r\n"
           " bh=2jUSOH9NhtVGCQWNr9BrIAPreKQjO6Sn7XIkfJVOzv8=;\r\n"
           " b=F45dVWDfMbQDGHJFlXUNB2HKfbCeLRyhDXgFpEL8GwpsRe0IeIixNTe3\r\n"
           " DhCVlUrSjV4BwcVcOF6+FF3Zo9Rpo1tFOeS9mPYQTnGdaSGsgeefOsk2Jz\r\n"
           " dA+L10TeYt9BgDfQNZtKdN1WO//KgIqXP7OdEFE4LjFYNcUxZQ4FADY+8=\r\n"
           "From: Joe SixPack <joe@football.example.com>\r\n"
           "To: Suzie Q <suzie@shopping.example.net>\r\n"
           "Subject: Is dinner ready?\r\n"
           "Date: Fri, 11 Jul 2003 21:00:37 -0700 (PDT)\r\n"
           "Message-ID: <20030712040037.46341.5F8J@football.example.com>\r\n"
           "\r\nHi.\r\n\r\nWe lost the game.  Are you hungry yet?\r\n\r\nJoe.\r\n\r\n";
}
static DnsKey keys(const std::string &name) {
    if (name.starts_with("brisbane."))
        return {DnsKey::State::Found, "v=DKIM1; k=ed25519; p=11qYAYKxCrfVS/7TyWQHOg7hcvPapiMlrwIaaPcHURo="};
    return {DnsKey::State::Found,
            "v=DKIM1; k=rsa; "
            "p=MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQDkHlOQoBTzWRiGs5V6NpP3idY6Wk08a5qhdR6wy5bdOKb2jLQiY/"
            "J16JYi0Qvx/byYzCNb3W91y3FutACDfzwQ/BC/e/"
            "8uBsCR+yz1Lxj+PL6lHvqMKrM3rG4hstT5QjvHO9PzoxZyVYLzBfO2EeC3Ip3G+2kryOTIKT+l/K4w3QIDAQAB"};
}
static std::string b64(const unsigned char *data, std::size_t size) {
    std::string out(4 * ((size + 2) / 3), '\0');
    EVP_EncodeBlock(reinterpret_cast<unsigned char *>(out.data()), data, static_cast<int>(size));
    return out;
}
static std::pair<std::string, std::string> sign_simple() {
    auto *key_context = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    CHECK(key_context);
    CHECK(EVP_PKEY_keygen_init(key_context) == 1);
    CHECK(EVP_PKEY_CTX_set_rsa_keygen_bits(key_context, 2048) == 1);
    EVP_PKEY *key = nullptr;
    CHECK(EVP_PKEY_keygen(key_context, &key) == 1);
    EVP_PKEY_CTX_free(key_context);
    unsigned char *der = nullptr;
    auto der_size = i2d_PUBKEY(key, &der);
    CHECK(der_size > 0);
    std::string dns = "v=DKIM1; k=rsa; p=" + b64(der, static_cast<std::size_t>(der_size));
    OPENSSL_free(der);
    std::string body = "body\r\n";
    unsigned char digest_bytes[32];
    unsigned int digest_size = 0;
    CHECK(EVP_Digest(body.data(), body.size(), digest_bytes, &digest_size, EVP_sha256(), nullptr) == 1);
    std::string header =
        "DKIM-Signature: v=1; a=rsa-sha256; d=example.invalid; s=test; h=from; bh=" + b64(digest_bytes, 32) +
        "; b=";
    std::string from = "From: Synthetic <a@example.invalid>\r\n";
    std::string signed_bytes = from + header;
    auto *ctx = EVP_MD_CTX_new();
    CHECK(ctx);
    CHECK(EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr, key) == 1);
    std::size_t len = 0;
    CHECK(EVP_DigestSign(ctx, nullptr, &len, reinterpret_cast<const unsigned char *>(signed_bytes.data()),
                         signed_bytes.size()) == 1);
    std::vector<unsigned char> signature(len);
    CHECK(EVP_DigestSign(ctx, signature.data(), &len,
                         reinterpret_cast<const unsigned char *>(signed_bytes.data()),
                         signed_bytes.size()) == 1);
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(key);
    return {header + b64(signature.data(), len) + "\r\n" + from + "\r\nbody\r\n\r\n", dns};
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    auto dir = std::filesystem::temp_directory_path() /
               ("afd-core-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    int passed = 0;
    auto test = [&](const char *name, const std::function<void()> &f) {
        try {
            f();
            ++passed;
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception &e) {
            std::cerr << "FAIL " << name << ": " << e.what() << '\n';
            throw;
        }
    };
    try {
        test("portable cancellation lifetime", [] {
            StopToken token;
            CHECK(!token.stop_requested());
            {
                StopSource source;
                token = source.get_token();
                CHECK(!token.stop_requested());
                CHECK(source.request_stop());
                CHECK(!source.request_stop());
            }
            CHECK(token.stop_requested());
        });
        test("empty archive completes without phantom messages", [&] {
            Memory empty("");
            Index index(dir / "empty.db", "empty", 0);
            index.build(empty, true);
            CHECK(index.coverage().complete);
            CHECK(index.coverage().messages == 0);
        });
        test("range bounds and traversal", [] {
            Memory m("abcdef");
            CHECK(m.read(2, 3) == Bytes({'c', 'd', 'e'}));
            CHECK(m.read(6, 0).empty());
            rejects([&] { m.read(5, 2); });
            rejects([&] { m.read(UINT64_MAX, 2); });
            rejects([&] { m.read(0, max_range + 1); });
            for (auto p :
                 {"../x", "a/../x", "C:\\x", "/tmp/x", "a\\..\\x", "a/NUL.txt", "a/file:stream", "a/b."})
                CHECK(!safe_relative_path(p));
            CHECK(safe_relative_path("archive/mail.mbox"));
        });
        test("MIME, text, header decoding", [] {
            auto m = parse_message(eml);
            CHECK(m.subject == "Synthetic archive test");
            CHECK(m.text.find("copper lantern") != m.text.npos);
            CHECK(m.timestamp > 0);
            CHECK(m.parts.size() == 1);
        });
        test("multipart attachment", [] {
            auto s =
                "From: a@example.invalid\r\nTo: b@example.invalid\r\nSubject: "
                "=?UTF-8?B?Y2Fmw6k=?=\r\nMIME-Version: 1.0\r\nContent-Type: multipart/mixed; "
                "boundary=synthetic\r\n\r\n--synthetic\r\nContent-Type: text/plain; "
                "charset=utf-8\r\nContent-Transfer-Encoding: "
                "quoted-printable\r\n\r\nhello=20world\r\n--synthetic\r\nContent-Type: "
                "application/octet-stream; name=test.txt\r\nContent-Disposition: attachment; "
                "filename=test.txt\r\nContent-Transfer-Encoding: base64\r\n\r\nYWJj\r\n--synthetic--\r\n";
            auto m = parse_message(s);
            CHECK(m.attachments);
            CHECK(m.parts.size() == 3);
            CHECK(m.text.find("hello world") != m.text.npos);
            CHECK(m.subject == "caf\xc3\xa9");
            CHECK(m.parts.back().size == 3);
        });
        test("parser limits", [] {
            rejects([] { parse_message("Broken\r\n\r\nbody"); });
            rejects([] { parse_message("Subject: " + std::string(1024 * 1024, 'x') + "\r\n\r\n"); });
            rejects([] { parse_message(std::string(max_range + 1, 'x')); });
        });
        test("MBOX chunk boundaries and escaped From", [] {
            Memory m(envelope + eml + std::string(65500, 'x') + "\n>From escaped body\n" + envelope + eml);
            std::vector<MessageRange> ranges;
            scan_mbox(m, 0, [&](auto r) {
                ranges.push_back(r);
                return true;
            });
            CHECK(ranges.size() == 2);
            CHECK(ranges[0].begin == envelope.size());
            CHECK(ranges.back().end == m.size());
            CHECK(ranges[0].checkpoint + envelope.size() == ranges[1].begin);
        });
        test("100 GiB virtual source stays bounded and cancellable", [] {
            class Huge : public Source {
              public:
                afd::StopSource stop;
                int reads{};
                std::uint64_t size() const override {
                    return 100ULL * 1024 * 1024 * 1024;
                }
                Bytes read(std::uint64_t o, std::size_t n, afd::StopToken) override {
                    CHECK(n <= 65536);
                    ++reads;
                    Bytes b(n, 'x');
                    if (o == 0) {
                        std::copy(envelope.begin(), envelope.end(), b.begin());
                    }
                    if (reads == 16)
                        stop.request_stop();
                    return b;
                }
            } h;
            scan_mbox(h, 0, [](auto) { return true; }, h.stop.get_token());
            CHECK(h.reads == 16);
        });
        test("index interruption, committed checkpoint, resume, search", [&] {
            Memory m(envelope + eml + envelope + eml);
            {
                Index index(dir / "index.db", "fixture", m.size());
                afd::StopSource stop;
                index.build(m, true, stop.get_token(), [&](auto) { stop.request_stop(); });
                auto c = index.coverage();
                CHECK(c.messages == 1);
                CHECK(!c.complete);
                CHECK(index.query("lantern").size() == 1);
            }
            {
                Index index(dir / "index.db", "fixture", m.size());
                index.build(m, true);
                CHECK(index.coverage().complete);
                CHECK(index.coverage().messages == 2);
                CHECK(index.query("lantern").size() == 2);
                CHECK(index.query("absent").empty());
                index.build(m, true);
                CHECK(index.coverage().messages == 2);
                rejects([&] { index.query("", 100, -1); });
            }
            rejects([&] { Index index(dir / "index.db", "fixture", m.size() + 1); });
        });
        test("corrupt SQLite fails closed", [&] {
            {
                std::ofstream f(dir / "corrupt.db", std::ios::binary);
                f << "not sqlite";
            }
            rejects([&] { Index index(dir / "corrupt.db", "fixture", 1); });
        });
        test("DKIM canonicalization", [] {
            CHECK(canonical_body("\r\n\r\n", false) == "\r\n");
            CHECK(canonical_body(" \t\r\n", true).empty());
            CHECK(canonical_body(" a \t b \t\r\n\r\n", true) == " a b\r\n");
            CHECK(canonical_header("SuBJeCt:  a\r\n \t b \t\r\n", true) == "subject:a b\r\n");
        });
        test("DKIM simple/simple independent EVP signature", [] {
            auto [mail, key] = sign_simple();
            auto resolver = [&](const std::string &) { return DnsKey{DnsKey::State::Found, key}; };
            auto results = verify_dkim(mail, resolver);
            CHECK(results.size() == 1);
            CHECK(results[0].status == DkimStatus::Valid);
            mail[mail.find("Synthetic")] = 'X';
            CHECK(verify_dkim(mail, resolver)[0].status == DkimStatus::Invalid);
        });
        test("corrupt index quarantine and rebuild", [&] {
            auto p = dir / "recover.db";
            {
                std::ofstream f(p);
                f << "corrupt";
            }
            auto backup = Index::quarantine(p);
            CHECK(std::filesystem::exists(backup));
            Memory m(eml);
            Index index(p, "recovered", m.size());
            index.build(m, false);
            index.check_integrity();
            CHECK(index.query("lantern").size() == 1);
        });
        test("RFC 8463 independent RSA and Ed25519 vectors", [] {
            auto r = verify_dkim(rfc_vector(), keys);
            CHECK(r.size() == 2);
            for (const auto &s : r) {
                if (s.status != DkimStatus::Valid)
                    std::cerr << s.detail << '\n';
                CHECK(s.status == DkimStatus::Valid);
            }
        });
        test("DKIM tampering and missing keys", [] {
            auto raw = rfc_vector();
            auto at = raw.find("We lost");
            raw[at] = 'X';
            for (const auto &r : verify_dkim(raw, keys))
                CHECK(r.status == DkimStatus::Invalid);
            auto missing = [](const std::string &) { return DnsKey{DnsKey::State::Missing, {}}; };
            for (const auto &r : verify_dkim(rfc_vector(), missing))
                CHECK(r.status == DkimStatus::MissingKey);
            CHECK(verify_dkim(eml, keys)[0].status == DkimStatus::MissingSignature);
        });
        test("DKIM unsupported algorithm, duplicate tags, changed line endings", [] {
            auto raw = rfc_vector();
            raw.replace(raw.find("ed25519-sha256"), 14, "obsolete-sha1");
            CHECK(verify_dkim(raw, keys)[0].status == DkimStatus::Unsupported);
            raw = rfc_vector();
            raw.insert(raw.find("a=ed25519"), "a=rsa-sha256; ");
            CHECK(verify_dkim(raw, keys)[0].status == DkimStatus::Invalid);
            raw = rfc_vector();
            raw.erase(std::remove(raw.begin(), raw.end(), '\r'), raw.end());
            CHECK(verify_dkim(raw, keys)[0].status == DkimStatus::Inconclusive);
        });
        test("file detector never executes SQL", [] {
            Memory m("CREATE TABLE example(id int);");
            auto b = m.read(0, static_cast<std::size_t>(m.size()));
            CHECK(detect_type(b) == "SQL text (never executed)");
            CHECK(!hex_preview(b).empty());
        });
        std::filesystem::remove_all(dir);
        std::cout << passed << " test groups passed\n";
        return 0;
    } catch (...) {
        std::filesystem::remove_all(dir);
        return 1;
    }
}
