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

## Golden vectors and provenance

The following vectors are synthetic, privacy-safe test data derived from the
field mappings above. They check implementation consistency; they are not
vehicle evidence or a claim of Australian-market compatibility. Per-channel
evidence confidence is recorded in
[Mazda signal evidence](signal-evidence.md).

| Message | Payload | Expected result | Provenance |
| --- | --- | --- | --- |
| `0x202` | `09 5B 00 00 00 00 00 00` | `598.75 rpm`, retained `0 km/h` candidate | synthetic raw values |
| `0x202` | `84 D0 FF FF 00 00 00 00` | `8500 rpm`, retained `655.35 km/h` candidate | synthetic boundary vector |
| `0x228` | `24 81 07 FF 04 F0 00 00` | selector `D`, actual `2nd` | synthetic transmission vector |
| `0x43E` | `00 00 00 40 3D 00 00 00` | all doors/liftgate open, unlocked | synthetic door vector |
| `0x09A` | `00 00 0C 00 02 00 00 00` | both lamps on, low wiper on | synthetic lamp/wiper vector |
| `0x091` | `00 00 30 00 00 00 00 00` | front wiper `Intermittent`, turn requests off | synthetic switch vector |

The reviewed source DBC is committed at [`mazda_custom.dbc`](mazda_custom.dbc).
Raw captures remain private and outside the repository. Only reviewed signal
definitions and synthetic test vectors are represented in the decoder and tests.
