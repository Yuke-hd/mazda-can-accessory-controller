#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "gvret/replay_stream.hpp"

namespace {

gvret::ParsedGvretFrame parsed(const std::uint64_t timestamp_us, const std::uint32_t bus,
                               const std::uint32_t identifier) {
  gvret::ParsedGvretFrame result;
  result.timestamp_us = timestamp_us;
  result.bus = bus;
  result.direction = gvret::Direction::Rx;
  result.frame.timestamp_us = timestamp_us;
  result.frame.identifier = identifier;
  result.frame.dlc = 1;
  result.frame.data[0] = static_cast<std::uint8_t>(identifier);
  return result;
}

} // namespace

TEST_CASE("replay defaults to bus zero and makes its first frame time zero") {
  const std::vector<gvret::ParsedGvretFrame> source{
      parsed(10'000, 1, 0x101), parsed(20'000, 0, 0x102), parsed(20'000, 1, 0x103),
      parsed(20'750, 0, 0x104)};

  const auto result = gvret::prepare_replay(source);

  REQUIRE(result.ok());
  REQUIRE(result.frames.size() == 2);
  CHECK(result.frames[0].frame.identifier == 0x102);
  CHECK(result.frames[0].relative_time_us == 0);
  CHECK(result.frames[0].frame.timestamp_us == 0);
  CHECK(result.frames[1].frame.identifier == 0x104);
  CHECK(result.frames[1].relative_time_us == 750);
  CHECK(result.frames[1].frame.timestamp_us == 750);
}

TEST_CASE("replay normalizes epoch microseconds across second and multi-second boundaries") {
  const std::vector<gvret::ParsedGvretFrame> source{
      parsed(1'790'000'000'999'900ULL, 0, 0x201), parsed(1'790'000'000'999'900ULL, 0, 0x202),
      parsed(1'790'000'001'000'100ULL, 0, 0x203), parsed(1'790'000'002'250'000ULL, 0, 0x204)};

  const auto result = gvret::prepare_replay(source);

  REQUIRE(result.ok());
  REQUIRE(result.frames.size() == 4);
  CHECK(result.frames[0].frame.identifier == 0x201);
  CHECK(result.frames[1].frame.identifier == 0x202);
  CHECK(result.frames[2].frame.identifier == 0x203);
  CHECK(result.frames[3].frame.identifier == 0x204);
  CHECK(result.frames[0].relative_time_us == 0);
  CHECK(result.frames[1].relative_time_us == 0);
  CHECK(result.frames[2].relative_time_us == 200);
  CHECK(result.frames[3].relative_time_us == 1'250'100);
  CHECK(result.frames[0].frame.timestamp_us == 0);
  CHECK(result.frames[1].frame.timestamp_us == 0);
  CHECK(result.frames[2].frame.timestamp_us == 200);
  CHECK(result.frames[3].frame.timestamp_us == 1'250'100);
}

TEST_CASE("replay selects an explicit bus and preserves equal timestamp order") {
  const std::vector<gvret::ParsedGvretFrame> source{
      parsed(5'000, 2, 0x201), parsed(5'000, 2, 0x202), parsed(5'001, 0, 0x203),
      parsed(5'500, 2, 0x204)};

  const auto result = gvret::prepare_replay(source, 2);

  REQUIRE(result.ok());
  REQUIRE(result.frames.size() == 3);
  CHECK(result.frames[0].frame.identifier == 0x201);
  CHECK(result.frames[1].frame.identifier == 0x202);
  CHECK(result.frames[2].frame.identifier == 0x204);
  CHECK(result.frames[0].relative_time_us == 0);
  CHECK(result.frames[1].relative_time_us == 0);
  CHECK(result.frames[2].relative_time_us == 500);
}

TEST_CASE("replay keeps a selected bus empty when no row matches") {
  const std::vector<gvret::ParsedGvretFrame> source{parsed(10, 0, 0x301), parsed(20, 1, 0x302)};

  const auto result = gvret::prepare_replay(source, 7);

  CHECK(result.ok());
  CHECK(result.frames.empty());
}

TEST_CASE("replay rejects timestamp regressions in the selected bus") {
  const std::vector<gvret::ParsedGvretFrame> source{parsed(100, 3, 0x401), parsed(200, 1, 0x402),
                                                    parsed(99, 3, 0x403)};

  const auto result = gvret::prepare_replay(source, 3);

  REQUIRE_FALSE(result.ok());
  REQUIRE(result.error.has_value());
  CHECK(result.error->code == gvret::ReplayErrorCode::TimestampRegression);
  CHECK(result.error->source_index == 2);
  CHECK(result.frames.empty());
  CHECK(result.error->message().find("source index 2") != std::string::npos);
}

TEST_CASE("replay does not narrow the selected source bus into RawCanFrame bus_id") {
  const std::vector<gvret::ParsedGvretFrame> source{parsed(900, 300, 0x501)};

  const auto result = gvret::prepare_replay(source, 300);

  REQUIRE(result.ok());
  REQUIRE(result.frames.size() == 1);
  CHECK(result.frames.front().frame.bus_id == 0);
}

TEST_CASE("replay skips Tx rows before checking timestamp order") {
  auto transmitted = parsed(50, 0, 0x402);
  transmitted.direction = gvret::Direction::Tx;
  const std::vector<gvret::ParsedGvretFrame> source{parsed(100, 0, 0x401), transmitted,
                                                    parsed(200, 0, 0x403)};

  const auto result = gvret::prepare_replay(source);

  REQUIRE(result.ok());
  REQUIRE(result.frames.size() == 2);
  CHECK(result.skipped_transmit_count == 1);
  CHECK(result.frames[0].frame.identifier == 0x401);
  CHECK(result.frames[0].relative_time_us == 0);
  CHECK(result.frames[1].frame.identifier == 0x403);
  CHECK(result.frames[1].relative_time_us == 100);
}

TEST_CASE("replay returns no frames for a selected bus containing only Tx rows") {
  auto transmitted = parsed(100, 0, 0x404);
  transmitted.direction = gvret::Direction::Tx;
  const std::vector<gvret::ParsedGvretFrame> source{transmitted};

  const auto result = gvret::prepare_replay(source);

  REQUIRE(result.ok());
  CHECK(result.frames.empty());
  CHECK(result.skipped_transmit_count == 1);
}

TEST_CASE("replay skips a parsed frame whose direction is unknown") {
  const std::vector<gvret::ParsedGvretFrame> source{parsed(100, 0, 0x405),
                                                    gvret::ParsedGvretFrame{}};

  const auto result = gvret::prepare_replay(source);

  REQUIRE(result.ok());
  REQUIRE(result.frames.size() == 1);
  CHECK(result.frames.front().frame.identifier == 0x405);
}
