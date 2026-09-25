#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>

#include "action_engine/action.hpp"
#include "action_engine/condition.hpp"
#include "action_engine/config_status.hpp"
#include "action_engine/range_rule_set.hpp"
#include "action_engine/rule_config.hpp"
#include "action_engine/rule_set.hpp"
#include "action_engine/sink_fan_out.hpp"
#include "action_engine/subscription_set.hpp"
#include "vehicle_signals/signal_contracts.hpp"
#include "vehicle_signals/signal_provider.hpp"

namespace action_engine {

// Coordinates one provider, bounded sets of rules and a bounded set of sinks.
// The engine knows only the generic provider port: application composition
// chooses the concrete provider at construction and it cannot be swapped.
//
// Setup: add_sink(), add_state_rule(), add_event_rule() and add_range_rule()
// are accepted only while detached (InvalidState otherwise). A sink is
// registered at most once (DuplicateSink). State and range rules are level
// outputs, so each ActionId is driven by at most one of them
// (DuplicateAction); otherwise the sink's level would depend on rule order.
// Event rules emit one-shot Triggers and may share an ActionId with each other
// and with one level rule. Rules are resolved through the provider catalog
// when added; runtime rules keep only SignalIds and compact values.
// Capacities are fixed (kMaxSinks, kMaxRules, kMaxRangeRules) and evaluation
// never allocates.
//
// Cadence: state and event rules run on provider notices. Range rules run
// only when the application calls sample_range_rules() at its own cadence;
// each call reads every range rule's signal through the provider port. The
// signal layer gains no polling worker or synthetic notification.
//
// Lifecycle: attach() and detach() are provider subscription mutations, so
// they run on the provider's lifecycle owner while the provider is stopped.
// attach() resets every rule's runtime state (including range rules) and
// subscribes once per distinct signal used by state and event rules; range
// rules need no subscription; if a subscription fails it unsubscribes the ones
// it made and returns the provider's failure. detach() unsubscribes
// everything; on a provider failure the remaining registrations are kept, the
// engine stays attached, and detach() can be retried. Misuse local to the
// engine (attach while attached, detach while detached) is InvalidState.
//
// Borrowing: the engine is the provider's callback context. Detach it before
// destroying it unless the provider is destroyed first. The provider and all
// sinks must outlive the engine's attachment.
//
// Concurrency: the provider delivers notifications serially on its dispatcher
// context, while sample_range_rules() runs on the caller's context. The engine
// serializes all rule evaluation and sink delivery with one mutex, so sinks
// never see concurrent execute() calls. Provider reads happen outside the
// lock. Call sample_range_rules() from one context at a time, and never
// concurrently with configuration, attach() or detach(). Rules on a signal see
// its notices in insertion order, and each command is sent to every sink in
// registration order.
class ActionEngine final {
public:
  static constexpr std::size_t kMaxSinks = SinkFanOut::kCapacity;
  static constexpr std::size_t kMaxRules = RuleSet::kCapacity;
  static constexpr std::size_t kMaxRangeRules = RangeRuleSet::kCapacity;

  explicit ActionEngine(vehicle_signals::SignalProvider &provider) noexcept;

  ActionEngine(const ActionEngine &) = delete;
  ActionEngine &operator=(const ActionEngine &) = delete;
  ActionEngine(ActionEngine &&) = delete;
  ActionEngine &operator=(ActionEngine &&) = delete;

  // Registers a borrowed sink that receives every command once.
  [[nodiscard]] ConfigStatus add_sink(ActionSink &sink) noexcept;
  [[nodiscard]] ConfigStatus add_state_rule(const StateRuleConfig &config) noexcept;
  [[nodiscard]] ConfigStatus add_event_rule(const EventRuleConfig &config) noexcept;
  [[nodiscard]] ConfigStatus add_range_rule(const RangeRuleConfig &config) noexcept;

  [[nodiscard]] vehicle_signals::SignalStatus attach() noexcept;
  [[nodiscard]] vehicle_signals::SignalStatus detach() noexcept;
  // True from a successful attach() (or a failed attach whose rollback left
  // registrations behind) until a successful detach().
  [[nodiscard]] bool attached() const noexcept { return attached_; }

  // Reads each range rule's signal and emits its SetLevel or fail-off
  // Deactivate when that changes. InvalidState while detached; otherwise Ok.
  // Read failures are per-rule fail-off, not a returned status.
  [[nodiscard]] vehicle_signals::SignalStatus sample_range_rules() noexcept;

private:
  static void on_notice(void *context, const vehicle_signals::SignalNotification &notice) noexcept;
  // Forgets every rule's runtime state.
  void reset_rules() noexcept;

  // Level outputs (Activate/Deactivate, SetLevel) own their ActionId;
  // one-shot Triggers may share one.
  enum class RuleOutput : std::uint8_t { Level, OneShot };

  // InvalidState, InvalidAction or DuplicateAction, else Ok.
  [[nodiscard]] ConfigStatus check_action(ActionId action, RuleOutput output) const noexcept;
  // Checks the action, then resolves the condition through the catalog.
  [[nodiscard]] ConditionResolution resolve_rule(const SignalCondition &condition, ActionId action,
                                                 FreshnessRequirement freshness,
                                                 RuleOutput output) const noexcept;

  vehicle_signals::SignalProvider *provider_{nullptr};
  SinkFanOut sinks_{};
  RuleSet rules_{};
  RangeRuleSet range_rules_{};
  SubscriptionSet subscriptions_{};
  // Serializes rule evaluation and sink delivery across the provider's
  // dispatcher and the sampling context.
  std::mutex evaluation_{};
  bool attached_{false};
};

} // namespace action_engine
