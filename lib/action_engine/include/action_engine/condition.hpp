#pragma once

#include <optional>

#include "action_engine/config_status.hpp"
#include "action_engine/rule_config.hpp"
#include "vehicle_signals/signal_catalog.hpp"
#include "vehicle_signals/signal_contracts.hpp"

namespace action_engine {

// True when `availability` satisfies `requirement`. NoData, Stale and
// Unavailable never do.
[[nodiscard]] constexpr bool meets(FreshnessRequirement requirement,
                                   vehicle_signals::Availability availability) noexcept {
  using vehicle_signals::Availability;
  if (availability == Availability::Fresh) {
    return true;
  }
  return availability == Availability::FreshnessUnverified &&
         requirement == FreshnessRequirement::FreshOrUnverified;
}

// Runtime form of a SignalCondition: the catalog key is resolved to a
// SignalId and a choice key to its raw enum value, so evaluation needs no
// catalog, string or allocation. Construct it through resolve_condition(); the
// constructor itself does not validate.
class ResolvedCondition final {
public:
  constexpr ResolvedCondition() noexcept = default;
  constexpr ResolvedCondition(vehicle_signals::SignalId signal, Comparison comparison,
                              vehicle_signals::SignalValue operand,
                              FreshnessRequirement freshness) noexcept
      : signal_(signal), operand_(operand), comparison_(comparison), freshness_(freshness) {}

  [[nodiscard]] constexpr vehicle_signals::SignalId signal() const noexcept { return signal_; }

  // Whether the condition holds for `reading`, or std::nullopt when the
  // reading is not actionable under this condition's freshness requirement.
  [[nodiscard]] std::optional<bool>
  evaluate(const vehicle_signals::SignalReading &reading) const noexcept;

private:
  [[nodiscard]] bool actionable(const vehicle_signals::SignalReading &reading) const noexcept;
  [[nodiscard]] bool holds(const vehicle_signals::SignalValue &value) const noexcept;

  vehicle_signals::SignalId signal_{};
  vehicle_signals::SignalValue operand_{};
  Comparison comparison_{Comparison::Equal};
  FreshnessRequirement freshness_{FreshnessRequirement::Fresh};
};

// condition is present exactly when status is Ok.
struct ConditionResolution {
  ConfigStatus status{ConfigStatus::UnknownSignal};
  std::optional<ResolvedCondition> condition{};
};

// Resolves a persisted condition against `catalog`. Failures, in check order:
// UnknownSignal, UnsupportedCapability (no Notify), TypeMismatch,
// InvalidOperand (non-finite Number), UnsupportedComparison (ordered
// comparison on a non-Number signal), UnknownChoice.
[[nodiscard]] ConditionResolution resolve_condition(vehicle_signals::SignalCatalogView catalog,
                                                    const SignalCondition &condition,
                                                    FreshnessRequirement freshness) noexcept;

} // namespace action_engine
