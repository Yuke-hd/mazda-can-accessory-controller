#include <cassert>
#include <vector>

#include "local_argb/lighting_sink.hpp"
#include "local_argb/local_argb.h"
#include "local_argb/renderer.hpp"
#include "vehicle_lighting_policy/command.hpp"

namespace {

class FakePixelSink final : public local_argb::PixelFrameSink {
public:
  bool write(const local_argb::PixelFrame &frame) noexcept override {
    writes.push_back(frame);
    if (failures_remaining != 0) {
      --failures_remaining;
      return false;
    }
    return true;
  }

  std::vector<local_argb::PixelFrame> writes;
  unsigned failures_remaining{0};
};

class FakeLightingSink final : public local_argb::internal::LightingSink {
public:
  bool publish(const local_argb::internal::LightingCommand &command) noexcept override {
    commands.push_back(command);
    return true;
  }

  std::vector<local_argb::internal::LightingCommand> commands;
};

local_argb::internal::LightingCommand green(const vehicle_core::MonotonicTimestamp deadline) {
  local_argb::internal::LightingCommand command{};
  command.color = {0, local_argb::kBrightnessCeiling, 0};
  command.valid_until_us = deadline;
  command.actionable = true;
  return command;
}

void test_renderer_enforces_brightness_ceiling() {
  FakePixelSink sink;
  local_argb::internal::RendererController renderer{sink};
  assert(renderer.start());

  const local_argb::internal::LightingCommand over_limit{{255, 127, 17}, false, false,
                                                         false,          100,   true};
  assert(renderer.apply(over_limit, 50));
  const local_argb::Rgb expected{local_argb::kBrightnessCeiling, local_argb::kBrightnessCeiling,
                                 local_argb::kBrightnessCeiling};
  assert(sink.writes.back().front() == expected);
}

void test_startup_and_independent_expiry() {
  FakePixelSink sink;
  local_argb::internal::RendererController renderer{sink};
  assert(renderer.start());
  assert(sink.writes.size() == 1);
  assert(sink.writes.front() == local_argb::kBlackFrame);

  assert(renderer.apply(green(250), 100));
  const local_argb::Rgb green_rgb{0, local_argb::kBrightnessCeiling, 0};
  assert(sink.writes.back().front() == green_rgb);
  assert(renderer.tick(250));
  assert(sink.writes.back() != local_argb::kBlackFrame);
  assert(renderer.tick(251));
  assert(sink.writes.back() == local_argb::kBlackFrame);
}

void test_failed_colour_attempt_retries_black_then_recovers() {
  FakePixelSink sink;
  local_argb::internal::RendererController renderer{sink};
  assert(renderer.start());
  sink.failures_remaining = 1;
  assert(!renderer.apply(green(250), 100));
  assert(renderer.faulted());
  assert(sink.writes.size() == 3); // colour failure, then immediate black retry
  assert(sink.writes.back() == local_argb::kBlackFrame);
  assert(renderer.tick(101)); // redundant black is suppressed after success
  assert(renderer.apply(green(300), 200));
  assert(!renderer.faulted());
}

void test_bounded_overwrite_and_generic_sink() {
  local_argb::internal::Mailbox mailbox;
  mailbox.submit(green(10));
  mailbox.submit(green(20));
  local_argb::internal::LightingCommand command{};
  assert(mailbox.take(command));
  assert(command.valid_until_us == 20);
  assert(!mailbox.take(command));

  FakeLightingSink sink;
  assert(sink.publish(green(42)));
  assert(sink.commands.size() == 1);
  assert(sink.commands.front().actionable);

  const vehicle_lighting_policy::LightingCommand policy_command{{1, 2, 3}, 99, true};
  const auto adapted = local_argb::internal::adapt_command(policy_command);
  assert(adapted.color.red == 1);
  assert(adapted.color.green == 2);
  assert(adapted.color.blue == 3);
  assert(adapted.valid_until_us == 99);
  assert(adapted.actionable);
}

void test_turn_flow_and_brake_regions() {
  FakePixelSink sink;
  local_argb::internal::RendererController renderer{sink};
  assert(renderer.start());

  local_argb::internal::LightingCommand left{};
  left.left_turn = true;
  left.valid_until_us = 1'000'000;
  left.actionable = true;
  assert(renderer.apply(left, 0));
  const auto first_left = sink.writes.back();
  assert(first_left[local_argb::kTurnLedCount - 1].red > 0);
  assert(first_left[local_argb::kBrakeLedStart] == local_argb::kBlack);
  assert(first_left[local_argb::kRightTurnLedStart] == local_argb::kBlack);
  assert(renderer.tick(50'000));
  assert(sink.writes.back() != first_left);

  local_argb::internal::LightingCommand brake{};
  brake.brake = true;
  brake.valid_until_us = 1'000;
  brake.actionable = true;
  assert(renderer.apply(brake, 100));
  const auto &brake_frame = sink.writes.back();
  for (std::size_t index = 0; index < local_argb::kLedCount; ++index) {
    const bool in_brake = index >= local_argb::kBrakeLedStart &&
                          index < local_argb::kBrakeLedStart + local_argb::kBrakeLedCount;
    assert((brake_frame[index] == local_argb::Rgb{local_argb::kBrightnessCeiling, 0, 0}) ==
           in_brake);
  }

  local_argb::internal::LightingCommand off{};
  assert(renderer.apply(off, 200));
  assert(sink.writes.back() == local_argb::kBlackFrame);
}

void test_turn_flow_starts_at_inner_edges_and_moves_outward() {
  const local_argb::Rgb amber{128, 16, 0};
  const local_argb::Rgb amber_tail{102, 12, 0};

  FakePixelSink left_sink;
  local_argb::internal::RendererController left_renderer{left_sink};
  assert(left_renderer.start());

  local_argb::internal::LightingCommand left{};
  left.left_turn = true;
  left.valid_until_us = 2'000'000;
  left.actionable = true;
  assert(left_renderer.apply(left, 0));
  const auto left_start = left_sink.writes.back();
  assert(left_start[34] == amber);
  assert(left_start[33] == local_argb::kBlack);
  assert(left_start[local_argb::kRightTurnLedStart] == local_argb::kBlack);

  assert(left_renderer.tick(50'000));
  const auto left_step = left_sink.writes.back();
  assert(left_step[33] == amber);
  assert(left_step[34] == amber_tail);
  assert(left_step[32] == local_argb::kBlack);

  assert(left_renderer.tick(1'133'322)); // 34 * 33,333 us: the outer-edge phase
  const auto left_outer = left_sink.writes.back();
  assert(left_outer[0] == amber);
  assert(left_outer[1] == amber_tail);
  assert(left_outer[34] == local_argb::kBlack);

  FakePixelSink right_sink;
  local_argb::internal::RendererController right_renderer{right_sink};
  assert(right_renderer.start());

  local_argb::internal::LightingCommand right{};
  right.right_turn = true;
  right.valid_until_us = 2'000'000;
  right.actionable = true;
  assert(right_renderer.apply(right, 0));
  const auto right_start = right_sink.writes.back();
  assert(right_start[local_argb::kRightTurnLedStart] == amber);
  assert(right_start[local_argb::kRightTurnLedStart + 1] == local_argb::kBlack);
  assert(right_start[34] == local_argb::kBlack);

  assert(right_renderer.tick(50'000));
  const auto right_step = right_sink.writes.back();
  assert(right_step[local_argb::kRightTurnLedStart + 1] == amber);
  assert(right_step[local_argb::kRightTurnLedStart] == amber_tail);
  assert(right_step[local_argb::kRightTurnLedStart + 2] == local_argb::kBlack);

  assert(right_renderer.tick(1'133'322)); // 34 * 33,333 us: the outer-edge phase
  const auto right_outer = right_sink.writes.back();
  assert(right_outer[local_argb::kLedCount - 1] == amber);
  assert(right_outer[local_argb::kLedCount - 2] == amber_tail);
  assert(right_outer[local_argb::kRightTurnLedStart] == local_argb::kBlack);
}

void test_onboard_status_collapses_logical_frame() {
  local_argb::PixelFrame turn_frame{};
  turn_frame[0] = {128, 16, 0};
  assert(local_argb::internal::onboard_status_color(turn_frame) ==
         local_argb::internal::kOnboardTurnStatus);

  local_argb::PixelFrame right_turn_frame{};
  right_turn_frame[local_argb::kRightTurnLedStart] = {128, 16, 0};
  assert(local_argb::internal::onboard_status_color(right_turn_frame) ==
         local_argb::internal::kOnboardTurnStatus);

  local_argb::PixelFrame brake_frame{};
  brake_frame[local_argb::kBrakeLedStart] = {local_argb::kBrightnessCeiling, 0, 0};
  assert(local_argb::internal::onboard_status_color(brake_frame) ==
         local_argb::internal::kOnboardBrakeStatus);

  brake_frame[local_argb::kRightTurnLedStart] = {128, 16, 0};
  assert(local_argb::internal::onboard_status_color(brake_frame) ==
         local_argb::internal::kOnboardBrakeStatus);
  assert(local_argb::internal::onboard_status_color(local_argb::kBlackFrame) == local_argb::kBlack);
}

} // namespace

int main() {
  test_startup_and_independent_expiry();
  test_failed_colour_attempt_retries_black_then_recovers();
  test_bounded_overwrite_and_generic_sink();
  test_renderer_enforces_brightness_ceiling();
  test_turn_flow_and_brake_regions();
  test_turn_flow_starts_at_inner_edges_and_moves_outward();
  test_onboard_status_collapses_logical_frame();
  return 0;
}
