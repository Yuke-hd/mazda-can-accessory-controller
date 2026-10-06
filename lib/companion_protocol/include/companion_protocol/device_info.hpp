#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "companion_protocol/bytes.hpp"

namespace companion_protocol {

inline constexpr std::uint8_t kProtocolMajor = 1;
inline constexpr std::uint8_t kProtocolMinor = 0;
inline constexpr std::uint8_t kLiveSignalLayoutVersion = 2;
// Largest config upload, equal to the canonical override storage limit.
inline constexpr std::uint16_t kMaxConfigBytes = 4096;
inline constexpr std::size_t kMaxDeviceInfoTextBytes = 31;
inline constexpr std::size_t kMaxDeviceInfoBytes = 72;

// Inputs to the Device info value. The protocol and layout versions and the
// upload limit are protocol constants, not caller choices.
struct DeviceInfo {
  std::uint16_t config_schema_version{0};
  bool pairing_window_open{false};
  std::string_view firmware_version{};
  std::string_view hardware_id{};
};

using EncodedDeviceInfo = BoundedBytes<kMaxDeviceInfoBytes>;

// Encodes the version 1 Device info value. Returns std::nullopt when a text
// field exceeds 31 bytes: a longer string is rejected rather than truncated,
// so an over-long input never reaches the wire.
[[nodiscard]] std::optional<EncodedDeviceInfo> encode_device_info(const DeviceInfo &info) noexcept;

} // namespace companion_protocol
