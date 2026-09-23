#pragma once

// Mazda-facing aliases for the frozen, value-copy availability contract.
// Definitions remain in vehicle_core so generic consumers can reason about
// data availability without depending on Mazda decoders or state storage.
#include "mazda/notification.hpp"
#include "vehicle_core/decoder_contracts.hpp"
#include "vehicle_core/health.hpp"
#include "vehicle_core/signal.hpp"
#include "vehicle_core/time.hpp"

namespace mazda {

using vehicle_core::Availability;
using vehicle_core::HealthObservation;
using vehicle_core::Notification;
using vehicle_core::Reading;
using vehicle_core::ValidationStatus;

// Evaluate one signal without changing the signal or any state that owns it.
// now values earlier than the accepted sample are clamped to age zero. A
// signal with no configured timeout is deliberately unverified rather than
// being assigned a made-up freshness deadline.
template <typename T>
[[nodiscard]] inline Availability status_at(
    const vehicle_core::Signal<T> &signal, const vehicle_core::MonotonicTimestamp now_us,
    const vehicle_core::MessageHealth message = vehicle_core::MessageHealth::Healthy,
    const vehicle_core::TransportHealth transport = vehicle_core::TransportHealth::Live) noexcept {
  if (transport == vehicle_core::TransportHealth::Stopped ||
      transport == vehicle_core::TransportHealth::Faulted ||
      transport == vehicle_core::TransportHealth::TimedOut) {
    return Availability::Unavailable;
  }
  if (!signal.has_value) {
    return Availability::NoData;
  }
  if (message == vehicle_core::MessageHealth::Faulted) {
    return Availability::Unavailable;
  }
  return signal.status_at(now_us);
}

template <typename T>
[[nodiscard]] inline Reading<T> snapshot(
    const vehicle_core::Signal<T> &signal, const vehicle_core::MonotonicTimestamp now_us,
    const ValidationStatus validation = ValidationStatus::Reference,
    const vehicle_core::MessageHealth message = vehicle_core::MessageHealth::Healthy,
    const vehicle_core::TransportHealth transport = vehicle_core::TransportHealth::Live) noexcept {
  Reading<T> result = signal.snapshot(now_us, validation);
  result.availability = status_at(signal, now_us, message, transport);
  return result;
}

template <typename T>
[[nodiscard]] inline Availability status_at(const vehicle_core::Signal<T> &signal,
                                            const vehicle_core::MonotonicTimestamp now_us,
                                            const HealthObservation &health) noexcept {
  if (health.signal == vehicle_core::SignalHealth::Unavailable && signal.has_value) {
    return Availability::Unavailable;
  }
  return status_at(signal, now_us, health.message, health.transport);
}

template <typename T>
[[nodiscard]] inline Reading<T>
snapshot(const vehicle_core::Signal<T> &signal, const vehicle_core::MonotonicTimestamp now_us,
         const HealthObservation &health,
         const ValidationStatus validation = ValidationStatus::Reference) noexcept {
  Reading<T> result = signal.snapshot(now_us, validation);
  result.availability = status_at(signal, now_us, health);
  return result;
}

[[nodiscard]] inline bool is_available(const Availability availability) noexcept {
  return availability == Availability::Fresh || availability == Availability::FreshnessUnverified;
}

} // namespace mazda
