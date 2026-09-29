#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "controller_config/json_loader.hpp"
#include "vehicle_signals/signal_catalog.hpp"
#include "vehicle_signals/signal_contracts.hpp"
#include <fstream>
#include <iterator>

#include <string>
#include <string_view>
#include <variant>

namespace {

using controller_config::ConfigErrorCategory;
using controller_config::ConfigErrorCode;
using controller_config::ConfigLoadResult;

constexpr vehicle_signals::SignalEnumChoice kTurnChoices[] = {
    {1, "left"}, {2, "right"}, {3, "hazard"}};
constexpr vehicle_signals::SignalMetadata kSignals[] = {
    {vehicle_signals::SignalId{1}, "vehicle.turn_state", vehicle_signals::SignalType::Enum,
     vehicle_signals::SignalUnit::None, vehicle_signals::ValidationStatus::Reference,
     vehicle_signals::SignalCapability::Notify, kTurnChoices, 3},
    {vehicle_signals::SignalId{2}, "vehicle.engine_rpm", vehicle_signals::SignalType::Number,
     vehicle_signals::SignalUnit::RevolutionsPerMinute,
     vehicle_signals::ValidationStatus::Reference, vehicle_signals::SignalCapability::Read, nullptr,
     0}};
constexpr vehicle_signals::SignalCatalogView kCatalog{kSignals};
static_assert(kCatalog.well_formed());

std::string production_json() {
  return R"json({
    "version": 1,
    "actions": [
      {"name": "left_turn"}, {"name": "right_turn"}, {"name": "hazard"},
      {"name": "rpm_fill"}, {"name": "red_zone"}
    ],
    "rules": [
      {"type": "state", "action": "left_turn", "signal": "vehicle.turn_state",
       "comparison": "equal", "operand": {"kind": "choice", "value": "left"},
       "freshness": "fresh"},
      {"type": "state", "action": "right_turn", "signal": "vehicle.turn_state",
       "comparison": "equal", "operand": {"kind": "choice", "value": "right"},
       "freshness": "fresh"},
      {"type": "state", "action": "hazard", "signal": "vehicle.turn_state",
       "comparison": "equal", "operand": {"kind": "choice", "value": "hazard"},
       "freshness": "fresh"},
      {"type": "range", "action": "rpm_fill", "signal": "vehicle.engine_rpm",
       "input": {"from": 0.0, "to": 6500.0}, "output": {"from": 0.0, "to": 1.0},
       "freshness": "fresh_or_unverified"},
      {"type": "sampled_state", "action": "red_zone", "signal": "vehicle.engine_rpm",
       "comparison": "greater", "operand": {"kind": "number", "value": 6000.0},
       "freshness": "fresh_or_unverified"}
    ],
    "effect_bindings": [
      {"action": "left_turn", "effect": "right_turn", "priority": 100},
      {"action": "right_turn", "effect": "left_turn", "priority": 100},
      {"action": "hazard", "effect": "left_turn", "priority": 100},
      {"action": "hazard", "effect": "right_turn", "priority": 100},
      {"action": "red_zone", "effect": "brake", "priority": 150}
    ],
    "fill_bindings": [{
      "action": "rpm_fill",
      "zone": {"start": 0, "length": 100, "direction": "center_out"},
      "color": {"red": 0, "green": 16, "blue": 32}, "priority": 50
    }]
  })json";
}

ConfigLoadResult load(std::string json) { return controller_config::parse_controller_config(json); }

void replace_once(std::string &json, const std::string_view from, const std::string_view to) {
  const std::size_t position = json.find(from);
  REQUIRE(position != std::string::npos);
  json.replace(position, from.size(), to);
}

void require_failure(const ConfigLoadResult &result, const ConfigErrorCategory category,
                     const ConfigErrorCode code) {
  REQUIRE_FALSE(result.ok());
  REQUIRE_FALSE(result.configuration.has_value());
  REQUIRE(result.diagnostic.has_value());
  CHECK(result.diagnostic->category == category);
  CHECK(result.diagnostic->code == code);
  CHECK_FALSE(result.diagnostic->path.empty());
  CHECK_FALSE(result.diagnostic->message.empty());
}

void require_rejection(const ConfigLoadResult &result, const ConfigErrorCategory category) {
  REQUIRE_FALSE(result.ok());
  REQUIRE_FALSE(result.configuration.has_value());
  REQUIRE(result.diagnostic.has_value());
  CHECK(result.diagnostic->category == category);
  CHECK_FALSE(result.diagnostic->path.empty());
  CHECK_FALSE(result.diagnostic->message.empty());
}

TEST_CASE("canonical production JSON loads into the owning runtime model") {
  const ConfigLoadResult result = load(production_json());

  REQUIRE(result.ok());
  REQUIRE(result.configuration.has_value());
  CHECK(result.configuration->version == 1U);
  CHECK(result.configuration->actions.size() == 5U);
  CHECK(result.configuration->rules.size() == 5U);
  CHECK(result.configuration->effect_bindings.size() == 5U);
  CHECK(result.configuration->fill_bindings.size() == 1U);
  CHECK(result.configuration->action_id("left_turn") == action_engine::ActionId{1});
  CHECK(result.configuration->rules[3].input.to == 6500.0F);
  CHECK(result.configuration->fill_bindings[0].zone.direction == "center_out");
}

TEST_CASE("malformed JSON and an unsupported schema are rejected with parse context") {
  require_failure(load("{\"version\": 1"), ConfigErrorCategory::Parse,
                  ConfigErrorCode::MalformedJson);
  require_failure(load(production_json() + " trailing"), ConfigErrorCategory::Parse,
                  ConfigErrorCode::MalformedJson);

  auto json = production_json();
  replace_once(json, "\"version\": 1", "\"version\": 2");
  const ConfigLoadResult result = load(std::move(json));
  require_failure(result, ConfigErrorCategory::Semantic, ConfigErrorCode::SchemaValidation);
  CHECK(result.diagnostic->schema_error == controller_config::SchemaError::UnsupportedVersion);
  CHECK(result.diagnostic->path == "version");
}

TEST_CASE("integral spellings and NUL-terminated inputs have explicit behavior") {
  auto json = production_json();
  replace_once(json, "\"version\": 1", "\"version\": 1.0");
  CHECK(load(json).ok());

  json = production_json();
  json.push_back('\0');
  CHECK(load(json).ok());

  json = production_json();
  replace_once(json, "\"value\": \"left\"", "\"value\": \"left\\u0000junk\"");
  const ConfigLoadResult embedded_nul = load(std::move(json));
  require_failure(embedded_nul, ConfigErrorCategory::Parse, ConfigErrorCode::EmbeddedNul);
}

TEST_CASE("numeric values outside the signed integer range are rejected") {
  auto json = production_json();
  replace_once(json, "\"version\": 1", "\"version\": 9223372036854775808");
  const ConfigLoadResult above_int64 = load(std::move(json));
  require_rejection(above_int64, ConfigErrorCategory::Structural);
  CHECK(above_int64.diagnostic->path == "version");

  json = production_json();
  replace_once(json, "\"version\": 1", "\"version\": -9223372036854777856");
  const ConfigLoadResult below_int64 = load(std::move(json));
  require_rejection(below_int64, ConfigErrorCategory::Structural);
  CHECK(below_int64.diagnostic->path == "version");
}

TEST_CASE("root shape, empty input, and excessive nesting are rejected safely") {
  const ConfigLoadResult root_array = load("[]");
  require_failure(root_array, ConfigErrorCategory::Structural, ConfigErrorCode::RootTypeMismatch);
  CHECK(root_array.diagnostic->path == "$");

  const ConfigLoadResult empty = load("");
  require_failure(empty, ConfigErrorCategory::Parse, ConfigErrorCode::MalformedJson);
  CHECK(empty.diagnostic->path == "$");

  const std::string too_large(controller_config::kMaxControllerConfigJsonBytes + 1U, ' ');
  require_failure(load(too_large), ConfigErrorCategory::Parse, ConfigErrorCode::InputTooLarge);

  std::string max_payload = production_json();
  max_payload.resize(controller_config::kMaxControllerConfigJsonBytes, ' ');
  max_payload.push_back('\0');
  CHECK(load(max_payload).ok());
  max_payload.insert(max_payload.end() - 1, ' ');
  require_failure(load(max_payload), ConfigErrorCategory::Parse, ConfigErrorCode::InputTooLarge);

  std::string at_limit(controller_config::kMaxControllerConfigJsonNesting, '[');
  at_limit.append(controller_config::kMaxControllerConfigJsonNesting, ']');
  require_failure(load(std::move(at_limit)), ConfigErrorCategory::Structural,
                  ConfigErrorCode::RootTypeMismatch);

  std::string over_limit(controller_config::kMaxControllerConfigJsonNesting + 1U, '[');
  over_limit.append(controller_config::kMaxControllerConfigJsonNesting + 1U, ']');
  require_failure(load(std::move(over_limit)), ConfigErrorCategory::Parse,
                  ConfigErrorCode::NestingLimitExceeded);
}

TEST_CASE("required fields, value types, and unknown fields are rejected") {
  require_failure(load("{\"version\": 1}"), ConfigErrorCategory::Structural,
                  ConfigErrorCode::MissingField);

  auto json = production_json();
  replace_once(json, "\"name\": \"left_turn\"", "\"name\": 4");
  const ConfigLoadResult action_type = load(std::move(json));
  require_failure(action_type, ConfigErrorCategory::Structural, ConfigErrorCode::TypeMismatch);
  CHECK(action_type.diagnostic->index == 0U);
  CHECK(action_type.diagnostic->path == "actions[0].name");

  json = production_json();
  const std::size_t actions_position = json.find("\"actions\"");
  REQUIRE(actions_position != std::string::npos);
  json.insert(actions_position, "\"future_field\": true, ");
  require_failure(load(std::move(json)), ConfigErrorCategory::Structural,
                  ConfigErrorCode::UnknownField);

  json = production_json();
  replace_once(json, "\"freshness\": \"fresh\"},",
               "\"freshness\": \"fresh\", \"input\": {\"from\": 0, \"to\": 1}},");
  const ConfigLoadResult cross_type_field = load(std::move(json));
  require_failure(cross_type_field, ConfigErrorCategory::Structural, ConfigErrorCode::UnknownField);

  json = production_json();
  replace_once(json, "\"version\": 1,", "\"version\": 1, \"version\": 1,");
  require_failure(load(std::move(json)), ConfigErrorCategory::Structural,
                  ConfigErrorCode::InvalidValue);
}

TEST_CASE("action and rule references are validated after parsing") {
  auto json = production_json();
  replace_once(json, "\"right_turn\", \"signal\"", "\"missing\", \"signal\"");
  const ConfigLoadResult unknown_action = load(std::move(json));
  require_failure(unknown_action, ConfigErrorCategory::Semantic, ConfigErrorCode::SchemaValidation);
  CHECK(unknown_action.diagnostic->schema_error == controller_config::SchemaError::UnknownAction);

  json = production_json();
  replace_once(json, "\"type\": \"state\"", "\"type\": \"unsupported\"");
  const ConfigLoadResult unknown_rule = load(std::move(json));
  require_failure(unknown_rule, ConfigErrorCategory::Structural, ConfigErrorCode::InvalidValue);
  CHECK(unknown_rule.diagnostic->schema_error == controller_config::SchemaError::UnknownRuleType);
  CHECK(unknown_rule.diagnostic->index == 0U);
  CHECK(unknown_rule.diagnostic->path == "rules[0].type");

  json = production_json();
  replace_once(json, "\"comparison\": \"equal\"", "\"comparison\": \"unsupported\"");
  const ConfigLoadResult unknown_comparison = load(std::move(json));
  require_failure(unknown_comparison, ConfigErrorCategory::Structural,
                  ConfigErrorCode::InvalidValue);
  CHECK(unknown_comparison.diagnostic->schema_error ==
        controller_config::SchemaError::UnknownComparison);
}

TEST_CASE("duplicate actions, operand shapes, and numeric ranges are rejected") {
  auto json = production_json();
  replace_once(json, "{\"name\": \"right_turn\"}", "{\"name\": \"left_turn\"}");
  const ConfigLoadResult duplicate = load(std::move(json));
  require_failure(duplicate, ConfigErrorCategory::Semantic, ConfigErrorCode::SchemaValidation);
  CHECK(duplicate.diagnostic->schema_error == controller_config::SchemaError::DuplicateActionName);

  json = production_json();
  replace_once(json, "\"kind\": \"choice\"", "\"kind\": \"invalid\"");
  const ConfigLoadResult operand_kind = load(std::move(json));
  require_failure(operand_kind, ConfigErrorCategory::Structural, ConfigErrorCode::InvalidValue);
  CHECK(operand_kind.diagnostic->schema_error ==
        controller_config::SchemaError::UnknownOperandKind);

  json = production_json();
  replace_once(json, "\"kind\": \"number\", \"value\": 6000.0",
               "\"kind\": \"number\", \"value\": \"not-a-number\"");
  const ConfigLoadResult operand_value = load(std::move(json));
  require_failure(operand_value, ConfigErrorCategory::Structural, ConfigErrorCode::TypeMismatch);

  json = production_json();
  replace_once(json, "\"to\": 6500.0", "\"to\": 0.0");
  const ConfigLoadResult range = load(std::move(json));
  require_failure(range, ConfigErrorCategory::Semantic, ConfigErrorCode::SchemaValidation);
  CHECK(range.diagnostic->schema_error == controller_config::SchemaError::InvalidRange);
  CHECK(range.diagnostic->path == "rules[3].input");
  CHECK(range.diagnostic->message.find("from") != std::string::npos);
}

TEST_CASE("LED effects, fill directions, zones, and binding priorities are validated") {
  auto json = production_json();
  replace_once(json, "\"effect\": \"brake\"", "\"effect\": \"strobe\"");
  const ConfigLoadResult effect = load(std::move(json));
  require_failure(effect, ConfigErrorCategory::Structural, ConfigErrorCode::InvalidValue);
  CHECK(effect.diagnostic->schema_error == controller_config::SchemaError::UnknownLedEffect);

  json = production_json();
  replace_once(json, "\"direction\": \"center_out\"", "\"direction\": \"diagonal\"");
  const ConfigLoadResult direction = load(std::move(json));
  require_failure(direction, ConfigErrorCategory::Structural, ConfigErrorCode::InvalidValue);
  CHECK(direction.diagnostic->schema_error == controller_config::SchemaError::UnknownFillDirection);

  json = production_json();
  replace_once(json, "\"length\": 100", "\"length\": 101");
  const ConfigLoadResult zone = load(std::move(json));
  require_failure(zone, ConfigErrorCategory::Semantic, ConfigErrorCode::SchemaValidation);
  CHECK(zone.diagnostic->schema_error == controller_config::SchemaError::InvalidLedZone);

  json = production_json();
  replace_once(json, "\"priority\": 50", "\"priority\": 256");
  const ConfigLoadResult priority = load(std::move(json));
  require_failure(priority, ConfigErrorCategory::Semantic, ConfigErrorCode::SchemaValidation);
  CHECK(priority.diagnostic->schema_error == controller_config::SchemaError::InvalidPriority);
}

TEST_CASE("catalog-aware loading delegates operand compatibility to the action engine") {
  auto json = production_json();
  replace_once(json, "\"kind\": \"choice\"", "\"kind\": \"number\"");
  replace_once(json, "\"value\": \"left\"", "\"value\": 1.0");

  const ConfigLoadResult result = controller_config::parse_controller_config(json, kCatalog);
  require_failure(result, ConfigErrorCategory::Semantic, ConfigErrorCode::SchemaValidation);
  CHECK(result.diagnostic->schema_error == controller_config::SchemaError::TypeMismatch);
}

TEST_CASE("catalog-aware loading accepts the canonical production profile") {
  const ConfigLoadResult result =
      controller_config::parse_controller_config(production_json(), kCatalog);

  REQUIRE(result.ok());
  REQUIRE(result.configuration.has_value());
  CHECK(result.configuration->rules.size() == 5U);
  CHECK(result.configuration->action_id("left_turn") == action_engine::ActionId{1});
  CHECK(result.configuration->action_id("red_zone") == action_engine::ActionId{5});
}

TEST_CASE("semantic binding failures identify effect and fill binding paths") {
  auto json = production_json();
  replace_once(json, "{\"action\": \"left_turn\", \"effect\": \"right_turn\", \"priority\": 100}",
               "{\"action\": \"missing_effect\", \"effect\": \"right_turn\", \"priority\": 100}");
  const ConfigLoadResult effect = load(std::move(json));
  require_failure(effect, ConfigErrorCategory::Semantic, ConfigErrorCode::SchemaValidation);
  CHECK(effect.diagnostic->schema_error == controller_config::SchemaError::UnknownAction);
  CHECK(effect.diagnostic->path == "effect_bindings[0].action");

  json = production_json();
  replace_once(json, "\"action\": \"rpm_fill\",\n      \"zone\"",
               "\"action\": \"missing_fill\",\n      \"zone\"");
  const ConfigLoadResult fill = load(std::move(json));
  require_failure(fill, ConfigErrorCategory::Semantic, ConfigErrorCode::SchemaValidation);
  CHECK(fill.diagnostic->schema_error == controller_config::SchemaError::UnknownAction);
  CHECK(fill.diagnostic->path == "fill_bindings[0].action");
}

TEST_CASE("canonical production JSON matches the in-code default configuration") {
  const auto expected = controller_config::default_configuration();
  const ConfigLoadResult result = load(production_json());

  REQUIRE(result.ok());
  REQUIRE(result.configuration.has_value());
  const auto &actual = *result.configuration;
  REQUIRE(actual.actions.size() == expected.actions.size());
  REQUIRE(actual.rules.size() == expected.rules.size());
  REQUIRE(actual.effect_bindings.size() == expected.effect_bindings.size());
  REQUIRE(actual.fill_bindings.size() == expected.fill_bindings.size());
  CHECK(actual.version == expected.version);

  for (std::size_t index = 0; index < actual.actions.size(); ++index)
    CHECK(actual.actions[index].name == expected.actions[index].name);
  for (std::size_t index = 0; index < actual.rules.size(); ++index) {
    CHECK(actual.rules[index].type == expected.rules[index].type);
    CHECK(actual.rules[index].action == expected.rules[index].action);
    CHECK(actual.rules[index].signal == expected.rules[index].signal);
    CHECK(actual.rules[index].comparison == expected.rules[index].comparison);
    CHECK(actual.rules[index].freshness == expected.rules[index].freshness);
  }
  for (std::size_t index = 0; index < actual.effect_bindings.size(); ++index) {
    CHECK(actual.effect_bindings[index].action == expected.effect_bindings[index].action);
    CHECK(actual.effect_bindings[index].effect == expected.effect_bindings[index].effect);
    CHECK(actual.effect_bindings[index].priority == expected.effect_bindings[index].priority);
  }
  for (std::size_t index = 0; index < actual.fill_bindings.size(); ++index) {
    CHECK(actual.fill_bindings[index].action == expected.fill_bindings[index].action);
    CHECK(actual.fill_bindings[index].zone.start == expected.fill_bindings[index].zone.start);
    CHECK(actual.fill_bindings[index].zone.length == expected.fill_bindings[index].zone.length);
    CHECK(actual.fill_bindings[index].zone.direction ==
          expected.fill_bindings[index].zone.direction);
    CHECK(actual.fill_bindings[index].color.red == expected.fill_bindings[index].color.red);
    CHECK(actual.fill_bindings[index].color.green == expected.fill_bindings[index].color.green);
    CHECK(actual.fill_bindings[index].color.blue == expected.fill_bindings[index].color.blue);
    CHECK(actual.fill_bindings[index].priority == expected.fill_bindings[index].priority);
  }
}

TEST_CASE("the checked-in production example loads as the default profile") {
  std::ifstream input(CONTROLLER_CONFIG_EXAMPLE_PATH);
  REQUIRE(input.good());
  const std::string example((std::istreambuf_iterator<char>(input)),
                            std::istreambuf_iterator<char>());
  const ConfigLoadResult result = load(example);
  REQUIRE(result.ok());
  REQUIRE(result.configuration.has_value());
  const auto expected = controller_config::default_configuration();
  CHECK(result.configuration->version == expected.version);
  REQUIRE(result.configuration->actions.size() == expected.actions.size());
  REQUIRE(result.configuration->rules.size() == expected.rules.size());
  REQUIRE(result.configuration->effect_bindings.size() == expected.effect_bindings.size());
  REQUIRE(result.configuration->fill_bindings.size() == expected.fill_bindings.size());
  CHECK(result.configuration->actions[0].name == expected.actions[0].name);
  CHECK(result.configuration->rules[3].input.to == expected.rules[3].input.to);
  CHECK(result.configuration->effect_bindings[4].effect == expected.effect_bindings[4].effect);
  CHECK(result.configuration->fill_bindings[0].color.blue == expected.fill_bindings[0].color.blue);
}

TEST_CASE("event rules are loaded with named actions and edge semantics") {
  const std::string json = R"json({
    "version": 1,
    "actions": [{"name": "event_action"}],
    "rules": [{
      "type": "event", "action": "event_action", "signal": "vehicle.turn_state",
      "comparison": "equal", "operand": {"kind": "choice", "value": "hazard"},
      "freshness": "fresh", "edge": "becomes_false"
    }],
    "effect_bindings": [],
    "fill_bindings": []
  })json";

  const ConfigLoadResult result = load(json);
  REQUIRE(result.ok());
  const auto runtime = controller_config::to_action_engine_rule(*result.configuration, 0U);
  REQUIRE(runtime.has_value());
  const auto &event = std::get<action_engine::EventRuleConfig>(*runtime);
  CHECK(event.action == action_engine::ActionId{1});
  CHECK(event.edge == action_engine::EventEdge::BecomesFalse);
}

} // namespace
