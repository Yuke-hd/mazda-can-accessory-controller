#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>

#include "../support/fake_clock.hpp"
#include "mazda/signal_provider.hpp"
#include "mazda/vehicle_telemetry_internal.hpp"
#include "mazda/vehicle_telemetry_service.hpp"

namespace {

using vehicle_signals::SignalId;
using vehicle_signals::SignalNotification;

class QuietLightingSink final : public mazda::internal::LightingSink {
public:
  [[nodiscard]] bool publish(const mazda::LightingUpdate &) noexcept override { return true; }
};

struct GenericRecorder final {
  mutable std::mutex mutex{};
  mutable std::condition_variable changed{};
  std::array<SignalNotification, 16> notices{};
  std::array<SignalId, 16> ids{};
  std::size_t count{0};
};

void record_generic(void *context, const SignalId signal,
                    const SignalNotification &notice) noexcept {
  auto &recorder = *static_cast<GenericRecorder *>(context);
  {
    std::lock_guard<std::mutex> lock{recorder.mutex};
    if (recorder.count < recorder.notices.size()) {
      recorder.ids[recorder.count] = signal;
      recorder.notices[recorder.count] = notice;
      ++recorder.count;
    }
  }
  recorder.changed.notify_all();
}

template <typename T> struct TypedRecorder final {
  mutable std::mutex mutex{};
  mutable std::condition_variable changed{};
  std::array<vehicle_core::Notification<T>, 16> notices{};
  std::size_t count{0};
};

template <typename T>
void record_typed(void *context, const vehicle_core::Notification<T> &notice) noexcept {
  auto &recorder = *static_cast<TypedRecorder<T> *>(context);
  {
    std::lock_guard<std::mutex> lock{recorder.mutex};
    if (recorder.count < recorder.notices.size())
      recorder.notices[recorder.count++] = notice;
  }
  recorder.changed.notify_all();
}

template <typename Recorder>
bool wait_for_count(const Recorder &recorder, const std::size_t count) {
  std::unique_lock<std::mutex> lock{recorder.mutex};
  return recorder.changed.wait_for(lock, std::chrono::seconds{2},
                                   [&recorder, count] { return recorder.count >= count; });
}

template <typename Recorder> auto notice_at(const Recorder &recorder, const std::size_t index) {
  std::lock_guard<std::mutex> lock{recorder.mutex};
  return recorder.notices[index];
}

SignalId id_for(const vehicle_signals::SignalCatalogView catalog, const char *key) {
  const auto *metadata = catalog.find(key);
  REQUIRE(metadata != nullptr);
  return metadata->id;
}

vehicle_core::RawCanFrame frame(const std::uint32_t identifier,
                                const vehicle_core::MonotonicTimestamp timestamp_us,
                                const std::array<std::uint8_t, 8> &bytes,
                                const std::uint8_t dlc = 8) {
  vehicle_core::RawCanFrame result{};
  result.identifier = identifier;
  result.timestamp_us = timestamp_us;
  result.dlc = dlc;
  result.data = bytes;
  return result;
}

void check_generic_matches_typed(const SignalNotification &generic,
                                 const vehicle_core::Notification<mazda::TurnState> &typed) {
  CHECK(generic.current.availability == typed.current.availability);
  CHECK(generic.current.validation == typed.current.validation);
  CHECK(generic.initial == typed.initial);
  CHECK(generic.became_unavailable == typed.became_unavailable);
  CHECK(generic.recovered == typed.recovered);
  CHECK(generic.coalesced == typed.coalesced);
  CHECK(generic.current.value.has_value() == typed.current.value.has_value());
  if (generic.current.value.has_value() && typed.current.value.has_value()) {
    CHECK(generic.current.value->kind() == vehicle_signals::SignalKind::Enum);
    CHECK(generic.current.value->as_enum() == static_cast<std::int32_t>(*typed.current.value));
  }
}

} // namespace

TEST_CASE("generic consumer resolves keys and matches typed Mazda telemetry") {
  test_support::FakeClock clock;
  mazda::internal::HostAcquisitionSource source;
  QuietLightingSink lighting;
  mazda::TelemetryConfig config{};
  config.callback_stop_timeout_us = 100'000;
  mazda::internal::VehicleTelemetryService service{clock, source, lighting, config};

  GenericRecorder turn_generic{};
  GenericRecorder left_request_generic{};
  GenericRecorder left_lamp_generic{};
  GenericRecorder front_wiper_generic{};
  TypedRecorder<mazda::TurnState> turn_typed{};
  TypedRecorder<bool> left_request_typed{};
  TypedRecorder<bool> left_lamp_typed{};
  TypedRecorder<mazda::FrontWiperPosition> front_wiper_typed{};

  // This is the complete consumer-side discovery path: the caller depends on
  // generic contracts, resolves stable keys, then uses IDs for reads/notices.
  auto provider = mazda::internal::VehicleTelemetryAccess::for_host_test(service);
  const auto catalog = provider.catalog();
  const auto rpm_id = id_for(catalog, "vehicle.engine_rpm");
  const auto turn_id = id_for(catalog, "vehicle.turn_state");
  const auto left_request_id = id_for(catalog, "vehicle.turn_request.left");
  const auto left_lamp_id = id_for(catalog, "vehicle.indicator_lamp.left");
  const auto front_wiper_id = id_for(catalog, "vehicle.wiper.front_position");
  CHECK(catalog.find("vehicle.door.front_left") == nullptr);

  const auto turn_subscription = provider.subscribe(turn_id, &record_generic, &turn_generic);
  const auto left_request_subscription =
      provider.subscribe(left_request_id, &record_generic, &left_request_generic);
  const auto left_lamp_subscription =
      provider.subscribe(left_lamp_id, &record_generic, &left_lamp_generic);
  const auto front_wiper_subscription =
      provider.subscribe(front_wiper_id, &record_generic, &front_wiper_generic);
  REQUIRE(turn_subscription.ok());
  REQUIRE(left_request_subscription.ok());
  REQUIRE(left_lamp_subscription.ok());
  REQUIRE(front_wiper_subscription.ok());

  const auto typed_turn = service.subscribe_turn(&record_typed<mazda::TurnState>, &turn_typed);
  const auto typed_left_request =
      service.subscribe_left_turn(&record_typed<bool>, &left_request_typed);
  const auto typed_left_lamp = service.subscribe_left_lamp(&record_typed<bool>, &left_lamp_typed);
  const auto typed_front_wiper =
      service.subscribe_front_wiper(&record_typed<mazda::FrontWiperPosition>, &front_wiper_typed);
  REQUIRE(typed_turn.ok());
  REQUIRE(typed_left_request.ok());
  REQUIRE(typed_left_lamp.ok());
  REQUIRE(typed_front_wiper.ok());

  // An invalid key differs from a valid key, which the catalog resolves.
  CHECK(provider.read({}).status == vehicle_signals::SignalStatus::InvalidSignal);

  REQUIRE(service.start().ok());
  REQUIRE(wait_for_count(turn_generic, 1));
  REQUIRE(wait_for_count(left_request_generic, 1));
  REQUIRE(wait_for_count(left_lamp_generic, 1));
  REQUIRE(wait_for_count(front_wiper_generic, 1));
  REQUIRE(wait_for_count(turn_typed, 1));
  REQUIRE(wait_for_count(left_request_typed, 1));
  REQUIRE(wait_for_count(left_lamp_typed, 1));
  REQUIRE(wait_for_count(front_wiper_typed, 1));
  // Before any sample, the running service reports a valid NoData reading.
  const auto empty_rpm = provider.read(rpm_id);
  REQUIRE(empty_rpm.status == vehicle_signals::SignalStatus::Ok);
  CHECK(empty_rpm.reading.availability == vehicle_core::Availability::NoData);
  CHECK_FALSE(empty_rpm.reading.value.has_value());
  CHECK(notice_at(turn_generic, 0).initial);
  CHECK(notice_at(turn_generic, 0).current.availability == vehicle_core::Availability::NoData);
  check_generic_matches_typed(notice_at(turn_generic, 0), notice_at(turn_typed, 0));

  // Feed a generated engine frame through the injected acquisition source.
  // The production runtime, decoder and publication store own all processing.
  clock.set(100);
  REQUIRE(source.inject(frame(mazda::candidate::kEngineDataId, 100,
                              {{0x09, 0x5b, 0x01, 0xf4, 0, 0, 0, 0}})) == mazda::ResultCode::Ok);
  const auto rpm_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
  while (std::chrono::steady_clock::now() < rpm_deadline && !service.engine_rpm().value.has_value())
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  REQUIRE(service.engine_rpm().value.has_value());
  const auto generic_rpm = provider.read(catalog.find("vehicle.engine_rpm")->id);
  const auto typed_rpm = service.engine_rpm();
  REQUIRE(generic_rpm.status == vehicle_signals::SignalStatus::Ok);
  REQUIRE(generic_rpm.reading.value.has_value());
  CHECK(generic_rpm.reading.value->kind() == vehicle_signals::SignalKind::Number);
  CHECK(generic_rpm.reading.value->as_number() == doctest::Approx(*typed_rpm.value));
  CHECK(generic_rpm.reading.availability == typed_rpm.availability);
  CHECK(generic_rpm.reading.validation == typed_rpm.validation);
  CHECK(*generic_rpm.reading.value->as_number() == doctest::Approx(598.75));

  // TURN_SWITCH carries a left request, normalized turn state and wiper enum.
  // None is inferred from the later BlinkInfo lamp frame below.
  clock.set(200);
  REQUIRE(source.inject(frame(mazda::candidate::kTurnSwitchId, 200,
                              {{0, 0x20, 0x10, 0, 0, 0, 0, 0}})) == mazda::ResultCode::Ok);
  REQUIRE(wait_for_count(turn_generic, 2));
  REQUIRE(wait_for_count(turn_typed, 2));
  REQUIRE(wait_for_count(left_request_generic, 2));
  REQUIRE(wait_for_count(left_request_typed, 2));
  REQUIRE(wait_for_count(front_wiper_generic, 2));
  REQUIRE(wait_for_count(front_wiper_typed, 2));

  const auto generic_turn_notice = notice_at(turn_generic, 1);
  const auto typed_turn_notice = notice_at(turn_typed, 1);
  check_generic_matches_typed(generic_turn_notice, typed_turn_notice);
  REQUIRE(generic_turn_notice.current.value.has_value());
  CHECK(generic_turn_notice.current.value->as_enum() ==
        static_cast<std::int32_t>(mazda::TurnState::Left));
  const auto generic_request_notice = notice_at(left_request_generic, 1);
  const auto typed_request_notice = notice_at(left_request_typed, 1);
  REQUIRE(generic_request_notice.current.value.has_value());
  REQUIRE(typed_request_notice.current.value.has_value());
  CHECK(generic_request_notice.current.value->as_boolean() == typed_request_notice.current.value);
  CHECK(generic_request_notice.current.validation == typed_request_notice.current.validation);
  CHECK(generic_request_notice.current.availability == typed_request_notice.current.availability);
  const auto generic_wiper_notice = notice_at(front_wiper_generic, 1);
  const auto typed_wiper_notice = notice_at(front_wiper_typed, 1);
  REQUIRE(generic_wiper_notice.current.value.has_value());
  REQUIRE(typed_wiper_notice.current.value.has_value());
  CHECK(generic_wiper_notice.current.value->as_enum() ==
        static_cast<std::int32_t>(*typed_wiper_notice.current.value));
  CHECK(generic_wiper_notice.current.validation == typed_wiper_notice.current.validation);
  CHECK(generic_wiper_notice.current.availability == typed_wiper_notice.current.availability);

  const auto generic_turn_read = provider.read(turn_id);
  REQUIRE(generic_turn_read.ok());
  REQUIRE(generic_turn_read.reading.value.has_value());
  CHECK(generic_turn_read.reading.value->as_enum() ==
        static_cast<std::int32_t>(mazda::TurnState::Left));
  CHECK(generic_turn_read.reading.availability == typed_turn_notice.current.availability);
  CHECK(generic_turn_read.reading.validation == typed_turn_notice.current.validation);

  // BLINK_INFO sets the left lamp independently. The active left request and
  // normalized turn state remain true/Left when only the lamp frame changes.
  clock.set(300);
  REQUIRE(source.inject(frame(mazda::candidate::kBlinkInfoId, 300,
                              {{0, 0, 0x04, 0, 0x02, 0, 0, 0}})) == mazda::ResultCode::Ok);
  REQUIRE(wait_for_count(left_lamp_generic, 2));
  REQUIRE(wait_for_count(left_lamp_typed, 2));
  const auto generic_lamp_notice = notice_at(left_lamp_generic, 1);
  const auto typed_lamp_notice = notice_at(left_lamp_typed, 1);
  REQUIRE(generic_lamp_notice.current.value.has_value());
  REQUIRE(typed_lamp_notice.current.value.has_value());
  CHECK(generic_lamp_notice.current.value->as_boolean() == typed_lamp_notice.current.value);
  CHECK(generic_lamp_notice.current.value->as_boolean() == true);
  CHECK(generic_lamp_notice.current.availability == typed_lamp_notice.current.availability);
  CHECK(generic_lamp_notice.current.validation == typed_lamp_notice.current.validation);
  CHECK(provider.read(turn_id).reading.value->as_enum() ==
        static_cast<std::int32_t>(mazda::TurnState::Left));
  CHECK(notice_at(left_request_generic, 1).current.value->as_boolean() == true);

  // A malformed TURN_SWITCH marks related values unavailable while retaining
  // their last accepted values. Generic and typed observers must report the
  // same notice flags and evidence.
  clock.set(400);
  REQUIRE(source.inject(frame(mazda::candidate::kTurnSwitchId, 400, {{0, 0, 0, 0, 0, 0, 0, 0}},
                              7)) == mazda::ResultCode::Ok);
  REQUIRE(wait_for_count(turn_generic, 3));
  REQUIRE(wait_for_count(turn_typed, 3));
  REQUIRE(wait_for_count(left_request_generic, 3));
  REQUIRE(wait_for_count(left_request_typed, 3));
  const auto unavailable_generic = notice_at(turn_generic, 2);
  const auto unavailable_typed = notice_at(turn_typed, 2);
  check_generic_matches_typed(unavailable_generic, unavailable_typed);
  CHECK(unavailable_generic.current.availability == vehicle_core::Availability::Unavailable);
  CHECK(unavailable_generic.became_unavailable);
  CHECK(unavailable_generic.current.value->as_enum() ==
        static_cast<std::int32_t>(mazda::TurnState::Left));
  CHECK(provider.read(turn_id).reading.availability == vehicle_core::Availability::Unavailable);
  CHECK(provider.read(turn_id).reading.value->as_enum() ==
        static_cast<std::int32_t>(mazda::TurnState::Left));

  // A newer valid request recovers the same value and emits matching recovery
  // evidence through both callback surfaces.
  clock.set(500);
  REQUIRE(source.inject(frame(mazda::candidate::kTurnSwitchId, 500,
                              {{0, 0x20, 0x10, 0, 0, 0, 0, 0}})) == mazda::ResultCode::Ok);
  REQUIRE(wait_for_count(turn_generic, 4));
  REQUIRE(wait_for_count(turn_typed, 4));
  const auto recovered_generic = notice_at(turn_generic, 3);
  const auto recovered_typed = notice_at(turn_typed, 3);
  check_generic_matches_typed(recovered_generic, recovered_typed);
  CHECK(recovered_generic.recovered);
  CHECK(recovered_generic.current.availability == vehicle_core::Availability::Fresh);

  // Callback contexts stay alive through successful stop. Unsubscription is
  // permitted only after workers are quiescent.
  REQUIRE(service.stop().ok());
  CHECK(provider.unsubscribe(turn_subscription.token) == vehicle_signals::SignalStatus::Ok);
  CHECK(provider.unsubscribe(left_request_subscription.token) == vehicle_signals::SignalStatus::Ok);
  CHECK(provider.unsubscribe(left_lamp_subscription.token) == vehicle_signals::SignalStatus::Ok);
  CHECK(provider.unsubscribe(front_wiper_subscription.token) == vehicle_signals::SignalStatus::Ok);
  CHECK(service.unsubscribe(typed_turn).ok());
  CHECK(service.unsubscribe(typed_left_request).ok());
  CHECK(service.unsubscribe(typed_left_lamp).ok());
  CHECK(service.unsubscribe(typed_front_wiper).ok());
}
