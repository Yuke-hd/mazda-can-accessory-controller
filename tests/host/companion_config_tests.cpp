#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "companion_config/apply_check.hpp"
#include "companion_config/config_service.hpp"
#include "companion_config/wire_codes.hpp"
#include "companion_protocol/config_status.hpp"
#include "companion_protocol/config_transfer.hpp"
#include "companion_protocol/crc32.hpp"
#include "companion_protocol/read_back.hpp"
#include "controller_config/persisted/config_store.hpp"
#include "controller_config/persisted/json_loader.hpp"
#include "mazda/signal_catalog.hpp"
#include "support/fake_clock.hpp"
#include "support/fake_signal_provider.hpp"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace persisted = controller_config::persisted;
namespace wire = companion_protocol;
using companion_config::ApplyCheck;
using companion_config::ConfigService;
using companion_config::ScratchApplyCheck;
using companion_config::to_wire;

constexpr std::string_view kMinimal = R"({"version":1})";
constexpr std::string_view kOneAction = R"({"version":1,"actions":[{"name":"left"}]})";
constexpr std::string_view kOneActionCanonical =
    R"({"version":1,"actions":[{"name":"left"}],"rules":[],"outputs":[]})";
constexpr std::string_view kDuplicateAction =
    R"({"version":1,"actions":[{"name":"left"},{"name":"left"}]})";
constexpr std::string_view kUnknownSignal =
    R"({"version":1,"actions":[{"name":"a"}],"rules":[{"type":"state","action":"a",)"
    R"("signal_key":"vehicle.not_a_signal","comparison":"equal",)"
    R"("operand":{"choice":"left"},"freshness":"fresh"}]})";

std::string factory_json() {
  std::ifstream file{CONTROLLER_CONFIG_FACTORY_JSON_PATH, std::ios::binary};
  REQUIRE(file.is_open());
  return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

wire::ByteView view(std::string_view text) {
  return wire::ByteView{reinterpret_cast<const std::uint8_t *>(text.data()), text.size()};
}

std::uint32_t crc_of(std::string_view text) { return wire::crc32(view(text)); }

std::uint16_t u16_at(wire::ByteView bytes, std::size_t offset) {
  return static_cast<std::uint16_t>(bytes[offset] | (bytes[offset + 1] << 8U));
}

std::uint32_t u32_at(wire::ByteView bytes, std::size_t offset) {
  return static_cast<std::uint32_t>(u16_at(bytes, offset)) |
         (static_cast<std::uint32_t>(u16_at(bytes, offset + 2)) << 16U);
}

class FakeBackend final : public persisted::ConfigStoreBackend {
public:
  ReadResult read_override() override {
    if (read_failure)
      return {ReadStatus::Failed, {}, "fake read failed"};
    if (stored.empty())
      return {};
    return {ReadStatus::Present, stored, {}};
  }
  WriteResult write_override(const std::string_view canonical_json) override {
    ++writes;
    if (write_failure)
      return {false, "fake write failed"};
    stored = canonical_json;
    return {true, {}};
  }
  ClearResult clear_override() override {
    ++clears;
    if (clear_failure)
      return {false, "fake clear failed"};
    stored.clear();
    return {true, {}};
  }

  std::string stored{};
  std::size_t writes{0};
  std::size_t clears{0};
  bool read_failure{false};
  bool write_failure{false};
  bool clear_failure{false};
};

// Records the model it was asked to check and answers with a scripted status.
class ScriptedApplyCheck final : public ApplyCheck {
public:
  persisted::ApplyStatus dry_run(const persisted::ControllerConfig &config) noexcept override {
    ++calls;
    checked_actions = config.actions.size();
    return status;
  }

  persisted::ApplyStatus status{};
  std::size_t calls{0};
  std::size_t checked_actions{0};
};

// One boot of the controller: the boot path selects a configuration and the
// service serves it, as the firmware composition root wires them.
struct Boot {
  explicit Boot(FakeBackend *store, std::string_view factory = kMinimal) {
    std::optional<persisted::ConfigStore> config_store;
    if (store != nullptr)
      config_store.emplace(*store);
    auto selected =
        persisted::load_boot_configuration(config_store ? &*config_store : nullptr, factory);
    service.record_boot(selected, store);
    if (selected.configuration.has_value())
      active = std::move(*selected.configuration);
  }

  persisted::ControllerConfig active{};
  ScriptedApplyCheck apply_check{};
  ConfigService service{active, apply_check};
};

} // namespace

TEST_CASE("every parse diagnostic category and code has its wire code") {
  CHECK(to_wire(persisted::ConfigErrorCategory::Parse) == wire::DiagnosticCategory::Parse);
  CHECK(to_wire(persisted::ConfigErrorCategory::Structural) ==
        wire::DiagnosticCategory::Structural);
  CHECK(to_wire(persisted::ConfigErrorCategory::Semantic) == wire::DiagnosticCategory::Semantic);

  using Code = persisted::ConfigErrorCode;
  using Wire = wire::DiagnosticCode;
  const std::vector<std::pair<Code, Wire>> codes{
      {Code::MalformedJson, Wire::MalformedJson},
      {Code::EmbeddedNul, Wire::EmbeddedNul},
      {Code::InputTooLarge, Wire::InputTooLarge},
      {Code::NestingLimitExceeded, Wire::NestingLimitExceeded},
      {Code::RootTypeMismatch, Wire::RootTypeMismatch},
      {Code::MissingField, Wire::MissingField},
      {Code::UnknownField, Wire::UnknownField},
      {Code::TypeMismatch, Wire::TypeMismatch},
      {Code::InvalidValue, Wire::InvalidValue},
      {Code::SchemaValidation, Wire::SchemaValidation},
      {Code::ResourceExhausted, Wire::ResourceExhausted},
  };
  for (const auto &entry : codes) {
    CAPTURE(static_cast<unsigned>(entry.first));
    CHECK(to_wire(entry.first) == entry.second);
  }
}

TEST_CASE("every validation error has the wire code of the same name") {
  using Error = persisted::ValidationError;
  using Wire = wire::ValidationCode;
  const std::vector<std::pair<Error, Wire>> codes{
      {Error::None, Wire::None},
      {Error::UnsupportedVersion, Wire::UnsupportedVersion},
      {Error::EmptyActionName, Wire::EmptyActionName},
      {Error::DuplicateActionName, Wire::DuplicateActionName},
      {Error::UndeclaredAction, Wire::UndeclaredAction},
      {Error::DuplicateAction, Wire::DuplicateAction},
      {Error::EmptySignalKey, Wire::EmptySignalKey},
      {Error::UnknownComparison, Wire::UnknownComparison},
      {Error::UnknownFreshness, Wire::UnknownFreshness},
      {Error::UnknownEventEdge, Wire::UnknownEventEdge},
      {Error::EmptyChoice, Wire::EmptyChoice},
      {Error::InvalidOperand, Wire::InvalidOperand},
      {Error::UnsupportedComparison, Wire::UnsupportedComparison},
      {Error::InvalidHysteresis, Wire::InvalidHysteresis},
      {Error::InvalidRange, Wire::InvalidRange},
      {Error::UnknownLedEffect, Wire::UnknownLedEffect},
      {Error::UnknownFillDirection, Wire::UnknownFillDirection},
      {Error::EmptyZone, Wire::EmptyZone},
      {Error::ZoneOutOfRange, Wire::ZoneOutOfRange},
      {Error::InvalidColor, Wire::InvalidColor},
      {Error::InvalidPriority, Wire::InvalidPriority},
      {Error::DuplicateBinding, Wire::DuplicateBinding},
      {Error::InvalidDuration, Wire::InvalidDuration},
      {Error::IncompatibleActionKind, Wire::IncompatibleActionKind},
  };
  for (const auto &entry : codes) {
    CAPTURE(static_cast<unsigned>(entry.first));
    CHECK(to_wire(entry.first) == entry.second);
  }
}

TEST_CASE("every section, apply stage, binding and engine status has its wire code") {
  CHECK(to_wire(persisted::ConfigSection::Document) == wire::ConfigSection::Document);
  CHECK(to_wire(persisted::ConfigSection::Actions) == wire::ConfigSection::Actions);
  CHECK(to_wire(persisted::ConfigSection::Rules) == wire::ConfigSection::Rules);
  CHECK(to_wire(persisted::ConfigSection::Outputs) == wire::ConfigSection::Outputs);

  CHECK(to_wire(persisted::ApplyStage::Complete) == wire::ApplyStage::Complete);
  CHECK(to_wire(persisted::ApplyStage::Validation) == wire::ApplyStage::Validation);
  CHECK(to_wire(persisted::ApplyStage::ActionId) == wire::ApplyStage::ActionId);
  CHECK(to_wire(persisted::ApplyStage::OutputBinding) == wire::ApplyStage::OutputBinding);
  CHECK(to_wire(persisted::ApplyStage::SinkRegistration) == wire::ApplyStage::SinkRegistration);
  CHECK(to_wire(persisted::ApplyStage::Rule) == wire::ApplyStage::Rule);

  using Binding = local_argb_actions::BindingStatus;
  CHECK(to_wire(Binding::Ok) == wire::BindingCode::Ok);
  CHECK(to_wire(Binding::InvalidAction) == wire::BindingCode::InvalidAction);
  CHECK(to_wire(Binding::DuplicateBinding) == wire::BindingCode::DuplicateBinding);
  CHECK(to_wire(Binding::CapacityExceeded) == wire::BindingCode::CapacityExceeded);
  CHECK(to_wire(Binding::InvalidEffect) == wire::BindingCode::InvalidEffect);

  using Engine = action_engine::ConfigStatus;
  using WireEngine = wire::EngineConfigCode;
  const std::vector<std::pair<Engine, WireEngine>> engine{
      {Engine::Ok, WireEngine::Ok},
      {Engine::InvalidState, WireEngine::InvalidState},
      {Engine::CapacityExceeded, WireEngine::CapacityExceeded},
      {Engine::DuplicateSink, WireEngine::DuplicateSink},
      {Engine::InvalidAction, WireEngine::InvalidAction},
      {Engine::DuplicateAction, WireEngine::DuplicateAction},
      {Engine::UnknownSignal, WireEngine::UnknownSignal},
      {Engine::UnsupportedCapability, WireEngine::UnsupportedCapability},
      {Engine::TypeMismatch, WireEngine::TypeMismatch},
      {Engine::UnknownChoice, WireEngine::UnknownChoice},
      {Engine::InvalidOperand, WireEngine::InvalidOperand},
      {Engine::UnsupportedComparison, WireEngine::UnsupportedComparison},
      {Engine::InvalidRange, WireEngine::InvalidRange},
      {Engine::InvalidHysteresis, WireEngine::InvalidHysteresis},
  };
  for (const auto &entry : engine) {
    CAPTURE(static_cast<unsigned>(entry.first));
    CHECK(to_wire(entry.first) == entry.second);
  }
}

TEST_CASE("a config diagnostic becomes the Config status rejection without its message") {
  persisted::ConfigDiagnostic diagnostic{};
  diagnostic.category = persisted::ConfigErrorCategory::Semantic;
  diagnostic.code = persisted::ConfigErrorCode::SchemaValidation;
  diagnostic.schema_error = persisted::ValidationError::ZoneOutOfRange;
  diagnostic.index = 3;
  diagnostic.path = "outputs[3].zone.length";
  diagnostic.message = "not sent";

  const auto rejection = companion_config::config_rejection(diagnostic);

  CHECK(rejection.category == wire::DiagnosticCategory::Semantic);
  CHECK(rejection.code == wire::DiagnosticCode::SchemaValidation);
  CHECK(rejection.validation == wire::ValidationCode::ZoneOutOfRange);
  CHECK(rejection.index == 3);
  CHECK(rejection.path.text() == "outputs[3].zone.length");
}

TEST_CASE("an apply status becomes the Config status apply rejection") {
  persisted::ApplyStatus status{};
  status.stage = persisted::ApplyStage::Validation;
  status.validation = {persisted::ValidationError::DuplicateBinding,
                       persisted::ConfigSection::Outputs, 4};
  status.index = 4;
  status.binding = local_argb_actions::BindingStatus::CapacityExceeded;
  status.engine = action_engine::ConfigStatus::UnknownChoice;

  const auto rejection = companion_config::apply_rejection(status);

  CHECK(rejection.stage == wire::ApplyStage::Validation);
  CHECK(rejection.validation == wire::ValidationCode::DuplicateBinding);
  CHECK(rejection.validation_section == wire::ConfigSection::Outputs);
  CHECK(rejection.index == 4);
  CHECK(rejection.binding == wire::BindingCode::CapacityExceeded);
  CHECK(rejection.engine == wire::EngineConfigCode::UnknownChoice);
}

TEST_CASE("a valid document is dry-run, saved in canonical form and reported with its CRC") {
  FakeBackend store;
  Boot boot{&store};

  const auto outcome = boot.service.commit(view(kOneAction));

  REQUIRE(outcome.result() == wire::TransferResult::Saved);
  CHECK(boot.apply_check.calls == 1);
  CHECK(boot.apply_check.checked_actions == 1);
  CHECK(store.stored == kOneActionCanonical);
  CHECK(outcome.saved_document().length == kOneActionCanonical.size());
  CHECK(outcome.saved_document().crc32 == crc_of(kOneActionCanonical));
}

TEST_CASE("malformed JSON is rejected before the dry run and writes nothing") {
  FakeBackend store;
  Boot boot{&store};

  const auto outcome = boot.service.commit(view(R"({"version":)"));

  REQUIRE(outcome.result() == wire::TransferResult::ConfigRejected);
  CHECK(outcome.rejection().category == wire::DiagnosticCategory::Parse);
  CHECK(outcome.rejection().code == wire::DiagnosticCode::MalformedJson);
  CHECK(boot.apply_check.calls == 0);
  CHECK(store.writes == 0);
}

TEST_CASE("a semantically invalid document reports its validation code and path") {
  FakeBackend store;
  Boot boot{&store};

  const auto outcome = boot.service.commit(view(kDuplicateAction));

  REQUIRE(outcome.result() == wire::TransferResult::ConfigRejected);
  CHECK(outcome.rejection().validation == wire::ValidationCode::DuplicateActionName);
  CHECK(outcome.rejection().index == 1);
  CHECK_FALSE(outcome.rejection().path.text().empty());
  CHECK(store.writes == 0);
}

TEST_CASE("a failed dry run is reported as ApplyRejected and writes nothing") {
  FakeBackend store;
  Boot boot{&store};
  boot.apply_check.status.stage = persisted::ApplyStage::Rule;
  boot.apply_check.status.index = 2;
  boot.apply_check.status.engine = action_engine::ConfigStatus::UnknownSignal;

  const auto outcome = boot.service.commit(view(kOneAction));

  REQUIRE(outcome.result() == wire::TransferResult::ApplyRejected);
  CHECK(outcome.apply().stage == wire::ApplyStage::Rule);
  CHECK(outcome.apply().index == 2);
  CHECK(outcome.apply().engine == wire::EngineConfigCode::UnknownSignal);
  CHECK(store.writes == 0);
}

TEST_CASE("a canonical form over the stored limit is rejected as too large") {
  FakeBackend store;
  Boot boot{&store};
  const std::string long_name(persisted::kMaxStoredControllerConfigJsonBytes, 'n');
  const std::string document = R"({"version":1,"actions":[{"name":")" + long_name + R"("}]})";

  const auto outcome = boot.service.commit(view(document));

  REQUIRE(outcome.result() == wire::TransferResult::ConfigRejected);
  CHECK(outcome.rejection().code == wire::DiagnosticCode::InputTooLarge);
  CHECK(outcome.rejection().path.text() == "$");
  CHECK(store.writes == 0);
}

TEST_CASE("a failed store write is reported as StorageFailed without diagnostics") {
  FakeBackend store;
  store.write_failure = true;
  Boot boot{&store};

  const auto outcome = boot.service.commit(view(kOneAction));

  CHECK(outcome.result() == wire::TransferResult::StorageFailed);
  CHECK(outcome.rejection().code == wire::DiagnosticCode::None);
}

TEST_CASE("without a config store a commit fails with StorageFailed") {
  Boot boot{nullptr};

  CHECK(boot.service.commit(view(kOneAction)).result() == wire::TransferResult::StorageFailed);
  CHECK(boot.apply_check.calls == 0);
}

TEST_CASE("revert to factory clears the override and reports a failed clear") {
  FakeBackend store;
  store.stored = std::string{kOneActionCanonical};
  Boot boot{&store};

  CHECK(boot.service.revert_to_factory());
  CHECK(store.stored.empty());

  store.clear_failure = true;
  CHECK_FALSE(boot.service.revert_to_factory());

  Boot without_store{nullptr};
  CHECK_FALSE(without_store.service.revert_to_factory());
}

TEST_CASE("the boot document is the canonical active config, served once") {
  FakeBackend store;
  Boot boot{&store, kOneAction};

  const auto first = boot.service.prepare_boot();
  const auto second = boot.service.prepare_boot();

  CHECK(first.status.active.source() == wire::ConfigSource::Factory);
  CHECK(first.status.active.length() == kOneActionCanonical.size());
  CHECK(first.status.active.crc32() == crc_of(kOneActionCanonical));
  CHECK(first.config_store_available);
  CHECK(first.status.flags.bits() == 0);
  CHECK(second.status.active.bytes().data() == first.status.active.bytes().data());
  CHECK(first.environment().active.bytes().data() == first.status.active.bytes().data());
}

TEST_CASE("Config status and the read-back page header describe the same document") {
  FakeBackend store;
  store.stored = std::string{kOneActionCanonical};
  Boot boot{&store, factory_json()};
  const auto prepared = boot.service.prepare_boot();
  REQUIRE(prepared.status.active.source() == wire::ConfigSource::Override);
  wire::ConfigTransfer transfer{boot.service, prepared.environment()};

  const auto status = wire::encode_config_status(prepared.status, transfer.status());
  const auto page = wire::encode_read_back_page(prepared.environment().active, 0);
  REQUIRE(page.has_value());

  const wire::ByteView status_bytes = status.view();
  const wire::ByteView page_bytes = page->view();
  CHECK(status_bytes[3] == page_bytes[0]);
  CHECK(u16_at(status_bytes, 4) == u16_at(page_bytes, 1));
  CHECK(u32_at(status_bytes, 6) == u32_at(page_bytes, 3));
  CHECK(u16_at(page_bytes, 1) == kOneActionCanonical.size());
  CHECK(u32_at(page_bytes, 3) == crc_of(kOneActionCanonical));
}

TEST_CASE("a saved document reads back after restart with the CRC the commit reported") {
  FakeBackend store;
  Boot before{&store, factory_json()};
  const std::string factory = factory_json();
  const auto outcome = before.service.commit(view(factory));
  REQUIRE(outcome.result() == wire::TransferResult::Saved);

  Boot after{&store, kMinimal};
  const auto prepared = after.service.prepare_boot();

  CHECK(prepared.status.active.source() == wire::ConfigSource::Override);
  CHECK(prepared.status.active.length() == outcome.saved_document().length);
  CHECK(prepared.status.active.crc32() == outcome.saved_document().crc32);
  const auto reparsed = persisted::parse_controller_config(store.stored);
  REQUIRE(reparsed.ok());
  CHECK(persisted::serialize_controller_config(*reparsed.configuration) == store.stored);
}

TEST_CASE("boot flags report an invalid override with its diagnostic codes") {
  FakeBackend store;
  store.stored = std::string{kDuplicateAction};
  Boot boot{&store};

  const auto prepared = boot.service.prepare_boot();

  CHECK(prepared.status.flags.invalid_override);
  CHECK_FALSE(prepared.status.flags.override_read_failed);
  CHECK(prepared.status.override_code == wire::DiagnosticCode::SchemaValidation);
  CHECK(prepared.status.override_validation == wire::ValidationCode::DuplicateActionName);
  CHECK(prepared.status.active.source() == wire::ConfigSource::Factory);
}

TEST_CASE("boot flags report a failed override read and a missing store") {
  FakeBackend store;
  store.read_failure = true;
  Boot read_failed{&store};
  CHECK(read_failed.service.prepare_boot().status.flags.override_read_failed);

  Boot no_store{nullptr};
  const auto prepared = no_store.service.prepare_boot();
  CHECK(prepared.status.flags.no_config_store);
  CHECK_FALSE(prepared.config_store_available);
  CHECK(prepared.status.active.source() == wire::ConfigSource::Factory);
}

TEST_CASE("with no configuration selected the active document is none") {
  Boot boot{nullptr, R"({"version":2})"};

  const auto prepared = boot.service.prepare_boot();

  CHECK(prepared.status.active.source() == wire::ConfigSource::None);
  CHECK(prepared.status.active.length() == 0);
  CHECK(prepared.status.active.crc32() == 0);
  CHECK(prepared.status.flags.lighting_setup_failed);
}

TEST_CASE("the scratch dry run accepts the factory config over the Mazda catalog") {
  test_support::FakeSignalProvider provider{mazda::internal::signal_catalog()};
  test_support::FakeClock clock;
  ScratchApplyCheck check{provider, clock};
  const auto factory = persisted::parse_controller_config(factory_json());
  REQUIRE(factory.ok());

  CHECK(check.dry_run(*factory.configuration).ok());
  // Fresh scratch objects each time: a second identical run does not see the
  // first run's actions or sink as duplicates.
  CHECK(check.dry_run(*factory.configuration).ok());
}

TEST_CASE("the scratch dry run rejects a signal the catalog does not have") {
  test_support::FakeSignalProvider provider{mazda::internal::signal_catalog()};
  test_support::FakeClock clock;
  ScratchApplyCheck check{provider, clock};
  const auto parsed = persisted::parse_controller_config(kUnknownSignal);
  REQUIRE(parsed.ok());

  const auto status = check.dry_run(*parsed.configuration);

  CHECK(status.stage == persisted::ApplyStage::Rule);
  CHECK(status.engine == action_engine::ConfigStatus::UnknownSignal);
  CHECK(provider.subscription_count() == 0);
}
