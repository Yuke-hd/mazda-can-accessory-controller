#include "mazda/definitions.hpp"
#include "mazda/vehicle_telemetry_internal.hpp"
#include "mazda/vehicle_telemetry_service.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <mutex>
#include <optional>
#include <thread>

namespace {

using mazda::candidate::kAccelerationId;
using mazda::candidate::kBlinkInfoId;
using mazda::candidate::kBrakePedalId;
using mazda::candidate::kDoorsId;
using mazda::candidate::kEngineDataId;
using mazda::candidate::kGearId;
using mazda::candidate::kTurnSwitchId;

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

// Runtime owns the receive loop in production. Keeping this source blocked
// makes direct FrameProcessor/Observer calls in the harness the only source of
// work, while still exercising the service lifecycle and stop handoff.
class BlockedSource final : public vehicle_telemetry::AcquisitionSource {
public:
  [[nodiscard]] vehicle_telemetry::StatusResult start() noexcept override {
    std::lock_guard<std::mutex> lock{mutex_};
    if (running_)
      return {vehicle_telemetry::ResultCode::AlreadyRunning};
    running_ = true;
    return {vehicle_telemetry::ResultCode::Ok};
  }

  [[nodiscard]] vehicle_telemetry::StatusResult stop() noexcept override {
    {
      std::lock_guard<std::mutex> lock{mutex_};
      running_ = false;
    }
    changed_.notify_all();
    return {vehicle_telemetry::ResultCode::Ok};
  }

  [[nodiscard]] vehicle_telemetry::ReceiveStatus receive(vehicle_core::RawCanFrame &,
                                                         std::uint32_t) noexcept override {
    std::unique_lock<std::mutex> lock{mutex_};
    changed_.wait(lock, [this] { return !running_; });
    return vehicle_telemetry::ReceiveStatus::NotStarted;
  }

  [[nodiscard]] vehicle_telemetry::AcquisitionStatistics statistics() const noexcept override {
    std::lock_guard<std::mutex> lock{mutex_};
    return statistics_;
  }

private:
  mutable std::mutex mutex_{};
  std::condition_variable changed_{};
  vehicle_telemetry::AcquisitionStatistics statistics_{};
  bool running_{false};
};

class TestLightingSink final : public mazda::internal::LightingSink {
public:
  [[nodiscard]] bool publish(const mazda::LightingUpdate &) noexcept override { return true; }
};

// on_diagnostics is invoked on the runtime owner in firmware. The harness
// invokes it on a short-lived worker and parks it after publication, which
// gives the test a concrete publication boundary before it drains callbacks.
class PublicationGate final : public mazda::internal::HostPublicationControl {
public:
  void arm() noexcept {
    std::lock_guard<std::mutex> lock{mutex_};
    entered_ = false;
    released_ = false;
    armed_ = true;
  }

  void publication_completed() noexcept override {
    std::unique_lock<std::mutex> lock{mutex_};
    if (!armed_)
      return;
    entered_ = true;
    changed_.notify_all();
    changed_.wait(lock, [this] { return released_; });
    armed_ = false;
  }

  void wait_until_entered() noexcept {
    std::unique_lock<std::mutex> lock{mutex_};
    changed_.wait(lock, [this] { return entered_; });
  }

  void release() noexcept {
    {
      std::lock_guard<std::mutex> lock{mutex_};
      released_ = true;
    }
    changed_.notify_all();
  }

private:
  std::mutex mutex_{};
  std::condition_variable changed_{};
  bool armed_{false};
  bool entered_{false};
  bool released_{false};
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

vehicle_telemetry::TransportDiagnostics diagnostics(
    const vehicle_core::TransportHealth transport = vehicle_core::TransportHealth::Live,
    const vehicle_telemetry::LifecycleState lifecycle = vehicle_telemetry::LifecycleState::Running,
    const bool has_last_frame = false, const vehicle_core::MonotonicTimestamp last_frame_us = 0) {
  vehicle_telemetry::TransportDiagnostics result{};
  result.lifecycle = lifecycle;
  result.transport = transport;
  result.has_last_frame = has_last_frame;
  result.last_frame_us = last_frame_us;
  return result;
}

constexpr std::array<std::uint32_t, 8> kMessageIds{0x7ffU,       kEngineDataId, kAccelerationId,
                                                   kGearId,      kDoorsId,      kTurnSwitchId,
                                                   kBlinkInfoId, kBrakePedalId};

struct EvaluationCounts final {
  std::array<std::size_t, kMessageIds.size()> by_message{};

  void clear() noexcept { by_message.fill(0); }

  [[nodiscard]] std::size_t total() const noexcept {
    std::size_t result = 0;
    for (const auto count : by_message)
      result += count;
    return result;
  }
};

void count_descriptor(void *context, const std::uint32_t identifier) noexcept {
  auto &counts = *static_cast<EvaluationCounts *>(context);
  for (std::size_t index = 0; index < kMessageIds.size(); ++index) {
    if (kMessageIds[index] == identifier) {
      ++counts.by_message[index];
      return;
    }
  }
}

struct TurnRecorder final {
  std::array<mazda::Notification<mazda::TurnState>, 16> notices{};
  std::size_t count{0};
};

struct BrakeRecorder final {
  std::array<mazda::Notification<bool>, 8> notices{};
  std::size_t count{0};
};

void record_turn(void *context, const mazda::Notification<mazda::TurnState> &notice) noexcept {
  auto &recorder = *static_cast<TurnRecorder *>(context);
  if (recorder.count < recorder.notices.size())
    recorder.notices[recorder.count++] = notice;
}

void record_brake(void *context, const mazda::Notification<bool> &notice) noexcept {
  auto &recorder = *static_cast<BrakeRecorder *>(context);
  if (recorder.count < recorder.notices.size())
    recorder.notices[recorder.count++] = notice;
}

struct Harness final {
  TestClock clock{};
  BlockedSource source{};
  TestLightingSink lighting{};
  PublicationGate publication_gate{};
  EvaluationCounts evaluations{};
  mazda::internal::TelemetryProfilerHooks profiler{};
  mazda::TelemetryConfig config{};
  mazda::internal::VehicleTelemetryService service;
  std::uint64_t frame_count{0};

  explicit Harness(const mazda::TelemetryConfig supplied = {})
      : config(supplied),
        service(clock, source, lighting, config,
                mazda::internal::HostServiceOptions{&publication_gate, true, &profiler}) {
    profiler.context = &evaluations;
    profiler.notification_descriptor_evaluation = &count_descriptor;
  }

  bool start() {
    if (!service.start().ok())
      return false;
    const auto drained = service.drain_notifications();
    if (!drained.ok())
      return false;
    evaluations.clear();
    frame_count = 0;
    return true;
  }

  void publish(const vehicle_core::MonotonicTimestamp now_us,
               const vehicle_core::TransportHealth transport = vehicle_core::TransportHealth::Live,
               const vehicle_telemetry::LifecycleState lifecycle =
                   vehicle_telemetry::LifecycleState::Running,
               const bool has_last_frame = false,
               const vehicle_core::MonotonicTimestamp last_frame_us = 0) {
    clock.set(now_us);
    const auto report = diagnostics(transport, lifecycle, has_last_frame, last_frame_us);
    publication_gate.arm();
    std::thread publication([this, report] { service.on_diagnostics(report); });
    publication_gate.wait_until_entered();
    publication_gate.release();
    publication.join();
  }

  vehicle_telemetry::ProcessResult
  process(const vehicle_core::RawCanFrame &received,
          const std::optional<vehicle_core::MonotonicTimestamp> arrival_now_us = std::nullopt) {
    const auto now_us = arrival_now_us.value_or(received.timestamp_us);
    clock.set(now_us);
    const auto result = service.process(received);
    ++frame_count;
    publish(now_us, vehicle_core::TransportHealth::Live, vehicle_telemetry::LifecycleState::Running,
            true, now_us);
    return result;
  }

  std::size_t drain() {
    const auto result = service.drain_notifications();
    return result.ok() && result.value ? *result.value : 0;
  }

  void stop() { (void)service.stop(); }
};

int failures = 0;

void expect(const bool condition, const char *expression, const char *file, const int line) {
  if (!condition) {
    std::cerr << file << ':' << line << ": failed: " << expression << '\n';
    ++failures;
  }
}

#define EXPECT(condition) expect((condition), #condition, __FILE__, __LINE__)

void test_exact_ordinary_group_counts() {
  Harness harness{};
  EXPECT(harness.start());

  EXPECT(harness.process(frame(kEngineDataId, 100, {0x09, 0x5b, 0, 0, 0, 0, 0, 0})).status ==
         vehicle_telemetry::ProcessStatus::Processed);
  EXPECT(
      harness.process(frame(kAccelerationId, 101, {0x27, 0x11, 0x38, 0x80, 0, 0, 0, 0})).status ==
      vehicle_telemetry::ProcessStatus::Processed);
  EXPECT(harness.process(frame(kGearId, 102, {0x24, 0x81, 0x07, 0xff, 0x04, 0xf0, 0, 0})).status ==
         vehicle_telemetry::ProcessStatus::Processed);
  EXPECT(harness.process(frame(kDoorsId, 103, {0, 0, 0, 0x40, 0x3d, 0, 0, 0})).status ==
         vehicle_telemetry::ProcessStatus::Processed);
  EXPECT(harness.process(frame(kTurnSwitchId, 104, {0, 0x20, 0x10, 0, 0, 0, 0, 0})).status ==
         vehicle_telemetry::ProcessStatus::Processed);
  EXPECT(harness.process(frame(kBlinkInfoId, 105, {0, 0, 0x08, 0, 0x02, 0, 0, 0})).status ==
         vehicle_telemetry::ProcessStatus::Processed);
  EXPECT(harness.process(frame(kBrakePedalId, 106, {0x10, 0, 0, 0, 0, 0, 0, 0})).status ==
         vehicle_telemetry::ProcessStatus::Processed);
  EXPECT(harness.process(frame(kMessageIds[0], 107, {0, 0, 0, 0, 0, 0, 0, 0})).status ==
         vehicle_telemetry::ProcessStatus::Ignored);

  EXPECT(harness.evaluations.by_message[0] == 0);
  EXPECT(harness.evaluations.by_message[1] == 0);
  EXPECT(harness.evaluations.by_message[2] == 0);
  EXPECT(harness.evaluations.by_message[3] == 2);
  EXPECT(harness.evaluations.by_message[4] == 6);
  EXPECT(harness.evaluations.by_message[5] == 5);
  EXPECT(harness.evaluations.by_message[6] == 3);
  EXPECT(harness.evaluations.by_message[7] == 1);
  harness.stop();
}

void test_due_groups_and_field_specific_deadlines() {
  mazda::TelemetryConfig config{};
  config.freshness.turn_state_timeout_us = 10;
  config.freshness.hazard_request_timeout_us = 10;
  config.freshness.left_turn_request_timeout_us = 10;
  config.freshness.right_turn_request_timeout_us = 10;
  config.freshness.front_wiper_timeout_us = 10;
  config.freshness.liftgate_open_timeout_us = 5;
  config.freshness.rear_right_door_open_timeout_us = 10;
  config.freshness.front_left_door_open_rhd_timeout_us = 0;
  config.freshness.selector_position_timeout_us = 20;
  config.freshness.actual_gear_timeout_us = 30;
  config.freshness.left_indicator_lamp_timeout_us = 7;
  config.freshness.right_indicator_lamp_timeout_us = 8;
  config.freshness.wiper_low_timeout_us = 9;
  config.freshness.brake_pressed_timeout_us = 11;
  Harness harness{config};
  EXPECT(harness.start());

  EXPECT(harness.process(frame(kTurnSwitchId, 100, {0, 0x20, 0x10, 0, 0, 0, 0, 0})).status ==
         vehicle_telemetry::ProcessStatus::Processed);
  EXPECT(harness.process(frame(kDoorsId, 100, {0, 0, 0, 0x40, 0x3d, 0, 0, 0})).status ==
         vehicle_telemetry::ProcessStatus::Processed);
  EXPECT(harness.process(frame(kGearId, 100, {0x04, 0, 0, 0, 0x1c, 0, 0, 0})).status ==
         vehicle_telemetry::ProcessStatus::Processed);
  EXPECT(harness.process(frame(kBlinkInfoId, 100, {0, 0, 0x08, 0, 0x02, 0, 0, 0})).status ==
         vehicle_telemetry::ProcessStatus::Processed);
  EXPECT(harness.process(frame(kBrakePedalId, 100, {0x10, 0, 0, 0, 0, 0, 0, 0})).status ==
         vehicle_telemetry::ProcessStatus::Processed);
  harness.evaluations.clear();

  // The deadline is inclusive. A zero-timeout field becomes due only after
  // its timestamp, so the doors group is serviced at 101.
  harness.publish(100);
  EXPECT(harness.evaluations.total() == 0);
  harness.publish(101);
  EXPECT(harness.evaluations.by_message[4] == 6);
  harness.evaluations.clear();

  // The five-microsecond liftgate timeout is still fresh at 105 and becomes
  // due at 106.
  harness.publish(105);
  EXPECT(harness.evaluations.total() == 0);
  harness.publish(106);
  EXPECT(harness.evaluations.by_message[4] == 6);
  EXPECT(harness.evaluations.by_message[5] == 0);

  // The lamp fields have independent deadlines within one message group.
  harness.evaluations.clear();
  harness.publish(107);
  EXPECT(harness.evaluations.total() == 0);
  harness.publish(108);
  EXPECT(harness.evaluations.by_message[6] == 3);
  harness.evaluations.clear();
  harness.publish(109);
  EXPECT(harness.evaluations.by_message[6] == 3);
  harness.evaluations.clear();
  harness.publish(110);
  EXPECT(harness.evaluations.by_message[6] == 3);

  // A field that has already been serviced does not cause a repeated sweep;
  // the ten-microsecond rear-right and turn deadlines are also inclusive.
  harness.evaluations.clear();
  harness.publish(110);
  EXPECT(harness.evaluations.total() == 0);
  harness.publish(111);
  EXPECT(harness.evaluations.by_message[4] == 6);
  EXPECT(harness.evaluations.by_message[5] == 5);
  harness.evaluations.clear();
  harness.publish(112);
  EXPECT(harness.evaluations.by_message[7] == 1);

  // Selector and actual-gear timeouts are independently honored even though
  // both readings share the same two-descriptor gear group.
  harness.evaluations.clear();
  harness.publish(120);
  EXPECT(harness.evaluations.total() == 0);
  harness.publish(121);
  EXPECT(harness.evaluations.by_message[3] == 2);
  harness.evaluations.clear();
  harness.publish(130);
  EXPECT(harness.evaluations.total() == 0);
  harness.publish(131);
  EXPECT(harness.evaluations.by_message[3] == 2);

  const auto descriptors = mazda::internal::VehicleTelemetryService::notification_descriptors();
  const auto rear_left = harness.service.read_descriptor(std::get<8>(descriptors));
  EXPECT(rear_left.value.has_value());
  EXPECT(rear_left.availability == mazda::Availability::FreshnessUnverified);
  harness.stop();
}

void test_unrelated_traffic_and_newer_same_value_extend_deadline() {
  mazda::TelemetryConfig config{};
  config.freshness.turn_state_timeout_us = 10;
  config.freshness.hazard_request_timeout_us = 10;
  config.freshness.left_turn_request_timeout_us = 10;
  config.freshness.right_turn_request_timeout_us = 10;
  config.freshness.front_wiper_timeout_us = 10;
  Harness harness{config};
  TurnRecorder recorder{};
  EXPECT(harness.service.subscribe_turn(&record_turn, &recorder).ok());
  EXPECT(harness.start());

  EXPECT(harness.process(frame(kTurnSwitchId, 100, {0, 0x20, 0, 0, 0, 0, 0, 0})).status ==
         vehicle_telemetry::ProcessStatus::Processed);
  EXPECT(harness.drain() > 0);
  harness.evaluations.clear();

  // A newer equal-value observation moves the freshness origin. At the
  // inclusive deadline the value remains fresh; one unit later it is due.
  EXPECT(harness.process(frame(kTurnSwitchId, 105, {0, 0x20, 0, 0, 0, 0, 0, 0})).status ==
         vehicle_telemetry::ProcessStatus::Processed);
  const auto descriptors = mazda::internal::VehicleTelemetryService::notification_descriptors();
  EXPECT(harness.service.read_descriptor(std::get<2>(descriptors)).availability ==
         mazda::Availability::Fresh);
  harness.publish(115);
  EXPECT(harness.service.read_descriptor(std::get<2>(descriptors)).availability ==
         mazda::Availability::Fresh);

  // A continuously unrelated frame still services due work once and does not
  // make an unrelated group evaluate.
  harness.evaluations.clear();
  EXPECT(harness.process(frame(0x7ff, 116, {0, 0, 0, 0, 0, 0, 0, 0})).status ==
         vehicle_telemetry::ProcessStatus::Ignored);
  EXPECT(harness.evaluations.by_message[5] == 5);
  harness.evaluations.clear();
  EXPECT(harness.process(frame(0x7ff, 117, {0, 0, 0, 0, 0, 0, 0, 0})).status ==
         vehicle_telemetry::ProcessStatus::Ignored);
  EXPECT(harness.evaluations.total() == 0);

  // The stale transition is queued, then a newer same-value frame recovers
  // before dispatch. NotificationChannel must retain both edges and mark the
  // merged notice coalesced.
  EXPECT(harness.process(frame(kTurnSwitchId, 118, {0, 0x20, 0, 0, 0, 0, 0, 0})).status ==
         vehicle_telemetry::ProcessStatus::Processed);
  EXPECT(harness.drain() > 0);
  EXPECT(recorder.count >= 2);
  const auto &notice = recorder.notices[recorder.count - 1];
  EXPECT(notice.current.availability == mazda::Availability::Fresh);
  EXPECT(notice.became_unavailable);
  EXPECT(notice.recovered);
  EXPECT(notice.coalesced);
  harness.stop();

  // No unrelated or diagnostic publication is required between the deadline
  // and a newer same-value observation. The pre-decode due pass must still
  // preserve both transition flags for one delayed, coalesced delivery.
  Harness direct{config};
  TurnRecorder direct_recorder{};
  EXPECT(direct.service.subscribe_turn(&record_turn, &direct_recorder).ok());
  EXPECT(direct.start());
  EXPECT(direct.process(frame(kTurnSwitchId, 100, {0, 0x20, 0, 0, 0, 0, 0, 0})).status ==
         vehicle_telemetry::ProcessStatus::Processed);
  EXPECT(direct.drain() > 0);
  direct.evaluations.clear();
  EXPECT(direct.process(frame(kTurnSwitchId, 129, {0, 0x20, 0, 0, 0, 0, 0, 0})).status ==
         vehicle_telemetry::ProcessStatus::Processed);
  EXPECT(direct.evaluations.by_message[5] == 10);
  EXPECT(direct.drain() > 0);
  EXPECT(direct_recorder.count >= 2);
  const auto &direct_notice = direct_recorder.notices[direct_recorder.count - 1];
  EXPECT(direct_notice.current.availability == mazda::Availability::Fresh);
  EXPECT(direct_notice.became_unavailable);
  EXPECT(direct_notice.recovered);
  EXPECT(direct_notice.coalesced);
  direct.stop();
}

void test_equal_older_conflict_do_not_recover() {
  mazda::TelemetryConfig config{};
  config.freshness.turn_state_timeout_us = 10;
  config.freshness.hazard_request_timeout_us = 10;
  config.freshness.left_turn_request_timeout_us = 10;
  config.freshness.right_turn_request_timeout_us = 10;
  config.freshness.front_wiper_timeout_us = 10;
  Harness harness{config};
  TurnRecorder recorder{};
  EXPECT(harness.service.subscribe_turn(&record_turn, &recorder).ok());
  EXPECT(harness.start());
  EXPECT(harness.process(frame(kTurnSwitchId, 100, {0, 0x20, 0, 0, 0, 0, 0, 0})).status ==
         vehicle_telemetry::ProcessStatus::Processed);
  EXPECT(harness.drain() > 0);
  harness.evaluations.clear();

  // Equal-time duplicate/conflicting and older observations are rejected by
  // the decoder. The due pass makes the retained value stale, but none of
  // those frames can recover it or move its observation timestamp.
  EXPECT(harness.process(frame(kTurnSwitchId, 100, {0, 0x20, 0, 0, 0, 0, 0, 0}), 111).status ==
         vehicle_telemetry::ProcessStatus::Processed);
  EXPECT(harness.service
             .read_descriptor(
                 std::get<2>(mazda::internal::VehicleTelemetryService::notification_descriptors()))
             .availability == mazda::Availability::Stale);
  EXPECT(harness.process(frame(kTurnSwitchId, 100, {0, 0x10, 0, 0, 0, 0, 0, 0}), 112).status ==
         vehicle_telemetry::ProcessStatus::Processed);
  EXPECT(harness.service
             .read_descriptor(
                 std::get<2>(mazda::internal::VehicleTelemetryService::notification_descriptors()))
             .availability == mazda::Availability::Stale);
  EXPECT(harness.process(frame(kTurnSwitchId, 99, {0, 0x20, 0, 0, 0, 0, 0, 0}), 113).status ==
         vehicle_telemetry::ProcessStatus::Processed);
  EXPECT(harness.service
             .read_descriptor(
                 std::get<2>(mazda::internal::VehicleTelemetryService::notification_descriptors()))
             .availability == mazda::Availability::Stale);

  // Only a newer observation recovers the signal. Delay dispatch so the
  // stale and recovered evidence must survive one coalesced handoff.
  EXPECT(harness.process(frame(kTurnSwitchId, 110, {0, 0x20, 0, 0, 0, 0, 0, 0}), 114).status ==
         vehicle_telemetry::ProcessStatus::Processed);
  EXPECT(harness.drain() > 0);
  EXPECT(recorder.count >= 2);
  const auto &notice = recorder.notices[recorder.count - 1];
  EXPECT(notice.current.availability == mazda::Availability::Fresh);
  EXPECT(notice.became_unavailable);
  EXPECT(notice.recovered);
  EXPECT(notice.coalesced);
  harness.stop();
}

void test_brake_default_unverified_and_malformed_failoff() {
  Harness harness{};
  BrakeRecorder recorder{};
  EXPECT(harness.service.subscribe_brake(&record_brake, &recorder).ok());
  EXPECT(harness.start());

  EXPECT(harness.process(frame(kBrakePedalId, 100, {0x10, 0, 0, 0, 0, 0, 0, 0})).status ==
         vehicle_telemetry::ProcessStatus::Processed);
  const auto descriptors = mazda::internal::VehicleTelemetryService::notification_descriptors();
  const auto brake = harness.service.read_descriptor(std::get<16>(descriptors));
  EXPECT(brake.value.has_value() && *brake.value);
  EXPECT(brake.availability == mazda::Availability::FreshnessUnverified);
  harness.publish(1'000'000);
  EXPECT(harness.service.read_descriptor(std::get<16>(descriptors)).availability ==
         mazda::Availability::FreshnessUnverified);

  // An owned malformed frame fails off at the processing boundary. A newer
  // valid observation is required to restore the unverified brake reading.
  const auto malformed = harness.process(frame(kBrakePedalId, 2'000, {0x10, 0, 0, 0, 0, 0, 0}));
  EXPECT(malformed.status == vehicle_telemetry::ProcessStatus::Malformed);
  EXPECT(harness.service.read_descriptor(std::get<16>(descriptors)).availability ==
         mazda::Availability::Unavailable);
  EXPECT(harness.process(frame(kBrakePedalId, 2'001, {0x10, 0, 0, 0, 0, 0, 0, 0})).status ==
         vehicle_telemetry::ProcessStatus::Processed);
  EXPECT(harness.service.read_descriptor(std::get<16>(descriptors)).availability ==
         mazda::Availability::FreshnessUnverified);
  EXPECT(harness.drain() > 0);
  EXPECT(recorder.count >= 2);
  EXPECT(recorder.notices[recorder.count - 1].recovered);
  harness.stop();
}

void test_global_fault_and_recovery_evaluate_all_groups() {
  Harness harness{};
  EXPECT(harness.start());
  harness.evaluations.clear();
  harness.publish(100, vehicle_core::TransportHealth::Faulted,
                  vehicle_telemetry::LifecycleState::Faulted);

  std::size_t released_descriptors = 0;
  std::apply(
      [&released_descriptors](const auto &...descriptor) {
        ((released_descriptors += descriptor.id.valid() ? 1U : 0U), ...);
      },
      mazda::internal::VehicleTelemetryService::notification_descriptors());
  EXPECT(harness.evaluations.total() == released_descriptors);

  harness.evaluations.clear();
  EXPECT(harness.process(frame(kTurnSwitchId, 101, {0, 0x20, 0, 0, 0, 0, 0, 0})).status ==
         vehicle_telemetry::ProcessStatus::Processed);
  EXPECT(harness.evaluations.total() == released_descriptors);
  harness.stop();
}

} // namespace

int main() {
  test_exact_ordinary_group_counts();
  test_due_groups_and_field_specific_deadlines();
  test_unrelated_traffic_and_newer_same_value_extend_deadline();
  test_equal_older_conflict_do_not_recover();
  test_brake_default_unverified_and_malformed_failoff();
  test_global_fault_and_recovery_evaluate_all_groups();
  if (failures != 0)
    std::cerr << failures << " vehicle telemetry group assertion(s) failed\n";
  return failures == 0 ? 0 : 1;
}
