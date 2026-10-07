#include "mazda/debug_telemetry_internal.hpp"
#include "mazda/vehicle_telemetry.hpp"
#include "mazda/vehicle_telemetry_internal.hpp"

#include "mazda/definitions.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>

namespace {

int failures = 0;

void expect(const bool condition, const char *expression, const int line) {
  if (!condition) {
    std::cerr << __FILE__ << ':' << line << ": failed: " << expression << '\n';
    ++failures;
  }
}

#define EXPECT(condition) expect((condition), #condition, __LINE__)

vehicle_core::RawCanFrame turn_frame(const std::uint64_t timestamp_us, const std::uint8_t counter) {
  vehicle_core::RawCanFrame frame{};
  frame.identifier = mazda::candidate::kTurnSwitchId;
  frame.identifier_format = vehicle_core::CanIdentifierFormat::Standard;
  frame.timestamp_us = timestamp_us;
  frame.dlc = 8;
  // Keep the high nibble nonzero so the test proves the issue's D4 low-nibble
  // convention rather than accepting the complete byte by accident.
  frame.data[3] = static_cast<std::uint8_t>(0xa0U | counter);
  return frame;
}

mazda::Diagnostics diagnostics(const std::uint64_t frames_processed) {
  mazda::Diagnostics result{};
  result.lifecycle = mazda::LifecycleState::Running;
  result.transport = vehicle_core::TransportHealth::Live;
  result.acquisition.frames_received = frames_processed;
  result.acquisition.frames_processed = frames_processed;
  return result;
}

void test_wrap_skip_and_latency() {
  mazda::internal::DebugRecorder recorder{};
  mazda::VehicleState state{};
  (void)state.observe_message(turn_frame(100, 0x0f), vehicle_core::DecodeValidity::Decoded);
  state.turn_state.update(mazda::TurnState::Off, 100);

  (void)recorder.record_processed(turn_frame(100, 0x0f),
                                  vehicle_telemetry::ProcessStatus::Processed, 120, false);
  // The pending frame is not visible until the normal publication boundary.
  EXPECT(!recorder.snapshot().has_frame);
  (void)recorder.record_published(state, diagnostics(1), 130, 130, 1000, 900, 130);
  auto snapshot = recorder.snapshot();
  EXPECT(snapshot.enabled);
  EXPECT(snapshot.counter == 0x0f);
  EXPECT(!snapshot.counter_sequential);
  EXPECT(snapshot.rx_process_latency_us == 20);
  EXPECT(snapshot.process_publish_latency_us == 10);
  EXPECT(snapshot.clock_delta_us == 100);

  state.turn_state.update(mazda::TurnState::Left, 200);
  (void)recorder.record_processed(turn_frame(200, 0x00),
                                  vehicle_telemetry::ProcessStatus::Processed, 205, false);
  (void)recorder.record_published(state, diagnostics(2), 210, 210, 1100, 1000, 210);
  snapshot = recorder.snapshot();
  EXPECT(snapshot.counter == 0x00);
  EXPECT(snapshot.previous_counter == 0x0f);
  EXPECT(snapshot.expected_counter == 0x00);
  EXPECT(snapshot.counter_sequential);
  EXPECT(snapshot.interarrival_us == 100);

  state.turn_state.update(mazda::TurnState::Right, 300);
  (void)recorder.record_processed(turn_frame(300, 0x02),
                                  vehicle_telemetry::ProcessStatus::Processed, 305, false);
  (void)recorder.record_published(state, diagnostics(3), 310, 310, 1200, 1100, 310);
  snapshot = recorder.snapshot();
  EXPECT(!snapshot.counter_sequential);
  EXPECT(snapshot.counter_gap_count == 1);

  // A numerically matching extended or remote frame is still an observation
  // for process-status/frame-count evidence, but it cannot contribute D4
  // counter continuity or gap evidence.
  auto extended = turn_frame(400, 3);
  extended.identifier_format = vehicle_core::CanIdentifierFormat::Extended;
  (void)recorder.record_processed(extended, vehicle_telemetry::ProcessStatus::Ignored, 405,
                                  false);
  (void)recorder.record_published(state, diagnostics(4), 410, 410, 1300, 1200, 410);
  snapshot = recorder.snapshot();
  EXPECT(!snapshot.counter_valid);
  EXPECT(snapshot.counter_gap_count == 1);

  auto remote = turn_frame(500, 4);
  remote.remote_request = true;
  (void)recorder.record_processed(remote, vehicle_telemetry::ProcessStatus::Ignored, 505, false);
  (void)recorder.record_published(state, diagnostics(5), 510, 510, 1400, 1300, 510);
  snapshot = recorder.snapshot();
  EXPECT(!snapshot.counter_valid);
  EXPECT(snapshot.counter_gap_count == 1);
}

void test_not_advanced_and_stale_snapshot() {
  mazda::internal::DebugRecorder recorder{};
  mazda::VehicleState state{};
  (void)state.observe_message(turn_frame(1'000, 1), vehicle_core::DecodeValidity::Decoded);
  state.turn_state.update(mazda::TurnState::Off, 1'000);
  (void)recorder.record_processed(turn_frame(1'000, 1),
                                  vehicle_telemetry::ProcessStatus::Processed, 1'010, false);
  (void)recorder.record_published(state, diagnostics(1), 1'020, 1'020, 1'020, 1'020, 1'020);

  // An older timestamp does not advance the signal watermark. The pending
  // raw frame is still published as evidence of the observed ordering.
  (void)recorder.record_processed(turn_frame(900, 2),
                                  vehicle_telemetry::ProcessStatus::Processed, 1'030, true);
  (void)recorder.record_published(state, diagnostics(2), 1'040, 1'040, 1'040, 1'040, 1'040);
  auto snapshot = recorder.snapshot();
  EXPECT(snapshot.update_not_advanced);
  EXPECT(snapshot.update_not_advanced_count == 1);
  EXPECT(snapshot.signal_last_update_us == 1'000);

  // The stale transition is evaluated at publication time, while its
  // frame/publication timestamps remain those of the latest 0x091 frame.
  (void)recorder.record_published(state, diagnostics(2), 3'000'001, 3'000'001, 3'000'001,
                                  3'000'001, 3'000'001);
  const auto stale = recorder.stale_snapshot();
  EXPECT(stale.stale_transition_count == 1);
  EXPECT(stale.availability == mazda::Availability::Stale);
  EXPECT(stale.frame_timestamp_us == 900);
  EXPECT(stale.publication_timestamp_us == 1'040);
  EXPECT(stale.signal_age_us == 2'999'001);
}

void test_reader_contention_preserves_stale_before_recovery() {
  mazda::internal::DebugRecorder recorder{};
  mazda::VehicleState state{};
  (void)state.observe_message(turn_frame(1'000, 1), vehicle_core::DecodeValidity::Decoded);
  state.turn_state.update(mazda::TurnState::Off, 1'000);
  (void)recorder.record_processed(turn_frame(1'000, 1),
                                  vehicle_telemetry::ProcessStatus::Processed, 1'010, false);
  EXPECT(recorder.record_published(state, diagnostics(1), 1'020, 1'020, 1'020, 1'020, 1'020));

  EXPECT(recorder.hold_snapshot_lock_for_test());
  const auto stale_publish =
      recorder.record_published(state, diagnostics(2), 3'000'001, 3'000'001, 3'000'001,
                                3'000'001, 3'000'001);
  EXPECT(!stale_publish);
  recorder.release_snapshot_lock_for_test();

  // The worker can immediately process a newer valid observation. The stale
  // worker record must still be handed off together with the recovery.
  state.turn_state.update(mazda::TurnState::Left, 3'000'002);
  (void)recorder.record_processed(turn_frame(3'000'002, 2),
                                  vehicle_telemetry::ProcessStatus::Processed, 3'000'003, false);
  EXPECT(recorder.record_published(state, diagnostics(3), 3'000'004, 3'000'004, 3'000'004,
                                   3'000'004, 3'000'004));
  const auto stale = recorder.stale_snapshot();
  const auto current = recorder.snapshot();
  EXPECT(stale.stale_transition_count == 1);
  EXPECT(stale.availability == mazda::Availability::Stale);
  EXPECT(current.availability == mazda::Availability::Fresh);
  EXPECT(current.recovery_count == 1);
  EXPECT(current.recorder_write_contention >= 1);
}

class FakeClock final : public vehicle_core::MonotonicClock {
public:
  [[nodiscard]] vehicle_core::MonotonicTimestamp now() const noexcept override {
    return now_us_.load(std::memory_order_relaxed);
  }

  void set(const vehicle_core::MonotonicTimestamp value) noexcept {
    now_us_.store(value, std::memory_order_relaxed);
  }

private:
  std::atomic<vehicle_core::MonotonicTimestamp> now_us_{0};
};

template <typename Predicate> bool wait_for(Predicate predicate) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{750};
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  return predicate();
}

void test_production_service_path() {
  FakeClock clock{};
  clock.set(100);
  mazda::internal::HostAcquisitionSource source{};
  mazda::internal::NullLightingSink lighting{};
  mazda::VehicleTelemetry telemetry{};
  mazda::TelemetryConfig config{};
  config.transport_silence_timeout_us = 5'000'000;
  mazda::internal::VehicleTelemetryAccess::emplace_host_service(telemetry, clock, source, lighting,
                                                                config);
  EXPECT(telemetry.start().ok());

  EXPECT(source.inject(turn_frame(100, 1)) == mazda::ResultCode::Ok);
  mazda::DebugSnapshot snapshot{};
  EXPECT(wait_for([&] {
    const auto current = telemetry.debug_snapshot();
    if (current.frame_count != 1)
      return false;
    snapshot = current;
    return true;
  }));
  EXPECT(snapshot.enabled);
  EXPECT(snapshot.has_frame);
  EXPECT(snapshot.counter == 1);
  EXPECT(snapshot.availability == mazda::Availability::Fresh);
  EXPECT(snapshot.diagnostics.acquisition.frames_processed >= 1);
  EXPECT(snapshot.diagnostics_sample_timestamp_us == 100);
  EXPECT(snapshot.diagnostics_sample_source == mazda::DebugSampleSource::TelemetryObserverFrame);

  // The decoder treats an identical equal-time frame as idempotent. The
  // process status remains Processed, while the signal watermark does not
  // advance; this metric is therefore not a decoder-rejection claim.
  EXPECT(source.inject(turn_frame(100, 1)) == mazda::ResultCode::Ok);
  EXPECT(wait_for([&] {
    const auto current = telemetry.debug_snapshot();
    if (current.frame_count != 2 || current.update_not_advanced_count != 1)
      return false;
    snapshot = current;
    return true;
  }));
  EXPECT(snapshot.update_not_advanced);
  EXPECT(snapshot.process_status ==
         static_cast<std::uint8_t>(vehicle_telemetry::ProcessStatus::Processed));

  clock.set(2'001'000);
  mazda::DebugSnapshot stale{};
  EXPECT(wait_for([&] {
    const auto current = telemetry.debug_snapshot();
    if (current.availability != mazda::Availability::Stale)
      return false;
    const auto transition = telemetry.debug_stale_snapshot();
    if (transition.stale_transition_count != 1)
      return false;
    stale = transition;
    return true;
  }));
  EXPECT(stale.stale_transition_count == 1);
  EXPECT(stale.frame_timestamp_us == 100);
  EXPECT(stale.publication_timestamp_us >= 100);
  EXPECT(stale.diagnostics_sample_timestamp_us == 2'001'000);
  EXPECT(stale.diagnostics_sample_source == mazda::DebugSampleSource::TelemetryObserverTimeout);
  EXPECT(stale.now_us == 2'001'000);

  clock.set(2'002'000);
  EXPECT(source.inject(turn_frame(2'002'000, 2)) == mazda::ResultCode::Ok);
  EXPECT(wait_for([&] {
    const auto current = telemetry.debug_snapshot();
    if (current.availability != mazda::Availability::Fresh || current.frame_count != 3)
      return false;
    snapshot = current;
    return true;
  }));
  EXPECT(snapshot.recovery_count == 1);
  EXPECT(snapshot.stale_transition_count == 1);
  EXPECT(snapshot.diagnostics_sample_timestamp_us == 2'002'000);
  EXPECT(snapshot.diagnostics_sample_source == mazda::DebugSampleSource::TelemetryObserverFrame);
  const auto preserved_stale = telemetry.debug_stale_snapshot();
  EXPECT(preserved_stale.stale_transition_count == 1);
  EXPECT(preserved_stale.diagnostics_sample_timestamp_us == 2'001'000);
  EXPECT(preserved_stale.diagnostics_sample_source ==
         mazda::DebugSampleSource::TelemetryObserverTimeout);

  // Unrelated traffic must not create a per-frame recorder cost, but the
  // bounded aggregate cadence still refreshes transport counters during a
  // long interval without another 0x091 frame.
  const auto publications_before_unrelated = snapshot.recorder_publications;
  const auto receives_before_unrelated = source.statistics().frames_received;
  auto unrelated = turn_frame(2'003'000, 3);
  unrelated.identifier = mazda::candidate::kEngineDataId;
  EXPECT(source.inject(unrelated) == mazda::ResultCode::Ok);
  EXPECT(wait_for([&] {
    const auto current = telemetry.debug_snapshot();
    return current.frame_count == 3 &&
           source.statistics().frames_received > receives_before_unrelated &&
           current.recorder_publications == publications_before_unrelated;
  }));
  clock.set(7'003'000);
  unrelated.timestamp_us = 7'003'000;
  EXPECT(source.inject(unrelated) == mazda::ResultCode::Ok);
  EXPECT(wait_for([&] {
    const auto current = telemetry.debug_snapshot();
    if (current.recorder_publications <= publications_before_unrelated)
      return false;
    snapshot = current;
    return true;
  }));
  EXPECT(snapshot.frame_count == 3);
  EXPECT(snapshot.diagnostics_sample_source == mazda::DebugSampleSource::TelemetryObserverFrame);

  // A malformed frame also leaves the signal watermark unchanged, but its
  // process status distinguishes that decoder outcome from the idempotent
  // equal-time frame above. Keep it after stale recovery because message
  // health correctly reports this newer malformed observation as unavailable.
  clock.set(2'003'000);
  auto malformed = turn_frame(2'003'000, 3);
  malformed.dlc = 7;
  EXPECT(source.inject(malformed) == mazda::ResultCode::Ok);
  EXPECT(wait_for([&] {
    const auto current = telemetry.debug_snapshot();
    if (current.frame_count != 4 || current.update_not_advanced_count != 2)
      return false;
    snapshot = current;
    return true;
  }));
  EXPECT(snapshot.update_not_advanced);
  EXPECT(snapshot.process_status ==
         static_cast<std::uint8_t>(vehicle_telemetry::ProcessStatus::Malformed));

  // A global source fault must remain visible in the recorder even when it
  // is unrelated to a new 0x091 frame.
  const auto publications_before_fault = snapshot.recorder_publications;
  source.fail();
  EXPECT(wait_for([&] {
    const auto current = telemetry.debug_snapshot();
    if (current.diagnostics.transport != vehicle_core::TransportHealth::Faulted ||
        current.recorder_publications <= publications_before_fault)
      return false;
    snapshot = current;
    return true;
  }));
  EXPECT(snapshot.diagnostics.transport == vehicle_core::TransportHealth::Faulted);
  EXPECT(telemetry.stop().status == mazda::ResultCode::Faulted);
}

} // namespace

int main() {
  test_wrap_skip_and_latency();
  test_not_advanced_and_stale_snapshot();
  test_reader_contention_preserves_stale_before_recovery();
  test_production_service_path();
  return failures == 0 ? 0 : 1;
}
