#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "gvret/replay_stream.hpp"

namespace gvret {

enum class FileErrorCode : std::uint8_t { OpenFailed, ReadFailed, ParseFailed, ReplayFailed };

struct FileError {
  FileErrorCode code{FileErrorCode::OpenFailed};
  std::optional<ParseError> parse_error;
  std::optional<ReplayError> replay_error;

  [[nodiscard]] std::string message() const;
};

struct FileReplayResult {
  std::vector<TimedCanFrame> frames;
  std::optional<FileError> error;

  [[nodiscard]] bool ok() const noexcept { return !error.has_value(); }
  explicit operator bool() const noexcept { return ok(); }
};

// Open a caller-supplied host file, parse it as GVRET CSV, and prepare one
// selected bus for replay. The path and file contents stay within the host
// tool; the returned stream contains only validated, relative-time frames.
[[nodiscard]] FileReplayResult load_file(const std::filesystem::path &path,
                                         ReplayOptions options = {});

struct ReplaySummary {
  std::uint32_t selected_bus{0};
  std::size_t frame_count{0};
  vehicle_core::Microseconds relative_duration_us{0};
  std::size_t standard_frame_count{0};
  std::size_t extended_frame_count{0};
  std::optional<std::uint32_t> min_identifier;
  std::optional<std::uint32_t> max_identifier;
};

[[nodiscard]] ReplaySummary summarize(const std::vector<TimedCanFrame> &frames,
                                      std::uint32_t selected_bus);

} // namespace gvret
