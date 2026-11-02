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
