# Local ARGB bring-up notes

> Historical implementation and bring-up context. Current renderer behavior
> is specified in [renderer runtime](../specs/lighting/renderer-runtime.md),
> current LED action semantics in
> [local LED actions](../specs/lighting/local-led-actions.md), and firmware
> lifecycle in [firmware composition](../architecture/firmware-composition.md).
> Use those documents and the current source when making changes.

## Retired telemetry-owned behavior

The earlier `mazda::application::bind_local_argb_sink()` path consumed copied
turn and brake values, availability, a validity deadline, and transport-owned
fail-off state. It animated left and right turns in amber, rendered brake as
solid red in the center region, and requested black for off, unknown, stale,
CAN-offline, or decoder-error state. That producer published state changes
and a bounded 100 ms heartbeat; the renderer also cleared a command after its
freshness deadline. The previous implementation notes used a 250 ms boundary
and described RMT output for both the vehicle strip and onboard status pixel.
The vehicle-strip driver has since changed; the current output and supervision
configuration is specified in the renderer runtime document.

Brake freshness was intentionally unset. The old binding lit brake only for
a Fresh brake reading, so decoded brake data remained dark without reviewed
timing evidence. Brake was not part of the generic signal catalog used by the
new action path. The production profile instead uses the brake effect for its
RPM red-zone action; the renderer effect name remains a generic output choice.

The generic action engine and held-level LED sink replaced the telemetry-owned
heartbeat and deadline policy. Current rule availability drives `Deactivate`
for missing, stale, or unavailable data; the renderer's watched-dispatcher
fail-off covers a stalled notification path. Held levels are not periodically
re-applied, so after a driver fault clears the renderer stays dark until a new
command. These current rules are documented in the linked specifications.

## Migration and validation record

During the migration, the legacy binding remained compiled in
`mazda_telemetry` as a rollout fallback, while firmware moved to the generic
action engine, `LedActionSink`, and the renderer's one-item queue. The plan at
the time was to remove the fallback after rollout. Check current firmware and
component ownership before using that plan as status.

The transition added host coverage for old semantic updates and freshness
heartbeats in `components/mazda_telemetry/tests/vehicle_telemetry_service_tests.cpp`,
and for the new held-command adapter in
`tests/host/local_led_action_composition_tests.cpp`. A source validator was
also added for the firmware's LED boundary. At that point it required the
profile to be applied once from `app_main`, kept direct binding and rule
registration out of firmware composition, attached the engine before starting
the telemetry facade, and called `local_argb::fail_off()` on setup-failure
returns. It also prohibited `FreshOrUnverified` in the application source and
checked call placement after switch labels. Regression cases covered missing
profile application, duplicated values, direct registration, commented-out and
string decoys, braceless and nested returns, switch labels, and helper-wrapped
bypasses.

These are historical notes about the migration; the validator source and
current architecture documentation are authoritative for today's checks.

## Bring-up follow-up recorded with the original implementation

The original implementation record stated that no physical bench or vehicle
test had been performed. It proposed running concurrent classic-CAN load and
repeated LED changes on a WeAct V1.1 board, recording receive and drop counts,
queue high-water marks, driver overrun/error counts, LED transitions, startup
and warm-reset clear behavior, and native USB logs. It required keeping K3 OFF, adding
no vehicle termination, and publishing no raw payloads or private vehicle
data. Recheck the present board and driver setup before treating those steps
as a current procedure.

The original record also called for checking the `mazda_notify` task's stack
high-water mark with debug logging enabled. It recorded an API/configuration
review on 2026-09-04 against the [Espressif `led_strip` v3.0.3 registry
release](https://components.espressif.com/components/espressif/led_strip/versions/3.0.3).
The dependency remains pinned at that version, but the original RMT description
does not describe the current vehicle-strip SPI backend.
