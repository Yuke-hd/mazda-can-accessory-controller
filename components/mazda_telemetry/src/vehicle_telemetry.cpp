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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

namespace mazda::internal {

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

constexpr std::uint64_t kNanosecondsPerMicrosecond = 1'000;
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
Reading<T> notification_reading(const PublishedSnapshot &snapshot,
                                const LightingDescriptor<T> &descriptor,
                                const vehicle_core::MonotonicTimestamp now_us,
                                const vehicle_core::TransportHealth transport) noexcept {
  const auto &metadata = *descriptor.metadata;
  return snapshot.state.reading_at(snapshot.state.*descriptor.signal, metadata.identifier, now_us,
                                   metadata.confidence, transport);
}

// turn_state is derived from the three TURN_SWITCH request fields. All three
// source definitions are Reference-confidence, so bind the derived channel to
// one of those authoritative definitions instead of duplicating a literal.
inline constexpr LightingDescriptor<TurnState> kTurnNotificationDescriptor{
    &VehicleState::turn_state, &candidate::kTurnLeftSwitchDefinition};

} // namespace

vehicle_core::MonotonicTimestamp SteadyClock::now() const noexcept {
  // steady_clock is monotonic by contract. The cast is intentionally kept
  // local to this implementation seam; public callers only receive values.
  const auto duration = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<vehicle_core::MonotonicTimestamp>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count() /
      kNanosecondsPerMicrosecond);
}

PublicationStore::PublicationStore() noexcept : clock_(&steady_clock_) {}

PublicationStore::PublicationStore(vehicle_core::MonotonicClock &clock,
                                   TelemetryConfig config) noexcept
    : clock_(&clock), config_(config) {}

StatusResult PublicationStore::configure(const TelemetryConfig &config) noexcept {
  std::lock_guard<std::mutex> lock{mutex_};
  if (published_.diagnostics.lifecycle != LifecycleState::Stopped) {
    return StatusResult{ResultCode::InvalidState};
  }
  config_ = config;
  published_.state.apply_freshness_policy(config_.freshness);
  return StatusResult{ResultCode::Ok};
}

void PublicationStore::publish(
    const VehicleState &state, const Diagnostics &diagnostics,
    const std::optional<vehicle_core::MonotonicTimestamp> last_transport_receive_us) noexcept {
  std::lock_guard<std::mutex> lock{mutex_};
  published_.state = state;
  published_.state.apply_freshness_policy(config_.freshness);
  published_.diagnostics = diagnostics;
  (void)last_transport_receive_us;
}

void PublicationStore::publish(
    const VehicleState &state, const LifecycleState lifecycle,
    const vehicle_core::TransportHealth transport, const AcquisitionMetrics &acquisition,
    const std::optional<vehicle_core::MonotonicTimestamp> last_transport_receive_us) noexcept {
  Diagnostics diagnostics{};
  diagnostics.lifecycle = lifecycle;
  diagnostics.transport = transport;
  diagnostics.acquisition = acquisition;
  publish(state, diagnostics, last_transport_receive_us);
}

void PublicationStore::reset(const Diagnostics &diagnostics) noexcept {
  const auto reset_time_us = clock_->now();
  std::lock_guard<std::mutex> lock{mutex_};
  published_.state = VehicleState{};
  published_.state.apply_freshness_policy(config_.freshness);
  published_.diagnostics = diagnostics;
  (void)reset_time_us;
}

Reading<float> PublicationStore::speed_kph() const noexcept {
  return read_signal(&VehicleState::speed_kph, candidate::kEngineDataId,
                     ValidationStatus::Reference);
}

Reading<float> PublicationStore::engine_rpm() const noexcept {
  return read_signal(&VehicleState::engine_rpm, candidate::kEngineDataId,
                     ValidationStatus::Confirmed);
}

Diagnostics PublicationStore::diagnostics() const noexcept {
  Diagnostics result{};
  {
    std::lock_guard<std::mutex> lock{mutex_};
    result = published_.diagnostics;
  }
  return result;
}

PublishedSnapshot PublicationStore::snapshot() const noexcept {
  PublishedSnapshot result{};
  {
    std::lock_guard<std::mutex> lock{mutex_};
    result = published_;
  }
  return result;
}

const PollingDescriptorTuple &VehicleTelemetryService::polling_descriptors() noexcept {
  return kPollingDescriptors;
}

const NotificationDescriptorTuple &VehicleTelemetryService::notification_descriptors() noexcept {
  static const NotificationDescriptorTuple descriptors {
    {"selector_position", &VehicleTelemetryService::selector_channel_,
     &VehicleState::selector_position, candidate::kGearId,
     candidate::kSelectorDefinition.confidence},
        {"actual_gear", &VehicleTelemetryService::actual_gear_channel_, &VehicleState::actual_gear,
         candidate::kGearId, candidate::kActualGearDefinition.confidence},
        {"turn_state", &VehicleTelemetryService::turn_channel_, &VehicleState::turn_state,
         candidate::kTurnSwitchId, candidate::kTurnLeftSwitchDefinition.confidence},
        {"hazard_request", &VehicleTelemetryService::hazard_channel_, &VehicleState::hazard_request,
         candidate::kTurnSwitchId, candidate::kHazardDefinition.confidence},
        {"left_turn_request", &VehicleTelemetryService::left_turn_channel_,
         &VehicleState::left_turn_request, candidate::kTurnSwitchId,
         candidate::kTurnLeftSwitchDefinition.confidence},
        {"right_turn_request", &VehicleTelemetryService::right_turn_channel_,
         &VehicleState::right_turn_request, candidate::kTurnSwitchId,
         candidate::kTurnRightSwitchDefinition.confidence},
        {"liftgate_open", &VehicleTelemetryService::liftgate_channel_, &VehicleState::liftgate_open,
         candidate::kDoorsId, candidate::kLiftgateOpenDefinition.confidence},
        {"rear_right_door_open", &VehicleTelemetryService::rear_right_door_channel_,
         &VehicleState::rear_right_door_open, candidate::kDoorsId,
         candidate::kRearRightDoorOpenDefinition.confidence},
        {"rear_left_door_open", &VehicleTelemetryService::rear_left_door_channel_,
         &VehicleState::rear_left_door_open, candidate::kDoorsId,
         candidate::kRearLeftDoorOpenDefinition.confidence},
        {"front_left_door_open_rhd", &VehicleTelemetryService::front_left_door_channel_,
         &VehicleState::front_left_door_open_rhd, candidate::kDoorsId,
         candidate::kFrontLeftDoorOpenRhdDefinition.confidence},
        {"front_right_door_open_rhd", &VehicleTelemetryService::front_right_door_channel_,
         &VehicleState::front_right_door_open_rhd, candidate::kDoorsId,
         candidate::kFrontRightDoorOpenRhdDefinition.confidence},
        {"doors_unlocked", &VehicleTelemetryService::doors_unlocked_channel_,
         &VehicleState::doors_unlocked, candidate::kDoorsId,
         candidate::kDoorsUnlockedDefinition.confidence},
        {"left_indicator_lamp", &VehicleTelemetryService::left_lamp_channel_,
         &VehicleState::left_indicator_lamp, candidate::kBlinkInfoId,
         candidate::kLeftIndicatorLampDefinition.confidence},
        {"right_indicator_lamp", &VehicleTelemetryService::right_lamp_channel_,
         &VehicleState::right_indicator_lamp, candidate::kBlinkInfoId,
         candidate::kRightIndicatorLampDefinition.confidence},
        {"wiper_low", &VehicleTelemetryService::wiper_low_channel_, &VehicleState::wiper_low,
         candidate::kBlinkInfoId, candidate::kWiperLowDefinition.confidence},
#if defined(ESP_PLATFORM)
        {"front_wiper", &VehicleTelemetryService::front_wiper_channel_, &VehicleState::front_wiper,
         candidate::kTurnSwitchId, candidate::kFrontWiperDefinition.confidence}
#else
        {"front_wiper", &VehicleTelemetryService::front_wiper_channel_, &VehicleState::front_wiper,
         candidate::kTurnSwitchId, candidate::kFrontWiperDefinition.confidence},
    {
      "test_front_wiper", &VehicleTelemetryService::test_front_wiper_channel_,
          &VehicleState::front_wiper, candidate::kTurnSwitchId,
          candidate::kFrontWiperDefinition.confidence
    }
#endif
  };
  return descriptors;
}

VehicleTelemetryService::VehicleTelemetryService() noexcept
#if defined(ESP_PLATFORM)
    : VehicleTelemetryService(steady_clock_, can_bus_source_, null_lighting_sink_){}
#else
    : VehicleTelemetryService(steady_clock_, host_source_, null_lighting_sink_) {
}
#endif

      VehicleTelemetryService::VehicleTelemetryService(
          vehicle_core::MonotonicClock & clock, vehicle_telemetry::AcquisitionSource & source,
          LightingSink & lighting_sink, const TelemetryConfig config) noexcept
    : clock_(&clock), lighting_sink_(&lighting_sink), runtime_(source, *this, *this, clock),
      publication_(clock, config), config_(config) {
  initialize_registration_slots();
  processing_state_.apply_freshness_policy(config_.freshness);
  vehicle_telemetry::RuntimeConfig runtime_config{};
  runtime_config.receive_timeout_ms =
      static_cast<std::uint32_t>(std::max<vehicle_core::Microseconds>(
          1, (config_.availability_service_target_us + 999) / 1'000));
  runtime_config.transport_silence_timeout_us = config_.transport_silence_timeout_us;
  (void)runtime_.configure(runtime_config);
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
  return config.transport_silence_timeout_us > 0 && config.max_frames_per_batch > 0 &&
         config.max_frames_per_batch <= kDefaultMaxFramesPerBatch &&
         config.availability_service_target_us > 0;
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
  const auto result = publication_.configure(config);
  if (result.ok()) {
    config_ = config;
    vehicle_telemetry::RuntimeConfig runtime_config{};
    runtime_config.receive_timeout_ms =
        static_cast<std::uint32_t>(std::max<vehicle_core::Microseconds>(
            1, (config.availability_service_target_us + 999) / 1'000));
    runtime_config.transport_silence_timeout_us = config.transport_silence_timeout_us;
    const auto runtime_result = runtime_.configure(runtime_config);
    if (!runtime_result.ok())
      return {map_runtime_status(runtime_result.status)};
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

std::size_t VehicleTelemetryService::dispatch_channels_once() noexcept {
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
  return delivered;
}

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
  last_transport_receive_us_.reset();
  runtime_diagnostics_ = {};
  lighting_sent_ = false;
  lighting_failure_ = false;
  const auto now_us = clock_->now();
  publish_startup_black(now_us);
  runtime_diagnostics_.lifecycle = vehicle_telemetry::LifecycleState::Running;
  runtime_diagnostics_.transport = vehicle_core::TransportHealth::AwaitingTraffic;
  lifecycle_state_.store(LifecycleState::Running, std::memory_order_release);
  publication_.reset(Diagnostics{LifecycleState::Running,
                                 vehicle_core::TransportHealth::AwaitingTraffic,
                                 AcquisitionMetrics{}});
  if (!start_channels()) {
    lifecycle_state_.store(LifecycleState::Faulted, std::memory_order_release);
    publication_.reset(Diagnostics{LifecycleState::Faulted, vehicle_core::TransportHealth::Faulted,
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
    publication_.reset(Diagnostics{failure,
                                   failure == LifecycleState::Faulted
                                       ? vehicle_core::TransportHealth::Faulted
                                       : vehicle_core::TransportHealth::Stopped,
                                   AcquisitionMetrics{}});
    return {mapped};
  }

  run_requested_.store(true, std::memory_order_release);
  dispatcher_done_.store(false, std::memory_order_release);
  dispatch_cursor_.store(0, std::memory_order_relaxed);
#if defined(ESP_PLATFORM)
  if (xTaskCreate(&VehicleTelemetryService::dispatcher_task_entry, "mazda_notify", 4096, this,
                  configMAX_PRIORITIES - 4,
                  reinterpret_cast<TaskHandle_t *>(&dispatcher_task_)) != pdPASS) {
    run_requested_.store(false, std::memory_order_release);
    (void)runtime_.stop();
    dispatcher_done_.store(true, std::memory_order_release);
    (void)stop_channels();
    lifecycle_state_.store(LifecycleState::Faulted, std::memory_order_release);
    publication_.reset(Diagnostics{LifecycleState::Faulted, vehicle_core::TransportHealth::Faulted,
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
    publication_.reset(Diagnostics{LifecycleState::Faulted, vehicle_core::TransportHealth::Faulted,
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
  const auto stopping_snapshot = publication_.snapshot();
  publication_.publish(stopping_snapshot.state,
                       Diagnostics{LifecycleState::Stopping, vehicle_core::TransportHealth::Stopped,
                                   stopping_snapshot.diagnostics.acquisition},
                       std::nullopt);
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
  publication_.reset(
      Diagnostics{LifecycleState::Stopped, vehicle_core::TransportHealth::Stopped, acquisition});
  processing_state_ = VehicleState{};
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
  return diagnostics;
}

void VehicleTelemetryService::reset() noexcept {
  processing_state_ = VehicleState{};
  processing_state_.apply_freshness_policy(config_.freshness);
  last_transport_receive_us_.reset();
  runtime_diagnostics_ = {};
  lighting_sent_ = false;
  lighting_failure_ = false;
}

vehicle_telemetry::ProcessResult
VehicleTelemetryService::process(const vehicle_core::RawCanFrame &frame) noexcept {
  std::optional<TurnEdgeEvent> edge{};
  vehicle_core::DecoderObservation observation{};
  vehicle_core::HealthObservation health{};
  const auto status = candidate::decode(frame, processing_state_, &edge, &observation, &health);
  switch (status) {
  case vehicle_core::DecodeValidity::Decoded:
    return {vehicle_telemetry::ProcessStatus::Processed};
  case vehicle_core::DecodeValidity::Malformed:
    return {vehicle_telemetry::ProcessStatus::Malformed};
  case vehicle_core::DecodeValidity::Ignored:
    return {vehicle_telemetry::ProcessStatus::Ignored};
  }
  return {vehicle_telemetry::ProcessStatus::Fault};
}

void VehicleTelemetryService::on_frame_processed(
    const vehicle_core::RawCanFrame &, const vehicle_telemetry::ProcessResult &) noexcept {
  // Runtime invokes on_diagnostics immediately after this callback. Keeping
  // publication there gives Mazda consumers one coherent state/transport
  // snapshot and avoids a second receive worker in this component.
}

void VehicleTelemetryService::on_diagnostics(
    const vehicle_telemetry::TransportDiagnostics &diagnostics) noexcept {
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
  publish_current(diagnostics.has_last_frame);
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
  const Diagnostics diagnostics{lifecycle, runtime_diagnostics_.transport, acquisition};
  publication_.publish(processing_state_, diagnostics,
                       received_frame ? last_transport_receive_us_ : std::nullopt);
  const auto now_us = clock_->now();
  const auto snapshot = publication_.snapshot();
  publish_notifications(snapshot, now_us);
}

template <typename T, std::uint16_t ChannelId>
void VehicleTelemetryService::publish_notification_descriptor(
    const PublishedSnapshot &snapshot, const vehicle_core::MonotonicTimestamp now_us,
    const NotificationDescriptor<T, ChannelId> &descriptor) noexcept {
  const auto transport = snapshot.diagnostics.transport;
  (void)(this->*descriptor.channel)
      .publish(snapshot.state.reading_at(snapshot.state.*descriptor.signal, descriptor.identifier,
                                         now_us, descriptor.validation, transport));
}

void VehicleTelemetryService::publish_notifications(
    const PublishedSnapshot &snapshot, const vehicle_core::MonotonicTimestamp now_us) noexcept {
  std::apply(
      [this, &snapshot, now_us](const auto &...descriptor) {
        (publish_notification_descriptor(snapshot, now_us, descriptor), ...);
      },
      notification_descriptors());
  publish_lighting(snapshot, now_us);
}

void VehicleTelemetryService::publish_lighting(
    const PublishedSnapshot &snapshot, const vehicle_core::MonotonicTimestamp now_us) noexcept {
  const auto turn_reading = notification_reading(snapshot, kTurnNotificationDescriptor, now_us,
                                                 snapshot.diagnostics.transport);
  const auto turn_health =
      snapshot.state.health_observation(candidate::kTurnSwitchId, snapshot.diagnostics.transport);
  const auto brake_reading =
      snapshot.state.reading_at(snapshot.state.brake_pressed, candidate::kBrakePedalId, now_us,
                                ValidationStatus::Confirmed, snapshot.diagnostics.transport);
  const bool actionable =
      is_available_reading(turn_reading) && *turn_reading.value != TurnState::Unknown &&
      snapshot.diagnostics.transport != vehicle_core::TransportHealth::Stopped &&
      snapshot.diagnostics.transport != vehicle_core::TransportHealth::Faulted &&
      snapshot.diagnostics.transport != vehicle_core::TransportHealth::TimedOut;
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
  if (actionable && snapshot.state.turn_state.has_value &&
      snapshot.state.turn_state.freshness_timeout_us) {
    deadline = saturating_add(snapshot.state.turn_state.last_update_us,
                              *snapshot.state.turn_state.freshness_timeout_us);
  }
  if (update.brake_pressed && snapshot.state.brake_pressed.has_value &&
      snapshot.state.brake_pressed.freshness_timeout_us) {
    const auto brake_deadline = saturating_add(snapshot.state.brake_pressed.last_update_us,
                                               *snapshot.state.brake_pressed.freshness_timeout_us);
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
  const auto turn_age_us =
      snapshot.state.turn_state.has_value && now_us >= snapshot.state.turn_state.last_update_us
          ? now_us - snapshot.state.turn_state.last_update_us
          : 0;
  ESP_LOGD(kLightingTag,
           "lighting decision: turn=%u availability=%u actionable=%d turn_age_us=%llu "
           "brake=%d brake_availability=%u transport=%u now_us=%llu deadline_us=%llu accepted=%d",
           static_cast<unsigned>(update.turn), static_cast<unsigned>(update.availability),
           actionable, static_cast<unsigned long long>(turn_age_us), update.brake_pressed,
           static_cast<unsigned>(update.brake_availability),
           static_cast<unsigned>(snapshot.diagnostics.transport),
           static_cast<unsigned long long>(now_us),
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
  if (registration == nullptr)
    return {ResultCode::InvalidSubscription};

  vehicle_core::NotificationStatus status = vehicle_core::NotificationStatus::InvalidSubscription;
  std::apply(
      [this, &token, &status, &registration](const auto &...descriptor) {
        (((token.channel == std::decay_t<decltype(descriptor)>::Channel::channel_id())
              ? status = (this->*descriptor.channel).unsubscribe(registration->handle)
              : status),
         ...);
      },
      notification_descriptors());
  if (status == vehicle_core::NotificationStatus::InvalidSubscription)
    return {ResultCode::InvalidSubscription};
  if (status != vehicle_core::NotificationStatus::Ok)
    return {map_notification_status(status)};
  registration->active = false;
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

#undef MAZDA_PUBLIC_SUBSCRIPTION_METHOD

StatusResult VehicleTelemetry::unsubscribe(const Subscription subscription) noexcept {
  const internal::SubscriptionToken token{ResultCode::Ok, subscription.channel_, subscription.slot_,
                                          subscription.generation_};
  return reinterpret_cast<internal::VehicleTelemetryService *>(implementation_storage_)
      ->unsubscribe(token);
}

} // namespace mazda
