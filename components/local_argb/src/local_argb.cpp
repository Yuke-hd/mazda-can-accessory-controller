#include "../private_include/local_argb/renderer.hpp"

#include "../private_include/local_argb/led_zone.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace local_argb::internal {

namespace {

constexpr vehicle_core::Microseconds kFlowStepUs = 33'333;
constexpr std::size_t kFlowTailLength = 5;
constexpr Rgb kAmber{128, 16, 0};
constexpr Rgb kRed{kBrightnessCeiling, 0, 0};

Rgb scaled(const Rgb color, const std::size_t numerator, const std::size_t denominator) noexcept {
  return {static_cast<std::uint8_t>(color.red * numerator / denominator),
          static_cast<std::uint8_t>(color.green * numerator / denominator),
          static_cast<std::uint8_t>(color.blue * numerator / denominator)};
}

std::uint8_t ceiling_clamped(const std::uint8_t channel) noexcept {
  return channel > kBrightnessCeiling ? kBrightnessCeiling : channel;
}

Rgb ceiling_clamped(const LightingRgb color) noexcept {
  return {ceiling_clamped(color.red), ceiling_clamped(color.green), ceiling_clamped(color.blue)};
}

void draw_flow(PixelFrame &frame, const bool left, const std::size_t phase) noexcept {
  for (std::size_t tail = 0; tail < kFlowTailLength; ++tail) {
    const auto brightness = kFlowTailLength - tail;
    if (left) {
      // The left flow starts beside the brake region and travels toward the
      // outer edge. Its tail remains behind it toward the center and is
      // clipped when it would leave the left turn region.
      const auto head = kTurnLedCount - 1 - phase;
      const auto position = head + tail;
      if (position < kTurnLedCount)
        frame[position] = scaled(kAmber, brightness, kFlowTailLength);
    } else {
      // The right flow starts beside the brake region and travels toward the
      // outer edge. Its tail remains behind it toward the center and is
      // clipped when it would leave the right turn region.
      const auto head = phase;
      if (head >= tail)
        frame[kRightTurnLedStart + head - tail] = scaled(kAmber, brightness, kFlowTailLength);
    }
  }
}

void draw_center_out_fill(PixelFrame &frame, const bool left, const std::size_t phase) noexcept {
  const auto fill_count = phase + 1;
  for (std::size_t offset = 0; offset < fill_count; ++offset) {
    const auto position = left ? kTurnLedCount - 1 - offset : kRightTurnLedStart + offset;
    frame[position] = kAmber;
  }
}

enum class LayerKind : std::uint8_t { Fill, Brake, LeftTurn, RightTurn };

// One lit effect and the strip region it owns while no higher layer covers it.
struct Layer {
  LayerKind kind{LayerKind::Brake};
  EffectPriority priority{};
  const LightingFill *fill{nullptr};
  std::size_t start{0};
  std::size_t count{0};
};

// The lit effects of a command, in drawing order: fills in list order, then
// brake, then the left turn, then the right turn. Fills that are empty or
// have an invalid zone light nothing and so own nothing.
class LayerStack {
public:
  explicit LayerStack(const LightingCommand &command) noexcept {
    for (const LightingFill &fill : command.fills) {
      if (fill.level != FillFraction::empty() && validate_zone(fill.zone) == ZoneValidity::Valid)
        push({LayerKind::Fill, fill.priority, &fill, fill.zone.start, fill.zone.length});
    }
    if (command.brake)
      push({LayerKind::Brake, command.priorities.brake, nullptr, kBrakeLedStart, kBrakeLedCount});
    if (command.left_turn)
      push({LayerKind::LeftTurn, command.priorities.left_turn, nullptr, 0, kTurnLedCount});
    if (command.right_turn)
      push({LayerKind::RightTurn, command.priorities.right_turn, nullptr, kRightTurnLedStart,
            kTurnLedCount});
  }

  // Stable insertion sort by ascending priority, so equal priorities keep
  // their drawing order and the last layer drawn is the one that wins.
  void sort_by_priority() noexcept {
    for (std::size_t index = 1; index < count_; ++index) {
      const Layer layer = layers_[index];
      std::size_t slot = index;
      for (; slot > 0 && layer.priority < layers_[slot - 1].priority; --slot)
        layers_[slot] = layers_[slot - 1];
      layers_[slot] = layer;
    }
  }

  [[nodiscard]] const Layer *begin() const noexcept { return layers_.data(); }
  [[nodiscard]] const Layer *end() const noexcept { return layers_.data() + count_; }

private:
  void push(const Layer &layer) noexcept { layers_[count_++] = layer; }

  std::array<Layer, LightingFills::kCapacity + 3> layers_{};
  std::size_t count_{0};
};

void draw_layer(PixelFrame &frame, const Layer &layer, const AnimationStrategy animation,
                const vehicle_core::Microseconds elapsed_us) noexcept {
  switch (layer.kind) {
  case LayerKind::Fill:
    (void)fill_zone(frame, layer.fill->zone, layer.fill->level, ceiling_clamped(layer.fill->color));
    return;
  case LayerKind::Brake:
    for (std::size_t index = 0; index < layer.count; ++index)
      frame[layer.start + index] = kRed;
    return;
  case LayerKind::LeftTurn:
    animation(frame, AnimationContext{true, false, elapsed_us});
    return;
  case LayerKind::RightTurn:
    animation(frame, AnimationContext{false, true, elapsed_us});
    return;
  }
}

// A layer owns its whole region, dark pixels included: clearing the region
// first removes every lower layer's pixels there. No colours are mixed.
void draw_owned(PixelFrame &frame, const Layer &layer, const AnimationStrategy animation,
                const vehicle_core::Microseconds elapsed_us) noexcept {
  for (std::size_t index = 0; index < layer.count; ++index)
    frame[layer.start + index] = kBlack;
  draw_layer(frame, layer, animation, elapsed_us);
}

} // namespace

void render_running_flow(PixelFrame &frame, const AnimationContext &context) noexcept {
  const auto phase = static_cast<std::size_t>((context.elapsed_us / kFlowStepUs) % kTurnLedCount);
  if (context.left_turn)
    draw_flow(frame, true, phase);
  if (context.right_turn)
    draw_flow(frame, false, phase);
}

void render_center_out_fill(PixelFrame &frame, const AnimationContext &context) noexcept {
  const auto elapsed_steps = context.elapsed_us / kFlowStepUs;
  constexpr auto kFillCycleSteps = static_cast<vehicle_core::Microseconds>(kTurnLedCount + 1);
  const auto cycle_step = static_cast<std::size_t>(elapsed_steps % kFillCycleSteps);
  if (cycle_step == kTurnLedCount) {
    return;
  }
  const auto phase = cycle_step;
  if (context.left_turn)
    draw_center_out_fill(frame, true, phase);
  if (context.right_turn)
    draw_center_out_fill(frame, false, phase);
}

bool RendererController::start() noexcept {
  has_command_ = false;
  has_last_written_ = false;
  faulted_ = false;
  animation_started_us_ = 0;
  return write_desired(kBlackFrame);
}

bool RendererController::apply(const LightingCommand command,
                               const vehicle_core::MonotonicTimestamp now_us) noexcept {
  const bool effects_changed = !has_command_ || command.left_turn != command_.left_turn ||
                               command.right_turn != command_.right_turn ||
                               command.brake != command_.brake;
  command_ = command;
  has_command_ = true;
  if (effects_changed)
    animation_started_us_ = now_us;
  // A fresh command is a recovery boundary only after black was successfully
  // written. If the clear also failed, keep retrying black.
  if (!faulted_ || (has_last_written_ && last_written_ == kBlackFrame)) {
    faulted_ = false;
  }
  return tick(now_us);
}

bool RendererController::tick(const vehicle_core::MonotonicTimestamp now_us) noexcept {
  PixelFrame desired = kBlackFrame;
  if (!faulted_ && has_command_ && command_.actionable && now_us <= command_.valid_until_us) {
    desired = frame_for(now_us);
  }
  return write_desired(desired);
}

PixelFrame
RendererController::frame_for(const vehicle_core::MonotonicTimestamp now_us) const noexcept {
  PixelFrame frame = kBlackFrame;
  if (!command_.left_turn && !command_.right_turn && !command_.brake && command_.fills.empty()) {
    // Preserve the generic colour-only handoff for compatibility consumers.
    frame.fill(ceiling_clamped(command_.color));
    return frame;
  }

  // Painter's order by priority: each layer owns its region wherever no
  // later, higher-or-equal layer overlaps it; see EffectPriority.
  LayerStack layers{command_};
  layers.sort_by_priority();
  const auto elapsed = now_us >= animation_started_us_ ? now_us - animation_started_us_ : 0;
  for (const Layer &layer : layers)
    draw_owned(frame, layer, animation_, elapsed);
  return frame;
}

bool RendererController::write_desired(const PixelFrame &desired) noexcept {
  if (has_last_written_ && desired == last_written_) {
    return true;
  }
  if (sink_->write(desired)) {
    last_written_ = desired;
    has_last_written_ = true;
    return true;
  }

  faulted_ = true;
  has_last_written_ = false;
  if (desired != kBlackFrame && sink_->write(kBlackFrame)) {
    last_written_ = kBlackFrame;
    has_last_written_ = true;
  }
  return false;
}

} // namespace local_argb::internal
