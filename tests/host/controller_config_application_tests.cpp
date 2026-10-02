#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "action_engine/engine.hpp"
#include "board/board_config.h"
#include "controller_config/persisted/application.hpp"
#include "controller_config/persisted/json_loader.hpp"
#include "controller_config/persisted/production_profile.hpp"
#include "local_argb/lighting_sink.hpp"
#include "local_argb_actions/led_action_sink.hpp"
#include "support/fake_signal_provider.hpp"
#include "vehicle_signals/signal_catalog.hpp"

#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace {

namespace persisted = controller_config::persisted;

persisted::ControllerConfig loaded_factory_config() {
  std::ifstream file{CONTROLLER_CONFIG_FACTORY_JSON_PATH, std::ios::binary};
  REQUIRE(file.is_open());
  const std::string json{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
  const auto loaded = persisted::parse_controller_config(json);
  REQUIRE(loaded.ok());
  return *loaded.configuration;
}

void check_condition(const persisted::Condition &actual, const persisted::Condition &expected) {
  CHECK(actual.signal_key == expected.signal_key);
  CHECK(actual.comparison == expected.comparison);
  REQUIRE(actual.operand.index() == expected.operand.index());
  std::visit(
      [&](const auto &operand) {
        using Operand = std::decay_t<decltype(operand)>;
        const auto &reference = std::get<Operand>(expected.operand);
        if constexpr (std::is_same_v<Operand, persisted::ChoiceOperand>)
          CHECK(operand.key == reference.key);
        else
          CHECK(operand.value == reference.value);
      },
      actual.operand);
}

void check_config(const persisted::ControllerConfig &actual,
                  const persisted::ControllerConfig &expected) {
  CHECK(actual.version == expected.version);
  REQUIRE(actual.actions.size() == expected.actions.size());
  for (std::size_t index = 0; index < actual.actions.size(); ++index) {
    CAPTURE(index);
    CHECK(actual.actions[index].name == expected.actions[index].name);
  }
  REQUIRE(actual.rules.size() == expected.rules.size());
  for (std::size_t index = 0; index < actual.rules.size(); ++index) {
    CAPTURE(index);
    REQUIRE(actual.rules[index].index() == expected.rules[index].index());
    std::visit(
        [&](const auto &rule) {
          using Rule = std::decay_t<decltype(rule)>;
          const auto &reference = std::get<Rule>(expected.rules[index]);
          CHECK(rule.action == reference.action);
          CHECK(rule.freshness == reference.freshness);
          if constexpr (std::is_same_v<Rule, persisted::RangeRule>) {
            CHECK(rule.signal_key == reference.signal_key);
            CHECK(rule.input.from == reference.input.from);
            CHECK(rule.input.to == reference.input.to);
            CHECK(rule.output.from == reference.output.from);
            CHECK(rule.output.to == reference.output.to);
          } else {
            check_condition(rule.condition, reference.condition);
            if constexpr (std::is_same_v<Rule, persisted::SampledStateRule>)
              CHECK(rule.release_threshold == reference.release_threshold);
            if constexpr (std::is_same_v<Rule, persisted::EventRule>)
              CHECK(rule.edge == reference.edge);
          }
        },
        actual.rules[index]);
  }
  REQUIRE(actual.outputs.size() == expected.outputs.size());
  for (std::size_t index = 0; index < actual.outputs.size(); ++index) {
    CAPTURE(index);
    REQUIRE(actual.outputs[index].index() == expected.outputs[index].index());
    std::visit(
        [&](const auto &binding) {
          using Binding = std::decay_t<decltype(binding)>;
          const auto &reference = std::get<Binding>(expected.outputs[index]);
          CHECK(binding.action == reference.action);
          CHECK(binding.priority == reference.priority);
          if constexpr (std::is_same_v<Binding, persisted::LedFillBinding>) {
            CHECK(binding.zone.start == reference.zone.start);
            CHECK(binding.zone.length == reference.zone.length);
            CHECK(binding.zone.direction == reference.zone.direction);
            CHECK(binding.color.red == reference.color.red);
            CHECK(binding.color.green == reference.color.green);
            CHECK(binding.color.blue == reference.color.blue);
          } else {
            CHECK(binding.effect == reference.effect);
          }
        },
        actual.outputs[index]);
  }
}

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

class RecordingLightingSink final : public local_argb::internal::LightingSink,
                                    public vehicle_core::MonotonicClock {
public:
  vehicle_core::MonotonicTimestamp now() const noexcept override { return now_us; }
  vehicle_core::MonotonicTimestamp now_us{0};

public:
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

SignalNotification turn(const std::uint16_t value,
                        const Availability availability = Availability::Fresh) {
  return SignalNotification{
      kTurnState,
      SignalReading{SignalValue::enumeration(value), availability, ValidationStatus::Reference},
      false,
      false,
      false,
      false};
}

} // namespace

TEST_CASE("generated factory YAML matches the production profile field by field") {
  check_config(loaded_factory_config(), persisted::production_lighting_config());
}

TEST_CASE("canonical serialization preserves factory configuration field by field") {
  const auto factory = loaded_factory_config();
  const auto json = persisted::serialize_controller_config(factory);
  const auto parsed = persisted::parse_controller_config(json);
  REQUIRE(parsed.ok());
  check_config(*parsed.configuration, factory);
  CHECK(persisted::serialize_controller_config(*parsed.configuration) == json);
}

TEST_CASE("canonical serialization preserves nondefault fields and every model alternative") {
  auto model = persisted::production_lighting_config();
  std::get<persisted::SampledStateRule>(model.rules[4]).release_threshold = 5800.1F;
  model.actions.push_back(persisted::Action{"pulse"});
  model.rules.push_back(
      persisted::EventRule{"pulse",
                           persisted::Condition{"test.boolean", action_engine::Comparison::NotEqual,
                                                persisted::BooleanOperand{true}},
                           action_engine::EventEdge::BecomesFalse,
                           action_engine::FreshnessRequirement::FreshOrUnverified});
  REQUIRE(persisted::validate(model).ok());

  const auto json = persisted::serialize_controller_config(model);
  const auto parsed = persisted::parse_controller_config(json);
  REQUIRE(parsed.ok());
  check_config(*parsed.configuration, model);
  CHECK(persisted::serialize_controller_config(*parsed.configuration) == json);
}

TEST_CASE("generated factory RPM fill covers the board vehicle light strip") {
  const auto config = loaded_factory_config();
  std::size_t rpm_fills = 0;
  for (const auto &output : config.outputs) {
    const auto *fill = std::get_if<persisted::LedFillBinding>(&output);
    if (fill != nullptr && fill->action == "rpm_fill") {
      ++rpm_fills;
      CHECK(fill->zone.start == 0);
      CHECK(fill->zone.length == board::kWeActCan485V11.vehicle_light_strip.pixel_count);
    }
  }
  CHECK(rpm_fills == 1);
}

TEST_CASE("applying generated factory YAML reproduces the controller setup") {
  Controller controller{};

  const auto config = loaded_factory_config();
  const auto status = controller_config::persisted::apply_controller_config(config, controller.leds,
                                                                            controller.engine);

  REQUIRE(status.ok());
  CHECK(status.outputs_applied == config.outputs.size());
  CHECK(status.rules_applied == config.rules.size());
  CHECK(status.sink_registered);
  CHECK(status.binding == local_argb_actions::BindingStatus::Ok);
  CHECK(status.engine == ConfigStatus::Ok);
}

TEST_CASE("generated factory YAML keeps mirrored turn and RPM behavior with fail-off") {
  Controller controller{};
  const auto config = loaded_factory_config();
  REQUIRE(controller_config::persisted::apply_controller_config(config, controller.leds,
                                                                controller.engine)
              .ok());
  REQUIRE(controller.engine.attach() == vehicle_signals::SignalStatus::Ok);
  controller.provider.start();

  REQUIRE(controller.provider.publish(turn(1)) == 1);
  REQUIRE_FALSE(controller.lighting.commands.empty());
  CHECK(controller.lighting.commands.back().right_turn);
  CHECK_FALSE(controller.lighting.commands.back().left_turn);

  REQUIRE(controller.provider.publish(turn(1, Availability::Stale)) == 1);
  CHECK_FALSE(controller.lighting.commands.back().actionable);

  REQUIRE(controller.provider.publish(turn(2)) == 1);
  CHECK(controller.lighting.commands.back().left_turn);
  CHECK_FALSE(controller.lighting.commands.back().right_turn);

  REQUIRE(controller.provider.publish(turn(3)) == 1);
  CHECK(controller.lighting.commands.back().left_turn);
  CHECK(controller.lighting.commands.back().right_turn);

  REQUIRE(controller.provider.publish(turn(1, Availability::FreshnessUnverified)) == 1);
  CHECK_FALSE(controller.lighting.commands.back().actionable);

  const auto sample_rpm = [&](float value) {
    controller.provider.set_reading(kEngineRpm, SignalReading{SignalValue::number(value),
                                                              Availability::FreshnessUnverified,
                                                              ValidationStatus::Reference});
    REQUIRE(controller.engine.sample_polled_rules() == vehicle_signals::SignalStatus::Ok);
  };
  sample_rpm(0.0F);
  CHECK(controller.lighting.commands.back().fills.empty());
  CHECK_FALSE(controller.lighting.commands.back().brake);
  sample_rpm(3250.0F);
  REQUIRE(controller.lighting.commands.back().fills.size() == 1);
  CHECK(controller.lighting.commands.back().fills.begin()->level ==
        local_argb::internal::FillFraction::of(32768, 65536));
  CHECK_FALSE(controller.lighting.commands.back().brake);
  sample_rpm(6000.0F);
  CHECK_FALSE(controller.lighting.commands.back().brake);
  sample_rpm(6001.0F);
  CHECK(controller.lighting.commands.back().brake);
  sample_rpm(6500.0F);
  REQUIRE(controller.lighting.commands.back().fills.size() == 1);
  CHECK(controller.lighting.commands.back().fills.begin()->level ==
        local_argb::internal::FillFraction::full());
  CHECK(controller.lighting.commands.back().brake);

  controller.provider.set_reading(kEngineRpm, SignalReading{});
  REQUIRE(controller.engine.sample_polled_rules() == vehicle_signals::SignalStatus::Ok);
  CHECK(controller.lighting.commands.back().fills.empty());
  CHECK_FALSE(controller.lighting.commands.back().brake);

  controller.provider.stop();
  CHECK(controller.engine.detach() == vehicle_signals::SignalStatus::Ok);
}
