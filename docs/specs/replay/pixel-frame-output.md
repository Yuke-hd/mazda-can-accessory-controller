# Timestamped pixel-frame output

The host replay CLI can expose the production renderer's deduplicated writes
as a machine-readable JSON-lines stream:

```text
can-replay render <capture.csv> --bus <number> --end-us <number>
```

The replay horizon is required and inclusive. The optional cadence settings
are bounded by the scheduler and default to the production host model:

```text
--availability-us <number>  default 10000
--output-tick-us <number>   default: the output stage tick period (10000)
--poll-us <number>          default 100000
```

The scheduler's two-million-event guard bounds scheduled work, not the byte
size of the JSONL stream. Long horizons and short render periods can therefore
produce very large output; choose `--end-us` and the cadence options with the
expected stream volume in mind. The JSONL sink writes one complete record at a
time, while the in-memory test sink retains every frame.

The stream is self-describing. The first line is a header record declaring the
format version and the pixel count; every later line is one record with a
`type` field. Pixel records carry a relative replay timestamp and exactly
`pixel_count` RGB triplets. A successful replay ends with an `end` record;
consumers should treat a stream without that marker as truncated:

```json
{"type":"header","version":1,"pixel_count":100}
{"type":"pixels","timestamp_us":0,"pixels":[[0,0,0], ... ]}
{"type":"end"}
```

Consumers must ignore record types they do not recognize, so new record types
(for example the `signal` records described in
[replay signal observers](signal-observers.md)) can be added without a version bump.
The version changes only when an existing record's meaning or shape changes.

The timestamp is supplied by the caller-owned replay clock after GVRET
timestamps have been normalized to zero. The stream contains no CAN identifier,
payload, source timestamp, vehicle identifier, or input filename. Renderer
deduplication means unchanged frames are omitted; startup and final fail-off
black writes remain observable when they change the physical frame. The RGB
sequence is nevertheless derived vehicle telemetry: real-capture output can
reconstruct RPM bands and turn or hazard timing. Treat it as reconstructable
trip data under the repository's privacy and publication rules. Synthetic
fixtures are the only output safe to publish.

The scheduler processes equal-time events in this order: all CAN frames in
source order, end of stream, availability timeout, polled-rule sample, signal
sample (only when signal observers are injected; see
[replay signal observers](signal-observers.md)), and output-stage
tick. No wall-clock sleep or playback pacing is used. The output
sink can also be used directly by host tests to retain timestamped
`PixelFrame` values in memory.

## Output stage

The replay controller and scheduler do not name the local ARGB renderer. They
drive an injected `replay::OutputStage` (`replay/output_stage.hpp`) through a
fixed lifecycle: `configure` registers the stage's `ActionSink`s and rules on
the controller's `ActionEngine`, then `start`, zero or more `tick(now)` calls,
`fail_off(now)` on a failed start, a failed tick, or a replay fault, and a
final `fail_off(now)` then `stop(now)`, including when startup is abandoned.
`fail_off` owns the blackout guarantee, so every stop leaves the output
inactive; `stop` releases any resource the stage acquired. Actions reach the
stage only through `action_engine::ActionSink`. When `--output-tick-us` is
omitted, the scheduler uses the stage's `tick_period_us()`.

`replay::LocalArgbOutputStage` (`replay/local_argb_stage.hpp`) is the only
stage that borrows the controller's replay clock and wires the default
lighting profile, `LedActionSink`, and the
private renderer. The CLI composes it with the JSONL pixel sink; host tests
compose it with the in-memory timestamped sink. A fake stage in
`tests/host/replay_output_stage_tests.cpp` checks the lifecycle sequence
without any local ARGB type.

The representative scenario in
`tests/host/replay_output_tests.cpp` uses synthetic frames only. It
covers startup and low/half/full RPM fill, the red-zone overlay, mirrored
left/right turns, hazard, turn ownership and priority, return to baseline,
stale turn fail-off, and byte-identical repeated JSONL output. These tests are
software evidence and do not establish bench or vehicle behavior.
