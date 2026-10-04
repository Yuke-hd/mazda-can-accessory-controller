#pragma once

#include "companion_ble/uuids.hpp"
#include "host/ble_uuid.h"

#include <algorithm>

namespace companion_ble::internal {

[[nodiscard]] inline ble_uuid128_t nimble_uuid(const CompanionAttribute attribute) noexcept {
  ble_uuid128_t uuid{};
  uuid.u.type = BLE_UUID_TYPE_128;
  const Uuid128 bytes = companion_uuid(attribute);
  std::copy(bytes.begin(), bytes.end(), uuid.value);
  return uuid;
}

} // namespace companion_ble::internal
