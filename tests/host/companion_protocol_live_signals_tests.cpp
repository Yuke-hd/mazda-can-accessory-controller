#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "companion_protocol/live_signal_layout.hpp"
#include "companion_protocol/live_signals.hpp"
#include "support/fake_signal_provider.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
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

// Raw catalog values deliberately differ from the protocol codes, so a test
// fails if the encoder sends a raw value or a declaration index.
constexpr SignalEnumChoice kTurnChoices[] = {
    {40, "unknown"}, {41, "off"}, {42, "left"}, {43, "right"}, {44, "hazard"}};
constexpr SignalEnumChoice kSelectorChoices[] = {{7, "drive"}, {6, "neutral"},  {5, "reverse"},
                                                 {4, "park"},  {3, "shifting"}, {9, "unknown"}};
constexpr SignalEnumChoice kGearChoices[] = {
    {100, "unknown"}, {101, "park_or_neutral"}, {102, "park"},   {103, "neutral"},
    {104, "reverse"}, {105, "first"},           {106, "second"}, {107, "third"},
    {108, "fourth"},  {109, "fifth"},           {110, "sixth"},  {111, "shifting"}};
constexpr SignalEnumChoice kWiperChoices[] = {
    {0, "unknown"}, {3, "off"}, {2, "on"}, {1, "high"}, {4, "intermittent"}};

constexpr std::array<std::string_view, 21> kKeys{
    "vehicle.engine_rpm",           "vehicle.speed_kph",
    "vehicle.turn_state",           "vehicle.selector_position",
    "vehicle.actual_gear",          "vehicle.wiper.front_position",
    "vehicle.hazard_request",       "vehicle.turn_request.left",
    "vehicle.turn_request.right",   "vehicle.indicator_lamp.left",
    "vehicle.indicator_lamp.right", "vehicle.liftgate_open",
    "vehicle.door.rear_right",      "vehicle.door.rear_left",
    "vehicle.door.front_left_rhd",  "vehicle.door.front_right_rhd",
    "vehicle.doors_unlocked",       "vehicle.wiper.low",
    "vehicle.brake_pressed",        "vehicle.acceleration.longitudinal",
    "vehicle.acceleration.lateral",
};

constexpr SignalId id_of(std::size_t slot) {
  return SignalId{static_cast<std::uint16_t>(slot + 1)};
}

SignalMetadata entry_for(std::size_t slot) {
  SignalMetadata entry{};
  entry.id = id_of(slot);
  entry.key = kKeys[slot];
  entry.validation = ValidationStatus::Reference;
  entry.capabilities = SignalCapability::Read | SignalCapability::Notify;
  if (slot < 2 || slot >= 19) {
    entry.type = SignalType::Number;
    entry.unit = slot == 0   ? SignalUnit::RevolutionsPerMinute
                 : slot == 1 ? SignalUnit::KilometresPerHour
                             : SignalUnit::MetresPerSecondSquared;
    return entry;
  }
  if (slot < 6) {
    const std::array<std::pair<const SignalEnumChoice *, std::size_t>, 4> tables{{
        {kTurnChoices, std::size(kTurnChoices)},
        {kSelectorChoices, std::size(kSelectorChoices)},
        {kGearChoices, std::size(kGearChoices)},
        {kWiperChoices, std::size(kWiperChoices)},
    }};
    entry.type = SignalType::Enum;
    entry.choices = tables[slot - 2].first;
    entry.choice_count = tables[slot - 2].second;
    return entry;
  }
  entry.type = SignalType::Boolean;
  return entry;
}

std::vector<SignalMetadata> full_catalog() {
  std::vector<SignalMetadata> entries;
  for (std::size_t slot = 0; slot < kKeys.size(); ++slot) {
    entries.push_back(entry_for(slot));
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
  LiveFrame frame(bool started = true, std::uint8_t sequence = 0) const {
    const LiveSignalSampler sampler{provider};
    return sampler.sample(started).frame(sequence);
  }

  std::vector<SignalMetadata> entries;
  test_support::FakeSignalProvider provider;
};

std::uint8_t nibble(const LiveFrame &frame, std::size_t slot) {
  const std::uint8_t byte = frame[13 + slot / 2];
  return static_cast<std::uint8_t>(slot % 2 == 0 ? byte & 0x0FU : byte >> 4U);
}
std::uint8_t code(const LiveFrame &frame, std::size_t slot) {
  return static_cast<std::uint8_t>(nibble(frame, slot) & 0x07U);
}
bool present(const LiveFrame &frame, std::size_t slot) {
  return (nibble(frame, slot) & 0x08U) != 0;
}
std::uint16_t u16(const LiveFrame &frame, std::size_t offset) {
  return static_cast<std::uint16_t>(frame[offset] | (frame[offset + 1] << 8U));
}

constexpr std::size_t kRpm = 0;
constexpr std::size_t kSpeed = 1;
constexpr std::size_t kTurn = 2;
constexpr std::size_t kSelector = 3;
constexpr std::size_t kGear = 4;
constexpr std::size_t kWiper = 5;
constexpr std::size_t kHazard = 6;
constexpr std::size_t kBrake = 18;

} // namespace

TEST_CASE("each provider Availability maps to its code, and only Fresh is 1") {
  CHECK(status_code(Availability::NoData) == SignalStatusCode::NoData);
  CHECK(status_code(Availability::Fresh) == SignalStatusCode::Fresh);
  CHECK(status_code(Availability::Stale) == SignalStatusCode::Stale);
  CHECK(status_code(Availability::FreshnessUnverified) == SignalStatusCode::FreshnessUnverified);
  CHECK(status_code(Availability::Unavailable) == SignalStatusCode::Unavailable);
  for (int raw = 0; raw <= 0xFF; ++raw) {
    const auto availability = static_cast<Availability>(raw);
    if (availability != Availability::Fresh) {
      CHECK(status_code(availability) != SignalStatusCode::Fresh);
    }
  }
}

TEST_CASE("read failures map to Not supported or Read failed") {
  CHECK(status_code(SignalStatus::InvalidSignal) == SignalStatusCode::NotSupported);
  CHECK(status_code(SignalStatus::UnsupportedCapability) == SignalStatusCode::NotSupported);
  for (const auto failure :
       {SignalStatus::Ok, SignalStatus::InvalidArgument, SignalStatus::CapacityExceeded,
        SignalStatus::InvalidState, SignalStatus::InvalidSubscription, SignalStatus::Faulted,
        SignalStatus::Timeout}) {
    CHECK(status_code(failure) == SignalStatusCode::ReadFailed);
  }
}

TEST_CASE("a frame has the layout version, sequence and telemetry-started flag") {
  Fixture fixture;
  const auto started = fixture.frame(true, 7);
  CHECK(started.size() == 28);
  CHECK(started[0] == 2);
  CHECK(started[1] == 7);
  CHECK(started[2] == 0x01);
  CHECK(fixture.frame(false, 255)[2] == 0x00);
  CHECK(fixture.frame(false, 255)[1] == 255);
}

TEST_CASE("a provider with no observations reports NoData and no values") {
  Fixture fixture;
  const auto frame = fixture.frame();
  for (std::size_t offset = 3; offset < 13; ++offset) {
    CHECK(frame[offset] == 0);
  }
  for (std::size_t offset = 13; offset < frame.size(); ++offset) {
    CHECK(frame[offset] == 0);
  }
}

TEST_CASE("the sequence is not part of the content comparison") {
  Fixture fixture;
  const LiveSignalSampler sampler{fixture.provider};
  const auto first = sampler.sample(true);
  CHECK(first == sampler.sample(true));
  CHECK(first.frame(0) != first.frame(1));
  CHECK(first != sampler.sample(false));
  fixture.set(kHazard, SignalValue::boolean(true), Availability::Fresh);
  CHECK(first != sampler.sample(true));
}

TEST_CASE("numbers encode in their units with rounding") {
  Fixture fixture;
  fixture.set(kRpm, SignalValue::number(598.75F), Availability::FreshnessUnverified);
  fixture.set(kSpeed, SignalValue::number(42.125F), Availability::Stale);
  const auto frame = fixture.frame();
  CHECK(u16(frame, 3) == 2395);
  CHECK(u16(frame, 5) == 4213);
  CHECK(code(frame, kRpm) == 3);
  CHECK(present(frame, kRpm));
  CHECK(code(frame, kSpeed) == 2);
  CHECK(present(frame, kSpeed));
}

TEST_CASE("a zero number is a present value") {
  Fixture fixture;
  fixture.set(kSpeed, SignalValue::number(0.0F), Availability::Fresh);
  const auto frame = fixture.frame();
  CHECK(u16(frame, 5) == 0);
  CHECK(present(frame, kSpeed));
  CHECK(code(frame, kSpeed) == 1);
}

TEST_CASE("unencodable numbers clear the value but keep the availability") {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float infinity = std::numeric_limits<float>::infinity();
  for (const float value : {nan, infinity, -infinity, -0.5F, -1.0F, 16383.875F + 0.25F}) {
    Fixture fixture;
    fixture.set(kRpm, SignalValue::number(value), Availability::Fresh);
    const auto frame = fixture.frame();
    CHECK(u16(frame, 3) == 0);
    CHECK_FALSE(present(frame, kRpm));
    CHECK(code(frame, kRpm) == 1);
  }
}

TEST_CASE("the 16-bit ceiling is inclusive after rounding") {
  Fixture fixture;
  fixture.set(kSpeed, SignalValue::number(655.35F), Availability::Fresh);
  CHECK(u16(fixture.frame(), 5) == 65535);
  CHECK(present(fixture.frame(), kSpeed));
  fixture.set(kSpeed, SignalValue::number(655.36F), Availability::Fresh);
  CHECK_FALSE(present(fixture.frame(), kSpeed));
  fixture.set(kRpm, SignalValue::number(16383.75F), Availability::Fresh);
  CHECK(u16(fixture.frame(), 3) == 65535);
  fixture.set(kRpm, SignalValue::number(16383.9F), Availability::Fresh);
  CHECK_FALSE(present(fixture.frame(), kRpm));
}

TEST_CASE("a value of the wrong type is not sent") {
  Fixture fixture;
  fixture.set(kRpm, SignalValue::boolean(true), Availability::Fresh);
  fixture.set(kTurn, SignalValue::number(2.0F), Availability::Stale);
  fixture.set(kHazard, SignalValue::enumeration(1), Availability::Unavailable);
  const auto frame = fixture.frame();
  CHECK(u16(frame, 3) == 0);
  CHECK_FALSE(present(frame, kRpm));
  CHECK(code(frame, kRpm) == 1);
  CHECK(frame[7] == 0);
  CHECK_FALSE(present(frame, kTurn));
  CHECK(code(frame, kTurn) == 2);
  CHECK(u16(frame, 11) == 0);
  CHECK_FALSE(present(frame, kHazard));
  CHECK(code(frame, kHazard) == 4);
}

TEST_CASE("enum values send the protocol code of their catalog choice key") {
  Fixture fixture;
  fixture.set(kTurn, SignalValue::enumeration(44), Availability::Fresh);
  fixture.set(kSelector, SignalValue::enumeration(7), Availability::FreshnessUnverified);
  fixture.set(kGear, SignalValue::enumeration(111), Availability::FreshnessUnverified);
  fixture.set(kWiper, SignalValue::enumeration(1), Availability::FreshnessUnverified);
  const auto frame = fixture.frame();
  CHECK(frame[7] == 4);
  CHECK(frame[8] == 5);
  CHECK(frame[9] == 11);
  CHECK(frame[10] == 3);
  for (const std::size_t slot : {kTurn, kSelector, kGear, kWiper}) {
    CHECK(present(frame, slot));
  }
}

TEST_CASE("the reported unknown choice is a present value with code 0") {
  Fixture fixture;
  fixture.set(kSelector, SignalValue::enumeration(9), Availability::FreshnessUnverified);
  const auto frame = fixture.frame();
  CHECK(frame[8] == 0);
  CHECK(present(frame, kSelector));
}

TEST_CASE("a raw enum value with no catalog choice is not sent") {
  Fixture fixture;
  fixture.set(kGear, SignalValue::enumeration(250), Availability::Stale);
  const auto frame = fixture.frame();
  CHECK(frame[9] == 0);
  CHECK_FALSE(present(frame, kGear));
  CHECK(code(frame, kGear) == 2);
}

TEST_CASE("a catalog choice key without a protocol code is not sent") {
  static constexpr SignalEnumChoice kExtraTurnChoices[] = {{1, "off"}, {2, "sideways"}};
  auto catalog = full_catalog();
  catalog[kTurn].choices = kExtraTurnChoices;
  catalog[kTurn].choice_count = 2;
  Fixture fixture{catalog};
  fixture.set(kTurn, SignalValue::enumeration(2), Availability::Fresh);
  CHECK_FALSE(present(fixture.frame(), kTurn));
  CHECK(code(fixture.frame(), kTurn) == 1);
  fixture.set(kTurn, SignalValue::enumeration(1), Availability::Fresh);
  CHECK(fixture.frame()[7] == 1);
}

TEST_CASE("each Boolean signal uses its own bit") {
  for (std::size_t slot = kHazard; slot <= kBrake; ++slot) {
    Fixture fixture;
    fixture.set(slot, SignalValue::boolean(true), Availability::FreshnessUnverified);
    const auto frame = fixture.frame();
    CHECK(u16(frame, 11) == (1U << (slot - kHazard)));
    CHECK(present(frame, slot));
  }
}

TEST_CASE("a false Boolean is a present value with its bit clear") {
  Fixture fixture;
  fixture.set(kHazard, SignalValue::boolean(false), Availability::Fresh);
  const auto frame = fixture.frame();
  CHECK(u16(frame, 11) == 0);
  CHECK(present(frame, kHazard));
  CHECK(code(frame, kHazard) == 1);
}

TEST_CASE("a Stale or Unavailable reading keeps its retained value") {
  Fixture fixture;
  fixture.set(kRpm, SignalValue::number(800.0F), Availability::Unavailable);
  fixture.set(kHazard, SignalValue::boolean(true), Availability::Stale);
  const auto frame = fixture.frame();
  CHECK(u16(frame, 3) == 3200);
  CHECK(present(frame, kRpm));
  CHECK(code(frame, kRpm) == 4);
  CHECK(present(frame, kHazard));
  CHECK(code(frame, kHazard) == 2);
}

TEST_CASE("status nibbles pack low for even and high for odd indexes") {
  Fixture fixture;
  fixture.set(kRpm, SignalValue::number(1.0F), Availability::Fresh);    // 0x9
  fixture.set(kSpeed, std::nullopt, Availability::Unavailable);         // 0x4
  fixture.set(kBrake, SignalValue::boolean(true), Availability::Stale); // 0xA
  const auto frame = fixture.frame();
  CHECK(frame[13] == 0x49);
  CHECK(frame[22] == 0x0A);
}

TEST_CASE("brake is never encoded as Fresh") {
  Fixture fixture;
  fixture.set(kBrake, SignalValue::boolean(true), Availability::Fresh);
  const auto frame = fixture.frame();
  CHECK(code(frame, kBrake) == 3);
  CHECK(present(frame, kBrake));
  CHECK(u16(frame, 11) == (1U << 12U));
}

TEST_CASE("read failures send no value") {
  Fixture fixture;
  fixture.set(kRpm, SignalValue::number(1000.0F), Availability::Fresh);
  fixture.provider.fail_reads(id_of(kRpm), SignalStatus::Faulted);
  fixture.set(kHazard, SignalValue::boolean(true), Availability::Fresh);
  fixture.provider.fail_reads(id_of(kHazard), SignalStatus::Timeout);
  const auto frame = fixture.frame();
  CHECK(code(frame, kRpm) == 5);
  CHECK_FALSE(present(frame, kRpm));
  CHECK(u16(frame, 3) == 0);
  CHECK(code(frame, kHazard) == 5);
  CHECK(u16(frame, 11) == 0);
}

TEST_CASE("a key missing from the catalog is Not supported") {
  auto catalog = full_catalog();
  catalog.erase(catalog.begin() + kBrake);
  Fixture fixture{catalog};
  CHECK(code(fixture.frame(), kBrake) == 6);
  CHECK_FALSE(present(fixture.frame(), kBrake));
}

TEST_CASE("a catalog type that differs from the signal table is Not supported") {
  auto catalog = full_catalog();
  catalog[kSpeed].type = SignalType::Boolean;
  catalog[kSpeed].unit = SignalUnit::None;
  Fixture fixture{catalog};
  fixture.set(kSpeed, SignalValue::boolean(true), Availability::Fresh);
  CHECK(code(fixture.frame(), kSpeed) == 6);
  CHECK_FALSE(present(fixture.frame(), kSpeed));
}

TEST_CASE("a signal without the Read capability is Not supported") {
  auto catalog = full_catalog();
  catalog[kTurn].capabilities = SignalCapability::Notify;
  Fixture fixture{catalog};
  CHECK(code(fixture.frame(), kTurn) == 6);
}

// A provider whose read() rejects ids its own catalog lists, to cover the
// read-outcome rows the catalog resolution cannot catch.
class RejectingProvider final : public vehicle_signals::SignalProvider {
public:
  explicit RejectingProvider(SignalCatalogView catalog, SignalStatus failure) noexcept
      : catalog_(catalog), failure_(failure) {}
  [[nodiscard]] SignalCatalogView catalog() const noexcept override { return catalog_; }
  [[nodiscard]] vehicle_signals::SignalResult<SignalReading>
  read(SignalId) const noexcept override {
    return vehicle_signals::SignalResult<SignalReading>::failure(failure_);
  }
  [[nodiscard]] vehicle_signals::SignalResult<vehicle_signals::SignalSubscription>
  subscribe(SignalId, vehicle_signals::SignalCallback, void *) noexcept override {
    return vehicle_signals::SignalResult<vehicle_signals::SignalSubscription>::failure(failure_);
  }
  [[nodiscard]] vehicle_signals::SignalStatusResult
  unsubscribe(vehicle_signals::SignalSubscription) noexcept override {
    return vehicle_signals::SignalStatusResult::failure(failure_);
  }

private:
  SignalCatalogView catalog_;
  SignalStatus failure_;
};

TEST_CASE("read() failing with InvalidSignal or UnsupportedCapability is Not supported") {
  const auto catalog = full_catalog();
  for (const auto failure : {SignalStatus::InvalidSignal, SignalStatus::UnsupportedCapability}) {
    const RejectingProvider provider{SignalCatalogView{catalog.data(), catalog.size()}, failure};
    const LiveSignalSampler sampler{provider};
    const auto frame = sampler.sample(true).frame(0);
    for (std::size_t slot = 0; slot < kLiveSignalCount; ++slot) {
      CHECK(nibble(frame, slot) == 6);
    }
  }
}

TEST_CASE("the signal table matches the specification") {
  const auto &slots = live_signal_slots();
  for (std::size_t slot = 0; slot < kLiveSignalCount; ++slot) {
    CHECK(slots[slot].key == kKeys[slot]);
    CHECK(slots[slot].freshness_unset == (slot == kBrake));
  }
  CHECK(slots[kRpm].type == SignalType::Number);
  CHECK(slots[kRpm].position == 3);
  CHECK(slots[kRpm].scale == 4.0F);
  CHECK(slots[kSpeed].position == 5);
  CHECK(slots[kSpeed].scale == 100.0F);
  for (std::size_t slot = kTurn; slot <= kWiper; ++slot) {
    CHECK(slots[slot].type == SignalType::Enum);
    CHECK(slots[slot].position == 7 + (slot - kTurn));
  }
  for (std::size_t slot = kHazard; slot <= kBrake; ++slot) {
    CHECK(slots[slot].type == SignalType::Boolean);
    CHECK(slots[slot].position == slot - kHazard);
  }
}

TEST_CASE("the choice-code tables match the specification") {
  const auto &slots = live_signal_slots();
  const std::array<std::vector<std::string_view>, 4> expected{{
      {"unknown", "off", "left", "right", "hazard"},
      {"unknown", "shifting", "park", "reverse", "neutral", "drive"},
      {"unknown", "park_or_neutral", "park", "neutral", "reverse", "first", "second", "third",
       "fourth", "fifth", "sixth", "shifting"},
      {"unknown", "off", "on", "high", "intermittent"},
  }};
  for (std::size_t table = 0; table < expected.size(); ++table) {
    const LiveSignalSlot &slot = slots[kTurn + table];
    CHECK(slot.choice_count == expected[table].size());
    for (std::size_t choice = 0; choice < expected[table].size(); ++choice) {
      CHECK(slot.choice_code(expected[table][choice]) ==
            std::optional<std::uint8_t>{static_cast<std::uint8_t>(choice)});
    }
    CHECK_FALSE(slot.choice_code("not_a_choice").has_value());
  }
}

TEST_CASE("layout 2 adds two signed SI acceleration fields without moving existing fields") {
  Fixture fixture;
  const auto frame = fixture.frame();
  REQUIRE(frame.size() == 28);
  CHECK(frame[0] == 2);
  REQUIRE(live_signal_slots().size() == 21);
  CHECK(live_signal_slots()[19].key == "vehicle.acceleration.longitudinal");
  CHECK(live_signal_slots()[19].position == 24);
  CHECK(live_signal_slots()[19].scale == 100.0F);
  CHECK(live_signal_slots()[19].signed_number);
  CHECK(live_signal_slots()[20].key == "vehicle.acceleration.lateral");
  CHECK(live_signal_slots()[20].position == 26);
  CHECK(live_signal_slots()[20].scale == 1000.0F);
  CHECK(live_signal_slots()[20].signed_number);
  CHECK(frame[23] == 0); // index 21 is reserved
}

TEST_CASE("negative zero and positive acceleration round trip in canonical SI units") {
  for (std::size_t slot : {19U, 20U}) {
    const double scale = slot == 19 ? 100.0 : 1000.0;
    const std::size_t offset = slot == 19 ? 24 : 26;
    for (float value : {-40.0F, -4.096F, -1.234F, 0.0F, 1.234F, 4.095F, 41.91F}) {
      if (slot == 20 && std::abs(value) > 32.0F)
        continue;
      Fixture fixture;
      fixture.set(slot, SignalValue::number(value), Availability::FreshnessUnverified);
      const auto frame = fixture.frame();
      REQUIRE(frame.size() == 28);
      const std::uint16_t raw = u16(frame, offset);
      const std::int32_t signed_raw = raw < 0x8000U ? raw : static_cast<std::int32_t>(raw) - 65536;
      CHECK(std::abs(static_cast<double>(signed_raw) / scale - value) <= 0.5 / scale + 0.00001);
      CHECK(present(frame, slot));
      CHECK(code(frame, slot) == 3);
    }
  }
}

TEST_CASE("signed acceleration limits and rounding never overflow") {
  for (std::size_t slot : {19U, 20U}) {
    const float scale = slot == 19 ? 100.0F : 1000.0F;
    const std::size_t offset = slot == 19 ? 24 : 26;
    for (auto pair : {std::pair<float, std::uint16_t>{-32768.0F / scale, 0x8000U},
                      {32767.0F / scale, 0x7FFFU},
                      {-0.5F / scale - 0.000001F, 0xFFFFU},
                      {0.5F / scale + 0.000001F, 1U},
                      {-0.49F / scale, 0U},
                      {-32768.49F / scale, 0x8000U},
                      {32767.49F / scale, 0x7FFFU},
                      {slot == 19 ? -0.125F : -0.0625F, slot == 19 ? 0xFFF3U : 0xFFC1U},
                      {slot == 19 ? 0.125F : 0.0625F, slot == 19 ? 13U : 63U}}) {
      Fixture fixture;
      fixture.set(slot, SignalValue::number(pair.first), Availability::Fresh);
      const auto frame = fixture.frame();
      REQUIRE(frame.size() == 28);
      CHECK(present(frame, slot));
      CHECK(u16(frame, offset) == pair.second);
    }
    for (float value :
         {-32769.0F / scale, 32768.0F / scale, -32768.51F / scale, 32767.51F / scale,
          std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
          -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::max(),
          -std::numeric_limits<float>::max()}) {
      Fixture fixture;
      fixture.set(slot, SignalValue::number(value), Availability::Stale);
      const auto frame = fixture.frame();
      REQUIRE(frame.size() == 28);
      CHECK_FALSE(present(frame, slot));
      CHECK(u16(frame, offset) == 0);
      CHECK(code(frame, slot) == 2);
    }
  }
}

TEST_CASE("each acceleration status is preserved independently of its retained value") {
  for (std::size_t slot : {19U, 20U}) {
    for (auto availability : {Availability::NoData, Availability::Fresh, Availability::Stale,
                              Availability::FreshnessUnverified, Availability::Unavailable}) {
      Fixture fixture;
      fixture.set(slot, SignalValue::number(-1.0F), availability);
      auto frame = fixture.frame();
      REQUIRE(frame.size() == 28);
      CHECK(code(frame, slot) == static_cast<std::uint8_t>(status_code(availability)));
      CHECK(present(frame, slot));
      fixture.set(slot, std::nullopt, availability);
      frame = fixture.frame();
      CHECK(code(frame, slot) == static_cast<std::uint8_t>(status_code(availability)));
      CHECK_FALSE(present(frame, slot));
    }
  }
}

TEST_CASE("unreadable acceleration follows the existing unsupported and failure codes") {
  for (std::size_t slot : {19U, 20U}) {
    for (int defect = 0; defect < 3; ++defect) {
      auto catalog = full_catalog();
      if (defect == 0)
        catalog.erase(catalog.begin() + slot);
      if (defect == 1)
        catalog[slot].type = SignalType::Boolean;
      if (defect == 2)
        catalog[slot].capabilities = SignalCapability::Notify;
      Fixture fixture{catalog};
      const auto frame = fixture.frame();
      REQUIRE(frame.size() == 28);
      CHECK(code(frame, slot) == 6);
      CHECK_FALSE(present(frame, slot));
    }
    for (auto failure : {SignalStatus::InvalidSignal, SignalStatus::UnsupportedCapability,
                         SignalStatus::Faulted, SignalStatus::Timeout}) {
      Fixture fixture;
      fixture.provider.fail_reads(id_of(slot), failure);
      const auto frame = fixture.frame();
      REQUIRE(frame.size() == 28);
      CHECK(code(frame, slot) == static_cast<std::uint8_t>(status_code(failure)));
      CHECK_FALSE(present(frame, slot));
    }
    Fixture fixture;
    fixture.set(slot, SignalValue::boolean(true), Availability::Unavailable);
    const auto frame = fixture.frame();
    REQUIRE(frame.size() == 28);
    CHECK(code(frame, slot) == 4);
    CHECK_FALSE(present(frame, slot));
  }
}
