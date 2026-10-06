#pragma once

#include <cstdint>

#include "companion_protocol/bytes.hpp"

namespace companion_protocol {

// CRC-32/ISO-HDLC (zlib/PNG): reflected polynomial 0x04C11DB7, initial value
// and final XOR 0xFFFFFFFF. The check value over "123456789" is 0xCBF43926.
[[nodiscard]] std::uint32_t crc32(ByteView bytes) noexcept;

} // namespace companion_protocol
