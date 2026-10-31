#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <mutex>
#include <new>
#include <thread>

#include "vehicle_core/telemetry_contracts.hpp"
#include "vehicle_core/time.hpp"
#include "vehicle_profile_runtime/provider.hpp"
#include "vehicle_profile_runtime/service.hpp"
#include "vehicle_profile_runtime/tables.hpp"
#include "vehicle_signals/signal_provider.hpp"
#include "vehicle_telemetry/receive.hpp"

namespace {
std::atomic<bool> g_watch_heap_allocations{false};
std::atomic<std::size_t> g_watched_heap_allocations{0};
} // namespace

void *operator new(const std::size_t size) {
  if (g_watch_heap_allocations.load(std::memory_order_relaxed)) {
    g_watched_heap_allocations.fetch_add(1U, std::memory_order_relaxed);
  }
  if (void *const allocation = std::malloc(size == 0U ? 1U : size)) {
    return allocation;
  }
  throw std::bad_alloc{};
}

void *operator new[](const std::size_t size) {
  if (g_watch_heap_allocations.load(std::memory_order_relaxed)) {
    g_watched_heap_allocations.fetch_add(1U, std::memory_order_relaxed);
  }
  if (void *const allocation = std::malloc(size == 0U ? 1U : size)) {
    return allocation;
  }
  throw std::bad_alloc{};
}

void operator delete(void *allocation) noexcept { std::free(allocation); }
void operator delete[](void *allocation) noexcept { std::free(allocation); }
void operator delete(void *allocation, std::size_t) noexcept { std::free(allocation); }
void operator delete[](void *allocation, std::size_t) noexcept { std::free(allocation); }

namespace {

using vehicle_profile_runtime::DecodeResult;
using vehicle_profile_runtime::DispatchEntry;
using vehicle_profile_runtime::ProfileRuntimeService;
using vehicle_profile_runtime::ProfileTables;
using vehicle_profile_runtime::SignalFreshnessPolicy;
using vehicle_signals::Availability;
using vehicle_signals::SignalCallback;
using vehicle_signals::SignalCapability;
using vehicle_signals::SignalCatalogView;
using vehicle_signals::SignalId;
using vehicle_signals::SignalMetadata;
using vehicle_signals::SignalNotification;
using vehicle_signals::SignalStatus;
using vehicle_signals::SignalType;
using vehicle_signals::SignalUnit;
using vehicle_signals::SignalValue;
using vehicle_signals::ValidationStatus;
using vehicle_telemetry::AcquisitionSource;
using vehicle_telemetry::AcquisitionStatistics;
using vehicle_telemetry::ReceiveStatus;
using vehicle_telemetry::ResultCode;
using vehicle_telemetry::StatusResult;

class FakeClock final : public vehicle_core::MonotonicClock {
public:
  [[nodiscard]] vehicle_core::MonotonicTimestamp now() const noexcept override {
    return now_us_.load(std::memory_order_relaxed);
  }

  void set(vehicle_core::MonotonicTimestamp value) noexcept {
    now_us_.store(value, std::memory_order_relaxed);
  }

private:
  std::atomic<vehicle_core::MonotonicTimestamp> now_us_{0};
};

class FakeSource final : public AcquisitionSource {
public:
  [[nodiscard]] StatusResult start() noexcept override {
    std::lock_guard<std::mutex> lock{mutex_};
    if (running_) {
      return {ResultCode::AlreadyRunning};
    }
    ++start_calls_;
    head_ = 0;
    tail_ = 0;
    completed_frames_ = 0;
    statistics_ = {};
    faulted_ = false;
    if (partial_start_failure_) {
      partial_start_failure_ = false;
      ownership_ = true;
      return {ResultCode::Faulted};
    }
    ownership_ = true;
    running_ = true;
    changed_.notify_all();
    return {ResultCode::Ok};
  }

  [[nodiscard]] StatusResult stop() noexcept override {
    std::lock_guard<std::mutex> lock{mutex_};
    ++stop_calls_;
    running_ = false;
    changed_.notify_all();
    if (ownership_ && stop_timeouts_ > 0U) {
      --stop_timeouts_;
      return {ResultCode::Timeout};
    }
    ownership_ = false;
    return {ResultCode::Ok};
  }

  [[nodiscard]] bool supports_stop_retry() const noexcept override { return true; }

  [[nodiscard]] StatusResult retry_stop() noexcept override {
    std::lock_guard<std::mutex> lock{mutex_};
    ++retry_stop_calls_;
    running_ = false;
    changed_.notify_all();
    if (!ownership_) {
      return {ResultCode::NotRunning};
    }
    ownership_ = false;
    return {ResultCode::Ok};
  }

  [[nodiscard]] ReceiveStatus receive(vehicle_core::RawCanFrame &frame,
                                      std::uint32_t timeout_ms) noexcept override {
    std::unique_lock<std::mutex> lock{mutex_};
    completed_frames_ = statistics_.frames_received;
    changed_.notify_all();
    (void)changed_.wait_for(lock, std::chrono::milliseconds{timeout_ms},
                            [this] { return head_ != tail_ || faulted_ || !running_; });
    if (faulted_) {
      return ReceiveStatus::Fault;
    }
    if (!running_) {
      return ReceiveStatus::NotStarted;
    }
    if (head_ == tail_) {
      return ReceiveStatus::Timeout;
    }
    frame = frames_[tail_ % frames_.size()];
    ++tail_;
    ++statistics_.frames_received;
    return ReceiveStatus::Frame;
  }

  [[nodiscard]] AcquisitionStatistics statistics() const noexcept override {
    std::lock_guard<std::mutex> lock{mutex_};
    return statistics_;
  }

  void push(const vehicle_core::RawCanFrame &frame) noexcept {
    {
      std::lock_guard<std::mutex> lock{mutex_};
      if (head_ - tail_ == frames_.size()) {
        ++statistics_.queue_overflows;
        return;
      }
      frames_[head_ % frames_.size()] = frame;
      ++head_;
    }
    changed_.notify_all();
  }

  void fail() noexcept {
    {
      std::lock_guard<std::mutex> lock{mutex_};
      faulted_ = true;
    }
    changed_.notify_all();
  }

  void fail_next_start_with_retained_ownership() noexcept {
    std::lock_guard<std::mutex> lock{mutex_};
    partial_start_failure_ = true;
  }

  void time_out_next_stop() noexcept {
    std::lock_guard<std::mutex> lock{mutex_};
    ++stop_timeouts_;
  }

  [[nodiscard]] std::size_t start_calls() const noexcept {
    std::lock_guard<std::mutex> lock{mutex_};
    return start_calls_;
  }

  [[nodiscard]] std::size_t stop_calls() const noexcept {
    std::lock_guard<std::mutex> lock{mutex_};
    return stop_calls_;
  }

  [[nodiscard]] std::size_t retry_stop_calls() const noexcept {
    std::lock_guard<std::mutex> lock{mutex_};
    return retry_stop_calls_;
  }

  [[nodiscard]] bool wait_for_completed_frames(const std::size_t expected) {
    std::unique_lock<std::mutex> lock{mutex_};
    return changed_.wait_for(lock, std::chrono::milliseconds{500},
                             [this, expected] { return completed_frames_ >= expected; });
  }

private:
  mutable std::mutex mutex_{};
  std::condition_variable changed_{};
  std::array<vehicle_core::RawCanFrame, 8> frames_{};
  std::size_t head_{0};
  std::size_t tail_{0};
  AcquisitionStatistics statistics_{};
  std::size_t start_calls_{0};
  std::size_t stop_calls_{0};
  std::size_t retry_stop_calls_{0};
  std::size_t completed_frames_{0};
  std::size_t stop_timeouts_{0};
  bool running_{false};
  bool faulted_{false};
  bool ownership_{false};
  bool partial_start_failure_{false};
};

constexpr SignalMetadata kSignals[]{
    {SignalId{1}, "speed", SignalType::Number, SignalUnit::KilometresPerHour,
     ValidationStatus::Confirmed, SignalCapability::Read, nullptr, 0},
    {SignalId{2}, "notice", SignalType::Boolean, SignalUnit::None, ValidationStatus::Reference,
     SignalCapability::Read | SignalCapability::Notify, nullptr, 0},
};
constexpr SignalFreshnessPolicy kFreshness[]{{SignalId{1}, true, 60'000'000},
                                             {SignalId{2}, true, 60'000'000}};
constexpr SignalId kOutputs[]{SignalId{1}, SignalId{2}};

void decode_speed(const vehicle_core::RawCanFrame &frame, DecodeResult *results,
                  const std::size_t capacity, std::size_t &count) {
  (void)vehicle_profile_runtime::append_result(
      results, capacity, count,
      DecodeResult::success(SignalId{1}, SignalValue::number(frame.data[0])));
  (void)vehicle_profile_runtime::append_result(
      results, capacity, count,
      DecodeResult::success(SignalId{2}, SignalValue::boolean(frame.data[0] != 0U)));
}

constexpr DispatchEntry kDispatch[]{{0x101U, 1U, &decode_speed, kOutputs, std::size(kOutputs)}};
const ProfileTables kTables{SignalCatalogView{kSignals}, kFreshness,
                            std::size(kFreshness),       kDispatch,
                            std::size(kDispatch),        std::size(kOutputs)};

[[nodiscard]] vehicle_core::RawCanFrame make_frame(const vehicle_core::MonotonicTimestamp timestamp,
                                                   const std::uint8_t speed) noexcept {
  vehicle_core::RawCanFrame frame{};
  frame.identifier = 0x101U;
  frame.timestamp_us = timestamp;
  frame.data[0] = speed;
  frame.dlc = 1U;
  return frame;
}

[[nodiscard]] bool wait_for_availability(vehicle_signals::SignalProvider &provider,
                                         const SignalId id, const Availability expected) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{500};
  while (std::chrono::steady_clock::now() < deadline) {
    const auto reading = provider.read(id);
    if (reading.ok() && reading.value->availability == expected) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  return false;
}

struct CallbackMutation final {
  ProfileRuntimeService *service{nullptr};
  std::mutex mutex{};
  std::condition_variable changed{};
  std::size_t calls{0};
  bool done{false};
  ResultCode stop_status{ResultCode::Faulted};
};

void ignore_notice(void *, const SignalNotification &) noexcept {}

void attempt_stop_from_callback(void *context, const SignalNotification &notice) noexcept {
  auto &mutation = *static_cast<CallbackMutation *>(context);
  {
    std::lock_guard<std::mutex> lock{mutation.mutex};
    if (!notice.initial || mutation.calls != 0U) {
      return;
    }
    ++mutation.calls;
  }
  const ResultCode status = mutation.service->stop().status;
  {
    std::lock_guard<std::mutex> lock{mutation.mutex};
    mutation.stop_status = status;
    mutation.done = true;
  }
  mutation.changed.notify_all();
}

TEST_CASE(
    "profile runtime stop timeout retains source ownership for retry and restart clears samples") {
  FakeClock clock;
  clock.set(10'000U);
  FakeSource source;
  ProfileRuntimeService service{kTables, clock, source};

  REQUIRE(service.start().ok());
  source.push(make_frame(10'001U, 42U));
  REQUIRE(wait_for_availability(service.provider(), SignalId{1}, Availability::Fresh));
  const auto before_stop = service.provider().read(SignalId{1});
  REQUIRE(before_stop.ok());
  REQUIRE(before_stop.value->value.has_value());
  CHECK(before_stop.value->value->as_number() == 42.0F);

  source.time_out_next_stop();
  CHECK(service.stop().status == ResultCode::Timeout);
  CHECK(source.stop_calls() == 1U);
  CHECK(source.retry_stop_calls() == 0U);
  int unused_context = 0;
  CHECK(service.provider().subscribe(SignalId{2}, &ignore_notice, &unused_context).status ==
        SignalStatus::InvalidState);

  CHECK(service.stop().ok());
  CHECK(source.stop_calls() == 1U);
  CHECK(source.retry_stop_calls() == 1U);
  CHECK(wait_for_availability(service.provider(), SignalId{1}, Availability::NoData));

  REQUIRE(service.start().ok());
  CHECK(wait_for_availability(service.provider(), SignalId{1}, Availability::NoData));
  source.push(make_frame(20'000U, 17U));
  REQUIRE(wait_for_availability(service.provider(), SignalId{1}, Availability::Fresh));
  const auto after_restart = service.provider().read(SignalId{1});
  REQUIRE(after_restart.ok());
  REQUIRE(after_restart.value->value.has_value());
  CHECK(after_restart.value->value->as_number() == 17.0F);
  CHECK(service.stop().ok());
}

struct AllocationCounter final {
  std::atomic<std::size_t> callbacks{0};
};

void count_notice(void *context, const SignalNotification &) noexcept {
  static_cast<AllocationCounter *>(context)->callbacks.fetch_add(1U, std::memory_order_relaxed);
}

TEST_CASE("profile runtime construction and runtime work avoid C++ heap allocations") {
  FakeClock clock;
  FakeSource source;
  AllocationCounter callback{};
  alignas(ProfileRuntimeService) std::byte storage[sizeof(ProfileRuntimeService)]{};

  g_watched_heap_allocations.store(0U, std::memory_order_relaxed);
  g_watch_heap_allocations.store(true, std::memory_order_release);
  auto *const service =
      ::new (static_cast<void *>(storage)) ProfileRuntimeService{kTables, clock, source};
  g_watch_heap_allocations.store(false, std::memory_order_release);
  CHECK(g_watched_heap_allocations.load(std::memory_order_relaxed) == 0U);

  REQUIRE(service->provider().subscribe(SignalId{2}, &count_notice, &callback).ok());
  // The pinned host Runtime starts its own std::thread and owns that platform
  // worker resource. The guard below covers adapter processing and dispatch.
  const auto started = service->start();
  REQUIRE(started.ok());

  const auto initial_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{500};
  while (callback.callbacks.load(std::memory_order_relaxed) == 0U &&
         std::chrono::steady_clock::now() < initial_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  REQUIRE(callback.callbacks.load(std::memory_order_relaxed) > 0U);

  bool fresh = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{500};
  const std::size_t callback_count = callback.callbacks.load(std::memory_order_relaxed);
  g_watched_heap_allocations.store(0U, std::memory_order_relaxed);
  g_watch_heap_allocations.store(true, std::memory_order_release);
  source.push(make_frame(1U, 12U));
  while (std::chrono::steady_clock::now() < deadline) {
    const auto reading = service->provider().read(SignalId{1});
    if (reading.ok() && reading.value->availability == Availability::Fresh &&
        callback.callbacks.load(std::memory_order_relaxed) > callback_count) {
      fresh = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  g_watch_heap_allocations.store(false, std::memory_order_release);
  CHECK(g_watched_heap_allocations.load(std::memory_order_relaxed) == 0U);
  CHECK(fresh);

  g_watched_heap_allocations.store(0U, std::memory_order_relaxed);
  g_watch_heap_allocations.store(true, std::memory_order_release);
  const auto stopped = service->stop();
  g_watch_heap_allocations.store(false, std::memory_order_release);
  CHECK(g_watched_heap_allocations.load(std::memory_order_relaxed) == 0U);
  CHECK(stopped.ok());
  service->~ProfileRuntimeService();
}

TEST_CASE("destroying a running profile runtime quiesces its dispatcher") {
  FakeClock clock;
  FakeSource source;
  {
    ProfileRuntimeService service{kTables, clock, source};
    REQUIRE(service.start().ok());
  }
  CHECK(source.stop_calls() >= 1U);
}

TEST_CASE("profile runtime driver failure invalidates a fresh sample immediately") {
  FakeClock clock;
  clock.set(30'000U);
  FakeSource source;
  ProfileRuntimeService service{kTables, clock, source};

  REQUIRE(service.start().ok());
  source.push(make_frame(30'001U, 55U));
  REQUIRE(wait_for_availability(service.provider(), SignalId{1}, Availability::Fresh));

  source.fail();
  CHECK(wait_for_availability(service.provider(), SignalId{1}, Availability::Unavailable));
  CHECK(service.stop().ok());
}

TEST_CASE("profile runtime fault ignores unknown and old frames until a newer frame recovers") {
  FakeClock clock;
  clock.set(100'000U);
  FakeSource source;
  ProfileRuntimeService service{kTables, clock, source};

  REQUIRE(service.start().ok());
  source.push(make_frame(100'001U, 55U));
  REQUIRE(wait_for_availability(service.provider(), SignalId{1}, Availability::Fresh));

  clock.set(1'300'000U);
  REQUIRE(wait_for_availability(service.provider(), SignalId{1}, Availability::Unavailable));
  CHECK(service.provider().read(SignalId{1}).value->availability == Availability::Unavailable);

  auto unknown = make_frame(100'001U, 99U);
  unknown.identifier = 0x999U;
  source.push(unknown);
  source.push(make_frame(100'001U, 99U));
  source.push(make_frame(100'000U, 99U));
  std::this_thread::sleep_for(std::chrono::milliseconds{25});
  CHECK(service.provider().read(SignalId{1}).value->availability == Availability::Unavailable);

  source.push(make_frame(1'300'001U, 56U));
  REQUIRE(wait_for_availability(service.provider(), SignalId{1}, Availability::Fresh));
  const auto recovered = service.provider().read(SignalId{1});
  REQUIRE(recovered.ok());
  REQUIRE(recovered.value->value.has_value());
  CHECK(recovered.value->value->as_number() == 56.0F);
  CHECK(service.stop().ok());
}

TEST_CASE("profile runtime callback lifecycle mutation is rejected before source effects") {
  FakeClock clock;
  FakeSource source;
  ProfileRuntimeService service{kTables, clock, source};
  CallbackMutation mutation{&service};
  const auto subscription =
      service.provider().subscribe(SignalId{2}, &attempt_stop_from_callback, &mutation);
  REQUIRE(subscription.ok());

  REQUIRE(service.start().ok());
  {
    std::unique_lock<std::mutex> lock{mutation.mutex};
    REQUIRE(mutation.changed.wait_for(lock, std::chrono::milliseconds{500},
                                      [&mutation] { return mutation.done; }));
    CHECK(mutation.calls == 1U);
    CHECK(mutation.stop_status == ResultCode::InvalidState);
  }
  CHECK(source.stop_calls() == 0U);
  CHECK(service.stop().ok());
}

struct BlockingCallback final {
  std::mutex mutex{};
  std::condition_variable changed{};
  std::size_t calls{0};
  bool entered{false};
  bool release{false};
  bool transition_seen{false};
  SignalNotification transition{};
};

void block_first_notice(void *context, const SignalNotification &notice) noexcept {
  auto &callback = *static_cast<BlockingCallback *>(context);
  std::unique_lock<std::mutex> lock{callback.mutex};
  ++callback.calls;
  if (notice.initial) {
    callback.entered = true;
    callback.changed.notify_all();
    callback.changed.wait(lock, [&callback] { return callback.release; });
    return;
  }
  callback.transition = notice;
  callback.transition_seen = true;
  callback.changed.notify_all();
}

TEST_CASE("profile runtime stop timeout retains callbacks until dispatcher quiesces") {
  FakeClock clock;
  FakeSource source;
  ProfileRuntimeService service{kTables, clock, source};
  BlockingCallback callback{};
  const auto subscription =
      service.provider().subscribe(SignalId{2}, &block_first_notice, &callback);
  REQUIRE(subscription.ok());

  REQUIRE(service.start().ok());
  {
    std::unique_lock<std::mutex> lock{callback.mutex};
    REQUIRE(callback.changed.wait_for(lock, std::chrono::milliseconds{500},
                                      [&callback] { return callback.entered; }));
  }

  CHECK(service.stop().status == ResultCode::Timeout);
  CHECK(source.stop_calls() == 1U);
  CHECK(service.provider().unsubscribe(*subscription.value).status == SignalStatus::InvalidState);

  {
    std::lock_guard<std::mutex> lock{callback.mutex};
    callback.release = true;
  }
  callback.changed.notify_all();
  CHECK(service.stop().ok());
  std::size_t stopped_callback_count = 0;
  {
    std::lock_guard<std::mutex> lock{callback.mutex};
    stopped_callback_count = callback.calls;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds{25});
  {
    std::lock_guard<std::mutex> lock{callback.mutex};
    CHECK(callback.calls == stopped_callback_count);
  }
}

TEST_CASE("profile runtime publishes transitions while a callback is blocked") {
  FakeClock clock;
  clock.set(100'000U);
  FakeSource source;
  ProfileRuntimeService service{kTables, clock, source};
  BlockingCallback callback{};
  const auto subscription =
      service.provider().subscribe(SignalId{2}, &block_first_notice, &callback);
  REQUIRE(subscription.ok());

  REQUIRE(service.start().ok());
  {
    std::unique_lock<std::mutex> lock{callback.mutex};
    REQUIRE(callback.changed.wait_for(lock, std::chrono::milliseconds{500},
                                      [&callback] { return callback.entered; }));
  }

  source.push(make_frame(100'001U, 42U));
  REQUIRE(wait_for_availability(service.provider(), SignalId{2}, Availability::Fresh));
  REQUIRE(source.wait_for_completed_frames(1U));
  auto malformed = make_frame(100'002U, 99U);
  malformed.dlc = 0U;
  source.push(malformed);
  REQUIRE(wait_for_availability(service.provider(), SignalId{2}, Availability::Unavailable));
  REQUIRE(source.wait_for_completed_frames(2U));
  source.push(make_frame(100'003U, 0U));
  REQUIRE(wait_for_availability(service.provider(), SignalId{2}, Availability::Fresh));
  REQUIRE(source.wait_for_completed_frames(3U));

  {
    std::lock_guard<std::mutex> lock{callback.mutex};
    callback.release = true;
  }
  callback.changed.notify_all();
  {
    std::unique_lock<std::mutex> lock{callback.mutex};
    REQUIRE(callback.changed.wait_for(lock, std::chrono::milliseconds{500},
                                      [&callback] { return callback.transition_seen; }));
    CHECK(callback.transition.current.value.has_value());
    CHECK(callback.transition.current.value->as_boolean() == false);
    CHECK(callback.transition.became_unavailable);
    CHECK(callback.transition.recovered);
    CHECK(callback.transition.coalesced);
  }
  CHECK(service.stop().ok());
}

TEST_CASE("profile runtime failed start retains cleanup ownership until retry succeeds") {
  FakeClock clock;
  FakeSource source;
  source.fail_next_start_with_retained_ownership();
  source.time_out_next_stop();
  ProfileRuntimeService service{kTables, clock, source};

  CHECK(service.start().status == ResultCode::Faulted);
  CHECK(source.start_calls() == 1U);
  CHECK(source.stop_calls() == 1U);
  CHECK(source.retry_stop_calls() == 0U);

  CHECK(service.stop().ok());
  CHECK(source.retry_stop_calls() == 1U);
  CHECK(service.start().ok());
  CHECK(source.start_calls() == 2U);
  CHECK(service.stop().ok());
}

} // namespace
