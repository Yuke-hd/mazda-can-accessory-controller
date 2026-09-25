#include "local_argb_actions/led_action_sink.hpp"

#include <limits>
#include <optional>

#include "vehicle_core/time.hpp"

namespace local_argb_actions {
namespace {

// Held levels have no deadline; the engine's Deactivate ends them.
constexpr vehicle_core::MonotonicTimestamp kHeldUntilUs =
    std::numeric_limits<vehicle_core::MonotonicTimestamp>::max();

local_argb::internal::LightingCommand held_command(const LedEffects effects) noexcept {
  local_argb::internal::LightingCommand command{};
  command.left_turn = effects.left_turn;
  command.right_turn = effects.right_turn;
  command.brake = effects.brake;
  command.actionable = effects.any();
  command.valid_until_us = command.actionable ? kHeldUntilUs : 0;
  return command;
}

// LED effects are on/off levels: Triggers and SetLevel carry none.
std::optional<bool> on_off_level(const action_engine::ActionCommandKind kind) noexcept {
  switch (kind) {
  case action_engine::ActionCommandKind::Activate:
    return true;
  case action_engine::ActionCommandKind::Deactivate:
    return false;
  case action_engine::ActionCommandKind::Trigger:
  case action_engine::ActionCommandKind::SetLevel:
    break;
  }
  return std::nullopt;
}

} // namespace

void LedActionSink::execute(const action_engine::ActionCommand &command) noexcept {
  const auto active = on_off_level(command.kind);
  if (!active || !bindings_.hold(command.action, *active))
    return;
  // A rejected publish is not retried; see the LightingSink precondition in
  // the header.
  (void)lighting_->publish(held_command(bindings_.lit()));
}

} // namespace local_argb_actions
