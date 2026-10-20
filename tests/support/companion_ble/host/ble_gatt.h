#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
struct os_mbuf {
  std::vector<std::uint8_t> bytes;
};
struct ble_gatt_access_ctxt {
  std::uint8_t op;
  os_mbuf *om;
};
using ble_gatt_chr_flags = std::uint16_t;
using ble_gatt_access_fn = int(std::uint16_t, std::uint16_t, ble_gatt_access_ctxt *, void *);
inline constexpr ble_gatt_chr_flags BLE_GATT_CHR_F_READ = 2, BLE_GATT_CHR_F_WRITE = 8;
inline constexpr std::uint8_t BLE_GATT_ACCESS_OP_READ_CHR = 0, BLE_GATT_ACCESS_OP_WRITE_CHR = 1;
inline constexpr int BLE_ATT_ERR_INSUFFICIENT_RES = 17, BLE_ATT_ERR_UNLIKELY = 14;
inline int os_mbuf_append(os_mbuf *buffer, const void *data, std::uint16_t length) {
  const auto *bytes = static_cast<const std::uint8_t *>(data);
  buffer->bytes.insert(buffer->bytes.end(), bytes, bytes + length);
  return 0;
}
