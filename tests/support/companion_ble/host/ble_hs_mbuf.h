#pragma once
#include "host/ble_gatt.h"
#include <algorithm>
#define OS_MBUF_PKTLEN(buffer) ((buffer)->bytes.size())
inline int ble_hs_mbuf_to_flat(const os_mbuf *buffer, void *destination, std::uint16_t length,
                               std::uint16_t *copied) {
  if (buffer->bytes.size() > length)
    return -1;
  std::copy(buffer->bytes.begin(), buffer->bytes.end(), static_cast<std::uint8_t *>(destination));
  *copied = static_cast<std::uint16_t>(buffer->bytes.size());
  return 0;
}
