#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "companion_protocol/bytes.hpp"
#include "companion_protocol/config_status.hpp"

namespace companion_protocol {

inline constexpr std::size_t kReadBackHeaderBytes = 9;
inline constexpr std::size_t kMaxReadBackDataBytes = 200;
inline constexpr std::size_t kMaxReadBackPageBytes = kReadBackHeaderBytes + kMaxReadBackDataBytes;

// The canonical serialization of the config this boot selected, with its
// CRC-32 computed once. It stays the same until the next restart. The viewed
// bytes must outlive the document.
class ReadBackDocument final {
public:
  // No config was selected during this boot: source 0xFF, length and CRC 0.
  constexpr ReadBackDocument() noexcept = default;
  // std::nullopt when `source` is None, or the document exceeds 65535 bytes.
  [[nodiscard]] static std::optional<ReadBackDocument> active(ConfigSource source,
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

using EncodedReadBackPage = BoundedBytes<kMaxReadBackPageBytes>;

// Encodes the read-back page at `page_offset`: the 9-byte header and up to
// 200 data bytes. An offset equal to the length gives a page with no data;
// a larger offset gives std::nullopt.
[[nodiscard]] std::optional<EncodedReadBackPage>
encode_read_back_page(const ReadBackDocument &document, std::uint16_t page_offset) noexcept;

} // namespace companion_protocol
