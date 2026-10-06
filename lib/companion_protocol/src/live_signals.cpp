#include "companion_protocol/live_signals.hpp"

#include <cmath>
#include <cstddef>

#include "companion_protocol/device_info.hpp"

namespace companion_protocol {

namespace {

using vehicle_signals::Availability;
using vehicle_signals::SignalCapability;
using vehicle_signals::SignalMetadata;
using vehicle_signals::SignalStatus;
using vehicle_signals::SignalType;
using vehicle_signals::SignalValue;

constexpr std::size_t kLayoutVersionOffset = 0;
constexpr std::size_t kSequenceOffset = 1;
constexpr std::size_t kFlagsOffset = 2;
constexpr std::size_t kBooleansOffset = 11;
constexpr std::size_t kStatusOffset = 13;
constexpr std::uint8_t kTelemetryStartedFlag = 0x01;
constexpr std::uint8_t kValuePresentBit = 0x08;
constexpr double kLargestUnsignedFieldValue = 65535.0;
constexpr double kSmallestSignedFieldValue = -32768.0;
constexpr double kLargestSignedFieldValue = 32767.0;

void write_u16(LiveFrame &bytes, std::size_t offset, std::uint16_t value) noexcept {
  bytes[offset] = static_cast<std::uint8_t>(value & 0xFFU);
  bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8U);
}

// Writes a finite Number when it fits its signed or unsigned 16-bit field
// after scaling and rounding. Returns whether the value was written.
bool write_number(const LiveSignalSlot &slot, const SignalValue &value, LiveFrame &bytes) noexcept {
  const auto number = value.as_number();
  if (!number.has_value() || !std::isfinite(*number) || (!slot.signed_number && *number < 0.0F)) {
    return false;
  }
  const double scaled = std::round(static_cast<double>(*number) * static_cast<double>(slot.scale));
  if (slot.signed_number) {
    if (scaled < kSmallestSignedFieldValue || scaled > kLargestSignedFieldValue) {
      return false;
    }
    // The checked float-to-signed conversion is in range; signed-to-unsigned
    // conversion is defined modulo 65536, producing the wire two's complement.
    write_u16(bytes, slot.position, static_cast<std::uint16_t>(static_cast<std::int16_t>(scaled)));
    return true;
  }
  if (scaled > kLargestUnsignedFieldValue) {
    return false;
  }
  write_u16(bytes, slot.position, static_cast<std::uint16_t>(scaled));
  return true;
}

// Writes the protocol code of the catalog choice key. A raw value with no
// catalog choice, or a choice key without a code, is not written.
bool write_choice(const LiveSignalSlot &slot, const SignalMetadata &signal,
                  const SignalValue &value, LiveFrame &bytes) noexcept {
  const auto raw = value.as_enumeration();
  if (!raw.has_value()) {
    return false;
  }
  const auto *choice = signal.find_choice(*raw);
  if (choice == nullptr) {
    return false;
  }
  const auto code = slot.choice_code(choice->key);
  if (!code.has_value()) {
    return false;
  }
  bytes[slot.position] = *code;
  return true;
}

bool write_boolean(const LiveSignalSlot &slot, const SignalValue &value,
                   std::uint16_t &booleans) noexcept {
  const auto flag = value.as_boolean();
  if (!flag.has_value()) {
    return false;
  }
  if (*flag) {
    booleans = static_cast<std::uint16_t>(booleans | (1U << slot.position));
  }
  return true;
}

bool write_value(const LiveSignalSlot &slot, const SignalMetadata &signal, const SignalValue &value,
                 LiveFrame &bytes, std::uint16_t &booleans) noexcept {
  switch (slot.type) {
  case SignalType::Number:
    return write_number(slot, value, bytes);
  case SignalType::Enum:
    return write_choice(slot, signal, value, bytes);
  case SignalType::Boolean:
    return write_boolean(slot, value, booleans);
  }
  return false;
}

void write_status(LiveFrame &bytes, std::size_t index, SignalStatusCode code,
                  bool value_present) noexcept {
  const auto nibble = static_cast<std::uint8_t>(static_cast<std::uint8_t>(code) |
                                                (value_present ? kValuePresentBit : 0U));
  const std::size_t offset = kStatusOffset + index / 2;
  const unsigned shift = index % 2 == 0 ? 0U : 4U;
  bytes[offset] = static_cast<std::uint8_t>(bytes[offset] | (nibble << shift));
}

// The provider's availability, except that a slot whose freshness is unset
// is never reported Fresh: a Fresh reading there is a provider defect.
SignalStatusCode reported_code(const LiveSignalSlot &slot, Availability availability) noexcept {
  const SignalStatusCode code = status_code(availability);
  if (slot.freshness_unset && code == SignalStatusCode::Fresh) {
    return SignalStatusCode::FreshnessUnverified;
  }
  return code;
}

const SignalMetadata *resolve(const vehicle_signals::SignalCatalogView &catalog,
                              const LiveSignalSlot &slot) noexcept {
  const SignalMetadata *signal = catalog.find(slot.key);
  if (signal == nullptr || signal->type != slot.type ||
      !signal->capabilities.has(SignalCapability::Read)) {
    return nullptr;
  }
  return signal;
}

} // namespace

SignalStatusCode status_code(Availability availability) noexcept {
  switch (availability) {
  case Availability::NoData:
    return SignalStatusCode::NoData;
  case Availability::Fresh:
    return SignalStatusCode::Fresh;
  case Availability::Stale:
    return SignalStatusCode::Stale;
  case Availability::FreshnessUnverified:
    return SignalStatusCode::FreshnessUnverified;
  case Availability::Unavailable:
    return SignalStatusCode::Unavailable;
  }
  // An enumerator this table does not know is never reported as fresh.
  return SignalStatusCode::ReadFailed;
}

SignalStatusCode status_code(SignalStatus failure) noexcept {
  switch (failure) {
  case SignalStatus::InvalidSignal:
  case SignalStatus::UnsupportedCapability:
    return SignalStatusCode::NotSupported;
  default:
    return SignalStatusCode::ReadFailed;
  }
}

LiveSignalContent::LiveSignalContent() noexcept {
  bytes_[kLayoutVersionOffset] = kLiveSignalLayoutVersion;
}

LiveFrame LiveSignalContent::frame(std::uint8_t sequence) const noexcept {
  LiveFrame bytes = bytes_;
  bytes[kSequenceOffset] = sequence;
  return bytes;
}

LiveSignalSampler::LiveSignalSampler(const vehicle_signals::SignalProvider &provider) noexcept
    : provider_(provider) {
  const auto catalog = provider_.catalog();
  const auto &slots = live_signal_slots();
  for (std::size_t index = 0; index < kLiveSignalCount; ++index) {
    resolved_[index] = resolve(catalog, slots[index]);
  }
}

LiveSignalContent LiveSignalSampler::sample(bool telemetry_started) const noexcept {
  LiveSignalContent content{};
  LiveFrame &bytes = content.bytes_;
  bytes[kFlagsOffset] = telemetry_started ? kTelemetryStartedFlag : std::uint8_t{0};
  std::uint16_t booleans = 0;
  const auto &slots = live_signal_slots();
  for (std::size_t index = 0; index < kLiveSignalCount; ++index) {
    const SignalMetadata *signal = resolved_[index];
    if (signal == nullptr) {
      write_status(bytes, index, SignalStatusCode::NotSupported, false);
      continue;
    }
    const auto result = provider_.read(signal->id);
    if (!result.ok()) {
      write_status(bytes, index, status_code(result.status), false);
      continue;
    }
    const auto &reading = *result.value;
    const bool present = reading.value.has_value() &&
                         write_value(slots[index], *signal, *reading.value, bytes, booleans);
    write_status(bytes, index, reported_code(slots[index], reading.availability), present);
  }
  write_u16(bytes, kBooleansOffset, booleans);
  return content;
}

} // namespace companion_protocol
