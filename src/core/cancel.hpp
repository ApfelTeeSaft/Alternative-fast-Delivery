#pragma once
#include <atomic>
#include <memory>
#include <utility>

namespace afd {
class StopSource;
class StopToken {
    std::shared_ptr<const std::atomic_bool> state_;
    explicit StopToken(std::shared_ptr<const std::atomic_bool> state) noexcept : state_(std::move(state)) {}
    friend class StopSource;

  public:
    StopToken() noexcept = default;
    bool stop_requested() const noexcept {
        return state_ && state_->load(std::memory_order_acquire);
    }
};
class StopSource {
    std::shared_ptr<std::atomic_bool> state_ = std::make_shared<std::atomic_bool>(false);

  public:
    StopToken get_token() const noexcept {
        return StopToken(state_);
    }
    bool stop_requested() const noexcept {
        return state_ && state_->load(std::memory_order_acquire);
    }
    bool request_stop() noexcept {
        return state_ && !state_->exchange(true, std::memory_order_acq_rel);
    }
};
} // namespace afd
