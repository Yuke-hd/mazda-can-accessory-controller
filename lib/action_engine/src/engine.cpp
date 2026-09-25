#include "action_engine/engine.hpp"

namespace action_engine {

using vehicle_signals::SignalNotification;
using vehicle_signals::SignalStatus;

ActionEngine::ActionEngine(vehicle_signals::SignalProvider &provider) noexcept
    : provider_(&provider) {}

ConfigStatus ActionEngine::add_sink(ActionSink &sink) noexcept {
  if (attached_) {
    return ConfigStatus::InvalidState;
  }
  return sinks_.add(sink);
}

ConfigStatus ActionEngine::add_state_rule(const StateRuleConfig &config) noexcept {
  const auto resolution =
      resolve_rule(config.condition, config.action, config.freshness, RuleOutput::Level);
  if (!resolution.condition.has_value()) {
    return resolution.status;
  }
  return rules_.add(StateRule{*resolution.condition, config.action});
}

ConfigStatus ActionEngine::add_event_rule(const EventRuleConfig &config) noexcept {
  const auto resolution =
      resolve_rule(config.condition, config.action, config.freshness, RuleOutput::OneShot);
  if (!resolution.condition.has_value()) {
    return resolution.status;
  }
  return rules_.add(EventRule{*resolution.condition, config.edge, config.action});
}

ConfigStatus ActionEngine::check_action(ActionId action, RuleOutput output) const noexcept {
  if (attached_) {
    return ConfigStatus::InvalidState;
  }
  if (!action.valid()) {
    return ConfigStatus::InvalidAction;
  }
  if (output == RuleOutput::Level && rules_.drives_level(action)) {
    return ConfigStatus::DuplicateAction;
  }
  return ConfigStatus::Ok;
}

ConditionResolution ActionEngine::resolve_rule(const SignalCondition &condition, ActionId action,
                                               FreshnessRequirement freshness,
                                               RuleOutput output) const noexcept {
  const auto status = check_action(action, output);
  if (status != ConfigStatus::Ok) {
    return ConditionResolution{status, std::nullopt};
  }
  return resolve_condition(provider_->catalog(), condition, freshness);
}

SignalStatus ActionEngine::attach() noexcept {
  if (attached_) {
    return SignalStatus::InvalidState;
  }
  rules_.reset();
  for (std::size_t index = 0; index < rules_.size(); ++index) {
    const auto status =
        subscriptions_.subscribe(*provider_, rules_.signal(index), &on_notice, this);
    if (status != SignalStatus::Ok) {
      // A failed rollback keeps the engine attached so detach() can retry.
      attached_ = subscriptions_.unsubscribe_all(*provider_) != SignalStatus::Ok;
      return status;
    }
  }
  attached_ = true;
  return SignalStatus::Ok;
}

SignalStatus ActionEngine::detach() noexcept {
  if (!attached_) {
    return SignalStatus::InvalidState;
  }
  const auto status = subscriptions_.unsubscribe_all(*provider_);
  attached_ = status != SignalStatus::Ok;
  return status;
}

void ActionEngine::on_notice(void *context, const SignalNotification &notice) noexcept {
  auto &engine = *static_cast<ActionEngine *>(context);
  engine.rules_.dispatch(notice, engine.sinks_);
}

} // namespace action_engine
