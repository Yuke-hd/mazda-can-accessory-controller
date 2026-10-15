#include "companion_protocol/extensible_live_signals.hpp"

#include <array>
#include <cmath>
#include <cstring>
#include <limits>

#include "companion_protocol/live_signal_layout.hpp"
#include "companion_protocol/live_signals.hpp"

namespace companion_protocol {

namespace {

using vehicle_signals::SignalCapability;
using vehicle_signals::SignalMetadata;
using vehicle_signals::SignalStatus;
using vehicle_signals::SignalType;
using vehicle_signals::SignalValue;

constexpr std::uint8_t kTelemetryStartedFlag = 0x01;
constexpr std::uint8_t kValuePresentBit = 0x08;

static_assert(sizeof(float) == sizeof(std::uint32_t),
              "extensible live signal Numbers require binary32-sized float");
static_assert(std::numeric_limits<float>::is_iec559,
              "extensible live signal Numbers require IEEE-754 float");

bool wire_type_code(SignalType type, std::uint8_t &code) noexcept {
  switch (type) {
  case SignalType::Boolean:
    code = 1;
    return true;
  case SignalType::Number:
    code = 2;
    return true;
  case SignalType::Enum:
    code = 3;
    return true;
  }
  return false;
}

bool freshness_is_unset(const SignalMetadata &metadata) noexcept {
  const auto &slots = live_signal_slots();
  for (const LiveSignalSlot &slot : slots) {
    if (slot.key == metadata.key && slot.type == metadata.type) {
      return slot.freshness_unset;
    }
  }
  return false;
}

SignalStatusCode reported_status(const SignalMetadata &metadata,
                                 vehicle_signals::Availability availability) noexcept {
  SignalStatusCode status = status_code(availability);
  if (freshness_is_unset(metadata) && status == SignalStatusCode::Fresh) {
    return SignalStatusCode::FreshnessUnverified;
  }
  return status;
}

std::uint32_t float_bits(float value) noexcept {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

} // namespace

ExtensibleLiveSignalPacketBuilder::ExtensibleLiveSignalPacketBuilder(
    const SignalCatalogExport &catalog, const vehicle_signals::SignalProvider &provider,
    const std::uint32_t catalog_token, const std::uint16_t sequence, const bool telemetry_started,
    const std::uint16_t att_mtu) noexcept
    : catalog_(catalog), provider_(provider) {
  const std::size_t mtu_limit =
      att_mtu > kExtensibleLiveSignalMtuOverheadBytes
          ? static_cast<std::size_t>(att_mtu) - kExtensibleLiveSignalMtuOverheadBytes
          : 0;
  packet_limit_ = mtu_limit < kExtensibleLiveSignalMaxPacketBytes
                      ? mtu_limit
                      : kExtensibleLiveSignalMaxPacketBytes;
  if (!catalog_.available() || catalog_token == 0 ||
      packet_limit_ < kExtensibleLiveSignalHeaderBytes) {
    return;
  }

  if (!packet_.push(kExtensibleLiveSignalFormat) || !packet_.push_u16(sequence) ||
      !packet_.push(telemetry_started ? kTelemetryStartedFlag : std::uint8_t{0}) ||
      !packet_.push_u32(catalog_token)) {
    packet_ = ExtensibleLiveSignalPacket{};
    return;
  }
  valid_ = true;
}

ExtensibleLiveSignalPacketBuilder::ExtensibleLiveSignalPacketBuilder(
    const vehicle_signals::SignalProvider &provider, const SignalCatalogExport &catalog,
    const std::uint32_t catalog_token, const std::uint16_t sequence, const bool telemetry_started,
    const std::uint16_t att_mtu) noexcept
    : ExtensibleLiveSignalPacketBuilder(catalog, provider, catalog_token, sequence,
                                        telemetry_started, att_mtu) {}

const SignalMetadata *ExtensibleLiveSignalPacketBuilder::metadata_for_handle(
    const std::uint16_t wire_handle) const noexcept {
  if (!valid_ || wire_handle == 0 || wire_handle > catalog_.signal_count()) {
    return nullptr;
  }
  const vehicle_signals::SignalId provider_id = catalog_.provider_id_for_handle(wire_handle);
  if (!provider_id.valid()) {
    return nullptr;
  }
  const SignalMetadata *metadata = provider_.catalog().find(provider_id);
  if (metadata == nullptr || !metadata->capabilities.has(SignalCapability::Read)) {
    return nullptr;
  }
  return metadata;
}

bool ExtensibleLiveSignalPacketBuilder::append(const std::uint16_t wire_handle) noexcept {
  if (wire_handle == 0 || wire_handle > 64 ||
      (appended_handles_ & (std::uint64_t{1} << (wire_handle - 1U))) != 0) {
    return false;
  }
  const SignalMetadata *metadata = metadata_for_handle(wire_handle);
  if (metadata == nullptr) {
    return false;
  }
  const auto result = provider_.read(metadata->id);
  if (!result.ok()) {
    return append_failure(wire_handle, result.status);
  }
  return append_reading(wire_handle, *result.value);
}

bool ExtensibleLiveSignalPacketBuilder::append_reading(
    const std::uint16_t wire_handle, const vehicle_signals::SignalReading &reading) noexcept {
  if (wire_handle == 0 || wire_handle > 64 ||
      (appended_handles_ & (std::uint64_t{1} << (wire_handle - 1U))) != 0) {
    return false;
  }
  const SignalMetadata *metadata = metadata_for_handle(wire_handle);
  if (metadata == nullptr) {
    return false;
  }

  std::array<std::uint8_t, 4> encoded_value{};
  std::size_t encoded_length = 0;
  if (reading.value.has_value() && reading.value->type() == metadata->type) {
    const SignalValue &value = *reading.value;
    switch (metadata->type) {
    case SignalType::Boolean: {
      const auto boolean = value.as_boolean();
      if (boolean.has_value()) {
        encoded_value[0] = *boolean ? 1U : 0U;
        encoded_length = 1;
      }
      break;
    }
    case SignalType::Number: {
      const auto number = value.as_number();
      if (number.has_value() && std::isfinite(*number)) {
        const std::uint32_t bits = float_bits(*number);
        encoded_value[0] = static_cast<std::uint8_t>(bits & 0xFFU);
        encoded_value[1] = static_cast<std::uint8_t>((bits >> 8U) & 0xFFU);
        encoded_value[2] = static_cast<std::uint8_t>((bits >> 16U) & 0xFFU);
        encoded_value[3] = static_cast<std::uint8_t>(bits >> 24U);
        encoded_length = 4;
      }
      break;
    }
    case SignalType::Enum: {
      const auto raw = value.as_enumeration();
      if (raw.has_value()) {
        const auto choice = catalog_.wire_choice_handle(wire_handle, *raw);
        if (choice.has_value() && *choice != 0) {
          encoded_value[0] = static_cast<std::uint8_t>(*choice & 0xFFU);
          encoded_value[1] = static_cast<std::uint8_t>(*choice >> 8U);
          encoded_length = 2;
        }
      }
      break;
    }
    }
  }

  return append_status(wire_handle, *metadata, reported_status(*metadata, reading.availability),
                       encoded_value.data(), encoded_length);
}

bool ExtensibleLiveSignalPacketBuilder::append_failure(const std::uint16_t wire_handle,
                                                       const SignalStatus failure) noexcept {
  if (wire_handle == 0 || wire_handle > 64 ||
      (appended_handles_ & (std::uint64_t{1} << (wire_handle - 1U))) != 0) {
    return false;
  }
  const SignalMetadata *metadata = metadata_for_handle(wire_handle);
  if (metadata == nullptr) {
    return false;
  }
  return append_status(wire_handle, *metadata, status_code(failure), nullptr, 0);
}

bool ExtensibleLiveSignalPacketBuilder::append_status(const std::uint16_t wire_handle,
                                                      const SignalMetadata &metadata,
                                                      const SignalStatusCode status,
                                                      const std::uint8_t *value_bytes,
                                                      const std::size_t value_length) noexcept {
  std::uint8_t type_code = 0;
  if (!wire_type_code(metadata.type, type_code) || value_length > 4 ||
      (value_length != 0 && value_bytes == nullptr)) {
    return false;
  }

  const std::size_t record_length = 4 + value_length;
  const std::size_t record_bytes = 1 + record_length;
  if (!valid_ || record_length > std::numeric_limits<std::uint8_t>::max() || wire_handle == 0 ||
      wire_handle > 64 || (appended_handles_ & (std::uint64_t{1} << (wire_handle - 1U))) != 0 ||
      packet_.size() > packet_limit_ || packet_limit_ - packet_.size() < record_bytes) {
    return false;
  }

  const std::uint8_t status_byte =
      static_cast<std::uint8_t>((static_cast<std::uint8_t>(status) & 0x07U) |
                                (value_length != 0 ? kValuePresentBit : std::uint8_t{0}));
  if (!packet_.push(static_cast<std::uint8_t>(record_length)) || !packet_.push_u16(wire_handle) ||
      !packet_.push(type_code) || !packet_.push(status_byte)) {
    return false;
  }
  for (std::size_t index = 0; index < value_length; ++index) {
    if (!packet_.push(value_bytes[index])) {
      return false;
    }
  }
  appended_handles_ |= std::uint64_t{1} << (wire_handle - 1U);
  return true;
}

ExtensibleLiveSignalPacket
ExtensibleLiveSignalSampler::sample(const std::uint32_t catalog_token, const std::uint16_t sequence,
                                    const bool telemetry_started,
                                    const std::uint16_t att_mtu) const noexcept {
  return sample_range(catalog_token, sequence, telemetry_started, att_mtu, 1,
                      catalog_.signal_count());
}

ExtensibleLiveSignalPacket ExtensibleLiveSignalSampler::sample_range(
    const std::uint32_t catalog_token, const std::uint16_t sequence, const bool telemetry_started,
    const std::uint16_t att_mtu, const std::uint16_t first_handle,
    const std::uint16_t handle_count) const noexcept {
  ExtensibleLiveSignalPacketBuilder builder{catalog_, provider_,         catalog_token,
                                            sequence, telemetry_started, att_mtu};
  if (!builder.valid()) {
    return builder.packet();
  }
  const std::uint32_t end = static_cast<std::uint32_t>(first_handle) + handle_count;
  for (std::uint32_t handle = first_handle; handle < end && handle <= catalog_.signal_count();
       ++handle) {
    if (!builder.append(static_cast<std::uint16_t>(handle))) {
      break;
    }
  }
  return builder.packet();
}

ExtensibleLiveSignalPacket
encode_extensible_live_signals(const SignalCatalogExport &catalog,
                               const vehicle_signals::SignalProvider &provider,
                               const std::uint32_t catalog_token, const std::uint16_t sequence,
                               const bool telemetry_started, const std::uint16_t att_mtu) noexcept {
  const ExtensibleLiveSignalSampler sampler{catalog, provider};
  return sampler.sample(catalog_token, sequence, telemetry_started, att_mtu);
}

} // namespace companion_protocol
