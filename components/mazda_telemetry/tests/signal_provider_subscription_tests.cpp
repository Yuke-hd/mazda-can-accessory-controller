#include "mazda/signal_provider.hpp"
#include "mazda/vehicle_telemetry.hpp"

#include "mazda/definitions.hpp"
#include "mazda/signal_catalog.hpp"
#include "mazda/vehicle_telemetry_internal.hpp"
#include "mazda/vehicle_telemetry_service.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace {

namespace ids = mazda::internal::signal_ids;
using vehicle_signals::SignalCapability;
using vehicle_signals::SignalId;
using vehicle_signals::SignalNotification;
using vehicle_signals::SignalStatus;
using vehicle_signals::SignalSubscription;
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

template <typename Enum> [[nodiscard]] SignalValue enum_value(const Enum value) noexcept {
  return SignalValue::enumeration(static_cast<std::uint16_t>(value));
}

template <typename Predicate>
bool wait_for_flag(Predicate predicate,
                   const std::chrono::milliseconds timeout = std::chrono::milliseconds{1000}) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  return predicate();
}

// Fixed notice log shared by every generic subscription of one test. The
// optional gate blocks the dispatcher inside the callback on a chosen value.
struct Recorder final {
  mutable std::mutex mutex{};
  mutable std::condition_variable changed{};
  std::array<SignalNotification, 256> notices{};
  std::size_t count{0};
  // Every delivery, including those beyond the fixed log.
  std::size_t delivered{0};
  std::size_t context_mismatches{0};
  std::optional<SignalValue> block_on{};
  bool entered_block{false};
  bool release_block{false};
  std::atomic<int> active_callbacks{0};

  [[nodiscard]] std::size_t size() const {
    std::lock_guard<std::mutex> lock{mutex};
    return count;
  }

  [[nodiscard]] std::size_t total() const {
    std::lock_guard<std::mutex> lock{mutex};
    return delivered;
  }

  [[nodiscard]] std::size_t count_for(const SignalId id) const {
    std::lock_guard<std::mutex> lock{mutex};
    std::size_t result = 0;
    for (std::size_t index = 0; index < count; ++index) {
      if (notices[index].id == id)
        ++result;
    }
    return result;
  }

  [[nodiscard]] std::optional<SignalNotification> latest(const SignalId id) const {
    std::lock_guard<std::mutex> lock{mutex};
    for (std::size_t index = count; index > 0; --index) {
      if (notices[index - 1].id == id)
        return notices[index - 1];
    }
    return std::nullopt;
  }

  [[nodiscard]] bool latest_value_is(const SignalId id, const SignalValue value) const {
    const auto notice = latest(id);
    return notice.has_value() && notice->current.value == value;
  }

  [[nodiscard]] SignalNotification at(const std::size_t index) const {
    std::lock_guard<std::mutex> lock{mutex};
    return notices[index];
  }

  void release() {
    {
      std::lock_guard<std::mutex> lock{mutex};
      release_block = true;
    }
    changed.notify_all();
  }

  [[nodiscard]] bool wait_until_blocked() const {
    std::unique_lock<std::mutex> lock{mutex};
    return changed.wait_for(lock, std::chrono::milliseconds{1000},
                            [this] { return entered_block; });
  }
};

// Per-subscription callback context: proves the bridge hands back the exact
// user context together with the SignalId it subscribed.
struct Tagged final {
  Recorder *recorder{nullptr};
  SignalId expected{};
};

void record_notice(void *context, const SignalNotification &notice) noexcept {
  auto &tagged = *static_cast<Tagged *>(context);
  auto &recorder = *tagged.recorder;
  recorder.active_callbacks.fetch_add(1, std::memory_order_acq_rel);
  {
    std::unique_lock<std::mutex> lock{recorder.mutex};
    ++recorder.delivered;
    if (notice.id != tagged.expected)
      ++recorder.context_mismatches;
    if (recorder.count < recorder.notices.size())
      recorder.notices[recorder.count++] = notice;
    if (recorder.block_on.has_value() && notice.current.value == recorder.block_on) {
      recorder.entered_block = true;
      recorder.changed.notify_all();
      recorder.changed.wait(lock, [&recorder] { return recorder.release_block; });
    }
  }
  recorder.changed.notify_all();
  recorder.active_callbacks.fetch_sub(1, std::memory_order_acq_rel);
}

struct TypedTurnRecorder final {
  std::mutex mutex{};
  std::size_t count{0};
  std::optional<mazda::TurnState> latest{};
};

void record_typed_turn(void *context,
                       const mazda::Notification<mazda::TurnState> &notice) noexcept {
  auto &recorder = *static_cast<TypedTurnRecorder *>(context);
  std::lock_guard<std::mutex> lock{recorder.mutex};
  ++recorder.count;
  recorder.latest = notice.current.value;
}

[[nodiscard]] std::size_t typed_count(TypedTurnRecorder &recorder) {
  std::lock_guard<std::mutex> lock{recorder.mutex};
  return recorder.count;
}

[[nodiscard]] std::optional<mazda::TurnState> typed_latest(TypedTurnRecorder &recorder) {
  std::lock_guard<std::mutex> lock{recorder.mutex};
  return recorder.latest;
}

// A facade whose private service runs the production runtime, decoder and
// publication path over an injected clock and host acquisition source.
struct Harness final {
  explicit Harness(const mazda::TelemetryConfig &config = test_config()) {
    mazda::internal::VehicleTelemetryAccess::emplace_host_service(telemetry, clock, source,
                                                                  lighting, config);
  }

  [[nodiscard]] static mazda::TelemetryConfig test_config() noexcept {
    mazda::TelemetryConfig config{};
    config.callback_stop_timeout_us = 20'000;
    return config;
  }

  [[nodiscard]] bool inject(const std::uint32_t identifier,
                            const vehicle_core::MonotonicTimestamp timestamp_us,
                            std::initializer_list<std::uint8_t> bytes) {
    clock.set(timestamp_us);
    return source.inject(frame(identifier, timestamp_us, bytes)) == mazda::ResultCode::Ok;
  }

  FakeClock clock{};
  mazda::internal::HostAcquisitionSource source{};
  mazda::internal::NullLightingSink lighting{};
  mazda::VehicleTelemetry telemetry{};
  mazda::MazdaSignalProvider provider{telemetry};
};

constexpr std::array<SignalId, 16> kNotifyIds{
    ids::kTurnState,         ids::kHazardRequest,     ids::kTurnRequestLeft,
    ids::kTurnRequestRight,  ids::kIndicatorLampLeft, ids::kIndicatorLampRight,
    ids::kSelectorPosition,  ids::kActualGear,        ids::kLiftgateOpen,
    ids::kDoorRearRight,     ids::kDoorRearLeft,      ids::kDoorFrontLeftRhd,
    ids::kDoorFrontRightRhd, ids::kDoorsUnlocked,     ids::kWiperLow,
    ids::kWiperFrontPosition};

// Waits until `target` is true and every other id of the same message is
// false, so each boolean is proven to route to its own typed channel.
bool wait_for_single_true(const Recorder &recorder, const SignalId target,
                          std::initializer_list<SignalId> group) {
  return wait_for_flag([&] {
    for (const auto id : group) {
      if (!recorder.latest_value_is(id, SignalValue::boolean(id == target)))
        return false;
    }
    return true;
  });
}

void test_every_notify_signal_routes_to_its_typed_channel() {
  Recorder recorder{};
  std::array<Tagged, kNotifyIds.size()> contexts{};
  Harness harness{};

  std::size_t notify_rows = 0;
  for (const auto &entry : harness.provider.catalog()) {
    if (entry.capabilities.has(SignalCapability::Notify))
      ++notify_rows;
  }
  EXPECT(notify_rows == kNotifyIds.size());

  std::array<SignalSubscription, kNotifyIds.size()> subscriptions{};
  for (std::size_t index = 0; index < kNotifyIds.size(); ++index) {
    contexts[index] = Tagged{&recorder, kNotifyIds[index]};
    const auto result =
        harness.provider.subscribe(kNotifyIds[index], &record_notice, &contexts[index]);
    EXPECT(result.ok());
    if (result.ok())
      subscriptions[index] = *result.value;
    EXPECT(subscriptions[index].valid());
    for (std::size_t other = 0; other < index; ++other)
      EXPECT(subscriptions[other] != subscriptions[index]);
  }

  EXPECT(harness.telemetry.start().ok());
  EXPECT(wait_for_flag([&] {
    for (const auto id : kNotifyIds) {
      if (recorder.count_for(id) == 0)
        return false;
    }
    return true;
  }));
  for (const auto id : kNotifyIds) {
    const auto initial = recorder.latest(id);
    const auto *entry = harness.provider.catalog().find(id);
    EXPECT(initial.has_value());
    EXPECT(entry != nullptr);
    if (!initial.has_value() || entry == nullptr)
      continue;
    EXPECT(initial->initial);
    EXPECT(!initial->current.value.has_value());
    EXPECT(initial->current.availability == vehicle_signals::Availability::NoData);
    EXPECT(initial->current.validation == entry->validation);
    EXPECT(!initial->became_unavailable && !initial->recovered && !initial->coalesced);
  }

  using namespace mazda::candidate;
  // TRANSMISSION: selector Drive (raw 4) and actual gear Second (raw 2).
  EXPECT(harness.inject(kTransmissionId, 10, {0x04, 0, 0, 0, 0x04, 0, 0, 0}));
  EXPECT(wait_for_flag([&] {
    return recorder.latest_value_is(ids::kSelectorPosition,
                                    enum_value(mazda::SelectorPosition::Drive)) &&
           recorder.latest_value_is(ids::kActualGear, enum_value(mazda::ActualGear::Second));
  }));
  // Wire values are the Mazda enumerators, which the catalog lists as choices.
  const auto selector = recorder.latest(ids::kSelectorPosition);
  EXPECT(
      selector.has_value() &&
      harness.provider.catalog().find(ids::kSelectorPosition)->accepts(*selector->current.value));

  // TURN_SWITCH: each switch bit reaches its own request channel, the derived
  // turn state and the front wiper field. Indicator lamps (BLINK_INFO) stay
  // without data, so requests, lamps and turn state are distinct channels.
  const std::initializer_list<SignalId> requests{ids::kHazardRequest, ids::kTurnRequestLeft,
                                                 ids::kTurnRequestRight};
  EXPECT(harness.inject(kTurnSwitchId, 20, {0, 0x20, 0, 0, 0, 0, 0, 0}));
  EXPECT(wait_for_single_true(recorder, ids::kTurnRequestLeft, requests));
  EXPECT(wait_for_flag([&] {
    return recorder.latest_value_is(ids::kTurnState, enum_value(mazda::TurnState::Left)) &&
           recorder.latest_value_is(ids::kWiperFrontPosition,
                                    enum_value(mazda::FrontWiperPosition::Off));
  }));
  EXPECT(harness.inject(kTurnSwitchId, 21, {0, 0x10, 0x10, 0, 0, 0, 0, 0}));
  EXPECT(wait_for_single_true(recorder, ids::kTurnRequestRight, requests));
  EXPECT(wait_for_flag([&] {
    return recorder.latest_value_is(ids::kTurnState, enum_value(mazda::TurnState::Right)) &&
           recorder.latest_value_is(ids::kWiperFrontPosition,
                                    enum_value(mazda::FrontWiperPosition::On));
  }));
  EXPECT(harness.inject(kTurnSwitchId, 22, {0, 0x04, 0, 0, 0, 0, 0, 0}));
  EXPECT(wait_for_single_true(recorder, ids::kHazardRequest, requests));
  EXPECT(wait_for_flag([&] {
    return recorder.latest_value_is(ids::kTurnState, enum_value(mazda::TurnState::Hazard));
  }));
  for (const auto lamp : {ids::kIndicatorLampLeft, ids::kIndicatorLampRight, ids::kWiperLow}) {
    const auto notice = recorder.latest(lamp);
    EXPECT(notice.has_value() && !notice->current.value.has_value());
  }

  // BLINK_INFO: lamps and low wiper, one bit at a time. Turn requests keep
  // their TURN_SWITCH values.
  const std::initializer_list<SignalId> blink{ids::kIndicatorLampLeft, ids::kIndicatorLampRight,
                                              ids::kWiperLow};
  EXPECT(harness.inject(kBlinkInfoId, 30, {0, 0, 0x04, 0, 0, 0, 0, 0}));
  EXPECT(wait_for_single_true(recorder, ids::kIndicatorLampLeft, blink));
  EXPECT(harness.inject(kBlinkInfoId, 31, {0, 0, 0x08, 0, 0, 0, 0, 0}));
  EXPECT(wait_for_single_true(recorder, ids::kIndicatorLampRight, blink));
  EXPECT(harness.inject(kBlinkInfoId, 32, {0, 0, 0, 0, 0x02, 0, 0, 0}));
  EXPECT(wait_for_single_true(recorder, ids::kWiperLow, blink));
  EXPECT(recorder.latest_value_is(ids::kHazardRequest, SignalValue::boolean(true)));
  EXPECT(recorder.latest_value_is(ids::kTurnRequestLeft, SignalValue::boolean(false)));

  // DOORS: each door/liftgate/lock bit reaches only its own channel.
  const std::initializer_list<SignalId> doors{ids::kLiftgateOpen,      ids::kDoorRearRight,
                                              ids::kDoorRearLeft,      ids::kDoorFrontLeftRhd,
                                              ids::kDoorFrontRightRhd, ids::kDoorsUnlocked};
  EXPECT(harness.inject(kDoorsId, 40, {0, 0, 0, 0, 0x01, 0, 0, 0}));
  EXPECT(wait_for_single_true(recorder, ids::kLiftgateOpen, doors));
  EXPECT(harness.inject(kDoorsId, 41, {0, 0, 0, 0, 0x04, 0, 0, 0}));
  EXPECT(wait_for_single_true(recorder, ids::kDoorRearRight, doors));
  EXPECT(harness.inject(kDoorsId, 42, {0, 0, 0, 0, 0x08, 0, 0, 0}));
  EXPECT(wait_for_single_true(recorder, ids::kDoorRearLeft, doors));
  EXPECT(harness.inject(kDoorsId, 43, {0, 0, 0, 0, 0x10, 0, 0, 0}));
  EXPECT(wait_for_single_true(recorder, ids::kDoorFrontLeftRhd, doors));
  EXPECT(harness.inject(kDoorsId, 44, {0, 0, 0, 0, 0x20, 0, 0, 0}));
  EXPECT(wait_for_single_true(recorder, ids::kDoorFrontRightRhd, doors));
  EXPECT(harness.inject(kDoorsId, 45, {0, 0, 0, 0x40, 0, 0, 0, 0}));
  EXPECT(wait_for_single_true(recorder, ids::kDoorsUnlocked, doors));

  EXPECT(harness.telemetry.stop().ok());
  {
    std::lock_guard<std::mutex> lock{recorder.mutex};
    EXPECT(recorder.context_mismatches == 0);
    EXPECT(recorder.count < recorder.notices.size());
  }
  for (const auto subscription : subscriptions)
    EXPECT(harness.provider.unsubscribe(subscription).ok());
}

void test_request_failures_are_distinct() {
  Recorder recorder{};
  Tagged context{&recorder, ids::kTurnState};
  Harness harness{};
  auto &provider = harness.provider;

  // RPM and speed are Read-only catalog rows backed by polling descriptors.
  EXPECT(provider.subscribe(ids::kEngineRpm, &record_notice, &context).status ==
         SignalStatus::UnsupportedCapability);
  EXPECT(provider.subscribe(ids::kSpeedKph, &record_notice, &context).status ==
         SignalStatus::UnsupportedCapability);
  EXPECT(provider.subscribe(ids::kEngineRpm, nullptr, nullptr).status ==
         SignalStatus::UnsupportedCapability);
  EXPECT(provider.subscribe(SignalId{}, &record_notice, &context).status ==
         SignalStatus::InvalidSignal);
  EXPECT(provider.subscribe(SignalId{19}, &record_notice, &context).status ==
         SignalStatus::InvalidSignal);
  EXPECT(provider.subscribe(SignalId{0xffffU}, &record_notice, &context).status ==
         SignalStatus::InvalidSignal);
  EXPECT(provider.subscribe(ids::kTurnState, nullptr, &context).status ==
         SignalStatus::InvalidArgument);
  const auto failed = provider.subscribe(ids::kTurnState, nullptr, &context);
  EXPECT(!failed.ok() && !failed.value.has_value());

  // None of the rejected requests consumed a slot or a trampoline record.
  const auto first = provider.subscribe(ids::kTurnState, &record_notice, &context);
  EXPECT(first.ok());

  // The first mutation above established the lifecycle owner. A non-owner
  // thread is rejected by the service and leaves the channel unchanged.
  std::atomic<SignalStatus> non_owner_subscribe{SignalStatus::Ok};
  std::atomic<SignalStatus> non_owner_unsubscribe{SignalStatus::Ok};
  std::thread non_owner([&] {
    non_owner_subscribe.store(provider.subscribe(ids::kTurnState, &record_notice, &context).status,
                              std::memory_order_release);
    non_owner_unsubscribe.store(provider.unsubscribe(*first.value).status,
                                std::memory_order_release);
  });
  non_owner.join();
  EXPECT(non_owner_subscribe.load(std::memory_order_acquire) == SignalStatus::InvalidState);
  EXPECT(non_owner_unsubscribe.load(std::memory_order_acquire) == SignalStatus::InvalidState);

  const auto second = provider.subscribe(ids::kTurnState, &record_notice, &context);
  EXPECT(second.ok());
  EXPECT(provider.subscribe(ids::kTurnState, &record_notice, &context).status ==
         SignalStatus::CapacityExceeded);
  EXPECT(provider.unsubscribe(*first.value).ok());
  EXPECT(provider.unsubscribe(*second.value).ok());
}

void test_typed_and_generic_subscribers_share_two_slots() {
  Recorder recorder{};
  Tagged context{&recorder, ids::kTurnState};
  TypedTurnRecorder typed{};
  TypedTurnRecorder second_typed{};
  Harness harness{};
  auto &provider = harness.provider;
  auto &telemetry = harness.telemetry;

  const auto typed_subscription = telemetry.on_turn_state_changed(&record_typed_turn, &typed);
  EXPECT(typed_subscription.ok());
  const auto generic = provider.subscribe(ids::kTurnState, &record_notice, &context);
  EXPECT(generic.ok());
  // Typed + generic fill the fixed two-slot channel for either kind.
  EXPECT(provider.subscribe(ids::kTurnState, &record_notice, &context).status ==
         SignalStatus::CapacityExceeded);
  EXPECT(telemetry.on_turn_state_changed(&record_typed_turn, &second_typed).status ==
         mazda::ResultCode::CapacityExceeded);
  // Other channels keep their own capacity.
  const auto hazard = provider.subscribe(ids::kHazardRequest, &record_notice, &context);
  EXPECT(hazard.ok());
  EXPECT(provider.unsubscribe(*hazard.value).ok());

  EXPECT(telemetry.start().ok());
  EXPECT(wait_for_flag([&] { return typed_count(typed) >= 1 && recorder.size() >= 1; }));
  EXPECT(harness.inject(mazda::candidate::kTurnSwitchId, 10, {0, 0x20, 0, 0, 0, 0, 0, 0}));
  EXPECT(wait_for_flag([&] {
    return typed_latest(typed) == mazda::TurnState::Left &&
           recorder.latest_value_is(ids::kTurnState, enum_value(mazda::TurnState::Left));
  }));
  EXPECT(telemetry.stop().ok());

  // Removing the generic subscriber frees its slot for a typed subscriber.
  EXPECT(provider.unsubscribe(*generic.value).ok());
  const auto second_typed_subscription =
      telemetry.on_turn_state_changed(&record_typed_turn, &second_typed);
  EXPECT(second_typed_subscription.ok());
  EXPECT(provider.subscribe(ids::kTurnState, &record_notice, &context).status ==
         SignalStatus::CapacityExceeded);
  EXPECT(telemetry.unsubscribe(*second_typed_subscription.value).ok());
  const auto regained = provider.subscribe(ids::kTurnState, &record_notice, &context);
  EXPECT(regained.ok());
  EXPECT(provider.unsubscribe(*regained.value).ok());
  EXPECT(telemetry.unsubscribe(*typed_subscription.value).ok());
  EXPECT(recorder.size() >= 2);
}

void test_unsubscribe_lifecycle_and_stale_tokens() {
  Recorder recorder{};
  Tagged context{&recorder, ids::kHazardRequest};
  Harness harness{};
  auto &provider = harness.provider;

  EXPECT(provider.unsubscribe(SignalSubscription{}).status == SignalStatus::InvalidSubscription);
  const auto first = provider.subscribe(ids::kHazardRequest, &record_notice, &context);
  EXPECT(first.ok());
  const auto token = *first.value;

  EXPECT(harness.telemetry.start().ok());
  EXPECT(provider.unsubscribe(token).status == SignalStatus::InvalidState);
  EXPECT(provider.subscribe(ids::kHazardRequest, &record_notice, &context).status ==
         SignalStatus::InvalidState);
  EXPECT(harness.telemetry.stop().ok());

  EXPECT(provider.unsubscribe(token).ok());
  EXPECT(provider.unsubscribe(token).status == SignalStatus::InvalidSubscription);

  // The resubscription reuses the typed slot with a newer generation, so the
  // earlier token cannot remove it.
  const auto second = provider.subscribe(ids::kHazardRequest, &record_notice, &context);
  EXPECT(second.ok());
  EXPECT(*second.value != token);
  EXPECT(provider.unsubscribe(token).status == SignalStatus::InvalidSubscription);
  EXPECT(provider.unsubscribe(SignalSubscription{}).status == SignalStatus::InvalidSubscription);
  EXPECT(provider
             .unsubscribe(SignalSubscription::from_provider_bits(second.value->provider_bits() |
                                                                 (std::uint64_t{1} << 60U)))
             .status == SignalStatus::InvalidSubscription);
  EXPECT(provider.unsubscribe(SignalSubscription::from_provider_bits(0xdeadbeefULL)).status ==
         SignalStatus::InvalidSubscription);
  EXPECT(provider.unsubscribe(*second.value).ok());
  EXPECT(provider.unsubscribe(*second.value).status == SignalStatus::InvalidSubscription);
}

void test_restart_creates_fresh_initial_notice() {
  Recorder recorder{};
  Tagged context{&recorder, ids::kTurnState};
  Harness harness{};
  const auto subscription = harness.provider.subscribe(ids::kTurnState, &record_notice, &context);
  EXPECT(subscription.ok());

  EXPECT(harness.telemetry.start().ok());
  EXPECT(wait_for_flag([&] { return recorder.size() >= 1; }));
  EXPECT(harness.inject(mazda::candidate::kTurnSwitchId, 10, {0, 0x20, 0, 0, 0, 0, 0, 0}));
  EXPECT(wait_for_flag([&] { return recorder.size() >= 2; }));
  EXPECT(harness.telemetry.stop().ok());
  const auto before_restart = recorder.size();

  EXPECT(harness.telemetry.start().ok());
  EXPECT(wait_for_flag([&] { return recorder.size() > before_restart; }));
  const auto restarted = recorder.at(before_restart);
  EXPECT(restarted.id == ids::kTurnState);
  EXPECT(restarted.initial);
  EXPECT(!restarted.current.value.has_value());
  EXPECT(restarted.current.availability == vehicle_signals::Availability::NoData);
  EXPECT(!restarted.recovered && !restarted.became_unavailable && !restarted.coalesced);
  EXPECT(harness.telemetry.stop().ok());
  EXPECT(harness.provider.unsubscribe(*subscription.value).ok());
}

void test_coalesced_unavailable_and_recovered_notices() {
  Recorder recorder{};
  recorder.block_on = enum_value(mazda::TurnState::Left);
  Tagged context{&recorder, ids::kTurnState};
  auto config = Harness::test_config();
  config.transport_silence_timeout_us = 100;
  Harness harness{config};
  const auto subscription = harness.provider.subscribe(ids::kTurnState, &record_notice, &context);
  EXPECT(subscription.ok());
  EXPECT(harness.telemetry.start().ok());
  EXPECT(wait_for_flag([&] { return recorder.size() >= 1; }));

  // Block the dispatcher in the callback on Left while two newer states are
  // published; the channel keeps one latest notice and marks it coalesced.
  EXPECT(harness.inject(mazda::candidate::kTurnSwitchId, 10, {0, 0x20, 0, 0, 0, 0, 0, 0}));
  EXPECT(recorder.wait_until_blocked());
  EXPECT(harness.inject(mazda::candidate::kTurnSwitchId, 11, {0, 0x10, 0, 0, 0, 0, 0, 0}));
  EXPECT(harness.inject(mazda::candidate::kTurnSwitchId, 12, {0, 0x04, 0, 0, 0, 0, 0, 0}));
  // Two later polled publications prove the processing owner finished the
  // publication pass that merged Hazard into the pending notice.
  EXPECT(harness.inject(mazda::candidate::kEngineDataId, 13, {0x09, 0x5b, 0, 0, 0, 0, 0, 0}));
  EXPECT(wait_for_flag([&] { return harness.telemetry.engine_rpm().value == 598.75F; }));
  EXPECT(harness.inject(mazda::candidate::kEngineDataId, 14, {0x00, 0x01, 0, 0, 0, 0, 0, 0}));
  EXPECT(wait_for_flag([&] { return harness.telemetry.engine_rpm().value == 0.25F; }));
  recorder.release();
  EXPECT(wait_for_flag([&] { return recorder.size() >= 3; }));
  const auto coalesced = recorder.at(2);
  EXPECT(coalesced.current.value == enum_value(mazda::TurnState::Hazard));
  EXPECT(coalesced.coalesced);
  EXPECT(!coalesced.initial);

  // Transport silence makes the reading unusable; a fresh frame recovers it.
  harness.clock.set(200);
  EXPECT(wait_for_flag([&] {
    const auto notice = recorder.latest(ids::kTurnState);
    return notice.has_value() && notice->became_unavailable;
  }));
  const auto unavailable = recorder.latest(ids::kTurnState);
  EXPECT(unavailable.has_value() &&
         unavailable->current.availability == vehicle_signals::Availability::Unavailable);
  EXPECT(harness.inject(mazda::candidate::kTurnSwitchId, 201, {0, 0x10, 0, 0, 0, 0, 0, 0}));
  EXPECT(wait_for_flag([&] {
    const auto notice = recorder.latest(ids::kTurnState);
    return notice.has_value() && notice->recovered;
  }));
  const auto recovered = recorder.latest(ids::kTurnState);
  EXPECT(recovered.has_value() && recovered->current.value == enum_value(mazda::TurnState::Right));
  EXPECT(harness.telemetry.stop().ok());
  EXPECT(harness.provider.unsubscribe(*subscription.value).ok());
}

void test_callback_context_is_borrowed_until_successful_stop() {
  Recorder recorder{};
  recorder.block_on = enum_value(mazda::TurnState::Left);
  Tagged context{&recorder, ids::kTurnState};
  auto config = Harness::test_config();
  config.callback_stop_timeout_us = 10'000;
  Harness harness{config};
  const auto subscription = harness.provider.subscribe(ids::kTurnState, &record_notice, &context);
  EXPECT(subscription.ok());
  EXPECT(harness.telemetry.start().ok());
  EXPECT(wait_for_flag([&] { return recorder.size() >= 1; }));
  EXPECT(harness.inject(mazda::candidate::kTurnSwitchId, 10, {0, 0x20, 0, 0, 0, 0, 0, 0}));
  EXPECT(recorder.wait_until_blocked());

  // A timed-out stop leaves the callback active and the context borrowed.
  EXPECT(harness.telemetry.stop().status == mazda::ResultCode::Timeout);
  EXPECT(recorder.active_callbacks.load(std::memory_order_acquire) == 1);
  EXPECT(harness.provider.unsubscribe(*subscription.value).status == SignalStatus::InvalidState);
  recorder.release();
  EXPECT(harness.telemetry.stop().ok());

  // After a successful stop no callback runs or starts.
  EXPECT(recorder.active_callbacks.load(std::memory_order_acquire) == 0);
  const auto delivered = recorder.size();
  std::this_thread::sleep_for(std::chrono::milliseconds{20});
  EXPECT(recorder.size() == delivered);
  EXPECT(recorder.active_callbacks.load(std::memory_order_acquire) == 0);
  EXPECT(harness.provider.unsubscribe(*subscription.value).ok());
}

struct MutationProbe final {
  mazda::MazdaSignalProvider *provider{nullptr};
  SignalSubscription own{};
  std::atomic<bool> done{false};
  std::atomic<SignalStatus> subscribe_result{SignalStatus::Ok};
  std::atomic<SignalStatus> unsubscribe_result{SignalStatus::Ok};
};

void mutate_from_callback(void *context, const SignalNotification &) noexcept {
  auto &probe = *static_cast<MutationProbe *>(context);
  if (probe.done.load(std::memory_order_acquire))
    return;
  probe.subscribe_result.store(
      probe.provider->subscribe(ids::kHazardRequest, &mutate_from_callback, context).status,
      std::memory_order_release);
  probe.unsubscribe_result.store(probe.provider->unsubscribe(probe.own).status,
                                 std::memory_order_release);
  probe.done.store(true, std::memory_order_release);
}

void test_callback_originated_mutations_are_rejected() {
  MutationProbe probe{};
  Harness harness{};
  probe.provider = &harness.provider;
  const auto subscription =
      harness.provider.subscribe(ids::kTurnState, &mutate_from_callback, &probe);
  EXPECT(subscription.ok());
  probe.own = *subscription.value;
  EXPECT(harness.telemetry.start().ok());
  EXPECT(wait_for_flag([&probe] { return probe.done.load(std::memory_order_acquire); }));
  EXPECT(probe.subscribe_result.load(std::memory_order_acquire) == SignalStatus::InvalidState);
  EXPECT(probe.unsubscribe_result.load(std::memory_order_acquire) == SignalStatus::InvalidState);
  EXPECT(harness.telemetry.stop().ok());

  // The rejected attempts left no registration behind: the hazard channel
  // still has both slots and the callback's own token is still live.
  const auto first = harness.provider.subscribe(ids::kHazardRequest, &mutate_from_callback, &probe);
  const auto second =
      harness.provider.subscribe(ids::kHazardRequest, &mutate_from_callback, &probe);
  EXPECT(first.ok() && second.ok());
  EXPECT(harness.provider.unsubscribe(*first.value).ok());
  EXPECT(harness.provider.unsubscribe(*second.value).ok());
  EXPECT(harness.provider.unsubscribe(probe.own).ok());
}

// Keeps injecting alternating Left/Right TURN_SWITCH frames until stopped, so
// the dispatcher is actively delivering while a test destroys objects.
class TurnFeeder final {
public:
  TurnFeeder(FakeClock &clock, mazda::internal::HostAcquisitionSource &source)
      : clock_(clock), source_(source), thread_([this] { run(); }) {}
  ~TurnFeeder() { stop(); }
  TurnFeeder(const TurnFeeder &) = delete;
  TurnFeeder &operator=(const TurnFeeder &) = delete;

  void stop() {
    stop_.store(true, std::memory_order_release);
    if (thread_.joinable())
      thread_.join();
  }

private:
  void run() {
    vehicle_core::MonotonicTimestamp timestamp_us = 1'000;
    bool left = true;
    while (!stop_.load(std::memory_order_acquire)) {
      clock_.set(timestamp_us);
      const auto request = static_cast<std::uint8_t>(left ? 0x20U : 0x10U);
      (void)source_.inject(
          frame(mazda::candidate::kTurnSwitchId, timestamp_us, {0, request, 0, 0, 0, 0, 0, 0}));
      ++timestamp_us;
      left = !left;
      std::this_thread::sleep_for(std::chrono::microseconds{500});
    }
  }

  FakeClock &clock_;
  mazda::internal::HostAcquisitionSource &source_;
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

void test_provider_reset_while_running_keeps_facade_registration() {
  Recorder recorder{};
  Tagged context{&recorder, ids::kTurnState};
  Harness harness{};
  auto provider = std::make_unique<mazda::MazdaSignalProvider>(harness.telemetry);
  const auto subscription = provider->subscribe(ids::kTurnState, &record_notice, &context);
  EXPECT(subscription.ok());
  EXPECT(harness.telemetry.start().ok());
  {
    TurnFeeder feeder{harness.clock, harness.source};
    EXPECT(wait_for_flag([&] { return recorder.total() >= 5; }));
    // The provider is only a view: destroying it mid-stream leaves the
    // facade-owned registration and record in place.
    provider.reset();
    const auto after_reset = recorder.total();
    EXPECT(wait_for_flag([&] { return recorder.total() >= after_reset + 5; }));
  }
  EXPECT(harness.telemetry.stop().ok());
  {
    std::lock_guard<std::mutex> lock{recorder.mutex};
    EXPECT(recorder.context_mismatches == 0);
  }

  // Tokens are facade-scoped: another provider over the same facade releases
  // the registration and frees its slot.
  mazda::MazdaSignalProvider second{harness.telemetry};
  EXPECT(second.unsubscribe(*subscription.value).ok());
  EXPECT(second.unsubscribe(*subscription.value).status == SignalStatus::InvalidSubscription);
  const auto first_slot = second.subscribe(ids::kTurnState, &record_notice, &context);
  const auto second_slot = second.subscribe(ids::kTurnState, &record_notice, &context);
  EXPECT(first_slot.ok() && second_slot.ok());
  EXPECT(second.unsubscribe(*first_slot.value).ok());
  EXPECT(second.unsubscribe(*second_slot.value).ok());
}

struct App final {
  mazda::VehicleTelemetry telemetry{};
  mazda::MazdaSignalProvider provider{telemetry};
};

void test_app_destroyed_while_running_quiesces_generic_callbacks() {
  Recorder recorder{};
  Tagged context{&recorder, ids::kTurnState};
  FakeClock clock{};
  mazda::internal::HostAcquisitionSource source{};
  mazda::internal::NullLightingSink lighting{};
  auto app = std::make_unique<App>();
  mazda::internal::VehicleTelemetryAccess::emplace_host_service(app->telemetry, clock, source,
                                                                lighting, Harness::test_config());
  EXPECT(app->provider.subscribe(ids::kTurnState, &record_notice, &context).ok());
  EXPECT(app->telemetry.start().ok());

  TurnFeeder feeder{clock, source};
  EXPECT(wait_for_flag([&] { return recorder.total() >= 5; }));
  // Members are destroyed in reverse order: the provider first while the
  // facade is still running, then the facade stops and joins its dispatcher.
  app.reset();
  const auto delivered = recorder.total();
  EXPECT(recorder.active_callbacks.load(std::memory_order_acquire) == 0);
  std::this_thread::sleep_for(std::chrono::milliseconds{20});
  feeder.stop();
  EXPECT(recorder.total() == delivered);
  EXPECT(recorder.active_callbacks.load(std::memory_order_acquire) == 0);
}

void test_generic_and_typed_tokens_are_not_interchangeable() {
  Recorder recorder{};
  Tagged context{&recorder, ids::kTurnState};
  TypedTurnRecorder typed{};
  Harness harness{};
  auto &service = mazda::internal::VehicleTelemetryAccess::service(harness.telemetry);
  const auto typed_subscription =
      harness.telemetry.on_turn_state_changed(&record_typed_turn, &typed);
  const auto generic = harness.provider.subscribe(ids::kTurnState, &record_notice, &context);
  EXPECT(typed_subscription.ok() && generic.ok());

  // The generic token carries the typed channel/slot/generation fields.
  const auto bits = generic.value->provider_bits();
  const auto channel = static_cast<std::uint16_t>(bits >> 32U);
  const auto generic_slot = static_cast<std::uint8_t>((bits >> 16U) & 0xffU);
  const auto generation = static_cast<std::uint16_t>(bits & 0xffffU);
  EXPECT(channel == mazda::internal::kTurnNotificationChannel);
  EXPECT(generic_slot <= 1U && generation == 1U);
  const mazda::internal::SubscriptionToken generic_token{mazda::ResultCode::Ok, channel,
                                                         generic_slot, generation};
  // On this fresh service the typed registration holds the other slot with
  // its first generation.
  const auto typed_slot = static_cast<std::uint8_t>(1U - generic_slot);
  const mazda::internal::SubscriptionToken typed_token{mazda::ResultCode::Ok, channel, typed_slot,
                                                       1U};

  // A typed unsubscribe cannot remove the generic registration, and a
  // generic unsubscribe (service or provider) cannot remove the typed one.
  EXPECT(service.unsubscribe(generic_token).status == mazda::ResultCode::InvalidSubscription);
  EXPECT(service.unsubscribe_generic(typed_token).status == mazda::ResultCode::InvalidSubscription);
  EXPECT(harness.provider
             .unsubscribe(SignalSubscription::from_provider_bits(
                 (std::uint64_t{channel} << 32U) | (std::uint64_t{typed_slot} << 16U) | 1U))
             .status == SignalStatus::InvalidSubscription);

  // Both registrations are intact and both still deliver.
  EXPECT(harness.provider.subscribe(ids::kTurnState, &record_notice, &context).status ==
         SignalStatus::CapacityExceeded);
  EXPECT(harness.telemetry.start().ok());
  EXPECT(wait_for_flag([&] { return typed_count(typed) >= 1 && recorder.total() >= 1; }));
  EXPECT(harness.telemetry.stop().ok());

  // Each kind is still removed through its own path; the typed token above
  // was the typed registration's exact token.
  EXPECT(service.unsubscribe_generic(generic_token).ok());
  EXPECT(service.unsubscribe(typed_token).ok());
  EXPECT(harness.telemetry.unsubscribe(*typed_subscription.value).status ==
         mazda::ResultCode::InvalidSubscription);
  EXPECT(harness.provider.unsubscribe(*generic.value).status == SignalStatus::InvalidSubscription);
}

} // namespace

int main() {
  test_every_notify_signal_routes_to_its_typed_channel();
  test_request_failures_are_distinct();
  test_typed_and_generic_subscribers_share_two_slots();
  test_unsubscribe_lifecycle_and_stale_tokens();
  test_restart_creates_fresh_initial_notice();
  test_coalesced_unavailable_and_recovered_notices();
  test_callback_context_is_borrowed_until_successful_stop();
  test_callback_originated_mutations_are_rejected();
  test_provider_reset_while_running_keeps_facade_registration();
  test_app_destroyed_while_running_quiesces_generic_callbacks();
  test_generic_and_typed_tokens_are_not_interchangeable();
  return failures == 0 ? 0 : 1;
}
