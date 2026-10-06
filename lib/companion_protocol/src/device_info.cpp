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

std::optional<EncodedDeviceInfo> encode_device_info(const DeviceInfo &info) noexcept {
  if (info.firmware_version.size() > kMaxDeviceInfoTextBytes ||
      info.hardware_id.size() > kMaxDeviceInfoTextBytes) {
    return std::nullopt;
  }
  EncodedDeviceInfo value{};
  value.push(kProtocolMajor);
  value.push(kProtocolMinor);
  value.push_u16(info.config_schema_version);
  value.push(kLiveSignalLayoutVersion);
  value.push(info.pairing_window_open ? kPairingWindowOpenFlag : std::uint8_t{0});
  value.push_u16(kMaxConfigBytes);
  if (!push_text(value, info.firmware_version) || !push_text(value, info.hardware_id)) {
    return std::nullopt;
  }
  return value;
}

} // namespace companion_protocol
