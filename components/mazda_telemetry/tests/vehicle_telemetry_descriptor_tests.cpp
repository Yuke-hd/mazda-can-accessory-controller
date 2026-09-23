#include "mazda/vehicle_telemetry_service.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <thread>
#include <tuple>
#include <type_traits>

namespace {

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

class TestLightingSink final : public mazda::internal::LightingSink {
public:
  [[nodiscard]] bool publish(const mazda::LightingUpdate &) noexcept override { return true; }
};

struct WiperRecorder final {
  std::mutex mutex{};
  std::condition_variable changed{};
  mazda::Notification<mazda::FrontWiperPosition> last{};
  std::size_t count{0};
};

void record_wiper(void *context,
                  const mazda::Notification<mazda::FrontWiperPosition> &notice) noexcept {
  auto &recorder = *static_cast<WiperRecorder *>(context);
  {
    std::lock_guard<std::mutex> lock{recorder.mutex};
    recorder.last = notice;
    ++recorder.count;
  }
  recorder.changed.notify_all();
}

vehicle_core::RawCanFrame frame(const std::uint32_t identifier,
                                const vehicle_core::MonotonicTimestamp timestamp_us,
                                std::initializer_list<std::uint8_t> bytes) {
  vehicle_core::RawCanFrame result{};
  result.identifier = identifier;
  result.timestamp_us = timestamp_us;
  result.dlc = static_cast<std::uint8_t>(bytes.size());
  std::size_t index = 0;
  for (const auto byte : bytes)
    result.data[index++] = byte;
  return result;
}

template <typename Predicate>
bool wait_for(Predicate predicate,
              const std::chrono::milliseconds timeout = std::chrono::milliseconds{500}) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  return predicate();
}

} // namespace

int main() {
  static_assert(std::tuple_size_v<mazda::internal::PollingDescriptorTuple> == 3);
  static_assert(std::tuple_size_v<mazda::internal::NotificationDescriptorTuple> ==
                mazda::internal::kNotificationChannelCount);
  using TestNotificationDescriptor =
      std::tuple_element_t<16, mazda::internal::NotificationDescriptorTuple>;
  static_assert(TestNotificationDescriptor::Channel::channel_id() ==
                mazda::internal::kTestFrontWiperNotificationChannel);

  TestClock clock;
  mazda::internal::HostAcquisitionSource source;
  TestLightingSink lighting;
  mazda::TelemetryConfig config{};
  config.callback_stop_timeout_us = 20'000;
  mazda::internal::VehicleTelemetryService service{clock, source, lighting, config};

  const auto &polling_descriptors = mazda::internal::VehicleTelemetryService::polling_descriptors();
  const auto &notification_descriptors =
      mazda::internal::VehicleTelemetryService::notification_descriptors();
  if (std::strcmp(std::get<0>(polling_descriptors).name, "speed_kph") != 0 ||
      std::strcmp(std::get<1>(polling_descriptors).name, "engine_rpm") != 0 ||
      std::strcmp(std::get<2>(polling_descriptors).name, "test_front_wiper") != 0 ||
      std::strcmp(std::get<15>(notification_descriptors).name, "front_wiper") != 0 ||
      std::strcmp(std::get<16>(notification_descriptors).name, "test_front_wiper") != 0 ||
      std::get<16>(notification_descriptors).channel == nullptr)
    return 1;

  // Both records are test-only extensions of the same fixed registries. Their
  // own channel/member and signal metadata are consumed by unchanged workers.
  const auto test_polling_descriptor = std::get<2>(polling_descriptors);
  const auto test_notify_descriptor = std::get<16>(notification_descriptors);
  WiperRecorder recorder{};
  const auto subscription =
      service.subscribe_notification_descriptor(test_notify_descriptor, &record_wiper, &recorder);
  if (!subscription.ok())
    return 2;
  if (!service.start().ok())
    return 3;

  if (!wait_for([&recorder] {
        std::lock_guard<std::mutex> lock{recorder.mutex};
        return recorder.count >= 1;
      }))
    return 4;

  clock.set(10);
  if (source.inject(frame(mazda::candidate::kEngineDataId, 10, {0x09, 0x5b, 0, 0, 0, 0, 0, 0})) !=
      mazda::ResultCode::Ok)
    return 5;

  clock.set(11);
  if (source.inject(frame(mazda::candidate::kTurnSwitchId, 11, {0, 0, 0x10, 0, 0, 0, 0, 0})) !=
      mazda::ResultCode::Ok)
    return 6;
  if (!wait_for([&service, &test_polling_descriptor] {
        return service.read_polling_descriptor(test_polling_descriptor).value ==
               mazda::FrontWiperPosition::On;
      }))
    return 7;
  if (!wait_for([&recorder] {
        std::lock_guard<std::mutex> lock{recorder.mutex};
        return recorder.count >= 2;
      }))
    return 8;

  {
    std::lock_guard<std::mutex> lock{recorder.mutex};
    if (recorder.last.current.value != mazda::FrontWiperPosition::On)
      return 9;
  }

  if (!service.stop().ok())
    return 10;
  if (!service.unsubscribe(subscription).ok())
    return 11;
  return 0;
}
