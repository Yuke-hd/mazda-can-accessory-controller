#include "replay/local_argb_stage.hpp"

#include "controller_config/lighting_profile.hpp"
#include "controller_config/lighting_profile_application.hpp"
#include "local_argb/lighting_sink.hpp"
#include "local_argb/local_argb.h"
#include "local_argb/renderer.hpp"
#include "local_argb_actions/led_action_sink.hpp"

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
  explicit Implementation(local_argb::PixelFrameSink &pixels)
      : mailbox_sink_(mailbox_), led_actions_(mailbox_sink_),
        renderer_(pixels, local_argb::internal::kCenterOutFillAnimation) {}

  [[nodiscard]] bool configure(action_engine::ActionEngine &engine) noexcept {
    return controller_config::apply_lighting_profile(controller_config::kDefaultLightingProfile,
                                                     led_actions_, engine)
        .ok();
  }

  [[nodiscard]] bool start() noexcept { return renderer_.start(); }

  // Applies the latest command the engine published since the previous
  // tick, or advances the current animation.
  [[nodiscard]] bool tick(const vehicle_core::MonotonicTimestamp now_us) noexcept {
    local_argb::internal::LightingCommand command{};
    return mailbox_.take(command) ? renderer_.apply(command, now_us) : renderer_.tick(now_us);
  }

  [[nodiscard]] bool fail_off(const vehicle_core::MonotonicTimestamp now_us) noexcept {
    return renderer_.apply(local_argb::internal::LightingCommand{}, now_us);
  }

private:
  local_argb::internal::Mailbox mailbox_{};
  MailboxLightingSink mailbox_sink_;
  local_argb_actions::LedActionSink led_actions_;
  local_argb::internal::RendererController renderer_;
};

LocalArgbOutputStage::LocalArgbOutputStage(local_argb::PixelFrameSink &pixels)
    : implementation_(std::make_unique<Implementation>(pixels)) {}

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
