#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

// Door and liftgate open actions (#149): the reviewed example configuration
// declares one action per door/liftgate open signal, each driven by its own
// Boolean state rule. The example is not part of the factory profile; it is
// exercised here through the canonical JSON loader and persisted application
// path with a recording action sink, since it declares no LED outputs.
#include "action_engine/engine.hpp"
#include "controller_config/persisted/application.hpp"
#include "controller_config/persisted/json_loader.hpp"
#include "local_argb/lighting_sink.hpp"
#include "local_argb_actions/led_action_sink.hpp"
#include "support/fake_signal_provider.hpp"
#include "support/recording_action_sink.hpp"
#include "vehicle_signals/signal_catalog.hpp"

#include <array>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

namespace persisted = controller_config::persisted;

using action_engine::ActionCommand;
using action_engine::ActionCommandKind;
using action_engine::ActionEngine;
using action_engine::ActionId;
using action_engine::Comparison;
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
using Commands = std::vector<ActionCommand>;

constexpr auto kReadNotify = SignalCapability::Read | SignalCapability::Notify;
constexpr SignalMetadata kCatalog[] = {
    {SignalId{1}, "vehicle.door.front_left_rhd", SignalType::Boolean, SignalUnit::None,
     ValidationStatus::Reference, kReadNotify, nullptr, 0},
    {SignalId{2}, "vehicle.door.front_right_rhd", SignalType::Boolean, SignalUnit::None,
     ValidationStatus::Reference, kReadNotify, nullptr, 0},
    {SignalId{3}, "vehicle.door.rear_left", SignalType::Boolean, SignalUnit::None,
     ValidationStatus::Reference, kReadNotify, nullptr, 0},
    {SignalId{4}, "vehicle.door.rear_right", SignalType::Boolean, SignalUnit::None,
     ValidationStatus::Reference, kReadNotify, nullptr, 0},
    {SignalId{5}, "vehicle.liftgate_open", SignalType::Boolean, SignalUnit::None,
     ValidationStatus::Reference, kReadNotify, nullptr, 0},
};
constexpr SignalCatalogView kView{kCatalog};
static_assert(kView.well_formed());

// One entry per door/liftgate: its action, its source signal, and the action
// ID assigned from the example's document order.
struct Entry final {
  std::string_view action;
  std::string_view signal_key;
  SignalId signal;
  ActionId id;
};

constexpr std::array<Entry, 5> kEntries{{
    {"door_front_left_open", "vehicle.door.front_left_rhd", SignalId{1}, ActionId{1}},
    {"door_front_right_open", "vehicle.door.front_right_rhd", SignalId{2}, ActionId{2}},
    {"door_rear_left_open", "vehicle.door.rear_left", SignalId{3}, ActionId{3}},
    {"door_rear_right_open", "vehicle.door.rear_right", SignalId{4}, ActionId{4}},
    {"liftgate_open", "vehicle.liftgate_open", SignalId{5}, ActionId{5}},
}};

constexpr const Entry &kFrontLeft = kEntries[0];
constexpr const Entry &kFrontRight = kEntries[1];
constexpr const Entry &kRearLeft = kEntries[2];
constexpr const Entry &kRearRight = kEntries[3];
constexpr const Entry &kLiftgate = kEntries[4];

persisted::ControllerConfig loaded_example_config() {
  std::ifstream file{CONTROLLER_CONFIG_DOOR_EXAMPLE_JSON_PATH, std::ios::binary};
  REQUIRE(file.is_open());
  const std::string json{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
  const auto loaded = persisted::parse_controller_config(json);
  REQUIRE(loaded.ok());
  return *loaded.configuration;
}

const persisted::StateRule *find_rule(const persisted::ControllerConfig &config,
                                      const std::string_view action) {
  for (const auto &rule : config.rules) {
    const auto *state = std::get_if<persisted::StateRule>(&rule);
    if (state != nullptr && state->action == action)
      return state;
  }
  return nullptr;
}

class DiscardingLightingSink final : public local_argb::internal::LightingSink,
                                     public vehicle_core::MonotonicClock {
public:
  vehicle_core::MonotonicTimestamp now() const noexcept override { return 0; }
  bool publish(const LightingCommand &) noexcept override { return true; }
};

// The example applied through the persisted path, with a recording sink
// registered beside the LED sink to observe the generic action commands.
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
  Controller(const Controller &) = delete;
  Controller &operator=(const Controller &) = delete;

  // Publishes one observation and returns the commands it emitted.
  Commands publish(const Entry &entry, const bool open,
                   const Availability availability = Availability::Fresh) {
    REQUIRE(
        provider.publish(SignalNotification{
            entry.signal,
            SignalReading{SignalValue::boolean(open), availability, ValidationStatus::Reference},
            false, false, false, false}) == 1);
    return actions.take();
  }

  const persisted::ControllerConfig config{loaded_example_config()};
  test_support::FakeSignalProvider provider{kView};
  DiscardingLightingSink lighting{};
  LedActionSink leds;
  ActionEngine engine;
  test_support::RecordingActionSink actions{};
};

ActionCommand activate(const Entry &entry) {
  return ActionCommand{entry.id, ActionCommandKind::Activate, 0.0F};
}
ActionCommand deactivate(const Entry &entry) {
  return ActionCommand{entry.id, ActionCommandKind::Deactivate, 0.0F};
}

} // namespace

TEST_CASE("the door example declares one open action per door and the liftgate") {
  const auto config = loaded_example_config();

  REQUIRE(config.actions.size() == kEntries.size());
  for (std::size_t index = 0; index < kEntries.size(); ++index) {
    CAPTURE(index);
    CHECK(config.actions[index].name == kEntries[index].action);
  }
  CHECK(config.rules.size() == kEntries.size());
  CHECK(config.outputs.empty());
}

TEST_CASE("each door example rule opens on its own signal and accepts unverified freshness") {
  const auto config = loaded_example_config();

  for (const Entry &entry : kEntries) {
    CAPTURE(entry.action);
    const auto *rule = find_rule(config, entry.action);
    REQUIRE(rule != nullptr);
    CHECK(rule->condition.signal_key == entry.signal_key);
    CHECK(rule->condition.comparison == Comparison::Equal);
    const auto *open = std::get_if<persisted::BooleanOperand>(&rule->condition.operand);
    REQUIRE(open != nullptr);
    CHECK(open->value);
    // No door/liftgate freshness timeout exists; the explicit opt-in accepts
    // an unverified observation without promoting it to Fresh.
    CHECK(rule->freshness == FreshnessRequirement::FreshOrUnverified);
  }
}

TEST_CASE("each door or liftgate controls only its own action") {
  for (const Entry &entry : kEntries) {
    CAPTURE(entry.action);
    Controller controller{};

    CHECK(controller.publish(entry, false) == Commands{deactivate(entry)});
    CHECK(controller.publish(entry, true) == Commands{activate(entry)});
    CHECK(controller.publish(entry, true) == Commands{});
    CHECK(controller.publish(entry, false) == Commands{deactivate(entry)});
  }
}

TEST_CASE("an unverified open observation activates its door action") {
  for (const Entry &entry : kEntries) {
    CAPTURE(entry.action);
    Controller controller{};

    CHECK(controller.publish(entry, true, Availability::FreshnessUnverified) ==
          Commands{activate(entry)});
    CHECK(controller.publish(entry, false, Availability::FreshnessUnverified) ==
          Commands{deactivate(entry)});
  }
}

TEST_CASE("simultaneously open doors stay active until each one closes") {
  Controller controller{};

  CHECK(controller.publish(kFrontLeft, true) == Commands{activate(kFrontLeft)});
  CHECK(controller.publish(kRearRight, true) == Commands{activate(kRearRight)});
  CHECK(controller.publish(kLiftgate, true) == Commands{activate(kLiftgate)});
  CHECK(controller.publish(kFrontRight, true) == Commands{activate(kFrontRight)});
  CHECK(controller.publish(kRearLeft, true) == Commands{activate(kRearLeft)});

  // Closing one entry deactivates only that entry's action.
  CHECK(controller.publish(kRearRight, false) == Commands{deactivate(kRearRight)});
  CHECK(controller.publish(kFrontLeft, true) == Commands{});
  CHECK(controller.publish(kLiftgate, true) == Commands{});

  CHECK(controller.publish(kFrontLeft, false) == Commands{deactivate(kFrontLeft)});
  CHECK(controller.publish(kLiftgate, true) == Commands{});
  CHECK(controller.publish(kRearRight, true) == Commands{activate(kRearRight)});
}

TEST_CASE("a lost door observation fails only its own action off") {
  for (const auto availability :
       {Availability::NoData, Availability::Stale, Availability::Unavailable}) {
    for (const Entry &lost : kEntries) {
      CAPTURE(static_cast<int>(availability));
      CAPTURE(lost.action);
      Controller controller{};
      for (const Entry &entry : kEntries)
        REQUIRE(controller.publish(entry, true) == Commands{activate(entry)});

      CHECK(controller.publish(lost, true, availability) == Commands{deactivate(lost)});

      // The others remain active and keep tracking their own signals.
      for (const Entry &entry : kEntries) {
        if (entry.id == lost.id)
          continue;
        CHECK(controller.publish(entry, true) == Commands{});
      }
      // A newer valid observation recovers the lost action.
      CHECK(controller.publish(lost, true) == Commands{activate(lost)});
    }
  }
}
