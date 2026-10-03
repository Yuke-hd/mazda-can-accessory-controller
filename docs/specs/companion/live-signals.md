# Companion live signals (layout version 1)

This document specifies the Live signals characteristic of the
[companion BLE protocol](ble-protocol.md): the sampling source, the frame
layout, the availability encoding and the rate cap. The core profile defines
the UUIDs, properties, security, versioning and ATT MTU rules that this
document builds on. Device info reports the layout as
`live_signal_layout_version` `1`.

This is a specification only. No firmware or app implementation exists yet, and
nothing here has been validated on hardware, on a phone, or in a vehicle.

## Invariants

These apply in addition to the
[core safety invariants](ble-protocol.md#safety-invariants).

1. **Availability is copied, never derived.** Each signal's availability code
   is the provider's `Availability` for that reading, unchanged. The encoder
   never promotes a reading, never computes its own freshness and never sends
   `Fresh` for a reading the provider did not report as `Fresh`. A failed
   read, an unsupported signal and an unmappable value are never `Fresh`.
2. **Brake freshness stays unset.** `vehicle.brake_pressed` has no freshness
   timeout, so the provider never reports it as `Fresh`, and the frame never
   does either. The owner-approved `fresh_or_unverified` brake rule opt-in
   affects only lighting rules; it does not change the reported availability.
3. **Live signals are read-only observation.** Sampling uses the provider's
   latest-state `read()`. It never subscribes, starts or stops the provider,
   and never requests data from the vehicle or causes a CAN transmission.
4. **No identifiers or timestamps.** A frame carries semantic values only: no
   CAN identifiers, raw payloads, VIN, location or timestamps. The sequence
   number is a wrapping counter, not a time.

## Sampling

- The controller samples the [catalog signals](#signal-table) through
  `SignalProvider::read()`, which is safe from any context. It resolves each
  signal key to its per-build `SignalId` through the provider catalog once at
  startup. `SignalId` values never appear on the wire.
- It does not use `subscribe()`. Subscriptions can change only while the
  provider is stopped, and the notification subscriber slots are shared with
  the lighting path; the turn channel's two slots are already in use.
- Sampling runs on the BLE side, not on the CAN acquisition, telemetry or
  lighting tasks, and must not delay them.
- All signals in one frame come from one sampling pass. The pass is not an
  atomic snapshot across signals; each value is the latest state of its own
  signal at the moment it was read.

## Notifications

Live signals supports only Notify and requires an encrypted link with an
accepted, stored bond, as the
[core profile's access control](ble-protocol.md#access-control) specifies.

The controller sends frames only while all of these hold:

- the central has enabled notifications in the Client Characteristic
  Configuration descriptor;
- the link is encrypted with an accepted, stored bond;
- the negotiated ATT MTU is at least 64.

Rate rules:

- **Rate cap: 10 Hz.** Consecutive frames on a connection are at least
  100 ms apart.
- The controller samples at most every 100 ms and sends a frame when any field
  other than `sequence` differs from the last frame sent.
- A **heartbeat** frame goes out when nothing has changed for 1 s, so the app
  can tell a quiet vehicle from a stalled link.
- The first frame goes out within 100 ms after notifications are enabled.
- If the BLE stack cannot queue a frame, the controller drops it and sends the
  latest state at the next opportunity. Frames are never queued behind each
  other, so a slow link sees fewer, current frames rather than old ones.
- A config commit runs on the BLE host context, so frames can pause while it
  runs.

The app treats the stream as stalled when no frame has arrived for **2 s**.
While it is stalled, the app shows every signal as unknown, not as its last
received availability. The app must never infer freshness from frame arrival
or timing; only the availability code says whether a value is fresh.

## Frame layout

A frame is 23 bytes, within the 61-byte notification limit at the minimum MTU.

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 1 | `layout_version` (u8): `1` |
| 1 | 1 | `sequence` (u8): increments by 1 for each frame sent on the connection, wrapping from 255 to 0; the first frame is 0 |
| 2 | 1 | `flags` (u8): bit 0 is set when vehicle CAN acquisition is running; other bits are zero |
| 3 | 2 | `engine_rpm` (u16): units of 0.25 rpm |
| 5 | 2 | `speed_kph` (u16): units of 0.01 km/h |
| 7 | 1 | `turn_state` (u8): [choice code](#enum-choice-codes) |
| 8 | 1 | `selector_position` (u8): choice code |
| 9 | 1 | `actual_gear` (u8): choice code |
| 10 | 1 | `front_wiper_position` (u8): choice code |
| 11 | 2 | `booleans` (u16): one bit per Boolean signal, `1` for true |
| 13 | 10 | `status`: one 4-bit [status nibble](#status-nibbles) per signal |

- Multi-byte fields are little-endian, as in the core profile.
- A field whose signal has no value is zero. The status nibble, not the
  field, says whether a value is present; a valid zero speed or RPM is
  distinguished from a missing value by the value-present bit.
- `sequence` lets the app detect dropped frames. It restarts at 0 on each
  connection.
- `flags` bit 0 is clear before vehicle CAN acquisition starts and after a
  lighting setup failure, when CAN is not started (see
  [BLE recovery](config-transfer.md#ble-recovery)). The status nibbles still
  report what each read returned.
- The app ignores bytes after the 23 it knows and unknown `flags` bits, so a
  later layout version can append fields. A change to existing fields needs a
  new `live_signal_layout_version`.

### Number encoding

| Signal | Field | Encoding | Range |
| --- | --- | --- | --- |
| `vehicle.engine_rpm` | `engine_rpm` | `round(rpm × 4)` | The decoder accepts `0..8500` rpm (raw `0..34000`). |
| `vehicle.speed_kph` | `speed_kph` | `round(km/h × 100)` | `0..655.35` km/h, the 16-bit encoding ceiling. |

The units match the decoder's own resolution, so encoding loses no precision.
A non-finite value or a value outside the field's range is not sent: the
field is zero and the value-present bit is clear. The availability code is
unchanged.

### Boolean bits

| Bit | Signal |
| ---: | --- |
| 0 | `vehicle.hazard_request` |
| 1 | `vehicle.turn_request.left` |
| 2 | `vehicle.turn_request.right` |
| 3 | `vehicle.indicator_lamp.left` |
| 4 | `vehicle.indicator_lamp.right` |
| 5 | `vehicle.liftgate_open` |
| 6 | `vehicle.door.rear_right` |
| 7 | `vehicle.door.rear_left` |
| 8 | `vehicle.door.front_left_rhd` |
| 9 | `vehicle.door.front_right_rhd` |
| 10 | `vehicle.doors_unlocked` |
| 11 | `vehicle.wiper.low` |
| 12 | `vehicle.brake_pressed` |

Bits 13–15 are zero.

### Enum choice codes

Choice codes are protocol values, mapped from the catalog choice **keys**. The
raw Mazda enumerator values and their declaration order are firmware details
and never appear on the wire. The firmware maps each choice through an
explicit table, and a catalog choice without a code must fail the build. A raw
value with no catalog choice is not sent: the field is zero and the
value-present bit is clear.

| Code | `turn_state` | `selector_position` | `actual_gear` | `front_wiper_position` |
| ---: | --- | --- | --- | --- |
| 0 | `unknown` | `unknown` | `unknown` | `unknown` |
| 1 | `off` | `shifting` | `park_or_neutral` | `off` |
| 2 | `left` | `park` | `park` | `on` |
| 3 | `right` | `reverse` | `neutral` | `high` |
| 4 | `hazard` | `neutral` | `reverse` | `intermittent` |
| 5 | | `drive` | `first` | |
| 6 | | | `second` | |
| 7 | | | `third` | |
| 8 | | | `fourth` | |
| 9 | | | `fifth` | |
| 10 | | | `sixth` | |
| 11 | | | `shifting` | |

The `unknown` choice is a value the vehicle reported, for example the selector
codes the source tables label `Unknown`. It is different from a missing value,
which the value-present bit reports.

### Status nibbles

Signal `i` in the [signal table](#signal-table) uses byte `13 + i / 2` (integer
division): the low nibble for an even `i` and the high nibble for an odd `i`.
The high nibble of the last byte, for the unused index 19, is zero.

| Bits | Field |
| --- | --- |
| 0–2 | Availability code |
| 3 | Value present: the field carries the signal's value |

| Code | Availability | Meaning |
| ---: | --- | --- |
| 0 | `NoData` | The provider has no observation yet. |
| 1 | `Fresh` | The provider reports a fresh reading within the signal's verified freshness timeout. |
| 2 | `Stale` | The reading is older than its freshness timeout. |
| 3 | `FreshnessUnverified` | The reading is valid, but no freshness timeout is configured for the signal. |
| 4 | `Unavailable` | The provider reports the reading unavailable, for example after a fault or loss of transport health. |
| 5 | Read failed | `read()` returned a request failure, such as `Faulted` or `Timeout`. No value is sent. |
| 6 | Not supported | This build's catalog has no such signal, or it is not Read-capable. No value is sent. |
| 7 | Reserved | Not sent in layout version 1. The app treats it as unknown. |

- Codes 0–4 are `vehicle_signals::Availability` copied through an explicit
  table. Codes 5 and 6 are protocol codes for readings the provider did not
  produce.
- The value-present bit mirrors whether the provider's reading has a value,
  cleared where this document says a value is not sent. A `Stale` or
  `Unavailable` reading can still carry its retained last value; the app may
  show it only as not fresh.
- Only code 1 is fresh. The app shows codes 2–7 as not fresh, and code 3 as
  valid but unverified.

## Signal table

Layout version 1 carries every signal in the controller signal catalog
(`tools/controller_signal_catalog.json`), in this fixed order. Evidence status
and source boundaries are recorded in
[signal evidence](../../protocol/signal-evidence.md); this protocol does not
change them, and a value in a frame is not vehicle validation of its signal.

| Index | Signal key | Type | Field | Default freshness timeout |
| ---: | --- | --- | --- | --- |
| 0 | `vehicle.engine_rpm` | Number | `engine_rpm` | None |
| 1 | `vehicle.speed_kph` | Number | `speed_kph` | None |
| 2 | `vehicle.turn_state` | Enum | `turn_state` | 250 ms |
| 3 | `vehicle.selector_position` | Enum | `selector_position` | None |
| 4 | `vehicle.actual_gear` | Enum | `actual_gear` | None |
| 5 | `vehicle.wiper.front_position` | Enum | `front_wiper_position` | None |
| 6 | `vehicle.hazard_request` | Boolean | Bit 0 | 250 ms |
| 7 | `vehicle.turn_request.left` | Boolean | Bit 1 | 250 ms |
| 8 | `vehicle.turn_request.right` | Boolean | Bit 2 | 250 ms |
| 9 | `vehicle.indicator_lamp.left` | Boolean | Bit 3 | None |
| 10 | `vehicle.indicator_lamp.right` | Boolean | Bit 4 | None |
| 11 | `vehicle.liftgate_open` | Boolean | Bit 5 | None |
| 12 | `vehicle.door.rear_right` | Boolean | Bit 6 | None |
| 13 | `vehicle.door.rear_left` | Boolean | Bit 7 | None |
| 14 | `vehicle.door.front_left_rhd` | Boolean | Bit 8 | None |
| 15 | `vehicle.door.front_right_rhd` | Boolean | Bit 9 | None |
| 16 | `vehicle.doors_unlocked` | Boolean | Bit 10 | None |
| 17 | `vehicle.wiper.low` | Boolean | Bit 11 | None |
| 18 | `vehicle.brake_pressed` | Boolean | Bit 12 | None; brake freshness is unset, so never `Fresh`. |

The timeout column records the default telemetry freshness policy
(`lib/mazda/include/mazda/freshness.hpp`) for orientation only. A signal with
no timeout reports a valid reading as `FreshnessUnverified`, never `Fresh`.
The frame reports whatever availability the provider computes, so this
document defines no timeout, and a later policy change needs no protocol
change.

Adding, removing or reordering a signal, or changing a field's encoding, needs
a new `live_signal_layout_version`. Firmware that drops a signal from its
catalog keeps the slot and reports code 6.

## Tests the implementation needs

The codec and `companion_ble` implementation (issues #162 and #166) must at
least cover:

- every `Availability` value maps to its code, and no input other than a
  `Fresh` reading produces code 1;
- `vehicle.brake_pressed` and `vehicle.engine_rpm` readings from the
  production provider policy never encode as `Fresh`;
- read failures, unsupported signals, non-finite and out-of-range numbers and
  unmapped enum values clear the value-present bit;
- every catalog choice has a code, and the choice-code tables match this
  document;
- the 100 ms rate cap, the 1 s heartbeat and the first-frame timing.

## Out of scope

- Firmware and app implementation, tracked by the companion-app milestone
  issues.
- Freshness timeouts for any signal, including brake.
- A per-signal subscription mask or a configurable rate.
- Raw CAN, diagnostic or logging streams.
