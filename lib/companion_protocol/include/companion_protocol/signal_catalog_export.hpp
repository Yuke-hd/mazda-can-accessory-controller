#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "companion_protocol/bytes.hpp"
#include "vehicle_signals/signal_catalog.hpp"
#include "vehicle_signals/signal_contracts.hpp"

namespace companion_protocol {

inline constexpr std::uint8_t kSignalCatalogFormat = 1;
inline constexpr std::size_t kMaxSignalCatalogSignals = 64;
inline constexpr std::size_t kMaxSignalCatalogChoices = 32;
inline constexpr std::size_t kMaxSignalCatalogKeyBytes = 63;
inline constexpr std::size_t kMaxSignalCatalogTokenBytes = 31;
inline constexpr std::size_t kMaxSignalCatalogDocumentBytes = 16U * 1024U;
inline constexpr std::size_t kSignalCatalogPageHeaderBytes = 11;
inline constexpr std::size_t kSignalCatalogPageDataBytes = 200;
inline constexpr std::size_t kMaxSignalCatalogPageBytes =
    kSignalCatalogPageHeaderBytes + kSignalCatalogPageDataBytes;

using EncodedSignalCatalogPage = BoundedBytes<kMaxSignalCatalogPageBytes>;

// A validated, non-owning export of the provider's Read-capable Boolean,
// Number, and Enum metadata. The provider catalog, its strings, and Enum
// choices must outlive this object and remain immutable, as required by the
// provider catalog contract. Invalid metadata or any exceeded wire bound
// makes the entire export unavailable; no partial catalog is exposed.
// Keep one export for the service lifetime: its handles and document are
// stable across reconnects. Clients rediscover every page on each reconnect,
// including when the connection token repeats; paging never renegotiates handles.
class SignalCatalogExport final {
public:
  explicit SignalCatalogExport(vehicle_signals::SignalCatalogView catalog) noexcept;

  [[nodiscard]] bool available() const noexcept { return available_; }
  [[nodiscard]] std::uint16_t signal_count() const noexcept {
    return available_ ? signal_count_ : 0;
  }
  [[nodiscard]] std::uint16_t document_length() const noexcept {
    return available_ ? document_length_ : 0;
  }

  // Returns the selected catalog page: format, token, total length, offset,
  // data length, and up to 200 document bytes. A nonzero connection token is
  // required. Offset equal to document_length() returns an empty terminal
  // page; an unavailable export or larger offset returns std::nullopt.
  [[nodiscard]] std::optional<EncodedSignalCatalogPage>
  encode_page(std::uint32_t token, std::uint16_t offset) const noexcept;

  // Local provider lookup helpers for consumers that encode values after
  // discovery. These runtime identifiers and raw Enum values are never
  // written to the catalog page.
  [[nodiscard]] vehicle_signals::SignalId
  provider_id_for_handle(std::uint16_t wire_handle) const noexcept;
  [[nodiscard]] std::optional<std::uint16_t>
  wire_choice_handle(std::uint16_t wire_handle, std::uint16_t provider_choice_value) const noexcept;

private:
  [[nodiscard]] const vehicle_signals::SignalMetadata *
  metadata_for_handle(std::uint16_t wire_handle) const noexcept;

  std::array<const vehicle_signals::SignalMetadata *, kMaxSignalCatalogSignals> entries_{};
  std::uint16_t signal_count_{0};
  std::uint16_t document_length_{0};
  bool available_{false};
};

} // namespace companion_protocol
