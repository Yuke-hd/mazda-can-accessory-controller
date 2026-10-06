#include "replay/signal_record_output.hpp"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace replay {
namespace {

using vehicle_signals::Availability;
using vehicle_signals::SignalMetadata;
using vehicle_signals::SignalReading;
using vehicle_signals::SignalType;
using vehicle_signals::SignalUnit;
using vehicle_signals::SignalValue;
using vehicle_signals::ValidationStatus;

constexpr std::string_view kNull{"null"};

void append_unsigned(std::string &line, const std::uint64_t value) {
  char buffer[24]{};
  const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
  line.append(buffer, static_cast<std::size_t>(result.ptr - buffer));
}

// Shortest round-trip decimal form, independent of the global locale.
void append_number(std::string &line, const float value) {
  if (!std::isfinite(value)) {
    line.append(kNull);
    return;
  }
  char buffer[48]{};
  const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
  line.append(buffer, static_cast<std::size_t>(result.ptr - buffer));
}

// JSON string for the catalog's static keys and choice names, which are ASCII.
// Quotes, backslashes and control bytes are escaped; bytes >= 0x80 are copied
// as-is without UTF-8 validation, so a non-ASCII catalog string is only valid
// JSON if it is already well-formed UTF-8.
void append_string(std::string &line, const std::string_view text) {
  line.push_back('"');
  for (const char character : text) {
    const auto byte = static_cast<unsigned char>(character);
    if (character == '"' || character == '\\') {
      line.push_back('\\');
      line.push_back(character);
    } else if (character == '\n') {
      line.append("\\n");
    } else if (byte < 0x20U) {
      char escaped[8]{};
      std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned>(byte));
      line.append(escaped);
    } else {
      line.push_back(character);
    }
  }
  line.push_back('"');
}

void append_enumeration(std::string &line, const SignalMetadata &signal,
                        const std::uint16_t value) {
  const auto *choice = signal.find_choice(value);
  if (choice == nullptr) {
    append_unsigned(line, value);
    return;
  }
  append_string(line, choice->key);
}

void append_value(std::string &line, const SignalMetadata &signal,
                  const std::optional<SignalValue> &value) {
  if (!value) {
    line.append(kNull);
    return;
  }
  switch (value->type()) {
  case SignalType::Boolean:
    line.append(*value->as_boolean() ? "true" : "false");
    return;
  case SignalType::Number:
    append_number(line, *value->as_number());
    return;
  case SignalType::Enum:
    append_enumeration(line, signal, *value->as_enumeration());
    return;
  }
  line.append(kNull);
}

[[nodiscard]] std::string_view unit_name(const SignalUnit unit) noexcept {
  switch (unit) {
  case SignalUnit::KilometresPerHour:
    return "\"km/h\"";
  case SignalUnit::RevolutionsPerMinute:
    return "\"rpm\"";
  case SignalUnit::MetresPerSecondSquared:
    return "\"m/s^2\"";
  case SignalUnit::None:
    break;
  }
  return kNull;
}

[[nodiscard]] std::string_view freshness_name(const Availability availability) noexcept {
  switch (availability) {
  case Availability::Fresh:
    return "\"fresh\"";
  case Availability::Stale:
    return "\"stale\"";
  case Availability::FreshnessUnverified:
    return "\"unverified\"";
  case Availability::NoData:
  case Availability::Unavailable:
    break;
  }
  return kNull;
}

[[nodiscard]] std::string_view availability_name(const Availability availability) noexcept {
  switch (availability) {
  case Availability::NoData:
    return "\"no_data\"";
  case Availability::Fresh:
    return "\"fresh\"";
  case Availability::Stale:
    return "\"stale\"";
  case Availability::FreshnessUnverified:
    return "\"freshness_unverified\"";
  case Availability::Unavailable:
    return "\"unavailable\"";
  }
  return kNull;
}

[[nodiscard]] std::string_view validation_name(const ValidationStatus validation) noexcept {
  switch (validation) {
  case ValidationStatus::Reference:
    return "\"reference\"";
  case ValidationStatus::Observed:
    return "\"observed\"";
  case ValidationStatus::Confirmed:
    return "\"confirmed\"";
  }
  return kNull;
}

[[nodiscard]] std::string format_record(const vehicle_core::MonotonicTimestamp time_us,
                                        const SignalMetadata &signal,
                                        const SignalReading &reading) {
  std::string line;
  line.reserve(192);
  line.append("{\"type\":\"signal\",\"timestamp_us\":");
  append_unsigned(line, time_us);
  line.append(",\"signal\":");
  append_string(line, signal.key);
  line.append(",\"value\":");
  append_value(line, signal, reading.value);
  line.append(",\"unit\":");
  line.append(unit_name(signal.unit));
  line.append(",\"freshness\":");
  line.append(freshness_name(reading.availability));
  line.append(",\"availability\":");
  line.append(availability_name(reading.availability));
  line.append(",\"validation\":");
  line.append(validation_name(reading.validation));
  line.append("}\n");
  return line;
}

} // namespace

void JsonlSignalRecordWriter::on_catalog(vehicle_signals::SignalCatalogView) noexcept {}

void JsonlSignalRecordWriter::on_reading(const vehicle_core::MonotonicTimestamp time_us,
                                         const SignalMetadata &signal,
                                         const SignalReading &reading) noexcept {
  if (!good())
    return;
  try {
    const auto line = format_record(time_us, signal, reading);
    output_->write(line.data(), static_cast<std::streamsize>(line.size()));
  } catch (...) {
    good_ = false;
  }
}

} // namespace replay
