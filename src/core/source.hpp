#pragma once
#include "core/cancel.hpp"
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace afd {
constexpr std::size_t max_range = 16 * 1024 * 1024;
using Bytes = std::vector<char>;
class Source {
  public:
    virtual ~Source() = default;
    virtual std::uint64_t size() const = 0;
    virtual Bytes read(std::uint64_t offset, std::size_t length, afd::StopToken stop = {}) = 0;
};
class FileSource final : public Source {
    std::ifstream stream_;
    std::uint64_t size_;

  public:
    explicit FileSource(const std::filesystem::path &path);
    std::uint64_t size() const override {
        return size_;
    }
    Bytes read(std::uint64_t, std::size_t, afd::StopToken = {}) override;
};
void check_range(std::uint64_t size, std::uint64_t offset, std::size_t length);
bool safe_relative_path(const std::string &path);
std::string hex_preview(std::span<const char> data, std::uint64_t base = 0);
std::string detect_type(std::span<const char> data);
std::string sha256(std::string_view bytes);
} // namespace afd
