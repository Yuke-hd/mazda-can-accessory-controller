#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string_view>

#include "mazda/definitions.hpp"
#include "mazda/types.hpp"
#include "vehicle_signals/signal_catalog.hpp"
#include "vehicle_signals/signal_contracts.hpp"

namespace mazda::internal {

// Mazda-owned generic signal catalog. It is implementation-only: the public
// MazdaSignalProvider returns a SignalCatalogView over it, while the Mazda enum
// values, state bindings and channels stay behind this private include root.
//
// Numeric ids are per-build runtime handles (see vehicle_signals::SignalId).
// The production polling/notification descriptors carry these same constants,
// so each catalog row is bound to exactly one typed descriptor.
namespace signal_ids {
inline constexpr vehicle_signals::SignalId kEngineRpm{1};
inline constexpr vehicle_signals::SignalId kSpeedKph{2};
inline constexpr vehicle_signals::SignalId kTurnState{3};
inline constexpr vehicle_signals::SignalId kHazardRequest{4};
inline constexpr vehicle_signals::SignalId kTurnRequestLeft{5};
inline constexpr vehicle_signals::SignalId kTurnRequestRight{6};
inline constexpr vehicle_signals::SignalId kIndicatorLampLeft{7};
inline constexpr vehicle_signals::SignalId kIndicatorLampRight{8};
inline constexpr vehicle_signals::SignalId kSelectorPosition{9};
inline constexpr vehicle_signals::SignalId kActualGear{10};
inline constexpr vehicle_signals::SignalId kLiftgateOpen{11};
inline constexpr vehicle_signals::SignalId kDoorRearRight{12};
inline constexpr vehicle_signals::SignalId kDoorRearLeft{13};
inline constexpr vehicle_signals::SignalId kDoorFrontLeftRhd{14};
inline constexpr vehicle_signals::SignalId kDoorFrontRightRhd{15};
inline constexpr vehicle_signals::SignalId kDoorsUnlocked{16};
inline constexpr vehicle_signals::SignalId kWiperLow{17};
inline constexpr vehicle_signals::SignalId kWiperFrontPosition{18};
} // namespace signal_ids

// Enum choice values are the raw Mazda enumerator values; keys are the
// persistent symbolic names. Every enumerator, including Unknown, is listed.
template <typename Enum> [[nodiscard]] constexpr std::uint16_t choice_value(Enum value) noexcept {
  return static_cast<std::uint16_t>(value);
}

inline constexpr vehicle_signals::SignalEnumChoice kTurnStateChoices[] = {
    {choice_value(TurnState::Unknown), "unknown"}, {choice_value(TurnState::Off), "off"},
    {choice_value(TurnState::Left), "left"},       {choice_value(TurnState::Right), "right"},
    {choice_value(TurnState::Hazard), "hazard"},
};

inline constexpr vehicle_signals::SignalEnumChoice kSelectorPositionChoices[] = {
    {choice_value(SelectorPosition::Unknown), "unknown"},
    {choice_value(SelectorPosition::Shifting), "shifting"},
    {choice_value(SelectorPosition::Park), "park"},
    {choice_value(SelectorPosition::Reverse), "reverse"},
    {choice_value(SelectorPosition::Neutral), "neutral"},
    {choice_value(SelectorPosition::Drive), "drive"},
};

inline constexpr vehicle_signals::SignalEnumChoice kActualGearChoices[] = {
    {choice_value(ActualGear::Unknown), "unknown"},
    {choice_value(ActualGear::ParkOrNeutral), "park_or_neutral"},
    {choice_value(ActualGear::Park), "park"},
    {choice_value(ActualGear::Neutral), "neutral"},
    {choice_value(ActualGear::Reverse), "reverse"},
    {choice_value(ActualGear::First), "first"},
    {choice_value(ActualGear::Second), "second"},
    {choice_value(ActualGear::Third), "third"},
    {choice_value(ActualGear::Fourth), "fourth"},
    {choice_value(ActualGear::Fifth), "fifth"},
    {choice_value(ActualGear::Sixth), "sixth"},
    {choice_value(ActualGear::Shifting), "shifting"},
};

inline constexpr vehicle_signals::SignalEnumChoice kFrontWiperPositionChoices[] = {
    {choice_value(FrontWiperPosition::Unknown), "unknown"},
    {choice_value(FrontWiperPosition::Off), "off"},
    {choice_value(FrontWiperPosition::On), "on"},
    {choice_value(FrontWiperPosition::High), "high"},
    {choice_value(FrontWiperPosition::Intermittent), "intermittent"},
};

namespace catalog_detail {

using vehicle_signals::SignalCapability;
using vehicle_signals::SignalEnumChoice;
using vehicle_signals::SignalId;
using vehicle_signals::SignalMetadata;
using vehicle_signals::SignalType;
using vehicle_signals::SignalUnit;
using vehicle_signals::ValidationStatus;

// RPM and speed are backed by polling descriptors only.
[[nodiscard]] constexpr SignalMetadata polled_number(SignalId id, std::string_view key,
                                                     SignalUnit unit,
                                                     ValidationStatus validation) noexcept {
  return {id, key, SignalType::Number, unit, validation, SignalCapability::Read, nullptr, 0};
}

// Discrete signals are backed by a typed notification channel.
[[nodiscard]] constexpr SignalMetadata notified_boolean(SignalId id, std::string_view key,
                                                        ValidationStatus validation) noexcept {
  return {id,
          key,
          SignalType::Boolean,
          SignalUnit::None,
          validation,
          SignalCapability::Read | SignalCapability::Notify,
          nullptr,
          0};
}

template <std::size_t N>
[[nodiscard]] constexpr SignalMetadata
notified_enum(SignalId id, std::string_view key, ValidationStatus validation,
              const SignalEnumChoice (&choices)[N]) noexcept {
  return {id,
          key,
          SignalType::Enum,
          SignalUnit::None,
          validation,
          SignalCapability::Read | SignalCapability::Notify,
          choices,
          N};
}

} // namespace catalog_detail

// Validation mirrors the production descriptors: the notification descriptors
// use the same candidate definition confidence, and the RPM/speed polling
// descriptors carry the RPM/speed definition confidence.
inline constexpr vehicle_signals::SignalMetadata kSignalCatalog[] = {
    catalog_detail::polled_number(signal_ids::kEngineRpm, "vehicle.engine_rpm",
                                  vehicle_signals::SignalUnit::RevolutionsPerMinute,
                                  candidate::kEngineRpmDefinition.confidence),
    catalog_detail::polled_number(signal_ids::kSpeedKph, "vehicle.speed_kph",
                                  vehicle_signals::SignalUnit::KilometresPerHour,
                                  candidate::kEngineSpeedDefinition.confidence),
    catalog_detail::notified_enum(signal_ids::kTurnState, "vehicle.turn_state",
                                  candidate::kTurnLeftSwitchDefinition.confidence,
                                  kTurnStateChoices),
    catalog_detail::notified_boolean(signal_ids::kHazardRequest, "vehicle.hazard_request",
                                     candidate::kHazardDefinition.confidence),
    catalog_detail::notified_boolean(signal_ids::kTurnRequestLeft, "vehicle.turn_request.left",
                                     candidate::kTurnLeftSwitchDefinition.confidence),
    catalog_detail::notified_boolean(signal_ids::kTurnRequestRight, "vehicle.turn_request.right",
                                     candidate::kTurnRightSwitchDefinition.confidence),
    catalog_detail::notified_boolean(signal_ids::kIndicatorLampLeft, "vehicle.indicator_lamp.left",
                                     candidate::kLeftIndicatorLampDefinition.confidence),
    catalog_detail::notified_boolean(signal_ids::kIndicatorLampRight,
                                     "vehicle.indicator_lamp.right",
                                     candidate::kRightIndicatorLampDefinition.confidence),
    catalog_detail::notified_enum(signal_ids::kSelectorPosition, "vehicle.selector_position",
                                  candidate::kSelectorDefinition.confidence,
                                  kSelectorPositionChoices),
    catalog_detail::notified_enum(signal_ids::kActualGear, "vehicle.actual_gear",
                                  candidate::kActualGearDefinition.confidence, kActualGearChoices),
    catalog_detail::notified_boolean(signal_ids::kLiftgateOpen, "vehicle.liftgate_open",
                                     candidate::kLiftgateOpenDefinition.confidence),
    catalog_detail::notified_boolean(signal_ids::kDoorRearRight, "vehicle.door.rear_right",
                                     candidate::kRearRightDoorOpenDefinition.confidence),
    catalog_detail::notified_boolean(signal_ids::kDoorRearLeft, "vehicle.door.rear_left",
                                     candidate::kRearLeftDoorOpenDefinition.confidence),
    catalog_detail::notified_boolean(signal_ids::kDoorFrontLeftRhd, "vehicle.door.front_left_rhd",
                                     candidate::kFrontLeftDoorOpenRhdDefinition.confidence),
    catalog_detail::notified_boolean(signal_ids::kDoorFrontRightRhd, "vehicle.door.front_right_rhd",
                                     candidate::kFrontRightDoorOpenRhdDefinition.confidence),
    catalog_detail::notified_boolean(signal_ids::kDoorsUnlocked, "vehicle.doors_unlocked",
                                     candidate::kDoorsUnlockedDefinition.confidence),
    catalog_detail::notified_boolean(signal_ids::kWiperLow, "vehicle.wiper.low",
                                     candidate::kWiperLowDefinition.confidence),
    catalog_detail::notified_enum(signal_ids::kWiperFrontPosition, "vehicle.wiper.front_position",
                                  candidate::kFrontWiperDefinition.confidence,
                                  kFrontWiperPositionChoices),
};

inline constexpr std::size_t kSignalCatalogSize = 18;

static_assert(std::size(kSignalCatalog) == kSignalCatalogSize);
static_assert(vehicle_signals::SignalCatalogView{kSignalCatalog}.well_formed());

[[nodiscard]] constexpr vehicle_signals::SignalCatalogView signal_catalog() noexcept {
  return vehicle_signals::SignalCatalogView{kSignalCatalog};
}

} // namespace mazda::internal
