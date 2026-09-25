#include "action_engine/range_rule_set.hpp"

namespace action_engine {

ConfigStatus RangeRuleSet::add(const RangeRule &rule) noexcept {
  if (count_ == kCapacity) {
    return ConfigStatus::CapacityExceeded;
  }
  rules_[count_++] = rule;
  return ConfigStatus::Ok;
}

bool RangeRuleSet::drives(ActionId action) const noexcept {
  for (std::size_t index = 0; index < count_; ++index) {
    if (rules_[index].action() == action) {
      return true;
    }
  }
  return false;
}

void RangeRuleSet::reset() noexcept {
  for (std::size_t index = 0; index < count_; ++index) {
    rules_[index].reset();
  }
}

void RangeRuleSet::on_sample(
    std::size_t index, const vehicle_signals::SignalResult<vehicle_signals::SignalReading> &read,
    ActionSink &out) noexcept {
  const auto command = rules_[index].on_sample(read);
  if (command.has_value()) {
    out.execute(*command);
  }
}

} // namespace action_engine
