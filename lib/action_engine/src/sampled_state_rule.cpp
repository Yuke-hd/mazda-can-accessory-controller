#include "action_engine/sampled_state_rule.hpp"

namespace action_engine {

using vehicle_signals::SignalReading;
using vehicle_signals::SignalResult;

std::optional<ActionCommand>
SampledStateRule::on_sample(const SignalResult<SignalReading> &read) noexcept {
  const bool output = read.ok() && condition_.evaluate(*read.value).value_or(false);
  if (last_output_ == output) {
    return std::nullopt;
  }
  last_output_ = output;
  return ActionCommand{action_,
                       output ? ActionCommandKind::Activate : ActionCommandKind::Deactivate};
}

} // namespace action_engine
