#pragma once

#include "companion_ble/uuids.hpp"
#include "host/ble_gatt.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>

namespace companion_ble::internal {

// One characteristic of the companion service. Each characteristic lives in
// its own translation unit and returns its definition; the service module
// turns the definitions into the NimBLE attribute table. Later
// characteristics (Command and pairing checks #164, Config and Config status
// #165, Live signals #166) add a definition here without changing the
// service, advertising or startup code.
struct CharacteristicDefinition {
  // The attribute has no default: every characteristic names its own UUID.
  explicit constexpr CharacteristicDefinition(const CompanionAttribute companion_attribute) noexcept
      : attribute(companion_attribute) {}

  CompanionAttribute attribute;
  // NimBLE properties and permissions, for example BLE_GATT_CHR_F_READ.
  ble_gatt_chr_flags flags{0U};
  ble_gatt_access_fn *access{nullptr};
  // Passed unchanged to `access`; must outlive the NimBLE host.
  void *context{nullptr};
  // Receives the value handle on registration, for notifications. Optional.
  std::uint16_t *value_handle{nullptr};
};

// The spec defines five companion characteristics.
inline constexpr std::size_t kMaxCompanionCharacteristics = 5U;

// Adds the GAP and GATT services and the companion primary service with
// `characteristics` to the NimBLE attribute table. Call it once, after
// nimble_port_init() and before the host task starts. Returns 0 or a NimBLE
// host error code; BLE_HS_EINVAL when the list is empty, too long, names the
// service attribute or names one attribute twice.
[[nodiscard]] int register_companion_service(
    std::initializer_list<CharacteristicDefinition> characteristics) noexcept;

} // namespace companion_ble::internal
