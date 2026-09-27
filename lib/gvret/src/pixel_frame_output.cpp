#include "gvret/pixel_frame_output.hpp"

#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>

namespace gvret {
namespace {

bool append_literal(std::string &output, const std::string_view text) {
  output.append(text.data(), text.size());
  return true;
}

bool append_decimal(std::string &output, const std::uint64_t value) {
  char buffer[32]{};
  const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value, 10);
  if (result.ec != std::errc{})
    return false;
  output.append(buffer, result.ptr - buffer);
  return true;
}

bool append_rgb(std::string &output, const local_argb::Rgb pixel) {
  return append_literal(output, "[") && append_decimal(output, pixel.red) &&
         append_literal(output, ",") && append_decimal(output, pixel.green) &&
         append_literal(output, ",") && append_decimal(output, pixel.blue) &&
         append_literal(output, "]");
}

} // namespace

bool TimestampedPixelFrameSink::write(const local_argb::PixelFrame &frame) noexcept {
  try {
    frames_.push_back({clock_->now(), frame});
    return true;
  } catch (...) {
    return false;
  }
}

bool JsonlPixelFrameSink::write_header() noexcept {
  if (wrote_header_)
    return output_->good();
  if (wrote_end_)
    return false;
  try {
    std::string line;
    line.reserve(64);
    append_literal(line, "{\"type\":\"header\",\"version\":");
    append_decimal(line, kJsonlFormatVersion);
    append_literal(line, ",\"pixel_count\":");
    append_decimal(line, local_argb::kLedCount);
    append_literal(line, "}\n");
    output_->write(line.data(), static_cast<std::streamsize>(line.size()));
    if (!output_->good())
      return false;
    wrote_header_ = true;
    return true;
  } catch (...) {
    return false;
  }
}

bool JsonlPixelFrameSink::write_end() noexcept {
  if (wrote_end_)
    return output_->good();
  try {
    if (!write_header())
      return false;
    constexpr std::string_view kEndRecord{"{\"type\":\"end\"}\n"};
    output_->write(kEndRecord.data(), static_cast<std::streamsize>(kEndRecord.size()));
    if (!output_->good())
      return false;
    wrote_end_ = true;
    return true;
  } catch (...) {
    return false;
  }
}

bool JsonlPixelFrameSink::write(const local_argb::PixelFrame &frame) noexcept {
  try {
    if (wrote_end_ || !write_header())
      return false;
    std::string line;
    line.reserve(64 + frame.size() * 16);
    append_literal(line, "{\"type\":\"pixels\",\"timestamp_us\":");
    if (!append_decimal(line, clock_->now()) || !append_literal(line, ",\"pixels\":["))
      return false;
    for (std::size_t index = 0; index < frame.size(); ++index) {
      if (index != 0)
        if (!append_literal(line, ","))
          return false;
      if (!append_rgb(line, frame[index]))
        return false;
    }
    append_literal(line, "]}\n");
    output_->write(line.data(), static_cast<std::streamsize>(line.size()));
    return output_->good();
  } catch (...) {
    return false;
  }
}

} // namespace gvret
