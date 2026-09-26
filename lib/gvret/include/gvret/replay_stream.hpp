#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "gvret/csv_parser.hpp"

namespace gvret {

// A parsed frame with a replay-relative schedule time. The frame timestamp is
// rewritten to the same relative value so downstream consumers never observe
// an absolute capture timestamp. RawCanFrame::bus_id remains at the neutral
// parser value; the selected source bus is retained only as a filtering input.
struct TimedCanFrame {
  vehicle_core::Microseconds relative_time_us{0};
  vehicle_core::RawCanFrame frame{};
};

enum class ReplayErrorCode : std::uint8_t { TimestampRegression };

struct ReplayError {
  ReplayErrorCode code{ReplayErrorCode::TimestampRegression};
  std::size_t source_index{0};

  [[nodiscard]] std::string message() const;
};

struct ReplayResult {
  std::vector<TimedCanFrame> frames;
  std::optional<ReplayError> error;

  [[nodiscard]] bool ok() const noexcept { return !error.has_value(); }
  explicit operator bool() const noexcept { return ok(); }
};

struct ReplayOptions {
  std::uint32_t bus{0};
};

// Select one source bus and normalize its timestamps to a zero-based replay
// clock. Rows from other buses are skipped in source order. Equal timestamps
// are valid and retain their source-file order. A timestamp regression in the
// selected bus fails the whole transformation rather than producing a stream
// with an ambiguous unsigned time delta.
[[nodiscard]] ReplayResult prepare_replay(const std::vector<ParsedGvretFrame> &parsed,
                                          ReplayOptions options = {});

// Convenience overload for callers that only need to select a bus.
[[nodiscard]] ReplayResult prepare_replay(const std::vector<ParsedGvretFrame> &parsed,
                                          std::uint32_t selected_bus);

} // namespace gvret
