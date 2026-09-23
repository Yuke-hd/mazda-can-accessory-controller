#pragma once

#include "mazda/reading.hpp"
#include "mazda/types.hpp"
#include "vehicle_core/time.hpp"
#include "vehicle_lighting_policy/command.hpp"

namespace vehicle_lighting_policy {

using Rgb = LightingRgb;

inline constexpr std::uint8_t kBrightnessCeiling = 16;
inline constexpr vehicle_core::Microseconds kHeartbeatUs = 100'000;

inline constexpr LightingRgb kBlack{};
inline constexpr LightingRgb kLeftGreen{0, kBrightnessCeiling, 0};
inline constexpr LightingRgb kRightBlue{0, 0, kBrightnessCeiling};
inline constexpr LightingRgb kHazardAmber{kBrightnessCeiling, kBrightnessCeiling / 2, 0};

// Deadlines are supplied by the service because the policy must not inspect
// decoder state or invent a transport timeout.  A zero deadline means that
// dimension has no independent expiry; when both are absent no command is
// actionable until the service supplies at least one finite deadline.
struct TurnInput {
  mazda::Reading<mazda::TurnState> turn{};
  vehicle_core::MonotonicTimestamp signal_valid_until_us{0};
  vehicle_core::MonotonicTimestamp transport_valid_until_us{0};
};

// Convert one typed turn reading into a minimal solid-colour command.  Only a
// Fresh or FreshnessUnverified reading with a representable actionable state
// can produce colour; unavailable, conflicting, unknown and off readings map
// to black.  The command deadline is the earliest supplied signal/transport
// deadline, so a heartbeat cannot extend an accepted sample's life.
[[nodiscard]] LightingCommand command_for(const TurnInput &input) noexcept;

[[nodiscard]] inline LightingCommand
command_for(const mazda::Reading<mazda::TurnState> &turn,
            vehicle_core::MonotonicTimestamp signal_valid_until_us,
            vehicle_core::MonotonicTimestamp transport_valid_until_us) noexcept {
  return command_for(TurnInput{turn, signal_valid_until_us, transport_valid_until_us});
}

[[nodiscard]] inline LightingCommand evaluate(const TurnInput &input) noexcept {
  return command_for(input);
}

// A bounded change/heartbeat gate for the private sink.  Every call records
// the newest input, but returns true only for a semantic transition or the
// bounded private heartbeat.  Thus repeated same-turn frames refresh the
// private command at a bounded cadence without creating public callbacks.
class PublicationPolicy {
public:
  [[nodiscard]] bool should_publish(TurnInput input,
                                    vehicle_core::MonotonicTimestamp now_us) noexcept;

  [[nodiscard]] LightingCommand command() const noexcept { return command_for(latest_); }
  [[nodiscard]] bool has_input() const noexcept { return has_input_; }

private:
  TurnInput latest_{};
  TurnInput published_{};
  vehicle_core::MonotonicTimestamp last_publish_us_{0};
  bool has_input_{false};
  bool has_published_{false};
};

} // namespace vehicle_lighting_policy
