#pragma once
#include "core/source.hpp"
#include <functional>
namespace afd {
struct MessageRange {
    std::uint64_t begin, end, checkpoint;
};
using MessageVisitor = std::function<bool(MessageRange)>;
void scan_mbox(Source &, std::uint64_t start, const MessageVisitor &, afd::StopToken = {});
} // namespace afd
