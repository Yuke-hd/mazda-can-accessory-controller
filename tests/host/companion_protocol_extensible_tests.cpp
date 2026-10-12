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
#include <limits>
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
