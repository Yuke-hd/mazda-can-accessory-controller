#include "companion_config/wire_codes.hpp"

// Each table is a switch with no default; the component builds with
// -Werror=switch, so an unmapped controller enumerator fails the build. The
// return after each switch is reached only for a value outside the enum.

namespace companion_config {
namespace {
namespace wire = companion_protocol;
namespace persisted = controller_config::persisted;
} // namespace

wire::DiagnosticCategory to_wire(const persisted::ConfigErrorCategory value) noexcept {
  switch (value) {
  case persisted::ConfigErrorCategory::Parse:
    return wire::DiagnosticCategory::Parse;
  case persisted::ConfigErrorCategory::Structural:
    return wire::DiagnosticCategory::Structural;
  case persisted::ConfigErrorCategory::Semantic:
    return wire::DiagnosticCategory::Semantic;
  }
  return wire::DiagnosticCategory::None;
}

wire::DiagnosticCode to_wire(const persisted::ConfigErrorCode value) noexcept {
  switch (value) {
  case persisted::ConfigErrorCode::MalformedJson:
    return wire::DiagnosticCode::MalformedJson;
  case persisted::ConfigErrorCode::EmbeddedNul:
    return wire::DiagnosticCode::EmbeddedNul;
  case persisted::ConfigErrorCode::InputTooLarge:
    return wire::DiagnosticCode::InputTooLarge;
  case persisted::ConfigErrorCode::NestingLimitExceeded:
    return wire::DiagnosticCode::NestingLimitExceeded;
  case persisted::ConfigErrorCode::RootTypeMismatch:
    return wire::DiagnosticCode::RootTypeMismatch;
  case persisted::ConfigErrorCode::MissingField:
    return wire::DiagnosticCode::MissingField;
  case persisted::ConfigErrorCode::UnknownField:
    return wire::DiagnosticCode::UnknownField;
  case persisted::ConfigErrorCode::TypeMismatch:
    return wire::DiagnosticCode::TypeMismatch;
  case persisted::ConfigErrorCode::InvalidValue:
    return wire::DiagnosticCode::InvalidValue;
  case persisted::ConfigErrorCode::SchemaValidation:
    return wire::DiagnosticCode::SchemaValidation;
  case persisted::ConfigErrorCode::ResourceExhausted:
    return wire::DiagnosticCode::ResourceExhausted;
  }
  return wire::DiagnosticCode::None;
}

wire::ValidationCode to_wire(const persisted::ValidationError value) noexcept {
  switch (value) {
  case persisted::ValidationError::None:
    return wire::ValidationCode::None;
  case persisted::ValidationError::UnsupportedVersion:
    return wire::ValidationCode::UnsupportedVersion;
  case persisted::ValidationError::EmptyActionName:
    return wire::ValidationCode::EmptyActionName;
  case persisted::ValidationError::DuplicateActionName:
    return wire::ValidationCode::DuplicateActionName;
  case persisted::ValidationError::UndeclaredAction:
    return wire::ValidationCode::UndeclaredAction;
  case persisted::ValidationError::DuplicateAction:
    return wire::ValidationCode::DuplicateAction;
  case persisted::ValidationError::EmptySignalKey:
    return wire::ValidationCode::EmptySignalKey;
  case persisted::ValidationError::UnknownComparison:
    return wire::ValidationCode::UnknownComparison;
  case persisted::ValidationError::UnknownFreshness:
    return wire::ValidationCode::UnknownFreshness;
  case persisted::ValidationError::UnknownEventEdge:
    return wire::ValidationCode::UnknownEventEdge;
  case persisted::ValidationError::EmptyChoice:
    return wire::ValidationCode::EmptyChoice;
  case persisted::ValidationError::InvalidOperand:
    return wire::ValidationCode::InvalidOperand;
  case persisted::ValidationError::UnsupportedComparison:
    return wire::ValidationCode::UnsupportedComparison;
  case persisted::ValidationError::InvalidHysteresis:
    return wire::ValidationCode::InvalidHysteresis;
  case persisted::ValidationError::InvalidRange:
    return wire::ValidationCode::InvalidRange;
  case persisted::ValidationError::UnknownLedEffect:
    return wire::ValidationCode::UnknownLedEffect;
  case persisted::ValidationError::UnknownFillDirection:
    return wire::ValidationCode::UnknownFillDirection;
  case persisted::ValidationError::EmptyZone:
    return wire::ValidationCode::EmptyZone;
  case persisted::ValidationError::ZoneOutOfRange:
    return wire::ValidationCode::ZoneOutOfRange;
  case persisted::ValidationError::InvalidColor:
    return wire::ValidationCode::InvalidColor;
  case persisted::ValidationError::InvalidPriority:
    return wire::ValidationCode::InvalidPriority;
  case persisted::ValidationError::DuplicateBinding:
    return wire::ValidationCode::DuplicateBinding;
  case persisted::ValidationError::InvalidDuration:
    return wire::ValidationCode::InvalidDuration;
  case persisted::ValidationError::IncompatibleActionKind:
    return wire::ValidationCode::IncompatibleActionKind;
  }
  return wire::ValidationCode::None;
}

wire::ConfigSection to_wire(const persisted::ConfigSection value) noexcept {
  switch (value) {
  case persisted::ConfigSection::Document:
    return wire::ConfigSection::Document;
  case persisted::ConfigSection::Actions:
    return wire::ConfigSection::Actions;
  case persisted::ConfigSection::Rules:
    return wire::ConfigSection::Rules;
  case persisted::ConfigSection::Outputs:
    return wire::ConfigSection::Outputs;
  }
  return wire::ConfigSection::Document;
}

wire::ApplyStage to_wire(const persisted::ApplyStage value) noexcept {
  switch (value) {
  case persisted::ApplyStage::Complete:
    return wire::ApplyStage::Complete;
  case persisted::ApplyStage::Validation:
    return wire::ApplyStage::Validation;
  case persisted::ApplyStage::ActionId:
    return wire::ApplyStage::ActionId;
  case persisted::ApplyStage::OutputBinding:
    return wire::ApplyStage::OutputBinding;
  case persisted::ApplyStage::SinkRegistration:
    return wire::ApplyStage::SinkRegistration;
  case persisted::ApplyStage::Rule:
    return wire::ApplyStage::Rule;
  }
  return wire::ApplyStage::Complete;
}

wire::BindingCode to_wire(const local_argb_actions::BindingStatus value) noexcept {
  switch (value) {
  case local_argb_actions::BindingStatus::Ok:
    return wire::BindingCode::Ok;
  case local_argb_actions::BindingStatus::InvalidAction:
    return wire::BindingCode::InvalidAction;
  case local_argb_actions::BindingStatus::DuplicateBinding:
    return wire::BindingCode::DuplicateBinding;
  case local_argb_actions::BindingStatus::CapacityExceeded:
    return wire::BindingCode::CapacityExceeded;
  case local_argb_actions::BindingStatus::InvalidEffect:
    return wire::BindingCode::InvalidEffect;
  }
  return wire::BindingCode::Ok;
}

wire::EngineConfigCode to_wire(const action_engine::ConfigStatus value) noexcept {
  switch (value) {
  case action_engine::ConfigStatus::Ok:
    return wire::EngineConfigCode::Ok;
  case action_engine::ConfigStatus::InvalidState:
    return wire::EngineConfigCode::InvalidState;
  case action_engine::ConfigStatus::CapacityExceeded:
    return wire::EngineConfigCode::CapacityExceeded;
  case action_engine::ConfigStatus::DuplicateSink:
    return wire::EngineConfigCode::DuplicateSink;
  case action_engine::ConfigStatus::InvalidAction:
    return wire::EngineConfigCode::InvalidAction;
  case action_engine::ConfigStatus::DuplicateAction:
    return wire::EngineConfigCode::DuplicateAction;
  case action_engine::ConfigStatus::UnknownSignal:
    return wire::EngineConfigCode::UnknownSignal;
  case action_engine::ConfigStatus::UnsupportedCapability:
    return wire::EngineConfigCode::UnsupportedCapability;
  case action_engine::ConfigStatus::TypeMismatch:
    return wire::EngineConfigCode::TypeMismatch;
  case action_engine::ConfigStatus::UnknownChoice:
    return wire::EngineConfigCode::UnknownChoice;
  case action_engine::ConfigStatus::InvalidOperand:
    return wire::EngineConfigCode::InvalidOperand;
  case action_engine::ConfigStatus::UnsupportedComparison:
    return wire::EngineConfigCode::UnsupportedComparison;
  case action_engine::ConfigStatus::InvalidRange:
    return wire::EngineConfigCode::InvalidRange;
  case action_engine::ConfigStatus::InvalidHysteresis:
    return wire::EngineConfigCode::InvalidHysteresis;
  }
  return wire::EngineConfigCode::Ok;
}

wire::ConfigRejection config_rejection(const persisted::ConfigDiagnostic &diagnostic) noexcept {
  wire::ConfigRejection rejection{};
  rejection.category = to_wire(diagnostic.category);
  rejection.code = to_wire(diagnostic.code);
  rejection.validation = to_wire(diagnostic.schema_error);
  rejection.index = diagnostic.index;
  rejection.path = wire::DiagnosticPath{diagnostic.path};
  return rejection;
}

wire::ApplyRejection apply_rejection(const persisted::ApplyStatus &status) noexcept {
  wire::ApplyRejection rejection{};
  rejection.stage = to_wire(status.stage);
  rejection.validation = to_wire(status.validation.error);
  rejection.validation_section = to_wire(status.validation.section);
  rejection.index = status.index;
  rejection.binding = to_wire(status.binding);
  rejection.engine = to_wire(status.engine);
  return rejection;
}

} // namespace companion_config
