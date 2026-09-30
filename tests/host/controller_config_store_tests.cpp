#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "controller_config/persisted/config_store.hpp"
#include "controller_config/persisted/nvs_config_store_internal.hpp"
#include "controller_config/persisted/production_profile.hpp"

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
    const auto found = values.find(key);
    if (found == values.end())
      return kNvsNotFound;
    const auto *stored = std::get_if<std::uint8_t>(&found->second);
    if (stored == nullptr)
      return kNvsFailure;
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
    values[key] = std::string(static_cast<const char *>(value), length);
    return kNvsOk;
  }

  NvsResult erase_key(const NvsHandle, const char *const key) override {
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
    return result == kNvsNotFound ? "not found" : "fake NVS failure";
  }

  static constexpr NvsResult kNvsFailure = -1;
  std::map<std::string, Value> values{};
  std::size_t commit_calls{0U};
  std::optional<std::size_t> fail_at_commit{};
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

TEST_CASE("canonical serialization round trips every production model alternative") {
  const auto model = controller_config::persisted::production_lighting_config();
  const auto json = controller_config::persisted::serialize_controller_config(model);
  const auto parsed = controller_config::persisted::parse_controller_config(json);

  REQUIRE(parsed.ok());
  CHECK(parsed.configuration->actions.size() == model.actions.size());
  CHECK(parsed.configuration->rules.size() == model.rules.size());
  CHECK(parsed.configuration->outputs.size() == model.outputs.size());
  CHECK(parsed.configuration->rules[3].index() == model.rules[3].index());
  CHECK(parsed.configuration->rules[4].index() == model.rules[4].index());
  CHECK(parsed.configuration->outputs[4].index() == model.outputs[4].index());
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
  REQUIRE(store.save_override(kReorderedEmpty).ok());
  backend.write_failure = true;

  const auto failed = store.save_override(R"({"version":1,"actions":[]})");
  CHECK_FALSE(failed.ok());

  const auto boot = load_boot_configuration(&store, kFactory);
  REQUIRE(boot.ok());
  CHECK(boot.source == ConfigurationSource::PersistedOverride);
  CHECK(backend.stored == R"({"version":1,"actions":[],"rules":[],"outputs":[]})");
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
  REQUIRE(store.save_override(kReorderedEmpty).ok());
  nvs.fail_at_commit = nvs.commit_calls + 2U;

  const auto failed = store.save_override(R"({"version":1,"actions":[]})");

  CHECK_FALSE(failed.ok());
  const auto boot = load_boot_configuration(&store, kFactory);
  REQUIRE(boot.ok());
  CHECK(boot.source == ConfigurationSource::PersistedOverride);
  CHECK(store.load_override().json == R"({"version":1,"actions":[],"rules":[],"outputs":[]})");
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
