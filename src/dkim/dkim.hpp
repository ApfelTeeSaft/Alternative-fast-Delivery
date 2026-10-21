#pragma once
#include <functional>
#include <string>
#include <string_view>
#include <vector>
namespace afd {
enum class DkimStatus { Valid, Invalid, MissingSignature, MissingKey, Unsupported, Inconclusive };
struct DkimResult {
    DkimStatus status;
    std::string domain, selector, detail;
};
struct DnsKey {
    enum class State { Found, Missing, Unavailable };
    State state;
    std::string record;
};
using KeyResolver = std::function<DnsKey(const std::string &)>;
std::string status_name(DkimStatus);
std::string canonical_body(std::string_view, bool relaxed);
std::string canonical_header(std::string_view, bool relaxed);
std::vector<DkimResult> verify_dkim(std::string_view raw, const KeyResolver &);
} // namespace afd
