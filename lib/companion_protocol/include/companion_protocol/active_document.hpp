#pragma once

#include <cstdint>
#include <optional>

#include "companion_protocol/bytes.hpp"

namespace companion_protocol {

// Active config source, shared by Config status and read-back.
enum class ConfigSource : std::uint8_t {
  Factory = 0,
  Override = 1,
  None = 0xFF,
};

// The canonical serialization of the config this boot selected, with its
// CRC-32 computed once. It is the single source of the active source, length
// and CRC that Config status, read-back pages and Select read page report, so
// they cannot disagree. It stays the same until the next restart. The viewed
// bytes must outlive every copy.
class ActiveDocument final {
public:
  // No config was selected during this boot: source 0xFF, length and CRC 0.
  constexpr ActiveDocument() noexcept = default;
  // std::nullopt when `source` is None, or the document exceeds 65535 bytes.
  [[nodiscard]] static std::optional<ActiveDocument> selected(ConfigSource source,
                                                              ByteView canonical) noexcept;

  [[nodiscard]] constexpr ConfigSource source() const noexcept { return source_; }
  [[nodiscard]] constexpr ByteView bytes() const noexcept { return bytes_; }
  [[nodiscard]] std::uint16_t length() const noexcept {
    return static_cast<std::uint16_t>(bytes_.size());
  }
  [[nodiscard]] constexpr std::uint32_t crc32() const noexcept { return crc32_; }

private:
  ConfigSource source_{ConfigSource::None};
  ByteView bytes_{};
  std::uint32_t crc32_{0};
};

} // namespace companion_protocol
