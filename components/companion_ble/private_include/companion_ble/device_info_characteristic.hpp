#pragma once

#include "companion_ble/gatt_service.hpp"
#include "companion_protocol/device_info.hpp"

namespace companion_ble::internal {

// The read-only Device info characteristic serving an already encoded value.
// `value` must outlive the NimBLE host. The value is readable without encryption so a client can
// check protocol compatibility before pairing.
[[nodiscard]] CharacteristicDefinition
device_info_characteristic(const companion_protocol::EncodedDeviceInfo &value) noexcept;

} // namespace companion_ble::internal
