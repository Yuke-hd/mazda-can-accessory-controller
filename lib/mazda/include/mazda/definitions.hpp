#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include "mazda/freshness.hpp"
#include "vehicle_core/frame.hpp"
#include "vehicle_core/signal.hpp"
#include "vehicle_core/telemetry_contracts.hpp"

namespace mazda::candidate {

constexpr std::uint32_t kEngineDataId = 0x202;
constexpr std::uint32_t kTransmissionId = 0x228;
// Retain the original symbol for callers that used the candidate name.
constexpr std::uint32_t kGearId = kTransmissionId;
constexpr std::uint32_t kDoorsId = 0x43e;
constexpr std::uint32_t kTurnSwitchId = 0x091;
constexpr std::uint32_t kBlinkInfoId = 0x09a;
constexpr std::uint32_t kBrakePedalId = 0x165;
constexpr std::uint8_t kCandidateDlc = 8;

struct CandidateMessageDefinition {
  const char *name;
  std::uint32_t identifier;
  std::uint8_t expected_dlc;
  std::optional<vehicle_core::Microseconds> expected_period_us;
  std::optional<vehicle_core::Microseconds> freshness_timeout_us;
  bool pending_validation;
  const char *provenance;
};

struct CandidateSignalDefinition {
  const char *name;
  std::uint32_t identifier;
  // Lowest payload bit coordinate in the decoder's byte/LSB numbering. For
  // Intel signals this is the DBC least-significant start bit; for Motorola
  // signals it is the lowest coordinate reached by walking from the DBC
  // most-significant bit through the sawtooth byte order. Numeric big-endian
  // fields are documented by byte offset in the MCAN-14 note.
  std::uint8_t start_bit;
  std::uint8_t bit_length;
  float scale;
  float offset;
  vehicle_core::SignalUnit unit;
  float physical_min;
  float physical_max;
  const char *invalid_values;
  // The reviewed DBC start bit and byte order are retained separately so the
  // decoder-facing coordinates above remain source-compatible.
  std::uint8_t dbc_start_bit{0};
  enum class ByteOrder : std::uint8_t { Motorola, Intel };
  ByteOrder byte_order{ByteOrder::Motorola};
  const char *value_table{nullptr};
  const char *provenance{nullptr};
  // Evidence confidence is independent from decoder validity and runtime
  // availability. In particular, synthetic vectors cannot promote a
  // source-only mapping to Observed or Confirmed.
  vehicle_core::ValidationStatus confidence{vehicle_core::ValidationStatus::Reference};
};

using EvidenceConfidence = vehicle_core::ValidationStatus;
using SignalConfidence = EvidenceConfidence;

inline constexpr const char *kCaptureConfirmedProvenance =
    "capture-derived Mazda custom DBC; confirmed from passive real-world capture";

inline constexpr const char *kEngineRpmEvidenceProvenance =
    "docs/development/signal-evidence.md; docs/protocol/mazda_custom.dbc#L95";
inline constexpr const char *kSelectorEvidenceProvenance =
    "docs/development/signal-evidence.md; docs/protocol/mazda_custom.dbc#L98";
inline constexpr const char *kActualGearEvidenceProvenance =
    "docs/development/signal-evidence.md; docs/protocol/mazda_custom.dbc#L99";
inline constexpr const char *kFrontRightDoorEvidenceProvenance =
    "docs/development/signal-evidence.md; docs/protocol/mazda_custom.dbc#L101";
inline constexpr const char *kWiperLowEvidenceProvenance =
    "docs/development/signal-evidence.md; docs/protocol/mazda_custom.dbc#L108";
inline constexpr const char *kFrontWiperEvidenceProvenance =
    "docs/development/signal-evidence.md; docs/protocol/mazda_custom.dbc#L110";
inline constexpr const char *kSourceOnlyEvidenceProvenance =
    "docs/development/signal-evidence.md; reviewed source mapping in "
    "docs/protocol/mazda_custom.dbc";
inline constexpr const char *kFrontOtherDoorEvidenceProvenance =
    "docs/development/signal-evidence.md; docs/protocol/mazda_custom.dbc#L49 and #L102; "
    "FrontOtherDoor is a Reference-only source field interpreted as a front-left RHD channel";
inline constexpr const char *kSpeedCandidateEvidenceProvenance =
    "docs/development/signal-evidence.md; retained SPEED candidate has no field in the reviewed "
    "Mazda custom DBC";

// The capture-derived DBC has no cycle-time declaration. Null periods/timeouts
// are intentional until reviewed replay or isolated-bench evidence establishes
// them; callers can supply per-signal timeouts through VehicleFreshnessPolicy.
inline constexpr CandidateMessageDefinition kEngineDataDefinition{"ENGINE_DATA",
                                                                  kEngineDataId,
                                                                  kCandidateDlc,
                                                                  std::nullopt,
                                                                  std::nullopt,
                                                                  false,
                                                                  kCaptureConfirmedProvenance};
inline constexpr CandidateMessageDefinition kGearDefinition{"TRANSMISSION",
                                                            kTransmissionId,
                                                            kCandidateDlc,
                                                            std::nullopt,
                                                            std::nullopt,
                                                            false,
                                                            kCaptureConfirmedProvenance};
inline constexpr const CandidateMessageDefinition &kTransmissionDefinition = kGearDefinition;
inline constexpr CandidateMessageDefinition kDoorsDefinition{"DOORS",
                                                             kDoorsId,
                                                             kCandidateDlc,
                                                             std::nullopt,
                                                             std::nullopt,
                                                             false,
                                                             kCaptureConfirmedProvenance};
inline constexpr CandidateMessageDefinition kTurnSwitchDefinition{"TURN_SWITCH",
                                                                  kTurnSwitchId,
                                                                  kCandidateDlc,
                                                                  std::nullopt,
                                                                  kTurnFreshnessTimeoutUs,
                                                                  false,
                                                                  kCaptureConfirmedProvenance};
inline constexpr CandidateMessageDefinition kBlinkInfoDefinition{"BLINK_INFO",
                                                                 kBlinkInfoId,
                                                                 kCandidateDlc,
                                                                 std::nullopt,
                                                                 std::nullopt,
                                                                 false,
                                                                 kCaptureConfirmedProvenance};
inline constexpr CandidateMessageDefinition kBrakePedalDefinition{"BRAKE_PEDAL",
                                                                  kBrakePedalId,
                                                                  kCandidateDlc,
                                                                  std::nullopt,
                                                                  std::nullopt,
                                                                  false,
                                                                  kCaptureConfirmedProvenance};

inline constexpr CandidateSignalDefinition kEngineRpmDefinition{
    "EngineRPM",
    kEngineDataId,
    0,
    16,
    0.25F,
    0.0F,
    vehicle_core::SignalUnit::RevolutionsPerMinute,
    0.0F,
    8500.0F,
    "raw values above 34000 (8500 rpm)",
    7,
    CandidateSignalDefinition::ByteOrder::Motorola,
    "none",
    kEngineRpmEvidenceProvenance,
    vehicle_core::ValidationStatus::Confirmed};
inline constexpr CandidateSignalDefinition kEngineSpeedDefinition{
    "SPEED",
    kEngineDataId,
    16,
    16,
    0.01F,
    0.0F,
    vehicle_core::SignalUnit::KilometresPerHour,
    0.0F,
    655.35F,
    "none declared by source; 655.35 is the representable 16-bit encoding maximum, not a "
    "validated vehicle limit",
    0,
    CandidateSignalDefinition::ByteOrder::Motorola,
    nullptr,
    kSpeedCandidateEvidenceProvenance,
    vehicle_core::ValidationStatus::Reference};
inline constexpr CandidateSignalDefinition kSelectorDefinition{
    "Selector",
    kGearId,
    0,
    3,
    1.0F,
    0.0F,
    vehicle_core::SignalUnit::None,
    0.0F,
    7.0F,
    "0=Shifting; 5=Unknown_5; 6=Unknown_6; 7=Unknown_7",
    2,
    CandidateSignalDefinition::ByteOrder::Motorola,
    "0=Shifting; 1=Park; 2=Reverse; 3=Neutral; 4=Drive; 5=Unknown_5; 6=Unknown_6; 7=Unknown_7",
    kSelectorEvidenceProvenance,
    vehicle_core::ValidationStatus::Confirmed};
inline constexpr CandidateSignalDefinition kActualGearDefinition{
    "ActualGear",
    kGearId,
    33,
    4,
    1.0F,
    0.0F,
    vehicle_core::SignalUnit::None,
    0.0F,
    15.0F,
    "7=Unknown_7; 8=Unknown_8; 9=Unknown_9; 10=Unknown_10; 11=Unknown_11; 12=Unknown_12; "
    "13=Unknown_13; 15=Shifting",
    36,
    CandidateSignalDefinition::ByteOrder::Motorola,
    "0=P_or_N; 1=1st; 2=2nd; 3=3rd; 4=4th; 5=5th; 6=6th; 7=Unknown_7; 8=Unknown_8; 9=Unknown_9; "
    "10=Unknown_10; 11=Unknown_11; 12=Unknown_12; 13=Unknown_13; 14=Reverse; 15=Shifting",
    kActualGearEvidenceProvenance,
    vehicle_core::ValidationStatus::Observed};

#define MAZDA_BOOLEAN_DEFINITION(symbol, label, identifier_value, bit, dbc_bit, order, table,      \
                                 evidence_confidence, evidence_provenance)                         \
  inline constexpr CandidateSignalDefinition symbol {                                              \
    label, identifier_value, bit, 1, 1.0F, 0.0F, vehicle_core::SignalUnit::Boolean, 0.0F, 1.0F,    \
        "none", dbc_bit, CandidateSignalDefinition::ByteOrder::order, table, evidence_provenance,  \
        evidence_confidence                                                                        \
  }

MAZDA_BOOLEAN_DEFINITION(kLiftgateOpenDefinition, "Liftgate_Open", kDoorsId, 32, 32, Motorola,
                         "0=Closed; 1=Open", vehicle_core::ValidationStatus::Reference,
                         kSourceOnlyEvidenceProvenance);
MAZDA_BOOLEAN_DEFINITION(kRearRightDoorOpenDefinition, "RearRightDoor_Open", kDoorsId, 34, 34,
                         Motorola, "0=Closed; 1=Open", vehicle_core::ValidationStatus::Reference,
                         kSourceOnlyEvidenceProvenance);
MAZDA_BOOLEAN_DEFINITION(kRearLeftDoorOpenDefinition, "RearLeftDoor_Open", kDoorsId, 35, 35,
                         Motorola, "0=Closed; 1=Open", vehicle_core::ValidationStatus::Reference,
                         kSourceOnlyEvidenceProvenance);
MAZDA_BOOLEAN_DEFINITION(kFrontLeftDoorOpenRhdDefinition, "FrontLeftDoor_Open_RHD", kDoorsId, 36,
                         36, Motorola, "0=Closed; 1=Open",
                         vehicle_core::ValidationStatus::Reference,
                         kFrontOtherDoorEvidenceProvenance);
inline constexpr const CandidateSignalDefinition &kFrontLeftDoorOpenDefinition =
    kFrontLeftDoorOpenRhdDefinition;
MAZDA_BOOLEAN_DEFINITION(kFrontRightDoorOpenRhdDefinition, "FrontRightDoor_Open_RHD", kDoorsId, 37,
                         37, Motorola, "0=Closed; 1=Open",
                         vehicle_core::ValidationStatus::Confirmed,
                         kFrontRightDoorEvidenceProvenance);
MAZDA_BOOLEAN_DEFINITION(kDoorsUnlockedDefinition, "DoorsUnlocked", kDoorsId, 30, 30, Motorola,
                         "0=Locked; 1=Unlocked", vehicle_core::ValidationStatus::Reference,
                         kSourceOnlyEvidenceProvenance);
MAZDA_BOOLEAN_DEFINITION(kLeftIndicatorLampDefinition, "LeftIndicatorLamp", kBlinkInfoId, 18, 18,
                         Intel, "0=Off; 1=On", vehicle_core::ValidationStatus::Reference,
                         kSourceOnlyEvidenceProvenance);
MAZDA_BOOLEAN_DEFINITION(kRightIndicatorLampDefinition, "RightIndicatorLamp", kBlinkInfoId, 19, 19,
                         Motorola, "0=Off; 1=On", vehicle_core::ValidationStatus::Reference,
                         kSourceOnlyEvidenceProvenance);
MAZDA_BOOLEAN_DEFINITION(kWiperLowDefinition, "WiperLow", kBlinkInfoId, 33, 33, Motorola,
                         "0=Off; 1=On", vehicle_core::ValidationStatus::Observed,
                         kWiperLowEvidenceProvenance);
MAZDA_BOOLEAN_DEFINITION(kHazardDefinition, "HazardSwitch", kTurnSwitchId, 10, 10, Motorola,
                         "0=Off; 1=On", vehicle_core::ValidationStatus::Reference,
                         kSourceOnlyEvidenceProvenance);
MAZDA_BOOLEAN_DEFINITION(kTurnRightSwitchDefinition, "RightIndicatorSwitch", kTurnSwitchId, 12, 12,
                         Motorola, "0=Off; 1=On", vehicle_core::ValidationStatus::Reference,
                         kSourceOnlyEvidenceProvenance);
MAZDA_BOOLEAN_DEFINITION(kTurnLeftSwitchDefinition, "LeftIndicatorSwitch", kTurnSwitchId, 13, 13,
                         Motorola, "0=Off; 1=On", vehicle_core::ValidationStatus::Reference,
                         kSourceOnlyEvidenceProvenance);
MAZDA_BOOLEAN_DEFINITION(
    kBrakePressedDefinition, "BrakePressed", kBrakePedalId, 4, 4, Motorola, "0=Released; 1=Pressed",
    vehicle_core::ValidationStatus::Confirmed,
    "docs/development/signal-evidence.md; docs/protocol/mazda_custom.dbc#L112");

#undef MAZDA_BOOLEAN_DEFINITION

inline constexpr CandidateSignalDefinition kFrontWiperDefinition{
    "FrontWiper",
    kTurnSwitchId,
    20,
    2,
    1.0F,
    0.0F,
    vehicle_core::SignalUnit::None,
    0.0F,
    3.0F,
    "none",
    21,
    CandidateSignalDefinition::ByteOrder::Motorola,
    "0=Off; 1=On; 2=High; 3=Intermittent",
    kFrontWiperEvidenceProvenance,
    vehicle_core::ValidationStatus::Observed};

// This is the supported subset consumed by the host DBC comparison tool. It
// intentionally excludes kEngineSpeedDefinition: SPEED is a retained legacy
// candidate and is not present in the reviewed source DBC. The FrontOtherDoor
// -> FrontLeftDoorOpenRhd entry is the sole interpretation/name exception
// documented by S1-E.
struct SupportedSignalDefinition {
  const char *message_name;
  const char *dbc_signal_name;
  const char *channel_name;
  const CandidateSignalDefinition *definition;
};

inline constexpr std::array kSupportedSignalDefinitions{
    SupportedSignalDefinition{"ENGINE_DATA", "EngineRPM", "engine_rpm", &kEngineRpmDefinition},
    SupportedSignalDefinition{"TRANSMISSION", "Selector", "selector_position",
                              &kSelectorDefinition},
    SupportedSignalDefinition{"TRANSMISSION", "ActualGear", "actual_gear", &kActualGearDefinition},
    SupportedSignalDefinition{"DOORS", "Liftgate_Open_Reference", "liftgate_open",
                              &kLiftgateOpenDefinition},
    SupportedSignalDefinition{"DOORS", "RearRightDoor_Open_Reference", "rear_right_door_open",
                              &kRearRightDoorOpenDefinition},
    SupportedSignalDefinition{"DOORS", "RearLeftDoor_Open_Reference", "rear_left_door_open",
                              &kRearLeftDoorOpenDefinition},
    SupportedSignalDefinition{"DOORS", "FrontOtherDoor_Open_Reference", "front_left_door_open_rhd",
                              &kFrontLeftDoorOpenRhdDefinition},
    SupportedSignalDefinition{"DOORS", "FrontRightDoor_Open_RHD", "front_right_door_open_rhd",
                              &kFrontRightDoorOpenRhdDefinition},
    SupportedSignalDefinition{"DOORS", "DoorsUnlocked_Reference", "doors_unlocked",
                              &kDoorsUnlockedDefinition},
    SupportedSignalDefinition{"BLINK_INFO", "LeftIndicatorLamp_Reference", "left_indicator_lamp",
                              &kLeftIndicatorLampDefinition},
    SupportedSignalDefinition{"BLINK_INFO", "RightIndicatorLamp_Reference", "right_indicator_lamp",
                              &kRightIndicatorLampDefinition},
    SupportedSignalDefinition{"BLINK_INFO", "WiperLow_Reference", "wiper_low",
                              &kWiperLowDefinition},
    SupportedSignalDefinition{"TURN_SWITCH", "HazardSwitch_Reference", "hazard_request",
                              &kHazardDefinition},
    SupportedSignalDefinition{"TURN_SWITCH", "RightIndicatorSwitch_Reference", "right_turn_request",
                              &kTurnRightSwitchDefinition},
    SupportedSignalDefinition{"TURN_SWITCH", "LeftIndicatorSwitch_Reference", "left_turn_request",
                              &kTurnLeftSwitchDefinition},
    SupportedSignalDefinition{"TURN_SWITCH", "FrontWiper", "front_wiper", &kFrontWiperDefinition},
    SupportedSignalDefinition{"BRAKE_PEDAL", "BrakePressed", "brake_pressed",
                              &kBrakePressedDefinition},
};

} // namespace mazda::candidate
