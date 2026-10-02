#pragma once

#include "local_argb/renderer.hpp"
#include "local_argb/stall_gated_sink.hpp"

namespace local_argb::internal {

// Shared worker wiring: every faulted pass invalidates pending starts, and
// explicit fail-off cancels unseen starts before overwriting the queue.
class RendererRuntime {
public:
  RendererRuntime(RendererController &renderer, StallGatedSink &gate) noexcept
      : renderer_(&renderer), gate_(&gate) {}

  bool apply(const LightingCommand &command, vehicle_core::MonotonicTimestamp now_us) noexcept {
    const bool success = renderer_->apply(command, now_us, gate_->transient_epoch());
    invalidate_if_faulted();
    return success;
  }

  bool tick(vehicle_core::MonotonicTimestamp now_us) noexcept {
    const bool success = renderer_->tick(now_us);
    invalidate_if_faulted();
    return success;
  }

  // Does not reopen a closed progress gate. False asks the caller to use its
  // direct black fallback if the queue is unavailable.
  bool fail_off(LightingSink &ungated) noexcept {
    gate_->invalidate_transients();
    return ungated.publish(LightingCommand{});
  }

private:
  void invalidate_if_faulted() noexcept {
    if (renderer_->faulted())
      gate_->invalidate_transients();
  }

  RendererController *renderer_;
  StallGatedSink *gate_;
};

} // namespace local_argb::internal
