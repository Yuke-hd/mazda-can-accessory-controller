#include <cassert>
#include <cstdint>

#include "vehicle_lighting_policy/policy.hpp"

namespace {

vehicle_lighting_policy::TurnInput
input(const mazda::TurnState turn, const vehicle_core::Availability availability,
      const vehicle_core::MonotonicTimestamp signal_deadline = 0,
      const vehicle_core::MonotonicTimestamp transport_deadline = 0) {
  vehicle_lighting_policy::TurnInput result{};
  result.turn.value = turn;
  result.turn.availability = availability;
  result.turn.validation = mazda::ValidationStatus::Observed;
  result.signal_valid_until_us = signal_deadline;
  result.transport_valid_until_us = transport_deadline;
  return result;
}

void test_mapping_and_fail_off() {
  using namespace vehicle_lighting_policy;
  assert(command_for(input(mazda::TurnState::Left, mazda::Availability::Fresh, 250)).color ==
         kLeftGreen);
  assert(command_for(input(mazda::TurnState::Right, mazda::Availability::Fresh, 250)).color ==
         kRightBlue);
  assert(command_for(input(mazda::TurnState::Hazard, mazda::Availability::Fresh, 250)).color ==
         kHazardAmber);
  assert(command_for(input(mazda::TurnState::Left, mazda::Availability::FreshnessUnverified, 250))
             .actionable);

  for (const auto availability : {mazda::Availability::NoData, mazda::Availability::Stale,
                                  mazda::Availability::Unavailable}) {
    assert(!command_for(input(mazda::TurnState::Left, availability, 250)).actionable);
  }
  assert(
      !command_for(input(mazda::TurnState::Unknown, mazda::Availability::Fresh, 250)).actionable);
  // Unknown is also the decoder's representation for conflicting left/right
  // requests, so a conflict cannot accidentally illuminate the indicator.
  assert(!command_for(input(mazda::TurnState::Off, mazda::Availability::Fresh, 250)).actionable);
  assert(kLeftGreen.green <= kBrightnessCeiling);
  assert(kRightBlue.blue <= kBrightnessCeiling);
  assert(kHazardAmber.red <= kBrightnessCeiling);
  assert(kHazardAmber.green <= kBrightnessCeiling);
}

void test_earliest_deadline() {
  using namespace vehicle_lighting_policy;
  auto command = command_for(input(mazda::TurnState::Left, mazda::Availability::Fresh, 700, 900));
  assert(command.actionable);
  assert(command.valid_until_us == 700);
  command = command_for(input(mazda::TurnState::Left, mazda::Availability::Fresh, 900, 700));
  assert(command.valid_until_us == 700);
  command = command_for(input(mazda::TurnState::Left, mazda::Availability::Fresh, 0, 700));
  assert(command.valid_until_us == 700);
  command = command_for(input(mazda::TurnState::Left, mazda::Availability::Fresh, 900, 0));
  assert(command.valid_until_us == 900);
  command = command_for(input(mazda::TurnState::Left, mazda::Availability::Fresh));
  assert(!command.actionable);
}

void test_private_heartbeat_keeps_latest_deadline() {
  using namespace vehicle_lighting_policy;
  PublicationPolicy policy;
  auto first = input(mazda::TurnState::Left, mazda::Availability::Fresh, 250, 1'000);
  assert(policy.should_publish(first, 100));
  assert(policy.command().actionable);
  assert(policy.command().valid_until_us == 250);

  auto refreshed = first;
  refreshed.signal_valid_until_us = 400;
  assert(!policy.should_publish(refreshed, 100 + kHeartbeatUs - 1));
  assert(policy.has_input());
  assert(policy.command().valid_until_us == 400);
  assert(policy.should_publish(refreshed, 100 + kHeartbeatUs));
  assert(policy.command().valid_until_us == 400);

  // A transport deadline is independent of the signal deadline; heartbeat
  // publication cannot make an already-expired command actionable again.
  auto transport_expired = refreshed;
  transport_expired.transport_valid_until_us = 300;
  assert(!policy.should_publish(transport_expired, 100 + kHeartbeatUs + 1));
  const auto command = policy.command();
  assert(command.actionable);
  assert(command.valid_until_us == 300);
}

} // namespace

int main() {
  test_mapping_and_fail_off();
  test_earliest_deadline();
  test_private_heartbeat_keeps_latest_deadline();
  return 0;
}
