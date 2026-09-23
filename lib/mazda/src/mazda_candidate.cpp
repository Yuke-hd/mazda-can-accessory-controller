#include "mazda/decoder.hpp"

namespace mazda::candidate {
namespace {

void initialize_observation(const vehicle_core::RawCanFrame &frame,
                            vehicle_core::DecoderObservation *observation) noexcept {
  if (observation == nullptr)
    return;
  observation->validity = vehicle_core::DecodeValidity::Ignored;
  observation->timestamp_us = frame.timestamp_us;
  observation->identifier = frame.identifier;
  observation->bus_id = frame.bus_id;
  observation->dlc = frame.dlc;
}

DecodeStatus finish_observation(const DecodeStatus status,
                                vehicle_core::DecoderObservation *observation) noexcept {
  if (observation != nullptr)
    observation->validity = status;
  return status;
}

void finish_health(const VehicleState &state, const std::uint32_t identifier,
                   const vehicle_core::SignalHealth signal,
                   vehicle_core::HealthObservation *health) noexcept {
  if (health == nullptr)
    return;
  const auto transport = health->transport;
  *health = state.health_observation(identifier, transport);
  health->signal = signal;
}

void record_malformed(const vehicle_core::RawCanFrame &frame, const std::uint32_t identifier,
                      VehicleState &state) noexcept {
  if (frame.identifier == identifier)
    (void)state.observe_message(frame, vehicle_core::DecodeValidity::Malformed);
}

MessageObservationResult record_healthy(const vehicle_core::RawCanFrame &frame,
                                        const std::uint32_t identifier,
                                        VehicleState &state) noexcept {
  if (frame.identifier != identifier)
    return MessageObservationResult::Ignored;
  return state.observe_message(frame, vehicle_core::DecodeValidity::Decoded);
}

DecodeStatus classify_frame(const vehicle_core::RawCanFrame &frame, const std::uint32_t identifier,
                            vehicle_core::DecoderObservation *observation) noexcept {
  initialize_observation(frame, observation);
  if (!frame.is_valid())
    return finish_observation(DecodeStatus::Malformed, observation);
  if (frame.identifier != identifier ||
      frame.identifier_format != vehicle_core::CanIdentifierFormat::Standard ||
      frame.remote_request) {
    return DecodeStatus::Ignored;
  }
  if (frame.dlc != kCandidateDlc)
    return finish_observation(DecodeStatus::Malformed, observation);
  return DecodeStatus::Decoded;
}

template <typename T>
bool invalidate_signal(vehicle_core::Signal<T> &signal,
                       const vehicle_core::MonotonicTimestamp timestamp) noexcept {
  // Keep the last accepted value for later stale/unavailable readings, but
  // make it non-actionable immediately. The accepted-value timestamp remains
  // the watermark; the decoder observation carries the undefined frame time.
  return signal.invalidate(timestamp);
}

std::uint16_t
big_endian_u16(const std::array<std::uint8_t, vehicle_core::kCanClassicPayloadBytes> &data,
               const std::size_t offset) noexcept {
  return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[offset]) << 8U) |
                                    data[offset + 1]);
}

SelectorPosition selector_from_raw(const std::uint8_t raw) noexcept {
  switch (raw) {
  case 0:
    return SelectorPosition::Shifting;
  case 1:
    return SelectorPosition::Park;
  case 2:
    return SelectorPosition::Reverse;
  case 3:
    return SelectorPosition::Neutral;
  case 4:
    return SelectorPosition::Drive;
  default:
    return SelectorPosition::Unknown;
  }
}

ActualGear actual_gear_from_raw(const std::uint8_t raw) noexcept {
  switch (raw) {
  case 0:
    return ActualGear::ParkOrNeutral;
  case 1:
    return ActualGear::First;
  case 2:
    return ActualGear::Second;
  case 3:
    return ActualGear::Third;
  case 4:
    return ActualGear::Fourth;
  case 5:
    return ActualGear::Fifth;
  case 6:
    return ActualGear::Sixth;
  case 14:
    return ActualGear::Reverse;
  case 15:
    return ActualGear::Shifting;
  default:
    return ActualGear::Unknown;
  }
}

FrontWiperPosition front_wiper_from_raw(const std::uint8_t raw) noexcept {
  switch (raw) {
  case 0:
    return FrontWiperPosition::Off;
  case 1:
    return FrontWiperPosition::On;
  case 2:
    return FrontWiperPosition::High;
  case 3:
    return FrontWiperPosition::Intermittent;
  default:
    return FrontWiperPosition::Unknown;
  }
}

TurnState normalize_turn(const bool hazard, const bool left, const bool right) noexcept {
  if (hazard)
    return TurnState::Hazard;
  if (left && right)
    return TurnState::Unknown;
  if (left)
    return TurnState::Left;
  if (right)
    return TurnState::Right;
  return TurnState::Off;
}

} // namespace

DecodeStatus decode_engine_data(const vehicle_core::RawCanFrame &frame, VehicleState &state,
                                vehicle_core::DecoderObservation *observation,
                                vehicle_core::HealthObservation *health) noexcept {
  const auto classification = classify_frame(frame, kEngineDataId, observation);
  if (classification != DecodeStatus::Decoded) {
    if (classification == DecodeStatus::Malformed)
      record_malformed(frame, kEngineDataId, state);
    finish_health(state, kEngineDataId, vehicle_core::SignalHealth::Unavailable, health);
    return classification;
  }

  const auto rpm_raw = big_endian_u16(frame.data, 0);
  const auto speed_raw = big_endian_u16(frame.data, 2);
  // The confirmed DBC declares EngineRPM in [0, 8500]. SPEED remains the
  // existing out-of-scope candidate; it has no invalid sentinel and its
  // 16-bit representation is already non-negative.
  if (rpm_raw > 34000U) {
    // The frame owns both numeric fields, so a physically invalid RPM makes
    // the message faulted while preserving unrelated Mazda signals.
    (void)state.observe_message(frame, vehicle_core::DecodeValidity::Malformed);
    finish_health(state, kEngineDataId, vehicle_core::SignalHealth::Unavailable, health);
    return finish_observation(DecodeStatus::Malformed, observation);
  }

  const auto message_result = record_healthy(frame, kEngineDataId, state);
  if (message_result == MessageObservationResult::Idempotent) {
    finish_health(state, kEngineDataId,
                  state.engine_rpm.has_value ? vehicle_core::SignalHealth::Available
                                             : vehicle_core::SignalHealth::NoData,
                  health);
    return finish_observation(DecodeStatus::Decoded, observation);
  }
  if (message_result != MessageObservationResult::Accepted) {
    finish_health(state, kEngineDataId,
                  state.engine_rpm.has_value ? vehicle_core::SignalHealth::Available
                                             : vehicle_core::SignalHealth::NoData,
                  health);
    return finish_observation(DecodeStatus::Decoded, observation);
  }

  const bool rpm_updated =
      state.engine_rpm.update(static_cast<float>(rpm_raw) * 0.25F, frame.timestamp_us);
  const bool speed_updated =
      state.speed_kph.update(static_cast<float>(speed_raw) * 0.01F, frame.timestamp_us);
  if (rpm_updated || speed_updated) {
    if (frame.timestamp_us > state.timestamp_us)
      state.timestamp_us = frame.timestamp_us;
  }
  finish_health(state, kEngineDataId, vehicle_core::SignalHealth::Available, health);
  return finish_observation(DecodeStatus::Decoded, observation);
}

DecodeStatus decode_gear(const vehicle_core::RawCanFrame &frame, VehicleState &state,
                         vehicle_core::DecoderObservation *observation,
                         vehicle_core::HealthObservation *health) noexcept {
  const auto classification = classify_frame(frame, kGearId, observation);
  if (classification != DecodeStatus::Decoded) {
    if (classification == DecodeStatus::Malformed)
      record_malformed(frame, kGearId, state);
    finish_health(state, kGearId, vehicle_core::SignalHealth::Unavailable, health);
    return classification;
  }

  const auto message_result = record_healthy(frame, kGearId, state);
  if (message_result == MessageObservationResult::Idempotent) {
    const auto signal_health = (state.selector_position.is_valid() && state.actual_gear.is_valid())
                                   ? vehicle_core::SignalHealth::Available
                                   : vehicle_core::SignalHealth::Unavailable;
    finish_health(state, kGearId, signal_health, health);
    return finish_observation(DecodeStatus::Decoded, observation);
  }
  if (message_result != MessageObservationResult::Accepted) {
    finish_health(state, kGearId, vehicle_core::SignalHealth::Unavailable, health);
    return finish_observation(DecodeStatus::Decoded, observation);
  }

  const auto selector_raw = static_cast<std::uint8_t>(frame.data[0] & 0x07U);
  const auto actual_raw = static_cast<std::uint8_t>((frame.data[4] >> 1U) & 0x0fU);
  const auto selector = selector_from_raw(selector_raw);
  const auto actual_gear = actual_gear_from_raw(actual_raw);
  // Undefined values are well-formed source encodings. Invalidate only the
  // affected signal while retaining its last accepted value for later stale /
  // unavailable readings; S1-G maps the non-actionable status to availability.
  const bool selector_updated = selector == SelectorPosition::Unknown
                                    ? invalidate_signal(state.selector_position, frame.timestamp_us)
                                    : state.selector_position.update(selector, frame.timestamp_us);
  const bool actual_gear_updated = actual_gear == ActualGear::Unknown
                                       ? invalidate_signal(state.actual_gear, frame.timestamp_us)
                                       : state.actual_gear.update(actual_gear, frame.timestamp_us);
  if (selector_updated || actual_gear_updated) {
    if (frame.timestamp_us > state.timestamp_us)
      state.timestamp_us = frame.timestamp_us;
  }
  const auto signal_health = (state.selector_position.is_valid() && state.actual_gear.is_valid())
                                 ? vehicle_core::SignalHealth::Available
                                 : vehicle_core::SignalHealth::Unavailable;
  finish_health(state, kGearId, signal_health, health);
  return finish_observation(DecodeStatus::Decoded, observation);
}

DecodeStatus decode_doors(const vehicle_core::RawCanFrame &frame, VehicleState &state,
                          vehicle_core::DecoderObservation *observation,
                          vehicle_core::HealthObservation *health) noexcept {
  const auto classification = classify_frame(frame, kDoorsId, observation);
  if (classification != DecodeStatus::Decoded) {
    if (classification == DecodeStatus::Malformed)
      record_malformed(frame, kDoorsId, state);
    finish_health(state, kDoorsId, vehicle_core::SignalHealth::Unavailable, health);
    return classification;
  }

  const auto message_result = record_healthy(frame, kDoorsId, state);
  if (message_result == MessageObservationResult::Idempotent) {
    finish_health(state, kDoorsId, vehicle_core::SignalHealth::Available, health);
    return finish_observation(DecodeStatus::Decoded, observation);
  }
  if (message_result != MessageObservationResult::Accepted) {
    finish_health(state, kDoorsId, vehicle_core::SignalHealth::Unavailable, health);
    return finish_observation(DecodeStatus::Decoded, observation);
  }

  // These are DBC Motorola single-bit fields. For single-bit fields the DBC
  // start bit maps directly to the byte/LSB mask used here.
  const bool liftgate_open = (frame.data[4] & 0x01U) != 0;
  const bool rear_right_door_open = (frame.data[4] & 0x04U) != 0;
  const bool rear_left_door_open = (frame.data[4] & 0x08U) != 0;
  const bool front_left_door_open_rhd = (frame.data[4] & 0x10U) != 0;
  const bool front_right_door_open_rhd = (frame.data[4] & 0x20U) != 0;
  const bool doors_unlocked = (frame.data[3] & 0x40U) != 0;

  bool updated = false;
  updated = state.liftgate_open.update(liftgate_open, frame.timestamp_us) || updated;
  updated = state.rear_right_door_open.update(rear_right_door_open, frame.timestamp_us) || updated;
  updated = state.rear_left_door_open.update(rear_left_door_open, frame.timestamp_us) || updated;
  updated = state.front_left_door_open_rhd.update(front_left_door_open_rhd, frame.timestamp_us) ||
            updated;
  updated = state.front_right_door_open_rhd.update(front_right_door_open_rhd, frame.timestamp_us) ||
            updated;
  updated = state.doors_unlocked.update(doors_unlocked, frame.timestamp_us) || updated;
  if (updated && frame.timestamp_us > state.timestamp_us)
    state.timestamp_us = frame.timestamp_us;
  finish_health(state, kDoorsId, vehicle_core::SignalHealth::Available, health);
  return finish_observation(DecodeStatus::Decoded, observation);
}

DecodeStatus decode_blink_info(const vehicle_core::RawCanFrame &frame, VehicleState &state,
                               vehicle_core::DecoderObservation *observation,
                               vehicle_core::HealthObservation *health) noexcept {
  const auto classification = classify_frame(frame, kBlinkInfoId, observation);
  if (classification != DecodeStatus::Decoded) {
    if (classification == DecodeStatus::Malformed)
      record_malformed(frame, kBlinkInfoId, state);
    finish_health(state, kBlinkInfoId, vehicle_core::SignalHealth::Unavailable, health);
    return classification;
  }

  const auto message_result = record_healthy(frame, kBlinkInfoId, state);
  if (message_result == MessageObservationResult::Idempotent) {
    finish_health(state, kBlinkInfoId, vehicle_core::SignalHealth::Available, health);
    return finish_observation(DecodeStatus::Decoded, observation);
  }
  if (message_result != MessageObservationResult::Accepted) {
    finish_health(state, kBlinkInfoId, vehicle_core::SignalHealth::Unavailable, health);
    return finish_observation(DecodeStatus::Decoded, observation);
  }

  // LEFT_BLINK is Intel in the DBC (bit 18), while the other two confirmed
  // fields use Motorola notation. Their single-bit payload masks are 0x04,
  // 0x08, and 0x02 respectively.
  const bool left_indicator_lamp = (frame.data[2] & 0x04U) != 0;
  const bool right_indicator_lamp = (frame.data[2] & 0x08U) != 0;
  const bool wiper_low = (frame.data[4] & 0x02U) != 0;

  bool updated = false;
  updated = state.left_indicator_lamp.update(left_indicator_lamp, frame.timestamp_us) || updated;
  updated = state.right_indicator_lamp.update(right_indicator_lamp, frame.timestamp_us) || updated;
  updated = state.wiper_low.update(wiper_low, frame.timestamp_us) || updated;
  if (updated && frame.timestamp_us > state.timestamp_us)
    state.timestamp_us = frame.timestamp_us;
  finish_health(state, kBlinkInfoId, vehicle_core::SignalHealth::Available, health);
  return finish_observation(DecodeStatus::Decoded, observation);
}

DecodeStatus decode_turn_switch(const vehicle_core::RawCanFrame &frame, VehicleState &state,
                                std::optional<TurnEdgeEvent> *edge,
                                vehicle_core::DecoderObservation *observation,
                                vehicle_core::HealthObservation *health) noexcept {
  if (edge != nullptr)
    edge->reset();
  const auto classification = classify_frame(frame, kTurnSwitchId, observation);
  if (classification != DecodeStatus::Decoded) {
    if (classification == DecodeStatus::Malformed)
      record_malformed(frame, kTurnSwitchId, state);
    finish_health(state, kTurnSwitchId, vehicle_core::SignalHealth::Unavailable, health);
    return classification;
  }

  const auto message_result = record_healthy(frame, kTurnSwitchId, state);
  if (message_result == MessageObservationResult::Idempotent) {
    const auto signal_health = state.turn_state.is_valid()
                                   ? vehicle_core::SignalHealth::Available
                                   : vehicle_core::SignalHealth::Unavailable;
    finish_health(state, kTurnSwitchId, signal_health, health);
    return finish_observation(DecodeStatus::Decoded, observation);
  }
  if (message_result != MessageObservationResult::Accepted) {
    finish_health(state, kTurnSwitchId, vehicle_core::SignalHealth::Unavailable, health);
    return finish_observation(DecodeStatus::Decoded, observation);
  }

  // DBC fields 10, 12, and 13 map to byte 1 bits 2, 4, and 5 when using
  // byte/LSB numbering. Hazard has precedence over either direction.
  const bool hazard = (frame.data[1] & (1U << 2U)) != 0;
  const bool right = (frame.data[1] & (1U << 4U)) != 0;
  const bool left = (frame.data[1] & (1U << 5U)) != 0;
  const auto front_wiper_raw = static_cast<std::uint8_t>((frame.data[2] >> 4U) & 0x03U);
  const auto front_wiper = front_wiper_from_raw(front_wiper_raw);
  const auto turn = normalize_turn(hazard, left, right);

  const auto turn_edge = state.update_turn(turn, frame.timestamp_us);
  if (edge != nullptr)
    *edge = turn_edge;
  bool updated = turn_edge.has_value();
  updated = state.hazard_request.update(hazard, frame.timestamp_us) || updated;
  updated = state.left_turn_request.update(left, frame.timestamp_us) || updated;
  updated = state.right_turn_request.update(right, frame.timestamp_us) || updated;
  updated = state.front_wiper.update(front_wiper, frame.timestamp_us) || updated;
  if (updated && frame.timestamp_us > state.timestamp_us)
    state.timestamp_us = frame.timestamp_us;
  const auto signal_health = state.turn_state.is_valid() ? vehicle_core::SignalHealth::Available
                                                         : vehicle_core::SignalHealth::Unavailable;
  finish_health(state, kTurnSwitchId, signal_health, health);
  return finish_observation(DecodeStatus::Decoded, observation);
}

DecodeStatus decode_brake_pedal(const vehicle_core::RawCanFrame &frame, VehicleState &state,
                                vehicle_core::DecoderObservation *observation,
                                vehicle_core::HealthObservation *health) noexcept {
  const auto classification = classify_frame(frame, kBrakePedalId, observation);
  if (classification != DecodeStatus::Decoded) {
    if (classification == DecodeStatus::Malformed)
      record_malformed(frame, kBrakePedalId, state);
    finish_health(state, kBrakePedalId, vehicle_core::SignalHealth::Unavailable, health);
    return classification;
  }

  const auto message_result = record_healthy(frame, kBrakePedalId, state);
  if (message_result == MessageObservationResult::Idempotent) {
    const auto signal_health = state.brake_pressed.is_valid()
                                   ? vehicle_core::SignalHealth::Available
                                   : vehicle_core::SignalHealth::Unavailable;
    finish_health(state, kBrakePedalId, signal_health, health);
    return finish_observation(DecodeStatus::Decoded, observation);
  }
  if (message_result != MessageObservationResult::Accepted) {
    finish_health(state, kBrakePedalId, vehicle_core::SignalHealth::Unavailable, health);
    return finish_observation(DecodeStatus::Decoded, observation);
  }

  const bool pressed = (frame.data[0] & 0x10U) != 0;
  const bool updated = state.brake_pressed.update(pressed, frame.timestamp_us);
  if (updated && frame.timestamp_us > state.timestamp_us)
    state.timestamp_us = frame.timestamp_us;
  finish_health(state, kBrakePedalId, vehicle_core::SignalHealth::Available, health);
  return finish_observation(DecodeStatus::Decoded, observation);
}

DecodeStatus decode(const vehicle_core::RawCanFrame &frame, VehicleState &state,
                    std::optional<TurnEdgeEvent> *edge,
                    vehicle_core::DecoderObservation *observation,
                    vehicle_core::HealthObservation *health) noexcept {
  if (edge != nullptr)
    edge->reset();
  const auto engine_status = decode_engine_data(frame, state, observation, health);
  if (engine_status != DecodeStatus::Ignored)
    return engine_status;
  const auto gear_status = decode_gear(frame, state, observation, health);
  if (gear_status != DecodeStatus::Ignored)
    return gear_status;
  const auto doors_status = decode_doors(frame, state, observation, health);
  if (doors_status != DecodeStatus::Ignored)
    return doors_status;
  const auto blink_status = decode_blink_info(frame, state, observation, health);
  if (blink_status != DecodeStatus::Ignored)
    return blink_status;
  const auto turn_status = decode_turn_switch(frame, state, edge, observation, health);
  if (turn_status != DecodeStatus::Ignored)
    return turn_status;
  return decode_brake_pedal(frame, state, observation, health);
}

} // namespace mazda::candidate
