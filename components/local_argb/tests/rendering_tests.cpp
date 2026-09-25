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

local_argb::internal::LightingCommand fill_command(const local_argb::internal::LightingRgb color,
                                                   const local_argb::internal::FillFraction level) {
  local_argb::internal::LightingCommand command{};
  command.color = {0, local_argb::kBrightnessCeiling, 0};
  const local_argb::internal::LedZone zone{10, 4, local_argb::internal::FillDirection::StartToEnd};
  assert(command.fills.add({zone, level, color}));
  command.valid_until_us = 1'000;
  command.actionable = true;
  return command;
}

void test_fill_renders_its_zone_level_under_the_brightness_ceiling() {
  FakePixelSink sink;
  local_argb::internal::RendererController renderer{sink};
  assert(renderer.start());

  assert(
      renderer.apply(fill_command({255, 8, 0}, local_argb::internal::FillFraction::of(1, 2)), 0));

  // Half of a four-pixel zone: two pixels, each channel capped at the ceiling.
  // The generic colour is not painted over a command that carries fills.
  local_argb::PixelFrame expected = local_argb::kBlackFrame;
  expected[10] = {local_argb::kBrightnessCeiling, 8, 0};
  expected[11] = {local_argb::kBrightnessCeiling, 8, 0};
  assert(sink.writes.back() == expected);
}

void test_empty_fill_list_keeps_the_generic_colour() {
  FakePixelSink sink;
  local_argb::internal::RendererController renderer{sink};
  assert(renderer.start());

  assert(renderer.apply(green(1'000), 0));

  const local_argb::Rgb green_rgb{0, local_argb::kBrightnessCeiling, 0};
  assert(sink.writes.back().front() == green_rgb);
  assert(sink.writes.back().back() == green_rgb);
}

void test_fill_list_is_bounded() {
  local_argb::internal::LightingFills fills;
  const local_argb::internal::LightingFill fill{};
  for (std::size_t index = 0; index < local_argb::internal::LightingFills::kCapacity; ++index)
    assert(fills.add(fill));
  assert(!fills.add(fill));
  assert(fills.size() == local_argb::internal::LightingFills::kCapacity);
}

namespace priority {

using local_argb::internal::EffectPriority;
using local_argb::internal::FillDirection;
using local_argb::internal::FillFraction;
using local_argb::internal::LedZone;
using local_argb::internal::LightingCommand;
using local_argb::internal::LightingFill;

constexpr local_argb::Rgb kGauge{0, 16, 0};
constexpr local_argb::Rgb kSignal{16, 8, 0};
constexpr local_argb::Rgb kAmber{128, 16, 0};
constexpr local_argb::Rgb kBrakeRed{local_argb::kBrightnessCeiling, 0, 0};

LightingFill fill(const LedZone zone, const FillFraction level, const local_argb::Rgb color,
                  const EffectPriority priority) {
  return LightingFill{zone, level, {color.red, color.green, color.blue}, priority};
}

LightingCommand held() {
  LightingCommand command{};
  command.valid_until_us = 10'000'000;
  command.actionable = true;
  return command;
}

local_argb::PixelFrame render(const LightingCommand &command) {
  FakePixelSink sink;
  local_argb::internal::RendererController renderer{sink};
  assert(renderer.start());
  assert(renderer.apply(command, 0));
  return sink.writes.back();
}

void paint(local_argb::PixelFrame &frame, const std::size_t first, const std::size_t last,
           const local_argb::Rgb color) {
  for (std::size_t index = first; index <= last; ++index)
    frame[index] = color;
}

// The issue's example: a gauge over 20..79 at priority 50 and a signal over
// 60..79 at priority 100. The signal owns all of 60..79, dark pixels too, and
// the gauge keeps 20..59. Binding order does not matter.
void test_higher_priority_fill_owns_its_whole_zone() {
  const auto gauge = fill(LedZone{20, 60, FillDirection::StartToEnd}, FillFraction::full(), kGauge,
                          EffectPriority{50});
  const auto signal = fill(LedZone{60, 20, FillDirection::StartToEnd}, FillFraction::of(1, 2),
                           kSignal, EffectPriority{100});
  local_argb::PixelFrame expected = local_argb::kBlackFrame;
  paint(expected, 20, 59, kGauge);
  paint(expected, 60, 69, kSignal);

  LightingCommand gauge_first = held();
  assert(gauge_first.fills.add(gauge));
  assert(gauge_first.fills.add(signal));
  assert(render(gauge_first) == expected);

  LightingCommand signal_first = held();
  assert(signal_first.fills.add(signal));
  assert(signal_first.fills.add(gauge));
  assert(render(signal_first) == expected);
}

// A turn above a gauge owns its whole region, including the pixels its
// animation leaves dark; the gauge keeps rendering outside that region.
void test_higher_priority_turn_owns_its_dark_animation_pixels() {
  LightingCommand command = held();
  assert(command.fills.add(fill(LedZone{20, 60, FillDirection::StartToEnd}, FillFraction::full(),
                                kGauge, EffectPriority{50})));
  command.right_turn = true;

  local_argb::PixelFrame expected = local_argb::kBlackFrame;
  paint(expected, 20, local_argb::kRightTurnLedStart - 1, kGauge);
  // At elapsed zero the right flow has only its head, at the inner edge.
  expected[local_argb::kRightTurnLedStart] = kAmber;
  assert(render(command) == expected);
}

// A fill above a turn owns its zone and the turn keeps animating elsewhere.
void test_lower_priority_turn_yields_to_a_fill() {
  LightingCommand command = held();
  command.left_turn = true;
  command.priorities.left_turn = EffectPriority{10};
  assert(command.fills.add(fill(LedZone{30, 5, FillDirection::StartToEnd}, FillFraction::of(2, 5),
                                kGauge, EffectPriority{11})));

  // The left flow starts at pixel 34, which the fill owns and leaves dark.
  local_argb::PixelFrame expected = local_argb::kBlackFrame;
  paint(expected, 30, 31, kGauge);
  assert(render(command) == expected);

  command.priorities.left_turn = EffectPriority{12};
  expected[34] = kAmber;
  expected[30] = local_argb::kBlack;
  expected[31] = local_argb::kBlack;
  assert(render(command) == expected);
}

// Equal priorities keep the drawing order: fills in binding order, then
// brake, then the turns; the later layer owns the overlap.
void test_equal_priorities_resolve_in_drawing_order() {
  LightingCommand fills = held();
  assert(fills.fills.add(fill(LedZone{10, 10, FillDirection::StartToEnd}, FillFraction::full(),
                              kGauge, EffectPriority{})));
  assert(fills.fills.add(fill(LedZone{15, 10, FillDirection::StartToEnd}, FillFraction::of(1, 2),
                              kSignal, EffectPriority{})));
  local_argb::PixelFrame expected = local_argb::kBlackFrame;
  paint(expected, 10, 14, kGauge);
  paint(expected, 15, 19, kSignal);
  assert(render(fills) == expected);

  LightingCommand brake = held();
  brake.brake = true;
  assert(brake.fills.add(fill(LedZone{30, 10, FillDirection::StartToEnd}, FillFraction::full(),
                              kGauge, EffectPriority{})));
  expected = local_argb::kBlackFrame;
  paint(expected, 30, local_argb::kBrakeLedStart - 1, kGauge);
  paint(expected, local_argb::kBrakeLedStart,
        local_argb::kBrakeLedStart + local_argb::kBrakeLedCount - 1, kBrakeRed);
  assert(render(brake) == expected);
}

// Priorities change nothing where effects do not overlap.
void test_priorities_do_not_change_disjoint_effects() {
  LightingCommand defaults = held();
  defaults.brake = true;
  defaults.left_turn = true;
  assert(defaults.fills.add(fill(LedZone{70, 10, FillDirection::EndToStart}, FillFraction::of(1, 2),
                                 kGauge, EffectPriority{})));
  LightingCommand ranked = defaults;
  ranked.priorities = {EffectPriority{200}, EffectPriority{0}, EffectPriority{7}};
  LightingCommand reranked = held();
  reranked.brake = true;
  reranked.left_turn = true;
  assert(reranked.fills.add(fill(LedZone{70, 10, FillDirection::EndToStart}, FillFraction::of(1, 2),
                                 kGauge, EffectPriority{255})));

  const auto expected = render(defaults);
  assert(expected[local_argb::kBrakeLedStart] == kBrakeRed);
  assert(expected[34] == kAmber);
  assert(expected[79] == kGauge);
  assert(render(ranked) == expected);
  assert(render(reranked) == expected);
}

void test_default_priority() {
  assert(EffectPriority{} == EffectPriority{EffectPriority::kDefault});
  assert(EffectPriority{50} < EffectPriority{100});
  const LightingCommand command{};
  assert(command.priorities.left_turn == EffectPriority{});
  assert(command.priorities.right_turn == EffectPriority{});
  assert(command.priorities.brake == EffectPriority{});
  assert(LightingFill{}.priority == EffectPriority{});
}

} // namespace priority

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
  test_fill_renders_its_zone_level_under_the_brightness_ceiling();
  test_empty_fill_list_keeps_the_generic_colour();
  test_fill_list_is_bounded();
  priority::test_default_priority();
  priority::test_higher_priority_fill_owns_its_whole_zone();
  priority::test_higher_priority_turn_owns_its_dark_animation_pixels();
  priority::test_lower_priority_turn_yields_to_a_fill();
  priority::test_equal_priorities_resolve_in_drawing_order();
  priority::test_priorities_do_not_change_disjoint_effects();
  return 0;
}
