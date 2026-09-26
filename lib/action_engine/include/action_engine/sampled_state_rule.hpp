#pragma once

#include <optional>

#include "action_engine/action.hpp"
#include "action_engine/condition.hpp"
#include "vehicle_signals/signal_contracts.hpp"

namespace action_engine {

// Runtime sampled state rule, sampled at the caller's cadence. Each sample is
// the result of reading the rule's signal. Its output is "the read succeeded,
// the reading is actionable and the activation/release condition holds"; a
// failed read or a non-actionable reading (NoData, Stale, Unavailable, a
// policy-rejected FreshnessUnverified, a mistyped or non-finite value) is
// fail-off. An optional release condition preserves the active state between
// distinct activation and release thresholds. It emits Activate/Deactivate
// only when the output differs from the last emitted one; after reset() the
// last output is unknown, so the first sample uses the activation condition
// and emits an explicit baseline.
class SampledStateRule final {
public:
  constexpr SampledStateRule() noexcept = default;
  constexpr SampledStateRule(
      const ResolvedCondition &activation_condition, ActionId action,
      std::optional<ResolvedCondition> release_condition = std::nullopt) noexcept
      : activation_condition_(activation_condition), release_condition_(release_condition),
        action_(action) {}

  [[nodiscard]] constexpr vehicle_signals::SignalId signal() const noexcept {
    return activation_condition_.signal();
  }
  [[nodiscard]] constexpr ActionId action() const noexcept { return action_; }
  // Forgets the last emitted output.
  void reset() noexcept { last_output_.reset(); }
  [[nodiscard]] std::optional<ActionCommand>
  on_sample(const vehicle_signals::SignalResult<vehicle_signals::SignalReading> &read) noexcept;

private:
  ResolvedCondition activation_condition_{};
  std::optional<ResolvedCondition> release_condition_{};
  ActionId action_{};
  std::optional<bool> last_output_{};
};

} // namespace action_engine
