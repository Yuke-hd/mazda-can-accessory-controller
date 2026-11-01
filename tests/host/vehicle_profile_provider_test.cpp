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
using vehicle_profile_runtime::ProviderProcessStatus;
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
  using Interleave = void (*)(void *context);

  [[nodiscard]] vehicle_core::MonotonicTimestamp now() const noexcept override {
    const Interleave callback = interleave_;
    void *const context = interleave_context_;
    interleave_ = nullptr;
    interleave_context_ = nullptr;
    if (callback != nullptr) {
      callback(context);
    }
    return now_us_.load(std::memory_order_relaxed);
  }

  void set(const vehicle_core::MonotonicTimestamp value) noexcept {
    now_us_.store(value, std::memory_order_relaxed);
  }

  void interleave_once(Interleave callback, void *context) const noexcept {
    interleave_context_ = context;
    interleave_ = callback;
  }

private:
  std::atomic<vehicle_core::MonotonicTimestamp> now_us_{0};
  mutable Interleave interleave_{nullptr};
  mutable void *interleave_context_{nullptr};
};

class FakeSynchronization final {
public:
  [[nodiscard]] PublicationSynchronization binding() noexcept {
    return PublicationSynchronization{this, &enter, &leave};
  }

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

void decode_profile_a_wrong_type(const vehicle_core::RawCanFrame &frame, DecodeResult *results,
                                 const std::size_t capacity, std::size_t &count) {
  (void)vehicle_profile_runtime::append_result(
      results, capacity, count,
      DecodeResult::success(SignalId{1}, SignalValue::number(frame.data[0])));
  (void)vehicle_profile_runtime::append_result(
      results, capacity, count,
      DecodeResult::success(SignalId{2}, SignalValue::number(frame.data[1] * 2.0F)));
  (void)vehicle_profile_runtime::append_result(
      results, capacity, count, DecodeResult::success(SignalId{3}, SignalValue::enumeration(1)));
}

void decode_profile_a_bad_enum(const vehicle_core::RawCanFrame &frame, DecodeResult *results,
                               const std::size_t capacity, std::size_t &count) {
  (void)vehicle_profile_runtime::append_result(
      results, capacity, count, DecodeResult::success(SignalId{1}, SignalValue::boolean(true)));
  (void)vehicle_profile_runtime::append_result(
      results, capacity, count,
      DecodeResult::success(SignalId{2}, SignalValue::number(frame.data[1] * 2.0F)));
  (void)vehicle_profile_runtime::append_result(
      results, capacity, count, DecodeResult::success(SignalId{3}, SignalValue::enumeration(99)));
}

void decode_profile_a_bad_count(const vehicle_core::RawCanFrame &, DecodeResult *,
                                const std::size_t capacity, std::size_t &count) {
  count = capacity + 1U;
}

void decode_profile_a_missing_id(const vehicle_core::RawCanFrame &frame, DecodeResult *results,
                                 const std::size_t capacity, std::size_t &count) {
  (void)vehicle_profile_runtime::append_result(
      results, capacity, count,
      DecodeResult::success(SignalId{1}, SignalValue::boolean((frame.data[0] & 1U) != 0U)));
  (void)vehicle_profile_runtime::append_result(
      results, capacity, count,
      DecodeResult::success(SignalId{2}, SignalValue::number(frame.data[1] * 2.0F)));
}

void decode_profile_a_duplicate_id(const vehicle_core::RawCanFrame &frame, DecodeResult *results,
                                   const std::size_t capacity, std::size_t &count) {
  (void)vehicle_profile_runtime::append_result(
      results, capacity, count,
      DecodeResult::success(SignalId{1}, SignalValue::boolean((frame.data[0] & 1U) != 0U)));
  (void)vehicle_profile_runtime::append_result(
      results, capacity, count,
      DecodeResult::success(SignalId{1}, SignalValue::boolean((frame.data[1] & 1U) != 0U)));
  (void)vehicle_profile_runtime::append_result(
      results, capacity, count,
      DecodeResult::success(SignalId{3}, SignalValue::enumeration(frame.data[2])));
}

void decode_profile_a_foreign_id(const vehicle_core::RawCanFrame &frame, DecodeResult *results,
                                 const std::size_t capacity, std::size_t &count) {
  (void)vehicle_profile_runtime::append_result(
      results, capacity, count,
      DecodeResult::success(SignalId{99}, SignalValue::boolean((frame.data[0] & 1U) != 0U)));
  (void)vehicle_profile_runtime::append_result(
      results, capacity, count,
      DecodeResult::success(SignalId{2}, SignalValue::number(frame.data[1] * 2.0F)));
  (void)vehicle_profile_runtime::append_result(
      results, capacity, count,
      DecodeResult::success(SignalId{3}, SignalValue::enumeration(frame.data[2])));
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
constexpr DispatchEntry kProfileAWrongTypeDispatch[]{
    {0x101U, 3U, &decode_profile_a_wrong_type, kProfileAOutputs, std::size(kProfileAOutputs)}};
constexpr DispatchEntry kProfileABadEnumDispatch[]{
    {0x101U, 3U, &decode_profile_a_bad_enum, kProfileAOutputs, std::size(kProfileAOutputs)}};
constexpr DispatchEntry kProfileABadCountDispatch[]{
    {0x101U, 3U, &decode_profile_a_bad_count, kProfileAOutputs, std::size(kProfileAOutputs)}};
constexpr DispatchEntry kProfileAMissingIdDispatch[]{
    {0x101U, 3U, &decode_profile_a_missing_id, kProfileAOutputs, std::size(kProfileAOutputs)}};
constexpr DispatchEntry kProfileADuplicateIdDispatch[]{
    {0x101U, 3U, &decode_profile_a_duplicate_id, kProfileAOutputs, std::size(kProfileAOutputs)}};
constexpr DispatchEntry kProfileAForeignIdDispatch[]{
    {0x101U, 3U, &decode_profile_a_foreign_id, kProfileAOutputs, std::size(kProfileAOutputs)}};
constexpr SignalId kProfileAForeignOutputs[]{SignalId{1}, SignalId{2}, SignalId{99}};
constexpr DispatchEntry kProfileAForeignOutputDispatch[]{
    {0x101U, 3U, &decode_profile_a, kProfileAForeignOutputs, std::size(kProfileAForeignOutputs)}};
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
const ProfileTables kWrongTypeTables{
    SignalCatalogView{kProfileASignals},   kProfileAFreshness,
    std::size(kProfileAFreshness),         kProfileAWrongTypeDispatch,
    std::size(kProfileAWrongTypeDispatch), std::size(kProfileAOutputs)};
const ProfileTables kBadEnumTables{
    SignalCatalogView{kProfileASignals}, kProfileAFreshness,
    std::size(kProfileAFreshness),       kProfileABadEnumDispatch,
    std::size(kProfileABadEnumDispatch), std::size(kProfileAOutputs)};
const ProfileTables kBadCountTables{
    SignalCatalogView{kProfileASignals},  kProfileAFreshness,
    std::size(kProfileAFreshness),        kProfileABadCountDispatch,
    std::size(kProfileABadCountDispatch), std::size(kProfileAOutputs)};
const ProfileTables kMissingIdTables{
    SignalCatalogView{kProfileASignals},   kProfileAFreshness,
    std::size(kProfileAFreshness),         kProfileAMissingIdDispatch,
    std::size(kProfileAMissingIdDispatch), std::size(kProfileAOutputs)};
const ProfileTables kDuplicateIdTables{
    SignalCatalogView{kProfileASignals},     kProfileAFreshness,
    std::size(kProfileAFreshness),           kProfileADuplicateIdDispatch,
    std::size(kProfileADuplicateIdDispatch), std::size(kProfileAOutputs)};
const ProfileTables kForeignIdTables{
    SignalCatalogView{kProfileASignals},   kProfileAFreshness,
    std::size(kProfileAFreshness),         kProfileAForeignIdDispatch,
    std::size(kProfileAForeignIdDispatch), std::size(kProfileAOutputs)};
const ProfileTables kForeignOutputTables{SignalCatalogView{kProfileASignals},
                                         kProfileAFreshness,
                                         std::size(kProfileAFreshness),
                                         kProfileAForeignOutputDispatch,
                                         std::size(kProfileAForeignOutputDispatch),
                                         std::size(kProfileAForeignOutputs)};
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

void process_during_clock(void *context) {
  auto &provider = *static_cast<ProfileProvider *>(context);
  const auto next = frame(0x101U, 2000U, {0, 20, 2, 0, 0, 0, 0, 0}, 3U);
  (void)ProviderAccess::process(provider, next);
}

void check_invalid_result_list(const ProfileTables &tables) {
  FakeClock clock;
  FakeSynchronization synchronization;
  ProfileProvider provider{tables, clock, synchronization.binding()};
  clock.set(100U);
  const auto result =
      ProviderAccess::process(provider, frame(0x101U, 100U, {1, 5, 1, 0, 0, 0, 0, 0}, 3U));
  CHECK(result.status == ProviderProcessStatus::InvalidResults);
  for (const SignalId id : {SignalId{1}, SignalId{2}, SignalId{3}}) {
    const auto reading = provider.read(id);
    REQUIRE(reading.ok());
    CHECK(reading.value->availability == Availability::Unavailable);
  }
}

void check_malformed_shape(const vehicle_core::RawCanFrame &malformed) {
  FakeClock clock;
  FakeSynchronization synchronization;
  ProfileProvider provider{kProfileATables, clock, synchronization.binding()};
  clock.set(10U);
  (void)ProviderAccess::process(provider, frame(0x101U, 10U, {1, 5, 1, 0, 0, 0, 0, 0}, 3U));
  clock.set(malformed.timestamp_us);
  const auto result = ProviderAccess::process(provider, malformed);
  CHECK(result.status == ProviderProcessStatus::Malformed);
  for (const SignalId id : {SignalId{1}, SignalId{2}, SignalId{3}}) {
    const auto reading = provider.read(id);
    REQUIRE(reading.ok());
    CHECK(reading.value->availability == Availability::Unavailable);
  }
}

TEST_CASE("provider returns copied typed readings and preserves request errors") {
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
  const auto input = frame(0x101U, 1000U, {1, 17, 2, 0, 0, 0, 0, 0}, 3U);
  (void)ProviderAccess::process(provider, input);

  const auto flag = provider.read(SignalId{1});
  REQUIRE(flag.ok());
  CHECK(flag.value->availability == Availability::Fresh);
  REQUIRE(flag.value->value.has_value());
  CHECK(flag.value->value->as_boolean() == true);

  const auto speed = provider.read(SignalId{2});
  REQUIRE(speed.ok());
  CHECK(speed.value->availability == Availability::FreshnessUnverified);
  REQUIRE(speed.value->value.has_value());
  CHECK(speed.value->value->as_number() == 34.0F);
  CHECK(speed.value->validation == ValidationStatus::Confirmed);

  const auto mode = provider.read(SignalId{3});
  REQUIRE(mode.ok());
  REQUIRE(mode.value->value.has_value());
  CHECK(mode.value->value->as_enumeration() == 2U);
}

TEST_CASE("provider isolates two populated profiles, an empty profile and custom decoders") {
  FakeClock clock_a;
  FakeClock clock_b;
  FakeClock empty_clock;
  FakeClock custom_clock;
  FakeSynchronization synchronization_a;
  FakeSynchronization synchronization_b;
  FakeSynchronization empty_synchronization;
  FakeSynchronization custom_synchronization;
  ProfileProvider profile_a{kProfileATables, clock_a, synchronization_a.binding()};
  ProfileProvider profile_b{kProfileBTables, clock_b, synchronization_b.binding()};
  ProfileProvider empty{kEmptyTables, empty_clock, empty_synchronization.binding()};
  ProfileProvider custom{kCustomTables, custom_clock, custom_synchronization.binding()};

  clock_a.set(100U);
  clock_b.set(100U);
  (void)ProviderAccess::process(profile_a, frame(0x101U, 100U, {1, 5, 1, 0, 0, 0, 0, 0}, 3U));
  (void)ProviderAccess::process(profile_b, frame(0x202U, 100U, {6, 0, 0, 0, 0, 0, 0, 0}, 1U));
  (void)ProviderAccess::process(custom, frame(0x303U, 100U, {0xa5, 0, 0, 0, 0, 0, 0, 0}, 1U));

  const auto a_value = profile_a.read(SignalId{1});
  const auto b_value = profile_b.read(SignalId{1});
  REQUIRE(a_value.ok());
  REQUIRE(b_value.ok());
  REQUIRE(a_value.value->value.has_value());
  REQUIRE(b_value.value->value.has_value());
  CHECK(a_value.value->value->as_boolean() == true);
  CHECK_FALSE(b_value.value->value->as_boolean().has_value());
  CHECK(b_value.value->value->as_number() == 9.0F);
  CHECK(a_value.value->availability == Availability::Fresh);
  CHECK(b_value.value->availability == Availability::FreshnessUnverified);

  CHECK(empty.catalog().empty());
  CHECK(empty.read(SignalId{1}).status == vehicle_signals::SignalStatus::InvalidSignal);
  CHECK(custom.read(SignalId{10}).value->value->as_boolean() == true);
  CHECK(profile_a.read(SignalId{10}).status == vehicle_signals::SignalStatus::InvalidSignal);
}

TEST_CASE(
    "freshness is inclusive, silence and unrelated traffic do not refresh, newer frames recover") {
  FakeClock clock;
  FakeSynchronization synchronization;
  ProfileProvider provider{kProfileATables, clock, synchronization.binding()};
  clock.set(1000U);
  (void)ProviderAccess::process(provider, frame(0x101U, 1000U, {1, 7, 1, 0, 0, 0, 0, 0}, 3U));
  (void)ProviderAccess::process(provider, frame(0x101U, 1000U, {0, 8, 1, 0, 0, 0, 0, 0}, 2U));
  CHECK(provider.read(SignalId{1}).value->availability == Availability::Fresh);

  clock.set(101000U);
  CHECK(provider.read(SignalId{1}).value->availability == Availability::Fresh);
  clock.set(101001U);
  CHECK(provider.read(SignalId{1}).value->availability == Availability::Stale);
  CHECK(provider.read(SignalId{2}).value->availability == Availability::FreshnessUnverified);

  (void)ProviderAccess::process(provider, frame(0x777U, 101001U, {}, 8U));
  CHECK(provider.read(SignalId{1}).value->availability == Availability::Stale);

  clock.set(101002U);
  (void)ProviderAccess::process(provider, frame(0x101U, 101002U, {0, 9, 1, 0, 0, 0, 0, 0}, 2U));
  CHECK(provider.read(SignalId{1}).value->availability == Availability::Unavailable);
  CHECK(provider.read(SignalId{1}).value->value->as_boolean() == true);

  // Equal and older observations cannot recover the malformed state.
  (void)ProviderAccess::process(provider, frame(0x101U, 101002U, {0, 9, 1, 0, 0, 0, 0, 0}, 3U));
  (void)ProviderAccess::process(provider, frame(0x101U, 101001U, {0, 9, 1, 0, 0, 0, 0, 0}, 3U));
  CHECK(provider.read(SignalId{1}).value->availability == Availability::Unavailable);

  clock.set(101003U);
  (void)ProviderAccess::process(provider, frame(0x101U, 101003U, {0, 9, 1, 0, 0, 0, 0, 0}, 3U));
  CHECK(provider.read(SignalId{1}).value->availability == Availability::Fresh);
  CHECK(provider.read(SignalId{2}).value->availability == Availability::FreshnessUnverified);
}

TEST_CASE(
    "fault and restart invalidate state and require observations newer than their watermarks") {
  FakeClock clock;
  FakeSynchronization synchronization;
  ProfileProvider provider{kProfileATables, clock, synchronization.binding()};
  clock.set(100U);
  (void)ProviderAccess::process(provider, frame(0x101U, 100U, {1, 4, 2, 0, 0, 0, 0, 0}, 3U));

  clock.set(200U);
  (void)ProviderAccess::fault(provider);
  CHECK(provider.read(SignalId{1}).value->availability == Availability::Unavailable);
  (void)ProviderAccess::process(provider, frame(0x101U, 200U, {0, 5, 1, 0, 0, 0, 0, 0}, 3U));
  CHECK(provider.read(SignalId{1}).value->availability == Availability::Unavailable);

  clock.set(201U);
  (void)ProviderAccess::process(provider, frame(0x101U, 201U, {0, 5, 1, 0, 0, 0, 0, 0}, 3U));
  CHECK(provider.read(SignalId{1}).value->availability == Availability::Fresh);

  clock.set(300U);
  (void)ProviderAccess::reset(provider);
  const auto restarted = provider.read(SignalId{1});
  REQUIRE(restarted.ok());
  CHECK(restarted.value->availability == Availability::NoData);
  CHECK_FALSE(restarted.value->value.has_value());
  (void)ProviderAccess::process(provider, frame(0x101U, 201U, {1, 8, 2, 0, 0, 0, 0, 0}, 3U));
  CHECK(provider.read(SignalId{1}).value->availability == Availability::NoData);
  (void)ProviderAccess::process(provider, frame(0x101U, 301U, {0, 8, 2, 0, 0, 0, 0, 0}, 3U));
  CHECK(provider.read(SignalId{1}).value->availability == Availability::Fresh);
}

TEST_CASE("due state is reported before a newer observation recovers the signal") {
  FakeClock clock;
  FakeSynchronization synchronization;
  ProfileProvider provider{kProfileATables, clock, synchronization.binding()};
  clock.set(1000U);
  (void)ProviderAccess::process(provider, frame(0x101U, 1000U, {1, 4, 1, 0, 0, 0, 0, 0}, 3U));

  clock.set(101001U);
  const auto outcome =
      ProviderAccess::process(provider, frame(0x101U, 101001U, {0, 5, 2, 0, 0, 0, 0, 0}, 3U));
  CHECK(outcome.due_count == 1U);
  CHECK(outcome.due[0] == SignalId{1});
  CHECK(outcome.recovered_count == 1U);
  CHECK(outcome.recovered[0] == SignalId{1});
  CHECK(provider.read(SignalId{1}).value->availability == Availability::Fresh);
}

TEST_CASE("invalid decoder types and enum values invalidate only their declared outputs") {
  FakeClock wrong_type_clock;
  FakeClock bad_enum_clock;
  FakeSynchronization wrong_type_synchronization;
  FakeSynchronization bad_enum_synchronization;
  ProfileProvider wrong_type{kWrongTypeTables, wrong_type_clock,
                             wrong_type_synchronization.binding()};
  ProfileProvider bad_enum{kBadEnumTables, bad_enum_clock, bad_enum_synchronization.binding()};
  wrong_type_clock.set(100U);
  bad_enum_clock.set(100U);

  (void)ProviderAccess::process(wrong_type, frame(0x101U, 100U, {1, 7, 1, 0, 0, 0, 0, 0}, 3U));
  CHECK(wrong_type.read(SignalId{1}).value->availability == Availability::Unavailable);
  CHECK(wrong_type.read(SignalId{2}).value->value->as_number() == 14.0F);
  CHECK(wrong_type.read(SignalId{2}).value->availability == Availability::FreshnessUnverified);

  (void)ProviderAccess::process(bad_enum, frame(0x101U, 100U, {1, 7, 1, 0, 0, 0, 0, 0}, 3U));
  CHECK(bad_enum.read(SignalId{3}).value->availability == Availability::Unavailable);
  CHECK(bad_enum.read(SignalId{1}).value->availability == Availability::Fresh);
}

TEST_CASE("reported decoder overflow fails off all outputs without reading beyond capacity") {
  FakeClock clock;
  FakeSynchronization synchronization;
  ProfileProvider provider{kBadCountTables, clock, synchronization.binding()};
  clock.set(100U);
  (void)ProviderAccess::process(provider, frame(0x101U, 100U, {}, 3U));

  CHECK(provider.read(SignalId{1}).value->availability == Availability::Unavailable);
  CHECK(provider.read(SignalId{2}).value->availability == Availability::Unavailable);
  CHECK(provider.read(SignalId{3}).value->availability == Availability::Unavailable);
}

TEST_CASE("missing, duplicate and foreign decoder result IDs fail off the owned message") {
  check_invalid_result_list(kMissingIdTables);
  check_invalid_result_list(kDuplicateIdTables);
  check_invalid_result_list(kForeignIdTables);
}

TEST_CASE("dispatch output IDs must belong to the injected catalog") {
  FakeClock clock;
  FakeSynchronization synchronization;
  ProfileProvider provider{kForeignOutputTables, clock, synchronization.binding()};

  CHECK(provider.read(SignalId{1}).status == vehicle_signals::SignalStatus::Faulted);
}

TEST_CASE("extended, remote and overlength owned frames invalidate their outputs") {
  auto extended = frame(0x101U, 20U, {0, 5, 1, 0, 0, 0, 0, 0}, 3U);
  extended.identifier_format = vehicle_core::CanIdentifierFormat::Extended;
  check_malformed_shape(extended);

  auto remote = frame(0x101U, 20U, {0, 5, 1, 0, 0, 0, 0, 0}, 3U);
  remote.remote_request = true;
  check_malformed_shape(remote);

  auto overlength = frame(0x101U, 20U, {0, 5, 1, 0, 0, 0, 0, 0}, 9U);
  check_malformed_shape(overlength);
}

TEST_CASE("read copies its state before sampling time outside the publication lock") {
  FakeClock clock;
  FakeSynchronization synchronization;
  ProfileProvider provider{kProfileATables, clock, synchronization.binding()};
  clock.set(1000U);
  (void)ProviderAccess::process(provider, frame(0x101U, 1000U, {0, 10, 1, 0, 0, 0, 0, 0}, 3U));

  clock.set(2000U);
  clock.interleave_once(&process_during_clock, &provider);
  const auto copied = provider.read(SignalId{2});
  REQUIRE(copied.ok());
  REQUIRE(copied.value->value.has_value());
  CHECK(copied.value->value->as_number() == 20.0F);
  CHECK(provider.read(SignalId{2}).value->value->as_number() == 40.0F);
}

} // namespace
