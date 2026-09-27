#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <optional>
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

TEST_CASE("next scheduled time can be inspected without consuming input") {
  gvret::ReplayClock clock;
  gvret::ReplayAcquisitionSource source{{timed_frame(25, 0x251), timed_frame(75, 0x252)}, clock};
  vehicle_core::RawCanFrame frame{};

  REQUIRE(source.start().ok());
  CHECK(source.next_frame_time() == std::optional<vehicle_core::MonotonicTimestamp>{25});
  CHECK(source.receive(frame, 0) == vehicle_telemetry::ReceiveStatus::Timeout);
  CHECK(source.next_frame_time() == std::optional<vehicle_core::MonotonicTimestamp>{25});

  REQUIRE(clock.advance_to(25));
  REQUIRE(source.receive(frame, 0) == vehicle_telemetry::ReceiveStatus::Frame);
  CHECK(source.next_frame_time() == std::optional<vehicle_core::MonotonicTimestamp>{75});

  REQUIRE(clock.advance_to(75));
  REQUIRE(source.receive(frame, 0) == vehicle_telemetry::ReceiveStatus::Frame);
  CHECK_FALSE(source.next_frame_time().has_value());
}

TEST_CASE("source start and stop enforce one-shot lifecycle") {
  gvret::ReplayClock clock;
  gvret::ReplayAcquisitionSource source{{timed_frame(0, 0x301), timed_frame(10, 0x302)}, clock};
  vehicle_core::RawCanFrame frame{};

  CHECK(source.receive(frame, 0) == vehicle_telemetry::ReceiveStatus::NotStarted);
  REQUIRE(source.start().ok());
  CHECK(source.start().status == vehicle_telemetry::ResultCode::AlreadyRunning);
  REQUIRE(source.receive(frame, 0) == vehicle_telemetry::ReceiveStatus::Frame);
  CHECK(source.statistics().frames_received == 1);
  REQUIRE(clock.advance_to(10));
  CHECK(source.stop().ok());
  CHECK(source.receive(frame, 0) == vehicle_telemetry::ReceiveStatus::NotStarted);
  CHECK(source.stop().ok());
  CHECK(source.start().status == vehicle_telemetry::ResultCode::InvalidState);
  CHECK(source.statistics().frames_received == 1);
  CHECK_FALSE(source.end_of_stream());
}
