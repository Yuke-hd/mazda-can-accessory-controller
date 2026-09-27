#pragma once

#include <vector>

#include "vehicle_core/time.hpp"
#include "vehicle_signals/signal_catalog.hpp"
#include "vehicle_signals/signal_contracts.hpp"

namespace replay {

// A read-only consumer of the replay's decoded vehicle signals. It sees only
// portable vehicle_signals types: no CAN frame, vehicle-specific decoder, or
// output technology crosses this boundary.
//
// Lifecycle, all on the replay controller's host thread:
//   on_catalog(catalog)  once, when the controller starts, before any reading;
//   on_reading(...)      for each notified signal whenever the provider
//                        delivers a notification (including the initial seed
//                        notice at start), and for each polled signal (Read
//                        but not Notify) on every scheduled signal sample.
// `time_us` is the replay clock time of the delivery. `signal` refers to the
// catalog entry, whose storage is static. The reading is passed through
// exactly as the provider produced it: freshness, availability and validation
// are not reinterpreted.
class SignalObserver {
public:
  SignalObserver(const SignalObserver &) = delete;
  SignalObserver &operator=(const SignalObserver &) = delete;
  SignalObserver(SignalObserver &&) = delete;
  SignalObserver &operator=(SignalObserver &&) = delete;

  virtual void on_catalog(vehicle_signals::SignalCatalogView catalog) noexcept = 0;
  virtual void on_reading(vehicle_core::MonotonicTimestamp time_us,
                          const vehicle_signals::SignalMetadata &signal,
                          const vehicle_signals::SignalReading &reading) noexcept = 0;

protected:
  SignalObserver() = default;
  ~SignalObserver() = default;
};

// Caller-owned observers; each must outlive the replay that uses it.
using SignalObservers = std::vector<SignalObserver *>;

} // namespace replay
