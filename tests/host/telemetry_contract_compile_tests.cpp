#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <type_traits>

#include "mazda/vehicle_telemetry.hpp"
#include "vehicle_core/notification.hpp"
#include "vehicle_core/reading.hpp"

namespace {
void turn_callback(void *, const mazda::Notification<mazda::TurnState> &) noexcept {}
void bool_callback(void *, const mazda::Notification<bool> &) noexcept {}

template <typename T>
using SubscriptionFn = mazda::Result<mazda::Subscription> (mazda::VehicleTelemetry::*)(
    mazda::Callback<T>, void *) noexcept;
} // namespace

TEST_CASE("telemetry contracts are value-copy and callback ABI seams") {
  static_assert(std::is_trivially_copyable_v<vehicle_core::Reading<float>>);
  static_assert(std::is_trivially_copyable_v<vehicle_core::Notification<bool>>);
  static_assert(mazda::kNotifySubscribersPerChannel == 2);

  mazda::Reading<float> speed{};
  speed.value = 12.5F;
  speed.availability = mazda::Availability::FreshnessUnverified;
  speed.validation = mazda::ValidationStatus::Observed;
  CHECK(*speed.value == doctest::Approx(12.5F));

  mazda::Notification<mazda::TurnState> notice{};
  notice.current = mazda::Reading<mazda::TurnState>{
      mazda::TurnState::Left, mazda::Availability::Fresh, mazda::ValidationStatus::Confirmed};
  notice.initial = true;
  notice.became_unavailable = true;
  notice.recovered = true;
  notice.coalesced = true;
  CHECK(notice.current.value == mazda::TurnState::Left);
  CHECK(notice.initial);

  const mazda::Callback<mazda::TurnState> turn = &turn_callback;
  const mazda::Callback<bool> boolean = &bool_callback;
  CHECK(turn != nullptr);
  CHECK(boolean != nullptr);
}

TEST_CASE("facade exposes fixed polling and notify channels without implementation") {
  using Facade = mazda::VehicleTelemetry;
  static_assert(!std::is_copy_constructible_v<Facade>);
  static_assert(!std::is_move_constructible_v<Facade>);
  static_assert(std::is_nothrow_default_constructible_v<Facade>);
  static_assert(std::is_nothrow_destructible_v<Facade>);

  using ConfigureFn = mazda::StatusResult (Facade::*)(const mazda::TelemetryConfig &) noexcept;
  using LifecycleFn = mazda::StatusResult (Facade::*)() noexcept;
  using PollingFn = mazda::Reading<float> (Facade::*)() const noexcept;
  using UnsubscribeFn = mazda::StatusResult (Facade::*)(mazda::Subscription) noexcept;
  using DiagnosticsFn = mazda::Diagnostics (Facade::*)() const noexcept;
  static_assert(std::is_same_v<decltype(&Facade::configure), ConfigureFn>);
  static_assert(std::is_same_v<decltype(&Facade::start), LifecycleFn>);
  static_assert(std::is_same_v<decltype(&Facade::stop), LifecycleFn>);
  static_assert(std::is_same_v<decltype(&Facade::speed_kph), PollingFn>);
  static_assert(std::is_same_v<decltype(&Facade::engine_rpm), PollingFn>);
  static_assert(std::is_same_v<decltype(&Facade::unsubscribe), UnsubscribeFn>);
  static_assert(std::is_same_v<decltype(&Facade::diagnostics), DiagnosticsFn>);

  static_assert(std::is_same_v<decltype(&Facade::on_selector_position_changed),
                               SubscriptionFn<mazda::SelectorPosition>>);
  static_assert(
      std::is_same_v<decltype(&Facade::on_actual_gear_changed), SubscriptionFn<mazda::ActualGear>>);
  static_assert(
      std::is_same_v<decltype(&Facade::on_turn_state_changed), SubscriptionFn<mazda::TurnState>>);
  static_assert(std::is_same_v<decltype(&Facade::on_hazard_request_changed), SubscriptionFn<bool>>);
  static_assert(
      std::is_same_v<decltype(&Facade::on_left_turn_request_changed), SubscriptionFn<bool>>);
  static_assert(
      std::is_same_v<decltype(&Facade::on_right_turn_request_changed), SubscriptionFn<bool>>);
  static_assert(std::is_same_v<decltype(&Facade::on_liftgate_open_changed), SubscriptionFn<bool>>);
  static_assert(
      std::is_same_v<decltype(&Facade::on_rear_right_door_open_changed), SubscriptionFn<bool>>);
  static_assert(
      std::is_same_v<decltype(&Facade::on_rear_left_door_open_changed), SubscriptionFn<bool>>);
  static_assert(
      std::is_same_v<decltype(&Facade::on_front_left_door_open_rhd_changed), SubscriptionFn<bool>>);
  static_assert(std::is_same_v<decltype(&Facade::on_front_right_door_open_rhd_changed),
                               SubscriptionFn<bool>>);
  static_assert(std::is_same_v<decltype(&Facade::on_doors_unlocked_changed), SubscriptionFn<bool>>);
  static_assert(
      std::is_same_v<decltype(&Facade::on_left_indicator_lamp_changed), SubscriptionFn<bool>>);
  static_assert(
      std::is_same_v<decltype(&Facade::on_right_indicator_lamp_changed), SubscriptionFn<bool>>);
  static_assert(std::is_same_v<decltype(&Facade::on_wiper_low_changed), SubscriptionFn<bool>>);
  static_assert(std::is_same_v<decltype(&Facade::on_front_wiper_changed),
                               SubscriptionFn<mazda::FrontWiperPosition>>);

  CHECK(mazda::TelemetryConfig{}.transport_silence_timeout_us == 1'000'000);
  CHECK(mazda::TelemetryConfig{}.callback_stop_timeout_us == 500'000);

  const mazda::AcquisitionMetrics acquisition{};
  CHECK(acquisition.controller_resets == 0);
  CHECK(acquisition.bus_off_events == 0);
}
