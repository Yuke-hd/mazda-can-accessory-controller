#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "vehicle_core/time.hpp"

namespace local_argb {

// Generic renderer values. No vehicle, transport, board, or RTOS
// type crosses the ordinary local_argb include boundary.
struct Rgb {
  std::uint8_t red{0};
  std::uint8_t green{0};
  std::uint8_t blue{0};
};

constexpr bool operator==(const Rgb &left, const Rgb &right) noexcept {
  return left.red == right.red && left.green == right.green && left.blue == right.blue;
}
constexpr bool operator!=(const Rgb &left, const Rgb &right) noexcept { return !(left == right); }

inline constexpr std::uint8_t kBrightnessCeiling = 16;
inline constexpr Rgb kBlack{};
inline constexpr std::size_t kLedCount = 100;
inline constexpr std::size_t kTurnLedCount = 35;
inline constexpr std::size_t kBrakeLedStart = kTurnLedCount;
inline constexpr std::size_t kBrakeLedCount = 30;
inline constexpr std::size_t kRightTurnLedStart = kBrakeLedStart + kBrakeLedCount;
static_assert(kRightTurnLedStart + kTurnLedCount == kLedCount);

using PixelFrame = std::array<Rgb, kLedCount>;
inline constexpr PixelFrame kBlackFrame{};

// These timing values describe renderer supervision, not vehicle freshness.
// Freshness deadlines are carried by the private LightingCommand handoff.
inline constexpr vehicle_core::Microseconds kDriverHangRestartUs = 100'000;
inline constexpr vehicle_core::Microseconds kWorkerStallRestartUs = 100'000;
inline constexpr vehicle_core::Microseconds kSupervisorPollUs = 10'000;
inline constexpr vehicle_core::Microseconds kDriverRestartRequestBoundUs =
    kDriverHangRestartUs + kSupervisorPollUs;
inline constexpr vehicle_core::Microseconds kWorkerRestartRequestBoundUs =
    kWorkerStallRestartUs + kSupervisorPollUs;

class PixelSink {
public:
  virtual ~PixelSink() = default;
  virtual bool write(Rgb color) noexcept = 0;
};

// Strip-facing sink used by the renderer. PixelSink above remains as a
// compatibility seam for the older single-colour host model.
class PixelFrameSink {
public:
  virtual ~PixelFrameSink() = default;
  virtual bool write(const PixelFrame &frame) noexcept = 0;
};

// ESP-IDF runtime. start() sends an explicit black RMT frame before returning.
bool start() noexcept;
void fail_off() noexcept;

} // namespace local_argb
