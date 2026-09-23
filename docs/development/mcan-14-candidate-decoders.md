# MCAN-14 Mazda decoders

Status: portable decoder implementation; the listed signal definitions are
confirmed from the capture-derived DBC supplied for #51.

The decoder accepts a `vehicle_core::RawCanFrame` and writes only semantic
fields in a `mazda::VehicleState`; it has no CAN driver, transport, polling,
injection, board header, or global clock dependency. Test-only direct frame
helpers deliver synthetic frames to the same functions used by a receive path;
there is no file-format parser or public replay API.

## Confirmed fields and retained candidate

DBC start bits use the DBC notation from the reviewed source. The decoder
locations show the corresponding fixed payload byte and LSB masks.

| Message | Field | DBC definition | Decoder location | Scale + offset | Unit / values |
| --- | --- | --- | --- | --- | --- |
| `0x202` | `EngineRPM` | `7\|16@0+ (0.25,0) [0\|8500]` | `data[0..1]`, big-endian | `0.25 + 0` | rpm; raw `>34000` invalid |
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

Selector and actual transmission gear are separate signals. Selector raw zero
is the source-defined `Shifting` state; raw values `5..7` are represented as
an explicit unknown semantic value. Actual gear raw zero is the DBC's `P_or_N`
value and is represented distinctly as `ActualGear::ParkOrNeutral`; raw 15 is
`ActualGear::Shifting`, while raw values `7..13` publish an unknown semantic
value. A well-formed undefined enumeration is `Decoded`, but it cannot
preserve a previous actionable value; availability processing maps the unknown
semantic value to unavailable. Boolean fields decode both states directly.
Frames must be standard, non-RTR, exactly eight bytes, and have the expected
identifier. Decoder outcomes are `Ignored`, `Decoded`, or `Malformed`; owned
malformed frames do not update any signal.

The existing SPEED candidate uses an unsigned 16-bit encoding with scale 0.01,
so `655.35 km/h` is its representable maximum. That encoding boundary is not a
validated vehicle physical limit.

`mazda::candidate::decode_turn_switch()` continues to normalize the confirmed hazard/right/left switch
bits into `TurnState` and retains the existing 250 ms turn/request freshness
behavior. The supplied DBC has no cycle-time declaration, so the other
confirmed signals expose unconfigured freshness by default; callers can set
per-signal timeouts through `VehicleFreshnessPolicy`. `Signal::refresh()`
still marks an unconfigured value stale when time advances, and snapshots
never mutate the source state. The prior speed candidate and other
out-of-scope definitions remain unchanged.

## Golden vectors and provenance

The following vectors are synthetic, privacy-safe test data. The confirmed
vectors are derived from the reviewed DBC layout and scale; the retained speed
candidate is included only to show that its existing behavior remains intact.
None is a vehicle capture or a claim of Australian-market compatibility.

| Message | Payload | Expected result | Provenance |
| --- | --- | --- | --- |
| `0x202` | `09 5B 00 00 00 00 00 00` | `598.75 rpm`, retained `0 km/h` candidate | synthetic raw values |
| `0x202` | `84 D0 FF FF 00 00 00 00` | `8500 rpm`, retained `655.35 km/h` candidate | synthetic boundary vector |
| `0x228` | `24 81 07 FF 04 F0 00 00` | selector `D`, actual `2nd` | synthetic transmission vector |
| `0x43E` | `00 00 00 40 3D 00 00 00` | all doors/liftgate open, unlocked | synthetic door vector |
| `0x09A` | `00 00 0C 00 02 00 00 00` | both lamps on, low wiper on | synthetic lamp/wiper vector |
| `0x091` | `00 00 30 00 00 00 00 00` | front wiper `Intermittent`, turn requests off | synthetic switch vector |

The reviewed source DBC is committed at
[`docs/protocol/mazda_custom.dbc`](../protocol/mazda_custom.dbc). Raw captures
remain private and outside the repository. Only the reviewed signal definitions
and synthetic test vectors are represented in the decoder and tests.
