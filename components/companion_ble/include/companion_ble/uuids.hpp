#pragma once

// Companion GATT UUIDs (docs/specs/companion/ble-protocol.md#uuids). The
// service and its characteristics share the random base
// ab49xxxx-09b6-4509-bf8e-2790253baf98; only the 16-bit `xxxx` field differs.

#include <array>
#include <cstdint>

namespace companion_ble {

// The `xxxx` field of each companion attribute. Values 0x0006-0xffff are
// reserved for later characteristics.
enum class CompanionAttribute : std::uint16_t {
  Service = 0x0000,
  DeviceInfo = 0x0001,
  Config = 0x0002,
  ConfigStatus = 0x0003,
  LiveSignals = 0x0004,
  Command = 0x0005,
};

// A 128-bit UUID in BLE wire order (least significant byte first).
using Uuid128 = std::array<std::uint8_t, 16>;

[[nodiscard]] constexpr Uuid128 companion_uuid(const CompanionAttribute attribute) noexcept {
  const auto field = static_cast<std::uint16_t>(attribute);
  // ab49xxxx-09b6-4509-bf8e-2790253baf98, reversed.
  return Uuid128{0x98,
                 0xaf,
                 0x3b,
                 0x25,
                 0x90,
                 0x27,
                 0x8e,
                 0xbf,
                 0x09,
                 0x45,
                 0xb6,
                 0x09,
                 static_cast<std::uint8_t>(field & 0xffU),
                 static_cast<std::uint8_t>(field >> 8U),
                 0x49,
                 0xab};
}

} // namespace companion_ble
