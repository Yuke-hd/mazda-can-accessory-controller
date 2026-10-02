#pragma once

#include <atomic>

#include "local_argb/lighting_sink.hpp"

namespace local_argb::internal {

// Rejects lighting commands while a watched loop is stalled, so no publisher,
// such as a polled rule, can relight a strip that was failed off. The gate is
// paired with a cancellation generation; it re-emits nothing when it opens.
class StallGatedSink final : public LightingSink {
public:
  explicit StallGatedSink(LightingSink &downstream) noexcept : downstream_(&downstream) {}

  bool publish(const LightingCommand &command) noexcept override {
    const auto epoch = transient_epoch();
    if (closed_.load(std::memory_order_seq_cst))
      return false;
    const bool accepted = downstream_->publish(command);
    if (!closed_.load(std::memory_order_seq_cst) && epoch == transient_epoch())
      return accepted;
    // The gate closed or its epoch changed during publication, including
    // faulted worker passes and explicit fail-off. Write black after any
    // racing command so it cannot stay lit.
    (void)downstream_->publish(LightingCommand{});
    return false;
  }

  [[nodiscard]] std::uint32_t transient_epoch() const noexcept override {
    return epoch_.load(std::memory_order_seq_cst);
  }
  void invalidate_transients() noexcept { epoch_.fetch_add(1, std::memory_order_seq_cst); }
  void close() noexcept {
    closed_.store(true, std::memory_order_seq_cst);
    invalidate_transients();
  }
  void open() noexcept {
    // Starts captured while closed must also be invalid after resume.
    invalidate_transients();
    closed_.store(false, std::memory_order_seq_cst);
  }

private:
  LightingSink *downstream_;
  std::atomic<bool> closed_{false};
  std::atomic<std::uint32_t> epoch_{0};
};

} // namespace local_argb::internal
