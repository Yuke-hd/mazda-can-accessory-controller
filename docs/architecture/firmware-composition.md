# Firmware composition and startup

The WeAct application is the ordinary public-facade consumer. Startup order is:

```text
board::initialize_safe_defaults()  # app_main, core 0
    -> vehicle I/O startup task      # pinned to core 1; app_main waits for its result
        -> local_argb::start()          # physical black frame and supervised worker
        -> load NVS override or embedded factory JSON, then apply it
        -> typed turn subscription, ActionEngine::attach()
        -> VehicleTelemetry::start()    # facade starts strict vehicle CAN
    -> companion_ble::start()       # optional; asynchronous; never gates lighting
    -> application polling cadence
```

ESP-IDF allocates a driver's interrupt on the core that installs it. The
vehicle I/O steps therefore run on a short-lived task pinned to core 1, so the
TWAI, RMT and SPI interrupts are installed there. Core 0 is left to the
Bluetooth controller, the NimBLE host and `esp_timer`. ADR-0004 (Proposed)
records this layout and the pending bench comparison. The task reports
started or refused through a task notification, logs its core, and exits.
`app_main` continues only on started. If no result arrives within 10 s,
`app_main` restarts the device through `restart_after_fail_off()` rather than
touching the LED drivers concurrently: it only queues black for the renderer
worker, if the renderer started, and never calls a driver itself. If the task cannot be created, nothing has started and the
board safe defaults still hold the outputs low.

`firmware/weact-can485-v1.1/main/main.cpp` is the only code that knows the
Mazda provider, action engine, LED adapter, and renderer sink. Static storage
builds `mazda::MazdaSignalProvider` over the telemetry facade, an
`action_engine::ActionEngine` over the provider, and a `LedActionSink` over
`local_argb::internal::sink()`.

## Lighting profile and setup

Before vehicle CAN starts, the WeAct composition root opens the dedicated NVS
configuration store, loads a persisted override when one is active, and falls
back to the generated factory JSON when the override is absent, unreadable,
malformed or semantically invalid. Both choices go through the same
`controller_config::persisted::parse_controller_config()` loader, and an invalid
override is retained for diagnostics rather than silently accepted or erased.
The selected model is then passed to
`controller_config::persisted::apply_controller_config()`. The persisted
validator checks the profile bindings and rules, including that each LED zone
fits within the logical strip capacity. The generated factory profile's RPM
fill covers the board's full vehicle strip; a host test checks its start and
length against the board capability record. The typed `apply_lighting_profile()`
helper remains a host parity path.
The persisted version 1 profile representation is specified in
[`controller-config.md`](../specs/configuration/controller-config.md#production-lighting-profile).
Profile application and RPM feature contracts are specified in
[`lighting-profile.md`](../specs/configuration/lighting-profile.md).
The adapter and renderer behavior are specified in
[`local-led-actions.md`](../specs/lighting/local-led-actions.md).

After applying the profile, the application registers its typed turn notice
callback, attaches the engine while the facade is stopped, and registers
dispatcher progress through `local_argb::watch_progress()`. It then starts the
facade, and `app_main` samples polled rules every 100 ms. Any setup failure
after `local_argb::start()` calls `local_argb::fail_off()` and refuses to start
CAN. The facade binds its lifecycle owner to the task that first subscribes, so
every lifecycle call (subscribe, attach, start) stays on the startup task.
The application does not stop the facade or detach the engine. Because the
lifecycle owner exits after startup, a future teardown stop would need a
long-lived owner.
The typed turn callback and the engine's shared turn subscription occupy the
turn channel's two subscriber slots.

## Companion BLE

After the facade has started CAN, the composition root builds the companion
Device info inputs from `controller_config::persisted::kSchemaVersion`, the
application description's version and the `weact-can485-v1.1` hardware
identifier, and passes them to `companion_ble::start()`; a static assertion
keeps `kMaxStoredControllerConfigJsonBytes` equal to the protocol's
`companion_protocol::kMaxConfigBytes`. That call encodes the value with
`companion_protocol::encode_device_info()`, creates a low-priority startup
task and returns; NimBLE initialization, GATT registration and advertising run on
that task and on the NimBLE host task, never on `app_main`. A failure at any
stage is logged and leaves the controller without a companion link. It does
not stop CAN, telemetry, LED rendering or fail-off, which are already
running. NVS is already initialized by the configuration store; the companion
component never initializes or erases NVS, and only NimBLE's own bond store
writes to it, in the `nimble_bond` namespace. The composition root also passes
`companion_ble::PairingInputs`: whether bonds can be stored
(`controller_config::persisted::nvs_initialized()`) and the
`board::user_key_pressed()` sampler that opens the pairing window.

The composition root also passes `companion_ble::ConfigInputs`, the three
`companion_protocol` config ports, all implemented by one static
`companion_config::ConfigService`. Right after
`load_boot_configuration()`, `configure_engine_lighting()` records the boot
selection and the NVS config backend in the service. The backend's owner is
the static `config_backend` in `main.cpp`, never released: the vehicle I/O
startup task that creates it exits, and the service keeps using it. The service serializes the active configuration once,
on the companion startup task, into the canonical document that Config status
and read-back both report. A commit parses the upload, dry-runs
`apply_controller_config()` on a static `companion_config::ScratchApplyCheck`
(a never-attached engine and LED sink whose lighting sink discards every
command) and saves it with `ConfigStore::save_override()`; Revert to factory
uses `clear_override()`. Neither changes the live engine or the running
lighting: a saved change takes effect only through the controlled restart
that follows. The composition root owns that restart: `ConfigInputs::restart`
is `restart_after_fail_off()` in `main.cpp`, which calls
`local_argb::fail_off_for_restart()` and then `esp_restart()`. The fail-off
shuts the renderer's publish gate for good, waits for in-flight publishers,
queues black and waits up to `kRestartFailOffWaitUs` (120 ms) for the worker
to write it; the BLE task never calls the LED driver. The restart goes ahead
even when black is not confirmed, and the boot path writes startup black
again. Its protocol is specified in
[`ble-protocol.md`](../specs/companion/ble-protocol.md) and its resource
budget in [`ble-resource-budget.md`](../development/ble-resource-budget.md).

## Application boundary

The application registers a typed `on_turn_state_changed` callback and polls
`speed_kph()` and `engine_rpm()` every 100 ms. It does not call
`can_bus::receive`, a decoder, `update()`, `tick()`, or
`local_argb::submit()`. Acquisition, decoding, freshness servicing,
notification dispatch, and LED publication remain owned by background tasks.
The legacy `bind_local_argb_sink()` telemetry binding is no longer used by the
firmware. Public facade headers do not depend on CAN, decoder, board, lighting,
or RTOS interfaces.

The ordinary application API remains small:

```cpp
#include "mazda/vehicle_telemetry.hpp"

mazda::VehicleTelemetry telemetry{};
auto turn = telemetry.on_turn_state_changed(&on_turn_state_changed, context);
auto speed = telemetry.speed_kph();
auto rpm = telemetry.engine_rpm();
auto started = telemetry.start();
```

These calls consume value-only contracts. The application does not receive
frames or invoke decoder, freshness, dispatcher, publication, or renderer
operations.

The vehicle project selects `vehicle_can_rx`, `mazda_telemetry`,
`vehicle_lighting_policy`, `vehicle_signals`, `action_engine`,
`local_argb_actions`, `local_argb_sink_contract`, `local_argb`,
`companion_protocol`, `companion_config`, and `companion_ble`. The receive-only CAN invariant is documented in
[`receive-only-boundary.md`](receive-only-boundary.md).

## Validation

Host tests and architecture checks provide software evidence; they do not
establish ESP-IDF scheduling, RMT behavior, ACK behavior, or physical vehicle
acceptance. See the [architecture validation procedure](../development/architecture-validation.md),
[host build guidance](../development/supported-build.md), and
[firmware build guidance](../development/firmware-build.md). Keep host,
firmware, bench, and vehicle validation claims distinct.
