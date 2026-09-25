#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <tuple>
#include <type_traits>
#if !defined(ESP_PLATFORM)
#include <chrono>
#include <condition_variable>
#include <thread>
#endif

#include "mazda/decoder.hpp"
#include "mazda/internal_contracts.hpp"
#include "mazda/publication_store.hpp"
#include "vehicle_core/notification_channel.hpp"
#include "vehicle_telemetry/observer.hpp"
#include "vehicle_telemetry/runtime.hpp"
#if defined(ESP_PLATFORM)
#include "vehicle_telemetry/can_bus_source.hpp"
#endif

namespace mazda::internal {

class VehicleTelemetryService;

// Generic lighting is a private sink. local_argb owns policy/driver details in
// S2-B; this service only sends bounded semantic updates and a validity
// deadline. Tests provide a fake sink without adding a production LED path.
class LightingSink {
public:
  virtual ~LightingSink() = default;
  [[nodiscard]] virtual bool publish(const LightingUpdate &update) noexcept = 0;
};

class NullLightingSink final : public LightingSink {
public:
  [[nodiscard]] bool publish(const LightingUpdate &) noexcept override { return true; }
};

inline constexpr std::uint16_t kSelectorNotificationChannel = 1;
inline constexpr std::uint16_t kActualGearNotificationChannel = 2;
inline constexpr std::uint16_t kTurnNotificationChannel = 3;
inline constexpr std::uint16_t kHazardNotificationChannel = 4;
inline constexpr std::uint16_t kLeftTurnNotificationChannel = 5;
inline constexpr std::uint16_t kRightTurnNotificationChannel = 6;
inline constexpr std::uint16_t kLiftgateNotificationChannel = 7;
inline constexpr std::uint16_t kRearRightDoorNotificationChannel = 8;
inline constexpr std::uint16_t kRearLeftDoorNotificationChannel = 9;
inline constexpr std::uint16_t kFrontLeftDoorNotificationChannel = 10;
inline constexpr std::uint16_t kFrontRightDoorNotificationChannel = 11;
inline constexpr std::uint16_t kDoorsUnlockedNotificationChannel = 12;
inline constexpr std::uint16_t kLeftLampNotificationChannel = 13;
inline constexpr std::uint16_t kRightLampNotificationChannel = 14;
inline constexpr std::uint16_t kWiperLowNotificationChannel = 15;
inline constexpr std::uint16_t kFrontWiperNotificationChannel = 16;

using SelectorNotificationChannel =
    vehicle_core::NotificationChannel<SelectorPosition, kSelectorNotificationChannel>;
using ActualGearNotificationChannel =
    vehicle_core::NotificationChannel<ActualGear, kActualGearNotificationChannel>;
using TurnNotificationChannel =
    vehicle_core::NotificationChannel<TurnState, kTurnNotificationChannel>;
using HazardNotificationChannel =
    vehicle_core::NotificationChannel<bool, kHazardNotificationChannel>;
using LeftTurnNotificationChannel =
    vehicle_core::NotificationChannel<bool, kLeftTurnNotificationChannel>;
using RightTurnNotificationChannel =
    vehicle_core::NotificationChannel<bool, kRightTurnNotificationChannel>;
using LiftgateNotificationChannel =
    vehicle_core::NotificationChannel<bool, kLiftgateNotificationChannel>;
using RearRightDoorNotificationChannel =
    vehicle_core::NotificationChannel<bool, kRearRightDoorNotificationChannel>;
using RearLeftDoorNotificationChannel =
    vehicle_core::NotificationChannel<bool, kRearLeftDoorNotificationChannel>;
using FrontLeftDoorNotificationChannel =
    vehicle_core::NotificationChannel<bool, kFrontLeftDoorNotificationChannel>;
using FrontRightDoorNotificationChannel =
    vehicle_core::NotificationChannel<bool, kFrontRightDoorNotificationChannel>;
using DoorsUnlockedNotificationChannel =
    vehicle_core::NotificationChannel<bool, kDoorsUnlockedNotificationChannel>;
using LeftLampNotificationChannel =
    vehicle_core::NotificationChannel<bool, kLeftLampNotificationChannel>;
using RightLampNotificationChannel =
    vehicle_core::NotificationChannel<bool, kRightLampNotificationChannel>;
using WiperLowNotificationChannel =
    vehicle_core::NotificationChannel<bool, kWiperLowNotificationChannel>;
using FrontWiperNotificationChannel =
    vehicle_core::NotificationChannel<FrontWiperPosition, kFrontWiperNotificationChannel>;

#if !defined(ESP_PLATFORM)
// Host-only extension seam. The channel is deliberately private and has no
// corresponding public facade callback; it proves a new fixed channel can be
// added by extending the descriptor tuple alone.
inline constexpr std::uint16_t kTestFrontWiperNotificationChannel = 17;
using TestFrontWiperNotificationChannel =
    vehicle_core::NotificationChannel<FrontWiperPosition, kTestFrontWiperNotificationChannel>;
#endif

// Fixed metadata records keep the service's typed state/channel wiring in one
// place. Member pointers preserve compile-time value types while allowing the
// worker loops to operate over a heterogeneous tuple without allocation.
template <typename T> struct PollingDescriptor final {
  using SignalMember = vehicle_core::Signal<T> VehicleState::*;

  const char *name{nullptr};
  SignalMember signal{nullptr};
  std::uint32_t identifier{0};
  ValidationStatus validation{ValidationStatus::Reference};
};

#if defined(ESP_PLATFORM)
using PollingDescriptorTuple = std::tuple<PollingDescriptor<float>, PollingDescriptor<float>>;

inline constexpr PollingDescriptorTuple kPollingDescriptors{
    PollingDescriptor<float>{"speed_kph", &VehicleState::speed_kph, candidate::kEngineDataId,
                             ValidationStatus::Reference},
    PollingDescriptor<float>{"engine_rpm", &VehicleState::engine_rpm, candidate::kEngineDataId,
                             ValidationStatus::Confirmed}};
#else
using PollingDescriptorTuple = std::tuple<PollingDescriptor<float>, PollingDescriptor<float>,
                                          PollingDescriptor<FrontWiperPosition>>;

inline constexpr PollingDescriptorTuple kPollingDescriptors{
    PollingDescriptor<float>{"speed_kph", &VehicleState::speed_kph, candidate::kEngineDataId,
                             ValidationStatus::Reference},
    PollingDescriptor<float>{"engine_rpm", &VehicleState::engine_rpm, candidate::kEngineDataId,
                             ValidationStatus::Confirmed},
    PollingDescriptor<FrontWiperPosition>{"test_front_wiper", &VehicleState::front_wiper,
                                          candidate::kTurnSwitchId, ValidationStatus::Observed}};
#endif

template <typename T, std::uint16_t ChannelId> struct NotificationDescriptor final {
  using Channel = vehicle_core::NotificationChannel<T, ChannelId>;
  using ChannelMember = Channel VehicleTelemetryService::*;
  using SignalMember = vehicle_core::Signal<T> VehicleState::*;

  const char *name{nullptr};
  ChannelMember channel{nullptr};
  SignalMember signal{nullptr};
  std::uint32_t identifier{0};
  ValidationStatus validation{ValidationStatus::Reference};
};

#if defined(ESP_PLATFORM)
using NotificationDescriptorTuple =
    std::tuple<NotificationDescriptor<SelectorPosition, kSelectorNotificationChannel>,
               NotificationDescriptor<ActualGear, kActualGearNotificationChannel>,
               NotificationDescriptor<TurnState, kTurnNotificationChannel>,
               NotificationDescriptor<bool, kHazardNotificationChannel>,
               NotificationDescriptor<bool, kLeftTurnNotificationChannel>,
               NotificationDescriptor<bool, kRightTurnNotificationChannel>,
               NotificationDescriptor<bool, kLiftgateNotificationChannel>,
               NotificationDescriptor<bool, kRearRightDoorNotificationChannel>,
               NotificationDescriptor<bool, kRearLeftDoorNotificationChannel>,
               NotificationDescriptor<bool, kFrontLeftDoorNotificationChannel>,
               NotificationDescriptor<bool, kFrontRightDoorNotificationChannel>,
               NotificationDescriptor<bool, kDoorsUnlockedNotificationChannel>,
               NotificationDescriptor<bool, kLeftLampNotificationChannel>,
               NotificationDescriptor<bool, kRightLampNotificationChannel>,
               NotificationDescriptor<bool, kWiperLowNotificationChannel>,
               NotificationDescriptor<FrontWiperPosition, kFrontWiperNotificationChannel>>;
#else
using NotificationDescriptorTuple =
    std::tuple<NotificationDescriptor<SelectorPosition, kSelectorNotificationChannel>,
               NotificationDescriptor<ActualGear, kActualGearNotificationChannel>,
               NotificationDescriptor<TurnState, kTurnNotificationChannel>,
               NotificationDescriptor<bool, kHazardNotificationChannel>,
               NotificationDescriptor<bool, kLeftTurnNotificationChannel>,
               NotificationDescriptor<bool, kRightTurnNotificationChannel>,
               NotificationDescriptor<bool, kLiftgateNotificationChannel>,
               NotificationDescriptor<bool, kRearRightDoorNotificationChannel>,
               NotificationDescriptor<bool, kRearLeftDoorNotificationChannel>,
               NotificationDescriptor<bool, kFrontLeftDoorNotificationChannel>,
               NotificationDescriptor<bool, kFrontRightDoorNotificationChannel>,
               NotificationDescriptor<bool, kDoorsUnlockedNotificationChannel>,
               NotificationDescriptor<bool, kLeftLampNotificationChannel>,
               NotificationDescriptor<bool, kRightLampNotificationChannel>,
               NotificationDescriptor<bool, kWiperLowNotificationChannel>,
               NotificationDescriptor<FrontWiperPosition, kFrontWiperNotificationChannel>,
               NotificationDescriptor<FrontWiperPosition, kTestFrontWiperNotificationChannel>>;
#endif

inline constexpr std::size_t kNotificationChannelCount =
    std::tuple_size_v<NotificationDescriptorTuple>;
inline constexpr std::size_t kSubscriptionCapacity =
    kNotificationChannelCount * vehicle_core::kNotificationSubscribersPerChannel;

struct SubscriptionToken final {
  ResultCode status{ResultCode::InvalidState};
  std::uint16_t channel{0xffffU};
  std::uint8_t slot{0xffU};
  std::uint16_t generation{0};

  [[nodiscard]] constexpr bool ok() const noexcept { return status == ResultCode::Ok; }
};

// The host backend is a deterministic injected source for facade tests only.
// ESP-IDF production receives through vehicle_telemetry::CanBusSource owned by
// the generic runtime; this queue never ships in the firmware component.
#if !defined(ESP_PLATFORM)
class HostRuntimeSource final : public vehicle_telemetry::AcquisitionSource {
public:
  static constexpr std::size_t kCapacity = 64;

  [[nodiscard]] vehicle_telemetry::StatusResult start() noexcept override;
  [[nodiscard]] vehicle_telemetry::StatusResult stop() noexcept override;
  [[nodiscard]] vehicle_telemetry::ReceiveStatus
  receive(vehicle_core::RawCanFrame &frame, std::uint32_t timeout_ms) noexcept override;
  [[nodiscard]] vehicle_telemetry::AcquisitionStatistics statistics() const noexcept override;

  [[nodiscard]] ResultCode inject(const vehicle_core::RawCanFrame &frame) noexcept;
  void fail() noexcept;

private:
  [[nodiscard]] bool frames_empty() const noexcept { return head_ == tail_; }

  mutable std::mutex mutex_{};
  std::condition_variable available_{};
  std::array<vehicle_core::RawCanFrame, kCapacity> frames_{};
  std::size_t head_{0};
  std::size_t tail_{0};
  vehicle_telemetry::AcquisitionStatistics statistics_{};
  bool running_{false};
  bool faulted_{false};
};

// Compatibility name retained only for the existing host fixture vocabulary;
// it is a concrete injected test source, not a Mazda-owned receive runtime.
using HostAcquisitionSource = HostRuntimeSource;
#endif

class VehicleTelemetryService final : public vehicle_telemetry::FrameProcessor,
                                      public vehicle_telemetry::Observer {
public:
  VehicleTelemetryService() noexcept;
  VehicleTelemetryService(vehicle_core::MonotonicClock &clock,
                          vehicle_telemetry::AcquisitionSource &source, LightingSink &lighting_sink,
                          TelemetryConfig config = {}) noexcept;
  ~VehicleTelemetryService() noexcept;

  VehicleTelemetryService(const VehicleTelemetryService &) = delete;
  VehicleTelemetryService &operator=(const VehicleTelemetryService &) = delete;

  [[nodiscard]] StatusResult configure(const TelemetryConfig &config) noexcept;
  [[nodiscard]] StatusResult bind_lighting_sink(LightingSink &lighting_sink) noexcept;
  [[nodiscard]] StatusResult start() noexcept;
  [[nodiscard]] StatusResult stop() noexcept;
  [[nodiscard]] Diagnostics diagnostics() const noexcept;

  [[nodiscard]] Reading<float> speed_kph() const noexcept { return publication_.speed_kph(); }
  [[nodiscard]] Reading<float> engine_rpm() const noexcept { return publication_.engine_rpm(); }

  // These records are implementation-only. Host extension tests can inspect
  // the exact metadata consumed by the service workers without widening the
  // public facade contract.
  [[nodiscard]] static const PollingDescriptorTuple &polling_descriptors() noexcept;
  [[nodiscard]] static const NotificationDescriptorTuple &notification_descriptors() noexcept;

  // Production-private descriptor reads for implementation-only consumers.
  // Both descriptor kinds are evaluated by PublicationStore's coherent
  // descriptor read, so a polling read matches typed RPM/speed polling and a
  // notification-descriptor read matches the current value a typed
  // notification would carry for the same publication and clock value.
  // Neither overload subscribes, dispatches or touches lifecycle state.
  template <typename T>
  [[nodiscard]] Reading<T> read_descriptor(const PollingDescriptor<T> &descriptor) const noexcept;
  template <typename T, std::uint16_t ChannelId>
  [[nodiscard]] Reading<T>
  read_descriptor(const NotificationDescriptor<T, ChannelId> &descriptor) const noexcept;

  template <typename T, std::uint16_t ChannelId>
  [[nodiscard]] SubscriptionToken
  subscribe_notification_descriptor(const NotificationDescriptor<T, ChannelId> &descriptor,
                                    Callback<T> callback, void *context) noexcept;

  [[nodiscard]] SubscriptionToken subscribe_selector(Callback<SelectorPosition> callback,
                                                     void *context) noexcept;
  [[nodiscard]] SubscriptionToken subscribe_actual_gear(Callback<ActualGear> callback,
                                                        void *context) noexcept;
  [[nodiscard]] SubscriptionToken subscribe_turn(Callback<TurnState> callback,
                                                 void *context) noexcept;
  [[nodiscard]] SubscriptionToken subscribe_hazard(Callback<bool> callback, void *context) noexcept;
  [[nodiscard]] SubscriptionToken subscribe_left_turn(Callback<bool> callback,
                                                      void *context) noexcept;
  [[nodiscard]] SubscriptionToken subscribe_right_turn(Callback<bool> callback,
                                                       void *context) noexcept;
  [[nodiscard]] SubscriptionToken subscribe_liftgate(Callback<bool> callback,
                                                     void *context) noexcept;
  [[nodiscard]] SubscriptionToken subscribe_rear_right_door(Callback<bool> callback,
                                                            void *context) noexcept;
  [[nodiscard]] SubscriptionToken subscribe_rear_left_door(Callback<bool> callback,
                                                           void *context) noexcept;
  [[nodiscard]] SubscriptionToken subscribe_front_left_door(Callback<bool> callback,
                                                            void *context) noexcept;
  [[nodiscard]] SubscriptionToken subscribe_front_right_door(Callback<bool> callback,
                                                             void *context) noexcept;
  [[nodiscard]] SubscriptionToken subscribe_doors_unlocked(Callback<bool> callback,
                                                           void *context) noexcept;
  [[nodiscard]] SubscriptionToken subscribe_left_lamp(Callback<bool> callback,
                                                      void *context) noexcept;
  [[nodiscard]] SubscriptionToken subscribe_right_lamp(Callback<bool> callback,
                                                       void *context) noexcept;
  [[nodiscard]] SubscriptionToken subscribe_wiper_low(Callback<bool> callback,
                                                      void *context) noexcept;
  [[nodiscard]] SubscriptionToken subscribe_front_wiper(Callback<FrontWiperPosition> callback,
                                                        void *context) noexcept;

  [[nodiscard]] StatusResult unsubscribe(const SubscriptionToken &token) noexcept;

  // vehicle_telemetry owns receive, lifecycle and transport diagnostics. The
  // Mazda adapter only decodes frames and publishes Mazda-specific state.
  void reset() noexcept override;
  [[nodiscard]] vehicle_telemetry::ProcessResult
  process(const vehicle_core::RawCanFrame &frame) noexcept override;
  void on_frame_processed(const vehicle_core::RawCanFrame &frame,
                          const vehicle_telemetry::ProcessResult &result) noexcept override;
  void on_diagnostics(const vehicle_telemetry::TransportDiagnostics &diagnostics) noexcept override;

private:
  struct Registration final {
    vehicle_core::NotificationHandle handle{};
    std::uint16_t channel{0xffffU};
    std::uint8_t slot{0xffU};
    std::uint16_t generation{0};
    bool active{false};
  };

  [[nodiscard]] static bool valid_config(const TelemetryConfig &config) noexcept;
  void initialize_registration_slots() noexcept;
  [[nodiscard]] Registration *free_registration(std::uint16_t channel) noexcept;
  [[nodiscard]] Registration *find_registration(const SubscriptionToken &token) noexcept;
  [[nodiscard]] const Registration *
  find_registration(const SubscriptionToken &token) const noexcept;
  [[nodiscard]] static ResultCode
  map_notification_status(vehicle_core::NotificationStatus status) noexcept;
  [[nodiscard]] static ResultCode map_runtime_status(vehicle_telemetry::ResultCode status) noexcept;

  // Execution identities are process-lifetime generation tokens. A token is
  // bounded to 64 bits, requires no allocation, and is assigned once per
  // host thread or ESP-IDF task. Do not use a TLS address or FreeRTOS task
  // handle directly: both may be recycled after their owner exits.
  using ExecutionIdentity = std::uint64_t;
  static constexpr ExecutionIdentity kNoExecutionIdentity{0};
  [[nodiscard]] static ExecutionIdentity current_execution_identity() noexcept;
  [[nodiscard]] bool callback_mutation_rejected() const noexcept;
  // Must be called while lifecycle_mutex_ is held. The first lifecycle
  // mutation establishes ownership; all later mutations must use that same
  // host thread or ESP-IDF task.
  [[nodiscard]] bool claim_or_validate_lifecycle_owner() noexcept;

  template <typename T, std::uint16_t ChannelId>
  [[nodiscard]] SubscriptionToken
  register_subscription(vehicle_core::NotificationChannel<T, ChannelId> &channel,
                        Callback<T> callback, void *context) noexcept;

  [[nodiscard]] bool start_channels() noexcept;
  [[nodiscard]] bool stop_channels() noexcept;
  [[nodiscard]] std::size_t dispatch_channels_once() noexcept;

  void dispatcher_loop() noexcept;
#if defined(ESP_PLATFORM)
  static void dispatcher_task_entry(void *context) noexcept;
#endif
  void publish_current(bool received_frame) noexcept;
  template <typename T, std::uint16_t ChannelId>
  void
  publish_notification_descriptor(const PublishedSnapshot &snapshot,
                                  vehicle_core::MonotonicTimestamp now_us,
                                  const NotificationDescriptor<T, ChannelId> &descriptor) noexcept;
  void publish_notifications(const PublishedSnapshot &snapshot,
                             vehicle_core::MonotonicTimestamp now_us) noexcept;
  void publish_lighting(const PublishedSnapshot &snapshot,
                        vehicle_core::MonotonicTimestamp now_us) noexcept;
  [[nodiscard]] bool workers_done() const noexcept;
  [[nodiscard]] bool wait_for_workers(std::uint64_t timeout_us) noexcept;
  void join_workers() noexcept;
  void publish_startup_black(vehicle_core::MonotonicTimestamp now_us) noexcept;

  SteadyClock steady_clock_{};
#if defined(ESP_PLATFORM)
  vehicle_telemetry::CanBusSource can_bus_source_{can_bus::Configuration {}};
#else
  HostRuntimeSource host_source_{};
#endif
  NullLightingSink null_lighting_sink_{};
  vehicle_core::MonotonicClock *clock_{nullptr};
  LightingSink *lighting_sink_{nullptr};
  vehicle_telemetry::Runtime runtime_;
  PublicationStore publication_;
  TelemetryConfig config_{};
  VehicleState processing_state_{};
  // This watermark is sampled from the private acquisition clock when the
  // source successfully returns a frame. It is deliberately independent from
  // RawCanFrame::timestamp_us, which belongs to decoder observation ordering.
  std::optional<vehicle_core::MonotonicTimestamp> last_transport_receive_us_{};
  vehicle_telemetry::TransportDiagnostics runtime_diagnostics_{};

  SelectorNotificationChannel selector_channel_{};
  ActualGearNotificationChannel actual_gear_channel_{};
  TurnNotificationChannel turn_channel_{};
  HazardNotificationChannel hazard_channel_{};
  LeftTurnNotificationChannel left_turn_channel_{};
  RightTurnNotificationChannel right_turn_channel_{};
  LiftgateNotificationChannel liftgate_channel_{};
  RearRightDoorNotificationChannel rear_right_door_channel_{};
  RearLeftDoorNotificationChannel rear_left_door_channel_{};
  FrontLeftDoorNotificationChannel front_left_door_channel_{};
  FrontRightDoorNotificationChannel front_right_door_channel_{};
  DoorsUnlockedNotificationChannel doors_unlocked_channel_{};
  LeftLampNotificationChannel left_lamp_channel_{};
  RightLampNotificationChannel right_lamp_channel_{};
  WiperLowNotificationChannel wiper_low_channel_{};
  FrontWiperNotificationChannel front_wiper_channel_{};
#if !defined(ESP_PLATFORM)
  TestFrontWiperNotificationChannel test_front_wiper_channel_{};
#endif
  std::array<Registration, kSubscriptionCapacity> registrations_{};

  mutable std::mutex lifecycle_mutex_{};
  // The first lifecycle mutation establishes the owner. That owner remains
  // stable across stop/start cycles and is checked before every subsequent
  // lifecycle mutation. Setup registration/configuration therefore has the
  // same single-context contract as runtime start/stop.
  ExecutionIdentity lifecycle_owner_identity_{kNoExecutionIdentity};
  // Set only around the bounded dispatcher call that can invoke a user
  // callback. It lets callback-originated mutations reject before acquiring
  // lifecycle state or touching the source, including a callback that calls
  // stop() on the owner thread.
  std::atomic<ExecutionIdentity> callback_context_identity_{kNoExecutionIdentity};
  std::atomic<LifecycleState> lifecycle_state_{LifecycleState::Stopped};
  std::atomic<bool> run_requested_{false};
  std::atomic<bool> dispatcher_done_{true};
  std::atomic<std::size_t> dispatch_cursor_{0};
#if defined(ESP_PLATFORM)
  void *dispatcher_task_{nullptr};
#else
  std::thread dispatcher_thread_{};
#endif

  bool lighting_sent_{false};
  bool lighting_failure_{false};
  TurnState lighting_turn_{TurnState::Unknown};
  vehicle_core::Availability lighting_availability_{vehicle_core::Availability::NoData};
  bool lighting_brake_pressed_{false};
  vehicle_core::Availability lighting_brake_availability_{vehicle_core::Availability::NoData};
  vehicle_core::MonotonicTimestamp lighting_next_heartbeat_us_{0};
};

template <typename T, std::uint16_t ChannelId>
SubscriptionToken VehicleTelemetryService::register_subscription(
    vehicle_core::NotificationChannel<T, ChannelId> &channel, Callback<T> callback,
    void *context) noexcept {
  Registration *registration = free_registration(ChannelId);
  if (registration == nullptr)
    return {ResultCode::CapacityExceeded, ChannelId, 0xffU, 0};

  const auto result = channel.subscribe(callback, context);
  if (!result.ok())
    return {map_notification_status(result.status), ChannelId, registration->slot, 0};

  registration->active = true;
  ++registration->generation;
  if (registration->generation == 0)
    registration->generation = 1;
  registration->handle = *result.value;
  return {ResultCode::Ok, ChannelId, registration->slot, registration->generation};
}

template <typename T>
Reading<T>
VehicleTelemetryService::read_descriptor(const PollingDescriptor<T> &descriptor) const noexcept {
  return publication_.read_descriptor_signal(descriptor.signal, descriptor.identifier,
                                             descriptor.validation);
}

template <typename T, std::uint16_t ChannelId>
Reading<T> VehicleTelemetryService::read_descriptor(
    const NotificationDescriptor<T, ChannelId> &descriptor) const noexcept {
  return publication_.read_descriptor_signal(descriptor.signal, descriptor.identifier,
                                             descriptor.validation);
}

template <typename T, std::uint16_t ChannelId>
SubscriptionToken VehicleTelemetryService::subscribe_notification_descriptor(
    const NotificationDescriptor<T, ChannelId> &descriptor, Callback<T> callback,
    void *context) noexcept {
  if (callback_mutation_rejected())
    return {ResultCode::InvalidState, 0xffffU, 0xffU, 0};
  std::lock_guard<std::mutex> lock{lifecycle_mutex_};
  if (!claim_or_validate_lifecycle_owner())
    return {ResultCode::InvalidState, 0xffffU, 0xffU, 0};
  if (lifecycle_state_.load(std::memory_order_acquire) != LifecycleState::Stopped)
    return {ResultCode::InvalidState, 0xffffU, 0xffU, 0};
  return register_subscription(this->*descriptor.channel, callback, context);
}

} // namespace mazda::internal
