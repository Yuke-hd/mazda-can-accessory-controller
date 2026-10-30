#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <thread>

#include "action_engine/action.hpp"
#include "action_engine/engine.hpp"
#include "action_engine/rule_config.hpp"
#include "companion_protocol/extensible_live_signals.hpp"
#include "companion_protocol/signal_catalog_export.hpp"
#include "provider_access.hpp"
#include "signal_observer_fan_out.hpp"
#include "vehicle_core/time.hpp"
#include "vehicle_profile_runtime/provider.hpp"
#include "vehicle_profile_runtime/service.hpp"
#include "vehicle_profile_runtime/tables.hpp"
#include "vehicle_signals/signal_provider.hpp"
#include "vehicle_telemetry/observer.hpp"
#include "vehicle_telemetry/receive.hpp"
#include "vehicle_telemetry/runtime.hpp"

namespace generated_profile_a {
#include "profile_a/vehicle_profile_runtime.hpp"
} // namespace generated_profile_a

namespace generated_profile_b {
#include "profile_b/vehicle_profile_runtime.hpp"
} // namespace generated_profile_b

namespace generated_profile_empty {
#include "profile_empty/vehicle_profile_runtime.hpp"
} // namespace generated_profile_empty

namespace {
std::atomic<bool> g_watch_heap_allocations{false};
std::atomic<std::size_t> g_watched_heap_allocations{0};
} // namespace

void *operator new(const std::size_t size) {
  if (g_watch_heap_allocations.load(std::memory_order_relaxed)) {
    g_watched_heap_allocations.fetch_add(1U, std::memory_order_relaxed);
  }
  if (void *const allocation = std::malloc(size == 0U ? 1U : size)) {
    return allocation;
  }
  throw std::bad_alloc{};
}

void *operator new[](const std::size_t size) {
  if (g_watch_heap_allocations.load(std::memory_order_relaxed)) {
    g_watched_heap_allocations.fetch_add(1U, std::memory_order_relaxed);
  }
  if (void *const allocation = std::malloc(size == 0U ? 1U : size)) {
    return allocation;
  }
  throw std::bad_alloc{};
}

void operator delete(void *allocation) noexcept { std::free(allocation); }
void operator delete[](void *allocation) noexcept { std::free(allocation); }
void operator delete(void *allocation, std::size_t) noexcept { std::free(allocation); }
void operator delete[](void *allocation, std::size_t) noexcept { std::free(allocation); }

namespace {

using action_engine::ActionCommand;
using action_engine::ActionCommandKind;
using action_engine::ActionEngine;
using action_engine::ActionId;
using action_engine::Comparison;
using action_engine::ConfigStatus;
using action_engine::FreshnessRequirement;
using action_engine::RangeRuleConfig;
using action_engine::RuleOperand;
using action_engine::SampledStateRuleConfig;
using action_engine::SignalCondition;
using action_engine::StateRuleConfig;
using companion_protocol::ExtensibleLiveSignalSampler;
using companion_protocol::SignalCatalogExport;
using vehicle_profile_runtime::ProfileProvider;
using vehicle_profile_runtime::ProfileRuntimeService;
using vehicle_profile_runtime::ProfileTables;
using vehicle_profile_runtime::ProviderAccess;
using vehicle_profile_runtime::PublicationSynchronization;
using vehicle_signals::Availability;
using vehicle_signals::SignalCatalogView;
using vehicle_signals::SignalId;
using vehicle_signals::SignalMetadata;
using vehicle_signals::SignalReading;
using vehicle_signals::SignalType;
using vehicle_telemetry::AcquisitionSource;
using vehicle_telemetry::FrameProcessor;
using vehicle_telemetry::Observer;
using vehicle_telemetry::ProcessResult;
using vehicle_telemetry::ReceiveStatus;
using vehicle_telemetry::ResultCode;
using vehicle_telemetry::Runtime;
using vehicle_telemetry::StatusResult;
using vehicle_telemetry::TransportDiagnostics;

constexpr ProfileTables kProfileATables{
    generated_profile_a::vehicle_profile::kCatalog,
    generated_profile_a::vehicle_profile::kFreshnessPolicies.data(),
    generated_profile_a::vehicle_profile::kFreshnessPolicies.size(),
    generated_profile_a::vehicle_profile::kDispatchTable.data(),
    generated_profile_a::vehicle_profile::kDispatchTable.size(),
    generated_profile_a::vehicle_profile::kMaxDecodeResults};
constexpr ProfileTables kProfileBTables{
    generated_profile_b::vehicle_profile::kCatalog,
    generated_profile_b::vehicle_profile::kFreshnessPolicies.data(),
    generated_profile_b::vehicle_profile::kFreshnessPolicies.size(),
    generated_profile_b::vehicle_profile::kDispatchTable.data(),
    generated_profile_b::vehicle_profile::kDispatchTable.size(),
    generated_profile_b::vehicle_profile::kMaxDecodeResults};
constexpr ProfileTables kEmptyTables{
    generated_profile_empty::vehicle_profile::kCatalog,
    generated_profile_empty::vehicle_profile::kFreshnessPolicies.data(),
    generated_profile_empty::vehicle_profile::kFreshnessPolicies.size(),
    generated_profile_empty::vehicle_profile::kDispatchTable.data(),
    generated_profile_empty::vehicle_profile::kDispatchTable.size(),
    generated_profile_empty::vehicle_profile::kMaxDecodeResults};

class FakeClock final : public vehicle_core::MonotonicClock {
public:
  [[nodiscard]] vehicle_core::MonotonicTimestamp now() const noexcept override {
    return now_us_.load(std::memory_order_relaxed);
  }

  void set(const vehicle_core::MonotonicTimestamp value) noexcept {
    now_us_.store(value, std::memory_order_relaxed);
  }

private:
  std::atomic<vehicle_core::MonotonicTimestamp> now_us_{0};
};

class FakeSynchronization final {
public:
  [[nodiscard]] PublicationSynchronization binding() noexcept {
    return PublicationSynchronization{this, &enter, &leave, this, &claim_owner};
  }

private:
  static void enter(void *context) noexcept {
    static_cast<FakeSynchronization *>(context)->mutex_.lock();
  }

  static void leave(void *context) noexcept {
    static_cast<FakeSynchronization *>(context)->mutex_.unlock();
  }

  static bool claim_owner(void *context) noexcept {
    auto &self = *static_cast<FakeSynchronization *>(context);
    std::lock_guard<std::mutex> lock{self.owner_mutex_};
    const std::thread::id current = std::this_thread::get_id();
    if (!self.owner_claimed_) {
      self.owner_ = current;
      self.owner_claimed_ = true;
      return true;
    }
    return self.owner_ == current;
  }

  std::mutex mutex_{};
  std::mutex owner_mutex_{};
  std::thread::id owner_{};
  bool owner_claimed_{false};
};

class RecordingActionSink final : public action_engine::ActionSink {
public:
  void execute(const ActionCommand &command) noexcept override {
    {
      std::lock_guard<std::mutex> lock{mutex_};
      if (count_ < commands_.size()) {
        commands_[count_++] = command;
      }
    }
    changed_.notify_all();
  }

  [[nodiscard]] bool contains(const ActionId action, const ActionCommandKind kind) const noexcept {
    std::lock_guard<std::mutex> lock{mutex_};
    return contains_locked(action, kind);
  }

  [[nodiscard]] bool wait_contains(const ActionId action, const ActionCommandKind kind,
                                   const std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock{mutex_};
    return changed_.wait_for(lock, timeout, [&] { return contains_locked(action, kind); });
  }

  [[nodiscard]] std::size_t size() const noexcept {
    std::lock_guard<std::mutex> lock{mutex_};
    return count_;
  }

private:
  [[nodiscard]] bool contains_locked(const ActionId action,
                                     const ActionCommandKind kind) const noexcept {
    for (std::size_t index = 0; index < count_; ++index) {
      if (commands_[index].action == action && commands_[index].kind == kind) {
        return true;
      }
    }
    return false;
  }

  mutable std::mutex mutex_{};
  std::condition_variable changed_{};
  std::array<ActionCommand, 32> commands_{};
  std::size_t count_{0};
};

class RecordingObserver final : public replay::SignalObserver {
public:
  void on_catalog(const SignalCatalogView catalog) noexcept override {
    ++catalog_calls_;
    catalog_size_ = catalog.size();
  }

  void on_reading(const vehicle_core::MonotonicTimestamp, const SignalMetadata &signal,
                  const SignalReading &reading) noexcept override {
    if (reading_count_ < readings_.size()) {
      ids_[reading_count_] = signal.id;
      readings_[reading_count_] = reading;
      ++reading_count_;
    }
  }

  [[nodiscard]] std::size_t catalog_calls() const noexcept { return catalog_calls_; }
  [[nodiscard]] std::size_t catalog_size() const noexcept { return catalog_size_; }
  [[nodiscard]] std::size_t reading_count() const noexcept { return reading_count_; }

  [[nodiscard]] const SignalReading *find(const SignalId id) const noexcept {
    for (std::size_t index = reading_count_; index > 0U; --index) {
      if (ids_[index - 1U] == id) {
        return &readings_[index - 1U];
      }
    }
    return nullptr;
  }

private:
  std::array<SignalId, 8> ids_{};
  std::array<SignalReading, 8> readings_{};
  std::size_t catalog_calls_{0};
  std::size_t catalog_size_{0};
  std::size_t reading_count_{0};
};

class QueuedSource final : public AcquisitionSource {
public:
  [[nodiscard]] StatusResult start() noexcept override {
    {
      std::lock_guard<std::mutex> lock{mutex_};
      running_ = true;
    }
    ready_.notify_all();
    return {ResultCode::Ok};
  }

  [[nodiscard]] StatusResult stop() noexcept override {
    {
      std::lock_guard<std::mutex> lock{mutex_};
      running_ = false;
    }
    ready_.notify_all();
    return {ResultCode::Ok};
  }

  [[nodiscard]] ReceiveStatus receive(vehicle_core::RawCanFrame &frame,
                                      const std::uint32_t timeout_ms) noexcept override {
    std::unique_lock<std::mutex> lock{mutex_};
    if (!running_) {
      return ReceiveStatus::NotStarted;
    }
    (void)ready_.wait_for(lock, std::chrono::milliseconds{timeout_ms},
                          [&] { return !running_ || count_ != 0U; });
    if (!running_) {
      return ReceiveStatus::NotStarted;
    }
    if (count_ == 0U) {
      return ReceiveStatus::Timeout;
    }
    frame = frames_[read_index_];
    read_index_ = (read_index_ + 1U) % frames_.size();
    --count_;
    return ReceiveStatus::Frame;
  }

  [[nodiscard]] vehicle_telemetry::AcquisitionStatistics statistics() const noexcept override {
    return {};
  }

  void push(const vehicle_core::RawCanFrame &frame) noexcept {
    {
      std::lock_guard<std::mutex> lock{mutex_};
      if (count_ < frames_.size()) {
        frames_[write_index_] = frame;
        write_index_ = (write_index_ + 1U) % frames_.size();
        ++count_;
      }
    }
    ready_.notify_one();
  }

private:
  std::mutex mutex_{};
  std::condition_variable ready_{};
  std::array<vehicle_core::RawCanFrame, 8> frames_{};
  std::size_t read_index_{0};
  std::size_t write_index_{0};
  std::size_t count_{0};
  bool running_{false};
};

class EmptyProcessor final : public FrameProcessor {
public:
  void reset() noexcept override {}
  [[nodiscard]] ProcessResult process(const vehicle_core::RawCanFrame &) noexcept override {
    return {};
  }
};

class EmptyObserver final : public Observer {
public:
  void on_frame_processed(const vehicle_core::RawCanFrame &,
                          const ProcessResult &) noexcept override {}
  void on_diagnostics(const TransportDiagnostics &) noexcept override {}
};

[[nodiscard]] vehicle_core::RawCanFrame make_frame(const std::uint32_t identifier,
                                                   const vehicle_core::MonotonicTimestamp timestamp,
                                                   const std::uint8_t raw) noexcept {
  vehicle_core::RawCanFrame result{};
  result.identifier = identifier;
  result.timestamp_us = timestamp;
  result.dlc = 1U;
  result.data[0] = raw;
  return result;
}

void begin_allocation_watch() noexcept {
  g_watched_heap_allocations.store(0U, std::memory_order_relaxed);
  g_watch_heap_allocations.store(true, std::memory_order_release);
}

[[nodiscard]] std::size_t end_allocation_watch() noexcept {
  g_watch_heap_allocations.store(false, std::memory_order_release);
  return g_watched_heap_allocations.load(std::memory_order_relaxed);
}

[[nodiscard]] std::uint16_t wire_handle_for(const SignalCatalogExport &catalog,
                                            const SignalId id) noexcept {
  for (std::uint16_t handle = 1U; handle <= catalog.signal_count(); ++handle) {
    if (catalog.provider_id_for_handle(handle) == id) {
      return handle;
    }
  }
  return 0U;
}

struct WireRecord final {
  std::uint16_t handle{0};
  std::uint8_t type{0};
  std::uint8_t status{0};
  std::array<std::uint8_t, 4> value{};
  std::size_t value_size{0};
};

struct ParsedPacket final {
  std::uint8_t format{0};
  std::uint16_t sequence{0};
  std::uint8_t flags{0};
  std::uint32_t token{0};
  std::array<WireRecord, 64> records{};
  std::size_t count{0};
  bool valid{false};
};

[[nodiscard]] ParsedPacket
parse_packet(const companion_protocol::ExtensibleLiveSignalPacket &packet) noexcept {
  ParsedPacket parsed{};
  if (packet.size() < companion_protocol::kExtensibleLiveSignalHeaderBytes) {
    return parsed;
  }
  parsed.format = packet[0];
  parsed.sequence = static_cast<std::uint16_t>(packet[1] | (packet[2] << 8U));
  parsed.flags = packet[3];
  parsed.token = static_cast<std::uint32_t>(packet[4]) |
                 (static_cast<std::uint32_t>(packet[5]) << 8U) |
                 (static_cast<std::uint32_t>(packet[6]) << 16U) |
                 (static_cast<std::uint32_t>(packet[7]) << 24U);
  std::size_t offset = companion_protocol::kExtensibleLiveSignalHeaderBytes;
  while (offset < packet.size() && parsed.count < parsed.records.size()) {
    const std::size_t record_length = packet[offset];
    if (record_length < 4U || offset + record_length + 1U > packet.size()) {
      return parsed;
    }
    WireRecord &record = parsed.records[parsed.count++];
    record.handle = static_cast<std::uint16_t>(packet[offset + 1U] | (packet[offset + 2U] << 8U));
    record.type = packet[offset + 3U];
    record.status = packet[offset + 4U];
    record.value_size = record_length - 4U;
    if (record.value_size > record.value.size()) {
      return parsed;
    }
    for (std::size_t index = 0; index < record.value_size; ++index) {
      record.value[index] = packet[offset + 5U + index];
    }
    offset += record_length + 1U;
  }
  parsed.valid = offset == packet.size();
  return parsed;
}

[[nodiscard]] const WireRecord *record_for(const ParsedPacket &packet,
                                           const std::uint16_t handle) noexcept {
  for (std::size_t index = 0; index < packet.count; ++index) {
    if (packet.records[index].handle == handle) {
      return &packet.records[index];
    }
  }
  return nullptr;
}

void check_packet_header(const ParsedPacket &packet, const std::uint16_t sequence,
                         const std::uint32_t token, const bool started) {
  CHECK(packet.valid);
  CHECK(packet.format == companion_protocol::kExtensibleLiveSignalFormat);
  CHECK(packet.sequence == sequence);
  CHECK(packet.flags == (started ? 1U : 0U));
  CHECK(packet.token == token);
}

void check_wire_boolean(const ParsedPacket &packet, const SignalCatalogExport &catalog,
                        const SignalId id, const Availability availability, const bool expected) {
  const WireRecord *const record = record_for(packet, wire_handle_for(catalog, id));
  REQUIRE(record != nullptr);
  CHECK(record->type == 1U);
  CHECK(record->status ==
        (static_cast<std::uint8_t>(companion_protocol::status_code(availability)) | 0x08U));
  REQUIRE(record->value_size == 1U);
  CHECK(record->value[0] == (expected ? 1U : 0U));
}

void check_wire_number(const ParsedPacket &packet, const SignalCatalogExport &catalog,
                       const SignalId id, const Availability availability, const float expected) {
  const WireRecord *const record = record_for(packet, wire_handle_for(catalog, id));
  REQUIRE(record != nullptr);
  CHECK(record->type == 2U);
  CHECK(record->status ==
        (static_cast<std::uint8_t>(companion_protocol::status_code(availability)) | 0x08U));
  REQUIRE(record->value_size == 4U);
  const std::uint32_t bits = static_cast<std::uint32_t>(record->value[0]) |
                             (static_cast<std::uint32_t>(record->value[1]) << 8U) |
                             (static_cast<std::uint32_t>(record->value[2]) << 16U) |
                             (static_cast<std::uint32_t>(record->value[3]) << 24U);
  float value = 0.0F;
  std::memcpy(&value, &bits, sizeof(value));
  CHECK(value == expected);
}

void check_wire_enum(const ParsedPacket &packet, const SignalCatalogExport &catalog,
                     const SignalId id, const Availability availability,
                     const std::uint16_t expected_choice_handle) {
  const WireRecord *const record = record_for(packet, wire_handle_for(catalog, id));
  REQUIRE(record != nullptr);
  CHECK(record->type == 3U);
  CHECK(record->status ==
        (static_cast<std::uint8_t>(companion_protocol::status_code(availability)) | 0x08U));
  REQUIRE(record->value_size == 2U);
  const std::uint16_t choice_handle =
      static_cast<std::uint16_t>(record->value[0] | (record->value[1] << 8U));
  CHECK(choice_handle == expected_choice_handle);
}

void process(ProfileProvider &provider, FakeClock &clock,
             const vehicle_core::RawCanFrame &frame) noexcept {
  clock.set(frame.timestamp_us);
  const auto result = ProviderAccess::process(provider, frame);
  ProviderAccess::deliver(provider, result);
}

void check_populated_consumers(
    ProfileProvider &provider, FakeClock &clock,
    const std::array<vehicle_core::RawCanFrame, 3> &frames, const char *boolean_key,
    const char *number_key, const SignalId boolean_id, const SignalId number_id,
    const SignalId enum_id, const float expected_number, const std::uint16_t strict_action,
    const std::uint16_t lenient_action, const Availability number_availability,
    const std::uint16_t expected_enum_value, const std::uint16_t enum_choice_handle,
    const bool check_declared_deadline) {
  RecordingActionSink sink;
  ActionEngine engine{provider};
  REQUIRE(engine.add_sink(sink) == ConfigStatus::Ok);
  REQUIRE(engine.add_sampled_state_rule(SampledStateRuleConfig{
              SignalCondition{boolean_key, Comparison::Equal, RuleOperand::boolean(true)},
              ActionId{strict_action}, FreshnessRequirement::Fresh}) == ConfigStatus::Ok);
  REQUIRE(engine.add_sampled_state_rule(SampledStateRuleConfig{
              SignalCondition{boolean_key, Comparison::Equal, RuleOperand::boolean(true)},
              ActionId{static_cast<std::uint16_t>(strict_action + 1U)},
              FreshnessRequirement::FreshOrUnverified}) == ConfigStatus::Ok);
  REQUIRE(engine.add_range_rule(RangeRuleConfig{number_key,
                                                {0.0F, 100.0F},
                                                {0.0F, 1.0F},
                                                ActionId{lenient_action},
                                                FreshnessRequirement::FreshOrUnverified}) ==
          ConfigStatus::Ok);
  REQUIRE(engine.add_range_rule(
              RangeRuleConfig{number_key,
                              {0.0F, 100.0F},
                              {0.0F, 1.0F},
                              ActionId{static_cast<std::uint16_t>(lenient_action + 1U)},
                              FreshnessRequirement::Fresh}) == ConfigStatus::Ok);

  RecordingObserver observer;
  replay::SignalObserverFanOut fan_out{provider, clock, {&observer}};
  REQUIRE(engine.attach() == vehicle_signals::SignalStatus::Ok);
  REQUIRE(fan_out.attach());
  fan_out.open();
  ProviderAccess::set_running(provider, true);
  ProviderAccess::deliver(provider, {});

  for (const auto &frame : frames) {
    process(provider, clock, frame);
  }

  const auto boolean_reading = provider.read(boolean_id);
  REQUIRE(boolean_reading.ok());
  REQUIRE(boolean_reading.value->value.has_value());
  CHECK(boolean_reading.value->availability == Availability::FreshnessUnverified);
  CHECK(boolean_reading.value->value->as_boolean() == true);
  const auto number_reading = provider.read(number_id);
  REQUIRE(number_reading.ok());
  REQUIRE(number_reading.value->value.has_value());
  CHECK(number_reading.value->availability == number_availability);
  CHECK(number_reading.value->value->as_number() == expected_number);
  const auto enum_reading = provider.read(enum_id);
  REQUIRE(enum_reading.ok());
  REQUIRE(enum_reading.value->value.has_value());
  CHECK(enum_reading.value->availability == Availability::FreshnessUnverified);
  CHECK(enum_reading.value->value->as_enumeration() == expected_enum_value);

  REQUIRE(engine.sample_polled_rules() == vehicle_signals::SignalStatus::Ok);
  CHECK(sink.contains(ActionId{strict_action}, ActionCommandKind::Deactivate));
  CHECK(sink.contains(ActionId{static_cast<std::uint16_t>(strict_action + 1U)},
                      ActionCommandKind::Activate));
  CHECK(sink.contains(ActionId{lenient_action}, ActionCommandKind::SetLevel));
  if (number_availability == Availability::FreshnessUnverified) {
    CHECK(sink.contains(ActionId{static_cast<std::uint16_t>(lenient_action + 1U)},
                        ActionCommandKind::Deactivate));
  } else {
    CHECK(sink.contains(ActionId{static_cast<std::uint16_t>(lenient_action + 1U)},
                        ActionCommandKind::SetLevel));
  }

  CHECK(fan_out.sample());
  CHECK(observer.catalog_calls() == 1U);
  CHECK(observer.catalog_size() == 3U);
  CHECK(observer.reading_count() >= 3U);
  const SignalReading *const observed_number = observer.find(number_id);
  REQUIRE(observed_number != nullptr);
  REQUIRE(observed_number->value.has_value());
  CHECK(observed_number->value->as_number() == expected_number);

  const SignalCatalogExport catalog_export{provider.catalog()};
  REQUIRE(catalog_export.available());
  CHECK(catalog_export.signal_count() == 3U);
  CHECK(wire_handle_for(catalog_export, boolean_id) != 0U);
  CHECK(wire_handle_for(catalog_export, number_id) != 0U);
  CHECK(wire_handle_for(catalog_export, enum_id) != 0U);
  const auto page = catalog_export.encode_page(0x10203040U, 0U);
  REQUIRE(page.has_value());
  CHECK((*page)[0] == companion_protocol::kSignalCatalogFormat);
  CHECK((*page)[11] == companion_protocol::kSignalCatalogFormat);
  CHECK((*page)[12] == 3U);
  CHECK((*page)[13] == 0U);
  const ExtensibleLiveSignalSampler sampler{catalog_export, provider};
  const auto packet = sampler.sample(0x10203040U, 7U, true, 247U);
  const ParsedPacket parsed = parse_packet(packet);
  check_packet_header(parsed, 7U, 0x10203040U, true);
  CHECK(parsed.count == 3U);
  check_wire_boolean(parsed, catalog_export, boolean_id, Availability::FreshnessUnverified, true);
  check_wire_number(parsed, catalog_export, number_id, number_availability, expected_number);
  check_wire_enum(parsed, catalog_export, enum_id, Availability::FreshnessUnverified,
                  enum_choice_handle);

  if (check_declared_deadline) {
    const auto number_time = frames[1].timestamp_us;
    clock.set(number_time + 500'000U);
    const auto at_deadline = ProviderAccess::service(provider);
    ProviderAccess::deliver(provider, at_deadline);
    const auto inclusive = provider.read(number_id);
    REQUIRE(inclusive.ok());
    CHECK(inclusive.value->availability == Availability::Fresh);

    clock.set(number_time + 500'001U);
    const auto after_deadline = ProviderAccess::service(provider);
    ProviderAccess::deliver(provider, after_deadline);
    const auto expired = provider.read(number_id);
    REQUIRE(expired.ok());
    CHECK(expired.value->availability == Availability::Stale);
    REQUIRE(engine.sample_polled_rules() == vehicle_signals::SignalStatus::Ok);
    CHECK(sink.contains(ActionId{static_cast<std::uint16_t>(lenient_action + 1U)},
                        ActionCommandKind::Deactivate));
  }

  CHECK(engine.detach() == vehicle_signals::SignalStatus::Ok);
  // The provider stop quiesces and clears every borrowed callback registration.
  // The fan-out's handles are therefore intentionally not unsubscribed again.
  ProviderAccess::set_running(provider, false);
}

void check_empty_consumers(ProfileProvider &provider, FakeClock &clock) {
  RecordingActionSink sink;
  ActionEngine engine{provider};
  REQUIRE(engine.add_sink(sink) == ConfigStatus::Ok);
  REQUIRE(engine.attach() == vehicle_signals::SignalStatus::Ok);
  CHECK(engine.sample_polled_rules() == vehicle_signals::SignalStatus::Ok);
  CHECK(sink.size() == 0U);

  RecordingObserver observer;
  replay::SignalObserverFanOut fan_out{provider, clock, {&observer}};
  REQUIRE(fan_out.attach());
  fan_out.open();
  CHECK(fan_out.sample());
  CHECK(observer.catalog_calls() == 1U);
  CHECK(observer.catalog_size() == 0U);
  CHECK(observer.reading_count() == 0U);

  const SignalCatalogExport catalog_export{provider.catalog()};
  CHECK(catalog_export.available());
  CHECK(catalog_export.signal_count() == 0U);
  const auto page = catalog_export.encode_page(0x10203040U, 0U);
  REQUIRE(page.has_value());
  CHECK(page->size() == companion_protocol::kSignalCatalogPageHeaderBytes + 3U);
  CHECK((*page)[0] == companion_protocol::kSignalCatalogFormat);
  CHECK((*page)[11] == companion_protocol::kSignalCatalogFormat);
  CHECK((*page)[12] == 0U);
  CHECK((*page)[13] == 0U);

  const ExtensibleLiveSignalSampler sampler{catalog_export, provider};
  const auto packet = sampler.sample(0x10203040U, 9U, false, 247U);
  CHECK(packet.size() == companion_protocol::kExtensibleLiveSignalHeaderBytes);
  const ParsedPacket parsed = parse_packet(packet);
  check_packet_header(parsed, 9U, 0x10203040U, false);
  CHECK(parsed.count == 0U);
  CHECK(provider.read(SignalId{1}).status == vehicle_signals::SignalStatus::InvalidSignal);

  const auto ignored = ProviderAccess::process(provider, make_frame(0x101U, 1U, 1U));
  CHECK(ignored.status == vehicle_profile_runtime::ProviderProcessStatus::Ignored);
  CHECK(engine.detach() == vehicle_signals::SignalStatus::Ok);
  CHECK(fan_out.detach());
}

void ignore_notice(void *, const vehicle_signals::SignalNotification &) noexcept {}

TEST_CASE("generated populated profiles and an empty profile use generic rule, replay, and codec "
          "consumers") {
  CHECK(generated_profile_a::vehicle_profile::kSelectedMessageCount == 3U);
  CHECK(generated_profile_b::vehicle_profile::kSelectedMessageCount == 3U);
  CHECK(generated_profile_empty::vehicle_profile::kSelectedMessageCount == 0U);

  FakeClock clock_a;
  FakeClock clock_b;
  FakeClock empty_clock;
  FakeSynchronization synchronization_a;
  FakeSynchronization synchronization_b;
  FakeSynchronization empty_synchronization;
  ProfileProvider profile_a{kProfileATables, clock_a, synchronization_a.binding()};
  ProfileProvider profile_b{kProfileBTables, clock_b, synchronization_b.binding()};
  ProfileProvider empty{kEmptyTables, empty_clock, empty_synchronization.binding()};

  const std::array profile_a_frames{make_frame(0x101U, 1'000U, 1U), make_frame(0x102U, 2'000U, 17U),
                                    make_frame(0x103U, 3'000U, 1U)};
  const std::array profile_b_frames{make_frame(0x201U, 4'000U, 1U), make_frame(0x202U, 5'000U, 5U),
                                    make_frame(0x203U, 6'000U, 3U)};

  check_populated_consumers(profile_a, clock_a, profile_a_frames, "vehicle.boolean",
                            "vehicle.number", SignalId{101U}, SignalId{102U}, SignalId{103U}, 17.0F,
                            71U, 73U, Availability::Fresh, 7U, 2U, true);
  check_populated_consumers(profile_b, clock_b, profile_b_frames, "profile_b.active",
                            "profile_b.level", SignalId{201U}, SignalId{202U}, SignalId{203U},
                            15.0F, 81U, 83U, Availability::FreshnessUnverified, 22U, 1U, false);
  check_empty_consumers(empty, empty_clock);
}

TEST_CASE("generated provider and populated adapter have fixed steady application storage") {
  FakeClock clock;
  FakeSynchronization synchronization;
  alignas(ProfileProvider) std::byte provider_storage[sizeof(ProfileProvider)]{};
  begin_allocation_watch();
  auto *const provider = ::new (static_cast<void *>(provider_storage))
      ProfileProvider{kProfileATables, clock, synchronization.binding()};
  const std::size_t provider_construction_allocations = end_allocation_watch();
  CHECK(provider_construction_allocations == 0U);

  int callback_context = 0;
  begin_allocation_watch();
  const auto extra_subscription =
      provider->subscribe(SignalId{102U}, &ignore_notice, &callback_context);
  const std::size_t subscribe_allocations = end_allocation_watch();
  REQUIRE(extra_subscription.ok());
  CHECK(subscribe_allocations == 0U);

  begin_allocation_watch();
  const auto unsubscribed = provider->unsubscribe(*extra_subscription.value);
  const std::size_t unsubscribe_allocations = end_allocation_watch();
  CHECK(unsubscribed.ok());
  CHECK(unsubscribe_allocations == 0U);

  ProviderAccess::set_running(*provider, true);
  begin_allocation_watch();
  const auto processed = ProviderAccess::process(*provider, make_frame(0x102U, 7'000U, 10U));
  ProviderAccess::deliver(*provider, processed);
  const auto reading = provider->read(SignalId{102U});
  const std::size_t ingest_dispatch_read_allocations = end_allocation_watch();
  CHECK(processed.status == vehicle_profile_runtime::ProviderProcessStatus::Applied);
  CHECK(reading.ok());
  CHECK(ingest_dispatch_read_allocations == 0U);
  ProviderAccess::set_running(*provider, false);
  provider->~ProfileProvider();

  // Direct pinned Runtime 0.2.1 startup is the platform worker reservation
  // baseline. Its C++ operator-new count is expected during startup only.
  // This guard does not see pthread/libc allocator calls and does not measure
  // the whole process heap.
  FakeClock core_clock;
  QueuedSource core_source;
  EmptyProcessor core_processor;
  EmptyObserver core_observer;
  Runtime core_runtime{core_source, core_processor, core_observer, core_clock};
  begin_allocation_watch();
  const auto core_started = core_runtime.start();
  const std::size_t core_start_allocations = end_allocation_watch();
  CHECK(core_started.ok());
  CHECK(core_runtime.stop().ok());

  FakeClock service_clock;
  QueuedSource service_source;
  alignas(ProfileRuntimeService) std::byte service_storage[sizeof(ProfileRuntimeService)]{};
  begin_allocation_watch();
  auto *const service = ::new (static_cast<void *>(service_storage))
      ProfileRuntimeService{kProfileATables, service_clock, service_source};
  const std::size_t service_construction_allocations = end_allocation_watch();
  CHECK(service_construction_allocations == 0U);
  ActionEngine engine{service->provider()};
  RecordingActionSink sink;
  REQUIRE(engine.add_sink(sink) == ConfigStatus::Ok);
  REQUIRE(engine.add_state_rule(StateRuleConfig{
              SignalCondition{"vehicle.number", Comparison::Greater, RuleOperand::number(0.0F)},
              ActionId{91U}, FreshnessRequirement::Fresh}) == ConfigStatus::Ok);
  REQUIRE(engine.add_sampled_state_rule(SampledStateRuleConfig{
              SignalCondition{"vehicle.boolean", Comparison::Equal, RuleOperand::boolean(true)},
              ActionId{92U}, FreshnessRequirement::Fresh}) == ConfigStatus::Ok);

  begin_allocation_watch();
  const auto first_attach = engine.attach();
  const auto first_detach = engine.detach();
  const auto second_attach = engine.attach();
  const std::size_t subscription_change_allocations = end_allocation_watch();
  CHECK(first_attach == vehicle_signals::SignalStatus::Ok);
  CHECK(first_detach == vehicle_signals::SignalStatus::Ok);
  CHECK(second_attach == vehicle_signals::SignalStatus::Ok);
  CHECK(subscription_change_allocations == 0U);

  begin_allocation_watch();
  const auto service_started = service->start();
  const std::size_t service_start_allocations = end_allocation_watch();
  CHECK(service_started.ok());
  CHECK(service_start_allocations == core_start_allocations);

  begin_allocation_watch();
  service_source.push(make_frame(0x101U, 10'000U, 1U));
  service_source.push(make_frame(0x102U, 11'000U, 10U));
  service_source.push(make_frame(0x103U, 12'000U, 1U));
  const auto number_reading = service->provider().read(SignalId{102U});
  const auto polled = engine.sample_polled_rules();
  const bool dispatched = sink.wait_contains(ActionId{91U}, ActionCommandKind::Activate,
                                             std::chrono::milliseconds{500});
  const std::size_t steady_adapter_allocations = end_allocation_watch();
  CHECK(number_reading.ok());
  CHECK(polled == vehicle_signals::SignalStatus::Ok);
  CHECK(dispatched);
  CHECK(steady_adapter_allocations == 0U);

  begin_allocation_watch();
  const auto service_stopped = service->stop();
  const std::size_t service_stop_allocations = end_allocation_watch();
  CHECK(service_stopped.ok());
  CHECK(service_stop_allocations == 0U);
  service->~ProfileRuntimeService();
}

} // namespace
