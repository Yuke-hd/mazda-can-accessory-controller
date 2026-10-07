# Mazda decoder mappings

This document records Mazda DBC field coordinates, their current decoder
locations, and synthetic golden vectors. Evidence confidence is assigned in
[Mazda signal evidence](signal-evidence.md). Runtime decoder
semantics are specified in
[telemetry decoder behaviour](../specs/telemetry/decoder-behaviour.md).

## Source fields and retained candidate

DBC start bits use the DBC notation from the reviewed source. The decoder
locations show the corresponding fixed payload byte and LSB masks.

| Message | Field | DBC definition | Decoder location | Scale + offset | Unit / values |
| --- | --- | --- | --- | --- | --- |
| `0x078` | `VEHICLE_ACC_X` | `5\|13@0+ (0.01,-40) [-40\|40]` | `((data[0] & 0x3f) << 7) \| (data[1] >> 1)` | `0.01 - 40` | m/s²; longitudinal candidate, Reference |
| `0x078` | `VEHICLE_ACC_Y` | `8\|13@0+ (0.001,-4.096) [-4.096\|4.096]` | `((data[1] & 1) << 12) \| (data[2] << 4) \| (data[3] >> 4)` | `0.001 - 4.096` | m/s²; lateral candidate, Reference |
| `0x202` | `EngineRPM` | `7\|16@0+ (0.25,0) [0\|8500]` | `data[0..1]`, big-endian | `0.25 + 0` | rpm |
| `0x202` | `SPEED` | existing out-of-scope candidate | `data[2..3]`, big-endian | `0.01 + 0` | km/h; retained unchanged |
| `0x228` | `Selector` | `2\|3@0+ (1,0) [0\|7]` | `data[0] & 0x07` | `1 + 0` | `0=Shifting`, `1=P`, `2=R`, `3=N`, `4=D`; `5..7` unknown |
| `0x228` | `ActualGear` | `36\|4@0+ (1,0) [0\|15]` | `(data[4] >> 1) & 0x0f` | `1 + 0` | `0=P_or_N`, `1..6=1st..6th`, `14=R`, `15=Shifting`; `7..13` unknown |
| `0x43e` | `Liftgate_Open` | `32\|1@0+ (1,0) [0\|1]` | `data[4] bit 0` | `1 + 0` | `0=Closed`, `1=Open` |
| `0x43e` | `RearRightDoor_Open` | `34\|1@0+ (1,0) [0\|1]` | `data[4] bit 2` | `1 + 0` | `0=Closed`, `1=Open` |
| `0x43e` | `RearLeftDoor_Open` | `35\|1@0+ (1,0) [0\|1]` | `data[4] bit 3` | `1 + 0` | `0=Closed`, `1=Open` |
| `0x43e` | `FrontLeftDoor_Open_RHD` | `36\|1@0+ (1,0) [0\|1]` | `data[4] bit 4` | `1 + 0` | `0=Closed`, `1=Open` |
| `0x43e` | `FrontRightDoor_Open_RHD` | `37\|1@0+ (1,0) [0\|1]` | `data[4] bit 5` | `1 + 0` | `0=Closed`, `1=Open` |
| `0x43e` | `DoorsUnlocked` | `30\|1@0+ (1,0) [0\|1]` | `data[3] bit 6` | `1 + 0` | `0=Locked`, `1=Unlocked` |
| `0x09a` | `LeftIndicatorLamp` | `18\|1@1+ (1,0) [0\|1]` | `data[2] bit 2` | `1 + 0` | `0=Off`, `1=On` |
| `0x09a` | `RightIndicatorLamp` | `19\|1@0+ (1,0) [0\|1]` | `data[2] bit 3` | `1 + 0` | `0=Off`, `1=On` |
| `0x09a` | `WiperLow` | `33\|1@0+ (1,0) [0\|1]` | `data[4] bit 1` | `1 + 0` | `0=Off`, `1=On` |
| `0x091` | `HazardSwitch` | `10\|1@0+ (1,0) [0\|1]` | `data[1] bit 2` | `1 + 0` | `0=Off`, `1=On` |
| `0x091` | `RightIndicatorSwitch` | `12\|1@0+ (1,0) [0\|1]` | `data[1] bit 4` | `1 + 0` | `0=Off`, `1=On` |
| `0x091` | `LeftIndicatorSwitch` | `13\|1@0+ (1,0) [0\|1]` | `data[1] bit 5` | `1 + 0` | `0=Off`, `1=On` |
| `0x091` | `FrontWiper` | `21\|2@0+ (1,0) [0\|3]` | `(data[2] >> 4) & 0x03` | `1 + 0` | `0=Off`, `1=On`, `2=High`, `3=Intermittent` |

Selector and actual gear are separate source fields. Their software handling,
including undefined values, frame acceptance, and freshness, is specified in
[telemetry decoder behaviour](../specs/telemetry/decoder-behaviour.md).

## Acceleration contract

`BRAKE` (`0x078`, DLC 8) is decoded independently of `BRAKE_PEDAL`
(`0x165`); acceleration does not update `brake_pressed`. The source X/Y fields
are exposed as `longitudinal_acceleration_mps2` and `lateral_acceleration_mps2`.
Axis assignment and sign remain Reference pending the annotated validation
sequence in [signal evidence](signal-evidence.md#candidate-acceleration-mapping-brake-0x078).
Exact source revision and MIT attribution are in
[opendbc provenance](opendbc-provenance.md).

Canonical values use m/s², never g. The pinned vehicle-core `0.1.0` has no
acceleration `SignalUnit` enum, so the portable state and candidate metadata
use `SignalUnit::None`; the member names and this contract specify SI units.
No raw invalid code is declared upstream. All 13-bit codes are decoded;
X code 8191 represents 41.91 m/s², above the source's declared maximum 40.
The source range is not treated as an undocumented invalid-code rule.

The decoder computes `(raw_x - 4000) / 100` and `(raw_y - 4096) / 1000`
using a signed integer subtraction before one float division. This preserves
the source scale and offset with one float-representation rounding, makes
neutral exactly zero, and preserves the signed one-code steps (`±0.01` and
`±0.001` m/s²). It adds no smoothing, dead zone, or clipping. Multiplying by
float scale and then adding a float offset can introduce cancellation noise;
fused and non-fused compiler evaluation can produce different noise.

The default policy assigns both acceleration readings a 250,000 us freshness
timeout. This is operational policy and does not claim timing evidence from
#186, which remains absent. The acceleration message period and candidate
metadata timeout remain unset. Under the default policy, accepted readings are
`Fresh`, with Reference validation, while they remain within the configured
timeout. Snapshot copies preserve both values, per-message health, and policy.
Wrong DLC or invalid frames fault only the acceleration message, preserve both
last values, and make them unavailable until a strictly newer valid frame.
Older observations and conflicting observations at the same timestamp do not
change either value; identical duplicates are idempotent.

The optional output `DecoderObservation` describes the attempted frame;
`HealthObservation` combines the retained message record with that attempt's
signal result. `finish_health` preserves the caller's transport status and
copies the retained message health and frame/acceptance/fault watermarks before
setting the attempt's signal status. Neither output changes retained state.

- A valid extended-ID or remote-request frame with identifier `0x078` is
  `Ignored`. It never observes or updates the standard message record, values,
  or timestamps. The optional health output reports `signal=Unavailable` for
  that ignored attempt while preserving the caller's transport and the accepted
  record's health/watermarks.
- An older or conflicting same-timestamp standard frame is reported `Decoded`
  after its fields pass validation, but its rejected attempt reports
  `signal=Unavailable`. The first accepted values and healthy message record
  remain intact; a retained-state reading with live transport still reports
  `FreshnessUnverified` under the default policy. A consumer evaluating the
  returned attempt health can conservatively report `Unavailable` instead.
  This matches brake-pedal conflict handling. Engine-data handling differs: it
  reports `signal=Available` whenever an accepted RPM value exists, even when
  the new observation is rejected.
- An identical healthy duplicate reports `signal=Available` while the retained
  raw signals are valid. A malformed frame faults the message record and
  reports `signal=Unavailable`; an equal-time valid frame cannot clear that
  fault. A strictly newer valid frame reports healthy/available and clears the
  fault watermark.

The supported custom-DBC comparison table remains restricted to fields in the
byte-for-byte reviewed custom DBC. Acceleration metadata is separately covered
by decoder tests against the exact upstream fields recorded in #186.

## Golden vectors and provenance

The following vectors are synthetic, privacy-safe test data derived from the
field mappings above. They check implementation consistency; they are not
vehicle evidence or a claim of Australian-market compatibility. Per-channel
evidence confidence is recorded in
[Mazda signal evidence](signal-evidence.md).

| Message | Payload | Expected result | Provenance |
| --- | --- | --- | --- |
| `0x078` | `00 00 00 00 00 00 00 00` | `-40 m/s²` longitudinal, `-4.096 m/s²` lateral | synthetic raw zero |
| `0x078` | `1F 41 00 00 00 00 00 00` | both `0 m/s²` | synthetic offset vector (raw X 4000, Y 4096) |
| `0x078` | `17 70 BB 80 00 00 00 00` | `-10 m/s²`, `-1.096 m/s²` | synthetic negative vector |
| `0x078` | `27 11 38 80 00 00 00 00` | `10 m/s²`, `0.904 m/s²` | synthetic positive vector |
| `0x078` | `FF FF FF FF FF FF FF FF` | `41.91 m/s²`, `4.095 m/s²` | synthetic full-width boundary; unrelated bits set |
| `0x202` | `09 5B 00 00 00 00 00 00` | `598.75 rpm`, retained `0 km/h` candidate | synthetic raw values |
| `0x202` | `84 D0 FF FF 00 00 00 00` | `8500 rpm`, retained `655.35 km/h` candidate | synthetic boundary vector |
| `0x228` | `24 81 07 FF 04 F0 00 00` | selector `D`, actual `2nd` | synthetic transmission vector |
| `0x43E` | `00 00 00 40 3D 00 00 00` | all doors/liftgate open, unlocked | synthetic door vector |
| `0x09A` | `00 00 0C 00 02 00 00 00` | both lamps on, low wiper on | synthetic lamp/wiper vector |
| `0x091` | `00 00 30 00 00 00 00 00` | front wiper `Intermittent`, turn requests off | synthetic switch vector |

The reviewed source DBC is committed at [`mazda_custom.dbc`](mazda_custom.dbc).
Raw captures remain private and outside the repository. Only reviewed signal
definitions and synthetic test vectors are represented in the decoder and tests.
