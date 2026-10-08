#include "freertos_runtime_stats_logger.hpp"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

#if !CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER
#error "FreeRTOS task runtime diagnostics require the ESP timer runtime-stat clock"
#endif

namespace weact_can485::freertos_runtime_stats {
namespace {

constexpr char kTag[] = "freertos_stats";
constexpr std::uint32_t kInitialDelayMs = 100;
constexpr std::uint32_t kIntervalMs = 5000;
constexpr std::uint32_t kTaskStackBytes = 6144;
constexpr UBaseType_t kTaskPriority = tskIDLE_PRIORITY + 1;
constexpr std::size_t kTaskCapacity = 40;

struct TaskSnapshot final {
  TaskHandle_t handle{nullptr};
  UBaseType_t task_number{0};
  eTaskState state{eInvalid};
  UBaseType_t current_priority{0};
  configRUN_TIME_COUNTER_TYPE runtime_counter{0};
  configSTACK_DEPTH_TYPE stack_high_water_mark{0};
  std::array<char, configMAX_TASK_NAME_LEN> name{};
#if defined(CONFIG_FREERTOS_SMP) && (configUSE_CORE_AFFINITY == 1) && (configNUMBER_OF_CORES > 1)
  UBaseType_t core_affinity_mask{0};
#elif configTASKLIST_INCLUDE_COREID == 1
  BaseType_t core_id{0};
#endif
};

std::array<TaskStatus_t, kTaskCapacity> raw_tasks{};
std::array<TaskSnapshot, kTaskCapacity> previous_tasks{};
std::array<TaskSnapshot, kTaskCapacity> current_tasks{};
UBaseType_t previous_task_count{0};
configRUN_TIME_COUNTER_TYPE previous_total_runtime{0};

struct Capture final {
  UBaseType_t task_count{0};
  configRUN_TIME_COUNTER_TYPE total_runtime{0};
};

void copy_task_snapshot(const TaskStatus_t &source, TaskSnapshot &destination) noexcept {
  destination.handle = source.xHandle;
  destination.task_number = source.xTaskNumber;
  destination.state = source.eCurrentState;
  destination.current_priority = source.uxCurrentPriority;
  destination.runtime_counter = source.ulRunTimeCounter;
  destination.stack_high_water_mark = source.usStackHighWaterMark;
  destination.name.fill('\0');
  if (source.pcTaskName != nullptr)
    std::memcpy(destination.name.data(), source.pcTaskName, configMAX_TASK_NAME_LEN - 1);
#if defined(CONFIG_FREERTOS_SMP) && (configUSE_CORE_AFFINITY == 1) && (configNUMBER_OF_CORES > 1)
  destination.core_affinity_mask = source.uxCoreAffinityMask;
#elif configTASKLIST_INCLUDE_COREID == 1
  destination.core_id = source.xCoreID;
#endif
}

[[nodiscard]] bool capture(std::array<TaskSnapshot, kTaskCapacity> &tasks,
                           Capture &result) noexcept {
  // TaskStatus_t::pcTaskName points into the task's TCB. FreeRTOS suspends and
  // resumes the scheduler inside uxTaskGetSystemState(), so keep an outer
  // suspension around the call and copy every name before task deletion can
  // reclaim that TCB.
  vTaskSuspendAll();
  const auto required = uxTaskGetNumberOfTasks();
  if (required > tasks.size()) {
    (void)xTaskResumeAll();
    ESP_LOGW(kTag, "task snapshot needs %u entries; fixed capacity is %u",
             static_cast<unsigned>(required), static_cast<unsigned>(tasks.size()));
    return false;
  }

  const auto task_count = uxTaskGetSystemState(
      raw_tasks.data(), static_cast<UBaseType_t>(raw_tasks.size()), &result.total_runtime);
  const bool capture_succeeded = task_count != 0 && task_count <= tasks.size();
  if (capture_succeeded) {
    for (UBaseType_t index = 0; index < task_count; ++index)
      copy_task_snapshot(raw_tasks[index], tasks[index]);
  }
  (void)xTaskResumeAll();

  result.task_count = capture_succeeded ? task_count : 0;
  if (!capture_succeeded) {
    ESP_LOGW(kTag, "task snapshot failed; task count changed or capacity was insufficient");
    return false;
  }
  return true;
}

template <typename Counter>
[[nodiscard]] std::uint64_t counter_delta(const Counter current, const Counter previous) noexcept {
  return static_cast<std::uint64_t>(current - previous);
}

[[nodiscard]] const TaskSnapshot *find_previous(const TaskHandle_t handle,
                                                const UBaseType_t task_number) noexcept {
  for (UBaseType_t index = 0; index < previous_task_count; ++index) {
    if (previous_tasks[index].handle == handle && previous_tasks[index].task_number == task_number)
      return &previous_tasks[index];
  }
  return nullptr;
}

[[nodiscard]] bool task_name_matches(const char *const actual,
                                     const char *const expected) noexcept {
  if (actual == nullptr)
    return false;
  return std::strncmp(actual, expected, configMAX_TASK_NAME_LEN - 1) == 0;
}

[[nodiscard]] const TaskSnapshot *find_current_named(const Capture &capture,
                                                     const char *const name) noexcept {
  for (UBaseType_t index = 0; index < capture.task_count; ++index) {
    if (task_name_matches(current_tasks[index].name.data(), name))
      return &current_tasks[index];
  }
  return nullptr;
}

[[nodiscard]] bool is_ble_nimble_task(const char *const name) noexcept {
  if (name == nullptr)
    return false;
  return std::strncmp(name, "nimble", 6) == 0 || std::strncmp(name, "ble_", 4) == 0 ||
         std::strncmp(name, "bt", 2) == 0 || std::strncmp(name, "BT", 2) == 0 ||
         std::strcmp(name, "hciT") == 0 || std::strcmp(name, "hciR") == 0;
}

[[nodiscard]] const char *task_state_name(const eTaskState state) noexcept {
  switch (state) {
  case eRunning:
    return "running";
  case eReady:
    return "ready";
  case eBlocked:
    return "blocked";
  case eSuspended:
    return "suspended";
  case eDeleted:
    return "deleted";
  case eInvalid:
    return "invalid";
  }
  return "unknown";
}

void format_affinity(const TaskSnapshot &task, char *const destination,
                     const std::size_t capacity) noexcept {
#if defined(CONFIG_FREERTOS_SMP) && (configUSE_CORE_AFFINITY == 1) && (configNUMBER_OF_CORES > 1)
  const auto all_cores = (static_cast<UBaseType_t>(1) << configNUMBER_OF_CORES) - 1;
  if (task.core_affinity_mask == all_cores)
    (void)std::snprintf(destination, capacity, "any");
  else if (task.core_affinity_mask == 1)
    (void)std::snprintf(destination, capacity, "core0");
  else if (task.core_affinity_mask == 2)
    (void)std::snprintf(destination, capacity, "core1");
  else
    (void)std::snprintf(destination, capacity, "mask_0x%lx",
                        static_cast<unsigned long>(task.core_affinity_mask));
#elif configTASKLIST_INCLUDE_COREID == 1
  if (task.core_id == tskNO_AFFINITY)
    (void)std::snprintf(destination, capacity, "any");
  else
    (void)std::snprintf(destination, capacity, "core%ld", static_cast<long>(task.core_id));
#else
  (void)task;
  (void)std::snprintf(destination, capacity, "unavailable");
#endif
}

[[nodiscard]] std::uint64_t share_x100(const std::uint64_t task_runtime,
                                       const std::uint64_t interval_runtime) noexcept {
  const auto capacity_runtime =
      interval_runtime * static_cast<std::uint64_t>(CONFIG_FREERTOS_NUMBER_OF_CORES);
  if (capacity_runtime == 0)
    return 0;
  return task_runtime * 10'000ULL / capacity_runtime;
}

void log_task(const Capture &capture, const char *const expected_name,
              const std::uint64_t interval_runtime) noexcept {
  const auto *const current = find_current_named(capture, expected_name);
  if (current == nullptr) {
    ESP_LOGI(kTag, "TASKSTAT_TASK v=1 name=%s status=missing", expected_name);
    return;
  }

  char affinity[16]{};
  format_affinity(*current, affinity, sizeof(affinity));
  const auto *const previous = find_previous(current->handle, current->task_number);
  if (previous == nullptr) {
    ESP_LOGI(kTag,
             "TASKSTAT_TASK v=1 name=%s status=new priority=%u affinity=%s "
             "observed_core=unavailable stack_hwm_bytes=%llu state=%s",
             current->name.data(), static_cast<unsigned>(current->current_priority), affinity,
             static_cast<unsigned long long>(current->stack_high_water_mark) * sizeof(StackType_t),
             task_state_name(current->state));
    return;
  }

  const auto runtime = counter_delta(current->runtime_counter, previous->runtime_counter);
  const auto share = share_x100(runtime, interval_runtime);
  ESP_LOGI(kTag,
           "TASKSTAT_TASK v=1 name=%s status=ok runtime_us=%llu cpu_pct=%llu.%02llu "
           "priority=%u affinity=%s observed_core=unavailable stack_hwm_bytes=%llu state=%s",
           current->name.data(), static_cast<unsigned long long>(runtime),
           static_cast<unsigned long long>(share / 100),
           static_cast<unsigned long long>(share % 100),
           static_cast<unsigned>(current->current_priority), affinity,
           static_cast<unsigned long long>(current->stack_high_water_mark) * sizeof(StackType_t),
           task_state_name(current->state));
}

void log_ble_nimble_group(const Capture &capture, const std::uint64_t interval_runtime) noexcept {
  std::uint64_t runtime = 0;
  std::uint64_t minimum_stack = std::numeric_limits<std::uint64_t>::max();
  UBaseType_t maximum_priority = 0;
  std::size_t members = 0;
  std::size_t new_members = 0;
  char common_affinity[16]{};
  bool affinity_initialized = false;
  bool affinity_mixed = false;

  for (UBaseType_t index = 0; index < capture.task_count; ++index) {
    const auto &task = current_tasks[index];
    if (!is_ble_nimble_task(task.name.data()))
      continue;
    ++members;
    minimum_stack = std::min(minimum_stack, static_cast<std::uint64_t>(task.stack_high_water_mark) *
                                                sizeof(StackType_t));
    maximum_priority = std::max(maximum_priority, task.current_priority);
    const auto *const previous = find_previous(task.handle, task.task_number);
    if (previous == nullptr)
      ++new_members;
    else
      runtime += counter_delta(task.runtime_counter, previous->runtime_counter);
    char task_affinity[16]{};
    format_affinity(task, task_affinity, sizeof(task_affinity));
    if (!affinity_initialized) {
      (void)std::snprintf(common_affinity, sizeof(common_affinity), "%s", task_affinity);
      affinity_initialized = true;
    } else if (std::strcmp(common_affinity, task_affinity) != 0) {
      affinity_mixed = true;
    }
  }

  if (members == 0) {
    ESP_LOGI(kTag, "TASKSTAT_GROUP v=1 name=ble_nimble status=missing members=0");
    return;
  }

  const char *affinity = common_affinity;
  if (affinity_mixed)
    affinity = "mixed";
  const auto share = share_x100(runtime, interval_runtime);
  ESP_LOGI(kTag,
           "TASKSTAT_GROUP v=1 name=ble_nimble status=ok runtime_us=%llu cpu_pct=%llu.%02llu "
           "priority_max=%u affinity=%s observed_core=unavailable stack_hwm_min_bytes=%llu "
           "members=%u new_members=%u",
           static_cast<unsigned long long>(runtime), static_cast<unsigned long long>(share / 100),
           static_cast<unsigned long long>(share % 100), static_cast<unsigned>(maximum_priority),
           affinity, static_cast<unsigned long long>(minimum_stack), static_cast<unsigned>(members),
           static_cast<unsigned>(new_members));
}

} // namespace

bool Logger::start() noexcept {
  return xTaskCreate(&Logger::task_entry, "runtime_stats", kTaskStackBytes, this, kTaskPriority,
                     nullptr) == pdPASS;
}

void Logger::task_entry(void *const context) noexcept {
  static_cast<Logger *>(context)->run();
  vTaskDelete(nullptr);
}

void Logger::run() noexcept {
  vTaskDelay(pdMS_TO_TICKS(kInitialDelayMs));

  Capture previous_capture{};
  PauseSnapshot previous_pause{};
  if (pause_reader_ != nullptr)
    previous_pause = pause_reader_(pause_context_);
  if (capture(previous_tasks, previous_capture)) {
    previous_task_count = previous_capture.task_count;
    previous_total_runtime = previous_capture.total_runtime;
  }

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(kIntervalMs));

    Capture current_capture{};
    if (!capture(current_tasks, current_capture))
      continue;
    if (previous_task_count == 0) {
      std::copy_n(current_tasks.begin(), current_capture.task_count, previous_tasks.begin());
      previous_task_count = current_capture.task_count;
      previous_total_runtime = current_capture.total_runtime;
      if (pause_reader_ != nullptr)
        previous_pause = pause_reader_(pause_context_);
      continue;
    }

    const auto interval_runtime =
        counter_delta(current_capture.total_runtime, previous_total_runtime);
    const auto current_pause =
        pause_reader_ == nullptr ? PauseSnapshot{} : pause_reader_(pause_context_);
    const auto pause_delta = current_pause.available && previous_pause.available
                                 ? current_pause.total_pauses - previous_pause.total_pauses
                                 : 0;
    const auto requested_ticks =
        current_pause.available
            ? (static_cast<std::uint64_t>(current_pause.requested_pause_ms) * configTICK_RATE_HZ +
               999ULL) /
                  1000ULL
            : 0;
    const auto requested_us = requested_ticks * 1'000'000ULL / configTICK_RATE_HZ;

    if (current_pause.actual_blocked_available && previous_pause.actual_blocked_available) {
      const auto actual_delta =
          current_pause.total_actual_blocked_us - previous_pause.total_actual_blocked_us;
      ESP_LOGI(kTag,
               "TASKSTAT v=1 interval_us=%llu tick_hz=%u cores=%u snapshot_tasks=%u "
               "capacity=%u pauses=%llu pause_request_ms=%u pause_request_ticks=%llu "
               "pause_request_us=%llu pause_actual_us=%llu watchdog_events=unavailable",
               static_cast<unsigned long long>(interval_runtime),
               static_cast<unsigned>(configTICK_RATE_HZ),
               static_cast<unsigned>(CONFIG_FREERTOS_NUMBER_OF_CORES),
               static_cast<unsigned>(current_capture.task_count),
               static_cast<unsigned>(current_tasks.size()),
               static_cast<unsigned long long>(pause_delta), current_pause.requested_pause_ms,
               static_cast<unsigned long long>(requested_ticks),
               static_cast<unsigned long long>(requested_us),
               static_cast<unsigned long long>(actual_delta));
    } else {
      ESP_LOGI(kTag,
               "TASKSTAT v=1 interval_us=%llu tick_hz=%u cores=%u snapshot_tasks=%u "
               "capacity=%u pauses=%llu pause_request_ms=%u pause_request_ticks=%llu "
               "pause_request_us=%llu pause_actual_us=unavailable "
               "watchdog_events=unavailable",
               static_cast<unsigned long long>(interval_runtime),
               static_cast<unsigned>(configTICK_RATE_HZ),
               static_cast<unsigned>(CONFIG_FREERTOS_NUMBER_OF_CORES),
               static_cast<unsigned>(current_capture.task_count),
               static_cast<unsigned>(current_tasks.size()),
               static_cast<unsigned long long>(pause_delta), current_pause.requested_pause_ms,
               static_cast<unsigned long long>(requested_ticks),
               static_cast<unsigned long long>(requested_us));
    }

    constexpr std::array<const char *, 6> kImportantTasks{
        "can_rx", "vehicle_telemetry", "mazda_notify", "local_argb", "IDLE0", "IDLE1"};
    for (const auto *const name : kImportantTasks)
      log_task(current_capture, name, interval_runtime);
    log_ble_nimble_group(current_capture, interval_runtime);

    std::copy_n(current_tasks.begin(), current_capture.task_count, previous_tasks.begin());
    previous_task_count = current_capture.task_count;
    previous_total_runtime = current_capture.total_runtime;
    previous_pause = current_pause;
  }
}

} // namespace weact_can485::freertos_runtime_stats
