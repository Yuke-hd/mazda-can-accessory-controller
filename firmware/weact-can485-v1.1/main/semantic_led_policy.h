#pragma once

#include "local_argb/legacy_compat.hpp"
#include "mazda/decoder.hpp"

namespace weact_app {

struct Context {
  mazda::VehicleState vehicle_state{};
  local_argb::SemanticHealth health{local_argb::SemanticHealth::CanOffline};
  vehicle_core::MonotonicTimestamp decoder_error_us{0};
};

// Decode one received frame and update LED health. Only a valid turn update may
// recover DecoderError; other valid traffic can establish initial CAN-online
// health but cannot relight a stored turn value after malformed turn input.
void process_received_frame(Context &context, const vehicle_core::RawCanFrame &frame) noexcept;

[[nodiscard]] local_argb::SemanticSnapshot
semantic_snapshot(const Context &context, vehicle_core::MonotonicTimestamp now_us) noexcept;

} // namespace weact_app
