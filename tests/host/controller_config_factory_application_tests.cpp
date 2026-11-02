#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "action_engine/engine.hpp"
#include "controller_config/persisted/application.hpp"
#include "controller_config/persisted/json_loader.hpp"
#include "local_argb/lighting_sink.hpp"
#include "local_argb_actions/led_action_sink.hpp"
#include "support/fake_signal_provider.hpp"

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

namespace persisted = controller_config::persisted;
using action_engine::ActionEngine;
using local_argb::internal::LightingCommand;
using local_argb_actions::LedActionSink;
using vehicle_signals::SignalCatalogView;
using vehicle_signals::SignalEnumChoice;
using vehicle_signals::SignalId;
using vehicle_signals::SignalMetadata;
using vehicle_signals::SignalType;
using vehicle_signals::SignalUnit;
using vehicle_signals::ValidationStatus;

constexpr SignalId kTurnState{1};
constexpr SignalId kEngineRpm{2};
constexpr SignalId kBrakePressed{3};
constexpr SignalEnumChoice kTurnChoices[] = {{1, "left"}, {2, "right"}, {3, "hazard"}};
constexpr SignalMetadata kCatalog[] = {
    {kTurnState, "vehicle.turn_state", SignalType::Enum, SignalUnit::None,
     ValidationStatus::Reference, vehicle_signals::SignalCapability::Notify, kTurnChoices, 3},
    {kEngineRpm, "vehicle.engine_rpm", SignalType::Number, SignalUnit::RevolutionsPerMinute,
     ValidationStatus::Reference, vehicle_signals::SignalCapability::Read, nullptr, 0},
    {kBrakePressed, "vehicle.brake_pressed", SignalType::Boolean, SignalUnit::None,
     ValidationStatus::Confirmed,
     vehicle_signals::SignalCapability::Read | vehicle_signals::SignalCapability::Notify,
     nullptr, 0},
};
constexpr SignalCatalogView kView{kCatalog};
static_assert(kView.well_formed());

class RecordingLightingSink final : public local_argb::internal::LightingSink,
                                    public vehicle_core::MonotonicClock {
public:
  vehicle_core::MonotonicTimestamp now() const noexcept override { return 0; }
  bool publish(const LightingCommand &command) noexcept override {
    commands.push_back(command);
    return true;
  }

  std::vector<LightingCommand> commands{};
};

struct Controller final {
  Controller() : leds{lighting, lighting}, engine{provider} {}

  test_support::FakeSignalProvider provider{kView};
  RecordingLightingSink lighting{};
  LedActionSink leds;
  ActionEngine engine;
};

persisted::ControllerConfig selected_factory_config() {
  std::ifstream file{CONTROLLER_CONFIG_FACTORY_JSON_PATH, std::ios::binary};
  REQUIRE(file.is_open());
  const std::string json{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
  const auto parsed = persisted::parse_controller_config(json);
  REQUIRE(parsed.ok());
  return *parsed.configuration;
}

} // namespace

TEST_CASE("selected-profile factory JSON parses and applies through the host path") {
  const auto config = selected_factory_config();
  Controller controller{};

  const auto applied = persisted::apply_controller_config(config, controller.leds,
                                                          controller.engine);

  CHECK(applied.ok());
  CHECK(applied.sink_registered);
  CHECK(applied.outputs_applied == config.outputs.size());
  CHECK(applied.rules_applied == config.rules.size());
#if !CONTROLLER_CONFIG_FACTORY_IS_NATIVE
  for (const auto &rule : config.rules) {
    std::visit([](const auto &entry) {
      CHECK(entry.freshness == action_engine::FreshnessRequirement::Fresh);
    }, rule);
  }
#endif
}
