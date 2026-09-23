#include "mazda/state.hpp"

#include <algorithm>
#include <utility>

namespace mazda {
namespace {

bool same_frame(const vehicle_core::RawCanFrame &left, const MessageHealthState &right) noexcept {
  return right.has_frame && left.identifier == right.identifier && left.bus_id == right.bus_id &&
         left.dlc == right.dlc && left.identifier_format == right.identifier_format &&
         left.remote_request == right.remote_request && left.data == right.data;
}

void copy_frame(const vehicle_core::RawCanFrame &frame, MessageHealthState &record) noexcept {
  record.identifier = frame.identifier;
  record.has_frame = true;
  record.last_frame_us = frame.timestamp_us;
  record.bus_id = frame.bus_id;
  record.dlc = frame.dlc;
  record.identifier_format = frame.identifier_format;
  record.remote_request = frame.remote_request;
  record.data = frame.data;
}

MessageHealthState *find_or_allocate_record(VehicleState &state,
                                            const std::uint32_t identifier) noexcept {
  for (auto &record : state.message_health) {
    if (record.has_frame && record.identifier == identifier) {
      return &record;
    }
  }
  for (auto &record : state.message_health) {
    if (!record.has_frame) {
      record.identifier = identifier;
      return &record;
    }
  }
  return nullptr;
}

} // namespace

std::optional<TurnEdgeEvent>
VehicleState::update_turn(const TurnState state,
                          const vehicle_core::MonotonicTimestamp timestamp) noexcept {
  // Equal and older observations are handled as no-ops before refresh can
  // mutate the signal's stale/valid status.
  if (turn_state.has_value && timestamp <= turn_state.last_update_us) {
    return std::nullopt;
  }
  // A mutable receive state can sit without a snapshot between packets. Make
  // freshness part of the edge decision so recovery is Unknown -> state even
  // when the raw value happens to match the last stored direction.
  turn_state.refresh(timestamp);
  const TurnState previous = effective_turn_state();
  if (!turn_state.update(state, timestamp)) {
    return std::nullopt;
  }
  timestamp_us = std::max(timestamp_us, timestamp);
  if (previous == state) {
    return std::nullopt;
  }
  return TurnEdgeEvent{TurnEventType::StateChanged, previous, state, timestamp};
}

void VehicleState::refresh(const vehicle_core::MonotonicTimestamp now) noexcept {
  speed_kph.refresh(now);
  engine_rpm.refresh(now);
  selector_position.refresh(now);
  actual_gear.refresh(now);
  liftgate_open.refresh(now);
  rear_right_door_open.refresh(now);
  rear_left_door_open.refresh(now);
  front_left_door_open_rhd.refresh(now);
  front_right_door_open_rhd.refresh(now);
  doors_unlocked.refresh(now);
  left_indicator_lamp.refresh(now);
  right_indicator_lamp.refresh(now);
  wiper_low.refresh(now);
  front_wiper.refresh(now);
  turn_state.refresh(now);
  hazard_request.refresh(now);
  left_turn_request.refresh(now);
  right_turn_request.refresh(now);
  brake_pressed.refresh(now);
}

void VehicleState::apply_freshness_policy(const VehicleFreshnessPolicy &policy) noexcept {
  speed_kph.set_freshness_timeout(policy.speed_kph_timeout_us);
  engine_rpm.set_freshness_timeout(policy.engine_rpm_timeout_us);
  selector_position.set_freshness_timeout(policy.selector_position_timeout_us);
  actual_gear.set_freshness_timeout(policy.actual_gear_timeout_us);
  liftgate_open.set_freshness_timeout(policy.liftgate_open_timeout_us);
  rear_right_door_open.set_freshness_timeout(policy.rear_right_door_open_timeout_us);
  rear_left_door_open.set_freshness_timeout(policy.rear_left_door_open_timeout_us);
  front_left_door_open_rhd.set_freshness_timeout(policy.front_left_door_open_rhd_timeout_us);
  front_right_door_open_rhd.set_freshness_timeout(policy.front_right_door_open_rhd_timeout_us);
  doors_unlocked.set_freshness_timeout(policy.doors_unlocked_timeout_us);
  left_indicator_lamp.set_freshness_timeout(policy.left_indicator_lamp_timeout_us);
  right_indicator_lamp.set_freshness_timeout(policy.right_indicator_lamp_timeout_us);
  wiper_low.set_freshness_timeout(policy.wiper_low_timeout_us);
  front_wiper.set_freshness_timeout(policy.front_wiper_timeout_us);
  turn_state.set_freshness_timeout(policy.turn_state_timeout_us);
  hazard_request.set_freshness_timeout(policy.hazard_request_timeout_us);
  left_turn_request.set_freshness_timeout(policy.left_turn_request_timeout_us);
  right_turn_request.set_freshness_timeout(policy.right_turn_request_timeout_us);
  brake_pressed.set_freshness_timeout(policy.brake_pressed_timeout_us);
}

MessageObservationResult
VehicleState::observe_message(const vehicle_core::RawCanFrame &frame,
                              const vehicle_core::DecodeValidity validity) noexcept {
  if (validity == vehicle_core::DecodeValidity::Ignored) {
    return MessageObservationResult::Ignored;
  }

  auto *record = find_or_allocate_record(*this, frame.identifier);
  if (record == nullptr) {
    return MessageObservationResult::CapacityExceeded;
  }

  if (validity == vehicle_core::DecodeValidity::Malformed) {
    if (!record->has_frame || frame.timestamp_us > record->last_frame_us) {
      copy_frame(frame, *record);
      record->health = vehicle_core::MessageHealth::Faulted;
      record->fault_timestamp_us = frame.timestamp_us;
      return MessageObservationResult::Accepted;
    }
    if (frame.timestamp_us < record->last_frame_us) {
      return MessageObservationResult::RejectedOlder;
    }
    // The current watermark already represents the first observation at this
    // timestamp. A malformed equal-time frame must not overwrite healthy
    // state or add a fault; an identical fault duplicate is merely idempotent.
    return record->health == vehicle_core::MessageHealth::Faulted && same_frame(frame, *record)
               ? MessageObservationResult::Idempotent
               : MessageObservationResult::RejectedConflict;
  }

  if (!record->has_frame || frame.timestamp_us > record->last_frame_us) {
    copy_frame(frame, *record);
    record->health = vehicle_core::MessageHealth::Healthy;
    record->has_accepted = true;
    record->last_accepted_us = frame.timestamp_us;
    record->fault_timestamp_us.reset();
    return MessageObservationResult::Accepted;
  }
  if (frame.timestamp_us < record->last_frame_us) {
    return MessageObservationResult::RejectedOlder;
  }
  if (record->health == vehicle_core::MessageHealth::Faulted) {
    // Recovery is strictly newer than the fault watermark. An equal-time
    // healthy frame is not allowed to erase evidence of a malformed frame.
    return MessageObservationResult::RejectedConflict;
  }
  if (same_frame(frame, *record)) {
    return MessageObservationResult::Idempotent;
  }
  // Equal-time healthy frames with different payload/metadata are ambiguous;
  // retain the first observation rather than allowing order to decide state.
  return MessageObservationResult::RejectedConflict;
}

const MessageHealthState *
VehicleState::message_health_for(const std::uint32_t identifier) const noexcept {
  for (const auto &record : message_health) {
    if (record.has_frame && record.identifier == identifier) {
      return &record;
    }
  }
  return nullptr;
}

vehicle_core::HealthObservation
VehicleState::health_observation(const std::uint32_t identifier,
                                 const vehicle_core::TransportHealth transport) const noexcept {
  vehicle_core::HealthObservation result{};
  result.transport = transport;
  const auto *record = message_health_for(identifier);
  if (record == nullptr) {
    return result;
  }
  result.message = record->health;
  result.has_last_frame = record->has_frame;
  result.last_frame_us = record->last_frame_us;
  result.has_last_accepted = record->has_accepted;
  result.last_accepted_us = record->last_accepted_us;
  result.fault_timestamp_us = record->fault_timestamp_us;
  return result;
}

VehicleState VehicleState::snapshot(const vehicle_core::MonotonicTimestamp now) const noexcept {
  VehicleState result = *this;
  result.refresh(now);
  result.timestamp_us = now;
  return result;
}

VehicleState VehicleState::snapshot(const vehicle_core::MonotonicTimestamp now,
                                    const VehicleFreshnessPolicy &policy) const noexcept {
  VehicleState result = *this;
  result.apply_freshness_policy(policy);
  return result.snapshot(now);
}

VehicleStateStore::VehicleStateStore(vehicle_core::MonotonicClock &clock,
                                     VehicleFreshnessPolicy policy) noexcept
    : clock_(&clock), policy_(policy) {}

VehicleState VehicleStateStore::snapshot() const noexcept {
  return state_.snapshot(clock_->now(), policy_);
}

} // namespace mazda
