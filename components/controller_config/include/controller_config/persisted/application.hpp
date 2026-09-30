#pragma once

#include <cstddef>
#include <cstdint>

#include "action_engine/config_status.hpp"
#include "action_engine/engine.hpp"
#include "controller_config/persisted/model.hpp"
#include "controller_config/persisted/validation.hpp"
#include "local_argb_actions/effect_bindings.hpp"
#include "local_argb_actions/led_action_sink.hpp"

namespace controller_config::persisted {

enum class ApplyStage : std::uint8_t {
  Complete,
  Validation,
  ActionId,
  OutputBinding,
  SinkRegistration,
  Rule,
};

struct ApplyStatus final {
  ApplyStage stage{ApplyStage::Complete};
  ValidationResult validation{};
  std::size_t index{0};
  std::size_t outputs_applied{0};
  std::size_t rules_applied{0};
  bool sink_registered{false};
  local_argb_actions::BindingStatus binding{local_argb_actions::BindingStatus::Ok};
  action_engine::ConfigStatus engine{action_engine::ConfigStatus::Ok};

  [[nodiscard]] bool ok() const noexcept { return stage == ApplyStage::Complete; }
};

// Applies an already validated, owning persisted configuration while the
// engine is detached. Action IDs are assigned from document order and remain
// an implementation detail; all string views passed to the engine refer to
// `config`, which must outlive the engine's attachment.
[[nodiscard]] ApplyStatus apply_controller_config(const ControllerConfig &config,
                                                  local_argb_actions::LedActionSink &leds,
                                                  action_engine::ActionEngine &engine) noexcept;

} // namespace controller_config::persisted
