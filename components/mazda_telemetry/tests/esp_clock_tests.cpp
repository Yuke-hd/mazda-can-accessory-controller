#include "mazda/publication_store.hpp"

#include <cstdint>
#include <iostream>

namespace {

std::int64_t esp_timer_value_us = 0;

} // namespace

extern "C" std::int64_t esp_timer_get_time(void) { return esp_timer_value_us; }

int main() {
  mazda::internal::SteadyClock clock;

  esp_timer_value_us = 123'456'789;
  if (clock.now() != 123'456'789U) {
    std::cerr << "ESP SteadyClock did not use esp_timer_get_time microseconds\n";
    return 1;
  }

  // A second sample proves this is a direct production-clock read rather than
  // a cached or independently converted host clock value.
  esp_timer_value_us = 123'456'790;
  if (clock.now() != 123'456'790U) {
    std::cerr << "ESP SteadyClock did not follow the timer domain\n";
    return 1;
  }

  // Use the default PublicationStore wiring with the same production clock
  // object. A source observation at the inclusive freshness boundary must
  // remain fresh, while the next microsecond must become stale.
  mazda::TelemetryConfig config{};
  config.freshness.speed_kph_timeout_us = 100;
  mazda::internal::PublicationStore store;
  if (!store.configure(config).ok()) {
    std::cerr << "default PublicationStore clock configuration failed\n";
    return 1;
  }

  constexpr vehicle_core::MonotonicTimestamp observation_us = 123'456'700;
  mazda::VehicleState state{};
  state.speed_kph.update(42.0F, observation_us);
  mazda::Diagnostics diagnostics{};
  diagnostics.lifecycle = mazda::LifecycleState::Running;
  diagnostics.transport = vehicle_core::TransportHealth::Live;
  store.publish(state, diagnostics, observation_us);

  esp_timer_value_us = observation_us + 100;
  const auto boundary = store.speed_kph();
  if (!boundary.value.has_value() || *boundary.value != 42.0F ||
      boundary.availability != mazda::Availability::Fresh) {
    std::cerr << "ESP clock freshness boundary did not remain inclusive\n";
    return 1;
  }

  esp_timer_value_us = observation_us + 101;
  const auto expired = store.speed_kph();
  if (!expired.value.has_value() || *expired.value != 42.0F ||
      expired.availability != mazda::Availability::Stale) {
    std::cerr << "ESP clock freshness did not expire after the boundary\n";
    return 1;
  }

  const auto snapshot = store.snapshot();
  if (snapshot.state.brake_pressed.freshness_timeout_us.has_value()) {
    std::cerr << "default brake freshness unexpectedly became configured\n";
    return 1;
  }

  return 0;
}
