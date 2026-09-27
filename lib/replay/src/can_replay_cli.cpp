#include "gvret/file_loader.hpp"
#include "replay/local_argb_stage.hpp"
#include "replay/pixel_frame_output.hpp"
#include "replay/scheduler.hpp"
#include "replay/signal_record_output.hpp"

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string_view>
#include <utility>

namespace {

struct Arguments {
  enum class Command : std::uint8_t { Inspect, Render };

  Command command{Command::Inspect};
  std::filesystem::path input_path;
  std::uint32_t bus{0};
  replay::ReplayScheduleOptions schedule{};
  bool saw_end_time{false};
  bool emit_signals{false};
};

const char *controller_status_name(const replay::ReplayControllerStatus status) {
  switch (status) {
  case replay::ReplayControllerStatus::Ok:
    return "ok";
  case replay::ReplayControllerStatus::InvalidState:
    return "invalid state";
  case replay::ReplayControllerStatus::ConfigurationFailed:
    return "configuration failed";
  case replay::ReplayControllerStatus::SynchronizationTimeout:
    return "synchronization timeout";
  case replay::ReplayControllerStatus::TelemetryFault:
    return "telemetry fault";
  case replay::ReplayControllerStatus::OutputFault:
    return "output fault";
  }
  return "unknown controller failure";
}

void print_schedule_error(const replay::ReplayScheduleResult &schedule) {
  switch (schedule.status) {
  case replay::ReplayScheduleStatus::InvalidInput:
    std::cerr << "error: invalid replay input\n";
    break;
  case replay::ReplayScheduleStatus::InvalidOptions:
    std::cerr << "error: invalid replay options: --end-us must cover the capture duration, "
                 "and cadence periods must be non-zero\n";
    break;
  case replay::ReplayScheduleStatus::ClockFailure:
    std::cerr << "error: replay clock failure\n";
    break;
  case replay::ReplayScheduleStatus::ControllerFailure:
    std::cerr << "error: replay controller failure: "
              << controller_status_name(schedule.controller_status);
    if (schedule.stop_status != replay::ReplayControllerStatus::Ok)
      std::cerr << "; stop: " << controller_status_name(schedule.stop_status);
    std::cerr << '\n';
    break;
  case replay::ReplayScheduleStatus::Ok:
    break;
  }
}

// Signal records may precede the first pixel frame. The catalog arrives only
// after the replay options were accepted, so the stream header is written
// there and a rejected replay still emits nothing.
class HeaderedSignalRecords final : public replay::SignalObserver {
public:
  HeaderedSignalRecords(replay::JsonlPixelFrameSink &pixels, std::ostream &output) noexcept
      : pixels_(&pixels), records_(output) {}

  void on_catalog(const vehicle_signals::SignalCatalogView catalog) noexcept override {
    header_written_ = pixels_->write_header();
    records_.on_catalog(catalog);
  }

  void on_reading(const vehicle_core::MonotonicTimestamp time_us,
                  const vehicle_signals::SignalMetadata &signal,
                  const vehicle_signals::SignalReading &reading) noexcept override {
    if (header_written_)
      records_.on_reading(time_us, signal, reading);
  }

  [[nodiscard]] bool good() const noexcept { return header_written_ && records_.good(); }

private:
  replay::JsonlPixelFrameSink *pixels_;
  replay::JsonlSignalRecordWriter records_;
  bool header_written_{false};
};

void print_usage(std::ostream &stream) {
  stream << "Usage: can-replay inspect <gvret-capture.csv> [--bus <number>]\n"
         << "       can-replay render <gvret-capture.csv> --end-us <number> [options]\n"
         << "Options: --bus <number> --availability-us <number> --output-tick-us <number> "
            "--poll-us <number>\n"
         << "         --signals [--signal-sample-us <number>]\n";
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
  bool saw_signal_sample = false;
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
      vehicle_core::Microseconds period = 0;
      if (!parse_period(argv[++index], period)) {
        return false;
      }
      if (argument == "--availability-us")
        arguments.schedule.availability_period_us = period;
      else if (argument == "--output-tick-us")
        arguments.schedule.output_tick_period_us = period;
      else
        arguments.schedule.poll_period_us = period;
      saw_period = true;
      continue;
    }

    if (argument == "--signals") {
      if (arguments.emit_signals || arguments.command != Arguments::Command::Render)
        return false;
      arguments.emit_signals = true;
      continue;
    }

    if (argument == "--signal-sample-us") {
      vehicle_core::Microseconds period = 0;
      if (saw_signal_sample || arguments.command != Arguments::Command::Render ||
          index + 1 >= argc || !parse_period(argv[++index], period)) {
        return false;
      }
      arguments.schedule.signal_sample_period_us = period;
      saw_signal_sample = true;
      continue;
    }

    if (argument.rfind("--", 0) == 0 || saw_path) {
      return false;
    }
    arguments.input_path = argument;
    saw_path = true;
  }
  if (saw_signal_sample && !arguments.emit_signals)
    return false;
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

  replay::ReplayClock clock;
  replay::JsonlPixelFrameSink pixels{clock, std::cout};
  replay::LocalArgbOutputStage output{pixels};
  HeaderedSignalRecords signal_records{pixels, std::cout};
  replay::SignalObservers observers{};
  if (arguments.emit_signals)
    observers.push_back(&signal_records);
  const auto schedule =
      replay::run_replay(replay.frames, clock, output, std::move(observers), arguments.schedule);
  if (!schedule.ok()) {
    print_schedule_error(schedule);
    return 1;
  }
  if (arguments.emit_signals && !signal_records.good()) {
    std::cerr << "error: unable to write signal output\n";
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
