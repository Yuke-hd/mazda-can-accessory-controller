#include <cassert>
#include <limits>
#include <vector>

#include "local_argb/lighting_sink.hpp"
#include "local_argb/local_argb.h"
#include "local_argb/renderer.hpp"
#include "local_argb/stall_gated_sink.hpp"
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

namespace transient {

using namespace local_argb::internal;
constexpr local_argb::Rgb kBlue{0, 0, 16};
static_assert(sizeof(LightingTransientStart) <= 64, "queue slots must not waste epoch padding");

LightingCommand started(std::uint64_t sequence = 1, vehicle_core::Microseconds duration = 800'000,
                        EffectPriority priority = EffectPriority{150},
                        vehicle_core::MonotonicTimestamp origin_us = 0, std::uint32_t epoch = 0) {
  LightingCommand command = priority::held();
  assert(command.fills.add(
      priority::fill({10, 20}, FillFraction::full(), priority::kGauge, EffectPriority{50})));
  assert(command.transients.add(
      {TransientId{1}, sequence, {{15, 5}, {0, 0, 255}, priority, duration}, origin_us, epoch}));
  return command;
}

void test_start_active_expiry_and_zone() {
  FakePixelSink sink;
  RendererController renderer{sink};
  assert(renderer.start());
  assert(renderer.apply(started(), 100));
  const auto first = sink.writes.back();
  assert(first[9] == local_argb::kBlack);
  assert(first[10] == priority::kGauge);
  assert(first[15] == kBlue);
  assert(first[19] == kBlue);
  assert(first[20] == priority::kGauge);
  assert(renderer.tick(400'100));
  assert(sink.writes.back() == first);
  assert(renderer.tick(800'100));
  assert(sink.writes.back()[15] == priority::kGauge);
  assert(renderer.apply(started(), 900'000));
  assert(sink.writes.back()[15] == priority::kGauge); // seen sequence never replays
}

void test_held_update_does_not_restart_but_new_sequence_does() {
  FakePixelSink sink;
  RendererController renderer{sink};
  assert(renderer.start());
  assert(renderer.apply(started(), 100));
  auto update = started();
  update.brake = true;
  assert(renderer.apply(update, 400'100));
  assert(renderer.tick(800'100));
  assert(sink.writes.back()[15] == priority::kGauge);
  assert(renderer.apply(started(2, 800'000, EffectPriority{150}, 900'000), 900'000));
  assert(renderer.tick(1'699'999));
  assert(sink.writes.back()[15] == kBlue);
  assert(renderer.tick(1'700'000));
  assert(sink.writes.back()[15] == priority::kGauge);
  assert(renderer.apply(started(3, 800'000, EffectPriority{150}, 1'800'000), 1'800'000));
  assert(renderer.apply(started(4, 800'000, EffectPriority{150}, 2'000'000), 2'000'000));
  assert(renderer.tick(2'799'999));
  assert(sink.writes.back()[15] == kBlue);
  assert(renderer.tick(2'800'000));
  assert(sink.writes.back()[15] == priority::kGauge);
}

void test_priorities_and_equal_priority_drawing_order() {
  assert(priority::render(started(1, 800'000, EffectPriority{49}))[15] == priority::kGauge);
  assert(priority::render(started(1, 800'000, EffectPriority{50}))[15] == kBlue);
  auto command = started();
  assert(
      command.transients.add({TransientId{2}, 1, {{17, 5}, {16, 0, 0}, EffectPriority{150}, 100}}));
  const auto frame = priority::render(command);
  assert(frame[16] == kBlue);
  assert(frame[17] == priority::kBrakeRed);
  command.left_turn = true;
  command.priorities.left_turn = EffectPriority{200};
  assert(priority::render(command)[15] == local_argb::kBlack);
}

void test_same_sequence_preserves_the_original_effect() {
  FakePixelSink sink;
  RendererController renderer{sink};
  assert(renderer.start());
  assert(renderer.apply(started(), 100));
  auto changed = priority::held();
  assert(changed.transients.add({TransientId{1}, 1, {{99, 2}, {16, 0, 0}, {}, 1}}));
  assert(renderer.apply(changed, 400'100));
  assert(sink.writes.back()[15] == kBlue);
  assert(sink.writes.back()[99] == local_argb::kBlack);
  assert(renderer.tick(800'100));
  assert(sink.writes.back() == local_argb::kBlackFrame);
}

void test_invalid_requests_are_bounded_and_fail_off() {
  LightingTransientStarts invalid;
  assert(!invalid.add({TransientId{1}, 1, {{15, 5}, {}, {}, 0}}));
  assert(!invalid.add({TransientId{1}, 1, {{15, 5}, {}, {}, kMaxTransientDurationUs + 1}}));
  assert(!invalid.add({TransientId{1},
                       1,
                       {{15, 5}, {}, {}, std::numeric_limits<vehicle_core::Microseconds>::max()}}));
  assert(invalid.empty());
  assert(invalid.add({TransientId{1}, 1, {{15, 5}, {}, {}, kMaxTransientDurationUs}}));
  auto command = priority::held();
  for (std::uint8_t id = 1; id <= LightingTransientStarts::kCapacity; ++id)
    assert(command.transients.add({TransientId{id}, 1, {{id, 1}, {0, 0, 16}, {}, 100}}));
  assert(!command.transients.add({TransientId{1}, 2, {}}));
  assert(!LightingTransientStarts{}.add({TransientId{0}, 1, {}}));
  assert(!LightingTransientStarts{}.add({TransientId{9}, 1, {}}));
  assert(!LightingTransientStarts{}.add({TransientId{1}, 0, {}}));
  command = priority::held();
  assert(command.transients.add({TransientId{1}, 1, {{99, 2}, {0, 0, 16}, {}, 100}}));
  assert(priority::render(command) == local_argb::kBlackFrame);
}

void test_cancel_expired_command_fault_and_backwards_time_do_not_replay() {
  FakePixelSink sink;
  RendererController renderer{sink};
  assert(renderer.start());
  assert(renderer.apply(started(), 100));
  assert(renderer.apply({}, 200));
  assert(renderer.apply(started(), 300));
  assert(sink.writes.back()[15] == priority::kGauge);
  auto expired = started(2);
  expired.valid_until_us = 400;
  assert(renderer.apply(expired, 400));
  assert(renderer.tick(401));
  assert(renderer.apply(started(2), 500));
  assert(sink.writes.back()[15] == priority::kGauge);
  sink.failures_remaining = 1;
  assert(!renderer.apply(started(3), 600));
  assert(sink.writes.back() == local_argb::kBlackFrame);
  assert(renderer.apply(started(3), 700));
  assert(sink.writes.back()[15] == priority::kGauge);
  assert(renderer.apply(started(4), 800));
  assert(renderer.tick(799));
  assert(sink.writes.back()[15] == priority::kGauge);
  assert(renderer.apply(started(4), 900));
  assert(sink.writes.back()[15] == priority::kGauge);
}

void test_mailbox_preserves_latest_start_and_duration_near_clock_limit() {
  Mailbox mailbox;
  mailbox.submit(started());
  auto update = started();
  update.brake = true;
  mailbox.submit(update);
  LightingCommand taken{};
  assert(mailbox.take(taken));
  assert(priority::render(taken)[15] == kBlue);
  FakePixelSink sink;
  RendererController renderer{sink};
  assert(renderer.start());
  const auto limit = std::numeric_limits<vehicle_core::MonotonicTimestamp>::max();
  const auto near_limit = limit - 50;
  auto command = started(1, 100, EffectPriority{150}, near_limit);
  command.valid_until_us = limit;
  assert(renderer.apply(command, near_limit));
  assert(renderer.tick(command.valid_until_us));
  assert(sink.writes.back()[15] == kBlue); // elapsed comparison cannot overflow
}

void test_gate_rejected_start_is_not_replayed_after_resume() {
  FakeLightingSink downstream;
  StallGatedSink gate{downstream};
  FakePixelSink sink;
  RendererController renderer{sink};
  assert(renderer.start());
  gate.close();
  const auto rejected = started(1, 1'000, EffectPriority{150}, 100, gate.transient_epoch());
  assert(!gate.publish(rejected));
  gate.open();
  assert(gate.publish(rejected)); // a held update retains the rejected start
  assert(renderer.apply(downstream.commands.back(), 200, gate.transient_epoch()));
  assert(sink.writes.back()[15] == priority::kGauge);
  const auto fresh = started(2, 1'000, EffectPriority{150}, 200, gate.transient_epoch());
  assert(gate.publish(fresh));
  assert(renderer.apply(downstream.commands.back(), 200, gate.transient_epoch()));
  assert(sink.writes.back()[15] == kBlue);
}

void test_unseen_starts_require_current_epoch_and_fresh_origin() {
  FakePixelSink sink;
  RendererController renderer{sink};
  assert(renderer.start());
  const auto old = started(1, 1'000, EffectPriority{150}, 100, 0);
  // The queue accepted the start, but explicit fail-off overwrote it before take.
  Mailbox mailbox;
  mailbox.submit(old);
  mailbox.submit({});
  LightingCommand taken;
  assert(mailbox.take(taken));
  assert(renderer.apply(taken, 150, 1));
  assert(renderer.apply(old, 200, 1));
  assert(sink.writes.back()[15] == priority::kGauge);
  const auto fresh = started(2, 1'000, EffectPriority{150}, 200, 1);
  assert(renderer.apply(fresh, 300, 1));
  assert(sink.writes.back()[15] == kBlue);
  assert(renderer.tick(1'299)); // full duration begins at application time
  assert(sink.writes.back()[15] == kBlue);
  assert(renderer.tick(1'300));
  assert(sink.writes.back()[15] == priority::kGauge);
  assert(renderer.apply(started(3, 1'000, EffectPriority{150}, 200, 1), 1'300, 1));
  assert(sink.writes.back()[15] == priority::kGauge);
  assert(renderer.apply(started(4, 1'000, EffectPriority{150}, 2'000, 1), 1'400, 1));
  assert(sink.writes.back()[15] == priority::kGauge); // future origin is invalid
}

void test_compatibility_colour_is_restored_after_transient_expiry() {
  FakePixelSink sink;
  RendererController renderer{sink};
  assert(renderer.start());
  auto command = priority::held();
  command.color = {0, 12, 0};
  assert(command.transients.add({TransientId{1}, 1, {{15, 5}, {0, 0, 16}, {}, 100}}));
  assert(renderer.apply(command, 0));
  assert((sink.writes.back()[14] == local_argb::Rgb{0, 12, 0}));
  assert(sink.writes.back()[15] == kBlue);
  assert((sink.writes.back()[20] == local_argb::Rgb{0, 12, 0}));
  assert(renderer.tick(100));
  assert((sink.writes.back()[15] == local_argb::Rgb{0, 12, 0}));
  assert(renderer.tick(50)); // latched expiry cannot revive on clock rollback
  assert((sink.writes.back()[15] == local_argb::Rgb{0, 12, 0}));
}

} // namespace transient

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
  transient::test_gate_rejected_start_is_not_replayed_after_resume();
  transient::test_unseen_starts_require_current_epoch_and_fresh_origin();
  transient::test_compatibility_colour_is_restored_after_transient_expiry();
  transient::test_start_active_expiry_and_zone();
  transient::test_held_update_does_not_restart_but_new_sequence_does();
  transient::test_priorities_and_equal_priority_drawing_order();
  transient::test_same_sequence_preserves_the_original_effect();
  transient::test_invalid_requests_are_bounded_and_fail_off();
  transient::test_cancel_expired_command_fault_and_backwards_time_do_not_replay();
  transient::test_mailbox_preserves_latest_start_and_duration_near_clock_limit();
  return 0;
}
