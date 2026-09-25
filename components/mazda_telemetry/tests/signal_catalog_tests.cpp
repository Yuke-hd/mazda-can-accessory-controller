#include "mazda/signal_catalog.hpp"

#include "mazda/definitions.hpp"
#include "mazda/signal_provider.hpp"
#include "mazda/vehicle_telemetry.hpp"
#include "mazda/vehicle_telemetry_service.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <string_view>
#include <tuple>
#include <type_traits>

namespace {

namespace ids = mazda::internal::signal_ids;
namespace candidate = mazda::candidate;
namespace internal = mazda::internal;

using mazda::ActualGear;
using mazda::FrontWiperPosition;
using mazda::SelectorPosition;
using mazda::TurnState;
using mazda::VehicleState;
using mazda::internal::kSignalCatalog;
using mazda::internal::kSignalCatalogSize;
using vehicle_signals::SignalCapabilities;
using vehicle_signals::SignalCapability;
using vehicle_signals::SignalCatalogView;
using vehicle_signals::SignalEnumChoice;
using vehicle_signals::SignalId;
using vehicle_signals::SignalMetadata;
using vehicle_signals::SignalType;
using vehicle_signals::SignalUnit;
using vehicle_signals::ValidationStatus;

int g_failures = 0;

void check(const bool condition, const char *what, const std::string_view detail = {}) {
  if (condition)
    return;
  ++g_failures;
  std::cerr << "FAIL: " << what;
  if (!detail.empty())
    std::cerr << " [" << detail << ']';
  std::cerr << '\n';
}

// ---------------------------------------------------------------------------
// Catalog shape

constexpr std::string_view kLockedKeys[] = {
    "vehicle.engine_rpm",
    "vehicle.speed_kph",
    "vehicle.turn_state",
    "vehicle.hazard_request",
    "vehicle.turn_request.left",
    "vehicle.turn_request.right",
    "vehicle.indicator_lamp.left",
    "vehicle.indicator_lamp.right",
    "vehicle.selector_position",
    "vehicle.actual_gear",
    "vehicle.liftgate_open",
    "vehicle.door.rear_right",
    "vehicle.door.rear_left",
    "vehicle.door.front_left_rhd",
    "vehicle.door.front_right_rhd",
    "vehicle.doors_unlocked",
    "vehicle.wiper.low",
    "vehicle.wiper.front_position",
};

constexpr SignalCatalogView kCatalog{kSignalCatalog};

static_assert(kCatalog.well_formed());
static_assert(kCatalog.size() == 18);
static_assert(kSignalCatalogSize == 18);
static_assert(std::size(kLockedKeys) == kSignalCatalogSize);

constexpr bool ids_nonzero_unique_and_sequential() noexcept {
  for (std::size_t index = 0; index < kCatalog.size(); ++index) {
    const SignalId id = kCatalog.at(index)->id;
    // Ids are assigned 1..18 in locked-key order.
    if (!id.valid() || id.value() != index + 1)
      return false;
    for (std::size_t other = index + 1; other < kCatalog.size(); ++other) {
      if (kCatalog.at(other)->id == id)
        return false;
    }
  }
  return true;
}
static_assert(ids_nonzero_unique_and_sequential());

constexpr bool keys_match_locked_list() noexcept {
  for (std::size_t index = 0; index < kCatalog.size(); ++index) {
    if (kCatalog.at(index)->key != kLockedKeys[index])
      return false;
    for (std::size_t other = index + 1; other < kCatalog.size(); ++other) {
      if (kCatalog.at(other)->key == kCatalog.at(index)->key)
        return false;
    }
  }
  return true;
}
static_assert(keys_match_locked_list());

// No unsuffixed aliases and no out-of-scope signals.
static_assert(kCatalog.find("vehicle.brake_pressed") == nullptr);
static_assert(kCatalog.find("vehicle.door.front_left") == nullptr);
static_assert(kCatalog.find("vehicle.door.front_right") == nullptr);
static_assert(kCatalog.find("vehicle.turn_request") == nullptr);
static_assert(kCatalog.find("vehicle.indicator_lamp") == nullptr);

// Every id constant resolves to the row carrying its locked key.
static_assert(kCatalog.find(ids::kEngineRpm)->key == "vehicle.engine_rpm");
static_assert(kCatalog.find(ids::kSpeedKph)->key == "vehicle.speed_kph");
static_assert(kCatalog.find(ids::kTurnState)->key == "vehicle.turn_state");
static_assert(kCatalog.find(ids::kHazardRequest)->key == "vehicle.hazard_request");
static_assert(kCatalog.find(ids::kTurnRequestLeft)->key == "vehicle.turn_request.left");
static_assert(kCatalog.find(ids::kTurnRequestRight)->key == "vehicle.turn_request.right");
static_assert(kCatalog.find(ids::kIndicatorLampLeft)->key == "vehicle.indicator_lamp.left");
static_assert(kCatalog.find(ids::kIndicatorLampRight)->key == "vehicle.indicator_lamp.right");
static_assert(kCatalog.find(ids::kSelectorPosition)->key == "vehicle.selector_position");
static_assert(kCatalog.find(ids::kActualGear)->key == "vehicle.actual_gear");
static_assert(kCatalog.find(ids::kLiftgateOpen)->key == "vehicle.liftgate_open");
static_assert(kCatalog.find(ids::kDoorRearRight)->key == "vehicle.door.rear_right");
static_assert(kCatalog.find(ids::kDoorRearLeft)->key == "vehicle.door.rear_left");
static_assert(kCatalog.find(ids::kDoorFrontLeftRhd)->key == "vehicle.door.front_left_rhd");
static_assert(kCatalog.find(ids::kDoorFrontRightRhd)->key == "vehicle.door.front_right_rhd");
static_assert(kCatalog.find(ids::kDoorsUnlocked)->key == "vehicle.doors_unlocked");
static_assert(kCatalog.find(ids::kWiperLow)->key == "vehicle.wiper.low");
static_assert(kCatalog.find(ids::kWiperFrontPosition)->key == "vehicle.wiper.front_position");

// RPM and speed are Number/Read only with their engineering units.
constexpr bool polled_numbers_are_read_only() noexcept {
  const SignalMetadata &rpm = *kCatalog.find(ids::kEngineRpm);
  const SignalMetadata &speed = *kCatalog.find(ids::kSpeedKph);
  const SignalCapabilities read_only{SignalCapability::Read};
  return rpm.type == SignalType::Number && rpm.unit == SignalUnit::RevolutionsPerMinute &&
         rpm.capabilities == read_only && speed.type == SignalType::Number &&
         speed.unit == SignalUnit::KilometresPerHour && speed.capabilities == read_only;
}
static_assert(polled_numbers_are_read_only());

constexpr bool others_are_read_notify() noexcept {
  std::size_t count = 0;
  for (const SignalMetadata &entry : kCatalog) {
    if (entry.id == ids::kEngineRpm || entry.id == ids::kSpeedKph)
      continue;
    if (entry.capabilities != (SignalCapability::Read | SignalCapability::Notify) ||
        entry.unit != SignalUnit::None || entry.type == SignalType::Number)
      return false;
    ++count;
  }
  return count == 16;
}
static_assert(others_are_read_notify());

// ---------------------------------------------------------------------------
// well_formed() rejects malformed local catalogs.

constexpr SignalMetadata boolean_row(std::uint16_t id, std::string_view key) noexcept {
  return {SignalId{id},
          key,
          SignalType::Boolean,
          SignalUnit::None,
          ValidationStatus::Reference,
          SignalCapability::Read,
          nullptr,
          0};
}

constexpr SignalMetadata kValidPair[] = {boolean_row(1, "a"), boolean_row(2, "b")};
constexpr SignalMetadata kDuplicateId[] = {boolean_row(1, "a"), boolean_row(1, "b")};
constexpr SignalMetadata kZeroId[] = {boolean_row(1, "a"), boolean_row(0, "b")};
constexpr SignalMetadata kDuplicateKey[] = {boolean_row(1, "a"), boolean_row(2, "a")};

static_assert(SignalCatalogView{kValidPair}.well_formed());
static_assert(!SignalCatalogView{kDuplicateId}.well_formed());
static_assert(!SignalCatalogView{kZeroId}.well_formed());
static_assert(!SignalCatalogView{kDuplicateKey}.well_formed());

// ---------------------------------------------------------------------------
// Enum choices. The switches below are exhaustive without a default and the
// target builds with -Werror=switch, so a new Mazda enumerator must be added
// here (and therefore to the catalog) before the test compiles.

constexpr std::string_view expected_key(const TurnState value) noexcept {
  switch (value) {
  case TurnState::Unknown:
    return "unknown";
  case TurnState::Off:
    return "off";
  case TurnState::Left:
    return "left";
  case TurnState::Right:
    return "right";
  case TurnState::Hazard:
    return "hazard";
  }
  return {};
}

constexpr std::string_view expected_key(const SelectorPosition value) noexcept {
  switch (value) {
  case SelectorPosition::Unknown:
    return "unknown";
  case SelectorPosition::Shifting:
    return "shifting";
  case SelectorPosition::Park:
    return "park";
  case SelectorPosition::Reverse:
    return "reverse";
  case SelectorPosition::Neutral:
    return "neutral";
  case SelectorPosition::Drive:
    return "drive";
  }
  return {};
}

constexpr std::string_view expected_key(const ActualGear value) noexcept {
  switch (value) {
  case ActualGear::Unknown:
    return "unknown";
  case ActualGear::ParkOrNeutral:
    return "park_or_neutral";
  case ActualGear::Park:
    return "park";
  case ActualGear::Neutral:
    return "neutral";
  case ActualGear::Reverse:
    return "reverse";
  case ActualGear::First:
    return "first";
  case ActualGear::Second:
    return "second";
  case ActualGear::Third:
    return "third";
  case ActualGear::Fourth:
    return "fourth";
  case ActualGear::Fifth:
    return "fifth";
  case ActualGear::Sixth:
    return "sixth";
  case ActualGear::Shifting:
    return "shifting";
  }
  return {};
}

constexpr std::string_view expected_key(const FrontWiperPosition value) noexcept {
  switch (value) {
  case FrontWiperPosition::Unknown:
    return "unknown";
  case FrontWiperPosition::Off:
    return "off";
  case FrontWiperPosition::On:
    return "on";
  case FrontWiperPosition::High:
    return "high";
  case FrontWiperPosition::Intermittent:
    return "intermittent";
  }
  return {};
}

template <typename T> constexpr SignalType expected_type() noexcept {
  if constexpr (std::is_same_v<T, bool>)
    return SignalType::Boolean;
  else if constexpr (std::is_same_v<T, float>)
    return SignalType::Number;
  else
    return SignalType::Enum;
}

template <typename T> constexpr const SignalEnumChoice *expected_choices() noexcept {
  if constexpr (std::is_same_v<T, TurnState>)
    return internal::kTurnStateChoices;
  else if constexpr (std::is_same_v<T, SelectorPosition>)
    return internal::kSelectorPositionChoices;
  else if constexpr (std::is_same_v<T, ActualGear>)
    return internal::kActualGearChoices;
  else if constexpr (std::is_same_v<T, FrontWiperPosition>)
    return internal::kFrontWiperPositionChoices;
  else
    return nullptr;
}

// Every raw value of the enum's underlying type that names an enumerator maps
// to exactly one choice with the same value and key; no other choice exists.
template <typename Enum> constexpr bool choices_cover_enum(const SignalMetadata &row) noexcept {
  static_assert(std::is_same_v<std::underlying_type_t<Enum>, std::uint8_t>);
  if (row.type != SignalType::Enum || row.choices != expected_choices<Enum>())
    return false;
  std::size_t enumerators = 0;
  for (unsigned raw = 0; raw <= 0xffU; ++raw) {
    const std::string_view key = expected_key(static_cast<Enum>(raw));
    if (key.empty())
      continue;
    ++enumerators;
    const SignalEnumChoice *choice = row.find_choice(static_cast<std::uint16_t>(raw));
    if (choice == nullptr || choice->key != key || row.find_choice(key) != choice)
      return false;
  }
  return enumerators == row.choice_count;
}

static_assert(choices_cover_enum<TurnState>(*kCatalog.find(ids::kTurnState)));
static_assert(choices_cover_enum<SelectorPosition>(*kCatalog.find(ids::kSelectorPosition)));
static_assert(choices_cover_enum<ActualGear>(*kCatalog.find(ids::kActualGear)));
static_assert(choices_cover_enum<FrontWiperPosition>(*kCatalog.find(ids::kWiperFrontPosition)));

constexpr bool enum_choices_unique() noexcept {
  std::size_t enum_rows = 0;
  for (const SignalMetadata &entry : kCatalog) {
    if (entry.type != SignalType::Enum) {
      if (entry.choices != nullptr || entry.choice_count != 0)
        return false;
      continue;
    }
    ++enum_rows;
    for (std::size_t index = 0; index < entry.choice_count; ++index) {
      for (std::size_t other = index + 1; other < entry.choice_count; ++other) {
        if (entry.choices[index].value == entry.choices[other].value ||
            entry.choices[index].key == entry.choices[other].key)
          return false;
      }
    }
  }
  return enum_rows == 4;
}
static_assert(enum_choices_unique());

// ---------------------------------------------------------------------------
// Descriptor bindings. The expected production source/channel/state member
// for each id is spelled out independently of the descriptor tables.

constexpr std::uint16_t kPolling = 0;

struct ExpectedBinding final {
  SignalId id{};
  std::uint32_t identifier{0};
  std::uint16_t channel{kPolling};
};

constexpr ExpectedBinding kExpectedBindings[] = {
    {ids::kEngineRpm, candidate::kEngineDataId, kPolling},
    {ids::kSpeedKph, candidate::kEngineDataId, kPolling},
    {ids::kTurnState, candidate::kTurnSwitchId, internal::kTurnNotificationChannel},
    {ids::kHazardRequest, candidate::kTurnSwitchId, internal::kHazardNotificationChannel},
    {ids::kTurnRequestLeft, candidate::kTurnSwitchId, internal::kLeftTurnNotificationChannel},
    {ids::kTurnRequestRight, candidate::kTurnSwitchId, internal::kRightTurnNotificationChannel},
    {ids::kIndicatorLampLeft, candidate::kBlinkInfoId, internal::kLeftLampNotificationChannel},
    {ids::kIndicatorLampRight, candidate::kBlinkInfoId, internal::kRightLampNotificationChannel},
    {ids::kSelectorPosition, candidate::kGearId, internal::kSelectorNotificationChannel},
    {ids::kActualGear, candidate::kGearId, internal::kActualGearNotificationChannel},
    {ids::kLiftgateOpen, candidate::kDoorsId, internal::kLiftgateNotificationChannel},
    {ids::kDoorRearRight, candidate::kDoorsId, internal::kRearRightDoorNotificationChannel},
    {ids::kDoorRearLeft, candidate::kDoorsId, internal::kRearLeftDoorNotificationChannel},
    {ids::kDoorFrontLeftRhd, candidate::kDoorsId, internal::kFrontLeftDoorNotificationChannel},
    {ids::kDoorFrontRightRhd, candidate::kDoorsId, internal::kFrontRightDoorNotificationChannel},
    {ids::kDoorsUnlocked, candidate::kDoorsId, internal::kDoorsUnlockedNotificationChannel},
    {ids::kWiperLow, candidate::kBlinkInfoId, internal::kWiperLowNotificationChannel},
    {ids::kWiperFrontPosition, candidate::kTurnSwitchId, internal::kFrontWiperNotificationChannel},
};
static_assert(std::size(kExpectedBindings) == kSignalCatalogSize);

template <typename T> struct ExpectedMember final {
  SignalId id{};
  vehicle_core::Signal<T> VehicleState::*member{nullptr};
};

constexpr ExpectedMember<float> kFloatMembers[] = {
    {ids::kEngineRpm, &VehicleState::engine_rpm},
    {ids::kSpeedKph, &VehicleState::speed_kph},
};
constexpr ExpectedMember<bool> kBoolMembers[] = {
    {ids::kHazardRequest, &VehicleState::hazard_request},
    {ids::kTurnRequestLeft, &VehicleState::left_turn_request},
    {ids::kTurnRequestRight, &VehicleState::right_turn_request},
    {ids::kIndicatorLampLeft, &VehicleState::left_indicator_lamp},
    {ids::kIndicatorLampRight, &VehicleState::right_indicator_lamp},
    {ids::kLiftgateOpen, &VehicleState::liftgate_open},
    {ids::kDoorRearRight, &VehicleState::rear_right_door_open},
    {ids::kDoorRearLeft, &VehicleState::rear_left_door_open},
    {ids::kDoorFrontLeftRhd, &VehicleState::front_left_door_open_rhd},
    {ids::kDoorFrontRightRhd, &VehicleState::front_right_door_open_rhd},
    {ids::kDoorsUnlocked, &VehicleState::doors_unlocked},
    {ids::kWiperLow, &VehicleState::wiper_low},
};
constexpr ExpectedMember<TurnState> kTurnStateMembers[] = {
    {ids::kTurnState, &VehicleState::turn_state},
};
constexpr ExpectedMember<SelectorPosition> kSelectorMembers[] = {
    {ids::kSelectorPosition, &VehicleState::selector_position},
};
constexpr ExpectedMember<ActualGear> kActualGearMembers[] = {
    {ids::kActualGear, &VehicleState::actual_gear},
};
constexpr ExpectedMember<FrontWiperPosition> kFrontWiperMembers[] = {
    {ids::kWiperFrontPosition, &VehicleState::front_wiper},
};

template <typename T, std::size_t N>
vehicle_core::Signal<T> VehicleState::*find_member(const ExpectedMember<T> (&members)[N],
                                                   const SignalId id) noexcept {
  for (const auto &entry : members) {
    if (entry.id == id)
      return entry.member;
  }
  return nullptr;
}

// A member of the wrong value type yields nullptr, so a type mismatch between
// descriptor and expectation also fails the member check.
template <typename T> vehicle_core::Signal<T> VehicleState::*expected_member(const SignalId id) {
  if constexpr (std::is_same_v<T, float>)
    return find_member(kFloatMembers, id);
  else if constexpr (std::is_same_v<T, bool>)
    return find_member(kBoolMembers, id);
  else if constexpr (std::is_same_v<T, TurnState>)
    return find_member(kTurnStateMembers, id);
  else if constexpr (std::is_same_v<T, SelectorPosition>)
    return find_member(kSelectorMembers, id);
  else if constexpr (std::is_same_v<T, ActualGear>)
    return find_member(kActualGearMembers, id);
  else
    return find_member(kFrontWiperMembers, id);
}

const ExpectedBinding *expected_binding(const SignalId id) noexcept {
  for (const auto &binding : kExpectedBindings) {
    if (binding.id == id)
      return &binding;
  }
  return nullptr;
}

struct Tally final {
  std::array<std::size_t, kSignalCatalogSize> matches{};
  std::size_t polling_rows{0};
  std::size_t notification_rows{0};
  std::size_t host_only{0};
};

template <typename T>
void check_binding(Tally &tally, const SignalId id, const char *name,
                   vehicle_core::Signal<T> VehicleState::*member, const std::uint32_t identifier,
                   const ValidationStatus validation, const std::uint16_t channel) {
  const std::string_view label{name == nullptr ? "" : name};
  if (!id.valid()) {
    // Host-only extension descriptors never enter the released catalog.
    ++tally.host_only;
    check(label == "test_front_wiper", "only the host test descriptors have no signal id", label);
    return;
  }

  const SignalMetadata *row = kCatalog.find(id);
  check(row != nullptr, "descriptor id resolves to a catalog row", label);
  if (row == nullptr)
    return;
  const auto index = static_cast<std::size_t>(row - kCatalog.begin());
  ++tally.matches[index];

  const bool polling = channel == kPolling;
  if (polling)
    ++tally.polling_rows;
  else
    ++tally.notification_rows;
  const SignalCapabilities expected_capabilities =
      polling ? SignalCapabilities{SignalCapability::Read}
              : SignalCapability::Read | SignalCapability::Notify;
  check(row->capabilities == expected_capabilities,
        "Read only <=> polling descriptor, Read|Notify <=> notification descriptor", row->key);
  check(row->type == expected_type<T>(), "catalog type matches descriptor value type", row->key);
  check(row->choices == expected_choices<T>(), "enum choices match descriptor enum", row->key);
  check(row->validation == validation, "catalog validation equals descriptor validation", row->key);

  const ExpectedBinding *binding = expected_binding(id);
  check(binding != nullptr && binding->identifier == identifier,
        "descriptor binds the expected message identifier", row->key);
  check(binding != nullptr && binding->channel == channel,
        "descriptor binds the expected notification channel (or polling)", row->key);
  const auto expected = expected_member<T>(id);
  check(expected != nullptr && expected == member, "descriptor binds the expected state member",
        row->key);
}

template <typename T>
void check_descriptor(Tally &tally, const internal::PollingDescriptor<T> &descriptor) {
  check_binding<T>(tally, descriptor.id, descriptor.name, descriptor.signal, descriptor.identifier,
                   descriptor.validation, kPolling);
}

template <typename T, std::uint16_t ChannelId>
void check_descriptor(Tally &tally,
                      const internal::NotificationDescriptor<T, ChannelId> &descriptor) {
  static_assert(ChannelId != kPolling);
  check(descriptor.channel != nullptr, "notification descriptor binds a channel member");
  check_binding<T>(tally, descriptor.id, descriptor.name, descriptor.signal, descriptor.identifier,
                   descriptor.validation, ChannelId);
}

void check_descriptor_bindings() {
  const auto &polling = internal::VehicleTelemetryService::polling_descriptors();
  const auto &notifications = internal::VehicleTelemetryService::notification_descriptors();

  Tally tally{};
  const auto visit = [&tally](const auto &...descriptor) {
    (check_descriptor(tally, descriptor), ...);
  };
  std::apply(visit, polling);
  std::apply(visit, notifications);

  for (std::size_t index = 0; index < kSignalCatalogSize; ++index) {
    check(tally.matches[index] == 1, "catalog row matches exactly one production descriptor",
          kCatalog.at(index)->key);
  }
  check(tally.polling_rows == 2, "two catalog rows are bound to polling descriptors");
  check(tally.notification_rows == 16, "sixteen catalog rows are bound to notification channels");

  // The host build carries one test polling descriptor and test channel 17.
  check(tally.host_only == 2, "host-only descriptors are present and excluded");
  check(!std::get<2>(polling).id.valid(), "host test polling descriptor has no signal id");
  using TestDescriptor = std::tuple_element_t<16, internal::NotificationDescriptorTuple>;
  static_assert(TestDescriptor::Channel::channel_id() ==
                internal::kTestFrontWiperNotificationChannel);
  check(!std::get<16>(notifications).id.valid(), "host test channel descriptor has no signal id");
}

// Explicit separation checks for the easily confused signals.
void check_separated_bindings() {
  const auto &notifications = internal::VehicleTelemetryService::notification_descriptors();
  const auto &turn = std::get<2>(notifications);
  const auto &left_request = std::get<4>(notifications);
  const auto &right_request = std::get<5>(notifications);
  const auto &left_lamp = std::get<12>(notifications);
  const auto &right_lamp = std::get<13>(notifications);
  check(turn.id == ids::kTurnState && turn.signal == &VehicleState::turn_state,
        "turn_state binds the semantic turn state");
  check(left_request.id == ids::kTurnRequestLeft &&
            left_request.signal == &VehicleState::left_turn_request,
        "turn_request.left binds the left turn request");
  check(right_request.id == ids::kTurnRequestRight &&
            right_request.signal == &VehicleState::right_turn_request,
        "turn_request.right binds the right turn request");
  check(left_lamp.id == ids::kIndicatorLampLeft &&
            left_lamp.signal == &VehicleState::left_indicator_lamp,
        "indicator_lamp.left binds the left lamp, not the request");
  check(right_lamp.id == ids::kIndicatorLampRight &&
            right_lamp.signal == &VehicleState::right_indicator_lamp,
        "indicator_lamp.right binds the right lamp, not the request");
}

// ---------------------------------------------------------------------------
// Provider declaration

using mazda::MazdaSignalProvider;
static_assert(!std::is_copy_constructible_v<MazdaSignalProvider>);
static_assert(!std::is_move_constructible_v<MazdaSignalProvider>);
static_assert(!std::is_default_constructible_v<MazdaSignalProvider>);
static_assert(std::is_nothrow_constructible_v<MazdaSignalProvider, mazda::VehicleTelemetry &>);
static_assert(std::is_same_v<decltype(&MazdaSignalProvider::catalog),
                             SignalCatalogView (MazdaSignalProvider::*)() const noexcept>);
static_assert(std::is_same_v<decltype(&MazdaSignalProvider::read),
                             vehicle_signals::SignalResult<vehicle_signals::SignalReading> (
                                 MazdaSignalProvider::*)(SignalId) const noexcept>);
static_assert(std::is_same_v<decltype(&MazdaSignalProvider::subscribe),
                             vehicle_signals::SignalResult<vehicle_signals::SignalSubscription> (
                                 MazdaSignalProvider::*)(SignalId, vehicle_signals::SignalCallback,
                                                         void *) noexcept>);
static_assert(std::is_same_v<decltype(&MazdaSignalProvider::unsubscribe),
                             vehicle_signals::SignalStatusResult (MazdaSignalProvider::*)(
                                 vehicle_signals::SignalSubscription) noexcept>);

void check_provider_catalog() {
  mazda::VehicleTelemetry telemetry;
  const MazdaSignalProvider provider{telemetry};
  const SignalCatalogView view = provider.catalog();
  check(view.begin() == kCatalog.begin() && view.size() == kCatalog.size(),
        "provider catalog() returns the static catalog view");
  check(view.well_formed(), "provider catalog is well formed");
  const SignalMetadata *turn = view.find("vehicle.turn_state");
  check(turn != nullptr && turn->id == ids::kTurnState, "provider catalog key lookup");
}

} // namespace

int main() {
  check_descriptor_bindings();
  check_separated_bindings();
  check_provider_catalog();
  if (g_failures != 0) {
    std::cerr << g_failures << " signal catalog check(s) failed\n";
    return 1;
  }
  return 0;
}
