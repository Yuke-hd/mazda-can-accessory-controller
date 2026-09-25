#pragma once

#include <array>
#include <cstddef>

#include "action_engine/action.hpp"
#include "action_engine/config_status.hpp"
#include "action_engine/range_rule.hpp"
#include "vehicle_signals/signal_contracts.hpp"

namespace action_engine {

// Fixed-capacity, allocation-free collection of range rules. The caller reads
// each rule's signal and hands the result to on_sample(), which sends the
// rule's command, if any, to `out`.
class RangeRuleSet final {
public:
  static constexpr std::size_t kCapacity = 8;

  // Ok, or CapacityExceeded when kCapacity rules are stored.
  [[nodiscard]] ConfigStatus add(const RangeRule &rule) noexcept;

  [[nodiscard]] std::size_t size() const noexcept { return count_; }
  // Signal of the rule at `index` (< size()).
  [[nodiscard]] vehicle_signals::SignalId signal(std::size_t index) const noexcept {
    return rules_[index].signal();
  }
  // True when a range rule already drives `action`.
  [[nodiscard]] bool drives(ActionId action) const noexcept;

  // Forgets every rule's last emitted command.
  void reset() noexcept;
  void on_sample(std::size_t index,
                 const vehicle_signals::SignalResult<vehicle_signals::SignalReading> &read,
                 ActionSink &out) noexcept;

private:
  std::array<RangeRule, kCapacity> rules_{};
  std::size_t count_{0};
};

} // namespace action_engine
