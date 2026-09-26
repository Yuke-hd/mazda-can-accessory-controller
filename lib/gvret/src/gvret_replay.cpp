#include "gvret/file_loader.hpp"

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string_view>

namespace {

struct Arguments {
  std::filesystem::path input_path;
  std::uint32_t bus{0};
};

void print_usage(std::ostream &stream) {
  stream << "Usage: gvret-replay inspect <capture.csv> [--bus <number>]\n";
}

bool parse_bus(const std::string_view text, std::uint32_t &bus) {
  if (text.empty()) {
    return false;
  }

  std::uint32_t parsed = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed, 10);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
    return false;
  }
  bus = parsed;
  return true;
}

bool parse_arguments(const int argc, char **argv, Arguments &arguments) {
  if (argc < 3 || std::string_view(argv[1]) != "inspect") {
    return false;
  }

  bool saw_path = false;
  for (int index = 2; index < argc; ++index) {
    const std::string_view argument = argv[index];
    if (argument == "--bus") {
      if (index + 1 >= argc || !parse_bus(argv[++index], arguments.bus)) {
        return false;
      }
      continue;
    }

    if (argument.rfind("--", 0) == 0 || saw_path) {
      return false;
    }
    arguments.input_path = argument;
    saw_path = true;
  }
  return saw_path;
}

std::string format_identifier(const std::uint32_t identifier) {
  std::ostringstream formatted;
  formatted << "0x" << std::uppercase << std::hex << std::setw(8) << std::setfill('0')
            << identifier;
  return formatted.str();
}

void print_summary(const gvret::ReplaySummary &summary) {
  std::cout << "GVRET replay summary\n"
            << "selected bus: " << summary.selected_bus << '\n'
            << "frames: " << summary.frame_count << '\n'
            << "relative duration (us): " << summary.relative_duration_us << '\n'
            << "standard frames: " << summary.standard_frame_count << '\n'
            << "extended frames: " << summary.extended_frame_count << '\n';
  if (summary.min_identifier.has_value()) {
    std::cout << "min CAN ID: " << format_identifier(*summary.min_identifier) << '\n'
              << "max CAN ID: " << format_identifier(*summary.max_identifier) << '\n';
  }
}

} // namespace

int main(const int argc, char **argv) {
  Arguments arguments;
  if (!parse_arguments(argc, argv, arguments)) {
    print_usage(std::cerr);
    return 2;
  }

  const gvret::FileReplayResult replay =
      gvret::load_file(arguments.input_path, gvret::ReplayOptions{arguments.bus});
  if (!replay.ok()) {
    std::cerr << "error: " << replay.error->message() << '\n';
    return 1;
  }

  print_summary(gvret::summarize(replay.frames, arguments.bus));
  return 0;
}
