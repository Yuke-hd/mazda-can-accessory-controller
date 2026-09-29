#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include "gvret/file_loader.hpp"

namespace {

constexpr char kHeader[] = "Time Stamp,ID,Extended,Bus,LEN,D1,D2,D3,D4,D5,D6,D7,D8";
constexpr char kV2Header[] = "Time Stamp,ID,Extended,Dir,Bus,LEN,D1,D2,D3,D4,D5,D6,D7,D8";

class TemporaryCsv {
public:
  explicit TemporaryCsv(const std::string &contents)
      : path_(std::filesystem::temp_directory_path() /
              ("mazda-gvret-inspect-" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
               ".csv")) {
    std::ofstream output(path_, std::ios::binary);
    REQUIRE(output.is_open());
    output << contents;
    REQUIRE(output.good());
  }

  ~TemporaryCsv() {
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
  }

  [[nodiscard]] const std::filesystem::path &path() const noexcept { return path_; }

private:
  std::filesystem::path path_;
};

} // namespace

TEST_CASE("file loader parses, selects, and summarizes a generated CSV") {
  const TemporaryCsv csv(std::string(kHeader) + "\n100000,00000123,false,1,2,AA,BB,,,,,,\n" +
                         "100100,001ABCDE,true,0,1,CC,,,,,,,\n" + "100750,00000456,false,0,0\n");

  const auto loaded = gvret::load_file(csv.path(), gvret::ReplayOptions{0});
  REQUIRE(loaded.ok());
  REQUIRE(loaded.frames.size() == 2);
  CHECK(loaded.frames[0].relative_time_us == 0);
  CHECK(loaded.frames[1].relative_time_us == 650);

  const auto summary = gvret::summarize(loaded.frames, 0);
  CHECK(summary.selected_bus == 0);
  CHECK(summary.frame_count == 2);
  CHECK(summary.relative_duration_us == 650);
  CHECK(summary.standard_frame_count == 1);
  CHECK(summary.extended_frame_count == 1);
  REQUIRE(summary.min_identifier.has_value());
  REQUIRE(summary.max_identifier.has_value());
  CHECK(*summary.min_identifier == 0x456);
  CHECK(*summary.max_identifier == 0x1ABCDE);
}

TEST_CASE("file loader normalizes epoch-microsecond captures without exposing wall time") {
  const TemporaryCsv csv(std::string(kHeader) + "\n1790000000999900,00000123,false,0,0\n"
                                                "1790000001000100,00000124,false,0,0\n"
                                                "1790000002250000,00000125,false,0,0\n");

  const auto loaded = gvret::load_file(csv.path());

  REQUIRE(loaded.ok());
  REQUIRE(loaded.frames.size() == 3);
  CHECK(loaded.frames[0].relative_time_us == 0);
  CHECK(loaded.frames[1].relative_time_us == 200);
  CHECK(loaded.frames[2].relative_time_us == 1'250'100);
  CHECK(loaded.frames[0].frame.timestamp_us == 0);
  CHECK(loaded.frames[1].frame.timestamp_us == 200);
  CHECK(loaded.frames[2].frame.timestamp_us == 1'250'100);
  CHECK(loaded.frames[2].frame.timestamp_us != 1'790'000'002'250'000ULL);
}

TEST_CASE("file loader accepts a SavvyCAN V2 capture and excludes Tx rows") {
  const TemporaryCsv csv(std::string(kV2Header) + "\n100,00000401,false,Tx,0,1,AA,,,,,,,,\n"
                                                  "200,00000402,false,Rx,0,1,BB,,,,,,,,\n");

  const auto loaded = gvret::load_file(csv.path());

  REQUIRE(loaded.ok());
  REQUIRE(loaded.frames.size() == 1);
  CHECK(loaded.skipped_transmit_count == 1);
  CHECK(loaded.frames.front().frame.identifier == 0x402);
  CHECK(loaded.frames.front().frame.dlc == 1);
  CHECK(loaded.frames.front().frame.data[0] == 0xBB);
  CHECK(loaded.frames.front().relative_time_us == 0);
}

TEST_CASE("file loader preserves parser diagnostics without exposing row data") {
  const TemporaryCsv csv(std::string(kHeader) + "\nnot-a-timestamp,00000123,false,0,0,,,,,,,,\n");

  const auto loaded = gvret::load_file(csv.path());
  REQUIRE_FALSE(loaded.ok());
  REQUIRE(loaded.error.has_value());
  CHECK(loaded.error->code == gvret::FileErrorCode::ParseFailed);
  CHECK(loaded.error->message() == "GVRET parse error: line 2: invalid timestamp");
  CHECK(loaded.error->message().find("not-a-timestamp") == std::string::npos);
}

TEST_CASE("file loader reports an unavailable path without echoing it") {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "mazda-gvret-inspect-file-that-does-not-exist.csv";
  const auto loaded = gvret::load_file(path);

  REQUIRE_FALSE(loaded.ok());
  REQUIRE(loaded.error.has_value());
  CHECK(loaded.error->code == gvret::FileErrorCode::OpenFailed);
  CHECK(loaded.error->message() == "unable to open GVRET input file");
  CHECK(loaded.error->message().find(path.string()) == std::string::npos);
}
