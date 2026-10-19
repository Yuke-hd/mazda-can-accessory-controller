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

// The frame buffer holds the longest strip the renderer can drive. The active
// length is how many pixels a normal frame covers; it is fixed at the default
// until the strip length becomes configurable.
inline constexpr std::size_t kMaxLedCount = 300;
inline constexpr std::size_t kDefaultLedCount = 100;
static_assert(kDefaultLedCount <= kMaxLedCount);

using PixelFrame = std::array<Rgb, kMaxLedCount>;
inline constexpr PixelFrame kBlackFrame{};

// Receives each frame the renderer produces; implemented by strip drivers,
// recorders, and serializers. `length` is how many leading pixels the write
// covers: the active length for a lit frame, and kMaxLedCount for every black
// frame so no pixel of the buffer can stay lit.
class PixelFrameSink {
public:
  virtual ~PixelFrameSink() = default;
  virtual bool write(const PixelFrame &frame, std::size_t length) noexcept = 0;
};

} // namespace local_argb
