#pragma once

#include <memory>

#include "local_argb/pixel_frame.hpp"
#include "replay/output_stage.hpp"

namespace replay {

// Replay output stage for the local ARGB strip. It applies the default
// lighting profile to the action engine and runs the production
// LedActionSink -> RendererController path with the center-out fill
// animation, writing each deduplicated frame to the caller's PixelFrameSink.
// The sink's write() runs on the controller's host thread.
class LocalArgbOutputStage final : public OutputStage {
public:
  LocalArgbOutputStage(local_argb::PixelFrameSink &pixels, vehicle_core::MonotonicClock &clock);
  ~LocalArgbOutputStage() noexcept;

  [[nodiscard]] bool configure(action_engine::ActionEngine &engine) noexcept override;
  [[nodiscard]] bool start(vehicle_core::MonotonicTimestamp now_us) noexcept override;
  [[nodiscard]] bool tick(vehicle_core::MonotonicTimestamp now_us) noexcept override;
  [[nodiscard]] bool fail_off(vehicle_core::MonotonicTimestamp now_us) noexcept override;
  [[nodiscard]] bool stop(vehicle_core::MonotonicTimestamp now_us) noexcept override;
  [[nodiscard]] vehicle_core::Microseconds tick_period_us() const noexcept override;

private:
  class Implementation;
  std::unique_ptr<Implementation> implementation_;
};

} // namespace replay
