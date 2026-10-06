#include "companion_protocol/live_signal_layout.hpp"

// The only file in this library that names catalog signal keys: the live
// signals layout version 1 table is a protocol definition, keyed by the
// catalog's semantic keys so that no per-build SignalId reaches the wire.

namespace companion_protocol {

namespace {

using vehicle_signals::SignalType;

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

constexpr LiveSignalSlot number(std::string_view key, std::uint8_t offset, float scale) {
  return LiveSignalSlot{key, SignalType::Number, offset, scale, nullptr, 0, false};
}

template <std::size_t N>
constexpr LiveSignalSlot enumeration(std::string_view key, std::uint8_t offset,
                                     const LiveChoiceCode (&codes)[N]) {
  return LiveSignalSlot{key, SignalType::Enum, offset, 0.0F, codes, N, false};
}

constexpr LiveSignalSlot boolean(std::string_view key, std::uint8_t bit,
                                 bool freshness_unset = false) {
  return LiveSignalSlot{key, SignalType::Boolean, bit, 0.0F, nullptr, 0, freshness_unset};
}

constexpr std::array<LiveSignalSlot, kLiveSignalCount> kSlots{{
    number("vehicle.engine_rpm", 3, 4.0F),
    number("vehicle.speed_kph", 5, 100.0F),
    enumeration("vehicle.turn_state", 7, kTurnCodes),
    enumeration("vehicle.selector_position", 8, kSelectorCodes),
    enumeration("vehicle.actual_gear", 9, kGearCodes),
    enumeration("vehicle.wiper.front_position", 10, kWiperCodes),
    boolean("vehicle.hazard_request", 0),
    boolean("vehicle.turn_request.left", 1),
    boolean("vehicle.turn_request.right", 2),
    boolean("vehicle.indicator_lamp.left", 3),
    boolean("vehicle.indicator_lamp.right", 4),
    boolean("vehicle.liftgate_open", 5),
    boolean("vehicle.door.rear_right", 6),
    boolean("vehicle.door.rear_left", 7),
    boolean("vehicle.door.front_left_rhd", 8),
    boolean("vehicle.door.front_right_rhd", 9),
    boolean("vehicle.doors_unlocked", 10),
    boolean("vehicle.wiper.low", 11),
    // Brake freshness is intentionally unset, so the slot can never be Fresh.
    boolean("vehicle.brake_pressed", 12, true),
}};

} // namespace

std::optional<std::uint8_t>
LiveSignalSlot::choice_code(std::string_view choice_key) const noexcept {
  for (std::size_t index = 0; choices != nullptr && index < choice_count; ++index) {
    if (choices[index].key == choice_key) {
      return choices[index].code;
    }
  }
  return std::nullopt;
}

const std::array<LiveSignalSlot, kLiveSignalCount> &live_signal_slots() noexcept { return kSlots; }

} // namespace companion_protocol
