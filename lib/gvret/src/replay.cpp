#include "gvret/replay_stream.hpp"

#include <utility>

namespace gvret {

std::string ReplayError::message() const {
  switch (code) {
  case ReplayErrorCode::TimestampRegression:
    return "selected GVRET bus has a timestamp regression at source index " +
           std::to_string(source_index);
  }
  return "invalid GVRET replay stream at source index " + std::to_string(source_index);
}

ReplayResult prepare_replay(const std::vector<ParsedGvretFrame> &parsed,
                            const ReplayOptions options) {
  ReplayResult result;
  std::optional<std::uint64_t> first_timestamp;
  std::optional<std::uint64_t> previous_timestamp;

  for (std::size_t source_index = 0; source_index < parsed.size(); ++source_index) {
    const ParsedGvretFrame &source = parsed[source_index];
    if (source.bus != options.bus) {
      continue;
    }

    if (previous_timestamp.has_value() && source.timestamp_us < *previous_timestamp) {
      result.frames.clear();
      result.error = ReplayError{ReplayErrorCode::TimestampRegression, source_index};
      return result;
    }

    if (!first_timestamp.has_value()) {
      first_timestamp = source.timestamp_us;
    }
    previous_timestamp = source.timestamp_us;

    const auto relative_time = source.timestamp_us - *first_timestamp;
    TimedCanFrame timed;
    timed.relative_time_us = relative_time;
    timed.frame = source.frame;
    // The parser timestamp is an absolute source observation value. Replace
    // it before the frame crosses into replay consumers.
    timed.frame.timestamp_us = relative_time;
    result.frames.push_back(std::move(timed));
  }

  return result;
}

ReplayResult prepare_replay(const std::vector<ParsedGvretFrame> &parsed,
                            const std::uint32_t selected_bus) {
  return prepare_replay(parsed, ReplayOptions{selected_bus});
}

} // namespace gvret
