#pragma once

#include <optional>

#include "vehicle_core/time.hpp"

namespace mazda {

constexpr vehicle_core::Microseconds kTurnFreshnessTimeoutUs = 250'000;
constexpr vehicle_core::Microseconds kRequestFreshnessTimeoutUs = 250'000;

// Only turn/request freshness has a confirmed default. The supplied DBC has
// no cycle-time declarations, so the remaining policies stay unconfigured.
struct VehicleFreshnessPolicy {
  std::optional<vehicle_core::Microseconds> speed_kph_timeout_us{};
  std::optional<vehicle_core::Microseconds> engine_rpm_timeout_us{};
  std::optional<vehicle_core::Microseconds> selector_position_timeout_us{};
  std::optional<vehicle_core::Microseconds> actual_gear_timeout_us{};
  std::optional<vehicle_core::Microseconds> turn_state_timeout_us{kTurnFreshnessTimeoutUs};
  std::optional<vehicle_core::Microseconds> hazard_request_timeout_us{kRequestFreshnessTimeoutUs};
  std::optional<vehicle_core::Microseconds> left_turn_request_timeout_us{
      kRequestFreshnessTimeoutUs};
  std::optional<vehicle_core::Microseconds> right_turn_request_timeout_us{
      kRequestFreshnessTimeoutUs};
  std::optional<vehicle_core::Microseconds> brake_pressed_timeout_us{};
  std::optional<vehicle_core::Microseconds> liftgate_open_timeout_us{};
  std::optional<vehicle_core::Microseconds> rear_right_door_open_timeout_us{};
  std::optional<vehicle_core::Microseconds> rear_left_door_open_timeout_us{};
  std::optional<vehicle_core::Microseconds> front_left_door_open_rhd_timeout_us{};
  std::optional<vehicle_core::Microseconds> front_right_door_open_rhd_timeout_us{};
  std::optional<vehicle_core::Microseconds> doors_unlocked_timeout_us{};
  std::optional<vehicle_core::Microseconds> left_indicator_lamp_timeout_us{};
  std::optional<vehicle_core::Microseconds> right_indicator_lamp_timeout_us{};
  std::optional<vehicle_core::Microseconds> wiper_low_timeout_us{};
  std::optional<vehicle_core::Microseconds> front_wiper_timeout_us{};
};

} // namespace mazda

// Stage 0 exposed this policy through vehicle_core. Keep that qualified name
// available when the Mazda policy header is present without duplicating the
// definition or making vehicle_core include Mazda headers.
namespace vehicle_core {
using VehicleFreshnessPolicy = ::mazda::VehicleFreshnessPolicy;
}
