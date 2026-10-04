#include "companion_ble/device_info_characteristic.hpp"

#include "host/ble_hs.h"

namespace companion_ble::internal {
namespace {

int access_device_info(std::uint16_t /*connection*/, std::uint16_t /*attribute*/,
                       ble_gatt_access_ctxt *const context, void *const argument) {
  if (context->op != BLE_GATT_ACCESS_OP_READ_CHR)
    return BLE_ATT_ERR_UNLIKELY;
  const auto &value = *static_cast<const companion_protocol::EncodedDeviceInfo *>(argument);
  const auto bytes = value.view();
  // NimBLE applies Read Blob offsets itself, so the whole value is appended.
  const int rc =
      os_mbuf_append(context->om, bytes.data(), static_cast<std::uint16_t>(bytes.size()));
  return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

} // namespace

CharacteristicDefinition
device_info_characteristic(const companion_protocol::EncodedDeviceInfo &value) noexcept {
  CharacteristicDefinition definition{CompanionAttribute::DeviceInfo};
  definition.flags = BLE_GATT_CHR_F_READ;
  definition.access = access_device_info;
  // NimBLE only hands the context back to access_device_info, which reads it.
  definition.context = const_cast<companion_protocol::EncodedDeviceInfo *>(&value);
  return definition;
}

} // namespace companion_ble::internal
