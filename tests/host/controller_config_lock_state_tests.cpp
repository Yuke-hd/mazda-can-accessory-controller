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

#include <cstddef>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// Exercises the reviewed lock-state example (not part of the factory profile)
// through the canonical JSON loader and the persisted application path.

namespace {

namespace persisted = controller_config::persisted;

using action_engine::ActionCommand;
using action_engine::ActionCommandKind;
using action_engine::ActionEngine;
using action_engine::ActionId;
using action_engine::FreshnessRequirement;
using local_argb::internal::LightingCommand;
using local_argb_actions::LedActionSink;
using vehicle_signals::Availability;
using vehicle_signals::SignalCapability;
using vehicle_signals::SignalCatalogView;
using vehicle_signals::SignalId;
using vehicle_signals::SignalMetadata;
using vehicle_signals::SignalNotification;
using vehicle_signals::SignalReading;
using vehicle_signals::SignalType;
using vehicle_signals::SignalUnit;
using vehicle_signals::SignalValue;
using vehicle_signals::ValidationStatus;

constexpr SignalId kDoorsUnlocked{1};
constexpr SignalId kDoorFrontLeft{2};
constexpr SignalMetadata kCatalog[] = {
    {kDoorsUnlocked, "vehicle.doors_unlocked", SignalType::Boolean, SignalUnit::None,
     ValidationStatus::Reference, SignalCapability::Read | SignalCapability::Notify, nullptr, 0},
    {kDoorFrontLeft, "vehicle.door.front_left", SignalType::Boolean, SignalUnit::None,
     ValidationStatus::Reference, SignalCapability::Read | SignalCapability::Notify, nullptr, 0},
};
constexpr SignalCatalogView kView{kCatalog};
static_assert(kView.well_formed());

persisted::ControllerConfig loaded_lock_state_example() {
  std::ifstream file{CONTROLLER_CONFIG_LOCK_STATE_JSON_PATH, std::ios::binary};
  REQUIRE(file.is_open());
  const std::string json{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
  const auto loaded = persisted::parse_controller_config(json);
  REQUIRE(loaded.ok());
  return *loaded.configuration;
}

class NullLightingSink final : public local_argb::internal::LightingSink,
                               public vehicle_core::MonotonicClock {
public:
  vehicle_core::MonotonicTimestamp now() const noexcept override { return 0; }
  bool publish(const LightingCommand &) noexcept override { return true; }
};

// Holds the level each action last received so tests observe actions, not LEDs.
class RecordingActionSink final : public action_engine::ActionSink {
public:
  void execute(const ActionCommand &command) noexcept override {
    if (command.kind == ActionCommandKind::Activate)
      active.push_back(command.action);
    if (command.kind == ActionCommandKind::Deactivate)
      active = without(command.action);
  }

  [[nodiscard]] bool is_active(ActionId action) const noexcept {
    for (const ActionId candidate : active)
      if (candidate == action)
        return true;
    return false;
  }

  std::vector<ActionId> active{};

private:
  [[nodiscard]] std::vector<ActionId> without(ActionId action) const {
    std::vector<ActionId> remaining{};
    for (const ActionId candidate : active)
      if (candidate != action)
        remaining.push_back(candidate);
    return remaining;
  }
};

// Action IDs are assigned from document order starting at 1.
ActionId action_id(const persisted::ControllerConfig &config, std::string_view name) {
  for (std::size_t index = 0; index < config.actions.size(); ++index)
    if (config.actions[index].name == name)
      return ActionId{static_cast<std::uint16_t>(index + 1)};
  FAIL("missing action " << name);
  return ActionId{};
}

struct Controller final {
  Controller() : leds{lighting, lighting}, engine{provider} {}

  void start() {
    REQUIRE(persisted::apply_controller_config(config, leds, engine).ok());
    REQUIRE(engine.add_sink(actions) == action_engine::ConfigStatus::Ok);
    REQUIRE(engine.attach() == vehicle_signals::SignalStatus::Ok);
    provider.start();
  }

  void stop() {
    provider.stop();
    CHECK(engine.detach() == vehicle_signals::SignalStatus::Ok);
  }

  [[nodiscard]] bool unlocked() const {
    return actions.is_active(action_id(config, "doors_unlocked"));
  }
  [[nodiscard]] bool locked() const { return actions.is_active(action_id(config, "doors_locked")); }

  // Owns the configuration the engine's string views refer to.
  persisted::ControllerConfig config{loaded_lock_state_example()};
  test_support::FakeSignalProvider provider{kView};
  NullLightingSink lighting{};
  LedActionSink leds;
  RecordingActionSink actions{};
  ActionEngine engine;
};

SignalNotification doors(const bool unlocked,
                         const Availability availability = Availability::Fresh) {
  return SignalNotification{
      kDoorsUnlocked,
      SignalReading{SignalValue::boolean(unlocked), availability, ValidationStatus::Reference},
      false,
      false,
      false,
      false};
}

SignalNotification front_left_door(const bool open) {
  return SignalNotification{
      kDoorFrontLeft,
      SignalReading{SignalValue::boolean(open), Availability::Fresh, ValidationStatus::Reference},
      false,
      false,
      false,
      false};
}

const persisted::StateRule *rule_for(const persisted::ControllerConfig &config,
                                     std::string_view action) {
  for (const auto &rule : config.rules) {
    const auto *state = std::get_if<persisted::StateRule>(&rule);
    if (state != nullptr && state->action == action)
      return state;
  }
  return nullptr;
}

} // namespace

TEST_CASE("the lock-state example declares both actions from the single unlocked signal") {
  const persisted::ControllerConfig config = loaded_lock_state_example();

  REQUIRE(config.actions.size() == 2);
  CHECK(config.actions[0].name == "doors_unlocked");
  CHECK(config.actions[1].name == "doors_locked");
  REQUIRE(config.rules.size() == 2);
  CHECK(config.outputs.empty());

  const std::pair<std::string_view, bool> expected[] = {{"doors_unlocked", true},
                                                        {"doors_locked", false}};
  for (const auto &[action, value] : expected) {
    CAPTURE(action);
    const auto *rule = rule_for(config, action);
    REQUIRE(rule != nullptr);
    CHECK(rule->condition.signal_key == "vehicle.doors_unlocked");
    CHECK(rule->condition.comparison == action_engine::Comparison::Equal);
    const auto *operand = std::get_if<persisted::BooleanOperand>(&rule->condition.operand);
    REQUIRE(operand != nullptr);
    CHECK(operand->value == value);
    // No lock-state freshness timeout exists; the opt-in is explicit.
    CHECK(rule->freshness == FreshnessRequirement::FreshOrUnverified);
  }
}

TEST_CASE("the lock-state example activates exactly one action per actionable value") {
  Controller controller{};
  controller.start();
  CHECK_FALSE(controller.unlocked());
  CHECK_FALSE(controller.locked());

  REQUIRE(controller.provider.publish(doors(true)) == 1);
  CHECK(controller.unlocked());
  CHECK_FALSE(controller.locked());

  REQUIRE(controller.provider.publish(doors(false)) == 1);
  CHECK_FALSE(controller.unlocked());
  CHECK(controller.locked());

  REQUIRE(controller.provider.publish(doors(true)) == 1);
  CHECK(controller.unlocked());
  CHECK_FALSE(controller.locked());

  controller.stop();
}

TEST_CASE("the lock-state example accepts an unverified observation") {
  for (const bool unlocked : {true, false}) {
    CAPTURE(unlocked);
    Controller controller{};
    controller.start();

    REQUIRE(controller.provider.publish(doors(unlocked, Availability::FreshnessUnverified)) == 1);
    CHECK(controller.unlocked() == unlocked);
    CHECK(controller.locked() == !unlocked);

    controller.stop();
  }
}

TEST_CASE("the lock-state example fails both actions off without a usable observation") {
  for (const auto availability :
       {Availability::NoData, Availability::Stale, Availability::Unavailable}) {
    for (const bool unlocked : {true, false}) {
      CAPTURE(static_cast<int>(availability));
      CAPTURE(unlocked);
      Controller controller{};
      controller.start();
      REQUIRE(controller.provider.publish(doors(unlocked)) == 1);
      REQUIRE(controller.unlocked() != controller.locked());

      REQUIRE(controller.provider.publish(doors(unlocked, availability)) == 1);
      CHECK_FALSE(controller.unlocked());
      CHECK_FALSE(controller.locked());

      controller.stop();
    }
  }
}

TEST_CASE("the lock-state example ignores individual door-open signals") {
  Controller controller{};
  controller.start();

  CHECK(controller.provider.publish(front_left_door(true)) == 0);
  CHECK_FALSE(controller.unlocked());
  CHECK_FALSE(controller.locked());

  controller.stop();
}
