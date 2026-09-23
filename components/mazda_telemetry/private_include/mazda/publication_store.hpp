#pragma once

#include <mutex>
#include <optional>

#include "mazda/availability.hpp"
#include "mazda/definitions.hpp"
#include "mazda/facade_contracts.hpp"
#include "mazda/state.hpp"

namespace mazda::internal {

// This clock is deliberately implementation-only. The public facade does not
// expose a time source, and host tests inject their deterministic clock into
// PublicationStore directly.
class SteadyClock final : public vehicle_core::MonotonicClock {
public:
  [[nodiscard]] vehicle_core::MonotonicTimestamp now() const noexcept override;
};

struct PublishedSnapshot {
  VehicleState state{};
  Diagnostics diagnostics{};
};

// Owns the fixed, coherent publication boundary between the service task and
// polling callers. The service task publishes a value-copy state and
// diagnostics together; readers copy the publication and its evaluation
// policy while holding the mutex, then sample the clock and evaluate
// freshness outside the critical section. No callback, driver, clock or
// logging operation runs under the mutex.
class PublicationStore final {
public:
  PublicationStore() noexcept;
  explicit PublicationStore(vehicle_core::MonotonicClock &clock,
                            TelemetryConfig config = {}) noexcept;

  PublicationStore(const PublicationStore &) = delete;
  PublicationStore &operator=(const PublicationStore &) = delete;

  // Configuration is accepted only while the published lifecycle is stopped.
  // Applying the policy to the stored copy does not mutate the service-owned
  // state supplied to publish().
  [[nodiscard]] StatusResult configure(const TelemetryConfig &config) noexcept;

  // Publish and reset are the private lifecycle handoff used by S2-A. Both
  // operations replace fixed storage under one short critical section. Reset
  // intentionally clears old-run samples before a restart.
  // Pass the receive watermark for every acquired frame, including frames the
  // decoder ignores. Callers must pass std::nullopt when no frame was
  // acquired; there is intentionally no overload without this basis. The
  // optional is private because transport receive time is not part of a
  // public Reading value.
  void publish(const VehicleState &state, const Diagnostics &diagnostics,
               std::optional<vehicle_core::MonotonicTimestamp> last_transport_receive_us) noexcept;
  void publish(const VehicleState &state, LifecycleState lifecycle,
               vehicle_core::TransportHealth transport, const AcquisitionMetrics &acquisition,
               std::optional<vehicle_core::MonotonicTimestamp> last_transport_receive_us) noexcept;
  void reset(const Diagnostics &diagnostics = {}) noexcept;

  [[nodiscard]] Reading<float> speed_kph() const noexcept;
  [[nodiscard]] Reading<float> engine_rpm() const noexcept;
  // Test-only descriptor seam. It exercises the same bounded synchronized
  // read path for an additional state member without adding a public facade
  // channel or another synchronization loop.
  template <typename T>
  [[nodiscard]] Reading<T>
  read_test_signal(vehicle_core::Signal<T> VehicleState::*member, std::uint32_t identifier,
                   ValidationStatus validation = ValidationStatus::Reference) const noexcept {
    return read_signal(member, identifier, validation);
  }
  [[nodiscard]] Diagnostics diagnostics() const noexcept;
  [[nodiscard]] PublishedSnapshot snapshot() const noexcept;

private:
  template <typename T>
  [[nodiscard]] Reading<T> read_signal(vehicle_core::Signal<T> VehicleState::*member,
                                       std::uint32_t identifier,
                                       ValidationStatus validation) const noexcept;

  SteadyClock steady_clock_{};
  vehicle_core::MonotonicClock *clock_{nullptr};
  TelemetryConfig config_{};
  PublishedSnapshot published_{};
  mutable std::mutex mutex_{};
};

template <typename T>
Reading<T> PublicationStore::read_signal(vehicle_core::Signal<T> VehicleState::*member,
                                         const std::uint32_t identifier,
                                         const ValidationStatus validation) const noexcept {
  // Copy all publication-dependent inputs from one coherent handoff while
  // holding the lock. The clock is sampled only after unlocking so a blocked
  // injected clock cannot hold up a publisher or be paired with a later
  // publication.
  vehicle_core::Signal<T> signal{};
  vehicle_core::MessageHealth message = vehicle_core::MessageHealth::Unknown;
  vehicle_core::TransportHealth transport = vehicle_core::TransportHealth::Stopped;
  {
    std::lock_guard<std::mutex> lock{mutex_};
    signal = published_.state.*member;
    transport = published_.diagnostics.transport;
    const auto *health = published_.state.message_health_for(identifier);
    if (health != nullptr)
      message = health->health;
  }

  const auto now_us = clock_->now();
  return mazda::snapshot(signal, now_us, validation, message, transport);
}

} // namespace mazda::internal
