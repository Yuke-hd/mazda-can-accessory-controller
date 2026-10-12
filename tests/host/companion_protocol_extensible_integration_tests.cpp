#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "companion_protocol/config_transfer.hpp"
#include "companion_protocol/crc32.hpp"
#include "companion_protocol/device_info.hpp"
#include "companion_protocol/extensible_live_stream.hpp"
#include "companion_protocol/live_signal_stream.hpp"
#include "companion_protocol/live_signals.hpp"
#include "companion_protocol/signal_catalog_export.hpp"
#include "companion_protocol/signal_catalog_selector.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace companion_protocol;
using namespace vehicle_signals;
using std::chrono::milliseconds;

constexpr std::size_t kSyntheticSignalCount = 64;
constexpr std::size_t kGenericSignalCount = kSyntheticSignalCount - 4;
constexpr std::uint32_t kInitialToken = 0x10203040U;
constexpr std::uint32_t kReconnectToken = 0xA0B0C0D0U;
constexpr std::uint8_t kBooleanType = 1;
constexpr std::uint8_t kNumberType = 2;
constexpr std::uint8_t kEnumType = 3;
constexpr std::uint8_t kValuePresent = 0x08;

static_assert(kSignalCatalogFormat == 1, "catalog format stays at version 1");
static_assert(kExtensibleLiveSignalFormat == 1, "live packet format stays at version 1");
static_assert(kLiveSignalLayoutVersion == 2, "legacy Live signals layout stays at version 2");
static_assert(kLiveFrameBytes == 28, "legacy Live signals frame size stays unchanged");

constexpr SignalEnumChoice kInitialModeChoices[] = {{0x0010, "off"}, {0x0020, "drive"}};
constexpr SignalEnumChoice kReconnectModeChoices[] = {
    {0x0010, "off"}, {0x0020, "drive"}, {0x0030, "tow"}};

SignalMetadata signal(const std::uint16_t id, const std::string_view key, const SignalType type,
                      const SignalUnit unit = SignalUnit::None,
                      const SignalEnumChoice *choices = nullptr,
                      const std::size_t choice_count = 0) {
  return SignalMetadata{
      SignalId{id},           key,     type,        unit, ValidationStatus::Confirmed,
      SignalCapability::Read, choices, choice_count};
}

class SyntheticProvider final : public SignalProvider {
public:
  void bind(const SignalCatalogView catalog) noexcept { catalog_ = catalog; }

  [[nodiscard]] SignalCatalogView catalog() const noexcept override { return catalog_; }

  [[nodiscard]] SignalResult<SignalReading> read(const SignalId id) const noexcept override {
    for (std::size_t index = 0; index < catalog_.size(); ++index) {
      const SignalMetadata *metadata = catalog_.at(index);
      if (metadata != nullptr && metadata->id == id) {
        return SignalResult<SignalReading>::success(readings_[index]);
      }
    }
    return SignalResult<SignalReading>::failure(SignalStatus::InvalidSignal);
  }

  [[nodiscard]] SignalResult<SignalSubscription> subscribe(SignalId, SignalCallback,
                                                           void *) noexcept override {
    return SignalResult<SignalSubscription>::failure(SignalStatus::UnsupportedCapability);
  }

  [[nodiscard]] SignalStatusResult unsubscribe(SignalSubscription) noexcept override {
    return SignalStatusResult::failure(SignalStatus::InvalidSubscription);
  }

  void set(const SignalId id, SignalReading reading) noexcept {
    for (std::size_t index = 0; index < catalog_.size(); ++index) {
      const SignalMetadata *metadata = catalog_.at(index);
      if (metadata != nullptr && metadata->id == id) {
        readings_[index] = std::move(reading);
        return;
      }
    }
  }

private:
  SignalCatalogView catalog_{};
  std::array<SignalReading, kSyntheticSignalCount> readings_{};
};

struct SyntheticFixture {
  explicit SyntheticFixture(const bool reconnect = false) {
    constexpr std::array<std::uint16_t, 4> special_ids{11, 22, 33, 44};
    constexpr std::array<std::string_view, 4> special_keys{
        "synthetic.enabled", "synthetic.signed_number", "synthetic.mode", "vehicle.engine_rpm"};
    constexpr std::array<SignalType, 4> special_types{SignalType::Boolean, SignalType::Number,
                                                      SignalType::Enum, SignalType::Number};

    for (std::size_t index = 0; index < generic_keys.size(); ++index) {
      generic_keys[index] = "synthetic.metric." + std::to_string(index);
    }

    if (reconnect) {
      entries[0] = signal(special_ids[2], special_keys[2], special_types[2], SignalUnit::None,
                          kReconnectModeChoices, std::size(kReconnectModeChoices));
      entries[1] = signal(special_ids[0], special_keys[0], special_types[0]);
      entries[2] = signal(special_ids[3], special_keys[3], special_types[3],
                          SignalUnit::RevolutionsPerMinute);
      entries[3] = signal(special_ids[1], special_keys[1], special_types[1],
                          SignalUnit::MetresPerSecondSquared);
      for (std::size_t index = 0; index < generic_keys.size(); ++index) {
        entries[4 + index] = signal(static_cast<std::uint16_t>(100 + index), generic_keys[index],
                                    SignalType::Number);
      }
    } else {
      entries[0] = signal(special_ids[0], special_keys[0], special_types[0]);
      entries[1] = signal(special_ids[1], special_keys[1], special_types[1],
                          SignalUnit::MetresPerSecondSquared);
      entries[2] = signal(special_ids[2], special_keys[2], special_types[2], SignalUnit::None,
                          kInitialModeChoices, std::size(kInitialModeChoices));
      entries[3] = signal(special_ids[3], special_keys[3], special_types[3],
                          SignalUnit::RevolutionsPerMinute);
      for (std::size_t index = 0; index < generic_keys.size(); ++index) {
        entries[4 + index] = signal(static_cast<std::uint16_t>(100 + index), generic_keys[index],
                                    SignalType::Number);
      }
    }

    provider.bind(SignalCatalogView{entries.data(), entries.size()});
    catalog.emplace(provider.catalog());
    provider.set(SignalId{11}, reading(SignalValue::boolean(true)));
    provider.set(SignalId{22}, reading(SignalValue::number(reconnect ? -4.5F : -12.5F)));
    provider.set(SignalId{33}, reading(SignalValue::enumeration(reconnect ? 0x0030 : 0x0020)));
    provider.set(SignalId{44}, reading(SignalValue::number(reconnect ? 720.0F : 650.0F)));
    for (std::size_t index = 0; index < generic_keys.size(); ++index) {
      provider.set(SignalId{static_cast<std::uint16_t>(100 + index)},
                   reading(SignalValue::number(static_cast<float>(index))));
    }
  }

  static SignalReading reading(SignalValue value) {
    return SignalReading{std::move(value), Availability::Fresh, ValidationStatus::Confirmed};
  }

  std::array<std::string, kGenericSignalCount> generic_keys{};
  std::array<SignalMetadata, kSyntheticSignalCount> entries{};
  SyntheticProvider provider{};
  std::optional<SignalCatalogExport> catalog{};
};

struct DiscoveredSignal {
  std::uint16_t handle{0};
  std::uint8_t type{0};
  std::string key{};
  std::vector<std::pair<std::uint16_t, std::string>> choices{};
};

struct WireRecord {
  std::uint16_t handle{0};
  std::uint8_t type{0};
  std::uint8_t status{0};
  std::vector<std::uint8_t> value{};

  friend bool operator==(const WireRecord &left, const WireRecord &right) {
    return left.handle == right.handle && left.type == right.type && left.status == right.status &&
           left.value == right.value;
  }
};

std::uint16_t read_u16(const ByteView bytes, const std::size_t offset) {
  return static_cast<std::uint16_t>(bytes[offset]) |
         (static_cast<std::uint16_t>(bytes[offset + 1] << 8U));
}

std::uint32_t read_u32(const ByteView bytes, const std::size_t offset) {
  return static_cast<std::uint32_t>(bytes[offset]) |
         (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
         (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) |
         (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
}

void append_u16(std::vector<std::uint8_t> &bytes, const std::uint16_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value & 0xFFU));
  bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
}

void append_u32(std::vector<std::uint8_t> &bytes, const std::uint32_t value) {
  for (std::size_t shift = 0; shift < 32; shift += 8) {
    bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
  }
}

std::vector<std::uint8_t> config_start_pdu(const std::vector<std::uint8_t> &document) {
  const ByteView view{document.data(), document.size()};
  std::vector<std::uint8_t> pdu{0x01};
  append_u16(pdu, static_cast<std::uint16_t>(document.size()));
  append_u32(pdu, crc32(view));
  return pdu;
}

std::vector<std::uint8_t> config_chunk_pdu(const std::vector<std::uint8_t> &document) {
  std::vector<std::uint8_t> pdu{0x02};
  append_u16(pdu, 0);
  pdu.insert(pdu.end(), document.begin(), document.end());
  return pdu;
}

bool parse_document(const std::vector<std::uint8_t> &document,
                    std::vector<DiscoveredSignal> &signals) {
  if (document.size() < 3 || document[0] != kSignalCatalogFormat) {
    return false;
  }
  const std::uint16_t count =
      static_cast<std::uint16_t>(document[1]) | (static_cast<std::uint16_t>(document[2]) << 8U);
  if (count > kMaxSignalCatalogSignals) {
    return false;
  }

  std::size_t cursor = 3;
  signals.clear();
  signals.reserve(count);
  for (std::uint16_t index = 0; index < count; ++index) {
    if (cursor + 2 > document.size()) {
      return false;
    }
    const std::uint16_t body_length = static_cast<std::uint16_t>(document[cursor]) |
                                      (static_cast<std::uint16_t>(document[cursor + 1]) << 8U);
    cursor += 2;
    if (body_length < 2 || cursor + body_length > document.size()) {
      return false;
    }

    const std::size_t body_end = cursor + body_length;
    DiscoveredSignal signal_entry{};
    signal_entry.handle = static_cast<std::uint16_t>(document[cursor]) |
                          (static_cast<std::uint16_t>(document[cursor + 1]) << 8U);
    cursor += 2;
    while (cursor < body_end) {
      if (cursor + 3 > body_end) {
        return false;
      }
      const std::uint8_t tag = document[cursor++];
      const std::uint16_t length = static_cast<std::uint16_t>(document[cursor]) |
                                   (static_cast<std::uint16_t>(document[cursor + 1]) << 8U);
      cursor += 2;
      if (cursor + length > body_end) {
        return false;
      }
      if (tag == 1) {
        signal_entry.key.assign(reinterpret_cast<const char *>(document.data() + cursor), length);
      } else if (tag == 2) {
        if (length != 1) {
          return false;
        }
        signal_entry.type = document[cursor];
      } else if (tag == 5) {
        const std::size_t choices_end = cursor + length;
        std::size_t choice_cursor = cursor;
        if (choice_cursor + 2 > choices_end) {
          return false;
        }
        const std::uint16_t choice_count =
            static_cast<std::uint16_t>(document[choice_cursor]) |
            (static_cast<std::uint16_t>(document[choice_cursor + 1]) << 8U);
        choice_cursor += 2;
        for (std::uint16_t choice_index = 0; choice_index < choice_count; ++choice_index) {
          if (choice_cursor + 3 > choices_end) {
            return false;
          }
          const std::uint16_t choice_handle =
              static_cast<std::uint16_t>(document[choice_cursor]) |
              (static_cast<std::uint16_t>(document[choice_cursor + 1]) << 8U);
          const std::uint8_t key_length = document[choice_cursor + 2];
          choice_cursor += 3;
          if (choice_cursor + key_length > choices_end) {
            return false;
          }
          signal_entry.choices.emplace_back(
              choice_handle,
              std::string{reinterpret_cast<const char *>(document.data() + choice_cursor),
                          key_length});
          choice_cursor += key_length;
        }
        if (choice_cursor != choices_end) {
          return false;
        }
      }
      cursor += length;
    }
    if (cursor != body_end || signal_entry.handle == 0 || signal_entry.key.empty() ||
        signal_entry.type < kBooleanType || signal_entry.type > kEnumType) {
      return false;
    }
    signals.push_back(std::move(signal_entry));
  }
  return cursor == document.size() && signals.size() == count;
}

class DiscoveryConsumer final {
public:
  void begin_connection() {
    committed_token_ = 0;
    committed_signals_.clear();
    staged_token_ = 0;
    staged_total_ = 0;
    staged_document_.clear();
  }

  bool receive_page(const EncodedSignalCatalogPage &page) {
    const ByteView bytes = page.view();
    if (bytes.size() < kSignalCatalogPageHeaderBytes || bytes[0] != kSignalCatalogFormat) {
      reset_staging();
      return false;
    }
    const std::uint32_t token = read_u32(bytes, 1);
    const std::uint16_t total = read_u16(bytes, 5);
    const std::uint16_t offset = read_u16(bytes, 7);
    const std::uint16_t data_length = read_u16(bytes, 9);
    if (token == 0 || total < 3 || bytes.size() != kSignalCatalogPageHeaderBytes + data_length ||
        static_cast<std::size_t>(offset) + data_length > total) {
      reset_staging();
      return false;
    }
    if (offset == 0) {
      staged_token_ = token;
      staged_total_ = total;
      staged_document_.clear();
      staged_document_.reserve(total);
    }
    if (staged_token_ != token || staged_total_ != total || offset != staged_document_.size()) {
      reset_staging();
      return false;
    }

    for (std::size_t index = 0; index < data_length; ++index) {
      staged_document_.push_back(bytes[kSignalCatalogPageHeaderBytes + index]);
    }
    if (staged_document_.size() != staged_total_) {
      return true;
    }

    std::vector<DiscoveredSignal> parsed{};
    if (!parse_document(staged_document_, parsed)) {
      reset_staging();
      return false;
    }
    committed_token_ = staged_token_;
    committed_signals_ = std::move(parsed);
    reset_staging();
    return true;
  }

  bool ingest(const ExtensibleLiveSignalPacket &packet, const bool stack_accepted) {
    if (!stack_accepted || packet.size() < kExtensibleLiveSignalHeaderBytes ||
        packet[0] != kExtensibleLiveSignalFormat ||
        read_u32(packet.view(), 4) != committed_token_ || committed_token_ == 0) {
      return false;
    }

    std::vector<std::pair<std::string, WireRecord>> updates{};
    std::size_t cursor = kExtensibleLiveSignalHeaderBytes;
    while (cursor < packet.size()) {
      const std::size_t record_length = packet[cursor];
      if (record_length < kExtensibleLiveSignalRecordHeaderBytes - 1 ||
          cursor + 1 + record_length > packet.size()) {
        return false;
      }
      WireRecord record{};
      record.handle = static_cast<std::uint16_t>(packet[cursor + 1]) |
                      (static_cast<std::uint16_t>(packet[cursor + 2]) << 8U);
      record.type = packet[cursor + 3];
      record.status = packet[cursor + 4];
      const DiscoveredSignal *metadata = signal_for_handle(record.handle);
      if (metadata == nullptr || metadata->type != record.type) {
        return false;
      }
      const std::size_t value_length = record_length - (kExtensibleLiveSignalRecordHeaderBytes - 1);
      const bool value_present = (record.status & kValuePresent) != 0;
      if ((value_present && value_length != expected_value_length(record.type)) ||
          (!value_present && value_length != 0)) {
        return false;
      }
      record.value.reserve(value_length);
      for (std::size_t index = 0; index < value_length; ++index) {
        record.value.push_back(packet[cursor + kExtensibleLiveSignalRecordHeaderBytes + index]);
      }
      updates.emplace_back(metadata->key, std::move(record));
      cursor += 1 + record_length;
    }

    for (auto &update : updates) {
      known_state_[std::move(update.first)] = std::move(update.second);
    }
    return true;
  }

  [[nodiscard]] std::uint32_t token() const noexcept { return committed_token_; }

  [[nodiscard]] const std::vector<DiscoveredSignal> &signals() const noexcept {
    return committed_signals_;
  }

  [[nodiscard]] const std::map<std::string, WireRecord> &known_state() const noexcept {
    return known_state_;
  }

  [[nodiscard]] const DiscoveredSignal *signal_for_key(const std::string_view key) const {
    const auto found = std::find_if(
        committed_signals_.begin(), committed_signals_.end(),
        [key](const DiscoveredSignal &signal_entry) { return signal_entry.key == key; });
    return found == committed_signals_.end() ? nullptr : &*found;
  }

  [[nodiscard]] const WireRecord *known(const std::string_view key) const {
    const auto found = known_state_.find(std::string{key});
    return found == known_state_.end() ? nullptr : &found->second;
  }

private:
  static std::size_t expected_value_length(const std::uint8_t type) {
    switch (type) {
    case kBooleanType:
      return 1;
    case kNumberType:
      return 4;
    case kEnumType:
      return 2;
    default:
      return 0;
    }
  }

  [[nodiscard]] const DiscoveredSignal *signal_for_handle(const std::uint16_t handle) const {
    const auto found = std::find_if(
        committed_signals_.begin(), committed_signals_.end(),
        [handle](const DiscoveredSignal &signal_entry) { return signal_entry.handle == handle; });
    return found == committed_signals_.end() ? nullptr : &*found;
  }

  void reset_staging() {
    staged_token_ = 0;
    staged_total_ = 0;
    staged_document_.clear();
  }

  std::uint32_t committed_token_{0};
  std::vector<DiscoveredSignal> committed_signals_{};
  std::map<std::string, WireRecord> known_state_{};
  std::uint32_t staged_token_{0};
  std::uint16_t staged_total_{0};
  std::vector<std::uint8_t> staged_document_{};
};

void select_page(SignalCatalogSelector &selector, const std::uint32_t connection,
                 const std::uint32_t token, const std::uint16_t offset) {
  BoundedBytes<kSignalCatalogSelectorBytes> request{};
  REQUIRE(request.push(kSignalCatalogFormat));
  REQUIRE(request.push_u32(token));
  REQUIRE(request.push_u16(offset));
  REQUIRE(selector.select(connection, request.view()) == AttError::None);
}

void discover_all(SignalCatalogSelector &selector, const std::uint32_t connection,
                  const std::uint32_t token, const std::uint16_t document_length,
                  DiscoveryConsumer &consumer) {
  std::uint16_t offset = 0;
  while (offset < document_length) {
    select_page(selector, connection, token, offset);
    const auto page = selector.read(connection);
    REQUIRE(page.has_value());
    REQUIRE(consumer.receive_page(*page));
    offset = static_cast<std::uint16_t>(offset + read_u16(page->view(), 9));
  }
  CHECK(offset == document_length);
}

class PacketSink final : public ExtensibleLiveFrameSink {
public:
  bool try_queue(const ExtensibleLiveSignalPacket &packet) noexcept override {
    packets.push_back(packet);
    return accept;
  }

  bool accept{true};
  std::vector<ExtensibleLiveSignalPacket> packets{};
};

class SyntheticConfigCommitter final : public ConfigCommitter {
public:
  CommitOutcome commit(const ByteView document) noexcept override {
    ++commit_count;
    return CommitOutcome::saved(
        SavedDocument{static_cast<std::uint16_t>(document.size()), crc32(document)});
  }

  std::size_t commit_count{0};
};

class LegacySink final : public LiveFrameSink {
public:
  bool try_queue(const LiveFrame &frame) noexcept override {
    frames.push_back(frame);
    return true;
  }

  std::vector<LiveFrame> frames{};
};

std::vector<std::uint16_t> packet_handles(const ExtensibleLiveSignalPacket &packet) {
  std::vector<std::uint16_t> handles{};
  std::size_t cursor = kExtensibleLiveSignalHeaderBytes;
  while (cursor < packet.size()) {
    handles.push_back(static_cast<std::uint16_t>(packet[cursor + 1]) |
                      (static_cast<std::uint16_t>(packet[cursor + 2]) << 8U));
    cursor += 1 + packet[cursor];
  }
  return handles;
}

bool packet_has_handle(const ExtensibleLiveSignalPacket &packet, const std::uint16_t handle) {
  const auto handles = packet_handles(packet);
  return std::find(handles.begin(), handles.end(), handle) != handles.end();
}

float number_value(const WireRecord &record) {
  REQUIRE(record.type == kNumberType);
  REQUIRE(record.value.size() == 4);
  const std::uint32_t bits = static_cast<std::uint32_t>(record.value[0]) |
                             (static_cast<std::uint32_t>(record.value[1]) << 8U) |
                             (static_cast<std::uint32_t>(record.value[2]) << 16U) |
                             (static_cast<std::uint32_t>(record.value[3]) << 24U);
  float value = 0.0F;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

void enable_streams(LiveSignalStream &legacy, ExtensibleLiveSignalStream &extensible,
                    const std::uint32_t token) {
  legacy.set_notifications_enabled(true);
  legacy.set_link_secured(true);
  legacy.set_att_mtu(64);
  extensible.set_notifications_enabled(true);
  extensible.set_link_secured(true);
  extensible.set_att_mtu(64);
  extensible.set_catalog_token(token);
}

} // namespace

TEST_CASE("catalog discovery and extensible streaming preserve semantic state end to end") {
  SyntheticFixture first{};
  REQUIRE(first.catalog.has_value());
  REQUIRE(first.catalog->available());
  REQUIRE(first.catalog->signal_count() == kSyntheticSignalCount);
  REQUIRE(first.catalog->document_length() > kSignalCatalogPageDataBytes);

  SignalCatalogSelector selector{*first.catalog};
  selector.connect(1, kInitialToken);
  DiscoveryConsumer consumer{};
  consumer.begin_connection();
  discover_all(selector, 1, kInitialToken, first.catalog->document_length(), consumer);
  REQUIRE(consumer.token() == kInitialToken);
  REQUIRE(consumer.signals().size() == kSyntheticSignalCount);
  REQUIRE(consumer.signal_for_key("synthetic.enabled") != nullptr);
  REQUIRE(consumer.signal_for_key("synthetic.signed_number") != nullptr);
  REQUIRE(consumer.signal_for_key("synthetic.mode") != nullptr);
  REQUIRE(consumer.signal_for_key("vehicle.engine_rpm") != nullptr);

  const auto *first_mode = consumer.signal_for_key("synthetic.mode");
  const auto *first_enabled = consumer.signal_for_key("synthetic.enabled");
  REQUIRE(first_mode != nullptr);
  REQUIRE(first_enabled != nullptr);
  const std::uint16_t first_mode_handle = first_mode->handle;
  const std::uint16_t first_enabled_handle = first_enabled->handle;
  REQUIRE(first_mode->choices.size() == 2);
  CHECK(first_mode->choices[1].first == 2);
  CHECK(first_mode->choices[1].second == "drive");

  ExtensibleLiveSignalStream extensible{*first.catalog, first.provider};
  LiveSignalStream legacy{};
  enable_streams(legacy, extensible, kInitialToken);
  REQUIRE(extensible.streaming());
  REQUIRE(legacy.streaming());

  LiveSignalSampler legacy_sampler{first.provider};
  LegacySink legacy_sink{};
  const LiveSignalContent initial_legacy = legacy_sampler.sample(true);
  REQUIRE(legacy.offer(milliseconds{0}, initial_legacy, legacy_sink));
  REQUIRE(legacy_sink.frames.size() == 1);
  CHECK(legacy_sink.frames.front()[0] == kLiveSignalLayoutVersion);
  CHECK(legacy_sink.frames.front().size() == 28);

  PacketSink packet_sink{};
  std::array<bool, kSyntheticSignalCount> observed{};
  milliseconds now{0};
  for (std::size_t slot = 0; slot < 24; ++slot) {
    now = milliseconds{static_cast<std::int64_t>(slot * 100)};
    if (!extensible.offer(now, true, packet_sink)) {
      continue;
    }
    const auto &packet = packet_sink.packets.back();
    REQUIRE(packet.size() <= 64 - kExtensibleLiveSignalMtuOverheadBytes);
    CHECK(packet[0] == kExtensibleLiveSignalFormat);
    CHECK(read_u32(packet.view(), 4) == kInitialToken);
    REQUIRE(consumer.ingest(packet, packet_sink.accept));
    for (const std::uint16_t handle : packet_handles(packet)) {
      REQUIRE(handle >= 1);
      REQUIRE(handle <= observed.size());
      observed[handle - 1] = true;
    }
    if (std::all_of(observed.begin(), observed.end(), [](const bool value) { return value; })) {
      break;
    }
  }
  CHECK(std::all_of(observed.begin(), observed.end(), [](const bool value) { return value; }));
  REQUIRE(consumer.known_state().size() == kSyntheticSignalCount);

  const WireRecord *boolean = consumer.known("synthetic.enabled");
  REQUIRE(boolean != nullptr);
  CHECK(boolean->type == kBooleanType);
  REQUIRE(boolean->value.size() == 1);
  CHECK(boolean->value[0] == 1);

  const WireRecord *signed_number = consumer.known("synthetic.signed_number");
  REQUIRE(signed_number != nullptr);
  CHECK(number_value(*signed_number) == doctest::Approx(-12.5F));

  const WireRecord *mode = consumer.known("synthetic.mode");
  REQUIRE(mode != nullptr);
  CHECK(mode->type == kEnumType);
  REQUIRE(mode->value.size() == 2);
  CHECK(read_u16(ByteView{mode->value.data(), mode->value.size()}, 0) == 2);

  const WireRecord *rpm = consumer.known("vehicle.engine_rpm");
  REQUIRE(rpm != nullptr);
  CHECK(number_value(*rpm) == doctest::Approx(650.0F));

  // Drive an actual synthetic Config transfer while both live streams are
  // subscribed. A write handler pauses extensible sampling while the legacy
  // fixed-layout subscription continues to publish its changed frame.
  SyntheticConfigCommitter config_committer{};
  ConfigTransfer config_transfer{config_committer, {true, ActiveDocument{}}};
  constexpr std::array<std::uint8_t, 8> config_bytes{'s', 'y', 'n', 't', 'h', 'e', 't', 'i'};
  const std::vector<std::uint8_t> config_document{config_bytes.begin(), config_bytes.end()};
  const auto start_pdu = config_start_pdu(config_document);
  const auto started = config_transfer.handle_write(ByteView{start_pdu.data(), start_pdu.size()},
                                                    ConfigWriteContext{64, TransferClock{1000}});
  REQUIRE(started.error == AttError::None);
  CHECK(config_transfer.status().state == TransferState::Receiving);

  extensible.set_config_paused(true);
  first.provider.set(SignalId{44}, SyntheticFixture::reading(SignalValue::number(700.0F)));
  const auto chunk_pdu = config_chunk_pdu(config_document);
  const auto chunked = config_transfer.handle_write(ByteView{chunk_pdu.data(), chunk_pdu.size()},
                                                    ConfigWriteContext{64, TransferClock{1100}});
  REQUIRE(chunked.error == AttError::None);
  CHECK(config_transfer.status().received_length == config_document.size());
  const LiveSignalContent changed_legacy = legacy_sampler.sample(true);
  const std::size_t extensible_packets_before_pause = packet_sink.packets.size();
  CHECK_FALSE(extensible.offer(now + milliseconds{100}, true, packet_sink));
  CHECK(packet_sink.packets.size() == extensible_packets_before_pause);
  REQUIRE(legacy.offer(now + milliseconds{100}, changed_legacy, legacy_sink));
  CHECK(legacy_sink.frames.size() == 2);
  CHECK(legacy_sink.frames.back()[0] == kLiveSignalLayoutVersion);
  CHECK(legacy.streaming());

  const std::array<std::uint8_t, 1> commit_pdu{0x03};
  const auto committed = config_transfer.handle_write(
      ByteView{commit_pdu.data(), commit_pdu.size()}, ConfigWriteContext{64, TransferClock{1200}});
  CHECK(committed.error == AttError::None);
  CHECK(config_transfer.restart_pending());
  CHECK(config_committer.commit_count == 1);

  extensible.set_config_paused(false);
  now += milliseconds{200};
  bool rpm_update_accepted = false;
  for (std::size_t slot = 0; slot < 24 && !rpm_update_accepted; ++slot) {
    now += milliseconds{100};
    if (!extensible.offer(now, true, packet_sink)) {
      continue;
    }
    REQUIRE(consumer.ingest(packet_sink.packets.back(), packet_sink.accept));
    rpm_update_accepted =
        consumer.known("vehicle.engine_rpm") != nullptr &&
        number_value(*consumer.known("vehicle.engine_rpm")) == doctest::Approx(700.0F);
  }
  REQUIRE(rpm_update_accepted);

  // The stream consumer keeps its previous semantic state when the BLE stack
  // refuses a packet, then applies the latest value only after a later packet
  // is accepted.
  const auto state_before_drop = consumer.known_state();
  first.provider.set(SignalId{22}, SyntheticFixture::reading(SignalValue::number(-99.25F)));
  packet_sink.accept = false;
  bool dropped_changed_record = false;
  for (std::size_t slot = 0; slot < 24 && !dropped_changed_record; ++slot) {
    now += milliseconds{100};
    if (!extensible.offer(now, true, packet_sink)) {
      continue;
    }
    const auto &packet = packet_sink.packets.back();
    const auto handles = packet_handles(packet);
    const auto *signed_entry = consumer.signal_for_key("synthetic.signed_number");
    REQUIRE(signed_entry != nullptr);
    if (std::find(handles.begin(), handles.end(), signed_entry->handle) != handles.end()) {
      dropped_changed_record = true;
      CHECK_FALSE(consumer.ingest(packet, packet_sink.accept));
      CHECK(consumer.known_state() == state_before_drop);
    }
  }
  REQUIRE(dropped_changed_record);

  packet_sink.accept = true;
  bool signed_update_accepted = false;
  for (std::size_t slot = 0; slot < 24 && !signed_update_accepted; ++slot) {
    now += milliseconds{100};
    if (!extensible.offer(now, true, packet_sink)) {
      continue;
    }
    const auto &packet = packet_sink.packets.back();
    const auto *signed_entry = consumer.signal_for_key("synthetic.signed_number");
    REQUIRE(signed_entry != nullptr);
    const bool carries_signed_number = packet_has_handle(packet, signed_entry->handle);
    REQUIRE(consumer.ingest(packet, packet_sink.accept));
    if (carries_signed_number) {
      signed_update_accepted = true;
      CHECK(number_value(*consumer.known("synthetic.signed_number")) == doctest::Approx(-99.25F));
    }
  }
  REQUIRE(signed_update_accepted);

  const auto known_before_reconnect = consumer.known_state();
  selector.disconnect(1);
  SyntheticFixture reconnected{true};
  REQUIRE(reconnected.catalog.has_value());
  selector.bind(*reconnected.catalog);
  selector.connect(2, kReconnectToken);
  consumer.begin_connection();

  select_page(selector, 2, kReconnectToken, 0);
  const auto first_page = selector.read(2);
  REQUIRE(first_page.has_value());
  REQUIRE(consumer.receive_page(*first_page));
  CHECK(consumer.token() == 0);
  CHECK(consumer.known_state() == known_before_reconnect);

  // A page gap is rejected atomically; it cannot replace the last committed
  // semantic values or establish handle bindings for this connection.
  constexpr std::uint16_t kSkippedOffset =
      static_cast<std::uint16_t>(2 * kSignalCatalogPageDataBytes);
  select_page(selector, 2, kReconnectToken, kSkippedOffset);
  const auto skipped_page = selector.read(2);
  REQUIRE(skipped_page.has_value());
  CHECK_FALSE(consumer.receive_page(*skipped_page));
  CHECK(consumer.token() == 0);
  CHECK(consumer.signals().empty());
  CHECK(consumer.known_state() == known_before_reconnect);

  discover_all(selector, 2, kReconnectToken, reconnected.catalog->document_length(), consumer);
  REQUIRE(consumer.token() == kReconnectToken);
  REQUIRE(consumer.signals().size() == kSyntheticSignalCount);
  REQUIRE(consumer.signal_for_key("synthetic.mode") != nullptr);
  REQUIRE(consumer.signal_for_key("synthetic.enabled") != nullptr);
  REQUIRE(consumer.signal_for_key("synthetic.signed_number") != nullptr);
  CHECK(consumer.signal_for_key("synthetic.mode")->handle != first_mode_handle);
  CHECK(consumer.signal_for_key("synthetic.enabled")->handle != first_enabled_handle);
  const auto *reconnected_mode = consumer.signal_for_key("synthetic.mode");
  REQUIRE(reconnected_mode->choices.size() == 3);
  CHECK(reconnected_mode->choices[2].first == 3);
  CHECK(reconnected_mode->choices[2].second == "tow");
  CHECK(consumer.known_state() == known_before_reconnect);

  ExtensibleLiveSignalStream reconnected_stream{*reconnected.catalog, reconnected.provider};
  reconnected_stream.set_notifications_enabled(true);
  reconnected_stream.set_link_secured(true);
  reconnected_stream.set_att_mtu(64);
  reconnected_stream.set_catalog_token(kReconnectToken);
  PacketSink reconnected_sink{};
  REQUIRE(reconnected_stream.streaming());
  REQUIRE(reconnected_stream.offer(milliseconds{0}, true, reconnected_sink));
  REQUIRE(consumer.ingest(reconnected_sink.packets.back(), reconnected_sink.accept));

  CHECK(number_value(*consumer.known("synthetic.signed_number")) == doctest::Approx(-4.5F));
  const WireRecord *reconnected_mode_value = consumer.known("synthetic.mode");
  REQUIRE(reconnected_mode_value != nullptr);
  REQUIRE(reconnected_mode_value->value.size() == 2);
  CHECK(
      read_u16(ByteView{reconnected_mode_value->value.data(), reconnected_mode_value->value.size()},
               0) == 3);
  CHECK(number_value(*consumer.known("vehicle.engine_rpm")) == doctest::Approx(720.0F));
  const WireRecord *reconnected_boolean = consumer.known("synthetic.enabled");
  REQUIRE(reconnected_boolean != nullptr);
  REQUIRE(reconnected_boolean->value.size() == 1);
  CHECK(reconnected_boolean->value[0] == 1);
}
