# FreeRTOS task runtime diagnostics

Issue #228 adds a default-off ESP-only scheduler diagnostic selected by
`CONFIG_WEACT_CAN_FREERTOS_RUNTIME_STATS`. It emits one summary every five
seconds from FreeRTOS runtime counters. It does not use the telemetry stage
profiler's wall-clock spans to infer CPU use, and it changes no task priority,
affinity, watchdog, receive-only, work-budget, or fail-off setting.

## Build configuration

The option selects the ESP-IDF 5.5.4 facilities needed by
`uxTaskGetSystemState()`:

- `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y`;
- `CONFIG_FREERTOS_USE_TRACE_FACILITY=y` and
  `CONFIG_FREERTOS_USE_STATS_FORMATTING_FUNCTIONS=y`, selected transitively by
  runtime stats;
- `CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID=y` for affinity metadata on the
  default ESP-IDF FreeRTOS kernel; the SMP kernel exposes an affinity mask; and
- `CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER=y`, the 1 MHz runtime clock
  selected by ESP-IDF's default choice and required by the diagnostic source.

The implementation calls the raw `uxTaskGetSystemState()` API and formats only
the bounded records it needs. ESP-IDF recommends the raw API over the formatted
convenience function for production code, while also warning that the snapshot
suspends the scheduler and is intended as a debug aid. The diagnostic therefore
uses fixed storage for 40 task records, samples only every five seconds, and is
off in `sdkconfig.defaults`.

The task rows use `xTaskGetHandle()` for the persistent task names and match
the returned handles plus each snapshot's task number. They never dereference
`TaskStatus_t::pcTaskName` after `uxTaskGetSystemState()` returns: that pointer
belongs to TCB storage and can become invalid when another SMP core deletes a
task. The BLE aggregate is reported as unavailable until the firmware has an
explicit stable handle set for the dynamic NimBLE task group.

Build the normal diagnostic image out of tree:

```sh
source /Users/yuke/ESP-IDF/v5.5.4/export.sh
idf.py -C firmware/weact-can485-v1.1 \
  -B /tmp/mazda-runtime-stats \
  -D SDKCONFIG=/tmp/mazda-runtime-stats/sdkconfig \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.runtime-stats.defaults' \
  set-target esp32 build
```

The isolated synthetic profile uses a fixed, reviewed sequence of 4,096
generated frames and starts no TWAI, CAN acquisition, BLE, or LED renderer:

```sh
idf.py -C firmware/weact-can485-v1.1 \
  -B /tmp/mazda-runtime-stats-synthetic \
  -D SDKCONFIG=/tmp/mazda-runtime-stats-synthetic/sdkconfig \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.runtime-stats-synthetic.defaults' \
  set-target esp32 build
```

The source is continuously ready while those frames are consumed. With runtime
diagnostics selected, it keeps the telemetry worker and dispatcher alive until
6.5 seconds after start so the five-second snapshot can include them. The
benchmark's `elapsed_us` still ends when the source is first requested after
the final frame, before this diagnostic hold.

Official references:

- [ESP-IDF 5.5 FreeRTOS API](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32/api-reference/system/freertos_idf.html)
- [ESP-IDF real-time stats example](https://github.com/espressif/esp-idf/tree/v5.5.4/examples/system/freertos/real_time_stats)
- [ESP-IDF 5.5.4 FreeRTOS Kconfig](https://github.com/espressif/esp-idf/blob/v5.5.4/components/freertos/Kconfig)

## Records

Each interval begins with one `TASKSTAT v=1` record. `interval_us` is the
ESP timer runtime-counter delta and `cpu_pct` in task/group records is the
interval task-runtime delta divided by the available runtime of both cores.
A task running continuously on one core therefore approaches 50%, while all
task rows together approach 100%.

The header also reports:

- `tick_hz` and core count;
- telemetry `pauses` in this interval;
- the configured `pause_request_ms`, its tick-rounded
  `pause_request_ticks`, and the requested minimum `pause_request_us`;
- `pause_actual_us`; and
- `watchdog_events`.

The following `TASKSTAT_TASK v=1` records cover `can_rx`,
`vehicle_telemetry` (which FreeRTOS may truncate to `vehicle_telemet`),
`mazda_notify`, `local_argb`, `IDLE0`, and `IDLE1`. Each present task reports
its interval `runtime_us`, `cpu_pct`, current priority, configured affinity,
lifetime stack high-water mark converted from FreeRTOS words to bytes, and
state at the end snapshot.
`observed_core=unavailable` is explicit because ESP-IDF's task snapshot exposes
configured affinity, not the last core on which an unpinned task ran.

`TASKSTAT_GROUP v=1 name=ble_nimble` is currently reported as unavailable.
Dynamic NimBLE task names do not provide a lifecycle-safe handle set for this
diagnostic, so the logger does not scan borrowed `TaskStatus_t::pcTaskName`
pointers to guess group membership. A future explicit stable handle set can
restore aggregate runtime, member count, maximum priority, minimum member stack
headroom, and affinity reporting.

## Pause and watchdog limits

The pinned `esp32-vehicle-can-core` 0.2.1 runtime exposes the cumulative
`work_budget_pauses` count and the configured request. It records the count
immediately before its private `vTaskDelay(1)` loop, but exposes no completion
timestamp or hook. Consequently this firmware can distinguish intervals with
deliberate budget pauses from intervals without them and can report the exact
tick-rounded request, but `pause_actual_us=unavailable` remains explicit.

The diagnostic does not treat wall time between telemetry callbacks, scheduler
delay, source receive waits, or time in another task as budget-block time. A
narrow exact measurement needs a reviewed core interface that brackets only
the private budget pause. A generic FreeRTOS delay/context-switch trace hook
would affect all tasks and cannot identify that cause reliably, so it is not
enabled here.

The application watchdogs likewise expose fail-off/restart behavior, not an
event counter that can be sampled without changing their interfaces. The
header therefore reports `watchdog_events=unavailable`. Existing watchdog
configuration and behavior are unchanged.

## Cost and validation scope

Enabling the option adds FreeRTOS's per-task runtime counter update on scheduler
activity, task metadata used by `uxTaskGetSystemState()`, the fixed diagnostic
task/storage, a five-second snapshot that temporarily suspends scheduling, and
serial formatting. The 32-bit 1 MHz ESP timer counter wraps after about 71.6
minutes; unsigned interval subtraction remains valid for one wrap within a
five-second interval. A snapshot that needs more than 40 task entries is
rejected and logged instead of returning a partial summary.

Target overhead evidence for this change is recorded below after comparing the
same 4,096-frame synthetic workload with runtime diagnostics off and on. These
results are scheduler/software evidence from an isolated board. They are not
CAN-driver, LED, bench-wiring, or vehicle validation.

An authorized isolated WeAct CAN485 V1.1 run used a protected USB supply with
vehicle CAN and LED-strip wiring disconnected. The ESP-IDF 5.5.4 synthetic
image completed the fixed workload with `started=1`, `timed_out=0`,
`frames_received=4096`, and `frames_processed=4096`. Its first five-second
snapshot reported `tick_hz=100`, `cores=2`, `pauses=167`,
`pause_request_ms=1`, `pause_request_ticks=1`, and
`pause_request_us=10000`. It also reported `vehicle_telemet` at 31.21% with
1,612 bytes of stack headroom, `mazda_notify` at 0.43% with 3,140 bytes,
`IDLE0` at 17.95% with 1,008 bytes, and `IDLE1` at 49.99% with 1,016 bytes.
The benchmark line reported `elapsed_us=4725530` and
`work_budget_pauses=216`.

The normal runtime image produced five-second records with 15 tasks. One
interval reported `can_rx` at 0.33% (priority 23, blocked, 3,328 bytes),
`vehicle_telemet` at 1.98% (priority 1, ready, 1,856 bytes), `mazda_notify`
at 0.43% (priority 21, blocked, 1,900 bytes), `local_argb` at 0.54%
(priority 2, blocked, 2,884 bytes), `IDLE0` at 38.76% on core 0 with 892
bytes, and `IDLE1` at 49.01% on core 1 with 996 bytes. The BLE group reports
`status=unavailable` because this implementation has no explicit stable handle
set for the dynamic NimBLE tasks.
Configured affinity and `observed_core=unavailable` are both retained in the
record because the snapshot does not expose the last core for an unpinned
task.

The matching diagnostics-off synthetic image also completed 4,096/4,096
frames and reported `elapsed_us=5589333` and `work_budget_pauses=186`. The
on/off elapsed values are inconsistent across this target run, so they are
recorded as observations and are not presented as a controlled overhead
measurement. The documented cost remains qualitative: runtime counters and
task metadata are enabled only by the opt-in Kconfig, the logger samples every
five seconds, and each snapshot temporarily suspends scheduling.

The pinned `vehicle_core` 0.2.1 interface exposes the pause count and the
tick-rounded request but no exact completion timestamp, so every target header
reports `pause_actual_us=unavailable`. The application watchdogs expose
fail-off/restart behavior but no sampled event counter, so every header also
reports `watchdog_events=unavailable`. No priority, affinity, watchdog, or
work-budget setting was changed for this diagnostic.
