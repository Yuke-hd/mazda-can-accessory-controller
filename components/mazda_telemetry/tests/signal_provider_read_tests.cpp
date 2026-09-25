#include "mazda/signal_provider.hpp"

#include "mazda/definitions.hpp"
#include "mazda/signal_catalog.hpp"
#include "mazda/signal_value_conversion.hpp"
#include "mazda/vehicle_telemetry.hpp"
#include "mazda/vehicle_telemetry_internal.hpp"
#include "mazda/vehicle_telemetry_service.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <mutex>
#include <optional>
#include <thread>
#include <tuple>
#include <type_traits>

namespace {

namespace ids = mazda::internal::signal_ids;
namespace candidate = mazda::candidate;
namespace internal = mazda::internal;

using mazda::ActualGear;
using mazda::Availability;
using mazda::FrontWiperPosition;
using mazda::SelectorPosition;
using mazda::TurnState;
using mazda::ValidationStatus;
using vehicle_signals::SignalCapability;
using vehicle_signals::SignalCatalogView;
using vehicle_signals::SignalId;
using vehicle_signals::SignalMetadata;
using vehicle_signals::SignalNotification;
using vehicle_signals::SignalReading;
using vehicle_signals::SignalStatus;
using vehicle_signals::SignalType;
using vehicle_signals::SignalUnit;
using vehicle_signals::SignalValue;

int failures = 0;

void expect(const bool condition, const char *expression, const char *file, const int line) {
  if (!condition) {
    std::cerr << file << ':' << line << ": failed: " << expression << '\n';
    ++failures;
  }
}

#define EXPECT(condition) expect((condition), #condition, __FILE__, __LINE__)

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

vehicle_core::RawCanFrame frame(const std::uint32_t identifier,
                                const vehicle_core::MonotonicTimestamp timestamp_us,
                                std::initializer_list<std::uint8_t> bytes) {
  vehicle_core::RawCanFrame result{};
  result.identifier = identifier;
  result.timestamp_us = timestamp_us;
  result.dlc = static_cast<std::uint8_t>(bytes.size());
  std::size_t index = 0;
  for (const auto byte : bytes)
    result.data[index++] = byte;
  return result;
}

// Byte patterns reuse the decoder fixtures in tests/host and the service
// tests; they are generated vectors, not captures.
// ENGINE_DATA: 0x095b is 598.75 rpm; speed bytes are zero.
vehicle_core::RawCanFrame engine_frame(const vehicle_core::MonotonicTimestamp timestamp_us) {
  return frame(candidate::kEngineDataId, timestamp_us, {0x09, 0x5b, 0, 0, 0, 0, 0, 0});
}

// TRANSMISSION: selector Drive, actual gear Second.
vehicle_core::RawCanFrame gear_frame(const vehicle_core::MonotonicTimestamp timestamp_us) {
  return frame(candidate::kGearId, timestamp_us, {0x24, 0x81, 0x07, 0xff, 0x04, 0xf0, 0, 0});
}

// TURN_SWITCH: D2 bit 5 is the left request (TurnState::Left), D3 bits 4-5
// are the front wiper (1 = On).
vehicle_core::RawCanFrame turn_frame(const vehicle_core::MonotonicTimestamp timestamp_us) {
  return frame(candidate::kTurnSwitchId, timestamp_us, {0, 0x20, 0x10, 0, 0, 0, 0, 0});
}

// BLINK_INFO: D3 bit 3 is the right lamp only; D5 bit 1 is WiperLow. The left
// lamp stays off while the separate left request above is on.
vehicle_core::RawCanFrame blink_frame(const vehicle_core::MonotonicTimestamp timestamp_us) {
  return frame(candidate::kBlinkInfoId, timestamp_us, {0, 0, 0x08, 0, 0x02, 0, 0, 0});
}

// DOORS: D4 bit 6 unlocked; D5 bits 0, 2, 3, 4, 5 liftgate and doors open.
vehicle_core::RawCanFrame doors_frame(const vehicle_core::MonotonicTimestamp timestamp_us) {
  return frame(candidate::kDoorsId, timestamp_us, {0, 0, 0, 0x40, 0x3d, 0, 0, 0});
}

// A facade whose private service is bound to the injected clock and source,
// so frames run through the production runtime, decoder and publication.
// Members are declared so the facade is destroyed before its injected seams.
struct Harness final {
  explicit Harness(const mazda::TelemetryConfig &config = test_config()) noexcept {
    internal::VehicleTelemetryAccess::emplace_host_service(telemetry, clock, source, lighting,
                                                           config);
  }
  ~Harness() {
    if (telemetry.diagnostics().lifecycle != mazda::LifecycleState::Stopped)
      (void)telemetry.stop();
  }

  Harness(const Harness &) = delete;
  Harness &operator=(const Harness &) = delete;

  static mazda::TelemetryConfig test_config() noexcept {
    mazda::TelemetryConfig config{};
    config.callback_stop_timeout_us = 20'000;
    return config;
  }

  void inject(const vehicle_core::RawCanFrame &value) noexcept {
    EXPECT(source.inject(value) == mazda::ResultCode::Ok);
  }

  FakeClock clock{};
  internal::HostAcquisitionSource source{};
  internal::NullLightingSink lighting{};
  mazda::VehicleTelemetry telemetry{};
  mazda::MazdaSignalProvider provider{telemetry};
};

template <typename Predicate>
bool wait_for(Predicate predicate,
              const std::chrono::milliseconds timeout = std::chrono::milliseconds{500}) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  return predicate();
}

bool wait_for_read(const mazda::MazdaSignalProvider &provider, const SignalId id,
                   const Availability availability) {
  return wait_for([&provider, id, availability] {
    const auto result = provider.read(id);
    return result.ok() && result.value->availability == availability;
  });
}

bool wait_for_value(const mazda::MazdaSignalProvider &provider, const SignalId id) {
  return wait_for([&provider, id] {
    const auto result = provider.read(id);
    return result.ok() && result.value->value.has_value();
  });
}

const SignalMetadata &row(const SignalId id) { return *internal::signal_catalog().find(id); }

// Independent typed/generic comparison: does not use the converter under test.
bool same_value(const SignalValue &generic, const bool typed) {
  return generic.as_boolean() == std::optional<bool>{typed};
}
bool same_value(const SignalValue &generic, const float typed) {
  return generic.as_number() == std::optional<float>{typed};
}
template <typename Enum, std::enable_if_t<std::is_enum_v<Enum>, int> = 0>
bool same_value(const SignalValue &generic, const Enum typed) {
  return generic.as_enumeration() ==
         std::optional<std::uint16_t>{static_cast<std::uint16_t>(typed)};
}

template <typename T>
bool same_reading(const SignalReading &generic, const mazda::Reading<T> &typed) {
  if (generic.value.has_value() != typed.value.has_value())
    return false;
  if (typed.value.has_value() && !same_value(*generic.value, *typed.value))
    return false;
  return generic.availability == typed.availability && generic.validation == typed.validation;
}

template <typename T> struct NoticeRecorder final {
  mutable std::mutex mutex{};
  mazda::Notification<T> latest{};
  std::size_t count{0};
};

template <typename T>
void record_notice(void *context, const mazda::Notification<T> &notice) noexcept {
  auto &recorder = *static_cast<NoticeRecorder<T> *>(context);
  std::lock_guard<std::mutex> lock{recorder.mutex};
  recorder.latest = notice;
  ++recorder.count;
}

template <typename T> mazda::Notification<T> latest_notice(const NoticeRecorder<T> &recorder) {
  std::lock_guard<std::mutex> lock{recorder.mutex};
  return recorder.latest;
}

template <typename T>
bool wait_for_notice_value(const NoticeRecorder<T> &recorder, const T expected) {
  return wait_for([&recorder, expected] {
    const auto notice = latest_notice(recorder);
    return notice.current.value.has_value() && *notice.current.value == expected;
  });
}

constexpr SignalId kAllIds[] = {
    ids::kEngineRpm,         ids::kSpeedKph,           ids::kTurnState,
    ids::kHazardRequest,     ids::kTurnRequestLeft,    ids::kTurnRequestRight,
    ids::kIndicatorLampLeft, ids::kIndicatorLampRight, ids::kSelectorPosition,
    ids::kActualGear,        ids::kLiftgateOpen,       ids::kDoorRearRight,
    ids::kDoorRearLeft,      ids::kDoorFrontLeftRhd,   ids::kDoorFrontRightRhd,
    ids::kDoorsUnlocked,     ids::kWiperLow,           ids::kWiperFrontPosition,
};
static_assert(std::size(kAllIds) == internal::kSignalCatalogSize);

// ---------------------------------------------------------------------------
// Conversion helpers

void test_value_and_type_conversion() {
  static_assert(internal::signal_type_of<bool>() == SignalType::Boolean);
  static_assert(internal::signal_type_of<float>() == SignalType::Number);
  static_assert(internal::signal_type_of<TurnState>() == SignalType::Enum);
  static_assert(internal::signal_type_of<FrontWiperPosition>() == SignalType::Enum);

  static_assert(internal::to_signal_value(true) == SignalValue::boolean(true));
  static_assert(internal::to_signal_value(false) == SignalValue::boolean(false));
  static_assert(internal::to_signal_value(598.75F) == SignalValue::number(598.75F));
  static_assert(internal::to_signal_value(TurnState::Hazard) ==
                SignalValue::enumeration(static_cast<std::uint16_t>(TurnState::Hazard)));
  static_assert(internal::to_signal_value(ActualGear::Shifting) ==
                SignalValue::enumeration(static_cast<std::uint16_t>(ActualGear::Shifting)));
  static_assert(internal::to_signal_value(SelectorPosition::Unknown) ==
                SignalValue::enumeration(static_cast<std::uint16_t>(SelectorPosition::Unknown)));

  // A converted enum value always names a catalog choice.
  EXPECT(row(ids::kTurnState).accepts(internal::to_signal_value(TurnState::Right)));
  EXPECT(row(ids::kWiperFrontPosition)
             .accepts(internal::to_signal_value(FrontWiperPosition::Intermittent)));
  // Numbers are not rescaled or rounded.
  EXPECT(internal::to_signal_value(655.35F).as_number() == std::optional<float>{655.35F});
}

void test_reading_conversion_preserves_fields() {
  // has_value, availability and validation are independent and copied as-is.
  mazda::Reading<float> no_value{};
  no_value.availability = Availability::Stale;
  no_value.validation = ValidationStatus::Confirmed;
  const auto converted_no_value = internal::to_signal_reading(no_value);
  EXPECT(!converted_no_value.value.has_value());
  EXPECT(converted_no_value.availability == Availability::Stale);
  EXPECT(converted_no_value.validation == ValidationStatus::Confirmed);

  mazda::Reading<bool> unavailable{};
  unavailable.value = true;
  unavailable.availability = Availability::Unavailable;
  unavailable.validation = ValidationStatus::Observed;
  const auto converted_unavailable = internal::to_signal_reading(unavailable);
  EXPECT(converted_unavailable.value == std::optional<SignalValue>{SignalValue::boolean(true)});
  EXPECT(converted_unavailable.availability == Availability::Unavailable);
  EXPECT(converted_unavailable.validation == ValidationStatus::Observed);

  mazda::Reading<SelectorPosition> fresh{};
  fresh.value = SelectorPosition::Reverse;
  fresh.availability = Availability::FreshnessUnverified;
  const auto converted_fresh = internal::to_signal_reading(fresh);
  EXPECT(converted_fresh.value == std::optional<SignalValue>{SignalValue::enumeration(
                                      static_cast<std::uint16_t>(SelectorPosition::Reverse))});
  EXPECT(converted_fresh.availability == Availability::FreshnessUnverified);
  EXPECT(converted_fresh.validation == ValidationStatus::Reference);

  const auto converted_default = internal::to_signal_reading(mazda::Reading<TurnState>{});
  EXPECT(!converted_default.value.has_value());
  EXPECT(converted_default.availability == Availability::NoData);
}

void test_notification_conversion_copies_exact_flags() {
  mazda::Notification<TurnState> typed{};
  typed.current.value = TurnState::Left;
  typed.current.availability = Availability::Fresh;
  typed.current.validation = ValidationStatus::Observed;

  const auto none = internal::to_signal_notification(ids::kTurnState, typed);
  EXPECT(none.id == ids::kTurnState);
  EXPECT(none.current.value == std::optional<SignalValue>{SignalValue::enumeration(
                                   static_cast<std::uint16_t>(TurnState::Left))});
  EXPECT(none.current.availability == Availability::Fresh);
  EXPECT(none.current.validation == ValidationStatus::Observed);
  EXPECT(!none.initial && !none.became_unavailable && !none.recovered && !none.coalesced);

  // Each flag is copied on its own; no flag leaks into another.
  bool mazda::Notification<TurnState>::*const typed_flags[] = {
      &mazda::Notification<TurnState>::initial,
      &mazda::Notification<TurnState>::became_unavailable,
      &mazda::Notification<TurnState>::recovered,
      &mazda::Notification<TurnState>::coalesced,
  };
  bool SignalNotification::*const generic_flags[] = {
      &SignalNotification::initial,
      &SignalNotification::became_unavailable,
      &SignalNotification::recovered,
      &SignalNotification::coalesced,
  };
  for (std::size_t set = 0; set < std::size(typed_flags); ++set) {
    auto single = typed;
    single.*typed_flags[set] = true;
    const auto generic = internal::to_signal_notification(ids::kTurnState, single);
    for (std::size_t flag = 0; flag < std::size(generic_flags); ++flag)
      EXPECT(generic.*generic_flags[flag] == (flag == set));
  }

  mazda::Notification<bool> all{};
  all.initial = true;
  all.became_unavailable = true;
  all.recovered = true;
  all.coalesced = true;
  const auto generic_all = internal::to_signal_notification(ids::kHazardRequest, all);
  EXPECT(generic_all.id == ids::kHazardRequest);
  EXPECT(generic_all.initial && generic_all.became_unavailable && generic_all.recovered &&
         generic_all.coalesced);
  EXPECT(!generic_all.current.value.has_value());
  EXPECT(generic_all.current.availability == Availability::NoData);
}

void test_unit_mapping_and_descriptor_types_match_catalog() {
  static_assert(internal::to_signal_unit(vehicle_core::SignalUnit::None) == SignalUnit::None);
  static_assert(internal::to_signal_unit(vehicle_core::SignalUnit::Boolean) == SignalUnit::None);
  static_assert(internal::to_signal_unit(vehicle_core::SignalUnit::KilometresPerHour) ==
                SignalUnit::KilometresPerHour);
  static_assert(internal::to_signal_unit(vehicle_core::SignalUnit::RevolutionsPerMinute) ==
                SignalUnit::RevolutionsPerMinute);

  // Every production descriptor's Mazda unit and value type map to the unit
  // and type its catalog row declares.
  const mazda::VehicleState state{};
  std::size_t bound = 0;
  const auto check = [&state, &bound](const auto &descriptor) {
    if (!descriptor.id.valid())
      return;
    ++bound;
    const auto &signal = state.*descriptor.signal;
    using Value = std::decay_t<decltype(signal.value)>;
    EXPECT(row(descriptor.id).unit == internal::to_signal_unit(signal.unit));
    EXPECT(row(descriptor.id).type == internal::signal_type_of<Value>());
  };
  std::apply([&check](const auto &...descriptor) { (check(descriptor), ...); },
             internal::VehicleTelemetryService::polling_descriptors());
  std::apply([&check](const auto &...descriptor) { (check(descriptor), ...); },
             internal::VehicleTelemetryService::notification_descriptors());
  EXPECT(bound == internal::kSignalCatalogSize);
}

// ---------------------------------------------------------------------------
// Request status

void test_invalid_and_unknown_ids_are_request_failures() {
  Harness harness{};
  const auto invalid = harness.provider.read(SignalId{});
  EXPECT(invalid.status == SignalStatus::InvalidSignal);
  EXPECT(!invalid.value.has_value());
  EXPECT(!invalid.ok());
  const auto unknown = harness.provider.read(SignalId{99});
  EXPECT(unknown.status == SignalStatus::InvalidSignal);
  EXPECT(!unknown.value.has_value());
  const auto beyond = harness.provider.read(SignalId{19});
  EXPECT(beyond.status == SignalStatus::InvalidSignal);
}

void test_catalog_capability_lookup() {
  // read() cannot report UnsupportedCapability through the released catalog:
  // every one of its rows is Read-capable. The shared lookup is therefore
  // exercised against a local catalog with a Notify-only row.
  for (const auto &entry : internal::signal_catalog())
    EXPECT(entry.capabilities.has(SignalCapability::Read));

  static constexpr SignalMetadata kRows[] = {
      {SignalId{1}, "read_only", SignalType::Boolean, SignalUnit::None, ValidationStatus::Reference,
       SignalCapability::Read, nullptr, 0},
      {SignalId{2}, "notify_only", SignalType::Boolean, SignalUnit::None,
       ValidationStatus::Reference, SignalCapability::Notify, nullptr, 0},
  };
  constexpr SignalCatalogView catalog{kRows};
  static_assert(catalog.well_formed());

  const auto read_only =
      internal::find_catalog_signal(catalog, SignalId{1}, SignalCapability::Read);
  EXPECT(read_only.ok());
  EXPECT(*read_only.value == &kRows[0]);
  const auto notify_only =
      internal::find_catalog_signal(catalog, SignalId{2}, SignalCapability::Read);
  EXPECT(notify_only.status == SignalStatus::UnsupportedCapability);
  EXPECT(!notify_only.value.has_value());
  EXPECT(internal::find_catalog_signal(catalog, SignalId{1}, SignalCapability::Notify).status ==
         SignalStatus::UnsupportedCapability);
  EXPECT(internal::find_catalog_signal(catalog, SignalId{2}, SignalCapability::Notify).ok());
  EXPECT(internal::find_catalog_signal(catalog, SignalId{}, SignalCapability::Read).status ==
         SignalStatus::InvalidSignal);
  EXPECT(internal::find_catalog_signal(catalog, SignalId{3}, SignalCapability::Read).status ==
         SignalStatus::InvalidSignal);

  // Production: RPM is Read-only, so a Notify lookup is unsupported.
  EXPECT(internal::find_catalog_signal(internal::signal_catalog(), ids::kEngineRpm,
                                       SignalCapability::Notify)
             .status == SignalStatus::UnsupportedCapability);
}

// ---------------------------------------------------------------------------
// Provider reads through the production path

void expect_no_value_for_all(const mazda::MazdaSignalProvider &provider,
                             const Availability availability) {
  for (const auto id : kAllIds) {
    const auto result = provider.read(id);
    EXPECT(result.status == SignalStatus::Ok);
    EXPECT(result.ok());
    if (!result.ok())
      continue;
    EXPECT(!result.value->value.has_value());
    EXPECT(result.value->availability == availability);
    EXPECT(result.value->validation == row(id).validation);
  }
}

void expect_no_data_for_all(const mazda::MazdaSignalProvider &provider) {
  expect_no_value_for_all(provider, Availability::NoData);
}

void test_all_ids_read_no_data_then_typed_values() {
  Harness harness{};
  // A stopped facade publishes a Stopped transport, so typed polling and
  // generic reads both report Unavailable without a value; still a success.
  EXPECT(harness.telemetry.engine_rpm().availability == Availability::Unavailable);
  expect_no_value_for_all(harness.provider, Availability::Unavailable);
  // Running before the first frame, every id reads successfully as NoData: a
  // valid NoData reading is not a request failure.
  EXPECT(harness.telemetry.start().ok());
  expect_no_data_for_all(harness.provider);

  harness.clock.set(1'000);
  harness.inject(engine_frame(1'000));
  harness.inject(gear_frame(1'000));
  harness.inject(turn_frame(1'000));
  harness.inject(blink_frame(1'000));
  harness.inject(doors_frame(1'000));
  EXPECT(wait_for([&harness] {
    for (const auto id : kAllIds) {
      const auto result = harness.provider.read(id);
      if (!result.ok() || !result.value->value.has_value())
        return false;
    }
    return true;
  }));

  for (const auto id : kAllIds) {
    const auto result = harness.provider.read(id);
    EXPECT(result.ok());
    if (!result.ok() || !result.value->value.has_value())
      continue;
    const auto &metadata = row(id);
    EXPECT(result.value->value->type() == metadata.type);
    EXPECT(metadata.accepts(*result.value->value));
    EXPECT(result.value->validation == metadata.validation);
    // Only turn/request signals have a configured freshness timeout.
    const bool freshness_configured = id == ids::kTurnState || id == ids::kHazardRequest ||
                                      id == ids::kTurnRequestLeft || id == ids::kTurnRequestRight;
    EXPECT(result.value->availability ==
           (freshness_configured ? Availability::Fresh : Availability::FreshnessUnverified));
  }

  const auto value_of = [&harness](const SignalId id) {
    return *harness.provider.read(id).value->value;
  };
  EXPECT(value_of(ids::kEngineRpm) == SignalValue::number(598.75F));
  EXPECT(value_of(ids::kSpeedKph) == SignalValue::number(0.0F));
  EXPECT(value_of(ids::kTurnState) ==
         SignalValue::enumeration(static_cast<std::uint16_t>(TurnState::Left)));
  EXPECT(value_of(ids::kHazardRequest) == SignalValue::boolean(false));
  EXPECT(value_of(ids::kTurnRequestLeft) == SignalValue::boolean(true));
  EXPECT(value_of(ids::kTurnRequestRight) == SignalValue::boolean(false));
  EXPECT(value_of(ids::kIndicatorLampLeft) == SignalValue::boolean(false));
  EXPECT(value_of(ids::kIndicatorLampRight) == SignalValue::boolean(true));
  EXPECT(value_of(ids::kSelectorPosition) ==
         SignalValue::enumeration(static_cast<std::uint16_t>(SelectorPosition::Drive)));
  EXPECT(value_of(ids::kActualGear) ==
         SignalValue::enumeration(static_cast<std::uint16_t>(ActualGear::Second)));
  EXPECT(value_of(ids::kLiftgateOpen) == SignalValue::boolean(true));
  EXPECT(value_of(ids::kDoorRearRight) == SignalValue::boolean(true));
  EXPECT(value_of(ids::kDoorRearLeft) == SignalValue::boolean(true));
  EXPECT(value_of(ids::kDoorFrontLeftRhd) == SignalValue::boolean(true));
  EXPECT(value_of(ids::kDoorFrontRightRhd) == SignalValue::boolean(true));
  EXPECT(value_of(ids::kDoorsUnlocked) == SignalValue::boolean(true));
  EXPECT(value_of(ids::kWiperLow) == SignalValue::boolean(true));
  EXPECT(value_of(ids::kWiperFrontPosition) ==
         SignalValue::enumeration(static_cast<std::uint16_t>(FrontWiperPosition::On)));
  EXPECT(harness.telemetry.stop().ok());
}

void test_rpm_and_speed_match_typed_polling() {
  Harness harness{};
  EXPECT(harness.telemetry.start().ok());
  // Same NoData state before the first frame.
  EXPECT(
      same_reading(*harness.provider.read(ids::kEngineRpm).value, harness.telemetry.engine_rpm()));
  EXPECT(same_reading(*harness.provider.read(ids::kSpeedKph).value, harness.telemetry.speed_kph()));

  harness.clock.set(500);
  harness.inject(engine_frame(500));
  EXPECT(wait_for_value(harness.provider, ids::kEngineRpm));
  EXPECT(wait_for_value(harness.provider, ids::kSpeedKph));

  // The clock is held, so the generic and typed reads evaluate one
  // publication at the same instant.
  const auto rpm = harness.provider.read(ids::kEngineRpm);
  const auto typed_rpm = harness.telemetry.engine_rpm();
  EXPECT(rpm.ok());
  EXPECT(typed_rpm.value.has_value());
  EXPECT(same_reading(*rpm.value, typed_rpm));
  EXPECT(rpm.value->value == std::optional<SignalValue>{SignalValue::number(598.75F)});
  EXPECT(rpm.value->validation == ValidationStatus::Confirmed);

  const auto speed = harness.provider.read(ids::kSpeedKph);
  const auto typed_speed = harness.telemetry.speed_kph();
  EXPECT(speed.ok());
  EXPECT(typed_speed.value.has_value());
  EXPECT(same_reading(*speed.value, typed_speed));
  EXPECT(speed.value->validation == ValidationStatus::Reference);

  // Parity holds as the same publication is evaluated at a later instant.
  harness.clock.set(900'000);
  EXPECT(
      same_reading(*harness.provider.read(ids::kEngineRpm).value, harness.telemetry.engine_rpm()));
  EXPECT(same_reading(*harness.provider.read(ids::kSpeedKph).value, harness.telemetry.speed_kph()));
  EXPECT(harness.telemetry.stop().ok());
}

void test_discrete_reads_match_typed_notification_current() {
  Harness harness{};
  NoticeRecorder<TurnState> turn{};
  NoticeRecorder<bool> left_request{};
  NoticeRecorder<bool> left_lamp{};
  NoticeRecorder<bool> right_lamp{};
  NoticeRecorder<SelectorPosition> selector{};
  NoticeRecorder<FrontWiperPosition> front_wiper{};
  EXPECT(harness.telemetry.on_turn_state_changed(&record_notice<TurnState>, &turn).ok());
  EXPECT(harness.telemetry.on_left_turn_request_changed(&record_notice<bool>, &left_request).ok());
  EXPECT(harness.telemetry.on_left_indicator_lamp_changed(&record_notice<bool>, &left_lamp).ok());
  EXPECT(harness.telemetry.on_right_indicator_lamp_changed(&record_notice<bool>, &right_lamp).ok());
  EXPECT(harness.telemetry.on_selector_position_changed(&record_notice<SelectorPosition>, &selector)
             .ok());
  EXPECT(harness.telemetry.on_front_wiper_changed(&record_notice<FrontWiperPosition>, &front_wiper)
             .ok());
  EXPECT(harness.telemetry.start().ok());

  // Hold the clock for the whole exchange: each typed notice's current and
  // the later generic read evaluate the same publication at the same instant.
  harness.clock.set(2'000);
  harness.inject(turn_frame(2'000));
  harness.inject(blink_frame(2'000));
  harness.inject(gear_frame(2'000));
  EXPECT(wait_for_notice_value(turn, TurnState::Left));
  EXPECT(wait_for_notice_value(left_request, true));
  EXPECT(wait_for_notice_value(left_lamp, false));
  EXPECT(wait_for_notice_value(right_lamp, true));
  EXPECT(wait_for_notice_value(selector, SelectorPosition::Drive));
  EXPECT(wait_for_notice_value(front_wiper, FrontWiperPosition::On));

  EXPECT(same_reading(*harness.provider.read(ids::kTurnState).value, latest_notice(turn).current));
  EXPECT(same_reading(*harness.provider.read(ids::kTurnRequestLeft).value,
                      latest_notice(left_request).current));
  EXPECT(same_reading(*harness.provider.read(ids::kIndicatorLampLeft).value,
                      latest_notice(left_lamp).current));
  EXPECT(same_reading(*harness.provider.read(ids::kIndicatorLampRight).value,
                      latest_notice(right_lamp).current));
  EXPECT(same_reading(*harness.provider.read(ids::kSelectorPosition).value,
                      latest_notice(selector).current));
  EXPECT(same_reading(*harness.provider.read(ids::kWiperFrontPosition).value,
                      latest_notice(front_wiper).current));

  // Request and lamp come from distinct frames and stay separate.
  EXPECT(harness.provider.read(ids::kTurnRequestLeft).value->value ==
         std::optional<SignalValue>{SignalValue::boolean(true)});
  EXPECT(harness.provider.read(ids::kIndicatorLampLeft).value->value ==
         std::optional<SignalValue>{SignalValue::boolean(false)});
  EXPECT(harness.telemetry.stop().ok());
}

void test_clock_advance_past_freshness_timeout_is_stale() {
  Harness harness{};
  EXPECT(harness.telemetry.start().ok());
  harness.clock.set(1'000);
  harness.inject(turn_frame(1'000));
  harness.inject(doors_frame(1'000));
  EXPECT(wait_for_read(harness.provider, ids::kTurnState, Availability::Fresh));
  EXPECT(wait_for_read(harness.provider, ids::kTurnRequestLeft, Availability::Fresh));
  EXPECT(wait_for_value(harness.provider, ids::kLiftgateOpen));

  // Turn/request freshness is 250 ms. At the boundary the value is still
  // fresh; one microsecond later it is stale but keeps its value. Transport
  // silence (1 s by default) has not elapsed.
  harness.clock.set(1'000 + mazda::kTurnFreshnessTimeoutUs);
  EXPECT(harness.provider.read(ids::kTurnState).value->availability == Availability::Fresh);
  harness.clock.set(1'000 + mazda::kTurnFreshnessTimeoutUs + 1);
  const auto turn = harness.provider.read(ids::kTurnState);
  EXPECT(turn.ok());
  EXPECT(turn.value->availability == Availability::Stale);
  EXPECT(turn.value->value == std::optional<SignalValue>{SignalValue::enumeration(
                                  static_cast<std::uint16_t>(TurnState::Left))});
  const auto request = harness.provider.read(ids::kTurnRequestLeft);
  EXPECT(request.ok());
  EXPECT(request.value->availability == Availability::Stale);
  EXPECT(request.value->value == std::optional<SignalValue>{SignalValue::boolean(true)});
  // A signal without a configured timeout stays FreshnessUnverified.
  EXPECT(harness.provider.read(ids::kLiftgateOpen).value->availability ==
         Availability::FreshnessUnverified);
  EXPECT(harness.telemetry.stop().ok());
}

void test_malformed_message_is_unavailable() {
  Harness harness{};
  EXPECT(harness.telemetry.start().ok());
  harness.clock.set(1);
  harness.inject(engine_frame(1));
  harness.inject(turn_frame(1));
  EXPECT(wait_for_read(harness.provider, ids::kTurnState, Availability::Fresh));
  EXPECT(wait_for_value(harness.provider, ids::kEngineRpm));

  // A short TURN_SWITCH frame faults that message only.
  harness.clock.set(2);
  harness.inject(frame(candidate::kTurnSwitchId, 2, {0}));
  EXPECT(wait_for_read(harness.provider, ids::kTurnState, Availability::Unavailable));
  const auto turn = harness.provider.read(ids::kTurnState);
  EXPECT(turn.ok());
  EXPECT(turn.value->availability == Availability::Unavailable);
  EXPECT(harness.provider.read(ids::kTurnRequestLeft).value->availability ==
         Availability::Unavailable);
  EXPECT(harness.provider.read(ids::kWiperFrontPosition).value->availability ==
         Availability::Unavailable);
  // The independently decoded ENGINE_DATA message stays usable.
  EXPECT(harness.provider.read(ids::kEngineRpm).value->availability ==
         Availability::FreshnessUnverified);
  EXPECT(
      same_reading(*harness.provider.read(ids::kEngineRpm).value, harness.telemetry.engine_rpm()));
  EXPECT(harness.telemetry.stop().ok());
}

void test_transport_fault_is_unavailable_and_restart_clears_samples() {
  Harness harness{};
  EXPECT(harness.telemetry.start().ok());
  harness.clock.set(30);
  harness.inject(engine_frame(30));
  harness.inject(turn_frame(30));
  EXPECT(wait_for_read(harness.provider, ids::kTurnState, Availability::Fresh));
  EXPECT(wait_for_value(harness.provider, ids::kEngineRpm));

  harness.source.fail();
  EXPECT(wait_for([&harness] {
    return harness.telemetry.diagnostics().lifecycle == mazda::LifecycleState::Faulted;
  }));
  EXPECT(wait_for_read(harness.provider, ids::kTurnState, Availability::Unavailable));
  EXPECT(wait_for_read(harness.provider, ids::kEngineRpm, Availability::Unavailable));
  const auto rpm = harness.provider.read(ids::kEngineRpm);
  EXPECT(rpm.ok());
  EXPECT(rpm.value->value.has_value());
  EXPECT(same_reading(*rpm.value, harness.telemetry.engine_rpm()));

  EXPECT(harness.telemetry.stop().status == mazda::ResultCode::Faulted);
  EXPECT(harness.telemetry.diagnostics().lifecycle == mazda::LifecycleState::Stopped);
  EXPECT(harness.telemetry.start().ok());
  expect_no_data_for_all(harness.provider);
  EXPECT(harness.telemetry.stop().ok());
}

void test_stop_and_restart_clear_old_samples() {
  Harness harness{};
  EXPECT(harness.telemetry.start().ok());
  harness.clock.set(10);
  harness.inject(engine_frame(10));
  harness.inject(gear_frame(10));
  harness.inject(turn_frame(10));
  harness.inject(blink_frame(10));
  harness.inject(doors_frame(10));
  EXPECT(wait_for_value(harness.provider, ids::kEngineRpm));
  EXPECT(wait_for_value(harness.provider, ids::kSelectorPosition));
  EXPECT(wait_for_value(harness.provider, ids::kTurnState));
  EXPECT(wait_for_value(harness.provider, ids::kWiperLow));
  EXPECT(wait_for_value(harness.provider, ids::kDoorsUnlocked));
  EXPECT(harness.telemetry.stop().ok());

  EXPECT(harness.telemetry.start().ok());
  expect_no_data_for_all(harness.provider);
  EXPECT(
      same_reading(*harness.provider.read(ids::kEngineRpm).value, harness.telemetry.engine_rpm()));
  EXPECT(harness.telemetry.stop().ok());
}

} // namespace

int main() {
  test_value_and_type_conversion();
  test_reading_conversion_preserves_fields();
  test_notification_conversion_copies_exact_flags();
  test_unit_mapping_and_descriptor_types_match_catalog();
  test_invalid_and_unknown_ids_are_request_failures();
  test_catalog_capability_lookup();
  test_all_ids_read_no_data_then_typed_values();
  test_rpm_and_speed_match_typed_polling();
  test_discrete_reads_match_typed_notification_current();
  test_clock_advance_past_freshness_timeout_is_stale();
  test_malformed_message_is_unavailable();
  test_transport_fault_is_unavailable_and_restart_clears_samples();
  test_stop_and_restart_clear_old_samples();
  return failures == 0 ? 0 : 1;
}
