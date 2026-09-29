#include "gvret/csv_parser.hpp"

#include <charconv>
#include <limits>
#include <string>

namespace gvret {
namespace {

constexpr std::string_view kLegacyHeader = "Time Stamp,ID,Extended,Bus,LEN,D1,D2,D3,D4,D5,D6,D7,D8";
constexpr std::string_view kSavvyCanV2Header =
    "Time Stamp,ID,Extended,Dir,Bus,LEN,D1,D2,D3,D4,D5,D6,D7,D8";
constexpr std::size_t kPayloadColumns = vehicle_core::kCanClassicPayloadBytes;

enum class CsvSchema : std::uint8_t { V1, V2 };

struct CsvLayout {
  bool has_direction;
  std::size_t direction_column;
  std::size_t bus_column;
  std::size_t dlc_column;
  std::size_t first_payload_column;
};

constexpr CsvLayout kV1Layout{false, 0, 3, 4, 5};
constexpr CsvLayout kV2Layout{true, 3, 4, 5, 6};

constexpr const CsvLayout &layout_for(const CsvSchema schema) noexcept {
  return schema == CsvSchema::V2 ? kV2Layout : kV1Layout;
}

struct Fields {
  std::vector<std::string_view> values;
};

Fields split_csv_line(const std::string_view line) {
  Fields fields;
  std::size_t begin = 0;
  while (true) {
    const std::size_t comma = line.find(',', begin);
    if (comma == std::string_view::npos) {
      fields.values.push_back(line.substr(begin));
      return fields;
    }
    fields.values.push_back(line.substr(begin, comma - begin));
    begin = comma + 1;
  }
}

std::string_view without_carriage_return(const std::string_view line) {
  if (!line.empty() && line.back() == '\r') {
    return line.substr(0, line.size() - 1);
  }
  return line;
}

template <typename Integer>
bool parse_integer(const std::string_view text, const int base, Integer &value) {
  if (text.empty()) {
    return false;
  }

  Integer parsed{};
  const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed, base);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
    return false;
  }
  value = parsed;
  return true;
}

bool parse_extended(const std::string_view text, bool &value) {
  if (text == "0" || text == "false" || text == "False" || text == "FALSE") {
    value = false;
    return true;
  }
  if (text == "1" || text == "true" || text == "True" || text == "TRUE") {
    value = true;
    return true;
  }
  return false;
}

bool parse_direction(const std::string_view text, Direction &value) {
  if (text == "Rx") {
    value = Direction::Rx;
    return true;
  }
  if (text == "Tx") {
    value = Direction::Tx;
    return true;
  }
  return false;
}

ParseResult failure(const std::size_t line_number, std::string reason) {
  ParseResult result;
  result.error = ParseError{line_number, std::move(reason)};
  return result;
}

} // namespace

std::string ParseError::message() const {
  return "line " + std::to_string(line_number) + ": " + reason;
}

ParseResult parse_csv(const std::string_view csv) {
  ParseResult result;
  std::size_t line_number = 1;
  std::size_t line_begin = 0;
  bool saw_header = false;
  CsvSchema schema = CsvSchema::V1;

  while (line_begin <= csv.size()) {
    const std::size_t newline = csv.find('\n', line_begin);
    const std::size_t line_end = newline == std::string_view::npos ? csv.size() : newline;
    const std::string_view line =
        without_carriage_return(csv.substr(line_begin, line_end - line_begin));

    // A final newline is normal CSV framing and does not represent an empty
    // record. Empty records in the middle remain errors with their line number.
    const bool final_empty_line = line.empty() && line_begin == csv.size();
    if (final_empty_line) {
      break;
    }

    if (!saw_header) {
      if (line != kLegacyHeader && line != kSavvyCanV2Header) {
        return failure(line_number, "malformed GVRET header");
      }
      schema = line == kSavvyCanV2Header ? CsvSchema::V2 : CsvSchema::V1;
      saw_header = true;
    } else {
      if (line.empty()) {
        return failure(line_number, "empty CSV row");
      }

      const Fields fields = split_csv_line(line);
      const CsvLayout &layout = layout_for(schema);
      if (fields.values.size() < layout.first_payload_column) {
        return failure(line_number, "row is missing required columns");
      }

      std::uint64_t timestamp_us = 0;
      if (!parse_integer(fields.values[0], 10, timestamp_us)) {
        return failure(line_number, "invalid timestamp");
      }

      std::uint32_t identifier = 0;
      if (!parse_integer(fields.values[1], 16, identifier)) {
        return failure(line_number, "invalid CAN identifier");
      }

      bool extended = false;
      if (!parse_extended(fields.values[2], extended)) {
        return failure(line_number, "invalid Extended flag");
      }

      Direction direction = Direction::Unknown;
      if (layout.has_direction) {
        if (!parse_direction(fields.values[layout.direction_column], direction)) {
          return failure(line_number, "invalid direction; expected Rx or Tx");
        }
      } else {
        direction = Direction::Rx;
      }

      std::uint32_t bus = 0;
      if (!parse_integer(fields.values[layout.bus_column], 10, bus)) {
        return failure(line_number, "invalid bus number");
      }

      std::uint32_t dlc = 0;
      if (!parse_integer(fields.values[layout.dlc_column], 10, dlc) || dlc > kPayloadColumns) {
        return failure(line_number, "invalid DLC; expected a value from 0 to 8");
      }

      std::size_t field_count = fields.values.size();
      if (field_count > layout.first_payload_column && fields.values.back().empty()) {
        --field_count;
      }

      if (field_count > layout.first_payload_column + kPayloadColumns) {
        return failure(line_number, "row contains unsupported columns");
      }

      if (field_count < layout.first_payload_column + dlc) {
        return failure(line_number, "row is missing a required payload byte");
      }

      ParsedGvretFrame parsed;
      parsed.timestamp_us = timestamp_us;
      parsed.bus = bus;
      parsed.direction = direction;
      parsed.frame.timestamp_us = timestamp_us;
      // ParsedGvretFrame::bus retains the complete source value. Keep the
      // embedded frame's narrow bus_id neutral until A2 selects replay-bus
      // semantics; narrowing here would alias distinct source buses.
      parsed.frame.identifier = identifier;
      parsed.frame.identifier_format = extended ? vehicle_core::CanIdentifierFormat::Extended
                                                : vehicle_core::CanIdentifierFormat::Standard;
      parsed.frame.dlc = static_cast<std::uint8_t>(dlc);

      for (std::size_t index = 0; index < dlc; ++index) {
        const std::string_view byte_text = fields.values[layout.first_payload_column + index];
        if (byte_text.empty() || byte_text.size() > 2) {
          return failure(line_number, "invalid hexadecimal payload byte");
        }
        std::uint32_t byte = 0;
        if (!parse_integer(byte_text, 16, byte) ||
            byte > std::numeric_limits<std::uint8_t>::max()) {
          return failure(line_number, "invalid hexadecimal payload byte");
        }
        parsed.frame.data[index] = static_cast<std::uint8_t>(byte);
      }

      if (!parsed.frame.is_valid()) {
        return failure(line_number, "CAN frame is outside the classic CAN range");
      }
      result.frames.push_back(parsed);
    }

    if (newline == std::string_view::npos) {
      break;
    }
    line_begin = newline + 1;
    ++line_number;
  }

  if (!saw_header) {
    return failure(1, "missing GVRET header");
  }
  return result;
}

} // namespace gvret
