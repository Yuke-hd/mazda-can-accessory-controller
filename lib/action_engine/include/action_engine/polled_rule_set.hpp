#pragma once

#include <array>
#include <cstddef>
#include <variant>

#include "action_engine/action.hpp"
#include "action_engine/config_status.hpp"
#include "action_engine/range_rule.hpp"
#include "action_engine/sampled_state_rule.hpp"
#include "vehicle_signals/signal_contracts.hpp"

namespace action_engine {

// A rule evaluated on sampled reads rather than provider notices. Every
// alternative provides signal(), action(), reset() and
// on_sample(SignalResult<SignalReading>) -> std::optional<ActionCommand>.
using PolledRule = std::variant<RangeRule, SampledStateRule>;

// Fixed-capacity, allocation-free collection of polled rules; range and
// sampled state rules share its capacity. The caller reads each rule's signal
// and hands the result to on_sample(), which sends the rule's command, if
// any, to `out`. All polled rules are level outputs.
class PolledRuleSet final {
public:
  static constexpr std::size_t kCapacity = 8;

  // Ok, or CapacityExceeded when kCapacity rules are stored.
  [[nodiscard]] ConfigStatus add(const PolledRule &rule) noexcept;

  [[nodiscard]] std::size_t size() const noexcept { return count_; }
  // Signal of the rule at `index` (< size()).
  [[nodiscard]] vehicle_signals::SignalId signal(std::size_t index) const noexcept;
  // True when a polled rule already drives `action`.
  [[nodiscard]] bool drives(ActionId action) const noexcept;

  // Forgets every rule's last emitted output.
  void reset() noexcept;
  void on_sample(std::size_t index,
                 const vehicle_signals::SignalResult<vehicle_signals::SignalReading> &read,
                 ActionSink &out) noexcept;

private:
  std::array<PolledRule, kCapacity> rules_{};
  std::size_t count_{0};
};

} // namespace action_engine
