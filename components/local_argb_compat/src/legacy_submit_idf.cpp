#include "local_argb/legacy_compat.hpp"
#include "local_argb/lighting_sink.hpp"

namespace local_argb {

bool submit(const SemanticSnapshot snapshot) noexcept {
  internal::LightingCommand command{};
  const auto color = color_for(snapshot, snapshot.turn_last_update_us);
  command.color = {color.red, color.green, color.blue};
  command.valid_until_us = snapshot.turn_last_update_us + kFailOffTimeoutUs;
  command.actionable = color != kBlack && snapshot.health == SemanticHealth::Online &&
                       snapshot.turn_status == vehicle_core::SignalStatus::Valid;
  if (!command.actionable) {
    command.color = {};
  }
  return internal::sink().publish(command);
}

} // namespace local_argb
