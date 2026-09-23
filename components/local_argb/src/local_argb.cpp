#include "../private_include/local_argb/renderer.hpp"

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
  if (command_.brake) {
    for (std::size_t index = 0; index < kBrakeLedCount; ++index)
      frame[kBrakeLedStart + index] = kRed;
  }

  const bool turn_active = command_.left_turn || command_.right_turn;
  if (turn_active) {
    const auto elapsed = now_us >= animation_started_us_ ? now_us - animation_started_us_ : 0;
    animation_(frame, AnimationContext{command_.left_turn, command_.right_turn, elapsed});
  } else if (!command_.brake) {
    // Preserve the generic colour-only handoff for compatibility consumers.
    const Rgb color{
        command_.color.red > kBrightnessCeiling ? kBrightnessCeiling : command_.color.red,
        command_.color.green > kBrightnessCeiling ? kBrightnessCeiling : command_.color.green,
        command_.color.blue > kBrightnessCeiling ? kBrightnessCeiling : command_.color.blue};
    frame.fill(color);
  }
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
