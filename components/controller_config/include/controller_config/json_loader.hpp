#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "controller_config/schema.hpp"
#include "vehicle_signals/signal_catalog.hpp"

// JSON is an input boundary only. The returned Configuration is the owning,
// parser-independent runtime model used by the action engine and LED adapters.
namespace controller_config {

enum class ConfigErrorCategory : std::uint8_t {
  Parse,
  Structural,
  Semantic,
};

enum class ConfigErrorCode : std::uint8_t {
  MalformedJson,
  RootTypeMismatch,
  MissingField,
  UnknownField,
  TypeMismatch,
  InvalidValue,
  SchemaValidation,
};

struct ConfigDiagnostic final {
  ConfigErrorCategory category{ConfigErrorCategory::Parse};
  ConfigErrorCode code{ConfigErrorCode::MalformedJson};
  SchemaError schema_error{SchemaError::None};
  std::size_t index{0};
  std::string path{};
  std::string message{};
};

struct ConfigLoadResult final {
  std::optional<Configuration> configuration{};
  std::optional<ConfigDiagnostic> diagnostic{};

  [[nodiscard]] bool ok() const noexcept {
    return configuration.has_value() && !diagnostic.has_value();
  }
};

// Parse, structurally validate, semantically validate, and own a versioned
// controller configuration. Parser-library types are intentionally hidden in
// the implementation file. A failed load never returns a partial model.
[[nodiscard]] ConfigLoadResult parse_controller_config(std::string_view json);

// The catalog-aware overload additionally checks signal type, capability, and
// enum-choice compatibility through the action-engine resolver.
[[nodiscard]] ConfigLoadResult parse_controller_config(std::string_view json,
                                                       vehicle_signals::SignalCatalogView catalog);

} // namespace controller_config
