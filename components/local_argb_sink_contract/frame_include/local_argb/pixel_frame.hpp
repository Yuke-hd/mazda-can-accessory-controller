#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace local_argb {

// Renderer-independent pixel output contract. Frame consumers (serializers,
// recorders, emulators) depend on these values and the sink interface only,
// without the renderer, ESP-IDF, or vehicle code.
struct Rgb {
  std::uint8_t red{0};
  std::uint8_t green{0};
  std::uint8_t blue{0};
};

inline constexpr Rgb kBlack{};

constexpr bool operator==(const Rgb &left, const Rgb &right) noexcept {
  return left.red == right.red && left.green == right.green && left.blue == right.blue;
}
constexpr bool operator!=(const Rgb &left, const Rgb &right) noexcept { return !(left == right); }

inline constexpr std::size_t kLedCount = 100;

using PixelFrame = std::array<Rgb, kLedCount>;
inline constexpr PixelFrame kBlackFrame{};

// Receives each whole frame the renderer produces; implemented by strip
// drivers, recorders, and serializers.
class PixelFrameSink {
public:
  virtual ~PixelFrameSink() = default;
  virtual bool write(const PixelFrame &frame) noexcept = 0;
};

} // namespace local_argb
