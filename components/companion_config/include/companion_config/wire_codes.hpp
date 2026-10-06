#pragma once

// Maps controller configuration diagnostics to the companion protocol's
// stable wire codes (config-transfer.md, "Wire codes"). Every mapping is a
// switch over the controller enum with no default, and this component builds
// with -Werror=switch: a new controller enumerator fails the build until it
// is given a wire code here, so a code can never be sent unmapped.

#include "action_engine/config_status.hpp"
#include "companion_protocol/config_status.hpp"
#include "controller_config/persisted/application.hpp"
#include "controller_config/persisted/json_loader.hpp"
#include "controller_config/persisted/validation.hpp"
#include "local_argb_actions/effect_bindings.hpp"

namespace companion_config {

[[nodiscard]] companion_protocol::DiagnosticCategory
to_wire(controller_config::persisted::ConfigErrorCategory category) noexcept;
[[nodiscard]] companion_protocol::DiagnosticCode
to_wire(controller_config::persisted::ConfigErrorCode code) noexcept;
[[nodiscard]] companion_protocol::ValidationCode
to_wire(controller_config::persisted::ValidationError error) noexcept;
[[nodiscard]] companion_protocol::ConfigSection
to_wire(controller_config::persisted::ConfigSection section) noexcept;
[[nodiscard]] companion_protocol::ApplyStage
to_wire(controller_config::persisted::ApplyStage stage) noexcept;
[[nodiscard]] companion_protocol::BindingCode
to_wire(local_argb_actions::BindingStatus status) noexcept;
[[nodiscard]] companion_protocol::EngineConfigCode
to_wire(action_engine::ConfigStatus status) noexcept;

// The Config status diagnostic fields of a rejected document. The free-text
// message is not sent.
[[nodiscard]] companion_protocol::ConfigRejection
config_rejection(const controller_config::persisted::ConfigDiagnostic &diagnostic) noexcept;

// The Config status fields of a failed dry-run apply.
[[nodiscard]] companion_protocol::ApplyRejection
apply_rejection(const controller_config::persisted::ApplyStatus &status) noexcept;

} // namespace companion_config
