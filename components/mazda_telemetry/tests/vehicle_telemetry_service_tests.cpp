#include "mazda/vehicle_telemetry.hpp"

#include "mazda/definitions.hpp"
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
#include <thread>

namespace {

class FakeClock final : public vehicle_core::MonotonicClock {
public:
  [[nodiscard]] vehicle_core::MonotonicTimestamp now() const noexcept override {
    std::unique_lock<std::mutex> lock{gate_mutex_};
    if (pause_reads_) {
      read_paused_ = true;
      gate_changed_.notify_all();
      gate_changed_.wait(lock, [this] { return !pause_reads_; });
    }
    return now_us_.load(std::memory_order_relaxed);
  }

  void set(const vehicle_core::MonotonicTimestamp value) noexcept {
    now_us_.store(value, std::memory_order_relaxed);
  }

  void pause_reads() noexcept {
    std::lock_guard<std::mutex> lock{gate_mutex_};
    pause_reads_ = true;
    read_paused_ = false;
  }

  [[nodiscard]] bool wait_until_read_paused(
      const std::chrono::milliseconds timeout = std::chrono::milliseconds{500}) const noexcept {
    std::unique_lock<std::mutex> lock{gate_mutex_};
    return gate_changed_.wait_for(lock, timeout, [this] { return read_paused_; });
  }

  void resume_reads() noexcept {
    {
      std::lock_guard<std::mutex> lock{gate_mutex_};
      pause_reads_ = false;
    }
    gate_changed_.notify_all();
  }

private:
  std::atomic<vehicle_core::MonotonicTimestamp> now_us_{0};
  mutable std::mutex gate_mutex_{};
  mutable std::condition_variable gate_changed_{};
  bool pause_reads_{false};
  mutable bool read_paused_{false};
};

class FakeLightingSink final : public mazda::internal::LightingSink {
public:
  [[nodiscard]] bool publish(const mazda::LightingUpdate &update) noexcept override {
    std::lock_guard<std::mutex> lock{mutex_};
    if (size_ < updates_.size())
      updates_[size_++] = update;
    const bool accepted = !fail_next_;
    fail_next_ = false;
    return accepted;
  }

  void fail_next() noexcept {
    std::lock_guard<std::mutex> lock{mutex_};
    fail_next_ = true;
  }

  [[nodiscard]] std::size_t size() const noexcept {
    std::lock_guard<std::mutex> lock{mutex_};
    return size_;
  }

  [[nodiscard]] mazda::LightingUpdate at(const std::size_t index) const noexcept {
    std::lock_guard<std::mutex> lock{mutex_};
    return updates_[index];
  }

private:
  mutable std::mutex mutex_{};
  std::array<mazda::LightingUpdate, 32> updates_{};
  std::size_t size_{0};
  bool fail_next_{false};
};

class RestartableSource final : public vehicle_telemetry::AcquisitionSource {
public:
  [[nodiscard]] vehicle_telemetry::StatusResult start() noexcept override {
    std::lock_guard<std::mutex> lock{mutex_};
    if (running_)
      return {vehicle_telemetry::ResultCode::AlreadyRunning};
    if (fail_next_start_) {
      fail_next_start_ = false;
      return {vehicle_telemetry::ResultCode::Faulted};
    }
    running_ = true;
    return {vehicle_telemetry::ResultCode::Ok};
  }

  [[nodiscard]] vehicle_telemetry::StatusResult stop() noexcept override {
    std::lock_guard<std::mutex> lock{mutex_};
    if (!running_)
      return {vehicle_telemetry::ResultCode::NotRunning};
    running_ = false;
    return {vehicle_telemetry::ResultCode::Ok};
  }

  [[nodiscard]] vehicle_telemetry::ReceiveStatus
  receive(vehicle_core::RawCanFrame &, const std::uint32_t timeout_ms) noexcept override {
    {
      std::lock_guard<std::mutex> lock{mutex_};
      if (!running_)
        return vehicle_telemetry::ReceiveStatus::NotStarted;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{timeout_ms});
    return vehicle_telemetry::ReceiveStatus::Timeout;
  }

  [[nodiscard]] vehicle_telemetry::AcquisitionStatistics statistics() const noexcept override {
    std::lock_guard<std::mutex> lock{mutex_};
    return statistics_;
  }

  void fail_next_start() noexcept {
    std::lock_guard<std::mutex> lock{mutex_};
    fail_next_start_ = true;
  }

private:
  mutable std::mutex mutex_{};
  vehicle_telemetry::AcquisitionStatistics statistics_{};
  bool running_{false};
  bool fail_next_start_{false};
};

class PartialOwnershipSource final : public vehicle_telemetry::AcquisitionSource {
public:
  [[nodiscard]] vehicle_telemetry::StatusResult start() noexcept override {
    std::lock_guard<std::mutex> lock{mutex_};
    if (running_)
      return {vehicle_telemetry::ResultCode::AlreadyRunning};
    running_ = true;
    if (fail_next_start_) {
      fail_next_start_ = false;
      return {vehicle_telemetry::ResultCode::Faulted};
    }
    return {vehicle_telemetry::ResultCode::Ok};
  }

  [[nodiscard]] vehicle_telemetry::StatusResult stop() noexcept override {
    std::lock_guard<std::mutex> lock{mutex_};
    ++stop_calls_;
    if (!running_)
      return {vehicle_telemetry::ResultCode::NotRunning};
    if (fail_next_stop_) {
      fail_next_stop_ = false;
      return {vehicle_telemetry::ResultCode::Faulted};
    }
    running_ = false;
    return {vehicle_telemetry::ResultCode::Ok};
  }

  [[nodiscard]] vehicle_telemetry::ReceiveStatus
  receive(vehicle_core::RawCanFrame &, const std::uint32_t timeout_ms) noexcept override {
    {
      std::lock_guard<std::mutex> lock{mutex_};
      if (!running_)
        return vehicle_telemetry::ReceiveStatus::NotStarted;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{timeout_ms});
    return vehicle_telemetry::ReceiveStatus::Timeout;
  }

  [[nodiscard]] vehicle_telemetry::AcquisitionStatistics statistics() const noexcept override {
    std::lock_guard<std::mutex> lock{mutex_};
    return statistics_;
  }

  void fail_next_start_with_ownership() noexcept {
    std::lock_guard<std::mutex> lock{mutex_};
    fail_next_start_ = true;
  }

  void fail_next_stop() noexcept {
    std::lock_guard<std::mutex> lock{mutex_};
    fail_next_stop_ = true;
  }

  [[nodiscard]] std::size_t stop_calls() const noexcept {
    std::lock_guard<std::mutex> lock{mutex_};
    return stop_calls_;
  }

  [[nodiscard]] bool running() const noexcept {
    std::lock_guard<std::mutex> lock{mutex_};
    return running_;
  }

private:
  mutable std::mutex mutex_{};
  vehicle_telemetry::AcquisitionStatistics statistics_{};
  std::size_t stop_calls_{0};
  bool running_{false};
  bool fail_next_start_{false};
  bool fail_next_stop_{false};
};

class ImmediateFaultSource final : public vehicle_telemetry::AcquisitionSource {
public:
  [[nodiscard]] vehicle_telemetry::StatusResult start() noexcept override {
    std::lock_guard<std::mutex> lock{mutex_};
    if (running_)
      return {vehicle_telemetry::ResultCode::AlreadyRunning};
    running_ = true;
    return {vehicle_telemetry::ResultCode::Ok};
  }

  [[nodiscard]] vehicle_telemetry::StatusResult stop() noexcept override {
    std::lock_guard<std::mutex> lock{mutex_};
    running_ = false;
    return {vehicle_telemetry::ResultCode::Ok};
  }

  [[nodiscard]] vehicle_telemetry::ReceiveStatus receive(vehicle_core::RawCanFrame &,
                                                         std::uint32_t) noexcept override {
    std::lock_guard<std::mutex> lock{mutex_};
    if (!running_)
      return vehicle_telemetry::ReceiveStatus::NotStarted;
    fault_started_.store(true, std::memory_order_release);
    ++statistics_.driver_errors;
    return vehicle_telemetry::ReceiveStatus::Fault;
  }

  [[nodiscard]] vehicle_telemetry::AcquisitionStatistics statistics() const noexcept override {
    if (fault_started_.load(std::memory_order_acquire)) {
      while (!release_statistics_.load(std::memory_order_acquire))
        std::this_thread::yield();
    }
    std::lock_guard<std::mutex> lock{mutex_};
    return statistics_;
  }

  [[nodiscard]] bool fault_started() const noexcept {
    return fault_started_.load(std::memory_order_acquire);
  }

  void release_statistics() noexcept { release_statistics_.store(true, std::memory_order_release); }

private:
  mutable std::mutex mutex_{};
  vehicle_telemetry::AcquisitionStatistics statistics_{};
  std::atomic<bool> fault_started_{false};
  std::atomic<bool> release_statistics_{false};
  bool running_{false};
};

class NonIdempotentStopSource final : public vehicle_telemetry::AcquisitionSource {
public:
  [[nodiscard]] vehicle_telemetry::StatusResult start() noexcept override {
    if (stop_called_)
      return {vehicle_telemetry::ResultCode::AlreadyRunning};
    const auto result = source_.start();
    if (result.ok())
      stop_called_ = false;
    return result;
  }

  [[nodiscard]] vehicle_telemetry::StatusResult stop() noexcept override {
    if (stop_called_)
      return {vehicle_telemetry::ResultCode::Faulted};
    stop_called_ = true;
    return source_.stop();
  }

  [[nodiscard]] vehicle_telemetry::ReceiveStatus
  receive(vehicle_core::RawCanFrame &frame, const std::uint32_t timeout_ms) noexcept override {
    return source_.receive(frame, timeout_ms);
  }

  [[nodiscard]] vehicle_telemetry::AcquisitionStatistics statistics() const noexcept override {
    return source_.statistics();
  }

  [[nodiscard]] mazda::ResultCode inject(const vehicle_core::RawCanFrame &frame) noexcept {
    return source_.inject(frame);
  }

private:
  mazda::internal::HostRuntimeSource source_{};
  bool stop_called_{false};
};

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

struct TurnRecorder final {
  mutable std::mutex mutex{};
  mutable std::condition_variable changed{};
  std::array<mazda::Notification<mazda::TurnState>, 32> notices{};
  std::size_t count{0};
  bool block_left{false};
  bool entered_block{false};
  bool release_block{false};
};

struct FrontWiperRecorder final {
  mutable std::mutex mutex{};
  mutable std::condition_variable changed{};
  std::array<mazda::Notification<mazda::FrontWiperPosition>, 8> notices{};
  std::size_t count{0};
};

template <typename T> struct ValidationRecorder final {
  mutable std::mutex mutex{};
  mutable std::condition_variable changed{};
  mazda::ValidationStatus validation{mazda::ValidationStatus::Reference};
  std::size_t count{0};
};

template <typename T>
void record_validation(void *context, const mazda::Notification<T> &notice) noexcept {
  auto &recorder = *static_cast<ValidationRecorder<T> *>(context);
  {
    std::lock_guard<std::mutex> lock{recorder.mutex};
    recorder.validation = notice.current.validation;
    ++recorder.count;
  }
  recorder.changed.notify_all();
}

struct LifecycleMutationRecorder final {
  mazda::internal::VehicleTelemetryService *service{nullptr};
  mazda::internal::SubscriptionToken subscription{};
  std::atomic<bool> callback_done{false};
  std::atomic<mazda::ResultCode> configure_result{mazda::ResultCode::Ok};
  std::atomic<mazda::ResultCode> start_result{mazda::ResultCode::Ok};
  std::atomic<mazda::ResultCode> stop_result{mazda::ResultCode::Ok};
  std::atomic<mazda::ResultCode> subscribe_result{mazda::ResultCode::Ok};
  std::atomic<mazda::ResultCode> unsubscribe_result{mazda::ResultCode::Ok};
};

void record_front_wiper(void *context,
                        const mazda::Notification<mazda::FrontWiperPosition> &notice) noexcept {
  auto &recorder = *static_cast<FrontWiperRecorder *>(context);
  std::lock_guard<std::mutex> lock{recorder.mutex};
  if (recorder.count < recorder.notices.size())
    recorder.notices[recorder.count++] = notice;
  recorder.changed.notify_all();
}

void reject_lifecycle_mutations(void *context,
                                const mazda::Notification<mazda::TurnState> &) noexcept {
  auto &recorder = *static_cast<LifecycleMutationRecorder *>(context);
  auto invalid = mazda::TelemetryConfig{};
  invalid.max_frames_per_batch = 0;
  recorder.configure_result.store(recorder.service->configure(invalid).status,
                                  std::memory_order_release);
  recorder.start_result.store(recorder.service->start().status, std::memory_order_release);
  recorder.stop_result.store(recorder.service->stop().status, std::memory_order_release);
  recorder.subscribe_result.store(
      recorder.service->subscribe_turn(&reject_lifecycle_mutations, context).status,
      std::memory_order_release);
  recorder.unsubscribe_result.store(recorder.service->unsubscribe(recorder.subscription).status,
                                    std::memory_order_release);
  recorder.callback_done.store(true, std::memory_order_release);
}

void record_turn(void *context, const mazda::Notification<mazda::TurnState> &notice) noexcept {
  auto &recorder = *static_cast<TurnRecorder *>(context);
  std::unique_lock<std::mutex> lock{recorder.mutex};
  if (recorder.count < recorder.notices.size())
    recorder.notices[recorder.count++] = notice;
  if (recorder.block_left && notice.current.value == mazda::TurnState::Left) {
    recorder.entered_block = true;
    recorder.changed.notify_all();
    recorder.changed.wait(lock, [&recorder] { return recorder.release_block; });
  }
  recorder.changed.notify_all();
}

bool wait_for_count(const TurnRecorder &recorder, const std::size_t count,
                    const std::chrono::milliseconds timeout = std::chrono::milliseconds{500}) {
  std::unique_lock<std::mutex> lock{recorder.mutex};
  return recorder.changed.wait_for(lock, timeout,
                                   [&recorder, count] { return recorder.count >= count; });
}

template <typename Telemetry>
bool wait_for_dispatch_progress_after(
    const Telemetry &telemetry, const std::uint32_t baseline,
    const std::chrono::milliseconds timeout = std::chrono::milliseconds{500}) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (telemetry.dispatch_progress() != baseline)
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  return false;
}

bool wait_for_lifecycle(const mazda::internal::VehicleTelemetryService &service,
                        const mazda::LifecycleState expected,
                        const std::chrono::milliseconds timeout = std::chrono::milliseconds{500}) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (service.diagnostics().lifecycle == expected)
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  return service.diagnostics().lifecycle == expected;
}

bool wait_for_reading(const mazda::internal::VehicleTelemetryService &service,
                      const std::chrono::milliseconds timeout = std::chrono::milliseconds{500}) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (service.speed_kph().value.has_value())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  return service.speed_kph().value.has_value();
}

bool wait_for_lighting_count(const FakeLightingSink &lighting, const std::size_t count,
                             const std::chrono::milliseconds timeout = std::chrono::milliseconds{
                                 500}) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (lighting.size() >= count)
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  return lighting.size() >= count;
}

template <typename Predicate>
bool wait_for_flag(Predicate predicate,
                   const std::chrono::milliseconds timeout = std::chrono::milliseconds{500}) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  return predicate();
}

template <typename T>
bool wait_for_notification(const ValidationRecorder<T> &recorder,
                           const std::chrono::milliseconds timeout = std::chrono::milliseconds{
                               500}) {
  std::unique_lock<std::mutex> lock{recorder.mutex};
  return recorder.changed.wait_for(lock, timeout, [&recorder] { return recorder.count >= 1; });
}

template <typename T>
mazda::ValidationStatus recorded_validation(const ValidationRecorder<T> &recorder) {
  std::lock_guard<std::mutex> lock{recorder.mutex};
  return recorder.validation;
}

int failures = 0;

void expect(const bool condition, const char *expression, const char *file, const int line) {
  if (!condition) {
    std::cerr << file << ':' << line << ": failed: " << expression << '\n';
    ++failures;
  }
}

#define EXPECT(condition) expect((condition), #condition, __FILE__, __LINE__)

void test_lifecycle_and_subscription_state() {
  FakeClock clock;
  mazda::internal::HostAcquisitionSource source;
  FakeLightingSink lighting;
  mazda::TelemetryConfig config{};
  config.callback_stop_timeout_us = 20'000;
  mazda::internal::VehicleTelemetryService service{clock, source, lighting, config};

  auto invalid = config;
  invalid.max_frames_per_batch = 17;
  EXPECT(!service.configure(invalid).ok());

  TurnRecorder recorder{};
  const auto subscription = service.subscribe_turn(&record_turn, &recorder);
  const auto second_subscription = service.subscribe_turn(&record_turn, &recorder);
  EXPECT(subscription.ok());
  EXPECT(service.subscribe_turn(&record_turn, &recorder).status ==
         mazda::ResultCode::CapacityExceeded);
  EXPECT(service.start().ok());
  EXPECT(service.start().status == mazda::ResultCode::AlreadyRunning);
  EXPECT(service.subscribe_turn(&record_turn, &recorder).status == mazda::ResultCode::InvalidState);
  EXPECT(service.unsubscribe(subscription).status == mazda::ResultCode::InvalidState);
  EXPECT(wait_for_count(recorder, 2));
  EXPECT(service.stop().ok());
  EXPECT(service.diagnostics().lifecycle == mazda::LifecycleState::Stopped);
  EXPECT(service.configure(config).ok());
  EXPECT(service.start().ok());
  EXPECT(wait_for_count(recorder, 4));
  EXPECT(service.stop().ok());
  EXPECT(service.unsubscribe(subscription).ok());
  EXPECT(service.unsubscribe(second_subscription).ok());
}

void test_lifecycle_state_precedes_configuration_validation() {
  FakeClock clock;
  mazda::internal::HostAcquisitionSource source;
  FakeLightingSink lighting;
  mazda::TelemetryConfig config{};
  config.callback_stop_timeout_us = 20'000;
  mazda::internal::VehicleTelemetryService service{clock, source, lighting, config};

  auto invalid = config;
  invalid.max_frames_per_batch = 0;
  EXPECT(service.start().ok());
  // A running service reports its lifecycle violation first, even when the
  // supplied configuration is also malformed. This avoids leaking validation
  // details through a state-incompatible mutation.
  EXPECT(service.configure(invalid).status == mazda::ResultCode::InvalidState);
  EXPECT(service.stop().ok());
  EXPECT(service.configure(invalid).status == mazda::ResultCode::InvalidConfiguration);
}

void test_callback_mutations_are_rejected_before_side_effects() {
  FakeClock clock;
  mazda::internal::HostAcquisitionSource source;
  FakeLightingSink lighting;
  mazda::TelemetryConfig config{};
  config.callback_stop_timeout_us = 20'000;
  mazda::internal::VehicleTelemetryService service{clock, source, lighting, config};
  LifecycleMutationRecorder recorder{&service};
  recorder.subscription = service.subscribe_turn(&reject_lifecycle_mutations, &recorder);
  EXPECT(recorder.subscription.ok());
  EXPECT(service.start().ok());
  EXPECT(wait_for_flag(
      [&recorder] { return recorder.callback_done.load(std::memory_order_acquire); }));

  EXPECT(recorder.configure_result.load(std::memory_order_acquire) ==
         mazda::ResultCode::InvalidState);
  EXPECT(recorder.start_result.load(std::memory_order_acquire) == mazda::ResultCode::InvalidState);
  EXPECT(recorder.stop_result.load(std::memory_order_acquire) == mazda::ResultCode::InvalidState);
  EXPECT(recorder.subscribe_result.load(std::memory_order_acquire) ==
         mazda::ResultCode::InvalidState);
  EXPECT(recorder.unsubscribe_result.load(std::memory_order_acquire) ==
         mazda::ResultCode::InvalidState);
  EXPECT(service.diagnostics().lifecycle == mazda::LifecycleState::Running);

  // The callback rejected every mutation before touching lifecycle/source
  // state, so the owner can still observe a live source and stop normally.
  clock.set(1);
  EXPECT(source.inject(frame(mazda::candidate::kEngineDataId, 1, {0x09, 0x5b, 0, 0, 0, 0, 0, 0})) ==
         mazda::ResultCode::Ok);
  EXPECT(wait_for_reading(service));
  EXPECT(service.stop().ok());
  EXPECT(service.unsubscribe(recorder.subscription).ok());
}

void test_non_owner_lifecycle_mutation_is_rejected_without_source_side_effect() {
  FakeClock clock;
  mazda::internal::HostAcquisitionSource source;
  FakeLightingSink lighting;
  mazda::TelemetryConfig config{};
  config.callback_stop_timeout_us = 20'000;
  mazda::internal::VehicleTelemetryService service{clock, source, lighting, config};
  TurnRecorder recorder{};
  const auto subscription = service.subscribe_turn(&record_turn, &recorder);
  EXPECT(subscription.ok());

  // Registration above establishes the owner even while stopped. Setup
  // mutations from a different host thread are rejected before configuration
  // or registration can change.
  std::atomic<mazda::ResultCode> pre_start_result{mazda::ResultCode::Ok};
  std::thread non_owner_setup([&service, &config, &pre_start_result] {
    pre_start_result.store(service.configure(config).status, std::memory_order_release);
  });
  non_owner_setup.join();
  EXPECT(pre_start_result.load(std::memory_order_acquire) == mazda::ResultCode::InvalidState);
  EXPECT(service.start().ok());

  std::atomic<mazda::ResultCode> configure_result{mazda::ResultCode::Ok};
  std::atomic<mazda::ResultCode> subscribe_result{mazda::ResultCode::Ok};
  std::atomic<mazda::ResultCode> unsubscribe_result{mazda::ResultCode::Ok};
  std::atomic<mazda::ResultCode> stop_result{mazda::ResultCode::Ok};
  std::thread non_owner([&] {
    auto invalid = config;
    invalid.max_frames_per_batch = 0;
    configure_result.store(service.configure(invalid).status, std::memory_order_release);
    subscribe_result.store(service.subscribe_turn(&record_turn, &recorder).status,
                           std::memory_order_release);
    unsubscribe_result.store(service
                                 .unsubscribe({mazda::ResultCode::Ok, subscription.channel,
                                               subscription.slot, subscription.generation})
                                 .status,
                             std::memory_order_release);
    stop_result.store(service.stop().status, std::memory_order_release);
  });
  non_owner.join();
  EXPECT(configure_result.load(std::memory_order_acquire) == mazda::ResultCode::InvalidState);
  EXPECT(subscribe_result.load(std::memory_order_acquire) == mazda::ResultCode::InvalidState);
  EXPECT(unsubscribe_result.load(std::memory_order_acquire) == mazda::ResultCode::InvalidState);
  EXPECT(stop_result.load(std::memory_order_acquire) == mazda::ResultCode::InvalidState);
  EXPECT(service.diagnostics().lifecycle == mazda::LifecycleState::Running);

  clock.set(2);
  EXPECT(source.inject(frame(mazda::candidate::kEngineDataId, 2, {0x09, 0x5b, 0, 0, 0, 0, 0, 0})) ==
         mazda::ResultCode::Ok);
  EXPECT(wait_for_reading(service));
  EXPECT(service.stop().ok());

  std::atomic<mazda::ResultCode> restart_result{mazda::ResultCode::Ok};
  std::thread non_owner_restart([&service, &restart_result] {
    restart_result.store(service.start().status, std::memory_order_release);
  });
  non_owner_restart.join();
  EXPECT(restart_result.load(std::memory_order_acquire) == mazda::ResultCode::InvalidState);
  EXPECT(service.start().ok());
  EXPECT(service.stop().ok());
  EXPECT(service.unsubscribe(subscription).ok());
}

void test_sequential_host_threads_cannot_inherit_lifecycle_ownership() {
  FakeClock clock;
  mazda::internal::HostAcquisitionSource source;
  FakeLightingSink lighting;
  mazda::TelemetryConfig config{};
  mazda::internal::VehicleTelemetryService service{clock, source, lighting, config};

  // The owner exits before the next thread is created. A TLS marker address
  // can be recycled in that gap; a generation token must still reject every
  // later context as a non-owner.
  std::atomic<mazda::ResultCode> owner_result{mazda::ResultCode::Faulted};
  std::thread owner([&service, &config, &owner_result] {
    owner_result.store(service.configure(config).status, std::memory_order_release);
  });
  owner.join();
  EXPECT(owner_result.load(std::memory_order_acquire) == mazda::ResultCode::Ok);

  for (std::size_t attempt = 0; attempt < 32; ++attempt) {
    std::atomic<mazda::ResultCode> contender_result{mazda::ResultCode::Ok};
    std::thread contender([&service, &config, &contender_result] {
      contender_result.store(service.configure(config).status, std::memory_order_release);
    });
    contender.join();
    EXPECT(contender_result.load(std::memory_order_acquire) == mazda::ResultCode::InvalidState);
  }
}

void test_added_poll_and_notify_signals_use_service_workers() {
  FakeClock clock;
  mazda::internal::HostAcquisitionSource source;
  FakeLightingSink lighting;
  mazda::TelemetryConfig config{};
  config.callback_stop_timeout_us = 20'000;
  mazda::internal::VehicleTelemetryService service{clock, source, lighting, config};
  FrontWiperRecorder recorder{};
  EXPECT(service.subscribe_front_wiper(&record_front_wiper, &recorder).ok());
  EXPECT(service.start().ok());
  EXPECT(wait_for_flag([&recorder] {
    std::lock_guard<std::mutex> lock{recorder.mutex};
    return recorder.count >= 1;
  }));

  // The test injects into the existing source seam only; it does not call a
  // decoder, tick, notification dispatcher, or LED update loop.
  clock.set(10);
  EXPECT(source.inject(frame(mazda::candidate::kEngineDataId, 10,
                             {0x09, 0x5b, 0, 0, 0, 0, 0, 0})) == mazda::ResultCode::Ok);
  EXPECT(wait_for_flag([&service] { return service.engine_rpm().value.has_value(); }));
  clock.set(11);
  EXPECT(source.inject(frame(mazda::candidate::kTurnSwitchId, 11, {0, 0, 0x10, 0, 0, 0, 0, 0})) ==
         mazda::ResultCode::Ok);
  EXPECT(wait_for_flag([&recorder] {
    std::lock_guard<std::mutex> lock{recorder.mutex};
    return recorder.count >= 2;
  }));
  {
    std::lock_guard<std::mutex> lock{recorder.mutex};
    EXPECT(recorder.notices[1].current.value == mazda::FrontWiperPosition::On);
    EXPECT(recorder.notices[1].current.availability == mazda::Availability::FreshnessUnverified);
  }
  EXPECT(service.stop().ok());
}

void test_notifications_preserve_metadata_confidence() {
  FakeClock clock;
  mazda::internal::HostAcquisitionSource source;
  FakeLightingSink lighting;
  mazda::TelemetryConfig config{};
  config.callback_stop_timeout_us = 20'000;
  mazda::internal::VehicleTelemetryService service{clock, source, lighting, config};

  ValidationRecorder<mazda::SelectorPosition> selector{};
  ValidationRecorder<mazda::ActualGear> actual_gear{};
  ValidationRecorder<mazda::TurnState> turn{};
  ValidationRecorder<bool> hazard{};
  ValidationRecorder<bool> left_turn{};
  ValidationRecorder<bool> right_turn{};
  ValidationRecorder<bool> liftgate{};
  ValidationRecorder<bool> rear_right_door{};
  ValidationRecorder<bool> rear_left_door{};
  ValidationRecorder<bool> front_left_door{};
  ValidationRecorder<bool> front_right_door{};
  ValidationRecorder<bool> doors_unlocked{};
  ValidationRecorder<bool> left_lamp{};
  ValidationRecorder<bool> right_lamp{};
  ValidationRecorder<bool> wiper_low{};
  ValidationRecorder<mazda::FrontWiperPosition> front_wiper{};

  EXPECT(service.subscribe_selector(&record_validation<mazda::SelectorPosition>, &selector).ok());
  EXPECT(service.subscribe_actual_gear(&record_validation<mazda::ActualGear>, &actual_gear).ok());
  EXPECT(service.subscribe_turn(&record_validation<mazda::TurnState>, &turn).ok());
  EXPECT(service.subscribe_hazard(&record_validation<bool>, &hazard).ok());
  EXPECT(service.subscribe_left_turn(&record_validation<bool>, &left_turn).ok());
  EXPECT(service.subscribe_right_turn(&record_validation<bool>, &right_turn).ok());
  EXPECT(service.subscribe_liftgate(&record_validation<bool>, &liftgate).ok());
  EXPECT(service.subscribe_rear_right_door(&record_validation<bool>, &rear_right_door).ok());
  EXPECT(service.subscribe_rear_left_door(&record_validation<bool>, &rear_left_door).ok());
  EXPECT(service.subscribe_front_left_door(&record_validation<bool>, &front_left_door).ok());
  EXPECT(service.subscribe_front_right_door(&record_validation<bool>, &front_right_door).ok());
  EXPECT(service.subscribe_doors_unlocked(&record_validation<bool>, &doors_unlocked).ok());
  EXPECT(service.subscribe_left_lamp(&record_validation<bool>, &left_lamp).ok());
  EXPECT(service.subscribe_right_lamp(&record_validation<bool>, &right_lamp).ok());
  EXPECT(service.subscribe_wiper_low(&record_validation<bool>, &wiper_low).ok());
  EXPECT(service.subscribe_front_wiper(&record_validation<mazda::FrontWiperPosition>, &front_wiper)
             .ok());

  EXPECT(service.start().ok());
  EXPECT(wait_for_notification(selector));
  EXPECT(wait_for_notification(actual_gear));
  EXPECT(wait_for_notification(turn));
  EXPECT(wait_for_notification(hazard));
  EXPECT(wait_for_notification(left_turn));
  EXPECT(wait_for_notification(right_turn));
  EXPECT(wait_for_notification(liftgate));
  EXPECT(wait_for_notification(rear_right_door));
  EXPECT(wait_for_notification(rear_left_door));
  EXPECT(wait_for_notification(front_left_door));
  EXPECT(wait_for_notification(front_right_door));
  EXPECT(wait_for_notification(doors_unlocked));
  EXPECT(wait_for_notification(left_lamp));
  EXPECT(wait_for_notification(right_lamp));
  EXPECT(wait_for_notification(wiper_low));
  EXPECT(wait_for_notification(front_wiper));

  // The initial notifications are NoData, proving that validation evidence is
  // independent from runtime availability and freshness evaluation.
  EXPECT(recorded_validation(selector) == mazda::ValidationStatus::Confirmed);
  EXPECT(recorded_validation(selector) == mazda::candidate::kSelectorDefinition.confidence);
  EXPECT(recorded_validation(actual_gear) == mazda::ValidationStatus::Observed);
  EXPECT(recorded_validation(actual_gear) == mazda::candidate::kActualGearDefinition.confidence);
  EXPECT(recorded_validation(turn) == mazda::ValidationStatus::Reference);
  EXPECT(recorded_validation(turn) == mazda::candidate::kTurnLeftSwitchDefinition.confidence);
  EXPECT(recorded_validation(hazard) == mazda::ValidationStatus::Reference);
  EXPECT(recorded_validation(hazard) == mazda::candidate::kHazardDefinition.confidence);
  EXPECT(recorded_validation(left_turn) == mazda::ValidationStatus::Reference);
  EXPECT(recorded_validation(left_turn) == mazda::candidate::kTurnLeftSwitchDefinition.confidence);
  EXPECT(recorded_validation(right_turn) == mazda::ValidationStatus::Reference);
  EXPECT(recorded_validation(right_turn) ==
         mazda::candidate::kTurnRightSwitchDefinition.confidence);
  EXPECT(recorded_validation(liftgate) == mazda::ValidationStatus::Reference);
  EXPECT(recorded_validation(liftgate) == mazda::candidate::kLiftgateOpenDefinition.confidence);
  EXPECT(recorded_validation(rear_right_door) == mazda::ValidationStatus::Reference);
  EXPECT(recorded_validation(rear_right_door) ==
         mazda::candidate::kRearRightDoorOpenDefinition.confidence);
  EXPECT(recorded_validation(rear_left_door) == mazda::ValidationStatus::Reference);
  EXPECT(recorded_validation(rear_left_door) ==
         mazda::candidate::kRearLeftDoorOpenDefinition.confidence);
  EXPECT(recorded_validation(front_left_door) == mazda::ValidationStatus::Reference);
  EXPECT(recorded_validation(front_left_door) ==
         mazda::candidate::kFrontLeftDoorOpenRhdDefinition.confidence);
  EXPECT(recorded_validation(front_right_door) == mazda::ValidationStatus::Confirmed);
  EXPECT(recorded_validation(front_right_door) ==
         mazda::candidate::kFrontRightDoorOpenRhdDefinition.confidence);
  EXPECT(recorded_validation(doors_unlocked) == mazda::ValidationStatus::Reference);
  EXPECT(recorded_validation(doors_unlocked) ==
         mazda::candidate::kDoorsUnlockedDefinition.confidence);
  EXPECT(recorded_validation(left_lamp) == mazda::ValidationStatus::Reference);
  EXPECT(recorded_validation(left_lamp) ==
         mazda::candidate::kLeftIndicatorLampDefinition.confidence);
  EXPECT(recorded_validation(right_lamp) == mazda::ValidationStatus::Reference);
  EXPECT(recorded_validation(right_lamp) ==
         mazda::candidate::kRightIndicatorLampDefinition.confidence);
  EXPECT(recorded_validation(wiper_low) == mazda::ValidationStatus::Observed);
  EXPECT(recorded_validation(wiper_low) == mazda::candidate::kWiperLowDefinition.confidence);
  EXPECT(recorded_validation(front_wiper) == mazda::ValidationStatus::Observed);
  EXPECT(recorded_validation(front_wiper) == mazda::candidate::kFrontWiperDefinition.confidence);
  EXPECT(service.stop().ok());
}

void test_public_facade_private_lighting_binding() {
  mazda::VehicleTelemetry telemetry{};
  FakeLightingSink lighting;
  EXPECT(mazda::internal::VehicleTelemetryAccess::bind_lighting_sink(telemetry, lighting).ok());
  EXPECT(telemetry.start().ok());
  EXPECT(wait_for_lighting_count(lighting, 1));
  EXPECT(lighting.at(0).turn == mazda::TurnState::Unknown);
  EXPECT(telemetry.stop().ok());
  EXPECT(mazda::internal::VehicleTelemetryAccess::bind_lighting_sink(telemetry, lighting).ok());
  EXPECT(telemetry.start().ok());
  EXPECT(telemetry.stop().ok());
}

void test_lighting_sink_binding_is_stopped_only() {
  FakeClock clock;
  mazda::internal::HostAcquisitionSource source;
  FakeLightingSink initial_lighting;
  FakeLightingSink bound_lighting;
  mazda::TelemetryConfig config{};
  config.callback_stop_timeout_us = 20'000;
  mazda::internal::VehicleTelemetryService service{clock, source, initial_lighting, config};

  EXPECT(service.bind_lighting_sink(bound_lighting).ok());
  EXPECT(service.start().ok());
  EXPECT(wait_for_lighting_count(bound_lighting, 1));
  EXPECT(initial_lighting.size() == 0);
  EXPECT(service.bind_lighting_sink(initial_lighting).status == mazda::ResultCode::InvalidState);
  EXPECT(service.stop().ok());
}

void test_lighting_brake_update_remains_unverified_and_binding_fails_off() {
  FakeClock clock;
  mazda::internal::HostAcquisitionSource source;
  FakeLightingSink lighting;
  mazda::TelemetryConfig config{};
  config.callback_stop_timeout_us = 20'000;
  mazda::internal::VehicleTelemetryService service{clock, source, lighting, config};
  EXPECT(service.start().ok());
  EXPECT(wait_for_lighting_count(lighting, 1));

  clock.set(1);
  EXPECT(source.inject(frame(mazda::candidate::kBrakePedalId, 1, {0x10, 0, 0, 0, 0, 0, 0, 0})) ==
         mazda::ResultCode::Ok);
  EXPECT(wait_for_lighting_count(lighting, 2));
  const auto update = lighting.at(1);
  EXPECT(update.turn == mazda::TurnState::Unknown);
  EXPECT(!update.brake_pressed);
  EXPECT(update.brake_availability == mazda::Availability::FreshnessUnverified);
  // No brake freshness timeout is established yet. Both the service handoff
  // and the production binding require Fresh and therefore keep the physical
  // brake region off.
  EXPECT(service.stop().ok());
}

void test_failed_shared_source_start_does_not_stop_existing_owner() {
  FakeClock clock;
  mazda::internal::HostAcquisitionSource source;
  FakeLightingSink first_lighting;
  FakeLightingSink second_lighting;
  mazda::TelemetryConfig config{};
  config.callback_stop_timeout_us = 20'000;
  mazda::internal::VehicleTelemetryService first{clock, source, first_lighting, config};
  EXPECT(first.start().ok());

  // A contending service remains stopped and must not release the source
  // owned by the already-running service.
  mazda::internal::VehicleTelemetryService second{clock, source, second_lighting, config};
  EXPECT(second.start().status == mazda::ResultCode::AlreadyRunning);
  EXPECT(second.diagnostics().lifecycle == mazda::LifecycleState::Stopped);

  clock.set(30);
  EXPECT(source.inject(frame(mazda::candidate::kEngineDataId, 30,
                             {0x09, 0x5b, 0, 0, 0, 0, 0, 0})) == mazda::ResultCode::Ok);
  EXPECT(wait_for_reading(first));
  EXPECT(first.diagnostics().lifecycle == mazda::LifecycleState::Running);
  EXPECT(first.stop().ok());
  EXPECT(second.start().ok());
  EXPECT(second.stop().ok());
}

void test_source_start_failure_without_ownership_is_restartable() {
  FakeClock clock;
  RestartableSource source;
  source.fail_next_start();
  FakeLightingSink lighting;
  mazda::TelemetryConfig config{};
  config.callback_stop_timeout_us = 20'000;
  mazda::internal::VehicleTelemetryService service{clock, source, lighting, config};

  EXPECT(service.start().status == mazda::ResultCode::Faulted);
  EXPECT(service.diagnostics().lifecycle == mazda::LifecycleState::Stopped);
  EXPECT(service.start().ok());
  EXPECT(service.diagnostics().lifecycle == mazda::LifecycleState::Running);
  EXPECT(service.stop().ok());
}

void test_partial_source_start_cleanup_success_is_restartable() {
  FakeClock clock;
  PartialOwnershipSource source;
  source.fail_next_start_with_ownership();
  FakeLightingSink lighting;
  mazda::TelemetryConfig config{};
  config.callback_stop_timeout_us = 20'000;
  mazda::internal::VehicleTelemetryService service{clock, source, lighting, config};

  // The source reports failure after acquiring ownership. The service must
  // release that ownership before returning and leave itself restartable.
  EXPECT(service.start().status == mazda::ResultCode::Faulted);
  EXPECT(source.stop_calls() == 1);
  EXPECT(!source.running());
  EXPECT(service.diagnostics().lifecycle == mazda::LifecycleState::Stopped);
  EXPECT(service.start().ok());
  EXPECT(service.stop().ok());
}

void test_partial_source_start_cleanup_failure_is_retried_by_stop() {
  FakeClock clock;
  PartialOwnershipSource source;
  source.fail_next_start_with_ownership();
  source.fail_next_stop();
  FakeLightingSink lighting;
  mazda::TelemetryConfig config{};
  config.callback_stop_timeout_us = 20'000;
  mazda::internal::VehicleTelemetryService service{clock, source, lighting, config};

  // Cleanup failure proves ownership was retained. A later stop must retry the
  // source release and finish the lifecycle once the source accepts it.
  EXPECT(service.start().status == mazda::ResultCode::Faulted);
  EXPECT(source.stop_calls() == 1);
  EXPECT(source.running());
  EXPECT(service.diagnostics().lifecycle == mazda::LifecycleState::Faulted);
  EXPECT(service.stop().status == mazda::ResultCode::Faulted);
  EXPECT(source.stop_calls() == 2);
  EXPECT(!source.running());
  EXPECT(service.diagnostics().lifecycle == mazda::LifecycleState::Stopped);
}

void test_immediate_worker_receive_fault_is_not_overwritten_by_running() {
  FakeClock clock;
  ImmediateFaultSource source;
  FakeLightingSink lighting;
  mazda::TelemetryConfig config{};
  config.callback_stop_timeout_us = 20'000;
  mazda::internal::VehicleTelemetryService service{clock, source, lighting, config};

  EXPECT(service.start().ok());
  EXPECT(wait_for_flag([&source] { return source.fault_started(); }));
  EXPECT(wait_for_lifecycle(service, mazda::LifecycleState::Running));
  source.release_statistics();
  EXPECT(wait_for_lifecycle(service, mazda::LifecycleState::Faulted));
  EXPECT(service.diagnostics().lifecycle == mazda::LifecycleState::Faulted);
  EXPECT(service.stop().status == mazda::ResultCode::Faulted);
  EXPECT(service.diagnostics().lifecycle == mazda::LifecycleState::Stopped);
}

void test_unknown_frame_is_transport_traffic_and_expires() {
  FakeClock clock;
  mazda::internal::HostAcquisitionSource source;
  FakeLightingSink lighting;
  mazda::TelemetryConfig config{};
  config.transport_silence_timeout_us = 100;
  config.callback_stop_timeout_us = 20'000;
  mazda::internal::VehicleTelemetryService service{clock, source, lighting, config};

  EXPECT(service.start().ok());
  clock.set(50);
  EXPECT(source.inject(frame(0x7ff, 50, {0, 0, 0, 0, 0, 0, 0, 0})) == mazda::ResultCode::Ok);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{500};
  while (std::chrono::steady_clock::now() < deadline &&
         service.diagnostics().acquisition.frames_processed == 0)
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  EXPECT(service.diagnostics().acquisition.frames_received == 1);
  EXPECT(service.diagnostics().acquisition.frames_processed == 1);
  EXPECT(service.diagnostics().transport == vehicle_core::TransportHealth::Live);
  clock.set(151);
  EXPECT(service.diagnostics().transport == vehicle_core::TransportHealth::TimedOut);
  EXPECT(service.stop().ok());
}

void test_sustained_bounded_overload_services_expiry_and_latest_state() {
  FakeClock clock;
  mazda::internal::HostAcquisitionSource source;
  FakeLightingSink lighting;
  mazda::TelemetryConfig config{};
  config.transport_silence_timeout_us = 100;
  config.callback_stop_timeout_us = 20'000;
  config.max_frames_per_batch = 16;
  config.availability_service_target_us = 1'000;
  mazda::internal::VehicleTelemetryService service{clock, source, lighting, config};

  // Keep the fixed max-16 batch boundary part of this service-level contract.
  auto invalid = config;
  invalid.max_frames_per_batch = 17;
  EXPECT(service.configure(invalid).status == mazda::ResultCode::InvalidConfiguration);
  EXPECT(service.configure(config).ok());

  TurnRecorder recorder{};
  EXPECT(service.subscribe_turn(&record_turn, &recorder).ok());
  EXPECT(service.start().ok());
  EXPECT(wait_for_count(recorder, 1));

  // Establish a known turn value and its transport timestamp at zero. The
  // later burst uses the same timestamp, so advancing the injected clock
  // cannot be hidden by frames that are still queued.
  clock.set(0);
  EXPECT(source.inject(frame(mazda::candidate::kTurnSwitchId, 0, {0, 0x20, 0, 0, 0, 0, 0, 0})) ==
         mazda::ResultCode::Ok);
  EXPECT(wait_for_count(recorder, 2));
  const auto processed_before = service.diagnostics().acquisition.frames_processed;
  const auto source_received_before = source.statistics().frames_received;
  EXPECT(processed_before >= 1);

  // Pause the injected clock at the processing publication boundary. This
  // gives the fixed-capacity source a deterministic finite backlog without
  // making any claim about physical task scheduling.
  clock.set(300'000);
  clock.pause_reads();
  EXPECT(clock.wait_until_read_paused());

  constexpr std::size_t kBurstFrames = mazda::internal::HostAcquisitionSource::kCapacity + 16;
  std::size_t rejected = 0;
  for (std::size_t index = 0; index < kBurstFrames; ++index) {
    if (source.inject(frame(0x7ff, 0, {0, 0, 0, 0, 0, 0, 0, 0})) ==
        mazda::ResultCode::CapacityExceeded)
      ++rejected;
  }
  const auto burst_statistics = source.statistics();
  EXPECT(rejected > 0);
  EXPECT(burst_statistics.frames_dropped > 0);
  EXPECT(burst_statistics.queue_overflows > 0);
  EXPECT(burst_statistics.frames_received > burst_statistics.frames_dropped);

  // Release the processing owner after the queue is known to contain frames.
  // Its bounded batch handoff must continue draining the finite backlog.
  clock.resume_reads();
  EXPECT(wait_for_flag([&service, processed_before] {
    return service.diagnostics().acquisition.frames_processed > processed_before;
  }));

  const auto accepted_burst =
      burst_statistics.frames_received - source_received_before - burst_statistics.frames_dropped;
  EXPECT(wait_for_flag([&service, processed_before, accepted_burst] {
    return service.diagnostics().acquisition.frames_processed >= processed_before + accepted_burst;
  }));

  // Once the finite backlog is drained, expiry must still be serviced even
  // though no further frames are arriving.
  clock.set(300'101);
  EXPECT(wait_for_flag([&service] {
    return service.diagnostics().transport == vehicle_core::TransportHealth::TimedOut;
  }));
  EXPECT(wait_for_flag([&recorder] {
    std::lock_guard<std::mutex> lock{recorder.mutex};
    for (std::size_t index = 0; index < recorder.count; ++index) {
      if (recorder.notices[index].current.availability == mazda::Availability::Unavailable)
        return true;
    }
    return false;
  }));

  // Polling remains live after the overload. A final accepted frame becomes
  // the eventual bounded state, with a fresh timestamp and live transport.
  clock.set(300'001);
  const auto latest =
      frame(mazda::candidate::kEngineDataId, 300'001, {0, 0, 0x2e, 0xe0, 0, 0, 0, 0});
  bool latest_injected = false;
  EXPECT(wait_for_flag([&] {
    if (!latest_injected)
      latest_injected = source.inject(latest) == mazda::ResultCode::Ok;
    return latest_injected;
  }));
  EXPECT(wait_for_flag([&service] {
    const auto reading = service.speed_kph();
    return reading.value.has_value() && *reading.value == 120.0F;
  }));
  EXPECT(service.diagnostics().transport == vehicle_core::TransportHealth::Live);
}

void test_transport_liveness_uses_acquisition_clock() {
  FakeClock clock;
  mazda::internal::HostAcquisitionSource source;
  FakeLightingSink lighting;
  mazda::TelemetryConfig config{};
  config.transport_silence_timeout_us = 100;
  config.callback_stop_timeout_us = 20'000;
  mazda::internal::VehicleTelemetryService service{clock, source, lighting, config};

  EXPECT(service.start().ok());
  const auto wait_for_processed = [&](const std::uint64_t count) {
    return wait_for_flag(
        [&service, count] { return service.diagnostics().acquisition.frames_processed >= count; });
  };

  // The first frame establishes both the semantic value and the receive
  // watermark. The second frame has an equal observation timestamp but was
  // acquired later; it must keep transport live without changing the value.
  clock.set(100);
  EXPECT(source.inject(frame(mazda::candidate::kEngineDataId, 100,
                             {0x00, 0x01, 0, 0, 0, 0, 0, 0})) == mazda::ResultCode::Ok);
  EXPECT(wait_for_processed(1));
  clock.set(180);
  EXPECT(source.inject(frame(mazda::candidate::kEngineDataId, 100,
                             {0x00, 0x02, 0, 0, 0, 0, 0, 0})) == mazda::ResultCode::Ok);
  EXPECT(wait_for_processed(2));
  EXPECT(service.engine_rpm().value.has_value());
  EXPECT(*service.engine_rpm().value == 0.25F);
  clock.set(201);
  // A frame-timestamp watermark would have timed out at 201; acquisition time
  // 180 remains within the 100-us silence interval.
  EXPECT(service.diagnostics().transport == vehicle_core::TransportHealth::Live);

  // An older observation timestamp is also transport traffic, but cannot
  // replace the accepted semantic value.
  clock.set(250);
  EXPECT(source.inject(frame(mazda::candidate::kEngineDataId, 50,
                             {0x00, 0x03, 0, 0, 0, 0, 0, 0})) == mazda::ResultCode::Ok);
  EXPECT(wait_for_processed(3));
  EXPECT(service.engine_rpm().value.has_value());
  EXPECT(*service.engine_rpm().value == 0.25F);
  EXPECT(service.diagnostics().transport == vehicle_core::TransportHealth::Live);

  // A backwards acquisition-clock step must not move the receive watermark
  // backwards. The high source timestamp is intentionally unrelated: it must
  // not keep transport live or make the timeout clock run backwards.
  clock.set(500);
  EXPECT(source.inject(frame(0x7ff, 500, {0, 0, 0, 0, 0, 0, 0, 0})) == mazda::ResultCode::Ok);
  EXPECT(wait_for_processed(4));
  clock.set(400);
  EXPECT(source.inject(frame(0x7ff, 1'000, {0, 0, 0, 0, 0, 0, 0, 0})) == mazda::ResultCode::Ok);
  EXPECT(wait_for_processed(5));
  clock.set(601);
  EXPECT(service.diagnostics().transport == vehicle_core::TransportHealth::TimedOut);
  EXPECT(service.stop().ok());
}

void test_notifications_coalesce_and_recover() {
  FakeClock clock;
  mazda::internal::HostAcquisitionSource source;
  FakeLightingSink lighting;
  mazda::TelemetryConfig config{};
  config.transport_silence_timeout_us = 100;
  config.callback_stop_timeout_us = 20'000;
  mazda::internal::VehicleTelemetryService service{clock, source, lighting, config};
  TurnRecorder recorder{};
  EXPECT(service.subscribe_turn(&record_turn, &recorder).ok());
  EXPECT(service.start().ok());
  EXPECT(wait_for_count(recorder, 1));

  recorder.block_left = true;
  clock.set(10);
  EXPECT(source.inject(frame(mazda::candidate::kTurnSwitchId, 10, {0, 0x20, 0, 0, 0, 0, 0, 0})) ==
         mazda::ResultCode::Ok);
  {
    std::unique_lock<std::mutex> lock{recorder.mutex};
    EXPECT(recorder.changed.wait_for(lock, std::chrono::milliseconds{500},
                                     [&recorder] { return recorder.entered_block; }));
  }

  // The dispatcher is blocked in the user callback while the processing owner
  // accepts two newer semantic states. NotificationChannel keeps one bounded
  // latest notice and marks that transition coalesced.
  clock.set(11);
  EXPECT(source.inject(frame(mazda::candidate::kTurnSwitchId, 11, {0, 0x10, 0, 0, 0, 0, 0, 0})) ==
         mazda::ResultCode::Ok);
  clock.set(12);
  EXPECT(source.inject(frame(mazda::candidate::kTurnSwitchId, 12, {0, 0x04, 0, 0, 0, 0, 0, 0})) ==
         mazda::ResultCode::Ok);
  {
    std::lock_guard<std::mutex> lock{recorder.mutex};
    recorder.release_block = true;
  }
  recorder.changed.notify_all();
  EXPECT(wait_for_count(recorder, 3));
  {
    std::lock_guard<std::mutex> lock{recorder.mutex};
    EXPECT(recorder.notices[2].current.value == mazda::TurnState::Hazard);
    EXPECT(recorder.notices[2].coalesced);
  }

  // Silence makes an observed value unavailable; a newer frame recovers it.
  clock.set(113);
  const auto timeout_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{500};
  while (std::chrono::steady_clock::now() < timeout_deadline &&
         service.diagnostics().transport != vehicle_core::TransportHealth::TimedOut)
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  EXPECT(service.diagnostics().transport == vehicle_core::TransportHealth::TimedOut);
  EXPECT(wait_for_count(recorder, 4));
  clock.set(114);
  EXPECT(source.inject(frame(mazda::candidate::kTurnSwitchId, 114, {0, 0x20, 0, 0, 0, 0, 0, 0})) ==
         mazda::ResultCode::Ok);
  EXPECT(wait_for_count(recorder, 5));
  {
    std::lock_guard<std::mutex> lock{recorder.mutex};
    EXPECT(recorder.notices[4].recovered);
  }
  EXPECT(service.stop().ok());
}

void test_blocked_callback_does_not_block_polling_and_stop_is_retryable() {
  FakeClock clock;
  mazda::internal::HostAcquisitionSource source;
  FakeLightingSink lighting;
  mazda::TelemetryConfig config{};
  config.callback_stop_timeout_us = 10'000;
  mazda::internal::VehicleTelemetryService service{clock, source, lighting, config};
  TurnRecorder recorder{};
  EXPECT(service.subscribe_turn(&record_turn, &recorder).ok());
  EXPECT(service.start().ok());
  EXPECT(wait_for_count(recorder, 1));
  recorder.block_left = true;
  clock.set(20);
  EXPECT(source.inject(frame(mazda::candidate::kTurnSwitchId, 20, {0, 0x20, 0, 0, 0, 0, 0, 0})) ==
         mazda::ResultCode::Ok);
  {
    std::unique_lock<std::mutex> lock{recorder.mutex};
    EXPECT(recorder.changed.wait_for(lock, std::chrono::milliseconds{500},
                                     [&recorder] { return recorder.entered_block; }));
  }

  clock.set(21);
  EXPECT(source.inject(frame(mazda::candidate::kEngineDataId, 21,
                             {0x09, 0x5b, 0, 0, 0, 0, 0, 0})) == mazda::ResultCode::Ok);
  EXPECT(wait_for_reading(service));
  EXPECT(service.speed_kph().value.has_value());
  EXPECT(*service.speed_kph().value == 0.0F);

  EXPECT(service.stop().status == mazda::ResultCode::Timeout);
  {
    std::lock_guard<std::mutex> lock{recorder.mutex};
    recorder.release_block = true;
  }
  recorder.changed.notify_all();
  EXPECT(service.stop().ok());
}

void test_callback_stop_timeout_can_retry_after_source_already_stopped() {
  FakeClock clock;
  NonIdempotentStopSource source;
  FakeLightingSink lighting;
  mazda::TelemetryConfig config{};
  config.callback_stop_timeout_us = 10'000;
  mazda::internal::VehicleTelemetryService service{clock, source, lighting, config};
  TurnRecorder recorder{};
  EXPECT(service.subscribe_turn(&record_turn, &recorder).ok());
  EXPECT(service.start().ok());
  EXPECT(wait_for_count(recorder, 1));
  recorder.block_left = true;
  clock.set(20);
  EXPECT(source.inject(frame(mazda::candidate::kTurnSwitchId, 20, {0, 0x20, 0, 0, 0, 0, 0, 0})) ==
         mazda::ResultCode::Ok);
  {
    std::unique_lock<std::mutex> lock{recorder.mutex};
    EXPECT(recorder.changed.wait_for(lock, std::chrono::milliseconds{500},
                                     [&recorder] { return recorder.entered_block; }));
  }

  EXPECT(service.stop().status == mazda::ResultCode::Timeout);
  {
    std::lock_guard<std::mutex> lock{recorder.mutex};
    recorder.release_block = true;
  }
  recorder.changed.notify_all();
  EXPECT(service.stop().ok());
  EXPECT(service.diagnostics().lifecycle == mazda::LifecycleState::Stopped);
}

void test_terminal_receive_fault_is_propagated_and_restart_clears_state() {
  FakeClock clock;
  mazda::internal::HostAcquisitionSource source;
  FakeLightingSink lighting;
  mazda::TelemetryConfig config{};
  config.callback_stop_timeout_us = 20'000;
  mazda::internal::VehicleTelemetryService service{clock, source, lighting, config};
  TurnRecorder recorder{};
  EXPECT(service.subscribe_turn(&record_turn, &recorder).ok());
  EXPECT(service.start().ok());
  EXPECT(wait_for_count(recorder, 1));
  clock.set(30);
  EXPECT(source.inject(frame(mazda::candidate::kEngineDataId, 30,
                             {0x09, 0x5b, 0, 0, 0, 0, 0, 0})) == mazda::ResultCode::Ok);
  EXPECT(wait_for_reading(service));
  clock.set(31);
  EXPECT(source.inject(frame(mazda::candidate::kTurnSwitchId, 31, {0, 0x20, 0, 0, 0, 0, 0, 0})) ==
         mazda::ResultCode::Ok);
  EXPECT(wait_for_count(recorder, 2));
  source.fail();
  EXPECT(wait_for_lifecycle(service, mazda::LifecycleState::Faulted));
  EXPECT(wait_for_count(recorder, 3));
  {
    std::lock_guard<std::mutex> lock{recorder.mutex};
    EXPECT(recorder.notices[2].current.value == mazda::TurnState::Left);
    EXPECT(recorder.notices[2].current.availability == mazda::Availability::Unavailable);
    EXPECT(recorder.notices[2].became_unavailable);
  }
  EXPECT(service.stop().status == mazda::ResultCode::Faulted);
  EXPECT(service.diagnostics().lifecycle == mazda::LifecycleState::Stopped);
  EXPECT(service.start().ok());
  EXPECT(service.diagnostics().transport == vehicle_core::TransportHealth::AwaitingTraffic);
  EXPECT(!service.speed_kph().value.has_value());
  EXPECT(service.stop().ok());
}

void test_lighting_startup_black_deadline_heartbeat_and_failure_retry() {
  FakeClock clock;
  mazda::internal::HostAcquisitionSource source;
  FakeLightingSink lighting;
  mazda::TelemetryConfig config{};
  config.callback_stop_timeout_us = 20'000;
  mazda::internal::VehicleTelemetryService service{clock, source, lighting, config};
  EXPECT(service.start().ok());
  EXPECT(lighting.size() >= 1);
  EXPECT(lighting.at(0).turn == mazda::TurnState::Unknown);

  clock.set(1);
  EXPECT(source.inject(frame(mazda::candidate::kTurnSwitchId, 1, {0, 0x20, 0, 0, 0, 0, 0, 0})) ==
         mazda::ResultCode::Ok);
  const auto changed_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{500};
  while (std::chrono::steady_clock::now() < changed_deadline && lighting.size() < 2)
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  EXPECT(lighting.size() >= 2);
  const auto left = lighting.at(1);
  EXPECT(left.turn == mazda::TurnState::Left);
  EXPECT(left.valid_until_us == 250'001);

  // A heartbeat is at most 100 ms and carries the earliest semantic or
  // transport deadline even when no new frame arrives.
  clock.set(100'001);
  const auto heartbeat_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{500};
  while (std::chrono::steady_clock::now() < heartbeat_deadline && lighting.size() < 3)
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  EXPECT(lighting.size() >= 3);
  EXPECT(lighting.at(2).turn == mazda::TurnState::Left);
  lighting.fail_next();
  clock.set(200'001);
  // A failed sink call is retried by the independent processing owner; the
  // retry is not tied to user callback progress.
  const auto retry_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{500};
  while (std::chrono::steady_clock::now() < retry_deadline && lighting.size() < 5)
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  EXPECT(lighting.size() >= 5);
  EXPECT(service.stop().ok());
}

void test_lighting_heartbeat_for_off_unknown_nodata_and_unavailable() {
  // Startup emits Unknown/NoData. A later heartbeat must still refresh the
  // private sink even though there is no actionable turn value.
  {
    FakeClock clock;
    mazda::internal::HostAcquisitionSource source;
    FakeLightingSink lighting;
    mazda::TelemetryConfig config{};
    config.callback_stop_timeout_us = 20'000;
    mazda::internal::VehicleTelemetryService service{clock, source, lighting, config};
    EXPECT(service.start().ok());
    EXPECT(wait_for_lighting_count(lighting, 1));
    EXPECT(lighting.at(0).turn == mazda::TurnState::Unknown);
    EXPECT(lighting.at(0).availability == mazda::Availability::NoData);
    clock.set(100'001);
    EXPECT(wait_for_lighting_count(lighting, 2));
    EXPECT(lighting.at(1).turn == mazda::TurnState::Unknown);
    EXPECT(lighting.at(1).availability == mazda::Availability::NoData);
    EXPECT(service.stop().ok());
  }

  // Off is semantically known but does not produce a colour command. It must
  // receive the same bounded private heartbeat as an active turn state.
  {
    FakeClock clock;
    mazda::internal::HostAcquisitionSource source;
    FakeLightingSink lighting;
    mazda::TelemetryConfig config{};
    config.callback_stop_timeout_us = 20'000;
    mazda::internal::VehicleTelemetryService service{clock, source, lighting, config};
    EXPECT(service.start().ok());
    EXPECT(wait_for_lighting_count(lighting, 1));
    clock.set(1);
    EXPECT(source.inject(frame(mazda::candidate::kTurnSwitchId, 1, {0, 0, 0, 0, 0, 0, 0, 0})) ==
           mazda::ResultCode::Ok);
    EXPECT(wait_for_lighting_count(lighting, 2));
    EXPECT(lighting.at(1).turn == mazda::TurnState::Off);
    EXPECT(lighting.at(1).availability == mazda::Availability::Fresh);
    clock.set(100'001);
    EXPECT(wait_for_lighting_count(lighting, 3));
    EXPECT(lighting.at(2).turn == mazda::TurnState::Off);
    EXPECT(service.stop().ok());
  }

  // A malformed turn message makes the private value unavailable while the
  // processing owner remains alive. That state also requires a heartbeat.
  {
    FakeClock clock;
    mazda::internal::HostAcquisitionSource source;
    FakeLightingSink lighting;
    mazda::TelemetryConfig config{};
    config.callback_stop_timeout_us = 20'000;
    mazda::internal::VehicleTelemetryService service{clock, source, lighting, config};
    EXPECT(service.start().ok());
    EXPECT(wait_for_lighting_count(lighting, 1));
    clock.set(1);
    EXPECT(source.inject(frame(mazda::candidate::kTurnSwitchId, 1, {0, 0x20, 0, 0, 0, 0, 0, 0})) ==
           mazda::ResultCode::Ok);
    EXPECT(wait_for_lighting_count(lighting, 2));
    EXPECT(source.inject(frame(mazda::candidate::kTurnSwitchId, 2, {0})) == mazda::ResultCode::Ok);
    EXPECT(wait_for_lighting_count(lighting, 3));
    EXPECT(lighting.at(2).turn == mazda::TurnState::Unknown);
    EXPECT(lighting.at(2).availability == mazda::Availability::Unavailable);
    clock.set(100'001);
    EXPECT(wait_for_lighting_count(lighting, 4));
    EXPECT(lighting.at(3).turn == mazda::TurnState::Unknown);
    EXPECT(lighting.at(3).availability == mazda::Availability::Unavailable);
    EXPECT(service.stop().ok());
  }
}

void test_dispatch_progress_counts_idle_loop_passes_and_stops_in_blocked_callback() {
  FakeClock clock;
  mazda::internal::HostAcquisitionSource source;
  FakeLightingSink lighting;
  mazda::TelemetryConfig config{};
  config.callback_stop_timeout_us = 10'000;
  mazda::internal::VehicleTelemetryService service{clock, source, lighting, config};
  TurnRecorder recorder{};
  EXPECT(service.subscribe_turn(&record_turn, &recorder).ok());
  EXPECT(service.start().ok());
  EXPECT(wait_for_count(recorder, 1));

  // Stable state: no notice is pending, yet every idle loop pass is progress.
  const auto idle = service.dispatch_progress();
  EXPECT(wait_for_dispatch_progress_after(service, idle));

  recorder.block_left = true;
  clock.set(30);
  EXPECT(source.inject(frame(mazda::candidate::kTurnSwitchId, 30, {0, 0x20, 0, 0, 0, 0, 0, 0})) ==
         mazda::ResultCode::Ok);
  {
    std::unique_lock<std::mutex> lock{recorder.mutex};
    EXPECT(recorder.changed.wait_for(lock, std::chrono::milliseconds{500},
                                     [&recorder] { return recorder.entered_block; }));
  }
  // A dispatcher stuck in a callback makes no progress.
  const auto blocked = service.dispatch_progress();
  EXPECT(!wait_for_dispatch_progress_after(service, blocked, std::chrono::milliseconds{20}));

  {
    std::lock_guard<std::mutex> lock{recorder.mutex};
    recorder.release_block = true;
  }
  recorder.changed.notify_all();
  EXPECT(wait_for_dispatch_progress_after(service, blocked));
  EXPECT(service.stop().ok());
}

void test_public_facade_lifecycle_contract() {
  mazda::VehicleTelemetry telemetry{};
  EXPECT(telemetry.configure(mazda::TelemetryConfig{}).ok());
  EXPECT(telemetry.start().ok());
  EXPECT(telemetry.diagnostics().lifecycle == mazda::LifecycleState::Running);
  EXPECT(wait_for_dispatch_progress_after(telemetry, telemetry.dispatch_progress()));
  EXPECT(telemetry.stop().ok());
  EXPECT(telemetry.diagnostics().lifecycle == mazda::LifecycleState::Stopped);
}

} // namespace

int main() {
  test_lifecycle_and_subscription_state();
  test_lifecycle_state_precedes_configuration_validation();
  test_callback_mutations_are_rejected_before_side_effects();
  test_non_owner_lifecycle_mutation_is_rejected_without_source_side_effect();
  test_sequential_host_threads_cannot_inherit_lifecycle_ownership();
  test_added_poll_and_notify_signals_use_service_workers();
  test_notifications_preserve_metadata_confidence();
  test_lighting_sink_binding_is_stopped_only();
  test_failed_shared_source_start_does_not_stop_existing_owner();
  test_source_start_failure_without_ownership_is_restartable();
  test_partial_source_start_cleanup_success_is_restartable();
  test_partial_source_start_cleanup_failure_is_retried_by_stop();
  test_immediate_worker_receive_fault_is_not_overwritten_by_running();
  test_unknown_frame_is_transport_traffic_and_expires();
  test_sustained_bounded_overload_services_expiry_and_latest_state();
  test_transport_liveness_uses_acquisition_clock();
  test_notifications_coalesce_and_recover();
  test_blocked_callback_does_not_block_polling_and_stop_is_retryable();
  test_dispatch_progress_counts_idle_loop_passes_and_stops_in_blocked_callback();
  test_callback_stop_timeout_can_retry_after_source_already_stopped();
  test_terminal_receive_fault_is_propagated_and_restart_clears_state();
  test_lighting_startup_black_deadline_heartbeat_and_failure_retry();
  test_lighting_brake_update_remains_unverified_and_binding_fails_off();
  test_lighting_heartbeat_for_off_unknown_nodata_and_unavailable();
  test_public_facade_private_lighting_binding();
  test_public_facade_lifecycle_contract();
  return failures == 0 ? 0 : 1;
}
