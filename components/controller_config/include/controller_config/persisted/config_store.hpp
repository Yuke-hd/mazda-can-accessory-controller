#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "controller_config/persisted/json_loader.hpp"

namespace controller_config::persisted {

// New overrides reserve room for both slots and an in-flight replacement in
// the default 24 KiB NVS partition. Other namespaces can still exhaust NVS.
inline constexpr std::size_t kMaxStoredControllerConfigJsonBytes = 4U * 1024U;

// A raw storage adapter owns the platform-specific persistence details. Its
// value is always one complete canonical JSON document. Implementations must
// retain the previous active value when a replacement cannot be committed.
class ConfigStoreBackend {
public:
  enum class ReadStatus : std::uint8_t { Missing, Present, Failed };

  struct ReadResult final {
    ReadStatus status{ReadStatus::Missing};
    std::string json{};
    std::string message{};
  };

  struct WriteResult final {
    bool ok{false};
    std::string message{};
  };

  struct ClearResult final {
    bool ok{false};
    std::string message{};
  };

  virtual ~ConfigStoreBackend() = default;

  [[nodiscard]] virtual ReadResult read_override() = 0;
  [[nodiscard]] virtual WriteResult write_override(std::string_view canonical_json) = 0;
  [[nodiscard]] virtual ClearResult clear_override() = 0;
};

enum class OverrideLoadStatus : std::uint8_t { Missing, Present, Failed };

struct OverrideLoadResult final {
  OverrideLoadStatus status{OverrideLoadStatus::Missing};
  std::string json{};
  std::string message{};

  [[nodiscard]] bool present() const noexcept { return status == OverrideLoadStatus::Present; }
};

struct SaveOverrideResult final {
  enum class Status : std::uint8_t { Saved, InvalidCandidate, Failed };

  Status status{Status::Failed};
  std::string canonical_json{};
  std::optional<ConfigDiagnostic> diagnostic{};
  std::string message{};

  [[nodiscard]] bool ok() const noexcept { return status == Status::Saved; }
};

struct ClearOverrideResult final {
  bool ok{false};
  std::string message{};
};

// Validates candidates with the canonical JSON loader before handing a
// deterministic JSON document to the platform backend.
class ConfigStore final {
public:
  explicit ConfigStore(ConfigStoreBackend &backend) noexcept : backend_{backend} {}

  [[nodiscard]] OverrideLoadResult load_override();
  [[nodiscard]] SaveOverrideResult save_override(std::string_view candidate_json);
  [[nodiscard]] ClearOverrideResult clear_override();

private:
  ConfigStoreBackend &backend_;
};

// Boot-time selection is deliberately a one-shot operation. The returned
// model is owned by the result and remains independent of storage buffers.
enum class ConfigurationSource : std::uint8_t { FactoryDefault, PersistedOverride };

struct BootConfigurationResult final {
  std::optional<ControllerConfig> configuration{};
  ConfigurationSource source{ConfigurationSource::FactoryDefault};
  std::optional<ConfigDiagnostic> override_diagnostic{};
  std::string override_storage_message{};
  std::optional<ConfigDiagnostic> factory_diagnostic{};

  [[nodiscard]] bool ok() const noexcept { return configuration.has_value(); }
};

[[nodiscard]] BootConfigurationResult load_boot_configuration(ConfigStore *store,
                                                              std::string_view factory_json);

// The concrete ESP-IDF adapter is declared only as the platform-neutral
// interface. NVS handles and APIs remain private to its implementation.
[[nodiscard]] std::unique_ptr<ConfigStoreBackend> make_nvs_config_store_backend() noexcept;

} // namespace controller_config::persisted
