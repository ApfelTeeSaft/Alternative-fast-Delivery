#include "core/source.hpp"
#include <array>
#include <cctype>
#include <iomanip>
#include <memory>
#include <openssl/evp.h>
#include <sstream>
#include <stdexcept>
#ifdef __ANDROID__
#include <sys/types.h>
static_assert(sizeof(off_t) == 8, "Large archives require 64-bit Android file offsets");
#endif

namespace afd {
void check_range(std::uint64_t size, std::uint64_t offset, std::size_t length) {
    if (length > max_range || offset > size || length > size - offset)
        throw std::out_of_range("Range exceeds source or 16 MiB inspection limit");
}
FileSource::FileSource(const std::filesystem::path &path)
    : stream_(path, std::ios::binary), size_(std::filesystem::file_size(path)) {
    if (!stream_)
        throw std::runtime_error("Cannot open source");
}
Bytes FileSource::read(std::uint64_t offset, std::size_t length, afd::StopToken stop) {
    check_range(size_, offset, length);
    if (stop.stop_requested())
        throw std::runtime_error("Cancelled");
    Bytes out(length);
    stream_.clear();
    stream_.seekg(static_cast<std::streamoff>(offset));
    stream_.read(out.data(), static_cast<std::streamsize>(length));
    if (static_cast<std::size_t>(stream_.gcount()) != length)
        throw std::runtime_error("Source changed or read failed");
    return out;
}
bool safe_relative_path(const std::string &p) {
    if (p.empty() || p.front() == '/' || p.front() == '\\' || p.find(':') != std::string::npos ||
        p.find('\0') != std::string::npos)
        return false;
    std::string segment;
    for (char c : p + "/") {
        if (c == '/' || c == '\\') {
            if (segment.empty() || segment == "." || segment == "..")
                return false;
            if (segment.back() == '.' || segment.back() == ' ')
                return false;
            auto upper = segment;
            for (auto &ch : upper)
                ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            upper = upper.substr(0, upper.find('.'));
            while (!upper.empty() && upper.back() == ' ')
                upper.pop_back();
            if (upper == "CON" || upper == "PRN" || upper == "AUX" || upper == "NUL" ||
                (upper.size() == 4 && (upper.starts_with("COM") || upper.starts_with("LPT")) &&
                 upper[3] >= '1' && upper[3] <= '9'))
                return false;
            segment.clear();
        } else {
            if (static_cast<unsigned char>(c) < 32 || c == '*' || c == '?' || c == '"' || c == '<' ||
                c == '>' || c == '|')
                return false;
            segment += c;
        }
    }
    return true;
}
std::string hex_preview(std::span<const char> data, std::uint64_t base) {
    std::ostringstream s;
    for (std::size_t row = 0; row < data.size(); row += 16) {
        s << std::hex << std::setw(12) << std::setfill('0') << base + row << "  ";
        for (std::size_t j = 0; j < 16; ++j) {
            if (row + j < data.size())
                s << std::setw(2) << static_cast<unsigned>(static_cast<unsigned char>(data[row + j])) << ' ';
            else
                s << "   ";
        }
        s << " ";
        for (std::size_t j = row; j < data.size() && j < row + 16; ++j)
            s << (data[j] >= 32 && data[j] < 127 ? data[j] : '.');
        s << '\n';
    }
    return s.str();
}
std::string detect_type(std::span<const char> data) {
    std::string_view s(data.data(), data.size());
    if (s.starts_with("From "))
        return "MBOX";
    if (s.starts_with("SQLite format 3"))
        return "SQLite database (inspection only)";
    if (s.starts_with("%PDF"))
        return "PDF";
    if (s.starts_with("PK\x03\x04"))
        return "ZIP";
    if (s.find("CREATE TABLE") != s.npos || s.find("INSERT INTO") != s.npos)
        return "SQL text (never executed)";
    if (s.find("Subject:") != s.npos || s.find("MIME-Version:") != s.npos)
        return "RFC 5322 email";
    if (s.find('\0') != s.npos)
        return "Binary";
    return "Text / unknown";
}
std::string sha256(std::string_view data) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int n{};
    if (EVP_Digest(data.data(), data.size(), digest.data(), &n, EVP_sha256(), nullptr) != 1)
        throw std::runtime_error("SHA-256 failed");
    std::ostringstream out;
    for (unsigned int i = 0; i < n; ++i)
        out << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(digest[i]);
    return out.str();
}
} // namespace afd
