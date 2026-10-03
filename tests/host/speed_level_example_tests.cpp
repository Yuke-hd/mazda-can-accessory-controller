#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

// Speed level example (#152): the reviewed example configuration maps
// vehicle.speed_kph through the existing range rule onto SetLevel(speed_level).
// The example is not part of the factory profile and binds no LED output, so
// the action commands are observed directly on a recording sink after applying
// the canonical JSON through the persisted configuration path.
#include "action_engine/engine.hpp"
#include "controller_config/persisted/application.hpp"
#include "controller_config/persisted/json_loader.hpp"
#include "local_argb/lighting_sink.hpp"
#include "local_argb_actions/led_action_sink.hpp"
#include "support/fake_signal_provider.hpp"
#include "support/recording_action_sink.hpp"
#include "vehicle_signals/signal_catalog.hpp"

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

namespace persisted = controller_config::persisted;

using action_engine::ActionCommand;
using action_engine::ActionCommandKind;
using action_engine::ActionEngine;
using action_engine::ActionId;
using action_engine::ConfigStatus;
using vehicle_signals::Availability;
using vehicle_signals::SignalCapability;
using vehicle_signals::SignalCatalogView;
using vehicle_signals::SignalId;
using vehicle_signals::SignalMetadata;
using vehicle_signals::SignalReading;
using vehicle_signals::SignalStatus;
using vehicle_signals::SignalType;
using vehicle_signals::SignalUnit;
using vehicle_signals::SignalValue;
using vehicle_signals::ValidationStatus;
using Commands = std::vector<ActionCommand>;

constexpr SignalId kSpeed{1};
constexpr SignalMetadata kCatalog[] = {
    {kSpeed, "vehicle.speed_kph", SignalType::Number, SignalUnit::KilometresPerHour,
     ValidationStatus::Reference, SignalCapability::Read, nullptr, 0},
};
constexpr SignalCatalogView kView{kCatalog};
static_assert(kView.well_formed());

// Action IDs follow document order; speed_level is the example's only action.
constexpr ActionId kSpeedLevel{1};

persisted::ControllerConfig loaded_example_config() {
  std::ifstream file{CONTROLLER_CONFIG_SPEED_LEVEL_EXAMPLE_JSON_PATH, std::ios::binary};
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
  bool publish(const local_argb::internal::LightingCommand &) noexcept override { return true; }
};

struct Controller final {
  Controller() : leds{lighting, lighting}, engine{provider} {
    REQUIRE(engine.add_sink(actions) == ConfigStatus::Ok);
    REQUIRE(persisted::apply_controller_config(config, leds, engine).ok());
    REQUIRE(engine.attach() == SignalStatus::Ok);
    provider.start();
  }
  ~Controller() {
    provider.stop();
    (void)engine.detach();
  }
  Controller(const Controller &) = delete;
  Controller &operator=(const Controller &) = delete;

  Commands sample(const float speed_kph,
                  const Availability availability = Availability::FreshnessUnverified) {
    provider.set_reading(kSpeed, SignalReading{SignalValue::number(speed_kph), availability,
                                               ValidationStatus::Reference});
    REQUIRE(engine.sample_polled_rules() == SignalStatus::Ok);
    return actions.take();
  }

  const persisted::ControllerConfig config{loaded_example_config()};
  test_support::FakeSignalProvider provider{kView};
  NullLightingSink lighting{};
  local_argb_actions::LedActionSink leds;
  ActionEngine engine;
  test_support::RecordingActionSink actions{};
};

Commands level(const float value) {
  return {ActionCommand{kSpeedLevel, ActionCommandKind::SetLevel, value}};
}
Commands fail_off() { return {ActionCommand{kSpeedLevel, ActionCommandKind::Deactivate, 0.0F}}; }

} // namespace

TEST_CASE("speed level example declares one range rule on vehicle.speed_kph") {
  const auto config = loaded_example_config();

  REQUIRE(config.actions.size() == 1);
  CHECK(config.actions[0].name == "speed_level");
  CHECK(config.outputs.empty());
  REQUIRE(config.rules.size() == 1);
  const auto *rule = std::get_if<persisted::RangeRule>(&config.rules[0]);
  REQUIRE(rule != nullptr);
  CHECK(rule->action == "speed_level");
  CHECK(rule->signal_key == "vehicle.speed_kph");
  CHECK(rule->input.from == 0.0F);
  CHECK(rule->input.to == 120.0F);
  CHECK(rule->output.from == 0.0F);
  CHECK(rule->output.to == 1.0F);
  CHECK(rule->freshness == action_engine::FreshnessRequirement::FreshOrUnverified);
}

TEST_CASE("speed level example maps the configured speed endpoints and midpoint") {
  Controller controller{};

  CHECK(controller.sample(0.0F) == level(0.0F));
  CHECK(controller.sample(60.0F) == level(0.5F));
  CHECK(controller.sample(120.0F) == level(1.0F));
  CHECK(controller.sample(30.0F) == level(0.25F));
}

TEST_CASE("speed level example clamps speeds outside the configured range") {
  Controller controller{};

  CHECK(controller.sample(180.0F) == level(1.0F));
  CHECK(controller.sample(-5.0F) == level(0.0F));
}

TEST_CASE("speed level example accepts fresh and unverified speed observations") {
  Controller controller{};

  CHECK(controller.sample(60.0F, Availability::FreshnessUnverified) == level(0.5F));
  CHECK(controller.sample(90.0F, Availability::Fresh) == level(0.75F));
}

TEST_CASE("speed level example fails off without a usable speed observation") {
  for (const auto availability :
       {Availability::NoData, Availability::Stale, Availability::Unavailable}) {
    CAPTURE(static_cast<int>(availability));
    Controller controller{};
    REQUIRE(controller.sample(60.0F) == level(0.5F));

    CHECK(controller.sample(60.0F, availability) == fail_off());
    CHECK(controller.sample(90.0F, availability).empty());
    CHECK(controller.sample(90.0F) == level(0.75F));
  }
}
