#pragma once

#include <cstddef>
#include <cstdint>

#include "mazda/facade_contracts.hpp"

namespace mazda::internal {
class VehicleTelemetryAccess;
}

namespace mazda {

// Application-facing background telemetry facade. Task, driver, decoder, and
// dispatcher ownership remains private to the implementation.
//
// Lifecycle ownership is deliberately single-context: the host thread or
// ESP-IDF task that first mutates a facade (configure, subscription, sink
// binding, start, or stop) becomes its owner. That owner performs all later
// lifecycle mutations, including mutations after a successful stop and
// restart. Polling and diagnostics are safe from any context.
//
// Callbacks receive value-only notification copies. Their context pointer and
// any pointed-to storage must remain valid until stop() returns successfully;
// callback execution can still be in progress while stop() reports Timeout.
// A callback must not call lifecycle-mutating methods on this facade; those
// calls are rejected before state or source changes. A successful stop returns
// only after callbacks and worker tasks are quiescent. If stop() times out or
// fails, keep the facade and callback context alive and retry stop() from the
// lifecycle owner. Destruction requires a successfully stopped, quiescent
// facade.
class VehicleTelemetry final {
public:
  VehicleTelemetry() noexcept;
  ~VehicleTelemetry() noexcept;

  VehicleTelemetry(const VehicleTelemetry &) = delete;
  VehicleTelemetry &operator=(const VehicleTelemetry &) = delete;
  VehicleTelemetry(VehicleTelemetry &&) = delete;
  VehicleTelemetry &operator=(VehicleTelemetry &&) = delete;

  [[nodiscard]] StatusResult configure(const TelemetryConfig &config) noexcept;
  [[nodiscard]] StatusResult start() noexcept;
  [[nodiscard]] StatusResult stop() noexcept;

  // Polling copies the latest accepted state. It never requests a CAN frame
  // and does not consume or acknowledge a sample.
  [[nodiscard]] Reading<float> speed_kph() const noexcept;
  [[nodiscard]] Reading<float> engine_rpm() const noexcept;

  [[nodiscard]] Result<Subscription>
  on_selector_position_changed(Callback<SelectorPosition> callback, void *context) noexcept;
  [[nodiscard]] Result<Subscription> on_actual_gear_changed(Callback<ActualGear> callback,
                                                            void *context) noexcept;
  [[nodiscard]] Result<Subscription> on_turn_state_changed(Callback<TurnState> callback,
                                                           void *context) noexcept;
  [[nodiscard]] Result<Subscription> on_hazard_request_changed(Callback<bool> callback,
                                                               void *context) noexcept;
  [[nodiscard]] Result<Subscription> on_left_turn_request_changed(Callback<bool> callback,
                                                                  void *context) noexcept;
  [[nodiscard]] Result<Subscription> on_right_turn_request_changed(Callback<bool> callback,
                                                                   void *context) noexcept;
  [[nodiscard]] Result<Subscription> on_liftgate_open_changed(Callback<bool> callback,
                                                              void *context) noexcept;
  [[nodiscard]] Result<Subscription> on_rear_right_door_open_changed(Callback<bool> callback,
                                                                     void *context) noexcept;
  [[nodiscard]] Result<Subscription> on_rear_left_door_open_changed(Callback<bool> callback,
                                                                    void *context) noexcept;
  [[nodiscard]] Result<Subscription> on_front_left_door_open_rhd_changed(Callback<bool> callback,
                                                                         void *context) noexcept;
  [[nodiscard]] Result<Subscription> on_front_right_door_open_rhd_changed(Callback<bool> callback,
                                                                          void *context) noexcept;
  [[nodiscard]] Result<Subscription> on_doors_unlocked_changed(Callback<bool> callback,
                                                               void *context) noexcept;
  [[nodiscard]] Result<Subscription> on_left_indicator_lamp_changed(Callback<bool> callback,
                                                                    void *context) noexcept;
  [[nodiscard]] Result<Subscription> on_right_indicator_lamp_changed(Callback<bool> callback,
                                                                     void *context) noexcept;
  [[nodiscard]] Result<Subscription> on_wiper_low_changed(Callback<bool> callback,
                                                          void *context) noexcept;
  [[nodiscard]] Result<Subscription> on_front_wiper_changed(Callback<FrontWiperPosition> callback,
                                                            void *context) noexcept;

  [[nodiscard]] StatusResult unsubscribe(Subscription subscription) noexcept;
  [[nodiscard]] Diagnostics diagnostics() const noexcept;

private:
  friend class internal::VehicleTelemetryAccess;

  // Keep synchronization, state copies and the clock seam out of the public
  // include graph. The implementation owns this fixed storage with placement
  // construction, so constructing the facade performs no heap allocation.
  // The service keeps all notification channels, worker state and the
  // publication store in one fixed opaque allocation. It remains private so
  // task/lock/decoder types never enter the public include graph.
  static constexpr std::size_t kImplementationStorageBytes = 32768;
  alignas(std::max_align_t) std::byte implementation_storage_[kImplementationStorageBytes]{};
};

} // namespace mazda
