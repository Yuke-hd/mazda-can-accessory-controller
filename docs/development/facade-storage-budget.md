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

The figures were measured at `43438c6` by instantiating an undefined template
with `sizeof` and `alignof` of `VehicleTelemetryService` in
`components/mazda_telemetry/src/vehicle_telemetry.cpp`, so the compiler printed
the values in its diagnostic. That probe was not committed.

| Configuration | Toolchain | `sizeof` | `alignof` | `alignof(max_align_t)` |
| --- | --- | ---: | ---: | ---: |
| Host, default options | Apple clang 17, arm64 | 27,976 B | 8 | 8 |
| Host, `MAZDA_ENABLE_DEBUG_TELEMETRY`, `MAZDA_ENABLE_TELEMETRY_PROFILING` and `MAZDA_ENABLE_TELEMETRY_STAGE_PROFILING` | Apple clang 17, arm64 | 31,704 B | 8 | 8 |
| ESP32, `sdkconfig.defaults` | ESP-IDF 5.5.4, xtensa-esp-elf GCC 14.2.0 | 23,704 B | 8 | 8 |
| ESP32, `sdkconfig.defaults` plus `CONFIG_WEACT_CAN_FRESHNESS_DEBUG` and `CONFIG_WEACT_CAN_TELEMETRY_PROFILING` | ESP-IDF 5.5.4, xtensa-esp-elf GCC 14.2.0 | 27,320 B | 8 | 8 |

The opt-in diagnostics add the debug recorder, its pending-frame state and the
stage-profiler histograms to the service, so the diagnostic layouts are the
largest per target. `CONFIG_WEACT_CAN_FREERTOS_RUNTIME_STATS` and
`CONFIG_WEACT_CAN_TELEMETRY_BENCHMARK` do not change the service layout.

On the largest host layout the generic `vehicle_telemetry::Runtime` is
16,384 B, the publication store 1,912 B, the host runtime source 2,248 B, the
debug recorder 1,816 B and `VehicleState` 1,352 B. The remainder is mainly the
fixed notification channels. The ESP32 layout is smaller because pointers and
`std::size_t` are 4 bytes there.

## Budget

| Target | `kImplementationStorageBytes` | Largest measured layout | Headroom |
| --- | ---: | ---: | ---: |
| Host (`ESP_PLATFORM` undefined) | 32,768 B (32 KiB) | 31,704 B | 1,064 B |
| ESP32 (`ESP_PLATFORM` defined) | 28,672 B (28 KiB) | 27,320 B | 1,352 B |

With default options the headroom is 4,792 B on the host and 4,968 B on
ESP32. Storage alignment is `alignof(std::max_align_t)` on both targets.

The constructor in `vehicle_telemetry.cpp` asserts at compile time that the
service size and alignment fit the reservation, and that the facade contains
nothing but the storage. A failed check names the budget and points back to
this page.

### Why per target

The host budget is unchanged: the largest host layout needs 31,704 B, so 32 KiB
is required there. The ESP32 layout is about 4 KiB smaller in every
configuration, so a shared 32 KiB reservation would leave about 9 KiB of unused
static DRAM in the production firmware. Lowering the ESP32 value to 28 KiB
returns 4,096 B of `.bss` to the heap.

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
