#include "gvret/file_loader.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <utility>

namespace gvret {
namespace {

FileReplayResult failed(const FileErrorCode code, std::optional<ParseError> parse_error = {},
                        std::optional<ReplayError> replay_error = {}) {
  FileReplayResult result;
  result.error = FileError{code, std::move(parse_error), std::move(replay_error)};
  return result;
}

} // namespace

std::string FileError::message() const {
  switch (code) {
  case FileErrorCode::OpenFailed:
    return "unable to open GVRET input file";
  case FileErrorCode::ReadFailed:
    return "unable to read GVRET input file";
  case FileErrorCode::ParseFailed:
    return parse_error.has_value() ? "GVRET parse error: " + parse_error->message()
                                   : "GVRET parse error";
  case FileErrorCode::ReplayFailed:
    return replay_error.has_value() ? "GVRET replay error: " + replay_error->message()
                                    : "GVRET replay error";
  }
  return "GVRET file error";
}

FileReplayResult load_file(const std::filesystem::path &path, const ReplayOptions options) {
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    return failed(FileErrorCode::OpenFailed);
  }

  std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  if (input.bad()) {
    return failed(FileErrorCode::ReadFailed);
  }

  const ParseResult parsed = parse_csv(contents);
  if (!parsed.ok()) {
    return failed(FileErrorCode::ParseFailed, parsed.error);
  }

  const ReplayResult replay = prepare_replay(parsed.frames, options);
  if (!replay.ok()) {
    return failed(FileErrorCode::ReplayFailed, {}, replay.error);
  }

  FileReplayResult result;
  result.frames = replay.frames;
  return result;
}

ReplaySummary summarize(const std::vector<TimedCanFrame> &frames,
                        const std::uint32_t selected_bus) {
  ReplaySummary summary;
  summary.selected_bus = selected_bus;
  summary.frame_count = frames.size();
  if (!frames.empty()) {
    summary.relative_duration_us = frames.back().relative_time_us;
  }

  for (const TimedCanFrame &timed : frames) {
    if (timed.frame.is_extended()) {
      ++summary.extended_frame_count;
    } else {
      ++summary.standard_frame_count;
    }

    summary.min_identifier = summary.min_identifier.has_value()
                                 ? std::min(*summary.min_identifier, timed.frame.identifier)
                                 : timed.frame.identifier;
    summary.max_identifier = summary.max_identifier.has_value()
                                 ? std::max(*summary.max_identifier, timed.frame.identifier)
                                 : timed.frame.identifier;
  }

  return summary;
}

} // namespace gvret
