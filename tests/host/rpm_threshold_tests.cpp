#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

// RPM threshold action (#33): the controller configuration turns "engine
// speed above a configurable threshold" into an ordinary Activate/Deactivate
// action through a sampled state rule. The action is adapter-independent: it
// is checked here on a recording sink and, bound to a high-priority LED
// effect, next to the #31 RPM level fill. The catalog is make-independent; it
// only names the "vehicle.engine_rpm" key the Mazda provider exposes,
// Read-only and always FreshnessUnverified.
#include "action_engine/engine.hpp"
#include "controller_config/rpm_level_fill.hpp"
#include "controller_config/rpm_threshold.hpp"
#include "local_argb/lighting_sink.hpp"
#include "local_argb_actions/led_action_sink.hpp"
#include "support/fake_signal_provider.hpp"
#include "support/recording_action_sink.hpp"
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
using controller_config::RpmLevelFillConfig;
using controller_config::RpmRange;
using controller_config::RpmThresholdConfig;
using local_argb::internal::EffectPriority;
using local_argb::internal::FillDirection;
using local_argb::internal::FillFraction;
using local_argb::internal::LedZone;
using local_argb::internal::LightingCommand;
using local_argb::internal::LightingRgb;
using local_argb_actions::BindingStatus;
using local_argb_actions::FillEffect;
using local_argb_actions::LedActionSink;
using local_argb_actions::LedEffect;
using test_support::FakeSignalProvider;
using test_support::RecordingActionSink;
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
using Commands = std::vector<ActionCommand>;

constexpr SignalId kEngineRpm{7};
constexpr SignalMetadata kCatalog[] = {
    {kEngineRpm, "vehicle.engine_rpm", SignalType::Number, SignalUnit::RevolutionsPerMinute,
     ValidationStatus::Reference, SignalCapability::Read, nullptr, 0},
};
constexpr vehicle_signals::SignalCatalogView kView{kCatalog};
static_assert(kView.well_formed());

constexpr ActionId kRedZoneAction{5};

ActionCommand activate() { return ActionCommand{kRedZoneAction, ActionCommandKind::Activate}; }
ActionCommand deactivate() { return ActionCommand{kRedZoneAction, ActionCommandKind::Deactivate}; }

RpmThresholdConfig red_zone(const float threshold_rpm) {
  RpmThresholdConfig config{};
  config.threshold_rpm = threshold_rpm;
  config.action = kRedZoneAction;
  return config;
}

// Reports `rpm` as the Mazda provider does: FreshnessUnverified.
void report(FakeSignalProvider &provider, const float rpm) {
  provider.set_reading(kEngineRpm,
                       SignalReading{SignalValue::number(rpm), Availability::FreshnessUnverified,
                                     ValidationStatus::Reference});
}

// The threshold action on a provider and engine, observed through a
// make- and LED-independent recording sink.
class ThresholdController final {
public:
  explicit ThresholdController(const RpmThresholdConfig &config) {
    REQUIRE(engine.add_sink(actions) == ConfigStatus::Ok);
    REQUIRE(controller_config::apply(config, engine) == ConfigStatus::Ok);
    REQUIRE(engine.attach() == SignalStatus::Ok);
    provider.start();
  }
  ~ThresholdController() {
    provider.stop();
    CHECK(engine.detach() == SignalStatus::Ok);
  }
  ThresholdController(const ThresholdController &) = delete;
  ThresholdController &operator=(const ThresholdController &) = delete;

  // Samples `rpm` and returns the commands that sample emitted.
  Commands sample(const float rpm) {
    report(provider, rpm);
    REQUIRE(engine.sample_polled_rules() == SignalStatus::Ok);
    return actions.take();
  }

  FakeSignalProvider provider{kView};
  RecordingActionSink actions{};
  ActionEngine engine{provider};
};

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
  CHECK(config.threshold_rpm == 6000.0F);
}

TEST_CASE("the RPM threshold rule samples vehicle.engine_rpm > threshold, accepting unverified "
          "freshness") {
  const auto rule = controller_config::sampled_state_rule(red_zone(5500.0F));
  CHECK(rule.condition.signal_key == std::string_view{"vehicle.engine_rpm"});
  CHECK(rule.condition.comparison == Comparison::Greater);
  CHECK(rule.condition.operand.as_number() == 5500.0F);
  CHECK(rule.action == kRedZoneAction);
  CHECK(rule.freshness == FreshnessRequirement::FreshOrUnverified);
}

TEST_CASE("RPM below the threshold is an inactive action") {
  ThresholdController controller{red_zone(6000.0F)};
  CHECK(controller.sample(3000.0F) == Commands{deactivate()});
  CHECK(controller.sample(5999.0F).empty());
}

TEST_CASE("RPM at the threshold is still inactive; the condition is strictly above") {
  ThresholdController controller{red_zone(6000.0F)};
  CHECK(controller.sample(6000.0F) == Commands{deactivate()});
}

TEST_CASE("crossing above the threshold activates once and crossing back below deactivates") {
  ThresholdController controller{red_zone(6000.0F)};
  CHECK(controller.sample(5000.0F) == Commands{deactivate()});
  CHECK(controller.sample(6100.0F) == Commands{activate()});
  CHECK(controller.sample(6500.0F).empty());
  CHECK(controller.sample(7200.0F).empty());
  CHECK(controller.sample(6000.0F) == Commands{deactivate()});
  CHECK(controller.sample(4000.0F).empty());
  CHECK(controller.sample(6001.0F) == Commands{activate()});
}

TEST_CASE("a configured threshold moves the activation point") {
  ThresholdController controller{red_zone(4500.0F)};
  CHECK(controller.sample(4500.0F) == Commands{deactivate()});
  CHECK(controller.sample(4600.0F) == Commands{activate()});
  CHECK(controller.sample(4400.0F) == Commands{deactivate()});
}

TEST_CASE("losing the RPM reading while above the threshold deactivates") {
  ThresholdController controller{red_zone(6000.0F)};
  CHECK(controller.sample(6500.0F) == Commands{activate()});
  controller.provider.set_reading(kEngineRpm, SignalReading{});
  REQUIRE(controller.engine.sample_polled_rules() == SignalStatus::Ok);
  CHECK(controller.actions.take() == Commands{deactivate()});
}

TEST_CASE("a non-finite RPM threshold is rejected before the engine attaches") {
  for (const float threshold :
       {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
    FakeSignalProvider provider{kView};
    ActionEngine engine{provider};
    CHECK(controller_config::apply(red_zone(threshold), engine) == ConfigStatus::InvalidOperand);
  }
}

TEST_CASE("the threshold action drives a high-priority LED effect independently of the RPM "
          "level fill") {
  constexpr ActionId kFillAction{4};
  constexpr LedZone kFillZone{0, 100, FillDirection::CenterOut};
  constexpr EffectPriority kWarningPriority{200};
  RpmLevelFillConfig fill{};
  fill.range = RpmRange{0.0F, 6500.0F};
  fill.action = kFillAction;
  fill.fill = FillEffect{kFillZone, LightingRgb{0, 16, 32}, EffectPriority{50}};

  FakeSignalProvider provider{kView};
  RecordingLightingSink lighting{};
  LedActionSink leds{lighting};
  ActionEngine engine{provider};
  REQUIRE(engine.add_sink(leds) == ConfigStatus::Ok);
  REQUIRE(controller_config::apply(fill, leds, engine).ok());
  REQUIRE(leds.bind(kRedZoneAction, LedEffect::Brake, kWarningPriority) == BindingStatus::Ok);
  REQUIRE(controller_config::apply(red_zone(6000.0F), engine) == ConfigStatus::Ok);
  REQUIRE(engine.attach() == SignalStatus::Ok);
  provider.start();

  // Returns the strip state published last by this sample.
  const auto sample = [&](const float rpm) {
    report(provider, rpm);
    lighting.commands.clear();
    REQUIRE(engine.sample_polled_rules() == SignalStatus::Ok);
    REQUIRE_FALSE(lighting.commands.empty());
    return lighting.commands.back();
  };

  const LightingCommand below = sample(3250.0F);
  CHECK_FALSE(below.brake);
  REQUIRE(below.fills.size() == 1);
  CHECK(below.fills.begin()->level == FillFraction::of(32768, 65536));

  const LightingCommand above = sample(6500.0F);
  CHECK(above.brake);
  CHECK(above.priorities.brake == kWarningPriority);
  REQUIRE(above.fills.size() == 1);
  CHECK(above.fills.begin()->level == FillFraction::full());

  const LightingCommand back_below = sample(3250.0F);
  CHECK_FALSE(back_below.brake);
  REQUIRE(back_below.fills.size() == 1);
  CHECK(back_below.fills.begin()->level == FillFraction::of(32768, 65536));

  provider.stop();
  CHECK(engine.detach() == SignalStatus::Ok);
}
