#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "action_engine/engine.hpp"
#include "controller_config/persisted/application.hpp"
#include "controller_config/persisted/json_loader.hpp"
#include "local_argb/lighting_sink.hpp"
#include "local_argb_actions/led_action_sink.hpp"
#include "mazda/signal_catalog.hpp"
#include "support/fake_signal_provider.hpp"
#include "support/recording_action_sink.hpp"

#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// The reviewed selector-position example (#147) applied through the
// persisted configuration path against the production Mazda signal catalog.
// The example is not part of the factory profile and binds no LED output, so
// the engine's commands are observed directly.

namespace {

namespace persisted = controller_config::persisted;

using action_engine::ActionCommand;
using action_engine::ActionCommandKind;
using action_engine::ActionEngine;
using action_engine::ActionId;
using action_engine::FreshnessRequirement;
using vehicle_signals::Availability;
using vehicle_signals::SignalMetadata;
using vehicle_signals::SignalNotification;
using vehicle_signals::SignalReading;
using vehicle_signals::SignalValue;
using vehicle_signals::ValidationStatus;

constexpr std::string_view kSelectorKey{"vehicle.selector_position"};

persisted::ControllerConfig loaded_selector_example() {
  std::ifstream file{CONTROLLER_CONFIG_SELECTOR_EXAMPLE_JSON_PATH, std::ios::binary};
  REQUIRE(file.is_open());
  const std::string json{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
  const auto loaded = persisted::parse_controller_config(json);
  REQUIRE(loaded.ok());
  return *loaded.configuration;
}

const SignalMetadata &selector_metadata() {
  const SignalMetadata *selector = nullptr;
  for (const SignalMetadata &signal : mazda::internal::signal_catalog()) {
    if (signal.key == kSelectorKey)
      selector = &signal;
  }
  REQUIRE(selector != nullptr);
  return *selector;
}

// "unknown" is the catalog's non-semantic placeholder; the decoder never
// publishes it as an actionable selector state.
bool semantic(const std::string_view choice) { return choice != "unknown"; }

std::string action_name(const std::string_view choice) { return "selector_" + std::string{choice}; }

// Action IDs follow document order, as apply_controller_config() assigns them.
ActionId action_id(const persisted::ControllerConfig &config, const std::string_view choice) {
  const std::string name = action_name(choice);
  for (std::size_t index = 0; index < config.actions.size(); ++index) {
    if (config.actions[index].name == name)
      return ActionId{static_cast<std::uint16_t>(index + 1U)};
  }
  FAIL("missing action " << name);
  return ActionId{};
}

std::uint16_t choice_value(const std::string_view choice) {
  const auto *entry = selector_metadata().find_choice(choice);
  REQUIRE(entry != nullptr);
  return entry->value;
}

SignalNotification selector(const std::string_view choice,
                            const Availability availability = Availability::Fresh) {
  return SignalNotification{selector_metadata().id,
                            SignalReading{SignalValue::enumeration(choice_value(choice)),
                                          availability, ValidationStatus::Confirmed},
                            false,
                            false,
                            false,
                            false};
}

ActionCommand activate(const ActionId action) { return {action, ActionCommandKind::Activate}; }
ActionCommand deactivate(const ActionId action) { return {action, ActionCommandKind::Deactivate}; }

class NullLightingSink final : public local_argb::internal::LightingSink,
                               public vehicle_core::MonotonicClock {
public:
  vehicle_core::MonotonicTimestamp now() const noexcept override { return 0; }
  bool publish(const local_argb::internal::LightingCommand &) noexcept override { return true; }
};

struct Controller final {
  Controller() : leds{lighting, lighting}, engine{provider} {
    REQUIRE(engine.add_sink(actions) == action_engine::ConfigStatus::Ok);
    REQUIRE(persisted::apply_controller_config(config, leds, engine).ok());
    REQUIRE(engine.attach() == vehicle_signals::SignalStatus::Ok);
    provider.start();
  }
  ~Controller() {
    provider.stop();
    CHECK(engine.detach() == vehicle_signals::SignalStatus::Ok);
  }

  persisted::ControllerConfig config{loaded_selector_example()};
  test_support::FakeSignalProvider provider{mazda::internal::signal_catalog()};
  NullLightingSink lighting{};
  local_argb_actions::LedActionSink leds;
  ActionEngine engine;
  test_support::RecordingActionSink actions{};
};

using Commands = std::vector<ActionCommand>;

// The first observation settles every level rule: the matching action
// activates and every other selector action reports off, in document order.
Commands first_observation(const persisted::ControllerConfig &config,
                           const std::string_view active_choice) {
  Commands commands{};
  for (std::size_t index = 0; index < config.actions.size(); ++index) {
    const ActionId action{static_cast<std::uint16_t>(index + 1U)};
    const bool active = config.actions[index].name == action_name(active_choice);
    commands.push_back(active ? activate(action) : deactivate(action));
  }
  return commands;
}

} // namespace

TEST_CASE("the selector example names one action per semantic catalog choice") {
  const persisted::ControllerConfig config = loaded_selector_example();
  const SignalMetadata &metadata = selector_metadata();

  std::size_t semantic_choices = 0;
  for (std::size_t index = 0; index < metadata.choice_count; ++index) {
    const std::string_view choice = metadata.choices[index].key;
    CAPTURE(choice);
    std::size_t actions = 0;
    std::size_t rules = 0;
    for (const auto &action : config.actions)
      actions += action.name == action_name(choice) ? 1U : 0U;
    for (const auto &entry : config.rules) {
      const auto *rule = std::get_if<persisted::StateRule>(&entry);
      if (rule == nullptr || rule->action != action_name(choice))
        continue;
      ++rules;
      CHECK(rule->condition.signal_key == kSelectorKey);
      CHECK(rule->condition.comparison == action_engine::Comparison::Equal);
      const auto *operand = std::get_if<persisted::ChoiceOperand>(&rule->condition.operand);
      REQUIRE(operand != nullptr);
      CHECK(operand->key == choice);
      // No selector freshness timeout exists; the example opts in explicitly.
      CHECK(rule->freshness == FreshnessRequirement::FreshOrUnverified);
    }
    const std::size_t expected = semantic(choice) ? 1U : 0U;
    CHECK(actions == expected);
    CHECK(rules == expected);
    semantic_choices += expected;
  }
  CHECK(config.actions.size() == semantic_choices);
  CHECK(config.rules.size() == semantic_choices);
  CHECK(config.outputs.empty());
}

TEST_CASE("the selector example follows park, reverse, neutral and drive transitions") {
  Controller controller{};
  const auto id = [&](std::string_view choice) { return action_id(controller.config, choice); };

  REQUIRE(controller.provider.publish(selector("park")) == 1);
  CHECK(controller.actions.take() == first_observation(controller.config, "park"));

  REQUIRE(controller.provider.publish(selector("reverse")) == 1);
  CHECK(controller.actions.take() == Commands{deactivate(id("park")), activate(id("reverse"))});

  REQUIRE(controller.provider.publish(selector("neutral")) == 1);
  CHECK(controller.actions.take() == Commands{deactivate(id("reverse")), activate(id("neutral"))});

  REQUIRE(controller.provider.publish(selector("drive")) == 1);
  CHECK(controller.actions.take() == Commands{deactivate(id("neutral")), activate(id("drive"))});

  // Commands follow rule document order; selector_shifting is the first rule.
  REQUIRE(controller.provider.publish(selector("shifting")) == 1);
  CHECK(controller.actions.take() == Commands{activate(id("shifting")), deactivate(id("drive"))});
}

TEST_CASE("the selector example accepts an unverified observation by explicit opt-in") {
  Controller controller{};

  REQUIRE(controller.provider.publish(selector("reverse", Availability::FreshnessUnverified)) == 1);
  CHECK(controller.actions.take() == first_observation(controller.config, "reverse"));
}

TEST_CASE("the selector example fails off without a usable observation") {
  for (const auto availability :
       {Availability::NoData, Availability::Stale, Availability::Unavailable}) {
    CAPTURE(static_cast<int>(availability));
    Controller controller{};
    const ActionId drive = action_id(controller.config, "drive");
    REQUIRE(controller.provider.publish(selector("drive")) == 1);
    REQUIRE(controller.actions.take() == first_observation(controller.config, "drive"));

    REQUIRE(controller.provider.publish(selector("drive", availability)) == 1);
    CHECK(controller.actions.take() == Commands{deactivate(drive)});
  }
}

TEST_CASE("the selector example does not invent a state for the unknown placeholder") {
  Controller controller{};
  const ActionId park = action_id(controller.config, "park");
  REQUIRE(controller.provider.publish(selector("park")) == 1);
  REQUIRE(controller.actions.take() == first_observation(controller.config, "park"));

  REQUIRE(controller.provider.publish(selector("unknown")) == 1);
  CHECK(controller.actions.take() == Commands{deactivate(park)});
}
