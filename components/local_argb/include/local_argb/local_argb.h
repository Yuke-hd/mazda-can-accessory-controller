#pragma once

#include <cstddef>
#include <cstdint>

#include "local_argb/pixel_frame.hpp"
#include "vehicle_core/time.hpp"

namespace local_argb {

// Generic renderer values. No vehicle, transport, board, or RTOS
// type crosses the ordinary local_argb include boundary. Rgb, kBlack,
// kLedCount, PixelFrame, kBlackFrame, and PixelFrameSink come from the renderer-
// independent frame contract in local_argb/pixel_frame.hpp.
inline constexpr std::uint8_t kBrightnessCeiling = 16;
inline constexpr std::size_t kTurnLedCount = 35;
inline constexpr std::size_t kBrakeLedStart = kTurnLedCount;
inline constexpr std::size_t kBrakeLedCount = 30;
inline constexpr std::size_t kRightTurnLedStart = kBrakeLedStart + kBrakeLedCount;
static_assert(kRightTurnLedStart + kTurnLedCount == kLedCount);

// These timing values describe renderer supervision, not vehicle freshness.
// Freshness deadlines are carried by the private LightingCommand handoff.
inline constexpr vehicle_core::Microseconds kDriverHangRestartUs = 100'000;
inline constexpr vehicle_core::Microseconds kWorkerStallRestartUs = 100'000;
inline constexpr vehicle_core::Microseconds kSupervisorPollUs = 10'000;
inline constexpr vehicle_core::Microseconds kDriverRestartRequestBoundUs =
    kDriverHangRestartUs + kSupervisorPollUs;
inline constexpr vehicle_core::Microseconds kWorkerRestartRequestBoundUs =
    kWorkerStallRestartUs + kSupervisorPollUs;
// A watched progress count (see watch_progress) that stops for longer than
// this fails the strip off. The supervisor samples it every poll, so it sees
// the last change up to one poll late and the stall up to one poll late.
// kProgressFailOffBoundUs bounds when black is commanded (queued past the
// closed gate); the worker renders it on its next pass, a few ms later.
inline constexpr vehicle_core::Microseconds kProgressStallFailOffUs = 2'000'000;
inline constexpr vehicle_core::Microseconds kProgressFailOffBoundUs =
    kProgressStallFailOffUs + 2 * kSupervisorPollUs;
// The longest fail_off_for_restart() waits for the worker to write black: a
// driver write that takes longer is a hang the supervisor already resets.
inline constexpr vehicle_core::Microseconds kRestartFailOffWaitUs =
    kDriverHangRestartUs + 2 * kSupervisorPollUs;

// Compatibility seam for the older single-colour host model; the renderer
// writes whole frames to PixelFrameSink.
class PixelSink {
public:
  virtual ~PixelSink() = default;
  virtual bool write(Rgb color) noexcept = 0;
};

// Reads a wrapping count that a monitored loop advances on every pass.
// It is called from the renderer supervisor task and must not block.
using ProgressProbe = std::uint32_t (*)(const void *context) noexcept;

// ESP-IDF runtime. start() sends an explicit black RMT frame before returning.
bool start() noexcept;
void fail_off() noexcept;
// Watches one progress count from the renderer supervisor after start(). If
// the count stops changing for kProgressStallFailOffUs, the strip fails off
// and lighting commands are rejected. Once the count changes again, the next
// command lights the strip; nothing is re-emitted. Returns false before
// start(), for a null probe, or when a probe is already watched.
bool watch_progress(ProgressProbe probe, const void *context) noexcept;
// Fails the strip off for a controlled restart. Closes the lighting gate for
// good, so no publisher can relight the strip and a progress resume cannot
// reopen it, then queues black and waits up to `timeout_us` for the worker
// to write it. Never calls the LED driver itself, so any task may call it.
// Returns true once black is written; false before start(), when the queue
// rejects black or on timeout. The caller restarts either way.
bool fail_off_for_restart(vehicle_core::Microseconds timeout_us) noexcept;

} // namespace local_argb
