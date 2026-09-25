#pragma once

#include <optional>

#include "action_engine/action.hpp"
#include "action_engine/condition.hpp"
#include "action_engine/rule_config.hpp"
#include "vehicle_signals/signal_contracts.hpp"

// Runtime rules. Each evaluates one latest-state notice of its signal and
// returns at most one command. Notices are latest-state: transitions merged
// by provider coalescing are never reconstructed.

namespace action_engine {

// Level rule. Its output is "the reading is actionable and the condition
// holds"; every non-actionable reading (NoData, Stale, Unavailable, a policy-
// rejected FreshnessUnverified, a mistyped or non-finite value) is fail-off.
// - initial notice: always emits the explicit Activate/Deactivate, so every
//   provider start re-establishes the sink baseline.
// - otherwise: emits only when the output differs from the last emitted one
//   (unknown after reset()). became_unavailable therefore yields Deactivate,
//   recovered re-evaluates, and a coalesced notice evaluates only its latest
//   state.
class StateRule final {
public:
  constexpr StateRule() noexcept = default;
  constexpr StateRule(const ResolvedCondition &condition, ActionId action) noexcept
      : condition_(condition), action_(action) {}

  [[nodiscard]] constexpr vehicle_signals::SignalId signal() const noexcept {
    return condition_.signal();
  }
  [[nodiscard]] constexpr ActionId action() const noexcept { return action_; }
  // Forgets the last emitted output.
  void reset() noexcept { last_output_.reset(); }
  [[nodiscard]] std::optional<ActionCommand>
  on_notice(const vehicle_signals::SignalNotification &notice) noexcept;

private:
  ResolvedCondition condition_{};
  ActionId action_{};
  std::optional<bool> last_output_{};
};

// Edge rule. It keeps the condition's last actionable value as a baseline and
// emits Trigger only for a change in the configured direction between two
// consecutive actionable readings. It never triggers on:
// - an initial notice (it sets the baseline);
// - a non-actionable notice (it clears the baseline);
// - a notice flagged became_unavailable or recovered: a data gap, so the
//   baseline restarts from the current reading when it is actionable.
// A coalesced notice compares only its latest state with the baseline, so it
// triggers at most once and edges merged away by coalescing are lost.
class EventRule final {
public:
  constexpr EventRule() noexcept = default;
  constexpr EventRule(const ResolvedCondition &condition, EventEdge edge, ActionId action) noexcept
      : condition_(condition), action_(action), edge_(edge) {}

  [[nodiscard]] constexpr vehicle_signals::SignalId signal() const noexcept {
    return condition_.signal();
  }
  // Forgets the baseline.
  void reset() noexcept { baseline_.reset(); }
  [[nodiscard]] std::optional<ActionCommand>
  on_notice(const vehicle_signals::SignalNotification &notice) noexcept;

private:
  [[nodiscard]] bool fires(std::optional<bool> previous,
                           std::optional<bool> current) const noexcept;

  ResolvedCondition condition_{};
  ActionId action_{};
  EventEdge edge_{EventEdge::BecomesTrue};
  std::optional<bool> baseline_{};
};

} // namespace action_engine
