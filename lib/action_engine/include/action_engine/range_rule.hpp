#pragma once

#include <array>
#include <cstddef>
#include <optional>

#include "action_engine/action.hpp"
#include "action_engine/config_status.hpp"
#include "action_engine/rule_config.hpp"
#include "vehicle_signals/signal_catalog.hpp"
#include "vehicle_signals/signal_contracts.hpp"

namespace action_engine {

// Clamped linear or piecewise-linear map. The NumericRange constructor
// preserves the original two-endpoint mapping API. The curve constructor
// copies its borrowed points into fixed storage. `value` must be finite. The
// constructors do not validate; resolve_range_rule() rejects configurations
// that would make interpolation non-finite.
class LinearMapping final {
public:
  constexpr LinearMapping() noexcept = default;
  constexpr LinearMapping(NumericRange input, NumericRange output) noexcept
      : points_{{{input.from, output.from}, {input.to, output.to}}}, point_count_(2) {}
  constexpr explicit LinearMapping(NumericCurveView curve) noexcept {
    if (curve.data == nullptr || curve.count < 2 || curve.count > points_.size()) {
      return;
    }
    point_count_ = curve.count;
    for (std::size_t index = 0; index < curve.count; ++index) {
      points_[index] = curve.data[index];
    }
  }

  [[nodiscard]] constexpr float map(float value) const noexcept {
    if (value <= points_[0].input) {
      return points_[0].output;
    }
    for (std::size_t index = 1; index < point_count_; ++index) {
      const NumericControlPoint &right = points_[index];
      if (value == right.input) {
        return right.output;
      }
      if (value < right.input) {
        const NumericControlPoint &left = points_[index - 1];
        const float fraction = (value - left.input) / (right.input - left.input);
        return left.output + fraction * (right.output - left.output);
      }
    }
    return points_[point_count_ - 1].output;
  }

private:
  std::array<NumericControlPoint, NumericCurveView::kMaxPoints> points_{};
  std::size_t point_count_{2};
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
// InvalidRange (invalid selected legacy ranges, or a present curve with a null
// non-empty view, fewer than two or more than eight points, non-finite values
// or spans, or non-ascending inputs). The ActionId is checked by the engine.
[[nodiscard]] RangeRuleResolution resolve_range_rule(vehicle_signals::SignalCatalogView catalog,
                                                     const RangeRuleConfig &config) noexcept;

} // namespace action_engine
