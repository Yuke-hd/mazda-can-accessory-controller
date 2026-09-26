#include <cassert>
#include <cstdint>
#include <limits>

#include "local_argb/lighting_sink.hpp"
#include "local_argb/local_argb.h"
#include "local_argb/progress_fail_off.hpp"
#include "local_argb/progress_watchdog.hpp"
#include "local_argb/stall_gated_sink.hpp"

namespace {

using local_argb::kProgressStallFailOffUs;
using local_argb::internal::LightingCommand;
using local_argb::internal::LightingSink;
using local_argb::internal::ProgressFailOff;
using local_argb::internal::ProgressTransition;
using local_argb::internal::ProgressWatchdog;
using local_argb::internal::StallGatedSink;
using vehicle_core::MonotonicTimestamp;

constexpr MonotonicTimestamp kArmedUs = 1'000;

ProgressWatchdog armed_at(const std::uint32_t progress) {
  ProgressWatchdog watchdog{};
  watchdog.arm(progress, kArmedUs);
  return watchdog;
}

void test_the_stall_bound_is_about_two_seconds() {
  static_assert(kProgressStallFailOffUs == 2'000'000);
}

void test_an_unarmed_watchdog_never_reports_a_stall() {
  ProgressWatchdog watchdog{};
  assert(watchdog.sample(0, kArmedUs + 10 * kProgressStallFailOffUs) == ProgressTransition::None);
  assert(!watchdog.stalled());
}

void test_an_unchanged_count_within_the_bound_is_healthy() {
  auto watchdog = armed_at(7);
  assert(watchdog.sample(7, kArmedUs + kProgressStallFailOffUs) == ProgressTransition::None);
  assert(!watchdog.stalled());
}

void test_an_unchanged_count_past_the_bound_is_a_stall_reported_once() {
  auto watchdog = armed_at(7);
  assert(watchdog.sample(7, kArmedUs + kProgressStallFailOffUs + 1) == ProgressTransition::Stalled);
  assert(watchdog.stalled());
  assert(watchdog.sample(7, kArmedUs + 5 * kProgressStallFailOffUs) == ProgressTransition::None);
  assert(watchdog.stalled());
}

void test_an_advancing_count_is_healthy_however_long_it_runs() {
  auto watchdog = armed_at(0);
  for (std::uint32_t pass = 1; pass <= 100; ++pass) {
    const auto now = kArmedUs + pass * (kProgressStallFailOffUs / 2);
    assert(watchdog.sample(pass, now) == ProgressTransition::None);
  }
  assert(!watchdog.stalled());
}

void test_the_bound_restarts_from_the_last_count_change() {
  auto watchdog = armed_at(0);
  const auto changed_us = kArmedUs + kProgressStallFailOffUs;
  assert(watchdog.sample(1, changed_us) == ProgressTransition::None);
  assert(watchdog.sample(1, changed_us + kProgressStallFailOffUs) == ProgressTransition::None);
  assert(watchdog.sample(1, changed_us + kProgressStallFailOffUs + 1) ==
         ProgressTransition::Stalled);
}

void test_a_wrapped_count_is_progress() {
  constexpr auto kLast = std::numeric_limits<std::uint32_t>::max();
  auto watchdog = armed_at(kLast);
  assert(watchdog.sample(0, kArmedUs + kProgressStallFailOffUs) == ProgressTransition::None);
  assert(watchdog.sample(0, kArmedUs + kProgressStallFailOffUs + 10) == ProgressTransition::None);
}

void test_recovery_is_reported_once_and_does_not_latch() {
  auto watchdog = armed_at(3);
  const auto stalled_us = kArmedUs + kProgressStallFailOffUs + 1;
  assert(watchdog.sample(3, stalled_us) == ProgressTransition::Stalled);
  assert(watchdog.sample(4, stalled_us + 10) == ProgressTransition::Resumed);
  assert(!watchdog.stalled());
  assert(watchdog.sample(5, stalled_us + 20) == ProgressTransition::None);
  // A later stall is detected again from the resumed count.
  assert(watchdog.sample(5, stalled_us + 20 + kProgressStallFailOffUs + 1) ==
         ProgressTransition::Stalled);
}

void test_a_clock_that_runs_backwards_is_treated_as_a_stall() {
  auto watchdog = armed_at(3);
  assert(watchdog.sample(3, kArmedUs - 1) == ProgressTransition::Stalled);
}

void test_rearming_clears_a_stall() {
  auto watchdog = armed_at(3);
  assert(watchdog.sample(3, kArmedUs + kProgressStallFailOffUs + 1) == ProgressTransition::Stalled);
  watchdog.arm(3, kArmedUs + kProgressStallFailOffUs + 2);
  assert(!watchdog.stalled());
}

class RecordingLightingSink final : public LightingSink {
public:
  bool publish(const LightingCommand &command) noexcept override {
    last = command;
    ++published;
    if (on_publish != nullptr)
      on_publish(*this);
    return accept;
  }

  LightingCommand last{};
  unsigned published{0};
  bool accept{true};
  void (*on_publish)(RecordingLightingSink &){nullptr};
  StallGatedSink *gate{nullptr};
};

LightingCommand lit() {
  LightingCommand command{};
  command.actionable = true;
  command.left_turn = true;
  command.valid_until_us = std::numeric_limits<MonotonicTimestamp>::max();
  return command;
}

void test_an_open_gate_forwards_commands_and_their_result() {
  RecordingLightingSink downstream{};
  StallGatedSink gate{downstream};
  assert(gate.publish(lit()));
  assert(downstream.published == 1);
  assert(downstream.last.actionable);

  downstream.accept = false;
  assert(!gate.publish(lit()));
}

void test_a_closed_gate_rejects_commands_without_forwarding() {
  RecordingLightingSink downstream{};
  StallGatedSink gate{downstream};
  gate.close();
  assert(!gate.publish(lit()));
  assert(downstream.published == 0);
}

void test_a_reopened_gate_forwards_the_next_command() {
  RecordingLightingSink downstream{};
  StallGatedSink gate{downstream};
  gate.close();
  gate.open();
  assert(gate.publish(lit()));
  assert(downstream.last.actionable);
}

void test_a_close_during_a_publish_is_followed_by_black() {
  RecordingLightingSink downstream{};
  StallGatedSink gate{downstream};
  downstream.gate = &gate;
  // The supervisor closes the gate after the publisher passed the check but
  // before its command reached the queue; the publisher must undo it.
  downstream.on_publish = [](RecordingLightingSink &sink) {
    sink.on_publish = nullptr;
    sink.gate->close();
  };
  assert(!gate.publish(lit()));
  assert(downstream.published == 2);
  assert(!downstream.last.actionable);
  assert(!downstream.last.left_turn);
}

// Records whether the gate already rejected a lit command when black arrived.
struct UngatedRecorder final : public LightingSink {
  bool publish(const LightingCommand &command) noexcept override {
    last = command;
    ++published;
    gate_closed_first = !gate->publish(lit());
    return true;
  }

  StallGatedSink *gate{nullptr};
  LightingCommand last{};
  unsigned published{0};
  bool gate_closed_first{false};
};

struct FailOffFixture {
  FailOffFixture() { ungated.gate = &gate; }

  RecordingLightingSink downstream{};
  StallGatedSink gate{downstream};
  UngatedRecorder ungated{};
  ProgressFailOff fail_off{gate, ungated};
};

void test_a_stall_closes_the_gate_before_writing_black_past_it() {
  FailOffFixture fixture{};
  fixture.fail_off.apply(ProgressTransition::Stalled);
  assert(fixture.ungated.published == 1);
  assert(!fixture.ungated.last.actionable);
  assert(fixture.ungated.gate_closed_first);
  assert(!fixture.gate.publish(lit()));
  assert(fixture.downstream.published == 0);
}

void test_a_resume_opens_the_gate_without_re_emitting() {
  FailOffFixture fixture{};
  fixture.fail_off.apply(ProgressTransition::Stalled);
  fixture.fail_off.apply(ProgressTransition::Resumed);
  assert(fixture.ungated.published == 1);
  assert(fixture.downstream.published == 0);
  assert(fixture.gate.publish(lit()));
}

void test_no_transition_changes_nothing() {
  FailOffFixture fixture{};
  fixture.fail_off.apply(ProgressTransition::None);
  assert(fixture.ungated.published == 0);
  assert(fixture.gate.publish(lit()));
  const auto report = fixture.fail_off.take_report();
  assert(report.stalls == 0 && report.resumes == 0);
}

void test_transitions_are_counted_for_a_later_report_taken_once() {
  FailOffFixture fixture{};
  fixture.fail_off.apply(ProgressTransition::Stalled);
  fixture.fail_off.apply(ProgressTransition::Resumed);
  fixture.fail_off.apply(ProgressTransition::Stalled);
  const auto report = fixture.fail_off.take_report();
  assert(report.stalls == 2);
  assert(report.resumes == 1);
  const auto drained = fixture.fail_off.take_report();
  assert(drained.stalls == 0 && drained.resumes == 0);
}

} // namespace

int main() {
  test_the_stall_bound_is_about_two_seconds();
  test_an_unarmed_watchdog_never_reports_a_stall();
  test_an_unchanged_count_within_the_bound_is_healthy();
  test_an_unchanged_count_past_the_bound_is_a_stall_reported_once();
  test_an_advancing_count_is_healthy_however_long_it_runs();
  test_the_bound_restarts_from_the_last_count_change();
  test_a_wrapped_count_is_progress();
  test_recovery_is_reported_once_and_does_not_latch();
  test_a_clock_that_runs_backwards_is_treated_as_a_stall();
  test_rearming_clears_a_stall();
  test_an_open_gate_forwards_commands_and_their_result();
  test_a_closed_gate_rejects_commands_without_forwarding();
  test_a_reopened_gate_forwards_the_next_command();
  test_a_close_during_a_publish_is_followed_by_black();
  test_a_stall_closes_the_gate_before_writing_black_past_it();
  test_a_resume_opens_the_gate_without_re_emitting();
  test_no_transition_changes_nothing();
  test_transitions_are_counted_for_a_later_report_taken_once();
  return 0;
}
