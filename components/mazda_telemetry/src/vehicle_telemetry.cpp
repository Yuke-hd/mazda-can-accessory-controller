#include "mazda/vehicle_telemetry.hpp"

#include "mazda/publication_store.hpp"
#include "mazda/vehicle_telemetry_internal.hpp"
#include "mazda/vehicle_telemetry_service.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <new>
#include <utility>
#if !defined(ESP_PLATFORM)
#include <condition_variable>
#include <mutex>
#include <thread>
#endif

#if defined(ESP_PLATFORM)
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#endif

#if defined(MAZDA_ENABLE_DEBUG_TELEMETRY) || defined(CONFIG_WEACT_CAN_FRESHNESS_DEBUG)
#define MAZDA_TELEMETRY_DEBUG_ENABLED 1
#endif

namespace mazda::internal {

#if defined(MAZDA_TELEMETRY_DEBUG_ENABLED)
inline constexpr vehicle_core::MonotonicTimestamp kDebugAggregateRefreshPeriodUs = 5'000'000;
#endif

#if !defined(ESP_PLATFORM)

vehicle_telemetry::StatusResult HostRuntimeSource::start() noexcept {
  std::lock_guard<std::mutex> lock{mutex_};
  if (running_)
    return {vehicle_telemetry::ResultCode::AlreadyRunning};
  head_ = 0;
  tail_ = 0;
  statistics_ = {};
  running_ = true;
  faulted_ = false;
  return {vehicle_telemetry::ResultCode::Ok};
}

vehicle_telemetry::StatusResult HostRuntimeSource::stop() noexcept {
  {
    std::lock_guard<std::mutex> lock{mutex_};
    running_ = false;
  }
  available_.notify_all();
  return {vehicle_telemetry::ResultCode::Ok};
}

vehicle_telemetry::ReceiveStatus
HostRuntimeSource::receive(vehicle_core::RawCanFrame &frame,
                           const std::uint32_t timeout_ms) noexcept {
  std::unique_lock<std::mutex> lock{mutex_};
  const auto ready = [this] { return !frames_empty() || faulted_ || !running_; };
  if (!ready() && timeout_ms != 0)
    (void)available_.wait_for(lock, std::chrono::milliseconds{timeout_ms}, ready);
  if (faulted_)
    return vehicle_telemetry::ReceiveStatus::Fault;
  if (!running_)
    return vehicle_telemetry::ReceiveStatus::NotStarted;
  if (frames_empty())
    return vehicle_telemetry::ReceiveStatus::Timeout;
  frame = frames_[tail_ % kCapacity];
  ++tail_;
  return vehicle_telemetry::ReceiveStatus::Frame;
}

vehicle_telemetry::AcquisitionStatistics HostRuntimeSource::statistics() const noexcept {
  std::lock_guard<std::mutex> lock{mutex_};
  return statistics_;
}

ResultCode HostRuntimeSource::inject(const vehicle_core::RawCanFrame &frame) noexcept {
  {
    std::lock_guard<std::mutex> lock{mutex_};
    if (!running_)
      return ResultCode::NotRunning;
    ++statistics_.frames_received;
    if ((head_ - tail_) >= kCapacity) {
      ++statistics_.frames_dropped;
      ++statistics_.queue_overflows;
      return ResultCode::CapacityExceeded;
    }
    frames_[head_ % kCapacity] = frame;
    ++head_;
  }
  available_.notify_one();
  return ResultCode::Ok;
}

void HostRuntimeSource::fail() noexcept {
  {
    std::lock_guard<std::mutex> lock{mutex_};
    faulted_ = true;
    ++statistics_.driver_errors;
  }
  available_.notify_all();
}

#endif

namespace {

constexpr vehicle_core::Microseconds kLightingHeartbeatUs = 100'000;
#if defined(ESP_PLATFORM)
constexpr char kLightingTag[] = "mazda_telemetry";
#endif
constexpr std::uint16_t kInvalidChannel = 0xffffU;
// This counter is deliberately never reset while the process is alive. The
// value is stored in each execution context's TLS, so a recycled TLS address
// or FreeRTOS task handle cannot recreate an earlier lifecycle identity.
std::atomic<std::uint64_t> g_next_execution_identity{1};
#if defined(ESP_PLATFORM)
// Keep task polling cooperative even when the configured tick rate truncates
// a one-millisecond delay to zero ticks.
constexpr TickType_t kMinimumTaskDelayTicks = pdMS_TO_TICKS(1) == 0 ? 1 : pdMS_TO_TICKS(1);
#endif

vehicle_core::MonotonicTimestamp saturating_add(const vehicle_core::MonotonicTimestamp value,
                                                const vehicle_core::Microseconds delta) noexcept {
  if (value > std::numeric_limits<vehicle_core::MonotonicTimestamp>::max() - delta)
    return std::numeric_limits<vehicle_core::MonotonicTimestamp>::max();
  return value + delta;
}

std::uint64_t saturating_counter_add(const std::uint64_t value,
                                     const std::uint64_t increment) noexcept {
  if (value > std::numeric_limits<std::uint64_t>::max() - increment)
    return std::numeric_limits<std::uint64_t>::max();
  return value + increment;
}

std::uint64_t
decoder_frames_processed(const vehicle_telemetry::TransportDiagnostics &diagnostics) noexcept {
  auto total = diagnostics.frames_processed;
  total = saturating_counter_add(total, diagnostics.frames_ignored);
  total = saturating_counter_add(total, diagnostics.frames_malformed);
  return saturating_counter_add(total, diagnostics.processor_faults);
}

AcquisitionMetrics
acquisition_metrics(const vehicle_telemetry::TransportDiagnostics &diagnostics) noexcept {
  AcquisitionMetrics acquisition{};
  acquisition.frames_received = diagnostics.acquisition.frames_received;
  acquisition.frames_processed = decoder_frames_processed(diagnostics);
  acquisition.frames_dropped = diagnostics.acquisition.frames_dropped;
  acquisition.queue_overflows = diagnostics.acquisition.queue_overflows;
  acquisition.driver_errors = diagnostics.acquisition.driver_errors;
  acquisition.missed_frames = diagnostics.acquisition.missed_frames;
  acquisition.controller_resets = diagnostics.acquisition.controller_resets;
  acquisition.bus_off_events = diagnostics.acquisition.bus_off_events;
  return acquisition;
}

template <typename T> bool is_available_reading(const Reading<T> &reading) noexcept {
  return reading.value.has_value() && (reading.availability == Availability::Fresh ||
                                       reading.availability == Availability::FreshnessUnverified);
}

template <typename T> struct LightingDescriptor final {
  vehicle_core::Signal<T> VehicleState::*signal;
  const candidate::CandidateSignalDefinition *metadata;
};

template <typename T>
Reading<T> notification_reading(const VehicleState &state, const Diagnostics &diagnostics,
                                const LightingDescriptor<T> &descriptor,
                                const vehicle_core::MonotonicTimestamp now_us) noexcept {
  const auto &metadata = *descriptor.metadata;
  return state.reading_at(state.*descriptor.signal, metadata.identifier, now_us,
                          metadata.confidence, diagnostics.transport);
}

struct MessageMutationMarker final {
  bool has_frame{false};
  vehicle_core::MonotonicTimestamp last_frame_us{0};
};

MessageMutationMarker message_mutation_marker(const VehicleState &state,
                                              const std::uint32_t identifier) noexcept {
  const auto *message = state.message_health_for(identifier);
  return message == nullptr ? MessageMutationMarker{}
                            : MessageMutationMarker{true, message->last_frame_us};
}

// turn_state is derived from the three TURN_SWITCH request fields. All three
// source definitions are Reference-confidence, so bind the derived channel to
// one of those authoritative definitions instead of duplicating a literal.
inline constexpr LightingDescriptor<TurnState> kTurnNotificationDescriptor{
    &VehicleState::turn_state, &candidate::kTurnLeftSwitchDefinition};

// Keep the facade's receive-target conversion inside the runtime contract's
// bounded range. The core validates the resulting RuntimeConfig, but it
// cannot validate an overflowing or truncated facade conversion after the
// value has been cast to milliseconds.
constexpr vehicle_core::Microseconds kRuntimeMaximumReceiveTimeoutUs = 60'000'000;
constexpr vehicle_core::Microseconds kRuntimeMaximumSilenceTimeoutUs = 300'000'000;
constexpr std::uint32_t kRuntimeMaximumRunnableReceiveCalls = 65'535;
constexpr vehicle_core::Microseconds kRuntimeMaximumRunnableTimeUs = 1'000'000;
constexpr std::uint32_t kRuntimeMaximumBudgetPauseMs = 1'000;

bool build_runtime_config(const TelemetryConfig &config,
                          vehicle_telemetry::RuntimeConfig &runtime_config) noexcept {
  if (config.availability_service_target_us == 0 ||
      config.availability_service_target_us > kRuntimeMaximumReceiveTimeoutUs)
    return false;

  runtime_config.receive_timeout_ms =
      static_cast<std::uint32_t>(std::max<vehicle_core::Microseconds>(
          1, (config.availability_service_target_us + 999) / 1'000));
  runtime_config.transport_silence_timeout_us = config.transport_silence_timeout_us;
  runtime_config.max_frames_per_batch = config.max_frames_per_batch;
  runtime_config.max_batch_time_us = config.max_batch_time_us;
  runtime_config.max_runnable_receive_calls = config.max_runnable_receive_calls;
  runtime_config.max_runnable_time_us = config.max_runnable_time_us;
  runtime_config.budget_pause_ms = config.budget_pause_ms;
  return true;
}

vehicle_telemetry::StatusResult configure_runtime(vehicle_telemetry::Runtime &runtime,
                                                  const TelemetryConfig &config) noexcept {
  vehicle_telemetry::RuntimeConfig runtime_config{};
  if (!build_runtime_config(config, runtime_config))
    return {vehicle_telemetry::ResultCode::InvalidConfiguration};
  return runtime.configure(runtime_config);
}

bool transport_requires_failoff(const vehicle_core::TransportHealth transport) noexcept {
  return transport == vehicle_core::TransportHealth::TimedOut ||
         transport == vehicle_core::TransportHealth::Faulted ||
         transport == vehicle_core::TransportHealth::Stopped;
}

bool requires_global_health_evaluation(const LifecycleState previous_lifecycle,
                                       const vehicle_core::TransportHealth previous_transport,
                                       const LifecycleState lifecycle,
                                       const vehicle_core::TransportHealth transport,
                                       const bool initialized) noexcept {
  if (!initialized)
    return true;
  // A transport/lifecycle failure is a global availability boundary. Publish
  // every owned group once so each subscriber sees the existing fail-off
  // evidence, even when the failing receive was unrelated to that group.
  if ((lifecycle != previous_lifecycle || transport != previous_transport) &&
      (lifecycle == LifecycleState::Faulted || transport_requires_failoff(transport)))
    return true;
  // Recovery is also global: a newer frame can restore transport health for
  // every message group, while each group still retains its own message and
  // freshness evidence.
  if (lifecycle == LifecycleState::Running &&
      (previous_lifecycle == LifecycleState::Faulted ||
       previous_transport == vehicle_core::TransportHealth::TimedOut ||
       previous_transport == vehicle_core::TransportHealth::Faulted) &&
      transport == vehicle_core::TransportHealth::Live)
    return true;
  return false;
}

} // namespace

const PollingDescriptorTuple &VehicleTelemetryService::polling_descriptors() noexcept {
  return kPollingDescriptors;
}

const NotificationDescriptorTuple &VehicleTelemetryService::notification_descriptors() noexcept {
  static const NotificationDescriptorTuple descriptors {
    {signal_ids::kSelectorPosition,
     "selector_position",
     &VehicleTelemetryService::selector_channel_,
     &VehicleState::selector_position,
     candidate::kGearId,
     candidate::kSelectorDefinition.confidence},
        {signal_ids::kActualGear,
         "actual_gear",
         &VehicleTelemetryService::actual_gear_channel_,
         &VehicleState::actual_gear,
         candidate::kGearId,
         candidate::kActualGearDefinition.confidence},
        {signal_ids::kTurnState,
         "turn_state",
         &VehicleTelemetryService::turn_channel_,
         &VehicleState::turn_state,
         candidate::kTurnSwitchId,
         candidate::kTurnLeftSwitchDefinition.confidence},
        {signal_ids::kHazardRequest,
         "hazard_request",
         &VehicleTelemetryService::hazard_channel_,
         &VehicleState::hazard_request,
         candidate::kTurnSwitchId,
         candidate::kHazardDefinition.confidence},
        {signal_ids::kTurnRequestLeft,
         "left_turn_request",
         &VehicleTelemetryService::left_turn_channel_,
         &VehicleState::left_turn_request,
         candidate::kTurnSwitchId,
         candidate::kTurnLeftSwitchDefinition.confidence},
        {signal_ids::kTurnRequestRight,
         "right_turn_request",
         &VehicleTelemetryService::right_turn_channel_,
         &VehicleState::right_turn_request,
         candidate::kTurnSwitchId,
         candidate::kTurnRightSwitchDefinition.confidence},
        {signal_ids::kLiftgateOpen,
         "liftgate_open",
         &VehicleTelemetryService::liftgate_channel_,
         &VehicleState::liftgate_open,
         candidate::kDoorsId,
         candidate::kLiftgateOpenDefinition.confidence},
        {signal_ids::kDoorRearRight,
         "rear_right_door_open",
         &VehicleTelemetryService::rear_right_door_channel_,
         &VehicleState::rear_right_door_open,
         candidate::kDoorsId,
         candidate::kRearRightDoorOpenDefinition.confidence},
        {signal_ids::kDoorRearLeft,
         "rear_left_door_open",
         &VehicleTelemetryService::rear_left_door_channel_,
         &VehicleState::rear_left_door_open,
         candidate::kDoorsId,
         candidate::kRearLeftDoorOpenDefinition.confidence},
        {signal_ids::kDoorFrontLeftRhd,
         "front_left_door_open_rhd",
         &VehicleTelemetryService::front_left_door_channel_,
         &VehicleState::front_left_door_open_rhd,
         candidate::kDoorsId,
         candidate::kFrontLeftDoorOpenRhdDefinition.confidence},
        {signal_ids::kDoorFrontRightRhd,
         "front_right_door_open_rhd",
         &VehicleTelemetryService::front_right_door_channel_,
         &VehicleState::front_right_door_open_rhd,
         candidate::kDoorsId,
         candidate::kFrontRightDoorOpenRhdDefinition.confidence},
        {signal_ids::kDoorsUnlocked,
         "doors_unlocked",
         &VehicleTelemetryService::doors_unlocked_channel_,
         &VehicleState::doors_unlocked,
         candidate::kDoorsId,
         candidate::kDoorsUnlockedDefinition.confidence},
        {signal_ids::kIndicatorLampLeft,
         "left_indicator_lamp",
         &VehicleTelemetryService::left_lamp_channel_,
         &VehicleState::left_indicator_lamp,
         candidate::kBlinkInfoId,
         candidate::kLeftIndicatorLampDefinition.confidence},
        {signal_ids::kIndicatorLampRight,
         "right_indicator_lamp",
         &VehicleTelemetryService::right_lamp_channel_,
         &VehicleState::right_indicator_lamp,
         candidate::kBlinkInfoId,
         candidate::kRightIndicatorLampDefinition.confidence},
        {signal_ids::kWiperLow,
         "wiper_low",
         &VehicleTelemetryService::wiper_low_channel_,
         &VehicleState::wiper_low,
         candidate::kBlinkInfoId,
         candidate::kWiperLowDefinition.confidence},
        {signal_ids::kWiperFrontPosition,
         "front_wiper",
         &VehicleTelemetryService::front_wiper_channel_,
         &VehicleState::front_wiper,
         candidate::kTurnSwitchId,
         candidate::kFrontWiperDefinition.confidence},
        {signal_ids::kBrakePressed,
         "brake_pressed",
         &VehicleTelemetryService::brake_channel_,
         &VehicleState::brake_pressed,
         candidate::kBrakePedalId,
         candidate::kBrakePressedDefinition.confidence},
#if !defined(ESP_PLATFORM)
    {
      vehicle_signals::SignalId{}, "test_front_wiper",
          &VehicleTelemetryService::test_front_wiper_channel_, &VehicleState::front_wiper,
          candidate::kTurnSwitchId, candidate::kFrontWiperDefinition.confidence
    }
#endif
  };
  return descriptors;
}

VehicleTelemetryService::VehicleTelemetryService() noexcept
#if defined(ESP_PLATFORM)
    : clock_(&steady_clock_), runtime_(can_bus_source_, *this, *this, steady_clock_),
      publication_(steady_clock_, TelemetryConfig{}), config_{} {
#else
    : clock_(&steady_clock_), runtime_(host_source_, *this, *this, steady_clock_),
      publication_(steady_clock_, TelemetryConfig{}), config_{} {
#endif
  initialize_registration_slots();
  processing_state_.apply_freshness_policy(config_.freshness);
  record_policy_application();
  configure_runtime(runtime_, config_);
}

VehicleTelemetryService::VehicleTelemetryService(vehicle_core::MonotonicClock &clock,
                                                 vehicle_telemetry::AcquisitionSource &source,
                                                 LightingSink &lighting_sink,
                                                 const TelemetryConfig config) noexcept
#if !defined(ESP_PLATFORM)
    : VehicleTelemetryService(clock, source, lighting_sink, config, HostServiceOptions{}) {
}

VehicleTelemetryService::VehicleTelemetryService(vehicle_core::MonotonicClock &clock,
                                                 vehicle_telemetry::AcquisitionSource &source,
                                                 LightingSink &lighting_sink,
                                                 const TelemetryConfig config,
                                                 const HostServiceOptions host_options) noexcept
#endif
    : clock_(&clock), lighting_sink_(&lighting_sink), runtime_(source, *this, *this, clock),
      publication_(clock, config), config_(config)
#if !defined(ESP_PLATFORM)
      ,
      host_options_(host_options)
#endif
{
  initialize_registration_slots();
  processing_state_.apply_freshness_policy(config_.freshness);
  record_policy_application();
  configure_runtime(runtime_, config_);
}

VehicleTelemetryService::ExecutionIdentity
VehicleTelemetryService::current_execution_identity() noexcept {
  // A monotonically assigned value is stable for this execution context but
  // remains unique after its TLS storage or FreeRTOS task handle is recycled.
  // The counter is bounded by uint64_t and this path performs no allocation.
  static thread_local const ExecutionIdentity execution_identity =
      g_next_execution_identity.fetch_add(1, std::memory_order_relaxed);
  return execution_identity;
}

bool VehicleTelemetryService::callback_mutation_rejected() const noexcept {
  return callback_context_identity_.load(std::memory_order_acquire) == current_execution_identity();
}

bool VehicleTelemetryService::claim_or_validate_lifecycle_owner() noexcept {
  if (lifecycle_owner_identity_ == kNoExecutionIdentity)
    lifecycle_owner_identity_ = current_execution_identity();
  return lifecycle_owner_identity_ == current_execution_identity();
}

VehicleTelemetryService::~VehicleTelemetryService() noexcept {
  if (lifecycle_state_.load(std::memory_order_acquire) != LifecycleState::Stopped) {
    (void)stop();
  }
#if !defined(ESP_PLATFORM)
  if (dispatcher_thread_.joinable()) {
    run_requested_.store(false, std::memory_order_release);
    while (!workers_done())
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    join_workers();
  }
#endif
}

bool VehicleTelemetryService::valid_config(const TelemetryConfig &config) noexcept {
  return config.transport_silence_timeout_us > 0 &&
         config.transport_silence_timeout_us <= kRuntimeMaximumSilenceTimeoutUs &&
         config.max_frames_per_batch > 0 && config.max_runnable_receive_calls > 0 &&
         config.max_runnable_receive_calls <= kRuntimeMaximumRunnableReceiveCalls &&
         config.max_frames_per_batch <= config.max_runnable_receive_calls &&
         config.max_batch_time_us > 0 && config.max_runnable_time_us > 0 &&
         config.max_batch_time_us <= kRuntimeMaximumRunnableTimeUs &&
         config.max_runnable_time_us <= kRuntimeMaximumRunnableTimeUs &&
         config.max_batch_time_us <= config.max_runnable_time_us && config.budget_pause_ms > 0 &&
         config.budget_pause_ms <= kRuntimeMaximumBudgetPauseMs &&
         config.availability_service_target_us > 0 &&
         config.availability_service_target_us <= kRuntimeMaximumReceiveTimeoutUs;
}

void VehicleTelemetryService::initialize_registration_slots() noexcept {
  std::size_t descriptor_index = 0;
  std::apply(
      [this, &descriptor_index](const auto &...descriptor) {
        (([&] {
           for (std::size_t slot = 0; slot < vehicle_core::kNotificationSubscribersPerChannel;
                ++slot) {
             auto &registration =
                 registrations_[descriptor_index *
                                    vehicle_core::kNotificationSubscribersPerChannel +
                                slot];
             registration.channel = std::decay_t<decltype(descriptor)>::Channel::channel_id();
             registration.slot = static_cast<std::uint8_t>(slot);
           }
           ++descriptor_index;
         }()),
         ...);
      },
      notification_descriptors());
}

VehicleTelemetryService::Registration *
VehicleTelemetryService::free_registration(const std::uint16_t channel) noexcept {
  for (auto &registration : registrations_) {
    if (registration.channel == channel && !registration.active)
      return &registration;
  }
  return nullptr;
}

VehicleTelemetryService::Registration *
VehicleTelemetryService::find_registration(const SubscriptionToken &token) noexcept {
  if (token.channel == kInvalidChannel)
    return nullptr;
  for (auto &registration : registrations_) {
    if (registration.active && registration.channel == token.channel &&
        registration.slot == token.slot && registration.generation == token.generation)
      return &registration;
  }
  return nullptr;
}

const VehicleTelemetryService::Registration *
VehicleTelemetryService::find_registration(const SubscriptionToken &token) const noexcept {
  if (token.channel == kInvalidChannel)
    return nullptr;
  for (const auto &registration : registrations_) {
    if (registration.active && registration.channel == token.channel &&
        registration.slot == token.slot && registration.generation == token.generation)
      return &registration;
  }
  return nullptr;
}

ResultCode VehicleTelemetryService::map_notification_status(
    const vehicle_core::NotificationStatus status) noexcept {
  switch (status) {
  case vehicle_core::NotificationStatus::Ok:
    return ResultCode::Ok;
  case vehicle_core::NotificationStatus::CapacityExceeded:
    return ResultCode::CapacityExceeded;
  case vehicle_core::NotificationStatus::InvalidSubscription:
    return ResultCode::InvalidSubscription;
  case vehicle_core::NotificationStatus::AlreadyRunning:
  case vehicle_core::NotificationStatus::NotRunning:
  case vehicle_core::NotificationStatus::InvalidState:
  case vehicle_core::NotificationStatus::InvalidCallback:
  case vehicle_core::NotificationStatus::NoPending:
    return ResultCode::InvalidState;
  }
  return ResultCode::InvalidState;
}

ResultCode
VehicleTelemetryService::map_runtime_status(const vehicle_telemetry::ResultCode status) noexcept {
  switch (status) {
  case vehicle_telemetry::ResultCode::Ok:
    return ResultCode::Ok;
  case vehicle_telemetry::ResultCode::InvalidConfiguration:
    return ResultCode::InvalidConfiguration;
  case vehicle_telemetry::ResultCode::AlreadyRunning:
    return ResultCode::AlreadyRunning;
  case vehicle_telemetry::ResultCode::NotRunning:
    return ResultCode::NotRunning;
  case vehicle_telemetry::ResultCode::Stopping:
    return ResultCode::Timeout;
  case vehicle_telemetry::ResultCode::Timeout:
    return ResultCode::Timeout;
  case vehicle_telemetry::ResultCode::CapacityExceeded:
    return ResultCode::CapacityExceeded;
  case vehicle_telemetry::ResultCode::InvalidState:
    return ResultCode::InvalidState;
  case vehicle_telemetry::ResultCode::Faulted:
    return ResultCode::Faulted;
  }
  return ResultCode::Faulted;
}

StatusResult VehicleTelemetryService::configure(const TelemetryConfig &config) noexcept {
  if (callback_mutation_rejected())
    return {ResultCode::InvalidState};
  std::lock_guard<std::mutex> lock{lifecycle_mutex_};
  if (!claim_or_validate_lifecycle_owner())
    return {ResultCode::InvalidState};
  if (lifecycle_state_.load(std::memory_order_acquire) != LifecycleState::Stopped)
    return {ResultCode::InvalidState};
  if (!valid_config(config))
    return {ResultCode::InvalidConfiguration};
  // Configure the backend before mutating either facade-side copy. This
  // keeps a rejected core budget or receive-target conversion from partially
  // applying to publication/configuration state.
  const auto runtime_result = configure_runtime(runtime_, config);
  if (!runtime_result.ok())
    return {map_runtime_status(runtime_result.status)};
  const auto result = publication_.configure(config);
  if (result.ok()) {
    record_policy_application();
    config_ = config;
    notification_due_observations_.fill(std::nullopt);
  }
  return result;
}

StatusResult VehicleTelemetryService::bind_lighting_sink(LightingSink &lighting_sink) noexcept {
  if (callback_mutation_rejected())
    return {ResultCode::InvalidState};
  std::lock_guard<std::mutex> lock{lifecycle_mutex_};
  if (!claim_or_validate_lifecycle_owner())
    return {ResultCode::InvalidState};
  if (lifecycle_state_.load(std::memory_order_acquire) != LifecycleState::Stopped)
    return {ResultCode::InvalidState};
  lighting_sink_ = &lighting_sink;
  return {ResultCode::Ok};
}

bool VehicleTelemetryService::start_channels() noexcept {
  bool result = true;
  std::apply(
      [this, &result](const auto &...descriptor) {
        ((result = result &&
                   ((this->*descriptor.channel).start() == vehicle_core::NotificationStatus::Ok)),
         ...);
      },
      notification_descriptors());
  return result;
}

bool VehicleTelemetryService::stop_channels() noexcept {
  bool result = true;
  std::apply(
      [this, &result](const auto &...descriptor) {
        ((result = ((this->*descriptor.channel).stop() !=
                    vehicle_core::NotificationStatus::InvalidState) &&
                   result),
         ...);
      },
      notification_descriptors());
  return result;
}

void VehicleTelemetryService::reset_publication(const Diagnostics &diagnostics) noexcept {
  publication_.reset(diagnostics);
  record_policy_application();
}

void VehicleTelemetryService::record_policy_application() noexcept {
#if defined(MAZDA_ENABLE_TELEMETRY_PROFILING)
  if (host_options_.profiler != nullptr && host_options_.profiler->policy_application != nullptr)
    host_options_.profiler->policy_application(host_options_.profiler->context);
#endif
}

void VehicleTelemetryService::record_worker_publication_lock() noexcept {
#if defined(MAZDA_ENABLE_TELEMETRY_PROFILING)
  if (host_options_.profiler != nullptr &&
      host_options_.profiler->worker_publication_lock != nullptr)
    host_options_.profiler->worker_publication_lock(host_options_.profiler->context);
#endif
}

void VehicleTelemetryService::record_lighting_evaluation() noexcept {
#if defined(MAZDA_ENABLE_TELEMETRY_PROFILING)
  if (host_options_.profiler != nullptr && host_options_.profiler->lighting_evaluation != nullptr)
    host_options_.profiler->lighting_evaluation(host_options_.profiler->context);
#endif
}

std::size_t VehicleTelemetryService::dispatch_channels_once() noexcept {
#if defined(MAZDA_ENABLE_TELEMETRY_PROFILING)
  if (host_options_.profiler != nullptr &&
      host_options_.profiler->notification_dispatch_begin != nullptr)
    host_options_.profiler->notification_dispatch_begin(host_options_.profiler->context);
#endif
  const std::size_t index =
      dispatch_cursor_.fetch_add(1, std::memory_order_relaxed) % kNotificationChannelCount;
  std::size_t delivered = 0;
  std::size_t descriptor_index = 0;
  std::apply(
      [this, index, &descriptor_index, &delivered](const auto &...descriptor) {
        (((descriptor_index++ == index)
              ? static_cast<void>(delivered = (this->*descriptor.channel).dispatch_pending(1))
              : static_cast<void>(0)),
         ...);
      },
      notification_descriptors());
#if defined(MAZDA_ENABLE_TELEMETRY_PROFILING)
  if (host_options_.profiler != nullptr &&
      host_options_.profiler->notification_dispatch_end != nullptr)
    host_options_.profiler->notification_dispatch_end(host_options_.profiler->context, delivered);
#endif
  return delivered;
}

#if !defined(ESP_PLATFORM)
Result<std::size_t> VehicleTelemetryService::drain_notifications() noexcept {
  if (callback_mutation_rejected())
    return {ResultCode::InvalidState, std::nullopt};
  {
    std::lock_guard<std::mutex> lock{lifecycle_mutex_};
    if (!host_options_.manual_notification_dispatch || !claim_or_validate_lifecycle_owner())
      return {ResultCode::InvalidState, std::nullopt};
    const auto lifecycle = lifecycle_state_.load(std::memory_order_acquire);
    if (lifecycle != LifecycleState::Running && lifecycle != LifecycleState::Faulted)
      return {ResultCode::InvalidState, std::nullopt};
  }

  callback_context_identity_.store(current_execution_identity(), std::memory_order_release);
  std::size_t delivered = 0;
  std::size_t sweep_delivered = 0;
  do {
    sweep_delivered = 0;
    for (std::size_t index = 0; index < kNotificationChannelCount; ++index)
      sweep_delivered += dispatch_channels_once();
    delivered += sweep_delivered;
    dispatch_progress_.fetch_add(1, std::memory_order_relaxed);
  } while (sweep_delivered != 0);
  callback_context_identity_.store(kNoExecutionIdentity, std::memory_order_release);
  return {ResultCode::Ok, delivered};
}
#endif

StatusResult VehicleTelemetryService::start() noexcept {
  if (callback_mutation_rejected())
    return {ResultCode::InvalidState};
  std::lock_guard<std::mutex> lock{lifecycle_mutex_};
  if (!claim_or_validate_lifecycle_owner())
    return {ResultCode::InvalidState};
  const auto current = lifecycle_state_.load(std::memory_order_acquire);
  if (current == LifecycleState::Running)
    return {ResultCode::AlreadyRunning};
  if (current != LifecycleState::Stopped)
    return {current == LifecycleState::Faulted ? ResultCode::Faulted : ResultCode::InvalidState};
  if (!valid_config(config_))
    return {ResultCode::InvalidConfiguration};

  processing_state_ = VehicleState{};
  processing_state_.apply_freshness_policy(config_.freshness);
  record_policy_application();
  notification_due_observations_.fill(std::nullopt);
  pending_observation_identifier_.reset();
  published_health_initialized_ = false;
  last_transport_receive_us_.reset();
  runtime_diagnostics_ = {};
#if defined(MAZDA_TELEMETRY_DEBUG_ENABLED)
  debug_recorder_.reset();
  debug_frame_pending_ = false;
  debug_frame_recorded_ = false;
  debug_pending_frame_ = {};
  debug_pending_status_ = vehicle_telemetry::ProcessStatus::Ignored;
  debug_pending_processing_timestamp_us_ = 0;
  debug_pending_update_not_advanced_ = false;
  debug_global_event_pending_ = false;
  debug_due_observation_.reset();
  debug_diagnostics_sample_timestamp_us_ = 0;
  debug_diagnostics_sample_source_ = DebugSampleSource::None;
  debug_observer_frame_callback_ = false;
  debug_last_aggregate_refresh_us_ = 0;
#endif
  lighting_sent_ = false;
  lighting_failure_ = false;
  const auto now_us = clock_->now();
  publish_startup_black(now_us);
  runtime_diagnostics_.lifecycle = vehicle_telemetry::LifecycleState::Running;
  runtime_diagnostics_.transport = vehicle_core::TransportHealth::AwaitingTraffic;
  lifecycle_state_.store(LifecycleState::Running, std::memory_order_release);
  reset_publication(Diagnostics{LifecycleState::Running,
                                vehicle_core::TransportHealth::AwaitingTraffic,
                                AcquisitionMetrics{}});
  if (!start_channels()) {
    lifecycle_state_.store(LifecycleState::Faulted, std::memory_order_release);
    reset_publication(Diagnostics{LifecycleState::Faulted, vehicle_core::TransportHealth::Faulted,
                                  AcquisitionMetrics{}});
    return {ResultCode::Faulted};
  }

  // Establish the coherent startup publication only after channels are
  // active, but before Runtime starts its receive worker. Runtime callbacks
  // can therefore never overwrite this handoff with a late startup write.
  publish_current(false);

  const auto runtime_result = runtime_.start();
  if (!runtime_result.ok()) {
    (void)stop_channels();
    const auto mapped = map_runtime_status(runtime_result.status);
    const auto failure = runtime_.lifecycle() == vehicle_telemetry::LifecycleState::Faulted
                             ? LifecycleState::Faulted
                             : LifecycleState::Stopped;
    lifecycle_state_.store(failure, std::memory_order_release);
    reset_publication(Diagnostics{failure,
                                  failure == LifecycleState::Faulted
                                      ? vehicle_core::TransportHealth::Faulted
                                      : vehicle_core::TransportHealth::Stopped,
                                  AcquisitionMetrics{}});
    return {mapped};
  }

  run_requested_.store(true, std::memory_order_release);
  dispatch_cursor_.store(0, std::memory_order_relaxed);
#if !defined(ESP_PLATFORM)
  if (host_options_.manual_notification_dispatch) {
    dispatcher_done_.store(true, std::memory_order_release);
    return {ResultCode::Ok};
  }
#endif
  dispatcher_done_.store(false, std::memory_order_release);
#if defined(ESP_PLATFORM)
  if (xTaskCreate(&VehicleTelemetryService::dispatcher_task_entry, "mazda_notify", 4096, this,
                  configMAX_PRIORITIES - 4,
                  reinterpret_cast<TaskHandle_t *>(&dispatcher_task_)) != pdPASS) {
    run_requested_.store(false, std::memory_order_release);
    (void)runtime_.stop();
    dispatcher_done_.store(true, std::memory_order_release);
    (void)stop_channels();
    lifecycle_state_.store(LifecycleState::Faulted, std::memory_order_release);
    reset_publication(Diagnostics{LifecycleState::Faulted, vehicle_core::TransportHealth::Faulted,
                                  AcquisitionMetrics{}});
    return {ResultCode::Faulted};
  }
#else
  bool dispatcher_started = false;
  try {
    dispatcher_thread_ = std::thread([this] {
      dispatcher_loop();
      dispatcher_done_.store(true, std::memory_order_release);
    });
    dispatcher_started = true;
  } catch (...) {
    run_requested_.store(false, std::memory_order_release);
    (void)runtime_.stop();
    if (!dispatcher_started)
      dispatcher_done_.store(true, std::memory_order_release);
    (void)wait_for_workers(config_.callback_stop_timeout_us);
    join_workers();
    (void)stop_channels();
    lifecycle_state_.store(LifecycleState::Faulted, std::memory_order_release);
    reset_publication(Diagnostics{LifecycleState::Faulted, vehicle_core::TransportHealth::Faulted,
                                  AcquisitionMetrics{}});
    return {ResultCode::Faulted};
  }
#endif
  return {ResultCode::Ok};
}

StatusResult VehicleTelemetryService::stop() noexcept {
  if (callback_mutation_rejected())
    return {ResultCode::InvalidState};
  bool was_faulted = false;
  {
    std::lock_guard<std::mutex> lock{lifecycle_mutex_};
    if (!claim_or_validate_lifecycle_owner())
      return {ResultCode::InvalidState};
    const auto current = lifecycle_state_.load(std::memory_order_acquire);
    if (current == LifecycleState::Stopped)
      return {ResultCode::NotRunning};
    was_faulted = current == LifecycleState::Faulted;
    lifecycle_state_.store(LifecycleState::Stopping, std::memory_order_release);
    run_requested_.store(false, std::memory_order_release);
  }
  const auto runtime_result = runtime_.stop();
  const bool runtime_stopped =
      runtime_result.ok() || runtime_result.status == vehicle_telemetry::ResultCode::NotRunning;
  const auto stopping_diagnostics = runtime_.diagnostics();
  publication_.publish_diagnostics(Diagnostics{
      LifecycleState::Stopping, vehicle_core::TransportHealth::Stopped,
      acquisition_metrics(stopping_diagnostics), stopping_diagnostics.work_budget_pauses});
  if (!wait_for_workers(config_.callback_stop_timeout_us))
    return {ResultCode::Timeout};
  join_workers();
  const bool channels_stopped = stop_channels();
  if (!runtime_stopped || !channels_stopped) {
    lifecycle_state_.store(LifecycleState::Faulted, std::memory_order_release);
    return {!runtime_stopped ? map_runtime_status(runtime_result.status) : ResultCode::Faulted};
  }
  runtime_diagnostics_ = runtime_.diagnostics();
  const auto acquisition = acquisition_metrics(runtime_diagnostics_);
  reset_publication(Diagnostics{LifecycleState::Stopped, vehicle_core::TransportHealth::Stopped,
                                acquisition, runtime_diagnostics_.work_budget_pauses});
  processing_state_ = VehicleState{};
  processing_state_changed_ = false;
  notification_due_observations_.fill(std::nullopt);
  pending_observation_identifier_.reset();
  published_lifecycle_ = LifecycleState::Stopped;
  published_transport_ = vehicle_core::TransportHealth::Stopped;
  published_health_initialized_ = true;
  last_transport_receive_us_.reset();
  lifecycle_state_.store(LifecycleState::Stopped, std::memory_order_release);
  return {was_faulted ? ResultCode::Faulted : ResultCode::Ok};
}

Diagnostics VehicleTelemetryService::diagnostics() const noexcept {
  auto diagnostics = publication_.diagnostics();
  const auto runtime_diagnostics = runtime_.diagnostics();
  diagnostics.lifecycle =
      runtime_diagnostics.lifecycle == vehicle_telemetry::LifecycleState::Running
          ? LifecycleState::Running
      : runtime_diagnostics.lifecycle == vehicle_telemetry::LifecycleState::Stopping
          ? LifecycleState::Stopping
      : runtime_diagnostics.lifecycle == vehicle_telemetry::LifecycleState::Faulted
          ? LifecycleState::Faulted
          : LifecycleState::Stopped;
  diagnostics.transport = runtime_diagnostics.transport;
  diagnostics.acquisition = acquisition_metrics(runtime_diagnostics);
  diagnostics.work_budget_pauses = runtime_diagnostics.work_budget_pauses;
  return diagnostics;
}

DebugSnapshot VehicleTelemetryService::debug_snapshot() const noexcept {
#if defined(MAZDA_TELEMETRY_DEBUG_ENABLED)
  return debug_recorder_.snapshot();
#else
  return {};
#endif
}

DebugSnapshot VehicleTelemetryService::debug_stale_snapshot() const noexcept {
#if defined(MAZDA_TELEMETRY_DEBUG_ENABLED)
  return debug_recorder_.stale_snapshot();
#else
  return {};
#endif
}

TelemetryProfileSnapshot VehicleTelemetryService::telemetry_profile_snapshot() const noexcept {
#if defined(CONFIG_WEACT_CAN_TELEMETRY_PROFILING)
  return telemetry_profiler_.snapshot();
#else
  return {};
#endif
}

#if defined(CONFIG_WEACT_CAN_TELEMETRY_PROFILING)
void VehicleTelemetryService::profile_stage(
    const TelemetryProfileStage stage, const vehicle_core::MonotonicTimestamp started_us,
    const vehicle_core::MonotonicTimestamp ended_us) noexcept {
  telemetry_profiler_.record(stage, started_us, ended_us);
}
#endif

void VehicleTelemetryService::reset() noexcept {
  processing_state_ = VehicleState{};
  processing_state_.apply_freshness_policy(config_.freshness);
  record_policy_application();
  processing_state_changed_ = false;
  notification_due_observations_.fill(std::nullopt);
  pending_observation_identifier_.reset();
  last_transport_receive_us_.reset();
  runtime_diagnostics_ = {};
#if defined(CONFIG_WEACT_CAN_TELEMETRY_PROFILING)
  telemetry_profiler_.reset(clock_->now());
  profile_total_active_ = false;
  profile_total_started_us_ = 0;
  profile_diagnostics_active_ = false;
  profile_diagnostics_started_us_ = 0;
#endif
#if defined(MAZDA_TELEMETRY_DEBUG_ENABLED)
  debug_recorder_.reset();
  debug_frame_pending_ = false;
  debug_frame_recorded_ = false;
  debug_pending_frame_ = {};
  debug_pending_status_ = vehicle_telemetry::ProcessStatus::Ignored;
  debug_pending_processing_timestamp_us_ = 0;
  debug_pending_update_not_advanced_ = false;
  debug_global_event_pending_ = false;
  debug_due_observation_.reset();
  debug_diagnostics_sample_timestamp_us_ = 0;
  debug_diagnostics_sample_source_ = DebugSampleSource::None;
  debug_observer_frame_callback_ = false;
  debug_last_aggregate_refresh_us_ = 0;
#endif
  lighting_sent_ = false;
  lighting_failure_ = false;
}

vehicle_telemetry::ProcessResult
VehicleTelemetryService::process(const vehicle_core::RawCanFrame &frame) noexcept {
#if defined(CONFIG_WEACT_CAN_TELEMETRY_PROFILING)
  const auto process_started_us = clock_->now();
  profile_total_started_us_ = process_started_us;
  profile_total_active_ = true;
  profile_stage(TelemetryProfileStage::QueueWait, frame.timestamp_us, process_started_us);
  const auto pre_decode_started_us = process_started_us;
#endif
#if defined(MAZDA_TELEMETRY_DEBUG_ENABLED)
  const bool is_turn_switch = frame.identifier == candidate::kTurnSwitchId;
  const bool turn_had_value = processing_state_.turn_state.has_value;
  const auto turn_last_update_us = processing_state_.turn_state.last_update_us;
#endif
  // Service freshness before decoding. A frame can arrive after a signal's
  // deadline but before the previous unavailable notice has been dispatched;
  // publishing the pre-observation state first preserves both the due
  // transition and the newer recovery in NotificationChannel's coalesced
  // evidence.
  pending_observation_identifier_ = frame.identifier;
  service_due_notifications(clock_->now());
#if defined(CONFIG_WEACT_CAN_TELEMETRY_PROFILING)
  profile_stage(TelemetryProfileStage::PreDecodeDue, pre_decode_started_us, clock_->now());
  const auto decode_started_us = clock_->now();
#endif
#if defined(MAZDA_ENABLE_TELEMETRY_PROFILING)
  if (host_options_.profiler != nullptr && host_options_.profiler->decode_begin != nullptr)
    host_options_.profiler->decode_begin(host_options_.profiler->context);
#endif
  // The service consumes only the decoder status and updated processing state.
  // Edge, observation, and per-frame health are optional decoder diagnostics;
  // omitting them avoids constructing and populating discarded outputs while
  // leaving direct decoder callers' contracts unchanged.
  const auto before = message_mutation_marker(processing_state_, frame.identifier);
  const auto status = candidate::decode(frame, processing_state_);
  const auto after = message_mutation_marker(processing_state_, frame.identifier);
  processing_state_changed_ = processing_state_changed_ || before.has_frame != after.has_frame ||
                              before.last_frame_us != after.last_frame_us;
  vehicle_telemetry::ProcessResult result{};
  switch (status) {
  case vehicle_core::DecodeValidity::Decoded:
    result = {vehicle_telemetry::ProcessStatus::Processed};
    break;
  case vehicle_core::DecodeValidity::Malformed:
    result = {vehicle_telemetry::ProcessStatus::Malformed};
    break;
  case vehicle_core::DecodeValidity::Ignored:
    result = {vehicle_telemetry::ProcessStatus::Ignored};
    break;
  default:
    result = {vehicle_telemetry::ProcessStatus::Fault};
    break;
  }
#if defined(CONFIG_WEACT_CAN_TELEMETRY_PROFILING)
  profile_stage(TelemetryProfileStage::Decode, decode_started_us, clock_->now());
#endif
#if defined(MAZDA_TELEMETRY_DEBUG_ENABLED)
  if (is_turn_switch) {
    const bool update_not_advanced =
        !processing_state_.turn_state.has_value ||
        (turn_had_value && processing_state_.turn_state.last_update_us <= turn_last_update_us);
    debug_pending_frame_ = frame;
    debug_pending_status_ = result.status;
    debug_pending_processing_timestamp_us_ = clock_->now();
    debug_pending_update_not_advanced_ = update_not_advanced;
    debug_frame_recorded_ = debug_recorder_.record_processed(
        debug_pending_frame_, debug_pending_status_, debug_pending_processing_timestamp_us_,
        debug_pending_update_not_advanced_);
    debug_frame_pending_ = true;
  }
#endif
#if defined(MAZDA_ENABLE_TELEMETRY_PROFILING)
  if (host_options_.profiler != nullptr && host_options_.profiler->decode_end != nullptr)
    host_options_.profiler->decode_end(host_options_.profiler->context, frame, result.status);
#endif
#if defined(CONFIG_WEACT_CAN_TELEMETRY_PROFILING)
  // Runtime performs its diagnostics snapshot and observer handoff after
  // process() returns. Starting here includes that work and the callback
  // handoff while keeping the span disjoint from publication below.
  profile_diagnostics_started_us_ = clock_->now();
  profile_diagnostics_active_ = true;
#endif
  return result;
}

void VehicleTelemetryService::on_frame_processed(
    const vehicle_core::RawCanFrame &frame,
    const vehicle_telemetry::ProcessResult &result) noexcept {
#if defined(MAZDA_TELEMETRY_DEBUG_ENABLED)
  debug_observer_frame_callback_ = true;
#endif
  // Runtime invokes on_diagnostics immediately after this callback. Keeping
  // publication there gives Mazda consumers one coherent state/transport
  // snapshot and avoids a second receive worker in this component.
#if defined(MAZDA_ENABLE_TELEMETRY_PROFILING)
  if (host_options_.profiler != nullptr && host_options_.profiler->frame_processed != nullptr)
    host_options_.profiler->frame_processed(host_options_.profiler->context, frame, result.status);
#else
  (void)frame;
  (void)result;
#endif
}

void VehicleTelemetryService::on_diagnostics(
    const vehicle_telemetry::TransportDiagnostics &diagnostics) noexcept {
#if defined(CONFIG_WEACT_CAN_TELEMETRY_PROFILING)
  const auto diagnostics_started_us =
      profile_diagnostics_active_ ? profile_diagnostics_started_us_ : clock_->now();
  profile_diagnostics_active_ = false;
#endif
#if defined(MAZDA_ENABLE_TELEMETRY_PROFILING)
  if (host_options_.profiler != nullptr && host_options_.profiler->diagnostics_begin != nullptr)
    host_options_.profiler->diagnostics_begin(host_options_.profiler->context);
#endif
  runtime_diagnostics_ = diagnostics;
  lifecycle_state_.store(diagnostics.lifecycle == vehicle_telemetry::LifecycleState::Running
                             ? LifecycleState::Running
                         : diagnostics.lifecycle == vehicle_telemetry::LifecycleState::Stopping
                             ? LifecycleState::Stopping
                         : diagnostics.lifecycle == vehicle_telemetry::LifecycleState::Faulted
                             ? LifecycleState::Faulted
                             : LifecycleState::Stopped,
                         std::memory_order_release);
  if (diagnostics.has_last_frame)
    last_transport_receive_us_ = diagnostics.last_frame_us;
#if defined(MAZDA_TELEMETRY_DEBUG_ENABLED)
  debug_diagnostics_sample_timestamp_us_ = clock_->now();
  debug_diagnostics_sample_source_ = debug_observer_frame_callback_
                                         ? DebugSampleSource::TelemetryObserverFrame
                                         : DebugSampleSource::TelemetryObserverTimeout;
  debug_observer_frame_callback_ = false;
  // Runtime reports receive timeouts as diagnostics even when no frame is
  // available. Capture the due transition without dispatching production
  // notifications; debug recording must not change policy semantics.
  record_debug_due_snapshot(clock_->now());
#endif
#if defined(CONFIG_WEACT_CAN_TELEMETRY_PROFILING)
  profile_stage(TelemetryProfileStage::Diagnostics, diagnostics_started_us, clock_->now());
#endif
  publish_current(diagnostics.has_last_frame);
#if defined(CONFIG_WEACT_CAN_TELEMETRY_PROFILING)
  if (profile_total_active_) {
    profile_stage(TelemetryProfileStage::Total, profile_total_started_us_, clock_->now());
    profile_total_active_ = false;
  }
  telemetry_profiler_.refresh(clock_->now());
#endif
#if defined(MAZDA_ENABLE_TELEMETRY_PROFILING)
  if (host_options_.profiler != nullptr && host_options_.profiler->diagnostics_end != nullptr)
    host_options_.profiler->diagnostics_end(host_options_.profiler->context);
#endif
#if !defined(ESP_PLATFORM)
  if (host_options_.publication_control != nullptr)
    host_options_.publication_control->publication_completed();
#endif
}

void VehicleTelemetryService::dispatcher_loop() noexcept {
  while (run_requested_.load(std::memory_order_acquire)) {
    // NotificationChannel releases its internal lock before invoking the
    // callback. Keep a service-level execution marker around that bounded
    // dispatch so facade mutations made directly by the callback can reject
    // before lifecycle state or the acquisition source is touched.
    callback_context_identity_.store(current_execution_identity(), std::memory_order_release);
    const std::size_t delivered = dispatch_channels_once();
    callback_context_identity_.store(kNoExecutionIdentity, std::memory_order_release);
    // Record loop progress, not delivery: an idle pass under stable vehicle
    // state is healthy. Only a pass stuck in a callback stops this count.
    // Relaxed: readers only compare successive values; nothing is published
    // through this count.
    dispatch_progress_.fetch_add(1, std::memory_order_relaxed);
    if (delivered == 0) {
#if defined(ESP_PLATFORM)
      vTaskDelay(kMinimumTaskDelayTicks);
#else
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
#endif
    }
  }
}

#if defined(ESP_PLATFORM)
void VehicleTelemetryService::dispatcher_task_entry(void *context) noexcept {
  auto *service = static_cast<VehicleTelemetryService *>(context);
  service->dispatcher_loop();
  service->dispatcher_done_.store(true, std::memory_order_release);
  service->dispatcher_task_ = nullptr;
  vTaskDelete(nullptr);
}

#endif

void VehicleTelemetryService::publish_current(const bool received_frame) noexcept {
  const auto acquisition = acquisition_metrics(runtime_diagnostics_);
  const auto lifecycle = lifecycle_state_.load(std::memory_order_acquire);
  const Diagnostics diagnostics{lifecycle, runtime_diagnostics_.transport, acquisition,
                                runtime_diagnostics_.work_budget_pauses};
  const bool global_health_transition =
      requires_global_health_evaluation(published_lifecycle_, published_transport_, lifecycle,
                                        diagnostics.transport, published_health_initialized_);
#if defined(CONFIG_WEACT_CAN_TELEMETRY_PROFILING)
  const auto publication_started_us = clock_->now();
#endif
#if defined(MAZDA_ENABLE_TELEMETRY_PROFILING)
  if (host_options_.profiler != nullptr && host_options_.profiler->publication_begin != nullptr)
    host_options_.profiler->publication_begin(host_options_.profiler->context);
#endif
  if (processing_state_changed_) {
    publication_.publish(processing_state_, diagnostics,
                         received_frame ? last_transport_receive_us_ : std::nullopt);
    processing_state_changed_ = false;
#if defined(MAZDA_ENABLE_TELEMETRY_PROFILING)
    if (host_options_.profiler != nullptr && host_options_.profiler->state_copy != nullptr)
      host_options_.profiler->state_copy(host_options_.profiler->context);
#endif
  } else {
    publication_.publish_diagnostics(diagnostics);
  }
  record_worker_publication_lock();
#if defined(CONFIG_WEACT_CAN_TELEMETRY_PROFILING)
  profile_stage(TelemetryProfileStage::Publication, publication_started_us, clock_->now());
#endif
#if defined(MAZDA_ENABLE_TELEMETRY_PROFILING)
  if (host_options_.profiler != nullptr && host_options_.profiler->publication_end != nullptr)
    host_options_.profiler->publication_end(host_options_.profiler->context);
  if (host_options_.profiler != nullptr &&
      host_options_.profiler->notification_evaluation_begin != nullptr)
    host_options_.profiler->notification_evaluation_begin(host_options_.profiler->context);
#endif
  const auto now_us = clock_->now();
#if defined(MAZDA_TELEMETRY_DEBUG_ENABLED)
  if (debug_frame_pending_ && !debug_frame_recorded_) {
    debug_frame_recorded_ = debug_recorder_.record_processed(
        debug_pending_frame_, debug_pending_status_, debug_pending_processing_timestamp_us_,
        debug_pending_update_not_advanced_);
  }
  if (global_health_transition)
    debug_global_event_pending_ = true;
  const bool aggregate_refresh_due =
      debug_last_aggregate_refresh_us_ == 0 ||
      now_us >= saturating_add(debug_last_aggregate_refresh_us_, kDebugAggregateRefreshPeriodUs);
  if ((debug_frame_pending_ && debug_frame_recorded_) || debug_global_event_pending_ ||
      aggregate_refresh_due) {
#if defined(ESP_PLATFORM)
    const auto esp_timer_us = static_cast<vehicle_core::MonotonicTimestamp>(esp_timer_get_time());
#else
    const auto esp_timer_us = now_us;
#endif
    const bool recorded = debug_recorder_.record_published(
        processing_state_, diagnostics, now_us, now_us, esp_timer_us, now_us,
        last_transport_receive_us_, debug_diagnostics_sample_timestamp_us_,
        debug_diagnostics_sample_source_);
    if (recorded) {
      debug_frame_pending_ = false;
      debug_frame_recorded_ = false;
      debug_global_event_pending_ = false;
      debug_last_aggregate_refresh_us_ = now_us;
    }
  }
#endif
  const auto evaluations =
      publish_notifications(processing_state_, diagnostics, now_us, pending_observation_identifier_,
                            global_health_transition, true);
  pending_observation_identifier_.reset();
#if defined(MAZDA_ENABLE_TELEMETRY_PROFILING)
  if (host_options_.profiler != nullptr &&
      host_options_.profiler->notification_evaluation_end != nullptr)
    host_options_.profiler->notification_evaluation_end(host_options_.profiler->context,
                                                        evaluations);
#else
  (void)evaluations;
#endif
  published_lifecycle_ = lifecycle;
  published_transport_ = diagnostics.transport;
  published_health_initialized_ = true;
}

void VehicleTelemetryService::service_due_notifications(
    const vehicle_core::MonotonicTimestamp now_us) noexcept {
  if (lifecycle_state_.load(std::memory_order_acquire) != LifecycleState::Running)
    return;
  const auto acquisition = acquisition_metrics(runtime_diagnostics_);
  const Diagnostics diagnostics{LifecycleState::Running, runtime_diagnostics_.transport,
                                acquisition};
#if defined(MAZDA_ENABLE_TELEMETRY_PROFILING)
  if (host_options_.profiler != nullptr &&
      host_options_.profiler->notification_evaluation_begin != nullptr)
    host_options_.profiler->notification_evaluation_begin(host_options_.profiler->context);
#endif
  const auto evaluations =
      publish_notifications(processing_state_, diagnostics, now_us, std::nullopt, false, false);
#if defined(MAZDA_TELEMETRY_DEBUG_ENABLED)
  record_debug_due_snapshot(now_us);
#endif
#if defined(MAZDA_ENABLE_TELEMETRY_PROFILING)
  if (host_options_.profiler != nullptr &&
      host_options_.profiler->notification_evaluation_end != nullptr)
    host_options_.profiler->notification_evaluation_end(host_options_.profiler->context,
                                                        evaluations);
#else
  (void)evaluations;
#endif
}

#if defined(MAZDA_TELEMETRY_DEBUG_ENABLED)
void VehicleTelemetryService::record_debug_due_snapshot(
    const vehicle_core::MonotonicTimestamp now_us) noexcept {
  if (lifecycle_state_.load(std::memory_order_acquire) != LifecycleState::Running)
    return;
  const auto acquisition = acquisition_metrics(runtime_diagnostics_);
  const Diagnostics diagnostics{LifecycleState::Running, runtime_diagnostics_.transport,
                                acquisition};
  const auto &turn_signal = processing_state_.turn_state;
  const bool turn_due =
      turn_signal.has_value && turn_signal.freshness_timeout_us &&
      now_us > saturating_add(turn_signal.last_update_us, *turn_signal.freshness_timeout_us);
  if (turn_due &&
      (!debug_due_observation_ || *debug_due_observation_ != turn_signal.last_update_us)) {
#if defined(ESP_PLATFORM)
    const auto esp_timer_us = static_cast<vehicle_core::MonotonicTimestamp>(esp_timer_get_time());
#else
    const auto esp_timer_us = now_us;
#endif
    if (debug_recorder_.record_published(processing_state_, diagnostics, now_us, now_us,
                                         esp_timer_us, now_us, last_transport_receive_us_,
                                         debug_diagnostics_sample_timestamp_us_,
                                         debug_diagnostics_sample_source_))
      debug_due_observation_ = turn_signal.last_update_us;
  }
}
#endif

template <typename T, std::uint16_t ChannelId>
bool VehicleTelemetryService::notification_descriptor_due(
    const VehicleState &state, const vehicle_core::MonotonicTimestamp now_us,
    const NotificationDescriptor<T, ChannelId> &descriptor,
    const std::size_t descriptor_index) const noexcept {
  // The host extension descriptor intentionally has no catalog id and is
  // excluded from production due scheduling. Its production twin owns the
  // same signal and provides the authoritative group.
  if (!descriptor.id.valid())
    return false;
  const auto &signal = state.*descriptor.signal;
  if (!signal.has_value || !signal.freshness_timeout_us)
    return false;
  const auto deadline = saturating_add(signal.last_update_us, *signal.freshness_timeout_us);
  // Signal freshness is inclusive: a reading remains Fresh at its deadline
  // and becomes due only after it.
  if (now_us <= deadline)
    return false;
  const auto &serviced = notification_due_observations_[descriptor_index];
  return !serviced || *serviced != signal.last_update_us;
}

template <typename T, std::uint16_t ChannelId>
void VehicleTelemetryService::publish_notification_descriptor(
    const VehicleState &state, const Diagnostics &diagnostics,
    const vehicle_core::MonotonicTimestamp now_us,
    const NotificationDescriptor<T, ChannelId> &descriptor) noexcept {
  (void)(this->*descriptor.channel)
      .publish(state.reading_at(state.*descriptor.signal, descriptor.identifier, now_us,
                                descriptor.validation, diagnostics.transport));
}

std::size_t VehicleTelemetryService::publish_notifications(
    const VehicleState &state, const Diagnostics &diagnostics,
    const vehicle_core::MonotonicTimestamp now_us,
    const std::optional<std::uint32_t> affected_identifier, const bool global_health_transition,
    const bool include_lighting) noexcept {
  std::array<std::uint32_t, kNotificationChannelCount> due_identifiers{};
  std::size_t due_identifier_count = 0;
  std::size_t descriptor_index = 0;
#if defined(CONFIG_WEACT_CAN_TELEMETRY_PROFILING)
  const auto notification_started_us = include_lighting ? clock_->now() : 0;
#endif
  std::apply(
      [this, &state, now_us, &due_identifiers, &due_identifier_count,
       &descriptor_index](const auto &...descriptor) {
        (([&] {
           if (notification_descriptor_due(state, now_us, descriptor, descriptor_index)) {
             const bool already_recorded =
                 std::find(due_identifiers.begin(), due_identifiers.begin() + due_identifier_count,
                           descriptor.identifier) != due_identifiers.begin() + due_identifier_count;
             if (!already_recorded)
               due_identifiers[due_identifier_count++] = descriptor.identifier;
           }
           ++descriptor_index;
         }()),
         ...);
      },
      notification_descriptors());

  std::size_t evaluations = 0;
  descriptor_index = 0;
  std::apply(
      [this, &state, &diagnostics, now_us, affected_identifier, global_health_transition,
       &due_identifiers, due_identifier_count, &descriptor_index,
       &evaluations](const auto &...descriptor) {
        (([&] {
           const bool affected =
               affected_identifier && descriptor.identifier == *affected_identifier;
           const bool due =
               std::find(due_identifiers.begin(), due_identifiers.begin() + due_identifier_count,
                         descriptor.identifier) != due_identifiers.begin() + due_identifier_count;
           if (global_health_transition || affected || due) {
             publish_notification_descriptor(state, diagnostics, now_us, descriptor);
             // Host-only extension descriptors share a production message
             // group but have no catalog id. They still receive the update;
             // profiler counts describe released catalog work only.
             if (descriptor.id.valid()) {
               ++evaluations;
#if defined(MAZDA_ENABLE_TELEMETRY_PROFILING)
               if (host_options_.profiler != nullptr &&
                   host_options_.profiler->notification_descriptor_evaluation != nullptr)
                 host_options_.profiler->notification_descriptor_evaluation(
                     host_options_.profiler->context, descriptor.identifier);
#endif
             }
             if (notification_descriptor_due(state, now_us, descriptor, descriptor_index))
               notification_due_observations_[descriptor_index] =
                   (state.*descriptor.signal).last_update_us;
           }
           ++descriptor_index;
         }()),
         ...);
      },
      notification_descriptors());
#if defined(CONFIG_WEACT_CAN_TELEMETRY_PROFILING)
  if (include_lighting)
    profile_stage(TelemetryProfileStage::NotificationEvaluation, notification_started_us,
                  clock_->now());
#endif
  if (include_lighting)
#if defined(CONFIG_WEACT_CAN_TELEMETRY_PROFILING)
  {
    const auto lighting_started_us = clock_->now();
    publish_lighting(state, diagnostics, now_us);
    profile_stage(TelemetryProfileStage::LightingEvaluation, lighting_started_us, clock_->now());
  }
#else
    publish_lighting(state, diagnostics, now_us);
#endif
  return evaluations;
}

void VehicleTelemetryService::publish_lighting(
    const VehicleState &state, const Diagnostics &diagnostics,
    const vehicle_core::MonotonicTimestamp now_us) noexcept {
  if (lighting_sink_ == nullptr)
    return;
  record_lighting_evaluation();

  const auto turn_reading =
      notification_reading(state, diagnostics, kTurnNotificationDescriptor, now_us);
  const auto turn_health =
      state.health_observation(candidate::kTurnSwitchId, diagnostics.transport);
  const auto brake_reading = state.reading_at(state.brake_pressed, candidate::kBrakePedalId, now_us,
                                              ValidationStatus::Confirmed, diagnostics.transport);
  const bool actionable = is_available_reading(turn_reading) &&
                          *turn_reading.value != TurnState::Unknown &&
                          diagnostics.transport != vehicle_core::TransportHealth::Stopped &&
                          diagnostics.transport != vehicle_core::TransportHealth::Faulted &&
                          diagnostics.transport != vehicle_core::TransportHealth::TimedOut;
  LightingUpdate update{};
  update.turn = actionable ? *turn_reading.value : TurnState::Unknown;
  update.availability = turn_reading.availability;
  update.brake_pressed = brake_reading.availability == Availability::Fresh &&
                         brake_reading.value.has_value() && *brake_reading.value;
  update.brake_availability = brake_reading.availability;
  // A malformed turn frame can fault the message before any semantic value
  // has been accepted. Preserve that distinction for the private sink rather
  // than presenting it as initial NoData.
  if (turn_health.signal == vehicle_core::SignalHealth::Unavailable)
    update.availability = Availability::Unavailable;

  std::optional<vehicle_core::MonotonicTimestamp> deadline{};
  if (actionable && state.turn_state.has_value && state.turn_state.freshness_timeout_us) {
    deadline =
        saturating_add(state.turn_state.last_update_us, *state.turn_state.freshness_timeout_us);
  }
  if (update.brake_pressed && state.brake_pressed.has_value &&
      state.brake_pressed.freshness_timeout_us) {
    const auto brake_deadline = saturating_add(state.brake_pressed.last_update_us,
                                               *state.brake_pressed.freshness_timeout_us);
    deadline = deadline ? std::min(*deadline, brake_deadline) : brake_deadline;
  }
  if (last_transport_receive_us_) {
    const auto transport_deadline =
        saturating_add(*last_transport_receive_us_, config_.transport_silence_timeout_us);
    deadline = deadline ? std::min(*deadline, transport_deadline) : transport_deadline;
  }
  if (!deadline)
    deadline = saturating_add(now_us, kLightingHeartbeatUs);
  update.valid_until_us = *deadline <= now_us ? now_us : *deadline;

  const bool changed = !lighting_sent_ || update.turn != lighting_turn_ ||
                       update.availability != lighting_availability_ ||
                       update.brake_pressed != lighting_brake_pressed_ ||
                       update.brake_availability != lighting_brake_availability_ ||
                       lighting_failure_;
  // The private sink also needs bounded refreshes while the semantic state is
  // unavailable (startup, timeout, fault, or unknown).  A 100 ms heartbeat
  // keeps validity deadlines from silently expiring in those states.
  const bool heartbeat_due = lighting_sent_ && now_us >= lighting_next_heartbeat_us_;
  if (!changed && !heartbeat_due)
    return;

  const bool accepted = lighting_sink_->publish(update);
#if defined(ESP_PLATFORM)
  const auto turn_age_us = state.turn_state.has_value && now_us >= state.turn_state.last_update_us
                               ? now_us - state.turn_state.last_update_us
                               : 0;
  ESP_LOGD(kLightingTag,
           "lighting decision: turn=%u availability=%u actionable=%d turn_age_us=%llu "
           "brake=%d brake_availability=%u transport=%u now_us=%llu deadline_us=%llu accepted=%d",
           static_cast<unsigned>(update.turn), static_cast<unsigned>(update.availability),
           actionable, static_cast<unsigned long long>(turn_age_us), update.brake_pressed,
           static_cast<unsigned>(update.brake_availability),
           static_cast<unsigned>(diagnostics.transport), static_cast<unsigned long long>(now_us),
           static_cast<unsigned long long>(update.valid_until_us), accepted);
#endif
  lighting_sent_ = true;
  lighting_turn_ = update.turn;
  lighting_availability_ = update.availability;
  lighting_brake_pressed_ = update.brake_pressed;
  lighting_brake_availability_ = update.brake_availability;
  lighting_failure_ = !accepted;
  lighting_next_heartbeat_us_ = saturating_add(now_us, kLightingHeartbeatUs);
}

bool VehicleTelemetryService::workers_done() const noexcept {
  return dispatcher_done_.load(std::memory_order_acquire);
}

bool VehicleTelemetryService::wait_for_workers(const std::uint64_t timeout_us) noexcept {
#if defined(ESP_PLATFORM)
  const auto deadline = saturating_add(clock_->now(), timeout_us);
  while (!workers_done()) {
    if (clock_->now() >= deadline)
      return false;
    vTaskDelay(kMinimumTaskDelayTicks);
  }
#else
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds{timeout_us};
  while (!workers_done()) {
    if (std::chrono::steady_clock::now() >= deadline)
      return false;
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
#endif
  return true;
}

void VehicleTelemetryService::join_workers() noexcept {
#if !defined(ESP_PLATFORM)
  if (dispatcher_thread_.joinable())
    dispatcher_thread_.join();
#endif
}

void VehicleTelemetryService::publish_startup_black(
    const vehicle_core::MonotonicTimestamp now_us) noexcept {
  if (lighting_sink_ == nullptr)
    return;

  LightingUpdate update{};
  update.turn = TurnState::Unknown;
  update.availability = Availability::NoData;
  update.valid_until_us = now_us;
  lighting_failure_ = !lighting_sink_->publish(update);
  lighting_sent_ = true;
  lighting_turn_ = update.turn;
  lighting_availability_ = update.availability;
  lighting_brake_pressed_ = update.brake_pressed;
  lighting_brake_availability_ = update.brake_availability;
  lighting_next_heartbeat_us_ = saturating_add(now_us, kLightingHeartbeatUs);
}

#define MAZDA_SUBSCRIBE_METHOD(method_name, descriptor_index, callback_type)                       \
  SubscriptionToken VehicleTelemetryService::method_name(callback_type callback,                   \
                                                         void *context) noexcept {                 \
    return subscribe_notification_descriptor(                                                      \
        std::get<descriptor_index>(notification_descriptors()), callback, context);                \
  }

MAZDA_SUBSCRIBE_METHOD(subscribe_selector, 0, Callback<SelectorPosition>)
MAZDA_SUBSCRIBE_METHOD(subscribe_actual_gear, 1, Callback<ActualGear>)
MAZDA_SUBSCRIBE_METHOD(subscribe_turn, 2, Callback<TurnState>)
MAZDA_SUBSCRIBE_METHOD(subscribe_hazard, 3, Callback<bool>)
MAZDA_SUBSCRIBE_METHOD(subscribe_left_turn, 4, Callback<bool>)
MAZDA_SUBSCRIBE_METHOD(subscribe_right_turn, 5, Callback<bool>)
MAZDA_SUBSCRIBE_METHOD(subscribe_liftgate, 6, Callback<bool>)
MAZDA_SUBSCRIBE_METHOD(subscribe_rear_right_door, 7, Callback<bool>)
MAZDA_SUBSCRIBE_METHOD(subscribe_rear_left_door, 8, Callback<bool>)
MAZDA_SUBSCRIBE_METHOD(subscribe_front_left_door, 9, Callback<bool>)
MAZDA_SUBSCRIBE_METHOD(subscribe_front_right_door, 10, Callback<bool>)
MAZDA_SUBSCRIBE_METHOD(subscribe_doors_unlocked, 11, Callback<bool>)
MAZDA_SUBSCRIBE_METHOD(subscribe_left_lamp, 12, Callback<bool>)
MAZDA_SUBSCRIBE_METHOD(subscribe_right_lamp, 13, Callback<bool>)
MAZDA_SUBSCRIBE_METHOD(subscribe_wiper_low, 14, Callback<bool>)
MAZDA_SUBSCRIBE_METHOD(subscribe_front_wiper, 15, Callback<FrontWiperPosition>)
MAZDA_SUBSCRIBE_METHOD(subscribe_brake, 16, Callback<bool>)

#undef MAZDA_SUBSCRIBE_METHOD

StatusResult VehicleTelemetryService::unsubscribe(const SubscriptionToken &token) noexcept {
  if (callback_mutation_rejected())
    return {ResultCode::InvalidState};
  std::lock_guard<std::mutex> lock{lifecycle_mutex_};
  if (!claim_or_validate_lifecycle_owner())
    return {ResultCode::InvalidState};
  if (lifecycle_state_.load(std::memory_order_acquire) != LifecycleState::Stopped)
    return {ResultCode::InvalidState};
  Registration *registration = find_registration(token);
  // A generic registration belongs to its trampoline record and is removed
  // only through unsubscribe_generic().
  if (registration == nullptr || registration->generic)
    return {ResultCode::InvalidSubscription};
  return unsubscribe_registration(*registration);
}

StatusResult VehicleTelemetryService::unsubscribe_generic(const SubscriptionToken &token) noexcept {
  if (callback_mutation_rejected())
    return {ResultCode::InvalidState};
  std::lock_guard<std::mutex> lock{lifecycle_mutex_};
  if (!claim_or_validate_lifecycle_owner())
    return {ResultCode::InvalidState};
  if (lifecycle_state_.load(std::memory_order_acquire) != LifecycleState::Stopped)
    return {ResultCode::InvalidState};
  SignalSubscriptionRecord *record = nullptr;
  for (auto &candidate : generic_records_) {
    if (candidate.active && candidate.channel == token.channel && candidate.slot == token.slot &&
        candidate.generation == token.generation) {
      record = &candidate;
      break;
    }
  }
  Registration *registration = record == nullptr ? nullptr : find_registration(token);
  if (registration == nullptr || !registration->generic)
    return {ResultCode::InvalidSubscription};
  const auto result = unsubscribe_registration(*registration);
  // The record is released only once its channel can no longer dispatch to it.
  if (result.ok())
    *record = {};
  return result;
}

StatusResult
VehicleTelemetryService::unsubscribe_registration(Registration &registration) noexcept {
  vehicle_core::NotificationStatus status = vehicle_core::NotificationStatus::InvalidSubscription;
  std::apply(
      [this, &status, &registration](const auto &...descriptor) {
        (((registration.channel == std::decay_t<decltype(descriptor)>::Channel::channel_id())
              ? status = (this->*descriptor.channel).unsubscribe(registration.handle)
              : status),
         ...);
      },
      notification_descriptors());
  if (status == vehicle_core::NotificationStatus::InvalidSubscription)
    return {ResultCode::InvalidSubscription};
  if (status != vehicle_core::NotificationStatus::Ok)
    return {map_notification_status(status)};
  registration.active = false;
  registration.generic = false;
  return {ResultCode::Ok};
}

} // namespace mazda::internal

namespace mazda {

StatusResult
internal::VehicleTelemetryAccess::bind_lighting_sink(VehicleTelemetry &facade,
                                                     internal::LightingSink &sink) noexcept {
  auto *service =
      reinterpret_cast<internal::VehicleTelemetryService *>(facade.implementation_storage_);
  return service->bind_lighting_sink(sink);
}

internal::VehicleTelemetryService &
internal::VehicleTelemetryAccess::service(VehicleTelemetry &facade) noexcept {
  return *reinterpret_cast<internal::VehicleTelemetryService *>(facade.implementation_storage_);
}

const internal::VehicleTelemetryService &
internal::VehicleTelemetryAccess::service(const VehicleTelemetry &facade) noexcept {
  return *reinterpret_cast<const internal::VehicleTelemetryService *>(
      facade.implementation_storage_);
}

#if !defined(ESP_PLATFORM)
// Only valid on a stopped, quiescent facade: the current service is destroyed
// and a new one is placement-constructed in the same fixed storage, so no
// subscriptions, lifecycle owner or samples survive the replacement.
void internal::VehicleTelemetryAccess::emplace_host_service(
    VehicleTelemetry &facade, vehicle_core::MonotonicClock &clock,
    vehicle_telemetry::AcquisitionSource &source, internal::LightingSink &lighting_sink,
    const TelemetryConfig &config, const internal::HostServiceOptions host_options) noexcept {
  reinterpret_cast<internal::VehicleTelemetryService *>(facade.implementation_storage_)
      ->~VehicleTelemetryService();
  ::new (static_cast<void *>(facade.implementation_storage_))
      internal::VehicleTelemetryService{clock, source, lighting_sink, config, host_options};
}

Result<std::size_t>
internal::VehicleTelemetryAccess::drain_host_notifications(VehicleTelemetry &facade) noexcept {
  return reinterpret_cast<internal::VehicleTelemetryService *>(facade.implementation_storage_)
      ->drain_notifications();
}
#else
// Only used by the opt-in firmware synthetic benchmark. The normal facade
// constructor remains bound to CanBusSource, and this replacement occurs
// before the service is started so no production acquisition or renderer
// lifecycle is entered.
void internal::VehicleTelemetryAccess::emplace_benchmark_service(
    VehicleTelemetry &facade, vehicle_core::MonotonicClock &clock,
    vehicle_telemetry::AcquisitionSource &source, internal::LightingSink &lighting_sink,
    const TelemetryConfig &config) noexcept {
  reinterpret_cast<internal::VehicleTelemetryService *>(facade.implementation_storage_)
      ->~VehicleTelemetryService();
  ::new (static_cast<void *>(facade.implementation_storage_))
      internal::VehicleTelemetryService{clock, source, lighting_sink, config};
}
#endif

static_assert(sizeof(internal::VehicleTelemetryService) <= sizeof(VehicleTelemetry));
static_assert(alignof(internal::VehicleTelemetryService) <= alignof(std::max_align_t));

VehicleTelemetry::VehicleTelemetry() noexcept {
  ::new (static_cast<void *>(implementation_storage_)) internal::VehicleTelemetryService{};
}

VehicleTelemetry::~VehicleTelemetry() noexcept {
  reinterpret_cast<internal::VehicleTelemetryService *>(implementation_storage_)
      ->~VehicleTelemetryService();
}

StatusResult VehicleTelemetry::configure(const TelemetryConfig &config) noexcept {
  return reinterpret_cast<internal::VehicleTelemetryService *>(implementation_storage_)
      ->configure(config);
}

StatusResult VehicleTelemetry::start() noexcept {
  return reinterpret_cast<internal::VehicleTelemetryService *>(implementation_storage_)->start();
}

StatusResult VehicleTelemetry::stop() noexcept {
  return reinterpret_cast<internal::VehicleTelemetryService *>(implementation_storage_)->stop();
}

Reading<float> VehicleTelemetry::speed_kph() const noexcept {
  return reinterpret_cast<const internal::VehicleTelemetryService *>(implementation_storage_)
      ->speed_kph();
}

Reading<float> VehicleTelemetry::engine_rpm() const noexcept {
  return reinterpret_cast<const internal::VehicleTelemetryService *>(implementation_storage_)
      ->engine_rpm();
}

Diagnostics VehicleTelemetry::diagnostics() const noexcept {
  return reinterpret_cast<const internal::VehicleTelemetryService *>(implementation_storage_)
      ->diagnostics();
}

DebugSnapshot VehicleTelemetry::debug_snapshot() const noexcept {
  return reinterpret_cast<const internal::VehicleTelemetryService *>(implementation_storage_)
      ->debug_snapshot();
}

DebugSnapshot VehicleTelemetry::debug_stale_snapshot() const noexcept {
  return reinterpret_cast<const internal::VehicleTelemetryService *>(implementation_storage_)
      ->debug_stale_snapshot();
}

TelemetryProfileSnapshot VehicleTelemetry::telemetry_profile_snapshot() const noexcept {
  return reinterpret_cast<const internal::VehicleTelemetryService *>(implementation_storage_)
      ->telemetry_profile_snapshot();
}

std::uint32_t VehicleTelemetry::dispatch_progress() const noexcept {
  return reinterpret_cast<const internal::VehicleTelemetryService *>(implementation_storage_)
      ->dispatch_progress();
}

#define MAZDA_PUBLIC_SUBSCRIPTION_METHOD(method_name, service_method, callback_type)               \
  Result<Subscription> VehicleTelemetry::method_name(callback_type callback,                       \
                                                     void *context) noexcept {                     \
    const auto token =                                                                             \
        reinterpret_cast<internal::VehicleTelemetryService *>(implementation_storage_)             \
            ->service_method(callback, context);                                                   \
    if (!token.ok())                                                                               \
      return {token.status, std::nullopt};                                                         \
    return {ResultCode::Ok, Subscription{token.channel, token.slot, token.generation}};            \
  }

MAZDA_PUBLIC_SUBSCRIPTION_METHOD(on_selector_position_changed, subscribe_selector,
                                 Callback<SelectorPosition>)
MAZDA_PUBLIC_SUBSCRIPTION_METHOD(on_actual_gear_changed, subscribe_actual_gear,
                                 Callback<ActualGear>)
MAZDA_PUBLIC_SUBSCRIPTION_METHOD(on_turn_state_changed, subscribe_turn, Callback<TurnState>)
MAZDA_PUBLIC_SUBSCRIPTION_METHOD(on_hazard_request_changed, subscribe_hazard, Callback<bool>)
MAZDA_PUBLIC_SUBSCRIPTION_METHOD(on_left_turn_request_changed, subscribe_left_turn, Callback<bool>)
MAZDA_PUBLIC_SUBSCRIPTION_METHOD(on_right_turn_request_changed, subscribe_right_turn,
                                 Callback<bool>)
MAZDA_PUBLIC_SUBSCRIPTION_METHOD(on_liftgate_open_changed, subscribe_liftgate, Callback<bool>)
MAZDA_PUBLIC_SUBSCRIPTION_METHOD(on_rear_right_door_open_changed, subscribe_rear_right_door,
                                 Callback<bool>)
MAZDA_PUBLIC_SUBSCRIPTION_METHOD(on_rear_left_door_open_changed, subscribe_rear_left_door,
                                 Callback<bool>)
MAZDA_PUBLIC_SUBSCRIPTION_METHOD(on_front_left_door_open_rhd_changed, subscribe_front_left_door,
                                 Callback<bool>)
MAZDA_PUBLIC_SUBSCRIPTION_METHOD(on_front_right_door_open_rhd_changed, subscribe_front_right_door,
                                 Callback<bool>)
MAZDA_PUBLIC_SUBSCRIPTION_METHOD(on_doors_unlocked_changed, subscribe_doors_unlocked,
                                 Callback<bool>)
MAZDA_PUBLIC_SUBSCRIPTION_METHOD(on_left_indicator_lamp_changed, subscribe_left_lamp,
                                 Callback<bool>)
MAZDA_PUBLIC_SUBSCRIPTION_METHOD(on_right_indicator_lamp_changed, subscribe_right_lamp,
                                 Callback<bool>)
MAZDA_PUBLIC_SUBSCRIPTION_METHOD(on_wiper_low_changed, subscribe_wiper_low, Callback<bool>)
MAZDA_PUBLIC_SUBSCRIPTION_METHOD(on_front_wiper_changed, subscribe_front_wiper,
                                 Callback<FrontWiperPosition>)

MAZDA_PUBLIC_SUBSCRIPTION_METHOD(on_brake_pressed_changed, subscribe_brake, Callback<bool>)

#undef MAZDA_PUBLIC_SUBSCRIPTION_METHOD

StatusResult VehicleTelemetry::unsubscribe(const Subscription subscription) noexcept {
  const internal::SubscriptionToken token{ResultCode::Ok, subscription.channel_, subscription.slot_,
                                          subscription.generation_};
  return reinterpret_cast<internal::VehicleTelemetryService *>(implementation_storage_)
      ->unsubscribe(token);
}

} // namespace mazda
