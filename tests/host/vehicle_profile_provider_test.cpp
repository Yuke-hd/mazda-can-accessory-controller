#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

#include "provider_access.hpp"
#include "vehicle_core/telemetry_contracts.hpp"
#include "vehicle_profile_runtime/provider.hpp"
#include "vehicle_profile_runtime/tables.hpp"

namespace {

using vehicle_profile_runtime::DecodeResult;
using vehicle_profile_runtime::DispatchEntry;
using vehicle_profile_runtime::ProfileProvider;
using vehicle_profile_runtime::ProfileTables;
using vehicle_profile_runtime::ProviderAccess;
using vehicle_profile_runtime::PublicationSynchronization;
using vehicle_profile_runtime::SignalFreshnessPolicy;
using vehicle_signals::Availability;
using vehicle_signals::SignalCapability;
using vehicle_signals::SignalCatalogView;
using vehicle_signals::SignalEnumChoice;
using vehicle_signals::SignalId;
using vehicle_signals::SignalMetadata;
using vehicle_signals::SignalType;
using vehicle_signals::SignalUnit;
using vehicle_signals::SignalValue;
using vehicle_signals::ValidationStatus;

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
  [[nodiscard]] PublicationSynchronization binding() noexcept { return {this, &enter, &leave}; }

private:
  static void enter(void *context) noexcept {
    static_cast<FakeSynchronization *>(context)->mutex_.lock();
  }
  static void leave(void *context) noexcept {
    static_cast<FakeSynchronization *>(context)->mutex_.unlock();
  }
  std::mutex mutex_{};
};

constexpr SignalEnumChoice kModes[]{{1, "off"}, {2, "on"}};
constexpr SignalMetadata kProfileASignals[]{
    {SignalId{1}, "flag", SignalType::Boolean, SignalUnit::None, ValidationStatus::Reference,
     SignalCapability::Read, nullptr, 0},
    {SignalId{2}, "speed", SignalType::Number, SignalUnit::KilometresPerHour,
     ValidationStatus::Confirmed, SignalCapability::Read, nullptr, 0},
    {SignalId{3}, "mode", SignalType::Enum, SignalUnit::None, ValidationStatus::Reference,
     SignalCapability::Read, kModes, std::size(kModes)},
    {SignalId{4}, "notify_only", SignalType::Boolean, SignalUnit::None, ValidationStatus::Reference,
     SignalCapability::Notify, nullptr, 0},
};
constexpr SignalFreshnessPolicy kProfileAFreshness[]{
    {SignalId{1}, true, 100},
    {SignalId{2}, false, 0},
    {SignalId{3}, true, 250},
    {SignalId{4}, false, 0},
};
constexpr SignalId kProfileAOutputs[]{SignalId{1}, SignalId{2}, SignalId{3}};

constexpr SignalMetadata kProfileBSignals[]{
    {SignalId{1}, "speed", SignalType::Number, SignalUnit::RevolutionsPerMinute,
     ValidationStatus::Confirmed, SignalCapability::Read, nullptr, 0},
};
constexpr SignalFreshnessPolicy kProfileBFreshness[]{{SignalId{1}, false, 0}};
constexpr SignalId kProfileBOutputs[]{SignalId{1}};

constexpr SignalMetadata kCustomSignals[]{
    {SignalId{10}, "custom", SignalType::Boolean, SignalUnit::None, ValidationStatus::Reference,
     SignalCapability::Read, nullptr, 0},
};
constexpr SignalFreshnessPolicy kCustomFreshness[]{{SignalId{10}, false, 0}};
constexpr SignalId kCustomOutputs[]{SignalId{10}};

void decode_profile_a(const vehicle_core::RawCanFrame &frame, DecodeResult *results,
                      const std::size_t capacity, std::size_t &count) {
  (void)vehicle_profile_runtime::append_result(
      results, capacity, count,
      DecodeResult::success(SignalId{1}, SignalValue::boolean((frame.data[0] & 1U) != 0U)));
  (void)vehicle_profile_runtime::append_result(
      results, capacity, count,
      DecodeResult::success(SignalId{2}, SignalValue::number(frame.data[1] * 2.0F)));
  (void)vehicle_profile_runtime::append_result(
      results, capacity, count,
      DecodeResult::success(SignalId{3}, SignalValue::enumeration(frame.data[2])));
}

void decode_profile_b(const vehicle_core::RawCanFrame &frame, DecodeResult *results,
                      const std::size_t capacity, std::size_t &count) {
  (void)vehicle_profile_runtime::append_result(
      results, capacity, count,
      DecodeResult::success(SignalId{1}, SignalValue::number(frame.data[0] * 1.5F)));
}

void decode_custom(const vehicle_core::RawCanFrame &frame, DecodeResult *results,
                   const std::size_t capacity, std::size_t &count) {
  (void)vehicle_profile_runtime::append_result(
      results, capacity, count,
      DecodeResult::success(SignalId{10}, SignalValue::boolean(frame.data[0] == 0xa5U)));
}

constexpr DispatchEntry kProfileADispatch[]{
    {0x101U, 3U, &decode_profile_a, kProfileAOutputs, std::size(kProfileAOutputs)}};
constexpr DispatchEntry kProfileBDispatch[]{
    {0x202U, 1U, &decode_profile_b, kProfileBOutputs, std::size(kProfileBOutputs)}};
constexpr DispatchEntry kCustomDispatch[]{
    {0x303U, 1U, &decode_custom, kCustomOutputs, std::size(kCustomOutputs)}};

const ProfileTables kProfileATables{
    SignalCatalogView{kProfileASignals}, kProfileAFreshness,
    std::size(kProfileAFreshness),       kProfileADispatch,
    std::size(kProfileADispatch),        std::size(kProfileAOutputs)};
const ProfileTables kProfileBTables{
    SignalCatalogView{kProfileBSignals}, kProfileBFreshness,
    std::size(kProfileBFreshness),       kProfileBDispatch,
    std::size(kProfileBDispatch),        std::size(kProfileBOutputs)};
const ProfileTables kEmptyTables{};
const ProfileTables kCustomTables{SignalCatalogView{kCustomSignals}, kCustomFreshness,
                                  std::size(kCustomFreshness),       kCustomDispatch,
                                  std::size(kCustomDispatch),        std::size(kCustomOutputs)};

vehicle_core::RawCanFrame frame(const std::uint32_t identifier,
                                const vehicle_core::MonotonicTimestamp timestamp,
                                const std::array<std::uint8_t, 8> &data,
                                const std::uint8_t dlc) noexcept {
  vehicle_core::RawCanFrame result{};
  result.identifier = identifier;
  result.timestamp_us = timestamp;
  result.data = data;
  result.dlc = dlc;
  return result;
}

TEST_CASE("provider reads copied types and preserves request errors") {
  FakeClock clock;
  FakeSynchronization synchronization;
  ProfileProvider provider{kProfileATables, clock, synchronization.binding()};

  CHECK(provider.catalog().size() == 4U);
  const auto no_data = provider.read(SignalId{1});
  REQUIRE(no_data.ok());
  CHECK(no_data.value->availability == Availability::NoData);
  CHECK_FALSE(no_data.value->value.has_value());
  CHECK(provider.read(SignalId{}).status == vehicle_signals::SignalStatus::InvalidSignal);
  CHECK(provider.read(SignalId{99}).status == vehicle_signals::SignalStatus::InvalidSignal);
  CHECK(provider.read(SignalId{4}).status == vehicle_signals::SignalStatus::UnsupportedCapability);

  clock.set(1000U);
  (void)ProviderAccess::process(provider, frame(0x101U, 1000U, {1, 17, 2, 0, 0, 0, 0, 0}, 3U));
  const auto flag = provider.read(SignalId{1});
  REQUIRE(flag.ok());
  CHECK(flag.value->availability == Availability::Fresh);
  REQUIRE(flag.value->value.has_value());
  CHECK(flag.value->value->as_boolean() == true);
  const auto speed = provider.read(SignalId{2});
  REQUIRE(speed.ok());
  CHECK(speed.value->availability == Availability::FreshnessUnverified);
  CHECK(speed.value->value->as_number() == 34.0F);
  CHECK(speed.value->validation == ValidationStatus::Confirmed);
  CHECK(provider.read(SignalId{3}).value->value->as_enumeration() == 2U);
}

TEST_CASE("provider instances isolate populated, empty and custom decoder tables") {
  FakeClock clock_a;
  FakeClock clock_b;
  FakeClock empty_clock;
  FakeClock custom_clock;
  FakeSynchronization sync_a;
  FakeSynchronization sync_b;
  FakeSynchronization sync_empty;
  FakeSynchronization sync_custom;
  ProfileProvider profile_a{kProfileATables, clock_a, sync_a.binding()};
  ProfileProvider profile_b{kProfileBTables, clock_b, sync_b.binding()};
  ProfileProvider empty{kEmptyTables, empty_clock, sync_empty.binding()};
  ProfileProvider custom{kCustomTables, custom_clock, sync_custom.binding()};

  clock_a.set(100U);
  clock_b.set(100U);
  (void)ProviderAccess::process(profile_a, frame(0x101U, 100U, {1, 5, 1, 0, 0, 0, 0, 0}, 3U));
  (void)ProviderAccess::process(profile_b, frame(0x202U, 100U, {6, 0, 0, 0, 0, 0, 0, 0}, 1U));
  (void)ProviderAccess::process(custom, frame(0x303U, 100U, {0xa5, 0, 0, 0, 0, 0, 0, 0}, 1U));

  CHECK(profile_a.read(SignalId{1}).value->value->as_boolean() == true);
  CHECK_FALSE(profile_b.read(SignalId{1}).value->value->as_boolean().has_value());
  CHECK(profile_b.read(SignalId{1}).value->value->as_number() == 9.0F);
  CHECK(profile_a.read(SignalId{1}).value->availability == Availability::Fresh);
  CHECK(profile_b.read(SignalId{1}).value->availability == Availability::FreshnessUnverified);
  CHECK(empty.catalog().empty());
  CHECK(empty.read(SignalId{1}).status == vehicle_signals::SignalStatus::InvalidSignal);
  CHECK(custom.read(SignalId{10}).value->value->as_boolean() == true);
  CHECK(profile_a.read(SignalId{10}).status == vehicle_signals::SignalStatus::InvalidSignal);
}

TEST_CASE("freshness uses an inclusive deadline and undeclared policies stay unverified") {
  FakeClock clock;
  FakeSynchronization synchronization;
  ProfileProvider provider{kProfileATables, clock, synchronization.binding()};
  clock.set(1000U);
  (void)ProviderAccess::process(provider, frame(0x101U, 1000U, {1, 7, 1, 0, 0, 0, 0, 0}, 3U));

  clock.set(101000U);
  CHECK(provider.read(SignalId{1}).value->availability == Availability::Fresh);
  clock.set(101001U);
  CHECK(provider.read(SignalId{1}).value->availability == Availability::Stale);
  CHECK(provider.read(SignalId{2}).value->availability == Availability::FreshnessUnverified);
  (void)ProviderAccess::process(provider, frame(0x777U, 101001U, {}, 8U));
  CHECK(provider.read(SignalId{1}).value->availability == Availability::Stale);
}

} // namespace
