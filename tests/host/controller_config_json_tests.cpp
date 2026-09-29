#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "controller_config/json_loader.hpp"
#include "vehicle_signals/signal_catalog.hpp"
#include "vehicle_signals/signal_contracts.hpp"

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

TEST_CASE("required fields, value types, and unknown fields are rejected") {
  require_failure(load("{\"version\": 1}"), ConfigErrorCategory::Structural,
                  ConfigErrorCode::MissingField);

  auto json = production_json();
  replace_once(json, "\"name\": \"left_turn\"", "\"name\": 4");
  require_failure(load(std::move(json)), ConfigErrorCategory::Structural,
                  ConfigErrorCode::TypeMismatch);

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
