#include "companion_protocol/crc32.hpp"

namespace companion_protocol {

namespace {

constexpr std::uint32_t kReflectedPolynomial = 0xEDB88320U;

} // namespace

std::uint32_t crc32(ByteView bytes) noexcept {
  std::uint32_t crc = 0xFFFFFFFFU;
  for (const std::uint8_t byte : bytes) {
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit) {
      const std::uint32_t mask = 0U - (crc & 1U);
      crc = (crc >> 1U) ^ (kReflectedPolynomial & mask);
    }
  }
  return crc ^ 0xFFFFFFFFU;
}

} // namespace companion_protocol
