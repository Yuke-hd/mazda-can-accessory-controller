#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>
#include <vector>

#include "local_argb/local_argb.h"
#include "mazda/freshness.hpp"
#include "replay/local_argb_stage.hpp"
#include "replay/pixel_frame_output.hpp"
#include "replay/scheduler.hpp"

namespace {

constexpr std::uint32_t kEngineDataId = 0x202;
constexpr std::uint32_t kTurnSwitchId = 0x091;
constexpr local_argb::Rgb kCyan{0, 16, 16};
constexpr local_argb::Rgb kHalfCyan{0, 8, 8};
constexpr local_argb::Rgb kAmber{128, 16, 0};
constexpr local_argb::Rgb kRed{16, 0, 0};

class GroupedPunct final : public std::numpunct<char> {
protected:
  char do_thousands_sep() const override { return ','; }
  std::string do_grouping() const override { return "\3"; }
};

gvret::TimedCanFrame timed_frame(const vehicle_core::MonotonicTimestamp timestamp_us,
                                 const std::uint32_t identifier,
                                 const std::initializer_list<std::uint8_t> bytes) {
  gvret::TimedCanFrame timed{};
  timed.relative_time_us = timestamp_us;
  timed.frame.timestamp_us = timestamp_us;
  timed.frame.identifier = identifier;
  timed.frame.dlc = static_cast<std::uint8_t>(bytes.size());
  std::copy(bytes.begin(), bytes.end(), timed.frame.data.begin());
  return timed;
}

gvret::TimedCanFrame rpm(const vehicle_core::MonotonicTimestamp timestamp_us,
                         const std::uint16_t raw_rpm_times_four) {
  return timed_frame(timestamp_us, kEngineDataId,
                     {static_cast<std::uint8_t>(raw_rpm_times_four >> 8),
                      static_cast<std::uint8_t>(raw_rpm_times_four & 0xff), 0, 0, 0, 0, 0, 0});
}

gvret::TimedCanFrame turn(const vehicle_core::MonotonicTimestamp timestamp_us,
                          const std::uint8_t state) {
  return timed_frame(timestamp_us, kTurnSwitchId, {0, state, 0, 0, 0, 0, 0, 0});
}

const replay::TimestampedPixelFrame &
frame_at(const std::vector<replay::TimestampedPixelFrame> &frames,
         const vehicle_core::MonotonicTimestamp timestamp_us) {
  const auto found =
      std::find_if(frames.rbegin(), frames.rend(), [timestamp_us](const auto &frame) {
        return frame.timestamp_us == timestamp_us;
      });
  REQUIRE(found != frames.rend());
  return *found;
}

std::size_t count_color(const local_argb::PixelFrame &frame, const local_argb::Rgb color) {
  return static_cast<std::size_t>(std::count(frame.begin(), frame.end(), color));
}

bool is_black(const local_argb::PixelFrame &frame) {
  return std::all_of(frame.begin(), frame.end(),
                     [](const auto pixel) { return pixel == local_argb::kBlack; });
}

} // namespace

TEST_CASE("timestamped sink records relative time and exactly 100 RGB pixels") {
  static_assert(local_argb::kLedCount == 100);
  replay::ReplayClock clock;
  replay::TimestampedPixelFrameSink pixels{clock};
  replay::LocalArgbOutputStage stage{pixels, clock};

  const auto result =
      replay::run_replay({rpm(0, 6500), rpm(100'000, 13'000)}, clock, stage, {110'000});

  REQUIRE(result.ok());
  REQUIRE_FALSE(pixels.frames().empty());
  CHECK(pixels.frames().front().timestamp_us == 0);
  for (const auto &output : pixels.frames()) {
    CHECK(output.timestamp_us <= 110'000);
    CHECK(output.pixels.size() == 100);
  }
}

TEST_CASE("synthetic replay covers production RPM, turn, hazard, priority, and baseline output") {
  replay::ReplayClock clock;
  replay::TimestampedPixelFrameSink pixels{clock};
  replay::LocalArgbOutputStage stage{pixels, clock};
  const std::vector<gvret::TimedCanFrame> input{
      rpm(0, 6'500),        // 1,625 RPM: 24 full cyan + two half-bright pixels.
      rpm(50'000, 10'000),  // 2,500 RPM: intermediate ramp point.
      rpm(100'000, 13'000), // 3,250 RPM: half fill.
      rpm(200'000, 30'000), // 7,500 RPM: clamped full fill and red zone.
      turn(300'000, 0x20),  // Vehicle left maps to strip right.
      turn(400'000, 0x10),  // Vehicle right maps to strip left.
      turn(500'000, 0x04),  // Hazard owns both turn regions.
      turn(600'000, 0x00),  // Turn off, returning to the RPM baseline.
  };

  replay::ReplayScheduleOptions options{};
  options.end_time_us = 610'000;
  options.poll_period_us = 50'000;
  const auto result = replay::run_replay(input, clock, stage, options);
  REQUIRE(result.ok());
  REQUIRE_FALSE(pixels.frames().empty());
  CHECK(is_black(pixels.frames().front().pixels));

  const auto &low = frame_at(pixels.frames(), 0).pixels;
  CHECK(count_color(low, kCyan) == 24);
  CHECK(count_color(low, kHalfCyan) == 2);
  CHECK(std::all_of(low.begin() + 38, low.begin() + 62,
                    [](const auto pixel) { return pixel == kCyan; }));
  CHECK(low[37] == kHalfCyan);
  CHECK(low[62] == kHalfCyan);
  CHECK(std::all_of(low.begin(), low.begin() + 37,
                    [](const auto pixel) { return pixel == local_argb::kBlack; }));
  CHECK(std::all_of(low.begin() + 63, low.end(),
                    [](const auto pixel) { return pixel == local_argb::kBlack; }));

  const auto &half = frame_at(pixels.frames(), 100'000).pixels;
  CHECK(count_color(half, kCyan) == 50);
  CHECK(std::all_of(half.begin() + 25, half.begin() + 75,
                    [](const auto pixel) { return pixel == kCyan; }));
  CHECK(std::all_of(half.begin(), half.begin() + 25,
                    [](const auto pixel) { return pixel == local_argb::kBlack; }));
  CHECK(std::all_of(half.begin() + 75, half.end(),
                    [](const auto pixel) { return pixel == local_argb::kBlack; }));

  const auto &ramp = frame_at(pixels.frames(), 50'000).pixels;
  CHECK(count_color(ramp, kCyan) > count_color(low, kCyan));
  CHECK(count_color(ramp, kCyan) < count_color(half, kCyan));

  const auto &full = frame_at(pixels.frames(), 200'000).pixels;
  CHECK(std::all_of(full.begin(), full.begin() + 35,
                    [](const auto pixel) { return pixel == kCyan; }));
  CHECK(std::all_of(full.begin() + 35, full.begin() + 65,
                    [](const auto pixel) { return pixel == kRed; }));
  CHECK(
      std::all_of(full.begin() + 65, full.end(), [](const auto pixel) { return pixel == kCyan; }));

  const auto &left = frame_at(pixels.frames(), 300'000).pixels;
  CHECK(left[65] == kAmber);
  CHECK(left[66] == local_argb::kBlack);
  CHECK(std::all_of(left.begin(), left.begin() + 35,
                    [](const auto pixel) { return pixel == kCyan; }));
  CHECK(std::all_of(left.begin() + 66, left.end(),
                    [](const auto pixel) { return pixel == local_argb::kBlack; }));
  CHECK(std::all_of(left.begin() + 35, left.begin() + 65,
                    [](const auto pixel) { return pixel == kRed; }));

  const auto &right = frame_at(pixels.frames(), 400'000).pixels;
  CHECK(right[34] == kAmber);
  CHECK(right[33] == local_argb::kBlack);
  CHECK(std::all_of(right.begin(), right.begin() + 34,
                    [](const auto pixel) { return pixel == local_argb::kBlack; }));
  CHECK(std::all_of(right.begin() + 65, right.end(),
                    [](const auto pixel) { return pixel == kCyan; }));

  const auto &hazard = frame_at(pixels.frames(), 500'000).pixels;
  CHECK(hazard[34] == kAmber);
  CHECK(hazard[65] == kAmber);
  CHECK(std::all_of(hazard.begin() + 35, hazard.begin() + 65,
                    [](const auto pixel) { return pixel == kRed; }));

  const auto &baseline = frame_at(pixels.frames(), 600'000).pixels;
  CHECK(baseline == full);
}

TEST_CASE("turn-only stale replay fails off after the inclusive freshness boundary") {
  replay::ReplayClock clock;
  replay::TimestampedPixelFrameSink pixels{clock};
  replay::LocalArgbOutputStage stage{pixels, clock};

  constexpr auto kStaleCheckTimeUs = mazda::kTurnFreshnessTimeoutUs + local_argb::kSupervisorPollUs;
  const auto result = replay::run_replay({turn(0, 0x20)}, clock, stage, {kStaleCheckTimeUs});

  REQUIRE(result.ok());
  CHECK(is_black(frame_at(pixels.frames(), kStaleCheckTimeUs).pixels));
}

TEST_CASE("transport silence leaves unavailable RPM output black") {
  replay::ReplayClock clock;
  replay::TimestampedPixelFrameSink pixels{clock};
  replay::LocalArgbOutputStage stage{pixels, clock};

  const auto result = replay::run_replay({}, clock, stage, {1'000'000});

  REQUIRE(result.ok());
  CHECK(result.end_of_stream);
  CHECK(result.polled_samples == 11);
  REQUIRE_FALSE(pixels.frames().empty());
  CHECK(std::all_of(pixels.frames().begin(), pixels.frames().end(),
                    [](const auto &output) { return is_black(output.pixels); }));
}

TEST_CASE("repeated synthetic replay produces byte-identical JSONL output") {
  const std::vector<gvret::TimedCanFrame> input{rpm(0, 6'500), rpm(100'000, 13'000),
                                                turn(200'000, 0x20), turn(300'000, 0)};
  std::ostringstream first_stream;
  std::ostringstream second_stream;
  replay::ReplayClock first_clock;
  replay::ReplayClock second_clock;
  replay::JsonlPixelFrameSink first_pixels{first_clock, first_stream};
  replay::LocalArgbOutputStage first_stage{first_pixels, first_clock};
  replay::JsonlPixelFrameSink second_pixels{second_clock, second_stream};
  replay::LocalArgbOutputStage second_stage{second_pixels, second_clock};

  REQUIRE(replay::run_replay(input, first_clock, first_stage, {310'000}).ok());
  REQUIRE(replay::run_replay(input, second_clock, second_stage, {310'000}).ok());

  CHECK(first_pixels.good());
  CHECK(second_pixels.good());
  CHECK(first_stream.str() == second_stream.str());
  CHECK(first_stream.str().find("timestamp_us") != std::string::npos);
  CHECK(first_stream.str().find("pixels") != std::string::npos);
  CHECK(first_stream.str().find("0x202") == std::string::npos);
  CHECK(first_stream.str().find("0x091") == std::string::npos);
}

TEST_CASE("JSONL serialization ignores caller stream flags and locale") {
  replay::ReplayClock clock;
  std::ostringstream output;
  output.imbue(std::locale(output.getloc(), new GroupedPunct));
  output << std::hex << std::showbase << std::setfill('x') << std::setw(8);
  const auto flags = output.flags();
  const auto fill = output.fill();
  const auto width = output.width();
  replay::JsonlPixelFrameSink pixels{clock, output};
  replay::LocalArgbOutputStage stage{pixels, clock};
  local_argb::PixelFrame frame = local_argb::kBlackFrame;
  frame[0] = {128, 16, 16};

  REQUIRE(pixels.write(frame));
  CHECK(pixels.good());
  CHECK(output.flags() == flags);
  CHECK(output.fill() == fill);
  CHECK(output.width() == width);
  const std::string serialized = output.str();
  CHECK(serialized.find("\"timestamp_us\":0") != std::string::npos);
  CHECK(serialized.find("[128,16,16]") != std::string::npos);
  CHECK(serialized.find("0x") == std::string::npos);
  CHECK(serialized.find("x0") == std::string::npos);
  CHECK(std::count(serialized.begin(), serialized.end(), '[') == 101);
}

TEST_CASE("JSONL stream starts with one header and types every record") {
  replay::ReplayClock clock;
  std::ostringstream output;
  replay::JsonlPixelFrameSink pixels{clock, output};
  replay::LocalArgbOutputStage stage{pixels, clock};

  REQUIRE(pixels.write_header());
  REQUIRE(pixels.write_header());
  REQUIRE(pixels.write(local_argb::kBlackFrame));
  REQUIRE(pixels.write(local_argb::kBlackFrame));

  std::istringstream lines{output.str()};
  std::string line;
  REQUIRE(std::getline(lines, line));
  CHECK(line == "{\"type\":\"header\",\"version\":1,\"pixel_count\":100}");
  std::size_t pixel_records = 0;
  while (std::getline(lines, line)) {
    CHECK(line.rfind("{\"type\":\"pixels\",\"timestamp_us\":0,\"pixels\":[", 0) == 0);
    CHECK(std::count(line.begin(), line.end(), '[') == 101);
    ++pixel_records;
  }
  CHECK(pixel_records == 2);
}

TEST_CASE("first pixel write emits the header implicitly") {
  replay::ReplayClock clock;
  std::ostringstream output;
  replay::JsonlPixelFrameSink pixels{clock, output};
  replay::LocalArgbOutputStage stage{pixels, clock};

  REQUIRE(pixels.write(local_argb::kBlackFrame));

  const std::string serialized = output.str();
  CHECK(serialized.rfind("{\"type\":\"header\",", 0) == 0);
  CHECK(std::count(serialized.begin(), serialized.end(), '\n') == 2);
}

TEST_CASE("JSONL stream ends with a completion marker") {
  replay::ReplayClock clock;
  std::ostringstream output;
  replay::JsonlPixelFrameSink pixels{clock, output};
  replay::LocalArgbOutputStage stage{pixels, clock};

  REQUIRE(pixels.write(local_argb::kBlackFrame));
  REQUIRE(pixels.write_end());
  CHECK(output.str().find("{\"type\":\"end\"}\n") != std::string::npos);
  CHECK(pixels.write_end());
}
