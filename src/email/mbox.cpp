#include "email/mbox.hpp"
#include <optional>
#include <regex>
#include <stdexcept>
namespace afd {
void scan_mbox(Source &src, std::uint64_t start, const MessageVisitor &visit, afd::StopToken stop) {
    if (start > src.size())
        throw std::out_of_range("MBOX checkpoint outside source");
    if (start == src.size())
        return;
    std::uint64_t line_start = start, pos = start;
    std::optional<std::uint64_t> message;
    std::string prefix;
    bool first = true;
    static const std::regex envelope_pattern(
        R"(^From [^ \t]+[ \t]+(Mon|Tue|Wed|Thu|Fri|Sat|Sun)[ \t]+(Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec)[ \t]+[0-3]?[0-9][ \t]+[0-2][0-9]:[0-5][0-9](:[0-6][0-9])?([ \t]+[^ \t]+)?[ \t]+[0-9]{4}([ \t]+.*)?\r?$)");
    auto line = [&](std::uint64_t next) {
        bool envelope = prefix.starts_with("From ") && std::regex_match(prefix, envelope_pattern);
        if (first && !envelope)
            throw std::runtime_error(
                "Expected MBOX envelope at checkpoint; select EML for standalone messages");
        first = false;
        if (envelope) {
            if (message && !visit({*message, line_start, line_start}))
                return false;
            message = next;
        }
        prefix.clear();
        line_start = next;
        return true;
    };
    while (pos < src.size()) {
        if (stop.stop_requested())
            return;
        auto data = src.read(
            pos, static_cast<std::size_t>(std::min<std::uint64_t>(64 * 1024, src.size() - pos)), stop);
        for (char c : data) {
            ++pos;
            if (c == '\n') {
                if (!line(pos))
                    return;
            } else if (prefix.size() < 512)
                prefix += c;
        }
    }
    if (line_start != pos && !line(pos))
        return;
    if (message && !stop.stop_requested())
        visit({*message, src.size(), src.size()});
}
} // namespace afd
