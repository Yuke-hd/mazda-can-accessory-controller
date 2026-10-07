#include "mazda/publication_store.hpp"

namespace mazda::internal {

PublicationStore::PublicationStore() noexcept : clock_(&steady_clock_) {}

PublicationStore::PublicationStore(vehicle_core::MonotonicClock &clock,
                                   TelemetryConfig config) noexcept
    : clock_(&clock), config_(config) {}

StatusResult PublicationStore::configure(const TelemetryConfig &config) noexcept {
  std::lock_guard<std::mutex> lock{mutex_};
  if (published_.diagnostics.lifecycle != LifecycleState::Stopped) {
    return StatusResult{ResultCode::InvalidState};
  }
  config_ = config;
  published_.state.apply_freshness_policy(config_.freshness);
  return StatusResult{ResultCode::Ok};
}

void PublicationStore::publish(
    const VehicleState &state, const Diagnostics &diagnostics,
    const std::optional<vehicle_core::MonotonicTimestamp> last_transport_receive_us) noexcept {
  std::lock_guard<std::mutex> lock{mutex_};
  published_.state = state;
  published_.diagnostics = diagnostics;
  (void)last_transport_receive_us;
}

void PublicationStore::publish(
    const VehicleState &state, const LifecycleState lifecycle,
    const vehicle_core::TransportHealth transport, const AcquisitionMetrics &acquisition,
    const std::optional<vehicle_core::MonotonicTimestamp> last_transport_receive_us) noexcept {
  Diagnostics diagnostics{};
  diagnostics.lifecycle = lifecycle;
  diagnostics.transport = transport;
  diagnostics.acquisition = acquisition;
  publish(state, diagnostics, last_transport_receive_us);
}

void PublicationStore::publish_diagnostics(const Diagnostics &diagnostics) noexcept {
  std::lock_guard<std::mutex> lock{mutex_};
  published_.diagnostics = diagnostics;
}

void PublicationStore::reset(const Diagnostics &diagnostics) noexcept {
  const auto reset_time_us = clock_->now();
  std::lock_guard<std::mutex> lock{mutex_};
  published_.state = VehicleState{};
  published_.state.apply_freshness_policy(config_.freshness);
  published_.diagnostics = diagnostics;
  (void)reset_time_us;
}

Reading<float> PublicationStore::speed_kph() const noexcept {
  return read_descriptor_signal(&VehicleState::speed_kph, candidate::kEngineDataId,
                                ValidationStatus::Reference);
}

Reading<float> PublicationStore::engine_rpm() const noexcept {
  return read_descriptor_signal(&VehicleState::engine_rpm, candidate::kEngineDataId,
                                ValidationStatus::Confirmed);
}

Diagnostics PublicationStore::diagnostics() const noexcept {
  Diagnostics result{};
  {
    std::lock_guard<std::mutex> lock{mutex_};
    result = published_.diagnostics;
  }
  return result;
}

PublishedSnapshot PublicationStore::snapshot() const noexcept {
  PublishedSnapshot result{};
  {
    std::lock_guard<std::mutex> lock{mutex_};
    result = published_;
  }
  return result;
}

} // namespace mazda::internal
