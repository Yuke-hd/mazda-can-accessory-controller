#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string_view>
#include <type_traits>

#include "vehicle_signals/signal_catalog.hpp"
#include "vehicle_signals/signal_contracts.hpp"
#include "vehicle_signals/signal_provider.hpp"

namespace {

using vehicle_signals::Availability;
using vehicle_signals::SignalCallback;
using vehicle_signals::SignalCapabilities;
using vehicle_signals::SignalCapability;
using vehicle_signals::SignalCatalogView;
using vehicle_signals::SignalEnumChoice;
using vehicle_signals::SignalId;
using vehicle_signals::SignalMetadata;
using vehicle_signals::SignalNotification;
using vehicle_signals::SignalProvider;
using vehicle_signals::SignalReading;
using vehicle_signals::SignalResult;
using vehicle_signals::SignalStatus;
using vehicle_signals::SignalStatusResult;
using vehicle_signals::SignalSubscription;
using vehicle_signals::SignalType;
using vehicle_signals::SignalUnit;
using vehicle_signals::SignalValue;
using vehicle_signals::ValidationStatus;

// A generic, make-independent fixture catalog.
constexpr SignalEnumChoice kGearChoices[] = {
    {0, "park"},
    {1, "reverse"},
    {2, "neutral"},
    {3, "drive"},
};

constexpr SignalCapabilities kReadOnly{SignalCapability::Read};
constexpr SignalCapabilities kReadNotify = SignalCapability::Read | SignalCapability::Notify;

constexpr SignalMetadata kCatalog[] = {
    {SignalId{1}, "vehicle.speed", SignalType::Number, SignalUnit::KilometresPerHour,
     ValidationStatus::Observed, kReadOnly, nullptr, 0},
    {SignalId{2}, "engine.rpm", SignalType::Number, SignalUnit::RevolutionsPerMinute,
     ValidationStatus::Reference, kReadOnly, nullptr, 0},
    {SignalId{3}, "transmission.gear", SignalType::Enum, SignalUnit::None,
     ValidationStatus::Confirmed, kReadNotify, kGearChoices, std::size(kGearChoices)},
    {SignalId{7}, "body.door_open", SignalType::Boolean, SignalUnit::None,
     ValidationStatus::Observed, kReadNotify, nullptr, 0},
};

constexpr SignalCatalogView kView{kCatalog};

static_assert(kView.size() == 4);
static_assert(kView.well_formed());
static_assert(kView.find(SignalId{3}) == &kCatalog[2]);
static_assert(kView.find("engine.rpm") == &kCatalog[1]);
static_assert(kView.find(SignalId{}) == nullptr);
static_assert(kCatalog[2].find_choice(std::uint16_t{3})->key == "drive");
static_assert(SignalValue::number(1.5F).as_number() == 1.5F);
static_assert(!SignalValue::number(1.5F).as_boolean().has_value());
static_assert(std::is_trivially_copyable_v<SignalId>);
static_assert(std::is_trivially_copyable_v<SignalValue>);
static_assert(std::is_trivially_copyable_v<SignalSubscription>);
static_assert(std::is_trivially_copyable_v<SignalCapabilities>);
static_assert(std::is_nothrow_copy_constructible_v<SignalNotification>);
static_assert(
    std::is_same_v<SignalCallback, void (*)(void *, const SignalNotification &) noexcept>);

// The provider port is an abstract, non-owning interface: it cannot be
// copied, moved, or destroyed through the base.
static_assert(std::is_abstract_v<SignalProvider>);
static_assert(!std::is_copy_constructible_v<SignalProvider>);
static_assert(!std::is_copy_assignable_v<SignalProvider>);
static_assert(!std::is_move_constructible_v<SignalProvider>);
static_assert(!std::is_destructible_v<SignalProvider>);
static_assert(!std::has_virtual_destructor_v<SignalProvider>);

// A minimal provider over the fixture catalog proves the port is
// implementable without any make-specific type.
class CatalogOnlyProvider final : public SignalProvider {
public:
  [[nodiscard]] SignalCatalogView catalog() const noexcept override { return kView; }
  [[nodiscard]] SignalResult<SignalSubscription> subscribe(SignalId, SignalCallback,
                                                           void *) noexcept override {
    return SignalResult<SignalSubscription>::failure(SignalStatus::UnsupportedCapability);
  }
  [[nodiscard]] SignalStatusResult unsubscribe(SignalSubscription) noexcept override {
    return SignalStatusResult::failure(SignalStatus::InvalidSubscription);
  }
};

struct CallbackProbe {
  int calls{0};
  SignalNotification last{};
};

void record_notification(void *context, const SignalNotification &notification) noexcept {
  auto *probe = static_cast<CallbackProbe *>(context);
  ++probe->calls;
  probe->last = notification;
}

} // namespace

TEST_CASE("signal ids reserve zero as invalid") {
  CHECK_FALSE(SignalId{}.valid());
  CHECK_FALSE(SignalId{0}.valid());
  CHECK(SignalId{1}.valid());
  CHECK(SignalId{5}.value() == 5);
  CHECK(SignalId{5} == SignalId{5});
  CHECK(SignalId{5} != SignalId{6});
  CHECK(SignalId{5} < SignalId{6});
}

TEST_CASE("a consumer resolves transmission.gear through the provider port") {
  CatalogOnlyProvider concrete{};
  const SignalProvider &provider = concrete;
  const SignalMetadata *gear = provider.catalog().find(std::string_view{"transmission.gear"});
  REQUIRE(gear != nullptr);
  CHECK(gear->id == SignalId{3});
  CHECK(concrete.subscribe(gear->id, &record_notification, nullptr).status ==
        SignalStatus::UnsupportedCapability);
  CHECK(concrete.unsubscribe(SignalSubscription{}).status == SignalStatus::InvalidSubscription);
}

TEST_CASE("catalog view looks up metadata by runtime id") {
  const SignalMetadata *speed = kView.find(SignalId{1});
  REQUIRE(speed != nullptr);
  CHECK(speed->key == "vehicle.speed");
  CHECK(speed->type == SignalType::Number);
  CHECK(speed->unit == SignalUnit::KilometresPerHour);
  CHECK(speed->validation == ValidationStatus::Observed);

  const SignalMetadata *door = kView.find(SignalId{7});
  REQUIRE(door != nullptr);
  CHECK(door->key == "body.door_open");
}

TEST_CASE("catalog view rejects zero and unknown ids") {
  CHECK(kView.find(SignalId{0}) == nullptr);
  CHECK(kView.find(SignalId{4}) == nullptr);
  CHECK(kView.find(SignalId{0xffff}) == nullptr);
}

TEST_CASE("catalog view looks up metadata by persistent key") {
  const SignalMetadata *gear = kView.find(std::string_view{"transmission.gear"});
  REQUIRE(gear != nullptr);
  CHECK(gear->id == SignalId{3});
  CHECK(kView.find(gear->id) == gear);

  CHECK(kView.find(std::string_view{"missing.signal"}) == nullptr);
  CHECK(kView.find(std::string_view{}) == nullptr);
  CHECK(kView.find(std::string_view{"engine.rpm.extra"}) == nullptr);
  CHECK(kView.find(std::string_view{"engine"}) == nullptr);
}

TEST_CASE("catalog view iterates the fixed array in order") {
  std::size_t count = 0;
  for (const SignalMetadata &entry : kView) {
    CHECK(&entry == &kCatalog[count]);
    ++count;
  }
  CHECK(count == kView.size());
  CHECK_FALSE(kView.empty());
  CHECK(kView.at(0) == &kCatalog[0]);
  CHECK(kView.at(3) == &kCatalog[3]);
  CHECK(kView.at(4) == nullptr);
}

TEST_CASE("empty catalog views are safe") {
  constexpr SignalCatalogView empty{};
  CHECK(empty.empty());
  CHECK(empty.begin() == empty.end());
  CHECK(empty.at(0) == nullptr);
  CHECK(empty.find(SignalId{1}) == nullptr);
  CHECK(empty.find(std::string_view{"vehicle.speed"}) == nullptr);
  CHECK(empty.well_formed());

  const SignalCatalogView null_entries{nullptr, 5};
  CHECK(null_entries.size() == 0);
  CHECK(null_entries.find(SignalId{1}) == nullptr);
}

TEST_CASE("capabilities report read and notify support") {
  const SignalMetadata *speed = kView.find(std::string_view{"vehicle.speed"});
  const SignalMetadata *gear = kView.find(std::string_view{"transmission.gear"});
  REQUIRE(speed != nullptr);
  REQUIRE(gear != nullptr);

  CHECK(speed->capabilities.has(SignalCapability::Read));
  CHECK_FALSE(speed->capabilities.has(SignalCapability::Notify));
  CHECK(gear->capabilities.has(SignalCapability::Read));
  CHECK(gear->capabilities.has(SignalCapability::Notify));

  CHECK(gear->capabilities.supports(kReadNotify));
  CHECK_FALSE(speed->capabilities.supports(kReadNotify));
  CHECK(speed->capabilities.supports(SignalCapability::Read));
  CHECK(speed->capabilities.supports(SignalCapabilities{}));

  CHECK(SignalCapabilities{}.empty());
  CHECK(SignalCapabilities::from_bits(0xff) == kReadNotify);
  CHECK(kReadNotify.bits() == 0x03);
}

TEST_CASE("enum metadata exposes choices by value and key") {
  const SignalMetadata *gear = kView.find(SignalId{3});
  REQUIRE(gear != nullptr);
  CHECK(gear->type == SignalType::Enum);
  CHECK(gear->choice_count == 4);

  const SignalEnumChoice *reverse = gear->find_choice(std::uint16_t{1});
  REQUIRE(reverse != nullptr);
  CHECK(reverse->key == "reverse");

  const SignalEnumChoice *drive = gear->find_choice(std::string_view{"drive"});
  REQUIRE(drive != nullptr);
  CHECK(drive->value == 3);

  CHECK(gear->find_choice(std::uint16_t{9}) == nullptr);
  CHECK(gear->find_choice(std::string_view{"sport"}) == nullptr);

  const SignalMetadata *speed = kView.find(SignalId{1});
  REQUIRE(speed != nullptr);
  CHECK(speed->find_choice(std::uint16_t{0}) == nullptr);
}

TEST_CASE("metadata accepts only values of its type and enum choices") {
  const SignalMetadata *gear = kView.find(SignalId{3});
  const SignalMetadata *speed = kView.find(SignalId{1});
  const SignalMetadata *door = kView.find(SignalId{7});
  REQUIRE(gear != nullptr);
  REQUIRE(speed != nullptr);
  REQUIRE(door != nullptr);

  CHECK(gear->accepts(SignalValue::enumeration(2)));
  CHECK_FALSE(gear->accepts(SignalValue::enumeration(42)));
  CHECK_FALSE(gear->accepts(SignalValue::number(2.0F)));
  CHECK(speed->accepts(SignalValue::number(88.0F)));
  CHECK_FALSE(speed->accepts(SignalValue::boolean(true)));
  CHECK(door->accepts(SignalValue::boolean(false)));
  CHECK_FALSE(door->accepts(SignalValue::enumeration(0)));
}

TEST_CASE("malformed catalogs are detected") {
  static constexpr SignalMetadata duplicate_id[] = {
      {SignalId{1}, "a", SignalType::Boolean, SignalUnit::None, ValidationStatus::Reference,
       kReadOnly, nullptr, 0},
      {SignalId{1}, "b", SignalType::Boolean, SignalUnit::None, ValidationStatus::Reference,
       kReadOnly, nullptr, 0},
  };
  static constexpr SignalMetadata duplicate_key[] = {
      {SignalId{1}, "a", SignalType::Boolean, SignalUnit::None, ValidationStatus::Reference,
       kReadOnly, nullptr, 0},
      {SignalId{2}, "a", SignalType::Boolean, SignalUnit::None, ValidationStatus::Reference,
       kReadOnly, nullptr, 0},
  };
  static constexpr SignalMetadata zero_id[] = {
      {SignalId{}, "a", SignalType::Boolean, SignalUnit::None, ValidationStatus::Reference,
       kReadOnly, nullptr, 0},
  };
  static constexpr SignalMetadata enum_without_choices[] = {
      {SignalId{1}, "a", SignalType::Enum, SignalUnit::None, ValidationStatus::Reference, kReadOnly,
       nullptr, 0},
  };
  static constexpr SignalEnumChoice duplicate_choices[] = {{0, "x"}, {0, "y"}};
  static constexpr SignalMetadata enum_duplicate_choice[] = {
      {SignalId{1}, "a", SignalType::Enum, SignalUnit::None, ValidationStatus::Reference, kReadOnly,
       duplicate_choices, 2},
  };
  static constexpr SignalMetadata boolean_with_unit[] = {
      {SignalId{1}, "a", SignalType::Boolean, SignalUnit::KilometresPerHour,
       ValidationStatus::Reference, kReadOnly, nullptr, 0},
  };
  static constexpr SignalMetadata no_capabilities[] = {
      {SignalId{1}, "a", SignalType::Boolean, SignalUnit::None, ValidationStatus::Reference,
       SignalCapabilities{}, nullptr, 0},
  };

  CHECK_FALSE(SignalCatalogView{duplicate_id}.well_formed());
  CHECK_FALSE(SignalCatalogView{duplicate_key}.well_formed());
  CHECK_FALSE(SignalCatalogView{zero_id}.well_formed());
  CHECK_FALSE(SignalCatalogView{enum_without_choices}.well_formed());
  CHECK_FALSE(SignalCatalogView{enum_duplicate_choice}.well_formed());
  CHECK_FALSE(SignalCatalogView{boolean_with_unit}.well_formed());
  CHECK_FALSE(SignalCatalogView{no_capabilities}.well_formed());

  // A zero-id entry is never returned, even when present in the array.
  CHECK(SignalCatalogView{zero_id}.find(SignalId{}) == nullptr);
}

TEST_CASE("signal values are tagged and fail safely on type mismatch") {
  const SignalValue flag = SignalValue::boolean(true);
  CHECK(flag.type() == SignalType::Boolean);
  CHECK(flag.as_boolean() == true);
  CHECK_FALSE(flag.as_number().has_value());
  CHECK_FALSE(flag.as_enumeration().has_value());

  const SignalValue rpm = SignalValue::number(2150.5F);
  CHECK(rpm.type() == SignalType::Number);
  CHECK(rpm.as_number() == 2150.5F);
  CHECK_FALSE(rpm.as_boolean().has_value());
  CHECK_FALSE(rpm.as_enumeration().has_value());

  const SignalValue gear = SignalValue::enumeration(3);
  CHECK(gear.type() == SignalType::Enum);
  CHECK(gear.as_enumeration() == std::uint16_t{3});
  CHECK_FALSE(gear.as_boolean().has_value());
  CHECK_FALSE(gear.as_number().has_value());

  CHECK(SignalValue{} == SignalValue::boolean(false));
  CHECK(SignalValue::boolean(true) == SignalValue::boolean(true));
  CHECK(SignalValue::boolean(true) != SignalValue::boolean(false));
  CHECK(SignalValue::number(1.0F) != SignalValue::enumeration(1));
  CHECK(SignalValue::enumeration(0) != SignalValue::boolean(false));
  CHECK(SignalValue::number(0.0F) != SignalValue::boolean(false));
}

TEST_CASE("a valid no-data reading is distinct from an invalid request") {
  const auto no_data = SignalResult<SignalReading>::success(SignalReading{});
  CHECK(no_data.ok());
  CHECK(static_cast<bool>(no_data));
  CHECK(no_data.status == SignalStatus::Ok);
  REQUIRE(no_data.value.has_value());
  CHECK_FALSE(no_data.value->value.has_value());
  CHECK(no_data.value->availability == Availability::NoData);
  CHECK(no_data.value->validation == ValidationStatus::Reference);

  const auto invalid = SignalResult<SignalReading>::failure(SignalStatus::InvalidSignal);
  CHECK_FALSE(invalid.ok());
  CHECK_FALSE(static_cast<bool>(invalid));
  CHECK(invalid.status == SignalStatus::InvalidSignal);
  CHECK_FALSE(invalid.value.has_value());

  const auto unsupported =
      SignalResult<SignalReading>::failure(SignalStatus::UnsupportedCapability);
  CHECK(unsupported.status == SignalStatus::UnsupportedCapability);
  CHECK_FALSE(unsupported.ok());

  // A failure never reports Ok, and a default result is not successful.
  CHECK(SignalResult<SignalReading>::failure(SignalStatus::Ok).status ==
        SignalStatus::InvalidState);
  CHECK_FALSE(SignalResult<SignalReading>{}.ok());
  CHECK_FALSE(SignalResult<SignalReading>{SignalStatus::Ok, std::nullopt}.ok());

  CHECK(SignalStatusResult::success().ok());
  CHECK_FALSE(SignalStatusResult::failure(SignalStatus::InvalidState).ok());
  CHECK(SignalStatusResult::failure(SignalStatus::Ok).status == SignalStatus::InvalidState);
  CHECK_FALSE(SignalStatusResult{}.ok());
}

TEST_CASE("subscription tokens are opaque and default invalid") {
  constexpr SignalSubscription none{};
  CHECK_FALSE(none.valid());
  CHECK(none.provider_bits() == 0);
  CHECK(none == SignalSubscription{});
  CHECK_FALSE(SignalSubscription::from_provider_bits(0).valid());

  constexpr std::uint64_t encoded = 0x0000'00AB'CD12'3401ULL;
  const SignalSubscription token = SignalSubscription::from_provider_bits(encoded);
  CHECK(token.valid());
  CHECK(token.provider_bits() == encoded);
  CHECK(token == SignalSubscription::from_provider_bits(encoded));
  CHECK(token != none);
}

TEST_CASE("callbacks receive the current reading and transition flags") {
  CallbackProbe probe{};
  const SignalCallback callback = &record_notification;

  SignalNotification notification{};
  notification.id = SignalId{3};
  notification.current.value = SignalValue::enumeration(1);
  notification.current.availability = Availability::Fresh;
  notification.current.validation = ValidationStatus::Confirmed;
  notification.recovered = true;
  notification.coalesced = true;
  callback(&probe, notification);

  CHECK(probe.calls == 1);
  CHECK(probe.last.id == SignalId{3});
  CHECK(probe.last.current.value == SignalValue::enumeration(1));
  CHECK(probe.last.current.availability == Availability::Fresh);
  CHECK_FALSE(probe.last.initial);
  CHECK_FALSE(probe.last.became_unavailable);
  CHECK(probe.last.recovered);
  CHECK(probe.last.coalesced);

  const SignalNotification defaults{};
  CHECK_FALSE(defaults.id.valid());
  CHECK_FALSE(defaults.current.value.has_value());
  CHECK(defaults.current.availability == Availability::NoData);
}
