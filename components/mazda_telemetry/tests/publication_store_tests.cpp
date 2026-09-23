#include "mazda/publication_store.hpp"

#include <atomic>
#include <cstdint>
#include <iostream>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>

#include "vehicle_core/time.hpp"

namespace {

class FakeClock final : public vehicle_core::MonotonicClock {
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

// Pauses one clock read after its value has been captured. The test can then
// publish a new handoff while the reader is between its synchronized copy and
// its availability evaluation. This makes the publication/clock ordering
// deterministic instead of relying on a scheduling race.
class ForcedInterleavingClock final : public vehicle_core::MonotonicClock {
public:
  [[nodiscard]] vehicle_core::MonotonicTimestamp now() const noexcept override {
    const auto captured = now_us_.load(std::memory_order_acquire);
    if (armed_.exchange(false, std::memory_order_acq_rel)) {
      entered_.store(true, std::memory_order_release);
      while (!release_.load(std::memory_order_acquire))
        std::this_thread::yield();
    }
    return captured;
  }

  void set(const vehicle_core::MonotonicTimestamp now_us) noexcept {
    now_us_.store(now_us, std::memory_order_release);
  }

  void arm() noexcept {
    release_.store(false, std::memory_order_release);
    entered_.store(false, std::memory_order_release);
    armed_.store(true, std::memory_order_release);
  }

  void wait_until_entered() const noexcept {
    while (!entered_.load(std::memory_order_acquire))
      std::this_thread::yield();
  }

  void release() noexcept { release_.store(true, std::memory_order_release); }

private:
  std::atomic<vehicle_core::MonotonicTimestamp> now_us_{0};
  mutable std::atomic<bool> armed_{false};
  mutable std::atomic<bool> entered_{false};
  mutable std::atomic<bool> release_{false};
};

int failures = 0;

void expect(const bool condition, const char *expression, const char *file, const int line) {
  if (!condition) {
    std::cerr << file << ':' << line << ": failed: " << expression << '\n';
    ++failures;
  }
}

#define EXPECT(condition) expect((condition), #condition, __FILE__, __LINE__)

mazda::Diagnostics diagnostics(const mazda::LifecycleState lifecycle,
                               const vehicle_core::TransportHealth transport,
                               const std::uint64_t frames_received = 0) {
  mazda::Diagnostics result{};
  result.lifecycle = lifecycle;
  result.transport = transport;
  result.acquisition.frames_received = frames_received;
  return result;
}

template <typename Store, typename = void> struct accepts_legacy_publish : std::false_type {};

template <typename Store>
struct accepts_legacy_publish<Store, std::void_t<decltype(std::declval<Store &>().publish(
                                         std::declval<const mazda::VehicleState &>(),
                                         std::declval<const mazda::Diagnostics &>()))>>
    : std::true_type {};

template <typename Store, typename = void>
struct accepts_lifecycle_publish_without_transport : std::false_type {};

template <typename Store>
struct accepts_lifecycle_publish_without_transport<
    Store, std::void_t<decltype(std::declval<Store &>().publish(
               std::declval<const mazda::VehicleState &>(), std::declval<mazda::LifecycleState>(),
               std::declval<vehicle_core::TransportHealth>(),
               std::declval<const mazda::AcquisitionMetrics &>()))>> : std::true_type {};

static_assert(!accepts_legacy_publish<mazda::internal::PublicationStore>::value,
              "publication must require an explicit transport receive basis");
static_assert(
    !accepts_lifecycle_publish_without_transport<mazda::internal::PublicationStore>::value,
    "lifecycle publication must require an explicit transport receive basis");

void test_availability_and_reset() {
  FakeClock clock;
  mazda::TelemetryConfig config{};
  config.freshness.speed_kph_timeout_us = 100;
  // RPM intentionally remains unconfigured: it must report
  // FreshnessUnverified rather than acquiring an invented timeout.
  mazda::internal::PublicationStore store{clock, config};

  store.reset(
      diagnostics(mazda::LifecycleState::Running, vehicle_core::TransportHealth::AwaitingTraffic));
  EXPECT(store.speed_kph().availability == mazda::Availability::NoData);
  EXPECT(store.engine_rpm().availability == mazda::Availability::NoData);

  mazda::VehicleState state{};
  EXPECT(state.speed_kph.update(42.5F, 10));
  EXPECT(state.engine_rpm.update(2'000.0F, 10));
  EXPECT(state.liftgate_open.update(true, 10));
  store.publish(state,
                diagnostics(mazda::LifecycleState::Running, vehicle_core::TransportHealth::Live, 1),
                10);

  clock.set(110);
  const auto speed = store.speed_kph();
  const auto rpm = store.engine_rpm();
  EXPECT(speed.availability == mazda::Availability::Fresh);
  EXPECT(rpm.availability == mazda::Availability::FreshnessUnverified);
  EXPECT(speed.validation == mazda::ValidationStatus::Reference);
  EXPECT(rpm.validation == mazda::ValidationStatus::Confirmed);
  EXPECT(speed.value.has_value() && *speed.value == 42.5F);
  EXPECT(rpm.value.has_value() && *rpm.value == 2'000.0F);
  const auto test_only_liftgate =
      store.read_test_signal(&mazda::VehicleState::liftgate_open, mazda::candidate::kDoorsId);
  EXPECT(test_only_liftgate.availability == mazda::Availability::FreshnessUnverified);
  EXPECT(test_only_liftgate.value.has_value() && *test_only_liftgate.value);

  clock.set(111);
  EXPECT(store.speed_kph().availability == mazda::Availability::Stale);
  EXPECT(store.engine_rpm().availability == mazda::Availability::FreshnessUnverified);

  // Transport silence is evaluated by vehicle_telemetry::Runtime. The store
  // consumes the explicit transport state from its observer handoff.
  clock.set(1'000'111);
  store.publish(
      state,
      diagnostics(mazda::LifecycleState::Running, vehicle_core::TransportHealth::TimedOut, 3),
      std::nullopt);
  EXPECT(store.diagnostics().transport == vehicle_core::TransportHealth::TimedOut);
  EXPECT(store.speed_kph().availability == mazda::Availability::Unavailable);
  clock.set(111);

  // A copied message fault makes both fields unavailable while retaining the
  // last accepted values for diagnostics/consumer display.
  vehicle_core::RawCanFrame malformed{};
  malformed.identifier = mazda::candidate::kEngineDataId;
  malformed.timestamp_us = 20;
  EXPECT(state.observe_message(malformed, vehicle_core::DecodeValidity::Malformed) ==
         mazda::MessageObservationResult::Accepted);
  store.publish(state,
                diagnostics(mazda::LifecycleState::Running, vehicle_core::TransportHealth::Live, 2),
                20);
  const auto unavailable_message = store.engine_rpm();
  EXPECT(unavailable_message.availability == mazda::Availability::Unavailable);
  EXPECT(unavailable_message.value.has_value() && *unavailable_message.value == 2'000.0F);

  store.publish(
      state, diagnostics(mazda::LifecycleState::Running, vehicle_core::TransportHealth::Faulted, 3),
      std::nullopt);
  const auto unavailable_speed = store.speed_kph();
  EXPECT(unavailable_speed.availability == mazda::Availability::Unavailable);
  EXPECT(unavailable_speed.value.has_value() && *unavailable_speed.value == 42.5F);

  // A restart handoff must clear old-run observations before awaiting traffic.
  store.reset(
      diagnostics(mazda::LifecycleState::Running, vehicle_core::TransportHealth::AwaitingTraffic));
  EXPECT(store.speed_kph().availability == mazda::Availability::NoData);
  EXPECT(!store.speed_kph().value.has_value());
  EXPECT(store.engine_rpm().availability == mazda::Availability::NoData);

  // Configuration is lifecycle-gated by the private handoff, not by a public
  // mutable policy API.
  EXPECT(!store.configure(config).ok());
  store.reset();
  EXPECT(store.configure(config).ok());
}

void test_unrelated_receive_keeps_transport_live() {
  FakeClock clock;
  mazda::TelemetryConfig config{};
  config.transport_silence_timeout_us = 100;
  mazda::internal::PublicationStore store{clock, config};
  store.reset(
      diagnostics(mazda::LifecycleState::Running, vehicle_core::TransportHealth::AwaitingTraffic));

  mazda::VehicleState state{};
  EXPECT(state.speed_kph.update(8.0F, 10));
  store.publish(state,
                diagnostics(mazda::LifecycleState::Running, vehicle_core::TransportHealth::Live, 1),
                10);

  // This is a receive-only transport watermark for an unrelated/unsupported
  // frame. It must not modify state, but it does prove the receiver is live.
  clock.set(90);
  store.publish(state,
                diagnostics(mazda::LifecycleState::Running, vehicle_core::TransportHealth::Live, 2),
                90);
  EXPECT(store.diagnostics().transport == vehicle_core::TransportHealth::Live);
  EXPECT(store.speed_kph().availability == mazda::Availability::FreshnessUnverified);
  EXPECT(store.speed_kph().value.has_value() && *store.speed_kph().value == 8.0F);

  clock.set(191);
  store.publish(
      state,
      diagnostics(mazda::LifecycleState::Running, vehicle_core::TransportHealth::TimedOut, 3),
      std::nullopt);
  EXPECT(store.diagnostics().transport == vehicle_core::TransportHealth::TimedOut);
  EXPECT(store.speed_kph().availability == mazda::Availability::Unavailable);
}

void test_polling_copies_publication_before_sampling_clock() {
  ForcedInterleavingClock clock;
  mazda::TelemetryConfig config{};
  config.freshness.speed_kph_timeout_us = 0;
  mazda::internal::PublicationStore store{clock, config};
  store.reset(
      diagnostics(mazda::LifecycleState::Running, vehicle_core::TransportHealth::AwaitingTraffic));

  mazda::VehicleState next{};
  EXPECT(next.speed_kph.update(42.5F, 0));

  mazda::Reading<float> observed{};
  clock.arm();
  std::thread reader{[&] { observed = store.speed_kph(); }};
  clock.wait_until_entered();

  // The reader has captured t=0. Publishing at the later wall-clock value
  // must not make the old no-data copy appear to be a fresh observation.
  clock.set(1'000);
  store.publish(
      next, diagnostics(mazda::LifecycleState::Running, vehicle_core::TransportHealth::Live), 0);
  clock.release();
  reader.join();

  EXPECT(observed.availability == mazda::Availability::NoData);
  EXPECT(!observed.value.has_value());
  EXPECT(store.speed_kph().availability == mazda::Availability::Stale);
}

void test_diagnostics_copies_publication_before_sampling_clock() {
  FakeClock clock;
  mazda::TelemetryConfig config{};
  mazda::internal::PublicationStore store{clock, config};
  store.reset(
      diagnostics(mazda::LifecycleState::Running, vehicle_core::TransportHealth::AwaitingTraffic));
  EXPECT(store.diagnostics().transport == vehicle_core::TransportHealth::AwaitingTraffic);
}

void test_snapshot_copies_publication_before_sampling_clock() {
  FakeClock clock;
  mazda::TelemetryConfig config{};
  mazda::internal::PublicationStore store{clock, config};
  store.reset(
      diagnostics(mazda::LifecycleState::Running, vehicle_core::TransportHealth::AwaitingTraffic));
  EXPECT(store.snapshot().diagnostics.transport == vehicle_core::TransportHealth::AwaitingTraffic);
}

void test_coherent_snapshot_under_concurrent_publication() {
  FakeClock clock;
  mazda::TelemetryConfig config{};
  config.freshness.speed_kph_timeout_us = 1'000'000;
  config.freshness.engine_rpm_timeout_us = 1'000'000;
  mazda::internal::PublicationStore store{clock, config};
  store.reset(diagnostics(mazda::LifecycleState::Running, vehicle_core::TransportHealth::Live));

  std::atomic<bool> writer_done{false};
  std::atomic<bool> inconsistent{false};
  std::thread writer{[&] {
    mazda::VehicleState state{};
    for (std::uint64_t sequence = 1; sequence <= 50'000; ++sequence) {
      const auto value = static_cast<float>(sequence);
      if (!state.speed_kph.update(value, sequence) || !state.engine_rpm.update(value, sequence) ||
          !state.liftgate_open.update((sequence % 2U) == 0U, sequence)) {
        inconsistent.store(true, std::memory_order_release);
        break;
      }
      store.publish(state,
                    diagnostics(mazda::LifecycleState::Running, vehicle_core::TransportHealth::Live,
                                sequence),
                    sequence);
    }
    writer_done.store(true, std::memory_order_release);
  }};

  std::thread reader{[&] {
    while (!writer_done.load(std::memory_order_acquire)) {
      const auto snapshot = store.snapshot();
      const auto test_channel =
          store.read_test_signal(&mazda::VehicleState::liftgate_open, mazda::candidate::kDoorsId);
      if (test_channel.value.has_value() &&
          test_channel.availability != mazda::Availability::FreshnessUnverified) {
        inconsistent.store(true, std::memory_order_release);
        break;
      }
      if (!snapshot.state.speed_kph.has_value || !snapshot.state.engine_rpm.has_value)
        continue;
      if (snapshot.state.speed_kph.value != snapshot.state.engine_rpm.value ||
          snapshot.diagnostics.acquisition.frames_received !=
              static_cast<std::uint64_t>(snapshot.state.speed_kph.value)) {
        inconsistent.store(true, std::memory_order_release);
        break;
      }
    }
  }};

  writer.join();
  reader.join();
  EXPECT(!inconsistent.load(std::memory_order_acquire));
  EXPECT(store.speed_kph().value.has_value());
  EXPECT(store.engine_rpm().value.has_value());
}

} // namespace

int main() {
  test_availability_and_reset();
  test_unrelated_receive_keeps_transport_live();
  test_polling_copies_publication_before_sampling_clock();
  test_diagnostics_copies_publication_before_sampling_clock();
  test_snapshot_copies_publication_before_sampling_clock();
  test_coherent_snapshot_under_concurrent_publication();
  if (failures != 0)
    std::cerr << failures << " publication-store assertion(s) failed\n";
  return failures == 0 ? 0 : 1;
}
