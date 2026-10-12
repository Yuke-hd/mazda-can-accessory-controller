#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "companion_protocol/extensible_live_signals.hpp"
#include "companion_protocol/live_signals.hpp"
#include "support/fake_signal_provider.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace companion_protocol;
using vehicle_signals::Availability;
using vehicle_signals::SignalCapability;
using vehicle_signals::SignalCatalogView;
using vehicle_signals::SignalEnumChoice;
using vehicle_signals::SignalId;
using vehicle_signals::SignalMetadata;
using vehicle_signals::SignalReading;
using vehicle_signals::SignalStatus;
using vehicle_signals::SignalType;
using vehicle_signals::SignalUnit;
using vehicle_signals::SignalValue;
using vehicle_signals::ValidationStatus;

#ifndef EXTENSIBLE_LIVE_FIXTURE_PATH
#error "EXTENSIBLE_LIVE_FIXTURE_PATH must point to the shared JSON fixture"
#endif

constexpr SignalEnumChoice kModeChoices[] = {
    {0xA501, "drive"},
    {0x0011, "park"},
    {0xFE00, "reverse"},
};

SignalMetadata boolean_signal(std::uint16_t id, std::string_view key) {
  return SignalMetadata{SignalId{id},
                        key,
                        SignalType::Boolean,
                        SignalUnit::None,
                        ValidationStatus::Confirmed,
                        SignalCapability::Read,
                        nullptr,
                        0};
}

SignalMetadata number_signal(std::uint16_t id, std::string_view key) {
  return SignalMetadata{SignalId{id},
                        key,
                        SignalType::Number,
                        SignalUnit::MetresPerSecondSquared,
                        ValidationStatus::Observed,
                        SignalCapability::Read,
                        nullptr,
                        0};
}

std::array<SignalMetadata, 27> maximum_packet_number_signals() {
  constexpr std::array<std::string_view, 27> keys{
      "capacity.maximum01", "capacity.maximum02", "capacity.maximum03", "capacity.maximum04",
      "capacity.maximum05", "capacity.maximum06", "capacity.maximum07", "capacity.maximum08",
      "capacity.maximum09", "capacity.maximum10", "capacity.maximum11", "capacity.maximum12",
      "capacity.maximum13", "capacity.maximum14", "capacity.maximum15", "capacity.maximum16",
      "capacity.maximum17", "capacity.maximum18", "capacity.maximum19", "capacity.maximum20",
      "capacity.maximum21", "capacity.maximum22", "capacity.maximum23", "capacity.maximum24",
      "capacity.maximum25", "capacity.maximum26", "capacity.maximum27",
  };
  std::array<SignalMetadata, 27> entries{};
  for (std::size_t index = 0; index < entries.size(); ++index) {
    entries[index] = number_signal(static_cast<std::uint16_t>(200 + index), keys[index]);
  }
  return entries;
}

SignalMetadata enum_signal(std::uint16_t id, std::string_view key) {
  return SignalMetadata{SignalId{id},
                        key,
                        SignalType::Enum,
                        SignalUnit::None,
                        ValidationStatus::Reference,
                        SignalCapability::Read,
                        kModeChoices,
                        std::size(kModeChoices)};
}

struct Fixture {
  Fixture()
      : entries{boolean_signal(11, "extra.boolean"), number_signal(22, "extra.signed_number"),
                enum_signal(33, "extra.mode"), boolean_signal(44, "vehicle.brake_pressed")},
        provider{SignalCatalogView{entries.data(), entries.size()}}, catalog{provider.catalog()} {}

  std::array<SignalMetadata, 4> entries;
  test_support::FakeSignalProvider provider;
  SignalCatalogExport catalog;
};

std::uint16_t u16(const ExtensibleLiveSignalPacket &packet, std::size_t offset) {
  return static_cast<std::uint16_t>(packet[offset]) |
         static_cast<std::uint16_t>(packet[offset + 1] << 8U);
}

std::uint32_t u32(const ExtensibleLiveSignalPacket &packet, std::size_t offset) {
  return static_cast<std::uint32_t>(u16(packet, offset)) |
         (static_cast<std::uint32_t>(u16(packet, offset + 2)) << 16U);
}

struct RecordView {
  std::uint16_t handle{0};
  std::uint8_t type{0};
  std::uint8_t status{0};
  ByteView value{};
};

std::vector<RecordView> records(const ExtensibleLiveSignalPacket &packet) {
  std::vector<RecordView> result;
  std::size_t cursor = kExtensibleLiveSignalHeaderBytes;
  while (cursor < packet.size()) {
    const std::size_t record_start = cursor;
    const std::size_t record_length = packet[cursor++];
    REQUIRE(record_length >= 4);
    REQUIRE(cursor + record_length <= packet.size());
    RecordView record{};
    record.handle = u16(packet, cursor);
    record.type = packet[cursor + 2];
    record.status = packet[cursor + 3];
    record.value = packet.view().from(cursor + 4);
    record.value = ByteView{record.value.data(), record_length - 4};
    result.push_back(record);
    cursor = record_start + 1 + record_length;
  }
  return result;
}

int fixture_hex_nibble(const char character) {
  if (character >= '0' && character <= '9') {
    return character - '0';
  }
  if (character >= 'a' && character <= 'f') {
    return character - 'a' + 10;
  }
  if (character >= 'A' && character <= 'F') {
    return character - 'A' + 10;
  }
  return -1;
}

std::vector<std::uint8_t> fixture_packet_hex(std::string_view notification_name) {
  std::ifstream input{EXTENSIBLE_LIVE_FIXTURE_PATH, std::ios::binary};
  REQUIRE(input.is_open());
  const std::string document{std::istreambuf_iterator<char>{input},
                             std::istreambuf_iterator<char>{}};
  const std::string name_needle = "\"name\": \"" + std::string(notification_name) + "\"";
  const std::size_t object_start = document.find(name_needle);
  REQUIRE(object_start != std::string::npos);
  const std::size_t object_end = document.find('}', object_start);
  REQUIRE(object_end != std::string::npos);

  constexpr std::string_view kPacketKey = "\"packet_hex\":";
  const std::size_t packet_key = document.find(kPacketKey, object_start);
  REQUIRE(packet_key != std::string::npos);
  REQUIRE(packet_key < object_end);
  const std::size_t first_quote = document.find('"', packet_key + kPacketKey.size());
  REQUIRE(first_quote != std::string::npos);
  const std::size_t second_quote = document.find('"', first_quote + 1);
  REQUIRE(second_quote != std::string::npos);
  const std::string_view hex{document.data() + first_quote + 1, second_quote - first_quote - 1};
  REQUIRE((hex.size() % 2U) == 0);

  std::vector<std::uint8_t> bytes;
  bytes.reserve(hex.size() / 2U);
  for (std::size_t index = 0; index < hex.size(); index += 2) {
    const int high = fixture_hex_nibble(hex[index]);
    const int low = fixture_hex_nibble(hex[index + 1]);
    REQUIRE(high >= 0);
    REQUIRE(low >= 0);
    bytes.push_back(static_cast<std::uint8_t>((high << 4) | low));
  }
  return bytes;
}

} // namespace

TEST_CASE("extensible packet has explicit little-endian header and catalog-keyed values") {
  Fixture fixture;
  fixture.provider.set_reading(
      SignalId{11},
      SignalReading{SignalValue::boolean(true), Availability::Fresh, ValidationStatus::Confirmed});
  fixture.provider.set_reading(
      SignalId{22},
      SignalReading{SignalValue::number(-12.5F), Availability::Stale, ValidationStatus::Observed});
  fixture.provider.set_reading(SignalId{33},
                               SignalReading{SignalValue::enumeration(0x0011), Availability::Fresh,
                                             ValidationStatus::Reference});

  const auto packet = encode_extensible_live_signals(fixture.catalog, fixture.provider, 0x78563412U,
                                                     0xA1B2U, true, 247);
  REQUIRE(packet.size() == 8 + 6 + 9 + 7 + 5);
  CHECK(packet[0] == 1);
  CHECK(u16(packet, 1) == 0xA1B2U);
  CHECK(packet[3] == 0x01);
  CHECK(u32(packet, 4) == 0x78563412U);

  const auto encoded = records(packet);
  REQUIRE(encoded.size() == 4);
  CHECK(encoded[0].handle == 1);
  CHECK(encoded[0].type == 1);
  CHECK(encoded[0].status == 0x09);
  CHECK(encoded[0].value.size() == 1);
  CHECK(encoded[0].value[0] == 1);

  CHECK(encoded[1].handle == 2);
  CHECK(encoded[1].type == 2);
  CHECK(encoded[1].status == 0x0A);
  CHECK(encoded[1].value.size() == 4);
  CHECK(encoded[1].value[0] == 0x00);
  CHECK(encoded[1].value[1] == 0x00);
  CHECK(encoded[1].value[2] == 0x48);
  CHECK(encoded[1].value[3] == 0xC1);

  CHECK(encoded[2].handle == 3);
  CHECK(encoded[2].type == 3);
  CHECK(encoded[2].status == 0x09);
  CHECK(encoded[2].value.size() == 2);
  CHECK(encoded[2].value[0] == 2);
  CHECK(encoded[2].value[1] == 0);

  CHECK(encoded[3].handle == 4);
  CHECK(encoded[3].type == 1);
  CHECK(encoded[3].status == 0x00);
  CHECK(encoded[3].value.empty());
}

TEST_CASE("the encoder matches the shared extensible live synthetic typed-values vector") {
  constexpr SignalEnumChoice kReferenceModeChoices[] = {
      {0x002A, "off"},
      {0x0077, "reverse"},
  };
  const std::array<SignalMetadata, 3> entries{
      boolean_signal(101, "example.enabled"),
      number_signal(102, "example.acceleration"),
      SignalMetadata{SignalId{103}, "example.mode", SignalType::Enum, SignalUnit::None,
                     ValidationStatus::Confirmed, SignalCapability::Read, kReferenceModeChoices,
                     std::size(kReferenceModeChoices)},
  };
  test_support::FakeSignalProvider provider{SignalCatalogView{entries.data(), entries.size()}};
  const SignalCatalogExport catalog{provider.catalog()};
  provider.set_reading(SignalId{101}, SignalReading{SignalValue::boolean(true), Availability::Fresh,
                                                    ValidationStatus::Reference});
  provider.set_reading(
      SignalId{102},
      SignalReading{SignalValue::number(-12.5F), Availability::Fresh, ValidationStatus::Observed});
  provider.set_reading(SignalId{103},
                       SignalReading{SignalValue::enumeration(0x0077), Availability::Fresh,
                                     ValidationStatus::Confirmed});

  const auto packet = encode_extensible_live_signals(catalog, provider, 0x12345678U, 0, true, 247);
  const auto expected = fixture_packet_hex("typed_values");
  REQUIRE(packet.size() == expected.size());
  for (std::size_t index = 0; index < expected.size(); ++index) {
    CHECK(packet[index] == expected[index]);
  }
}

TEST_CASE("extensible encoder preserves every availability and maps read failures") {
  Fixture fixture;
  const std::array<Availability, 5> availabilities{
      Availability::NoData, Availability::Fresh, Availability::Stale,
      Availability::FreshnessUnverified, Availability::Unavailable};
  for (std::size_t index = 0; index < availabilities.size(); ++index) {
    ExtensibleLiveSignalPacketBuilder builder{fixture.catalog, fixture.provider, 1, 0, false, 247};
    fixture.provider.set_reading(SignalId{11}, SignalReading{std::nullopt, availabilities[index],
                                                             ValidationStatus::Reference});
    REQUIRE(builder.append(1));
    const auto encoded = records(builder.packet());
    REQUIRE(encoded.size() == 1);
    CHECK((encoded[0].status & 0x07U) == index);
    CHECK((encoded[0].status & 0x08U) == 0);
  }

  ExtensibleLiveSignalPacketBuilder failed{fixture.catalog, fixture.provider, 1, 0, false, 247};
  REQUIRE(failed.append_failure(2, SignalStatus::Faulted));
  REQUIRE(failed.append_failure(3, SignalStatus::UnsupportedCapability));
  const auto failures = records(failed.packet());
  REQUIRE(failures.size() == 2);
  CHECK((failures[0].status & 0x07U) == 5);
  CHECK((failures[1].status & 0x07U) == 6);
  CHECK((failures[0].status & 0x08U) == 0);
  CHECK((failures[1].status & 0x08U) == 0);
}

TEST_CASE("brake-only Fresh demotion remains in the extensible stream") {
  Fixture fixture;
  const SignalReading fresh_brake{SignalValue::boolean(true), Availability::Fresh,
                                  ValidationStatus::Confirmed};
  const SignalReading fresh_other{SignalValue::boolean(true), Availability::Fresh,
                                  ValidationStatus::Confirmed};
  ExtensibleLiveSignalPacketBuilder builder{fixture.catalog, fixture.provider, 1, 0, false, 247};
  REQUIRE(builder.append_reading(4, fresh_brake));
  REQUIRE(builder.append_reading(1, fresh_other));
  const auto encoded = records(builder.packet());
  REQUIRE(encoded.size() == 2);
  CHECK((encoded[0].status & 0x07U) == 3);
  CHECK((encoded[1].status & 0x07U) == 1);
  CHECK((encoded[0].status & 0x08U) != 0);
  CHECK((encoded[1].status & 0x08U) != 0);
}

TEST_CASE("missing, wrong-type and non-finite values retain status but omit bytes") {
  Fixture fixture;
  const std::array<std::pair<std::uint16_t, SignalReading>, 3> cases{{
      {1, SignalReading{std::nullopt, Availability::Stale, ValidationStatus::Reference}},
      {2,
       SignalReading{SignalValue::boolean(true), Availability::Fresh, ValidationStatus::Reference}},
      {3, SignalReading{SignalValue::enumeration(0xBEEF), Availability::Fresh,
                        ValidationStatus::Reference}},
  }};
  for (const auto &[handle, reading] : cases) {
    ExtensibleLiveSignalPacketBuilder builder{fixture.catalog, fixture.provider, 1, 0, false, 247};
    REQUIRE(builder.append_reading(handle, reading));
    const auto encoded = records(builder.packet());
    REQUIRE(encoded.size() == 1);
    CHECK((encoded[0].status & 0x08U) == 0);
    CHECK(encoded[0].value.empty());
  }

  ExtensibleLiveSignalPacketBuilder non_finite{fixture.catalog, fixture.provider, 1, 0, false, 247};
  REQUIRE(non_finite.append_reading(
      2, SignalReading{SignalValue::number(std::numeric_limits<float>::infinity()),
                       Availability::Unavailable, ValidationStatus::Reference}));
  const auto encoded = records(non_finite.packet());
  REQUIRE(encoded.size() == 1);
  CHECK(encoded[0].status == 0x04);
  CHECK(encoded[0].value.empty());
}

TEST_CASE("records are bounded by the negotiated MTU and are never split") {
  Fixture fixture;
  fixture.provider.set_reading(
      SignalId{11},
      SignalReading{SignalValue::boolean(true), Availability::Fresh, ValidationStatus::Reference});
  fixture.provider.set_reading(
      SignalId{22},
      SignalReading{SignalValue::number(1.0F), Availability::Fresh, ValidationStatus::Reference});
  fixture.provider.set_reading(SignalId{33},
                               SignalReading{SignalValue::enumeration(0xA501), Availability::Fresh,
                                             ValidationStatus::Reference});

  const auto packet =
      encode_extensible_live_signals(fixture.catalog, fixture.provider, 1, 7, true, 64);
  CHECK(packet.size() <= 61);
  const auto encoded = records(packet);
  CHECK(encoded.size() == 4);
  for (const RecordView &record : encoded) {
    const bool valid_length = record.value.size() == 0 || record.value.size() == 1 ||
                              record.value.size() == 2 || record.value.size() == 4;
    CHECK(valid_length);
  }

  ExtensibleLiveSignalPacketBuilder builder{fixture.catalog, fixture.provider, 1, 0, false, 10};
  CHECK_FALSE(builder.valid());
  CHECK(builder.packet().size() == 0);
}

TEST_CASE("a near-full packet rejects the next record atomically") {
  const std::array<SignalMetadata, 7> entries{
      number_signal(101, "capacity.number1"),   number_signal(102, "capacity.number2"),
      number_signal(103, "capacity.number3"),   number_signal(104, "capacity.number4"),
      number_signal(105, "capacity.number5"),   boolean_signal(106, "capacity.boolean1"),
      boolean_signal(107, "capacity.boolean2"),
  };
  test_support::FakeSignalProvider provider{SignalCatalogView{entries.data(), entries.size()}};
  const SignalCatalogExport catalog{provider.catalog()};
  for (std::uint16_t id = 101; id <= 105; ++id) {
    provider.set_reading(SignalId{id}, SignalReading{SignalValue::number(1.0F), Availability::Fresh,
                                                     ValidationStatus::Reference});
  }
  provider.set_reading(SignalId{106}, SignalReading{SignalValue::boolean(true), Availability::Fresh,
                                                    ValidationStatus::Reference});
  provider.set_reading(
      SignalId{107},
      SignalReading{SignalValue::boolean(false), Availability::Fresh, ValidationStatus::Reference});

  // ATT MTU 64 gives a 61-byte packet budget. Five nine-byte Number records
  // and one six-byte Boolean record leave only two bytes, so the next record
  // cannot fit and must not partially change the packet.
  ExtensibleLiveSignalPacketBuilder builder{catalog, provider, 1, 0, false, 64};
  for (std::uint16_t handle = 1; handle <= 6; ++handle) {
    REQUIRE(builder.append(handle));
  }
  REQUIRE(builder.packet().size() == 59);
  CHECK_FALSE(builder.full());
  const auto before = builder.packet();

  CHECK_FALSE(builder.append(7));
  CHECK(builder.packet().size() == before.size());
  for (std::size_t index = 0; index < before.size(); ++index) {
    CHECK(builder.packet()[index] == before[index]);
  }
  CHECK(records(builder.packet()).size() == 6);

  // With the exact 59-byte budget, the same records fill the packet and the
  // rejected record still cannot overrun the bounded storage.
  ExtensibleLiveSignalPacketBuilder exact{catalog, provider, 1, 0, false, 62};
  for (std::uint16_t handle = 1; handle <= 6; ++handle) {
    REQUIRE(exact.append(handle));
  }
  REQUIRE(exact.packet().size() == 59);
  CHECK(exact.full());
  const auto exact_before = exact.packet();
  CHECK_FALSE(exact.append(7));
  CHECK(exact.packet().size() == exact_before.size());
  for (std::size_t index = 0; index < exact_before.size(); ++index) {
    CHECK(exact.packet()[index] == exact_before[index]);
  }
}

TEST_CASE("the maximum packet ceiling keeps only complete records") {
  const auto entries = maximum_packet_number_signals();
  test_support::FakeSignalProvider provider{SignalCatalogView{entries.data(), entries.size()}};
  const SignalCatalogExport catalog{provider.catalog()};
  for (std::uint16_t id = 200; id <= 226; ++id) {
    provider.set_reading(SignalId{id},
                         SignalReading{SignalValue::number(-1.0F), Availability::Fresh,
                                       ValidationStatus::Reference});
  }

  // The eight-byte header plus 26 nine-byte Number records is 242 bytes;
  // record 27 would exceed the 244-byte protocol ceiling by seven bytes.
  ExtensibleLiveSignalPacketBuilder builder{catalog, provider, 1, 0, false, 247};
  for (std::uint16_t handle = 1; handle <= 26; ++handle) {
    REQUIRE(builder.append(handle));
  }
  REQUIRE(builder.packet().size() == 242);
  CHECK(builder.packet().size() <= kExtensibleLiveSignalMaxPacketBytes);
  CHECK_FALSE(builder.full());
  const auto before = builder.packet();

  CHECK_FALSE(builder.append(27));
  CHECK(builder.packet().size() == before.size());
  CHECK(builder.packet().size() <= kExtensibleLiveSignalMaxPacketBytes);
  for (std::size_t index = 0; index < before.size(); ++index) {
    CHECK(builder.packet()[index] == before[index]);
  }
  CHECK(records(builder.packet()).size() == 26);
}

TEST_CASE("a builder never emits a duplicate wire handle") {
  Fixture fixture;
  ExtensibleLiveSignalPacketBuilder builder{fixture.catalog, fixture.provider, 1, 0, false, 247};
  const SignalReading reading{SignalValue::boolean(true), Availability::Fresh,
                              ValidationStatus::Reference};
  REQUIRE(builder.append_reading(1, reading));
  CHECK_FALSE(builder.append_reading(1, reading));
  CHECK(records(builder.packet()).size() == 1);
}

TEST_CASE("empty catalogs and invalid tokens fail conservatively while v1 bytes stay available") {
  const SignalCatalogExport empty{SignalCatalogView{}};
  test_support::FakeSignalProvider provider{SignalCatalogView{}};
  const ExtensibleLiveSignalPacketBuilder zero_token{empty, provider, 0, 0, false, 247};
  CHECK_FALSE(zero_token.valid());
  CHECK(zero_token.packet().size() == 0);

  const ExtensibleLiveSignalPacketBuilder valid_empty{empty, provider, 1, 0, false, 247};
  REQUIRE(valid_empty.valid());
  CHECK(valid_empty.packet().size() == kExtensibleLiveSignalHeaderBytes);
  CHECK(valid_empty.packet()[0] == kExtensibleLiveSignalFormat);
  CHECK(valid_empty.packet()[3] == 0);

  const LiveSignalSampler legacy{provider};
  const LiveFrame frame = legacy.sample(false).frame(0);
  std::array<std::uint8_t, kLiveFrameBytes> expected{};
  expected[0] = 2;
  for (std::size_t index = 13; index <= 22; ++index) {
    expected[index] = 0x66;
  }
  expected[23] = 0x06;
  CHECK(frame == expected);
}
