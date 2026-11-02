#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "action_engine/engine.hpp"
#include "action_engine/rule_config.hpp"
#include "companion_config/apply_check.hpp"
#include "companion_config/config_service.hpp"
#include "companion_protocol/config_transfer.hpp"
#include "companion_protocol/crc32.hpp"
#include "controller_config/persisted/application.hpp"
#include "controller_config/persisted/config_store.hpp"
#include "local_argb/lighting_sink.hpp"
#include "local_argb_actions/led_action_sink.hpp"
#include "support/fake_clock.hpp"
#include "vehicle_profile_runtime.hpp"
#include "vehicle_profile_runtime/replay_adapter.hpp"
#include "vehicle_profile_runtime/tables.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace persisted = controller_config::persisted;
namespace wire = companion_protocol;
using action_engine::UnverifiedFreshnessPolicy;

constexpr std::string_view kBareV1Config =
    R"({"version":1,"actions":[{"name":"rpm"},{"name":"brake"}],"rules":[)"
    R"({"type":"range","action":"rpm","signal_key":"vehicle.engine_rpm",)"
    R"("input":{"from":0,"to":6500},"output":{"from":0,"to":1},"freshness":"fresh"},)"
    R"({"type":"state","action":"brake","signal_key":"vehicle.brake_pressed",)"
    R"("comparison":"equal","operand":{"boolean":true},"freshness":"fresh"}]})";
constexpr std::string_view kUnverifiedBrakeConfig =
    R"({"version":1,"actions":[{"name":"brake"}],"rules":[{"type":"state",)"
    R"("action":"brake","signal_key":"vehicle.brake_pressed","comparison":"equal",)"
    R"("operand":{"boolean":true},"freshness":"fresh_or_unverified"}]})";
constexpr std::string_view kPartialUnverifiedBrakeConfig =
    R"({"version":1,"actions":[{"name":"rpm"},{"name":"brake"}],"rules":[)"
    R"({"type":"range","action":"rpm","signal_key":"vehicle.engine_rpm",)"
    R"("input":{"from":0,"to":6500},"output":{"from":0,"to":1},"freshness":"fresh"},)"
    R"({"type":"state","action":"brake","signal_key":"vehicle.brake_pressed",)"
    R"("comparison":"equal","operand":{"boolean":true},)"
    R"("freshness":"fresh_or_unverified"}]})";
constexpr std::string_view kPartialMissingSignalConfig =
    R"({"version":1,"actions":[{"name":"rpm"},{"name":"missing"}],"rules":[)"
    R"({"type":"range","action":"rpm","signal_key":"vehicle.engine_rpm",)"
    R"("input":{"from":0,"to":6500},"output":{"from":0,"to":1},"freshness":"fresh"},)"
    R"({"type":"state","action":"missing","signal_key":"vehicle.not_in_selected_profile",)"
    R"("comparison":"equal","operand":{"boolean":true},"freshness":"fresh"}]})";
constexpr std::string_view kMinimalConfig = R"({"version":1})";
constexpr std::uint16_t kAttMtu = 247U;

class FakeBackend final : public persisted::ConfigStoreBackend {
public:
  ReadResult read_override() override {
    if (stored.empty())
      return {};
    return {ReadStatus::Present, stored, {}};
  }

  WriteResult write_override(const std::string_view canonical_json) override {
    ++writes;
    stored = canonical_json;
    return {true, {}};
  }

  ClearResult clear_override() override {
    ++clears;
    stored.clear();
    return {true, {}};
  }

  std::string stored{};
  std::size_t writes{0};
  std::size_t clears{0};
};

class DiscardingLightingSink final : public local_argb::internal::LightingSink {
public:
  bool publish(const local_argb::internal::LightingCommand & /*command*/) noexcept override {
    return true;
  }
};

vehicle_profile_runtime::ProfileTables selected_profile_tables() noexcept {
  return {vehicle_profile::kCatalog,
          vehicle_profile::kFreshnessPolicies.data(),
          vehicle_profile::kFreshnessPolicies.size(),
          vehicle_profile::kDispatchTable.data(),
          vehicle_profile::kDispatchTable.size(),
          vehicle_profile::kMaxDecodeResults};
}

struct SelectedComposition {
  test_support::FakeClock clock{};
  vehicle_profile_runtime::ReplayAdapter runtime{selected_profile_tables(), clock};
  companion_config::ScratchApplyCheck scratch{runtime.provider(), clock,
                                              UnverifiedFreshnessPolicy::Reject};
  persisted::ControllerConfig active{};
  companion_config::ConfigService config{active, scratch};
};

std::string factory_json() {
  std::ifstream file{CONTROLLER_CONFIG_FACTORY_JSON_PATH, std::ios::binary};
  REQUIRE(file.is_open());
  return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

std::string over_capacity_factory_json() {
  persisted::ControllerConfig config{};
  for (std::size_t index = 0; index < local_argb_actions::LedActionSink::kMaxFillBindings + 1;
       ++index) {
    const std::string action = "factory_" + std::to_string(index);
    config.actions.push_back(persisted::Action{action});
    config.outputs.push_back(persisted::LedSolidBinding{
        action, {static_cast<persisted::Integer>(index), 1}, {16, 0, 0}, 100});
  }
  return persisted::serialize_controller_config(config);
}

wire::ByteView view(const std::vector<std::uint8_t> &bytes) {
  return wire::ByteView{bytes.data(), bytes.size()};
}

void push_u16(std::vector<std::uint8_t> &pdu, const std::size_t value) {
  pdu.push_back(static_cast<std::uint8_t>(value & 0xFFU));
  pdu.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
}

wire::ConfigWriteResponse upload(wire::ConfigTransfer &transfer, const std::string_view document) {
  const std::vector<std::uint8_t> bytes(document.begin(), document.end());
  const std::uint32_t crc = wire::crc32(view(bytes));
  std::vector<std::uint8_t> start{0x01};
  push_u16(start, bytes.size());
  push_u16(start, crc & 0xFFFFU);
  push_u16(start, crc >> 16U);
  const wire::ConfigWriteContext context{kAttMtu, wire::TransferClock{1'000}};
  auto response = transfer.handle_write(view(start), context);
  if (response.error != wire::AttError::None)
    return response;

  constexpr std::size_t kChunkBytes = 200U;
  for (std::size_t offset = 0; offset < bytes.size(); offset += kChunkBytes) {
    const auto count = std::min(kChunkBytes, bytes.size() - offset);
    std::vector<std::uint8_t> chunk{0x02};
    push_u16(chunk, offset);
    chunk.insert(chunk.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                 bytes.begin() + static_cast<std::ptrdiff_t>(offset + count));
    response = transfer.handle_write(view(chunk), context);
    if (response.error != wire::AttError::None)
      return response;
  }

  const std::vector<std::uint8_t> commit{0x03};
  return transfer.handle_write(view(commit), context);
}

std::string active_text(const wire::ActiveDocument &document) {
  const auto bytes = document.bytes();
  return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
}

} // namespace

TEST_CASE("selected generated provider reuses a bare v1 config and rejects unverified upload") {
  if (vehicle_profile::kCatalog.empty())
    return;

  SelectedComposition composition{};
  const auto catalog = composition.runtime.provider().catalog();
  REQUIRE(catalog.find("vehicle.engine_rpm") != nullptr);
  REQUIRE(catalog.find("vehicle.brake_pressed") != nullptr);

  FakeBackend backend{};
  backend.stored = kBareV1Config;
  const std::string stored_before = backend.stored;
  persisted::ConfigStore store{backend};
  auto selected = persisted::load_boot_configuration(&store, factory_json());
  REQUIRE(selected.ok());
  CHECK(selected.source == persisted::ConfigurationSource::PersistedOverride);
  CHECK_FALSE(selected.override_diagnostic.has_value());
  composition.config.record_boot(selected, &backend);
  composition.active = std::move(*selected.configuration);

  // The same public v1 document applies through the selected generated table.
  DiscardingLightingSink lighting{};
  local_argb_actions::LedActionSink leds{lighting, composition.clock};
  action_engine::ActionEngine engine{composition.runtime.provider(),
                                     UnverifiedFreshnessPolicy::Reject};
  const auto applied = persisted::apply_controller_config(composition.active, leds, engine);
  REQUIRE(applied.ok());
  REQUIRE(engine.attach() == vehicle_signals::SignalStatus::Ok);
  CHECK(engine.detach() == vehicle_signals::SignalStatus::Ok);
  CHECK(backend.writes == 0U);
  CHECK(backend.stored == stored_before);

  const auto active_before = composition.config.prepare_boot().status.active;
  const auto active_before_text = active_text(active_before);
  CHECK(active_before.source() == wire::ConfigSource::Override);

  wire::ConfigTransfer transfer{composition.config,
                                composition.config.prepare_boot().environment()};
  const auto response = upload(transfer, kUnverifiedBrakeConfig);
  CHECK(response.error == wire::AttError::ApplyRejected);
  CHECK(transfer.status().result == wire::TransferResult::ApplyRejected);
  CHECK(transfer.status().apply.stage == wire::ApplyStage::Rule);
  CHECK(transfer.status().apply.engine == wire::EngineConfigCode::UnsupportedCapability);
  CHECK(backend.writes == 0U);
  CHECK(backend.stored == stored_before);
  CHECK(composition.active.rules.size() == 2U);

  const auto active_after = composition.config.prepare_boot().status.active;
  CHECK(active_after.crc32() == active_before.crc32());
  CHECK(active_text(active_after) == active_before_text);
}

TEST_CASE("imported boot policy rejection clears partial rules before factory fallback") {
  SelectedComposition composition{};
  const auto catalog = composition.runtime.provider().catalog();
  if (catalog.find("vehicle.engine_rpm") == nullptr ||
      catalog.find("vehicle.brake_pressed") == nullptr)
    return;

  FakeBackend backend{};
  backend.stored = kPartialUnverifiedBrakeConfig;
  const std::string stored_before = backend.stored;
  persisted::ConfigStore store{backend};
  auto selected = persisted::load_boot_configuration(&store, factory_json());
  REQUIRE(selected.ok());
  REQUIRE(selected.source == persisted::ConfigurationSource::PersistedOverride);
  REQUIRE(selected.configuration.has_value());
  REQUIRE(selected.configuration->rules.size() == 2U);
  composition.active = std::move(*selected.configuration);

  DiscardingLightingSink lighting{};
  local_argb_actions::LedActionSink leds{lighting, composition.clock};
  action_engine::ActionEngine engine{composition.runtime.provider(),
                                     UnverifiedFreshnessPolicy::Reject};
  const auto result = persisted::apply_persisted_override_with_factory_fallback(
      composition.active, factory_json(), leds, engine);

  REQUIRE_FALSE(result.override_status.ok());
  CHECK(result.override_status.stage == persisted::ApplyStage::Rule);
  CHECK(result.override_status.index == 1U);
  CHECK(result.override_status.rules_applied == 1U);
  CHECK(result.override_status.engine == action_engine::ConfigStatus::UnsupportedCapability);
  REQUIRE(result.factory_status.ok());
  CHECK(result.ok());
  CHECK(backend.writes == 0U);
  CHECK(backend.stored == stored_before);

  selected.source = persisted::ConfigurationSource::FactoryDefault;
  composition.config.record_boot(selected, &backend, result.ok(), true);
  const auto boot = composition.config.prepare_boot();
  CHECK(boot.status.active.source() == wire::ConfigSource::Factory);
  CHECK(boot.status.flags.invalid_override);
  CHECK_FALSE(boot.status.flags.lighting_setup_failed);
  CHECK(boot.status.override_code == wire::DiagnosticCode::None);
  CHECK(boot.status.override_validation == wire::ValidationCode::None);
  const auto factory = persisted::parse_controller_config(factory_json());
  REQUIRE(factory.ok());
  CHECK(active_text(boot.status.active) ==
        persisted::serialize_controller_config(*factory.configuration));
}

TEST_CASE("a saved rule missing from the selected provider uses factory without pruning") {
  SelectedComposition composition{};
  FakeBackend backend{};
  backend.stored = kPartialMissingSignalConfig;
  const std::string stored_before = backend.stored;
  persisted::ConfigStore store{backend};
  auto selected = persisted::load_boot_configuration(&store, factory_json());
  REQUIRE(selected.ok());
  REQUIRE(selected.source == persisted::ConfigurationSource::PersistedOverride);
  REQUIRE(selected.configuration.has_value());
  REQUIRE(selected.configuration->rules.size() == 2U);
  composition.active = std::move(*selected.configuration);

  const auto catalog = composition.runtime.provider().catalog();
  const bool has_first_rule_signal = catalog.find("vehicle.engine_rpm") != nullptr;
  DiscardingLightingSink lighting{};
  local_argb_actions::LedActionSink leds{lighting, composition.clock};
  action_engine::ActionEngine engine{composition.runtime.provider(),
                                     UnverifiedFreshnessPolicy::Reject};
  const auto result = persisted::apply_persisted_override_with_factory_fallback(
      composition.active, factory_json(), leds, engine);

  REQUIRE_FALSE(result.override_status.ok());
  CHECK(result.override_status.stage == persisted::ApplyStage::Rule);
  CHECK(result.override_status.index == (has_first_rule_signal ? 1U : 0U));
  CHECK(result.override_status.rules_applied == (has_first_rule_signal ? 1U : 0U));
  CHECK(result.override_status.engine == action_engine::ConfigStatus::UnknownSignal);
  REQUIRE(result.factory_status.ok());
  CHECK(result.ok());
  CHECK(backend.writes == 0U);
  CHECK(backend.stored == stored_before);

  selected.source = persisted::ConfigurationSource::FactoryDefault;
  composition.config.record_boot(selected, &backend, result.ok(), true);
  const auto boot = composition.config.prepare_boot();
  CHECK(boot.status.active.source() == wire::ConfigSource::Factory);
  CHECK(boot.status.flags.invalid_override);
  CHECK_FALSE(boot.status.flags.lighting_setup_failed);
  CHECK(boot.status.override_code == wire::DiagnosticCode::None);
  CHECK(boot.status.override_validation == wire::ValidationCode::None);
}

TEST_CASE("failed selected factory apply reports no active config and permits repair upload") {
  SelectedComposition composition{};
  FakeBackend backend{};
  backend.stored = kPartialMissingSignalConfig;
  const std::string stored_before = backend.stored;
  persisted::ConfigStore store{backend};
  auto selected = persisted::load_boot_configuration(&store, factory_json());
  REQUIRE(selected.ok());
  REQUIRE(selected.source == persisted::ConfigurationSource::PersistedOverride);
  REQUIRE(selected.configuration.has_value());
  composition.active = std::move(*selected.configuration);

  DiscardingLightingSink lighting{};
  local_argb_actions::LedActionSink leds{lighting, composition.clock};
  action_engine::ActionEngine engine{composition.runtime.provider(),
                                     UnverifiedFreshnessPolicy::Reject};
  const std::string failing_factory = over_capacity_factory_json();
  const auto result = persisted::apply_persisted_override_with_factory_fallback(
      composition.active, failing_factory, leds, engine);

  CHECK_FALSE(result.ok());
  CHECK_FALSE(result.override_status.ok());
  CHECK_FALSE(result.factory_status.ok());
  CHECK(result.factory_status.stage == persisted::ApplyStage::OutputBinding);
  CHECK(result.factory_status.binding == local_argb_actions::BindingStatus::CapacityExceeded);
  CHECK(backend.writes == 0U);
  CHECK(backend.stored == stored_before);

  selected.source = persisted::ConfigurationSource::FactoryDefault;
  composition.config.record_boot(selected, &backend, result.ok(), true);
  const auto boot = composition.config.prepare_boot();
  CHECK(boot.status.active.source() == wire::ConfigSource::None);
  CHECK(boot.status.active.length() == 0U);
  CHECK(boot.status.active.crc32() == 0U);
  CHECK(boot.status.flags.invalid_override);
  CHECK(boot.status.flags.lighting_setup_failed);
  CHECK_FALSE(boot.status.flags.no_config_store);
  CHECK(boot.status.override_code == wire::DiagnosticCode::None);
  CHECK(boot.status.override_validation == wire::ValidationCode::None);

  wire::ConfigTransfer transfer{composition.config, boot.environment()};
  const auto response = upload(transfer, kMinimalConfig);
  CHECK(response.error == wire::AttError::None);
  CHECK(transfer.status().result == wire::TransferResult::Saved);
  CHECK(transfer.status().state == wire::TransferState::RestartPending);
  CHECK(backend.writes == 1U);
  const auto repaired = persisted::parse_controller_config(kMinimalConfig);
  REQUIRE(repaired.ok());
  CHECK(backend.stored == persisted::serialize_controller_config(*repaired.configuration));
  CHECK(composition.config.prepare_boot().status.active.source() == wire::ConfigSource::None);
}

TEST_CASE("a refused startup with no store reports no active configuration") {
  SelectedComposition composition{};

  composition.config.mark_lighting_setup_failed();

  const auto boot = composition.config.prepare_boot();
  CHECK(boot.status.active.source() == wire::ConfigSource::None);
  CHECK(boot.status.flags.lighting_setup_failed);
  CHECK(boot.status.flags.no_config_store);
  CHECK_FALSE(boot.config_store_available);
}

TEST_CASE("a later startup refusal hides active config but keeps the repair store") {
  SelectedComposition composition{};
  FakeBackend backend{};
  persisted::ConfigStore store{backend};
  auto selected = persisted::load_boot_configuration(&store, factory_json());
  REQUIRE(selected.ok());
  composition.active = std::move(*selected.configuration);
  composition.config.record_boot(selected, &backend);

  composition.config.mark_lighting_setup_failed();

  const auto boot = composition.config.prepare_boot();
  CHECK(boot.status.active.source() == wire::ConfigSource::None);
  CHECK(boot.status.flags.lighting_setup_failed);
  CHECK_FALSE(boot.status.flags.no_config_store);
  CHECK(boot.config_store_available);
}

TEST_CASE("empty selected provider rejects the whole saved v1 document without rewriting it") {
  if (!vehicle_profile::kCatalog.empty())
    return;

  SelectedComposition composition{};
  CHECK(composition.runtime.provider().catalog().empty());
  FakeBackend backend{};
  backend.stored = kBareV1Config;
  const std::string stored_before = backend.stored;
  persisted::ConfigStore store{backend};
  auto selected = persisted::load_boot_configuration(&store, factory_json());

  REQUIRE(selected.ok());
  CHECK(selected.source == persisted::ConfigurationSource::PersistedOverride);
  CHECK_FALSE(selected.override_diagnostic.has_value());
  composition.config.record_boot(selected, &backend);
  composition.active = std::move(*selected.configuration);

  DiscardingLightingSink lighting{};
  local_argb_actions::LedActionSink leds{lighting, composition.clock};
  action_engine::ActionEngine engine{composition.runtime.provider(),
                                     UnverifiedFreshnessPolicy::Reject};
  const auto applied = persisted::apply_controller_config(composition.active, leds, engine);
  CHECK_FALSE(applied.ok());
  CHECK(applied.stage == persisted::ApplyStage::Rule);
  CHECK(applied.engine == action_engine::ConfigStatus::UnknownSignal);
  CHECK(composition.active.rules.size() == 2U);

  const auto active_before = composition.config.prepare_boot().status.active;
  const auto active_before_text = active_text(active_before);
  wire::ConfigTransfer transfer{composition.config,
                                composition.config.prepare_boot().environment()};
  const auto response = upload(transfer, kBareV1Config);
  CHECK(response.error == wire::AttError::ApplyRejected);
  CHECK(transfer.status().result == wire::TransferResult::ApplyRejected);
  CHECK(transfer.status().apply.engine == wire::EngineConfigCode::UnknownSignal);
  CHECK(backend.writes == 0U);
  CHECK(backend.stored == stored_before);
  const auto active_after = composition.config.prepare_boot().status.active;
  CHECK(active_after.crc32() == active_before.crc32());
  CHECK(active_text(active_after) == active_before_text);
}
