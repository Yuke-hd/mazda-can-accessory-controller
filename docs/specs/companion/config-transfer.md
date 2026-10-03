# Companion config transfer (protocol version 1)

This document specifies the Config and Config status characteristics of the
[companion BLE protocol](ble-protocol.md): chunked config upload, commit,
read-back, size limits, error reporting and failure behaviour. The core
profile defines the UUIDs, properties, security, error-code conventions,
versioning and ATT MTU rules that this document builds on.

This is a specification only. No firmware or app implementation exists yet, and
nothing here has been validated on hardware, on a phone, or in a vehicle.

## Invariants

These apply in addition to the
[core safety invariants](ble-protocol.md#safety-invariants).

1. **Only a validated, persisted config becomes active, and only at boot.**
   Config is still loaded and applied once during startup, as the
   [configuration spec](../configuration/controller-config.md#boot-time-override-storage)
   requires. A commit persists a candidate through `save_override()` and then
   restarts; it never mutates the running action engine, LED adapter or
   renderer. There is no hot reload.
2. **A rejected or partial config never replaces the active or persisted
   config.** Received bytes stay in a RAM buffer until a commit passes every
   check below. An aborted, timed-out, interrupted, incomplete, oversized,
   corrupted, malformed, invalid or unappliable transfer is discarded without
   reaching storage.
3. **A commit check has no runtime side effects.** The dry-run apply described
   below uses scratch objects. It never subscribes to, starts or stops the
   signal provider, and never touches the live engine, LED adapter, renderer
   or CAN.
4. **Config transfer never blocks vehicle work.** Transfer and commit run on
   the BLE host context. They must not run on, or wait for, the CAN
   acquisition, telemetry or lighting tasks.

## Document encoding

- The transferred document is version 1 controller-config JSON, encoded as
  UTF-8, as specified in
  [canonical JSON loading](../configuration/controller-config.md#canonical-json-loading).
  YAML is an authoring format only; the app compiles or generates JSON.
- The app sends the document without a NUL terminator. The controller passes
  the received bytes to `parse_controller_config()` unchanged.
- The document's `version` must equal device info `config_schema_version`; see
  [versioning](ble-protocol.md#versioning-and-compatibility).
- The controller stores and reads back the **canonical** serialization
  produced by `serialize_controller_config()`, not the bytes the app sent. Key
  order, whitespace, number formatting and explicit defaults can therefore
  differ from the upload.

## Size limits

| Limit | Value | Source |
| --- | --- | --- |
| Upload size (`total_length`) | `1..max_config_bytes` | Device info `max_config_bytes`, `4096` in version 1. |
| Canonical size of a saved override | At most 4 KiB | `kMaxStoredControllerConfigJsonBytes`. |
| Read-back size | At most 16 KiB | `kMaxControllerConfigJsonBytes`; see below. |

Version 1 reports the 4 KiB canonical storage limit as `max_config_bytes`.
Canonicalization can expand a document, so an upload within
`max_config_bytes` can still be rejected at commit with an `InputTooLarge`
diagnostic.

Only the factory config can produce a read-back larger than
`max_config_bytes`, because the embedded factory document is not bound by the
override limit. The version 1 factory profile compiles to about 1.8 KiB. A
read-back that exceeds `max_config_bytes` cannot be written back unchanged.

The receive buffer is statically allocated at `max_config_bytes`. The
controller holds at most one transfer at a time, across all connections.

## Config characteristic

Config supports Read and Write (with response) and requires an encrypted link
with an accepted, stored bond, as the
[core profile's access control](ble-protocol.md#access-control) specifies.

### Write PDUs

Every write starts with a one-byte opcode.

| Opcode | Name | Length | Layout |
| --- | --- | ---: | --- |
| `0x01` | Start | 7 | `opcode`, `total_length` (u16), `crc32` (u32) |
| `0x02` | Chunk | 4..`MTU - 3` | `opcode`, `offset` (u16), `data` (at least 1 byte) |
| `0x03` | Commit | 1 | `opcode` |
| `0x04` | Abort | 1 | `opcode` |
| `0x05` | Select read page | 3 | `opcode`, `offset` (u16) |

A chunk carries at most `MTU - 6` data bytes: the ATT Write Request header
takes 3 bytes and the chunk header 3 more. At the minimum MTU of 64 that is
58 bytes, and at the preferred MTU of 247 it is 241 bytes.

The controller checks every write in this order and returns the first
failure:

1. An empty write fails with `Invalid Attribute Value Length` (`0x0D`).
2. An unknown opcode fails with `UnsupportedOperation`.
3. A length outside the opcode's range fails with `Invalid Attribute Value
   Length`.
4. While a restart is pending, every write fails with `Busy`.
5. The opcode's own checks follow, in the order listed in its section.

A write that fails changes no transfer state, except where a section says the
transfer is discarded.

### Checksum

`crc32` is CRC-32/ISO-HDLC, the CRC used by zlib and PNG: polynomial
`0x04C11DB7`, reflected input and output, initial value `0xFFFFFFFF` and final
XOR `0xFFFFFFFF`. Its check value over the ASCII bytes `123456789` is
`0xCBF43926`. The Start PDU's CRC covers exactly the `total_length` document
bytes the app sends.

The CRC detects an app that sent the wrong bytes or reassembled them in the
wrong order. It is not an authenticity check; link encryption and bonding
provide that.

### Transfer states

| State | Meaning |
| --- | --- |
| Idle | No transfer is held. |
| Receiving | A transfer is open and its buffer holds `received_length` bytes. |
| RestartPending | A commit succeeded. The controller is disconnecting and restarting. |

The controller discards an open transfer, and returns to Idle, when:

- no Start or accepted Chunk arrives for **10 s** (result `TimedOut`);
- the connection ends (result `Interrupted`);
- the app aborts it (result `Aborted`);
- a commit fails for any reason other than `Incomplete`.

The idle timeout restarts on Start and on each accepted chunk. Rejected chunks
do not restart it. A 4 KiB upload at the minimum MTU takes 71 chunks, which
completes well within this timeout at normal connection intervals.

### Start

Start opens a transfer. After the common checks:

1. A negotiated ATT MTU below 64 fails with `MtuTooSmall`.
2. `total_length` of 0 fails with `InvalidPdu`.
3. `total_length` above `max_config_bytes` fails with `TooLarge`.
4. An open transfer fails with `Busy`. The app aborts it first.
5. If the controller has no config store during this boot, the write fails
   with `StorageFailure`. Config status `boot_flags` reports why.

On success the state becomes Receiving, `received_length` is 0, and the
result is cleared to `None`.

### Chunk

Chunks must arrive in order and without gaps. After the common checks:

1. With no open transfer, the write fails with `InvalidState`.
2. An `offset` other than the current `received_length` fails with
   `OffsetMismatch`. This includes a repeated chunk.
3. A chunk that would extend past `total_length` fails with `InvalidPdu`.

On success the controller appends `data` to the buffer. A rejected chunk
leaves the buffer and `received_length` unchanged, so the app can read
`received_length` from Config status and resume from there.

### Commit

Commit runs every check inside the write handler and answers only when it has
finished. The steps run in this order; the first failure ends the commit.

| Step | Failure | ATT error | Result | Transfer |
| --- | --- | --- | --- | --- |
| 1. A transfer is open. | No open transfer. | `InvalidState` | Unchanged | — |
| 2. All bytes have arrived. | `received_length < total_length`. | `Incomplete` | Unchanged | Kept |
| 3. The CRC matches. | CRC mismatch. | `ChecksumMismatch` | `ChecksumMismatch` | Discarded |
| 4. `parse_controller_config()` accepts the document. | Parse, structural or `validate()` error. | `ConfigRejected` | `ConfigRejected` | Discarded |
| 5. The dry-run apply succeeds. | Any `ApplyStatus` other than `Complete`. | `ApplyRejected` | `ApplyRejected` | Discarded |
| 6. `save_override()` returns `Saved`. | `InvalidCandidate`. | `ConfigRejected` | `ConfigRejected` | Discarded |
| | `Failed`. | `StorageFailure` | `StorageFailed` | Discarded |

A rejected commit leaves the active config unchanged and does not restart. A
storage failure follows the
[override storage contract](../configuration/controller-config.md#boot-time-override-storage):
a failed backend write does not replace the active override.

When every step succeeds, the controller:

1. records result `Saved` with the stored canonical length and CRC, sets the
   state to RestartPending and notifies Config status;
2. sends the write response;
3. disconnects and performs a controlled restart, as the
   [disconnect and restart sequence](ble-protocol.md#disconnect-and-restart-sequence)
   specifies. The restart follows the normal boot path: startup black, then
   the persisted override. It does not open the pairing window.

A commit, including an NVS write and its verification, must finish well
within the 30 s ATT transaction timeout. The app must allow for a slow write
response.

#### Dry-run apply

`validate()` cannot check signal keys, operand types, signal capabilities or
runtime capacities. A failure of those checks at boot would leave the LEDs
failed off with no factory fallback. The dry run moves those checks before the
save.

The controller calls `apply_controller_config()` with the parsed model, a
scratch `action_engine::ActionEngine` over the live signal provider, and a
scratch `LedActionSink`. Both scratch objects use the same catalog, capacities
and types as the boot path. The scratch engine only reads the provider's
catalog during setup. It is never attached, so no rule subscribes, samples or
fires, and the scratch sink never publishes.

The dry run does not cover checks that happen only at attach, such as the
provider's subscriber slots. An override that passes the dry run can therefore
still fail at boot; [BLE recovery](#ble-recovery) covers that case.

### Abort

Abort discards an open transfer and sets result `Aborted`. With no open
transfer it succeeds and changes nothing, so the app may send it before every
upload to clear a transfer left from an earlier attempt.

### Read-back

A Config read returns one page of the canonical serialization of the active
config:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 1 | `source` (u8): `0` factory, `1` persisted override, `0xFF` none |
| 1 | 2 | `total_length` (u16) of the whole document |
| 3 | 4 | `crc32` (u32) of the whole document |
| 7 | 2 | `page_offset` (u16) |
| 9 | n | `data`: `min(total_length - page_offset, 200)` bytes |

A page is at most 209 bytes. Paging is needed because ATT limits an attribute
value to 512 bytes. The client reads each page with ATT Read and Read Blob,
which CoreBluetooth does automatically. Read Blob offsets apply within the
page value, not within the document.

- The active config is the model selected at boot, serialized with
  `serialize_controller_config()`. It stays the same until the next restart,
  so pages read in any order belong to one document. A commit does not change
  it.
- `source` is `0xFF`, and `total_length` and `crc32` are 0, when the
  controller selected no config during this boot.
- **Select read page** sets the page offset for later reads on this
  connection. An `offset` above `total_length` fails with `InvalidPdu`. An
  `offset` equal to `total_length` returns a page with no data. The page
  offset is 0 at the start of each connection.
- Read-back works in every transfer state and at the default MTU.

Serializing a stored canonical document again must reproduce it byte for
byte, so after a restart the active `crc32` equals the `saved_crc32` reported
by the commit. The implementation must test this round trip.

## Config status characteristic

Config status supports Read and Notify and requires an encrypted link with an
accepted, stored bond.

The value is at most 61 bytes. A read returns the whole value, using Read Blob
at the default MTU. The controller notifies only on links with a negotiated ATT
MTU of at least 64, and it notifies when `state` or `result` changes. It does
not notify per chunk; the write response acknowledges each chunk.

Fields that do not apply to the current result are zero.

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 1 | `state` (u8): `0` Idle, `1` Receiving, `2` RestartPending |
| 1 | 1 | `result` (u8): see [results](#results) |
| 2 | 1 | `boot_flags` (u8): see [boot flags](#boot-flags) |
| 3 | 1 | `active_source` (u8): as in read-back |
| 4 | 2 | `active_length` (u16): read-back `total_length` |
| 6 | 4 | `active_crc32` (u32): read-back `crc32` |
| 10 | 2 | `transfer_length` (u16): Start `total_length` while Receiving |
| 12 | 2 | `received_length` (u16) while Receiving |
| 14 | 2 | `saved_length` (u16): canonical length for `Saved` |
| 16 | 4 | `saved_crc32` (u32): canonical CRC for `Saved` |
| 20 | 1 | `diag_category` (u8): for `ConfigRejected` |
| 21 | 1 | `diag_code` (u8): for `ConfigRejected` |
| 22 | 1 | `diag_validation` (u8): for `ConfigRejected` |
| 23 | 2 | `diag_index` (u16): for `ConfigRejected` |
| 25 | 1 | `apply_stage` (u8): for `ApplyRejected` |
| 26 | 1 | `apply_validation` (u8): for `ApplyRejected` |
| 27 | 1 | `apply_section` (u8): for `ApplyRejected` |
| 28 | 2 | `apply_index` (u16): for `ApplyRejected` |
| 30 | 1 | `apply_binding` (u8): for `ApplyRejected` |
| 31 | 1 | `apply_engine` (u8): for `ApplyRejected` |
| 32 | 1 | `boot_diag_code` (u8): for boot flag 0 |
| 33 | 1 | `boot_diag_validation` (u8): for boot flag 0 |
| 34 | 1 | `path_info` (u8): bits 0–6 path length, bit 7 truncated |
| 35 | n | `path`: the `ConfigRejected` diagnostic path, at most 26 bytes |

- The diagnostic fields come from the `ConfigDiagnostic` of the parse or of
  `save_override()`. The apply fields come from `ApplyStatus`. The boot fields
  come from `BootConfigurationResult::override_diagnostic`.
- Indexes saturate at `0xFFFF`.
- `path` uses the loader's path format, for example `outputs[3].zone.length`
  or `$`. A longer path keeps its first bytes, cut at a UTF-8 character
  boundary, and sets bit 7 of `path_info`.
- The free-text diagnostic `message` is not sent. The app derives its message
  from the codes, the index and the path.
- The result persists for the rest of the boot, across connections, until the
  next Start or a restart. After a restart, `result` is `None`.

### Results

| Code | Result | Set when |
| ---: | --- | --- |
| 0 | `None` | No transfer has finished since boot or since the last Start. |
| 1 | `Saved` | A commit succeeded. |
| 2 | `ConfigRejected` | Parse, validation or the canonical size check failed. |
| 3 | `ApplyRejected` | The dry-run apply failed. |
| 4 | `StorageFailed` | `save_override()` returned `Failed`. |
| 5 | `Aborted` | The app aborted an open transfer. |
| 6 | `TimedOut` | The idle timeout discarded the transfer. |
| 7 | `ChecksumMismatch` | The CRC did not match at commit. |
| 8 | `Interrupted` | The connection ended during a transfer. |

### Boot flags

| Bit | Meaning |
| ---: | --- |
| 0 | A persisted override was present but invalid, and the factory config was selected. `boot_diag_*` give the reason. |
| 1 | Reading the persisted override failed, and the factory config was selected. |
| 2 | No config store is available during this boot, so Start fails with `StorageFailure`. |
| 3 | Lighting setup failed. The LEDs are failed off and CAN acquisition was not started. |

Bits 4–7 are zero in version 1.

### Wire codes

Wire codes are stable protocol values. The firmware maps each enumerator
through an explicit table rather than casting its declaration order, and a
new enumerator without a wire code must fail the build. The app shows a code it
does not know as `unknown (n)`. A later protocol minor version may add codes
but never renumbers them.

`diag_category`, `ConfigErrorCategory`:

| Code | Name |
| ---: | --- |
| 0 | None |
| 1 | `Parse` |
| 2 | `Structural` |
| 3 | `Semantic` |

`diag_code` and `boot_diag_code`, `ConfigErrorCode`:

| Code | Name | Code | Name |
| ---: | --- | ---: | --- |
| 0 | None | 6 | `MissingField` |
| 1 | `MalformedJson` | 7 | `UnknownField` |
| 2 | `EmbeddedNul` | 8 | `TypeMismatch` |
| 3 | `InputTooLarge` | 9 | `InvalidValue` |
| 4 | `NestingLimitExceeded` | 10 | `SchemaValidation` |
| 5 | `RootTypeMismatch` | 11 | `ResourceExhausted` |

`diag_validation`, `apply_validation` and `boot_diag_validation`,
`ValidationError` (see [validation](../configuration/controller-config.md#validation)):

| Code | Name | Code | Name |
| ---: | --- | ---: | --- |
| 0 | `None` | 12 | `UnsupportedComparison` |
| 1 | `UnsupportedVersion` | 13 | `InvalidHysteresis` |
| 2 | `EmptyActionName` | 14 | `InvalidRange` |
| 3 | `DuplicateActionName` | 15 | `UnknownLedEffect` |
| 4 | `UndeclaredAction` | 16 | `UnknownFillDirection` |
| 5 | `DuplicateAction` | 17 | `EmptyZone` |
| 6 | `EmptySignalKey` | 18 | `ZoneOutOfRange` |
| 7 | `UnknownComparison` | 19 | `InvalidColor` |
| 8 | `UnknownFreshness` | 20 | `InvalidPriority` |
| 9 | `UnknownEventEdge` | 21 | `DuplicateBinding` |
| 10 | `EmptyChoice` | 22 | `InvalidDuration` |
| 11 | `InvalidOperand` | 23 | `IncompatibleActionKind` |

`apply_section`, `ConfigSection`: `0` Document, `1` Actions, `2` Rules,
`3` Outputs.

`apply_stage`, `ApplyStage`: `0` Complete (no failure), `1` Validation,
`2` ActionId, `3` OutputBinding, `4` SinkRegistration, `5` Rule.

`apply_binding`, `local_argb_actions::BindingStatus`: `0` Ok,
`1` InvalidAction, `2` DuplicateBinding, `3` CapacityExceeded,
`4` InvalidEffect.

`apply_engine`, `action_engine::ConfigStatus`:

| Code | Name | Code | Name |
| ---: | --- | ---: | --- |
| 0 | `Ok` | 7 | `UnsupportedCapability` |
| 1 | `InvalidState` | 8 | `TypeMismatch` |
| 2 | `CapacityExceeded` | 9 | `UnknownChoice` |
| 3 | `DuplicateSink` | 10 | `InvalidOperand` |
| 4 | `InvalidAction` | 11 | `UnsupportedComparison` |
| 5 | `DuplicateAction` | 12 | `InvalidRange` |
| 6 | `UnknownSignal` | 13 | `InvalidHysteresis` |

## Application error codes

This document defines these codes from the range the core profile reserves:

| Code | Name | Meaning |
| --- | --- | --- |
| `0x86` | `TooLarge` | Start `total_length` exceeds `max_config_bytes`. |
| `0x87` | `OffsetMismatch` | A chunk's `offset` is not the current `received_length`. |
| `0x88` | `Incomplete` | Commit arrived before all bytes. The transfer is kept. |
| `0x89` | `ChecksumMismatch` | The received bytes do not match the Start CRC. |
| `0x8A` | `ConfigRejected` | The loader or `save_override()` rejected the document. Config status has the diagnostic. |
| `0x8B` | `ApplyRejected` | The dry-run apply failed. Config status has the stage and status. |

Codes `0x8C`–`0x9F` remain reserved.

## Upload procedure

The app uploads a config like this:

1. Read device info and check versions as the core profile requires. Check
   that the document's `version` equals `config_schema_version` and that its
   length is at most `max_config_bytes`.
2. Subscribe to Config status, then send Abort to clear any stale transfer.
3. Send Start with the length and CRC.
4. Send chunks in order. Each chunk's offset is the sum of the data already
   accepted. After an `OffsetMismatch`, read `received_length` from Config
   status and continue from there.
5. Send Commit. On a rejection, show the diagnostic from Config status and
   stop. On `Saved`, keep `saved_crc32` and expect a disconnect.
6. Reconnect after the restart and read Config status. The upload is active
   when `active_source` is `1` and `active_crc32` equals `saved_crc32`, and
   `boot_flags` bits 0 and 3 are clear.

If the connection drops after Commit without a write response, the outcome is
unknown. The app reconnects and reads Config status. If `active_source` is
`1` and the app has `saved_crc32` from the `Saved` notification, it compares
the CRCs. Otherwise it reads back the active config and compares it with the
uploaded document as JSON values, because the canonical bytes can differ.

## BLE recovery

Before BLE, a lighting setup failure called `local_argb::fail_off()` and
refused CAN, with no way to repair the config short of reflashing. An override
can still fail at boot despite the dry run: an attach-time failure, a firmware
update that changes the signal catalog or runtime capacities, or a factory
config defect.

With BLE, after a lighting setup failure:

- The LEDs stay failed off and CAN acquisition is not started, as before.
- BLE still starts. Its startup depends on the board safe defaults, startup
  black and NVS initialization, not on a successful config apply.
- Config status sets boot flag 3, and Config, Config status and Command stay
  reachable over a bonded link, so the owner can commit a corrected config or
  revert to factory.
- Live signals report no fresh data, because the telemetry facade is not
  started.

The firmware composition update for this path belongs to the
`companion_ble` implementation (issue #163).

## Out of scope

- Firmware and app implementation, tracked by the companion-app milestone
  issues.
- Hot reload or live mutation of the running action engine.
- Schema migrations and accepting more than one schema version.
- Brightness and preset config fields.
