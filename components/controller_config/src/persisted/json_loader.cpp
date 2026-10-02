#include "controller_config/persisted/json_loader.hpp"
#include "controller_config/persisted/names.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "cJSON.h"

namespace controller_config::persisted {
namespace {

using Category = ConfigErrorCategory;
using Code = ConfigErrorCode;

// cJSON reports allocation failure through the same null result as malformed input.
thread_local bool *cjson_allocation_failure = nullptr;

void *tracking_malloc(const std::size_t size) {
  void *allocation = std::malloc(size);
  if (allocation == nullptr && cjson_allocation_failure != nullptr)
    *cjson_allocation_failure = true;
  return allocation;
}

void tracking_free(void *const pointer) { std::free(pointer); }

struct CJsonAllocationGuard final {
  bool failed{false};

  CJsonAllocationGuard() {
    cJSON_Hooks hooks{tracking_malloc, tracking_free};
    cJSON_InitHooks(&hooks);
    cjson_allocation_failure = &failed;
  }

  ~CJsonAllocationGuard() {
    cJSON_InitHooks(nullptr);
    cjson_allocation_failure = nullptr;
  }
};

[[nodiscard]] ConfigLoadResult failure(ConfigDiagnostic diagnostic) {
  ConfigLoadResult result{};
  result.diagnostic = std::move(diagnostic);
  return result;
}

[[nodiscard]] ConfigLoadResult success(ControllerConfig configuration) {
  ConfigLoadResult result{};
  result.configuration = std::move(configuration);
  return result;
}

void set_error(ConfigDiagnostic &error, const Category category, const Code code, std::string path,
               std::string message, const ValidationError schema_error = ValidationError::None,
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
              "number at '" + std::string(path) + "' must be finite",
              ValidationError::InvalidOperand);
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

// cJSON keeps numbers as doubles, so recover this field's original token
// before accepting it. A rounded fractional/large input must not silently
// become a different duration. Number order in the tree matches source order.
[[nodiscard]] bool number_index(const cJSON &node, const cJSON *target,
                                std::size_t &index) noexcept {
  if (&node == target)
    return true;
  if (cJSON_IsNumber(&node))
    ++index;
  for (const cJSON *child = node.child; child != nullptr; child = child->next)
    if (number_index(*child, target, index))
      return true;
  return false;
}

[[nodiscard]] std::string_view number_token(const std::string_view json,
                                            std::size_t number) noexcept {
  for (std::size_t index = 0; index < json.size(); ++index) {
    if (json[index] == '"') {
      while (++index < json.size() && json[index] != '"')
        if (json[index] == '\\')
          ++index;
      continue;
    }
    if (json[index] != '-' && (json[index] < '0' || json[index] > '9'))
      continue;
    const auto start = index;
    while (index < json.size() && json[index] != ',' && json[index] != ']' && json[index] != '}' &&
           json[index] != ' ' && json[index] != '\t' && json[index] != '\r' && json[index] != '\n')
      ++index;
    if (number-- == 0)
      return json.substr(start, index - start);
    --index;
  }
  return {};
}

[[nodiscard]] bool parse_duration(const cJSON *value, const cJSON &root,
                                  const std::string_view json, const std::string_view path,
                                  Integer &output, ConfigDiagnostic &error) {
  if (value == nullptr || !cJSON_IsNumber(value)) {
    set_error(error, Category::Structural, Code::TypeMismatch, std::string(path),
              "duration_ms must be an integer number");
    return false;
  }
  std::size_t index = 0;
  if (!number_index(root, value, index))
    return false;
  const auto token = number_token(json, index);
  std::from_chars_result parsed{};
  if (!token.empty())
    parsed = std::from_chars(token.data(), token.data() + token.size(), output);
  if (token.empty() || parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size()) {
    set_error(error, Category::Structural, Code::InvalidValue, std::string(path),
              "duration_ms must use an integer token in 1..9007199254740991",
              ValidationError::InvalidDuration);
    return false;
  }
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

template <typename Enum>
[[nodiscard]] bool parse_enum(const cJSON *value, const std::string_view path, Enum &output,
                              ConfigDiagnostic &error) {
  std::string name;
  if (!parse_string(value, path, name, error))
    return false;
  const auto parsed = parse_name<Enum>(name);
  if (!parsed.has_value()) {
    set_error(error, Category::Structural, Code::InvalidValue, std::string(path),
              "unsupported enum value '" + name + "'");
    return false;
  }
  output = *parsed;
  return true;
}

[[nodiscard]] bool parse_operand(const cJSON &object, const std::string_view path, Operand &output,
                                 ConfigDiagnostic &error) {
  if (!reject_unknown_fields(object, path, {"boolean", "number", "choice"}, error))
    return false;
  if (object.child == nullptr || object.child->next != nullptr) {
    set_error(error, Category::Structural, Code::InvalidValue, std::string(path),
              "operand must contain exactly one of boolean, number or choice");
    return false;
  }
  const auto &value = *object.child;
  const std::string value_path = child_path(path, value.string);
  if (std::string_view(value.string) == "boolean") {
    BooleanOperand operand{};
    if (!parse_bool(&value, value_path, operand.value, error))
      return false;
    output = operand;
  } else if (std::string_view(value.string) == "number") {
    NumberOperand operand{};
    if (!parse_finite_number(&value, value_path, operand.value, error))
      return false;
    output = operand;
  } else {
    ChoiceOperand operand{};
    if (!parse_string(&value, value_path, operand.key, error))
      return false;
    output = std::move(operand);
  }
  return true;
}

[[nodiscard]] bool parse_numeric_range(const cJSON *object, const std::string_view path,
                                       action_engine::NumericRange &output,
                                       ConfigDiagnostic &error) {
  if (!require_object(object, path, error) ||
      !reject_unknown_fields(*object, path, {"from", "to"}, error))
    return false;
  const cJSON *from = required_field(*object, "from", path, error);
  if (from == nullptr)
    return false;
  const cJSON *to = required_field(*object, "to", path, error);
  return to != nullptr && parse_finite_number(from, child_path(path, "from"), output.from, error) &&
         parse_finite_number(to, child_path(path, "to"), output.to, error);
}

[[nodiscard]] bool parse_condition(const cJSON &object, const std::string_view path,
                                   Condition &output, ConfigDiagnostic &error) {
  const cJSON *signal = required_field(object, "signal_key", path, error);
  if (signal == nullptr ||
      !parse_string(signal, child_path(path, "signal_key"), output.signal_key, error))
    return false;
  const cJSON *comparison = required_field(object, "comparison", path, error);
  if (comparison == nullptr ||
      !parse_enum(comparison, child_path(path, "comparison"), output.comparison, error))
    return false;
  const cJSON *operand = required_field(object, "operand", path, error);
  return operand != nullptr && require_object(operand, child_path(path, "operand"), error) &&
         parse_operand(*operand, child_path(path, "operand"), output.operand, error);
}

template <typename RuleAlternative>
[[nodiscard]] bool parse_rule_body(const cJSON &object, const std::string_view path,
                                   RuleAlternative &output, ConfigDiagnostic &error) {
  const cJSON *action = required_field(object, "action", path, error);
  if (action == nullptr || !parse_string(action, child_path(path, "action"), output.action, error))
    return false;
  if (const cJSON *freshness = optional_field(object, "freshness");
      freshness != nullptr &&
      !parse_enum(freshness, child_path(path, "freshness"), output.freshness, error))
    return false;
  if constexpr (std::is_same_v<RuleAlternative, RangeRule>) {
    const cJSON *signal = required_field(object, "signal_key", path, error);
    if (signal == nullptr ||
        !parse_string(signal, child_path(path, "signal_key"), output.signal_key, error))
      return false;
    const cJSON *input = required_field(object, "input", path, error);
    if (input == nullptr ||
        !parse_numeric_range(input, child_path(path, "input"), output.input, error))
      return false;
    const cJSON *range = required_field(object, "output", path, error);
    return range != nullptr &&
           parse_numeric_range(range, child_path(path, "output"), output.output, error);
  } else {
    if (!parse_condition(object, path, output.condition, error))
      return false;
    if constexpr (std::is_same_v<RuleAlternative, EventRule>) {
      const cJSON *edge = required_field(object, "edge", path, error);
      if (edge == nullptr || !parse_enum(edge, child_path(path, "edge"), output.edge, error))
        return false;
    }
    if constexpr (std::is_same_v<RuleAlternative, SampledStateRule>) {
      if (const cJSON *release = optional_field(object, "release_threshold");
          release != nullptr && !parse_finite_number(release, child_path(path, "release_threshold"),
                                                     output.release_threshold.emplace(), error))
        return false;
    }
    return true;
  }
}

template <typename Alternative>
[[nodiscard]] bool parse_rule_alternative(const cJSON &object, const std::string_view path,
                                          Rule &output, ConfigDiagnostic &error,
                                          std::initializer_list<std::string_view> fields) {
  if (!reject_unknown_fields(object, path, fields, error))
    return false;
  Alternative rule{};
  if (!parse_rule_body(object, path, rule, error))
    return false;
  output = std::move(rule);
  return true;
}

[[nodiscard]] bool parse_rule(const cJSON &object, const std::string_view path, Rule &output,
                              ConfigDiagnostic &error) {
  const cJSON *type = required_field(object, "type", path, error);
  RuleType kind{};
  if (type == nullptr || !parse_enum(type, child_path(path, "type"), kind, error))
    return false;
  switch (kind) {
  case RuleType::State:
    return parse_rule_alternative<StateRule>(
        object, path, output, error,
        {"type", "action", "signal_key", "comparison", "operand", "freshness"});
  case RuleType::SampledState:
    return parse_rule_alternative<SampledStateRule>(object, path, output, error,
                                                    {"type", "action", "signal_key", "comparison",
                                                     "operand", "freshness", "release_threshold"});
  case RuleType::Event:
    return parse_rule_alternative<EventRule>(
        object, path, output, error,
        {"type", "action", "signal_key", "comparison", "operand", "freshness", "edge"});
  case RuleType::Range:
    return parse_rule_alternative<RangeRule>(
        object, path, output, error,
        {"type", "action", "signal_key", "input", "output", "freshness"});
  }
  return false;
}

[[nodiscard]] bool parse_action(const cJSON &object, const std::string_view path, Action &output,
                                ConfigDiagnostic &error) {
  if (!reject_unknown_fields(object, path, {"name"}, error))
    return false;
  const cJSON *name = required_field(object, "name", path, error);
  return name != nullptr && parse_string(name, child_path(path, "name"), output.name, error);
}

[[nodiscard]] bool parse_zone(const cJSON *object, const std::string_view path, LedZone &output,
                              ConfigDiagnostic &error) {
  if (!require_object(object, path, error) ||
      !reject_unknown_fields(*object, path, {"start", "length", "direction"}, error))
    return false;
  const cJSON *start = required_field(*object, "start", path, error);
  if (start == nullptr || !parse_integer(start, child_path(path, "start"), output.start, error))
    return false;
  const cJSON *length = required_field(*object, "length", path, error);
  if (length == nullptr || !parse_integer(length, child_path(path, "length"), output.length, error))
    return false;
  const cJSON *direction = required_field(*object, "direction", path, error);
  return direction != nullptr &&
         parse_enum(direction, child_path(path, "direction"), output.direction, error);
}

[[nodiscard]] bool parse_color(const cJSON *object, const std::string_view path, Rgb &output,
                               ConfigDiagnostic &error) {
  if (!require_object(object, path, error) ||
      !reject_unknown_fields(*object, path, {"red", "green", "blue"}, error))
    return false;
  const cJSON *red = required_field(*object, "red", path, error);
  if (red == nullptr || !parse_integer(red, child_path(path, "red"), output.red, error))
    return false;
  const cJSON *green = required_field(*object, "green", path, error);
  if (green == nullptr || !parse_integer(green, child_path(path, "green"), output.green, error))
    return false;
  const cJSON *blue = required_field(*object, "blue", path, error);
  return blue != nullptr && parse_integer(blue, child_path(path, "blue"), output.blue, error);
}

template <typename Binding>
[[nodiscard]] bool parse_binding_body(const cJSON &object, const std::string_view path,
                                      Binding &output, ConfigDiagnostic &error) {
  const cJSON *action = required_field(object, "action", path, error);
  if (action == nullptr || !parse_string(action, child_path(path, "action"), output.action, error))
    return false;
  if (const cJSON *priority = optional_field(object, "priority");
      priority != nullptr &&
      !parse_integer(priority, child_path(path, "priority"), output.priority, error))
    return false;
  if constexpr (std::is_same_v<Binding, LedEffectBinding>) {
    const cJSON *effect = required_field(object, "effect", path, error);
    return effect != nullptr &&
           parse_enum(effect, child_path(path, "effect"), output.effect, error);
  } else {
    const cJSON *zone = required_field(object, "zone", path, error);
    if (zone == nullptr || !parse_zone(zone, child_path(path, "zone"), output.zone, error))
      return false;
    const cJSON *color = required_field(object, "color", path, error);
    return color != nullptr && parse_color(color, child_path(path, "color"), output.color, error);
  }
}

[[nodiscard]] bool parse_output(const cJSON &object, const std::string_view path,
                                OutputBinding &output, ConfigDiagnostic &error, const cJSON &root,
                                const std::string_view json) {
  const cJSON *type = required_field(object, "type", path, error);
  OutputType kind{};
  if (type == nullptr || !parse_enum(type, child_path(path, "type"), kind, error))
    return false;
  if (kind == OutputType::LedEffect) {
    if (!reject_unknown_fields(object, path, {"type", "action", "effect", "priority"}, error))
      return false;
    LedEffectBinding binding{};
    if (!parse_binding_body(object, path, binding, error))
      return false;
    output = std::move(binding);
  } else if (kind == OutputType::LedFill) {
    if (!reject_unknown_fields(object, path, {"type", "action", "zone", "color", "priority"},
                               error))
      return false;
    LedFillBinding binding{};
    if (!parse_binding_body(object, path, binding, error))
      return false;
    output = std::move(binding);
  } else {
    if (!reject_unknown_fields(
            object, path, {"type", "action", "zone", "color", "duration_ms", "priority"}, error))
      return false;
    LedTransientBinding binding{};
    if (!parse_binding_body(object, path, binding, error))
      return false;
    const cJSON *duration = required_field(object, "duration_ms", path, error);
    if (duration == nullptr ||
        !parse_duration(duration, root, json, child_path(path, "duration_ms"), binding.duration_ms,
                        error))
      return false;
    output = std::move(binding);
  }
  return true;
}

template <typename Entry, typename Parser>
[[nodiscard]] bool parse_array(const cJSON &root, const char *name, std::vector<Entry> &output,
                               Parser parser, ConfigDiagnostic &error) {
  const cJSON *array = optional_field(root, name);
  if (array == nullptr)
    return true;
  if (!require_array(array, name, error))
    return false;
  std::size_t index = 0;
  for (const cJSON *item = array->child; item != nullptr; item = item->next, ++index) {
    const std::string path = indexed_path(name, index);
    Entry entry{};
    if (!require_object(item, path, error) || !parser(*item, path, entry, error)) {
      error.index = index;
      return false;
    }
    output.push_back(std::move(entry));
  }
  return true;
}

[[nodiscard]] std::string validation_path(const ValidationResult result) {
  switch (result.section) {
  case ConfigSection::Actions:
    return indexed_path("actions", result.index);
  case ConfigSection::Rules:
    return indexed_path("rules", result.index);
  case ConfigSection::Outputs:
    return indexed_path("outputs", result.index);
  case ConfigSection::Document:
    return "version";
  }
  return "$";
}
[[nodiscard]] const char *validation_message(const ValidationError error) {
  switch (error) {
  case ValidationError::UnsupportedVersion:
    return "unsupported configuration version; expected 1";
  case ValidationError::EmptyActionName:
    return "action name must not be empty";
  case ValidationError::DuplicateActionName:
    return "action name must be unique";
  case ValidationError::UndeclaredAction:
    return "action reference is not declared";
  case ValidationError::DuplicateAction:
    return "an action may have only one level rule";
  case ValidationError::EmptySignalKey:
    return "signal key must not be empty";
  case ValidationError::UnknownComparison:
    return "comparison is unsupported";
  case ValidationError::UnknownFreshness:
    return "freshness requirement is unsupported";
  case ValidationError::UnknownEventEdge:
    return "event edge is unsupported";
  case ValidationError::EmptyChoice:
    return "choice key must not be empty";
  case ValidationError::InvalidOperand:
    return "numeric operand must be finite";
  case ValidationError::UnsupportedComparison:
    return "ordered comparisons require a numeric operand";
  case ValidationError::InvalidHysteresis:
    return "release threshold must be on the release side of activation";
  case ValidationError::InvalidRange:
    return "range bounds and span must be finite, with ascending input";
  case ValidationError::UnknownLedEffect:
    return "LED effect is unsupported";
  case ValidationError::UnknownFillDirection:
    return "fill direction is unsupported";
  case ValidationError::EmptyZone:
    return "LED zone must not be empty";
  case ValidationError::ZoneOutOfRange:
    return "LED zone must fit within the strip";
  case ValidationError::InvalidColor:
    return "RGB channels must be in 0..255";
  case ValidationError::InvalidPriority:
    return "priority must be in 0..255";
  case ValidationError::DuplicateBinding:
    return "output binding target must be unique";
  case ValidationError::InvalidDuration:
    return "transient duration_ms must be in 1..9007199254740991";
  case ValidationError::None:
    return "configuration is valid";
  }
  return "configuration validation failed";
}

enum class InputPreflight { Ok, EmbeddedNul, TooDeep, ResourceExhausted };

[[nodiscard]] InputPreflight preflight_input(const std::string_view json,
                                             const std::size_t parse_length) {
  std::size_t depth = 0U;
  std::size_t structural_elements = 0U;
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
      if (++structural_elements > kMaxControllerConfigJsonNodes)
        return InputPreflight::ResourceExhausted;
    } else if (character == ',') {
      if (++structural_elements > kMaxControllerConfigJsonNodes)
        return InputPreflight::ResourceExhausted;
    } else if ((character == '}' || character == ']') && depth > 0U) {
      --depth;
    }
  }
  return InputPreflight::Ok;
}

} // namespace

ConfigLoadResult parse_controller_config(const std::string_view json) {
  const std::size_t length = !json.empty() && json.back() == '\0' ? json.size() - 1U : json.size();
  ConfigDiagnostic error{};
  if (length > kMaxControllerConfigJsonBytes) {
    set_error(error, Category::Parse, Code::InputTooLarge, "$",
              "configuration JSON exceeds maximum size");
    return failure(std::move(error));
  }
  const InputPreflight preflight = preflight_input(json, length);
  if (preflight != InputPreflight::Ok) {
    auto code = Code::ResourceExhausted;
    const char *message = "configuration JSON exceeds parser resource limits";
    if (preflight == InputPreflight::EmbeddedNul) {
      code = Code::EmbeddedNul;
      message = "configuration JSON contains an embedded NUL character";
    } else if (preflight == InputPreflight::TooDeep) {
      code = Code::NestingLimitExceeded;
      message = "configuration JSON exceeds maximum nesting depth";
    }
    set_error(error, Category::Parse, code, "$", message);
    return failure(std::move(error));
  }
  const char *end = nullptr;
  CJsonAllocationGuard allocation_guard;
  cJSON *root = cJSON_ParseWithLengthOpts(json.data(), length, &end, 0);
  if (root == nullptr) {
    set_error(error, Category::Parse,
              allocation_guard.failed ? Code::ResourceExhausted : Code::MalformedJson, "$",
              allocation_guard.failed ? "configuration JSON exhausted available memory"
                                      : "malformed JSON input");
    return failure(std::move(error));
  }
  struct RootGuard {
    cJSON *value;
    ~RootGuard() { cJSON_Delete(value); }
  } guard{root};
  if (end == nullptr || std::any_of(end, json.data() + length, [](const char c) {
        return c != ' ' && c != '\t' && c != '\r' && c != '\n';
      })) {
    set_error(error, Category::Parse, Code::MalformedJson, "$",
              "trailing data after JSON document");
    return failure(std::move(error));
  }
  if (!cJSON_IsObject(root)) {
    set_error(error, Category::Structural, Code::RootTypeMismatch, "$",
              "configuration root must be an object");
    return failure(std::move(error));
  }
  if (!reject_unknown_fields(*root, "", {"version", "actions", "rules", "outputs"}, error))
    return failure(std::move(error));
  ControllerConfig config{};
  const cJSON *version = required_field(*root, "version", "", error);
  if (version == nullptr || !parse_integer(version, "version", config.version, error) ||
      !parse_array(*root, "actions", config.actions, parse_action, error) ||
      !parse_array(*root, "rules", config.rules, parse_rule, error) ||
      !parse_array(
          *root, "outputs", config.outputs,
          [&](const cJSON &object, const std::string_view path, OutputBinding &output,
              ConfigDiagnostic &diagnostic) {
            return parse_output(object, path, output, diagnostic, *root, json.substr(0, length));
          },
          error))
    return failure(std::move(error));
  const auto validation = validate(config);
  if (!validation.ok()) {
    set_error(error, Category::Semantic, Code::SchemaValidation, validation_path(validation),
              validation_message(validation.error), validation.error, validation.index);
    return failure(std::move(error));
  }
  return success(std::move(config));
}

namespace {

void append_json_string(std::string &output, const std::string_view value) {
  output.push_back('"');
  constexpr char kHex[] = "0123456789abcdef";
  for (const unsigned char character : value) {
    switch (character) {
    case '"':
      output += "\\\"";
      break;
    case '\\':
      output += "\\\\";
      break;
    case '\b':
      output += "\\b";
      break;
    case '\f':
      output += "\\f";
      break;
    case '\n':
      output += "\\n";
      break;
    case '\r':
      output += "\\r";
      break;
    case '\t':
      output += "\\t";
      break;
    default:
      if (character < 0x20U) {
        output += "\\u00";
        output.push_back(kHex[(character >> 4U) & 0x0fU]);
        output.push_back(kHex[character & 0x0fU]);
      } else {
        output.push_back(static_cast<char>(character));
      }
      break;
    }
  }
  output.push_back('"');
}

void append_integer(std::string &output, const persisted::Integer value) {
  output += std::to_string(value);
}

void append_float(std::string &output, const float value) {
  std::ostringstream stream;
  stream.imbue(std::locale::classic());
  stream << std::setprecision(std::numeric_limits<float>::max_digits10) << std::defaultfloat
         << value;
  output += stream.str();
}

template <typename Enum> void append_enum(std::string &output, const Enum value) {
  const auto name = name_of(value);
  append_json_string(output, name.value_or(""));
}

void append_operand(std::string &output, const persisted::Operand &operand) {
  output.push_back('{');
  std::visit(
      [&output](const auto &value) {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, persisted::BooleanOperand>) {
          output += "\"boolean\":";
          output += value.value ? "true" : "false";
        } else if constexpr (std::is_same_v<Value, persisted::NumberOperand>) {
          output += "\"number\":";
          append_float(output, value.value);
        } else {
          output += "\"choice\":";
          append_json_string(output, value.key);
        }
      },
      operand);
  output.push_back('}');
}

void append_condition(std::string &output, const persisted::Condition &condition) {
  output += "\"signal_key\":";
  append_json_string(output, condition.signal_key);
  output += ",\"comparison\":";
  append_enum(output, condition.comparison);
  output += ",\"operand\":";
  append_operand(output, condition.operand);
}

void append_numeric_range(std::string &output, const action_engine::NumericRange &range) {
  output += "{\"from\":";
  append_float(output, range.from);
  output += ",\"to\":";
  append_float(output, range.to);
  output.push_back('}');
}

void append_rule(std::string &output, const persisted::Rule &rule) {
  std::visit(
      [&output](const auto &value) {
        using Rule = std::decay_t<decltype(value)>;
        output.push_back('{');
        if constexpr (std::is_same_v<Rule, persisted::StateRule>) {
          output += "\"type\":\"state\",\"action\":";
          append_json_string(output, value.action);
          output.push_back(',');
          append_condition(output, value.condition);
          output += ",\"freshness\":";
          append_enum(output, value.freshness);
        } else if constexpr (std::is_same_v<Rule, persisted::SampledStateRule>) {
          output += "\"type\":\"sampled_state\",\"action\":";
          append_json_string(output, value.action);
          output.push_back(',');
          append_condition(output, value.condition);
          if (value.release_threshold.has_value()) {
            output += ",\"release_threshold\":";
            append_float(output, *value.release_threshold);
          }
          output += ",\"freshness\":";
          append_enum(output, value.freshness);
        } else if constexpr (std::is_same_v<Rule, persisted::EventRule>) {
          output += "\"type\":\"event\",\"action\":";
          append_json_string(output, value.action);
          output.push_back(',');
          append_condition(output, value.condition);
          output += ",\"edge\":";
          append_enum(output, value.edge);
          output += ",\"freshness\":";
          append_enum(output, value.freshness);
        } else {
          output += "\"type\":\"range\",\"action\":";
          append_json_string(output, value.action);
          output += ",\"signal_key\":";
          append_json_string(output, value.signal_key);
          output += ",\"input\":";
          append_numeric_range(output, value.input);
          output += ",\"output\":";
          append_numeric_range(output, value.output);
          output += ",\"freshness\":";
          append_enum(output, value.freshness);
        }
        output.push_back('}');
      },
      rule);
}

void append_output_binding(std::string &output, const persisted::OutputBinding &binding) {
  std::visit(
      [&output](const auto &value) {
        using Binding = std::decay_t<decltype(value)>;
        output.push_back('{');
        output += "\"type\":";
        if constexpr (std::is_same_v<Binding, persisted::LedEffectBinding>) {
          output += "\"led_effect\",\"action\":";
          append_json_string(output, value.action);
          output += ",\"effect\":";
          append_enum(output, value.effect);
          output += ",\"priority\":";
          append_integer(output, value.priority);
        } else {
          if constexpr (std::is_same_v<Binding, persisted::LedTransientBinding>)
            output += "\"led_transient\",\"action\":";
          else
            output += "\"led_fill\",\"action\":";
          append_json_string(output, value.action);
          output += ",\"zone\":{\"start\":";
          append_integer(output, value.zone.start);
          output += ",\"length\":";
          append_integer(output, value.zone.length);
          output += ",\"direction\":";
          append_enum(output, value.zone.direction);
          output += "},\"color\":{\"red\":";
          append_integer(output, value.color.red);
          output += ",\"green\":";
          append_integer(output, value.color.green);
          output += ",\"blue\":";
          append_integer(output, value.color.blue);
          output.push_back('}');
          if constexpr (std::is_same_v<Binding, persisted::LedTransientBinding>) {
            output += ",\"duration_ms\":";
            append_integer(output, value.duration_ms);
          }
          output += ",\"priority\":";
          append_integer(output, value.priority);
        }
        output.push_back('}');
      },
      binding);
}

} // namespace

std::string serialize_controller_config(const ControllerConfig &configuration) {
  std::string output;
  output.reserve(512U);
  output += "{\"version\":";
  append_integer(output, configuration.version);
  output += ",\"actions\":[";
  for (std::size_t index = 0; index < configuration.actions.size(); ++index) {
    if (index != 0U)
      output.push_back(',');
    output += "{\"name\":";
    append_json_string(output, configuration.actions[index].name);
    output.push_back('}');
  }
  output += "],\"rules\":[";
  for (std::size_t index = 0; index < configuration.rules.size(); ++index) {
    if (index != 0U)
      output.push_back(',');
    append_rule(output, configuration.rules[index]);
  }
  output += "],\"outputs\":[";
  for (std::size_t index = 0; index < configuration.outputs.size(); ++index) {
    if (index != 0U)
      output.push_back(',');
    append_output_binding(output, configuration.outputs[index]);
  }
  output += "]}";
  return output;
}

} // namespace controller_config::persisted
