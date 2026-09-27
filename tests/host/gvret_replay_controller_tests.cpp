#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <utility>
#include <vector>

#include "gvret/replay_controller.hpp"

namespace {

constexpr std::uint32_t kEngineDataId = 0x202;
constexpr std::uint32_t kTurnSwitchId = 0x091;

class RecordingPixelSink final : public local_argb::PixelFrameSink {
public:
  bool write(const local_argb::PixelFrame &frame) noexcept override {
    if (fail_next_) {
      fail_next_ = false;
      return false;
    }
    frames_.push_back(frame);
    return true;
  }

  void fail_next_write() noexcept { fail_next_ = true; }

  [[nodiscard]] const std::vector<local_argb::PixelFrame> &frames() const noexcept {
    return frames_;
  }

private:
  std::vector<local_argb::PixelFrame> frames_{};
  bool fail_next_{false};
};

gvret::TimedCanFrame timed_frame(const vehicle_core::MonotonicTimestamp timestamp_us,
                                 const std::uint32_t identifier,
                                 std::initializer_list<std::uint8_t> bytes) {
  gvret::TimedCanFrame timed{};
  timed.relative_time_us = timestamp_us;
  timed.frame.identifier = identifier;
  timed.frame.timestamp_us = timestamp_us;
  timed.frame.dlc = static_cast<std::uint8_t>(bytes.size());
  std::copy(bytes.begin(), bytes.end(), timed.frame.data.begin());
  return timed;
}

gvret::TimedCanFrame left_turn(const vehicle_core::MonotonicTimestamp timestamp_us = 0) {
  return timed_frame(timestamp_us, kTurnSwitchId, {0, 0x20, 0, 0, 0, 0, 0, 0});
}

gvret::TimedCanFrame half_rpm(const vehicle_core::MonotonicTimestamp timestamp_us = 0) {
  // ENGINE_DATA raw RPM 13,000 / 4 = 3,250 rpm, half the production 0..6500 range.
  return timed_frame(timestamp_us, kEngineDataId, {0x32, 0xc8, 0, 0, 0, 0, 0, 0});
}

bool is_black(const local_argb::PixelFrame &frame) {
  return std::all_of(frame.begin(), frame.end(),
                     [](const local_argb::Rgb pixel) { return pixel == local_argb::kBlack; });
}

std::size_t colored_pixels(const local_argb::PixelFrame &frame) {
  return static_cast<std::size_t>(
      std::count_if(frame.begin(), frame.end(),
                    [](const local_argb::Rgb pixel) { return pixel != local_argb::kBlack; }));
}

bool region_has_color(const local_argb::PixelFrame &frame, const std::size_t start,
                      const std::size_t count) {
  return std::any_of(frame.begin() + static_cast<std::ptrdiff_t>(start),
                     frame.begin() + static_cast<std::ptrdiff_t>(start + count),
                     [](const local_argb::Rgb pixel) { return pixel != local_argb::kBlack; });
}

} // namespace

TEST_CASE(
    "timestamp-zero turn replay preserves startup black then renders production turn output") {
  gvret::ReplayClock clock;
  RecordingPixelSink pixels;
  gvret::ReplayController controller{{left_turn()}, clock, pixels};

  CHECK(controller.next_frame_time() == std::optional<vehicle_core::MonotonicTimestamp>{0});
  REQUIRE(controller.start() == gvret::ReplayControllerStatus::Ok);
  REQUIRE(pixels.frames().size() == 1);
  CHECK(is_black(pixels.frames().back()));

  const auto step = controller.process_next_frame();
  REQUIRE(step.status == gvret::ReplayControllerStatus::Ok);
  CHECK(step.input == gvret::ReplayInputResult::Frame);
  REQUIRE(controller.render() == gvret::ReplayControllerStatus::Ok);

  const auto &frame = pixels.frames().back();
  CHECK_FALSE(region_has_color(frame, 0, local_argb::kTurnLedCount));
  CHECK(region_has_color(frame, local_argb::kRightTurnLedStart, local_argb::kTurnLedCount));
  CHECK_FALSE(controller.next_frame_time().has_value());

  CHECK(controller.stop() == gvret::ReplayControllerStatus::Ok);
  CHECK(is_black(pixels.frames().back()));
}

TEST_CASE("production RPM range rule emits a half-strip SetLevel through the real renderer") {
  gvret::ReplayClock clock;
  RecordingPixelSink pixels;
  gvret::ReplayController controller{{half_rpm()}, clock, pixels};

  REQUIRE(controller.start() == gvret::ReplayControllerStatus::Ok);
  REQUIRE(controller.process_next_frame().input == gvret::ReplayInputResult::Frame);
  REQUIRE(controller.sample_polled_rules() == gvret::ReplayControllerStatus::Ok);
  REQUIRE(controller.render() == gvret::ReplayControllerStatus::Ok);

  CHECK(colored_pixels(pixels.frames().back()) == 50);
  CHECK(controller.stop() == gvret::ReplayControllerStatus::Ok);
}

TEST_CASE("an explicit timeout publication makes a held turn stale and renders black") {
  gvret::ReplayClock clock;
  RecordingPixelSink pixels;
  gvret::ReplayController controller{{left_turn()}, clock, pixels};

  REQUIRE(controller.start() == gvret::ReplayControllerStatus::Ok);
  REQUIRE(controller.process_next_frame().input == gvret::ReplayInputResult::Frame);
  REQUIRE(controller.render() == gvret::ReplayControllerStatus::Ok);
  REQUIRE_FALSE(is_black(pixels.frames().back()));

  REQUIRE(clock.advance_to(250'001));
  const auto timeout = controller.process_timeout();
  REQUIRE(timeout.status == gvret::ReplayControllerStatus::Ok);
  CHECK(timeout.input == gvret::ReplayInputResult::Timeout);
  REQUIRE(controller.render() == gvret::ReplayControllerStatus::Ok);
  CHECK(is_black(pixels.frames().back()));

  CHECK(controller.stop() == gvret::ReplayControllerStatus::Ok);
}

TEST_CASE("empty replay supports timeout work and remains one-shot after clean stop") {
  gvret::ReplayClock clock;
  RecordingPixelSink pixels;
  gvret::ReplayController controller{{}, clock, pixels};

  REQUIRE(controller.start() == gvret::ReplayControllerStatus::Ok);
  CHECK_FALSE(controller.next_frame_time().has_value());
  const auto end = controller.process_next_frame();
  REQUIRE(end.status == gvret::ReplayControllerStatus::Ok);
  CHECK(end.input == gvret::ReplayInputResult::EndOfStream);
  CHECK(controller.process_timeout().input == gvret::ReplayInputResult::Timeout);
  CHECK(controller.stop() == gvret::ReplayControllerStatus::Ok);
  CHECK(controller.start() == gvret::ReplayControllerStatus::InvalidState);
}

TEST_CASE("renderer failure attempts black and stop keeps the host output failed off") {
  gvret::ReplayClock clock;
  RecordingPixelSink pixels;
  gvret::ReplayController controller{{left_turn()}, clock, pixels};

  REQUIRE(controller.start() == gvret::ReplayControllerStatus::Ok);
  REQUIRE(controller.process_next_frame().input == gvret::ReplayInputResult::Frame);
  pixels.fail_next_write();
  CHECK(controller.render() == gvret::ReplayControllerStatus::RendererFault);
  CHECK_FALSE(controller.running());
  CHECK(is_black(pixels.frames().back()));
  CHECK(controller.stop() == gvret::ReplayControllerStatus::Ok);
  CHECK(is_black(pixels.frames().back()));
}

TEST_CASE("source fault publication completes before faulted telemetry stops") {
  gvret::ReplayClock clock;
  RecordingPixelSink pixels;
  gvret::ReplayController controller{{left_turn()}, clock, pixels};

  REQUIRE(controller.start() == gvret::ReplayControllerStatus::Ok);
  REQUIRE(controller.process_next_frame().input == gvret::ReplayInputResult::Frame);
  REQUIRE(controller.render() == gvret::ReplayControllerStatus::Ok);
  REQUIRE_FALSE(is_black(pixels.frames().back()));
  const auto fault = controller.process_source_fault();
  CHECK(fault.status == gvret::ReplayControllerStatus::TelemetryFault);
  CHECK(fault.input == gvret::ReplayInputResult::Fault);
  CHECK(is_black(pixels.frames().back()));
  CHECK(controller.stop() == gvret::ReplayControllerStatus::TelemetryFault);
  CHECK(is_black(pixels.frames().back()));
}
