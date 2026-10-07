# Host telemetry performance baseline

Issue #210 uses a small synthetic host fixture to measure the production
composition path. Its FIFO scenarios drive `HostRuntimeSource`, and its
unrelated scenario uses a bounded continuously-ready source wrapper; both
enter the pinned generic `vehicle_telemetry::Runtime`,
`mazda::VehicleTelemetryService`, the Mazda decoder and publication store,
`MazdaSignalProvider`,
`action_engine::ActionEngine`, and `local_argb_actions::LedActionSink`. Its
final sink is an in-memory value-only lighting sink. No decoder microbenchmark
or retired capture format is involved.

The fixture covers an 8 ms silent source, a 256-frame preloaded ready backlog of
unrelated IDs, a 256-frame supported mix, 128 malformed owned frames, and a
finite 512-frame burst. The ready backlog keeps the source continuously ready
until its bounded input is exhausted. The supported mix alternates turn-left,
turn-right, and engine frames with a 2,000 frame/s submission target, then
waits for publication and drains the manual notification dispatcher after each
frame so the ActionEngine and LED sink observe real traffic. The resulting
accepted rate is reported separately because service and dispatch time are part
of the measured window. The malformed and ready-backlog scenarios drain at the
end. The burst parks the Runtime worker after its first publication, fills
the 64-frame host FIFO, and verifies drop-newest accounting. Every frame is
synthetic and uses a monotonic injected timestamp; no vehicle capture or
identifier is included.

## Focused release run

Use a fresh out-of-tree build and the exact pinned generic-core checkout. The
runner records the source revision, compiler string, build mode, raw reports,
and min/median/max repeat ranges:

```sh
python3 tools/run_telemetry_baseline.py \
  --core-source-dir /path/to/pinned/vehicle-can-core-0.1.0 \
  --build-dir /tmp/mazda-gh210-baseline-release \
  --repeats 5 \
  --output /tmp/mazda-gh210-baseline-release/report.txt
```

The CTest accounting checks can be run directly after configuring the focused
build:

```sh
ctest --test-dir /tmp/mazda-gh210-baseline-release \
  -R 'synthetic' --output-on-failure
```

The executable runs each scenario twice and compares deterministic counts.
The release runner repeats the executable so host scheduling variance is
visible in the report. Timing values are evidence for this host revision and
compiler only; they are not pass/fail thresholds.

## Measurements and interpretation

The report separates receive wait, enqueue-to-process and dequeue-to-process
wall time, decoder, diagnostics, publication/state copies, notification
evaluation, notification dispatch, ActionEngine-to-LED action-sink time, and
typed-observer callback delivery. It records source injection attempts,
accepted/dequeued frames, queue depth, drop/overflow deltas,
per-identifier ignored/decoded/malformed/fault outcomes, notification
evaluation counts, callbacks, and LED sink publishes. `queue_depth_mode=exact`
applies to the preloaded 256-frame ready backlog and the parked FIFO burst.
Other scenarios label the fixture's concurrent pending-frame high-water as
`shadow-pending`; it is an acquisition backlog indicator and is not presented
as a synchronized driver queue read.
The report records both the synthetic timestamp horizon and the wall schedule;
the ready backlog and burst are immediate input, while the paced streams use
500 microseconds between host submissions.

`profiler=stage-timers` enables bounded host aggregate callbacks and their
`steady_clock` measurements. `profiler=counters-only` keeps the same accounting
callbacks while suppressing service-stage timer reads. Receive wait,
enqueue/dequeue wall timings, action-sink timing, and typed callback delivery
timing remain host measurements in both modes, so the comparison is not a total
profiler-overhead measurement. This mode does not measure the ESP serial logger
from issue #208 and must not be described as an ESP logging result. The
injected queue-age value uses the same synthetic `MonotonicTimestamp` domain as
each frame; it does not correct a host clock to an ESP clock. Controller clock
alignment remains a separate #211 concern, and no ESP scheduling result is
claimed.

Use the repeated host ranges to state an engineering load envelope in terms of
the tested synthetic traffic mix, accepted frame count, queue capacity, and
observed host stage totals. Keep any budget explicitly host-only and tied to
the recorded build revision and compiler settings. Do not promote it to a
firmware, bench, or vehicle budget.

The `wall_ns` window starts after the service and action composition has been
constructed and ends before stop/detach. Aggregate stage and callback counters
remain active during construction and teardown, so they include lifecycle
startup/stop diagnostics and callbacks even though those operations are outside
the wall window. Keep the stage values as named measurements rather than adding
them: diagnostics contains publication and notification evaluation work, and
notification dispatch is an inclusive callback traversal measurement.

The recorded five-repeat Release run on 2026-10-07 used revision `a707429`,
`source_dirty=true`, AppleClang 17, `-O3 -DNDEBUG`, C++17,
`-Wall -Wextra -Wpedantic`, BLE off, and the subscription graph shown in each
report. It produced 110 reports and passed 5,518 deterministic accounting
assertions. The supported mix accepted all 256 frames with no drops; its
15-sample wall range was 193.62–258.69 ms for stage timers and
193.23–251.83 ms for counters-only. The 128-frame malformed stream ranged
from 96.27–126.16 ms with stage timers and 96.26–109.72 ms with counters-only.
The parked FIFO burst accepted 65 of 512 frames, dropped 447, and measured an
exact depth of 64; its stage-timer wall range was 5.610–7.873 ms, including the
deliberate 5 ms timeout-guard regression. The accepted supported-frame rate was
approximately 989–1,324 frames/s across the two modes and repeats, below the
submission target because publication and manual-dispatch waits are included.
The ready-backlog source reported its exact preloaded depth of 256 with no
drops. These values support a provisional 280 ms host fixture budget for the
256-frame supported window and a 64-frame FIFO capacity check. The wide host
variance is retained as evidence rather than hidden by a CTest threshold; these remain
host results and claims about ESP task scheduling are excluded.

## Authorized bench follow-up

A hardware follow-up requires separate written authorization and a coordinated
review before any wiring, flashing, or vehicle connection. The proposed plan,
after authorization, is to use the normal receive-only CAN source with
`TWAI_MODE_LISTEN_ONLY`, a zero-length transmit queue, and verified board
power, CAN wiring, and fuse state. Record the exact firmware revision, ESP-IDF
version, target, CPU/task configuration, idle time, receive backlog high-water,
drops, and callback latency over the same finite synthetic mix. Repeat with
production diagnostics disabled and with the separately reviewed [#208
production recorder/logger modes](https://github.com/Yuke-hd/mazda-can-accessory-controller/issues/208)
enabled separately; record ESP CPU/idle/backlog measurements and label serial
output cost as hardware evidence rather than this host result. Store only
reviewed aggregate measurements and no raw vehicle frames or trip data.

The [coordinated generic-core #132 baseline](https://github.com/Yuke-hd/esp32-vehicle-can-core/issues/132)
is a separate measurement. Run it only with the generic-core maintainers'
authorization and coordinate its revision, build flags, traffic mix, and
inclusive cost with this controller baseline. Rebaseline any host or bench
envelope after the separately reviewed #211 clock-alignment change; treat the
removed offset correction as a measurement correction, not as a processing
speedup. This task performs no bench wiring, flashing, vehicle test, or BLE
workload; the report records `ble=off` because no BLE task is composed into the
host fixture.
