#include "companion_config/apply_check.hpp"

namespace companion_config {

controller_config::persisted::ApplyStatus
ScratchApplyCheck::dry_run(const controller_config::persisted::ControllerConfig &config) noexcept {
  // The engine holds a pointer to the LED sink once it is registered, so the
  // engine goes first.
  engine_.reset();
  leds_.reset();
  leds_.emplace(lighting_, clock_);
  engine_.emplace(provider_);
  // Never attached: the dry run resolves rules against the provider catalog
  // and binds outputs, but subscribes to nothing and publishes nothing.
  return controller_config::persisted::apply_controller_config(config, *leds_, *engine_);
}

} // namespace companion_config
