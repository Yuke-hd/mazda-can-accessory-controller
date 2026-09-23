#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>

#include "../support/direct_frame_feeder.hpp"
#include "../support/fake_clock.hpp"
#include "mazda/decoder.hpp"

namespace {

vehicle_core::RawCanFrame frame(const std::uint32_t id, const std::uint64_t timestamp,
                                std::initializer_list<std::uint8_t> bytes,
                                const std::uint8_t dlc = 8) {
  vehicle_core::RawCanFrame result{};
  result.timestamp_us = timestamp;
  result.identifier = id;
  result.dlc = dlc;
  std::copy(bytes.begin(), bytes.end(), result.data.begin());
  return result;
}

} // namespace

TEST_CASE("confirmed definitions retain DBC provenance and exact metadata") {
  using namespace mazda::candidate;
  CHECK(kEngineDataDefinition.identifier == 0x202);
  CHECK(kEngineDataDefinition.expected_dlc == 8);
  CHECK_FALSE(kEngineDataDefinition.expected_period_us.has_value());
  CHECK_FALSE(kEngineDataDefinition.freshness_timeout_us.has_value());
  CHECK_FALSE(kEngineDataDefinition.pending_validation);
  CHECK(std::string{kEngineDataDefinition.provenance}.find("capture-derived") != std::string::npos);
  CHECK(kTransmissionDefinition.name == std::string{"TRANSMISSION"});
  CHECK(kDoorsDefinition.identifier == kDoorsId);
  CHECK_FALSE(kBlinkInfoDefinition.pending_validation);
  CHECK(kEngineRpmDefinition.name == std::string{"EngineRPM"});
  CHECK(kEngineRpmDefinition.scale == doctest::Approx(0.25F));
  CHECK(kEngineSpeedDefinition.scale == doctest::Approx(0.01F));
  CHECK(kEngineSpeedDefinition.physical_max == doctest::Approx(655.35F));
  CHECK(std::string{kEngineSpeedDefinition.invalid_values}.find("validated vehicle limit") !=
        std::string::npos);
  CHECK(kEngineRpmDefinition.unit == vehicle_core::SignalUnit::RevolutionsPerMinute);
  CHECK(kEngineSpeedDefinition.unit == vehicle_core::SignalUnit::KilometresPerHour);
  CHECK(kEngineRpmDefinition.dbc_start_bit == 7);
  CHECK(kEngineRpmDefinition.start_bit == 0);
  CHECK(kSelectorDefinition.dbc_start_bit == 2);
  CHECK(kSelectorDefinition.start_bit == 0);
  CHECK(kSelectorDefinition.physical_min == doctest::Approx(0.0F));
  CHECK(kSelectorDefinition.physical_max == doctest::Approx(7.0F));
  CHECK(kActualGearDefinition.dbc_start_bit == 36);
  CHECK(kActualGearDefinition.start_bit == 33);
  CHECK(kActualGearDefinition.physical_min == doctest::Approx(0.0F));
  CHECK(kActualGearDefinition.physical_max == doctest::Approx(15.0F));
  CHECK(kLeftIndicatorLampDefinition.byte_order ==
        mazda::candidate::CandidateSignalDefinition::ByteOrder::Intel);
  CHECK(kLeftIndicatorLampDefinition.start_bit == 18);
  CHECK(kFrontWiperDefinition.bit_length == 2);
  CHECK(kFrontWiperDefinition.dbc_start_bit == 21);
  CHECK(std::string{kFrontLeftDoorOpenRhdDefinition.name}.find("Reference") == std::string::npos);
}

TEST_CASE("ENGINE_DATA decodes the confirmed big-endian RPM vector") {
  // EngineRPM is 7|16@0+ (0.25,0) in the capture-derived DBC. SPEED remains
  // an out-of-scope candidate retained for existing consumers.
  const auto input = frame(0x202, 1000, {0x09, 0x5b, 0x00, 0x00, 0, 0, 0, 0});
  mazda::VehicleState state{};
  vehicle_core::DecoderObservation observation{};

  CHECK(mazda::candidate::decode_engine_data(input, state) ==
        mazda::candidate::DecodeStatus::Decoded);
  CHECK(mazda::candidate::decode_engine_data(input, state, &observation) ==
        vehicle_core::DecodeValidity::Decoded);
  CHECK(observation.identifier == mazda::candidate::kEngineDataId);
  CHECK(observation.timestamp_us == 1000);
  CHECK(observation.dlc == 8);
  CHECK(state.engine_rpm.is_valid());
  CHECK(state.engine_rpm.value == doctest::Approx(598.75F));
  CHECK(state.speed_kph.is_valid());
  CHECK(state.speed_kph.value == doctest::Approx(0.0F));
  CHECK(state.timestamp_us == 1000);
}

TEST_CASE("ENGINE_DATA accepts representable boundaries and rejects invalid RPM") {
  mazda::VehicleState state{};
  const auto boundary = frame(0x202, 10, {0x84, 0xd0, 0xff, 0xff, 0, 0, 0, 0});
  CHECK(mazda::candidate::decode_engine_data(boundary, state) ==
        mazda::candidate::DecodeStatus::Decoded);
  CHECK(state.engine_rpm.value == doctest::Approx(8500.0F));
  CHECK(state.speed_kph.value == doctest::Approx(655.35F));

  const auto invalid = frame(0x202, 11, {0x84, 0xd1, 0, 0, 0, 0, 0, 0});
  vehicle_core::DecoderObservation observation{};
  CHECK(mazda::candidate::decode_engine_data(invalid, state) ==
        mazda::candidate::DecodeStatus::Malformed);
  CHECK(mazda::candidate::decode_engine_data(invalid, state, &observation) ==
        vehicle_core::DecodeValidity::Malformed);
  CHECK(observation.identifier == mazda::candidate::kEngineDataId);
  CHECK(state.engine_rpm.value == doctest::Approx(8500.0F));
  CHECK(state.engine_rpm.last_update_us == 10);
}

TEST_CASE("candidate decoders reject wrong DLC, extended, remote, and other IDs") {
  mazda::VehicleState state{};
  const auto short_engine = frame(0x202, 1, {0, 0, 0, 0, 0, 0, 0}, 7);
  CHECK(mazda::candidate::decode(short_engine, state) == mazda::candidate::DecodeStatus::Malformed);
  CHECK(state.engine_rpm.is_unknown());
  const auto short_gear = frame(0x228, 1, {0x04, 0, 0, 0, 0, 0, 0}, 7);
  CHECK(mazda::candidate::decode(short_gear, state) == mazda::candidate::DecodeStatus::Malformed);
  CHECK(state.selector_position.is_unknown());
  const auto short_doors = frame(0x43e, 1, {0, 0, 0, 0, 0, 0, 0}, 7);
  CHECK(mazda::candidate::decode(short_doors, state) == mazda::candidate::DecodeStatus::Malformed);
  CHECK(state.liftgate_open.is_unknown());
  const auto short_blink = frame(0x09a, 1, {0, 0, 0, 0, 0, 0, 0}, 7);
  CHECK(mazda::candidate::decode(short_blink, state) == mazda::candidate::DecodeStatus::Malformed);
  CHECK(state.left_indicator_lamp.is_unknown());

  auto invalid_format = frame(0x202, 2, {0, 0, 0, 0, 0, 0, 0, 0});
  invalid_format.identifier_format = static_cast<vehicle_core::CanIdentifierFormat>(0xff);
  vehicle_core::DecoderObservation invalid_observation{};
  CHECK_FALSE(invalid_format.is_valid());
  CHECK(mazda::candidate::decode(invalid_format, state, nullptr, &invalid_observation) ==
        vehicle_core::DecodeValidity::Malformed);
  CHECK(invalid_observation.validity == vehicle_core::DecodeValidity::Malformed);

  auto extended = frame(0x202, 2, {0, 0, 0, 0, 0, 0, 0, 0});
  extended.identifier_format = vehicle_core::CanIdentifierFormat::Extended;
  vehicle_core::DecoderObservation extended_observation{};
  CHECK(mazda::candidate::decode(extended, state, nullptr, &extended_observation) ==
        mazda::candidate::DecodeStatus::Ignored);
  CHECK(extended_observation.validity == vehicle_core::DecodeValidity::Ignored);
  auto remote = frame(0x202, 3, {0, 0, 0, 0, 0, 0, 0, 0});
  remote.remote_request = true;
  CHECK(mazda::candidate::decode(remote, state) == mazda::candidate::DecodeStatus::Ignored);
  const auto other = frame(0x201, 4, {0, 0, 0, 0, 0, 0, 0, 0});
  CHECK(mazda::candidate::decode(other, state) == mazda::candidate::DecodeStatus::Ignored);
}

TEST_CASE("GEAR keeps selector and actual transmission gear independent") {
  // Synthetic vector from the issue acceptance example: Drive, second gear.
  const auto input = frame(0x228, 2000, {0x24, 0x81, 0x07, 0xff, 0x04, 0xf0, 0, 0});
  mazda::VehicleState state{};
  CHECK(mazda::candidate::decode_gear(input, state) == mazda::candidate::DecodeStatus::Decoded);
  CHECK(state.selector_position.value == mazda::SelectorPosition::Drive);
  CHECK(state.actual_gear.value == mazda::ActualGear::Second);
  CHECK(state.selector_position.last_update_us == 2000);
  CHECK(state.actual_gear.last_update_us == 2000);

  const auto reverse = frame(0x228, 2001, {0x02, 0, 0, 0, 0x1c, 0, 0, 0});
  REQUIRE(mazda::candidate::decode_gear(reverse, state) == mazda::candidate::DecodeStatus::Decoded);
  CHECK(state.selector_position.value == mazda::SelectorPosition::Reverse);
  CHECK(state.actual_gear.value == mazda::ActualGear::Reverse);
  CHECK(mazda::candidate::kActualGearDefinition.physical_max >= 14.0F);
  const auto park = frame(0x228, 2002, {0x01, 0, 0, 0, 0x00, 0, 0, 0});
  REQUIRE(mazda::candidate::decode_gear(park, state) == mazda::candidate::DecodeStatus::Decoded);
  CHECK(state.selector_position.value == mazda::SelectorPosition::Park);
  CHECK(state.actual_gear.value == mazda::ActualGear::ParkOrNeutral);

  const auto neutral = frame(0x228, 2003, {0x03, 0, 0, 0, 0x02, 0, 0, 0});
  REQUIRE(mazda::candidate::decode_gear(neutral, state) == mazda::candidate::DecodeStatus::Decoded);
  CHECK(state.selector_position.value == mazda::SelectorPosition::Neutral);
  CHECK(state.actual_gear.value == mazda::ActualGear::First);

  const std::array<std::uint8_t, 8> actual_raw{0, 1, 2, 3, 4, 5, 6, 14};
  const std::array<mazda::ActualGear, 8> actual_expected{
      mazda::ActualGear::ParkOrNeutral, mazda::ActualGear::First,   mazda::ActualGear::Second,
      mazda::ActualGear::Third,         mazda::ActualGear::Fourth,  mazda::ActualGear::Fifth,
      mazda::ActualGear::Sixth,         mazda::ActualGear::Reverse,
  };
  for (std::size_t index = 0; index < actual_raw.size(); ++index) {
    const auto actual =
        frame(0x228, 2010 + index,
              {0x01, 0, 0, 0, static_cast<std::uint8_t>(actual_raw[index] << 1U), 0, 0, 0});
    REQUIRE(mazda::candidate::decode_gear(actual, state) ==
            mazda::candidate::DecodeStatus::Decoded);
    CHECK(state.actual_gear.value == actual_expected[index]);
  }
}

TEST_CASE("GEAR preserves source shifting and invalidates undefined semantic values") {
  mazda::VehicleState state{};
  const auto shifting = frame(0x228, 1, {0x00, 0, 0, 0, 0x1e, 0, 0, 0});
  CHECK(mazda::candidate::decode_gear(shifting, state) == mazda::candidate::DecodeStatus::Decoded);
  CHECK(state.selector_position.value == mazda::SelectorPosition::Shifting);
  CHECK(state.actual_gear.value == mazda::ActualGear::Shifting);
  CHECK(state.selector_position.is_valid());
  CHECK(state.actual_gear.is_valid());

  const auto park_or_neutral = frame(0x228, 2, {0x01, 0, 0, 0, 0x00, 0, 0, 0});
  REQUIRE(mazda::candidate::decode_gear(park_or_neutral, state) ==
          mazda::candidate::DecodeStatus::Decoded);
  CHECK(state.actual_gear.value == mazda::ActualGear::ParkOrNeutral);
  CHECK(state.actual_gear.value != mazda::ActualGear::Park);

  const auto supported = frame(0x228, 3, {0x04, 0, 0, 0, 0x1c, 0, 0, 0});
  REQUIRE(mazda::candidate::decode_gear(supported, state) ==
          mazda::candidate::DecodeStatus::Decoded);
  CHECK(state.selector_position.value == mazda::SelectorPosition::Drive);
  CHECK(state.actual_gear.value == mazda::ActualGear::Reverse);

  const auto undefined = frame(0x228, 4, {0x05, 0, 0, 0, 0x0e, 0, 0, 0});
  CHECK(mazda::candidate::decode_gear(undefined, state) == mazda::candidate::DecodeStatus::Decoded);
  CHECK(state.selector_position.value == mazda::SelectorPosition::Drive);
  CHECK(state.actual_gear.value == mazda::ActualGear::Reverse);
  CHECK(state.selector_position.is_unknown());
  CHECK(state.actual_gear.is_unknown());
  CHECK(state.selector_position.last_update_us == 3);
  CHECK(state.actual_gear.last_update_us == 3);
}

TEST_CASE("malformed owned messages do not overwrite unrelated accepted signals") {
  mazda::VehicleState state{};
  const auto turn = frame(mazda::candidate::kTurnSwitchId, 100, {0, 0x20, 0, 0, 0, 0, 0, 0});
  REQUIRE(mazda::candidate::decode_turn_switch(turn, state) ==
          mazda::candidate::DecodeStatus::Decoded);
  REQUIRE(state.turn_state.value == mazda::TurnState::Left);

  const auto invalid_engine =
      frame(mazda::candidate::kEngineDataId, 101, {0x84, 0xd1, 0, 0, 0, 0, 0, 0});
  CHECK(mazda::candidate::decode(invalid_engine, state) ==
        mazda::candidate::DecodeStatus::Malformed);
  CHECK(state.turn_state.value == mazda::TurnState::Left);
  CHECK(state.turn_state.last_update_us == 100);

  const auto short_gear = frame(mazda::candidate::kGearId, 102, {0, 0, 0, 0, 0, 0, 0}, 7);
  CHECK(mazda::candidate::decode(short_gear, state) == mazda::candidate::DecodeStatus::Malformed);
  CHECK(state.turn_state.value == mazda::TurnState::Left);

  const auto short_turn = frame(mazda::candidate::kTurnSwitchId, 103, {0, 0, 0, 0, 0, 0, 0}, 7);
  CHECK(mazda::candidate::decode_turn_switch(short_turn, state) ==
        mazda::candidate::DecodeStatus::Malformed);
  CHECK(state.turn_state.value == mazda::TurnState::Left);
  CHECK(state.turn_state.last_update_us == 100);
}

TEST_CASE("DOORS decodes every confirmed boolean with the supplied DBC bit layout") {
  using namespace mazda;
  using namespace mazda::candidate;
  using namespace vehicle_core;

  VehicleState state{};
  const auto closed = frame(kDoorsId, 100, {0, 0, 0, 0, 0, 0, 0, 0});
  REQUIRE(decode_doors(closed, state) == DecodeStatus::Decoded);
  CHECK_FALSE(state.liftgate_open.value);
  CHECK_FALSE(state.rear_right_door_open.value);
  CHECK_FALSE(state.rear_left_door_open.value);
  CHECK_FALSE(state.front_left_door_open_rhd.value);
  CHECK_FALSE(state.front_right_door_open_rhd.value);
  CHECK_FALSE(state.doors_unlocked.value);

  struct DoorBitTest {
    std::uint8_t byte3_mask;
    std::uint8_t byte4_mask;
    Signal<bool> VehicleState::*signal;
  };
  const std::array<DoorBitTest, 6> door_bits{{
      {0, 0x01, &VehicleState::liftgate_open},
      {0, 0x04, &VehicleState::rear_right_door_open},
      {0, 0x08, &VehicleState::rear_left_door_open},
      {0, 0x10, &VehicleState::front_left_door_open_rhd},
      {0, 0x20, &VehicleState::front_right_door_open_rhd},
      {0x40, 0, &VehicleState::doors_unlocked},
  }};
  for (std::size_t index = 0; index < door_bits.size(); ++index) {
    VehicleState one_bit_state{};
    const auto one_bit =
        frame(kDoorsId, 110 + index,
              {0, 0, 0, door_bits[index].byte3_mask, door_bits[index].byte4_mask, 0, 0, 0});
    REQUIRE(decode_doors(one_bit, one_bit_state) == DecodeStatus::Decoded);
    CHECK((one_bit_state.*door_bits[index].signal).value);
  }

  // D4 bit 6 is DoorsUnlocked; D5 bits 0, 2, 3, 4, and 5 are the five
  // confirmed door/liftgate fields.
  const auto open = frame(kDoorsId, 200, {0, 0, 0, 0x40, 0x3d, 0, 0, 0});
  REQUIRE(decode_doors(open, state) == DecodeStatus::Decoded);
  CHECK(state.liftgate_open.value);
  CHECK(state.rear_right_door_open.value);
  CHECK(state.rear_left_door_open.value);
  CHECK(state.front_left_door_open_rhd.value);
  CHECK(state.front_right_door_open_rhd.value);
  CHECK(state.doors_unlocked.value);
  CHECK(state.timestamp_us == 200);
  CHECK(state.liftgate_open.last_update_us == 200);
}

TEST_CASE("BLINK_INFO decodes indicator lamps and low-speed wiper status") {
  using namespace mazda;
  using namespace mazda::candidate;
  using namespace vehicle_core;

  VehicleState state{};
  const auto off = frame(kBlinkInfoId, 300, {0, 0, 0, 0, 0, 0, 0, 0});
  REQUIRE(decode_blink_info(off, state) == DecodeStatus::Decoded);
  CHECK_FALSE(state.left_indicator_lamp.value);
  CHECK_FALSE(state.right_indicator_lamp.value);
  CHECK_FALSE(state.wiper_low.value);

  struct BlinkBitTest {
    std::uint8_t byte2_mask;
    std::uint8_t byte4_mask;
    Signal<bool> VehicleState::*signal;
  };
  const std::array<BlinkBitTest, 3> blink_bits{{
      {0x04, 0, &VehicleState::left_indicator_lamp},
      {0x08, 0, &VehicleState::right_indicator_lamp},
      {0, 0x02, &VehicleState::wiper_low},
  }};
  for (std::size_t index = 0; index < blink_bits.size(); ++index) {
    VehicleState one_bit_state{};
    const auto one_bit =
        frame(kBlinkInfoId, 310 + index,
              {0, 0, blink_bits[index].byte2_mask, 0, blink_bits[index].byte4_mask, 0, 0, 0});
    REQUIRE(decode_blink_info(one_bit, one_bit_state) == DecodeStatus::Decoded);
    CHECK((one_bit_state.*blink_bits[index].signal).value);
  }

  // D3 bits 2 and 3 are the left/right lamps; D5 bit 1 is WiperLow.
  const auto on = frame(kBlinkInfoId, 400, {0, 0, 0x0c, 0, 0x02, 0, 0, 0});
  REQUIRE(decode_blink_info(on, state) == DecodeStatus::Decoded);
  CHECK(state.left_indicator_lamp.value);
  CHECK(state.right_indicator_lamp.value);
  CHECK(state.wiper_low.value);
  CHECK(state.timestamp_us == 400);
  CHECK(state.right_indicator_lamp.last_update_us == 400);

  const auto late = frame(kBlinkInfoId, 399, {0, 0, 0, 0, 0, 0, 0, 0});
  CHECK(decode_blink_info(late, state) == DecodeStatus::Decoded);
  CHECK(state.left_indicator_lamp.value);
  CHECK(state.right_indicator_lamp.value);
  CHECK(state.wiper_low.value);
}

TEST_CASE("BRAKE_PEDAL decodes the confirmed pressed bit") {
  using namespace mazda;
  using namespace mazda::candidate;

  VehicleState state{};
  const auto released = frame(kBrakePedalId, 450, {0, 0, 0, 0, 0, 0, 0, 0});
  REQUIRE(decode_brake_pedal(released, state) == DecodeStatus::Decoded);
  CHECK_FALSE(state.brake_pressed.value);
  CHECK(state.brake_pressed.last_update_us == 450);

  const auto pressed = frame(kBrakePedalId, 451, {0x10, 0, 0, 0, 0, 0, 0, 0});
  REQUIRE(decode_brake_pedal(pressed, state) == DecodeStatus::Decoded);
  CHECK(state.brake_pressed.value);
  CHECK(state.brake_pressed.last_update_us == 451);
  CHECK(state.timestamp_us == 451);
}

TEST_CASE("TURN_SWITCH decodes all front-wiper enumeration values") {
  using namespace mazda;
  using namespace mazda::candidate;
  using namespace vehicle_core;

  VehicleState state{};
  const std::array<FrontWiperPosition, 4> expected{
      FrontWiperPosition::Off,
      FrontWiperPosition::On,
      FrontWiperPosition::High,
      FrontWiperPosition::Intermittent,
  };
  for (std::uint8_t raw = 0; raw < expected.size(); ++raw) {
    const auto input =
        frame(kTurnSwitchId, 500 + raw, {0, 0, static_cast<std::uint8_t>(raw << 4), 0, 0, 0, 0, 0});
    REQUIRE(decode_turn_switch(input, state) == DecodeStatus::Decoded);
    CHECK(state.front_wiper.value == expected[raw]);
    CHECK(state.front_wiper.last_update_us == 500 + raw);
  }
}

TEST_CASE("invalid and missing updates become stale on the same replay clock") {
  using namespace mazda::candidate;
  test_support::FakeClock clock;
  test_support::DirectFrameFeeder feeder;
  mazda::VehicleFreshnessPolicy policy{};
  policy.speed_kph_timeout_us = 100;
  policy.engine_rpm_timeout_us = 100;
  policy.selector_position_timeout_us = 100;
  policy.actual_gear_timeout_us = 100;
  mazda::VehicleStateStore store{clock, policy};
  feeder.feed(frame(kEngineDataId, 1000, {0x09, 0x5b, 0, 0, 0, 0, 0, 0}),
              [&](const vehicle_core::RawCanFrame &value) {
                clock.set(value.timestamp_us);
                (void)mazda::candidate::decode(value, store.mutable_state());
              });
  feeder.feed(frame(kGearId, 1100, {0x24, 0x81, 0x07, 0xff, 0x04, 0xf0, 0, 0}),
              [&](const vehicle_core::RawCanFrame &value) {
                clock.set(value.timestamp_us);
                (void)mazda::candidate::decode(value, store.mutable_state());
              });
  // A wrong-DLC frame is delivered directly and cannot replace accepted data.
  feeder.feed(frame(kEngineDataId, 1150, {0x84, 0xd1, 0, 0, 0, 0, 0}, 7),
              [&](const vehicle_core::RawCanFrame &value) {
                clock.set(value.timestamp_us);
                (void)mazda::candidate::decode(value, store.mutable_state());
              });
  // Advance the test clock to the end of the synthetic frame sequence after
  // all frames have been delivered.
  clock.set(std::numeric_limits<vehicle_core::MonotonicTimestamp>::max());
  REQUIRE(store.state().engine_rpm.is_valid());
  REQUIRE(store.state().actual_gear.is_valid());
  const auto stale = store.snapshot();
  CHECK(stale.engine_rpm.is_stale());
  CHECK(stale.speed_kph.is_stale());
  CHECK(stale.selector_position.is_stale());
  CHECK(stale.actual_gear.is_stale());
  CHECK(stale.engine_rpm.value == doctest::Approx(598.75F));
}
