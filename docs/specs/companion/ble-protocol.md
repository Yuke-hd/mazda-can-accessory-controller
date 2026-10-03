# Companion BLE protocol (protocol version 1)

This document is the authoritative core profile for the Bluetooth Low Energy
(BLE) link between the controller and the iOS companion app. It defines the
safety invariants, roles, advertising, pairing and bonding, the GATT service
and characteristic table, the device info and command payloads, versioning and
ATT MTU assumptions.

The config transfer protocol and the live-signal frame are specified in their
own companion documents, which are added under the same issue:

- `config-transfer.md`: the Config and Config status payloads, chunked transfer,
  commit, read-back, size limits and failure behaviour.
- `live-signals.md`: the Live signals frame layout, availability encoding and
  rate cap.

This is a specification only. No firmware or app implementation exists yet, and
nothing here has been validated on hardware, on a phone, or in a vehicle.

## Safety invariants

These invariants bind every implementation of this protocol and every later
protocol version.

1. **CAN stays receive-only.** The vehicle firmware keeps
   `TWAI_MODE_LISTEN_ONLY`, a zero-length transmit queue, and no application
   transmit, diagnostic polling or vehicle ACK behaviour. BLE adds no CAN
   transmit path: no characteristic, command, config field or telemetry
   subscription can cause a CAN transmission or request data from the vehicle.
   See [receive-only boundary](../../architecture/receive-only-boundary.md).
2. **A rejected or partial config never replaces the active or persisted
   config.** A config reaches storage only through the validated
   `save_override()` path, and it becomes active only through the normal boot
   path. An aborted, timed-out, oversized, malformed, invalid or interrupted
   transfer leaves both the active and the persisted config unchanged.
3. **Telemetry is never reported fresher than the provider reports it.** Each
   live-signal value carries the provider's `Availability` unchanged. Stale,
   unknown, missing or unavailable telemetry is never encoded as `Fresh`.
4. **Brake freshness stays unset.** This protocol defines no brake timeout and
   never reports `vehicle.brake_pressed` as `Fresh`. The app must not infer a
   freshness window for brake from notification timing.
5. **BLE has no direct LED control.** No characteristic writes pixels, effects,
   brightness or action state. Lighting changes only through a validated,
   persisted config applied at boot. Startup black, driver-failure fail-off and
   the existing watchdogs are unchanged.
6. **BLE failure is isolated.** A BLE stack initialization failure, advertising
   failure, connection loss or client misbehaviour must not block or delay
   startup black, config selection, CAN acquisition or lighting. Lighting never
   depends on whether a phone is connected.
7. **No vehicle identifiers or raw CAN data.** The protocol carries only
   catalog-level semantic values. It never carries CAN identifiers, raw frame
   payloads, the VIN, location or absolute trip timestamps.

## Roles and connections

The controller is a BLE peripheral and GATT server. The companion app is the
central and GATT client.

- The controller accepts **one connection at a time**. While a central is
  connected, the controller does not advertise.
- All multi-byte integers are little-endian. Strings are UTF-8 without a
  terminator and are prefixed by a one-byte length.
- The controller exposes the Generic Attribute service (`0x1801`) with the
  Service Changed characteristic. A firmware update that changes the attribute
  table must indicate Service Changed to each bonded client on its next
  connection, because iOS caches the attribute table of bonded peripherals.

## Advertising

The controller advertises connectable, undirected advertisements whenever no
central is connected, including outside the pairing window. Continuous
advertising lets a bonded iPhone reconnect in the background through a pending
CoreBluetooth connection.

| Field | Location | Content |
| --- | --- | --- |
| Flags | Advertising data | LE General Discoverable, BR/EDR not supported. |
| Complete list of 128-bit service UUIDs | Advertising data | The companion service UUID. |
| Complete local name | Scan response | `Mazda CAN Controller`. |

The app discovers controllers by the companion service UUID and must not rely
on the local name. Advertising intervals are an implementation choice and are
not part of the protocol.

The advertisement carries no vehicle data and no pairing-window state. Protocol
version 1 does not require a resolvable private address, so a nearby observer
can track the controller's advertisement. Address privacy is deferred.

## Pairing and bonding

### Security level

The controller supports **LE Secure Connections only** and rejects legacy
pairing. It declares the IO capability `NoInputNoOutput`, so every pairing uses
the **Just Works** association model, and it requires the bonding flag.

Just Works keys are **unauthenticated** in BLE terms. A link encrypted with
them reaches Security Mode 1 Level 2 (encryption with unauthenticated pairing),
not Level 4. LE Secure Connections protects the key exchange against passive
eavesdropping, but Just Works gives **no man-in-the-middle (MITM) protection**.
An active attacker in radio range during an open pairing window can pair. The
pairing window and the owner's physical presence are the only mitigation.

During pairing the controller requests the central's identity resolving key
(IRK) and identity address, so it can recognise an iPhone that uses resolvable
private addresses, and it distributes its own long-term key and identity
information.

### Pairing window

The controller accepts a **new** pairing only while the pairing window is open.

- The window opens for **120 s** when the BLE stack becomes ready after
  power-up, and for 120 s after a debounced press of the user key (GPIO0; see
  the [hardware record](../../architecture/hardware/weact-can485-v1.1.md)). A
  press while the window is open restarts the 120 s period.
- The window closes when the period expires or after one new bond is stored.
- Outside the window, the controller rejects a pairing request from a central
  it has no bond for with SMP reason `Pairing Not Supported` (`0x05`).
- A bonded central can re-encrypt with its stored keys at any time.
- A central that already has a bond but lost its keys (for example after
  "Forget This Device" in iOS) must pair again inside a window. The new bond
  replaces the old bond for that identity.

GPIO0 is also an ESP32 boot strapping pin. Holding the key during reset enters
the ROM serial bootloader instead of the application, so the firmware sees only
presses that start after boot.

Because many vehicles switch accessory power with the ignition, the power-up
window typically opens at every ignition cycle. This is an accepted exposure of
protocol version 1.

### Bond storage

- Bonds are stored in NVS by the BLE host's own store, in a namespace separate
  from the config store's `mazda_config` namespace. Bond storage must never read,
  write or erase `mazda_config`, and clearing bonds must not erase the NVS
  partition. Bonds share the default 24 KiB NVS partition with the config
  store, so the bond capacity must be budgeted against it (see
  [boot-time override storage](../configuration/controller-config.md#boot-time-override-storage)).
- The controller stores up to **4 bonds**. When a new pairing inside the window
  would exceed the capacity, the least recently stored bond is deleted. New
  pairings happen only inside the window, so physical presence is needed to
  displace an existing bond.
- The Clear bonds command deletes every stored bond.

## GATT profile

### UUIDs

The companion service and its characteristics share a random 128-bit base
UUID. Only the 16-bit field shown as `xxxx` differs:

```text
ab49xxxx-09b6-4509-bf8e-2790253baf98
```

| Attribute | `xxxx` | UUID |
| --- | --- | --- |
| Companion service (primary) | `0000` | `ab490000-09b6-4509-bf8e-2790253baf98` |
| Device info | `0001` | `ab490001-09b6-4509-bf8e-2790253baf98` |
| Config | `0002` | `ab490002-09b6-4509-bf8e-2790253baf98` |
| Config status | `0003` | `ab490003-09b6-4509-bf8e-2790253baf98` |
| Live signals | `0004` | `ab490004-09b6-4509-bf8e-2790253baf98` |
| Command | `0005` | `ab490005-09b6-4509-bf8e-2790253baf98` |

These UUIDs are fixed for every protocol version, so an app can always find the
device info characteristic and read the protocol version. Values `0006`–`ffff`
are reserved for later characteristics.

### Characteristics

| Characteristic | Properties | Security | Payload |
| --- | --- | --- | --- |
| Device info | Read | None (readable before pairing) | [Device info](#device-info) |
| Config | Read, Write | Encrypted, bonded | `config-transfer.md` |
| Config status | Read, Notify | Encrypted, bonded | `config-transfer.md` |
| Live signals | Notify | Encrypted, bonded | `live-signals.md` |
| Command | Write | Encrypted, bonded | [Command](#command) |

"Write" means ATT Write Request (write with response). No characteristic
supports Write Without Response, so every write is acknowledged and flow
controlled. No characteristic uses indications; the Service Changed indication
belongs to the Generic Attribute service.

### Access control

- **Device info** is readable on an unencrypted link so the app can check
  compatibility before it pairs. It contains no vehicle data and no config
  state.
- Every other read, write and Client Characteristic Configuration Descriptor
  (CCCD) write requires an **encrypted link**. The controller requires the
  bonding flag for every pairing, so an encrypted link always uses a stored
  bond. Encryption is required even for reads and notifications, because live
  signals include door lock state, speed and gear.
- The controller requires encryption but **not an authenticated (MITM) key**,
  because Just Works keys are unauthenticated. Requiring authentication would
  reject every bond this profile creates.
- On an unencrypted link, a protected access fails with ATT error
  `Insufficient Encryption` (`0x0F`) or `Insufficient Authentication` (`0x05`).
  iOS starts pairing when it receives one of these errors; outside the pairing
  window that pairing is rejected and the access keeps failing.

### Application error codes

Protected writes report failures with ATT error responses. Standard ATT errors
keep their usual meaning, including `Invalid Attribute Value Length` (`0x0D`)
for a payload of the wrong length. Protocol-specific failures use the ATT
application error range:

| Code | Name | Meaning |
| --- | --- | --- |
| `0x80` | `InvalidPdu` | The payload has the right length but a malformed field or check byte. |
| `0x81` | `UnsupportedOperation` | The opcode is not defined in this protocol version. |
| `0x82` | `Busy` | Another operation is in progress, such as a config transfer, a commit or a pending restart. |
| `0x83` | `InvalidState` | The operation is not valid in the current state. |
| `0x84` | `StorageFailure` | Persistent storage could not be updated. The operation's section states the resulting state. |
| `0x85` | `MtuTooSmall` | The negotiated ATT MTU is below the minimum the operation needs. |

Codes `0x86`–`0x9F` are reserved for later protocol versions and the
companion documents. The app treats an unknown application error code as a
generic failure of that operation.

## Device info

Device info is a read-only binary value. A read returns the whole value; it is
longer than a default-MTU read, so the client reads it with ATT Read Blob,
which CoreBluetooth does automatically.

| Offset | Size | Field | Version 1 value |
| ---: | ---: | --- | --- |
| 0 | 1 | `protocol_major` (u8) | `1` |
| 1 | 1 | `protocol_minor` (u8) | `0` |
| 2 | 2 | `config_schema_version` (u16) | `1` (`kSchemaVersion`) |
| 4 | 1 | `live_signal_layout_version` (u8) | `1` |
| 5 | 1 | `flags` (u8) | Bit 0: pairing window open. Other bits are zero. |
| 6 | 2 | `max_config_bytes` (u16) | `4096` |
| 8 | 1 + n | `firmware_version` (length-prefixed string) | Application version, at most 31 bytes. |
| 9 + n | 1 + m | `hardware_id` (length-prefixed string) | `weact-can485-v1.1`, at most 32 bytes. |

- `protocol_major` and `protocol_minor` stay at offsets 0 and 1 in every
  protocol version.
- `config_schema_version` is the persisted config schema version the firmware
  accepts; see [versioning](../configuration/controller-config.md#versioning).
- `max_config_bytes` is the largest config the controller accepts over BLE.
  `config-transfer.md` specifies how it applies. Version 1 firmware reports the
  4 KiB canonical-JSON storage limit.
- `firmware_version` is the ESP-IDF application version string. It is
  informational; the app must not parse it for compatibility.
- `hardware_id` identifies the board record. It is informational.

A later minor version may append fields and define more flag bits. The app
ignores bytes after the fields it knows and ignores unknown flag bits.

## Command

The Command characteristic accepts a two-byte write:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 1 | `opcode` (u8) |
| 1 | 1 | `check` (u8): `opcode XOR 0xFF` |

The check byte guards against an accidental single-byte write. A write of any
other length fails with `Invalid Attribute Value Length`. A wrong check byte
fails with `InvalidPdu`, and an unknown opcode fails with
`UnsupportedOperation`. While a config transfer, commit or restart is in
progress, every command fails with `Busy`.

A successful write response means the controller accepted the command and will
perform the action described below.

| Opcode | Name | Behaviour |
| --- | --- | --- |
| `0x01` | Revert to factory | Clears the persisted config override, then restarts. |
| `0x02` | Clear bonds | Deletes every stored bond, then disconnects. |

### Revert to factory

1. The controller calls `clear_override()`. It succeeds even when no override
   is active.
2. On failure the write fails with `StorageFailure`. The controller does not
   restart, and the active config is unchanged. The clear operation attempts to
   restore the previous active marker, but the persisted selection after a
   failed clear is not guaranteed.
3. On success the controller sends the write response, then performs a
   controlled restart once the response has been sent. The restart follows the
   normal boot path: startup black, then the embedded factory config.

Clearing deactivates the override but does not erase the stored slots; see
[boot-time override storage](../configuration/controller-config.md#boot-time-override-storage).

### Clear bonds

1. The controller deletes every stored bond, including the bond of the
   connected central.
2. On failure the write fails with `StorageFailure`, and some bonds may remain.
3. On success the controller sends the write response and then disconnects.
   The app must also remove the controller from its own records. On iOS the
   user removes the pairing in Settings, because an app cannot delete a system
   bond.

Clearing bonds does not open the pairing window. To pair again, the user
presses the user key or power-cycles the controller.

## Versioning and compatibility

The protocol uses three independent versions, all reported in device info:

| Version | Covers | Changes when |
| --- | --- | --- |
| Protocol (`major.minor`) | UUIDs, characteristic properties, security, PDUs, error codes, device info and command layouts. | Major: any incompatible change. Minor: backward-compatible additions. |
| Config schema | The JSON document carried by the Config characteristic. | The persisted schema changes; see the configuration spec. |
| Live-signal layout | The Live signals frame. | The frame layout or signal set changes. |

A minor protocol version may add fields at the end of a value, flag bits,
opcodes, error codes and characteristics. It may not remove, reorder or
reinterpret anything defined by an earlier minor version.

The app must enforce this compatibility rule:

1. Read device info before pairing and before any other access.
2. If the app does not support `protocol_major`, it must not pair, write a
   config, send a command or decode live signals. It reports that the app or
   the firmware needs an update.
3. If `protocol_minor` is newer than the app knows, the app proceeds and
   ignores unknown fields, flag bits and error codes.
4. The app writes only config documents whose `version` equals
   `config_schema_version`. It may show a read-back config of an unknown schema
   version only as opaque JSON.
5. The app decodes live signals only for a `live_signal_layout_version` it
   supports. Otherwise it does not subscribe.

The firmware enforces its side independently: it rejects unknown opcodes and
malformed PDUs, and `validate()` rejects a config whose `version` is not
`kSchemaVersion`. The app's checks do not replace firmware validation.

## ATT MTU

- The controller's preferred ATT MTU is **247**. iOS normally negotiates a
  larger MTU than the default of 23 after connecting.
- Device info and the Command characteristic work at the default MTU of 23.
- Config transfer and live signals need a negotiated ATT MTU of at least
  **64**. Below that, the controller rejects a config transfer with
  `MtuTooSmall` and sends no live-signal notifications.
- A notification never exceeds `MTU - 3` bytes. The protocol never splits one
  PDU across notifications, except where `config-transfer.md` defines chunking.

## Out of scope

- Firmware and app implementation, tracked by the companion-app milestone
  issues.
- Wi-Fi, OTA updates and CarPlay.
- Brightness and preset config schema fields.
- Resolvable private addresses for the controller (address privacy).
- Authenticated (MITM-protected) pairing, such as passkey entry.
