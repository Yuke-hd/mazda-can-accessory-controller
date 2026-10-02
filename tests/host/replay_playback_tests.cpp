#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <initializer_list>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "local_argb/local_argb.h"
#include "replay/local_argb_stage.hpp"
#include "replay/pixel_frame_output.hpp"
#include "replay/playback_command.hpp"
#include "replay/playback_pacer.hpp"
#include "replay/scheduler.hpp"
#include "replay/wall_clock.hpp"

namespace {

using replay::PlaybackCommand;
using replay::PlaybackCommandKind;
using replay::PlaybackRate;
using std::chrono::microseconds;
using std::chrono::milliseconds;

// A wall clock whose sleeps complete instantly by jumping to the deadline.
class FakeWallClock final : public replay::WallClock {
public:
  [[nodiscard]] microseconds now() const override { return now_; }
  void sleep_until(const microseconds deadline) override {
    ++sleeps_;
    if (deadline > now_)
      now_ = deadline;
  }
  void advance(const microseconds by) { now_ += by; }
  [[nodiscard]] std::size_t sleeps() const noexcept { return sleeps_; }

private:
  microseconds now_{1'000'000};
  std::size_t sleeps_{0};
};

struct ScriptedCommand {
  microseconds at;
  std::optional<PlaybackCommand> command; // nullopt closes the channel
};

// Delivers each scripted command once the fake wall clock reaches its time.
class ScriptedChannel final : public replay::PlaybackChannel {
public:
  explicit ScriptedChannel(const FakeWallClock &clock) : clock_(clock) {}

  void at(const microseconds wall_offset, std::optional<PlaybackCommand> command) {
    script_.push_back({start_ + wall_offset, command});
  }

  void after(const microseconds delay, std::optional<PlaybackCommand> command) {
    script_.push_back({clock_.now() + delay, command});
  }

  replay::PlaybackPoll poll() override {
    if (script_.empty() || script_.front().at > clock_.now())
      return {replay::PlaybackPollStatus::Idle, {}};
    const auto next = script_.front();
    script_.pop_front();
    if (!next.command)
      return {replay::PlaybackPollStatus::Closed, {}};
    return {replay::PlaybackPollStatus::Command, *next.command};
  }

  void report(const replay::PlaybackState state) override { reports_.push_back(state); }

  [[nodiscard]] const std::vector<replay::PlaybackState> &reports() const noexcept {
    return reports_;
  }

private:
  const FakeWallClock &clock_;
  microseconds start_{clock_.now()};
  std::deque<ScriptedCommand> script_{};
  std::vector<replay::PlaybackState> reports_{};
};

// Records the wall time at which each replay event was processed.
class WallStampedEvents final : public replay::ReplayEventSink {
public:
  explicit WallStampedEvents(const FakeWallClock &clock) : clock_(clock) {}
  void record(const replay::ReplayEvent event) noexcept override {
    events_.push_back({event, clock_.now()});
  }
  struct Entry {
    replay::ReplayEvent event;
    microseconds wall;
  };
  [[nodiscard]] const std::vector<Entry> &entries() const noexcept { return events_; }

private:
  const FakeWallClock &clock_;
  std::vector<Entry> events_{};
};

PlaybackCommand play_command() { return {PlaybackCommandKind::Play, PlaybackRate::normal()}; }
PlaybackCommand pause_command() { return {PlaybackCommandKind::Pause, PlaybackRate::normal()}; }
PlaybackCommand restart_command() { return {PlaybackCommandKind::Restart, PlaybackRate::normal()}; }
PlaybackCommand rate(const std::string_view token) {
  return {PlaybackCommandKind::SetRate, *PlaybackRate::from_token(token)};
}

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

// Left turn, half RPM, then turn off: exercises frames, polled rules, and
// renderer animation ticks.
std::vector<gvret::TimedCanFrame> turn_and_rpm_input() {
  return {
      timed_frame(0, 0x091, {0, 0x20, 0, 0, 0, 0, 0, 0}),
      timed_frame(50'000, 0x202, {0x32, 0xc8, 0, 0, 0, 0, 0, 0}),
      timed_frame(400'000, 0x091, {0, 0, 0, 0, 0, 0, 0, 0}),
  };
}

constexpr vehicle_core::MonotonicTimestamp kHorizonUs = 900'000;

struct PacedRun {
  std::string jsonl;
  replay::ReplayScheduleStatus status;
  std::vector<WallStampedEvents::Entry> events;
};

// Runs the production scheduler and JSONL sink behind a pacer.
PacedRun run_paced(FakeWallClock &wall, replay::PlaybackPacer &pacer) {
  replay::ReplayClock clock;
  std::ostringstream jsonl;
  replay::JsonlPixelFrameSink sink{clock, jsonl};
  replay::LocalArgbOutputStage stage{sink, clock};
  WallStampedEvents events{wall};
  pacer.begin_replay();
  const auto result =
      replay::run_replay(turn_and_rpm_input(), clock, stage, {kHorizonUs}, &events, &pacer);
  return {jsonl.str(), result.status, events.entries()};
}

std::string run_unpaced() {
  replay::ReplayClock clock;
  std::ostringstream jsonl;
  replay::JsonlPixelFrameSink sink{clock, jsonl};
  replay::LocalArgbOutputStage stage{sink, clock};
  REQUIRE(replay::run_replay(turn_and_rpm_input(), clock, stage, {kHorizonUs}).ok());
  return jsonl.str();
}

} // namespace

TEST_CASE("playback rates parse only the supported JSON number tokens") {
  for (const auto token : {"0.25", "0.5", "1", "2", "5"}) {
    const auto parsed = PlaybackRate::from_token(token);
    REQUIRE(parsed.has_value());
    CHECK(parsed->text() == token);
  }
  for (const auto token : {"", "0", "3", "1.0", "0.50", "-1", "1e0", "10", "0.1", " 1", "Infinity"})
    CHECK_FALSE(PlaybackRate::from_token(token).has_value());
  CHECK(PlaybackRate::normal().text() == "1");
}

TEST_CASE("a playback rate converts replay time to wall time, rounding up") {
  CHECK(PlaybackRate::normal().wall_span(100'000) == microseconds{100'000});
  CHECK(PlaybackRate::from_token("0.25")->wall_span(100'000) == microseconds{400'000});
  CHECK(PlaybackRate::from_token("0.5")->wall_span(3) == microseconds{6});
  CHECK(PlaybackRate::from_token("2")->wall_span(3) == microseconds{2});
  CHECK(PlaybackRate::from_token("5")->wall_span(100'000) == microseconds{20'000});
  CHECK(PlaybackRate::from_token("0.25")->wall_span(std::numeric_limits<std::uint64_t>::max()) ==
        microseconds::max());
  CHECK(PlaybackRate::from_token("2")->replay_span(microseconds{3}) == 6U);
  CHECK(PlaybackRate::from_token("0.25")->replay_span(microseconds{7}) == 1U);
  CHECK(PlaybackRate::from_token("5")->replay_span(microseconds::max()) ==
        std::numeric_limits<std::uint64_t>::max());
}

TEST_CASE("control messages parse into playback commands") {
  const auto parse = [](const std::string_view text) {
    return replay::parse_playback_command(text);
  };
  CHECK(parse(R"({"type":"control","command":"play"})").command->kind == PlaybackCommandKind::Play);
  CHECK(parse(R"({"type":"control","command":"pause"})").command->kind ==
        PlaybackCommandKind::Pause);
  CHECK(parse(R"( { "command" : "restart" , "type" : "control" } )").command->kind ==
        PlaybackCommandKind::Restart);
  const auto set_rate = parse(R"({"type":"control","command":"rate","rate":0.25})");
  REQUIRE(set_rate.command.has_value());
  CHECK(set_rate.command->kind == PlaybackCommandKind::SetRate);
  CHECK(set_rate.command->rate.text() == "0.25");
}

TEST_CASE("invalid control messages are rejected with a reason") {
  const auto reason = [](const std::string_view text) {
    const auto parsed = replay::parse_playback_command(text);
    CHECK_FALSE(parsed.command.has_value());
    return std::string{parsed.rejection};
  };
  CHECK(reason("") == "malformed");
  CHECK(reason("not json") == "malformed");
  CHECK(reason(R"({"type":"control","command":"play"} trailing)") == "malformed");
  CHECK(reason(R"({"type":"control","command":"play",})") == "malformed");
  CHECK(reason(R"({"type":"control","command":"pl\u0061y"})") == "malformed");
  CHECK(reason(R"({"type":"control","type":"control","command":"pl\u0061y"})") == "malformed");
  CHECK(reason(R"({"type":"control","command":"play","extra":1})") == "malformed");
  CHECK(reason(R"({"type":"control","command":"play","rate":1})") == "malformed");
  CHECK(reason(R"({"type":"control","command":"rate"})") == "malformed");
  CHECK(reason(R"({"type":"control","command":"rate","rate":"1"})") == "malformed");
  CHECK(reason(R"({"type":"control","command":{"x":1}})") == "malformed");
  CHECK(reason(R"({"type":"control","command":"play"}{})") == "malformed");
  CHECK(reason(R"({"type":"pixels","command":"play"})") == "unknown_type");
  CHECK(reason(R"({"command":"play"})") == "unknown_type");
  CHECK(reason(R"({"type":"control","command":"seek"})") == "unknown_command");
  CHECK(reason(R"({"type":"control"})") == "unknown_command");
  CHECK(reason(R"({"type":"control","command":"rate","rate":3})") == "unsupported_rate");
  CHECK(reason(R"({"type":"control","command":"rate","rate":1.0})") == "unsupported_rate");
  CHECK(reason(std::string(replay::kMaxControlMessageBytes + 1, ' ')) == "too_long");
}

TEST_CASE("the pacer releases each replay time after its wall-time equivalent") {
  for (const auto token : {"0.25", "0.5", "1", "2", "5"}) {
    CAPTURE(token);
    FakeWallClock wall;
    ScriptedChannel channel{wall};
    replay::PlaybackPacer pacer{wall, channel};
    channel.at(microseconds{0}, rate(token));
    const auto start = wall.now();
    const auto run = run_paced(wall, pacer);
    REQUIRE(run.status == replay::ReplayScheduleStatus::Ok);
    const auto chosen = *PlaybackRate::from_token(token);
    for (const auto &entry : run.events)
      CHECK(entry.wall - start == chosen.wall_span(entry.event.time_us));
    CHECK(wall.now() - start == chosen.wall_span(kHorizonUs));
  }
}

TEST_CASE("paced output is byte-identical to unpaced output at every rate") {
  const auto expected = run_unpaced();
  for (const auto token : {"0.25", "0.5", "1", "2", "5"}) {
    CAPTURE(token);
    FakeWallClock wall;
    ScriptedChannel channel{wall};
    replay::PlaybackPacer pacer{wall, channel};
    channel.at(microseconds{0}, rate(token));
    CHECK(run_paced(wall, pacer).jsonl == expected);
  }
}

TEST_CASE("pause freezes replay time and resume continues from the same position") {
  FakeWallClock wall;
  ScriptedChannel channel{wall};
  replay::PlaybackPacer pacer{wall, channel};
  channel.at(milliseconds{230}, pause_command());
  channel.at(milliseconds{5'230}, play_command());
  const auto start = wall.now();
  const auto run = run_paced(wall, pacer);

  REQUIRE(run.status == replay::ReplayScheduleStatus::Ok);
  CHECK(run.jsonl == run_unpaced());
  bool saw_before = false;
  bool saw_after = false;
  for (const auto &entry : run.events) {
    const auto wall_offset = entry.wall - start;
    // Nothing is processed while paused.
    CHECK((wall_offset <= milliseconds{230} || wall_offset >= milliseconds{5'230}));
    if (wall_offset <= milliseconds{230})
      saw_before = true;
    if (wall_offset < milliseconds{5'230})
      continue;
    saw_after = true;
    // Replay time resumes at 230 ms, so it now lags wall time by the pause.
    CHECK(wall_offset == microseconds{entry.event.time_us} + milliseconds{5'000});
  }
  CHECK(saw_before);
  CHECK(saw_after);
  CHECK(wall.now() - start == microseconds{kHorizonUs} + milliseconds{5'000});
  REQUIRE(channel.reports().size() == 3);
  CHECK_FALSE(channel.reports()[0].paused);
  CHECK(channel.reports()[1].paused);
  CHECK_FALSE(channel.reports()[2].paused);
}

TEST_CASE("a rate change keeps the replay position and changes only later pacing") {
  FakeWallClock wall;
  ScriptedChannel channel{wall};
  replay::PlaybackPacer pacer{wall, channel};
  channel.at(milliseconds{100}, rate("5"));
  const auto start = wall.now();
  const auto run = run_paced(wall, pacer);

  REQUIRE(run.status == replay::ReplayScheduleStatus::Ok);
  CHECK(run.jsonl == run_unpaced());
  // 100 ms at 1x, then the remaining 800 ms of replay time at 5x.
  CHECK(wall.now() - start == milliseconds{100} + milliseconds{160});
  REQUIRE(channel.reports().size() == 2);
  CHECK(channel.reports()[1].rate.text() == "5");
}

TEST_CASE("restart interrupts the replay and closing the channel stops it") {
  FakeWallClock wall;
  ScriptedChannel channel{wall};
  replay::PlaybackPacer pacer{wall, channel};
  channel.at(milliseconds{300}, rate("2"));
  channel.at(milliseconds{310}, pause_command());
  channel.at(milliseconds{320}, restart_command());
  const auto interrupted = run_paced(wall, pacer);
  CHECK(interrupted.status == replay::ReplayScheduleStatus::Interrupted);
  CHECK(pacer.interruption() == replay::PlaybackInterruption::Restart);

  // Restart keeps the rate and starts playing from replay time zero.
  const auto start = wall.now();
  const auto replayed = run_paced(wall, pacer);
  CHECK(replayed.status == replay::ReplayScheduleStatus::Ok);
  CHECK(replayed.jsonl == run_unpaced());
  CHECK(wall.now() - start == PlaybackRate::from_token("2")->wall_span(kHorizonUs));
  CHECK_FALSE(channel.reports().back().paused);
  CHECK(channel.reports().back().rate.text() == "2");

  channel.after(milliseconds{50}, std::nullopt);
  CHECK(run_paced(wall, pacer).status == replay::ReplayScheduleStatus::Interrupted);
  CHECK(pacer.interruption() == replay::PlaybackInterruption::Stop);
}

TEST_CASE("after a replay ends the pacer waits for restart or close") {
  FakeWallClock wall;
  ScriptedChannel channel{wall};
  replay::PlaybackPacer pacer{wall, channel};
  channel.at(milliseconds{2'000}, pause_command());
  channel.at(milliseconds{3'000}, restart_command());
  channel.at(milliseconds{9'000}, std::nullopt);

  REQUIRE(run_paced(wall, pacer).status == replay::ReplayScheduleStatus::Ok);
  CHECK(pacer.await_restart());
  CHECK(channel.reports().back().paused);
  REQUIRE(run_paced(wall, pacer).status == replay::ReplayScheduleStatus::Ok);
  CHECK_FALSE(pacer.await_restart());
  CHECK(pacer.interruption() == replay::PlaybackInterruption::Stop);
}

TEST_CASE("the pacer never sleeps longer than one command slice") {
  FakeWallClock wall;
  ScriptedChannel channel{wall};
  replay::PlaybackPacer pacer{wall, channel};
  channel.at(microseconds{0}, rate("0.25"));
  const auto start = wall.now();
  REQUIRE(run_paced(wall, pacer).status == replay::ReplayScheduleStatus::Ok);
  const auto elapsed = wall.now() - start;
  CHECK(wall.sleeps() >= static_cast<std::size_t>(elapsed / replay::kPlaybackCommandSlice));
}
