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

The profiling fixture also verifies descriptor ownership directly. On startup,
the released notification catalog evaluates gear twice, doors six times,
0x091 five times, 0x09A three times, and brake once; unowned, RPM, and
acceleration identifiers evaluate zero descriptors. Subsequent frames add only
their owning group. These counts exclude the host-only descriptor extension and
are deterministic evaluator counts rather than callback counts, which may be
suppressed when a channel receives an equal reading.

`process_cpu_ns` uses `std::clock` around the same scenario window as
`wall_ns`. It is whole-process CPU time across the fixture, Runtime worker,
manual notification dispatch, callbacks, and enabled profiling hooks. It is
not worker-only CPU time or an ESP measurement. Construction and teardown are
outside this window. The policy, worker-publication-lock, and private-lighting
evaluation counts cover their explicitly instrumented service operations;
they do not count polling reads or lifecycle store locks.

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

## Issue #212 routing comparison

The five-repeat Release comparison for #212 used baseline revision `9888241`
and the candidate working tree containing the decoder-routing change. Both
builds used the same pinned generic-core checkout, AppleClang 17, C++17,
`-O3 -DNDEBUG`, `-Wall -Wextra -Wpedantic`, BLE off, and the same synthetic
fixture and subscriptions. `MAZDA_ENABLE_DECODER_CALL_PROBING=OFF` was used
for both timing builds so the measurement does not include probe overhead. The
runner produced 110 reports per build; the stage-timer rows recorded these
decoder ranges as min/median/max nanoseconds across repeats:

| Scenario | Baseline `9888241` | Candidate |
| --- | ---: | ---: |
| Supported mix | 45,120 / 109,753 / 121,215 | 53,168 / 73,411 / 99,368 |
| Malformed owned | 26,831 / 39,026.5 / 57,114 | 13,210 / 22,376 / 26,918 |
| Ready unowned | 23,336 / 28,565 / 57,073 | 6,754 / 7,900 / 10,213 |
| Finite FIFO burst | 8,255 / 18,081 / 19,918 | 2,623 / 4,207 / 8,340 |

The runner used `--skip-build`, so its `build_revision` and `source_dirty`
fields were `unknown`. The baseline archive was nevertheless fixed at
`9888241`, and the candidate was the working tree containing this change; the
compiler, flags, pinned core checkout, and probe setting above describe both
builds. Wall windows remain host scheduling evidence; the overlapping ranges do
not support a wall-time speedup claim. The opt-in call-count build separately
recorded exactly one owner call for each supported standard ID, zero calls for
valid unowned and non-acceleration extended/remote frames, and one engine call
for an invalid non-acceleration frame. The normal timing build had probing
disabled.

The `wall_ns` window starts after the service and action composition has been
constructed and ends before stop/detach. Aggregate stage and callback counters
remain active during construction and teardown, so they include lifecycle
startup/stop diagnostics and callbacks even though those operations are outside
the wall window. Keep the stage values as named measurements rather than adding
them: diagnostics contains publication and post-frame notification evaluation
work, while pre-observation due evaluation runs before decode and is counted in
the aggregate evaluator total. Notification dispatch is an inclusive callback
traversal measurement.

## Issue #213 publication comparison

The five-repeat Release comparison for #213 used baseline revision `47e5ee9`
and the candidate working tree. Both builds used the same pinned generic-core
checkout (`f30764b`, release `0.1.0`), AppleClang 17, C++17, `-O3 -DNDEBUG`,
`-Wall -Wextra -Wpedantic`, BLE off, and the same fixture inputs. The detached
baseline had one measurement-only patch adding the candidate's `std::clock`
field and runner summary; it contained no telemetry service change. Each build
produced 110 reports and passed its complete deterministic accounting set
(5,520 baseline assertions and 5,529 candidate assertions).

The state-copy counts cover normal `publish_current` work and do not include
reset assignments. The candidate copies a full `VehicleState` only after a
signal or message-health mutation. Ignored frames retain Runtime
receive/accounting recovery through diagnostics-only publication. A
scheduling-dependent idle diagnostics pass can add one normal publication;
the table retains that variation instead of presenting it as frame work.

| Scenario | Frames | Baseline state copies | Candidate state copies |
| --- | ---: | ---: | ---: |
| Supported mix | 256 decoded | 514–516 | 256 |
| Ready unowned | 256 ignored | 514 | 0 |
| Malformed owned | 128 malformed | 258–260 | 128 |

The ready-unowned run recorded exactly 257 normal worker publications,
including startup. Both builds recorded 257–258 for supported traffic because
of an optional idle pass. Baseline malformed traffic recorded 129–130
publications; the candidate recorded 129. The baseline took two
publication-store mutexes per publication: one full-state publish and one
snapshot readback. The candidate directly counted one worker publication lock
per publication: 257 ready-unowned, 257–258 supported, and 129 malformed. Its
policy counter recorded five
construction, configuration, and lifecycle applications per scenario. The
baseline also reapplied the policy in every full-state publication: 257
hot-path applications for ready-unowned traffic, 257–258 for supported
traffic, and 129–130 for malformed traffic, in addition to its lifecycle
applications. The fixture supplies an explicit private lighting sink, so both
versions still evaluate that bound sink per publication. The production
facade's unbound private sink now exits before evaluation; this benchmark
deliberately does not count that separate path.

The immediately paired rerun recorded these stage-timer publication totals
and counters-only whole-process CPU ranges as min/median/max milliseconds:

| Scenario | Publication baseline | Publication candidate | CPU baseline | CPU candidate |
| --- | ---: | ---: | ---: | ---: |
| Supported mix | 0.050 / 0.116 / 0.136 | 0.039 / 0.103 / 0.125 | 7.188 / 7.779 / 8.423 | 6.548 / 7.541 / 9.264 |
| Ready unowned | 0.022 / 0.027 / 0.058 | 0.009 / 0.013 / 0.021 | 0.296 / 0.611 / 1.506 | 0.284 / 0.566 / 1.260 |
| Malformed owned | 0.030 / 0.060 / 0.067 | 0.038 / 0.052 / 0.061 | 1.879 / 2.701 / 3.074 | 1.978 / 2.589 / 2.909 |

The overlapping host ranges support the deterministic copy, lock, and policy
reductions, but they do not establish a CPU speedup. Stage timers are nested:
their sums are not CPU time. Enabling stage timers also changes the profiling
work, so the CPU column uses the counters-only run. These figures are host
software evidence only; they make no firmware scheduling, bench, or vehicle
claim.

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
