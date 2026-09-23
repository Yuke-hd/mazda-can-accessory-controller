# Versioned USB raw CAN capture format (retired)

> Retired 2026-09-08 by S1-D / [#59](https://github.com/Yuke-hd/esp32-vehicle-can-core/issues/59).
> The custom format and its reader, writer, replay harness, fixture, and
> validator were removed from the repository. SavvyCAN is the selected capture
> and replay tool. Do not add a new implementation, parser, exporter, or
> adapter based on this historical specification.

Status: historical v1 specification for superseded MCAN-5 work; retained for
context and attribution only.

This format is an output-only, line-oriented text stream. It records frames
received by a receive-only CAN device and the loss or discontinuity information
needed to interpret the stream. It is not a command protocol and it does not
provide a CAN transmit path.

## 1. Encoding and framing

The stream is UTF-8 text with LF (`0x0a`) line endings. A producer writes one
complete record per line and flushes records in capture order. A line is never
split into a second record. An optional UTF-8 BOM is forbidden. ASCII is used
for all v1 record syntax; non-ASCII metadata is represented with percent
encoding.

The first line is exactly:

```text
MCAN-CAPTURE 1
```

Blank lines and lines beginning with `#` are comments and have no semantic
meaning. A record consists of an uppercase type followed by space-separated
`key=value` fields. Keys are ASCII lower-case with underscores. Values contain
no spaces. A key occurs at most once in a record.

Metadata values use UTF-8 percent encoding: unreserved bytes (`A-Z`, `a-z`,
`0-9`, `-`, `.`, `_`, `~`) are literal and every other byte is written as
`%HH` with uppercase hexadecimal digits. A literal percent sign is therefore
`%25`. Producers must use the canonical form; readers must reject malformed
percent escapes in strict mode.

There are no binary integers in this text format. Decimal integer fields are
unsigned, base 10, with `0` as the only representation of zero and no leading
zeroes. CAN identifiers are lower-case hexadecimal with a `0x` prefix. The
mandatory `byte_order=big-endian` session field makes conversion to a future
binary representation deterministic. In v1, frame `data` is not byte-swapped:
bytes appear left-to-right in CAN wire/index order (`data=001122...`).

## 2. Session record

Exactly one `SESSION` record is required immediately after the version line
(comments may occur between them). These fields are required:

| Field | Meaning |
| --- | --- |
| `firmware` | Percent-encoded firmware identity and version. |
| `board` | Percent-encoded board identity/revision. |
| `bitrate_bps` | Configured nominal CAN bitrate in bits per second. |
| `clock` | Clock source; v1 requires `monotonic`. |
| `clock_unit` | Timestamp unit; v1 requires `us` (microseconds). |
| `byte_order` | v1 requires `big-endian`; see the encoding rule above. |
| `clock_hz` | Clock frequency used by the timestamp conversion, in Hz. |
| `dropped_frames` | Cumulative frames unavailable to the stream at session start. |
| `dropped_records` | Cumulative records unavailable to the stream at session start. |

`SESSION` may include additional namespaced fields in future versions. Unknown
fields are ignored by a forward-compatible reader. The `bitrate_bps`,
`clock_hz`, and counter values are decimal unsigned integers and must be
non-zero except for the two counters.

The timestamp origin is the device monotonic clock at boot or reset; it is not
wall-clock time. A session does not contain VIN, location, or absolute time.
Timestamp values are unsigned 64-bit microseconds and are never converted to
local time.

## 3. Frame record

A received classic-CAN frame is represented by one `FRAME` record with all of
the `RawCanFrame` identity fields explicit:

```text
FRAME t_us=1000 bus=0 id=0x091 format=std rtr=0 dlc=8 data=0100000000000000
```

Required fields are:

| Field | Allowed values and meaning |
| --- | --- |
| `t_us` | Monotonic receive timestamp, unsigned 64-bit microseconds. |
| `bus` | Bus identifier, unsigned 8-bit decimal (`0` through `255`). |
| `id` | CAN identifier: exactly three lower-case hex digits (`0x000`–`0x7ff`) for `format=std`, or exactly eight (`0x00000000`–`0x1fffffff`) for `format=ext`. |
| `format` | `std` for an 11-bit identifier or `ext` for a 29-bit identifier. |
| `rtr` | `0` for a data frame or `1` for a remote frame. |
| `dlc` | Classic CAN data length code, decimal `0` through `8`. |
| `data` | Upper/lower-case-independent hexadecimal payload, exactly `2 * dlc` digits for a data frame, or `-` when `dlc=0` or `rtr=1`. |

The `data` bytes are the bytes received in CAN byte order. For a remote frame
(`rtr=1`), `data` must be `-` regardless of DLC because a remote frame carries
no payload. A data frame with `dlc=0` also uses `data=-`. A reader must not
infer an application-level signal endianness from this capture format.

Every timestamped v1 record (`FRAME`, `DROP`, `STATS`, and
`DISCONTINUITY`) uses an unsigned 64-bit `t_us`. Timestamps are non-decreasing
within a segment; equal timestamps are valid. The `DISCONTINUITY` marker itself
is ordered in the preceding segment, then starts its declared segment for the
following records. A reader must use it to reset any ordering assumption.

## 4. Drop, statistics, and discontinuity records

When one or more received frames cannot be represented in the stream, the
producer emits a `DROP` record as soon as possible:

```text
DROP t_us=2000 bus=0 count=2 reason=queue-overflow
```

`t_us` is the timestamp at which the loss was observed, `bus` is the affected
bus (`all` is permitted when the source bus is unknown), `count` is the number
of omitted frames, and `reason` is a percent-encoded diagnostic token. The
session `dropped_frames` counter is the cumulative baseline at session start.
Every `DROP count` increases that cumulative count; a producer must not
fabricate a frame for a dropped record.

`STATS` provides observable cumulative counters after session start:

```text
STATS t_us=1200 segment=0 dropped_frames=2 dropped_records=0
```

Required fields are `t_us`, `segment`, `dropped_frames`, and
`dropped_records`. Both counters are absolute cumulative values from the same
origin as the `SESSION` counters, never deltas. `dropped_frames` equals the
session baseline plus the sum of all `DROP count` values observed so far.
`dropped_records` counts output records that could not be emitted; it may be
greater than the visible `DROP` records when a diagnostic record was itself
lost. Counters never decrease. A producer should emit checkpoints periodically
and must emit a final `STATS` before a clean shutdown. A power loss or reset
may leave the last checkpoint absent.

`DISCONTINUITY` marks a timestamp or capture-sequence break that cannot be
represented by a frame loss alone:

```text
DISCONTINUITY t_us=3000 bus=all segment=1 reason=clock-reset
```

Required fields are `t_us`, `bus` (`0`–`255` or `all`), `segment` (unsigned
integer), and a percent-encoded `reason`. The segment number increases for
each new monotonic-clock segment. A reader must not compare timestamps across
segments. Typical reasons are `clock-reset`, `bus-reconnect`,
`session-resume`, and `unknown`; readers must preserve unknown reasons.

Drop and discontinuity records are diagnostic records, not `RawCanFrame`
instances. They never grant a consumer permission to transmit a replacement
frame.

## 5. Malformed input and forward compatibility

The producer is fail-closed: it must stop the capture and report an error if
it cannot emit a valid version or session record. It must never substitute a
partial `FRAME` record or an SLCAN command.

A reader has two modes:

* **Strict mode** rejects the stream on an invalid header, session, known
  record, duplicate field, invalid value, or malformed UTF-8/percent escape.
* **Recovery mode** reports the line number and error, discards that complete
  line, and resumes at the next LF. It must not reinterpret malformed text as
  a valid frame. A malformed known record increments the reader's diagnostic
  error count; it does not change the last valid frame.

Unknown record types and unknown fields on known records are ignored after
their syntax is checked. This is the v1 forward-compatibility rule: a v2
producer may add records/fields without changing v1 frame semantics, but it
must retain the v1 required fields. A future version must use a new header
version rather than silently changing the meaning of an existing field.

The following are explicitly not part of this format:

* SLCAN/LAWICEL commands, including `t`, `T`, `r`, `R`, `O`, `C`, `S`, and
  `M` input commands;
* any input, acknowledgement, transmit, gateway, diagnostic, or frame-injection
  channel;
* CAN FD frames (the v1 target is classic CAN and `dlc` is limited to 8).

## 6. Normative example and golden fixture

The following complete stream was the canonical small golden fixture for the
superseded MCAN-5 work. Its values are synthetic and contain no vehicle data;
the fixture file was removed with the custom capture product.

```text
MCAN-CAPTURE 1
SESSION firmware=mcan-weact-can485-v11%2B0.1.0 board=weact-can485-v1.1 bitrate_bps=500000 clock=monotonic clock_unit=us byte_order=big-endian clock_hz=1000000 dropped_frames=0 dropped_records=0
FRAME t_us=1000 bus=0 id=0x091 format=std rtr=0 dlc=8 data=0100000000000000
FRAME t_us=1100 bus=0 id=0x202 format=std rtr=0 dlc=8 data=0000000000000000
DROP t_us=1200 bus=0 count=2 reason=queue-overflow
STATS t_us=1200 segment=0 dropped_frames=2 dropped_records=0
DISCONTINUITY t_us=2000 bus=all segment=1 reason=clock-reset
FRAME t_us=2100 bus=0 id=0x1fffffff format=ext rtr=0 dlc=0 data=-
FRAME t_us=2200 bus=1 id=0x123 format=std rtr=1 dlc=2 data=-
STATS t_us=2200 segment=1 dropped_frames=2 dropped_records=0
```

The former repository validator checked this example and its fixture
byte-for-byte. Both were removed with the custom capture product; this
historical document is no longer part of the build or test contract.
