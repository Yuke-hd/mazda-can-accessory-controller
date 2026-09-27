#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include "local_argb/local_argb.h"
#include "replay/controller.hpp"
#include "replay/local_argb_stage.hpp"

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
  replay::ReplayClock clock;
  RecordingPixelSink pixels;
  replay::LocalArgbOutputStage stage{pixels};
  replay::ReplayController controller{{left_turn()}, clock, stage};

  CHECK(controller.next_frame_time() == std::optional<vehicle_core::MonotonicTimestamp>{0});
  REQUIRE(controller.start() == replay::ReplayControllerStatus::Ok);
  REQUIRE(pixels.frames().size() == 1);
  CHECK(is_black(pixels.frames().back()));

  const auto step = controller.process_next_frame();
  REQUIRE(step.status == replay::ReplayControllerStatus::Ok);
  CHECK(step.input == replay::ReplayInputResult::Frame);
  REQUIRE(controller.tick_output() == replay::ReplayControllerStatus::Ok);

  const auto &frame = pixels.frames().back();
  CHECK_FALSE(region_has_color(frame, 0, local_argb::kTurnLedCount));
  CHECK(region_has_color(frame, local_argb::kRightTurnLedStart, local_argb::kTurnLedCount));
  CHECK_FALSE(controller.next_frame_time().has_value());

  CHECK(controller.stop() == replay::ReplayControllerStatus::Ok);
  CHECK(is_black(pixels.frames().back()));
}

TEST_CASE("a frame scheduled in the future is rejected without consuming or rendering it") {
  replay::ReplayClock clock;
  RecordingPixelSink pixels;
  replay::LocalArgbOutputStage stage{pixels};
  replay::ReplayController controller{{left_turn(100)}, clock, stage};

  REQUIRE(controller.start() == replay::ReplayControllerStatus::Ok);
  REQUIRE(pixels.frames().size() == 1);
  const auto before = pixels.frames().size();

  const auto not_due = controller.process_next_frame();
  CHECK(not_due.status == replay::ReplayControllerStatus::InvalidState);
  CHECK(not_due.input == replay::ReplayInputResult::Timeout);
  CHECK(controller.next_frame_time() == std::optional<vehicle_core::MonotonicTimestamp>{100});
  CHECK(controller.running());
  CHECK(pixels.frames().size() == before);

  REQUIRE(clock.advance_to(100));
  const auto frame = controller.process_next_frame();
  REQUIRE(frame.status == replay::ReplayControllerStatus::Ok);
  CHECK(frame.input == replay::ReplayInputResult::Frame);
  CHECK_FALSE(controller.next_frame_time().has_value());
  CHECK(controller.stop() == replay::ReplayControllerStatus::Ok);
}

TEST_CASE("sequential replay frames each publish once without drifting the barrier epoch") {
  replay::ReplayClock clock;
  RecordingPixelSink pixels;
  replay::LocalArgbOutputStage stage{pixels};
  replay::ReplayController controller{{left_turn(0), half_rpm(1'000)}, clock, stage};

  REQUIRE(controller.start() == replay::ReplayControllerStatus::Ok);
  REQUIRE(controller.process_next_frame().input == replay::ReplayInputResult::Frame);
  REQUIRE(controller.next_frame_time() == std::optional<vehicle_core::MonotonicTimestamp>{1'000});
  REQUIRE(controller.tick_output() == replay::ReplayControllerStatus::Ok);
  REQUIRE(pixels.frames().size() == 2);
  const auto first_frame = pixels.frames().back();
  CHECK(region_has_color(first_frame, local_argb::kRightTurnLedStart, local_argb::kTurnLedCount));

  REQUIRE(clock.advance_to(1'000));
  const auto second_step = controller.process_next_frame();
  REQUIRE(second_step.status == replay::ReplayControllerStatus::Ok);
  CHECK(second_step.input == replay::ReplayInputResult::Frame);
  CHECK_FALSE(controller.next_frame_time().has_value());
  REQUIRE(controller.sample_polled_rules() == replay::ReplayControllerStatus::Ok);
  REQUIRE(controller.tick_output() == replay::ReplayControllerStatus::Ok);
  REQUIRE(pixels.frames().size() == 3);
  CHECK(colored_pixels(pixels.frames().back()) > colored_pixels(first_frame));
  CHECK(controller.stop() == replay::ReplayControllerStatus::Ok);
}

TEST_CASE("destroying a running replay controller fails the output off") {
  replay::ReplayClock clock;
  RecordingPixelSink pixels;
  replay::LocalArgbOutputStage stage{pixels};
  {
    replay::ReplayController controller{{left_turn()}, clock, stage};
    REQUIRE(controller.start() == replay::ReplayControllerStatus::Ok);
    REQUIRE(controller.process_next_frame().input == replay::ReplayInputResult::Frame);
    REQUIRE(controller.tick_output() == replay::ReplayControllerStatus::Ok);
    REQUIRE_FALSE(is_black(pixels.frames().back()));
  }

  REQUIRE_FALSE(pixels.frames().empty());
  CHECK(is_black(pixels.frames().back()));
}

TEST_CASE("zero and maximum synchronization timeouts fail configuration before starting") {
  for (const auto timeout :
       {vehicle_core::Microseconds{0}, std::numeric_limits<vehicle_core::Microseconds>::max()}) {
    replay::ReplayClock clock;
    RecordingPixelSink pixels;
    replay::LocalArgbOutputStage stage{pixels};
    replay::ReplayController controller{{left_turn()}, clock, stage, timeout};

    CHECK(controller.start() == replay::ReplayControllerStatus::ConfigurationFailed);
    CHECK_FALSE(controller.running());
  }
}

TEST_CASE("largest chrono microseconds timeout permits immediate timeout publication") {
  using TimeoutRep = std::chrono::microseconds::rep;
  const auto timeout =
      static_cast<vehicle_core::Microseconds>(std::numeric_limits<TimeoutRep>::max());
  replay::ReplayClock clock;
  RecordingPixelSink pixels;
  replay::LocalArgbOutputStage stage{pixels};
  replay::ReplayController controller{{}, clock, stage, timeout};

  REQUIRE(controller.start() == replay::ReplayControllerStatus::Ok);
  const auto step = controller.process_timeout();
  CHECK(step.status == replay::ReplayControllerStatus::Ok);
  CHECK(step.input == replay::ReplayInputResult::Timeout);
  CHECK(controller.stop() == replay::ReplayControllerStatus::Ok);
}

TEST_CASE("production RPM range rule emits a half-strip SetLevel through the real renderer") {
  replay::ReplayClock clock;
  RecordingPixelSink pixels;
  replay::LocalArgbOutputStage stage{pixels};
  replay::ReplayController controller{{half_rpm()}, clock, stage};

  REQUIRE(controller.start() == replay::ReplayControllerStatus::Ok);
  REQUIRE(controller.process_next_frame().input == replay::ReplayInputResult::Frame);
  REQUIRE(controller.sample_polled_rules() == replay::ReplayControllerStatus::Ok);
  REQUIRE(controller.tick_output() == replay::ReplayControllerStatus::Ok);

  CHECK(colored_pixels(pixels.frames().back()) == 50);
  CHECK(controller.stop() == replay::ReplayControllerStatus::Ok);
}

TEST_CASE("an explicit timeout publication makes a held turn stale and renders black") {
  replay::ReplayClock clock;
  RecordingPixelSink pixels;
  replay::LocalArgbOutputStage stage{pixels};
  replay::ReplayController controller{{left_turn()}, clock, stage};

  REQUIRE(controller.start() == replay::ReplayControllerStatus::Ok);
  REQUIRE(controller.process_next_frame().input == replay::ReplayInputResult::Frame);
  REQUIRE(controller.tick_output() == replay::ReplayControllerStatus::Ok);
  REQUIRE_FALSE(is_black(pixels.frames().back()));

  REQUIRE(clock.advance_to(250'001));
  const auto timeout = controller.process_timeout();
  REQUIRE(timeout.status == replay::ReplayControllerStatus::Ok);
  CHECK(timeout.input == replay::ReplayInputResult::Timeout);
  REQUIRE(controller.tick_output() == replay::ReplayControllerStatus::Ok);
  CHECK(is_black(pixels.frames().back()));

  CHECK(controller.stop() == replay::ReplayControllerStatus::Ok);
}

TEST_CASE("empty replay supports timeout work and remains one-shot after clean stop") {
  replay::ReplayClock clock;
  RecordingPixelSink pixels;
  replay::LocalArgbOutputStage stage{pixels};
  replay::ReplayController controller{{}, clock, stage};

  REQUIRE(controller.start() == replay::ReplayControllerStatus::Ok);
  CHECK_FALSE(controller.next_frame_time().has_value());
  const auto end = controller.process_next_frame();
  REQUIRE(end.status == replay::ReplayControllerStatus::Ok);
  CHECK(end.input == replay::ReplayInputResult::EndOfStream);
  CHECK(controller.process_timeout().input == replay::ReplayInputResult::Timeout);
  CHECK(controller.stop() == replay::ReplayControllerStatus::Ok);
  CHECK(controller.start() == replay::ReplayControllerStatus::InvalidState);
}

TEST_CASE("stop releases the Runtime waiting at the input gate with a frozen clock") {
  replay::ReplayClock clock{9'000'000};
  RecordingPixelSink pixels;
  replay::LocalArgbOutputStage stage{pixels};
  replay::ReplayController controller{{}, clock, stage};

  REQUIRE(controller.start() == replay::ReplayControllerStatus::Ok);
  CHECK(controller.running());
  CHECK(controller.stop() == replay::ReplayControllerStatus::Ok);
  CHECK(clock.now() == 9'000'000);
  CHECK(is_black(pixels.frames().back()));
}

TEST_CASE("renderer failure attempts black and stop keeps the host output failed off") {
  replay::ReplayClock clock;
  RecordingPixelSink pixels;
  replay::LocalArgbOutputStage stage{pixels};
  replay::ReplayController controller{{left_turn()}, clock, stage};

  REQUIRE(controller.start() == replay::ReplayControllerStatus::Ok);
  REQUIRE(controller.process_next_frame().input == replay::ReplayInputResult::Frame);
  pixels.fail_next_write();
  CHECK(controller.tick_output() == replay::ReplayControllerStatus::OutputFault);
  CHECK_FALSE(controller.running());
  CHECK(is_black(pixels.frames().back()));
  CHECK(controller.stop() == replay::ReplayControllerStatus::Ok);
  CHECK(is_black(pixels.frames().back()));
}

TEST_CASE("source fault publication completes before faulted telemetry stops") {
  replay::ReplayClock clock;
  RecordingPixelSink pixels;
  replay::LocalArgbOutputStage stage{pixels};
  replay::ReplayController controller{{left_turn()}, clock, stage};

  REQUIRE(controller.start() == replay::ReplayControllerStatus::Ok);
  REQUIRE(controller.process_next_frame().input == replay::ReplayInputResult::Frame);
  REQUIRE(controller.tick_output() == replay::ReplayControllerStatus::Ok);
  REQUIRE_FALSE(is_black(pixels.frames().back()));
  const auto fault = controller.process_source_fault();
  CHECK(fault.status == replay::ReplayControllerStatus::TelemetryFault);
  CHECK(fault.input == replay::ReplayInputResult::Fault);
  CHECK(is_black(pixels.frames().back()));
  CHECK(controller.stop() == replay::ReplayControllerStatus::TelemetryFault);
  CHECK(is_black(pixels.frames().back()));
}
