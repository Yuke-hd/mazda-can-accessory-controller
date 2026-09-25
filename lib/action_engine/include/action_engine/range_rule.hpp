#pragma once

#include <optional>

#include "action_engine/action.hpp"
#include "action_engine/config_status.hpp"
#include "action_engine/rule_config.hpp"
#include "vehicle_signals/signal_catalog.hpp"
#include "vehicle_signals/signal_contracts.hpp"

namespace action_engine {

// Clamped linear map from an ascending input range onto an output range:
// x <= input.from gives exactly output.from, x >= input.to gives exactly
// output.to, and values in between are interpolated. The endpoint branches
// also keep extreme inputs from overflowing the interpolation. The output may
// be descending (inverse) or equal (constant). `value` must be finite. The
// constructor does not validate; resolve_range_rule() rejects ranges that
// would make the result non-finite.
class LinearMapping final {
public:
  constexpr LinearMapping() noexcept = default;
  constexpr LinearMapping(NumericRange input, NumericRange output) noexcept
      : input_(input), output_(output) {}

  [[nodiscard]] constexpr float map(float value) const noexcept {
    if (value <= input_.from) {
      return output_.from;
    }
    if (value >= input_.to) {
      return output_.to;
    }
    const float fraction = (value - input_.from) / (input_.to - input_.from);
    return output_.from + fraction * (output_.to - output_.from);
  }

private:
  NumericRange input_{};
  NumericRange output_{};
};

// Runtime range rule, sampled at the caller's cadence. Each sample is the
// result of reading the rule's signal:
// - an actionable reading emits SetLevel(map(value)) when the level differs
//   from the last emitted command (or nothing was emitted since reset());
// - a failed read or a non-actionable reading (NoData, Stale, Unavailable, a
//   policy-rejected FreshnessUnverified, a mistyped or non-finite value) is
//   fail-off: Deactivate once, then SetLevel again on recovery.
class RangeRule final {
public:
  constexpr RangeRule() noexcept = default;
  constexpr RangeRule(vehicle_signals::SignalId signal, LinearMapping mapping, ActionId action,
                      FreshnessRequirement freshness) noexcept
      : mapping_(mapping), signal_(signal), action_(action), freshness_(freshness) {}

  [[nodiscard]] constexpr vehicle_signals::SignalId signal() const noexcept { return signal_; }
  [[nodiscard]] constexpr ActionId action() const noexcept { return action_; }
  // Forgets the last emitted command.
  void reset() noexcept { last_.reset(); }
  [[nodiscard]] std::optional<ActionCommand>
  on_sample(const vehicle_signals::SignalResult<vehicle_signals::SignalReading> &read) noexcept;

private:
  [[nodiscard]] ActionCommand command_for(
      const vehicle_signals::SignalResult<vehicle_signals::SignalReading> &read) const noexcept;

  LinearMapping mapping_{};
  vehicle_signals::SignalId signal_{};
  ActionId action_{};
  FreshnessRequirement freshness_{FreshnessRequirement::Fresh};
  std::optional<ActionCommand> last_{};
};

// rule is present exactly when status is Ok.
struct RangeRuleResolution {
  ConfigStatus status{ConfigStatus::UnknownSignal};
  std::optional<RangeRule> rule{};
};

// Resolves a persisted range rule against `catalog`. Failures, in check order:
// UnknownSignal, UnsupportedCapability (no Read), TypeMismatch (not Number),
// InvalidRange (a non-finite bound or span, or input.from >= input.to). The
// ActionId is checked by the engine.
[[nodiscard]] RangeRuleResolution resolve_range_rule(vehicle_signals::SignalCatalogView catalog,
                                                     const RangeRuleConfig &config) noexcept;

} // namespace action_engine
