#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "companion_protocol/bytes.hpp"

namespace companion_protocol {

inline constexpr std::uint8_t kProtocolMajor = 1;
inline constexpr std::uint8_t kProtocolMinor = 2;
inline constexpr std::uint8_t kExtensibleProtocolMinor = 1;
inline constexpr std::uint8_t kLiveSignalLayoutVersion = 2;
// Largest config upload, equal to the canonical override storage limit.
inline constexpr std::uint16_t kMaxConfigBytes = 4096;
inline constexpr std::size_t kMaxDeviceInfoTextBytes = 31;
inline constexpr std::size_t kLegacyMaxDeviceInfoBytes = 72;
inline constexpr std::uint8_t kDeviceInfoExtensionVersion = 1;
inline constexpr std::uint8_t kDeviceInfoExtensionUsableFlag = 0x01;
inline constexpr std::uint8_t kDeviceInfoOutputCatalogUsableFlag = 0x02;
inline constexpr std::size_t kMaxDeviceInfoBytes = 74;

// Status of the signal catalog and extensible stream capability (bit 0).
// Absent is retained for callers without signals; protocol 1.2 still appends
// extension version 1 and independently reports the output capability.
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

// Protocol 1.2 always appends extension version 1 and both capability bits.
// Absent and Unusable both clear the signal capability. Long text is rejected.
[[nodiscard]] std::optional<EncodedDeviceInfo>
encode_device_info(const DeviceInfo &info,
                   DeviceInfoExtensionStatus extension = DeviceInfoExtensionStatus::Absent,
                   bool output_catalog_usable = false) noexcept;

} // namespace companion_protocol
