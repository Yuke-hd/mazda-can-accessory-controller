#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "mazda/types.hpp"
#include "vehicle_signals/catalog.hpp"

namespace mazda::internal::signals {

// Stable generic identities are independent of CAN frame identifiers. The
// zero value remains reserved by vehicle_signals::SignalId as invalid.
enum class Id : std::uint16_t {
  Invalid = 0,
  EngineRpm = 1,
  SpeedKph = 2,
  TurnState = 3,
  HazardRequest = 4,
  TurnRequestLeft = 5,
  TurnRequestRight = 6,
  IndicatorLampLeft = 7,
  IndicatorLampRight = 8,
  SelectorPosition = 9,
  ActualGear = 10,
  LiftgateOpen = 11,
  DoorRearRight = 12,
  DoorRearLeft = 13,
  DoorFrontLeftRhd = 14,
  DoorFrontRightRhd = 15,
  DoorsUnlocked = 16,
  WiperLow = 17,
  FrontWiperPosition = 18,
};

[[nodiscard]] constexpr vehicle_signals::SignalId id(const Id value) noexcept {
  return {static_cast<std::uint16_t>(value)};
}

inline constexpr vehicle_signals::SignalId kEngineRpm{id(Id::EngineRpm)};
inline constexpr vehicle_signals::SignalId kSpeedKph{id(Id::SpeedKph)};
inline constexpr vehicle_signals::SignalId kTurnState{id(Id::TurnState)};
inline constexpr vehicle_signals::SignalId kHazardRequest{id(Id::HazardRequest)};
inline constexpr vehicle_signals::SignalId kTurnRequestLeft{id(Id::TurnRequestLeft)};
inline constexpr vehicle_signals::SignalId kTurnRequestRight{id(Id::TurnRequestRight)};
inline constexpr vehicle_signals::SignalId kIndicatorLampLeft{id(Id::IndicatorLampLeft)};
inline constexpr vehicle_signals::SignalId kIndicatorLampRight{id(Id::IndicatorLampRight)};
inline constexpr vehicle_signals::SignalId kSelectorPosition{id(Id::SelectorPosition)};
inline constexpr vehicle_signals::SignalId kActualGear{id(Id::ActualGear)};
inline constexpr vehicle_signals::SignalId kLiftgateOpen{id(Id::LiftgateOpen)};
inline constexpr vehicle_signals::SignalId kDoorRearRight{id(Id::DoorRearRight)};
inline constexpr vehicle_signals::SignalId kDoorRearLeft{id(Id::DoorRearLeft)};
inline constexpr vehicle_signals::SignalId kDoorFrontLeftRhd{id(Id::DoorFrontLeftRhd)};
inline constexpr vehicle_signals::SignalId kDoorFrontRightRhd{id(Id::DoorFrontRightRhd)};
inline constexpr vehicle_signals::SignalId kDoorsUnlocked{id(Id::DoorsUnlocked)};
inline constexpr vehicle_signals::SignalId kWiperLow{id(Id::WiperLow)};
inline constexpr vehicle_signals::SignalId kFrontWiperPosition{id(Id::FrontWiperPosition)};

inline constexpr std::size_t kSignalCount = 18;

inline constexpr std::array<vehicle_signals::SignalChoice, 5> kTurnStateChoices{{
    {static_cast<std::int32_t>(TurnState::Unknown), "unknown"},
    {static_cast<std::int32_t>(TurnState::Off), "off"},
    {static_cast<std::int32_t>(TurnState::Left), "left"},
    {static_cast<std::int32_t>(TurnState::Right), "right"},
    {static_cast<std::int32_t>(TurnState::Hazard), "hazard"},
}};

inline constexpr std::array<vehicle_signals::SignalChoice, 6> kSelectorPositionChoices{{
    {static_cast<std::int32_t>(SelectorPosition::Unknown), "unknown"},
    {static_cast<std::int32_t>(SelectorPosition::Shifting), "shifting"},
    {static_cast<std::int32_t>(SelectorPosition::Park), "park"},
    {static_cast<std::int32_t>(SelectorPosition::Reverse), "reverse"},
    {static_cast<std::int32_t>(SelectorPosition::Neutral), "neutral"},
    {static_cast<std::int32_t>(SelectorPosition::Drive), "drive"},
}};

inline constexpr std::array<vehicle_signals::SignalChoice, 12> kActualGearChoices{{
    {static_cast<std::int32_t>(ActualGear::Unknown), "unknown"},
    {static_cast<std::int32_t>(ActualGear::ParkOrNeutral), "park_or_neutral"},
    {static_cast<std::int32_t>(ActualGear::Park), "park"},
    {static_cast<std::int32_t>(ActualGear::Neutral), "neutral"},
    {static_cast<std::int32_t>(ActualGear::Reverse), "reverse"},
    {static_cast<std::int32_t>(ActualGear::First), "first"},
    {static_cast<std::int32_t>(ActualGear::Second), "second"},
    {static_cast<std::int32_t>(ActualGear::Third), "third"},
    {static_cast<std::int32_t>(ActualGear::Fourth), "fourth"},
    {static_cast<std::int32_t>(ActualGear::Fifth), "fifth"},
    {static_cast<std::int32_t>(ActualGear::Sixth), "sixth"},
    {static_cast<std::int32_t>(ActualGear::Shifting), "shifting"},
}};

inline constexpr std::array<vehicle_signals::SignalChoice, 5> kFrontWiperPositionChoices{{
    {static_cast<std::int32_t>(FrontWiperPosition::Unknown), "unknown"},
    {static_cast<std::int32_t>(FrontWiperPosition::Off), "off"},
    {static_cast<std::int32_t>(FrontWiperPosition::On), "on"},
    {static_cast<std::int32_t>(FrontWiperPosition::High), "high"},
    {static_cast<std::int32_t>(FrontWiperPosition::Intermittent), "intermittent"},
}};

namespace detail {
inline constexpr vehicle_signals::SignalCapabilities kReadOnly{true, false, false};
inline constexpr vehicle_signals::SignalCapabilities kReadNotify{true, true, false};
using vehicle_signals::SignalKind;
using vehicle_signals::SignalMetadata;
using vehicle_signals::SignalUnit;
using vehicle_signals::SignalValidation;
} // namespace detail

// The order is the public catalog order. Request signals, normalized turn
// state, and indicator-lamp observations intentionally use separate keys.
inline constexpr std::array<vehicle_signals::SignalMetadata, kSignalCount> kMetadata{{
    {kEngineRpm, "vehicle.engine_rpm", "Engine speed", detail::SignalKind::Number,
     detail::SignalUnit::RevolutionsPerMinute, detail::SignalValidation::Confirmed,
     detail::kReadOnly, 0.0, 8500.0, nullptr, 0},
    {kSpeedKph, "vehicle.speed_kph", "Vehicle speed", detail::SignalKind::Number,
     detail::SignalUnit::KilometresPerHour, detail::SignalValidation::Reference, detail::kReadOnly,
     0.0, std::nullopt, nullptr, 0},
    {kTurnState, "vehicle.turn_state", "Normalized turn state", detail::SignalKind::Enum,
     detail::SignalUnit::None, detail::SignalValidation::Reference, detail::kReadNotify,
     std::nullopt, std::nullopt, kTurnStateChoices.data(), kTurnStateChoices.size()},
    {kHazardRequest, "vehicle.hazard_request", "Hazard switch request", detail::SignalKind::Boolean,
     detail::SignalUnit::Boolean, detail::SignalValidation::Reference, detail::kReadNotify,
     std::nullopt, std::nullopt, nullptr, 0},
    {kTurnRequestLeft, "vehicle.turn_request.left", "Left turn request",
     detail::SignalKind::Boolean, detail::SignalUnit::Boolean, detail::SignalValidation::Reference,
     detail::kReadNotify, std::nullopt, std::nullopt, nullptr, 0},
    {kTurnRequestRight, "vehicle.turn_request.right", "Right turn request",
     detail::SignalKind::Boolean, detail::SignalUnit::Boolean, detail::SignalValidation::Reference,
     detail::kReadNotify, std::nullopt, std::nullopt, nullptr, 0},
    {kIndicatorLampLeft, "vehicle.indicator_lamp.left", "Left indicator lamp",
     detail::SignalKind::Boolean, detail::SignalUnit::Boolean, detail::SignalValidation::Reference,
     detail::kReadNotify, std::nullopt, std::nullopt, nullptr, 0},
    {kIndicatorLampRight, "vehicle.indicator_lamp.right", "Right indicator lamp",
     detail::SignalKind::Boolean, detail::SignalUnit::Boolean, detail::SignalValidation::Reference,
     detail::kReadNotify, std::nullopt, std::nullopt, nullptr, 0},
    {kSelectorPosition, "vehicle.selector_position", "Transmission selector position",
     detail::SignalKind::Enum, detail::SignalUnit::None, detail::SignalValidation::Confirmed,
     detail::kReadNotify, std::nullopt, std::nullopt, kSelectorPositionChoices.data(),
     kSelectorPositionChoices.size()},
    {kActualGear, "vehicle.actual_gear", "Actual transmission gear", detail::SignalKind::Enum,
     detail::SignalUnit::None, detail::SignalValidation::Observed, detail::kReadNotify,
     std::nullopt, std::nullopt, kActualGearChoices.data(), kActualGearChoices.size()},
    {kLiftgateOpen, "vehicle.liftgate_open", "Liftgate open", detail::SignalKind::Boolean,
     detail::SignalUnit::Boolean, detail::SignalValidation::Reference, detail::kReadNotify,
     std::nullopt, std::nullopt, nullptr, 0},
    {kDoorRearRight, "vehicle.door.rear_right", "Rear right door open", detail::SignalKind::Boolean,
     detail::SignalUnit::Boolean, detail::SignalValidation::Reference, detail::kReadNotify,
     std::nullopt, std::nullopt, nullptr, 0},
    {kDoorRearLeft, "vehicle.door.rear_left", "Rear left door open", detail::SignalKind::Boolean,
     detail::SignalUnit::Boolean, detail::SignalValidation::Reference, detail::kReadNotify,
     std::nullopt, std::nullopt, nullptr, 0},
    {kDoorFrontLeftRhd, "vehicle.door.front_left_rhd", "Front left door open (RHD)",
     detail::SignalKind::Boolean, detail::SignalUnit::Boolean, detail::SignalValidation::Reference,
     detail::kReadNotify, std::nullopt, std::nullopt, nullptr, 0},
    {kDoorFrontRightRhd, "vehicle.door.front_right_rhd", "Front right door open (RHD)",
     detail::SignalKind::Boolean, detail::SignalUnit::Boolean, detail::SignalValidation::Confirmed,
     detail::kReadNotify, std::nullopt, std::nullopt, nullptr, 0},
    {kDoorsUnlocked, "vehicle.doors_unlocked", "Doors unlocked", detail::SignalKind::Boolean,
     detail::SignalUnit::Boolean, detail::SignalValidation::Reference, detail::kReadNotify,
     std::nullopt, std::nullopt, nullptr, 0},
    {kWiperLow, "vehicle.wiper.low", "Low speed front wiper", detail::SignalKind::Boolean,
     detail::SignalUnit::Boolean, detail::SignalValidation::Observed, detail::kReadNotify,
     std::nullopt, std::nullopt, nullptr, 0},
    {kFrontWiperPosition, "vehicle.wiper.front_position", "Front wiper position",
     detail::SignalKind::Enum, detail::SignalUnit::None, detail::SignalValidation::Observed,
     detail::kReadNotify, std::nullopt, std::nullopt, kFrontWiperPositionChoices.data(),
     kFrontWiperPositionChoices.size()},
}};

[[nodiscard]] constexpr vehicle_signals::SignalCatalogView catalog() noexcept {
  return {kMetadata.data(), kMetadata.size()};
}

} // namespace mazda::internal::signals
