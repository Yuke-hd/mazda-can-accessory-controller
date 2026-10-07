# CAN freshness debug runbook

This runbook is for issue #208, where the `TURN_SWITCH` (`0x091`) signal can
become stale after an apparent two-second gap even though the available timing
evidence shows roughly 100 ms traffic. It is an evidence-gathering procedure;
it does not change freshness policy or claim that the issue is resolved.

The production acquisition, scheduling, decode, publication, and fail-off
paths remain the paths under test. The diagnostic variant only adds bounded,
opt-in snapshots and a low-rate serial logger. Keep the diagnostic option off
for normal vehicle images. Hardware reproduction and final failure
classification are still pending.

## Before reproducing

Read the [CAN acquisition specification](../specs/can/acquisition.md), the
[signal evidence record](../protocol/signal-evidence.md), and the
[vehicle-data policy](license-and-vehicle-data.md). The reviewed DBC has no
cycle-time declaration, so the approximately 100 ms `0x091` cadence is an
observation used to guide investigation, not a new protocol or freshness
contract. `0x091` D4 (the fourth payload byte, `data[3]`) low nibble is treated
as a wrapping counter for diagnostics only; it is not a timestamp and a
missing value can be ambiguous when a modulo-16 wrap occurs.

Keep the existing production freshness configuration unchanged during a
reproduction. Do not relax the timeout, add a fallback, promote an unverified
reading to `Fresh`, or change the controller's fail-off behavior. Run one
controlled comparison with the diagnostic option disabled and one with it
enabled, using the same firmware inputs and scenario. A standalone replay may
exercise the recorder with reviewed synthetic frames, but it cannot substitute
for a production-path reproduction.

## Build the opt-in diagnostic variant

The option is disabled by default as `CONFIG_WEACT_CAN_FRESHNESS_DEBUG=n`.
From the repository root, activate ESP-IDF 5.5.4 and check the toolchain as
described in [firmware builds](firmware-build.md):

```sh
source /path/to/esp-idf/v5.5.4/export.sh
python3 tools/check_toolchain.py --scope firmware
cd firmware/weact-can485-v1.1
idf.py set-target esp32
idf.py menuconfig
```

In `WeAct CAN485 V1.1 diagnostics`, enable `CONFIG_WEACT_CAN_FRESHNESS_DEBUG`.
Then build the diagnostic image:

```sh
idf.py reconfigure
idf.py build
```

The portable boundary is the value-only `mazda::DebugSnapshot` returned by
`VehicleTelemetry::debug_snapshot()`. The firmware logger polls that snapshot;
when `CONFIG_WEACT_CAN_FRESHNESS_DEBUG` is disabled, the recorder is inactive
and the snapshot is zeroed. This keeps instrumentation out of the receive,
decode, and publication control flow.

The image keeps `TWAI_MODE_LISTEN_ONLY` and a zero-length transmit queue. Do
not add a raw frame logger, diagnostic polling, or a transmit path to make the
run easier. Serial output is bounded to periodic summaries and at most one
record per second for fresh-to-stale transitions. The transition count remains
complete; the logger records coalesced and rate-suppressed events. Logging must
not run in the CAN receive task.

The host recorder has a separate `MAZDA_ENABLE_DEBUG_TELEMETRY` CMake option,
also disabled by default. The host option exercises the same value recorder
and synthetic service path without enabling the ESP logger or claiming an ESP
scheduling result. `DebugSampleSource::TelemetryObserverFrame` and
`TelemetryObserverTimeout` identify the runtime observer callback that supplied
the diagnostic sample timestamp. A freshness expiry check can publish a stale
snapshot later, but it retains that observer sample timestamp and source; the
snapshot's `now_us` records when the expiry check ran. The retained
`transport_last_frame_age_us` watermark is transport evidence and is not used
as the observer callback timestamp.

For issue #215, the paired Release comparison used the same source tree,
AppleClang 17, C++17, `-O3 -DNDEBUG`, `-Wall -Wextra -Wpedantic`, pinned
generic core `f30764b` (release `0.1.0`), `MAZDA_ENABLE_TELEMETRY_PROFILING=ON`,
and the same synthetic baseline runner. Only
`MAZDA_ENABLE_DEBUG_TELEMETRY` changed. Each build produced 110 reports and
passed 5,538 deterministic assertions. The candidate source was the
`codex/gh-215-bound-freshness-recording` worktree at base `bbe6962` with the
issue #215 working-tree changes. The reports record
`build_revision=working-tree` and `source_dirty=true`, so they do not use
unknown build provenance:

| Build | Supported mix wall range (stage timers) | Supported mix CPU range (counters only) | Ready-unrelated wall range (stage timers) |
| --- | ---: | ---: | ---: |
| Recorder off | 166.8–175.9 ms | 5.942–10.719 ms | 0.345–0.973 ms |
| Recorder on | 165.0–172.1 ms | 4.226–9.689 ms | 0.156–0.947 ms |

Ranges are min/max across the clean rerun's repeated reports (15
supported-mix reports and 10 ready-unrelated reports per build). Both clean
rerun supported windows are inside the existing provisional 280 ms host
engineering budget; the result is software evidence for this source/compiler
and synthetic mix only. The first refreshed run is retained separately in the
same temporary report directory: its recorder-on supported-mix wall maximums
were 564.8 ms in counters-only mode and 364.2 ms in stage-timers mode, with a
500 ms receive-wait excursion. That host scheduling outlier exceeded the
provisional budget and is why the rerun evidence is described as controlled,
not as a hard real-time guarantee. The stage profiler is a separate host
measurement mode and must not be described as the recorder's serial logging
cost. The recorder publishes frame evidence at the frame boundary and
refreshes aggregate counters at most once per five seconds when traffic
continues or the bus is silent; unrelated frames therefore do not create a
per-frame logging path.

Before any physical run, confirm the board and wiring against the
[WeAct CAN485 V1.1 hardware record](../architecture/hardware/weact-can485-v1.1.md).
That means checking the actual board revision, CAN H/CAN L/ground and
protected power wiring, K3 termination, fuse and power source, the USB serial
port, and GPIO startup levels. The build proves software configuration only.
No hardware interaction is authorized by this runbook; bench or road work
requires explicit task scope and an independent safety check.

## Capture and privacy

The logger emits a stable, one-line `key=value` record. Save live serial logs
only in an ignored local directory. Private logs may retain the bounded
relative timing needed to diagnose the incident, but treat them as sensitive
because a long timing series can reconstruct a trip. Do not commit or attach
live logs. Public examples must use synthetic values and a relative timebase,
with no raw payloads, VIN, device identifiers, credentials, precise location,
absolute time, or reconstructable trip sequence. The policy for captures,
fixtures, issue attachments, and screenshots is in
[license and vehicle data](license-and-vehicle-data.md).

The logger emits:

- `event=summary`, periodically, with aggregate counters and the latest
  transport/signal snapshot;
- `event=stale`, at most once per second while preserving the complete
  Fresh-to-Stale transition count. If several transitions arrive between
  polls, the logger coalesces them and reports `stale_events_coalesced`; if
  the one-second output limit suppresses a transition, it reports
  `stale_events_suppressed`. Recovery and clock comparisons are retained in
  the next summary; they are not separate event types.

The logger task polls the telemetry snapshot every 100 ms, emits summaries
every five seconds, and samples read-only TWAI status at the poll. A
`twai_status_sample_ts_us` value therefore describes the logger's sample time,
not the arrival time of a CAN frame. If either snapshot read contends, the
logger retries on the next poll and does not advance stale-event accounting.
`snapshot_read_drops` is the recorder's aggregate count, including failed
stale-cache reads; `logger_snapshot_read_drops` counts current- and
stale-cache misses observed by the logger itself, while
`recorder_write_contention` counts a non-blocking worker-to-cache handoff miss.
The worker retains the current and immutable stale evidence and retries the
handoff, so a held reader cannot lose a stale transition, recovery, or counter
continuity. `recorder_publications` counts recorder updates, including
aggregate refreshes, rather than serial lines.

Do not infer a fault from one counter gap alone. Compare the counter, arrival
interval, processing latency, state-update timestamp, availability, and
transport counters in the same interval.

## Fields to retain

For `0x091`, retain these fields when the diagnostic snapshot provides them:

| Group | Fields | Use |
| --- | --- | --- |
| Counter continuity | `ctr`, `prev_ctr`, `expected_ctr`, `ctr_ok`, `counter_gap_count` | Detect a sequence discontinuity. Compare modulo 16; a wrap from `0xf` to `0x0` is valid. `counter_gap_count` counts discontinuity events, not the number of skipped frames. A counter can show at most a modulo-16 observation, so it cannot prove how many frames were lost after multiple wraps. |
| Arrival | `frame_count`, `prev_frame_ts_us`, `frame_ts_us`, `interarrival_us` | `frame_count` counts processed `0x091` observations, not raw per-ID receives. Distinguish ECU silence or loss before timestamping from later processing delay. |
| Processing | `process_ts_us`, `rx_process_latency_us` | Detect a received frame waiting in the queue or behind a starved task. |
| Publication | `publish_ts_us`, `process_publish_latency_us` | Detect delay after decode and before publication. |
| Semantic state | `signal_last_update_us`, `now_us`, `signal_age_us`, `freshness_timeout_us`, `availability`, `process_status`, `update_not_advanced`, `update_not_advanced_count`, `stale_transition_count` | Establish whether the state accepted a newer observation and whether the stale result follows the configured arithmetic. `update_not_advanced` means the signal's `last_update_us` watermark did not advance for that processed frame; it includes malformed, ignored, and equal-time observations and is not proof that the decoder rejected the frame. Use `process_status` to distinguish those categories. |
| Clocks | `esp_timer_us`, `steady_clock_us`, `clock_delta_us` | Check the integration's assumption that both monotonic microsecond clocks share a usable epoch and do not drift materially. |
| Acquisition/runtime | `frames_received`, `frames_processed`, `frames_dropped`, `ring_overflow`, `twai_rx_missed`, `twai_rx_overrun`, `driver_errors`, `bus_off_events`, `controller_resets`, `transport_health`, `transport_last_frame_age_us` | Separate driver loss, application-ring loss, runtime failure, and signal-specific silence. |

The acquisition counters have defined limits. `frames_dropped` and
`ring_overflow` describe the bounded application ring; `core_missed_frames`
is the pinned core's aggregate loss count; and `twai_rx_missed` plus
`twai_rx_overrun` are raw driver status values sampled by the logger. A zero
count does not prove that no frame was ever lost outside those counters.
`transport_last_frame_age_us` describes the latest received CAN frame, not
specifically `0x091`; it is therefore useful for separating an ID-specific
problem from a bus-wide stall. Snapshot values are aggregates and may be
sampled between events. The logger also records `twai_status_valid`, the
status error, sample timestamp, controller state, RX/TX queue counts, error
counters, arbitration loss, and bus-error count. Those fields describe a
read-only logger-time status sample and may be unavailable if the driver
status call fails; they are not per-frame loss proof.

`frame_ts_us` is captured from `esp_timer_get_time()` in the receive path.
Freshness evaluation uses the injected monotonic `SteadyClock::now()` seam.
Both are expected to be monotonic on ESP32, but this runbook requires the
observed delta to be checked rather than assumed. A stable offset is an epoch
contract question; a changing or backwards delta is evidence of a clock
integration defect. A host synthetic test's fake clock is software evidence,
not ESP32 clock validation.

## Classify the first anomaly

Use the first stale transition and the surrounding `summary` records to assign
one provisional class. Keep the classification provisional until repeated
evidence agrees.

### A. Acquisition loss or bus-wide stall

Evidence includes a large `interarrival_us`, a modulo-16 counter gap, rising
driver/ring loss counters, or a large `transport_last_frame_age_us`. If other
IDs such as `0x09A` also stop, investigate the receive task, TWAI status, bus
state, power, and wiring. If only `0x091` stops while transport and `0x09A`
remain healthy, keep the investigation ID-specific. Do not call this a
freshness-policy defect.

### B. Processing backlog

If `interarrival_us` remains near the observed cadence and the counter is
continuous, but `rx_process_latency_us` is hundreds of milliseconds or
seconds, the frame arrived on time and waited before processing. Check queue
depth/overflow, task starvation, dispatcher blocking, and logger overhead.

### C. Decoder or state-update rejection

If a recent frame reaches processing but `signal_last_update_us` does not
advance, inspect frame length/format validation, decoder outcome, timestamp
ordering, equal or older timestamps, and state-update behavior. The
`update_not_advanced` field records that watermark result; it includes
malformed, ignored, and equal-time observations and does not by itself prove
decoder rejection. Use `process_status` for the category. A modulo-16 counter
gap cannot by itself distinguish a dropped frame from an invalid frame that
was received; correlate it with processing and acquisition fields.

### D. Clock-domain mismatch

If `esp_timer_us` and `steady_clock_us` have a large, changing, or backwards
`clock_delta_us`, verify the timestamp source and clock conversion at the
Mazda integration boundary. Confirm that `signal_last_update_us` and `now_us`
are compared in the same unit and epoch. Do not fix a clock mismatch by
changing the freshness timeout.

### E. Freshness/status evaluation defect

If the newest accepted frame is recent, `signal_age_us` is below
`freshness_timeout_us`, and transport and clocks are sane, but `availability`
is still `Stale`, preserve the logs and inspect the generic freshness/status
path. This is the only class that directly supports a `vehicle_core` freshness
defect. If `signal_age_us` is above the configured timeout, the stale result is
consistent with the current policy even if the cause of the old timestamp is
still unknown.

Decoder rejection and modulo-16 ambiguity must remain explicit in the report;
they are not silently folded into A or E. If evidence spans more than one
class, report the earliest supported boundary and list later symptoms
separately.

## Synthetic result example

This example is sanitized and relative. It illustrates how to report a
classification without publishing a raw frame or vehicle log:

```text
CANDBG v=1 event=stale id=0x091 ctr=9 prev_ctr=8 expected_ctr=9 ctr_ok=1 interarrival_us=100012 rx_process_latency_us=2100000 process_publish_latency_us=70 signal_age_us=2100289 freshness_timeout_us=2000000 availability=stale transport_health=live transport_last_frame_age_us=1200 twai_rx_missed=0 twai_rx_overrun=0 ring_overflow=0 frames_dropped=0 classification=B
```

Here the synthetic counter is continuous and transport remains live, while a
2.1 s processing latency explains the stale observation. It is an example of
software interpretation only, not an incident result.

## Report and stopping conditions

For each run, record the firmware source revision, whether the option was
enabled, configured timeout as read from the image, relative test duration,
event lines, provisional class, and which counters changed. Keep private live
logs local; publish only sanitized summaries or synthetic examples.

Stop and report the run if the board identity, wiring, fuse, protected power,
termination, or actual serial port cannot be verified, if the logger affects
receive timing, or if any active-CAN/ACK behavior is observed. Do not flash or
connect to a vehicle as part of ordinary software validation. Hardware
reproduction, electrical validation, and final classification for #208 remain
pending; this runbook does not claim that the issue has been resolved.

## Related procedures

- [Firmware builds](firmware-build.md)
- [CAN acquisition](../specs/can/acquisition.md)
- [Vehicle CAN receive-only boundary](../architecture/receive-only-boundary.md)
- [WeAct CAN485 V1.1 hardware record](../architecture/hardware/weact-can485-v1.1.md)
- [Mazda signal evidence](../protocol/signal-evidence.md)
- [License and vehicle-data policy](license-and-vehicle-data.md)
