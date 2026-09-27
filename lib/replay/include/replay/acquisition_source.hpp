#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

#include "gvret/replay_stream.hpp"
#include "replay/clock.hpp"
#include "vehicle_telemetry/receive.hpp"

namespace replay {

// Adapts a prepared replay sequence to the production acquisition boundary.
// The caller owns clock advancement; receive() never skips scheduled work by
// moving time itself. A future frame and end of stream both return Timeout.
// The source is one-shot because its shared monotonic clock cannot rewind on
// restart without changing frame timestamps.
class ReplayAcquisitionSource final : public vehicle_telemetry::AcquisitionSource {
public:
  ReplayAcquisitionSource(std::vector<gvret::TimedCanFrame> frames, ReplayClock &clock);

  [[nodiscard]] vehicle_telemetry::StatusResult start() noexcept override;
  [[nodiscard]] vehicle_telemetry::StatusResult stop() noexcept override;
  [[nodiscard]] vehicle_telemetry::ReceiveStatus
  receive(vehicle_core::RawCanFrame &frame, std::uint32_t timeout_ms) noexcept override;
  [[nodiscard]] vehicle_telemetry::AcquisitionStatistics statistics() const noexcept override;

  // Distinguishes EOF from a future scheduled frame after receive() times out.
  [[nodiscard]] bool end_of_stream() const noexcept;
  // Allows a scheduler to run earlier work before advancing replay time.
  [[nodiscard]] std::optional<vehicle_core::MonotonicTimestamp> next_frame_time() const noexcept;

private:
  const std::vector<gvret::TimedCanFrame> frames_;
  ReplayClock &clock_;
  mutable std::mutex mutex_;
  std::size_t next_frame_{0};
  vehicle_telemetry::AcquisitionStatistics statistics_{};
  bool running_{false};
  bool started_once_{false};
};

} // namespace replay
