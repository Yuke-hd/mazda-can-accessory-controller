#pragma once

#include <optional>

#include "action_engine/engine.hpp"
#include "controller_config/persisted/application.hpp"
#include "controller_config/persisted/model.hpp"
#include "local_argb/lighting_sink.hpp"
#include "local_argb_actions/led_action_sink.hpp"
#include "vehicle_core/time.hpp"
#include "vehicle_signals/signal_provider.hpp"

namespace companion_config {

// Checks that a parsed configuration would apply at boot, without touching
// the running engine or LED sink (config-transfer.md, "Dry-run apply").
class ApplyCheck {
public:
  ApplyCheck(const ApplyCheck &) = delete;
  ApplyCheck &operator=(const ApplyCheck &) = delete;

  [[nodiscard]] virtual controller_config::persisted::ApplyStatus
  dry_run(const controller_config::persisted::ControllerConfig &config) noexcept = 0;

protected:
  ApplyCheck() noexcept = default;
  ~ApplyCheck() = default;
};

// The production dry run: apply_controller_config() into a fresh scratch
// ActionEngine over the live provider and a fresh scratch LedActionSink, so
// the catalog, capacities and types are those of the boot path. The engine
// is never attached, so it only reads the provider's catalog; the sink
// publishes into a discarding LightingSink. ActionEngine has no reset, so
// each dry run rebuilds both in this object's static-lifetime storage rather
// than on the caller's stack. One dry run at a time: the BLE host task is
// the only caller.
class ScratchApplyCheck final : public ApplyCheck {
public:
  ScratchApplyCheck(vehicle_signals::SignalProvider &provider,
                    vehicle_core::MonotonicClock &clock) noexcept
      : ScratchApplyCheck(provider, clock, action_engine::UnverifiedFreshnessPolicy::Allow) {}
  // Match the live engine's selected-profile policy. Imported compositions
  // pass Reject; the native composition keeps the existing Allow behavior.
  ScratchApplyCheck(vehicle_signals::SignalProvider &provider, vehicle_core::MonotonicClock &clock,
                    action_engine::UnverifiedFreshnessPolicy unverified_freshness) noexcept
      : provider_(provider), clock_(clock), unverified_freshness_(unverified_freshness) {}

  [[nodiscard]] controller_config::persisted::ApplyStatus
  dry_run(const controller_config::persisted::ControllerConfig &config) noexcept override;

private:
  class DiscardingLightingSink final : public local_argb::internal::LightingSink {
  public:
    bool publish(const local_argb::internal::LightingCommand & /*command*/) noexcept override {
      return true;
    }
  };

  vehicle_signals::SignalProvider &provider_;
  vehicle_core::MonotonicClock &clock_;
  action_engine::UnverifiedFreshnessPolicy unverified_freshness_;
  DiscardingLightingSink lighting_{};
  std::optional<action_engine::ActionEngine> engine_{};
  std::optional<local_argb_actions::LedActionSink> leds_{};
};

} // namespace companion_config
