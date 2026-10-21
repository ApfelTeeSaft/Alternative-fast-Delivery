#include "email/message.hpp"
#include "core/source.hpp"
#include <QDateTime>
#include <QStringDecoder>
#include <algorithm>
#include <cctype>
#include <functional>
#include <mailio/message.hpp>
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
Message parse_message(std::string_view raw) {
    if (raw.size() > max_range)
        throw std::runtime_error("Message exceeds 16 MiB inspection limit");
    Message out;
    std::size_t body{};
    out.headers = parse_headers(raw, body);
    out.sender = header_value(out.headers, "from");
    out.recipient = header_value(out.headers, "to");
    out.subject = header_value(out.headers, "subject");
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
    mailio::message msg;
    msg.line_policy(mailio::codec::line_len_policy_t::NONE);
    msg.parse(std::string(raw), false);
    out.subject = msg.subject();
    std::size_t count = 0;
    std::function<void(mailio::mime &, int)> walk = [&](mailio::mime &part, int depth) {
        if (depth > 24 || ++count > 256)
            throw std::runtime_error("MIME part limit exceeded");
        auto ct = part.content_type();
        auto content = part.content();
        auto name = part.name().buffer;
        bool attachment =
            part.content_disposition() == mailio::mime::content_disposition_t::ATTACHMENT || !name.empty();
        std::string type = ct.media_subtype();
        out.parts.push_back({type, name, ct.charset().buffer, content.size(), attachment});
        out.attachments |= attachment;
        if (!attachment && ct.media_type() == mailio::mime::media_type_t::TEXT) {
            auto text = utf8(content, ct.charset().buffer, out.warning);
            if (lower(type) == "html")
                out.html += text;
            else
                out.text += text;
        }
        auto children = part.parts();
        for (auto &child : children)
            walk(child, depth + 1);
    };
    walk(msg, 0);
    return out;
}
} // namespace afd
