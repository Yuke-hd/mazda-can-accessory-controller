#pragma once

#include "companion_protocol/bytes.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace companion_protocol {

inline constexpr std::uint8_t kOutputCatalogFormat = 1;
inline constexpr std::size_t kMaxOutputCatalogDocumentBytes = 16384;
inline constexpr std::size_t kOutputCatalogPageHeaderBytes = 11;
inline constexpr std::size_t kOutputCatalogPageDataBytes = 200;
inline constexpr std::size_t kMaxOutputCatalogPageBytes =
    kOutputCatalogPageHeaderBytes + kOutputCatalogPageDataBytes;
using EncodedOutputCatalogPage = BoundedBytes<kMaxOutputCatalogPageBytes>;

// Non-owning page source for a generated, validated format-1 JSON document.
// The document must remain immutable and outlive this export and its selector.
// This transport checks size only; JSON validation belongs to the generator.
class OutputCatalogExport final {
public:
  explicit OutputCatalogExport(ByteView document) noexcept : document_(document) {}

  [[nodiscard]] bool available() const noexcept {
    return !document_.empty() && document_.size() <= kMaxOutputCatalogDocumentBytes;
  }
  [[nodiscard]] std::uint16_t document_length() const noexcept {
    return available() ? static_cast<std::uint16_t>(document_.size()) : 0U;
  }

  // A nonzero connection token is required. Offset equal to the document
  // length produces an empty terminal page; invalid input returns no page.
  [[nodiscard]] std::optional<EncodedOutputCatalogPage>
  encode_page(std::uint32_t token, std::uint16_t offset) const noexcept;

private:
  ByteView document_;
};

} // namespace companion_protocol
