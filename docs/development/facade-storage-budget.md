# Telemetry facade storage budget

`mazda::VehicleTelemetry` reserves fixed storage inside the facade object and
placement-constructs the private `internal::VehicleTelemetryService` in it.
Task, lock, decoder and publication types therefore stay out of the public
include graph, and constructing the facade does not allocate. This page records
the measured size of the service and why each reservation has its value
(issue #117 in `Yuke-hd/esp32-vehicle-can-core`, filed before the repository
split). The figures are compile-time build evidence only. No hardware or
vehicle validation is involved.

## Measured service size

The figures were re-measured for the blind-spot channels (#307) on 2026-10-27
using temporary `sizeof`/`alignof` host probes and ESP32 object-symbol sizes
compiled with the firmware compile-command flags. The probes were not committed.

| Configuration | Toolchain | `sizeof` | `alignof` | `alignof(max_align_t)` |
| --- | --- | ---: | ---: | ---: |
| Host, default options | Apple clang 17, arm64 | 29,168 B | 8 | 8 |
| Host, `MAZDA_ENABLE_DEBUG_TELEMETRY`, `MAZDA_ENABLE_TELEMETRY_PROFILING` and `MAZDA_ENABLE_TELEMETRY_STAGE_PROFILING` | Apple clang 17, arm64 | 32,896 B | 8 | 8 |
| ESP32, `sdkconfig.defaults` | ESP-IDF 5.5.4, xtensa-esp-elf GCC 14.2.0 | 24,704 B | 8 | 8 |
| ESP32, `sdkconfig.defaults` plus `CONFIG_WEACT_CAN_FRESHNESS_DEBUG` and `CONFIG_WEACT_CAN_TELEMETRY_PROFILING` | ESP-IDF 5.5.4, xtensa-esp-elf GCC 14.2.0 | 28,320 B | 8 | 8 |

The opt-in diagnostics add the debug recorder, its pending-frame state and the
stage-profiler histograms to the service, so the diagnostic layouts are the
largest per target. `CONFIG_WEACT_CAN_FREERTOS_RUNTIME_STATS` and
`CONFIG_WEACT_CAN_TELEMETRY_BENCHMARK` do not change the service layout.

The two additional bounded notification channels, registration records and
BSM state increase the host diagnostic layout beyond its previous 32 KiB
reservation. The runtime core dependency and its own storage remain unchanged.

## Budget

| Target | `kImplementationStorageBytes` | Largest measured layout | Headroom |
| --- | ---: | ---: | ---: |
| Host (`ESP_PLATFORM` undefined) | 33,792 B (33 KiB) | 32,896 B | 896 B |
| ESP32 (`ESP_PLATFORM` defined) | 29,696 B (29 KiB) | 28,320 B | 1,376 B |

With default options the headroom is 4,624 B on the host and 4,992 B on
ESP32. Storage alignment is `alignof(std::max_align_t)` on both targets.

The constructor in `vehicle_telemetry.cpp` asserts at compile time that the
service size and alignment fit the reservation, and that the facade contains
nothing but the storage. A failed check names the budget and points back to
this page.

### Why per target

The host diagnostic layout needs 32,896 B, so its reservation grows by one
KiB to 33 KiB. The ESP32 reservation grows by one KiB to 29 KiB to keep its
1,376 B diagnostic headroom at least equal to the host's 896 B. A shared
33 KiB reservation would waste four KiB of static DRAM in production firmware.

The ESP32 budget keeps at least as much headroom as the host budget, in both
the default and the diagnostic layouts. Because the 64-bit host layout grows
faster than the 32-bit layout for the same member change, a service that
outgrows its budget fails the host build first. The host build then needs a new
measurement, and the firmware budget is re-checked at the same time. A change
should not pass the host build and fail only the firmware build.

The `ESP_PLATFORM` condition in the public header selects a number only. It
does not add an RTOS, driver or implementation type to the include graph, and
every translation unit in one build sees the same layout.

### Updating the budget

When a service change fails the guard, re-measure all four configurations
above, raise the affected budget in 1 KiB steps until the largest layout for
that target fits, and update the tables on this page. Keep the ESP32 headroom
at least equal to the host headroom. Do not lower a budget below the largest
measured diagnostic layout for its target.

## Allocation and stack

The firmware composition root defines the facade as a namespace-scope
`static` in `firmware/weact-can485-v1.1/main/main.cpp`, and the opt-in
synthetic benchmark uses a function-local `static` in
`components/mazda_telemetry/src/telemetry_synthetic_benchmark.cpp`. The
storage is therefore static `.bss` in internal DRAM. It is never on a FreeRTOS
task stack, and it is not heap memory.

The facade constructor placement-constructs the service directly in the
storage. C++17 guaranteed copy elision means no temporary service exists on
the constructing stack. The benchmark-only and host-only replacement paths
destroy the service and construct the new one in the same storage, also
without a temporary.

Host tests and the replay controller place facades on the test thread's stack
or inside a heap-owned controller. Host stacks are large enough that a
32 KiB object there has no embedded resource implication.

Steady-state receive, decode, poll and notify paths are unchanged by the
budget. They neither allocate nor move the service.

## Alternatives considered

- **Shared 32 KiB reservation (no change).** Sufficient on both targets, but
  it holds about 9 KiB of unused static DRAM on ESP32 with no benefit.
- **Private static singleton.** A function-local or namespace-scope service in
  the implementation file would shrink the facade to a handle. Host tests and
  the replay controller create several facades in one process, so this would
  need a multi-instance manager, which is out of scope.
- **Heap allocation at startup.** This removes the size coupling but breaks the
  documented no-allocation construction and adds a failure path to an
  infallible constructor.
- **A smaller shared budget per configuration.** Selecting the budget from
  diagnostic options would put `sdkconfig` or feature macros into the public
  header and couple every consumer to diagnostic build options. The two
  target values cover all measured configurations without that coupling.
