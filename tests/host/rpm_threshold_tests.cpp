#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

// RPM threshold action (#33): the controller configuration turns a configurable
// RPM threshold into a sampled state rule, "vehicle.engine_rpm Greater
// <threshold>", that drives a generic Activate/Deactivate action. The action is
// bound independently of the RPM level fill, here to the local LED warning
// effect. The catalog is make-independent; it only names the
// "vehicle.engine_rpm" key the Mazda provider exposes, Read-only and always
// FreshnessUnverified.
#include "action_engine/engine.hpp"
#include "controller_config/rpm_level_fill.hpp"
#include "controller_config/rpm_threshold.hpp"
#include "local_argb/lighting_sink.hpp"
#include "local_argb_actions/led_action_sink.hpp"
#include "support/fake_signal_provider.hpp"
#include "vehicle_signals/signal_catalog.hpp"
#include "vehicle_signals/signal_contracts.hpp"

#include <limits>
#include <string_view>
#include <vector>

namespace {

using action_engine::ActionCommand;
using action_engine::ActionCommandKind;
using action_engine::ActionEngine;
using action_engine::ActionId;
using action_engine::Comparison;
using action_engine::ConfigStatus;
using action_engine::FreshnessRequirement;
using controller_config::RpmThreshold;
using controller_config::RpmThresholdConfig;
using controller_config::RpmThresholdStatus;
using local_argb::internal::EffectPriority;
using local_argb::internal::FillDirection;
using local_argb::internal::LedZone;
using local_argb::internal::LightingCommand;
using local_argb::internal::LightingRgb;
using local_argb_actions::BindingStatus;
using local_argb_actions::FillEffect;
using local_argb_actions::LedActionSink;
using local_argb_actions::LedEffect;
using vehicle_signals::Availability;
using vehicle_signals::SignalCapability;
using vehicle_signals::SignalId;
using vehicle_signals::SignalMetadata;
using vehicle_signals::SignalReading;
using vehicle_signals::SignalStatus;
using vehicle_signals::SignalType;
using vehicle_signals::SignalUnit;
using vehicle_signals::SignalValue;
using vehicle_signals::ValidationStatus;

constexpr SignalId kEngineRpm{7};
constexpr SignalMetadata kCatalog[] = {
    {kEngineRpm, "vehicle.engine_rpm", SignalType::Number, SignalUnit::RevolutionsPerMinute,
     ValidationStatus::Reference, SignalCapability::Read, nullptr, 0},
};
constexpr vehicle_signals::SignalCatalogView kView{kCatalog};
static_assert(kView.well_formed());

constexpr ActionId kRedZoneAction{5};
constexpr ActionId kRpmLevelAction{4};
constexpr EffectPriority kWarningPriority{150};

constexpr ActionCommand kActivate{kRedZoneAction, ActionCommandKind::Activate, 0.0F};
constexpr ActionCommand kDeactivate{kRedZoneAction, ActionCommandKind::Deactivate, 0.0F};

RpmThresholdConfig red_zone(const RpmThreshold threshold) {
  RpmThresholdConfig config{};
  config.threshold = threshold;
  config.action = kRedZoneAction;
  config.effect = LedEffect::Brake;
  config.priority = kWarningPriority;
  return config;
}

SignalReading unverified_rpm(const float rpm) {
  return SignalReading{SignalValue::number(rpm), Availability::FreshnessUnverified,
                       ValidationStatus::Reference};
}

// Any output adapter: records every command the engine sends.
class RecordingActionSink final : public action_engine::ActionSink {
public:
  void execute(const ActionCommand &command) noexcept override { commands.push_back(command); }

  std::vector<ActionCommand> commands{};
};

// The threshold rule added straight to the engine, with no LED binding, so
// the action reaches whichever adapter the composition root registers.
class ThresholdController final {
public:
  explicit ThresholdController(const RpmThreshold threshold) {
    REQUIRE(engine.add_sink(sink) == ConfigStatus::Ok);
    REQUIRE(engine.add_sampled_state_rule(controller_config::threshold_rule(red_zone(threshold))) ==
            ConfigStatus::Ok);
    REQUIRE(engine.attach() == SignalStatus::Ok);
    provider.start();
  }
  ~ThresholdController() {
    provider.stop();
    CHECK(engine.detach() == SignalStatus::Ok);
  }
  ThresholdController(const ThresholdController &) = delete;
  ThresholdController &operator=(const ThresholdController &) = delete;

  // Samples `rpm` as the Mazda provider reports it and returns the commands
  // that sample emitted.
  std::vector<ActionCommand> sample(const float rpm) {
    provider.set_reading(kEngineRpm, unverified_rpm(rpm));
    sink.commands.clear();
    REQUIRE(engine.sample_polled_rules() == SignalStatus::Ok);
    return sink.commands;
  }

  test_support::FakeSignalProvider provider{kView};
  RecordingActionSink sink{};
  ActionEngine engine{provider};
};

using Commands = std::vector<ActionCommand>;

class RecordingLightingSink final : public local_argb::internal::LightingSink {
public:
  bool publish(const LightingCommand &command) noexcept override {
    commands.push_back(command);
    return true;
  }

  std::vector<LightingCommand> commands{};
};

} // namespace

TEST_CASE("the default RPM threshold is 6000 rpm") {
  const RpmThresholdConfig config{};
  CHECK(config.threshold.rpm == 6000.0F);
}

TEST_CASE("the threshold rule is vehicle.engine_rpm Greater threshold, accepting unverified "
          "freshness") {
  const auto rule = controller_config::threshold_rule(red_zone(RpmThreshold{5500.0F}));
  CHECK(rule.condition.signal_key == std::string_view{"vehicle.engine_rpm"});
  CHECK(rule.condition.comparison == Comparison::Greater);
  CHECK(rule.condition.operand.as_number() == 5500.0F);
  CHECK(rule.action == kRedZoneAction);
  CHECK(rule.freshness == FreshnessRequirement::FreshOrUnverified);
}

TEST_CASE("RPM below the threshold keeps the action inactive") {
  ThresholdController controller{RpmThreshold{}};
  CHECK(controller.sample(3000.0F) == Commands{kDeactivate});
  CHECK(controller.sample(5999.0F).empty());
}

TEST_CASE("RPM at the threshold is not above it") {
  ThresholdController controller{RpmThreshold{}};
  CHECK(controller.sample(6000.0F) == Commands{kDeactivate});
}

TEST_CASE("crossing above the threshold emits Activate once while RPM stays above it") {
  ThresholdController controller{RpmThreshold{}};
  CHECK(controller.sample(5000.0F) == Commands{kDeactivate});
  CHECK(controller.sample(6001.0F) == Commands{kActivate});
  CHECK(controller.sample(6500.0F).empty());
  CHECK(controller.sample(7200.0F).empty());
}

TEST_CASE("crossing back below the threshold emits Deactivate") {
  ThresholdController controller{RpmThreshold{}};
  CHECK(controller.sample(6500.0F) == Commands{kActivate});
  CHECK(controller.sample(5800.0F) == Commands{kDeactivate});
  CHECK(controller.sample(4000.0F).empty());
}

TEST_CASE("a configured threshold moves the switching point") {
  ThresholdController controller{RpmThreshold{4500.0F}};
  CHECK(controller.sample(4500.0F) == Commands{kDeactivate});
  CHECK(controller.sample(4600.0F) == Commands{kActivate});
  CHECK(controller.sample(4400.0F) == Commands{kDeactivate});
}

TEST_CASE("losing the RPM reading deactivates an active threshold action") {
  ThresholdController controller{RpmThreshold{}};
  CHECK(controller.sample(6500.0F) == Commands{kActivate});
  controller.provider.set_reading(kEngineRpm, SignalReading{});
  controller.sink.commands.clear();
  REQUIRE(controller.engine.sample_polled_rules() == SignalStatus::Ok);
  CHECK(controller.sink.commands == Commands{kDeactivate});
}

TEST_CASE("a non-finite threshold is rejected") {
  test_support::FakeSignalProvider provider{kView};
  ActionEngine engine{provider};
  const float threshold = std::numeric_limits<float>::quiet_NaN();
  CHECK(engine.add_sampled_state_rule(controller_config::threshold_rule(
            red_zone(RpmThreshold{threshold}))) == ConfigStatus::InvalidOperand);
}

TEST_CASE("apply binds the red zone to the LED warning effect, independently of the RPM level "
          "fill") {
  RecordingLightingSink lighting{};
  LedActionSink leds{lighting};
  test_support::FakeSignalProvider provider{kView};
  ActionEngine engine{provider};
  REQUIRE(engine.add_sink(leds) == ConfigStatus::Ok);
  controller_config::RpmLevelFillConfig level_fill{};
  level_fill.action = kRpmLevelAction;
  level_fill.fill =
      FillEffect{LedZone{0, 100, FillDirection::CenterOut}, LightingRgb{0, 16, 32}, {}};
  REQUIRE(controller_config::apply(level_fill, leds, engine).ok());
  const RpmThresholdStatus status =
      controller_config::apply(red_zone(RpmThreshold{}), leds, engine);
  REQUIRE(status.ok());
  REQUIRE(engine.attach() == SignalStatus::Ok);
  provider.start();

  provider.set_reading(kEngineRpm, unverified_rpm(3250.0F));
  REQUIRE(engine.sample_polled_rules() == SignalStatus::Ok);
  REQUIRE_FALSE(lighting.commands.empty());
  CHECK_FALSE(lighting.commands.back().brake);
  CHECK(lighting.commands.back().fills.size() == 1);

  provider.set_reading(kEngineRpm, unverified_rpm(6500.0F));
  REQUIRE(engine.sample_polled_rules() == SignalStatus::Ok);
  CHECK(lighting.commands.back().brake);
  CHECK(lighting.commands.back().priorities.brake == kWarningPriority);
  CHECK(lighting.commands.back().fills.size() == 1);

  provider.set_reading(kEngineRpm, unverified_rpm(5000.0F));
  REQUIRE(engine.sample_polled_rules() == SignalStatus::Ok);
  CHECK_FALSE(lighting.commands.back().brake);
  CHECK(lighting.commands.back().fills.size() == 1);

  provider.stop();
  CHECK(engine.detach() == SignalStatus::Ok);
}

TEST_CASE("the red zone cannot share the RPM level fill's action") {
  RecordingLightingSink lighting{};
  LedActionSink leds{lighting};
  test_support::FakeSignalProvider provider{kView};
  ActionEngine engine{provider};
  controller_config::RpmLevelFillConfig level_fill{};
  level_fill.action = kRedZoneAction;
  level_fill.fill =
      FillEffect{LedZone{0, 100, FillDirection::CenterOut}, LightingRgb{0, 16, 32}, {}};
  REQUIRE(controller_config::apply(level_fill, leds, engine).ok());
  const RpmThresholdStatus status =
      controller_config::apply(red_zone(RpmThreshold{}), leds, engine);
  CHECK_FALSE(status.ok());
  CHECK(status.binding == BindingStatus::Ok);
  CHECK(status.rule == ConfigStatus::DuplicateAction);
}

TEST_CASE("a failed warning binding adds no threshold rule") {
  RecordingLightingSink lighting{};
  LedActionSink leds{lighting};
  test_support::FakeSignalProvider provider{kView};
  ActionEngine engine{provider};
  REQUIRE(engine.add_sink(leds) == ConfigStatus::Ok);
  RpmThresholdConfig config = red_zone(RpmThreshold{});
  config.action = ActionId{};
  const RpmThresholdStatus status = controller_config::apply(config, leds, engine);
  CHECK_FALSE(status.ok());
  CHECK(status.binding == BindingStatus::InvalidAction);
  CHECK_FALSE(status.rule.has_value());
  REQUIRE(engine.attach() == SignalStatus::Ok);
  provider.start();
  provider.set_reading(kEngineRpm, unverified_rpm(6500.0F));
  REQUIRE(engine.sample_polled_rules() == SignalStatus::Ok);
  CHECK(lighting.commands.empty());
  provider.stop();
  CHECK(engine.detach() == SignalStatus::Ok);
}
