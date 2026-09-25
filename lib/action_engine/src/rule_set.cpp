#include "action_engine/rule_set.hpp"

namespace action_engine {

ConfigStatus RuleSet::add(const Rule &rule) noexcept {
  if (count_ == kCapacity) {
    return ConfigStatus::CapacityExceeded;
  }
  rules_[count_++] = rule;
  return ConfigStatus::Ok;
}

vehicle_signals::SignalId RuleSet::signal(std::size_t index) const noexcept {
  return std::visit([](const auto &rule) noexcept { return rule.signal(); }, rules_[index]);
}

void RuleSet::reset() noexcept {
  for (std::size_t index = 0; index < count_; ++index) {
    std::visit([](auto &rule) noexcept { rule.reset(); }, rules_[index]);
  }
}

void RuleSet::dispatch(const vehicle_signals::SignalNotification &notice,
                       ActionSink &out) noexcept {
  for (std::size_t index = 0; index < count_; ++index) {
    const auto command = std::visit(
        [&notice](auto &rule) noexcept -> std::optional<ActionCommand> {
          return rule.signal() == notice.id ? rule.on_notice(notice) : std::nullopt;
        },
        rules_[index]);
    if (command.has_value()) {
      out.execute(*command);
    }
  }
}

} // namespace action_engine
