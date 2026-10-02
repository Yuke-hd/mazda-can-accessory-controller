#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "controller_config/persisted/validation.hpp"

// JSON is an input boundary only. The returned ControllerConfig is the owning,
// parser-independent persisted model; catalog checks and runtime IDs belong
// to the application step, as specified by the current model contract.
namespace controller_config::persisted {

enum class ConfigErrorCategory : std::uint8_t {
  Parse,
  Structural,
  Semantic,
};

enum class ConfigErrorCode : std::uint8_t {
  MalformedJson,
  EmbeddedNul,
  InputTooLarge,
  NestingLimitExceeded,
  RootTypeMismatch,
  MissingField,
  UnknownField,
  TypeMismatch,
  InvalidValue,
  SchemaValidation,
  ResourceExhausted,
};

inline constexpr std::size_t kMaxControllerConfigJsonBytes = 16U * 1024U;
inline constexpr std::size_t kMaxControllerConfigJsonNesting = 16U;
// Conservative syntax budget used to bound cJSON parse-tree growth.
inline constexpr std::size_t kMaxControllerConfigJsonNodes = 512U;

struct ConfigDiagnostic final {
  ConfigErrorCategory category{ConfigErrorCategory::Parse};
  ConfigErrorCode code{ConfigErrorCode::MalformedJson};
  ValidationError schema_error{ValidationError::None};
  // For structural errors in an array, this is the containing item index;
  // top-level and parse errors leave it at zero. Semantic errors always use
  // the index reported by ValidationResult.
  std::size_t index{0};
  std::string path{};
  std::string message{};
};

struct ConfigLoadResult final {
  std::optional<ControllerConfig> configuration{};
  std::optional<ConfigDiagnostic> diagnostic{};

  [[nodiscard]] bool ok() const noexcept {
    return configuration.has_value() && !diagnostic.has_value();
  }
};

// Parse, structurally validate, semantically validate, and own a versioned
// controller configuration. Parser-library types are intentionally hidden in
// the implementation file. A failed load never returns a partial model.
[[nodiscard]] ConfigLoadResult parse_controller_config(std::string_view json);

// Serialize a validated persisted model into the deterministic JSON spelling
// used by configuration storage. The serializer has no parser-library types
// in its contract; callers must validate a model before serializing it.
[[nodiscard]] std::string serialize_controller_config(const ControllerConfig &configuration);

} // namespace controller_config::persisted
