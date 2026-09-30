#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include <string>
#include <variant>

#include "controller_config/persisted/json_loader.hpp"

using namespace controller_config::persisted;

TEST_CASE("JSON loader defaults omitted lists and owns strings") {
  auto empty = parse_controller_config(R"({"version":1})");
  REQUIRE(empty.ok());
  CHECK(empty.configuration->actions.empty());
  CHECK(empty.configuration->rules.empty());
  CHECK(empty.configuration->outputs.empty());
  std::string input = R"({"version":1,"actions":[{"name":"owned"}]})";
  auto result = parse_controller_config(input);
  REQUIRE(result.ok());
  input.assign(input.size(), 'x');
  CHECK(result.configuration->actions[0].name == "owned");
}

TEST_CASE("JSON loader constructs every persisted rule and output alternative") {
  auto result = parse_controller_config(R"({
    "version":1,"actions":[{"name":"turn"},{"name":"rpm"},{"name":"fill"}],
    "rules":[
      {"type":"state","action":"turn","signal_key":"vehicle.turn_state","comparison":"equal","operand":{"choice":"left"}},
      {"type":"event","action":"turn","signal_key":"vehicle.turn_state","comparison":"not_equal","operand":{"boolean":false},"edge":"becomes_false","freshness":"fresh_or_unverified"},
      {"type":"sampled_state","action":"rpm","signal_key":"vehicle.engine_rpm","comparison":"greater","operand":{"number":6000},"release_threshold":5900},
      {"type":"range","action":"fill","signal_key":"vehicle.engine_rpm","input":{"from":0,"to":6500},"output":{"from":1,"to":0}}
    ],"outputs":[
      {"type":"led_effect","action":"turn","effect":"left_turn"},
      {"type":"led_fill","action":"fill","zone":{"start":0,"length":100,"direction":"center_out"},"color":{"red":0,"green":16,"blue":32},"priority":255}
    ]})");
  REQUIRE(result.ok());
  const auto &config = *result.configuration;
  REQUIRE(config.rules.size() == 4);
  CHECK(std::get<StateRule>(config.rules[0]).condition.signal_key == "vehicle.turn_state");
  CHECK(std::get<ChoiceOperand>(std::get<StateRule>(config.rules[0]).condition.operand).key ==
        "left");
  CHECK(std::get<EventRule>(config.rules[1]).edge == action_engine::EventEdge::BecomesFalse);
  CHECK(std::get<SampledStateRule>(config.rules[2]).release_threshold == 5900.0F);
  CHECK(std::get<RangeRule>(config.rules[3]).output.from == 1.0F);
  CHECK(std::get<LedEffectBinding>(config.outputs[0]).priority == kDefaultPriority);
  CHECK(std::get<LedFillBinding>(config.outputs[1]).zone.length == 100);
}

TEST_CASE("JSON loader rejects malformed and structurally invalid input without partial models") {
  for (const auto input :
       {"", "{", "[]", "{}", "{\"version\":1} trailing", "{\"version\":1,\"unknown\":0}",
        "{\"version\":1,\"version\":1}", "{\"version\":1.00000001}", "{\"version\":true}",
        "{\"version\":9223372036854775808}", "{\"version\":1,\"actions\":null}",
        "{\"version\":1,\"actions\":[{\"name\":42}]}",
        "{\"version\":1,\"actions\":[{\"name\":\"a\\u0000b\"}]}"}) {
    INFO(input);
    const auto result = parse_controller_config(input);
    CHECK_FALSE(result.ok());
    CHECK_FALSE(result.configuration.has_value());
    REQUIRE(result.diagnostic.has_value());
    CHECK_FALSE(result.diagnostic->path.empty());
  }
  std::string deep(17, '[');
  deep += std::string(17, ']');
  CHECK(parse_controller_config(deep).diagnostic->code == ConfigErrorCode::NestingLimitExceeded);
  CHECK(parse_controller_config(std::string(16385, ' ')).diagnostic->code ==
        ConfigErrorCode::InputTooLarge);
}

TEST_CASE("JSON loader reuses current semantic validation") {
  const auto result =
      parse_controller_config(R"({"version":1,"actions":[{"name":"a"},{"name":"a"}]})");
  REQUIRE(result.diagnostic.has_value());
  CHECK_FALSE(result.configuration.has_value());
  CHECK(result.diagnostic->schema_error == ValidationError::DuplicateActionName);
  CHECK(result.diagnostic->index == 1);
  CHECK(result.diagnostic->path == "actions[1]");
  CHECK(parse_controller_config(R"({"version":2})").diagnostic->schema_error ==
        ValidationError::UnsupportedVersion);
}

TEST_CASE("JSON loader rejects invalid rule and output fields with their paths") {
  for (
      const auto body :
      {R"("rules":[{"type":"bogus","action":"a","signal_key":"s"}])",
       R"("rules":[{"type":"state","action":"a","signal_key":"s","comparison":"bogus","operand":{"boolean":true}}])",
       R"("rules":[{"type":"state","action":"a","signal_key":"s","comparison":"equal","operand":{"boolean":true,"number":1}}])",
       R"("rules":[{"type":"state","action":"a","signal_key":"s","comparison":"equal","operand":{"boolean":true},"edge":"becomes_true"}])",
       R"("rules":[{"type":"range","action":"a","signal_key":"s","input":{"from":1,"to":1},"output":{"from":0,"to":1}}])",
       R"("rules":[{"type":"sampled_state","action":"a","signal_key":"s","comparison":"greater","operand":{"number":6000.0001},"release_threshold":6000.00001}])",
       R"("outputs":[{"type":"led_effect","action":"a","effect":"bogus"}])",
       R"("outputs":[{"type":"led_fill","action":"a","zone":{"start":0,"length":1,"direction":"bogus"},"color":{"red":0,"green":0,"blue":0}}])",
       R"("outputs":[{"type":"led_fill","action":"a","zone":{"start":99,"length":2,"direction":"center_out"},"color":{"red":0,"green":0,"blue":0}}])",
       R"("outputs":[{"type":"led_effect","action":"missing","effect":"brake"}])",
       R"("outputs":[{"type":"led_effect","action":"a","effect":"brake","priority":255.000001}])",
       R"("outputs":[{"type":"led_effect","action":"a","effect":"brake","priority":256}])"}) {
    INFO(body);
    const auto result = parse_controller_config(
        std::string(R"({"version":1,"actions":[{"name":"a"}],)") + body + "}");
    CHECK_FALSE(result.ok());
    CHECK_FALSE(result.configuration.has_value());
    REQUIRE(result.diagnostic.has_value());
    CHECK_FALSE(result.diagnostic->message.empty());
  }
}
