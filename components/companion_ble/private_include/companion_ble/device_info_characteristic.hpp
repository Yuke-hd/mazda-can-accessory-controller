#pragma once

#include "companion_ble/gatt_service.hpp"
#include "companion_protocol/device_info.hpp"

namespace companion_ble::internal {

// The Device info value, encoded once for each pairing window state so a read
// only selects bytes.
struct DeviceInfoValues {
  companion_protocol::EncodedDeviceInfo window_closed{};
  companion_protocol::EncodedDeviceInfo window_open{};
};

// The read-only Device info characteristic serving already encoded values.
// `values` must outlive the NimBLE host. The value is readable without
// encryption so a client can check protocol compatibility before pairing;
// flags bit 0 reports whether the pairing window is open at the read.
[[nodiscard]] CharacteristicDefinition
device_info_characteristic(const DeviceInfoValues &values) noexcept;

} // namespace companion_ble::internal
