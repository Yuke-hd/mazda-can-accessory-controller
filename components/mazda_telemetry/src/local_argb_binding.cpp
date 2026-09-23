#include "mazda/accessory_telemetry.hpp"

#include "local_argb/lighting_sink.hpp"
#include "mazda/internal_contracts.hpp"
#include "mazda/vehicle_telemetry_internal.hpp"

namespace mazda::application {
namespace {

class LocalArgbLightingSink final : public internal::LightingSink {
public:
  [[nodiscard]] bool publish(const LightingUpdate &update) noexcept override {
    local_argb::internal::LightingCommand command{};
    command.left_turn = update.turn == TurnState::Right || update.turn == TurnState::Hazard;
    command.right_turn = update.turn == TurnState::Left || update.turn == TurnState::Hazard;
    // Brake freshness is intentionally unset until vehicle timing evidence is
    // established. Treat an unverified sample as fail-off rather than holding
    // the brake region on indefinitely.
    command.brake = update.brake_pressed && update.brake_availability == Availability::Fresh;
    command.valid_until_us = update.valid_until_us;
    command.actionable = command.left_turn || command.right_turn || command.brake;
    return local_argb::internal::sink().publish(command);
  }
};

// The binding is process-wide because the ESP-IDF renderer is process-wide.
// VehicleTelemetry itself remains non-copyable and owns no renderer handle.
LocalArgbLightingSink g_local_argb_sink{};

} // namespace

StatusResult bind_local_argb_sink(VehicleTelemetry &telemetry) noexcept {
  return internal::VehicleTelemetryAccess::bind_lighting_sink(telemetry, g_local_argb_sink);
}

} // namespace mazda::application
