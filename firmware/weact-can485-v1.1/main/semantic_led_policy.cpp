#include "semantic_led_policy.h"

namespace weact_app {

void process_received_frame(Context &context, const vehicle_core::RawCanFrame &frame) noexcept {
  const auto turn_update_before = context.vehicle_state.turn_state.last_update_us;
  const auto decode_status = mazda::candidate::decode(frame, context.vehicle_state);
  const bool valid_turn_update =
      decode_status == mazda::candidate::DecodeStatus::Decoded &&
      context.vehicle_state.turn_state.status == vehicle_core::SignalStatus::Valid &&
      context.vehicle_state.turn_state.last_update_us > turn_update_before &&
      (context.health != local_argb::SemanticHealth::DecoderError ||
       context.vehicle_state.turn_state.last_update_us > context.decoder_error_us);

  switch (decode_status) {
  case mazda::candidate::DecodeStatus::Decoded:
    if (valid_turn_update || context.health == local_argb::SemanticHealth::CanOffline) {
      context.health = local_argb::SemanticHealth::Online;
    }
    break;
  case mazda::candidate::DecodeStatus::Ignored:
    if (context.health == local_argb::SemanticHealth::CanOffline) {
      context.health = local_argb::SemanticHealth::Online;
    }
    break;
  case mazda::candidate::DecodeStatus::Malformed:
    context.health = local_argb::SemanticHealth::DecoderError;
    if (frame.timestamp_us > context.decoder_error_us) {
      context.decoder_error_us = frame.timestamp_us;
    }
    break;
  }
}

local_argb::SemanticSnapshot
semantic_snapshot(const Context &context, const vehicle_core::MonotonicTimestamp now_us) noexcept {
  return local_argb::from_vehicle_state(context.vehicle_state.snapshot(now_us), context.health);
}

} // namespace weact_app
