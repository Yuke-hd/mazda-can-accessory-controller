#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "vehicle_core/frame.hpp"

namespace gvret {

// A parsed GVRET row with the source bus retained for downstream filtering.
// `bus` is the sole source-bus authority. The embedded frame keeps its
// RawCanFrame default bus_id until replay selects the frame-bus semantics.
// The frame timestamp and convenience timestamp_us field are both kept in the
// source unit. Timestamp normalization belongs to the replay layer.
struct ParsedGvretFrame {
  std::uint64_t timestamp_us{0};
  std::uint32_t bus{0};
  vehicle_core::RawCanFrame frame{};
};

struct ParseError {
  std::size_t line_number{0};
  std::string reason;

  [[nodiscard]] std::string message() const;
};

struct ParseResult {
  std::vector<ParsedGvretFrame> frames;
  std::optional<ParseError> error;

  [[nodiscard]] bool ok() const noexcept { return !error.has_value(); }
  explicit operator bool() const noexcept { return ok(); }
};

// Parse a complete SavvyCAN/GVRET CSV stream. The parser accepts the
// following header exactly (apart from a CR line ending):
//
//   Time Stamp,ID,Extended,Bus,LEN,D1,D2,D3,D4,D5,D6,D7,D8
//
// Timestamps and buses are unsigned decimal values. IDs and payload bytes are
// hexadecimal. The first LEN payload columns are required and validated;
// remaining D columns may be populated by SavvyCAN and are ignored.
[[nodiscard]] ParseResult parse_csv(std::string_view csv);

} // namespace gvret
