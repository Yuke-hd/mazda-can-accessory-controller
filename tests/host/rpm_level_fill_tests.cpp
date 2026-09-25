#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

// RPM level fill (#31): the controller configuration maps a configurable RPM
// range onto a 0.0..1.0 SetLevel and binds it to a local LED fill. The catalog
// is make-independent; it only names the "vehicle.engine_rpm" key the Mazda
// provider exposes, Read-only and always FreshnessUnverified.
#include "action_engine/engine.hpp"
#include "controller_config/rpm_level_fill.hpp"
#include "local_argb/lighting_sink.hpp"
#include "local_argb_actions/led_action_sink.hpp"
#include "support/fake_signal_provider.hpp"
#include "vehicle_signals/signal_catalog.hpp"
#include "vehicle_signals/signal_contracts.hpp"

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace {

using action_engine::ActionEngine;
using action_engine::ActionId;
using action_engine::ConfigStatus;
using action_engine::FreshnessRequirement;
using controller_config::RpmLevelFillConfig;
using controller_config::RpmLevelFillStatus;
using controller_config::RpmRange;
using local_argb::internal::FillDirection;
using local_argb::internal::FillFraction;
using local_argb::internal::LedZone;
using local_argb::internal::LightingCommand;
using local_argb::internal::LightingRgb;
using local_argb_actions::BindingStatus;
using local_argb_actions::FillEffect;
using local_argb_actions::LedActionSink;
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

constexpr ActionId kRpmAction{4};
constexpr LedZone kRpmZone{0, 100, FillDirection::CenterOut};
constexpr LightingRgb kRpmColor{0, 16, 32};

class RecordingLightingSink final : public local_argb::internal::LightingSink {
public:
  bool publish(const LightingCommand &command) noexcept override {
    commands.push_back(command);
    return true;
  }

  std::vector<LightingCommand> commands{};
};

RpmLevelFillConfig config_with(const RpmRange range) {
  RpmLevelFillConfig config{};
  config.range = range;
  config.action = kRpmAction;
  config.fill = FillEffect{kRpmZone, kRpmColor, {}};
  return config;
}

// Controller, provider, engine and LED sink wired as the firmware wires them.
class Controller final {
public:
  explicit Controller(const RpmLevelFillConfig &config) {
    REQUIRE(engine.add_sink(leds) == ConfigStatus::Ok);
    REQUIRE(controller_config::apply(config, leds, engine).ok());
    REQUIRE(engine.attach() == SignalStatus::Ok);
    provider.start();
  }
  ~Controller() {
    provider.stop();
    CHECK(engine.detach() == SignalStatus::Ok);
  }
  Controller(const Controller &) = delete;
  Controller &operator=(const Controller &) = delete;

  // Samples `rpm` as the Mazda provider reports it and returns the fill level
  // the LED sink published, or nothing when the sample published nothing.
  std::optional<FillFraction> sample(const float rpm) {
    provider.set_reading(kEngineRpm,
                         SignalReading{SignalValue::number(rpm), Availability::FreshnessUnverified,
                                       ValidationStatus::Reference});
    lighting.commands.clear();
    REQUIRE(engine.sample_polled_rules() == SignalStatus::Ok);
    if (lighting.commands.empty())
      return std::nullopt;
    const auto &fills = lighting.commands.back().fills;
    if (fills.empty())
      return FillFraction::empty();
    REQUIRE(fills.size() == 1);
    CHECK(fills.begin()->zone == kRpmZone);
    return fills.begin()->level;
  }

  test_support::FakeSignalProvider provider{kView};
  RecordingLightingSink lighting{};
  LedActionSink leds{lighting};
  ActionEngine engine{provider};
};

FillFraction level(const double fraction) {
  return FillFraction::of(static_cast<std::uint32_t>(fraction * 65536.0 + 0.5), 65536);
}

} // namespace

TEST_CASE("the default RPM range is 0..6500 rpm") {
  const RpmLevelFillConfig config{};
  CHECK(config.range.min_rpm == 0.0F);
  CHECK(config.range.max_rpm == 6500.0F);
}

TEST_CASE("the RPM range rule samples vehicle.engine_rpm into 0..1, accepting unverified "
          "freshness") {
  const auto rule = controller_config::range_rule(config_with(RpmRange{1000.0F, 5000.0F}));
  CHECK(rule.signal_key == std::string_view{"vehicle.engine_rpm"});
  CHECK(rule.input.from == 1000.0F);
  CHECK(rule.input.to == 5000.0F);
  CHECK(rule.output.from == 0.0F);
  CHECK(rule.output.to == 1.0F);
  CHECK(rule.action == kRpmAction);
  CHECK(rule.freshness == FreshnessRequirement::FreshOrUnverified);
}

TEST_CASE("with the default range, 0 rpm is an empty fill and 6500 rpm a full fill") {
  Controller controller{config_with(RpmRange{})};
  CHECK(controller.sample(0.0F) == FillFraction::empty());
  CHECK(controller.sample(6500.0F) == FillFraction::full());
}

TEST_CASE("RPM between the bounds fills linearly, and RPM beyond them clamps") {
  Controller controller{config_with(RpmRange{})};
  CHECK(controller.sample(3250.0F) == level(0.5));
  CHECK(controller.sample(1625.0F) == level(0.25));
  CHECK(controller.sample(9000.0F) == FillFraction::full());
  CHECK(controller.sample(-100.0F) == FillFraction::empty());
}

TEST_CASE("a configured RPM range changes the mapping with the same LED fill") {
  Controller controller{config_with(RpmRange{1000.0F, 5000.0F})};
  CHECK(controller.sample(1000.0F) == FillFraction::empty());
  CHECK(controller.sample(3000.0F) == level(0.5));
  CHECK(controller.sample(5000.0F) == FillFraction::full());
  CHECK(controller.sample(500.0F) == FillFraction::empty());
  CHECK(controller.sample(6500.0F) == FillFraction::full());
}

TEST_CASE("unverified RPM freshness lights the fill; losing the reading turns it off") {
  Controller controller{config_with(RpmRange{})};
  CHECK(controller.sample(3250.0F) == level(0.5));
  controller.provider.set_reading(kEngineRpm, SignalReading{});
  controller.lighting.commands.clear();
  REQUIRE(controller.engine.sample_polled_rules() == SignalStatus::Ok);
  REQUIRE_FALSE(controller.lighting.commands.empty());
  CHECK(controller.lighting.commands.back().fills.empty());
}

TEST_CASE("an empty or inverted RPM range is rejected before the engine attaches") {
  for (const RpmRange range : {RpmRange{3000.0F, 3000.0F}, RpmRange{6500.0F, 0.0F}}) {
    RecordingLightingSink lighting{};
    LedActionSink leds{lighting};
    test_support::FakeSignalProvider provider{kView};
    ActionEngine engine{provider};
    const RpmLevelFillStatus status = controller_config::apply(config_with(range), leds, engine);
    CHECK_FALSE(status.ok());
    CHECK(status.binding == BindingStatus::Ok);
    CHECK(status.rule == ConfigStatus::InvalidRange);
  }
}

TEST_CASE("a failed fill binding adds no RPM rule") {
  RecordingLightingSink lighting{};
  LedActionSink leds{lighting};
  test_support::FakeSignalProvider provider{kView};
  ActionEngine engine{provider};
  REQUIRE(engine.add_sink(leds) == ConfigStatus::Ok);
  RpmLevelFillConfig config = config_with(RpmRange{});
  config.action = ActionId{};
  const RpmLevelFillStatus status = controller_config::apply(config, leds, engine);
  CHECK_FALSE(status.ok());
  CHECK(status.binding == BindingStatus::InvalidAction);
  CHECK_FALSE(status.rule.has_value());
  REQUIRE(engine.attach() == SignalStatus::Ok);
  provider.start();
  provider.set_reading(kEngineRpm, SignalReading{SignalValue::number(3250.0F),
                                                 Availability::FreshnessUnverified,
                                                 ValidationStatus::Reference});
  REQUIRE(engine.sample_polled_rules() == SignalStatus::Ok);
  CHECK(lighting.commands.empty());
  provider.stop();
  CHECK(engine.detach() == SignalStatus::Ok);
}
