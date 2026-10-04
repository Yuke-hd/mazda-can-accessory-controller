#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

// Companion BLE UUIDs (#163) defined by docs/specs/companion/ble-protocol.md.
// The Device info value itself is encoded by lib/companion_protocol (#162) and
// tested there; the NimBLE binding only copies these bytes into the GATT table.
#include "companion_ble/uuids.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace {

using companion_ble::CompanionAttribute;
using companion_ble::Uuid128;

struct ExpectedUuid {
  CompanionAttribute attribute;
  std::string_view text;
};

// The UUID table in ble-protocol.md#uuids, written as the spec prints it.
constexpr std::array<ExpectedUuid, 6> kSpecUuids{{
    {CompanionAttribute::Service, "ab490000-09b6-4509-bf8e-2790253baf98"},
    {CompanionAttribute::DeviceInfo, "ab490001-09b6-4509-bf8e-2790253baf98"},
    {CompanionAttribute::Config, "ab490002-09b6-4509-bf8e-2790253baf98"},
    {CompanionAttribute::ConfigStatus, "ab490003-09b6-4509-bf8e-2790253baf98"},
    {CompanionAttribute::LiveSignals, "ab490004-09b6-4509-bf8e-2790253baf98"},
    {CompanionAttribute::Command, "ab490005-09b6-4509-bf8e-2790253baf98"},
}};

std::uint8_t hex_digit(const char digit) {
  if (digit >= '0' && digit <= '9')
    return static_cast<std::uint8_t>(digit - '0');
  REQUIRE(digit >= 'a');
  REQUIRE(digit <= 'f');
  return static_cast<std::uint8_t>(digit - 'a' + 10);
}

// Parses the canonical text form into BLE wire order (least significant byte
// first), independently of companion_uuid().
Uuid128 wire_order(const std::string_view text) {
  std::string digits;
  for (const char character : text) {
    if (character != '-')
      digits.push_back(character);
  }
  REQUIRE(digits.size() == 32U);
  Uuid128 bytes{};
  for (std::size_t index = 0U; index < bytes.size(); ++index) {
    const auto high = hex_digit(digits[2U * index]);
    const auto low = hex_digit(digits[2U * index + 1U]);
    bytes[bytes.size() - 1U - index] = static_cast<std::uint8_t>((high << 4U) | low);
  }
  return bytes;
}

} // namespace

TEST_CASE("every companion UUID matches the ble-protocol.md table in wire order") {
  for (const auto &expected : kSpecUuids) {
    CAPTURE(expected.text);
    CHECK(companion_ble::companion_uuid(expected.attribute) == wire_order(expected.text));
  }
}

TEST_CASE("the companion service UUID is little-endian on the wire") {
  constexpr std::array<std::uint8_t, 16> expected{0x98, 0xaf, 0x3b, 0x25, 0x90, 0x27, 0x8e, 0xbf,
                                                  0x09, 0x45, 0xb6, 0x09, 0x00, 0x00, 0x49, 0xab};

  CHECK(companion_ble::companion_uuid(CompanionAttribute::Service) == expected);
}
