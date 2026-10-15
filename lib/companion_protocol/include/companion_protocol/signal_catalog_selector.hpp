#pragma once

#include "companion_protocol/att_error.hpp"
#include "companion_protocol/bytes.hpp"
#include "companion_protocol/signal_catalog_export.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace companion_protocol {

inline constexpr std::size_t kSignalCatalogSelectorBytes = 7U;

// Portable state and validation for the protected catalog selector. The BLE
// binding supplies a connection handle and a per-connection token; this class
// owns the selected offset and never reaches the provider or a configuration
// store.
class SignalCatalogSelector final {
public:
  SignalCatalogSelector() noexcept = default;
  explicit SignalCatalogSelector(const SignalCatalogExport &catalog) noexcept
      : catalog_(&catalog) {}

  // Rebinds the immutable export and clears any connection selection.
  void bind(const SignalCatalogExport &catalog) noexcept;

  // The single active connection is the same one-connection boundary used by
  // the BLE host. A second connection is ignored rather than taking over the
  // first connection's selected page.
  void connect(std::uint32_t connection, std::uint32_t token) noexcept;
  void disconnect(std::uint32_t connection) noexcept;

  // Validates and commits one seven-byte selector. Checks are deliberately in
  // wire order: length, format, active export/token, then document offset.
  [[nodiscard]] AttError select(std::uint32_t connection, ByteView selector) noexcept;

  // Returns the stable page selected for this connection, or no page when the
  // connection is not active or the export is unavailable.
  [[nodiscard]] std::optional<EncodedSignalCatalogPage>
  read(std::uint32_t connection) const noexcept;

  // Returns the token only for the active connection. Zero means unavailable.
  [[nodiscard]] std::uint32_t token_for_connection(std::uint32_t connection) const noexcept;

private:
  static constexpr std::uint32_t kNoConnection = 0xFFFFFFFFU;

  [[nodiscard]] bool active(std::uint32_t connection) const noexcept;
  void reset_connection() noexcept;

  const SignalCatalogExport *catalog_{nullptr};
  std::uint32_t connection_{kNoConnection};
  std::uint32_t token_{0U};
  std::uint16_t page_offset_{0U};
};

} // namespace companion_protocol
