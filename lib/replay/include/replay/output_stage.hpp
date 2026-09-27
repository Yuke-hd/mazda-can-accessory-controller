#pragma once

#include "action_engine/engine.hpp"
#include "vehicle_core/time.hpp"

namespace replay {

// The output side of a host replay: what receives the action engine's
// commands and turns them into observable output. ReplayController drives a
// stage on replay time and never names a concrete output technology.
//
// Lifecycle, all on the controller's host thread:
//   configure(engine)  once, at controller construction, before start();
//   start(now)         once, before the engine attaches;
//   tick(now)          at the end of a successful start(), then on each
//                      scheduled output tick;
//   fail_off(now)      when replay fails, startup is abandoned (including a
//                      failed start()), a tick fails, and immediately before
//                      every stop(); it must leave the output inactive and
//                      may repeat;
//   stop(now)          on every controller stop attempt, after fail_off(),
//                      and may repeat. It releases the stage; the blackout
//                      guarantee belongs to fail_off().
// configure() registers the stage's action_engine::ActionSink(s) and the
// rules that route to them. The engine delivers actions through those sinks
// while the controller processes input or samples polled rules; a stage makes
// them observable on its next tick(). Each method returns false on an output
// fault.
class OutputStage {
public:
  OutputStage(const OutputStage &) = delete;
  OutputStage &operator=(const OutputStage &) = delete;
  OutputStage(OutputStage &&) = delete;
  OutputStage &operator=(OutputStage &&) = delete;

  [[nodiscard]] virtual bool configure(action_engine::ActionEngine &engine) noexcept = 0;
  [[nodiscard]] virtual bool start(vehicle_core::MonotonicTimestamp now_us) noexcept = 0;
  [[nodiscard]] virtual bool tick(vehicle_core::MonotonicTimestamp now_us) noexcept = 0;
  [[nodiscard]] virtual bool fail_off(vehicle_core::MonotonicTimestamp now_us) noexcept = 0;
  [[nodiscard]] virtual bool stop(vehicle_core::MonotonicTimestamp now_us) noexcept = 0;

  // The stage's natural tick cadence, used when the schedule does not
  // override it. Must be non-zero.
  [[nodiscard]] virtual vehicle_core::Microseconds tick_period_us() const noexcept = 0;

protected:
  OutputStage() = default;
  ~OutputStage() = default;
};

} // namespace replay
