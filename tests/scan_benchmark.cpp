#include "email/mbox.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>

class VirtualArchive final : public afd::Source {
    std::uint64_t size_;
    const std::string first_ = "From fixture Tue Oct 20 12:00:00 2026\nFrom: test@example.invalid\n\nx";
    const std::string last_ = "\nFrom fixture Tue Oct 20 12:01:00 2026\nFrom: end@example.invalid\n\nend\n";

  public:
    std::uint64_t bytes_read{}, reads{};
    std::size_t largest_read{};
    explicit VirtualArchive(std::uint64_t size) : size_(size) {}
    std::uint64_t size() const override {
        return size_;
    }
    afd::Bytes read(std::uint64_t offset, std::size_t length, afd::StopToken) override {
        afd::check_range(size_, offset, length);
        bytes_read += length;
        ++reads;
        largest_read = std::max(largest_read, length);
        afd::Bytes result(length, 'x');
        auto copy = [&](std::uint64_t start, const std::string &data) {
            auto begin = std::max(start, offset);
            auto end = std::min(start + data.size(), offset + length);
            if (begin < end)
                std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(begin - start), end - begin,
                            result.begin() + static_cast<std::ptrdiff_t>(begin - offset));
        };
        copy(0, first_);
        copy(size_ - last_.size(), last_);
        return result;
    }
};

int main(int argc, char **argv) {
    try {
        auto gib = argc == 2 ? std::stoull(argv[1]) : 100ULL;
        if (gib < 1 || gib > 1024)
            throw std::runtime_error("Choose 1 through 1024 GiB");
        VirtualArchive source(gib * 1024 * 1024 * 1024);
        unsigned int messages = 0;
        std::uint64_t checkpoint = 0;
        auto start = std::chrono::steady_clock::now();
        afd::scan_mbox(source, 0, [&](afd::MessageRange range) {
            if (range.end < range.begin || range.checkpoint < checkpoint)
                throw std::runtime_error("Incorrect 64-bit boundary");
            ++messages;
            checkpoint = range.checkpoint;
            return true;
        });
        auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (messages != 2 || checkpoint != source.size() || source.bytes_read != source.size() ||
            source.largest_read > 65536)
            throw std::runtime_error("Incomplete streaming scan");
        std::cout
            << "PASS virtual " << gib << " GiB MBOX: " << messages << " messages; " << source.bytes_read
            << " bytes scanned; " << source.reads << " reads; max read " << source.largest_read << " bytes; "
            << elapsed << " seconds; " << static_cast<double>(source.size()) / (1024 * 1024) / elapsed
            << " MiB/s\n"
            << "Virtual generated source; this does not measure physical storage or torrent throughput.\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
