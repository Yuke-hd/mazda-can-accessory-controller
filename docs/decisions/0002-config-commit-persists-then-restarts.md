# ADR-0002: A config commit is checked in full, persisted, then restarts

Status: Accepted
Date: 2026-10-03
Retrospective: no

## Context

The companion app uploads a controller configuration over BLE
([ADR-0001](0001-ble-companion-transport.md)). The firmware applies a
configuration once at boot: startup black, load the persisted override or the
factory config, apply it, then start the telemetry facade. Signal
subscriptions can change only while the facade is stopped, and no runtime
re-apply path exists.

`save_override()` runs only parse and `validate()`. Signal keys, operand types,
signal capabilities and runtime capacities are checked later, by the boot-time
apply. When that apply fails, `app_main` calls `fail_off()` and does not start
CAN, and it does not fall back to the factory config. A config that passed
`validate()` but fails the apply would therefore leave every later boot dark
until it is reverted, and before BLE the only repair was a reflash.

## Decision

- A commit runs, in order: the transfer checks, `parse_controller_config()`
  with `validate()`, a **dry-run apply**, and `save_override()`. The first
  failure rejects the commit, and a rejected commit writes nothing to storage.
- The dry run calls `apply_controller_config()` against scratch action-engine
  and LED-sink instances with the same catalog, capacities and types as the
  boot path. It never subscribes to, starts or stops the signal provider, and
  never touches the live engine, renderer or CAN. The firmware composition root
  injects the check into the BLE component.
- A successful commit **persists, then restarts**: it reports `Saved` with
  the stored canonical length and CRC, sends the write response, disconnects
  and performs a controlled restart. The restart takes the normal boot path
  and does not open the pairing window.
- **BLE recovery:** BLE starts even after a lighting setup failure. The LEDs
  stay failed off and CAN acquisition is not started, but Config, Config
  status and Command stay reachable over a bonded link, so the owner can commit
  a corrected config or revert to factory without reflashing.

## Consequences

Easier:

- The new config takes effect through the existing, tested boot path, with
  startup black, factory fallback and a single apply. No live action-engine
  mutation or new runtime path is needed.
- Most configs that would fail at boot are rejected before they are saved,
  with an apply-stage diagnostic the app can show.
- An override that still fails at boot, for example after a firmware update
  changes the catalog, can be repaired over BLE.

Harder:

- Every successful commit costs a restart and a BLE reconnect. The app has to
  verify the result after reconnecting, and it has to handle a lost write
  response as an unknown outcome.
- The dry run needs the Mazda provider, action engine and renderer sink types,
  so the composition root has to inject it, and the BLE host task stack must be
  sized for parse, serialization and apply in the write handler.
- BLE startup must not depend on a successful config apply, which changes the
  firmware composition (issue #163).

## Alternatives

- **Persist and apply live:** stop telemetry, black out, re-apply and restart
  the facade, with no reconnect. Not chosen, because it adds a new runtime
  path that would have to be built and verified (issue #165).
- **Dry run only, without BLE recovery:** an override that fails at boot would
  need a reflash to repair.
- **Boot falls back to factory when an override fails to apply:** this changes
  boot behaviour outside the companion work.

## References

- [#160](https://github.com/Yuke-hd/mazda-can-accessory-controller/issues/160),
  [#163](https://github.com/Yuke-hd/mazda-can-accessory-controller/issues/163),
  [#165](https://github.com/Yuke-hd/mazda-can-accessory-controller/issues/165).
- [Config transfer: Commit, dry-run apply and BLE recovery](../specs/companion/config-transfer.md#commit).
- [Companion BLE protocol: disconnect and restart sequence](../specs/companion/ble-protocol.md#disconnect-and-restart-sequence).
- [Firmware composition](../architecture/firmware-composition.md),
  [controller configuration](../specs/configuration/controller-config.md).
