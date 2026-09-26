#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <string>

#include "gvret/csv_parser.hpp"

namespace {

constexpr char kHeader[] = "Time Stamp,ID,Extended,Bus,LEN,D1,D2,D3,D4,D5,D6,D7,D8";

std::string csv_row(const char *timestamp, const char *id, const char *extended, const char *bus,
                    const char *length, const char *bytes) {
  return std::string(timestamp) + "," + id + "," + extended + "," + bus + "," + length + "," +
         bytes;
}

std::string one_row(const std::string &row) { return std::string(kHeader) + "\n" + row + "\n"; }

void check_failure(const std::string &csv, const std::size_t expected_line,
                   const char *expected_reason) {
  const gvret::ParseResult parsed = gvret::parse_csv(csv);
  REQUIRE_FALSE(parsed.ok());
  REQUIRE(parsed.error.has_value());
  CHECK(parsed.error->line_number == expected_line);
  CHECK(parsed.error->reason == expected_reason);
  CHECK(parsed.error->message().find("line ") == 0);
}

} // namespace

TEST_CASE("parser accepts standard CAN IDs and preserves the source bus") {
  const auto parsed = gvret::parse_csv(
      one_row(csv_row("973", "0000013B", "false", "3", "3", "00,01,FE,AA,BB,CC,DD,EE")));

  REQUIRE(parsed.ok());
  REQUIRE(parsed.frames.size() == 1);
  const auto &frame = parsed.frames.front();
  CHECK(frame.timestamp_us == 973);
  CHECK(frame.bus == 3);
  CHECK(frame.frame.timestamp_us == 973);
  CHECK(frame.frame.bus_id == 3);
  CHECK(frame.frame.identifier == 0x13B);
  CHECK(frame.frame.is_extended() == false);
  CHECK(frame.frame.dlc == 3);
  CHECK(frame.frame.data[0] == 0x00);
  CHECK(frame.frame.data[1] == 0x01);
  CHECK(frame.frame.data[2] == 0xFE);
  CHECK(frame.frame.data[3] == 0);
}

TEST_CASE("parser accepts extended IDs and true or numeric Extended flags") {
  const auto parsed = gvret::parse_csv(
      std::string(kHeader) + "\n" + csv_row("2009", "1ABCDE", "1", "0", "1", "a5,,,,,,,") + "\n" +
      csv_row("2988", "01ABCDE", "TRUE", "1", "0", ",,,,,,,") + "\n");

  REQUIRE(parsed.ok());
  REQUIRE(parsed.frames.size() == 2);
  CHECK(parsed.frames[0].frame.identifier == 0x1ABCDE);
  CHECK(parsed.frames[0].frame.is_extended());
  CHECK(parsed.frames[0].frame.data[0] == 0xA5);
  CHECK(parsed.frames[1].frame.is_extended());
  CHECK(parsed.frames[1].bus == 1);
}

TEST_CASE("parser accepts DLC zero and DLC eight") {
  const auto parsed = gvret::parse_csv(
      std::string(kHeader) + "\n" + csv_row("0", "00000000", "0", "0", "0", "") + "\n" +
      csv_row("1", "000007FF", "0", "0", "8", "00,11,22,33,44,55,66,77") + "\n");

  REQUIRE(parsed.ok());
  REQUIRE(parsed.frames.size() == 2);
  CHECK(parsed.frames[0].frame.dlc == 0);
  CHECK(parsed.frames[1].frame.dlc == 8);
  CHECK(parsed.frames[1].frame.data[7] == 0x77);
}

TEST_CASE("parser ignores populated columns after the DLC") {
  const auto parsed = gvret::parse_csv(
      one_row(csv_row("4", "00000123", "0", "0", "7", "00,11,22,33,44,55,66,not-a-byte")));

  REQUIRE(parsed.ok());
  REQUIRE(parsed.frames.size() == 1);
  CHECK(parsed.frames.front().frame.dlc == 7);
  CHECK(parsed.frames.front().frame.data[6] == 0x66);
}

TEST_CASE("parser rejects malformed headers and rows with useful diagnostics") {
  check_failure("Time Stamp,ID,Extended,Bus,LEN,D1\n", 1, "malformed GVRET header");
  check_failure(one_row(csv_row("1", "not-hex", "0", "0", "0", "")), 2, "invalid CAN identifier");
  check_failure(one_row(csv_row("1", "123", "0", "0", "9", "00,11,22,33,44,55,66,77,88")), 2,
                "invalid DLC; expected a value from 0 to 8");
  check_failure(one_row(csv_row("1", "123", "0", "0", "1", "GG")), 2,
                "invalid hexadecimal payload byte");
  check_failure(one_row(csv_row("1", "123", "0", "0", "2", "AA")), 2,
                "row is missing a required payload byte");
  check_failure(one_row(csv_row("1.5", "123", "0", "0", "0", "")), 2, "invalid timestamp");
  check_failure(one_row(csv_row("1", "123", "maybe", "0", "0", "")), 2, "invalid Extended flag");
}

TEST_CASE("parser rejects IDs that do not match their frame format") {
  check_failure(one_row(csv_row("1", "800", "false", "0", "0", "")), 2,
                "CAN frame is outside the classic CAN range");
  check_failure(one_row(csv_row("1", "20000000", "true", "0", "0", "")), 2,
                "CAN frame is outside the classic CAN range");
}
