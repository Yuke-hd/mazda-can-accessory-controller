#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "../support/fake_clock.hpp"
#include "mazda/state.hpp"

TEST_CASE("turn updates create semantic edge events without CAN identity") {
  mazda::VehicleState state{};
  CHECK(state.turn_state.is_unknown());

  const auto first = state.update_turn(mazda::TurnState::Left, 10);
  REQUIRE(first.has_value());
  CHECK(first->previous == mazda::TurnState::Unknown);
  CHECK(first->current == mazda::TurnState::Left);
  CHECK(first->timestamp_us == 10);

  CHECK_FALSE(state.update_turn(mazda::TurnState::Left, 11).has_value());
  const auto second = state.update_turn(mazda::TurnState::Off, 20);
  REQUIRE(second.has_value());
  CHECK(second->previous == mazda::TurnState::Left);
  CHECK(second->current == mazda::TurnState::Off);
  CHECK(state.update_turn(mazda::TurnState::Right, 19).has_value() == false);
}

TEST_CASE("selector position and actual gear are independent signals") {
  mazda::VehicleState state{};
  REQUIRE(state.selector_position.update(mazda::SelectorPosition::Drive, 100));
  REQUIRE(state.actual_gear.update(mazda::ActualGear::Third, 100));

  CHECK(state.selector_position.value == mazda::SelectorPosition::Drive);
  CHECK(state.actual_gear.value == mazda::ActualGear::Third);
  CHECK(static_cast<const void *>(&state.selector_position) !=
        static_cast<const void *>(&state.actual_gear));
}

TEST_CASE("snapshot applies each signal's freshness policy independently") {
  mazda::VehicleState state{};
  REQUIRE(state.speed_kph.update(20.0F, 100));
  REQUIRE(state.turn_state.update(mazda::TurnState::Left, 100));
  REQUIRE(state.front_wiper.update(mazda::FrontWiperPosition::On, 100));
  REQUIRE(state.liftgate_open.update(true, 100));

  mazda::VehicleFreshnessPolicy policy{};
  policy.speed_kph_timeout_us = 500'000;
  policy.front_wiper_timeout_us = 500'000;
  const auto snapshot = state.snapshot(350'001, policy);
  CHECK(snapshot.speed_kph.is_valid());
  CHECK(snapshot.front_wiper.is_valid());
  CHECK(snapshot.liftgate_open.is_stale());
  CHECK(snapshot.turn_state.is_stale());
  CHECK(state.speed_kph.is_valid());
  CHECK(state.front_wiper.is_valid());
  CHECK(state.liftgate_open.is_valid());
  CHECK(state.turn_state.is_valid());
}

TEST_CASE("unconfigured freshness conservatively becomes stale after time advances") {
  mazda::VehicleState state{};
  REQUIRE(state.speed_kph.update(20.0F, 100));

  const auto snapshot = state.snapshot(101);
  CHECK(snapshot.speed_kph.is_stale());
  CHECK(state.speed_kph.is_valid());
}

TEST_CASE("snapshot freshness is deterministic and does not mutate source") {
  test_support::FakeClock clock;
  mazda::VehicleFreshnessPolicy policy{};
  policy.speed_kph_timeout_us = 500'000;
  mazda::VehicleStateStore store{clock, policy};
  REQUIRE(store.mutable_state().speed_kph.update(12.5F, 100));

  clock.set(100'350);
  const auto fresh = store.snapshot();
  CHECK(fresh.speed_kph.is_valid());
  CHECK(fresh.timestamp_us == 100'350);

  clock.set(600'001);
  const auto stale = store.snapshot();
  CHECK(stale.speed_kph.is_stale());
  CHECK(stale.speed_kph.value == doctest::Approx(12.5F));
  CHECK(store.state().speed_kph.is_valid());
}
