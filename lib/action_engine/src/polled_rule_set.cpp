#include "action_engine/polled_rule_set.hpp"

namespace action_engine {

ConfigStatus PolledRuleSet::add(const PolledRule &rule) noexcept {
  if (count_ == kCapacity) {
    return ConfigStatus::CapacityExceeded;
  }
  rules_[count_++] = rule;
  return ConfigStatus::Ok;
}

vehicle_signals::SignalId PolledRuleSet::signal(std::size_t index) const noexcept {
  return std::visit([](const auto &rule) noexcept { return rule.signal(); }, rules_[index]);
}

bool PolledRuleSet::drives(ActionId action) const noexcept {
  for (std::size_t index = 0; index < count_; ++index) {
    if (std::visit([](const auto &rule) noexcept { return rule.action(); }, rules_[index]) ==
        action) {
      return true;
    }
  }
  return false;
}

void PolledRuleSet::reset() noexcept {
  for (std::size_t index = 0; index < count_; ++index) {
    std::visit([](auto &rule) noexcept { rule.reset(); }, rules_[index]);
  }
}

void PolledRuleSet::on_sample(
    std::size_t index, const vehicle_signals::SignalResult<vehicle_signals::SignalReading> &read,
    ActionSink &out) noexcept {
  const auto command =
      std::visit([&read](auto &rule) noexcept { return rule.on_sample(read); }, rules_[index]);
  if (command.has_value()) {
    out.execute(*command);
  }
}

} // namespace action_engine
