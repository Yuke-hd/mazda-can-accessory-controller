#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <tuple>
#include <type_traits>

#include "mazda/definitions.hpp"
#include "mazda/signal_catalog.hpp"
#include "mazda/state.hpp"
#include "mazda/vehicle_telemetry_service.hpp"

namespace signals = mazda::internal::signals;
using vehicle_signals::SignalKind;
using vehicle_signals::SignalUnit;
using vehicle_signals::SignalValidation;

template <typename T>
void check_polling_binding(const mazda::internal::PollingDescriptor<T> &descriptor,
                           const vehicle_signals::SignalId signal_id,
                           const vehicle_core::Signal<T> mazda::VehicleState::*signal,
                           const std::uint32_t source_identifier,
                           const SignalValidation validation) {
  const auto *metadata = signals::catalog().find(signal_id);
  REQUIRE(metadata != nullptr);
  CHECK(descriptor.signal_id == metadata->id);
  CHECK(descriptor.signal == signal);
  CHECK(descriptor.identifier == source_identifier);
  CHECK(descriptor.validation == validation);
  CHECK(metadata->validation == validation);
  CHECK(metadata->kind == SignalKind::Number);
  CHECK(metadata->capabilities.readable);
  CHECK_FALSE(metadata->capabilities.subscribable);
  CHECK_FALSE(metadata->capabilities.writable);
}

template <typename T, std::uint16_t ChannelId>
void check_notification_binding(
    const mazda::internal::NotificationDescriptor<T, ChannelId> &descriptor,
    const vehicle_signals::SignalId signal_id,
    const vehicle_core::Signal<T> mazda::VehicleState::*signal,
    const std::uint32_t source_identifier, const SignalValidation validation,
    const std::uint16_t expected_channel_id) {
  const auto *metadata = signals::catalog().find(signal_id);
  REQUIRE(metadata != nullptr);
  using Channel = typename mazda::internal::NotificationDescriptor<T, ChannelId>::Channel;
  auto expected_kind = SignalKind::Number;
  if constexpr (std::is_same_v<T, bool>)
    expected_kind = SignalKind::Boolean;
  else if constexpr (std::is_enum_v<T>)
    expected_kind = SignalKind::Enum;
  CHECK(descriptor.signal_id == metadata->id);
  CHECK(descriptor.signal == signal);
  CHECK(descriptor.identifier == source_identifier);
  CHECK(descriptor.validation == validation);
  CHECK(metadata->validation == validation);
  CHECK(metadata->kind == expected_kind);
  if constexpr (std::is_same_v<T, bool>)
    CHECK(metadata->unit == SignalUnit::Boolean);
  else if constexpr (std::is_enum_v<T>)
    CHECK(metadata->unit == SignalUnit::None);
  CHECK(descriptor.channel != nullptr);
  CHECK(Channel::channel_id() == expected_channel_id);
  CHECK(metadata->capabilities.readable);
  CHECK(metadata->capabilities.subscribable);
  CHECK_FALSE(metadata->capabilities.writable);
}

TEST_CASE("Mazda catalog contains the exact ordered production key set") {
  constexpr std::array<std::string_view, signals::kSignalCount> expected_keys{{
      "vehicle.engine_rpm",
      "vehicle.speed_kph",
      "vehicle.turn_state",
      "vehicle.hazard_request",
      "vehicle.turn_request.left",
      "vehicle.turn_request.right",
      "vehicle.indicator_lamp.left",
      "vehicle.indicator_lamp.right",
      "vehicle.selector_position",
      "vehicle.actual_gear",
      "vehicle.liftgate_open",
      "vehicle.door.rear_right",
      "vehicle.door.rear_left",
      "vehicle.door.front_left_rhd",
      "vehicle.door.front_right_rhd",
      "vehicle.doors_unlocked",
      "vehicle.wiper.low",
      "vehicle.wiper.front_position",
  }};

  const auto catalog = signals::catalog();
  REQUIRE(catalog.size() == expected_keys.size());
  CHECK(catalog.find(vehicle_signals::SignalId{}) == nullptr);
  CHECK(catalog.find("vehicle.door.front_left") == nullptr);
  CHECK(catalog.find("vehicle.door.front_right") == nullptr);

  for (std::size_t i = 0; i < expected_keys.size(); ++i) {
    const auto *metadata = catalog.find(expected_keys[i]);
    REQUIRE(metadata != nullptr);
    CHECK(metadata == &signals::kMetadata[i]);
    CHECK(metadata->id.value == i + 1);
    CHECK(metadata->name == expected_keys[i]);
    CHECK(metadata->capabilities.readable);
    CHECK_FALSE(metadata->capabilities.writable);
    CHECK(metadata->capabilities.subscribable == (i >= 2));
  }
}

TEST_CASE("catalog preserves type unit and evidence metadata") {
  const auto catalog = signals::catalog();

  const auto *rpm = catalog.find("vehicle.engine_rpm");
  REQUIRE(rpm != nullptr);
  CHECK(rpm->kind == SignalKind::Number);
  CHECK(rpm->unit == SignalUnit::RevolutionsPerMinute);
  CHECK(rpm->validation == SignalValidation::Confirmed);
  CHECK(rpm->minimum == 0.0);
  CHECK(rpm->maximum == 8500.0);

  const auto *speed = catalog.find("vehicle.speed_kph");
  REQUIRE(speed != nullptr);
  CHECK(speed->kind == SignalKind::Number);
  CHECK(speed->unit == SignalUnit::KilometresPerHour);
  CHECK(speed->validation == SignalValidation::Reference);
  CHECK(speed->minimum == 0.0);
  CHECK_FALSE(speed->maximum.has_value());

  const auto *actual_gear = catalog.find("vehicle.actual_gear");
  REQUIRE(actual_gear != nullptr);
  CHECK(actual_gear->validation == SignalValidation::Observed);

  const auto *front_left = catalog.find("vehicle.door.front_left_rhd");
  const auto *front_right = catalog.find("vehicle.door.front_right_rhd");
  REQUIRE(front_left != nullptr);
  REQUIRE(front_right != nullptr);
  CHECK(front_left->validation == SignalValidation::Reference);
  CHECK(front_right->validation == SignalValidation::Confirmed);

  const auto *wiper_low = catalog.find("vehicle.wiper.low");
  const auto *front_wiper = catalog.find("vehicle.wiper.front_position");
  REQUIRE(wiper_low != nullptr);
  REQUIRE(front_wiper != nullptr);
  CHECK(wiper_low->validation == SignalValidation::Observed);
  CHECK(front_wiper->validation == SignalValidation::Observed);
}

template <std::size_t ChoiceCount>
void check_enum_choices(const vehicle_signals::SignalMetadata &metadata,
                        const std::array<vehicle_signals::SignalChoice, ChoiceCount> &expected) {
  REQUIRE(metadata.kind == SignalKind::Enum);
  REQUIRE(metadata.choices != nullptr);
  REQUIRE(metadata.choice_count == expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    CHECK(metadata.choices[i].value == expected[i].value);
    CHECK(metadata.choices[i].label == expected[i].label);
    for (std::size_t j = i + 1; j < expected.size(); ++j)
      CHECK(metadata.choices[i].value != metadata.choices[j].value);
  }
}

TEST_CASE("enum metadata choices are complete and unique for Mazda enums") {
  const auto catalog = signals::catalog();

  constexpr std::array<vehicle_signals::SignalChoice, 5> turn_state_choices{{
      {static_cast<std::int32_t>(mazda::TurnState::Unknown), "unknown"},
      {static_cast<std::int32_t>(mazda::TurnState::Off), "off"},
      {static_cast<std::int32_t>(mazda::TurnState::Left), "left"},
      {static_cast<std::int32_t>(mazda::TurnState::Right), "right"},
      {static_cast<std::int32_t>(mazda::TurnState::Hazard), "hazard"},
  }};
  constexpr std::array<vehicle_signals::SignalChoice, 6> selector_choices{{
      {static_cast<std::int32_t>(mazda::SelectorPosition::Unknown), "unknown"},
      {static_cast<std::int32_t>(mazda::SelectorPosition::Shifting), "shifting"},
      {static_cast<std::int32_t>(mazda::SelectorPosition::Park), "park"},
      {static_cast<std::int32_t>(mazda::SelectorPosition::Reverse), "reverse"},
      {static_cast<std::int32_t>(mazda::SelectorPosition::Neutral), "neutral"},
      {static_cast<std::int32_t>(mazda::SelectorPosition::Drive), "drive"},
  }};
  constexpr std::array<vehicle_signals::SignalChoice, 12> actual_gear_choices{{
      {static_cast<std::int32_t>(mazda::ActualGear::Unknown), "unknown"},
      {static_cast<std::int32_t>(mazda::ActualGear::ParkOrNeutral), "park_or_neutral"},
      {static_cast<std::int32_t>(mazda::ActualGear::Park), "park"},
      {static_cast<std::int32_t>(mazda::ActualGear::Neutral), "neutral"},
      {static_cast<std::int32_t>(mazda::ActualGear::Reverse), "reverse"},
      {static_cast<std::int32_t>(mazda::ActualGear::First), "first"},
      {static_cast<std::int32_t>(mazda::ActualGear::Second), "second"},
      {static_cast<std::int32_t>(mazda::ActualGear::Third), "third"},
      {static_cast<std::int32_t>(mazda::ActualGear::Fourth), "fourth"},
      {static_cast<std::int32_t>(mazda::ActualGear::Fifth), "fifth"},
      {static_cast<std::int32_t>(mazda::ActualGear::Sixth), "sixth"},
      {static_cast<std::int32_t>(mazda::ActualGear::Shifting), "shifting"},
  }};
  constexpr std::array<vehicle_signals::SignalChoice, 5> front_wiper_choices{{
      {static_cast<std::int32_t>(mazda::FrontWiperPosition::Unknown), "unknown"},
      {static_cast<std::int32_t>(mazda::FrontWiperPosition::Off), "off"},
      {static_cast<std::int32_t>(mazda::FrontWiperPosition::On), "on"},
      {static_cast<std::int32_t>(mazda::FrontWiperPosition::High), "high"},
      {static_cast<std::int32_t>(mazda::FrontWiperPosition::Intermittent), "intermittent"},
  }};

  const auto *turn_state = catalog.find("vehicle.turn_state");
  REQUIRE(turn_state != nullptr);
  check_enum_choices(*turn_state, turn_state_choices);

  const auto *selector = catalog.find("vehicle.selector_position");
  REQUIRE(selector != nullptr);
  check_enum_choices(*selector, selector_choices);

  const auto *gear = catalog.find("vehicle.actual_gear");
  REQUIRE(gear != nullptr);
  check_enum_choices(*gear, actual_gear_choices);

  const auto *front_wiper = catalog.find("vehicle.wiper.front_position");
  REQUIRE(front_wiper != nullptr);
  check_enum_choices(*front_wiper, front_wiper_choices);
}

TEST_CASE("production descriptors map IDs and host-only descriptors stay invalid") {
  using namespace mazda::internal;
  static_assert(std::tuple_size_v<PollingDescriptorTuple> == 3);
  static_assert(std::tuple_size_v<NotificationDescriptorTuple> == 17);

  const auto &polling = VehicleTelemetryService::polling_descriptors();
  check_polling_binding(std::get<0>(polling), signals::kSpeedKph, &mazda::VehicleState::speed_kph,
                        mazda::candidate::kEngineDataId, SignalValidation::Reference);
  check_polling_binding(std::get<1>(polling), signals::kEngineRpm, &mazda::VehicleState::engine_rpm,
                        mazda::candidate::kEngineDataId, SignalValidation::Confirmed);
  CHECK_FALSE(std::get<2>(polling).signal_id.valid());
  CHECK(std::get<2>(polling).signal == &mazda::VehicleState::front_wiper);

  const auto &notifications = VehicleTelemetryService::notification_descriptors();
  using namespace mazda::candidate;
  check_notification_binding(std::get<0>(notifications), signals::kSelectorPosition,
                             &mazda::VehicleState::selector_position, kGearId,
                             SignalValidation::Confirmed, kSelectorNotificationChannel);
  check_notification_binding(std::get<1>(notifications), signals::kActualGear,
                             &mazda::VehicleState::actual_gear, kGearId, SignalValidation::Observed,
                             kActualGearNotificationChannel);
  check_notification_binding(std::get<2>(notifications), signals::kTurnState,
                             &mazda::VehicleState::turn_state, kTurnSwitchId,
                             SignalValidation::Reference, kTurnNotificationChannel);
  check_notification_binding(std::get<3>(notifications), signals::kHazardRequest,
                             &mazda::VehicleState::hazard_request, kTurnSwitchId,
                             SignalValidation::Reference, kHazardNotificationChannel);
  check_notification_binding(std::get<4>(notifications), signals::kTurnRequestLeft,
                             &mazda::VehicleState::left_turn_request, kTurnSwitchId,
                             SignalValidation::Reference, kLeftTurnNotificationChannel);
  check_notification_binding(std::get<5>(notifications), signals::kTurnRequestRight,
                             &mazda::VehicleState::right_turn_request, kTurnSwitchId,
                             SignalValidation::Reference, kRightTurnNotificationChannel);
  check_notification_binding(std::get<6>(notifications), signals::kLiftgateOpen,
                             &mazda::VehicleState::liftgate_open, kDoorsId,
                             SignalValidation::Reference, kLiftgateNotificationChannel);
  check_notification_binding(std::get<7>(notifications), signals::kDoorRearRight,
                             &mazda::VehicleState::rear_right_door_open, kDoorsId,
                             SignalValidation::Reference, kRearRightDoorNotificationChannel);
  check_notification_binding(std::get<8>(notifications), signals::kDoorRearLeft,
                             &mazda::VehicleState::rear_left_door_open, kDoorsId,
                             SignalValidation::Reference, kRearLeftDoorNotificationChannel);
  check_notification_binding(std::get<9>(notifications), signals::kDoorFrontLeftRhd,
                             &mazda::VehicleState::front_left_door_open_rhd, kDoorsId,
                             SignalValidation::Reference, kFrontLeftDoorNotificationChannel);
  check_notification_binding(std::get<10>(notifications), signals::kDoorFrontRightRhd,
                             &mazda::VehicleState::front_right_door_open_rhd, kDoorsId,
                             SignalValidation::Confirmed, kFrontRightDoorNotificationChannel);
  check_notification_binding(std::get<11>(notifications), signals::kDoorsUnlocked,
                             &mazda::VehicleState::doors_unlocked, kDoorsId,
                             SignalValidation::Reference, kDoorsUnlockedNotificationChannel);
  check_notification_binding(std::get<12>(notifications), signals::kIndicatorLampLeft,
                             &mazda::VehicleState::left_indicator_lamp, kBlinkInfoId,
                             SignalValidation::Reference, kLeftLampNotificationChannel);
  check_notification_binding(std::get<13>(notifications), signals::kIndicatorLampRight,
                             &mazda::VehicleState::right_indicator_lamp, kBlinkInfoId,
                             SignalValidation::Reference, kRightLampNotificationChannel);
  check_notification_binding(std::get<14>(notifications), signals::kWiperLow,
                             &mazda::VehicleState::wiper_low, kBlinkInfoId,
                             SignalValidation::Observed, kWiperLowNotificationChannel);
  check_notification_binding(std::get<15>(notifications), signals::kFrontWiperPosition,
                             &mazda::VehicleState::front_wiper, kTurnSwitchId,
                             SignalValidation::Observed, kFrontWiperNotificationChannel);
  CHECK_FALSE(std::get<16>(notifications).signal_id.valid());
  CHECK(std::get<16>(notifications).signal == &mazda::VehicleState::front_wiper);
}
