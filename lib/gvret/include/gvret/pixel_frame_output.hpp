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

// Version of the self-describing JSONL stream. Consumers ignore unknown record
// types, so adding a record type does not require a version bump.
inline constexpr unsigned kJsonlFormatVersion = 1;

// Writes a self-describing JSON-lines stream. The first line is a header
// record declaring the format version and pixel count; each renderer write
// then becomes one "pixels" record. Every record carries a "type" field. The
// stream contains only replay-relative time and RGB values; CAN identity and
// payload data never cross this output boundary.
class JsonlPixelFrameSink final : public local_argb::PixelFrameSink {
public:
  JsonlPixelFrameSink(const ReplayClock &clock, std::ostream &output) noexcept
      : clock_(&clock), output_(&output) {}

  // Writes the header record once. write() calls it implicitly; call it
  // directly to make a stream with no pixel records self-describing.
  bool write_header() noexcept;

  bool write(const local_argb::PixelFrame &frame) noexcept override;

  [[nodiscard]] bool good() const noexcept { return output_->good(); }

private:
  const ReplayClock *clock_;
  std::ostream *output_;
  bool wrote_header_{false};
};

} // namespace gvret
