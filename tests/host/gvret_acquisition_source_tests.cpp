#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <vector>

#include "gvret/replay_acquisition_source.hpp"

namespace {

gvret::TimedCanFrame timed_frame(const vehicle_core::Microseconds time_us,
                                 const std::uint32_t identifier) {
  gvret::TimedCanFrame result;
  result.relative_time_us = time_us;
  result.frame.timestamp_us = time_us;
  result.frame.identifier = identifier;
  return result;
}

} // namespace

TEST_CASE("empty replay times out and reports end of stream") {
  gvret::ReplayClock clock;
  gvret::ReplayAcquisitionSource source{{}, clock};
  vehicle_core::RawCanFrame frame{};

  CHECK(source.start().ok());
  CHECK(source.receive(frame, 100) == vehicle_telemetry::ReceiveStatus::Timeout);
  CHECK(source.end_of_stream());
  CHECK(source.statistics().frames_received == 0);
}

TEST_CASE("one frame is delivered at its scheduled clock time") {
  gvret::ReplayClock clock;
  gvret::ReplayAcquisitionSource source{{timed_frame(10, 0x101)}, clock};
  vehicle_core::RawCanFrame frame{};

  REQUIRE(source.start().ok());
  CHECK(source.receive(frame, 0) == vehicle_telemetry::ReceiveStatus::Timeout);
  CHECK_FALSE(source.end_of_stream());
  CHECK(clock.now() == 0);
  REQUIRE(clock.advance_to(10));
  CHECK(source.receive(frame, 0) == vehicle_telemetry::ReceiveStatus::Frame);
  CHECK(frame.identifier == 0x101);
  CHECK(frame.timestamp_us == 10);
  CHECK(source.statistics().frames_received == 1);
  CHECK(source.end_of_stream());
  CHECK(source.receive(frame, 0) == vehicle_telemetry::ReceiveStatus::Timeout);
  CHECK(source.statistics().frames_received == 1);
}

TEST_CASE("multiple timed frames wait for the clock and preserve equal time order") {
  gvret::ReplayClock clock;
  gvret::ReplayAcquisitionSource source{
      {timed_frame(0, 0x201), timed_frame(5, 0x202), timed_frame(5, 0x203), timed_frame(10, 0x204)},
      clock};
  vehicle_core::RawCanFrame frame{};

  REQUIRE(source.start().ok());
  REQUIRE(source.receive(frame, 0) == vehicle_telemetry::ReceiveStatus::Frame);
  CHECK(frame.identifier == 0x201);
  CHECK(source.receive(frame, 0) == vehicle_telemetry::ReceiveStatus::Timeout);
  REQUIRE(clock.advance_to(5));
  REQUIRE(source.receive(frame, 0) == vehicle_telemetry::ReceiveStatus::Frame);
  CHECK(frame.identifier == 0x202);
  REQUIRE(source.receive(frame, 0) == vehicle_telemetry::ReceiveStatus::Frame);
  CHECK(frame.identifier == 0x203);
  CHECK(source.receive(frame, 0) == vehicle_telemetry::ReceiveStatus::Timeout);
  REQUIRE(clock.advance_to(10));
  REQUIRE(source.receive(frame, 0) == vehicle_telemetry::ReceiveStatus::Frame);
  CHECK(frame.identifier == 0x204);
  CHECK(source.end_of_stream());
  CHECK(source.statistics().frames_received == 4);
}

TEST_CASE("source start and stop enforce lifecycle and restart from the beginning") {
  gvret::ReplayClock clock;
  gvret::ReplayAcquisitionSource source{{timed_frame(0, 0x301)}, clock};
  vehicle_core::RawCanFrame frame{};

  CHECK(source.receive(frame, 0) == vehicle_telemetry::ReceiveStatus::NotStarted);
  REQUIRE(source.start().ok());
  CHECK(source.start().status == vehicle_telemetry::ResultCode::AlreadyRunning);
  REQUIRE(source.receive(frame, 0) == vehicle_telemetry::ReceiveStatus::Frame);
  CHECK(source.statistics().frames_received == 1);
  CHECK(source.stop().ok());
  CHECK(source.receive(frame, 0) == vehicle_telemetry::ReceiveStatus::NotStarted);
  CHECK(source.stop().ok());
  REQUIRE(source.start().ok());
  CHECK(source.statistics().frames_received == 0);
  REQUIRE(source.receive(frame, 0) == vehicle_telemetry::ReceiveStatus::Frame);
  CHECK(frame.identifier == 0x301);
}
