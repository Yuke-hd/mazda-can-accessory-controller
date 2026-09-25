#pragma once

#include <array>
#include <cstddef>

#include "action_engine/action.hpp"
#include "action_engine/config_status.hpp"

namespace action_engine {

// Bounded broadcast to borrowed sinks: execute() forwards every command to
// every registered sink in registration order. Registered sinks must outlive
// the fan-out's use.
class SinkFanOut final : public ActionSink {
public:
  static constexpr std::size_t kCapacity = 4;

  // Ok, or CapacityExceeded when kCapacity sinks are registered.
  [[nodiscard]] ConfigStatus add(ActionSink &sink) noexcept;
  void execute(const ActionCommand &command) noexcept override;

private:
  std::array<ActionSink *, kCapacity> sinks_{};
  std::size_t count_{0};
};

} // namespace action_engine
