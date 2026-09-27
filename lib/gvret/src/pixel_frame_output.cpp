#include "gvret/pixel_frame_output.hpp"

#include <charconv>
#include <cstdint>
#include <string_view>

namespace gvret {
namespace {

bool write_literal(std::ostream &output, const std::string_view text) {
  output.write(text.data(), static_cast<std::streamsize>(text.size()));
  return output.good();
}

bool write_decimal(std::ostream &output, const std::uint64_t value) {
  char buffer[32]{};
  const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value, 10);
  if (result.ec != std::errc{})
    return false;
  output.write(buffer, result.ptr - buffer);
  return output.good();
}

bool write_rgb(std::ostream &output, const local_argb::Rgb pixel) {
  return write_literal(output, "[") && write_decimal(output, pixel.red) &&
         write_literal(output, ",") && write_decimal(output, pixel.green) &&
         write_literal(output, ",") && write_decimal(output, pixel.blue) &&
         write_literal(output, "]");
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

bool JsonlPixelFrameSink::write(const local_argb::PixelFrame &frame) noexcept {
  try {
    if (!write_literal(*output_, "{\"timestamp_us\":") || !write_decimal(*output_, clock_->now()) ||
        !write_literal(*output_, ",\"pixels\":["))
      return false;
    for (std::size_t index = 0; index < frame.size(); ++index) {
      if (index != 0)
        if (!write_literal(*output_, ","))
          return false;
      if (!write_rgb(*output_, frame[index]))
        return false;
    }
    return write_literal(*output_, "]}\n");
  } catch (...) {
    return false;
  }
}

} // namespace gvret
