#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "controller_config/persisted/config_store.hpp"
#include "controller_config/persisted/nvs_config_store_internal.hpp"

#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <variant>

#include "doctest/doctest.h"

namespace {

using controller_config::persisted::ConfigStore;
using controller_config::persisted::ConfigStoreBackend;
using controller_config::persisted::ConfigurationSource;
using controller_config::persisted::load_boot_configuration;
using controller_config::persisted::OverrideLoadStatus;
using controller_config::persisted::SaveOverrideResult;
using controller_config::persisted::internal::kNvsNotFound;
using controller_config::persisted::internal::kNvsOk;
using controller_config::persisted::internal::NvsApi;
using controller_config::persisted::internal::NvsConfigBackend;
using controller_config::persisted::internal::NvsHandle;
using controller_config::persisted::internal::NvsResult;

constexpr std::string_view kFactory = R"({"version":1})";
constexpr std::string_view kReorderedEmpty =
    R"({"outputs":[],"rules":[],"actions":[],"version":1})";
constexpr std::string_view kReplacement = R"({"version":1,"actions":[{"name":"left"}]})";
constexpr std::string_view kReplacementCanonical =
    R"({"version":1,"actions":[{"name":"left"}],"rules":[],"outputs":[]})";
constexpr std::string_view kSemanticInvalid =
    R"({"version":1,"actions":[{"name":"left"},{"name":"left"}]})";

class FakeBackend final : public ConfigStoreBackend {
public:
  ReadResult read_override() override {
    if (read_failure)
      return {ReadStatus::Failed, {}, "fake read failed"};
    if (stored.empty())
      return {};
    return {ReadStatus::Present, stored, {}};
  }

  WriteResult write_override(const std::string_view canonical_json) override {
    if (write_failure)
      return {false, "fake write failed"};
    stored = canonical_json;
    return {true, {}};
  }

  ClearResult clear_override() override {
    if (clear_failure)
      return {false, "fake clear failed"};
    stored.clear();
    return {true, {}};
  }

  std::string stored{};
  bool read_failure{false};
  bool write_failure{false};
  bool clear_failure{false};
};

class ImmediateWriteNvs final : public NvsApi {
public:
  using Value = std::variant<std::uint8_t, std::string>;

  NvsResult get_u8(const NvsHandle, const char *const key, std::uint8_t &value) override {
    if (fail_next_marker_read) {
      fail_next_marker_read = false;
      return kNvsFailure;
    }
    const auto found = values.find(key);
    if (found == values.end())
      return kNvsNotFound;
    const auto *stored = std::get_if<std::uint8_t>(&found->second);
    if (stored == nullptr)
      return kNvsTypeMismatch;
    value = *stored;
    return kNvsOk;
  }

  NvsResult set_u8(const NvsHandle, const char *const key, const std::uint8_t value) override {
    values[key] = value;
    return kNvsOk;
  }

  NvsResult get_blob(const NvsHandle, const char *const key, void *const buffer,
                     const std::size_t capacity, std::size_t &length) override {
    const auto found = values.find(key);
    if (found == values.end())
      return kNvsNotFound;
    const auto *stored = std::get_if<std::string>(&found->second);
    if (stored == nullptr)
      return kNvsFailure;
    length = stored->size();
    if (buffer == nullptr)
      return kNvsOk;
    if (capacity < stored->size())
      return kNvsFailure;
    std::memcpy(buffer, stored->data(), stored->size());
    return kNvsOk;
  }

  NvsResult set_blob(const NvsHandle, const char *const key, const void *const value,
                     const std::size_t length) override {
    if (blob_write_error.has_value())
      return *blob_write_error;
    values[key] = std::string(static_cast<const char *>(value), length);
    return kNvsOk;
  }

  NvsResult erase_key(const NvsHandle, const char *const key) override {
    if (ignore_erase)
      return kNvsOk;
    const auto erased = values.erase(key);
    return erased == 0U ? kNvsNotFound : kNvsOk;
  }

  NvsResult commit(const NvsHandle) override {
    ++commit_calls;
    if (fail_at_commit.has_value() && commit_calls == *fail_at_commit)
      return kNvsFailure;
    return kNvsOk;
  }

  void close(const NvsHandle) noexcept override {}

  const char *error_name(const NvsResult result) const noexcept override {
    switch (result) {
    case kNvsNotFound:
      return "not found";
    case kNvsTypeMismatch:
      return "ESP_ERR_NVS_TYPE_MISMATCH";
    case kNvsNotEnoughSpace:
      return "ESP_ERR_NVS_NOT_ENOUGH_SPACE";
    default:
      return "fake NVS failure";
    }
  }

  static constexpr NvsResult kNvsFailure = -1;
  static constexpr NvsResult kNvsTypeMismatch = 0x1103;
  static constexpr NvsResult kNvsNotEnoughSpace = 0x1105;
  std::map<std::string, Value> values{};
  std::size_t commit_calls{0U};
  std::optional<std::size_t> fail_at_commit{};
  std::optional<NvsResult> blob_write_error{};
  bool fail_next_marker_read{false};
  bool ignore_erase{false};
};

} // namespace

TEST_CASE("missing override selects the embedded factory configuration") {
  FakeBackend backend;
  ConfigStore store{backend};

  const auto result = load_boot_configuration(&store, kFactory);

  REQUIRE(result.ok());
  CHECK(result.source == ConfigurationSource::FactoryDefault);
  CHECK(result.configuration->version == 1);
  CHECK_FALSE(result.override_diagnostic.has_value());
}

TEST_CASE("valid override is selected through the canonical loader") {
  FakeBackend backend;
  ConfigStore store{backend};
  REQUIRE(store.save_override(kReorderedEmpty).ok());

  const auto result = load_boot_configuration(&store, kFactory);

  REQUIRE(result.ok());
  CHECK(result.source == ConfigurationSource::PersistedOverride);
  CHECK(result.configuration->actions.empty());
}

TEST_CASE("malformed override reports a diagnostic and selects the factory") {
  FakeBackend backend;
  backend.stored = "{";
  ConfigStore store{backend};

  const auto result = load_boot_configuration(&store, kFactory);

  REQUIRE(result.ok());
  CHECK(result.source == ConfigurationSource::FactoryDefault);
  REQUIRE(result.override_diagnostic.has_value());
  CHECK(result.override_diagnostic->path == "$");
}

TEST_CASE("semantically invalid override reports a diagnostic and selects the factory") {
  FakeBackend backend;
  backend.stored = kSemanticInvalid;
  ConfigStore store{backend};

  const auto result = load_boot_configuration(&store, kFactory);

  REQUIRE(result.ok());
  CHECK(result.source == ConfigurationSource::FactoryDefault);
  REQUIRE(result.override_diagnostic.has_value());
  CHECK(result.override_diagnostic->schema_error ==
        controller_config::persisted::ValidationError::DuplicateActionName);
}

TEST_CASE("save canonicalizes JSON and a new store reloads it") {
  FakeBackend backend;
  ConfigStore first{backend};

  const auto saved = first.save_override(kReorderedEmpty);

  REQUIRE(saved.ok());
  CHECK(saved.canonical_json == R"({"version":1,"actions":[],"rules":[],"outputs":[]})");
  CHECK(backend.stored == saved.canonical_json);

  ConfigStore second{backend};
  const auto loaded = second.load_override();
  CHECK(loaded.status == OverrideLoadStatus::Present);
  CHECK(loaded.json == saved.canonical_json);
}

TEST_CASE("invalid candidates never reach storage") {
  FakeBackend backend;
  ConfigStore store{backend};

  const auto saved = store.save_override(kSemanticInvalid);

  CHECK(saved.status == SaveOverrideResult::Status::InvalidCandidate);
  CHECK(backend.stored.empty());
}

TEST_CASE("a failed save leaves the prior valid override active") {
  FakeBackend backend;
  ConfigStore store{backend};
  const auto prior = store.save_override(kReorderedEmpty);
  REQUIRE(prior.ok());
  backend.write_failure = true;

  REQUIRE(prior.canonical_json != kReplacementCanonical);
  const auto failed = store.save_override(kReplacement);
  CHECK_FALSE(failed.ok());

  const auto boot = load_boot_configuration(&store, kFactory);
  REQUIRE(boot.ok());
  CHECK(boot.source == ConfigurationSource::PersistedOverride);
  CHECK(backend.stored == prior.canonical_json);
  CHECK(backend.stored != kReplacementCanonical);
}

TEST_CASE("clearing the override selects the factory on the next boot") {
  FakeBackend backend;
  ConfigStore store{backend};
  REQUIRE(store.save_override(kReorderedEmpty).ok());

  const auto cleared = store.clear_override();
  REQUIRE(cleared.ok);
  const auto boot = load_boot_configuration(&store, kFactory);

  REQUIRE(boot.ok());
  CHECK(boot.source == ConfigurationSource::FactoryDefault);
}

TEST_CASE("a failed clear leaves the prior valid override active") {
  FakeBackend backend;
  ConfigStore store{backend};
  REQUIRE(store.save_override(kReorderedEmpty).ok());
  backend.clear_failure = true;

  const auto cleared = store.clear_override();
  CHECK_FALSE(cleared.ok);

  const auto boot = load_boot_configuration(&store, kFactory);
  REQUIRE(boot.ok());
  CHECK(boot.source == ConfigurationSource::PersistedOverride);
}

TEST_CASE("storage read failures fall back with a storage diagnostic") {
  FakeBackend backend;
  backend.read_failure = true;
  ConfigStore store{backend};

  const auto result = load_boot_configuration(&store, kFactory);

  REQUIRE(result.ok());
  CHECK(result.source == ConfigurationSource::FactoryDefault);
  CHECK(result.override_storage_message == "fake read failed");
}

TEST_CASE("the NVS adapter preserves the active slot after an immediate-write commit failure") {
  ImmediateWriteNvs nvs;
  NvsConfigBackend backend{nvs, 7U};
  ConfigStore store{backend};
  const auto prior = store.save_override(kReorderedEmpty);
  REQUIRE(prior.ok());
  nvs.fail_at_commit = nvs.commit_calls + 2U;

  REQUIRE(prior.canonical_json != kReplacementCanonical);
  const auto failed = store.save_override(kReplacement);

  CHECK_FALSE(failed.ok());
  const auto boot = load_boot_configuration(&store, kFactory);
  REQUIRE(boot.ok());
  CHECK(boot.source == ConfigurationSource::PersistedOverride);
  CHECK(store.load_override().json == prior.canonical_json);
  CHECK(store.load_override().json != kReplacementCanonical);
}

TEST_CASE("factory reset removes unusable active markers and permits a new save") {
  ImmediateWriteNvs nvs;
  SUBCASE("out of range") { nvs.values["active_slot"] = std::uint8_t{2U}; }
  SUBCASE("wrong type") { nvs.values["active_slot"] = std::string{"corrupt"}; }
  NvsConfigBackend backend{nvs, 7U};
  ConfigStore store{backend};
  REQUIRE(store.load_override().status == OverrideLoadStatus::Failed);

  REQUIRE(store.clear_override().ok);
  CHECK(nvs.values.count("active_slot") == 0U);
  CHECK(store.load_override().status == OverrideLoadStatus::Missing);
  const auto boot = load_boot_configuration(&store, kFactory);
  REQUIRE(boot.ok());
  CHECK(boot.source == ConfigurationSource::FactoryDefault);
  REQUIRE(store.save_override(kReplacement).ok());
  CHECK(store.load_override().json == kReplacementCanonical);
}

TEST_CASE("factory reset attempts erasure after an active marker read error") {
  ImmediateWriteNvs nvs;
  NvsConfigBackend backend{nvs, 7U};
  ConfigStore store{backend};
  REQUIRE(store.save_override(kReplacement).ok());
  nvs.fail_next_marker_read = true;
  REQUIRE(store.clear_override().ok);
  CHECK(nvs.values.count("active_slot") == 0U);
  CHECK(store.load_override().status == OverrideLoadStatus::Missing);
  // Reset deactivates, rather than wipes, the old slot blobs.
  CHECK(std::get<std::string>(nvs.values.at("slot_a")) == kReplacementCanonical);
}

TEST_CASE("factory reset reports failure when a corrupt marker survives erase or commit fails") {
  ImmediateWriteNvs nvs;
  nvs.values["active_slot"] = std::string{"corrupt"};
  SUBCASE("read-back still sees the marker") { nvs.ignore_erase = true; }
  SUBCASE("commit fails") { nvs.fail_at_commit = 1U; }
  NvsConfigBackend backend{nvs, 7U};
  CHECK_FALSE(backend.clear_override().ok);
}

TEST_CASE("NVS failure messages distinguish type mismatch from exhausted storage") {
  ImmediateWriteNvs nvs;
  NvsConfigBackend backend{nvs, 7U};
  ConfigStore store{backend};
  nvs.values["active_slot"] = std::string{"corrupt"};
  CHECK(store.load_override().message == "read active slot failed: ESP_ERR_NVS_TYPE_MISMATCH");
  REQUIRE(store.clear_override().ok);
  nvs.blob_write_error = ImmediateWriteNvs::kNvsNotEnoughSpace;
  const auto failed = store.save_override(kReplacement);
  CHECK(failed.status == SaveOverrideResult::Status::Failed);
  CHECK(failed.message == "write configuration slot failed: ESP_ERR_NVS_NOT_ENOUGH_SPACE");
  CHECK(store.load_override().status == OverrideLoadStatus::Missing);
}

TEST_CASE("canonical storage size rejects expansion before writing and preserves the prior value") {
  FakeBackend backend;
  ConfigStore store{backend};
  const auto prior = store.save_override(kReplacement);
  REQUIRE(prior.ok());
  // Omitted arrays expand during canonicalization. Exercise both the persisted
  // limit and the larger parser limit without relying on float formatting.
  for (const std::size_t limit :
       {std::size_t{4096U}, controller_config::persisted::kMaxControllerConfigJsonBytes}) {
    std::string candidate = R"({"version":1,"actions":[{"name":")";
    const std::string suffix = R"("}]})";
    candidate.append(limit - candidate.size() - suffix.size(), 'a');
    candidate += suffix;
    REQUIRE(candidate.size() == limit);
    REQUIRE(controller_config::persisted::parse_controller_config(candidate).ok());

    const auto saved = store.save_override(candidate);
    CHECK(saved.status == SaveOverrideResult::Status::InvalidCandidate);
    REQUIRE(saved.diagnostic.has_value());
    CHECK(saved.diagnostic->code == controller_config::persisted::ConfigErrorCode::InputTooLarge);
    CHECK(saved.diagnostic->path == "$");
    CHECK(saved.canonical_json.empty());
    CHECK(backend.stored == prior.canonical_json);
  }
}

TEST_CASE("the NVS adapter bounds new slots to four KiB without changing the active marker") {
  ImmediateWriteNvs nvs;
  NvsConfigBackend backend{nvs, 7U};
  for (const char value : {'a', 'b', 'c', 'd'}) {
    const std::string document(4096U, value);
    REQUIRE(backend.write_override(document).ok);
    CHECK(backend.read_override().json == document);
  }
  const auto before = nvs.values;
  CHECK_FALSE(backend.write_override(std::string(4097U, 'e')).ok);
  CHECK(nvs.values == before);
}

TEST_CASE("a canonical document at the storage limit saves and a larger one is rejected") {
  FakeBackend backend;
  ConfigStore store{backend};
  std::string candidate = R"({"version":1,"actions":[{"name":")";
  const std::string suffix = R"("}],"rules":[],"outputs":[]})";
  candidate.append(controller_config::persisted::kMaxStoredControllerConfigJsonBytes -
                       candidate.size() - suffix.size(),
                   'a');
  candidate += suffix;
  const auto saved = store.save_override(candidate);
  REQUIRE(saved.ok());
  CHECK(saved.canonical_json == candidate);
  candidate.insert(candidate.size() - suffix.size(), 1U, 'b');
  CHECK(store.save_override(candidate).status == SaveOverrideResult::Status::InvalidCandidate);
  CHECK(backend.stored == saved.canonical_json);
}

TEST_CASE("an existing override above the new write bound can still load") {
  ImmediateWriteNvs nvs;
  nvs.values["active_slot"] = std::uint8_t{0U};
  std::string document = R"({"version":1,"actions":[{"name":")";
  document.append(4096U, 'a');
  document += R"("}],"rules":[],"outputs":[]})";
  nvs.values["slot_a"] = document;
  NvsConfigBackend backend{nvs, 7U};
  ConfigStore store{backend};
  const auto boot = load_boot_configuration(&store, kFactory);
  REQUIRE(boot.ok());
  CHECK(boot.source == ConfigurationSource::PersistedOverride);
  CHECK(store.load_override().json == document);
}

TEST_CASE("the NVS adapter restores an override when clearing fails after immediate erase") {
  ImmediateWriteNvs nvs;
  NvsConfigBackend backend{nvs, 7U};
  ConfigStore store{backend};
  REQUIRE(store.save_override(kReorderedEmpty).ok());
  nvs.fail_at_commit = nvs.commit_calls + 1U;

  const auto failed = store.clear_override();

  CHECK_FALSE(failed.ok);
  const auto boot = load_boot_configuration(&store, kFactory);
  REQUIRE(boot.ok());
  CHECK(boot.source == ConfigurationSource::PersistedOverride);
}
