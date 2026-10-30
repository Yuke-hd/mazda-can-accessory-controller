#include "vehicle_profile_runtime/service.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <new>
#if !defined(ESP_PLATFORM) && !defined(_WIN32)
#include <pthread.h>
#elif defined(_WIN32)
#include <thread>
#endif

#include "provider_access.hpp"
#include "vehicle_profile_runtime/provider.hpp"
#include "vehicle_profile_runtime/tables.hpp"
#include "vehicle_telemetry/observer.hpp"
#include "vehicle_telemetry/receive.hpp"
#include "vehicle_telemetry/runtime.hpp"

#if defined(ESP_PLATFORM)
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#endif

namespace vehicle_profile_runtime {
namespace {

using ExecutionIdentity = std::uint64_t;
constexpr std::uint32_t kDispatchQuiescenceTimeoutMs = 1000U;
std::atomic<ExecutionIdentity> g_next_execution_identity{1};

[[nodiscard]] ExecutionIdentity current_execution_identity() noexcept {
  // Keep the identity stable if a FreeRTOS task handle or host thread storage
  // is recycled after the original lifecycle owner exits.
  static thread_local const ExecutionIdentity identity =
      g_next_execution_identity.fetch_add(1, std::memory_order_relaxed);
  return identity;
}

class RuntimeMutex final {
public:
  RuntimeMutex() noexcept {
#if defined(ESP_PLATFORM)
    handle_ = xSemaphoreCreateMutexStatic(&storage_);
#endif
  }

  RuntimeMutex(const RuntimeMutex &) = delete;
  RuntimeMutex &operator=(const RuntimeMutex &) = delete;

  void lock() noexcept {
#if defined(ESP_PLATFORM)
    if (handle_ != nullptr) {
      (void)xSemaphoreTake(handle_, portMAX_DELAY);
    }
#else
    mutex_.lock();
#endif
  }

  void unlock() noexcept {
#if defined(ESP_PLATFORM)
    if (handle_ != nullptr) {
      (void)xSemaphoreGive(handle_);
    }
#else
    mutex_.unlock();
#endif
  }

  [[nodiscard]] bool valid() const noexcept {
#if defined(ESP_PLATFORM)
    return handle_ != nullptr;
#else
    return true;
#endif
  }

private:
#if defined(ESP_PLATFORM)
  StaticSemaphore_t storage_{};
  SemaphoreHandle_t handle_{nullptr};
#else
  std::mutex mutex_{};
#endif
};

#if defined(ESP_PLATFORM)
using DispatchMutex = RuntimeMutex;
constexpr std::size_t kDispatcherStackBytes = 4096U;
static_assert(kDispatcherStackBytes % sizeof(StackType_t) == 0U,
              "profile dispatcher stack must contain whole RTOS words");
struct StaticDispatcherTaskStorage final {
  StaticTask_t task{};
  std::array<StackType_t, kDispatcherStackBytes / sizeof(StackType_t)> stack{};
};
StaticDispatcherTaskStorage g_dispatcher_task_storage{};
std::atomic<void *> g_dispatcher_owner{nullptr};
#else
using DispatchMutex = std::mutex;
#endif

void append_unique(std::array<vehicle_signals::SignalId, kMaxRuntimeSignals> &ids,
                   std::size_t &count, const vehicle_signals::SignalId id) noexcept {
  for (std::size_t index = 0; index < count; ++index) {
    if (ids[index] == id) {
      return;
    }
  }
  if (count < ids.size()) {
    ids[count++] = id;
  }
}

void merge(ProviderProcessResult &target, const ProviderProcessResult &source) noexcept {
  for (std::size_t index = 0; index < source.changed_count; ++index) {
    append_unique(target.changed, target.changed_count, source.changed[index]);
  }
  for (std::size_t index = 0; index < source.due_count; ++index) {
    append_unique(target.due, target.due_count, source.due[index]);
  }
  for (std::size_t index = 0; index < source.recovered_count; ++index) {
    append_unique(target.recovered, target.recovered_count, source.recovered[index]);
  }
  if (source.status == ProviderProcessStatus::Faulted) {
    target.status = ProviderProcessStatus::Faulted;
  } else if (target.status == ProviderProcessStatus::Ignored) {
    target.status = source.status;
  }
}

[[nodiscard]] bool
transport_requires_failoff(const vehicle_telemetry::TransportDiagnostics &diagnostics) noexcept {
  using vehicle_core::TransportHealth;
  using vehicle_telemetry::LifecycleState;
  return diagnostics.lifecycle == LifecycleState::Faulted ||
         diagnostics.lifecycle == LifecycleState::Stopping ||
         diagnostics.lifecycle == LifecycleState::Stopped ||
         diagnostics.transport == TransportHealth::TimedOut ||
         diagnostics.transport == TransportHealth::Faulted ||
         diagnostics.transport == TransportHealth::Stopped;
}

[[nodiscard]] vehicle_telemetry::ProcessStatus
map_process_status(const ProviderProcessStatus status) noexcept {
  switch (status) {
  case ProviderProcessStatus::Ignored:
    return vehicle_telemetry::ProcessStatus::Ignored;
  case ProviderProcessStatus::Applied:
    return vehicle_telemetry::ProcessStatus::Processed;
  case ProviderProcessStatus::Malformed:
  case ProviderProcessStatus::InvalidResults:
    return vehicle_telemetry::ProcessStatus::Malformed;
  case ProviderProcessStatus::Faulted:
    return vehicle_telemetry::ProcessStatus::Fault;
  }
  return vehicle_telemetry::ProcessStatus::Fault;
}

class ProfileRuntimeServiceImplementation final : public vehicle_telemetry::FrameProcessor,
                                                  public vehicle_telemetry::Observer {
public:
  ProfileRuntimeServiceImplementation(const ProfileTables &tables,
                                      vehicle_core::MonotonicClock &clock,
                                      vehicle_telemetry::AcquisitionSource &source) noexcept
      : provider_(tables, clock, publication_synchronization()),
        runtime_(source, *this, *this, clock), configuration_status_(runtime_.configure({})) {}

  [[nodiscard]] bool ready() const noexcept {
    bool dispatch_mutex_valid = true;
#if defined(ESP_PLATFORM)
    dispatch_mutex_valid = dispatch_mutex_.valid();
#endif
    return publication_mutex_.valid() && processing_mutex_.valid() && dispatch_mutex_valid &&
           configuration_status_.ok();
  }

  [[nodiscard]] vehicle_signals::SignalProvider &provider() noexcept { return provider_; }
  [[nodiscard]] const vehicle_signals::SignalProvider &provider() const noexcept {
    return provider_;
  }

  [[nodiscard]] vehicle_telemetry::StatusResult start() noexcept {
    if (callback_mutation_rejected()) {
      return {vehicle_telemetry::ResultCode::InvalidState};
    }
    if (!ready()) {
      return {vehicle_telemetry::ResultCode::Faulted};
    }
    if (!claim_lifecycle_owner()) {
      return {vehicle_telemetry::ResultCode::InvalidState};
    }
    if (provider_active_) {
      return {vehicle_telemetry::ResultCode::InvalidState};
    }

    const auto dispatcher_start = start_dispatcher();
    if (dispatcher_start != vehicle_telemetry::ResultCode::Ok) {
      return {dispatcher_start};
    }
    stopping_requested_.store(false, std::memory_order_release);
    fault_latched_.store(false, std::memory_order_release);
    {
      std::lock_guard<RuntimeMutex> lock{processing_mutex_};
      (void)ProviderAccess::reset(provider_);
      ProviderAccess::set_running(provider_, true);
      provider_active_ = true;
    }
    {
      std::lock_guard<DispatchMutex> lock{dispatch_mutex_};
      dispatch_enabled_ = true;
    }
    enqueue_dispatch();
    const auto result = runtime_.start();
    return result;
  }

  [[nodiscard]] vehicle_telemetry::StatusResult stop() noexcept {
    if (callback_mutation_rejected()) {
      return {vehicle_telemetry::ResultCode::InvalidState};
    }
    if (!ready()) {
      return {vehicle_telemetry::ResultCode::Faulted};
    }
    if (!claim_lifecycle_owner()) {
      return {vehicle_telemetry::ResultCode::InvalidState};
    }

    const bool retrying_dispatch_quiescence = provider_active_;
    const auto state = runtime_.lifecycle();
    if (state == vehicle_telemetry::LifecycleState::Stopped && !provider_active_) {
      return runtime_.stop();
    }

    stopping_requested_.store(true, std::memory_order_release);
    fault_latched_.store(true, std::memory_order_release);
    {
      std::lock_guard<RuntimeMutex> lock{processing_mutex_};
      const ProviderProcessResult summary = ProviderAccess::fault(provider_);
      ProviderAccess::publish(provider_, summary);
    }
    enqueue_dispatch();

    const auto result = runtime_.stop();
    if (result.ok() || result.status == vehicle_telemetry::ResultCode::NotRunning) {
      if (!wait_for_dispatch_quiescence() || !stop_dispatcher()) {
        return {vehicle_telemetry::ResultCode::Timeout};
      }
      std::lock_guard<RuntimeMutex> lock{processing_mutex_};
      ProviderAccess::set_running(provider_, false);
      (void)ProviderAccess::reset(provider_);
      provider_active_ = false;
    }
    if (retrying_dispatch_quiescence &&
        result.status == vehicle_telemetry::ResultCode::NotRunning && !provider_active_) {
      return {vehicle_telemetry::ResultCode::Ok};
    }
    return result;
  }

  void reset() noexcept override {
    std::lock_guard<RuntimeMutex> lock{processing_mutex_};
    (void)ProviderAccess::reset(provider_);
  }

  [[nodiscard]] vehicle_telemetry::ProcessResult
  process(const vehicle_core::RawCanFrame &frame) noexcept override {
    std::lock_guard<RuntimeMutex> lock{processing_mutex_};
    if (stopping_requested_.load(std::memory_order_acquire)) {
      return {};
    }

    ProviderProcessResult summary = ProviderAccess::process(provider_, frame);
    if (summary.status == ProviderProcessStatus::Faulted) {
      fault_latched_.store(true, std::memory_order_release);
      merge(summary, ProviderAccess::fault(provider_));
    }
    ProviderAccess::publish(provider_, summary);
    enqueue_dispatch();
    return {map_process_status(summary.status)};
  }

  void on_frame_processed(const vehicle_core::RawCanFrame &,
                          const vehicle_telemetry::ProcessResult &) noexcept override {
    // Callback delivery runs on a separate serialized dispatcher.
  }

  void
  on_diagnostics(const vehicle_telemetry::TransportDiagnostics &diagnostics) noexcept override {
    std::lock_guard<RuntimeMutex> lock{processing_mutex_};
    ProviderProcessResult summary = ProviderAccess::service(provider_);
    if (transport_requires_failoff(diagnostics)) {
      if (!fault_latched_.exchange(true, std::memory_order_acq_rel)) {
        merge(summary, ProviderAccess::fault(provider_));
      }
    } else if (diagnostics.lifecycle == vehicle_telemetry::LifecycleState::Running &&
               diagnostics.transport == vehicle_core::TransportHealth::Live) {
      fault_latched_.store(false, std::memory_order_release);
    }
    ProviderAccess::publish(provider_, summary);
    enqueue_dispatch();
  }

private:
  class DispatcherGuard final {
  public:
    explicit DispatcherGuard(ProfileRuntimeServiceImplementation *owner) noexcept : owner_(owner) {}
    ~DispatcherGuard() noexcept {
      if (owner_ != nullptr) {
        owner_->shutdown_dispatcher_for_destructor();
      }
    }

    DispatcherGuard(const DispatcherGuard &) = delete;
    DispatcherGuard &operator=(const DispatcherGuard &) = delete;

  private:
    ProfileRuntimeServiceImplementation *owner_;
  };

  [[nodiscard]] bool claim_lifecycle_owner() noexcept {
    const ExecutionIdentity current = current_execution_identity();
    ExecutionIdentity expected{0};
    if (lifecycle_owner_.compare_exchange_strong(expected, current, std::memory_order_acq_rel,
                                                 std::memory_order_acquire)) {
      return true;
    }
    return expected == current;
  }

  [[nodiscard]] bool callback_mutation_rejected() const noexcept {
    const ExecutionIdentity dispatcher =
        callback_dispatch_identity_.load(std::memory_order_acquire);
    return dispatcher != 0U && dispatcher == current_execution_identity();
  }

  [[nodiscard]] static bool claim_lifecycle_owner_callback(void *context) noexcept {
    return static_cast<ProfileRuntimeServiceImplementation *>(context)->claim_lifecycle_owner();
  }

  [[nodiscard]] vehicle_profile_runtime::PublicationSynchronization
  publication_synchronization() noexcept {
    vehicle_profile_runtime::PublicationSynchronization synchronization{};
    synchronization.context = &publication_mutex_;
    synchronization.enter = [](void *context) noexcept {
      static_cast<RuntimeMutex *>(context)->lock();
    };
    synchronization.leave = [](void *context) noexcept {
      static_cast<RuntimeMutex *>(context)->unlock();
    };
    synchronization.lifecycle_owner_context = this;
    synchronization.claim_or_validate_lifecycle_owner =
        &ProfileRuntimeServiceImplementation::claim_lifecycle_owner_callback;
    return synchronization;
  }

  void enqueue_dispatch() noexcept {
    {
      std::lock_guard<DispatchMutex> lock{dispatch_mutex_};
      dispatch_pending_ = true;
    }
#if defined(ESP_PLATFORM)
    if (dispatch_wake_ != nullptr) {
      (void)xSemaphoreGive(dispatch_wake_);
    }
#else
    dispatch_changed_.notify_one();
#endif
  }

  [[nodiscard]] vehicle_telemetry::ResultCode start_dispatcher() noexcept {
    {
      std::lock_guard<DispatchMutex> lock{dispatch_mutex_};
      if (dispatcher_started_) {
        return vehicle_telemetry::ResultCode::InvalidState;
      }
      dispatcher_started_ = true;
      dispatch_enabled_ = false;
      dispatch_shutdown_requested_ = false;
      dispatch_pending_ = false;
      dispatch_active_ = false;
      dispatcher_exited_.store(false, std::memory_order_release);
    }
#if defined(ESP_PLATFORM)
    void *expected_owner = nullptr;
    if (!g_dispatcher_owner.compare_exchange_strong(expected_owner, this, std::memory_order_acq_rel,
                                                    std::memory_order_acquire)) {
      std::lock_guard<DispatchMutex> lock{dispatch_mutex_};
      dispatcher_started_ = false;
      dispatcher_exited_.store(true, std::memory_order_release);
      return vehicle_telemetry::ResultCode::InvalidState;
    }
    if (dispatch_wake_ == nullptr) {
      g_dispatcher_owner.store(nullptr, std::memory_order_release);
      std::lock_guard<DispatchMutex> lock{dispatch_mutex_};
      dispatcher_started_ = false;
      dispatcher_exited_.store(true, std::memory_order_release);
      return vehicle_telemetry::ResultCode::Faulted;
    }
    while (xSemaphoreTake(dispatch_wake_, 0) == pdTRUE) {
    }
    dispatch_task_ = xTaskCreateStatic(
        &ProfileRuntimeServiceImplementation::dispatch_task_entry, "profile_notify",
        static_cast<configSTACK_DEPTH_TYPE>(g_dispatcher_task_storage.stack.size()), this,
        tskIDLE_PRIORITY + 1, g_dispatcher_task_storage.stack.data(),
        &g_dispatcher_task_storage.task);
    if (dispatch_task_ == nullptr) {
      g_dispatcher_owner.store(nullptr, std::memory_order_release);
      std::lock_guard<DispatchMutex> lock{dispatch_mutex_};
      dispatcher_started_ = false;
      dispatcher_exited_.store(true, std::memory_order_release);
      return vehicle_telemetry::ResultCode::Faulted;
    }
#elif defined(_WIN32)
    try {
      dispatch_thread_ = std::thread(&ProfileRuntimeServiceImplementation::dispatch_loop, this);
    } catch (...) {
      std::lock_guard<DispatchMutex> lock{dispatch_mutex_};
      dispatcher_started_ = false;
      dispatcher_exited_.store(true, std::memory_order_release);
      return vehicle_telemetry::ResultCode::Faulted;
    }
#else
    if (pthread_create(&dispatch_thread_, nullptr,
                       &ProfileRuntimeServiceImplementation::dispatch_thread_entry, this) != 0) {
      std::lock_guard<DispatchMutex> lock{dispatch_mutex_};
      dispatcher_started_ = false;
      dispatcher_exited_.store(true, std::memory_order_release);
      return vehicle_telemetry::ResultCode::Faulted;
    }
    dispatch_thread_joinable_ = true;
#endif
    return vehicle_telemetry::ResultCode::Ok;
  }

  [[nodiscard]] bool wait_for_dispatch_quiescence() noexcept {
#if defined(ESP_PLATFORM)
    constexpr TickType_t wait_ticks = pdMS_TO_TICKS(1) == 0 ? 1 : pdMS_TO_TICKS(1);
    for (std::uint32_t waited = 0; waited < kDispatchQuiescenceTimeoutMs; ++waited) {
      bool idle = false;
      {
        std::lock_guard<DispatchMutex> lock{dispatch_mutex_};
        idle = !dispatch_pending_ && !dispatch_active_;
      }
      if (idle) {
        return true;
      }
      vTaskDelay(wait_ticks);
    }
    return false;
#else
    std::unique_lock<std::mutex> lock{dispatch_mutex_};
    return dispatch_changed_.wait_for(lock, std::chrono::milliseconds{kDispatchQuiescenceTimeoutMs},
                                      [this] { return !dispatch_pending_ && !dispatch_active_; });
#endif
  }

  [[nodiscard]] bool stop_dispatcher() noexcept {
    {
      std::lock_guard<DispatchMutex> lock{dispatch_mutex_};
      if (!dispatcher_started_) {
        return true;
      }
      dispatch_enabled_ = true;
      dispatch_shutdown_requested_ = true;
    }
#if defined(ESP_PLATFORM)
    if (dispatch_wake_ != nullptr) {
      (void)xSemaphoreGive(dispatch_wake_);
    }
    constexpr TickType_t wait_ticks = pdMS_TO_TICKS(1) == 0 ? 1 : pdMS_TO_TICKS(1);
    for (std::uint32_t waited = 0; waited < kDispatchQuiescenceTimeoutMs; ++waited) {
      if (dispatcher_exited_.load(std::memory_order_acquire) && dispatch_task_ != nullptr &&
          eTaskGetState(dispatch_task_) == eSuspended) {
        break;
      }
      vTaskDelay(wait_ticks);
    }
    if (!dispatcher_exited_.load(std::memory_order_acquire) || dispatch_task_ == nullptr ||
        eTaskGetState(dispatch_task_) != eSuspended) {
      return false;
    }
    if (dispatch_task_ != nullptr) {
      vTaskDelete(dispatch_task_);
    }
    dispatch_task_ = nullptr;
    g_dispatcher_owner.store(nullptr, std::memory_order_release);
#elif defined(_WIN32)
    dispatch_changed_.notify_one();
    if (dispatch_thread_.joinable()) {
      dispatch_thread_.join();
    }
#else
    dispatch_changed_.notify_one();
    if (dispatch_thread_joinable_) {
      (void)pthread_join(dispatch_thread_, nullptr);
      dispatch_thread_joinable_ = false;
    }
#endif
    std::lock_guard<DispatchMutex> lock{dispatch_mutex_};
    dispatcher_started_ = false;
    return true;
  }

  void stop_dispatcher_unbounded() noexcept {
    {
      std::lock_guard<DispatchMutex> lock{dispatch_mutex_};
      if (!dispatcher_started_) {
        return;
      }
      dispatch_enabled_ = true;
    }
    for (;;) {
      if (wait_for_dispatch_quiescence()) {
        break;
      }
#if defined(ESP_PLATFORM)
      taskYIELD();
#endif
    }
    ProviderAccess::set_running(provider_, false);
    {
      std::lock_guard<DispatchMutex> lock{dispatch_mutex_};
      dispatch_shutdown_requested_ = true;
    }
#if defined(ESP_PLATFORM)
    if (dispatch_wake_ != nullptr) {
      (void)xSemaphoreGive(dispatch_wake_);
    }
    while (!dispatcher_exited_.load(std::memory_order_acquire) || dispatch_task_ == nullptr ||
           eTaskGetState(dispatch_task_) != eSuspended) {
      vTaskDelay(1);
    }
    if (dispatch_task_ != nullptr) {
      vTaskDelete(dispatch_task_);
    }
    dispatch_task_ = nullptr;
    g_dispatcher_owner.store(nullptr, std::memory_order_release);
#elif defined(_WIN32)
    dispatch_changed_.notify_one();
    if (dispatch_thread_.joinable()) {
      dispatch_thread_.join();
    }
#else
    dispatch_changed_.notify_one();
    if (dispatch_thread_joinable_) {
      (void)pthread_join(dispatch_thread_, nullptr);
      dispatch_thread_joinable_ = false;
    }
#endif
    std::lock_guard<DispatchMutex> lock{dispatch_mutex_};
    dispatcher_started_ = false;
  }

  void shutdown_dispatcher_for_destructor() noexcept { stop_dispatcher_unbounded(); }

#if defined(ESP_PLATFORM)
  static void dispatch_task_entry(void *context) noexcept {
    auto *const self = static_cast<ProfileRuntimeServiceImplementation *>(context);
    self->dispatch_loop();
    self->dispatcher_exited_.store(true, std::memory_order_release);
    for (;;) {
      vTaskSuspend(nullptr);
    }
  }
#elif !defined(_WIN32)
  static void *dispatch_thread_entry(void *context) noexcept {
    static_cast<ProfileRuntimeServiceImplementation *>(context)->dispatch_loop();
    return nullptr;
  }
#endif

  void dispatch_loop() noexcept {
#if defined(ESP_PLATFORM)
    for (;;) {
      if (dispatch_wake_ == nullptr || xSemaphoreTake(dispatch_wake_, portMAX_DELAY) != pdTRUE) {
        continue;
      }
      bool should_exit = false;
      bool should_dispatch = false;
      {
        std::lock_guard<DispatchMutex> lock{dispatch_mutex_};
        should_exit = dispatch_shutdown_requested_ && !dispatch_pending_;
        should_dispatch = !should_exit && dispatch_enabled_ && dispatch_pending_;
        if (should_dispatch) {
          dispatch_pending_ = false;
          dispatch_active_ = true;
        }
      }
      if (should_exit) {
        return;
      }
      if (!should_dispatch) {
        continue;
      }
      callback_dispatch_identity_.store(current_execution_identity(), std::memory_order_release);
      ProviderAccess::dispatch_pending(provider_);
      callback_dispatch_identity_.store(0U, std::memory_order_release);
      {
        std::lock_guard<DispatchMutex> lock{dispatch_mutex_};
        dispatch_active_ = false;
      }
    }
#else
    for (;;) {
      {
        std::unique_lock<std::mutex> lock{dispatch_mutex_};
        dispatch_changed_.wait(lock, [this] {
          return dispatch_shutdown_requested_ || (dispatch_enabled_ && dispatch_pending_);
        });
        if (dispatch_shutdown_requested_ && !dispatch_pending_) {
          break;
        }
        if (!dispatch_enabled_ || !dispatch_pending_) {
          continue;
        }
        dispatch_pending_ = false;
        dispatch_active_ = true;
      }
      callback_dispatch_identity_.store(current_execution_identity(), std::memory_order_release);
      ProviderAccess::dispatch_pending(provider_);
      callback_dispatch_identity_.store(0U, std::memory_order_release);
      {
        std::lock_guard<DispatchMutex> lock{dispatch_mutex_};
        dispatch_active_ = false;
        if (!dispatch_pending_) {
          dispatch_changed_.notify_all();
        }
      }
    }
#endif
    dispatcher_exited_.store(true, std::memory_order_release);
  }

  RuntimeMutex publication_mutex_{};
  RuntimeMutex processing_mutex_{};
  DispatchMutex dispatch_mutex_{};
  std::atomic<ExecutionIdentity> lifecycle_owner_{0};
  std::atomic<ExecutionIdentity> callback_dispatch_identity_{0};
  std::atomic<bool> stopping_requested_{false};
  std::atomic<bool> fault_latched_{false};
  bool dispatcher_started_{false};
  bool dispatch_enabled_{false};
  bool dispatch_shutdown_requested_{false};
  bool dispatch_pending_{false};
  bool dispatch_active_{false};
  std::atomic<bool> dispatcher_exited_{true};
  bool provider_active_{false};
#if defined(ESP_PLATFORM)
  StaticSemaphore_t dispatch_wake_storage_{};
  SemaphoreHandle_t dispatch_wake_{xSemaphoreCreateBinaryStatic(&dispatch_wake_storage_)};
  TaskHandle_t dispatch_task_{nullptr};
#elif defined(_WIN32)
  std::condition_variable dispatch_changed_{};
  std::thread dispatch_thread_{};
#else
  std::condition_variable dispatch_changed_{};
  pthread_t dispatch_thread_{};
  bool dispatch_thread_joinable_{false};
#endif
  ProfileProvider provider_;
  DispatcherGuard dispatcher_guard_{this};
  vehicle_telemetry::Runtime runtime_;
  vehicle_telemetry::StatusResult configuration_status_{};
};

static_assert(sizeof(ProfileRuntimeServiceImplementation) <= kProfileRuntimeServiceStorageBytes,
              "profile runtime service storage is below its private implementation size");
static_assert(alignof(ProfileRuntimeServiceImplementation) <= alignof(ProfileRuntimeService),
              "profile runtime service storage alignment is insufficient");

} // namespace

#if defined(WEACT_PROFILE_LINK_PROBE)
extern "C" {
__attribute__((used)) volatile const std::uint32_t
    vehicle_profile_link_probe_service_implementation_bytes =
        static_cast<std::uint32_t>(sizeof(ProfileRuntimeServiceImplementation));
__attribute__((used)) volatile const std::uint32_t
    vehicle_profile_link_probe_service_implementation_alignment =
        static_cast<std::uint32_t>(alignof(ProfileRuntimeServiceImplementation));
}
#endif

ProfileRuntimeService::ProfileRuntimeService(
    const ProfileTables &tables, vehicle_core::MonotonicClock &clock,
    vehicle_telemetry::AcquisitionSource &source) noexcept {
  (void)new (static_cast<void *>(storage_))
      ProfileRuntimeServiceImplementation{tables, clock, source};
}

ProfileRuntimeService::~ProfileRuntimeService() noexcept {
  auto *const service =
      std::launder(reinterpret_cast<ProfileRuntimeServiceImplementation *>(storage_));
  service->~ProfileRuntimeServiceImplementation();
}

vehicle_telemetry::StatusResult ProfileRuntimeService::start() noexcept {
  auto *const service =
      std::launder(reinterpret_cast<ProfileRuntimeServiceImplementation *>(storage_));
  return service->start();
}

vehicle_telemetry::StatusResult ProfileRuntimeService::stop() noexcept {
  auto *const service =
      std::launder(reinterpret_cast<ProfileRuntimeServiceImplementation *>(storage_));
  return service->stop();
}

vehicle_signals::SignalProvider &ProfileRuntimeService::provider() noexcept {
  auto *const service =
      std::launder(reinterpret_cast<ProfileRuntimeServiceImplementation *>(storage_));
  return service->provider();
}

const vehicle_signals::SignalProvider &ProfileRuntimeService::provider() const noexcept {
  const auto *const service =
      std::launder(reinterpret_cast<const ProfileRuntimeServiceImplementation *>(storage_));
  return service->provider();
}

} // namespace vehicle_profile_runtime
