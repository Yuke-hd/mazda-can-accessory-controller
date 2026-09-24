#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <type_traits>

#include "vehicle_signals/catalog.hpp"

using namespace vehicle_signals;

TEST_CASE("signal ids reserve zero and subscription tokens carry generations") {
  constexpr SignalId invalid{};
  constexpr SignalId speed{1};
  CHECK_FALSE(invalid.valid());
  CHECK(speed.valid());

  constexpr SignalSubscriptionToken absent{};
  constexpr SignalSubscriptionToken active{speed, 2, 7};
  CHECK_FALSE(absent.valid());
  CHECK(active.valid());
  CHECK(active != (SignalSubscriptionToken{speed, 2, 8}));
}

TEST_CASE("signal values preserve their boolean number and enum tags") {
  const auto enabled = SignalValue::boolean(false);
  const auto speed = SignalValue::number(42.5);
  const auto gear = SignalValue::enumeration(3);

  CHECK(enabled.kind() == SignalKind::Boolean);
  CHECK(enabled.as_boolean().has_value());
  CHECK_FALSE(*enabled.as_boolean());
  CHECK_FALSE(enabled.as_number().has_value());

  CHECK(speed.kind() == SignalKind::Number);
  REQUIRE(speed.as_number().has_value());
  CHECK(*speed.as_number() == doctest::Approx(42.5));
  CHECK_FALSE(speed.as_enum().has_value());

  CHECK(gear.kind() == SignalKind::Enum);
  CHECK(gear.as_enum() == 3);
  CHECK(SignalValue{}.kind() == SignalKind::Invalid);
}

TEST_CASE("catalog views borrow metadata and expose lookup and enum choices") {
  constexpr std::array<SignalChoice, 3> gear_choices{{
      {0, "park"},
      {1, "reverse"},
      {2, "drive"},
  }};
  const std::array<SignalMetadata, 2> metadata{{
      {SignalId{1},
       "vehicle.speed",
       "Vehicle speed",
       SignalKind::Number,
       SignalUnit::KilometresPerHour,
       SignalValidation::Reference,
       {true, true, false},
       0.0,
       300.0,
       nullptr,
       0},
      {SignalId{2},
       "transmission.gear",
       "Selected gear",
       SignalKind::Enum,
       SignalUnit::None,
       SignalValidation::Observed,
       {true, true, false},
       std::nullopt,
       std::nullopt,
       gear_choices.data(),
       gear_choices.size()},
  }};

  const SignalCatalogView catalog{metadata.data(), metadata.size()};
  REQUIRE(catalog.size() == 2);
  CHECK_FALSE(catalog.empty());
  CHECK(catalog.begin() == metadata.data());
  CHECK(catalog.end() == metadata.data() + metadata.size());
  CHECK(catalog.find(SignalId{}) == nullptr);
  CHECK(catalog.find(SignalId{99}) == nullptr);
  CHECK(catalog.find("vehicle.speed") == &metadata[0]);
  CHECK(catalog.find("missing.signal") == nullptr);

  const auto *speed = catalog.find(SignalId{1});
  REQUIRE(speed != nullptr);
  CHECK(speed->kind == SignalKind::Number);
  CHECK(speed->unit == SignalUnit::KilometresPerHour);
  CHECK(speed->minimum == 0.0);
  CHECK(speed->maximum == 300.0);
  CHECK_FALSE(speed->capabilities.writable);

  const auto *gear = catalog.find(SignalId{2});
  REQUIRE(gear != nullptr);
  REQUIRE(gear->choice_count == gear_choices.size());
  CHECK(gear->choices[2].value == 2);
  CHECK(gear->choices[2].label == "drive");
}

TEST_CASE("signal readings and notifications use vehicle core value contracts") {
  static_assert(std::is_same_v<SignalReading, vehicle_core::Reading<SignalValue>>);
  static_assert(std::is_same_v<SignalNotification, vehicle_core::Notification<SignalValue>>);
  static_assert(std::is_same_v<SignalAvailability, vehicle_core::Availability>);
  static_assert(std::is_same_v<SignalValidation, vehicle_core::ValidationStatus>);

  SignalReading reading{};
  reading.value = SignalValue::boolean(true);
  reading.availability = SignalAvailability::Fresh;
  reading.validation = SignalValidation::Observed;

  SignalNotification notification{};
  notification.current = reading;
  notification.recovered = true;
  REQUIRE(notification.current.value.has_value());
  CHECK(notification.current.value->as_boolean() == true);
  CHECK(notification.current.availability == SignalAvailability::Fresh);
  CHECK(notification.recovered);
}
