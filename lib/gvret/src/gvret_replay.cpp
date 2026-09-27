#include "gvret/file_loader.hpp"
#include "gvret/pixel_frame_output.hpp"
#include "gvret/replay_scheduler.hpp"

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string_view>

namespace {

struct Arguments {
  enum class Command : std::uint8_t { Inspect, Render };

  Command command{Command::Inspect};
  std::filesystem::path input_path;
  std::uint32_t bus{0};
  gvret::ReplayScheduleOptions schedule{};
  bool saw_end_time{false};
};

const char *controller_status_name(const gvret::ReplayControllerStatus status) {
  switch (status) {
  case gvret::ReplayControllerStatus::Ok:
    return "ok";
  case gvret::ReplayControllerStatus::InvalidState:
    return "invalid state";
  case gvret::ReplayControllerStatus::ConfigurationFailed:
    return "configuration failed";
  case gvret::ReplayControllerStatus::SynchronizationTimeout:
    return "synchronization timeout";
  case gvret::ReplayControllerStatus::TelemetryFault:
    return "telemetry fault";
  case gvret::ReplayControllerStatus::RendererFault:
    return "renderer fault";
  }
  return "unknown controller failure";
}

void print_schedule_error(const gvret::ReplayScheduleResult &schedule) {
  switch (schedule.status) {
  case gvret::ReplayScheduleStatus::InvalidInput:
    std::cerr << "error: invalid replay input\n";
    break;
  case gvret::ReplayScheduleStatus::InvalidOptions:
    std::cerr << "error: invalid replay options: --end-us must cover the capture duration, "
                 "and cadence periods must be non-zero\n";
    break;
  case gvret::ReplayScheduleStatus::ClockFailure:
    std::cerr << "error: replay clock failure\n";
    break;
  case gvret::ReplayScheduleStatus::ControllerFailure:
    std::cerr << "error: replay controller failure: "
              << controller_status_name(schedule.controller_status);
    if (schedule.stop_status != gvret::ReplayControllerStatus::Ok)
      std::cerr << "; stop: " << controller_status_name(schedule.stop_status);
    std::cerr << '\n';
    break;
  case gvret::ReplayScheduleStatus::Ok:
    break;
  }
}

void print_usage(std::ostream &stream) {
  stream << "Usage: gvret-replay inspect <capture.csv> [--bus <number>]\n"
         << "       gvret-replay render <capture.csv> --end-us <number> [options]\n"
         << "Options: --bus <number> --availability-us <number> --output-tick-us <number> "
            "--poll-us <number>\n";
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

bool parse_timestamp(const std::string_view text, vehicle_core::MonotonicTimestamp &timestamp_us) {
  if (text.empty()) {
    return false;
  }

  vehicle_core::MonotonicTimestamp parsed = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed, 10);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
    return false;
  }
  timestamp_us = parsed;
  return true;
}

bool parse_period(const std::string_view text, vehicle_core::Microseconds &period_us) {
  return parse_timestamp(text, period_us) && period_us != 0;
}

bool parse_arguments(const int argc, char **argv, Arguments &arguments) {
  if (argc < 3) {
    return false;
  }

  const std::string_view command = argv[1];
  if (command == "inspect") {
    arguments.command = Arguments::Command::Inspect;
  } else if (command == "render") {
    arguments.command = Arguments::Command::Render;
  } else {
    return false;
  }

  bool saw_path = false;
  bool saw_bus = false;
  bool saw_availability = false;
  bool saw_output_tick = false;
  bool saw_poll = false;
  for (int index = 2; index < argc; ++index) {
    const std::string_view argument = argv[index];
    if (argument == "--bus") {
      if (saw_bus || index + 1 >= argc || !parse_bus(argv[++index], arguments.bus)) {
        return false;
      }
      saw_bus = true;
      continue;
    }

    if (argument == "--end-us") {
      if (arguments.saw_end_time || arguments.command != Arguments::Command::Render ||
          index + 1 >= argc || !parse_timestamp(argv[++index], arguments.schedule.end_time_us)) {
        return false;
      }
      arguments.saw_end_time = true;
      continue;
    }

    if (argument == "--availability-us" || argument == "--output-tick-us" ||
        argument == "--poll-us") {
      if (arguments.command != Arguments::Command::Render || index + 1 >= argc) {
        return false;
      }
      bool &saw_period = argument == "--availability-us"  ? saw_availability
                         : argument == "--output-tick-us" ? saw_output_tick
                                                          : saw_poll;
      if (saw_period)
        return false;
      auto &period = argument == "--availability-us"  ? arguments.schedule.availability_period_us
                     : argument == "--output-tick-us" ? arguments.schedule.output_tick_period_us
                                                      : arguments.schedule.poll_period_us;
      if (!parse_period(argv[++index], period)) {
        return false;
      }
      saw_period = true;
      continue;
    }

    if (argument.rfind("--", 0) == 0 || saw_path) {
      return false;
    }
    arguments.input_path = argument;
    saw_path = true;
  }
  return saw_path && (arguments.command == Arguments::Command::Inspect || arguments.saw_end_time);
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

  if (arguments.command == Arguments::Command::Inspect) {
    print_summary(gvret::summarize(replay.frames, arguments.bus));
    return 0;
  }

  gvret::ReplayClock clock;
  gvret::JsonlPixelFrameSink pixels{clock, std::cout};
  const auto schedule = gvret::run_replay(replay.frames, clock, pixels, arguments.schedule);
  if (!schedule.ok()) {
    print_schedule_error(schedule);
    return 1;
  }
  if (!pixels.write_end()) {
    std::cerr << "error: unable to write pixel output\n";
    return 1;
  }
  std::cout.flush();
  if (!pixels.good()) {
    std::cerr << "error: unable to write pixel output\n";
    return 1;
  }
  return 0;
}
