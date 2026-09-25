#include "local_argb_actions/led_action_sink.hpp"

#include <limits>
#include <optional>

#include "vehicle_core/time.hpp"

namespace local_argb_actions {
namespace {

// Held levels have no deadline; the engine's Deactivate ends them.
constexpr vehicle_core::MonotonicTimestamp kHeldUntilUs =
    std::numeric_limits<vehicle_core::MonotonicTimestamp>::max();

using local_argb::internal::FillFraction;

local_argb::internal::LightingCommand
held_command(const LedEffects effects, const local_argb::internal::LightingFills &fills) noexcept {
  local_argb::internal::LightingCommand command{};
  command.left_turn = effects.left_turn;
  command.right_turn = effects.right_turn;
  command.brake = effects.brake;
  command.fills = fills;
  command.actionable = effects.any() || !fills.empty();
  command.valid_until_us = command.actionable ? kHeldUntilUs : 0;
  return command;
}

// On/off effects follow only Activate and Deactivate.
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

// Fill effects take a level from every command but Trigger.
std::optional<FillFraction> commanded_level(const action_engine::ActionCommand &command) noexcept {
  switch (command.kind) {
  case action_engine::ActionCommandKind::Activate:
    return FillFraction::full();
  case action_engine::ActionCommandKind::Deactivate:
    return FillFraction::empty();
  case action_engine::ActionCommandKind::SetLevel:
    return fill_level(command.level);
  case action_engine::ActionCommandKind::Trigger:
    break;
  }
  return std::nullopt;
}

} // namespace

void LedActionSink::execute(const action_engine::ActionCommand &command) noexcept {
  const auto active = on_off_level(command.kind);
  const auto level = commanded_level(command);
  // Both are evaluated so that one action can drive a fill and an effect.
  const bool effect_bound = active && bindings_.hold(command.action, *active);
  const bool fill_bound = level && fills_.hold(command.action, *level);
  if (!effect_bound && !fill_bound)
    return;
  // A rejected publish is not retried; see the LightingSink precondition in
  // the header.
  (void)lighting_->publish(held_command(bindings_.lit(), fills_.lit()));
}

} // namespace local_argb_actions
