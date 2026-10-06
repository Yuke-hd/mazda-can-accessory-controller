#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
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

TEST_CASE("0x078 acceleration frame is decoded as a supported message") {
  mazda::VehicleState state{};
  CHECK(mazda::candidate::decode(frame(0x078, 100, {0x1f, 0x41, 0, 0, 0, 0, 0, 0}), state) ==
        mazda::candidate::DecodeStatus::Decoded);
}

TEST_CASE("0x078 Motorola acceleration vectors retain SI scale offsets and Reference confidence") {
  using namespace mazda::candidate;
  CHECK(kAccelerationDefinition.identifier == 0x078);
  CHECK(kAccelerationDefinition.expected_dlc == 8);
  CHECK(kAccelerationDefinition.pending_validation);
  CHECK_FALSE(kAccelerationDefinition.expected_period_us.has_value());
  CHECK_FALSE(kAccelerationDefinition.freshness_timeout_us.has_value());
  CHECK(kLongitudinalAccelerationDefinition.dbc_start_bit == 5);
  CHECK(kLateralAccelerationDefinition.dbc_start_bit == 8);
  CHECK(kLongitudinalAccelerationDefinition.bit_length == 13);
  CHECK(kLateralAccelerationDefinition.bit_length == 13);
  CHECK(kLongitudinalAccelerationDefinition.confidence == mazda::ValidationStatus::Reference);
  CHECK(kLateralAccelerationDefinition.confidence == mazda::ValidationStatus::Reference);
  CHECK(std::string{kAccelerationDefinition.provenance}.find("95f3d52") != std::string::npos);

  struct Vector {
    std::array<std::uint8_t, 8> payload;
    float longitudinal;
    float lateral;
  };
  // Literal byte vectors exercise sawtooth boundaries without sharing the
  // production extractor or an encoder helper.
  const std::array vectors{
      Vector{{0, 0, 0, 0, 0, 0, 0, 0}, -40.0F, -4.096F},
      Vector{{0x1f, 0x41, 0, 0, 0, 0, 0, 0}, 0.0F, 0.0F},
      Vector{{0x17, 0x70, 0xbb, 0x80, 0, 0, 0, 0}, -10.0F, -1.096F},
      Vector{{0x27, 0x11, 0x38, 0x80, 0, 0, 0, 0}, 10.0F, 0.904F},
      Vector{{0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}, 41.91F, 4.095F},
  };
  for (const auto &vector : vectors) {
    mazda::VehicleState state{};
    auto observation = frame(0x078, 0, {});
    observation.data = vector.payload;
    REQUIRE(decode(observation, state) == DecodeStatus::Decoded);
    CHECK(state.longitudinal_acceleration_mps2.value == doctest::Approx(vector.longitudinal));
    CHECK(state.lateral_acceleration_mps2.value == doctest::Approx(vector.lateral));
    CHECK(state.longitudinal_acceleration_mps2.has_value);
    CHECK(state.lateral_acceleration_mps2.has_value);
    CHECK_FALSE(state.longitudinal_acceleration_mps2.freshness_timeout_us.has_value());
    CHECK_FALSE(state.lateral_acceleration_mps2.freshness_timeout_us.has_value());
    CHECK_FALSE(state.brake_pressed.has_value);
  }
}

TEST_CASE("0x078 malformed observations fault only acceleration and recover strictly newer") {
  using namespace mazda::candidate;
  mazda::VehicleState state{};
  for (const auto identifier :
       {kEngineDataId, kTransmissionId, kDoorsId, kBlinkInfoId, kTurnSwitchId, kBrakePedalId}) {
    REQUIRE(decode(frame(identifier, 100, {0x10}), state) == DecodeStatus::Decoded);
  }
  REQUIRE(state.brake_pressed.value);
  const auto neutral = frame(kAccelerationId, 100, {0x1f, 0x41});
  REQUIRE(decode(neutral, state) == DecodeStatus::Decoded);
  REQUIRE(state.message_health_for(kAccelerationId) != nullptr);

  for (const auto dlc : {0, 7, 9}) {
    auto malformed = frame(kAccelerationId, 200 + dlc, {0xff, 0xff, 0xff, 0xff}, dlc);
    CHECK(decode(malformed, state) == DecodeStatus::Malformed);
    CHECK(state.longitudinal_acceleration_mps2.value == doctest::Approx(0.0F));
    CHECK(state.lateral_acceleration_mps2.value == doctest::Approx(0.0F));
    CHECK(state.longitudinal_acceleration_mps2.last_update_us == 100);
    CHECK(state.lateral_acceleration_mps2.last_update_us == 100);
    CHECK(state.status_at(state.longitudinal_acceleration_mps2, kAccelerationId, 300) ==
          mazda::Availability::Unavailable);
    CHECK(state.status_at(state.brake_pressed, kBrakePedalId, 300) ==
          mazda::Availability::FreshnessUnverified);
  }
  auto older = neutral;
  older.timestamp_us = 150;
  CHECK(decode(older, state) == DecodeStatus::Decoded);
  CHECK(state.message_health_for(kAccelerationId)->health == vehicle_core::MessageHealth::Faulted);
  auto equal = neutral;
  equal.timestamp_us = 209;
  CHECK(decode(equal, state) == DecodeStatus::Decoded);
  CHECK(state.message_health_for(kAccelerationId)->health == vehicle_core::MessageHealth::Faulted);
  auto newer = neutral;
  newer.timestamp_us = 210;
  CHECK(decode(newer, state) == DecodeStatus::Decoded);
  CHECK(state.message_health_for(kAccelerationId)->health == vehicle_core::MessageHealth::Healthy);
  CHECK(state.longitudinal_acceleration_mps2.last_update_us == 210);
  CHECK(state.lateral_acceleration_mps2.last_update_us == 210);
}

TEST_CASE("0x078 ignores remote and extended frames and preserves equal-time first observation") {
  using namespace mazda::candidate;
  mazda::VehicleState state{};
  auto neutral = frame(kAccelerationId, 100, {0x1f, 0x41});
  neutral.remote_request = true;
  CHECK(decode(neutral, state) == DecodeStatus::Ignored);
  CHECK_FALSE(state.longitudinal_acceleration_mps2.has_value);
  neutral.remote_request = false;
  neutral.identifier_format = vehicle_core::CanIdentifierFormat::Extended;
  CHECK(decode(neutral, state) == DecodeStatus::Ignored);
  CHECK_FALSE(state.longitudinal_acceleration_mps2.has_value);
  neutral.identifier_format = vehicle_core::CanIdentifierFormat::Standard;
  REQUIRE(decode(neutral, state) == DecodeStatus::Decoded);
  REQUIRE(decode(neutral, state) == DecodeStatus::Decoded);
  auto conflict = frame(kAccelerationId, 100, {0x27, 0x11, 0x38, 0x80});
  CHECK(decode(conflict, state) == DecodeStatus::Decoded);
  conflict.timestamp_us = 99;
  CHECK(decode(conflict, state) == DecodeStatus::Decoded);
  CHECK(state.longitudinal_acceleration_mps2.value == doctest::Approx(0.0F));
  CHECK(state.lateral_acceleration_mps2.value == doctest::Approx(0.0F));
  neutral.dlc = 7;
  CHECK(decode(neutral, state) == DecodeStatus::Malformed);
  CHECK(state.message_health_for(kAccelerationId)->health == vehicle_core::MessageHealth::Healthy);
}

TEST_CASE("0x078 snapshots copy availability evidence and independent optional freshness") {
  using namespace mazda::candidate;
  mazda::VehicleState state{};
  CHECK(state.status_at(state.longitudinal_acceleration_mps2, kAccelerationId, 0) ==
        mazda::Availability::NoData);
  REQUIRE(decode(frame(kAccelerationId, 100, {0x27, 0x11, 0x38, 0x80}), state) ==
          DecodeStatus::Decoded);
  const auto copy = state.snapshot(1000);
  const auto reading = copy.reading_at(copy.longitudinal_acceleration_mps2, kAccelerationId, 1000,
                                       kLongitudinalAccelerationDefinition.confidence);
  REQUIRE(reading.value.has_value());
  CHECK(*reading.value == doctest::Approx(10.0F));
  CHECK(reading.validation == mazda::ValidationStatus::Reference);
  CHECK(reading.availability == mazda::Availability::FreshnessUnverified);
  CHECK(copy.message_health_for(kAccelerationId)->last_accepted_us == 100);
  CHECK(copy.longitudinal_acceleration_mps2.is_stale());
  CHECK(state.longitudinal_acceleration_mps2.is_valid());
  mazda::VehicleFreshnessPolicy policy{};
  policy.longitudinal_acceleration_mps2_timeout_us = 10;
  policy.lateral_acceleration_mps2_timeout_us = 20;
  const auto boundary = state.snapshot(110, policy);
  CHECK(boundary.status_at(boundary.longitudinal_acceleration_mps2, kAccelerationId, 110) ==
        mazda::Availability::Fresh);
  const auto aged = state.snapshot(111, policy);
  CHECK(aged.status_at(aged.longitudinal_acceleration_mps2, kAccelerationId, 111) ==
        mazda::Availability::Stale);
  CHECK(aged.status_at(aged.lateral_acceleration_mps2, kAccelerationId, 111) ==
        mazda::Availability::Fresh);
  CHECK(aged.status_at(aged.lateral_acceleration_mps2, kAccelerationId, 111,
                       vehicle_core::TransportHealth::Stopped) == mazda::Availability::Unavailable);
}

TEST_CASE("0x078 neutral and adjacent codes round only the centred SI value") {
  using namespace mazda::candidate;
  struct Vector {
    std::array<std::uint8_t, 8> payload;
    float longitudinal;
    float lateral;
  };
  const std::array vectors{
      Vector{{0x1f, 0x41, 0x00, 0x00, 0, 0, 0, 0}, 0.0F, 0.0F},
      Vector{{0x1f, 0x43, 0x00, 0x10, 0, 0, 0, 0}, 0.01F, 0.001F},
      Vector{{0x1f, 0x3e, 0xff, 0xf0, 0, 0, 0, 0}, -0.01F, -0.001F},
      Vector{{0, 0, 0, 0, 0, 0, 0, 0}, -40.0F, -4.096F},
      Vector{{0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}, 41.91F, 4.095F},
  };
  for (const auto &vector : vectors) {
    mazda::VehicleState state{};
    auto sample = frame(kAccelerationId, 100, {});
    sample.data = vector.payload;
    REQUIRE(decode(sample, state) == DecodeStatus::Decoded);
    CHECK(state.longitudinal_acceleration_mps2.value == vector.longitudinal);
    CHECK(state.lateral_acceleration_mps2.value == vector.lateral);
    CHECK(std::isfinite(state.longitudinal_acceleration_mps2.value));
    CHECK(std::isfinite(state.lateral_acceleration_mps2.value));
  }
}

TEST_CASE(
    "0x078 ignored extended and remote attempts preserve accepted message and caller transport") {
  using namespace mazda::candidate;
  for (const auto extended : {false, true}) {
    mazda::VehicleState state{};
    REQUIRE(decode(frame(kAccelerationId, 100, {0x27, 0x11, 0x38, 0x80}), state) ==
            DecodeStatus::Decoded);
    const auto accepted = *state.message_health_for(kAccelerationId);
    auto ignored = frame(kAccelerationId, 200, {0x1f, 0x41});
    ignored.bus_id = 2;
    ignored.remote_request = !extended;
    ignored.identifier_format = extended ? vehicle_core::CanIdentifierFormat::Extended
                                         : vehicle_core::CanIdentifierFormat::Standard;
    vehicle_core::DecoderObservation observation{};
    vehicle_core::HealthObservation health{};
    health.transport = vehicle_core::TransportHealth::TimedOut;
    REQUIRE(decode(ignored, state, nullptr, &observation, &health) == DecodeStatus::Ignored);
    CHECK(observation.validity == DecodeStatus::Ignored);
    CHECK(observation.identifier == kAccelerationId);
    CHECK(observation.timestamp_us == 200);
    CHECK(observation.bus_id == 2);
    CHECK(observation.dlc == 8);
    CHECK(health.transport == vehicle_core::TransportHealth::TimedOut);
    CHECK(health.signal == vehicle_core::SignalHealth::Unavailable);
    CHECK(health.message == vehicle_core::MessageHealth::Healthy);
    CHECK(health.has_last_frame);
    CHECK(health.last_frame_us == 100);
    CHECK(health.has_last_accepted);
    CHECK(health.last_accepted_us == 100);
    CHECK_FALSE(health.fault_timestamp_us.has_value());
    const auto retained = *state.message_health_for(kAccelerationId);
    CHECK(retained.data == accepted.data);
    CHECK(retained.identifier_format == accepted.identifier_format);
    CHECK(retained.remote_request == accepted.remote_request);
    CHECK(retained.bus_id == accepted.bus_id);
    CHECK(state.longitudinal_acceleration_mps2.value == 10.0F);
    CHECK(state.longitudinal_acceleration_mps2.last_update_us == 100);
    CHECK(state.lateral_acceleration_mps2.last_update_us == 100);
    CHECK(state.timestamp_us == 100);
    CHECK(
        state.reading_at(state.longitudinal_acceleration_mps2, kAccelerationId, 200).availability ==
        mazda::Availability::FreshnessUnverified);
  }
}

TEST_CASE("0x078 attempt health distinguishes conflicts duplicates and newer fault recovery") {
  using namespace mazda::candidate;
  mazda::VehicleState state{};
  const auto accepted = frame(kAccelerationId, 100, {0x27, 0x11, 0x38, 0x80});
  vehicle_core::DecoderObservation observation{};
  vehicle_core::HealthObservation health{};
  health.transport = vehicle_core::TransportHealth::Live;
  REQUIRE(decode(accepted, state, nullptr, &observation, &health) == DecodeStatus::Decoded);
  REQUIRE(health.signal == vehicle_core::SignalHealth::Available);

  const auto conflict = frame(kAccelerationId, 100, {0x1f, 0x41});
  REQUIRE(decode(conflict, state, nullptr, &observation, &health) == DecodeStatus::Decoded);
  CHECK(observation.validity == DecodeStatus::Decoded);
  CHECK(health.signal == vehicle_core::SignalHealth::Unavailable);
  CHECK(health.message == vehicle_core::MessageHealth::Healthy);
  CHECK(health.transport == vehicle_core::TransportHealth::Live);
  CHECK(health.last_frame_us == 100);
  CHECK(health.last_accepted_us == 100);
  CHECK_FALSE(health.fault_timestamp_us.has_value());
  const auto retained = state.snapshot(200);
  CHECK(retained.longitudinal_acceleration_mps2.value == 10.0F);
  CHECK(retained.lateral_acceleration_mps2.value == 0.904F);
  CHECK(retained.reading_at(retained.longitudinal_acceleration_mps2, kAccelerationId, 200)
            .availability == mazda::Availability::FreshnessUnverified);
  // A consumer of this attempted observation can conservatively reject it.
  CHECK(mazda::status_at(state.longitudinal_acceleration_mps2, 200, health) ==
        mazda::Availability::Unavailable);
  REQUIRE(decode(accepted, state, nullptr, &observation, &health) == DecodeStatus::Decoded);
  CHECK(health.signal == vehicle_core::SignalHealth::Available);

  auto malformed = accepted;
  malformed.timestamp_us = 200;
  malformed.dlc = 7;
  REQUIRE(decode(malformed, state, nullptr, &observation, &health) == DecodeStatus::Malformed);
  CHECK(observation.validity == DecodeStatus::Malformed);
  CHECK(health.message == vehicle_core::MessageHealth::Faulted);
  CHECK(health.signal == vehicle_core::SignalHealth::Unavailable);
  CHECK(health.last_frame_us == 200);
  CHECK(health.last_accepted_us == 100);
  REQUIRE(health.fault_timestamp_us.has_value());
  CHECK(*health.fault_timestamp_us == 200);
  auto equal_recovery = accepted;
  equal_recovery.timestamp_us = 200;
  REQUIRE(decode(equal_recovery, state, nullptr, &observation, &health) == DecodeStatus::Decoded);
  CHECK(health.message == vehicle_core::MessageHealth::Faulted);
  CHECK(health.signal == vehicle_core::SignalHealth::Unavailable);
  equal_recovery.timestamp_us = 201;
  REQUIRE(decode(equal_recovery, state, nullptr, &observation, &health) == DecodeStatus::Decoded);
  CHECK(health.message == vehicle_core::MessageHealth::Healthy);
  CHECK(health.signal == vehicle_core::SignalHealth::Available);
  CHECK(health.last_frame_us == 201);
  CHECK(health.last_accepted_us == 201);
  CHECK_FALSE(health.fault_timestamp_us.has_value());
}
