#pragma once

#include <cstdint>

namespace companion_protocol {

enum class UserKeyLevel : std::uint8_t { Released, Pressed };

// Consecutive equal samples that make a level change stable. At a 20 ms
// sample period a press must last 60 ms.
inline constexpr std::uint8_t kUserKeyDebounceSamples = 3U;

// Turns periodic user key samples into debounced presses. The key starts as
// pressed, so a key held since boot never counts: it must be released and
// pressed again. GPIO0 held during reset enters the ROM bootloader anyway.
class UserKeyDebouncer final {
public:
  // Returns true once for each debounced press that follows a debounced
  // release.
  [[nodiscard]] bool sample(UserKeyLevel level) noexcept;

private:
  UserKeyLevel stable_{UserKeyLevel::Pressed};
  std::uint8_t changed_samples_{0U};
};

} // namespace companion_protocol
