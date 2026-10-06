# ADR-0004: Install the vehicle I/O interrupts on core 1 and leave BLE on core 0

Status: Proposed
Date: 2026-10-04
Retrospective: no

## Context

The companion BLE link ([ADR-0001](0001-ble-companion-transport.md)) adds the
ESP32 Bluetooth controller, the NimBLE host and NimBLE timer callouts to a
firmware whose priority is receiving vehicle CAN and driving the LEDs.

ESP-IDF allocates a driver's interrupt on the core that calls its install
function. Before this change, `app_main` ran on core 0 and installed:

- the RMT and SPI interrupts, via `local_argb::start()`;
- the TWAI interrupt, via `VehicleTelemetry::start()` and the external
  `can_bus` `start()`.

None of these drivers exposes a core option that the firmware can use:

- legacy `twai_general_config_t` has only `intr_flags`;
- `led_strip`'s SPI backend builds its own `spi_bus_config_t` without
  `isr_cpu_id`;
- RMT has only `intr_priority`.

`can_bus` is pinned at `esp32-vehicle-can-core` 0.1.0 and installs TWAI only
in `start()`. Pinning the `can_rx` task alone would not move the interrupt.

By default the Bluetooth controller and the NimBLE host also run on core 0.
The controller uses level-4 interrupts there. With
`CONFIG_BT_NIMBLE_USE_ESP_TIMER=y`, NimBLE callouts fire in the `esp_timer`
task at priority 22 on core 0.

CAN is the main concern. The TWAI hardware receive FIFO absorbs short delays,
and overruns appear in the `can_bus` `rx_missed` and `rx_overrun` counters.
The LED risk is lower: the 100-pixel vehicle strip uses SPI with DMA, and only
the single onboard RMT LED is refilled from an interrupt.

## Decision

Candidate: layout C. This is pending the bench comparison below.

| Layout | Bluetooth controller and NimBLE host | TWAI, RMT and SPI interrupts |
| --- | --- | --- |
| A: ESP-IDF default | core 0 | core 0, installed from `app_main` |
| B: #161 recommendation | core 1 (`*_PINNED_TO_CORE_1`) | core 0, installed from `app_main` |
| C: this record | core 0 (ESP-IDF default) | core 1, installed from a core-1 startup task |

- `app_main` applies the board safe defaults on core 0. It then creates a
  short-lived task pinned to core 1 with its own 6,144 B stack.
- That task runs the vehicle I/O startup unchanged and in order:
  1. startup black through `local_argb::start()`;
  2. configuration load and apply;
  3. the turn subscription and `ActionEngine::attach()`;
  4. `VehicleTelemetry::start()`.
- Any failure after `local_argb::start()` still calls `local_argb::fail_off()`
  and refuses to start CAN. No CAN start follows a failed LED start.
- The task reports started or refused to `app_main` through a task
  notification and exits. `app_main` continues to the companion link and
  the polling loop only on started.
- The firmware sets `CONFIG_BTDM_CTRL_PINNED_TO_CORE_0` and
  `CONFIG_BT_NIMBLE_PINNED_TO_CORE_0`. These are the ESP-IDF defaults.
  `esp_timer` stays on core 0 with the Bluetooth tasks.
- Application processing tasks stay unpinned: `argb_guard`, `can_rx`,
  `mazda_notify`, `local_argb` and `vehicle_telemetry`.
- The startup task logs its core.
  `CONFIG_WEACT_DUMP_INTERRUPT_ALLOCATION=y` also prints `esp_intr_dump()`
  so the bench can confirm the allocation.

The bench comparison is pending (#175, measured alongside #167). It must run
with CAN traffic and a connected BLE central, under layouts B and C, and
compare:

- the `can_bus` `rx_missed` and `rx_overrun` counters;
- the LED output.

If layout B is as good as or better than layout C, this record changes to
layout B before it is accepted: the core-1 startup task is dropped and both
Bluetooth pinning options return to core 1.

## Consequences

- Radio work and `esp_timer` stay on core 0, and the vehicle I/O interrupts
  move to core 1. Only interrupt placement changes; task priorities do not.
- The Bluetooth configuration goes back to the ESP-IDF defaults. The `ipc1`
  headroom loss measured under layout B (464 B to 288 B free) should not
  apply, but layout C IPC headroom has not been measured.
- `app_main` no longer runs the driver installs, so its stack use should
  fall. Under layout B it had 388 B of 3,584 B free. The startup task's stack
  is freed after startup.
- The telemetry facade binds its lifecycle owner to the task that first
  subscribes. That is now the startup task, so every facade lifecycle call
  must stay inside it. The owner exits after startup, so a future teardown
  `stop()` would need a long-lived owner. The firmware never stops the
  facade today.
- Created tasks do not inherit the startup task's pinning. ESP-IDF
  `xTaskCreate()` uses `tskNO_AFFINITY`.
- Failure paths gain two cases:
  - If the startup task cannot be created, nothing has started, the board
    safe defaults still hold the outputs low, and `app_main` refuses to
    continue.
  - If no result arrives within 10 s, the task may be blocked inside a
    driver. `app_main` then restarts the device through
    `restart_after_fail_off()` rather than calling the LED drivers
    concurrently: `local_argb::fail_off_for_restart()` only queues black for
    the renderer worker, if the renderer started, and waits a bounded time. The next boot repeats the safe defaults and
    startup black. A hang that persists on every boot would become a restart
    loop with the outputs off.
- The composition validator (`tools/validate_local_argb_boundary.py`)
  checks the startup sequence, the core-1 pinning and the board-defaults
  order.

## Alternatives

- **Layout A: ESP-IDF defaults.** The Bluetooth level-4 interrupts, IPC and
  `esp_timer` all share core 0 with the TWAI interrupt.
- **Layout B: Bluetooth pinned to core 1** (the #161 recommendation, PR
  #173). It is a configuration-only change and keeps startup code
  untouched. It separates the Bluetooth tasks and controller interrupts from
  the vehicle I/O interrupts, but NimBLE callouts still fire in `esp_timer`
  on core 0 at priority 22.

  The #161 idle bench measured layout B with no CAN bus, LED strip or
  central attached:

  | Measure | Free |
  | --- | --- |
  | Internal heap | 155,332 B (tuned NimBLE, advertising) |
  | `btController` stack | 2,056 B |
  | `nimble_host` stack | 1,972 B of 4,096 B |
  | `esp_timer` stack | 3,480 B |
  | `main` stack | 388 B of 3,584 B |
  | `ipc1` stack | 288 B of 1,024 B, down from 464 B with Bluetooth on core 0 |

  These figures carry no CAN receive or LED timing evidence. See
  [the NimBLE resource budget](../development/ble-resource-budget.md).
- **Pinning `can_rx` or moving "car reading" instead of BT.** This would not
  move the TWAI interrupt, whose core is fixed by the caller of
  `twai_driver_install`.
- **`CONFIG_ESP_TIMER_TASK_AFFINITY_CPU1`.** This was considered and not
  measured. It needs the experimental options, and its help text warns that
  it may break other features.

## References

- #175: install the CAN and LED interrupts on core 1.
- #161 and PR #173: the NimBLE resource budget and layout B.
- #163 and PR #178: NimBLE companion service bring-up.
- #167: bench measurement with a connected central.
- [Firmware composition and startup](../architecture/firmware-composition.md)
- [NimBLE resource budget](../development/ble-resource-budget.md#task-and-core-placement)
