#pragma once

#include <cstddef>
#include <ostream>
#include <vector>

#include "gvret/replay_clock.hpp"
#include "local_argb/local_argb.h"

namespace gvret {

struct TimestampedPixelFrame final {
  vehicle_core::MonotonicTimestamp timestamp_us{0};
  local_argb::PixelFrame pixels{};
};

// Records the production renderer's deduplicated writes with the caller-owned
// replay clock timestamp. The clock is the only timestamp source, so a
// recording cannot expose an original capture timestamp.
class TimestampedPixelFrameSink final : public local_argb::PixelFrameSink {
public:
  explicit TimestampedPixelFrameSink(const ReplayClock &clock) noexcept : clock_(&clock) {}

  bool write(const local_argb::PixelFrame &frame) noexcept override;

  [[nodiscard]] const std::vector<TimestampedPixelFrame> &frames() const noexcept {
    return frames_;
  }

private:
  const ReplayClock *clock_;
  std::vector<TimestampedPixelFrame> frames_{};
};

// Writes one compact JSON object per renderer write. The stream contains only
// replay-relative time and the 100 RGB values; CAN identity and payload data
// never cross this output boundary.
class JsonlPixelFrameSink final : public local_argb::PixelFrameSink {
public:
  JsonlPixelFrameSink(const ReplayClock &clock, std::ostream &output) noexcept
      : clock_(&clock), output_(&output) {}

  bool write(const local_argb::PixelFrame &frame) noexcept override;

  [[nodiscard]] bool good() const noexcept { return output_->good(); }

private:
  const ReplayClock *clock_;
  std::ostream *output_;
};

} // namespace gvret
