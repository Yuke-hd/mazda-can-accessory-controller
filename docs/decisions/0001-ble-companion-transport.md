# ADR-0001: Use BLE as the companion app transport

Status: Accepted
Date: 2026-10-03
Retrospective: no

## Context

An iOS companion app
([Yuke-hd/esp-can-companion](https://github.com/Yuke-hd/esp-can-companion)) will
read and update the persisted controller configuration. Later it will show
live telemetry on an in-app Drive dashboard. The vehicle firmware currently
has no wireless transport. Configuration changes require reflashing or
host-side tooling.

The transport has to fit these conditions:

- The phone is typically connected to the head unit through wireless CarPlay,
  which uses the phone's Wi-Fi.
- The app should reconnect to the controller without user action, including
  while backgrounded, whenever the phone and the vehicle are in range.
- The payloads are small: a JSON schema v1 controller configuration, and a
  compact telemetry frame at roughly 10–20 Hz.
- The firmware must keep the receive-only CAN boundary, startup black, and
  fail-off behaviour unchanged. The radio stack must not take timing priority
  over CAN acquisition or LED rendering.
- The app is distributed as development builds to the owner's own device.

## Decision

- The companion transport is Bluetooth Low Energy. The controller is a GATT
  peripheral using the ESP-IDF NimBLE host stack.
- The GATT service, payload formats, and versioning are specified separately
  ([#160](https://github.com/Yuke-hd/mazda-can-accessory-controller/issues/160)).
- Writable characteristics require LE Secure Connections pairing and an
  encrypted link with a stored bond. Just Works keys are unauthenticated in
  BLE terms; [ADR-0003](0003-ble-pairing-window.md) records how pairing is
  gated instead.
- Wi-Fi is not enabled in the vehicle firmware. Any future Wi-Fi use, such as
  OTA firmware updates, requires a separate ADR. That use is expected to be
  OTA-only and off by default.

This decision does not change the CAN boundary. BLE adds no CAN transmit,
diagnostic polling, or acknowledgement path. The companion service only reads
telemetry, and it is a config writer only through the existing parse,
`validate()`, and persistence path.

## Consequences

Easier:

- The phone's Wi-Fi remains available for wireless CarPlay and internet
  access. The phone does not have to join a controller-hosted network.
- CoreBluetooth provides background reconnection and state restoration for a
  remembered peripheral. The app can therefore reconnect whenever it comes
  into range.
- The firmware adds one radio stack and leaves Wi-Fi disabled. This is
  expected to cost less memory and flash than Wi-Fi plus an HTTP or WebSocket
  server. The actual budget is measured in
  [#161](https://github.com/Yuke-hd/mazda-can-accessory-controller/issues/161).
- Pairing and bonding give link-level authentication without credential
  provisioning, such as Wi-Fi passwords or a hotspot setup.

Harder:

- Throughput is low. Config upload needs a chunked transfer protocol with
  explicit commit, abort, and timeout handling. Telemetry must be compact and
  rate-capped.
- BLE needs a custom GATT protocol and codecs on both sides, instead of
  generic HTTP tooling. Without the app, testing relies on generic BLE
  clients such as nRF Connect.

## Alternatives

- **Wi-Fi with a controller-hosted network and an HTTP or WebSocket server.**
  Not chosen: the phone would have to join the controller's network, which
  competes with wireless CarPlay for the phone's Wi-Fi, needs credential
  provisioning, and is expected to cost more memory and flash.

## References

- [#160](https://github.com/Yuke-hd/mazda-can-accessory-controller/issues/160)
  companion BLE protocol and GATT profile, and
  [#161](https://github.com/Yuke-hd/mazda-can-accessory-controller/issues/161)
  NimBLE spike.
- [Companion BLE protocol](../specs/companion/ble-protocol.md),
  [config transfer](../specs/companion/config-transfer.md),
  [live signals](../specs/companion/live-signals.md).
- [Receive-only boundary](../architecture/receive-only-boundary.md).
