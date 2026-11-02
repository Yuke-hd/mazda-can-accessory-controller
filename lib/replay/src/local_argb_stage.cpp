#include "replay/local_argb_stage.hpp"

#include <optional>
#include <utility>

#include "controller_config/lighting_profile.hpp"
#include "controller_config/lighting_profile_application.hpp"
#include "controller_config/persisted/application.hpp"
#include "controller_config/persisted/json_loader.hpp"
#include "local_argb/lighting_sink.hpp"
#include "local_argb/local_argb.h"
#include "local_argb/renderer.hpp"
#include "local_argb/stall_gated_sink.hpp"
#include "local_argb_actions/led_action_sink.hpp"

#if defined(CONTROLLER_CONFIG_FACTORY_JSON_EMBEDDED)
#include "factory_config.hpp"
#endif

namespace replay {
namespace {

class MailboxLightingSink final : public local_argb::internal::LightingSink {
public:
  explicit MailboxLightingSink(local_argb::internal::Mailbox &mailbox) noexcept
      : mailbox_(&mailbox) {}

  [[nodiscard]] bool
  publish(const local_argb::internal::LightingCommand &command) noexcept override {
    mailbox_->submit(command);
    return true;
  }

private:
  local_argb::internal::Mailbox *mailbox_;
};

} // namespace

class LocalArgbOutputStage::Implementation final {
public:
  Implementation(local_argb::PixelFrameSink &pixels, vehicle_core::MonotonicClock &clock)
      : mailbox_sink_(mailbox_), gate_(mailbox_sink_), led_actions_(gate_, clock),
        renderer_(pixels, local_argb::internal::kCenterOutFillAnimation) {}

  [[nodiscard]] bool configure(action_engine::ActionEngine &engine) noexcept {
#if defined(CONTROLLER_CONFIG_FACTORY_JSON_EMBEDDED)
    auto parsed =
        controller_config::persisted::parse_controller_config(generated::kFactoryConfigJson);
    if (!parsed.ok())
      return false;
    factory_config_.emplace(std::move(*parsed.configuration));
    return controller_config::persisted::apply_controller_config(*factory_config_, led_actions_,
                                                                 engine)
        .ok();
#else
#if defined(REPLAY_SELECTED_PROFILE_NATIVE) && REPLAY_SELECTED_PROFILE_NATIVE
    return controller_config::apply_lighting_profile(controller_config::kDefaultLightingProfile,
                                                     led_actions_, engine)
        .ok();
#else
#error "selected-profile replay must consume generated factory JSON"
#endif
#endif
  }

  [[nodiscard]] bool start() noexcept { return renderer_.start(); }

  // Applies the latest command the engine published since the previous
  // tick, or advances the current animation.
  [[nodiscard]] bool tick(const vehicle_core::MonotonicTimestamp now_us) noexcept {
    local_argb::internal::LightingCommand command{};
    return mailbox_.take(command) ? renderer_.apply(command, now_us, gate_.transient_epoch())
                                  : renderer_.tick(now_us);
  }

  [[nodiscard]] bool fail_off(const vehicle_core::MonotonicTimestamp now_us) noexcept {
    gate_.invalidate_transients();
    local_argb::internal::LightingCommand discarded{};
    (void)mailbox_.take(discarded);
    return renderer_.apply(local_argb::internal::LightingCommand{}, now_us,
                           gate_.transient_epoch());
  }

private:
  local_argb::internal::Mailbox mailbox_{};
  MailboxLightingSink mailbox_sink_;
  local_argb::internal::StallGatedSink gate_;
  local_argb_actions::LedActionSink led_actions_;
  local_argb::internal::RendererController renderer_;
  std::optional<controller_config::persisted::ControllerConfig> factory_config_{};
};

LocalArgbOutputStage::LocalArgbOutputStage(local_argb::PixelFrameSink &pixels,
                                           vehicle_core::MonotonicClock &clock)
    : implementation_(std::make_unique<Implementation>(pixels, clock)) {}

LocalArgbOutputStage::~LocalArgbOutputStage() noexcept = default;

bool LocalArgbOutputStage::configure(action_engine::ActionEngine &engine) noexcept {
  return implementation_->configure(engine);
}

bool LocalArgbOutputStage::start(vehicle_core::MonotonicTimestamp /*now_us*/) noexcept {
  return implementation_->start();
}

bool LocalArgbOutputStage::tick(const vehicle_core::MonotonicTimestamp now_us) noexcept {
  return implementation_->tick(now_us);
}

bool LocalArgbOutputStage::fail_off(const vehicle_core::MonotonicTimestamp now_us) noexcept {
  return implementation_->fail_off(now_us);
}

// The controller has already failed the renderer off; the renderer holds
// no resource that outlives the stage.
bool LocalArgbOutputStage::stop(vehicle_core::MonotonicTimestamp /*now_us*/) noexcept {
  return true;
}

// One renderer pass per supervisor poll, matching the firmware worker.
vehicle_core::Microseconds LocalArgbOutputStage::tick_period_us() const noexcept {
  return local_argb::kSupervisorPollUs;
}

} // namespace replay
