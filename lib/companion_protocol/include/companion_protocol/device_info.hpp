#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "companion_protocol/bytes.hpp"

namespace companion_protocol {

inline constexpr std::uint8_t kProtocolMajor = 1;
inline constexpr std::uint8_t kProtocolMinor = 0;
inline constexpr std::uint8_t kExtensibleProtocolMinor = 1;
inline constexpr std::uint8_t kLiveSignalLayoutVersion = 2;
// Largest config upload, equal to the canonical override storage limit.
inline constexpr std::uint16_t kMaxConfigBytes = 4096;
inline constexpr std::size_t kMaxDeviceInfoTextBytes = 31;
inline constexpr std::size_t kLegacyMaxDeviceInfoBytes = 72;
inline constexpr std::uint8_t kDeviceInfoExtensionVersion = 1;
inline constexpr std::uint8_t kDeviceInfoExtensionUsableFlag = 0x01;
inline constexpr std::size_t kMaxDeviceInfoBytes = 74;

// The extension is version 1 when present. Only a fully usable catalog and
// stream set the capability bit; an unusable extension reports version 1 and
// zero flags so clients can fall back without observing a usable capability.
enum class DeviceInfoExtensionStatus : std::uint8_t {
  Absent,
  Unusable,
  Usable,
};

// Inputs to the Device info value. The protocol and layout versions and the
// upload limit are protocol constants, not caller choices.
struct DeviceInfo {
  std::uint16_t config_schema_version{0};
  bool pairing_window_open{false};
  std::string_view firmware_version{};
  std::string_view hardware_id{};
};

using EncodedDeviceInfo = BoundedBytes<kMaxDeviceInfoBytes>;

// Encodes the legacy Device info prefix. Protocol 1.1 always appends extension
// version and flags; only a usable catalog and stream set the capability bit.
// Absent preserves the protocol 1.0 prefix. Long text is rejected, not truncated.
[[nodiscard]] std::optional<EncodedDeviceInfo> encode_device_info(
    const DeviceInfo &info,
    DeviceInfoExtensionStatus extension = DeviceInfoExtensionStatus::Absent) noexcept;

} // namespace companion_protocol
