#include "action_engine/rules.hpp"

namespace action_engine {

using vehicle_signals::SignalNotification;

namespace {

// A provider start or a data gap: no transition may span this notice.
[[nodiscard]] bool restarts_baseline(const SignalNotification &notice) noexcept {
  return notice.initial || notice.became_unavailable || notice.recovered;
}

} // namespace

std::optional<ActionCommand> StateRule::on_notice(const SignalNotification &notice) noexcept {
  const bool output = condition_.evaluate(notice.current).value_or(false);
  if (!notice.initial && last_output_ == output) {
    return std::nullopt;
  }
  last_output_ = output;
  return ActionCommand{action_,
                       output ? ActionCommandKind::Activate : ActionCommandKind::Deactivate};
}

std::optional<ActionCommand> EventRule::on_notice(const SignalNotification &notice) noexcept {
  const std::optional<bool> previous = restarts_baseline(notice) ? std::nullopt : baseline_;
  baseline_ = condition_.evaluate(notice.current);
  if (!fires(previous, baseline_)) {
    return std::nullopt;
  }
  return ActionCommand{action_, ActionCommandKind::Trigger};
}

bool EventRule::fires(std::optional<bool> previous, std::optional<bool> current) const noexcept {
  if (!previous.has_value() || !current.has_value() || *previous == *current) {
    return false;
  }
  return *current == (edge_ == EventEdge::BecomesTrue);
}

} // namespace action_engine
