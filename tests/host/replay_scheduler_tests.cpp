#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <vector>

#include "controller_config/timing.hpp"
#include "local_argb/local_argb.h"
#include "mazda/facade_contracts.hpp"
#include "replay/local_argb_stage.hpp"
#include "replay/scheduler.hpp"

namespace {

struct OutputFrame {
  vehicle_core::MonotonicTimestamp time_us;
  local_argb::PixelFrame pixels;
};

class RecordingPixels final : public local_argb::PixelFrameSink {
public:
  explicit RecordingPixels(const replay::ReplayClock &clock) : clock_(clock) {}

  bool write(const local_argb::PixelFrame &frame) noexcept override {
    if (fail_write_number_ == frames_.size() + 1) {
      fail_write_number_ = 0;
      return false;
    }
    frames_.push_back({clock_.now(), frame});
    return true;
  }

  void fail_write_number(const std::size_t number) noexcept { fail_write_number_ = number; }
  [[nodiscard]] const std::vector<OutputFrame> &frames() const noexcept { return frames_; }

private:
  const replay::ReplayClock &clock_;
  std::vector<OutputFrame> frames_{};
  std::size_t fail_write_number_{0};
};

class RecordingEvents final : public replay::ReplayEventSink {
public:
  void record(const replay::ReplayEvent event) noexcept override { events_.push_back(event); }
  [[nodiscard]] const std::vector<replay::ReplayEvent> &events() const noexcept { return events_; }

private:
  std::vector<replay::ReplayEvent> events_{};
};

gvret::TimedCanFrame timed_frame(const vehicle_core::MonotonicTimestamp time_us,
                                 const std::uint32_t identifier,
                                 const std::initializer_list<std::uint8_t> bytes) {
  gvret::TimedCanFrame frame{};
  frame.relative_time_us = time_us;
  frame.frame.timestamp_us = time_us;
  frame.frame.identifier = identifier;
  frame.frame.dlc = static_cast<std::uint8_t>(bytes.size());
  std::copy(bytes.begin(), bytes.end(), frame.frame.data.begin());
  return frame;
}

gvret::TimedCanFrame left_turn(const vehicle_core::MonotonicTimestamp time_us) {
  return timed_frame(time_us, 0x091, {0, 0x20, 0, 0, 0, 0, 0, 0});
}

gvret::TimedCanFrame turn_off(const vehicle_core::MonotonicTimestamp time_us) {
  return timed_frame(time_us, 0x091, {0, 0, 0, 0, 0, 0, 0, 0});
}

gvret::TimedCanFrame half_rpm(const vehicle_core::MonotonicTimestamp time_us) {
  return timed_frame(time_us, 0x202, {0x32, 0xc8, 0, 0, 0, 0, 0, 0});
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

} // namespace

TEST_CASE("held turn animates at output-stage ticks without new CAN frames") {
  replay::ReplayClock clock;
  RecordingPixels pixels{clock};
  replay::LocalArgbOutputStage stage{pixels};
  const auto result = replay::run_replay({left_turn(0)}, clock, stage, {40'000});

  REQUIRE(result.ok());
  CHECK(result.frames_delivered == 1);
  CHECK(result.end_of_stream);
  CHECK(clock.now() == 40'000);
  const auto &output = pixels.frames();
  const auto at_zero = std::find_if(output.begin(), output.end(), [](const OutputFrame &frame) {
    return frame.time_us == 0 && !is_black(frame.pixels);
  });
  const auto at_forty = std::find_if(output.begin(), output.end(), [](const OutputFrame &frame) {
    return frame.time_us == 40'000 && !is_black(frame.pixels);
  });
  REQUIRE(at_zero != output.end());
  REQUIRE(at_forty != output.end());
  CHECK(colored_pixels(at_forty->pixels) > colored_pixels(at_zero->pixels));
  CHECK(is_black(output.back().pixels));
}

TEST_CASE("RPM frame takes effect at the next 100 ms sample") {
  replay::ReplayClock clock;
  RecordingPixels pixels{clock};
  replay::LocalArgbOutputStage stage{pixels};
  const auto result =
      replay::run_replay({timed_frame(0, 0x777, {0}), half_rpm(1)}, clock, stage, {110'000});

  REQUIRE(result.ok());
  const auto &output = pixels.frames();
  const auto first_color = std::find_if(output.begin(), output.end(), [](const OutputFrame &frame) {
    return !is_black(frame.pixels);
  });
  REQUIRE(first_color != output.end());
  CHECK(first_color->time_us == 100'000);
  CHECK(colored_pixels(first_color->pixels) == 50);
}

TEST_CASE("equal-time rows precede timeout, poll, and output tick in source order") {
  replay::ReplayClock clock;
  RecordingPixels pixels{clock};
  replay::LocalArgbOutputStage stage{pixels};
  RecordingEvents events;
  const auto result =
      replay::run_replay({left_turn(0), turn_off(0)}, clock, stage, {10'000}, &events);

  REQUIRE(result.ok());
  REQUIRE(events.events().size() == 7);
  CHECK(events.events()[0].kind == replay::ReplayEventKind::Frame);
  CHECK(events.events()[1].kind == replay::ReplayEventKind::Frame);
  CHECK(events.events()[2].kind == replay::ReplayEventKind::EndOfStream);
  CHECK(events.events()[3].kind == replay::ReplayEventKind::Poll);
  CHECK(events.events()[4].kind == replay::ReplayEventKind::OutputTick);
  CHECK(events.events()[5].kind == replay::ReplayEventKind::Timeout);
  CHECK(events.events()[6].kind == replay::ReplayEventKind::OutputTick);
  CHECK(events.events()[0].time_us == 0);
  CHECK(events.events()[5].time_us == 10'000);
  CHECK(result.frames_delivered == 2);
}

TEST_CASE("frame, EOF, timeout, poll, and output tick ties follow the documented order") {
  replay::ReplayClock clock;
  RecordingPixels pixels{clock};
  replay::LocalArgbOutputStage stage{pixels};
  RecordingEvents events;
  const replay::ReplayScheduleOptions options{10'000, 10'000, 10'000, 10'000};

  const auto result =
      replay::run_replay({timed_frame(0, 0x777, {0}), left_turn(10'000), turn_off(10'000)}, clock,
                         stage, options, &events);

  REQUIRE(result.ok());
  REQUIRE(events.events().size() == 9);
  CHECK(events.events()[3].time_us == 10'000);
  CHECK(events.events()[3].kind == replay::ReplayEventKind::Frame);
  CHECK(events.events()[4].kind == replay::ReplayEventKind::Frame);
  CHECK(events.events()[5].kind == replay::ReplayEventKind::EndOfStream);
  CHECK(events.events()[6].kind == replay::ReplayEventKind::Timeout);
  CHECK(events.events()[7].kind == replay::ReplayEventKind::Poll);
  CHECK(events.events()[8].kind == replay::ReplayEventKind::OutputTick);
}

TEST_CASE("repeated replay produces identical event and pixel traces") {
  const std::vector<gvret::TimedCanFrame> input{left_turn(0), half_rpm(20'000), turn_off(40'000)};
  replay::ReplayClock first_clock;
  replay::ReplayClock second_clock;
  RecordingPixels first_pixels{first_clock};
  replay::LocalArgbOutputStage first_stage{first_pixels};
  RecordingPixels second_pixels{second_clock};
  replay::LocalArgbOutputStage second_stage{second_pixels};
  RecordingEvents first_events;
  RecordingEvents second_events;

  const auto first = replay::run_replay(input, first_clock, first_stage, {120'000}, &first_events);
  const auto second =
      replay::run_replay(input, second_clock, second_stage, {120'000}, &second_events);

  REQUIRE(first.ok());
  REQUIRE(second.ok());
  REQUIRE(first_events.events().size() == second_events.events().size());
  for (std::size_t i = 0; i < first_events.events().size(); ++i) {
    CHECK(first_events.events()[i].time_us == second_events.events()[i].time_us);
    CHECK(first_events.events()[i].kind == second_events.events()[i].kind);
  }
  REQUIRE(first_pixels.frames().size() == second_pixels.frames().size());
  for (std::size_t i = 0; i < first_pixels.frames().size(); ++i) {
    CHECK(first_pixels.frames()[i].time_us == second_pixels.frames()[i].time_us);
    CHECK(first_pixels.frames()[i].pixels == second_pixels.frames()[i].pixels);
  }
}

TEST_CASE("timeout publications stale a sparse turn after EOF") {
  replay::ReplayClock clock;
  RecordingPixels pixels{clock};
  replay::LocalArgbOutputStage stage{pixels};
  const auto result = replay::run_replay({left_turn(0)}, clock, stage, {260'000});

  REQUIRE(result.ok());
  CHECK(result.timeout_publications == 26);
  CHECK(result.end_of_stream);
  CHECK(is_black(pixels.frames().back().pixels));
  const auto last_color =
      std::find_if(pixels.frames().rbegin(), pixels.frames().rend(),
                   [](const OutputFrame &frame) { return !is_black(frame.pixels); });
  REQUIRE(last_color != pixels.frames().rend());
  CHECK(last_color->time_us < 260'000);
}

TEST_CASE("empty replay reports EOF and runs inclusive tail cadence") {
  replay::ReplayClock clock;
  RecordingPixels pixels{clock};
  replay::LocalArgbOutputStage stage{pixels};
  RecordingEvents events;
  const auto result = replay::run_replay({}, clock, stage, {20'000}, &events);

  REQUIRE(result.ok());
  CHECK(result.end_of_stream);
  CHECK(result.frames_delivered == 0);
  CHECK(result.timeout_publications == 2);
  CHECK(result.polled_samples == 1);
  CHECK(result.output_ticks == 3);
  CHECK(clock.now() == 20'000);
  CHECK(events.events().front().kind == replay::ReplayEventKind::EndOfStream);
  CHECK(is_black(pixels.frames().back().pixels));
}

TEST_CASE("default cadences follow the firmware poll and local ARGB output stage") {
  const replay::ReplayScheduleOptions options{};

  CHECK(options.availability_period_us == mazda::kDefaultAvailabilityServiceTargetUs);
  CHECK(options.poll_period_us == controller_config::kPolledRuleSamplePeriodUs);
  CHECK_FALSE(options.output_tick_period_us.has_value());

  replay::ReplayClock clock;
  RecordingPixels pixels{clock};
  const replay::LocalArgbOutputStage stage{pixels};
  CHECK(stage.tick_period_us() == local_argb::kSupervisorPollUs);
  CHECK(stage.tick_period_us() == 10'000);
}

TEST_CASE("an end between cadence ticks still ends at the exact replay horizon") {
  replay::ReplayClock clock;
  RecordingPixels pixels{clock};
  replay::LocalArgbOutputStage stage{pixels};

  const auto result = replay::run_replay({}, clock, stage, {10'001});

  CHECK(result.ok());
  CHECK(result.timeout_publications == 1);
  CHECK(result.output_ticks == 2);
  CHECK(clock.now() == 10'001);
}

TEST_CASE("invalid input and options fail before touching the sink or clock") {
  const auto check_invalid = [](std::vector<gvret::TimedCanFrame> input,
                                const replay::ReplayScheduleOptions options,
                                const replay::ReplayScheduleStatus expected) {
    replay::ReplayClock clock;
    RecordingPixels pixels{clock};
    replay::LocalArgbOutputStage stage{pixels};
    const auto result = replay::run_replay(std::move(input), clock, stage, options);
    CHECK(result.status == expected);
    CHECK(pixels.frames().empty());
    CHECK(clock.now() == 0);
  };

  check_invalid({left_turn(0)}, {0, 0}, replay::ReplayScheduleStatus::InvalidOptions);
  check_invalid({left_turn(0)}, {0, 10'000, 0}, replay::ReplayScheduleStatus::InvalidOptions);
  check_invalid({left_turn(0)}, {0, 10'000, 10'000, 0},
                replay::ReplayScheduleStatus::InvalidOptions);
  check_invalid({left_turn(0)}, {std::numeric_limits<std::uint64_t>::max(), 1, 1, 1},
                replay::ReplayScheduleStatus::InvalidOptions);
  check_invalid({left_turn(0), left_turn(10)}, {9}, replay::ReplayScheduleStatus::InvalidOptions);
  check_invalid({left_turn(10)}, {10}, replay::ReplayScheduleStatus::InvalidInput);
  check_invalid({left_turn(0), left_turn(5), left_turn(4)}, {10},
                replay::ReplayScheduleStatus::InvalidInput);
  auto mismatched = left_turn(0);
  mismatched.frame.timestamp_us = 1;
  check_invalid({mismatched}, {0}, replay::ReplayScheduleStatus::InvalidInput);

  replay::ReplayClock nonzero_clock{1};
  RecordingPixels pixels{nonzero_clock};
  replay::LocalArgbOutputStage stage{pixels};
  const auto nonzero = replay::run_replay({}, nonzero_clock, stage, {1});
  CHECK(nonzero.status == replay::ReplayScheduleStatus::InvalidOptions);
  CHECK(pixels.frames().empty());
  CHECK(nonzero_clock.now() == 1);
}

TEST_CASE("renderer failure stops and attempts a final black frame") {
  replay::ReplayClock clock;
  RecordingPixels pixels{clock};
  replay::LocalArgbOutputStage stage{pixels};
  pixels.fail_write_number(2);

  const auto result = replay::run_replay({left_turn(0)}, clock, stage, {10'000});

  CHECK(result.status == replay::ReplayScheduleStatus::ControllerFailure);
  CHECK(result.controller_status == replay::ReplayControllerStatus::OutputFault);
  CHECK(result.stop_status == replay::ReplayControllerStatus::Ok);
  REQUIRE_FALSE(pixels.frames().empty());
  CHECK(is_black(pixels.frames().back().pixels));
}

TEST_CASE("maximum timestamp with sparse cadence completes without deadline wrap") {
  replay::ReplayClock clock;
  RecordingPixels pixels{clock};
  replay::LocalArgbOutputStage stage{pixels};
  const auto largest = std::numeric_limits<std::uint64_t>::max();
  const replay::ReplayScheduleOptions options{largest, largest, largest, largest};

  const auto result = replay::run_replay({}, clock, stage, options);

  CHECK(result.ok());
  CHECK(clock.now() == largest);
  CHECK(result.timeout_publications == 1);
  CHECK(result.polled_samples == 2);
  CHECK(result.output_ticks == 2);
}
