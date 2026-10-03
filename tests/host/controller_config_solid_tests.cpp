#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "action_engine/engine.hpp"
#include "controller_config/persisted/application.hpp"
#include "controller_config/persisted/json_loader.hpp"
#include "controller_config/persisted/names.hpp"
#include "controller_config/persisted/production_profile.hpp"
#include "local_argb/renderer.hpp"
#include "support/fake_clock.hpp"
#include "support/fake_signal_provider.hpp"

#include <string>
#include <vector>

namespace {
namespace persisted = controller_config::persisted;
using local_argb::internal::FillDirection;
using vehicle_signals::Availability;
using vehicle_signals::SignalCapability;
using vehicle_signals::SignalId;
using vehicle_signals::SignalMetadata;
using vehicle_signals::SignalNotification;
using vehicle_signals::SignalReading;
using vehicle_signals::SignalType;
using vehicle_signals::SignalUnit;
using vehicle_signals::SignalValue;
using vehicle_signals::ValidationStatus;

persisted::Condition is_true(const std::string &signal_key) {
  return {signal_key, action_engine::Comparison::Equal, persisted::BooleanOperand{true}};
}

persisted::ControllerConfig solid_config() {
  persisted::ControllerConfig config{};
  config.actions = {{"stop"}};
  config.outputs = {
      persisted::LedSolidBinding{"stop", {35, 30, FillDirection::StartToEnd}, {16, 0, 0}, 200}};
  return config;
}

std::string solid_json() {
  return R"({"version":1,"actions":[{"name":"stop"}],"outputs":[{"type":"led_solid","action":"stop","zone":{"start":35,"length":30,"direction":"start_to_end"},"color":{"red":16,"green":0,"blue":0},"priority":200}]})";
}

void check_error(const persisted::ControllerConfig &config, persisted::ValidationError error) {
  const auto result = persisted::validate(config);
  CHECK(result.error == error);
  CHECK(result.section == persisted::ConfigSection::Outputs);
}

class PixelSink final : public local_argb::PixelFrameSink {
public:
  bool write(const local_argb::PixelFrame &value) noexcept override {
    frame = value;
    return true;
  }
  local_argb::PixelFrame frame{};
};

class RendererSink final : public local_argb::internal::LightingSink {
public:
  explicit RendererSink(local_argb::internal::RendererController &renderer) : renderer_(renderer) {}
  bool publish(const local_argb::internal::LightingCommand &command) noexcept override {
    return renderer_.apply(command, 1000);
  }

private:
  local_argb::internal::RendererController &renderer_;
};

constexpr SignalId kBrakePressed{1};
constexpr SignalId kRedZone{2};
constexpr SignalId kTurnState{3};
constexpr SignalId kEngineRpm{4};
constexpr vehicle_signals::SignalEnumChoice kTurnChoices[] = {
    {1, "left"}, {2, "right"}, {3, "hazard"}};
// Covers every signal the factory profile names, so it applies unchanged.
constexpr SignalMetadata kCatalog[] = {
    {kTurnState, "vehicle.turn_state", SignalType::Enum, SignalUnit::None,
     ValidationStatus::Reference, SignalCapability::Notify, kTurnChoices, 3},
    {kEngineRpm, "vehicle.engine_rpm", SignalType::Number, SignalUnit::RevolutionsPerMinute,
     ValidationStatus::Reference, SignalCapability::Read, nullptr, 0},
    {kBrakePressed, "vehicle.brake_pressed", SignalType::Boolean, SignalUnit::None,
     ValidationStatus::Confirmed, SignalCapability::Read | SignalCapability::Notify, nullptr, 0},
    {kRedZone, "test.red_zone", SignalType::Boolean, SignalUnit::None, ValidationStatus::Reference,
     SignalCapability::Notify, nullptr, 0},
};

// Loads a configuration through JSON and drives it into the production
// renderer, so every assertion is on rendered pixels.
struct RenderedController final {
  explicit RenderedController(const persisted::ControllerConfig &model) {
    REQUIRE(renderer.start());
    const auto loaded =
        persisted::parse_controller_config(persisted::serialize_controller_config(model));
    REQUIRE(loaded.ok());
    REQUIRE(persisted::apply_controller_config(*loaded.configuration, leds, engine).ok());
    REQUIRE(engine.attach() == vehicle_signals::SignalStatus::Ok);
    provider.start();
  }
  ~RenderedController() {
    provider.stop();
    CHECK(engine.detach() == vehicle_signals::SignalStatus::Ok);
  }

  void publish(SignalId id, bool value, Availability availability = Availability::Fresh) {
    REQUIRE(
        provider.publish(SignalNotification{
            id,
            SignalReading{SignalValue::boolean(value), availability, ValidationStatus::Reference},
            false, false, false, false}) == 1);
  }

  void sample_rpm(SignalReading reading) {
    provider.set_reading(kEngineRpm, reading);
    REQUIRE(engine.sample_polled_rules() == vehicle_signals::SignalStatus::Ok);
  }

  test_support::FakeSignalProvider provider{vehicle_signals::SignalCatalogView{kCatalog}};
  PixelSink pixels{};
  local_argb::internal::RendererController renderer{pixels};
  RendererSink lighting{renderer};
  test_support::FakeClock clock{};
  local_argb_actions::LedActionSink leds{lighting, clock};
  action_engine::ActionEngine engine{provider};
};

bool region_is(const local_argb::PixelFrame &frame, std::size_t start, std::size_t length,
               const local_argb::Rgb &color) {
  for (std::size_t pixel = start; pixel < start + length; ++pixel)
    if (frame[pixel] != color)
      return false;
  return true;
}

persisted::ControllerConfig brake_and_red_zone_config() {
  persisted::ControllerConfig config{};
  config.actions = {{"red_zone"}, {"brake"}};
  config.rules = {persisted::StateRule{"red_zone", is_true("test.red_zone")},
                  persisted::StateRule{"brake", is_true("vehicle.brake_pressed")}};
  config.outputs = {
      persisted::LedSolidBinding{"red_zone", {30, 30, FillDirection::StartToEnd}, {0, 0, 12}, 150},
      persisted::LedSolidBinding{"brake", {40, 30, FillDirection::StartToEnd}, {12, 0, 0}, 200}};
  return config;
}

// The factory profile with its brake-region bindings restored to the legacy
// fixed brake effect, as shipped before led_solid.
persisted::ControllerConfig legacy_brake_profile() {
  auto config = persisted::production_lighting_config();
  for (auto &output : config.outputs) {
    if (const auto *solid = std::get_if<persisted::LedSolidBinding>(&output))
      output = persisted::LedEffectBinding{solid->action, local_argb_actions::LedEffect::Brake,
                                           solid->priority};
  }
  return config;
}
} // namespace

TEST_CASE("led_solid round trips through canonical JSON") {
  const auto loaded = persisted::parse_controller_config(solid_json());
  REQUIRE(loaded.ok());
  const auto &binding = std::get<persisted::LedSolidBinding>(loaded.configuration->outputs[0]);
  CHECK(binding.action == "stop");
  CHECK(binding.zone.start == 35);
  CHECK(binding.zone.length == 30);
  CHECK(binding.color.red == 16);
  CHECK(binding.priority == 200);
  const auto json = persisted::serialize_controller_config(*loaded.configuration);
  CHECK(json.find("\"type\":\"led_solid\"") != std::string::npos);
  const auto reloaded = persisted::parse_controller_config(json);
  REQUIRE(reloaded.ok());
  CHECK(persisted::serialize_controller_config(*reloaded.configuration) == json);
  CHECK(persisted::name_of(persisted::OutputType::LedSolid) == "led_solid");
}

TEST_CASE("led_solid priority defaults to the adapter default") {
  auto json = solid_json();
  json.erase(json.find(",\"priority\":200"), std::string{",\"priority\":200"}.size());
  const auto loaded = persisted::parse_controller_config(json);
  REQUIRE(loaded.ok());
  CHECK(std::get<persisted::LedSolidBinding>(loaded.configuration->outputs[0]).priority ==
        persisted::kDefaultPriority);
}

TEST_CASE("led_solid rejects invalid zones colors priorities and action references") {
  auto config = solid_config();
  auto &binding = std::get<persisted::LedSolidBinding>(config.outputs[0]);
  binding.action = "missing";
  check_error(config, persisted::ValidationError::UndeclaredAction);
  binding.action = "stop";
  binding.zone.length = 0;
  check_error(config, persisted::ValidationError::EmptyZone);
  binding.zone.length = 66;
  check_error(config, persisted::ValidationError::ZoneOutOfRange);
  binding.zone.length = 30;
  binding.zone.direction = static_cast<FillDirection>(255);
  check_error(config, persisted::ValidationError::UnknownFillDirection);
  binding.zone.direction = FillDirection::StartToEnd;
  binding.color.green = -1;
  check_error(config, persisted::ValidationError::InvalidColor);
  binding.color = {256, 0, 0};
  check_error(config, persisted::ValidationError::InvalidColor);
  binding.color.red = 255;
  binding.priority = 256;
  check_error(config, persisted::ValidationError::InvalidPriority);
  binding.priority = -1;
  check_error(config, persisted::ValidationError::InvalidPriority);
}

TEST_CASE("led_solid accepts state and sampled state actions") {
  auto config = solid_config();
  config.rules = {persisted::StateRule{"stop", is_true("vehicle.brake_pressed")}};
  CHECK(persisted::validate(config).ok());
  config.rules = {persisted::SampledStateRule{"stop", is_true("vehicle.brake_pressed")}};
  CHECK(persisted::validate(config).ok());
}

TEST_CASE("led_solid rejects actions driven by range or event rules") {
  for (const auto &rule :
       {persisted::Rule{persisted::RangeRule{"stop", "vehicle.engine_rpm", {0, 1}, {0, 1}}},
        persisted::Rule{persisted::EventRule{"stop", is_true("vehicle.brake_pressed")}}}) {
    CAPTURE(rule.index());
    auto config = solid_config();
    config.rules = {rule};
    check_error(config, persisted::ValidationError::IncompatibleActionKind);
    CHECK(persisted::validate(config).index == 0);

    const auto loaded =
        persisted::parse_controller_config(persisted::serialize_controller_config(config));
    REQUIRE_FALSE(loaded.ok());
    CHECK(loaded.diagnostic->schema_error == persisted::ValidationError::IncompatibleActionKind);
    CHECK(loaded.diagnostic->path == "outputs[0]");
    CHECK(loaded.diagnostic->message.find("led_solid") != std::string::npos);
  }
}

TEST_CASE("led_solid duplicate targets ignore direction and appearance but not kind or zone") {
  auto config = solid_config();
  auto binding = std::get<persisted::LedSolidBinding>(config.outputs[0]);
  config.outputs.push_back(persisted::LedFillBinding{
      binding.action,
      {binding.zone.start, binding.zone.length, FillDirection::EndToStart},
      binding.color,
      binding.priority});
  config.outputs.push_back(persisted::LedEffectBinding{
      binding.action, local_argb_actions::LedEffect::Brake, binding.priority});
  binding.zone.start = 0;
  config.outputs.push_back(binding);
  REQUIRE(persisted::validate(config).ok());
  binding.zone.direction = FillDirection::CenterOut;
  binding.color = {0, 0, 255};
  binding.priority = 0;
  config.outputs.push_back(binding);
  check_error(config, persisted::ValidationError::DuplicateBinding);
  CHECK(persisted::validate(config).index == 4);
}

TEST_CASE("led_solid and led_fill on the same action and zone share one fill slot") {
  for (const bool fill_first : {false, true}) {
    CAPTURE(fill_first);
    auto config = solid_config();
    const auto solid = std::get<persisted::LedSolidBinding>(config.outputs[0]);
    const persisted::OutputBinding fill =
        persisted::LedFillBinding{solid.action, solid.zone, {0, 0, 9}, 10};
    config.outputs.insert(fill_first ? config.outputs.begin() : config.outputs.end(), fill);
    check_error(config, persisted::ValidationError::DuplicateBinding);
    CHECK(persisted::validate(config).index == 1);
  }
}

TEST_CASE("JSON led_solid bindings require zone and color and reject unknown fields") {
  for (const std::string field : {"zone", "color", "action"}) {
    CAPTURE(field);
    auto json = solid_json();
    const auto key = json.find("\"" + field + "\"");
    const auto value_end = field == "action" ? json.find(',', key) : json.find('}', key);
    json.erase(key, value_end - key + 1 + (field == "action" ? 0 : 1));
    const auto result = persisted::parse_controller_config(json);
    REQUIRE_FALSE(result.ok());
    CHECK(result.diagnostic->path == "outputs[0]." + field);
  }
  for (const std::string field : {"\"duration_ms\":800", "\"effect\":\"brake\""}) {
    CAPTURE(field);
    auto json = solid_json();
    json.insert(json.find("\"priority\""), field + ",");
    const auto result = persisted::parse_controller_config(json);
    REQUIRE_FALSE(result.ok());
    CHECK(result.diagnostic->path == "outputs[0]." + field.substr(1, field.find('"', 1) - 1));
  }
}

TEST_CASE("led_solid lights its zone and colour on activate and clears it on deactivate") {
  auto model = solid_config();
  model.outputs[0] =
      persisted::LedSolidBinding{"stop", {10, 5, FillDirection::EndToStart}, {0, 9, 3}, 200};
  model.rules = {persisted::StateRule{"stop", is_true("vehicle.brake_pressed")}};
  RenderedController controller{model};
  CHECK(controller.pixels.frame == local_argb::kBlackFrame);

  controller.publish(kBrakePressed, true);
  CHECK(region_is(controller.pixels.frame, 10, 5, local_argb::Rgb{0, 9, 3}));
  CHECK(region_is(controller.pixels.frame, 0, 10, local_argb::kBlack));
  CHECK(region_is(controller.pixels.frame, 15, local_argb::kLedCount - 15, local_argb::kBlack));

  controller.publish(kBrakePressed, false);
  CHECK(controller.pixels.frame == local_argb::kBlackFrame);
}

TEST_CASE("led_solid caps its colour at the brightness ceiling") {
  auto model = solid_config();
  std::get<persisted::LedSolidBinding>(model.outputs[0]).color = {255, 255, 255};
  model.rules = {persisted::StateRule{"stop", is_true("vehicle.brake_pressed")}};
  RenderedController controller{model};
  controller.publish(kBrakePressed, true);
  const auto ceiling = local_argb::kBrightnessCeiling;
  CHECK(region_is(controller.pixels.frame, 35, 30, local_argb::Rgb{ceiling, ceiling, ceiling}));
}

TEST_CASE("led_solid fails off to black without a usable observation") {
  for (const auto availability : {Availability::NoData, Availability::Stale,
                                  Availability::Unavailable, Availability::FreshnessUnverified}) {
    CAPTURE(static_cast<int>(availability));
    auto model = solid_config();
    model.rules = {persisted::StateRule{"stop", is_true("vehicle.brake_pressed")}};
    RenderedController controller{model};
    controller.publish(kBrakePressed, true);
    REQUIRE(region_is(controller.pixels.frame, 35, 30,
                      local_argb::Rgb{local_argb::kBrightnessCeiling, 0, 0}));
    controller.publish(kBrakePressed, true, availability);
    CHECK(controller.pixels.frame == local_argb::kBlackFrame);
  }
}

TEST_CASE("led_solid driven by a sampled rule fails off to black when a read fails") {
  auto model = solid_config();
  model.rules = {persisted::SampledStateRule{
      "stop",
      {"vehicle.engine_rpm", action_engine::Comparison::Greater, persisted::NumberOperand{6000}},
      action_engine::FreshnessRequirement::FreshOrUnverified}};
  RenderedController controller{model};
  const auto rpm = [](float value) {
    return SignalReading{SignalValue::number(value), Availability::FreshnessUnverified,
                         ValidationStatus::Reference};
  };
  controller.sample_rpm(rpm(6500.0F));
  REQUIRE(region_is(controller.pixels.frame, 35, 30,
                    local_argb::Rgb{local_argb::kBrightnessCeiling, 0, 0}));
  controller.sample_rpm(SignalReading{});
  CHECK(controller.pixels.frame == local_argb::kBlackFrame);
  controller.sample_rpm(rpm(6500.0F));
  REQUIRE(region_is(controller.pixels.frame, 35, 30,
                    local_argb::Rgb{local_argb::kBrightnessCeiling, 0, 0}));
  controller.sample_rpm(rpm(1000.0F));
  CHECK(controller.pixels.frame == local_argb::kBlackFrame);
}

TEST_CASE("coexisting led_solid outputs in different colours resolve overlap by priority") {
  RenderedController controller{brake_and_red_zone_config()};
  const local_argb::Rgb blue{0, 0, 12};
  const local_argb::Rgb red{12, 0, 0};

  controller.publish(kRedZone, true);
  CHECK(region_is(controller.pixels.frame, 30, 30, blue));

  controller.publish(kBrakePressed, true);
  CHECK(region_is(controller.pixels.frame, 30, 10, blue));
  CHECK(region_is(controller.pixels.frame, 40, 30, red));

  controller.publish(kRedZone, false);
  CHECK(region_is(controller.pixels.frame, 30, 10, local_argb::kBlack));
  CHECK(region_is(controller.pixels.frame, 40, 30, red));

  controller.publish(kRedZone, true);
  controller.publish(kBrakePressed, false);
  CHECK(region_is(controller.pixels.frame, 30, 30, blue));
  CHECK(region_is(controller.pixels.frame, 60, 10, local_argb::kBlack));
}

TEST_CASE("coexisting led_solid outputs keep the higher priority regardless of order") {
  auto model = brake_and_red_zone_config();
  std::swap(model.outputs[0], model.outputs[1]);
  RenderedController controller{model};
  controller.publish(kBrakePressed, true);
  controller.publish(kRedZone, true);
  CHECK(region_is(controller.pixels.frame, 30, 10, local_argb::Rgb{0, 0, 12}));
  CHECK(region_is(controller.pixels.frame, 40, 30, local_argb::Rgb{12, 0, 0}));
}

TEST_CASE("factory brake via led_solid renders pixel-identical to the legacy brake effect") {
  RenderedController solid{persisted::production_lighting_config()};
  RenderedController legacy{legacy_brake_profile()};
  CHECK(solid.pixels.frame == legacy.pixels.frame);

  for (const bool pressed : {true, false, true}) {
    solid.publish(kBrakePressed, pressed);
    legacy.publish(kBrakePressed, pressed);
    CAPTURE(pressed);
    CHECK(solid.pixels.frame == legacy.pixels.frame);
  }
  CHECK(region_is(solid.pixels.frame, local_argb::kBrakeLedStart, local_argb::kBrakeLedCount,
                  local_argb::Rgb{local_argb::kBrightnessCeiling, 0, 0}));

  solid.publish(kBrakePressed, true, Availability::Stale);
  legacy.publish(kBrakePressed, true, Availability::Stale);
  CHECK(solid.pixels.frame == local_argb::kBlackFrame);
  CHECK(solid.pixels.frame == legacy.pixels.frame);
}

TEST_CASE("factory brake via led_solid keeps the onboard brake status pixel") {
  RenderedController controller{persisted::production_lighting_config()};
  CHECK(local_argb::internal::onboard_status_color(controller.pixels.frame) == local_argb::kBlack);
  controller.publish(kBrakePressed, true);
  CHECK(local_argb::internal::onboard_status_color(controller.pixels.frame) ==
        local_argb::internal::kOnboardBrakeStatus);
  controller.publish(kBrakePressed, false);
  CHECK(local_argb::internal::onboard_status_color(controller.pixels.frame) == local_argb::kBlack);
}
