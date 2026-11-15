#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "companion_protocol/display_snapshot.hpp"
#include "support/fake_signal_provider.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
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
using vehicle_signals::SignalValue;
using vehicle_signals::ValidationStatus;

// Raw catalog values deliberately differ from the protocol codes.
constexpr SignalEnumChoice kTurnChoices[] = {
    {40, "unknown"}, {41, "off"}, {42, "left"}, {43, "right"}, {44, "hazard"}};
constexpr SignalEnumChoice kSelectorChoices[] = {{7, "drive"}, {6, "neutral"},  {5, "reverse"},
                                                 {4, "park"},  {3, "shifting"}, {9, "unknown"},
                                                 {8, "manual"}};
constexpr SignalEnumChoice kGearChoices[] = {
    {100, "unknown"}, {101, "park_or_neutral"}, {102, "park"},   {103, "neutral"},
    {104, "reverse"}, {105, "first"},           {106, "second"}, {107, "third"},
    {108, "fourth"},  {109, "fifth"},           {110, "sixth"},  {111, "shifting"}};
constexpr SignalEnumChoice kWiperChoices[] = {
    {0, "unknown"}, {3, "off"}, {2, "on"}, {1, "high"}, {4, "intermittent"}};

// Zero-based slot indexes; wire ID is slot + 1.
constexpr std::size_t kRpm = 0;
constexpr std::size_t kSpeed = 1;
constexpr std::size_t kSelector = 8;
constexpr std::size_t kGear = 9;
constexpr std::size_t kBrake = 18;
constexpr std::size_t kLongitudinal = 19;
constexpr std::size_t kPedal = 21;
constexpr std::size_t kPressure = 22;

constexpr SignalId id_of(std::size_t slot) {
  return SignalId{static_cast<std::uint16_t>(slot + 1)};
}

std::vector<SignalMetadata> full_catalog() {
  std::vector<SignalMetadata> entries;
  const auto &slots = display_signal_slots();
  for (std::size_t slot = 0; slot < slots.size(); ++slot) {
    SignalMetadata entry{};
    entry.id = id_of(slot);
    entry.key = slots[slot].key;
    entry.type = slots[slot].type;
    entry.validation = ValidationStatus::Reference;
    entry.capabilities = SignalCapability::Read | SignalCapability::Notify;
    if (entry.type == SignalType::Enum) {
      const auto key = slots[slot].key;
      auto table = std::pair{kWiperChoices, std::size(kWiperChoices)};
      if (key == "vehicle.turn_state") {
        table = {kTurnChoices, std::size(kTurnChoices)};
      } else if (key == "vehicle.selector_position") {
        table = {kSelectorChoices, std::size(kSelectorChoices)};
      } else if (key == "vehicle.actual_gear") {
        table = {kGearChoices, std::size(kGearChoices)};
      }
      entry.choices = table.first;
      entry.choice_count = table.second;
    }
    entries.push_back(entry);
  }
  return entries;
}

struct Fixture {
  explicit Fixture(std::vector<SignalMetadata> catalog_entries = full_catalog())
      : entries(std::move(catalog_entries)),
        provider(SignalCatalogView{entries.data(), entries.size()}) {}

  void set(std::size_t slot, std::optional<SignalValue> value, Availability availability) {
    provider.set_reading(id_of(slot),
                         SignalReading{value, availability, ValidationStatus::Reference});
  }
  DisplaySnapshot sample(bool started = true, std::uint32_t session = 1,
                         std::uint32_t sequence = 0) const {
    const DisplaySnapshotSampler sampler{provider};
    return sampler.sample(started, session, sequence);
  }

  std::vector<SignalMetadata> entries;
  test_support::FakeSignalProvider provider;
};

std::uint8_t availability(const DisplaySnapshot &bytes, std::size_t slot) {
  return bytes[16 + 8 * slot + 2];
}
std::array<std::uint8_t, 4> value(const DisplaySnapshot &bytes, std::size_t slot) {
  const std::size_t at = 16 + 8 * slot + 4;
  return {bytes[at], bytes[at + 1], bytes[at + 2], bytes[at + 3]};
}
using Bytes4 = std::array<std::uint8_t, 4>;

// Transcribed by hand from the display contract (esp-now-v1.md, Vector A).
constexpr std::array<std::uint8_t, 200> kVectorA{
    0x45, 0x43, 0x41, 0x4e, 0x01, 0x01, 0x17, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x00, 0x00, 0x00, 0x7a, 0x44, //
    0x02, 0x02, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00,                         //
    0x03, 0x03, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00,                         //
    0x04, 0x01, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00,                         //
    0x05, 0x01, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00,                         //
    0x06, 0x01, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00,                         //
    0x07, 0x01, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00,                         //
    0x08, 0x01, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00,                         //
    0x09, 0x03, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00,                         //
    0x0a, 0x03, 0x03, 0x00, 0x05, 0x00, 0x00, 0x00,                         //
    0x0b, 0x01, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00,                         //
    0x0c, 0x01, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00,                         //
    0x0d, 0x01, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00,                         //
    0x0e, 0x01, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00,                         //
    0x0f, 0x01, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00,                         //
    0x10, 0x01, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00,                         //
    0x11, 0x01, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00,                         //
    0x12, 0x03, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00,                         //
    0x13, 0x01, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00,                         //
    0x14, 0x02, 0x01, 0x00, 0x00, 0x00, 0x80, 0xbf,                         //
    0x15, 0x02, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00,                         //
    0x16, 0x02, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00,                         //
    0x17, 0x02, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00,
};

// Transcribed by hand from the display contract (esp-now-v1.md, Vector B).
constexpr std::array<std::uint8_t, 200> kVectorB{
    0x45, 0x43, 0x41, 0x4e, 0x01, 0x00, 0x17, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, //
    0x02, 0x02, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00,                         //
    0x03, 0x03, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00,                         //
    0x04, 0x01, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00,                         //
    0x05, 0x01, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00,                         //
    0x06, 0x01, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00,                         //
    0x07, 0x01, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00,                         //
    0x08, 0x01, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00,                         //
    0x09, 0x03, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00,                         //
    0x0a, 0x03, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00,                         //
    0x0b, 0x01, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00,                         //
    0x0c, 0x01, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00,                         //
    0x0d, 0x01, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00,                         //
    0x0e, 0x01, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00,                         //
    0x0f, 0x01, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00,                         //
    0x10, 0x01, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00,                         //
    0x11, 0x01, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00,                         //
    0x12, 0x03, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00,                         //
    0x13, 0x01, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00,                         //
    0x14, 0x02, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00,                         //
    0x15, 0x02, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00,                         //
    0x16, 0x02, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00,                         //
    0x17, 0x02, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00,
};

// Vector A's readings. Usable readings with no value encode as Unknown, which
// is how every "all other values Unknown" record is produced here.
std::vector<SignalMetadata> catalog_without_speed() {
  std::vector<SignalMetadata> entries = full_catalog();
  entries.erase(entries.begin() + kSpeed); // speed unsupported by this sender
  return entries;
}

void set_vector_a(Fixture &fixture) {
  for (std::size_t slot = 0; slot < kDisplaySignalCount; ++slot) {
    if (slot != kSpeed) {
      fixture.set(slot, std::nullopt, Availability::Fresh);
    }
  }
  fixture.set(kRpm, SignalValue::number(1000.0F), Availability::FreshnessUnverified);
  fixture.set(kGear, SignalValue::enumeration(105), Availability::FreshnessUnverified);
  fixture.set(kBrake, SignalValue::boolean(true), Availability::FreshnessUnverified);
  fixture.set(kLongitudinal, SignalValue::number(-1.0F), Availability::Fresh);
}

} // namespace

TEST_CASE("Vector A encodes byte for byte") {
  Fixture fixture{catalog_without_speed()};
  set_vector_a(fixture);
  CHECK(fixture.sample(true, 1, 0) == kVectorA);
}

TEST_CASE("Vector B is the canonical stopped packet whatever the readings") {
  Fixture fixture{catalog_without_speed()};
  set_vector_a(fixture);
  CHECK(fixture.sample(false, 1, 0) == kVectorB);
}

TEST_CASE("session and sequence are little-endian u32") {
  const Fixture fixture;
  const auto bytes = fixture.sample(true, 0x11223344U, 0xfffffffeU);
  CHECK(bytes[8] == 0x44);
  CHECK(bytes[11] == 0x11);
  CHECK(bytes[12] == 0xfe);
  CHECK(bytes[15] == 0xff);
}

TEST_CASE("accelerator fraction bounds") {
  Fixture fixture;
  fixture.set(kPedal, SignalValue::number(0.0F), Availability::FreshnessUnverified);
  CHECK(availability(fixture.sample(), kPedal) == 3);
  CHECK(value(fixture.sample(), kPedal) == Bytes4{0x00, 0x00, 0x00, 0x00});
  fixture.set(kPedal, SignalValue::number(1.0F), Availability::FreshnessUnverified);
  CHECK(value(fixture.sample(), kPedal) == Bytes4{0x00, 0x00, 0x80, 0x3f});
  fixture.set(kPedal, SignalValue::number(-1.0F), Availability::FreshnessUnverified);
  CHECK(availability(fixture.sample(), kPedal) == 7);
  CHECK(value(fixture.sample(), kPedal) == Bytes4{});
  fixture.set(kPedal, SignalValue::number(1.01F), Availability::FreshnessUnverified);
  CHECK(availability(fixture.sample(), kPedal) == 7);
}

TEST_CASE("non-finite and negative non-negative numbers are Unknown") {
  Fixture fixture;
  fixture.set(kRpm, SignalValue::number(std::numeric_limits<float>::quiet_NaN()),
              Availability::Fresh);
  CHECK(availability(fixture.sample(), kRpm) == 7);
  fixture.set(kRpm, SignalValue::number(std::numeric_limits<float>::infinity()),
              Availability::Fresh);
  CHECK(availability(fixture.sample(), kRpm) == 7);
  fixture.set(kPressure, SignalValue::number(-0.5F), Availability::FreshnessUnverified);
  CHECK(availability(fixture.sample(), kPressure) == 7);
  fixture.set(kLongitudinal, SignalValue::number(-4.0F), Availability::Fresh);
  CHECK(availability(fixture.sample(), kLongitudinal) == 1);
}

TEST_CASE("brake pedal and pressure are never sent as Fresh") {
  Fixture fixture;
  fixture.set(kBrake, SignalValue::boolean(true), Availability::Fresh);
  fixture.set(kPressure, SignalValue::number(12.5F), Availability::Fresh);
  fixture.set(kRpm, SignalValue::number(800.0F), Availability::Fresh);
  const auto bytes = fixture.sample();
  CHECK(availability(bytes, kBrake) == 3);
  CHECK(availability(bytes, kPressure) == 3);
  CHECK(availability(bytes, kRpm) == 1);
}

TEST_CASE("unusable availability is copied with a zero value") {
  Fixture fixture;
  fixture.set(kRpm, SignalValue::number(800.0F), Availability::Stale);
  fixture.set(kSpeed, SignalValue::number(50.0F), Availability::Unavailable);
  fixture.set(kBrake, SignalValue::boolean(true), Availability::NoData);
  const auto bytes = fixture.sample();
  CHECK(availability(bytes, kRpm) == 2);
  CHECK(value(bytes, kRpm) == Bytes4{});
  CHECK(availability(bytes, kSpeed) == 4);
  CHECK(value(bytes, kSpeed) == Bytes4{});
  CHECK(availability(bytes, kBrake) == 0);
  CHECK(value(bytes, kBrake) == Bytes4{});
}

TEST_CASE("enum choices are remapped by key and unmapped choices are Unknown") {
  Fixture fixture;
  fixture.set(kSelector, SignalValue::enumeration(4), Availability::Fresh); // park
  CHECK(value(fixture.sample(), kSelector) == Bytes4{0x02, 0x00, 0x00, 0x00});
  fixture.set(kSelector, SignalValue::enumeration(8), Availability::Fresh); // manual: no code
  CHECK(availability(fixture.sample(), kSelector) == 7);
  fixture.set(kSelector, SignalValue::enumeration(250), Availability::Fresh); // not in catalog
  CHECK(availability(fixture.sample(), kSelector) == 7);
}

TEST_CASE("read failures, missing keys and type mismatches") {
  std::vector<SignalMetadata> entries = full_catalog();
  entries[kSpeed].type = SignalType::Boolean;
  Fixture fixture{std::move(entries)};
  fixture.provider.fail_reads(id_of(kRpm), SignalStatus::Faulted);
  const auto bytes = fixture.sample();
  CHECK(availability(bytes, kRpm) == 5);
  CHECK(availability(bytes, kSpeed) == 6);
  // Every record keeps its ID and type even when unsupported.
  CHECK(bytes[16 + 8 * kSpeed] == kSpeed + 1);
  CHECK(bytes[16 + 8 * kSpeed + 1] == 2);
}
