# ADR-0006: Drive WLED through ESP-NOW remote packets from a bounded action sink

Status: Accepted
Date: 2026-10-12
Retrospective: no

## Context

[#12](https://github.com/Yuke-hd/mazda-can-accessory-controller/issues/12)
asks for WLED as a later output adapter beside the local ARGB strip. Generic
action commands should reach a separate WLED controller in the car. The
transport, network ownership and failure contract had to be chosen before any
implementation.

The choice is constrained by:

- [ADR-0001](0001-ble-companion-transport.md), which keeps Wi-Fi disabled in
  the vehicle firmware and requires a separate ADR for any Wi-Fi use. The
  phone's Wi-Fi is taken by wireless CarPlay, and nobody provisions network
  credentials in the car.
- The fail-off invariant: stale, unavailable or faulted telemetry must turn
  outputs off, and outputs start black.
- The action-engine contract: `ActionSink::execute()` runs under the engine
  lock, on the provider dispatcher or the polling context. A sink owns its own
  bounded queueing and must not block the engine.
- The generic engine and signal headers must stay free of WLED types, which
  `tools/check_architecture.py` and `tools/check_public_headers.py` already
  enforce.
- Stock WLED firmware. The owner does not want to maintain a WLED fork or
  usermod for the first version.

WLED (checked at tag `v16.0.1`, commit
`29b389df1c1aaec6ff53aea742d17063b985906c`) accepts ESP-NOW packets in the
13-byte WiZmote remote format from up to ten trusted sender MAC addresses,
without either side joining a network. It maps each button code to a JSON
state command from its `/remote.json` file. It discards a packet whose 32-bit
sequence number equals the previous one, and keeps only the latest unhandled
button, which its main loop applies. WLED offers no per-command deadline,
so it holds the last applied state. The details are in the
[WLED action sink spec](../specs/lighting/wled-actions.md#wled-behaviour-relied-on).

## Decision

- WLED is driven over **ESP-NOW unicast** in the WiZmote packet format. The
  controller enables the Wi-Fi radio in station mode on one fixed channel, only
  for ESP-NOW. It never associates with an access point, hosts one, holds
  credentials, or sends IP traffic. This narrows ADR-0001's Wi-Fi exclusion to
  this one use. Other Wi-Fi uses, such as OTA, still need their own ADR.
- A new portable adapter, a `WledActionSink`, implements
  `action_engine::ActionSink`. It maps each bound action to one WLED segment
  through a deterministic table of button codes. A tool generates the matching
  `remote.json` from the same controller configuration, so both ends share one
  source.
- The sink keeps only the latest desired state per binding. It sends
  absolute states, never toggles or deltas, so a lost or overwritten packet is
  repaired by the next send. It has fixed capacities, one packet in flight,
  bounded retries, minimum send spacing and a periodic full refresh.
- Fail-off has two layers:
  1. **Controller side.** All-off at link start, at `fail_off()` (provider
     stop, engine detach, dispatcher stall) and before a controlled restart.
     Every binding also follows the engine's `Deactivate`.
  2. **WLED side, stock firmware.** A lease built on WLED's nightlight timer,
     re-armed periodically. If the controller falls silent, WLED sets its
     brightness to 0 within one lease period, about one minute.

  WLED is therefore an accessory output with coarse, best-effort fail-off.
  Lighting that must fail off within the local renderer's bound, such as the
  factory brake and turn effects, stays on the local strip.
- The ESP-IDF radio binding lives in a firmware component behind a mockable
  `EspNowLink` port. Portable code holds no ESP-IDF or Wi-Fi header. The link
  starts after vehicle CAN, like the companion BLE service, and never gates
  CAN, telemetry, local lighting or fail-off.

The receive-only CAN boundary is unchanged. ESP-NOW is a separate 2.4 GHz
radio path, with no CAN transmit, diagnostic polling or acknowledgement.

## Consequences

Easier:

- No network provisioning or ownership: WLED keeps its own access point or
  home network, and the controller only needs WLED's MAC address and channel.
- Stock WLED works. Setup is "enable ESP-NOW, trust the controller MAC, upload
  the generated `remote.json`".
- Unicast ESP-NOW reports MAC-layer delivery, so the controller can retry and
  report link health.
- The sink and its scheduler are host-testable against a fake link and clock.

Harder:

- The Wi-Fi driver, ESP-NOW and Wi-Fi/Bluetooth coexistence add flash, heap
  and core-0 load beside NimBLE. The `factory` partition was 35 % free after
  NimBLE. A measured budget spike gates the implementation and may require a
  partition-layout change.
- WLED-side fail-off is coarse (minutes, not milliseconds) and global to the
  WLED device. A controller that loses power keeps WLED lit until the lease
  expires.
- The sink owns WLED's nightlight timer, so the owner cannot use the nightlight
  feature for other purposes on that WLED device.
- WLED trusts only the sender MAC, and ESP-NOW frames are not encrypted. A
  nearby device can spoof the controller and change the accessory lighting. It
  cannot reach the vehicle CAN bus.
- Levels are quantized to a few brightness steps per binding, because every
  state needs its own button code.
- MAC delivery does not prove that WLED applied the command, because WLED keeps
  only the latest unhandled button. Send spacing and periodic refresh cover
  this. Bench tests have to confirm the spacing.

## Alternatives

- **Wi-Fi with the WLED JSON API over HTTP.** Rejected: the controller would
  join WLED's access point as a client with credentials, HTTP adds latency and
  heap, and WLED holds the last state after link loss in the same way.
- **Wi-Fi with UDP realtime streaming (DNRGB/DDP) and WLED's realtime
  timeout.** It gives the best WLED-side fail-off, about one second.
  Rejected for now: it needs IP networking and network ownership, and the
  controller would render pixels for WLED instead of sending action states.
- **Wired serial JSON (UART or the board's RS485 transceiver).** Rejected: it
  needs extra wiring and a transceiver at the WLED side, and it has no lease
  either.
- **A WLED usermod that implements a sub-second lease.** Deferred: the owner
  prefers stock WLED. It stays an optional follow-up if WLED ever drives
  time-critical lighting.

## References

- [#12](https://github.com/Yuke-hd/mazda-can-accessory-controller/issues/12),
  tracked by [#2](https://github.com/Yuke-hd/mazda-can-accessory-controller/issues/2).
- [WLED action sink spec](../specs/lighting/wled-actions.md),
  [action engine](../specs/action-engine.md),
  [local LED actions](../specs/lighting/local-led-actions.md),
  [firmware composition](../architecture/firmware-composition.md),
  [NimBLE resource budget](../development/ble-resource-budget.md).
- [ADR-0001](0001-ble-companion-transport.md) (Wi-Fi exclusion, narrowed here)
  and [ADR-0004](0004-vehicle-io-interrupts-on-core-1.md) (core placement).
- WLED [ESP-NOW remotes documentation](https://kno.wled.ge/interfaces/espnow/)
  and source at `v16.0.1`: `wled00/remote.cpp`, `wled00/udp.cpp`
  (`espNowReceiveCB`), `wled00/led.cpp` (`handleNightlight`) and
  `wled00/json.cpp` (`nl` state).
