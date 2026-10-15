# Spec: Extensible companion live signals

Status: **Final wire contract for companion protocol 1.1.** This extension
does not replace the implemented [fixed-layout contract](live-signals.md) and
does not claim firmware, phone, bench, or vehicle validation. Firmware
implementation is tracked in
[#195](https://github.com/Yuke-hd/mazda-can-accessory-controller/issues/195);
production iOS integration remains in
[#41](https://github.com/Yuke-hd/esp-can-companion/issues/41).

[ADR-0005](../../decisions/0005-discoverable-live-signals.md) records the
accepted architectural direction and its tradeoffs. This document freezes the
wire details for the firmware and companion-app implementations.

## Objective

Add a discoverable, bounded live-signal stream so adding a readable Boolean,
Number, or Enum to the provider catalog does not require another BLE layout
version, a hand-maintained BLE slot, or an app release to display the signal.
Keep the current stream available for existing clients.

This is one capability: catalog discovery and value streaming are the two
parts of the same decoding contract. A client cannot interpret compact handles
without the matching catalog.

Owner-confirmed requirements:

- Preserve the current stream alongside the extensible stream.
- Let clients discover and display new signals and metadata automatically.
- Track firmware work here and production iOS work in
  `Yuke-hd/esp-can-companion`.

The app retains fixed-layout 1 and 2 decoding. Existing G-meter tickets own
layout 2 work. Preserve both legacy decoders and whichever supported legacy
layout firmware reports; this initiative does not supersede those tickets or
change legacy bytes.

Approved decisions:

- Export every Read-capable catalog entry whose type is Boolean, Number, or
  Enum. Do not add a configurable BLE allowlist or per-signal subscription.
- Encode Numbers as little-endian IEEE-754 binary32. The initial bounds are 64
  signals, 32 choices per Enum, and a 16 KiB serialized catalog.
- This repository owns the firmware contract and synthetic reference vectors;
  production iOS integration remains in its separate tracker.
- Generic signal rows may use semantic keys as labels. Curated/localized UI
  names can be added by the app without becoming protocol requirements.
- The provider catalog is immutable for the lifetime of the BLE service.
- At a small MTU, signals refresh across several notifications rather than all
  21 current signals arriving in every 100 ms slot.

## Current limitation and proposed solution

`LiveSignalSampler` currently encodes 21 slots into 28 bytes in fixed
layout 2, which adds acceleration to the original 19-slot, 23-byte layout 1.
Numeric offsets, Boolean bits, enum codes, and status positions are fixed by
`live_signal_layout.cpp`. The fixed-layout contract requires a new layout
version even for an appended signal.

The new stream separates three concerns:

| Concern | Identity / version | Changes when |
| --- | --- | --- |
| Catalog grammar | `catalog_format` | The catalog header, descriptor, or existing TLV meaning changes incompatibly. |
| Packet grammar | `format` | Packet or record framing, or an existing field meaning, changes incompatibly. |
| Signal meaning | Catalog semantic key | A genuinely different signal or incompatible interpretation is introduced. |
| Handle mapping and metadata | Catalog token, scoped to one connection | The catalog is regenerated; clients always discover it again after reconnecting. |

A new signal or enum choice changes catalog content, not packet grammar.
Handles are compact references, not persistent identities. Clients key their
UI by the semantic signal key and enum choice key.

Length-delimited records let an older decoder skip unknown signals, types,
and optional metadata fields without misreading the records that follow.
Fixed spare bits or an append-only packed struct would still impose shared
slot limits and require the app to know each new signal beforehand.

## Compatibility and GATT contract

Companion protocol **1.1** adds two characteristics under the existing base
UUID `ab49xxxx-09b6-4509-bf8e-2790253baf98`:

| Attribute | `xxxx` | Properties | Security |
| --- | --- | --- | --- |
| Signal catalog | `0006` | Read, Write with response | Encrypted with an accepted, stored bond |
| Extensible live signals | `0007` | Notify | Encrypted with an accepted, stored bond |

- Keep UUID `0004`, its current 28-byte layout `2`, enum tables, status
  mapping, and pacing unchanged. This extension does not add signals to that
  fixed layout. The app retains layout `1` decoding for older firmware.
- Keep Device info's original fields and offsets, and continue reporting the
  actual legacy `live_signal_layout_version` (`2` on current firmware). Append
  `extension_version` u8 (`1`) and `extension_flags` u8 after `hardware_id`;
  bit 0 means the catalog and extensible stream are usable, and other bits are
  zero. Existing clients ignore the appended bytes under the minor-version
  compatibility rule. New clients require extension version `1` and bit 0.
- `catalog_format` and packet `format` are independent u8 versions, both
  initially `1`. Neither reinterprets the legacy layout-version field.
- Existing protocol 1.0 devices still work through the legacy stream.
- The two streams have separate CCCDs and gates. Enabling or disabling either
  one does not change the other. If both are enabled, serve both under their
  individual rate and size bounds and account for their combined resource cost.
- Apply the existing Service Changed behavior for bonded clients after a GATT
  table update. The service now has seven characteristics; GATT capacity and
  storage for both independent Live signals CCCDs must cover the additions.
- New signal keys and choices require no format or protocol-version increment.
  New optional metadata tags or type codes are compatible additions documented
  in a later protocol minor. Changing the catalog grammar or packet/record
  encoding requires a new corresponding format version. Retiring a supported
  fixed layout needs a separate approved migration.

## Catalog discovery

Build the export from `SignalProvider::catalog()`, without including Mazda
private headers. Export every Read-capable entry of the supported types.
Resolve provider handles once; do not send `SignalId` or raw Mazda enum values.

- Assign nonzero u16 wire handles in the order Read-capable, supported entries
  appear in `SignalProvider::catalog()`. Handles may change between firmware
  builds. Assign nonzero u16 enum-choice handles in each descriptor's choice
  order and map provider enum values through choice keys.
- Semantic keys must be unique. Never reuse a key for a different meaning,
  type, or unit; introduce a new key for an incompatible replacement.
- Export type, unit, catalog validation/evidence status, and enum choice keys.
  Validation is catalog metadata, not a claim that this BLE feature or a
  reading was validated on a vehicle. Never infer confidence from arrival time.
- Use the explicit provider-to-wire maps below, not provider enum ordinals.
  Tokens are case-sensitive UTF-8 byte strings. Unrecognized token strings are
  preserved as opaque metadata; a client must not assume a unit or assign
  stronger evidence to an unfamiliar token.
- Assign a nonzero u32 catalog token for each connection. It must differ from
  the token used for the preceding connection during the current firmware
  run. It is not a firmware version, hash, identifier, or time, and it may
  repeat after reboot. Disconnect discards the map; on reconnect, the client
  reads the complete catalog again and rejects packets or selector writes
  carrying the prior connection's token.

Catalog document grammar, all integers little-endian:

- Header: `catalog_format` u8 (`1`), `signal_count` u16.
- Each descriptor: `body_length` u16, then a body starting with `handle` u16
  and metadata TLVs (`tag` u8, `length` u16, `value[length]`).
- Mandatory tags, each present exactly once: `1` semantic key (UTF-8); `2`
  type (u8: Boolean `1`, Number `2`, Enum `3`); `3` unit token (UTF-8); `4`
  evidence token (UTF-8). Tag `5` choices is mandatory exactly once for Enum
  and forbidden for Boolean or Number. Duplicate tags are malformed, including
  duplicate unknown tags.
- Choices value: `count` u16, followed by `choice_handle` u16, `key_length`
  u8, and UTF-8 key bytes per choice. No raw provider choice numbers. Choice
  count is 1–32; handles start at 1 and are unique within that descriptor,
  and choice keys are non-empty and unique within the Enum.
- Unknown, non-duplicated tags are skipped by length. No string terminators
  are transmitted. Strings must be valid UTF-8; lengths count encoded bytes.

Wire token maps are explicit and exhaustive for the current provider enums:

| Provider unit | Wire token | Meaning |
| --- | --- | --- |
| `SignalUnit::None` | `none` | No engineering unit; required for Boolean and Enum. |
| `SignalUnit::KilometresPerHour` | `km/h` | Kilometres per hour. |
| `SignalUnit::RevolutionsPerMinute` | `rpm` | Revolutions per minute. |
| `SignalUnit::MetresPerSecondSquared` | `m/s^2` | Metres per second squared, including both acceleration channels. |
| Unrecognized provider unit | `unknown` | Unit is not known by this protocol implementation. |

| Provider validation status | Wire token | Meaning |
| --- | --- | --- |
| `ValidationStatus::Reference` | `reference` | Source-derived or interpreted, without a matching documented vehicle observation. |
| `ValidationStatus::Observed` | `observed` | Correlation or partial exercise; complete semantics or range is not established. |
| `ValidationStatus::Confirmed` | `confirmed` | Reviewed repository evidence supports the specific field interpretation and returned value/scale. |
| Unrecognized provider status | `unknown` | Evidence status is not known by this protocol implementation. |

The evidence token describes catalog metadata only. It does not certify this
BLE feature, a reading, a vehicle, timing, or freshness. See
[`signal-evidence.md`](../../protocol/signal-evidence.md) for project evidence
definitions.

Initial catalog bounds are 64 exported signals, 32 choices per Enum, 63 UTF-8
bytes per signal or choice key, 31 UTF-8 bytes per unit/evidence token, and
16 KiB for the complete serialized catalog. Keys and tokens must be non-empty;
Boolean and Enum entries use unit token `none`. An empty catalog is valid.
An unsupported provider type is omitted. A supported entry with invalid
metadata or an export exceeding any bound disables the extension instead of
producing a partial catalog.

Catalog readers reject a malformed document as a whole: truncated lengths,
invalid UTF-8, invalid mandatory field sizes, duplicate signal keys or
handles, duplicate Enum choice keys or handles, Enum choice counts outside
1–32, tag `5` on Boolean or Number, and a document over the published bounds.
Unknown type codes are retained as opaque descriptors and do not prevent known
descriptors from being used. Clients do not decode or expose values for an
opaque type; an exactly matching complete record still proves that the
producer reported that signal for liveness.

Transfer uses a bounded page selector on the catalog characteristic, following
the existing Config read-back pattern without sharing its state:

- Initial Read on a connection selects offset zero. Response header:
  `catalog_format` u8,
  `token` u32, `total_bytes` u16, `offset` u16, `data_length` u16, followed by
  up to 200 document bytes. The response is at most 211 bytes; ATT long reads
  read a single selected page. A page can split a descriptor or TLV.
- Select a subsequent page with a seven-byte Write Request: format u8, token
  u32, document offset u16. The successful write selects the next Read; it
  has no provider, config, storage, or CAN side effect.
- A selector Write Request is exactly seven bytes. After the GATT service has
  checked the accepted-bond security gate, validation returns the first error
  in this order: wrong length is `Invalid Attribute Value Length` (`0x0D`);
  unknown format is `UnsupportedOperation` (`0x81`); an unusable export or
  wrong token is `InvalidState` (`0x83`); offset beyond the document is
  `InvalidPdu` (`0x80`). Offset equal to total returns an empty terminal page.
  A failed selector write leaves the selected page unchanged.
- The selector belongs to the connection and resets on disconnect. One page
  remains byte-stable across its ATT Read Blob requests. A page may split a
  descriptor or TLV. A failed or inconsistent read requires restarting
  discovery from offset zero. Every catalog read and selector write requires
  an accepted bond; the GATT service enforces that before protocol parsing.
- The client verifies matching token, contiguous requested offsets, stable
  total size, page lengths, descriptor/TLV bounds, and catalog uniqueness
  before installing the complete map. It does not display values from a
  partial or failed download and downloads the full catalog again after every
  reconnect, even when the token matches a prior connection.

## Extensible value packets

Every notification is independently parseable and at most `min(MTU - 3, 244)`
bytes. Streaming still requires ATT MTU at least 64. Records never split
across notifications; several packets update independent sets of signals,
not fragments of an atomic snapshot.

| Packet field | Bytes | Meaning |
| --- | ---: | --- |
| `format` | 1 | Extensible format `1`. |
| `sequence` | 2 | Little-endian wrapping attempt counter; starts at zero whenever streaming restarts. |
| `flags` | 1 | Bit 0 has the existing telemetry-started meaning; other bits are zero when sent and ignored by clients. |
| `catalog_token` | 4 | Mapping required to interpret the records. |
| Records | Remaining | Consume length-delimited records to the end of the packet. |

All multi-byte fields are little-endian. A packet is at least the eight-byte
header, and its remaining bytes must be consumed exactly by records. Each
record begins with `record_length` u8 (bytes after that byte), `handle` u16,
`type` u8, and `status` u8, followed by value bytes. Thus a record without a
value takes five bytes and `record_length` is at least four. A record never
crosses a notification boundary.

- Status bits 0–2 reuse the legacy availability/read-outcome codes; bit 3 is
  value-present; bits 4–7 are reserved and zero. Code `3` means
  `FreshnessUnverified`; code `2` means `Stale`, matching the implementation
  and the legacy status table. The brake demotion is code `3`; legacy bytes
  and code assignments do not change.
- Boolean is one byte, `0` or `1`; Number is little-endian IEEE-754 binary32
  in catalog engineering units; Enum is a two-byte nonzero choice handle from
  the matching descriptor. No per-signal scale or offset table is used.
  Finite negative Numbers, including signed zero and subnormals, are
  representable. NaN and either infinity are not.
- When value-present is clear, there are no value bytes. When it is set, the
  exact value length is one byte for Boolean, four bytes for Number, and two
  bytes for Enum. Boolean values other than `0` or `1`, non-finite Numbers,
  unknown Enum choices, failed reads, and wrong-type provider values omit the
  value. An unencodable value preserves the provider's availability.
- Retained Stale/Unavailable values may be present but must be shown with
  their non-fresh status. Only availability code `1` means Fresh.
- Preserve the existing brake demotion: `vehicle.brake_pressed` never becomes
  Fresh, even if a defective provider reports it. Reuse the existing narrowly
  scoped policy exception; do not infer demotion from an absent timeout on
  other signals.
- A different token invalidates the map and clears every displayed value until
  rediscovery. An unknown packet format is rejected and does not refresh stream
  or per-signal liveness. Structurally malformed packets are rejected as a
  whole before applying any records or updating liveness.
- Unknown handles are skipped by record length. A well-framed record whose
  handle and type exactly match an opaque catalog descriptor refreshes that
  signal's liveness and clears its usable value; its type and value are not
  decoded. For a known handle, a packet type byte that differs from the
  catalog type clears the value but does not refresh liveness, including when
  the packet type byte is unknown to this client. For a known type match, an
  invalid value length, invalid Boolean/Enum value, or unknown status clears
  the prior value and refreshes liveness. Unknown status includes reserved
  status code `7` and any nonzero reserved status bits. Duplicate handles
  anywhere in one packet are malformed, including duplicate unknown handles.
- Wrong-token, unknown-format, or whole-packet rejected notifications refresh
  neither the stream watchdog nor any signal timer. A matching-token packet
  with malformed record framing is rejected as a whole and does not refresh
  the two-second stream watchdog. Unknown well-framed handles/types are skipped
  but do not make the packet malformed. A structurally valid packet with the
  active token refreshes the stream watchdog even if it contains no recognized
  records.

## Sampling, pacing, and liveness

- Sample with `read()` only, at most once per signal per 100 ms BLE slot.
  No subscriptions, new tasks, provider lifecycle calls, or CAN requests.
- Each stream independently attempts at most one notification per 100 ms. If
  both CCCDs are enabled, both streams may send in a slot. The legacy stream
  retains its existing behavior. The extensible stream packages latest
  per-signal readings; it keeps no history queue and never waits for mbuf
  availability.
- Select due records fairly in rotating handle order. A record is due on
  startup, after a value/status change versus its last stack-accepted record,
  or one second after its last accepted record. Continuous changes must not
  starve other due signals. Advance the fair cursor after every attempt,
  including a failed stack-queue attempt; failed records remain due with their
  latest state. An empty catalog is valid and sends an eight-byte empty packet
  on startup and as its one-second heartbeat.
- Acceptance by the stack updates only the records in that packet. It is not
  proof of delivery to the phone. Sequence gaps describe attempted packets.
- Streaming requires the extensible CCCD enabled, an encrypted link with an
  accepted stored bond, ATT MTU at least 64, and a usable export. First packet
  is attempted within 100 ms of all gates holding.
  With successful queueing, all signals must be attempted within 1.3 s at the
  minimum MTU and maximum supported catalog. After the one-second heartbeat
  threshold, due records must be attempted within another 1.3 s.
- At MTU 247 the current 21-signal catalog's records occupy 142 bytes plus
  the eight-byte header and fit in one packet. At MTU 64 they require three
  packets. For 64
  Numbers, five nine-byte records fit per packet, requiring 13 packets. These
  are software byte-count bounds, not measured throughput guarantees.
- Treat the whole stream as stalled after two seconds without a structurally
  valid matching-token packet, as for the legacy stream. Also clear each
  signal independently after three seconds without a recognized record, so
  traffic for other signals cannot preserve an obsolete Fresh display after
  partial loss. A recognized record is fully framed and has a handle in the
  active catalog with exactly the declared type. It refreshes that signal's
  liveness timer even when its status or value is unknown, including an
  opaque descriptor or invalid value length, and clears its usable value. A
  known handle with a mismatched type clears its value but does not refresh
  liveness. Unknown handles are skipped. A rejected whole packet updates no signal timer.
  Reconnect, security loss, CCCD disable, and map invalidation clear all
  values.
- Arrival never creates freshness or updates another signal's liveness.
  Per-signal expiration is a client display guard, not a provider freshness
  timeout.
- The 100 ms sampling/rate, one-second heartbeat, and 1.3 s coverage deadlines
  exclude time spent in a Config or Command write handler on the BLE host
  context. No notification is sent while a Config or Command write is being
  handled; write responses, Config status, clear-bonds, disconnect, and restart
  work retain their existing priority. Frames may continue until disconnect
  completes after commit or revert-to-factory. After Clear bonds, both streams
  stop at the next sample because the bond is no longer accepted.

## Resource limits and failure behavior

Initial published implementation bounds:

- 64 exported signals; 32 choices per Enum; 63 UTF-8 bytes per signal/choice
  key; 31 bytes per unit/evidence token; 16 KiB serialized catalog.
- One connection; one selected metadata page of at most 211 bytes; one
  extensible packet of at most 244 bytes; bounded per-signal state.
- Serialize metadata pages from immutable descriptors without allocating a
  full 16 KiB document. No unbounded allocations or per-sample allocation in
  project codec/scheduling code; NimBLE mbuf allocation remains nonblocking.
- Validate bounds and descriptor well-formedness before marking the extension
  usable. An invalid/oversized export disables only the extension, clears its
  Device info capability bit, and fails catalog accesses with `InvalidState`.
  Keep legacy BLE, startup, CAN, and lighting behavior operational. Never
  silently truncate the export or promote missing data to Fresh.
- Report build size and static stack estimates, and account for both Live
  signals notifiers, their separate CCCDs, and metadata paging in the NimBLE
  resource budget. Hardware high-water and timing measurements remain
  separate required bench evidence before release.

Increasing capacity is a reviewed resource decision, not a new packet grammar.
Clients enforce the advertised document length and these initial decoding
bounds; raising a published limit requires a compatibility review for older
clients.

## Tech stack and project structure

C++17, ESP-IDF 5.5.4 (`esp32`), the existing NimBLE binding, and the pinned
`esp32-vehicle-can-core` 0.2.1 provider contracts. No new runtime dependency.

| Location | Responsibility |
| --- | --- |
| `lib/companion_protocol/` | Catalog export/page encoding, typed record encoding, bounded scheduling and format constants. |
| `components/companion_ble/` | Protected GATT reads/writes, CCCDs, host-task timers and nonblocking notifications. |
| `components/mazda_telemetry/` | Existing semantic catalog and read-only provider; the ordinary signal-addition path. |
| `tests/host/` | Doctest codec, discovery, compatibility and scheduler tests with synthetic providers/reference decoder. |
| `tools/` | Existing architecture/header validators; adjust only for the new owned public headers and boundaries. |
| `docs/specs/companion/` | This proposed contract and the existing legacy/core contracts. |

Keep Mazda keys confined to the existing permitted layout/policy location.
The extensible codec consumes generic catalog metadata. Preserve the public
ownership contracts in [module boundaries](../../architecture/module-boundaries.md).

## Code style

Follow existing namespace, snake_case function, `kConstant`, and bounded,
`noexcept` codec conventions. The following is an existing append idiom from
`companion_protocol/bytes.hpp`, representative of the required style:

```cpp
constexpr bool push_u16(std::uint16_t value) noexcept {
  return room_for(2) && push(static_cast<std::uint8_t>(value & 0xFFU)) &&
         push(static_cast<std::uint8_t>(value >> 8U));
}
```

Do not cast structs to wire bytes or rely on padding/native endian. Verify
binary32 support at compile time and use byte-safe copying for float bits.

## Commands and testing strategy

For implementation validation, use a fresh host build directory and the
repository's pinned doctest tests plus architecture/header gates:

```sh
python3 tools/check_toolchain.py --scope host
cmake -S . -B /tmp/mazda-extensible-live-host -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DMAZDA_BUILD_HOST_TESTS=ON
cmake --build /tmp/mazda-extensible-live-host --parallel
ctest --test-dir /tmp/mazda-extensible-live-host --output-on-failure
python3 tools/check_architecture.py --root . --compiler c++ --cmake cmake
python3 tests/tools/check_architecture_test.py -v
```

Use a new path if that directory already exists. Follow
[supported builds](../../development/supported-build.md) for offline pins,
Python prerequisites, and the Linux ASan/UBSan gate. For firmware compilation
and resource evidence (no flashing):

```sh
source /path/to/esp-idf/v5.5.4/export.sh
python3 tools/check_toolchain.py --scope firmware
idf.py -C firmware/weact-can485-v1.1 set-target esp32
idf.py -C firmware/weact-can485-v1.1 build
idf.py -C firmware/weact-can485-v1.1 size
```

The test matrix must cover every encoding/status outcome, not a superficial
line-coverage target:

1. Legacy layout 1/2 golden packets remain unchanged; clients retain their
   reported-layout fallback behavior.
2. Add a synthetic Boolean, signed Number, and Enum solely to the provider
   catalog. The unchanged exporter and reference decoder discover and decode
   them, with identical extensible format/version constants.
3. Renumber/reorder provider IDs and raw enum values; new discovery preserves
   semantic keys and values. Reconnect cannot reuse old handles.
4. Add an enum choice without a BLE-specific choice table; generic display
   uses its discovered key. Unknown types/tags/handles do not corrupt peers.
5. Exercise every availability, read failure, brake demotion, non-finite
   Number, missing value, unsupported outcome, and malformed record/page.
6. Boundary catalogs (0, 1, 19, 21, 64, and over-limit), choice/string/document
   bounds, page/Read Blob continuity, wrong tokens, duplicates, and truncated
   TLVs fail conservatively without out-of-bounds access or partial UI updates.
7. At MTUs 23, 63, 64, and 247, prove gates, payload bounds, fair coverage,
   heartbeat deadlines, sequence wrap, repeated queue refusal, retry/latest
   state, re-enable behavior, and both-stream subscription bounds.
8. A synthetic client clears missing individual records despite other traffic,
   clears everything on stream stall, and never upgrades availability from
   packet arrival. No partial catalog becomes active.
9. Existing boundary checks prove no CAN, subscription, LED, Mazda-private,
   or config-persistence dependency has leaked into the codec/BLE binding.

Phone discovery, upgrade Service Changed behavior, bond gates, host stack,
radio/CAN/LED coexistence, and actual timing require separate bench tests.
They are not established by host tests or a firmware build.

## Boundaries

- **Always:** preserve receive-only CAN and existing fail-off behavior; copy
  provider availability with only the existing brake demotion; use generic
  catalog contracts and synthetic fixtures; validate before advertising the
  extension; keep config processing and lighting independent of BLE.
- **Ask first:** retire a supported fixed layout; expose non-Read signals;
  introduce a configurable subscription policy; change safety/freshness rules; add dependencies or
  change the pinned core; increase published limits after reviewing resource
  and client compatibility; flash or interact with hardware.
- **Never:** transmit/poll CAN; serialize runtime SignalIds, raw enum values,
  raw frames, VINs, locations or timestamps; invent brake freshness; hand-edit
  generated/vendor trees; make discovery change active/persisted config.

## Success criteria

- Adding a supported readable provider signal or enum choice requires no
  extensible format bump, packet-offset change, BLE signal table, or generic
  decoder change. Its semantic metadata and current status/value are visible.
- Existing layout 2 clients keep receiving byte-compatible packets; new
  clients discover the extension and retain layout 1/2 fallback according to
  the legacy layout reported by firmware.
- Unknown extensions and malformed/mismatched input cannot produce a wrong
  known value, an obsolete Fresh display, or an availability promotion.
- Discovery, buffers, packet lengths, fair refresh, retries, and work per BLE
  slot stay within the declared bounds, with no historical frame queue.
- Host tests, relevant structural/sanitizer gates, firmware compilation, and
  resource reporting pass. Unrun bench/phone/vehicle checks are stated plainly.

## Synthetic reference vectors

[`tests/fixtures/companion/extensible-live-v1.json`](../../../tests/fixtures/companion/extensible-live-v1.json)
is the canonical synthetic cross-language oracle for catalog paging and live
records. It contains no vehicle or third-party data. Both repositories identify
this set by `fixture_id: extensible-live-v1` and `fixture_version: 1`; they
must consume identical wire hex and expected outcomes. Increase
`fixture_version` only when correcting or extending vectors without changing
the wire contract. An incompatible wire grammar uses a new contract ID and
fixture file.

The vectors cover multi-page discovery, Boolean, signed binary32, Enum,
unknown/failing values, duplicate-handle rejection, and token invalidation.
They prove byte-level software agreement only; they are not phone, bench, or
vehicle validation.
