#include "vehicle_lighting_policy/policy.hpp"

#include <algorithm>

namespace vehicle_lighting_policy {
namespace {

bool available(const mazda::Reading<mazda::TurnState> &reading) noexcept {
  return reading.value.has_value() &&
         (reading.availability == vehicle_core::Availability::Fresh ||
          reading.availability == vehicle_core::Availability::FreshnessUnverified);
}

vehicle_core::MonotonicTimestamp earliest_deadline(const TurnInput &input) noexcept {
  const auto signal = input.signal_valid_until_us;
  const auto transport = input.transport_valid_until_us;
  if (signal == 0)
    return transport;
  if (transport == 0)
    return signal;
  return std::min(signal, transport);
}

bool semantic_equal(const TurnInput &left, const TurnInput &right) noexcept {
  return left.turn.value == right.turn.value && left.turn.availability == right.turn.availability;
}

} // namespace

LightingCommand command_for(const TurnInput &input) noexcept {
  LightingCommand result{};
  if (!available(input.turn))
    return result;

  switch (*input.turn.value) {
  case mazda::TurnState::Left:
    result.color = kLeftGreen;
    break;
  case mazda::TurnState::Right:
    result.color = kRightBlue;
    break;
  case mazda::TurnState::Hazard:
    result.color = kHazardAmber;
    break;
  case mazda::TurnState::Unknown:
  case mazda::TurnState::Off:
    return result;
  }

  result.valid_until_us = earliest_deadline(input);
  if (result.valid_until_us == 0) {
    result.color = kBlack;
    return result;
  }
  result.actionable = true;
  return result;
}

bool PublicationPolicy::should_publish(const TurnInput input,
                                       const vehicle_core::MonotonicTimestamp now_us) noexcept {
  latest_ = input;
  const bool changed = !has_published_ || !semantic_equal(input, published_);
  const bool heartbeat =
      has_published_ && (now_us < last_publish_us_ || now_us - last_publish_us_ >= kHeartbeatUs);
  has_input_ = true;
  if (!changed && !heartbeat)
    return false;

  published_ = input;
  last_publish_us_ = now_us;
  has_published_ = true;
  return true;
}

} // namespace vehicle_lighting_policy
