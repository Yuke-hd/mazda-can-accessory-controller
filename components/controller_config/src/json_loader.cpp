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

[[nodiscard]] bool reject_inapplicable_fields(const cJSON &object, const std::string_view path,
                                              const std::initializer_list<std::string_view> allowed,
                                              const std::string_view rule_type,
                                              ConfigDiagnostic &error) {
  for (const cJSON *child = object.child; child != nullptr; child = child->next) {
    const std::string_view name = child->string == nullptr ? std::string_view{} : child->string;
    if (!has_allowed_name(name, allowed)) {
      set_error(error, Category::Structural, Code::UnknownField, child_path(path, name),
                "field '" + std::string(name) + "' does not apply to rule type '" +
                    std::string(rule_type) + "'");
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
  // Do not compare against static_cast<double>(INT64_MAX): that value rounds
  // to 2^63, and casting 2^63 to int64_t is undefined behaviour. The
  // exclusive upper bound is exactly representable in binary64.
  constexpr double kInt64Min = -9223372036854775808.0;
  constexpr double kInt64UpperExclusive = 9223372036854775808.0;
  if (!std::isfinite(number) || std::floor(number) != number || number < kInt64Min ||
      number >= kInt64UpperExclusive) {
    set_error(error, Category::Structural, Code::InvalidValue, std::string(path),
              "value at '" + std::string(path) + "' must be a finite integer");
    return false;
  }
  output = static_cast<std::int64_t>(number);
  return true;
}

[[nodiscard]] bool parse_color_channel(const cJSON *value, const std::string_view path, int &output,
                                       ConfigDiagnostic &error) {
  std::int64_t integer = 0;
  if (!parse_integer(value, path, integer, error))
    return false;
  if (integer < 0 || integer > 255) {
    set_error(error, Category::Structural, Code::InvalidValue, std::string(path),
              "RGB channel at '" + std::string(path) + "' must be in 0..255");
    return false;
  }
  output = static_cast<int>(integer);
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
      !reject_inapplicable_fields(object, path,
                                  {"type", "action", "signal", "input", "output", "freshness"},
                                  output.type, error))
    return false;
  if (output.type == "state" &&
      !reject_inapplicable_fields(
          object, path, {"type", "action", "signal", "comparison", "operand", "freshness"},
          output.type, error))
    return false;
  if (output.type == "sampled_state" &&
      !reject_inapplicable_fields(
          object, path,
          {"type", "action", "signal", "comparison", "operand", "freshness", "release_threshold"},
          output.type, error))
    return false;
  if (output.type == "event" &&
      !reject_inapplicable_fields(
          object, path, {"type", "action", "signal", "comparison", "operand", "freshness", "edge"},
          output.type, error))
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
  return parse_color_channel(red, child_path(path, "red"), output.red, error) &&
         parse_color_channel(green, child_path(path, "green"), output.green, error) &&
         parse_color_channel(blue, child_path(path, "blue"), output.blue, error);
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

enum class ArrayKind : std::uint8_t { Actions, Rules, EffectBindings, FillBindings };

[[nodiscard]] bool parse_array(const cJSON &root, const char *name, const ArrayKind kind,
                               const std::string_view path, Configuration &output,
                               ConfigDiagnostic &error) {
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
    if (!require_object(item, item_path, error)) {
      error.index = index;
      return false;
    }
    if (kind == ArrayKind::Actions) {
      ActionSpec action{};
      if (!parse_action(*item, item_path, action, error)) {
        error.index = index;
        return false;
      }
      output.actions.push_back(std::move(action));
    } else if (kind == ArrayKind::Rules) {
      RuleSpec rule{};
      if (!parse_rule(*item, item_path, rule, error)) {
        error.index = index;
        return false;
      }
      output.rules.push_back(std::move(rule));
    } else if (kind == ArrayKind::EffectBindings) {
      EffectBindingSpec binding{};
      if (!parse_effect_binding(*item, item_path, binding, error)) {
        error.index = index;
        return false;
      }
      output.effect_bindings.push_back(std::move(binding));
    } else {
      FillBindingSpec binding{};
      if (!parse_fill_binding(*item, item_path, binding, error)) {
        error.index = index;
        return false;
      }
      output.fill_bindings.push_back(std::move(binding));
    }
  }
  return true;
}

struct ValidationLocation final {
  std::string path{};
  std::string message{};
};

[[nodiscard]] std::string validation_item_path(const ValidationResult validation) {
  switch (validation.section) {
  case ValidationSection::Actions:
    return indexed_path("actions", validation.index);
  case ValidationSection::Rules:
    return indexed_path("rules", validation.index);
  case ValidationSection::EffectBindings:
    return indexed_path("effect_bindings", validation.index);
  case ValidationSection::FillBindings:
    return indexed_path("fill_bindings", validation.index);
  case ValidationSection::None:
    return "$";
  }
  return "$";
}

[[nodiscard]] ValidationLocation validation_location(const ValidationResult validation) {
  const std::string item = validation_item_path(validation);
  switch (validation.error) {
  case SchemaError::UnsupportedVersion:
    return {"version", "unsupported configuration version; expected version 1"};
  case SchemaError::EmptyActionName:
    return {item + ".name", "action name must not be empty"};
  case SchemaError::DuplicateActionName:
    return {item + ".name", "action name must be unique"};
  case SchemaError::ActionIdExhausted:
    return {item, "configuration declares too many actions"};
  case SchemaError::UnknownAction:
    return {item + ".action", "action reference is not declared"};
  case SchemaError::EmptySignal:
    return {item + ".signal", "signal name must not be empty"};
  case SchemaError::UnknownRuleType:
    return {item + ".type", "rule type is unsupported"};
  case SchemaError::UnknownComparison:
  case SchemaError::UnsupportedComparison:
    return {item + ".comparison", "comparison is unsupported"};
  case SchemaError::UnknownFreshness:
    return {item + ".freshness", "freshness requirement is unsupported"};
  case SchemaError::UnknownEventEdge:
    return {item + ".edge", "event edge is unsupported"};
  case SchemaError::UnknownOperandKind:
    return {item + ".operand.kind", "operand kind is unsupported"};
  case SchemaError::EmptyChoice:
    return {item + ".operand.value", "choice operand must not be empty"};
  case SchemaError::NonFiniteNumber:
    return {item, "numeric value must be finite"};
  case SchemaError::OperandComparisonMismatch:
    return {item + ".operand", "ordered comparisons require a numeric operand"};
  case SchemaError::InvalidRange:
    return {item + ".input", "input range 'from' must be less than 'to'"};
  case SchemaError::InvalidHysteresis:
    return {item + ".release_threshold",
            "release threshold must be on the release side of activation"};
  case SchemaError::UnknownSignal:
    return {item + ".signal", "signal is not present in the catalog"};
  case SchemaError::UnsupportedCapability:
    return {item + ".signal", "signal does not support the requested operation"};
  case SchemaError::TypeMismatch:
    return {item + ".operand", "operand type is incompatible with the signal"};
  case SchemaError::UnknownChoice:
    return {item + ".operand.value", "choice is not present in the signal catalog"};
  case SchemaError::UnknownLedEffect:
    return {item + ".effect", "LED effect is unsupported"};
  case SchemaError::UnknownFillDirection:
    return {item + ".zone.direction", "fill direction is unsupported"};
  case SchemaError::InvalidLedZone:
    return {item + ".zone", "LED zone must be non-empty and fit within the strip"};
  case SchemaError::InvalidRgb:
    return {item + ".color", "RGB channels must be in 0..255"};
  case SchemaError::InvalidPriority:
    return {item + ".priority", "priority must be in 0..255"};
  case SchemaError::None:
    break;
  }
  return {item, "configuration validation failed"};
}

enum class InputPreflight { Ok, EmbeddedNul, TooDeep };

[[nodiscard]] InputPreflight preflight_input(const std::string_view json,
                                             const std::size_t parse_length) {
  std::size_t depth = 0U;
  bool in_string = false;
  bool escaped = false;
  for (std::size_t index = 0U; index < parse_length; ++index) {
    const char character = json[index];
    if (character == '\0')
      return InputPreflight::EmbeddedNul;
    if (in_string) {
      if (escaped) {
        if (character == 'u' && index + 4U < parse_length && json[index + 1U] == '0' &&
            json[index + 2U] == '0' && json[index + 3U] == '0' && json[index + 4U] == '0')
          return InputPreflight::EmbeddedNul;
        escaped = false;
      } else if (character == '\\') {
        escaped = true;
      } else if (character == '"') {
        in_string = false;
      }
      continue;
    }
    if (character == '"') {
      in_string = true;
    } else if (character == '{' || character == '[') {
      ++depth;
      if (depth > kMaxControllerConfigJsonNesting)
        return InputPreflight::TooDeep;
    } else if ((character == '}' || character == ']') && depth > 0U) {
      --depth;
    }
  }
  return InputPreflight::Ok;
}

[[nodiscard]] ConfigLoadResult parse_impl(std::string_view json,
                                          const vehicle_signals::SignalCatalogView *catalog) {
  // NVS blobs commonly include one terminal C-string NUL. Accept that single
  // terminator, while rejecting embedded NUL bytes and JSON \u0000 strings.
  const std::size_t parse_length =
      json.size() > 0U && json.back() == '\0' ? json.size() - 1U : json.size();
  if (parse_length > kMaxControllerConfigJsonBytes)
    return failure(ConfigDiagnostic{Category::Parse, Code::InputTooLarge, SchemaError::None, 0U,
                                    "$", "configuration JSON exceeds the maximum supported size"});

  const InputPreflight preflight = preflight_input(json, parse_length);
  if (preflight == InputPreflight::EmbeddedNul)
    return failure(ConfigDiagnostic{Category::Parse, Code::EmbeddedNul, SchemaError::None, 0U, "$",
                                    "configuration JSON must not contain NUL characters"});
  if (preflight == InputPreflight::TooDeep)
    return failure(ConfigDiagnostic{Category::Parse, Code::NestingLimitExceeded, SchemaError::None,
                                    0U, "$",
                                    "configuration JSON exceeds the maximum nesting depth"});

  const char *parse_end = nullptr;
  cJSON *root = cJSON_ParseWithLengthOpts(json.data(), parse_length, &parse_end, 0);
  if (root == nullptr)
    return failure(ConfigDiagnostic{Category::Parse, Code::MalformedJson, SchemaError::None, 0U,
                                    "$", "malformed JSON input"});
  if (parse_end == nullptr ||
      std::any_of(parse_end, json.data() + parse_length, [](const char character) {
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
  if (!parse_array(*root, "actions", ArrayKind::Actions, "", configuration, error) ||
      !parse_array(*root, "rules", ArrayKind::Rules, "", configuration, error) ||
      !parse_array(*root, "effect_bindings", ArrayKind::EffectBindings, "", configuration, error) ||
      !parse_array(*root, "fill_bindings", ArrayKind::FillBindings, "", configuration, error))
    return failure(std::move(error));

  const ValidationResult validation =
      catalog == nullptr ? validate(configuration) : validate(configuration, *catalog);
  if (!validation.ok()) {
    ConfigDiagnostic diagnostic{};
    diagnostic.category = Category::Semantic;
    diagnostic.code = Code::SchemaValidation;
    diagnostic.schema_error = validation.error;
    diagnostic.index = validation.index;
    const ValidationLocation location = validation_location(validation);
    diagnostic.path = location.path;
    diagnostic.message = location.message;
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
