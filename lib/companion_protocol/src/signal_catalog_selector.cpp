#include "companion_protocol/signal_catalog_selector.hpp"

namespace companion_protocol {
namespace {

constexpr std::uint8_t kFormatOffset = 0U;
constexpr std::uint8_t kTokenOffset = 1U;
constexpr std::uint8_t kDocumentOffset = 5U;

std::uint32_t read_u32(const ByteView bytes, const std::size_t offset) noexcept {
  return static_cast<std::uint32_t>(bytes[offset]) |
         (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
         (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
         (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

std::uint16_t read_u16(const ByteView bytes, const std::size_t offset) noexcept {
  return static_cast<std::uint16_t>(bytes[offset]) |
         (static_cast<std::uint16_t>(bytes[offset + 1U]) << 8U);
}

} // namespace

void SignalCatalogSelector::bind(const SignalCatalogExport &catalog) noexcept {
  catalog_ = &catalog;
  reset_connection();
}

void SignalCatalogSelector::connect(const std::uint32_t connection,
                                    const std::uint32_t token) noexcept {
  if (connection_ != kNoConnection || token == 0U)
    return;
  connection_ = connection;
  token_ = token;
  page_offset_ = 0U;
}

void SignalCatalogSelector::disconnect(const std::uint32_t connection) noexcept {
  if (connection == connection_)
    reset_connection();
}

AttError SignalCatalogSelector::select(const std::uint32_t connection,
                                       const ByteView selector) noexcept {
  if (selector.size() != kSignalCatalogSelectorBytes)
    return AttError::InvalidAttributeValueLength;
  if (selector[kFormatOffset] != kSignalCatalogFormat)
    return AttError::UnsupportedOperation;
  if (!active(connection) || catalog_ == nullptr || !catalog_->available() ||
      read_u32(selector, kTokenOffset) != token_)
    return AttError::InvalidState;

  const std::uint16_t offset = read_u16(selector, kDocumentOffset);
  if (offset > catalog_->document_length())
    return AttError::InvalidPdu;

  page_offset_ = offset;
  return AttError::None;
}

std::optional<EncodedSignalCatalogPage>
SignalCatalogSelector::read(const std::uint32_t connection) const noexcept {
  if (!active(connection) || catalog_ == nullptr || !catalog_->available())
    return std::nullopt;
  return catalog_->encode_page(token_, page_offset_);
}

std::uint32_t
SignalCatalogSelector::token_for_connection(const std::uint32_t connection) const noexcept {
  return active(connection) ? token_ : 0U;
}

bool SignalCatalogSelector::active(const std::uint32_t connection) const noexcept {
  return connection_ != kNoConnection && connection == connection_ && token_ != 0U;
}

void SignalCatalogSelector::reset_connection() noexcept {
  connection_ = kNoConnection;
  token_ = 0U;
  page_offset_ = 0U;
}

} // namespace companion_protocol
