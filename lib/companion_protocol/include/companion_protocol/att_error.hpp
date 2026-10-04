#pragma once

#include <cstdint>

namespace companion_protocol {

// Outcome of a protected write, sent as the ATT error response code. None
// means the write succeeded. Values are stable protocol codes from the core
// profile and the config transfer specification; they are never renumbered.
enum class AttError : std::uint8_t {
  None = 0x00,
  InvalidAttributeValueLength = 0x0D,
  InvalidPdu = 0x80,
  UnsupportedOperation = 0x81,
  Busy = 0x82,
  InvalidState = 0x83,
  StorageFailure = 0x84,
  MtuTooSmall = 0x85,
  TooLarge = 0x86,
  OffsetMismatch = 0x87,
  Incomplete = 0x88,
  ChecksumMismatch = 0x89,
  ConfigRejected = 0x8A,
  ApplyRejected = 0x8B,
};

// Smallest negotiated ATT MTU for config uploads and notifications.
inline constexpr std::uint16_t kMinimumTransferMtu = 64;

} // namespace companion_protocol
