#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include "mazda/availability.hpp"
#include "mazda/freshness.hpp"
#include "mazda/types.hpp"
#include "vehicle_core/decoder_contracts.hpp"
#include "vehicle_core/frame.hpp"
#include "vehicle_core/signal.hpp"

namespace mazda {

// A fixed-size, value-copy record for one decoder-owned message. It is kept
// beside VehicleState so decoding can reject older/conflicting observations
// without allocating or coupling the portable core to Mazda identifiers.
struct MessageHealthState {
  std::uint32_t identifier{0};
  bool has_frame{false};
  vehicle_core::MonotonicTimestamp last_frame_us{0};
  bool has_accepted{false};
  vehicle_core::MonotonicTimestamp last_accepted_us{0};
  vehicle_core::MessageHealth health{vehicle_core::MessageHealth::Unknown};
  std::optional<vehicle_core::MonotonicTimestamp> fault_timestamp_us{};
  std::uint8_t bus_id{0};
  std::uint8_t dlc{0};
  vehicle_core::CanIdentifierFormat identifier_format{vehicle_core::CanIdentifierFormat::Standard};
  bool remote_request{false};
  std::array<std::uint8_t, vehicle_core::kCanClassicPayloadBytes> data{};
};

enum class MessageObservationResult : std::uint8_t {
  Ignored,
  Accepted,
  Idempotent,
  RejectedOlder,
  RejectedConflict,
  CapacityExceeded,
};

inline constexpr std::size_t kTrackedMessageCapacity = 6;

struct VehicleState {
  vehicle_core::MonotonicTimestamp timestamp_us{0};
  vehicle_core::Signal<float> speed_kph{vehicle_core::SignalUnit::KilometresPerHour};
  vehicle_core::Signal<float> engine_rpm{vehicle_core::SignalUnit::RevolutionsPerMinute};
  vehicle_core::Signal<SelectorPosition> selector_position{};
  vehicle_core::Signal<ActualGear> actual_gear{};
  vehicle_core::Signal<TurnState> turn_state{vehicle_core::SignalUnit::None,
                                             kTurnFreshnessTimeoutUs};
  vehicle_core::Signal<bool> hazard_request{vehicle_core::SignalUnit::Boolean,
                                            kRequestFreshnessTimeoutUs};
  vehicle_core::Signal<bool> left_turn_request{vehicle_core::SignalUnit::Boolean,
                                               kRequestFreshnessTimeoutUs};
  vehicle_core::Signal<bool> right_turn_request{vehicle_core::SignalUnit::Boolean,
                                                kRequestFreshnessTimeoutUs};
  // Brake timing remains unverified until vehicle evidence establishes a
  // freshness timeout; the accessory binding therefore fails off by default.
  vehicle_core::Signal<bool> brake_pressed{vehicle_core::SignalUnit::Boolean};
  vehicle_core::Signal<bool> liftgate_open{vehicle_core::SignalUnit::Boolean};
  vehicle_core::Signal<bool> rear_right_door_open{vehicle_core::SignalUnit::Boolean};
  vehicle_core::Signal<bool> rear_left_door_open{vehicle_core::SignalUnit::Boolean};
  vehicle_core::Signal<bool> front_left_door_open_rhd{vehicle_core::SignalUnit::Boolean};
  vehicle_core::Signal<bool> front_right_door_open_rhd{vehicle_core::SignalUnit::Boolean};
  vehicle_core::Signal<bool> doors_unlocked{vehicle_core::SignalUnit::Boolean};
  vehicle_core::Signal<bool> left_indicator_lamp{vehicle_core::SignalUnit::Boolean};
  vehicle_core::Signal<bool> right_indicator_lamp{vehicle_core::SignalUnit::Boolean};
  vehicle_core::Signal<bool> wiper_low{vehicle_core::SignalUnit::Boolean};
  vehicle_core::Signal<FrontWiperPosition> front_wiper{};

  // One fixed record per currently decoded Mazda message. This is copied by
  // snapshots and never owns transport or task resources.
  std::array<MessageHealthState, kTrackedMessageCapacity> message_health{};

  // Apply a semantic turn state and return an edge only when the state
  // changed. The first known state has Unknown as its previous state.
  std::optional<TurnEdgeEvent> update_turn(TurnState state,
                                           vehicle_core::MonotonicTimestamp timestamp_us) noexcept;

  // Unknown and stale turn values are never actionable.
  [[nodiscard]] TurnState effective_turn_state() const noexcept {
    return turn_state.is_valid() ? turn_state.value : TurnState::Unknown;
  }

  [[nodiscard]] VehicleState snapshot(vehicle_core::MonotonicTimestamp now_us) const noexcept;
  [[nodiscard]] VehicleState snapshot(vehicle_core::MonotonicTimestamp now_us,
                                      const VehicleFreshnessPolicy &policy) const noexcept;

  void refresh(vehicle_core::MonotonicTimestamp now_us) noexcept;
  void apply_freshness_policy(const VehicleFreshnessPolicy &policy) noexcept;

  // Record decoder validity and apply timestamp-watermark rules. Ignored
  // frames are not passed here. Equal or older observations are no-ops for
  // state; only a strictly newer Decoded frame can clear a malformed fault.
  [[nodiscard]] MessageObservationResult
  observe_message(const vehicle_core::RawCanFrame &frame,
                  vehicle_core::DecodeValidity validity) noexcept;

  [[nodiscard]] const MessageHealthState *
  message_health_for(std::uint32_t identifier) const noexcept;

  [[nodiscard]] vehicle_core::HealthObservation health_observation(
      std::uint32_t identifier,
      vehicle_core::TransportHealth transport = vehicle_core::TransportHealth::Live) const noexcept;

  template <typename T>
  [[nodiscard]] Availability status_at(
      const vehicle_core::Signal<T> &signal, std::uint32_t identifier,
      vehicle_core::MonotonicTimestamp now_us,
      vehicle_core::TransportHealth transport = vehicle_core::TransportHealth::Live) const noexcept;

  template <typename T>
  [[nodiscard]] Reading<T> reading_at(
      const vehicle_core::Signal<T> &signal, std::uint32_t identifier,
      vehicle_core::MonotonicTimestamp now_us,
      ValidationStatus validation = ValidationStatus::Reference,
      vehicle_core::TransportHealth transport = vehicle_core::TransportHealth::Live) const noexcept;
};

class SnapshotProvider {
public:
  virtual ~SnapshotProvider() = default;
  [[nodiscard]] virtual VehicleState snapshot() const noexcept = 0;
};

// A small state owner for deterministic integrations. It owns one state by
// value, performs no allocation, and exposes snapshots only by value.
class VehicleStateStore final : public SnapshotProvider {
public:
  explicit VehicleStateStore(vehicle_core::MonotonicClock &clock,
                             VehicleFreshnessPolicy policy = {}) noexcept;

  [[nodiscard]] VehicleState &mutable_state() noexcept { return state_; }
  [[nodiscard]] const VehicleState &state() const noexcept { return state_; }
  [[nodiscard]] VehicleState snapshot() const noexcept override;

private:
  vehicle_core::MonotonicClock *clock_;
  VehicleFreshnessPolicy policy_;
  VehicleState state_{};
};

} // namespace mazda

namespace mazda {

template <typename T>
Availability VehicleState::status_at(const vehicle_core::Signal<T> &signal,
                                     const std::uint32_t identifier,
                                     const vehicle_core::MonotonicTimestamp now_us,
                                     const vehicle_core::TransportHealth transport) const noexcept {
  const auto *message = message_health_for(identifier);
  const auto message_status =
      message == nullptr ? vehicle_core::MessageHealth::Healthy : message->health;
  return mazda::status_at(signal, now_us, message_status, transport);
}

template <typename T>
Reading<T> VehicleState::reading_at(const vehicle_core::Signal<T> &signal,
                                    const std::uint32_t identifier,
                                    const vehicle_core::MonotonicTimestamp now_us,
                                    const ValidationStatus validation,
                                    const vehicle_core::TransportHealth transport) const noexcept {
  const auto *message = message_health_for(identifier);
  const auto message_status =
      message == nullptr ? vehicle_core::MessageHealth::Healthy : message->health;
  return mazda::snapshot(signal, now_us, validation, message_status, transport);
}

} // namespace mazda
