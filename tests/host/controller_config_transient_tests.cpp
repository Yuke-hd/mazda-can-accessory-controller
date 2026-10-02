#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "action_engine/engine.hpp"
#include "controller_config/persisted/application.hpp"
#include "controller_config/persisted/json_loader.hpp"
#include "controller_config/persisted/names.hpp"
#include "local_argb/renderer.hpp"
#include "support/fake_signal_provider.hpp"

#include <string>
#include <vector>

namespace {
namespace persisted = controller_config::persisted;
using local_argb::internal::FillDirection;

persisted::ControllerConfig transient_config() {
  persisted::ControllerConfig config{};
  config.actions = {{"pulse"}};
  config.outputs = {persisted::LedTransientBinding{
      "pulse", {20, 20, FillDirection::StartToEnd}, {32, 16, 0}, 800, 120}};
  return config;
}

std::string transient_json(const std::string &duration = "800") {
  return R"({"version":1,"actions":[{"name":"pulse"}],"outputs":[{"type":"led_transient","action":"pulse","zone":{"start":20,"length":20,"direction":"start_to_end"},"color":{"red":32,"green":16,"blue":0},"duration_ms":)" +
         duration + R"(,"priority":120}]})";
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
    commands.push_back(command);
    return renderer_.apply(command, 1000);
  }
  std::vector<local_argb::internal::LightingCommand> commands;

private:
  local_argb::internal::RendererController &renderer_;
};
} // namespace

TEST_CASE("persisted transients are capped at five seconds with useful duration diagnostics") {
  CHECK(persisted::kMaxTransientDurationMs == 5000);
  auto config = transient_config();
  std::get<persisted::LedTransientBinding>(config.outputs[0]).duration_ms = 5001;
  check_error(config, persisted::ValidationError::InvalidDuration);
  for (const auto *duration : {"5001", "5000.0"}) {
    const auto loaded = persisted::parse_controller_config(transient_json(duration));
    REQUIRE_FALSE(loaded.ok());
    REQUIRE(loaded.diagnostic.has_value());
    CHECK(loaded.diagnostic->schema_error == persisted::ValidationError::InvalidDuration);
    CHECK(loaded.diagnostic->message.find("1..5000") != std::string::npos);
  }
}

TEST_CASE("transient duplicate physical zones reject direction-only variations") {
  auto config = transient_config();
  auto binding = std::get<persisted::LedTransientBinding>(config.outputs[0]);
  binding.zone.direction = FillDirection::EndToStart;
  config.outputs.push_back(binding);
  check_error(config, persisted::ValidationError::DuplicateBinding);
  const auto loaded =
      persisted::parse_controller_config(persisted::serialize_controller_config(config));
  REQUIRE_FALSE(loaded.ok());
  CHECK(loaded.diagnostic->schema_error == persisted::ValidationError::DuplicateBinding);
}

TEST_CASE("transient duration preserves exact milliseconds through JSON round trips") {
  auto config = transient_config();
  for (const auto duration : {persisted::Integer{1}, persisted::kMaxTransientDurationMs}) {
    std::get<persisted::LedTransientBinding>(config.outputs[0]).duration_ms = duration;
    REQUIRE(persisted::validate(config).ok());
    const auto json = persisted::serialize_controller_config(config);
    const auto loaded = persisted::parse_controller_config(json);
    REQUIRE(loaded.ok());
    CHECK(std::get<persisted::LedTransientBinding>(loaded.configuration->outputs[0]).duration_ms ==
          duration);
    CHECK(persisted::serialize_controller_config(*loaded.configuration) == json);
  }
  CHECK(persisted::name_of(persisted::OutputType::LedTransient) == "led_transient");
}

TEST_CASE("transient duration rejects zero negative unbounded and lossy numbers") {
  auto config = transient_config();
  for (const auto duration :
       {persisted::Integer{0}, persisted::Integer{-1}, persisted::kMaxTransientDurationMs + 1}) {
    std::get<persisted::LedTransientBinding>(config.outputs[0]).duration_ms = duration;
    check_error(config, persisted::ValidationError::InvalidDuration);
  }
  for (const std::string duration :
       {"0", "-1", "9007199254740992", "9007199254740993", "1e100", "800.1", "800.0", "8e2",
        "9007199254740991.1", "true", "\"800\""}) {
    CAPTURE(duration);
    const auto result = persisted::parse_controller_config(transient_json(duration));
    REQUIRE_FALSE(result.ok());
    CHECK(result.diagnostic->message.find("duration") != std::string::npos);
  }
}

TEST_CASE("transient outputs reject invalid zones colors priorities and action references") {
  auto config = transient_config();
  auto &binding = std::get<persisted::LedTransientBinding>(config.outputs[0]);
  binding.action = "missing";
  check_error(config, persisted::ValidationError::UndeclaredAction);
  binding.action = "pulse";
  binding.zone.length = 0;
  check_error(config, persisted::ValidationError::EmptyZone);
  binding.zone.length = 81;
  check_error(config, persisted::ValidationError::ZoneOutOfRange);
  binding.zone.length = 20;
  binding.zone.direction = static_cast<FillDirection>(255);
  check_error(config, persisted::ValidationError::UnknownFillDirection);
  binding.zone.direction = FillDirection::StartToEnd;
  binding.color.red = -1;
  check_error(config, persisted::ValidationError::InvalidColor);
  binding.color = {0, 255, 256};
  check_error(config, persisted::ValidationError::InvalidColor);
  binding.color.blue = 255;
  binding.priority = 256;
  check_error(config, persisted::ValidationError::InvalidPriority);
}

TEST_CASE("transient duplicate targets ignore appearance but distinguish binding kinds and zones") {
  auto config = transient_config();
  auto binding = std::get<persisted::LedTransientBinding>(config.outputs[0]);
  config.outputs.push_back(
      persisted::LedFillBinding{binding.action, binding.zone, binding.color, binding.priority});
  binding.zone.start = 40;
  config.outputs.push_back(binding);
  REQUIRE(persisted::validate(config).ok());
  binding.color.red = 255;
  binding.duration_ms = 1;
  binding.priority = 0;
  config.outputs.push_back(binding);
  check_error(config, persisted::ValidationError::DuplicateBinding);
  CHECK(persisted::validate(config).index == 3);
}

TEST_CASE("JSON transient bindings require duration and reject unknown fields") {
  auto json = transient_json();
  json.erase(json.find("\"duration_ms\""), std::string{"\"duration_ms\":800,"}.size());
  auto result = persisted::parse_controller_config(json);
  REQUIRE_FALSE(result.ok());
  CHECK(result.diagnostic->path == "outputs[0].duration_ms");
  json = transient_json();
  json.insert(json.find("\"priority\""), "\"animation\":\"blink\",");
  result = persisted::parse_controller_config(json);
  REQUIRE_FALSE(result.ok());
  CHECK(result.diagnostic->path == "outputs[0].animation");
}

TEST_CASE("loaded event transient expires through the production renderer and leaves held fill") {
  constexpr vehicle_signals::SignalId signal{1};
  constexpr vehicle_signals::SignalMetadata metadata[] = {
      {signal, "test.event", vehicle_signals::SignalType::Boolean,
       vehicle_signals::SignalUnit::None, vehicle_signals::ValidationStatus::Reference,
       vehicle_signals::SignalCapability::Notify, nullptr, 0}};
  test_support::FakeSignalProvider provider{vehicle_signals::SignalCatalogView{metadata}};
  PixelSink pixels;
  local_argb::internal::RendererController renderer{pixels};
  REQUIRE(renderer.start());
  RendererSink lighting{renderer};
  local_argb_actions::LedActionSink leds{lighting};
  action_engine::ActionEngine engine{provider};
  auto model = transient_config();
  model.actions.push_back({"baseline"});
  model.outputs.push_back(
      persisted::LedFillBinding{"baseline", {20, 20, FillDirection::StartToEnd}, {0, 0, 8}, 10});
  model.rules.push_back(persisted::StateRule{
      "baseline",
      {"test.event", action_engine::Comparison::Equal, persisted::BooleanOperand{true}}});
  model.rules.push_back(persisted::EventRule{
      "pulse", {"test.event", action_engine::Comparison::Equal, persisted::BooleanOperand{true}}});
  const auto loaded =
      persisted::parse_controller_config(persisted::serialize_controller_config(model));
  REQUIRE(loaded.ok());
  REQUIRE(persisted::apply_controller_config(*loaded.configuration, leds, engine).ok());
  REQUIRE(engine.attach() == vehicle_signals::SignalStatus::Ok);
  provider.start();
  vehicle_signals::SignalNotification notification{};
  notification.id = signal;
  notification.current = {vehicle_signals::SignalValue::boolean(false),
                          vehicle_signals::Availability::Fresh,
                          vehicle_signals::ValidationStatus::Reference};
  REQUIRE(provider.publish(notification) == 1);
  notification.current.value = vehicle_signals::SignalValue::boolean(true);
  REQUIRE(provider.publish(notification) == 1);
  REQUIRE(lighting.commands.back().transients.size() == 1);
  const auto &effect = lighting.commands.back().transients.begin()->effect;
  CHECK(effect.duration_us == 800000);
  CHECK(effect.priority.rank() == 120);
  REQUIRE(renderer.tick(800999));
  CHECK(pixels.frame[20] == local_argb::Rgb{local_argb::kBrightnessCeiling, 16, 0});
  REQUIRE(renderer.tick(801000));
  CHECK(pixels.frame[20] == local_argb::Rgb{0, 0, 8});
  provider.stop();
  CHECK(engine.detach() == vehicle_signals::SignalStatus::Ok);
}

TEST_CASE("duration source token mapping skips escaped digit strings and adjacent rule numbers") {
  auto config = transient_config();
  const std::string action = "pulse 123\"\\\n456";
  config.actions[0].name = action;
  std::get<persisted::LedTransientBinding>(config.outputs[0]).action = action;
  config.rules.push_back(persisted::RangeRule{action, "test.78\"\\9", {1.5F, 20.25F}, {0, 1}});
  config.outputs.push_back(persisted::LedTransientBinding{action,
                                                          {40, 1, FillDirection::EndToStart},
                                                          {1, 2, 3},
                                                          persisted::kMaxTransientDurationMs,
                                                          255});
  auto json = persisted::serialize_controller_config(config);
  auto loaded = persisted::parse_controller_config(json);
  REQUIRE(loaded.ok());
  CHECK(loaded.configuration->actions[0].name == action);
  CHECK(std::get<persisted::LedTransientBinding>(loaded.configuration->outputs[0]).duration_ms ==
        800);
  CHECK(std::get<persisted::LedTransientBinding>(loaded.configuration->outputs[1]).duration_ms ==
        persisted::kMaxTransientDurationMs);
  CHECK(persisted::serialize_controller_config(*loaded.configuration) == json);
  // The schema key can also be escaped; token recovery follows the parsed node.
  json.replace(json.find("duration_ms"), 11, "\\u0064uration_ms");
  REQUIRE(persisted::parse_controller_config(json).ok());
  const auto maximum = std::to_string(persisted::kMaxTransientDurationMs);
  const auto offset = json.find(maximum);
  REQUIRE(offset != std::string::npos);
  json.insert(offset + maximum.size(), ".1");
  loaded = persisted::parse_controller_config(json);
  REQUIRE_FALSE(loaded.ok());
  CHECK(loaded.diagnostic->path == "outputs[1].duration_ms");
}

TEST_CASE("JSON transient failures retain diagnostics for each invalid binding field") {
  struct InvalidField {
    std::string before;
    std::string after;
    persisted::ValidationError error;
  };
  const InvalidField invalid[] = {
      {"\"action\":\"pulse\"", "\"action\":\"missing\"",
       persisted::ValidationError::UndeclaredAction},
      {"\"length\":20", "\"length\":0", persisted::ValidationError::EmptyZone},
      {"\"start\":20", "\"start\":99", persisted::ValidationError::ZoneOutOfRange},
      {"\"red\":32", "\"red\":256", persisted::ValidationError::InvalidColor},
      {"\"priority\":120", "\"priority\":-1", persisted::ValidationError::InvalidPriority},
  };
  for (const auto &field : invalid) {
    auto json = transient_json();
    json.replace(json.find(field.before), field.before.size(), field.after);
    const auto result = persisted::parse_controller_config(json);
    REQUIRE_FALSE(result.ok());
    CHECK(result.diagnostic->schema_error == field.error);
    CHECK(result.diagnostic->index == 0);
    CHECK_FALSE(result.diagnostic->message.empty());
  }
}
