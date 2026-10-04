#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

// Companion BLE UUIDs (#163) defined by docs/specs/companion/ble-protocol.md.
// The Device info value itself is encoded by lib/companion_protocol (#162) and
// tested there; the NimBLE binding only copies these bytes into the GATT table.
#include "companion_ble/uuids.hpp"

#include <array>
#include <cstdint>

namespace {
using companion_ble::CompanionAttribute;
} // namespace

TEST_CASE("companion service UUID ab490000-09b6-4509-bf8e-2790253baf98 is little-endian") {
  constexpr std::array<std::uint8_t, 16> expected{0x98, 0xaf, 0x3b, 0x25, 0x90, 0x27, 0x8e, 0xbf,
                                                  0x09, 0x45, 0xb6, 0x09, 0x00, 0x00, 0x49, 0xab};

  CHECK(companion_ble::companion_uuid(CompanionAttribute::Service) == expected);
}

TEST_CASE(
    "device info UUID ab490001-09b6-4509-bf8e-2790253baf98 differs only in the 16-bit field") {
  constexpr auto uuid = companion_ble::companion_uuid(CompanionAttribute::DeviceInfo);

  CHECK(uuid[12] == 0x01U);
  CHECK(uuid[13] == 0x00U);
  CHECK(uuid[14] == 0x49U);
  CHECK(uuid[15] == 0xabU);
}
