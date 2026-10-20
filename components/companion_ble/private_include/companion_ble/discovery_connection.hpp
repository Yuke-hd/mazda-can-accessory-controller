#pragma once

#include <cstdint>

namespace companion_ble::internal {

// Owned by the NimBLE host task, independently of either catalog's usability.
class DiscoveryConnection final {
public:
  // Zero rejects an unexpected second connection without replacing the first.
  [[nodiscard]] std::uint32_t connect(std::uint16_t connection) noexcept {
    if (connection_ != kNoConnection || connection == kNoConnection)
      return 0U;
    connection_ = connection;
    if (++next_token_ == 0U)
      ++next_token_;
    return next_token_;
  }

  // Only the active link may clear both catalog selectors.
  [[nodiscard]] bool disconnect(std::uint16_t connection) noexcept {
    if (connection_ == kNoConnection || connection != connection_)
      return false;
    connection_ = kNoConnection;
    return true;
  }

private:
  static constexpr std::uint16_t kNoConnection = 0xFFFFU;
  std::uint16_t connection_{kNoConnection};
  std::uint32_t next_token_{0U};
};

} // namespace companion_ble::internal
