#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

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

namespace {

constexpr char kHeader[] = "Time Stamp,ID,Extended,Bus,LEN,D1,D2,D3,D4,D5,D6,D7,D8";

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

std::filesystem::path replay_executable_for(const char *test_executable) {
  const std::filesystem::path test_path =
      std::filesystem::absolute(std::filesystem::path(test_executable));
  const auto build_root = test_path.parent_path().parent_path().parent_path();
  std::filesystem::path replay = build_root / "lib" / "gvret" / "gvret-replay";
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

int main(int argc, char **argv) {
  g_replay_executable = replay_executable_for(argv[0]);
  doctest::Context context(argc, argv);
  return context.run();
}
