#pragma once

#include <array>
#include <cstddef>
#include <variant>

#include "action_engine/action.hpp"
#include "action_engine/config_status.hpp"
#include "action_engine/rules.hpp"
#include "vehicle_signals/signal_contracts.hpp"

namespace action_engine {

using Rule = std::variant<StateRule, EventRule>;

// Fixed-capacity, allocation-free collection of runtime rules. dispatch()
// hands a notice to every rule on its signal, in insertion order, and sends
// each resulting command to `out`.
class RuleSet final {
public:
  static constexpr std::size_t kCapacity = 16;

  // Ok, or CapacityExceeded when kCapacity rules are stored.
  [[nodiscard]] ConfigStatus add(const Rule &rule) noexcept;

  [[nodiscard]] std::size_t size() const noexcept { return count_; }
  // Signal of the rule at `index` (< size()).
  [[nodiscard]] vehicle_signals::SignalId signal(std::size_t index) const noexcept;
  // True when a state (level) rule already drives `action`.
  [[nodiscard]] bool drives_level(ActionId action) const noexcept;

  // Forgets every rule's runtime state.
  void reset() noexcept;
  void dispatch(const vehicle_signals::SignalNotification &notice, ActionSink &out) noexcept;

private:
  std::array<Rule, kCapacity> rules_{};
  std::size_t count_{0};
};

} // namespace action_engine
