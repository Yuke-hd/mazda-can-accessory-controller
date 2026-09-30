# Firmware composition and startup

The WeAct application is the ordinary public-facade consumer. Startup order is:

```text
board::initialize_safe_defaults()
    -> local_argb::start()          # physical black frame and supervised worker
    -> load and apply embedded factory JSON
    -> typed turn subscription, ActionEngine::attach()
    -> VehicleTelemetry::start()    # facade starts strict vehicle CAN
    -> application polling cadence
```

`firmware/weact-can485-v1.1/main/main.cpp` is the only code that knows the
Mazda provider, action engine, LED adapter, and renderer sink. Static storage
builds `mazda::MazdaSignalProvider` over the telemetry facade, an
`action_engine::ActionEngine` over the provider, and a `LedActionSink` over
`local_argb::internal::sink()`.

## Lighting profile and setup

Before vehicle CAN starts, the WeAct composition root obtains the generated
factory JSON through `controller_config::factory_default_config_json()`, parses
it with the canonical persisted loader, and invokes
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
facade and samples polled rules every 100 ms. Any setup failure after
`local_argb::start()` calls `local_argb::fail_off()` and refuses to start CAN.
The application does not stop the facade or detach the engine.
The typed turn callback and the engine's shared turn subscription occupy the
turn channel's two subscriber slots.

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
`local_argb_actions`, `local_argb_sink_contract`, and `local_argb`. The
receive-only CAN invariant is documented in
[`receive-only-boundary.md`](receive-only-boundary.md).

## Validation

Host tests and architecture checks provide software evidence; they do not
establish ESP-IDF scheduling, RMT behavior, ACK behavior, or physical vehicle
acceptance. See the [architecture validation procedure](../development/architecture-validation.md),
[host build guidance](../development/supported-build.md), and
[firmware build guidance](../development/firmware-build.md). Keep host,
firmware, bench, and vehicle validation claims distinct.
