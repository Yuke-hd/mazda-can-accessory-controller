#pragma once

#include <atomic>

#include "local_argb/lighting_sink.hpp"

namespace local_argb::internal {

// Rejects lighting commands while a watched loop is stalled, so no publisher,
// such as a polled rule, can relight a strip that was failed off. The gate is
// the only state; it re-emits nothing when it opens.
class StallGatedSink final : public LightingSink {
public:
  explicit StallGatedSink(LightingSink &downstream) noexcept : downstream_(&downstream) {}

  bool publish(const LightingCommand &command) noexcept override {
    if (closed_.load(std::memory_order_seq_cst))
      return false;
    const bool accepted = downstream_->publish(command);
    if (!closed_.load(std::memory_order_seq_cst))
      return accepted;
    // The gate closed while this command was on its way, possibly after the
    // stall fail-off. Fail off again so the command cannot stay lit.
    (void)downstream_->publish(LightingCommand{});
    return false;
  }

  void close() noexcept { closed_.store(true, std::memory_order_seq_cst); }
  void open() noexcept { closed_.store(false, std::memory_order_seq_cst); }

private:
  LightingSink *downstream_;
  std::atomic<bool> closed_{false};
};

} // namespace local_argb::internal
