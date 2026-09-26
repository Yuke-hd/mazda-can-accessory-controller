#include "action_engine/engine.hpp"

#include <cmath>

namespace action_engine {

using vehicle_signals::SignalNotification;
using vehicle_signals::SignalStatus;

namespace {

[[nodiscard]] bool has_valid_release_threshold(const SignalCondition &condition,
                                               float release_threshold) noexcept {
  const auto activation_threshold = condition.operand.as_number();
  if (!activation_threshold.has_value() || !std::isfinite(*activation_threshold) ||
      !std::isfinite(release_threshold)) {
    return false;
  }

  switch (condition.comparison) {
  case Comparison::Greater:
  case Comparison::GreaterOrEqual:
    return release_threshold < *activation_threshold;
  case Comparison::Less:
  case Comparison::LessOrEqual:
    return release_threshold > *activation_threshold;
  case Comparison::Equal:
  case Comparison::NotEqual:
    return false;
  }
  return false;
}

} // namespace

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

ConfigStatus ActionEngine::add_range_rule(const RangeRuleConfig &config) noexcept {
  const auto status = check_action(config.action, RuleOutput::Level);
  if (status != ConfigStatus::Ok) {
    return status;
  }
  const auto resolution = resolve_range_rule(provider_->catalog(), config);
  if (!resolution.rule.has_value()) {
    return resolution.status;
  }
  return polled_rules_.add(*resolution.rule);
}

ConfigStatus ActionEngine::add_sampled_state_rule(const SampledStateRuleConfig &config) noexcept {
  const auto status = check_action(config.action, RuleOutput::Level);
  if (status != ConfigStatus::Ok) {
    return status;
  }
  const auto resolution =
      resolve_sampled_condition(provider_->catalog(), config.condition, config.freshness);
  if (!resolution.condition.has_value()) {
    return resolution.status;
  }

  if (config.release_threshold.has_value()) {
    if (!has_valid_release_threshold(config.condition, *config.release_threshold)) {
      return ConfigStatus::InvalidHysteresis;
    }
    SignalCondition release_condition = config.condition;
    release_condition.operand = RuleOperand::number(*config.release_threshold);
    const auto release_resolution =
        resolve_sampled_condition(provider_->catalog(), release_condition, config.freshness);
    if (!release_resolution.condition.has_value()) {
      return release_resolution.status;
    }
    return polled_rules_.add(
        SampledStateRule{*resolution.condition, config.action, release_resolution.condition});
  }
  return polled_rules_.add(SampledStateRule{*resolution.condition, config.action});
}

ConfigStatus ActionEngine::check_action(ActionId action, RuleOutput output) const noexcept {
  if (attached_) {
    return ConfigStatus::InvalidState;
  }
  if (!action.valid()) {
    return ConfigStatus::InvalidAction;
  }
  if (output == RuleOutput::Level &&
      (rules_.drives_level(action) || polled_rules_.drives(action))) {
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
  reset_rules();
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

SignalStatus ActionEngine::sample_polled_rules() noexcept {
  if (!attached_) {
    return SignalStatus::InvalidState;
  }
  for (std::size_t index = 0; index < polled_rules_.size(); ++index) {
    // Read outside the lock: a provider read must never wait on a sink.
    const auto read = provider_->read(polled_rules_.signal(index));
    const std::lock_guard<std::mutex> lock{evaluation_};
    polled_rules_.on_sample(index, read, sinks_);
  }
  return SignalStatus::Ok;
}

void ActionEngine::reset_rules() noexcept {
  const std::lock_guard<std::mutex> lock{evaluation_};
  rules_.reset();
  polled_rules_.reset();
}

void ActionEngine::on_notice(void *context, const SignalNotification &notice) noexcept {
  auto &engine = *static_cast<ActionEngine *>(context);
  const std::lock_guard<std::mutex> lock{engine.evaluation_};
  engine.rules_.dispatch(notice, engine.sinks_);
}

} // namespace action_engine
