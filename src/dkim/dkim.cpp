#include "dkim/dkim.hpp"
#include "core/source.hpp"
#include "email/message.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <map>
#include <memory>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <set>
#include <stdexcept>
namespace afd {
std::string status_name(DkimStatus s) {
    switch (s) {
    case DkimStatus::Valid:
        return "Valid";
    case DkimStatus::Invalid:
        return "Invalid";
    case DkimStatus::MissingSignature:
        return "Missing signature";
    case DkimStatus::MissingKey:
        return "Missing public key";
    case DkimStatus::Unsupported:
        return "Unsupported verification method";
    default:
        return "Inconclusive";
    }
}
static std::string compact(std::string s) {
    s.erase(std::remove_if(s.begin(), s.end(),
                           [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }),
            s.end());
    return s;
}
static std::string wsp(std::string_view s) {
    std::string out;
    bool space = false;
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            space = true;
        else {
            if (space && !out.empty())
                out += ' ';
            out += c;
            space = false;
        }
    }
    return out;
}
std::string canonical_header(std::string_view raw, bool relaxed) {
    if (!relaxed)
        return std::string(raw);
    auto colon = raw.find(':');
    if (colon == raw.npos)
        throw std::runtime_error("Invalid header");
    return lower(trim(std::string(raw.substr(0, colon)))) + ":" + wsp(raw.substr(colon + 1)) + "\r\n";
}
std::string canonical_body(std::string_view body, bool relaxed) {
    std::string out;
    std::size_t pos = 0;
    while (pos < body.size()) {
        auto end = body.find("\r\n", pos);
        auto line = body.substr(pos, (end == body.npos ? body.size() : end) - pos);
        if (relaxed) {
            std::string normalized;
            bool space = false;
            for (char c : line) {
                if (c == ' ' || c == '\t')
                    space = true;
                else {
                    if (space)
                        normalized += ' ';
                    normalized += c;
                    space = false;
                }
            }
            out += normalized;
        } else
            out += line;
        out += "\r\n";
        if (end == body.npos)
            break;
        pos = end + 2;
    }
    while (out.size() >= 2 && out.ends_with("\r\n"))
        out.resize(out.size() - 2);
    if (!out.empty() || !relaxed)
        out += "\r\n";
    return out;
}
static std::map<std::string, std::string> tags(std::string_view s) {
    std::map<std::string, std::string> out;
    while (!s.empty()) {
        auto semi = s.find(';');
        auto part = trim(std::string(s.substr(0, semi)));
        if (!part.empty()) {
            auto eq = part.find('=');
            if (eq == part.npos)
                throw std::runtime_error("Malformed DKIM tag");
            auto key = trim(part.substr(0, eq));
            if (!out.emplace(key, trim(part.substr(eq + 1))).second)
                throw std::runtime_error("Duplicate DKIM tag");
        }
        if (semi == s.npos)
            break;
        s.remove_prefix(semi + 1);
    }
    return out;
}
static std::uint64_t number(const std::string &s) {
    std::uint64_t n{};
    auto [p, e] = std::from_chars(s.data(), s.data() + s.size(), n);
    if (s.empty() || e != std::errc{} || p != s.data() + s.size())
        throw std::runtime_error("Invalid DKIM integer");
    return n;
}
static std::vector<unsigned char> unbase64(std::string s) {
    s = compact(s);
    if (s.empty() || s.size() % 4 == 1 || s.size() > 128 * 1024)
        throw std::runtime_error("Invalid base64 length");
    std::size_t padding = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        auto c = static_cast<unsigned char>(s[i]);
        if (c == '=') {
            ++padding;
            if (i < s.size() - 2 || padding > 2)
                throw std::runtime_error("Invalid base64 padding");
        } else if (padding || !(std::isalnum(c) || c == '+' || c == '/'))
            throw std::runtime_error("Invalid base64 alphabet");
    }
    while (s.size() % 4) {
        s += '=';
        ++padding;
    }
    std::vector<unsigned char> out(s.size());
    auto n = EVP_DecodeBlock(out.data(), reinterpret_cast<const unsigned char *>(s.data()),
                             static_cast<int>(s.size()));
    if (n < 0 || static_cast<std::size_t>(n) < padding)
        throw std::runtime_error("Invalid base64");
    out.resize(static_cast<std::size_t>(n) - padding);
    return out;
}
static std::array<unsigned char, 32> digest(std::string_view data) {
    std::array<unsigned char, 32> out{};
    unsigned int n{};
    if (EVP_Digest(data.data(), data.size(), out.data(), &n, EVP_sha256(), nullptr) != 1 || n != 32)
        throw std::runtime_error("SHA256 failure");
    return out;
}
static bool domain_name(const std::string &s, bool selector = false) {
    if (s.empty() || s.size() > 253 || s.front() == '.' || s.back() == '.' || s.find("..") != s.npos)
        return false;
    std::size_t len = 0;
    for (unsigned char c : s) {
        if (c == '.') {
            if (!len || len > 63)
                return false;
            len = 0;
        } else {
            if (!std::isalnum(c) && c != '-' && !(selector && c == '_'))
                return false;
            ++len;
        }
    }
    return len && len <= 63;
}
static std::string empty_signature(std::string raw) {
    auto colon = raw.find(':');
    std::size_t pos = colon + 1;
    while (pos < raw.size()) {
        auto end = raw.find(';', pos);
        if (end == raw.npos)
            end = raw.size();
        auto eq = raw.find('=', pos);
        if (eq < end && trim(raw.substr(pos, eq - pos)) == "b") {
            // Retain the terminating CRLF of the physical header.
            auto value_end = end;
            if (end == raw.size() && raw.ends_with("\r\n"))
                value_end -= 2;
            raw.erase(eq + 1, value_end - eq - 1);
            return raw;
        }
        pos = end + 1;
    }
    throw std::runtime_error("Missing b tag");
}
std::vector<DkimResult> verify_dkim(std::string_view raw, const KeyResolver &resolver) {
    if (raw.size() > max_range)
        return {{DkimStatus::Inconclusive, {}, {}, "Message exceeds verification limit"}};
    std::size_t body_offset{};
    std::vector<Header> headers;
    try {
        headers = parse_headers(raw, body_offset);
    } catch (const std::exception &e) {
        return {{DkimStatus::Inconclusive, {}, {}, e.what()}};
    }
    std::vector<DkimResult> results;
    for (std::size_t index = 0; index < headers.size(); ++index) {
        if (headers[index].name != "dkim-signature")
            continue;
        if (results.size() >= 16) {
            results.push_back({DkimStatus::Inconclusive, {}, {}, "Signature count limit exceeded"});
            break;
        }
        DkimResult r{DkimStatus::Invalid, {}, {}, ""};
        auto finish = [&](DkimStatus s, std::string detail) {
            r.status = s;
            r.detail = std::move(detail);
        };
        try {
            auto t = tags(headers[index].value);
            r.domain = lower(t["d"]);
            r.selector = t["s"];
            for (const auto *k : {"v", "a", "b", "bh", "d", "h", "s"})
                if (t[k].empty())
                    throw std::runtime_error("Missing required DKIM tag");
            if (t["v"] != "1" || !trim(headers[index].value).starts_with("v="))
                throw std::runtime_error("Invalid signature version");
            if (!domain_name(r.domain) || !domain_name(r.selector, true))
                throw std::runtime_error("Invalid DNS name");
            auto algorithm = t["a"];
            if (algorithm != "rsa-sha256" && algorithm != "ed25519-sha256") {
                finish(DkimStatus::Unsupported,
                       "Only RSA-SHA256 and Ed25519-SHA256 are accepted (RFC 8301/8463)");
                results.push_back(r);
                continue;
            }
            if (t.contains("q") && (":" + compact(t["q"]) + ":").find(":dns/txt:") == std::string::npos) {
                finish(DkimStatus::Unsupported, "Unsupported key query method");
                results.push_back(r);
                continue;
            }
            auto canon = t.contains("c") ? t["c"] : "simple/simple";
            auto slash = canon.find('/');
            auto hc = canon.substr(0, slash), bc = slash == canon.npos ? "simple" : canon.substr(slash + 1);
            if ((hc != "simple" && hc != "relaxed") || (bc != "simple" && bc != "relaxed")) {
                finish(DkimStatus::Unsupported, "Unsupported canonicalization");
                results.push_back(r);
                continue;
            }
            bool non_crlf = false;
            for (std::size_t i = 0; i < raw.size(); ++i)
                if ((raw[i] == '\n' && (i == 0 || raw[i - 1] != '\r')) ||
                    (raw[i] == '\r' && (i + 1 == raw.size() || raw[i + 1] != '\n')))
                    non_crlf = true;
            if (non_crlf) {
                finish(DkimStatus::Inconclusive,
                       "Original CRLF bytes unavailable; archive may have normalized mail");
                results.push_back(r);
                continue;
            }
            auto canonical = canonical_body(raw.substr(body_offset), bc == "relaxed");
            bool partial = false;
            if (t.contains("l")) {
                auto len = number(t["l"]);
                if (len > canonical.size())
                    throw std::runtime_error("Signed body length exceeds canonical body");
                partial = len < canonical.size();
                canonical.resize(static_cast<std::size_t>(len));
            }
            auto bh = unbase64(t["bh"]);
            auto hash = digest(canonical);
            if (bh.size() != 32 || CRYPTO_memcmp(bh.data(), hash.data(), 32) != 0)
                throw std::runtime_error("Body hash mismatch");
            std::set<std::size_t> used{index};
            std::string signed_bytes, names = compact(lower(t["h"]));
            bool from = false;
            unsigned int signed_fields = 0;
            while (!names.empty()) {
                if (++signed_fields > 512)
                    throw std::length_error("Signed header count exceeds verification limit");
                auto sep = names.find(':');
                auto name = names.substr(0, sep);
                if (name.empty())
                    throw std::runtime_error("Empty signed header name");
                if (name == "from")
                    from = true;
                for (std::size_t j = headers.size(); j-- > 0;)
                    if (headers[j].name == name && !used.contains(j)) {
                        signed_bytes += canonical_header(headers[j].raw, hc == "relaxed");
                        used.insert(j);
                        break;
                    }
                if (sep == names.npos)
                    break;
                names.erase(0, sep + 1);
            }
            if (!from || std::count_if(headers.begin(), headers.end(),
                                       [](const Header &h) { return h.name == "from"; }) != 1)
                throw std::runtime_error("A single From header must be signed");
            auto sig_header = canonical_header(empty_signature(headers[index].raw), hc == "relaxed");
            if (sig_header.ends_with("\r\n"))
                sig_header.resize(sig_header.size() - 2);
            signed_bytes += sig_header;
            std::string identity_domain = r.domain;
            if (t.contains("i")) {
                auto at = t["i"].rfind('@');
                if (at == t["i"].npos)
                    throw std::runtime_error("Invalid AUID");
                identity_domain = lower(t["i"].substr(at + 1));
                if (identity_domain != r.domain && !identity_domain.ends_with("." + r.domain))
                    throw std::runtime_error("AUID outside signing domain");
            }
            auto key_result = resolver(r.selector + "._domainkey." + r.domain);
            if (key_result.state != DnsKey::State::Found) {
                finish(key_result.state == DnsKey::State::Missing ? DkimStatus::MissingKey
                                                                  : DkimStatus::Inconclusive,
                       "DNS key absent or unavailable; historical keys may have rotated");
                results.push_back(r);
                continue;
            }
            auto key_tags = tags(key_result.record);
            if (key_tags.contains("v") && key_tags["v"] != "DKIM1")
                throw std::runtime_error("Invalid key version");
            if (key_tags.contains("v") && !trim(key_result.record).starts_with("v="))
                throw std::runtime_error("Key version must be first");
            if (!key_tags.contains("p") || compact(key_tags["p"]).empty())
                throw std::runtime_error("Key revoked or missing p tag");
            auto kt = key_tags.contains("k") ? key_tags["k"] : "rsa";
            if ((algorithm == "rsa-sha256" && kt != "rsa") ||
                (algorithm == "ed25519-sha256" && kt != "ed25519"))
                throw std::runtime_error("Key algorithm mismatch");
            auto contains_token = [](std::string list, const std::string &token) {
                list = ":" + compact(list) + ":";
                return list.find(":" + token + ":") != list.npos;
            };
            if (key_tags.contains("h") && !contains_token(key_tags["h"], "sha256"))
                throw std::runtime_error("Key excludes SHA256");
            if (key_tags.contains("s") && !contains_token(key_tags["s"], "email") &&
                !contains_token(key_tags["s"], "*"))
                throw std::runtime_error("Key excludes email service");
            if (contains_token(key_tags["t"], "s") && identity_domain != r.domain)
                throw std::runtime_error("Key requires exact AUID domain");
            auto der = unbase64(key_tags["p"]);
            EVP_PKEY *raw_key = nullptr;
            if (kt == "ed25519") {
                if (der.size() != 32)
                    throw std::runtime_error("Invalid Ed25519 key size");
                raw_key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, der.data(), der.size());
            } else {
                const unsigned char *cursor = der.data();
                raw_key = d2i_PUBKEY(nullptr, &cursor, static_cast<long>(der.size()));
                if (cursor != der.data() + der.size()) {
                    EVP_PKEY_free(raw_key);
                    raw_key = nullptr;
                }
            }
            std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(raw_key, EVP_PKEY_free);
            if (!key || (kt == "rsa" &&
                         (EVP_PKEY_base_id(key.get()) != EVP_PKEY_RSA || EVP_PKEY_bits(key.get()) < 1024)))
                throw std::runtime_error("Invalid or weak public key");
            if (kt == "rsa" && EVP_PKEY_bits(key.get()) > 8192)
                throw std::length_error("RSA key exceeds verification resource limit");
            std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
            auto sig = unbase64(t["b"]);
            auto signed_hash = digest(signed_bytes);
            auto data = kt == "ed25519" ? std::string_view(reinterpret_cast<const char *>(signed_hash.data()),
                                                           signed_hash.size())
                                        : std::string_view(signed_bytes);
            if (!ctx ||
                EVP_DigestVerifyInit(ctx.get(), nullptr, kt == "ed25519" ? nullptr : EVP_sha256(), nullptr,
                                     key.get()) != 1 ||
                EVP_DigestVerify(ctx.get(), sig.data(), sig.size(),
                                 reinterpret_cast<const unsigned char *>(data.data()), data.size()) != 1)
                throw std::runtime_error("Header signature mismatch");
            bool expired = false;
            if (t.contains("t"))
                number(t["t"]);
            if (t.contains("x")) {
                auto x = number(t["x"]);
                if (t.contains("t") && x <= number(t["t"]))
                    throw std::runtime_error("Invalid signature expiration");
                expired = x < static_cast<std::uint64_t>(
                                  std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
            }
            if (partial || expired || contains_token(key_tags["t"], "y"))
                finish(
                    DkimStatus::Inconclusive,
                    "Cryptographic match with partial body, expired signature or test key; inspect policy");
            else
                finish(
                    DkimStatus::Valid,
                    "Body hash and signed headers match domain key; this does not prove personal identity");
        } catch (const std::length_error &e) {
            finish(DkimStatus::Inconclusive, e.what());
        } catch (const std::exception &e) {
            finish(DkimStatus::Invalid, e.what());
        }
        results.push_back(std::move(r));
    }
    if (results.empty())
        results.push_back({DkimStatus::MissingSignature, {}, {}, "No DKIM-Signature header"});
    return results;
}
} // namespace afd
