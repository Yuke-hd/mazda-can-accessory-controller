#include "mazda/definitions.hpp"
#include "mazda/signal_provider.hpp"
#include "mazda/vehicle_telemetry_internal.hpp"
#include "mazda/vehicle_telemetry_service.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <mutex>
#include <optional>
#include <thread>

namespace {

int failures = 0;

void check(const bool condition, const char *expression, const char *file, const int line) {
  if (!condition) {
    std::cerr << file << ':' << line << ": failed: " << expression << '\n';
    ++failures;
  }
}

#define CHECK(condition) check((condition), #condition, __FILE__, __LINE__)

class TestClock final : public vehicle_core::MonotonicClock {
public:
  [[nodiscard]] vehicle_core::MonotonicTimestamp now() const noexcept override {
    return now_us_.load(std::memory_order_relaxed);
  }

  void set(const vehicle_core::MonotonicTimestamp now_us) noexcept {
    now_us_.store(now_us, std::memory_order_relaxed);
  }

private:
  std::atomic<vehicle_core::MonotonicTimestamp> now_us_{0};
};

mazda::TelemetryConfig test_config() {
  mazda::TelemetryConfig config{};
  config.transport_silence_timeout_us = 1'000'000;
  config.callback_stop_timeout_us = 10'000;
  return config;
}

struct Fixture final {
  TestClock clock{};
  mazda::internal::HostRuntimeSource source{};
  mazda::internal::NullLightingSink lighting{};
  mazda::TelemetryConfig config{test_config()};
  mazda::internal::VehicleTelemetryService service{clock, source, lighting, config};
  mazda::MazdaSignalProvider provider{
      mazda::internal::VehicleTelemetryAccess::for_host_test(service)};
};

struct Recorder final {
  mutable std::mutex mutex{};
  mutable std::condition_variable changed{};
  std::array<std::array<vehicle_signals::SignalNotification, 8>, 19> notices{};
  std::array<std::size_t, 19> counts{};
  bool block_first{false};
  bool block_next_update{false};
  bool entered_block{false};
  bool release_block{false};
  bool context_alive{true};
  bool callback_saw_dead_context{false};
};

void record_signal(void *context, const vehicle_signals::SignalId signal,
                   const vehicle_signals::SignalNotification &notice) noexcept {
  auto &recorder = *static_cast<Recorder *>(context);
  std::unique_lock<std::mutex> lock{recorder.mutex};
  if (!recorder.context_alive)
    recorder.callback_saw_dead_context = true;

  if (signal.value < recorder.counts.size()) {
    const std::size_t index = recorder.counts[signal.value];
    if (index < recorder.notices[signal.value].size())
      recorder.notices[signal.value][index] = notice;
    ++recorder.counts[signal.value];
  }

  const bool should_block =
      !recorder.entered_block &&
      ((recorder.block_first) || (recorder.block_next_update && !notice.initial));
  if (should_block) {
    recorder.entered_block = true;
    recorder.changed.notify_all();
    recorder.changed.wait(lock, [&recorder] { return recorder.release_block; });
  }
  recorder.changed.notify_all();
}

void ignore_turn(void *, const mazda::Notification<mazda::TurnState> &) noexcept {}

bool wait_for_count(const Recorder &recorder, const std::uint16_t signal, const std::size_t count,
                    const std::chrono::milliseconds timeout = std::chrono::milliseconds{500}) {
  std::unique_lock<std::mutex> lock{recorder.mutex};
  return recorder.changed.wait_for(
      lock, timeout, [&recorder, signal, count] { return recorder.counts[signal] >= count; });
}

bool wait_for_all_initial_notices(const Recorder &recorder) {
  std::unique_lock<std::mutex> lock{recorder.mutex};
  return recorder.changed.wait_for(lock, std::chrono::milliseconds{500}, [&recorder] {
    for (std::uint16_t signal = 3; signal <= 18; ++signal) {
      if (recorder.counts[signal] == 0)
        return false;
    }
    return true;
  });
}

bool wait_for_signal_value(const mazda::MazdaSignalProvider &provider,
                           const vehicle_signals::SignalId signal, const std::int32_t expected) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{500};
  while (std::chrono::steady_clock::now() < deadline) {
    const auto result = provider.read(signal);
    if (result.ok() && result.reading.value.has_value() &&
        result.reading.value->as_enum().value_or(-1) == expected)
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  return false;
}

vehicle_core::RawCanFrame frame(const std::uint32_t identifier,
                                const vehicle_core::MonotonicTimestamp timestamp_us,
                                const std::initializer_list<std::uint8_t> bytes) {
  vehicle_core::RawCanFrame result{};
  result.identifier = identifier;
  result.timestamp_us = timestamp_us;
  result.dlc = static_cast<std::uint8_t>(bytes.size());
  std::size_t index = 0;
  for (const auto byte : bytes)
    result.data[index++] = byte;
  return result;
}

void test_all_notify_ids_route_and_preserve_initial_notification() {
  Fixture fixture;
  Recorder recorder{};
  std::array<vehicle_signals::SignalSubscriptionToken, 16> subscriptions{};

  CHECK(fixture.provider.subscribe({0}, &record_signal, &recorder).status ==
        vehicle_signals::SignalStatus::InvalidSignal);
  CHECK(fixture.provider.subscribe({99}, &record_signal, &recorder).status ==
        vehicle_signals::SignalStatus::InvalidSignal);
  CHECK(fixture.provider.subscribe({1}, &record_signal, &recorder).status ==
        vehicle_signals::SignalStatus::UnsupportedCapability);
  CHECK(fixture.provider.subscribe({2}, &record_signal, &recorder).status ==
        vehicle_signals::SignalStatus::UnsupportedCapability);
  CHECK(fixture.provider.subscribe({3}, nullptr, &recorder).status ==
        vehicle_signals::SignalStatus::InvalidCallback);

  for (std::uint16_t signal = 3; signal <= 18; ++signal) {
    const auto result = fixture.provider.subscribe({signal}, &record_signal, &recorder);
    CHECK(result.ok());
    if (result.ok())
      subscriptions[signal - 3] = result.token;
  }

  CHECK(fixture.service.start().ok());
  CHECK(wait_for_all_initial_notices(recorder));
  {
    std::lock_guard<std::mutex> lock{recorder.mutex};
    for (std::uint16_t signal = 3; signal <= 18; ++signal) {
      CHECK(recorder.counts[signal] == 1);
      const auto &notice = recorder.notices[signal][0];
      CHECK(notice.initial);
      CHECK(!notice.became_unavailable);
      CHECK(!notice.recovered);
      CHECK(!notice.coalesced);
      CHECK(!notice.current.value.has_value());
    }
  }
  CHECK(fixture.service.stop().ok());

  for (const auto token : subscriptions) {
    CHECK(fixture.provider.unsubscribe(token) == vehicle_signals::SignalStatus::Ok);
    CHECK(fixture.provider.unsubscribe(token) ==
          vehicle_signals::SignalStatus::InvalidSubscription);
  }
}

void test_generic_and_typed_users_share_channel_slots_and_generations() {
  Fixture fixture;
  Recorder recorder{};

  const auto typed = fixture.service.subscribe_turn(&ignore_turn, nullptr);
  CHECK(typed.ok());
  const auto first = fixture.provider.subscribe({3}, &record_signal, &recorder);
  CHECK(first.ok());
  CHECK(first.token.slot != typed.slot);
  CHECK(fixture.provider.subscribe({3}, &record_signal, &recorder).status ==
        vehicle_signals::SignalStatus::CapacityExceeded);

  CHECK(fixture.provider.unsubscribe(first.token) == vehicle_signals::SignalStatus::Ok);
  CHECK(fixture.provider.unsubscribe(first.token) ==
        vehicle_signals::SignalStatus::InvalidSubscription);
  const auto reused = fixture.provider.subscribe({3}, &record_signal, &recorder);
  CHECK(reused.ok());
  CHECK(reused.token.slot == first.token.slot);
  CHECK(reused.token.generation != first.token.generation);
  CHECK(fixture.provider.unsubscribe(first.token) ==
        vehicle_signals::SignalStatus::InvalidSubscription);
  CHECK(fixture.provider.unsubscribe(reused.token) == vehicle_signals::SignalStatus::Ok);
  CHECK(fixture.service.unsubscribe(typed).ok());
}

void test_restart_reseeds_initial_and_running_unsubscribe_is_rejected() {
  Fixture fixture;
  Recorder recorder{};
  const auto subscribed = fixture.provider.subscribe({3}, &record_signal, &recorder);
  CHECK(subscribed.ok());

  CHECK(fixture.service.start().ok());
  CHECK(wait_for_count(recorder, 3, 1));
  CHECK(fixture.provider.unsubscribe(subscribed.token) ==
        vehicle_signals::SignalStatus::InvalidState);
  CHECK(fixture.service.stop().ok());

  CHECK(fixture.service.start().ok());
  CHECK(wait_for_count(recorder, 3, 2));
  {
    std::lock_guard<std::mutex> lock{recorder.mutex};
    CHECK(recorder.notices[3][0].initial);
    CHECK(recorder.notices[3][1].initial);
  }
  CHECK(fixture.service.stop().ok());
  CHECK(fixture.provider.unsubscribe(subscribed.token) == vehicle_signals::SignalStatus::Ok);
}

void test_provider_destruction_after_successful_stop_removes_channel_records() {
  TestClock clock;
  mazda::internal::HostRuntimeSource source;
  mazda::internal::NullLightingSink lighting;
  const auto config = test_config();
  mazda::internal::VehicleTelemetryService service{clock, source, lighting, config};
  Recorder recorder{};

  {
    mazda::MazdaSignalProvider provider{
        mazda::internal::VehicleTelemetryAccess::for_host_test(service)};
    const auto subscribed = provider.subscribe({3}, &record_signal, &recorder);
    CHECK(subscribed.ok());
    CHECK(service.start().ok());
    CHECK(wait_for_count(recorder, 3, 1));
    CHECK(service.stop().ok());
  }

  {
    std::lock_guard<std::mutex> lock{recorder.mutex};
    recorder.context_alive = false;
  }
  CHECK(service.start().ok());
  std::this_thread::sleep_for(std::chrono::milliseconds{20});
  CHECK(service.stop().ok());
  {
    std::lock_guard<std::mutex> lock{recorder.mutex};
    CHECK(recorder.counts[3] == 1);
    CHECK(!recorder.callback_saw_dead_context);
  }
}

void test_generic_notification_coalesces_and_stop_timeout_keeps_context_borrowed() {
  Fixture fixture;
  Recorder recorder{};
  recorder.block_next_update = true;
  const auto subscribed = fixture.provider.subscribe({3}, &record_signal, &recorder);
  CHECK(subscribed.ok());
  CHECK(fixture.service.start().ok());
  CHECK(wait_for_count(recorder, 3, 1));

  fixture.clock.set(10);
  CHECK(fixture.source.inject(frame(mazda::candidate::kTurnSwitchId, 10,
                                    {0, 0x20, 0, 0, 0, 0, 0, 0})) == mazda::ResultCode::Ok);
  {
    std::unique_lock<std::mutex> lock{recorder.mutex};
    CHECK(recorder.changed.wait_for(lock, std::chrono::milliseconds{500},
                                    [&recorder] { return recorder.entered_block; }));
  }

  fixture.clock.set(11);
  CHECK(fixture.source.inject(frame(mazda::candidate::kTurnSwitchId, 11,
                                    {0, 0x10, 0, 0, 0, 0, 0, 0})) == mazda::ResultCode::Ok);
  fixture.clock.set(12);
  CHECK(fixture.source.inject(frame(mazda::candidate::kTurnSwitchId, 12,
                                    {0, 0x04, 0, 0, 0, 0, 0, 0})) == mazda::ResultCode::Ok);
  CHECK(wait_for_signal_value(fixture.provider, {3},
                              static_cast<std::int32_t>(mazda::TurnState::Hazard)));

  {
    std::lock_guard<std::mutex> lock{recorder.mutex};
    recorder.release_block = true;
  }
  recorder.changed.notify_all();
  CHECK(wait_for_count(recorder, 3, 3));
  {
    std::lock_guard<std::mutex> lock{recorder.mutex};
    CHECK(recorder.notices[3][2].current.value.has_value());
    CHECK(recorder.notices[3][2].current.value->as_enum().value_or(-1) ==
          static_cast<std::int32_t>(mazda::TurnState::Hazard));
    CHECK(recorder.notices[3][2].coalesced);
  }

  // A callback still using its borrowed context makes the first stop time out.
  // Keep the context alive, release that invocation, then require a successful
  // stop before clearing the registration or ending the borrow.
  {
    std::lock_guard<std::mutex> lock{recorder.mutex};
    recorder.block_first = true;
    recorder.entered_block = false;
    recorder.release_block = false;
  }
  // Trigger another callback and hold it while stop waits for callback quiescence.
  fixture.clock.set(13);
  CHECK(fixture.source.inject(frame(mazda::candidate::kTurnSwitchId, 13,
                                    {0, 0x20, 0, 0, 0, 0, 0, 0})) == mazda::ResultCode::Ok);
  {
    std::unique_lock<std::mutex> lock{recorder.mutex};
    CHECK(recorder.changed.wait_for(lock, std::chrono::milliseconds{500},
                                    [&recorder] { return recorder.entered_block; }));
  }
  CHECK(fixture.service.stop().status == mazda::ResultCode::Timeout);
  {
    std::lock_guard<std::mutex> lock{recorder.mutex};
    CHECK(recorder.context_alive);
    recorder.release_block = true;
  }
  recorder.changed.notify_all();
  CHECK(fixture.service.stop().ok());
  CHECK(fixture.provider.unsubscribe(subscribed.token) == vehicle_signals::SignalStatus::Ok);
  {
    std::lock_guard<std::mutex> lock{recorder.mutex};
    recorder.context_alive = false;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds{5});
  {
    std::lock_guard<std::mutex> lock{recorder.mutex};
    CHECK(!recorder.callback_saw_dead_context);
  }
}

} // namespace

int main() {
  test_all_notify_ids_route_and_preserve_initial_notification();
  test_generic_and_typed_users_share_channel_slots_and_generations();
  test_restart_reseeds_initial_and_running_unsubscribe_is_rejected();
  test_provider_destruction_after_successful_stop_removes_channel_records();
  test_generic_notification_coalesces_and_stop_timeout_keeps_context_borrowed();
  return failures == 0 ? 0 : 1;
}
