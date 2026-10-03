# ADR-0003: Gate BLE pairing with Just Works and a physical-presence window

Status: Accepted
Date: 2026-10-03
Retrospective: no

## Context

The companion link ([ADR-0001](0001-ble-companion-transport.md)) carries live
vehicle state, including door lock state, speed and gear, and can replace the
persisted lighting configuration. Every characteristic except device info
therefore requires an encrypted link with an accepted, stored bond.

The WeAct CAN485 V1.1 board has no display and no keypad, only a user key on
GPIO0. Without input or output, pairing can only use the LE Secure Connections
**Just Works** model. Just Works keys are unauthenticated in BLE terms and give
no man-in-the-middle protection, so "authenticated writes" in the BLE sense
cannot be met.

The iOS app must also reconnect to a bonded controller in the background,
which needs the controller to keep advertising.

## Decision

- The controller supports LE Secure Connections only, declares
  `NoInputNoOutput` and requires bonding. Access control requires encryption
  with an accepted, stored bond, not an authenticated key.
- The controller **always advertises** while no central is connected, so
  CoreBluetooth can reconnect bonded phones in the background.
- **New pairings are accepted only inside a 120 s pairing window.** The window
  opens after a power-on reset and after a debounced press of the user key
  (GPIO0). On the ESP32 an EN-pin reset, from the RST button or a USB serial
  auto-reset circuit, also counts as a power-on reset and opens it; both need
  access to the board or its USB port. Controlled restarts, software resets,
  panics, watchdog and brownout resets do not open it. It closes when the
  period expires or after one new bond is stored.
- Outside the window every pairing request is rejected, and a rejected or
  failed pairing never modifies, evicts or deletes an existing bond. Bonded
  centrals re-encrypt at any time without a window.

The detailed rules, including the unbonded-link time limits, enforcement
fallback and bond capacity, are in the
[companion BLE protocol](../specs/companion/ble-protocol.md#pairing-and-bonding).

## Consequences

Easier:

- No passkey or other secret has to be provisioned or managed.
- Physical presence, by power-cycling the controller or pressing the key,
  stands in for MITM protection.
- Background reconnection works for bonded phones while driving.

Harder:

- An active attacker in radio range during an open window can pair. Because
  accessory power usually follows the ignition, the power-up window typically
  opens at every ignition cycle. This is an accepted exposure of protocol
  version 1.
- The controller is always discoverable, and protocol version 1 uses no
  resolvable private address, so its advertisement can be tracked. Address
  privacy is deferred.
- Enforcing the window and protecting existing bonds needs custom host
  handling (repeat-pairing and store-full events, access handlers that check
  the accepted bond), implemented in issue #164.

## Alternatives

- **Static passkey:** a fixed 6-digit passkey, for example set at build time,
  gives MITM-flagged keys, but a static passkey is weak and is a secret to
  manage.
- **Defer the IO capability to #164:** leaves the security model unspecified
  in the protocol.
- **Advertise only during the window:** smaller exposure, but it breaks
  automatic background reconnection while driving.
- **Always advertise with open pairing:** anyone nearby could bond.

## References

- [#160](https://github.com/Yuke-hd/mazda-can-accessory-controller/issues/160),
  [#164](https://github.com/Yuke-hd/mazda-can-accessory-controller/issues/164).
- Companion BLE protocol:
  [advertising](../specs/companion/ble-protocol.md#advertising),
  [pairing and bonding](../specs/companion/ble-protocol.md#pairing-and-bonding),
  [access control](../specs/companion/ble-protocol.md#access-control).
- [WeAct CAN485 V1.1 hardware record](../architecture/hardware/weact-can485-v1.1.md).
