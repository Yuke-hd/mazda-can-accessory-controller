#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "action_engine/action.hpp"
#include "action_engine/engine.hpp"
#include "controller_config/persisted/application.hpp"
#include "controller_config/persisted/json_loader.hpp"
#include "local_argb/lighting_sink.hpp"
#include "local_argb_actions/led_action_sink.hpp"
#include "support/fake_signal_provider.hpp"
#include "vehicle_signals/signal_catalog.hpp"

#include <fstream>
#include <iterator>
#include <string>
#include <variant>
#include <vector>

// Applies the reviewed indicator-lamp example (issue #151) through the
// persisted loader and application path. The example is not part of the
// factory profile and binds no LED output; a recording sink observes the
// generic action commands instead.

namespace {

namespace persisted = controller_config::persisted;

using action_engine::ActionCommand;
using action_engine::ActionCommandKind;
using action_engine::ActionEngine;
using action_engine::ActionId;
using action_engine::ConfigStatus;
using action_engine::FreshnessRequirement;
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
using vehicle_signals::SignalStatus;
using vehicle_signals::SignalType;
using vehicle_signals::SignalUnit;
using vehicle_signals::SignalValue;
using vehicle_signals::ValidationStatus;

constexpr SignalId kTurnState{1};
constexpr SignalId kTurnRequestLeft{2};
constexpr SignalId kTurnRequestRight{3};
constexpr SignalId kIndicatorLampLeft{4};
constexpr SignalId kIndicatorLampRight{5};
constexpr SignalEnumChoice kTurnChoices[] = {{1, "left"}, {2, "right"}, {3, "hazard"}};
constexpr auto kNotifiedBoolean = SignalCapability::Read | SignalCapability::Notify;
// Mirrors the Mazda catalog entries: both lamps are notified booleans.
constexpr SignalMetadata kCatalog[] = {
    {kTurnState, "vehicle.turn_state", SignalType::Enum, SignalUnit::None,
     ValidationStatus::Reference, SignalCapability::Notify, kTurnChoices, 3},
    {kTurnRequestLeft, "vehicle.turn_request.left", SignalType::Boolean, SignalUnit::None,
     ValidationStatus::Reference, kNotifiedBoolean, nullptr, 0},
    {kTurnRequestRight, "vehicle.turn_request.right", SignalType::Boolean, SignalUnit::None,
     ValidationStatus::Reference, kNotifiedBoolean, nullptr, 0},
    {kIndicatorLampLeft, "vehicle.indicator_lamp.left", SignalType::Boolean, SignalUnit::None,
     ValidationStatus::Reference, kNotifiedBoolean, nullptr, 0},
    {kIndicatorLampRight, "vehicle.indicator_lamp.right", SignalType::Boolean, SignalUnit::None,
     ValidationStatus::Reference, kNotifiedBoolean, nullptr, 0},
};
constexpr SignalCatalogView kView{kCatalog};
static_assert(kView.well_formed());

// Action IDs follow document order in the example.
constexpr ActionId kLeftLampAction{1};
constexpr ActionId kRightLampAction{2};

persisted::ControllerConfig loaded_example_config() {
  std::ifstream file{INDICATOR_LAMP_EXAMPLE_JSON_PATH, std::ios::binary};
  REQUIRE(file.is_open());
  const std::string json{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
  const auto loaded = persisted::parse_controller_config(json);
  REQUIRE(loaded.ok());
  return *loaded.configuration;
}

class RecordingActionSink final : public action_engine::ActionSink {
public:
  void execute(const ActionCommand &command) noexcept override { commands.push_back(command); }

  // An action is on after Activate until a later Deactivate; never commanded is off.
  [[nodiscard]] bool active(const ActionId action) const {
    for (auto command = commands.rbegin(); command != commands.rend(); ++command) {
      if (command->action == action)
        return command->kind == ActionCommandKind::Activate;
    }
    return false;
  }

  std::vector<ActionCommand> commands{};
};

class UnusedLightingSink final : public local_argb::internal::LightingSink,
                                 public vehicle_core::MonotonicClock {
public:
  vehicle_core::MonotonicTimestamp now() const noexcept override { return 0; }
  bool publish(const LightingCommand &) noexcept override { return true; }
};

struct Controller final {
  Controller() : config{loaded_example_config()}, leds{lighting, lighting}, engine{provider} {
    REQUIRE(engine.add_sink(actions) == ConfigStatus::Ok);
    REQUIRE(persisted::apply_controller_config(config, leds, engine).ok());
    REQUIRE(engine.attach() == SignalStatus::Ok);
    provider.start();
  }
  ~Controller() {
    provider.stop();
    CHECK(engine.detach() == SignalStatus::Ok);
  }
  Controller(const Controller &) = delete;
  Controller &operator=(const Controller &) = delete;

  persisted::ControllerConfig config;
  test_support::FakeSignalProvider provider{kView};
  UnusedLightingSink lighting{};
  LedActionSink leds;
  RecordingActionSink actions{};
  ActionEngine engine;
};

SignalNotification boolean(const SignalId signal, const bool value,
                           const Availability availability = Availability::Fresh) {
  return SignalNotification{
      signal, SignalReading{SignalValue::boolean(value), availability, ValidationStatus::Reference},
      false,  false,
      false,  false};
}

void check_lamp_rule(const persisted::Rule &rule, const char *action, const char *signal_key) {
  REQUIRE(std::holds_alternative<persisted::StateRule>(rule));
  const auto &state = std::get<persisted::StateRule>(rule);
  CHECK(state.action == action);
  CHECK(state.condition.signal_key == signal_key);
  CHECK(state.condition.comparison == action_engine::Comparison::Equal);
  REQUIRE(std::holds_alternative<persisted::BooleanOperand>(state.condition.operand));
  CHECK(std::get<persisted::BooleanOperand>(state.condition.operand).value);
  CHECK(state.freshness == FreshnessRequirement::FreshOrUnverified);
}

} // namespace

TEST_CASE("indicator-lamp example declares boolean lamp state rules without outputs") {
  const auto config = loaded_example_config();

  REQUIRE(config.actions.size() == 2);
  CHECK(config.actions[0].name == "left_indicator_lamp");
  CHECK(config.actions[1].name == "right_indicator_lamp");
  REQUIRE(config.rules.size() == 2);
  check_lamp_rule(config.rules[0], "left_indicator_lamp", "vehicle.indicator_lamp.left");
  check_lamp_rule(config.rules[1], "right_indicator_lamp", "vehicle.indicator_lamp.right");
  CHECK(config.outputs.empty());
}

TEST_CASE("indicator-lamp example follows the left lamp only") {
  Controller controller{};

  REQUIRE(controller.provider.publish(boolean(kIndicatorLampLeft, true)) == 1);
  CHECK(controller.actions.active(kLeftLampAction));
  CHECK_FALSE(controller.actions.active(kRightLampAction));

  REQUIRE(controller.provider.publish(boolean(kIndicatorLampLeft, false)) == 1);
  CHECK_FALSE(controller.actions.active(kLeftLampAction));
  CHECK_FALSE(controller.actions.active(kRightLampAction));
}

TEST_CASE("indicator-lamp example follows the right lamp only") {
  Controller controller{};

  REQUIRE(controller.provider.publish(boolean(kIndicatorLampRight, true)) == 1);
  CHECK(controller.actions.active(kRightLampAction));
  CHECK_FALSE(controller.actions.active(kLeftLampAction));

  REQUIRE(controller.provider.publish(boolean(kIndicatorLampRight, false)) == 1);
  CHECK_FALSE(controller.actions.active(kRightLampAction));
  CHECK_FALSE(controller.actions.active(kLeftLampAction));
}

TEST_CASE("indicator-lamp example keeps both lamp actions active together") {
  Controller controller{};

  REQUIRE(controller.provider.publish(boolean(kIndicatorLampLeft, true)) == 1);
  REQUIRE(controller.provider.publish(boolean(kIndicatorLampRight, true)) == 1);
  CHECK(controller.actions.active(kLeftLampAction));
  CHECK(controller.actions.active(kRightLampAction));

  REQUIRE(controller.provider.publish(boolean(kIndicatorLampLeft, false)) == 1);
  CHECK_FALSE(controller.actions.active(kLeftLampAction));
  CHECK(controller.actions.active(kRightLampAction));
}

TEST_CASE("indicator-lamp example accepts an unverified lamp observation") {
  Controller controller{};

  REQUIRE(controller.provider.publish(
              boolean(kIndicatorLampLeft, true, Availability::FreshnessUnverified)) == 1);
  REQUIRE(controller.provider.publish(
              boolean(kIndicatorLampRight, true, Availability::FreshnessUnverified)) == 1);
  CHECK(controller.actions.active(kLeftLampAction));
  CHECK(controller.actions.active(kRightLampAction));
}

TEST_CASE("indicator-lamp example fails lamp actions off without a usable observation") {
  for (const auto availability :
       {Availability::NoData, Availability::Stale, Availability::Unavailable}) {
    CAPTURE(static_cast<int>(availability));
    for (const auto signal : {kIndicatorLampLeft, kIndicatorLampRight}) {
      CAPTURE(signal.value());
      const auto action = signal == kIndicatorLampLeft ? kLeftLampAction : kRightLampAction;
      Controller controller{};
      REQUIRE(controller.provider.publish(boolean(signal, true)) == 1);
      REQUIRE(controller.actions.active(action));

      REQUIRE(controller.provider.publish(boolean(signal, true, availability)) == 1);
      CHECK_FALSE(controller.actions.active(action));
    }
  }
}

TEST_CASE("indicator-lamp example ignores turn-state and turn-request signals") {
  Controller controller{};
  const SignalNotification hazard{
      kTurnState,
      SignalReading{SignalValue::enumeration(3), Availability::Fresh, ValidationStatus::Reference},
      false,
      false,
      false,
      false};

  CHECK(controller.provider.publish(hazard) == 0);
  CHECK(controller.provider.publish(boolean(kTurnRequestLeft, true)) == 0);
  CHECK(controller.provider.publish(boolean(kTurnRequestRight, true)) == 0);
  CHECK(controller.actions.commands.empty());
}
