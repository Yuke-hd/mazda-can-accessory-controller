#pragma once

#include <cstddef>
#include <mutex>
#include <optional>
#include <string_view>

#include "mazda/signal_provider.hpp"
#include "vehicle_signals/signal_catalog.hpp"
#include "vehicle_signals/signal_contracts.hpp"

// A minimal generic consumer written only against the public signal provider
// and the portable vehicle_signals contracts. It knows canonical keys and
// enum choice keys, never make-specific types, CAN identifiers, descriptors or
// notification channels. architecture_contracts checks that this module
// includes nothing else, and its build target has no private include path.

namespace generic_signal_consumer {

// Canonical keys are the persistent identities; numeric SignalIds are resolved
// through the catalog for the running build.
inline constexpr std::string_view kEngineRpmKey{"vehicle.engine_rpm"};
inline constexpr std::string_view kTurnStateKey{"vehicle.turn_state"};

// Choice key of an Enum reading (for example "left"), or empty when the reading
// has no enum value or the signal lists no matching choice.
[[nodiscard]] std::string_view choice_key(const vehicle_signals::SignalMetadata &signal,
                                          const vehicle_signals::SignalReading &reading) noexcept;

// Resolves engine RPM for reads and subscribes to turn state. attach() and
// detach() are provider subscription mutations: call them on the facade's
// lifecycle owner while it is stopped. The facade owns the registration and
// the consumer is its callback context: keep the consumer alive until a
// successful facade stop, and detach() before destroying it unless the facade
// is destroyed first.
class GenericSignalConsumer final {
public:
  explicit GenericSignalConsumer(mazda::MazdaSignalProvider &provider) noexcept;

  GenericSignalConsumer(const GenericSignalConsumer &) = delete;
  GenericSignalConsumer &operator=(const GenericSignalConsumer &) = delete;
  GenericSignalConsumer(GenericSignalConsumer &&) = delete;
  GenericSignalConsumer &operator=(GenericSignalConsumer &&) = delete;

  [[nodiscard]] vehicle_signals::SignalStatus attach() noexcept;
  [[nodiscard]] vehicle_signals::SignalStatus detach() noexcept;

  [[nodiscard]] const vehicle_signals::SignalMetadata *engine_rpm_signal() const noexcept {
    return engine_rpm_;
  }
  [[nodiscard]] const vehicle_signals::SignalMetadata *turn_state_signal() const noexcept {
    return turn_state_;
  }

  [[nodiscard]] vehicle_signals::SignalResult<vehicle_signals::SignalReading>
  read_engine_rpm() const noexcept;

  // Latest turn-state notice delivered to this consumer, if any.
  [[nodiscard]] std::optional<vehicle_signals::SignalNotification> latest_turn_notice() const;
  [[nodiscard]] std::size_t turn_notice_count() const;
  // Choice key of a turn-state reading, for example "left" or "hazard".
  [[nodiscard]] std::string_view
  turn_choice(const vehicle_signals::SignalReading &reading) const noexcept;

private:
  static void on_turn_state(void *context,
                            const vehicle_signals::SignalNotification &notice) noexcept;

  mazda::MazdaSignalProvider *provider_{nullptr};
  const vehicle_signals::SignalMetadata *engine_rpm_{nullptr};
  const vehicle_signals::SignalMetadata *turn_state_{nullptr};
  vehicle_signals::SignalSubscription turn_subscription_{};

  mutable std::mutex mutex_{};
  vehicle_signals::SignalNotification latest_turn_{};
  std::size_t turn_notices_{0};
};

} // namespace generic_signal_consumer
