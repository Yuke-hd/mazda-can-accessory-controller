# Companion BLE protocol (protocol version 1)

This document is the authoritative core profile for the Bluetooth Low Energy
(BLE) link between the controller and the iOS companion app. It defines the
safety invariants, roles, advertising, pairing and bonding, the GATT service
and characteristic table, the device info and command payloads, versioning and
ATT MTU assumptions.

The config transfer protocol and the live-signal frame are specified in their
own companion documents:

- [`config-transfer.md`](config-transfer.md): the Config and Config status
  payloads, chunked transfer, commit, read-back, size limits and failure
  behaviour.
- [`live-signals.md`](live-signals.md): the Live signals frame layout,
  availability encoding and rate cap.

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
- The controller bounds how long a link that is not yet encrypted with an
  accepted, stored bond can hold the only connection slot. It disconnects such
  a link, and advertising resumes, at whichever deadline comes first:
  - **30 s** after the latest of the connection, the first protected-attribute
    rejection on the link, and the most recent Security Manager Protocol (SMP)
    PDU **received** as part of a pairing the controller accepted inside the
    pairing window. The controller's own SMP PDUs (for example a Security
    Request or Pairing Failed) and PDUs of a rejected pairing do not restart
    this period.
  - **90 s** after the connection, regardless of SMP activity. Nothing extends
    this absolute cap.

  Restarting the period on accepted pairing PDUs keeps a pairing in progress
  alive while the user answers the iOS pairing alert, and restarting it on the
  first protected-attribute rejection covers the alert that iOS may show before
  it sends its first SMP PDU. The SMP procedure's own 30 s timeout bounds each
  step of the pairing. Whether these values suit the iOS pairing flow must be
  checked in a bench trace under issue #164.
- When a connected central's identity resolves to a stored bond, the
  controller sends a Security Request, so a bonded central re-encrypts
  promptly instead of waiting until it touches a protected attribute.
- All multi-byte integers are little-endian. Strings are UTF-8 without a
  terminator and are prefixed by a one-byte length.
- The controller exposes the Generic Attribute service (`0x1801`) with the
  Service Changed characteristic. A firmware update that changes the attribute
  table must indicate Service Changed to each bonded client on its next
  connection, because iOS caches the attribute table of bonded peripherals.

### Startup and NVS dependency

The BLE stack starts only after the board safe defaults and startup black
(`local_argb::start()`) are complete, and only after NVS initialization has
been attempted. The config component currently owns the global
`nvs_flash_init()` call; BLE must reuse that initialization through a shared
NVS owner and must never initialize, erase or repair the partition itself (see
[boot-time override storage](../configuration/controller-config.md#boot-time-override-storage)).

If NVS initialization failed during this boot:

- the controller can store no bonds, so it rejects every new pairing, and no
  link can have the accepted, stored bond that protected access requires;
- the pairing window does not open, and device info `flags` bit 0 stays `0`,
  so the app does not ask the user to pair when pairing cannot succeed;
- consequently no protected characteristic or command is reachable during that
  boot. Device info remains readable.

The shared NVS owner must therefore report the result of NVS initialization
separately from the result of opening any one namespace.

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

Secure Connections only is enforced on the pairing itself: the controller
rejects a pairing that does not use LE Secure Connections, or deletes its keys
as described under [enforcement fallback](#enforcement-fallback). It must not
be enforced by a host setting that also demands authenticated keys for
attribute access. For example, NimBLE's `sm_sc_only` option makes the stock
permission check require an authenticated link, which no Just Works bond can
satisfy.

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

- The window opens for **120 s** only after a debounced press of the user key
  (GPIO0; see the
  [hardware record](../../architecture/hardware/weact-can485-v1.1.md)). A press
  while the window is open restarts the 120 s period.
- No reset opens the window. This includes a power-on reset
  (`ESP_RST_POWERON`), an EN-pin reset from the RST button or a USB serial
  auto-reset circuit, the controlled restarts this protocol triggers (revert to
  factory and config commit), a software restart, a panic, a watchdog reset and
  a brownout reset. Power-up and remotely triggered restarts or crashes
  therefore never open the window (see
  [power-up exposure](#power-up-exposure)).
- The window closes when the period expires or after one new bond is stored.
- Outside the window, the controller rejects **every** pairing request. Where
  the host supports it, the rejection uses SMP reason `Pairing Not Supported`
  (`0x05`). This includes a repeated pairing from a central it already has a
  bond for: the controller keeps the existing bond and rejects or ignores the
  new pairing. On NimBLE that case raises `BLE_GAP_EVENT_REPEAT_PAIRING`, and
  returning `BLE_GAP_REPEAT_PAIRING_IGNORE` drops the request without an SMP
  response. The controller then disconnects the link (see
  [enforcement fallback](#enforcement-fallback)), so the central sees a
  disconnect rather than its own SMP timeout.
- A pairing attempted outside the window, whether rejected, ignored or failed,
  never modifies, evicts or deletes an existing bond.
- A bonded central can re-encrypt with its stored long-term key at any time.
  Re-encryption is not pairing and needs no window.
- A central that already has a bond but lost its keys (for example after
  "Forget This Device" in iOS) must pair again inside a window. Inside the
  window, a repeated pairing deletes the old bond for that identity **before**
  key exchange, and the new bond replaces it. If the new pairing then fails or
  times out, the old bond stays deleted and the central must pair again while
  the window is still open. On NimBLE this is the
  `BLE_GAP_EVENT_REPEAT_PAIRING` handler deleting the old bond and returning
  `BLE_GAP_REPEAT_PAIRING_RETRY`.

#### Enforcement fallback

Rejecting a pairing request before key exchange depends on the BLE host
offering a hook for it. Where it does not, the controller enforces the same
outcome after pairing completes. If pairing completed outside the window,
without LE Secure Connections, without bonding, or without the bond being
stored (for example because NVS is full), the controller deletes any keys from
that pairing and disconnects. The SMP reason above is the preferred behaviour;
the app must not depend on receiving it.

Because the fallback may write keys to NVS and then delete them, the
controller disconnects a link after its first rejected pairing, so one
connection cannot drive repeated NVS writes. A nearby device that reconnects
and retries still causes writes, and only connection setup and pairing time
limit how often that can happen. The NimBLE binding goes further: an
outside-window pairing never writes to NVS (see
[implementation notes](#nimble-implementation-notes)).

The fallback alone does not protect existing bonds, because a host may make
room for a new bond before key exchange. NimBLE, for example, checks bond
capacity when the pairing request arrives and raises a store-full event, and
its stock handler deletes the oldest bond. The controller's store-full and
overflow handling must therefore delete a bond only while the pairing window
is open. Outside the window it must fail the event, which makes the host
reject the pairing, so the stored bonds stay untouched.

A link is never treated as bonded for access control until its bond is stored
and the pairing was accepted. A host's stock encryption permission check grants
access as soon as a link is encrypted, so ATT requests queued just after
encryption could be served before a rejected link is terminated. The access
handlers of protected attributes must therefore check the link's accepted bond
themselves, not only the stock encryption flag. When the link is encrypted but
has no accepted, stored bond, the access fails with ATT error
`Insufficient Authentication` (`0x05`) and the controller disconnects the link
immediately.

GPIO0 is also an ESP32 boot strapping pin. Holding the key during reset enters
the ROM serial bootloader instead of the application, so the firmware sees only
presses that start after boot.

#### Power-up exposure

The window deliberately does not open at power-up. This mitigates the main
risk of Just Works pairing. Many vehicles switch accessory power with the
ignition, so a window that opened at power-up would open at every ignition
cycle, usually without the owner intending to pair. An active attacker in
radio range could then pair, or sit between the owner's phone and the
controller, at each ignition cycle. Requiring a key press limits every window
to a deliberate action by someone with access to the board.

The cost is that every new pairing needs the user key: the first pairing, a
phone that lost its keys, and re-pairing after Clear bonds. An installation
must therefore keep the user key reachable, or the board must be accessed to
pair. An active attacker in radio range during a window the owner opened can
still pair; that remains an accepted exposure of protocol version 1.

### Bond storage

- Bonds are stored in NVS by the BLE host's own store, in a namespace separate
  from the config store's `mazda_config` namespace. Bond storage must never read,
  write or erase `mazda_config`, and clearing bonds must not erase the NVS
  partition. Bonds share the 64 KiB NVS partition with the config
  store, so the bond capacity must be budgeted against it (see
  [boot-time override storage](../configuration/controller-config.md#boot-time-override-storage)).
- The controller stores up to **4 bonds**. When a new pairing inside the window
  would exceed the capacity, the least recently stored bond is deleted. Bonds
  are evicted only inside the window (see
  [enforcement fallback](#enforcement-fallback)), so physical presence is
  needed to displace an existing bond.
- The Clear bonds command deletes every stored bond.

### App pairing flow

The app pairs through the operating system's pairing prompt; it never handles
keys itself.

1. Connect and read Device info. It is readable without pairing. Check the
   protocol version, then read `flags` bit 0 (pairing window open).
2. If the app has no bond with this controller and bit 0 is `0`, ask the user
   to press the controller's user key, then read Device info again (or
   reconnect) until bit 0 is `1`. The window stays open for 120 s, and another
   press restarts it.
3. Read a protected attribute, for example Config status. The controller
   answers `Insufficient Authentication` (`0x05`), and iOS shows its pairing
   alert. Use a read, not a subscription, as the trigger: NimBLE rejects an
   unencrypted CCCD write before the controller's access check runs, so that
   rejection does not restart the 30 s period. When the user accepts, Just Works LE Secure
   Connections pairing runs and the bond is stored. The window then closes.
4. Retry the protected access. It succeeds once the link is encrypted with the
   accepted bond.
5. On later connections a bonded central re-encrypts by itself: the controller
   sends a Security Request, or answers `Insufficient Encryption` (`0x0F`) to an
   unencrypted protected access. No window is needed.

The app must allow for:

- a disconnect instead of an SMP `Pairing Failed` when it pairs outside the
  window, or when its link stays unencrypted for the time limits in
  [roles and connections](#roles-and-connections);
- `Insufficient Authentication` (`0x05`) followed by a disconnect when the link
  is encrypted but its bond was not accepted, for example a stale bond after
  Clear bonds;
- a re-encryption failure after Clear bonds or a lost bond, which means the user
  must remove the controller in iOS Settings and pair again inside a window.

### NimBLE implementation notes

The firmware binds this section to NimBLE in `components/companion_ble`
(issue #164). The decisions themselves (the window, the user-key debouncing,
the unbonded-link limits and the access verdicts) are portable, host-tested
code in `lib/companion_protocol`. Where NimBLE offers no hook for the preferred
behaviour, the binding chooses the more conservative outcome:

- **Pairing rejection.** Outside the window NimBLE's `sm_sec_lvl` is set to
  `1` (no security), which makes NimBLE reject a Pairing Request with SMP reason `Command Not Supported` (`0x07`) before any key
  exchange, rather than the preferred `Pairing Not Supported` (`0x05`). This
  rejection raises no GAP event, so the link is not disconnected at once. The
  unbonded-link time limit, or the first protected access, closes it.
- **Repeat pairing outside the window.** The same rejection fires before
  NimBLE's repeat-pairing check, so a bonded central that lost its keys gets
  `0x07`, and its existing bond is untouched. The `BLE_GAP_EVENT_REPEAT_PAIRING`
  handler still returns `IGNORE` and disconnects if it is ever reached outside
  the window.
- **No NVS writes outside the window.** The bond store's write callback refuses
  pairing records (both security records and the peer address record) unless
  the window is open. It checks the window's deadline itself, so a pairing
  accepted in the moment between the deadline and the window timer stores no
  keys and is then rejected. ESP-IDF forces NimBLE's host-based privacy, so
  NimBLE writes a peer device record for a peer using a resolvable address
  straight to NVS, outside the store callbacks. When the guard refused that
  pairing's peer address record, the binding deletes the peer device record
  with `ble_store_util_delete_peer()` once the link reports encryption, or at
  disconnect. No resolving-list entry is created, because NimBLE adds one only
  after the peer security record is stored. NimBLE re-initializes its store callbacks at
  every host sync (its default-IRK setup calls `ble_store_config_init()`), so
  the binding re-installs this guard at each sync and checks it on every GAP
  event. If no store callback exists to guard, the window stays closed. The
  store-full handler
  refuses to evict a bond outside it (the pairing then fails with
  `Unspecified Reason`, `0x08`). An outside-window pairing therefore never
  writes, evicts or deletes a bond.
- **Unbonded-link timing.** NimBLE reports no received SMP PDUs, so the 30 s
  period restarts on the connection, on the first protected-attribute
  rejection, on a repeat pairing accepted inside the window and on pairing
  completion. A store-full event inside the window does not restart it. The
  90 s cap is unchanged. An unencrypted CCCD write is rejected by NimBLE's
  stock permission check before the access check, so it does not restart the
  period either. Whether 30 s after the first rejection suits the iOS pairing
  alert still needs a bench trace.
- **Accepted bond.** A link has an accepted bond only when it is encrypted with
  a 16-byte key and its peer has a stored bond. A bond counts as new only when
  this link wrote its keys during the window, and only then does the window
  close. Any other encryption result disconnects the link, and keys written by
  a rejected pairing are deleted.
- **CCCD writes.** NimBLE checks CCCD writes itself, so notify characteristics
  use NimBLE's encrypted and authorized CCCD permissions. An unencrypted CCCD
  write gets `0x05` or `0x0F` as above. An encrypted CCCD write without an
  accepted bond is refused through the authorization hook, which NimBLE
  reports as `Insufficient Authorization` (`0x08`) rather than `0x05`, and the
  link is then disconnected.
- **Notification gate.** A protected notifier sends only to a link for which
  `link_has_accepted_bond()` is true. NimBLE's `sec_state.encrypted` and
  `sec_state.bonded` flags are not enough, because a rejected pairing is
  encrypted and bonded until its disconnect completes. The Live signals
  notifier (#166) must use this check when it is integrated with this
  binding.
- **Clear bonds.** The operation deletes each bonded peer's security, CCCD and
  address records and keeps the controller's own identity resolving key, so
  the controller's identity address does not change. Entries in the
  controller's address-resolution list stay until the next reboot. The
  operation does not disconnect by itself; the Command characteristic (#165)
  sends its write response and then disconnects. Protocol version 1 has no
  physical trigger for Clear bonds; without the app, bonds can be removed only
  by erasing flash with a serial tool.
- **User key.** The board samples GPIO0 every 20 ms from a host-task timer. It
  treats a low level as pressed and needs three equal samples to change state.
  A key held from boot does not count until it is released. The active-low
  reading is taken from the hardware record and still needs a bench check.

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
| Config | Read, Write | Encrypted, bonded | [Config transfer](config-transfer.md#config-characteristic) |
| Config status | Read, Notify | Encrypted, bonded | [Config transfer](config-transfer.md#config-status-characteristic) |
| Live signals | Notify | Encrypted, bonded | [`live-signals.md`](live-signals.md) |
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
  (CCCD) write requires an **encrypted link with an accepted, stored bond**
  (see [enforcement fallback](#enforcement-fallback)). Encryption is required
  even for reads and notifications, because live signals include door lock
  state, speed and gear.
- The controller requires encryption but **not an authenticated (MITM) key**,
  because Just Works keys are unauthenticated. Requiring authentication would
  reject every bond this profile creates.
- On an unencrypted link, a protected access fails with ATT error
  `Insufficient Authentication` (`0x05`) when the controller has no bond for
  the central, and `Insufficient Encryption` (`0x0F`) when it has one, so the
  central re-encrypts with its stored key. The app must handle both. iOS starts
  pairing or re-encryption when it receives one of these errors; outside the
  pairing window a new pairing is rejected and the controller then disconnects
  (see [enforcement fallback](#enforcement-fallback)).

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

Codes `0x86`–`0x8B` are defined in
[config transfer](config-transfer.md#application-error-codes). Codes
`0x8C`–`0x9F` are reserved for later protocol versions and the companion
documents. The app treats an unknown application error code as a
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
| 9 + n | 1 + m | `hardware_id` (length-prefixed string) | `weact-can485-v1.1`, at most 31 bytes. |

Both strings are at most 31 bytes. For `firmware_version` this matches the
32-byte, NUL-terminated `esp_app_desc_t` version field; `hardware_id` is a
protocol constant with the same cap. The version 1 value is therefore at most
72 bytes.

- `protocol_major` and `protocol_minor` stay at offsets 0 and 1 in every
  protocol version.
- `config_schema_version` is the persisted config schema version the firmware
  accepts; see [versioning](../configuration/controller-config.md#versioning).
- `max_config_bytes` is the largest config the controller accepts over BLE.
  [Config transfer](config-transfer.md#size-limits) specifies how it
  applies. Version 1 firmware reports the 4 KiB canonical-JSON storage limit.
- `firmware_version` is the ESP-IDF application version string. It is
  informational; the app must not parse it for compatibility.
- `hardware_id` identifies the board record. It is informational.
- `flags` bit 0 is readable before pairing, so any central in range can learn
  whether the window is open. This is accepted: the window relies on the
  owner's physical presence, not on secrecy. The advertisement still omits it.

A later minor version may append fields and define more flag bits. The app
ignores bytes after the fields it knows and ignores unknown flag bits.

## Command

The Command characteristic accepts a two-byte write:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 1 | `opcode` (u8) |
| 1 | 1 | `check` (u8): `opcode XOR 0xFF` |

The check byte guards against an accidental single-byte write. The controller
checks a write in this order and returns the first failure:

1. Any length other than 2 fails with `Invalid Attribute Value Length`.
2. A wrong check byte fails with `InvalidPdu`.
3. An unknown opcode fails with `UnsupportedOperation`.
4. While a config transfer, commit or restart is in progress, the command
   fails with `Busy`.

A successful write response means the controller accepted and performed the
command's storage change, and will perform the follow-up action described
below.

| Opcode | Name | Behaviour |
| --- | --- | --- |
| `0x01` | Revert to factory | Clears the persisted config override, then restarts. |
| `0x02` | Clear bonds | Deletes every stored bond, then disconnects. |

### Disconnect and restart sequence

A GATT server cannot observe that the central received a write response. When
a command ends in a disconnect or restart, the controller sends the write
response, then terminates the connection, waiting at most **1 s** for the
disconnection to complete, and then performs the follow-up action. The app
treats a disconnect without a write response as an **unknown outcome**: it
reconnects and re-reads state instead of assuming success or failure.

The write response is sent only after the access handler returns, so the
terminate must be issued after the handler, not from inside it. The link layer
does not guarantee that the response reaches the central before the
disconnection; the unknown-outcome rule covers that case.

### Revert to factory

1. The controller calls `clear_override()`. It succeeds even when no override
   is active.
2. On failure the write fails with `StorageFailure`. The controller does not
   restart, and the active config is unchanged. The clear operation attempts to
   restore the previous active marker, but the persisted selection after a
   failed clear is not guaranteed.
3. On success the controller sends the write response, disconnects and
   performs a controlled restart, as described in the
   [disconnect and restart sequence](#disconnect-and-restart-sequence). The
   restart follows the normal boot path: startup black, then the embedded
   factory config. It does not open the pairing window.

Clearing deactivates the override but does not erase the stored slots; see
[boot-time override storage](../configuration/controller-config.md#boot-time-override-storage).

### Clear bonds

1. The controller deletes every stored bond, including the bond of the
   connected central.
2. On failure the write fails with `StorageFailure`, and some bonds may remain.
3. On success the controller sends the write response and then disconnects,
   as described in the
   [disconnect and restart sequence](#disconnect-and-restart-sequence).
   The app must also remove the controller from its own records. On iOS the
   user removes the pairing in Settings, because an app cannot delete a system
   bond.
4. After an unknown-outcome disconnect, the app reconnects. If re-encryption
   fails because the peer no longer has the bond (iOS reports that the peer
   removed its pairing information), the clear succeeded and the app directs
   the user to Settings. If re-encryption succeeds, the clear did not happen.

Clearing bonds does not open the pairing window. To pair again, the user
presses the user key.

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

A later minor version may append a lowest accepted schema version to device
info if the firmware gains schema migrations. Version 1 accepts exactly one
schema version.

The firmware enforces its side independently: it rejects unknown opcodes and
malformed PDUs, and `validate()` rejects a config whose `version` is not
`kSchemaVersion`. The app's checks do not replace firmware validation.

## ATT MTU

- The controller's preferred ATT MTU is **247**. iOS normally negotiates a
  larger MTU than the default of 23 after connecting.
- Device info, the Command characteristic, Config read-back and Config status
  reads work at the default MTU of 23.
- Config uploads and notifications need a negotiated ATT MTU of at least
  **64**. Below that, the controller rejects a config transfer with
  `MtuTooSmall` and sends no Config status or Live signals notifications.
- A notification never exceeds `MTU - 3` bytes. The protocol never splits one
  PDU across notifications.

## Out of scope

- Firmware and app implementation, tracked by the companion-app milestone
  issues.
- Wi-Fi, OTA updates and CarPlay.
- Brightness and preset config schema fields.
- Resolvable private addresses for the controller (address privacy).
- Authenticated (MITM-protected) pairing, such as passkey entry.
