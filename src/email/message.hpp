#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
namespace afd {
struct Header {
    std::string name, value, raw;
};
struct Part {
    std::string type, name, charset;
    std::uint64_t size{};
    bool attachment{};
};
struct Message {
    std::vector<Header> headers;
    std::vector<Part> parts;
    std::string sender, recipient, subject, date, text, html, warning;
    std::int64_t timestamp{};
    bool attachments{};
};
std::string lower(std::string);
std::string trim(std::string);
std::vector<Header> parse_headers(std::string_view raw, std::size_t &body_offset);
std::string header_value(const std::vector<Header> &, const std::string &name);
Message parse_message(std::string_view raw);
} // namespace afd
