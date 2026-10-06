# Telemetry decoder behaviour

The Mazda decoder consumes `vehicle_core::RawCanFrame` values and writes
semantic fields in `mazda::VehicleState`. It has no CAN driver, transport,
polling, injection, board-header, or global-clock dependency. Test-only direct
frame helpers exercise the same functions as the receive path; there is no
file-format parser or public replay API. Field coordinates and synthetic
vectors are in [Mazda decoder mappings](../../protocol/decoder-mappings.md).

## Frame handling

Frames are accepted only when they are standard, non-RTR frames with the
expected identifier and exactly eight data bytes. Decoder outcomes are
`Ignored`, `Decoded`, or `Malformed`. An owned malformed frame does not update
any signal. Boolean fields decode both states directly.

## Values and availability

Selector and actual transmission gear are separate signals. Selector raw zero
is the source-defined `Shifting` state; raw values `5..7` have an explicit
unknown semantic value. Actual gear raw zero is `P_or_N` and is represented as
`ActualGear::ParkOrNeutral`; raw 15 is `ActualGear::Shifting`; raw values
`7..13` have an unknown semantic value. A well-formed undefined enumeration is
still decoded, but it cannot preserve a previous actionable value: availability
processing maps the unknown semantic value to unavailable.

The engine RPM decoder rejects raw values greater than `34000`, corresponding
to the source-declared `8500 rpm` maximum at scale `0.25`. The retained SPEED
candidate uses an unsigned 16-bit value at scale `0.01`, so `655.35 km/h` is its
representable maximum. That encoding boundary is not a validated vehicle
physical limit.

The supplied DBC has no cycle-time declaration. Other signals therefore have
unconfigured freshness by default; callers can set per-signal timeouts through
`VehicleFreshnessPolicy`. `Signal::refresh()` marks an unconfigured value stale
when time advances, and snapshots do not mutate the source state. Turn/request
normalization and its 2 s freshness policy are specified in
[turn state](turn-state.md).
