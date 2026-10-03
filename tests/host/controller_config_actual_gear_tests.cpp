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
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// The actual-gear example (docs/specs/configuration/examples/
// actual-gear-actions-v1.*) applied through the persisted configuration path
// against the real Mazda signal catalog. The example is reviewed but not part
// of the factory profile.

namespace {

namespace persisted = controller_config::persisted;

using action_engine::ActionCommandKind;
using action_engine::ActionId;
using vehicle_signals::Availability;
using vehicle_signals::SignalNotification;
using vehicle_signals::SignalReading;
using vehicle_signals::SignalValue;
using vehicle_signals::ValidationStatus;

constexpr std::string_view kSignalKey = "vehicle.actual_gear";
constexpr std::string_view kUnknownChoice = "unknown";

persisted::ControllerConfig loaded_actual_gear_config() {
  std::ifstream file{CONTROLLER_CONFIG_ACTUAL_GEAR_JSON_PATH, std::ios::binary};
  REQUIRE(file.is_open());
  const std::string json{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
  const auto loaded = persisted::parse_controller_config(json);
  REQUIRE(loaded.ok());
  return *loaded.configuration;
}

const vehicle_signals::SignalMetadata &actual_gear_signal() {
  const auto *signal = mazda::internal::signal_catalog().find(kSignalKey);
  REQUIRE(signal != nullptr);
  return *signal;
}

std::uint16_t choice_value(const std::string_view key) {
  const auto &signal = actual_gear_signal();
  for (std::size_t index = 0; index < signal.choice_count; ++index) {
    if (signal.choices[index].key == key)
      return signal.choices[index].value;
  }
  FAIL("no catalog choice " << std::string{key});
  return 0;
}

std::string action_name(const std::string_view choice) { return "gear_" + std::string{choice}; }

// The configuration's 1-based document-order action ID, as application assigns.
std::optional<ActionId> action_id(const persisted::ControllerConfig &config,
                                  const std::string_view name) {
  for (std::size_t index = 0; index < config.actions.size(); ++index) {
    if (config.actions[index].name == name)
      return ActionId{static_cast<std::uint16_t>(index + 1U)};
  }
  return std::nullopt;
}

class NullLightingSink final : public local_argb::internal::LightingSink,
                               public vehicle_core::MonotonicClock {
public:
  vehicle_core::MonotonicTimestamp now() const noexcept override { return 0; }
  bool publish(const local_argb::internal::LightingCommand &) noexcept override { return true; }
};

struct Controller final {
  Controller() : leds{lighting, lighting}, engine{provider} {
    REQUIRE(persisted::apply_controller_config(config, leds, engine).ok());
    REQUIRE(engine.add_sink(actions) == action_engine::ConfigStatus::Ok);
    REQUIRE(engine.attach() == vehicle_signals::SignalStatus::Ok);
    provider.start();
  }
  ~Controller() {
    provider.stop();
    CHECK(engine.detach() == vehicle_signals::SignalStatus::Ok);
  }

  // Publishes one actual-gear observation; returns the actions active after it.
  std::set<std::string> publish(const std::uint16_t value,
                                const Availability availability = Availability::Fresh) {
    const SignalNotification notification{
        actual_gear_signal().id,
        SignalReading{SignalValue::enumeration(value), availability, ValidationStatus::Observed},
        false,
        false,
        false,
        false};
    REQUIRE(provider.publish(notification) == 1);
    for (const auto &command : actions.take()) {
      const auto &name = config.actions[command.action.value() - 1U].name;
      if (command.kind == ActionCommandKind::Activate)
        active.insert(name);
      else if (command.kind == ActionCommandKind::Deactivate)
        active.erase(name);
    }
    return active;
  }

  std::set<std::string> publish(const std::string_view choice,
                                const Availability availability = Availability::Fresh) {
    return publish(choice_value(choice), availability);
  }

  persisted::ControllerConfig config{loaded_actual_gear_config()};
  test_support::FakeSignalProvider provider{mazda::internal::signal_catalog()};
  NullLightingSink lighting{};
  local_argb_actions::LedActionSink leds;
  action_engine::ActionEngine engine;
  test_support::RecordingActionSink actions{};
  std::set<std::string> active{};
};

using Active = std::set<std::string>;

} // namespace

TEST_CASE("the actual-gear example names one action per semantic catalog choice") {
  const auto config = loaded_actual_gear_config();
  const auto &signal = actual_gear_signal();

  std::size_t semantic_choices = 0;
  for (std::size_t index = 0; index < signal.choice_count; ++index) {
    const std::string_view choice = signal.choices[index].key;
    CAPTURE(std::string{choice});
    const auto name = action_name(choice);
    std::size_t actions = 0;
    for (const auto &action : config.actions)
      actions += action.name == name ? 1U : 0U;

    if (choice == kUnknownChoice) {
      CHECK(actions == 0);
      continue;
    }
    ++semantic_choices;
    CHECK(actions == 1);

    std::size_t rules = 0;
    for (const auto &entry : config.rules) {
      const auto *rule = std::get_if<persisted::StateRule>(&entry);
      if (rule == nullptr || rule->action != name)
        continue;
      ++rules;
      CHECK(rule->condition.signal_key == kSignalKey);
      CHECK(rule->condition.comparison == action_engine::Comparison::Equal);
      const auto *operand = std::get_if<persisted::ChoiceOperand>(&rule->condition.operand);
      REQUIRE(operand != nullptr);
      CHECK(operand->key == choice);
      CHECK(rule->freshness == action_engine::FreshnessRequirement::FreshOrUnverified);
    }
    CHECK(rules == 1);
  }

  CHECK(config.actions.size() == semantic_choices);
  CHECK(config.rules.size() == semantic_choices);
  CHECK(config.outputs.empty());
}

TEST_CASE("applying the actual-gear example registers every rule without outputs") {
  const auto config = loaded_actual_gear_config();
  test_support::FakeSignalProvider provider{mazda::internal::signal_catalog()};
  NullLightingSink lighting{};
  local_argb_actions::LedActionSink leds{lighting, lighting};
  action_engine::ActionEngine engine{provider};

  const auto status = persisted::apply_controller_config(config, leds, engine);

  REQUIRE(status.ok());
  CHECK(status.outputs_applied == 0);
  CHECK(status.rules_applied == config.rules.size());
}

TEST_CASE("actual-gear transitions keep exactly the matched gear action active") {
  Controller controller{};

  CHECK(controller.publish("park_or_neutral") == Active{"gear_park_or_neutral"});
  CHECK(controller.publish("first") == Active{"gear_first"});
  CHECK(controller.publish("shifting") == Active{"gear_shifting"});
  CHECK(controller.publish("second") == Active{"gear_second"});
  CHECK(controller.publish("third") == Active{"gear_third"});
  CHECK(controller.publish("fourth") == Active{"gear_fourth"});
  CHECK(controller.publish("fifth") == Active{"gear_fifth"});
  CHECK(controller.publish("sixth") == Active{"gear_sixth"});
  CHECK(controller.publish("reverse") == Active{"gear_reverse"});
  CHECK(controller.publish("neutral") == Active{"gear_neutral"});
  CHECK(controller.publish("park") == Active{"gear_park"});
}

TEST_CASE("an actual-gear change deactivates the previous action and activates the next") {
  Controller controller{};
  REQUIRE(controller.publish("first") == Active{"gear_first"});
  const auto first = action_id(controller.config, "gear_first");
  const auto second = action_id(controller.config, "gear_second");
  REQUIRE(first.has_value());
  REQUIRE(second.has_value());

  REQUIRE(controller.provider.publish(
              SignalNotification{actual_gear_signal().id,
                                 SignalReading{SignalValue::enumeration(choice_value("second")),
                                               Availability::Fresh, ValidationStatus::Observed},
                                 false, false, false, false}) == 1);
  const auto commands = controller.actions.take();

  bool deactivated_first = false;
  bool activated_second = false;
  std::size_t activations = 0;
  for (const auto &command : commands) {
    deactivated_first |= command.action == *first && command.kind == ActionCommandKind::Deactivate;
    activated_second |= command.action == *second && command.kind == ActionCommandKind::Activate;
    activations += command.kind == ActionCommandKind::Activate ? 1U : 0U;
  }
  CHECK(deactivated_first);
  CHECK(activated_second);
  CHECK(activations == 1);
}

TEST_CASE("the actual-gear example accepts an unverified observation by explicit opt-in") {
  Controller controller{};

  CHECK(controller.publish("third", Availability::FreshnessUnverified) == Active{"gear_third"});
  CHECK(controller.publish("reverse", Availability::FreshnessUnverified) == Active{"gear_reverse"});
}

TEST_CASE("the actual-gear example fails off without a usable observation") {
  for (const auto availability :
       {Availability::NoData, Availability::Stale, Availability::Unavailable}) {
    CAPTURE(static_cast<int>(availability));
    Controller controller{};
    REQUIRE(controller.publish("second") == Active{"gear_second"});

    CHECK(controller.publish("second", availability).empty());

    // Recovery requires a newer usable observation.
    CHECK(controller.publish("second") == Active{"gear_second"});
  }
}

TEST_CASE("an unknown actual-gear value maps to no gear action") {
  Controller controller{};
  REQUIRE(controller.publish("fourth") == Active{"gear_fourth"});

  CHECK(controller.publish(kUnknownChoice).empty());
  CHECK(controller.publish(std::uint16_t{0xFFFF}).empty());
}
