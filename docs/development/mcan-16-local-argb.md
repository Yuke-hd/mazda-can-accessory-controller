# MCAN-16 local semantic ARGB indicator

## Semantic boundary and colors

`local_argb` consumes only copied turn/brake effect values, their availability
deadline, and transport-owned fail-off state. It
does not include or inspect `RawCanFrame`, CAN identifiers, Mazda decoder
constants, `can_bus`, TWAI, or driver handles.

The GPIO16 WS2812B output drives a 100-pixel vehicle light strip. GPIO4 drives
the board's separate one-pixel status indicator:

| Semantic input | RGB output |
| --- | --- |
| Left/right turn | Amber running flow in the corresponding 35-pixel side area |
| Brake pressed | Solid red in the center 30-pixel area |
| Off, unknown, stale, CAN offline, or decoder error | Black `(0, 0, 0)` |

The generic solid-color compatibility path is capped by the compile-time value
16. Animation palettes use explicit bounded RGB constants; the installation
still requires a strip current budget and suitable external power. The strip
is not a safety indicator and must not be used instead of the factory cluster.

## Runtime isolation and fail-off behavior

The ESP-IDF runtime binds the one-pixel status surface to
`board::kWeActCan485V11.onboard_rgb` on GPIO4 and the 100-pixel accessory
surface to `vehicle_light_strip` on GPIO16. It configures `led_strip` v3.0.3
for WS2812, explicit GRB component order, a 10 MHz RMT resolution, and DMA
disabled. It sends explicit black frames and refreshes them before CAN startup.
Merely holding either data line low does not clear a previously latched
WS2812B color.

LED writes run in a dedicated `local_argb` task at `tskIDLE_PRIORITY + 2`, below
the `can_rx` task. Producers copy semantic snapshots into a statically allocated
length-one queue with `xQueueOverwrite`, so they never wait for LED/RMT work and
new state replaces obsolete backpressure. The application publishes only when
turn/brake state or health changes, plus a bounded 100 ms heartbeat carrying
the latest freshness timestamp; repeated turn, engine, gear, and unrelated
frames do not cause per-frame LED-task wakeups. The worker checks freshness at
least every 10 ms and clears
when elapsed time becomes greater than 250,000 us even if semantic updates stop.
The controller suppresses redundant hardware refreshes.

Every pixel-set and refresh result is checked. A failed colored write
immediately attempts black, remains fail-off for that submission, and retries
black on later worker ticks. A fresh semantic submission is required before a
color can be attempted again. Failure to create or initially clear the strip
prevents CAN startup. CAN startup/runtime failure and semantic submission
failure request black and stop acquisition.

`led_strip` 3.0.3 waits indefinitely for RMT completion internally. Every
set/refresh operation is therefore supervised by a separate task at
`configMAX_PRIORITIES - 1`, above `can_rx`; it performs only timestamp checks
and no LED/RMT work. The same supervisor checks a progress lease renewed at the
top of every worker loop, so a stall before or after the driver call (including
error logging) is also covered. Either a driver operation or worker lease that
remains stalled for more than 100,000 us requests `esp_restart`. The 10,000 us
polling interval gives a configured reset-request bound of 110,000 us under
scheduler operation. The lease is armed only after startup black and successful
worker creation, and remains disarmed on creation failure, preventing
initialization from causing a false restart. On reboot, GPIO4 and GPIO16 are
first held low and RMT black frames are sent before CAN starts. The reboot duration and successful
physical black transmission cannot be bounded if scheduling is disabled or the
CPU, RMT peripheral, or LED remains faulty.

`DecodeStatus::Ignored` is normal unrelated traffic. It establishes initial CAN
online health without changing turn state and does not erase an existing
decoder error. `DecodeStatus::Malformed` sets decoder-error health and therefore
black. Engine or gear `Decoded` traffic also cannot erase that error: only a
newer valid turn update, including the same direction, recovers it.

## Evidence and physical limitation

Deterministic host tests cover startup black, every mapping, the exact
250,000/250,001 us boundary, same-direction recovery, offline/error fail-off,
brightness limits, duplicate coalescing, length-one overwrite behavior, and
driver failure/black retry. Mixed turn/engine/ignored traffic, publication
throttling/heartbeat boundaries, driver timeout, and worker-lease
expiry/heartbeat/disarm behavior are also deterministic host tests. A
structural validator enforces semantic
isolation and the fixed WeAct/RMT configuration. CI builds the WeAct firmware.

No physical bench or vehicle test is claimed by this change. A total CPU/RMT
failure cannot transmit a black frame, and a WS2812B retains its last color
while powered; instantaneous physical fail-off therefore cannot be guaranteed
without hardware LED power gating.

Before vehicle release, run concurrent classic-CAN load and repeated LED state
changes on the WeAct V1.1 board. Record generated/received counts, application
drops, queue overflows/high-watermark, driver missed/overrun counts, bus errors,
LED transition results, startup/warm-reset clear behavior, and native USB logs.
Keep K3 OFF, add no vehicle termination, and include no raw payloads or private
vehicle data in public evidence.

The component API and configuration were reviewed on 2026-09-04 against the
[Espressif `led_strip` v3.0.3 registry release](https://components.espressif.com/components/espressif/led_strip/versions/3.0.3).
