#include "companion_protocol/display_snapshot.hpp"

#include <cmath>
#include <cstring>

#include "companion_protocol/live_signals.hpp"

namespace companion_protocol {

namespace {

using vehicle_signals::Availability;
using vehicle_signals::SignalCapability;
using vehicle_signals::SignalMetadata;
using vehicle_signals::SignalType;
using vehicle_signals::SignalValue;

constexpr std::size_t kHeaderBytes = 16;
constexpr std::size_t kRecordBytes = 8;
constexpr std::uint8_t kVersion = 1;
constexpr std::uint8_t kStartedFlag = 0x01;

// Display protocol tables, kept separate from the BLE live layout on purpose:
// the two protocols version independently even though the codes match today.
constexpr LiveChoiceCode kTurnCodes[] = {
    {"unknown", 0}, {"off", 1}, {"left", 2}, {"right", 3}, {"hazard", 4},
};
constexpr LiveChoiceCode kSelectorCodes[] = {
    {"unknown", 0}, {"shifting", 1}, {"park", 2}, {"reverse", 3}, {"neutral", 4}, {"drive", 5},
};
constexpr LiveChoiceCode kGearCodes[] = {
    {"unknown", 0}, {"park_or_neutral", 1}, {"park", 2},   {"neutral", 3},
    {"reverse", 4}, {"first", 5},           {"second", 6}, {"third", 7},
    {"fourth", 8},  {"fifth", 9},           {"sixth", 10}, {"shifting", 11},
};
constexpr LiveChoiceCode kWiperCodes[] = {
    {"unknown", 0}, {"off", 1}, {"on", 2}, {"high", 3}, {"intermittent", 4},
};

constexpr DisplaySignalSlot number(std::string_view key, DisplayRange range,
                                   bool freshness_unset = false) {
  return DisplaySignalSlot{key, SignalType::Number, range, nullptr, 0, freshness_unset};
}
template <std::size_t N>
constexpr DisplaySignalSlot enumeration(std::string_view key, const LiveChoiceCode (&codes)[N]) {
  return DisplaySignalSlot{key, SignalType::Enum, DisplayRange::Signed, codes, N, false};
}
constexpr DisplaySignalSlot boolean(std::string_view key, bool freshness_unset = false) {
  return DisplaySignalSlot{key, SignalType::Boolean, DisplayRange::Signed, nullptr, 0,
                           freshness_unset};
}

// Index i is wire ID i + 1; never reorder.
constexpr std::array<DisplaySignalSlot, kDisplaySignalCount> kSlots{{
    number("vehicle.engine_rpm", DisplayRange::NonNegative),
    number("vehicle.speed_kph", DisplayRange::NonNegative),
    enumeration("vehicle.turn_state", kTurnCodes),
    boolean("vehicle.hazard_request"),
    boolean("vehicle.turn_request.left"),
    boolean("vehicle.turn_request.right"),
    boolean("vehicle.indicator_lamp.left"),
    boolean("vehicle.indicator_lamp.right"),
    enumeration("vehicle.selector_position", kSelectorCodes),
    enumeration("vehicle.actual_gear", kGearCodes),
    boolean("vehicle.liftgate_open"),
    boolean("vehicle.door.rear_right"),
    boolean("vehicle.door.rear_left"),
    boolean("vehicle.door.front_left_rhd"),
    boolean("vehicle.door.front_right_rhd"),
    boolean("vehicle.doors_unlocked"),
    boolean("vehicle.wiper.low"),
    enumeration("vehicle.wiper.front_position", kWiperCodes),
    boolean("vehicle.brake_pressed", true),
    number("vehicle.acceleration.longitudinal", DisplayRange::Signed),
    number("vehicle.acceleration.lateral", DisplayRange::Signed),
    number("vehicle.accelerator_pedal_position", DisplayRange::Fraction),
    number("vehicle.brake_pressure", DisplayRange::NonNegative, true),
}};

void write_u32(DisplaySnapshot &bytes, std::size_t offset, std::uint32_t value) noexcept {
  for (std::size_t index = 0; index < 4; ++index) {
    bytes[offset + index] = static_cast<std::uint8_t>(value >> (8U * index));
  }
}

std::optional<std::uint32_t> number_bits(DisplayRange range, const SignalValue &value) noexcept {
  const auto number = value.as_number();
  if (!number.has_value() || !std::isfinite(*number)) {
    return std::nullopt;
  }
  if ((range != DisplayRange::Signed && *number < 0.0F) ||
      (range == DisplayRange::Fraction && *number > 1.0F)) {
    return std::nullopt;
  }
  static_assert(sizeof(float) == sizeof(std::uint32_t), "binary32 float required");
  std::uint32_t bits = 0;
  std::memcpy(&bits, &*number, sizeof bits);
  return bits;
}

// The wire value of a usable reading, or std::nullopt when it cannot be sent.
std::optional<std::uint32_t> wire_value(const DisplaySignalSlot &slot, const SignalMetadata &signal,
                                        const SignalValue &value) noexcept {
  switch (slot.type) {
  case SignalType::Number:
    return number_bits(slot.range, value);
  case SignalType::Boolean: {
    const auto flag = value.as_boolean();
    return flag.has_value() ? std::optional<std::uint32_t>{*flag ? 1U : 0U} : std::nullopt;
  }
  case SignalType::Enum: {
    const auto raw = value.as_enumeration();
    const auto *choice = raw.has_value() ? signal.find_choice(*raw) : nullptr;
    const auto code = choice != nullptr ? slot.choice_code(choice->key) : std::nullopt;
    return code.has_value() ? std::optional<std::uint32_t>{*code} : std::nullopt;
  }
  }
  return std::nullopt;
}

struct Record {
  DisplayAvailability availability{DisplayAvailability::Unknown};
  std::uint32_t value{0};
};

Record record_for(const DisplaySignalSlot &slot, const SignalMetadata *signal,
                  const vehicle_signals::SignalProvider &provider) noexcept {
  if (signal == nullptr) {
    return Record{DisplayAvailability::NotSupported};
  }
  const auto result = provider.read(signal->id);
  if (!result.ok()) {
    return Record{static_cast<DisplayAvailability>(status_code(result.status))};
  }
  const auto &reading = *result.value;
  const bool usable = reading.availability == Availability::Fresh ||
                      reading.availability == Availability::FreshnessUnverified;
  if (!usable) {
    return Record{static_cast<DisplayAvailability>(status_code(reading.availability))};
  }
  const auto value =
      reading.value.has_value() ? wire_value(slot, *signal, *reading.value) : std::nullopt;
  if (!value.has_value()) {
    return Record{DisplayAvailability::Unknown};
  }
  const bool fresh = reading.availability == Availability::Fresh && !slot.freshness_unset;
  return Record{fresh ? DisplayAvailability::Fresh : DisplayAvailability::FreshnessUnverified,
                *value};
}

const SignalMetadata *resolve(const vehicle_signals::SignalCatalogView &catalog,
                              const DisplaySignalSlot &slot) noexcept {
  const SignalMetadata *signal = catalog.find(slot.key);
  if (signal == nullptr || signal->type != slot.type ||
      !signal->capabilities.has(SignalCapability::Read)) {
    return nullptr;
  }
  return signal;
}

} // namespace

std::optional<std::uint8_t>
DisplaySignalSlot::choice_code(std::string_view choice_key) const noexcept {
  for (std::size_t index = 0; choices != nullptr && index < choice_count; ++index) {
    if (choices[index].key == choice_key) {
      return choices[index].code;
    }
  }
  return std::nullopt;
}

const std::array<DisplaySignalSlot, kDisplaySignalCount> &display_signal_slots() noexcept {
  return kSlots;
}

DisplaySnapshotSampler::DisplaySnapshotSampler(const vehicle_signals::SignalProvider &provider) noexcept
    : provider_(provider) {
  const auto catalog = provider_.catalog();
  for (std::size_t index = 0; index < kDisplaySignalCount; ++index) {
    resolved_[index] = resolve(catalog, kSlots[index]);
  }
}

DisplaySnapshot DisplaySnapshotSampler::sample(bool telemetry_started, std::uint32_t session,
                                               std::uint32_t sequence) const noexcept {
  DisplaySnapshot bytes{};
  bytes[0] = 'E';
  bytes[1] = 'C';
  bytes[2] = 'A';
  bytes[3] = 'N';
  bytes[4] = kVersion;
  bytes[5] = telemetry_started ? kStartedFlag : std::uint8_t{0};
  bytes[6] = static_cast<std::uint8_t>(kDisplaySignalCount);
  write_u32(bytes, 8, session);
  write_u32(bytes, 12, sequence);
  for (std::size_t index = 0; index < kDisplaySignalCount; ++index) {
    const std::size_t offset = kHeaderBytes + kRecordBytes * index;
    const Record record =
        telemetry_started ? record_for(kSlots[index], resolved_[index], provider_) : Record{};
    bytes[offset] = static_cast<std::uint8_t>(index + 1);
    bytes[offset + 1] = static_cast<std::uint8_t>(kSlots[index].type == SignalType::Boolean ? 1
                                                  : kSlots[index].type == SignalType::Number
                                                      ? 2
                                                      : 3);
    bytes[offset + 2] = static_cast<std::uint8_t>(record.availability);
    write_u32(bytes, offset + 4, record.value);
  }
  return bytes;
}

} // namespace companion_protocol
