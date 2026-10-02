#include "controller_config/persisted/config_store.hpp"

#include <utility>

namespace controller_config::persisted {

OverrideLoadResult ConfigStore::load_override() {
  const auto result = backend_.read_override();
  OverrideLoadResult loaded{};
  loaded.json = result.json;
  loaded.message = result.message;
  switch (result.status) {
  case ConfigStoreBackend::ReadStatus::Missing:
    loaded.status = OverrideLoadStatus::Missing;
    break;
  case ConfigStoreBackend::ReadStatus::Present:
    loaded.status = OverrideLoadStatus::Present;
    break;
  case ConfigStoreBackend::ReadStatus::Failed:
    loaded.status = OverrideLoadStatus::Failed;
    break;
  }
  return loaded;
}

SaveOverrideResult ConfigStore::save_override(const std::string_view candidate_json) {
  SaveOverrideResult result{};
  const auto loaded = parse_controller_config(candidate_json);
  if (!loaded.ok()) {
    result.status = SaveOverrideResult::Status::InvalidCandidate;
    result.diagnostic = loaded.diagnostic;
    result.message = "candidate configuration failed canonical parsing and validation";
    return result;
  }

  result.canonical_json = serialize_controller_config(*loaded.configuration);
  if (result.canonical_json.size() > kMaxStoredControllerConfigJsonBytes) {
    result.status = SaveOverrideResult::Status::InvalidCandidate;
    result.message = "canonical configuration JSON exceeds the 4 KiB override storage limit";
    result.diagnostic = ConfigDiagnostic{ConfigErrorCategory::Structural,
                                         ConfigErrorCode::InputTooLarge,
                                         ValidationError::None,
                                         0U,
                                         "$",
                                         result.message};
    result.canonical_json.clear();
    return result;
  }
  const auto canonical = parse_controller_config(result.canonical_json);
  if (!canonical.ok()) {
    result.status = SaveOverrideResult::Status::Failed;
    result.diagnostic = canonical.diagnostic;
    result.message = "canonical configuration serialization failed validation";
    result.canonical_json.clear();
    return result;
  }
  const auto written = backend_.write_override(result.canonical_json);
  if (!written.ok) {
    result.status = SaveOverrideResult::Status::Failed;
    result.message = written.message;
    result.canonical_json.clear();
    return result;
  }
  result.status = SaveOverrideResult::Status::Saved;
  return result;
}

ClearOverrideResult ConfigStore::clear_override() {
  const auto cleared = backend_.clear_override();
  return ClearOverrideResult{cleared.ok, cleared.message};
}

BootConfigurationResult load_boot_configuration(ConfigStore *const store,
                                                const std::string_view factory_json) {
  BootConfigurationResult result{};
  if (store != nullptr) {
    const auto override_result = store->load_override();
    if (override_result.status == OverrideLoadStatus::Present) {
      const auto parsed = parse_controller_config(override_result.json);
      if (parsed.ok()) {
        result.configuration = *parsed.configuration;
        result.source = ConfigurationSource::PersistedOverride;
        return result;
      }
      result.override_diagnostic = parsed.diagnostic;
    } else if (override_result.status == OverrideLoadStatus::Failed) {
      result.override_storage_message = override_result.message;
    }
  }

  const auto factory = parse_controller_config(factory_json);
  if (factory.ok()) {
    result.configuration = *factory.configuration;
    result.source = ConfigurationSource::FactoryDefault;
  } else {
    result.factory_diagnostic = factory.diagnostic;
  }
  return result;
}

} // namespace controller_config::persisted
