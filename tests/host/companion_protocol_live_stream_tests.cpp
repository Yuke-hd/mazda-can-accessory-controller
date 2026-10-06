#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "companion_protocol/live_signal_stream.hpp"
#include "companion_protocol/live_signals.hpp"
#include "support/fake_signal_provider.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

using namespace companion_protocol;
using std::chrono::milliseconds;

// Records every frame offered to the BLE stack; `accept` decides whether the
// stack could queue it.
class RecordingSink final : public LiveFrameSink {
public:
  bool try_queue(const LiveFrame &frame) noexcept override {
    frames.push_back(frame);
    return accept;
  }

  bool accept{true};
  std::vector<LiveFrame> frames{};
};

// Two distinguishable sampling passes: the flags byte is the only difference.
struct Contents {
  test_support::FakeSignalProvider provider{vehicle_signals::SignalCatalogView{}};
  LiveSignalSampler sampler{provider};
  LiveSignalContent quiet{sampler.sample(false)};
  LiveSignalContent changed{sampler.sample(true)};
};

constexpr std::size_t kSequence = 1;
constexpr std::size_t kFlags = 2;

void make_streamable(LiveSignalStream &stream) {
  stream.set_att_mtu(kMinLiveSignalsAttMtu);
  stream.set_link_secured(true);
  stream.set_notifications_enabled(true);
}

// Offers `content` whenever a sample is due between `from` and `to`, in 1 ms
// steps, and returns the number of attempts.
std::size_t run(LiveSignalStream &stream, const LiveSignalContent &content, RecordingSink &sink,
                const milliseconds from, const milliseconds to) {
  std::size_t attempts = 0;
  for (milliseconds now = from; now <= to; now += milliseconds{1}) {
    const auto wait = stream.time_until_sample(now);
    if (wait.has_value() && *wait == milliseconds{0} && stream.offer(now, content, sink))
      ++attempts;
  }
  return attempts;
}

TEST_CASE("frames stream only while notifications, a secured link and MTU 64 all hold") {
  const Contents contents{};
  LiveSignalStream stream{};
  RecordingSink sink{};
  CHECK_FALSE(stream.streaming());
  CHECK_FALSE(stream.time_until_sample(milliseconds{0}).has_value());
  CHECK_FALSE(stream.offer(milliseconds{0}, contents.quiet, sink));

  stream.set_notifications_enabled(true);
  stream.set_link_secured(true);
  stream.set_att_mtu(kMinLiveSignalsAttMtu - 1);
  CHECK_FALSE(stream.streaming());
  stream.set_att_mtu(kMinLiveSignalsAttMtu);
  CHECK(stream.streaming());

  stream.set_link_secured(false);
  CHECK_FALSE(stream.streaming());
  stream.set_link_secured(true);
  stream.set_notifications_enabled(false);
  CHECK_FALSE(stream.streaming());
  CHECK_FALSE(stream.offer(milliseconds{5000}, contents.quiet, sink));
  CHECK(sink.frames.empty());
}

TEST_CASE("the first frame goes out at once with sequence 0") {
  const Contents contents{};
  LiveSignalStream stream{};
  RecordingSink sink{};
  make_streamable(stream);
  REQUIRE(stream.time_until_sample(milliseconds{50}) == milliseconds{0});
  CHECK(stream.offer(milliseconds{50}, contents.quiet, sink));
  REQUIRE(sink.frames.size() == 1);
  CHECK(sink.frames[0] == contents.quiet.frame(0));
}

TEST_CASE("frames are never closer than 100 ms, however often content changes") {
  const Contents contents{};
  LiveSignalStream stream{};
  RecordingSink sink{};
  make_streamable(stream);
  std::vector<milliseconds> sent_at{};
  for (milliseconds now{0}; now <= milliseconds{2000}; now += milliseconds{1}) {
    const auto &content = ((now.count() / 100) % 2 == 0) ? contents.quiet : contents.changed;
    // Offering without waiting must still respect the cap.
    if (stream.offer(now, content, sink))
      sent_at.push_back(now);
  }
  REQUIRE(sent_at.size() == 21);
  for (std::size_t index = 1; index < sent_at.size(); ++index)
    CHECK(sent_at[index] - sent_at[index - 1] >= kLiveFrameInterval);
}

TEST_CASE("time_until_sample reports the remaining rate-cap wait") {
  const Contents contents{};
  LiveSignalStream stream{};
  RecordingSink sink{};
  make_streamable(stream);
  REQUIRE(stream.offer(milliseconds{1000}, contents.quiet, sink));
  CHECK(stream.time_until_sample(milliseconds{1030}) == milliseconds{70});
  CHECK(stream.time_until_sample(milliseconds{1100}) == milliseconds{0});
  // A clock that steps back never stalls the stream.
  CHECK(stream.time_until_sample(milliseconds{900}) == milliseconds{0});
}

TEST_CASE("unchanged content is sent only as a 1 s heartbeat") {
  const Contents contents{};
  LiveSignalStream stream{};
  RecordingSink sink{};
  make_streamable(stream);
  CHECK(run(stream, contents.quiet, sink, milliseconds{0}, milliseconds{999}) == 1);
  CHECK(run(stream, contents.quiet, sink, milliseconds{1000}, milliseconds{1000}) == 1);
  CHECK(run(stream, contents.quiet, sink, milliseconds{1001}, milliseconds{3000}) == 2);
  REQUIRE(sink.frames.size() == 4);
  for (std::size_t index = 0; index < sink.frames.size(); ++index)
    CHECK(sink.frames[index][kSequence] == index);
}

TEST_CASE("a changed sample is sent at the next 100 ms slot") {
  const Contents contents{};
  LiveSignalStream stream{};
  RecordingSink sink{};
  make_streamable(stream);
  REQUIRE(stream.offer(milliseconds{0}, contents.quiet, sink));
  CHECK_FALSE(stream.offer(milliseconds{100}, contents.quiet, sink));
  CHECK_FALSE(stream.offer(milliseconds{150}, contents.changed, sink));
  CHECK(stream.time_until_sample(milliseconds{150}) == milliseconds{50});
  CHECK(stream.offer(milliseconds{200}, contents.changed, sink));
  REQUIRE(sink.frames.size() == 2);
  CHECK(sink.frames[1] == contents.changed.frame(1));
}

TEST_CASE("a frame the stack could not queue is retried with the latest content") {
  const Contents contents{};
  LiveSignalStream stream{};
  RecordingSink sink{};
  make_streamable(stream);
  sink.accept = false;
  // The first frame is retried until the stack accepts one.
  CHECK(stream.offer(milliseconds{0}, contents.quiet, sink));
  CHECK(stream.offer(milliseconds{100}, contents.quiet, sink));
  sink.accept = true;
  CHECK(stream.offer(milliseconds{200}, contents.quiet, sink));
  CHECK_FALSE(stream.offer(milliseconds{300}, contents.quiet, sink));
  // A dropped change is sent again at the next slot, as the latest state.
  sink.accept = false;
  CHECK(stream.offer(milliseconds{400}, contents.changed, sink));
  sink.accept = true;
  CHECK(stream.offer(milliseconds{500}, contents.changed, sink));
  CHECK_FALSE(stream.offer(milliseconds{600}, contents.changed, sink));
  REQUIRE(sink.frames.size() == 5);
  // The sequence counts attempts, including dropped frames.
  for (std::size_t index = 0; index < sink.frames.size(); ++index)
    CHECK(sink.frames[index][kSequence] == index);
  CHECK(sink.frames[4][kFlags] == 0x01);
}

TEST_CASE("the sequence wraps from 255 to 0") {
  const Contents contents{};
  LiveSignalStream stream{};
  RecordingSink sink{};
  make_streamable(stream);
  for (int index = 0; index < 257; ++index) {
    const auto &content = (index % 2 == 0) ? contents.quiet : contents.changed;
    REQUIRE(stream.offer(milliseconds{index * 100}, content, sink));
  }
  CHECK(sink.frames[255][kSequence] == 255);
  CHECK(sink.frames[256][kSequence] == 0);
}

TEST_CASE("re-enabling notifications restarts the sequence and resends unchanged content") {
  const Contents contents{};
  LiveSignalStream stream{};
  RecordingSink sink{};
  make_streamable(stream);
  REQUIRE(stream.offer(milliseconds{0}, contents.quiet, sink));
  REQUIRE(stream.offer(milliseconds{100}, contents.changed, sink));
  stream.set_notifications_enabled(false);
  stream.set_notifications_enabled(true);
  // The rate cap still holds across the re-enable on the same connection, so
  // the first frame waits at most 100 ms.
  CHECK(stream.time_until_sample(milliseconds{150}) == milliseconds{50});
  CHECK(stream.offer(milliseconds{200}, contents.changed, sink));
  REQUIRE(sink.frames.size() == 3);
  CHECK(sink.frames[2] == contents.changed.frame(0));
}

TEST_CASE("losing link security mid-stream stops frames until it recovers, then restarts") {
  const Contents contents{};
  LiveSignalStream stream{};
  RecordingSink sink{};
  make_streamable(stream);
  REQUIRE(stream.offer(milliseconds{0}, contents.quiet, sink));
  REQUIRE(stream.offer(milliseconds{100}, contents.changed, sink));
  stream.set_link_secured(false);
  CHECK_FALSE(stream.streaming());
  CHECK_FALSE(stream.time_until_sample(milliseconds{200}).has_value());
  CHECK_FALSE(stream.offer(milliseconds{200}, contents.quiet, sink));
  CHECK_FALSE(stream.offer(milliseconds{1500}, contents.changed, sink));
  stream.set_link_secured(true);
  // The sequence restarts at 0 and the unchanged content is sent again.
  CHECK(stream.offer(milliseconds{1600}, contents.changed, sink));
  REQUIRE(sink.frames.size() == 3);
  CHECK(sink.frames[2] == contents.changed.frame(0));
}

TEST_CASE("an ATT MTU below 64 mid-stream stops frames until it recovers, then restarts") {
  const Contents contents{};
  LiveSignalStream stream{};
  RecordingSink sink{};
  make_streamable(stream);
  REQUIRE(stream.offer(milliseconds{0}, contents.quiet, sink));
  REQUIRE(stream.offer(milliseconds{100}, contents.changed, sink));
  stream.set_att_mtu(23);
  CHECK_FALSE(stream.streaming());
  CHECK_FALSE(stream.offer(milliseconds{200}, contents.changed, sink));
  stream.set_att_mtu(kMinLiveSignalsAttMtu);
  // The rate cap still holds across the gap: the earliest frame is 100 ms
  // after the last sample.
  CHECK(stream.time_until_sample(milliseconds{150}) == milliseconds{50});
  CHECK(stream.offer(milliseconds{250}, contents.changed, sink));
  REQUIRE(sink.frames.size() == 3);
  CHECK(sink.frames[2] == contents.changed.frame(0));
}

TEST_CASE("a refused heartbeat is retried at the next heartbeat, a change at the next slot") {
  const Contents contents{};
  LiveSignalStream stream{};
  RecordingSink sink{};
  make_streamable(stream);
  REQUIRE(stream.offer(milliseconds{0}, contents.quiet, sink));
  sink.accept = false;
  // The heartbeat interval counts from the last attempt, so an unchanged
  // state whose heartbeat the stack refused waits a full interval again.
  CHECK(run(stream, contents.quiet, sink, milliseconds{1}, milliseconds{1999}) == 1);
  CHECK(sink.frames.back()[kSequence] == 1);
  sink.accept = true;
  CHECK(run(stream, contents.quiet, sink, milliseconds{2000}, milliseconds{2000}) == 1);
  REQUIRE(sink.frames.size() == 3);
  CHECK(sink.frames[2][kSequence] == 2);
  // A change after a refused heartbeat still goes out at the next slot.
  sink.accept = false;
  CHECK(run(stream, contents.quiet, sink, milliseconds{2001}, milliseconds{3000}) == 1);
  sink.accept = true;
  CHECK(stream.offer(milliseconds{3100}, contents.changed, sink));
  REQUIRE(sink.frames.size() == 5);
  CHECK(sink.frames[4] == contents.changed.frame(4));
}

TEST_CASE("a new connection clears the rate history and every condition") {
  const Contents contents{};
  LiveSignalStream stream{};
  RecordingSink sink{};
  make_streamable(stream);
  REQUIRE(stream.offer(milliseconds{0}, contents.quiet, sink));
  stream.reset_connection();
  CHECK_FALSE(stream.streaming());
  make_streamable(stream);
  CHECK(stream.time_until_sample(milliseconds{10}) == milliseconds{0});
  CHECK(stream.offer(milliseconds{10}, contents.quiet, sink));
  REQUIRE(sink.frames.size() == 2);
  CHECK(sink.frames[1] == contents.quiet.frame(0));
}

TEST_CASE("redundant condition updates do not restart the stream") {
  const Contents contents{};
  LiveSignalStream stream{};
  RecordingSink sink{};
  make_streamable(stream);
  REQUIRE(stream.offer(milliseconds{0}, contents.quiet, sink));
  // A bonded peer's restored subscription or a repeated MTU event while
  // already streaming changes nothing.
  make_streamable(stream);
  stream.set_att_mtu(247);
  CHECK_FALSE(stream.offer(milliseconds{100}, contents.quiet, sink));
  CHECK(stream.offer(milliseconds{200}, contents.changed, sink));
  REQUIRE(sink.frames.size() == 2);
  CHECK(sink.frames[1][kSequence] == 1);
}

} // namespace
