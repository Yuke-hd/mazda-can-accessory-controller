#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "companion_protocol/active_document.hpp"
#include "companion_protocol/bytes.hpp"

// Config status value and its wire-code tables. Every enumerator carries its
// explicit protocol code, so declaration order never reaches the wire. This
// library does not know the loader, validator, action engine or LED adapter
// types; the integration maps each of their enumerators to these codes
// through an explicit table of its own.

namespace companion_protocol {

enum class TransferState : std::uint8_t {
  Idle = 0,
  Receiving = 1,
  RestartPending = 2,
};

enum class TransferResult : std::uint8_t {
  None = 0,
  Saved = 1,
  ConfigRejected = 2,
  ApplyRejected = 3,
  StorageFailed = 4,
  Aborted = 5,
  TimedOut = 6,
  ChecksumMismatch = 7,
  Interrupted = 8,
};

// `diag_category`: ConfigErrorCategory.
enum class DiagnosticCategory : std::uint8_t {
  None = 0,
  Parse = 1,
  Structural = 2,
  Semantic = 3,
};

// `diag_code` and `boot_diag_code`: ConfigErrorCode.
enum class DiagnosticCode : std::uint8_t {
  None = 0,
  MalformedJson = 1,
  EmbeddedNul = 2,
  InputTooLarge = 3,
  NestingLimitExceeded = 4,
  RootTypeMismatch = 5,
  MissingField = 6,
  UnknownField = 7,
  TypeMismatch = 8,
  InvalidValue = 9,
  SchemaValidation = 10,
  ResourceExhausted = 11,
};

// `diag_validation`, `apply_validation` and `boot_diag_validation`:
// ValidationError.
enum class ValidationCode : std::uint8_t {
  None = 0,
  UnsupportedVersion = 1,
  EmptyActionName = 2,
  DuplicateActionName = 3,
  UndeclaredAction = 4,
  DuplicateAction = 5,
  EmptySignalKey = 6,
  UnknownComparison = 7,
  UnknownFreshness = 8,
  UnknownEventEdge = 9,
  EmptyChoice = 10,
  InvalidOperand = 11,
  UnsupportedComparison = 12,
  InvalidHysteresis = 13,
  InvalidRange = 14,
  UnknownLedEffect = 15,
  UnknownFillDirection = 16,
  EmptyZone = 17,
  ZoneOutOfRange = 18,
  InvalidColor = 19,
  InvalidPriority = 20,
  DuplicateBinding = 21,
  InvalidDuration = 22,
  IncompatibleActionKind = 23,
};

// `apply_section`: ConfigSection.
enum class ConfigSection : std::uint8_t {
  Document = 0,
  Actions = 1,
  Rules = 2,
  Outputs = 3,
};

// `apply_stage`: ApplyStage. Complete means no failure.
enum class ApplyStage : std::uint8_t {
  Complete = 0,
  Validation = 1,
  ActionId = 2,
  OutputBinding = 3,
  SinkRegistration = 4,
  Rule = 5,
};

// `apply_binding`: the LED adapter's BindingStatus.
enum class BindingCode : std::uint8_t {
  Ok = 0,
  InvalidAction = 1,
  DuplicateBinding = 2,
  CapacityExceeded = 3,
  InvalidEffect = 4,
};

// `apply_engine`: the action engine's ConfigStatus.
enum class EngineConfigCode : std::uint8_t {
  Ok = 0,
  InvalidState = 1,
  CapacityExceeded = 2,
  DuplicateSink = 3,
  InvalidAction = 4,
  DuplicateAction = 5,
  UnknownSignal = 6,
  UnsupportedCapability = 7,
  TypeMismatch = 8,
  UnknownChoice = 9,
  InvalidOperand = 10,
  UnsupportedComparison = 11,
  InvalidRange = 12,
  InvalidHysteresis = 13,
};

inline constexpr std::size_t kMaxDiagnosticPathBytes = 26;
inline constexpr std::size_t kConfigStatusFixedBytes = 35;
inline constexpr std::size_t kMaxConfigStatusBytes =
    kConfigStatusFixedBytes + kMaxDiagnosticPathBytes;

// A loader diagnostic path such as `outputs[3].zone.length`, kept to its
// first 26 bytes. A longer path is cut at a UTF-8 character boundary and
// marked truncated, so the stored bytes never end inside a character.
class DiagnosticPath final {
public:
  constexpr DiagnosticPath() noexcept = default;
  explicit DiagnosticPath(std::string_view path) noexcept;

  [[nodiscard]] std::string_view text() const noexcept {
    return std::string_view{bytes_.data(), length_};
  }
  [[nodiscard]] constexpr bool truncated() const noexcept { return truncated_; }

private:
  std::array<char, kMaxDiagnosticPathBytes> bytes_{};
  std::size_t length_{0};
  bool truncated_{false};
};

// Diagnostic of a ConfigRejected commit: the parse/validate or
// save_override() ConfigDiagnostic. The free-text message is never sent.
struct ConfigRejection {
  DiagnosticCategory category{DiagnosticCategory::None};
  DiagnosticCode code{DiagnosticCode::None};
  ValidationCode validation{ValidationCode::None};
  std::size_t index{0};
  DiagnosticPath path{};
};

// Dry-run ApplyStatus of an ApplyRejected commit. `validation_section` is
// ValidationResult::section, which the encoder uses only for stage Validation.
struct ApplyRejection {
  ApplyStage stage{ApplyStage::Complete};
  ValidationCode validation{ValidationCode::None};
  ConfigSection validation_section{ConfigSection::Document};
  std::size_t index{0};
  BindingCode binding{BindingCode::Ok};
  EngineConfigCode engine{EngineConfigCode::Ok};

  // `apply_section`, derived from the stage as the specification requires.
  [[nodiscard]] ConfigSection section() const noexcept;
};

// Canonical length and CRC-32 of a saved override.
struct SavedDocument {
  std::uint16_t length{0};
  std::uint32_t crc32{0};
};

struct BootFlags {
  bool invalid_override{false};      // Bit 0.
  bool override_read_failed{false};  // Bit 1.
  bool no_config_store{false};       // Bit 2.
  bool lighting_setup_failed{false}; // Bit 3.

  [[nodiscard]] std::uint8_t bits() const noexcept;
};

// What this boot selected. Fixed until the next restart.
struct BootStatus {
  BootFlags flags{};
  // `active_source`, `active_length` and `active_crc32` are read from the
  // same document the read-back pages serve.
  ActiveDocument active{};
  // BootConfigurationResult::override_diagnostic, sent only with boot flag 0.
  DiagnosticCode override_code{DiagnosticCode::None};
  ValidationCode override_validation{ValidationCode::None};
};

// Transfer state and the last finished transfer's result. The detail fields
// are meaningful only for their own result; the encoder zeroes the others.
struct TransferStatus {
  TransferState state{TransferState::Idle};
  TransferResult result{TransferResult::None};
  std::uint16_t transfer_length{0};
  std::uint16_t received_length{0};
  SavedDocument saved{};
  ConfigRejection rejection{};
  ApplyRejection apply{};
};

using EncodedConfigStatus = BoundedBytes<kMaxConfigStatusBytes>;

// Encodes the Config status value (35 bytes plus the path, at most 61).
// Fields that do not apply to the current state, result or boot flags are
// zero, whatever the inputs hold. Indexes saturate at 0xFFFF.
[[nodiscard]] EncodedConfigStatus encode_config_status(const BootStatus &boot,
                                                       const TransferStatus &transfer) noexcept;

} // namespace companion_protocol
