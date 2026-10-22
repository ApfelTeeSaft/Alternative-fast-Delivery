#include "email/message.hpp"
#include "core/source.hpp"
#include <QByteArray>
#include <QDateTime>
#include <QStringDecoder>
#include <algorithm>
#include <cctype>
#include <functional>
#include <mailio/mime.hpp>
#include <mailio/q_codec.hpp>
#include <map>
#include <stdexcept>
namespace afd {
std::string lower(std::string s) {
    for (auto &c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string trim(std::string s) {
    auto a = s.find_first_not_of(" \t\r\n");
    if (a == s.npos)
        return {};
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}
std::vector<Header> parse_headers(std::string_view raw, std::size_t &body) {
    std::vector<Header> out;
    std::size_t pos = 0;
    while (pos < raw.size()) {
        auto end = raw.find('\n', pos);
        auto next = end == raw.npos ? raw.size() : end + 1;
        if (next > 1024 * 1024)
            throw std::runtime_error("Headers exceed 1 MiB");
        auto line = raw.substr(pos, (end == raw.npos ? raw.size() : end) - pos);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        if (line.empty()) {
            body = next;
            return out;
        }
        std::string original(raw.substr(pos, next - pos));
        if (line.front() == ' ' || line.front() == '\t') {
            if (out.empty())
                throw std::runtime_error("Orphan header continuation");
            out.back().raw += original;
            out.back().value += " " + trim(std::string(line));
        } else {
            auto colon = line.find(':');
            if (colon == line.npos || colon == 0 || out.size() >= 4096)
                throw std::runtime_error("Malformed or excessive headers");
            std::string name(line.substr(0, colon));
            if (!std::all_of(name.begin(), name.end(),
                             [](unsigned char c) { return c >= 33 && c <= 126 && c != ':'; }))
                throw std::runtime_error("Invalid header name");
            out.push_back({lower(name), trim(std::string(line.substr(colon + 1))), original});
        }
        pos = next;
    }
    throw std::runtime_error("Message has no header/body separator");
}
std::string header_value(const std::vector<Header> &h, const std::string &name) {
    for (const auto &v : h)
        if (v.name == name)
            return v.value;
    return {};
}
static std::string utf8(const std::string &bytes, std::string charset, std::string &warning) {
    if (charset.empty())
        charset = "UTF-8";
    QStringDecoder dec(charset.c_str());
    if (!dec.isValid()) {
        warning += "Unsupported charset: " + charset + ". ";
        return bytes;
    }
    QString s = dec.decode(QByteArrayView(bytes.data(), static_cast<qsizetype>(bytes.size())));
    if (dec.hasError())
        warning += "Invalid character sequence replaced. ";
    return s.toUtf8().toStdString();
}
static std::string decoded_header(const std::string &value, std::string &warning) {
    auto unlimited = static_cast<std::size_t>(mailio::codec::line_len_policy_t::NONE);
    mailio::q_codec codec(unlimited, unlimited);
    std::string decoded;
    std::size_t pos = 0;
    bool previous_encoded = false;
    while (pos < value.size()) {
        auto begin = value.find("=?", pos);
        if (begin == value.npos) {
            decoded += value.substr(pos);
            break;
        }
        auto charset_end = value.find('?', begin + 2);
        auto encoding_end = charset_end == value.npos ? value.npos : value.find('?', charset_end + 1);
        auto end = encoding_end == value.npos ? value.npos : value.find("?=", encoding_end + 1);
        if (end == value.npos) {
            warning += "Unreadable encoded header kept as stored. ";
            return value;
        }
        auto gap = value.substr(pos, begin - pos);
        if (!previous_encoded || gap.find_first_not_of(" \t") != gap.npos)
            decoded += gap;
        try {
            auto [bytes, charset, encoding] = codec.check_decode(value.substr(begin, end + 2 - begin));
            decoded += utf8(bytes, charset, warning);
        } catch (const std::exception &) {
            warning += "Unreadable encoded header kept as stored. ";
            return value;
        }
        previous_encoded = true;
        pos = end + 2;
    }
    return decoded;
}
namespace {
struct MimeValue {
    std::string value;
    std::map<std::string, std::string> parameters;
};
MimeValue mime_value(std::string_view input) {
    MimeValue result;
    auto semi = input.find(';');
    result.value = lower(trim(std::string(input.substr(0, semi))));
    std::size_t pos = semi == input.npos ? input.size() : semi + 1;
    while (pos < input.size()) {
        while (pos < input.size() && (input[pos] == ';' || input[pos] == ' ' || input[pos] == '\t'))
            ++pos;
        auto equals = input.find('=', pos);
        if (equals == input.npos)
            break;
        auto name = lower(trim(std::string(input.substr(pos, equals - pos))));
        pos = equals + 1;
        while (pos < input.size() && (input[pos] == ' ' || input[pos] == '\t'))
            ++pos;
        std::string value;
        if (pos < input.size() && input[pos] == '"') {
            ++pos;
            bool closed = false;
            while (pos < input.size()) {
                auto ch = input[pos++];
                if (ch == '"') {
                    closed = true;
                    break;
                }
                if (ch == '\\' && pos < input.size())
                    ch = input[pos++];
                value += ch;
            }
            if (!closed)
                throw std::runtime_error("MIME parameter has an unclosed quote");
        } else {
            auto end = input.find(';', pos);
            if (end == input.npos)
                end = input.size();
            value = trim(std::string(input.substr(pos, end - pos)));
            pos = end;
        }
        result.parameters.emplace(std::move(name), std::move(value));
        while (pos < input.size() && input[pos] != ';')
            ++pos;
    }
    return result;
}
int hex_digit(unsigned char ch) {
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'A' && ch <= 'F')
        return ch - 'A' + 10;
    if (ch >= 'a' && ch <= 'f')
        return ch - 'a' + 10;
    return -1;
}
std::string transfer_decode(std::string_view body, const std::string &encoding, std::string &warning) {
    if (encoding == "base64") {
        QByteArray compact;
        for (unsigned char ch : body)
            if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n')
                compact += static_cast<char>(ch);
        auto decoded = QByteArray::fromBase64Encoding(compact, QByteArray::AbortOnBase64DecodingErrors);
        if (!decoded)
            throw std::runtime_error("Invalid base64 MIME body");
        return decoded.decoded.toStdString();
    }
    if (encoding != "quoted-printable") {
        if (!encoding.empty() && encoding != "7bit" && encoding != "8bit" && encoding != "binary")
            throw std::runtime_error("Unsupported MIME transfer encoding: " + encoding);
        return std::string(body);
    }
    std::string decoded;
    decoded.reserve(body.size());
    bool invalid_escape = false;
    for (std::size_t i = 0; i < body.size(); ++i) {
        if (body[i] == '=') {
            if (i + 1 < body.size() && body[i + 1] == '\n') {
                ++i;
                continue;
            }
            if (i + 2 < body.size() && body[i + 1] == '\r' && body[i + 2] == '\n') {
                i += 2;
                continue;
            }
            if (i + 2 < body.size() && hex_digit(body[i + 1]) >= 0 && hex_digit(body[i + 2]) >= 0) {
                decoded += static_cast<char>((hex_digit(body[i + 1]) << 4) | hex_digit(body[i + 2]));
                i += 2;
                continue;
            }
            invalid_escape = true;
        }
        // Broken exporters sometimes leave literal 8-bit bytes or CR in QP.
        // Retain those bytes instead of discarding an otherwise readable part.
        decoded += body[i];
    }
    if (invalid_escape)
        warning += "Invalid quoted-printable escape kept as stored. ";
    return decoded;
}
class ArchiveMime {
    Message &out_;
    std::size_t count_{};

  public:
    explicit ArchiveMime(Message &out) : out_(out) {}
    void read(std::string_view raw, int depth = 0) {
        if (depth > 24 || ++count_ > 256)
            throw std::runtime_error("MIME part limit exceeded");
        std::size_t offset{};
        auto headers = parse_headers(raw, offset);
        auto type = mime_value(header_value(headers, "content-type"));
        auto disposition = mime_value(header_value(headers, "content-disposition"));
        if (type.value.empty())
            type.value = "text/plain";
        auto body = raw.substr(offset);
        auto name = disposition.parameters.contains("filename") ? disposition.parameters["filename"]
                                                                : type.parameters["name"];
        name = decoded_header(name, out_.warning);
        bool attachment = disposition.value == "attachment" || !name.empty();
        out_.attachments |= attachment;
        auto slash = type.value.find('/');
        auto subtype = slash == type.value.npos ? type.value : type.value.substr(slash + 1);
        auto charset = type.parameters["charset"];
        auto part_index = out_.parts.size();
        out_.parts.push_back({subtype, name, charset, 0, attachment});
        if (!type.value.starts_with("multipart/")) {
            auto content = transfer_decode(
                body, lower(trim(header_value(headers, "content-transfer-encoding"))), out_.warning);
            out_.parts[part_index].size = content.size();
            if (!attachment && type.value.starts_with("text/")) {
                auto text = utf8(content, charset, out_.warning);
                (subtype == "html" ? out_.html : out_.text) += text;
            }
            return;
        }
        auto boundary = type.parameters["boundary"];
        if (boundary.empty() || boundary.size() > 200)
            throw std::runtime_error("Missing or excessive MIME boundary");
        std::string marker = "--" + boundary;
        std::size_t pos = 0, begin = std::string_view::npos;
        bool found = false;
        while (pos < body.size()) {
            auto end = body.find('\n', pos);
            auto next = end == body.npos ? body.size() : end + 1;
            auto line = body.substr(pos, (end == body.npos ? body.size() : end) - pos);
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
                line.remove_suffix(1);
            bool close = line == marker + "--";
            if (line == marker || close) {
                found = true;
                if (begin != body.npos) {
                    auto part = body.substr(begin, pos - begin);
                    // The line ending before a MIME delimiter belongs to it.
                    // Keep the header separator when an exporter omits the
                    // extra delimiter newline after an empty body.
                    std::size_t part_body{};
                    parse_headers(part, part_body);
                    if (part.ends_with("\r\n") && part.size() >= part_body + 2)
                        part.remove_suffix(2);
                    else if (part.ends_with("\n") && part.size() > part_body)
                        part.remove_suffix(1);
                    read(part, depth + 1);
                }
                if (close)
                    return;
                begin = next;
            }
            pos = next;
        }
        if (!found || begin == body.npos)
            throw std::runtime_error("MIME boundary not found");
        out_.warning += "Multipart closing boundary missing. ";
        if (begin < body.size())
            read(body.substr(begin), depth + 1);
    }
};
} // namespace
Message parse_message(std::string_view raw) {
    if (raw.size() > max_range)
        throw std::runtime_error("Message exceeds 16 MiB inspection limit");
    Message out;
    std::size_t body{};
    out.headers = parse_headers(raw, body);
    out.sender = decoded_header(header_value(out.headers, "from"), out.warning);
    out.recipient = decoded_header(header_value(out.headers, "to"), out.warning);
    out.subject = decoded_header(header_value(out.headers, "subject"), out.warning);
    out.date = header_value(out.headers, "date");
    out.timestamp =
        QDateTime::fromString(QString::fromStdString(out.date), Qt::RFC2822Date).toSecsSinceEpoch();
    // Bound recursive parser input before handing untrusted MIME to mailio.
    std::size_t multipart = 0, lines = 0;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\n')
            ++lines;
        if ((raw[i] == 'm' || raw[i] == 'M') && i + 10 < raw.size() &&
            lower(std::string(raw.substr(i, 10))) == "multipart/")
            ++multipart;
    }
    if (multipart > 24 || lines > 200000)
        throw std::runtime_error("MIME complexity limit exceeded");
    // Archive readers need MIME decoding, not SMTP address/date validation.
    // mailio::message rejects otherwise readable mail with unusual addresses.
    mailio::mime msg;
    msg.line_policy(mailio::codec::line_len_policy_t::NONE);
    std::string mime_input;
    mime_input.reserve(raw.size() + lines);
    bool normalized = false;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\n' && (i == 0 || raw[i - 1] != '\r')) {
            mime_input += '\r';
            normalized = true;
        }
        mime_input += raw[i];
    }
    // Only this decoding copy changes. Header.raw and DKIM input retain the
    // original archive bytes, including any LF-only or mixed line endings.
    if (normalized)
        out.warning += "Archive line endings adjusted for reading; original bytes kept. ";
    std::size_t count = 0;
    std::function<void(mailio::mime &, int)> walk = [&](mailio::mime &part, int depth) {
        if (depth > 24 || ++count > 256)
            throw std::runtime_error("MIME part limit exceeded");
        auto ct = part.content_type();
        auto content = part.content();
        auto name = part.name().buffer;
        bool attachment =
            part.content_disposition() == mailio::mime::content_disposition_t::ATTACHMENT || !name.empty();
        auto children = part.parts();
        // mailio can silently drop a one-character boundary parameter, or
        // ignore otherwise valid delimiter lines with trailing whitespace.
        if (ct.media_type() == mailio::mime::media_type_t::MULTIPART && children.empty())
            throw mailio::mime_error("Multipart body was not decoded", "");
        bool default_text = ct.media_type() == mailio::mime::media_type_t::NONE && children.empty();
        std::string type = default_text ? "plain" : ct.media_subtype();
        out.parts.push_back({type, name, ct.charset().buffer, content.size(), attachment});
        out.attachments |= attachment;
        if (!attachment && (ct.media_type() == mailio::mime::media_type_t::TEXT || default_text)) {
            auto text = utf8(content, ct.charset().buffer, out.warning);
            if (lower(type) == "html")
                out.html += text;
            else
                out.text += text;
        }
        for (auto &child : children)
            walk(child, depth + 1);
    };
    auto recover = [&] {
        out.parts.clear();
        out.text.clear();
        out.html.clear();
        out.attachments = false;
        out.warning += "Nonstandard MIME formatting recovered; original bytes kept. ";
        ArchiveMime(out).read(raw);
        return out;
    };
    try {
        msg.parse(mime_input, false);
        walk(msg, 0);
        return out;
    } catch (const mailio::mime_error &) {
        return recover();
    } catch (const mailio::codec_error &) {
        return recover();
    }
}
} // namespace afd
