#include "companion_protocol/device_info.hpp"

namespace companion_protocol {

namespace {

constexpr std::uint8_t kPairingWindowOpenFlag = 0x01;

bool push_text(EncodedDeviceInfo &value, std::string_view text) noexcept {
  const auto *bytes = reinterpret_cast<const std::uint8_t *>(text.data());
  return value.push(static_cast<std::uint8_t>(text.size())) &&
         value.append(ByteView{bytes, text.size()});
}

} // namespace

std::optional<EncodedDeviceInfo>
encode_device_info(const DeviceInfo &info, const DeviceInfoExtensionStatus extension) noexcept {
  if (info.firmware_version.size() > kMaxDeviceInfoTextBytes ||
      info.hardware_id.size() > kMaxDeviceInfoTextBytes) {
    return std::nullopt;
  }
  EncodedDeviceInfo value{};
  value.push(kProtocolMajor);
  const bool extension_present = extension != DeviceInfoExtensionStatus::Absent;
  const bool extension_usable = extension == DeviceInfoExtensionStatus::Usable;
  value.push(extension_present ? kExtensibleProtocolMinor : kProtocolMinor);
  value.push_u16(info.config_schema_version);
  value.push(kLiveSignalLayoutVersion);
  value.push(info.pairing_window_open ? kPairingWindowOpenFlag : std::uint8_t{0});
  value.push_u16(kMaxConfigBytes);
  if (!push_text(value, info.firmware_version) || !push_text(value, info.hardware_id)) {
    return std::nullopt;
  }
  if (extension_present) {
    const std::uint8_t extension_flags =
        extension_usable ? kDeviceInfoExtensionUsableFlag : std::uint8_t{0};
    if (!value.push(kDeviceInfoExtensionVersion) || !value.push(extension_flags)) {
      return std::nullopt;
    }
  }
  return value;
}

} // namespace companion_protocol
