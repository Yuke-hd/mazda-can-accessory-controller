#include "action_engine/range_rule.hpp"

#include <cmath>

#include "action_engine/actionability.hpp"

namespace action_engine {

using vehicle_signals::SignalReading;
using vehicle_signals::SignalResult;
using vehicle_signals::SignalType;

namespace {

[[nodiscard]] bool finite_range(NumericRange range) noexcept {
  return std::isfinite(range.from) && std::isfinite(range.to) &&
         std::isfinite(range.to - range.from);
}

[[nodiscard]] bool valid_ranges(const RangeRuleConfig &config) noexcept {
  return finite_range(config.input) && finite_range(config.output) &&
         config.input.from < config.input.to;
}

[[nodiscard]] RangeRuleResolution failure(ConfigStatus status) noexcept {
  return RangeRuleResolution{status, std::nullopt};
}

} // namespace

std::optional<ActionCommand>
RangeRule::on_sample(const SignalResult<SignalReading> &read) noexcept {
  const ActionCommand command = command_for(read);
  if (last_ == command) {
    return std::nullopt;
  }
  last_ = command;
  return command;
}

ActionCommand RangeRule::command_for(const SignalResult<SignalReading> &read) const noexcept {
  const auto value =
      read.ok() ? actionable_value(*read.value, SignalType::Number, freshness_) : std::nullopt;
  if (!value.has_value()) {
    return ActionCommand{action_, ActionCommandKind::Deactivate};
  }
  return ActionCommand{action_, ActionCommandKind::SetLevel, mapping_.map(*value->as_number())};
}

RangeRuleResolution resolve_range_rule(vehicle_signals::SignalCatalogView catalog,
                                       const RangeRuleConfig &config) noexcept {
  const vehicle_signals::SignalMetadata *signal = catalog.find(config.signal_key);
  if (signal == nullptr) {
    return failure(ConfigStatus::UnknownSignal);
  }
  if (!signal->capabilities.has(vehicle_signals::SignalCapability::Read)) {
    return failure(ConfigStatus::UnsupportedCapability);
  }
  if (signal->type != SignalType::Number) {
    return failure(ConfigStatus::TypeMismatch);
  }
  if (!valid_ranges(config)) {
    return failure(ConfigStatus::InvalidRange);
  }
  return RangeRuleResolution{ConfigStatus::Ok,
                             RangeRule{signal->id, LinearMapping{config.input, config.output},
                                       config.action, config.freshness}};
}

} // namespace action_engine
