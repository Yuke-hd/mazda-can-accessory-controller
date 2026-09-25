#pragma once

#include <cstddef>

#include "action_engine/action.hpp"
#include "action_engine/condition.hpp"
#include "action_engine/config_status.hpp"
#include "action_engine/rule_config.hpp"
#include "action_engine/rule_set.hpp"
#include "action_engine/sink_fan_out.hpp"
#include "action_engine/subscription_set.hpp"
#include "vehicle_signals/signal_contracts.hpp"
#include "vehicle_signals/signal_provider.hpp"

namespace action_engine {

// Coordinates one provider, a bounded set of rules and a bounded set of sinks.
// The engine knows only the generic provider port: application composition
// chooses the concrete provider at construction and it cannot be swapped.
//
// Setup: add_sink(), add_state_rule() and add_event_rule() are accepted only
// while detached (InvalidState otherwise). Rules are resolved through the
// provider catalog when added; runtime rules keep only SignalIds and compact
// values. Capacities are fixed (kMaxSinks, kMaxRules) and evaluation never
// allocates.
//
// Lifecycle: attach() and detach() are provider subscription mutations, so
// they run on the provider's lifecycle owner while the provider is stopped.
// attach() resets every rule's runtime state and subscribes once per distinct
// signal used by the rules; if a subscription fails it unsubscribes the ones
// it made and returns the provider's failure. detach() unsubscribes
// everything; on a provider failure the remaining registrations are kept, the
// engine stays attached, and detach() can be retried. Misuse local to the
// engine (attach while attached, detach while detached) is InvalidState.
//
// Borrowing: the engine is the provider's callback context. Detach it before
// destroying it unless the provider is destroyed first. The provider and all
// sinks must outlive the engine's attachment.
//
// Concurrency: the provider must deliver notifications serially (one
// dispatcher context, as the port requires); the engine does no locking.
// Rules on a signal see its notices in insertion order, and each command is
// sent to every sink in registration order on that dispatcher context.
class ActionEngine final {
public:
  static constexpr std::size_t kMaxSinks = SinkFanOut::kCapacity;
  static constexpr std::size_t kMaxRules = RuleSet::kCapacity;

  explicit ActionEngine(vehicle_signals::SignalProvider &provider) noexcept;

  ActionEngine(const ActionEngine &) = delete;
  ActionEngine &operator=(const ActionEngine &) = delete;
  ActionEngine(ActionEngine &&) = delete;
  ActionEngine &operator=(ActionEngine &&) = delete;

  // Registers a borrowed sink that receives every command.
  [[nodiscard]] ConfigStatus add_sink(ActionSink &sink) noexcept;
  [[nodiscard]] ConfigStatus add_state_rule(const StateRuleConfig &config) noexcept;
  [[nodiscard]] ConfigStatus add_event_rule(const EventRuleConfig &config) noexcept;

  [[nodiscard]] vehicle_signals::SignalStatus attach() noexcept;
  [[nodiscard]] vehicle_signals::SignalStatus detach() noexcept;
  // True from a successful attach() (or a failed attach whose rollback left
  // registrations behind) until a successful detach().
  [[nodiscard]] bool attached() const noexcept { return attached_; }

private:
  static void on_notice(void *context, const vehicle_signals::SignalNotification &notice) noexcept;

  // Checks the detached state and the action, then resolves the condition.
  [[nodiscard]] ConditionResolution resolve_rule(const SignalCondition &condition, ActionId action,
                                                 FreshnessRequirement freshness) const noexcept;

  vehicle_signals::SignalProvider *provider_{nullptr};
  SinkFanOut sinks_{};
  RuleSet rules_{};
  SubscriptionSet subscriptions_{};
  bool attached_{false};
};

} // namespace action_engine
