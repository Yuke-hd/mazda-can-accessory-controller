#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "action_engine/engine.hpp"
#include "controller_config/persisted/application.hpp"
#include "controller_config/persisted/production_profile.hpp"
#include "local_argb/lighting_sink.hpp"
#include "local_argb_actions/led_action_sink.hpp"
#include "support/fake_signal_provider.hpp"
#include "vehicle_signals/signal_catalog.hpp"

#include <cstdint>
#include <vector>

namespace {

using action_engine::ActionEngine;
using action_engine::ConfigStatus;
using local_argb::internal::LightingCommand;
using local_argb_actions::LedActionSink;
using vehicle_signals::Availability;
using vehicle_signals::SignalCapability;
using vehicle_signals::SignalCatalogView;
using vehicle_signals::SignalEnumChoice;
using vehicle_signals::SignalId;
using vehicle_signals::SignalMetadata;
using vehicle_signals::SignalNotification;
using vehicle_signals::SignalReading;
using vehicle_signals::SignalType;
using vehicle_signals::SignalUnit;
using vehicle_signals::SignalValue;
using vehicle_signals::ValidationStatus;

constexpr SignalId kTurnState{1};
constexpr SignalId kEngineRpm{2};
constexpr SignalEnumChoice kTurnChoices[] = {{1, "left"}, {2, "right"}, {3, "hazard"}};
constexpr SignalMetadata kCatalog[] = {
    {kTurnState, "vehicle.turn_state", SignalType::Enum, SignalUnit::None,
     ValidationStatus::Reference, SignalCapability::Notify, kTurnChoices, 3},
    {kEngineRpm, "vehicle.engine_rpm", SignalType::Number, SignalUnit::RevolutionsPerMinute,
     ValidationStatus::Reference, SignalCapability::Read, nullptr, 0},
};
constexpr SignalCatalogView kView{kCatalog};
static_assert(kView.well_formed());

class RecordingLightingSink final : public local_argb::internal::LightingSink {
public:
  bool publish(const LightingCommand &command) noexcept override {
    commands.push_back(command);
    return true;
  }

  std::vector<LightingCommand> commands{};
};

struct Controller final {
  Controller() : leds{lighting}, engine{provider} {}

  test_support::FakeSignalProvider provider{kView};
  RecordingLightingSink lighting{};
  LedActionSink leds;
  ActionEngine engine;
};

SignalNotification turn(const std::uint16_t value) {
  return SignalNotification{kTurnState,
                            SignalReading{SignalValue::enumeration(value), Availability::Fresh,
                                          ValidationStatus::Reference},
                            false,
                            false,
                            false,
                            false};
}

} // namespace

TEST_CASE("applying the persisted production configuration reproduces the controller setup") {
  Controller controller{};

  const auto config = controller_config::persisted::production_lighting_config();
  const auto status = controller_config::persisted::apply_controller_config(config, controller.leds,
                                                                            controller.engine);

  REQUIRE(status.ok());
  CHECK(status.outputs_applied == config.outputs.size());
  CHECK(status.rules_applied == config.rules.size());
  CHECK(status.sink_registered);
  CHECK(status.binding == local_argb_actions::BindingStatus::Ok);
  CHECK(status.engine == ConfigStatus::Ok);
}

TEST_CASE("the persisted production configuration keeps mirrored turn and fail-off behavior") {
  Controller controller{};
  const auto config = controller_config::persisted::production_lighting_config();
  REQUIRE(controller_config::persisted::apply_controller_config(config, controller.leds,
                                                                controller.engine)
              .ok());
  REQUIRE(controller.engine.attach() == vehicle_signals::SignalStatus::Ok);
  controller.provider.start();

  REQUIRE(controller.provider.publish(turn(1)) == 1);
  REQUIRE_FALSE(controller.lighting.commands.empty());
  CHECK(controller.lighting.commands.back().right_turn);
  CHECK_FALSE(controller.lighting.commands.back().left_turn);

  REQUIRE(controller.provider.publish(
              SignalNotification{kTurnState,
                                 SignalReading{SignalValue::enumeration(1), Availability::Stale,
                                               ValidationStatus::Reference},
                                 false, false, false, false}) == 1);
  CHECK_FALSE(controller.lighting.commands.back().actionable);

  controller.provider.stop();
  CHECK(controller.engine.detach() == vehicle_signals::SignalStatus::Ok);
}
