# MCAN-67 timestamped pixel-frame output

The host replay CLI can expose the production renderer's deduplicated writes
as a machine-readable JSON-lines stream:

```text
gvret-replay render <capture.csv> --bus <number> --end-us <number>
```

The replay horizon is required and inclusive. The optional cadence settings
are bounded by the scheduler and default to the production host model:

```text
--availability-us <number>  default 10000
--output-tick-us <number>   default 10000
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
(for example decoded signal records, #94) can be added without a version bump.
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
source order, end of stream, availability timeout, polled-rule sample, and
renderer tick. No wall-clock sleep or playback pacing is used. The output
sink can also be used directly by host tests to retain timestamped
`PixelFrame` values in memory.

The representative scenario in
`tests/host/gvret_replay_output_tests.cpp` uses synthetic frames only. It
covers startup and low/half/full RPM fill, the red-zone overlay, mirrored
left/right turns, hazard, turn ownership and priority, return to baseline,
stale turn fail-off, and byte-identical repeated JSONL output. These tests are
software evidence and do not establish bench or vehicle behavior.
