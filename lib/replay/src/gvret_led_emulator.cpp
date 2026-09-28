#include "gvret/file_loader.hpp"
#include "replay/web_emulator.hpp"

#include <charconv>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string_view>

namespace {

std::atomic_bool *g_stop_requested = nullptr;

void handle_signal(const int) noexcept {
  if (g_stop_requested != nullptr)
    g_stop_requested->store(true);
}

bool parse_unsigned(const std::string_view text, std::uint32_t &value) {
  if (text.empty())
    return false;
  std::uint32_t parsed = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed, 10);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
    return false;
  value = parsed;
  return true;
}

bool parse_port(const std::string_view text, std::uint16_t &port) {
  std::uint32_t parsed = 0;
  if (!parse_unsigned(text, parsed) || parsed > 65535)
    return false;
  port = static_cast<std::uint16_t>(parsed);
  return true;
}

bool parse_period(const std::string_view text, vehicle_core::Microseconds &period) {
  std::uint32_t parsed = 0;
  if (!parse_unsigned(text, parsed) || parsed == 0)
    return false;
  period = parsed;
  return true;
}

void print_usage(std::ostream &stream) {
  stream << "Usage: gvret-led-emulator <capture.csv> [options]\n"
         << "Options: --bus <number> --port <number> --bind <127.0.0.1|::1> --ipv6\n"
         << "         --availability-us <number> --output-tick-us <number> --poll-us <number>\n";
}

bool parse_arguments(const int argc, char **argv, replay::WebEmulatorOptions &options,
                     std::filesystem::path &input_path) {
  if (argc < 2)
    return false;

  bool saw_path = false;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument = argv[index];
    if (argument == "--ipv6") {
      if (options.ipv6)
        return false;
      options.ipv6 = true;
      options.bind_address = "::1";
      continue;
    }

    if (argument == "--bus" || argument == "--port" || argument == "--bind" ||
        argument == "--availability-us" || argument == "--output-tick-us" ||
        argument == "--poll-us") {
      if (index + 1 >= argc)
        return false;
      const std::string_view value = argv[++index];
      if (argument == "--bus") {
        if (!parse_unsigned(value, options.bus))
          return false;
      } else if (argument == "--port") {
        if (!parse_port(value, options.port))
          return false;
      } else if (argument == "--bind") {
        options.bind_address = value;
        options.ipv6 = value == "::1";
      } else {
        vehicle_core::Microseconds period = 0;
        if (!parse_period(value, period))
          return false;
        if (argument == "--availability-us")
          options.schedule.availability_period_us = period;
        else if (argument == "--output-tick-us")
          options.schedule.output_tick_period_us = period;
        else
          options.schedule.poll_period_us = period;
      }
      continue;
    }

    if (argument.rfind("--", 0) == 0 || saw_path)
      return false;
    input_path = argument;
    saw_path = true;
  }
  return saw_path;
}

} // namespace

int main(const int argc, char **argv) {
  replay::WebEmulatorOptions options;
  std::filesystem::path input_path;
  if (!parse_arguments(argc, argv, options, input_path)) {
    print_usage(std::cerr);
    return 2;
  }

  const gvret::FileReplayResult replay =
      gvret::load_file(input_path, gvret::ReplayOptions{options.bus});
  if (!replay.ok()) {
    std::cerr << "error: " << replay.error->message() << '\n';
    return 1;
  }

  replay::WebEmulatorServer server{replay.frames, options};
  if (!server.start()) {
    std::cerr << "error: " << server.error() << '\n';
    return 1;
  }

  std::atomic_bool stop_requested{false};
  g_stop_requested = &stop_requested;
  std::signal(SIGINT, handle_signal);
#ifdef SIGTERM
  std::signal(SIGTERM, handle_signal);
#endif
  std::cout << "gvret-led-emulator listening at " << server.url() << '\n' << std::flush;
  const bool served = server.serve(stop_requested);
  g_stop_requested = nullptr;
  return served ? 0 : 1;
}
