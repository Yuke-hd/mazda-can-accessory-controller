#pragma once

#include "mazda/reading.hpp"
#include "mazda/types.hpp"
#include "vehicle_core/time.hpp"
#include "vehicle_lighting_policy/command.hpp"

namespace vehicle_lighting_policy {

using Rgb = LightingRgb;

inline constexpr std::uint8_t kBrightnessCeiling = 16;

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

// Publication change/heartbeat gating is owned solely by the telemetry
// service's private lighting publication (VehicleTelemetryService); this
// module only maps one reading to a value-only command.

} // namespace vehicle_lighting_policy
