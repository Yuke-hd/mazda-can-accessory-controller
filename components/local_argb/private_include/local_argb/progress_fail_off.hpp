#pragma once

#include <atomic>
#include <cstdint>

#include "local_argb/lighting_sink.hpp"
#include "local_argb/progress_watchdog.hpp"
#include "local_argb/stall_gated_sink.hpp"

namespace local_argb::internal {

// Transitions applied since the last report was taken.
struct ProgressReport {
  std::uint32_t stalls{0};
  std::uint32_t resumes{0};
};

// The renderer's response to a watched-progress transition. A stall closes
// the gate first, then writes black past it, so a publish racing the close is
// followed by black (see StallGatedSink). A resume only opens the gate.
//
// apply() runs on the highest-priority supervisor, whose small stack must not
// format log output, so it only counts transitions. A lower-priority task
// takes the report and logs it.
class ProgressFailOff {
public:
  ProgressFailOff(StallGatedSink &gate, LightingSink &ungated) noexcept
      : gate_(&gate), ungated_(&ungated) {}

  void apply(const ProgressTransition transition) noexcept {
    if (transition == ProgressTransition::Stalled)
      fail_off();
    if (transition == ProgressTransition::Resumed)
      resume();
  }

  [[nodiscard]] ProgressReport take_report() noexcept {
    return ProgressReport{stalls_.exchange(0, std::memory_order_relaxed),
                          resumes_.exchange(0, std::memory_order_relaxed)};
  }

private:
  void fail_off() noexcept {
    gate_->close();
    (void)ungated_->publish(LightingCommand{});
    stalls_.fetch_add(1, std::memory_order_relaxed);
  }

  void resume() noexcept {
    gate_->open();
    resumes_.fetch_add(1, std::memory_order_relaxed);
  }

  StallGatedSink *gate_;
  LightingSink *ungated_;
  std::atomic<std::uint32_t> stalls_{0};
  std::atomic<std::uint32_t> resumes_{0};
};

} // namespace local_argb::internal
