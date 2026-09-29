#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr char kHeader[] = "Time Stamp,ID,Extended,Bus,LEN,D1,D2,D3,D4,D5,D6,D7,D8";
constexpr char kV2Header[] = "Time Stamp,ID,Extended,Dir,Bus,LEN,D1,D2,D3,D4,D5,D6,D7,D8";

std::filesystem::path g_replay_executable;
std::atomic<std::uint64_t> g_temp_suffix{0};

std::filesystem::path unique_temp_path(const char *label) {
  const auto clock_value = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto suffix = g_temp_suffix.fetch_add(1, std::memory_order_relaxed);
  return std::filesystem::temp_directory_path() /
         (std::string("mazda-") + label + "-" + std::to_string(clock_value) + "-" +
          std::to_string(suffix));
}

class TemporaryFile {
public:
  TemporaryFile(const std::string &label, const std::string &contents)
      : path_(unique_temp_path(label.c_str())) {
    std::ofstream output(path_, std::ios::binary);
    if (!output.is_open()) {
      throw std::runtime_error("unable to create temporary CLI test input");
    }
    output << contents;
    if (!output.good()) {
      throw std::runtime_error("unable to write temporary CLI test input");
    }
  }

  ~TemporaryFile() {
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
  }

  [[nodiscard]] const std::filesystem::path &path() const noexcept { return path_; }

private:
  std::filesystem::path path_;
};

class TemporaryPath {
public:
  explicit TemporaryPath(const char *label) : path_(unique_temp_path(label)) {}

  ~TemporaryPath() {
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
  }

  [[nodiscard]] const std::filesystem::path &path() const noexcept { return path_; }

private:
  std::filesystem::path path_;
};

std::string shell_quote(const std::filesystem::path &path) {
  const std::string value = path.string();
#ifdef _WIN32
  std::string quoted = "\"";
  for (const char character : value) {
    if (character == '"') {
      quoted += "\\\"";
    } else {
      quoted += character;
    }
  }
  quoted += '"';
  return quoted;
#else
  std::string quoted = "'";
  for (const char character : value) {
    if (character == '\'') {
      quoted += "'\\''";
    } else {
      quoted += character;
    }
  }
  quoted += '\'';
  return quoted;
#endif
}

struct CommandResult {
  int exit_code{0};
  std::string standard_output;
  std::string standard_error;
};

std::string read_file(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

CommandResult run_inspect(const std::filesystem::path &input_path, const std::string_view bus,
                          TemporaryPath &standard_output, TemporaryPath &standard_error) {
  const std::string command = shell_quote(g_replay_executable) + " inspect " +
                              shell_quote(input_path) + " --bus " + std::string(bus) + " > " +
                              shell_quote(standard_output.path()) + " 2> " +
                              shell_quote(standard_error.path());
  CommandResult result;
  result.exit_code = std::system(command.c_str());
  result.standard_output = read_file(standard_output.path());
  result.standard_error = read_file(standard_error.path());
  return result;
}

CommandResult run_render_options(const std::filesystem::path &input_path,
                                 const std::string_view options, TemporaryPath &standard_output,
                                 TemporaryPath &standard_error) {
  const std::string command = shell_quote(g_replay_executable) + " render " +
                              shell_quote(input_path) + " " + std::string(options) + " > " +
                              shell_quote(standard_output.path()) + " 2> " +
                              shell_quote(standard_error.path());
  CommandResult result;
  result.exit_code = std::system(command.c_str());
  result.standard_output = read_file(standard_output.path());
  result.standard_error = read_file(standard_error.path());
  return result;
}

CommandResult run_render(const std::filesystem::path &input_path, const std::string_view bus,
                         const std::string_view end_us, TemporaryPath &standard_output,
                         TemporaryPath &standard_error) {
  return run_render_options(input_path,
                            "--bus " + std::string(bus) + " --end-us " + std::string(end_us),
                            standard_output, standard_error);
}

std::filesystem::path replay_executable_for(const char *test_executable) {
  const std::filesystem::path test_path =
      std::filesystem::absolute(std::filesystem::path(test_executable));
  const auto build_root = test_path.parent_path().parent_path().parent_path();
  std::filesystem::path replay = build_root / "lib" / "replay" / "can-replay";
#ifdef _WIN32
  if (!std::filesystem::exists(replay)) {
    replay += ".exe";
  }
#endif
  return replay;
}

} // namespace

TEST_CASE("inspect command prints a privacy-safe summary for a generated CSV") {
  const TemporaryFile input("gvret-cli-input", std::string(kHeader) +
                                                   "\n100000,00000123,false,0,2,AA,BB,,,,,,\n" +
                                                   "100750,001ABCDE,true,0,1,CC,,,,,,,\n" +
                                                   "100900,00000456,false,1,0\n");
  TemporaryPath standard_output("gvret-cli-stdout");
  TemporaryPath standard_error("gvret-cli-stderr");

  const CommandResult result = run_inspect(input.path(), "0", standard_output, standard_error);

  REQUIRE(result.exit_code == 0);
  CHECK(result.standard_error.empty());
  CHECK(result.standard_output.find("selected bus: 0") != std::string::npos);
  CHECK(result.standard_output.find("frames: 2") != std::string::npos);
  CHECK(result.standard_output.find("relative duration (us): 750") != std::string::npos);
  CHECK(result.standard_output.find("standard frames: 1") != std::string::npos);
  CHECK(result.standard_output.find("extended frames: 1") != std::string::npos);
  CHECK(result.standard_output.find("min CAN ID: 0x00000123") != std::string::npos);
  CHECK(result.standard_output.find("max CAN ID: 0x001ABCDE") != std::string::npos);
  CHECK(result.standard_output.find("AA") == std::string::npos);
  CHECK(result.standard_output.find("BB") == std::string::npos);
  CHECK(result.standard_output.find("CC") == std::string::npos);
  CHECK(result.standard_output.find("100000") == std::string::npos);
  CHECK(result.standard_output.find(input.path().string()) == std::string::npos);
}

TEST_CASE("inspect command accepts a SavvyCAN V2 file with trailing separators") {
  const TemporaryFile input("gvret-cli-v2-input",
                            std::string(kV2Header) +
                                "\n100,00000401,false,Tx,0,1,AA,,,,,,,,\n"
                                "200,000005EB,false,Rx,0,8,E8,45,85,4B,4A,28,36,69,\n");
  TemporaryPath standard_output("gvret-cli-v2-stdout");
  TemporaryPath standard_error("gvret-cli-v2-stderr");

  const CommandResult result = run_inspect(input.path(), "0", standard_output, standard_error);

  REQUIRE(result.exit_code == 0);
  CHECK(result.standard_error.empty());
  CHECK(result.standard_output.find("frames: 1") != std::string::npos);
  CHECK(result.standard_output.find("transmitted rows skipped: 1") != std::string::npos);
}

TEST_CASE("inspect command returns a diagnostic without echoing invalid input") {
  const TemporaryFile input("gvret-cli-invalid", std::string(kHeader) +
                                                     "\n1234567890123,00000123,false,0,0\n" +
                                                     "not-a-timestamp,00000124,false,0,0\n");
  TemporaryPath standard_output("gvret-cli-invalid-stdout");
  TemporaryPath standard_error("gvret-cli-invalid-stderr");

  const CommandResult result = run_inspect(input.path(), "0", standard_output, standard_error);

  CHECK(result.exit_code != 0);
  CHECK(result.standard_output.empty());
  CHECK(result.standard_error == "error: GVRET parse error: line 3: invalid timestamp\n");
  CHECK(result.standard_error.find("not-a-timestamp") == std::string::npos);
  CHECK(result.standard_error.find("1234567890123") == std::string::npos);
  CHECK(result.standard_error.find(input.path().string()) == std::string::npos);
}

TEST_CASE("inspect command reports missing files without echoing their path") {
  const TemporaryPath input("gvret-cli-missing");
  TemporaryPath standard_output("gvret-cli-missing-stdout");
  TemporaryPath standard_error("gvret-cli-missing-stderr");

  const CommandResult result = run_inspect(input.path(), "0", standard_output, standard_error);

  CHECK(result.exit_code != 0);
  CHECK(result.standard_output.empty());
  CHECK(result.standard_error == "error: unable to open GVRET input file\n");
  CHECK(result.standard_error.find(input.path().string()) == std::string::npos);
}

TEST_CASE("render command emits only relative timestamped 100-pixel JSONL") {
  const TemporaryFile input("gvret-render-input",
                            std::string(kHeader) +
                                "\n123456789,00000202,false,0,8,32,C8,00,00,00,00,00,00\n" +
                                "123556789,00000091,false,0,8,00,20,00,00,00,00,00,00\n");
  TemporaryPath standard_output("gvret-render-stdout");
  TemporaryPath standard_error("gvret-render-stderr");

  const CommandResult result =
      run_render(input.path(), "0", "110000", standard_output, standard_error);

  REQUIRE(result.exit_code == 0);
  CHECK(result.standard_error.empty());
  REQUIRE_FALSE(result.standard_output.empty());
  CHECK(result.standard_output.find("123456789") == std::string::npos);
  CHECK(result.standard_output.find("123556789") == std::string::npos);
  CHECK(result.standard_output.find("00000202") == std::string::npos);
  CHECK(result.standard_output.find("00000091") == std::string::npos);
  CHECK(result.standard_output.find("C8") == std::string::npos);

  const auto header_end = result.standard_output.find('\n');
  REQUIRE(header_end != std::string::npos);
  CHECK(result.standard_output.substr(0, header_end) ==
        "{\"type\":\"header\",\"version\":1,\"pixel_count\":100}");

  std::size_t pixel_line_count = 0;
  std::size_t line_begin = header_end + 1;
  while (line_begin < result.standard_output.size()) {
    const auto line_end = result.standard_output.find('\n', line_begin);
    REQUIRE(line_end != std::string::npos);
    const auto line = result.standard_output.substr(line_begin, line_end - line_begin);
    if (line == "{\"type\":\"end\"}") {
      CHECK(line_begin + line.size() == result.standard_output.size() - 1);
    } else {
      CHECK(line.find("{\"type\":\"pixels\",\"timestamp_us\":") == 0);
      CHECK(line.find("\"pixels\":[") != std::string::npos);
      CHECK(std::count(line.begin(), line.end(), '[') == 101);
      ++pixel_line_count;
    }
    line_begin = line_end + 1;
  }
  CHECK(pixel_line_count >= 2);
  CHECK(result.standard_output.rfind("{\"type\":\"end\"}\n") ==
        result.standard_output.size() - std::string{"{\"type\":\"end\"}\n"}.size());
}

TEST_CASE("render command requires an explicit bounded replay horizon") {
  const TemporaryFile input("gvret-render-no-end",
                            std::string(kHeader) + "\n123456789,00000202,false,0,0\n");
  TemporaryPath standard_output("gvret-render-no-end-stdout");
  TemporaryPath standard_error("gvret-render-no-end-stderr");
  const std::string command =
      shell_quote(g_replay_executable) + " render " + shell_quote(input.path()) + " --bus 0 > " +
      shell_quote(standard_output.path()) + " 2> " + shell_quote(standard_error.path());

  const int exit_code = std::system(command.c_str());
  CHECK(exit_code != 0);
  CHECK(read_file(standard_output.path()).empty());
}

TEST_CASE("render command reports invalid horizon and emits no stream") {
  const TemporaryFile input("gvret-render-short-horizon",
                            std::string(kHeader) +
                                "\n100000,00000202,false,0,0\n101000,00000202,false,0,0\n");
  TemporaryPath standard_output("gvret-render-short-horizon-stdout");
  TemporaryPath standard_error("gvret-render-short-horizon-stderr");

  const CommandResult result =
      run_render(input.path(), "0", "500", standard_output, standard_error);

  CHECK(result.exit_code != 0);
  CHECK(result.standard_output.empty());
  CHECK(result.standard_error.find("invalid replay options") != std::string::npos);
  CHECK(result.standard_error.find("--end-us") != std::string::npos);
}

TEST_CASE("render command rejects zero cadence and repeated flags during parsing") {
  const TemporaryFile input("gvret-render-invalid-options",
                            std::string(kHeader) + "\n100000,00000202,false,0,0\n");
  TemporaryPath standard_output("gvret-render-invalid-options-stdout");
  TemporaryPath standard_error("gvret-render-invalid-options-stderr");

  const CommandResult zero_period = run_render_options(
      input.path(), "--bus 0 --end-us 100000 --poll-us 0", standard_output, standard_error);
  CHECK(zero_period.exit_code != 0);
  CHECK(zero_period.standard_output.empty());
  CHECK(zero_period.standard_error.find("Usage:") != std::string::npos);

  const CommandResult repeated_flag = run_render_options(
      input.path(), "--bus 0 --end-us 100000 --end-us 100000", standard_output, standard_error);
  CHECK(repeated_flag.exit_code != 0);
  CHECK(repeated_flag.standard_output.empty());
  CHECK(repeated_flag.standard_error.find("Usage:") != std::string::npos);
}

namespace {

std::vector<std::string> lines_of_type(const std::string &stream, const std::string_view type) {
  const std::string prefix = "{\"type\":\"" + std::string(type) + "\"";
  std::vector<std::string> lines;
  std::size_t line_begin = 0;
  while (line_begin < stream.size()) {
    const auto line_end = stream.find('\n', line_begin);
    if (line_end == std::string::npos)
      break;
    auto line = stream.substr(line_begin, line_end - line_begin);
    if (line.rfind(prefix, 0) == 0)
      lines.push_back(std::move(line));
    line_begin = line_end + 1;
  }
  return lines;
}

const std::string kTurnCapture = std::string(kHeader) +
                                 "\n123456789,00000202,false,0,8,32,C8,00,00,00,00,00,00\n" +
                                 "123556789,00000091,false,0,8,00,20,00,00,00,00,00,00\n";

} // namespace

TEST_CASE("render command emits signal records after the header only with --signals") {
  const TemporaryFile input("gvret-render-signals", kTurnCapture);
  TemporaryPath standard_output("gvret-render-signals-stdout");
  TemporaryPath standard_error("gvret-render-signals-stderr");

  const CommandResult result = run_render_options(input.path(), "--bus 0 --end-us 110000 --signals",
                                                  standard_output, standard_error);

  REQUIRE(result.exit_code == 0);
  CHECK(result.standard_error.empty());
  CHECK(result.standard_output.rfind("{\"type\":\"header\",\"version\":1,\"pixel_count\":100}\n",
                                     0) == 0);
  const auto signals = lines_of_type(result.standard_output, "signal");
  REQUIRE_FALSE(signals.empty());
  const std::string turn_left =
      "{\"type\":\"signal\",\"timestamp_us\":100000,\"signal\":\"vehicle.turn_state\","
      "\"value\":\"left\",\"unit\":null,\"freshness\":\"fresh\",\"availability\":\"fresh\",";
  CHECK(std::any_of(signals.begin(), signals.end(), [&turn_left](const std::string &line) {
    return line.rfind(turn_left, 0) == 0;
  }));
  CHECK(result.standard_output.find("123556789") == std::string::npos);
  CHECK(result.standard_output.find("00000091") == std::string::npos);
  CHECK_FALSE(lines_of_type(result.standard_output, "pixels").empty());
  CHECK(result.standard_output.rfind("{\"type\":\"end\"}\n") ==
        result.standard_output.size() - std::string{"{\"type\":\"end\"}\n"}.size());
}

TEST_CASE("render command output without --signals is unchanged by signal support") {
  const TemporaryFile input("gvret-render-no-signals", kTurnCapture);
  TemporaryPath standard_output("gvret-render-no-signals-stdout");
  TemporaryPath standard_error("gvret-render-no-signals-stderr");
  TemporaryPath signal_output("gvret-render-with-signals-stdout");

  const CommandResult plain =
      run_render(input.path(), "0", "110000", standard_output, standard_error);
  const CommandResult with_signals = run_render_options(
      input.path(), "--bus 0 --end-us 110000 --signals", signal_output, standard_error);

  REQUIRE(plain.exit_code == 0);
  REQUIRE(with_signals.exit_code == 0);
  CHECK(lines_of_type(plain.standard_output, "signal").empty());
  CHECK(lines_of_type(plain.standard_output, "pixels") ==
        lines_of_type(with_signals.standard_output, "pixels"));
}

TEST_CASE("render command applies --signal-sample-us only together with --signals") {
  const TemporaryFile input("gvret-render-signal-cadence", kTurnCapture);
  TemporaryPath standard_output("gvret-render-signal-cadence-stdout");
  TemporaryPath standard_error("gvret-render-signal-cadence-stderr");

  const CommandResult sampled =
      run_render_options(input.path(), "--bus 0 --end-us 110000 --signals --signal-sample-us 50000",
                         standard_output, standard_error);
  REQUIRE(sampled.exit_code == 0);
  std::size_t rpm_records = 0;
  for (const auto &line : lines_of_type(sampled.standard_output, "signal"))
    if (line.find("\"signal\":\"vehicle.engine_rpm\"") != std::string::npos)
      ++rpm_records;
  CHECK(rpm_records == 3);

  const CommandResult without_signals =
      run_render_options(input.path(), "--bus 0 --end-us 110000 --signal-sample-us 50000",
                         standard_output, standard_error);
  CHECK(without_signals.exit_code != 0);
  CHECK(without_signals.standard_output.empty());
  CHECK(without_signals.standard_error.find("Usage:") != std::string::npos);

  const CommandResult zero_cadence =
      run_render_options(input.path(), "--bus 0 --end-us 110000 --signals --signal-sample-us 0",
                         standard_output, standard_error);
  CHECK(zero_cadence.exit_code != 0);
  CHECK(zero_cadence.standard_output.empty());

  const CommandResult short_horizon = run_render_options(
      input.path(), "--bus 0 --end-us 50000 --signals", standard_output, standard_error);
  CHECK(short_horizon.exit_code != 0);
  CHECK(short_horizon.standard_output.empty());

  const CommandResult repeated = run_render_options(
      input.path(), "--bus 0 --end-us 110000 --signals --signals", standard_output, standard_error);
  CHECK(repeated.exit_code != 0);
  CHECK(repeated.standard_output.empty());
}

TEST_CASE("inspect command rejects render-only and unknown options") {
  const TemporaryFile input("gvret-inspect-invalid-options",
                            std::string(kHeader) + "\n100000,00000202,false,0,0\n");
  TemporaryPath standard_output("gvret-inspect-invalid-options-stdout");
  TemporaryPath standard_error("gvret-inspect-invalid-options-stderr");

  const std::string inspect_end_command = shell_quote(g_replay_executable) + " inspect " +
                                          shell_quote(input.path()) + " --end-us 100000 > " +
                                          shell_quote(standard_output.path()) + " 2> " +
                                          shell_quote(standard_error.path());
  const int inspect_end_exit = std::system(inspect_end_command.c_str());
  CHECK(inspect_end_exit != 0);
  CHECK(read_file(standard_output.path()).empty());
  CHECK(read_file(standard_error.path()).find("Usage:") != std::string::npos);

  const std::string unknown_option_command =
      shell_quote(g_replay_executable) + " inspect " + shell_quote(input.path()) + " --mystery > " +
      shell_quote(standard_output.path()) + " 2> " + shell_quote(standard_error.path());
  const int unknown_option_exit = std::system(unknown_option_command.c_str());
  CHECK(unknown_option_exit != 0);
  CHECK(read_file(standard_output.path()).empty());
  CHECK(read_file(standard_error.path()).find("Usage:") != std::string::npos);
}

int main(int argc, char **argv) {
  g_replay_executable = replay_executable_for(argv[0]);
  doctest::Context context(argc, argv);
  return context.run();
}
