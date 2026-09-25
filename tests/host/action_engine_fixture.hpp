#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <ostream>
#include <sstream>
#include <vector>

#include <doctest/doctest.h>

#include "action_engine/action.hpp"
#include "vehicle_signals/signal_catalog.hpp"
#include "vehicle_signals/signal_contracts.hpp"

// Shared, make-independent fixture for the action-engine host tests: a small
// catalog with one Notify signal of each type, Read-only and Notify-only
// Number signals, and a notice builder.

namespace action_engine_fixture {

using vehicle_signals::Availability;
using vehicle_signals::SignalCapabilities;
using vehicle_signals::SignalCapability;
using vehicle_signals::SignalEnumChoice;
using vehicle_signals::SignalId;
using vehicle_signals::SignalMetadata;
using vehicle_signals::SignalNotification;
using vehicle_signals::SignalType;
using vehicle_signals::SignalUnit;
using vehicle_signals::SignalValue;
using vehicle_signals::ValidationStatus;

inline constexpr SignalId kDoorOpen{1};
inline constexpr SignalId kSpeed{2};
inline constexpr SignalId kGear{3};
inline constexpr SignalId kOdometer{4};
inline constexpr SignalId kRpm{5};
inline constexpr SignalId kFanLevel{6};

inline constexpr std::uint16_t kPark = 0;
inline constexpr std::uint16_t kReverse = 1;
inline constexpr std::uint16_t kNeutral = 2;
inline constexpr std::uint16_t kDrive = 3;

inline constexpr SignalEnumChoice kGearChoices[] = {
    {kPark, "park"},
    {kReverse, "reverse"},
    {kNeutral, "neutral"},
    {kDrive, "drive"},
};

inline constexpr SignalCapabilities kReadNotify = SignalCapability::Read | SignalCapability::Notify;

inline constexpr SignalMetadata kCatalog[] = {
    {kDoorOpen, "body.door_open", SignalType::Boolean, SignalUnit::None, ValidationStatus::Observed,
     kReadNotify, nullptr, 0},
    {kSpeed, "motion.speed_kph", SignalType::Number, SignalUnit::KilometresPerHour,
     ValidationStatus::Reference, kReadNotify, nullptr, 0},
    {kGear, "transmission.gear", SignalType::Enum, SignalUnit::None, ValidationStatus::Confirmed,
     kReadNotify, kGearChoices, std::size(kGearChoices)},
    {kOdometer, "trip.odometer_km", SignalType::Number, SignalUnit::None,
     ValidationStatus::Reference, SignalCapability::Read, nullptr, 0},
    {kRpm, "engine.rpm", SignalType::Number, SignalUnit::RevolutionsPerMinute,
     ValidationStatus::Reference, SignalCapability::Read, nullptr, 0},
    {kFanLevel, "climate.fan_level", SignalType::Number, SignalUnit::None,
     ValidationStatus::Reference, SignalCapability::Notify, nullptr, 0},
};

inline constexpr vehicle_signals::SignalCatalogView kView{kCatalog};
static_assert(kView.well_formed());

// Builds a latest-state notice: Notice{kDoorOpen}.value(boolean(true)).initial().
class Notice final {
public:
  explicit Notice(SignalId id) noexcept { notice_.id = id; }

  Notice &value(SignalValue value, Availability availability = Availability::Fresh) noexcept {
    notice_.current.value = value;
    notice_.current.availability = availability;
    return *this;
  }
  Notice &no_data() noexcept {
    notice_.current.value.reset();
    notice_.current.availability = Availability::NoData;
    return *this;
  }
  Notice &initial() noexcept {
    notice_.initial = true;
    return *this;
  }
  Notice &became_unavailable() noexcept {
    notice_.became_unavailable = true;
    return *this;
  }
  Notice &recovered() noexcept {
    notice_.recovered = true;
    return *this;
  }
  Notice &coalesced() noexcept {
    notice_.coalesced = true;
    return *this;
  }

  operator SignalNotification() const noexcept { return notice_; } // NOLINT: implicit by design

private:
  SignalNotification notice_{};
};

inline SignalNotification door(bool open, Availability availability = Availability::Fresh) {
  return Notice{kDoorOpen}.value(SignalValue::boolean(open), availability);
}
inline SignalNotification speed(float kph, Availability availability = Availability::Fresh) {
  return Notice{kSpeed}.value(SignalValue::number(kph), availability);
}
inline SignalNotification gear(std::uint16_t raw, Availability availability = Availability::Fresh) {
  return Notice{kGear}.value(SignalValue::enumeration(raw), availability);
}

inline action_engine::ActionCommand activate(std::uint16_t action) {
  return {action_engine::ActionId{action}, action_engine::ActionCommandKind::Activate};
}
inline action_engine::ActionCommand deactivate(std::uint16_t action) {
  return {action_engine::ActionId{action}, action_engine::ActionCommandKind::Deactivate};
}
inline action_engine::ActionCommand trigger(std::uint16_t action) {
  return {action_engine::ActionId{action}, action_engine::ActionCommandKind::Trigger};
}
inline action_engine::ActionCommand set_level(std::uint16_t action, float level) {
  return {action_engine::ActionId{action}, action_engine::ActionCommandKind::SetLevel, level};
}

} // namespace action_engine_fixture

namespace action_engine {

// Readable doctest failure output, for example "Activate(7)" or
// "SetLevel(3, 0.5)".
inline std::ostream &operator<<(std::ostream &stream, const ActionCommand &command) {
  constexpr const char *kNames[] = {"Activate", "Deactivate", "Trigger", "SetLevel"};
  stream << kNames[static_cast<std::size_t>(command.kind)] << '(' << command.action.value();
  if (command.kind == ActionCommandKind::SetLevel) {
    stream << ", " << command.level;
  }
  return stream << ')';
}

} // namespace action_engine

namespace doctest {

template <> struct StringMaker<std::optional<action_engine::ActionCommand>> {
  static String convert(const std::optional<action_engine::ActionCommand> &command) {
    if (!command.has_value()) {
      return String{"none"};
    }
    std::ostringstream stream;
    stream << *command;
    return String{stream.str().c_str()};
  }
};

template <> struct StringMaker<std::vector<action_engine::ActionCommand>> {
  static String convert(const std::vector<action_engine::ActionCommand> &commands) {
    std::ostringstream stream;
    stream << '[';
    for (std::size_t index = 0; index < commands.size(); ++index) {
      stream << (index == 0 ? "" : ", ") << commands[index];
    }
    stream << ']';
    return String{stream.str().c_str()};
  }
};

} // namespace doctest
