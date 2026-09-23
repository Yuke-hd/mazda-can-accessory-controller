#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "../support/direct_frame_feeder.hpp"
#include "../support/fake_clock.hpp"
#include "mazda/decoder.hpp"

namespace {

vehicle_core::RawCanFrame turn_frame(const vehicle_core::MonotonicTimestamp timestamp,
                                     const bool hazard, const bool left, const bool right) {
  vehicle_core::RawCanFrame frame{};
  frame.timestamp_us = timestamp;
  frame.identifier = mazda::candidate::kTurnSwitchId;
  frame.dlc = 8;
  frame.data[1] = static_cast<std::uint8_t>((hazard ? 1U << 2U : 0U) | (right ? 1U << 4U : 0U) |
                                            (left ? 1U << 5U : 0U));
  return frame;
}

} // namespace

TEST_CASE("turn switch normalizes candidate bits with hazard and conflict precedence") {
  using namespace mazda;
  using namespace mazda::candidate;

  VehicleState state{};
  std::optional<TurnEdgeEvent> edge;
  CHECK(decode_turn_switch(turn_frame(0, false, false, false), state, &edge) ==
        DecodeStatus::Decoded);
  REQUIRE(edge.has_value());
  CHECK(edge->previous == TurnState::Unknown);
  CHECK(edge->current == TurnState::Off);
  CHECK(state.turn_state.value == TurnState::Off);
  CHECK(state.effective_turn_state() == TurnState::Off);

  CHECK(decode_turn_switch(turn_frame(1, false, true, false), state, &edge) ==
        DecodeStatus::Decoded);
  REQUIRE(edge.has_value());
  CHECK(edge->previous == TurnState::Off);
  CHECK(edge->current == TurnState::Left);
  CHECK(state.turn_state.value == TurnState::Left);
  CHECK(decode_turn_switch(turn_frame(2, false, false, true), state, &edge) ==
        DecodeStatus::Decoded);
  REQUIRE(edge.has_value());
  CHECK(edge->previous == TurnState::Left);
  CHECK(edge->current == TurnState::Right);
  CHECK(state.turn_state.value == TurnState::Right);
  CHECK(decode_turn_switch(turn_frame(3, false, true, true), state, &edge) ==
        DecodeStatus::Decoded);
  REQUIRE(edge.has_value());
  CHECK(edge->previous == TurnState::Right);
  CHECK(edge->current == TurnState::Unknown);
  CHECK(state.turn_state.value == TurnState::Unknown);
  CHECK(state.effective_turn_state() == TurnState::Unknown);
  CHECK(decode_turn_switch(turn_frame(4, true, true, true), state, &edge) == DecodeStatus::Decoded);
  REQUIRE(edge.has_value());
  CHECK(edge->previous == TurnState::Unknown);
  CHECK(edge->current == TurnState::Hazard);
  CHECK(state.turn_state.value == TurnState::Hazard);
  CHECK(state.hazard_request.value);
  CHECK(state.left_turn_request.value);
  CHECK(state.right_turn_request.value);
}

TEST_CASE("turn switch rejects malformed input and dispatch decodes blink info") {
  using namespace mazda;
  using namespace mazda::candidate;

  VehicleState state{};
  std::optional<TurnEdgeEvent> edge;
  const auto initial = turn_frame(100, false, true, false);
  REQUIRE(decode_turn_switch(initial, state, &edge) == DecodeStatus::Decoded);
  REQUIRE(edge.has_value());

  auto short_frame = initial;
  short_frame.dlc = 7;
  CHECK(decode_turn_switch(short_frame, state, &edge) == DecodeStatus::Malformed);
  CHECK_FALSE(edge.has_value());
  CHECK(state.turn_state.value == TurnState::Left);

  auto extended = initial;
  extended.identifier_format = vehicle_core::CanIdentifierFormat::Extended;
  CHECK(decode_turn_switch(extended, state, &edge) == DecodeStatus::Ignored);
  CHECK_FALSE(edge.has_value());

  auto remote = initial;
  remote.remote_request = true;
  CHECK(decode_turn_switch(remote, state, &edge) == DecodeStatus::Ignored);
  CHECK_FALSE(edge.has_value());

  auto invalid_identifier = initial;
  invalid_identifier.identifier = 0x800;
  CHECK(decode_turn_switch(invalid_identifier, state, &edge) == DecodeStatus::Malformed);
  CHECK_FALSE(edge.has_value());

  auto blink_info = initial;
  blink_info.identifier = kBlinkInfoId;
  blink_info.timestamp_us = 200;
  blink_info.data[1] = 0;
  blink_info.data[2] = 0x0c;
  blink_info.data[4] = 0x02;
  CHECK(decode(blink_info, state, &edge) == DecodeStatus::Decoded);
  CHECK_FALSE(edge.has_value());
  CHECK(state.turn_state.value == TurnState::Left);
  CHECK(state.left_turn_request.value);
  CHECK(state.left_indicator_lamp.value);
  CHECK(state.right_indicator_lamp.value);
  CHECK(state.wiper_low.value);
}

TEST_CASE("duplicate turn states do not create duplicate semantic edges") {
  mazda::VehicleState state{};

  const auto first = state.update_turn(mazda::TurnState::Left, 10);
  REQUIRE(first.has_value());
  CHECK(first->previous == mazda::TurnState::Unknown);
  CHECK(first->current == mazda::TurnState::Left);
  CHECK_FALSE(state.update_turn(mazda::TurnState::Left, 11).has_value());

  const auto changed = state.update_turn(mazda::TurnState::Hazard, 20);
  REQUIRE(changed.has_value());
  CHECK(changed->previous == mazda::TurnState::Left);
  CHECK(changed->current == mazda::TurnState::Hazard);
  CHECK_FALSE(state.update_turn(mazda::TurnState::Hazard, 21).has_value());
}

TEST_CASE("simulated replay makes turn stale after 250 ms and recovery actionable") {
  using namespace mazda;
  using namespace mazda::candidate;

  test_support::FakeClock clock;
  test_support::DirectFrameFeeder feeder;
  VehicleStateStore store{clock};
  CHECK(store.snapshot().effective_turn_state() == TurnState::Unknown);
  feeder.feed(turn_frame(1'000, false, true, false), [&](const vehicle_core::RawCanFrame &value) {
    clock.set(value.timestamp_us);
    CHECK(decode_turn_switch(value, store.mutable_state()) == DecodeStatus::Decoded);
  });
  CHECK(feeder.delivered() == 1);
  CHECK(store.snapshot().turn_state.is_valid());
  CHECK(store.snapshot().effective_turn_state() == TurnState::Left);

  CHECK(store.state().snapshot(251'000).turn_state.is_valid());
  clock.set(251'001);
  const auto stale = store.snapshot();
  CHECK(stale.turn_state.is_stale());
  CHECK(stale.effective_turn_state() == TurnState::Unknown);

  feeder.feed(turn_frame(301'000, false, false, true), [&](const vehicle_core::RawCanFrame &value) {
    clock.set(value.timestamp_us);
    CHECK(decode(value, store.mutable_state()) == DecodeStatus::Decoded);
  });
  CHECK(feeder.delivered() == 2);
  const auto recovered = store.snapshot();
  CHECK(recovered.turn_state.is_valid());
  CHECK(recovered.effective_turn_state() == TurnState::Right);
  CHECK(recovered.timestamp_us == 301'000);
}

TEST_CASE("decoder emits recovery edges after freshness loss, including same direction") {
  using namespace mazda;
  using namespace mazda::candidate;

  VehicleState state{};
  std::optional<TurnEdgeEvent> edge;
  REQUIRE(decode_turn_switch(turn_frame(1'000, false, true, false), state, &edge) ==
          DecodeStatus::Decoded);
  REQUIRE(edge.has_value());
  CHECK(edge->previous == TurnState::Unknown);
  CHECK(edge->current == TurnState::Left);

  state.refresh(251'001);
  CHECK(state.turn_state.is_stale());
  CHECK(decode_turn_switch(turn_frame(251'002, false, true, false), state, &edge) ==
        DecodeStatus::Decoded);
  REQUIRE(edge.has_value());
  CHECK(edge->previous == TurnState::Unknown);
  CHECK(edge->current == TurnState::Left);

  state.refresh(501'003);
  CHECK(state.turn_state.is_stale());
  CHECK(decode_turn_switch(turn_frame(501'004, false, false, true), state, &edge) ==
        DecodeStatus::Decoded);
  REQUIRE(edge.has_value());
  CHECK(edge->previous == TurnState::Unknown);
  CHECK(edge->current == TurnState::Right);
}

TEST_CASE("duplicate frames through decoder clear edge output") {
  using namespace mazda;
  using namespace mazda::candidate;

  VehicleState state{};
  std::optional<TurnEdgeEvent> edge;
  REQUIRE(decode_turn_switch(turn_frame(10, false, true, false), state, &edge) ==
          DecodeStatus::Decoded);
  REQUIRE(edge.has_value());
  CHECK(decode_turn_switch(turn_frame(11, false, true, false), state, &edge) ==
        DecodeStatus::Decoded);
  CHECK_FALSE(edge.has_value());
}

TEST_CASE("confirmed switch definitions document capture provenance") {
  using namespace mazda::candidate;
  CHECK(kTurnSwitchDefinition.identifier == 0x091);
  CHECK(kTurnSwitchDefinition.expected_dlc == 8);
  CHECK(kTurnSwitchDefinition.freshness_timeout_us.value() == 250'000);
  CHECK_FALSE(kTurnSwitchDefinition.pending_validation);
  CHECK(kBlinkInfoDefinition.identifier == 0x09a);
  CHECK_FALSE(kBlinkInfoDefinition.freshness_timeout_us.has_value());
  CHECK_FALSE(kBlinkInfoDefinition.pending_validation);
  CHECK(kHazardDefinition.start_bit == 10);
  CHECK(kTurnRightSwitchDefinition.start_bit == 12);
  CHECK(kTurnLeftSwitchDefinition.start_bit == 13);
  CHECK(kFrontWiperDefinition.start_bit == 20);
  CHECK(kFrontWiperDefinition.value_table != nullptr);
}
