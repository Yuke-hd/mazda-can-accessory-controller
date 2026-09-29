#include "controller_config/json_loader.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include "cJSON.h"

namespace controller_config {
namespace {

using Category = ConfigErrorCategory;
using Code = ConfigErrorCode;

[[nodiscard]] ConfigLoadResult failure(ConfigDiagnostic diagnostic) {
  ConfigLoadResult result{};
  result.diagnostic = std::move(diagnostic);
  return result;
}

[[nodiscard]] ConfigLoadResult success(Configuration configuration) {
  ConfigLoadResult result{};
  result.configuration = std::move(configuration);
  return result;
}

void set_error(ConfigDiagnostic &error, const Category category, const Code code, std::string path,
               std::string message, const SchemaError schema_error = SchemaError::None,
               const std::size_t index = 0U) {
  error.category = category;
  error.code = code;
  error.schema_error = schema_error;
  error.index = index;
  error.path = std::move(path);
  error.message = std::move(message);
}

[[nodiscard]] std::string child_path(const std::string_view parent, const std::string_view child) {
  if (parent.empty())
    return std::string(child);
  return std::string(parent) + "." + std::string(child);
}

[[nodiscard]] std::string indexed_path(const std::string_view parent, const std::size_t index) {
  return std::string(parent) + "[" + std::to_string(index) + "]";
}

[[nodiscard]] bool has_allowed_name(const std::string_view name,
                                    const std::initializer_list<std::string_view> allowed) {
  return std::find(allowed.begin(), allowed.end(), name) != allowed.end();
}

[[nodiscard]] bool reject_unknown_fields(const cJSON &object, const std::string_view path,
                                         const std::initializer_list<std::string_view> allowed,
                                         ConfigDiagnostic &error) {
  for (const cJSON *child = object.child; child != nullptr; child = child->next) {
    const std::string_view name = child->string == nullptr ? std::string_view{} : child->string;
    for (const cJSON *prior = object.child; prior != child; prior = prior->next) {
      const std::string_view prior_name =
          prior->string == nullptr ? std::string_view{} : prior->string;
      if (prior_name == name) {
        set_error(error, Category::Structural, Code::InvalidValue, child_path(path, name),
                  "duplicate configuration field '" + std::string(name) + "'");
        return false;
      }
    }
    if (!has_allowed_name(name, allowed)) {
      set_error(error, Category::Structural, Code::UnknownField, child_path(path, name),
                "unknown configuration field '" + std::string(name) + "'");
      return false;
    }
  }
  return true;
}

[[nodiscard]] const cJSON *required_field(const cJSON &object, const char *name,
                                          const std::string_view path, ConfigDiagnostic &error) {
  const cJSON *value = cJSON_GetObjectItemCaseSensitive(&object, name);
  if (value == nullptr) {
    set_error(error, Category::Structural, Code::MissingField, child_path(path, name),
              "missing required field '" + std::string(name) + "'");
  }
  return value;
}

[[nodiscard]] const cJSON *optional_field(const cJSON &object, const char *name) {
  return cJSON_GetObjectItemCaseSensitive(&object, name);
}

[[nodiscard]] bool require_object(const cJSON *value, const std::string_view path,
                                  ConfigDiagnostic &error) {
  if (value != nullptr && cJSON_IsObject(value))
    return true;
  set_error(error, Category::Structural, Code::TypeMismatch, std::string(path),
            "expected an object at '" + std::string(path) + "'");
  return false;
}

[[nodiscard]] bool require_array(const cJSON *value, const std::string_view path,
                                 ConfigDiagnostic &error) {
  if (value != nullptr && cJSON_IsArray(value))
    return true;
  set_error(error, Category::Structural, Code::TypeMismatch, std::string(path),
            "expected an array at '" + std::string(path) + "'");
  return false;
}

[[nodiscard]] bool parse_string(const cJSON *value, const std::string_view path,
                                std::string &output, ConfigDiagnostic &error) {
  if (value == nullptr || !cJSON_IsString(value) || value->valuestring == nullptr) {
    set_error(error, Category::Structural, Code::TypeMismatch, std::string(path),
              "expected a string at '" + std::string(path) + "'");
    return false;
  }
  output = value->valuestring;
  return true;
}

[[nodiscard]] bool parse_finite_number(const cJSON *value, const std::string_view path,
                                       float &output, ConfigDiagnostic &error) {
  if (value == nullptr || !cJSON_IsNumber(value)) {
    set_error(error, Category::Structural, Code::TypeMismatch, std::string(path),
              "expected a number at '" + std::string(path) + "'");
    return false;
  }
  const double number = value->valuedouble;
  output = static_cast<float>(number);
  if (!std::isfinite(number) || !std::isfinite(output)) {
    set_error(error, Category::Structural, Code::InvalidValue, std::string(path),
              "number at '" + std::string(path) + "' must be finite", SchemaError::NonFiniteNumber);
    return false;
  }
  return true;
}

[[nodiscard]] bool parse_integer(const cJSON *value, const std::string_view path,
                                 std::int64_t &output, ConfigDiagnostic &error) {
  if (value == nullptr || !cJSON_IsNumber(value)) {
    set_error(error, Category::Structural, Code::TypeMismatch, std::string(path),
              "expected an integer at '" + std::string(path) + "'");
    return false;
  }
  const double number = value->valuedouble;
  if (!std::isfinite(number) || std::floor(number) != number ||
      number < static_cast<double>(std::numeric_limits<std::int64_t>::min()) ||
      number > static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
    set_error(error, Category::Structural, Code::InvalidValue, std::string(path),
              "value at '" + std::string(path) + "' must be a finite integer");
    return false;
  }
  output = static_cast<std::int64_t>(number);
  return true;
}

[[nodiscard]] bool parse_priority(const cJSON *value, const std::string_view path, int &output,
                                  ConfigDiagnostic &error) {
  std::int64_t integer = 0;
  if (!parse_integer(value, path, integer, error))
    return false;
  if (integer < static_cast<std::int64_t>(std::numeric_limits<int>::min()) ||
      integer > static_cast<std::int64_t>(std::numeric_limits<int>::max())) {
    set_error(error, Category::Structural, Code::InvalidValue, std::string(path),
              "integer at '" + std::string(path) + "' is outside the supported range");
    return false;
  }
  output = static_cast<int>(integer);
  return true;
}

[[nodiscard]] bool parse_bool(const cJSON *value, const std::string_view path, bool &output,
                              ConfigDiagnostic &error) {
  if (value == nullptr || !cJSON_IsBool(value)) {
    set_error(error, Category::Structural, Code::TypeMismatch, std::string(path),
              "expected a boolean at '" + std::string(path) + "'");
    return false;
  }
  output = cJSON_IsTrue(value) != 0;
  return true;
}

[[nodiscard]] bool supported_rule_type(const std::string_view type) {
  return type == "state" || type == "sampled_state" || type == "event" || type == "range";
}

[[nodiscard]] bool parse_operand(const cJSON &object, const std::string_view path,
                                 OperandSpec &output, ConfigDiagnostic &error) {
  if (!reject_unknown_fields(object, path, {"kind", "value"}, error))
    return false;
  const cJSON *kind_value = required_field(object, "kind", path, error);
  const cJSON *operand_value = required_field(object, "value", path, error);
  if (kind_value == nullptr || operand_value == nullptr)
    return false;
  std::string kind;
  if (!parse_string(kind_value, child_path(path, "kind"), kind, error))
    return false;
  const auto operand_kind = operand_kind_from_name(kind);
  if (!operand_kind.has_value()) {
    set_error(error, Category::Structural, Code::InvalidValue, child_path(path, "kind"),
              "unsupported operand kind '" + kind + "'", SchemaError::UnknownOperandKind);
    return false;
  }
  switch (*operand_kind) {
  case OperandKind::Boolean:
    if (!parse_bool(operand_value, child_path(path, "value"), output.boolean_value, error))
      return false;
    output.kind = OperandKind::Boolean;
    return true;
  case OperandKind::Number:
    if (!parse_finite_number(operand_value, child_path(path, "value"), output.number_value, error))
      return false;
    output.kind = OperandKind::Number;
    return true;
  case OperandKind::Choice:
    output.kind = OperandKind::Choice;
    return parse_string(operand_value, child_path(path, "value"), output.choice_value, error);
  }
  return false;
}

[[nodiscard]] bool parse_numeric_range(const cJSON &object, const std::string_view path,
                                       action_engine::NumericRange &output,
                                       ConfigDiagnostic &error) {
  if (!reject_unknown_fields(object, path, {"from", "to"}, error))
    return false;
  const cJSON *from = required_field(object, "from", path, error);
  const cJSON *to = required_field(object, "to", path, error);
  if (from == nullptr || to == nullptr)
    return false;
  return parse_finite_number(from, child_path(path, "from"), output.from, error) &&
         parse_finite_number(to, child_path(path, "to"), output.to, error);
}

[[nodiscard]] bool parse_rule(const cJSON &object, const std::string_view path, RuleSpec &output,
                              ConfigDiagnostic &error) {
  if (!reject_unknown_fields(object, path,
                             {"type", "action", "signal", "comparison", "operand", "freshness",
                              "edge", "release_threshold", "input", "output"},
                             error))
    return false;
  const cJSON *type_value = required_field(object, "type", path, error);
  const cJSON *action_value = required_field(object, "action", path, error);
  const cJSON *signal_value = required_field(object, "signal", path, error);
  if (type_value == nullptr || action_value == nullptr || signal_value == nullptr)
    return false;
  if (!parse_string(type_value, child_path(path, "type"), output.type, error) ||
      !parse_string(action_value, child_path(path, "action"), output.action, error) ||
      !parse_string(signal_value, child_path(path, "signal"), output.signal, error))
    return false;
  if (!supported_rule_type(output.type)) {
    set_error(error, Category::Structural, Code::InvalidValue, child_path(path, "type"),
              "unsupported rule type '" + output.type + "'", SchemaError::UnknownRuleType);
    return false;
  }
  if (output.type == "range" &&
      !reject_unknown_fields(object, path,
                             {"type", "action", "signal", "input", "output", "freshness"}, error))
    return false;
  if (output.type == "state" &&
      !reject_unknown_fields(
          object, path, {"type", "action", "signal", "comparison", "operand", "freshness"}, error))
    return false;
  if (output.type == "sampled_state" &&
      !reject_unknown_fields(
          object, path,
          {"type", "action", "signal", "comparison", "operand", "freshness", "release_threshold"},
          error))
    return false;
  if (output.type == "event" &&
      !reject_unknown_fields(
          object, path, {"type", "action", "signal", "comparison", "operand", "freshness", "edge"},
          error))
    return false;
  const bool range = output.type == "range";
  const cJSON *freshness_value = required_field(object, "freshness", path, error);
  if (freshness_value == nullptr ||
      !parse_string(freshness_value, child_path(path, "freshness"), output.freshness, error))
    return false;
  if (!freshness_from_name(output.freshness).has_value()) {
    set_error(error, Category::Structural, Code::InvalidValue, child_path(path, "freshness"),
              "unsupported freshness requirement '" + output.freshness + "'",
              SchemaError::UnknownFreshness);
    return false;
  }
  if (range) {
    const cJSON *input = required_field(object, "input", path, error);
    const cJSON *output_range = required_field(object, "output", path, error);
    if (!require_object(input, child_path(path, "input"), error) ||
        !require_object(output_range, child_path(path, "output"), error))
      return false;
    return parse_numeric_range(*input, child_path(path, "input"), output.input, error) &&
           parse_numeric_range(*output_range, child_path(path, "output"), output.output, error);
  }

  const cJSON *comparison = required_field(object, "comparison", path, error);
  const cJSON *operand = required_field(object, "operand", path, error);
  if (comparison == nullptr || operand == nullptr)
    return false;
  if (!parse_string(comparison, child_path(path, "comparison"), output.comparison, error))
    return false;
  if (!comparison_from_name(output.comparison).has_value()) {
    set_error(error, Category::Structural, Code::InvalidValue, child_path(path, "comparison"),
              "unsupported comparison '" + output.comparison + "'", SchemaError::UnknownComparison);
    return false;
  }
  if (!require_object(operand, child_path(path, "operand"), error) ||
      !parse_operand(*operand, child_path(path, "operand"), output.operand, error))
    return false;
  if (output.type == "event") {
    const cJSON *edge = required_field(object, "edge", path, error);
    if (edge == nullptr || !parse_string(edge, child_path(path, "edge"), output.edge, error))
      return false;
    if (!event_edge_from_name(output.edge).has_value()) {
      set_error(error, Category::Structural, Code::InvalidValue, child_path(path, "edge"),
                "unsupported event edge '" + output.edge + "'", SchemaError::UnknownEventEdge);
      return false;
    }
  }
  const cJSON *release = optional_field(object, "release_threshold");
  if (release != nullptr && !parse_finite_number(release, child_path(path, "release_threshold"),
                                                 output.release_threshold.emplace(), error))
    return false;
  return true;
}

[[nodiscard]] bool parse_action(const cJSON &object, const std::string_view path,
                                ActionSpec &output, ConfigDiagnostic &error) {
  if (!reject_unknown_fields(object, path, {"name"}, error))
    return false;
  const cJSON *name = required_field(object, "name", path, error);
  return name != nullptr && parse_string(name, child_path(path, "name"), output.name, error);
}

[[nodiscard]] bool parse_effect_binding(const cJSON &object, const std::string_view path,
                                        EffectBindingSpec &output, ConfigDiagnostic &error) {
  if (!reject_unknown_fields(object, path, {"action", "effect", "priority"}, error))
    return false;
  const cJSON *action = required_field(object, "action", path, error);
  const cJSON *effect = required_field(object, "effect", path, error);
  const cJSON *priority = required_field(object, "priority", path, error);
  if (action == nullptr || effect == nullptr || priority == nullptr ||
      !parse_string(action, child_path(path, "action"), output.action, error) ||
      !parse_string(effect, child_path(path, "effect"), output.effect, error) ||
      !parse_priority(priority, child_path(path, "priority"), output.priority, error))
    return false;
  if (!led_effect_from_name(output.effect).has_value()) {
    set_error(error, Category::Structural, Code::InvalidValue, child_path(path, "effect"),
              "unsupported LED effect '" + output.effect + "'", SchemaError::UnknownLedEffect);
    return false;
  }
  return true;
}

[[nodiscard]] bool parse_zone(const cJSON &object, const std::string_view path, ZoneSpec &output,
                              ConfigDiagnostic &error) {
  if (!reject_unknown_fields(object, path, {"start", "length", "direction"}, error))
    return false;
  const cJSON *start = required_field(object, "start", path, error);
  const cJSON *length = required_field(object, "length", path, error);
  const cJSON *direction = required_field(object, "direction", path, error);
  if (start == nullptr || length == nullptr || direction == nullptr)
    return false;
  std::int64_t start_value = 0;
  std::int64_t length_value = 0;
  if (!parse_integer(start, child_path(path, "start"), start_value, error) ||
      !parse_integer(length, child_path(path, "length"), length_value, error) ||
      !parse_string(direction, child_path(path, "direction"), output.direction, error))
    return false;
  if (start_value < 0 || length_value < 0 ||
      static_cast<std::uint64_t>(start_value) > std::numeric_limits<std::size_t>::max() ||
      static_cast<std::uint64_t>(length_value) > std::numeric_limits<std::size_t>::max()) {
    set_error(error, Category::Structural, Code::InvalidValue, std::string(path),
              "zone coordinates must be non-negative integers", SchemaError::InvalidLedZone);
    return false;
  }
  output.start = static_cast<std::size_t>(start_value);
  output.length = static_cast<std::size_t>(length_value);
  if (!fill_direction_from_name(output.direction).has_value()) {
    set_error(error, Category::Structural, Code::InvalidValue, child_path(path, "direction"),
              "unsupported fill direction '" + output.direction + "'",
              SchemaError::UnknownFillDirection);
    return false;
  }
  return true;
}

[[nodiscard]] bool parse_color(const cJSON &object, const std::string_view path, RgbSpec &output,
                               ConfigDiagnostic &error) {
  if (!reject_unknown_fields(object, path, {"red", "green", "blue"}, error))
    return false;
  const cJSON *red = required_field(object, "red", path, error);
  const cJSON *green = required_field(object, "green", path, error);
  const cJSON *blue = required_field(object, "blue", path, error);
  if (red == nullptr || green == nullptr || blue == nullptr)
    return false;
  return parse_priority(red, child_path(path, "red"), output.red, error) &&
         parse_priority(green, child_path(path, "green"), output.green, error) &&
         parse_priority(blue, child_path(path, "blue"), output.blue, error);
}

[[nodiscard]] bool parse_fill_binding(const cJSON &object, const std::string_view path,
                                      FillBindingSpec &output, ConfigDiagnostic &error) {
  if (!reject_unknown_fields(object, path, {"action", "zone", "color", "priority"}, error))
    return false;
  const cJSON *action = required_field(object, "action", path, error);
  const cJSON *zone = required_field(object, "zone", path, error);
  const cJSON *color = required_field(object, "color", path, error);
  const cJSON *priority = required_field(object, "priority", path, error);
  if (action == nullptr || zone == nullptr || color == nullptr || priority == nullptr ||
      !parse_string(action, child_path(path, "action"), output.action, error) ||
      !parse_priority(priority, child_path(path, "priority"), output.priority, error) ||
      !require_object(zone, child_path(path, "zone"), error) ||
      !parse_zone(*zone, child_path(path, "zone"), output.zone, error) ||
      !require_object(color, child_path(path, "color"), error) ||
      !parse_color(*color, child_path(path, "color"), output.color, error))
    return false;
  return true;
}

[[nodiscard]] bool parse_array(const cJSON &root, const char *name, const std::string_view path,
                               Configuration &output, ConfigDiagnostic &error) {
  const cJSON *array = required_field(root, name, "", error);
  if (array == nullptr)
    return false;
  const std::string array_path = child_path(path, name);
  if (!require_array(array, array_path, error))
    return false;
  const int count = cJSON_GetArraySize(array);
  for (int raw_index = 0; raw_index < count; ++raw_index) {
    const std::size_t index = static_cast<std::size_t>(raw_index);
    const std::string item_path = indexed_path(array_path, index);
    const cJSON *item = cJSON_GetArrayItem(array, raw_index);
    if (!require_object(item, item_path, error))
      return false;
    if (std::string_view(name) == "actions") {
      ActionSpec action{};
      if (!parse_action(*item, item_path, action, error))
        return false;
      output.actions.push_back(std::move(action));
    } else if (std::string_view(name) == "rules") {
      RuleSpec rule{};
      if (!parse_rule(*item, item_path, rule, error))
        return false;
      output.rules.push_back(std::move(rule));
    } else if (std::string_view(name) == "effect_bindings") {
      EffectBindingSpec binding{};
      if (!parse_effect_binding(*item, item_path, binding, error))
        return false;
      output.effect_bindings.push_back(std::move(binding));
    } else {
      FillBindingSpec binding{};
      if (!parse_fill_binding(*item, item_path, binding, error))
        return false;
      output.fill_bindings.push_back(std::move(binding));
    }
  }
  return true;
}

[[nodiscard]] std::string validation_path(const Configuration &configuration,
                                          const ValidationResult validation) {
  if (validation.error == SchemaError::UnsupportedVersion)
    return "version";
  if (validation.error == SchemaError::EmptyActionName ||
      validation.error == SchemaError::DuplicateActionName ||
      validation.error == SchemaError::ActionIdExhausted)
    return indexed_path("actions", validation.index);
  if (validation.error == SchemaError::UnknownLedEffect)
    return indexed_path("effect_bindings", validation.index) + ".effect";
  if (validation.error == SchemaError::UnknownFillDirection ||
      validation.error == SchemaError::InvalidLedZone ||
      validation.error == SchemaError::InvalidRgb)
    return indexed_path("fill_bindings", validation.index);
  if (validation.error == SchemaError::InvalidPriority) {
    if (validation.index < configuration.effect_bindings.size() &&
        (configuration.effect_bindings[validation.index].priority < 0 ||
         configuration.effect_bindings[validation.index].priority > 255))
      return indexed_path("effect_bindings", validation.index) + ".priority";
    return indexed_path("fill_bindings", validation.index) + ".priority";
  }
  if (validation.error == SchemaError::UnknownAction) {
    if (validation.index < configuration.rules.size() &&
        !configuration.action_id(configuration.rules[validation.index].action).has_value())
      return indexed_path("rules", validation.index) + ".action";
    if (validation.index < configuration.effect_bindings.size() &&
        !configuration.action_id(configuration.effect_bindings[validation.index].action)
             .has_value())
      return indexed_path("effect_bindings", validation.index) + ".action";
    return indexed_path("fill_bindings", validation.index) + ".action";
  }
  return indexed_path("rules", validation.index);
}

[[nodiscard]] ConfigLoadResult parse_impl(std::string_view json,
                                          const vehicle_signals::SignalCatalogView *catalog) {
  std::string source(json);
  const char *parse_end = nullptr;
  cJSON *root = cJSON_ParseWithLengthOpts(source.data(), source.size(), &parse_end, 0);
  if (root == nullptr)
    return failure(ConfigDiagnostic{Category::Parse, Code::MalformedJson, SchemaError::None, 0U,
                                    "$", "malformed JSON input"});
  if (parse_end == nullptr ||
      std::any_of(parse_end, source.c_str() + source.size(), [](const char character) {
        return character != ' ' && character != '\t' && character != '\r' && character != '\n';
      })) {
    cJSON_Delete(root);
    return failure(ConfigDiagnostic{Category::Parse, Code::MalformedJson, SchemaError::None, 0U,
                                    "$", "trailing data after the JSON document"});
  }
  struct RootGuard final {
    cJSON *value;
    ~RootGuard() { cJSON_Delete(value); }
  } guard{root};

  if (!cJSON_IsObject(root))
    return failure(ConfigDiagnostic{Category::Structural, Code::RootTypeMismatch, SchemaError::None,
                                    0U, "$", "configuration root must be an object"});

  ConfigDiagnostic error{};
  if (!reject_unknown_fields(
          *root, "", {"version", "actions", "rules", "effect_bindings", "fill_bindings"}, error))
    return failure(std::move(error));

  const cJSON *version = required_field(*root, "version", "", error);
  if (version == nullptr)
    return failure(std::move(error));
  std::int64_t version_value = 0;
  if (!parse_integer(version, "version", version_value, error))
    return failure(std::move(error));
  if (version_value < 0 || version_value > std::numeric_limits<std::uint32_t>::max()) {
    set_error(error, Category::Structural, Code::InvalidValue, "version",
              "version must be a non-negative 32-bit integer");
    return failure(std::move(error));
  }

  Configuration configuration{};
  configuration.version = static_cast<std::uint32_t>(version_value);
  if (!parse_array(*root, "actions", "", configuration, error) ||
      !parse_array(*root, "rules", "", configuration, error) ||
      !parse_array(*root, "effect_bindings", "", configuration, error) ||
      !parse_array(*root, "fill_bindings", "", configuration, error))
    return failure(std::move(error));

  const ValidationResult validation =
      catalog == nullptr ? validate(configuration) : validate(configuration, *catalog);
  if (!validation.ok()) {
    ConfigDiagnostic diagnostic{};
    diagnostic.category = Category::Semantic;
    diagnostic.code = Code::SchemaValidation;
    diagnostic.schema_error = validation.error;
    diagnostic.index = validation.index;
    diagnostic.path = validation_path(configuration, validation);
    diagnostic.message = "configuration validation failed at " + diagnostic.path;
    return failure(std::move(diagnostic));
  }
  return success(std::move(configuration));
}

} // namespace

ConfigLoadResult parse_controller_config(const std::string_view json) {
  return parse_impl(json, nullptr);
}

ConfigLoadResult parse_controller_config(const std::string_view json,
                                         const vehicle_signals::SignalCatalogView catalog) {
  return parse_impl(json, &catalog);
}

} // namespace controller_config
