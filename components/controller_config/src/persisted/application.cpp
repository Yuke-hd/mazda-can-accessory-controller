#include "controller_config/persisted/application.hpp"

#include <limits>
#include <optional>
#include <string_view>
#include <type_traits>
#include <variant>

namespace controller_config::persisted {
namespace {

using action_engine::ActionId;
using action_engine::RuleOperand;
using action_engine::SignalCondition;

[[nodiscard]] std::optional<ActionId> action_id(const ControllerConfig &config,
                                                const std::string_view name) noexcept {
  for (std::size_t index = 0; index < config.actions.size(); ++index) {
    if (config.actions[index].name == name)
      return ActionId{static_cast<std::uint16_t>(index + 1U)};
  }
  return std::nullopt;
}

[[nodiscard]] RuleOperand operand(const Operand &value) noexcept {
  return std::visit(
      [](const auto &alternative) -> RuleOperand {
        using Alternative = std::decay_t<decltype(alternative)>;
        if constexpr (std::is_same_v<Alternative, BooleanOperand>)
          return RuleOperand::boolean(alternative.value);
        else if constexpr (std::is_same_v<Alternative, NumberOperand>)
          return RuleOperand::number(alternative.value);
        else
          return RuleOperand::choice(alternative.key);
      },
      value);
}

[[nodiscard]] SignalCondition condition(const Condition &value) noexcept {
  return SignalCondition{value.signal_key, value.comparison, operand(value.operand)};
}

[[nodiscard]] local_argb::internal::LedZone zone(const LedZone &value) noexcept {
  return local_argb::internal::LedZone{static_cast<std::size_t>(value.start),
                                       static_cast<std::size_t>(value.length), value.direction};
}

[[nodiscard]] local_argb::internal::LightingRgb color(const Rgb &value) noexcept {
  return local_argb::internal::LightingRgb{static_cast<std::uint8_t>(value.red),
                                           static_cast<std::uint8_t>(value.green),
                                           static_cast<std::uint8_t>(value.blue)};
}

[[nodiscard]] local_argb::internal::EffectPriority priority(const Integer value) noexcept {
  return local_argb::internal::EffectPriority{static_cast<std::uint8_t>(value)};
}

[[nodiscard]] local_argb_actions::BindingStatus
bind_output(const OutputBinding &output, const ControllerConfig &config,
            local_argb_actions::LedActionSink &leds) noexcept {
  return std::visit(
      [&](const auto &binding) {
        const auto id = action_id(config, binding.action);
        if (!id.has_value())
          return local_argb_actions::BindingStatus::InvalidAction;
        using Binding = std::decay_t<decltype(binding)>;
        if constexpr (std::is_same_v<Binding, LedEffectBinding>)
          return leds.bind(*id, binding.effect, priority(binding.priority));
        else if constexpr (std::is_same_v<Binding, LedFillBinding>)
          return leds.bind(*id,
                           local_argb_actions::FillEffect{zone(binding.zone), color(binding.color),
                                                          priority(binding.priority)});
        else {
          // validate() precedes application: this checked persisted bound makes
          // conversion to unsigned microseconds lossless and non-overflowing.
          static_assert(static_cast<std::uint64_t>(kMaxTransientDurationMs) <=
                        std::numeric_limits<vehicle_core::Microseconds>::max() / 1000U);
          const auto duration =
              static_cast<vehicle_core::Microseconds>(binding.duration_ms) * 1000U;
          return leds.bind(
              *id, local_argb::internal::LightingTransient{zone(binding.zone), color(binding.color),
                                                           priority(binding.priority), duration});
        }
      },
      output);
}

[[nodiscard]] action_engine::ConfigStatus add_rule(const Rule &rule, const ControllerConfig &config,
                                                   action_engine::ActionEngine &engine) noexcept {
  return std::visit(
      [&](const auto &value) {
        const auto id = action_id(config, value.action);
        if (!id.has_value())
          return action_engine::ConfigStatus::InvalidAction;
        using RuleAlternative = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<RuleAlternative, StateRule>) {
          return engine.add_state_rule({condition(value.condition), *id, value.freshness});
        } else if constexpr (std::is_same_v<RuleAlternative, SampledStateRule>) {
          return engine.add_sampled_state_rule(
              {condition(value.condition), *id, value.freshness, value.release_threshold});
        } else if constexpr (std::is_same_v<RuleAlternative, EventRule>) {
          return engine.add_event_rule(
              {condition(value.condition), value.edge, *id, value.freshness});
        } else {
          return engine.add_range_rule(
              {value.signal_key, value.input, value.output, *id, value.freshness});
        }
      },
      rule);
}

} // namespace

ApplyStatus apply_controller_config(const ControllerConfig &config,
                                    local_argb_actions::LedActionSink &leds,
                                    action_engine::ActionEngine &engine) noexcept {
  ApplyStatus status{};
  status.validation = validate(config);
  if (!status.validation.ok()) {
    status.stage = ApplyStage::Validation;
    status.index = status.validation.index;
    return status;
  }
  if (config.actions.size() > std::numeric_limits<std::uint16_t>::max()) {
    status.stage = ApplyStage::ActionId;
    // Zero is reserved, so this is the first action whose 1-based ID cannot fit.
    status.index = std::numeric_limits<std::uint16_t>::max();
    return status;
  }

  for (std::size_t index = 0; index < config.outputs.size(); ++index) {
    status.binding = bind_output(config.outputs[index], config, leds);
    if (status.binding != local_argb_actions::BindingStatus::Ok) {
      status.stage = ApplyStage::OutputBinding;
      status.index = index;
      return status;
    }
    ++status.outputs_applied;
  }

  status.engine = engine.add_sink(leds);
  if (status.engine != action_engine::ConfigStatus::Ok) {
    status.stage = ApplyStage::SinkRegistration;
    return status;
  }
  status.sink_registered = true;

  for (std::size_t index = 0; index < config.rules.size(); ++index) {
    status.engine = add_rule(config.rules[index], config, engine);
    if (status.engine != action_engine::ConfigStatus::Ok) {
      status.stage = ApplyStage::Rule;
      status.index = index;
      return status;
    }
    ++status.rules_applied;
  }
  return status;
}

} // namespace controller_config::persisted
