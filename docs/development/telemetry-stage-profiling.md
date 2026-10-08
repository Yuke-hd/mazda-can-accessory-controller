# Firmware telemetry stage profiling

Issue #226 adds an opt-in ESP-IDF profiler for investigating the wall-clock
cost of the synchronous telemetry worker. `CONFIG_WEACT_CAN_TELEMETRY_PROFILING`
defaults to `n`; production images keep the profiler accumulator, timing reads,
and logger task out of the firmware path. The option is independent of
`CONFIG_WEACT_CAN_FRESHNESS_DEBUG`.

## Scope and stage boundaries

The profiler uses fixed storage: one 32-bucket logarithmic histogram and
bounded aggregate counters for each stage. It performs no allocation and emits
no output per frame. A low-priority task polls completed snapshots and emits at
most one compact record per completed five-second interval. Measurements are
wall-clock elapsed microseconds. Task preemption and other scheduling can
inflate an individual sample; the values are not task CPU time.

The stages are measured as disjoint spans on the frame path:

| Stage | Boundary |
| --- | --- |
| `queue_wait` | `RawCanFrame::timestamp_us` to the start of `process()`; this is separate from receive and processing work. |
| `pre_decode_due` | Service of due freshness notifications before decoding. |
| `decode` | Mazda decoder call and result mapping. |
| `diagnostics` | End of `process()` through the Runtime diagnostics snapshot and observer handoff, ending before publication. A timeout-only diagnostics callback is measured from callback entry. |
| `publication` | State or diagnostics publication and its associated publication-store work. |
| `notification` | Post-frame notification descriptor evaluation, excluding lighting. The pre-decode due pass is included in `pre_decode_due` and is not counted here. |
| `lighting` | Synchronous lighting evaluation after notification evaluation. |
| `total` | End-to-end synchronous frame work from `process()` entry through publication, notifications, and synchronous lighting. Queue wait is excluded. |

Ignored and malformed frames still count the stages that actually execute.
The profiler does not alter decoder, freshness, publication, notification, or
lighting behavior.

## Serial format

Each completed interval has one `TELPROF v=1` record. Every stage value is a
slash-separated tuple whose fields are named by the record's
`metric_fields=calls,total_us,avg_us,p50_us,p95_us,p99_us,max_us` token. The
percentiles are approximate upper bounds from the fixed logarithmic histogram.
For example, `decode=12/840/70/64/128/128/256` means 12 calls, 840 total
microseconds, a 70 microsecond integer average, and the reported percentile
and maximum bounds.

The interval is published only after its five-second boundary. The logger
deduplicates interval end timestamps, so a slow logger task cannot repeat a
record. It runs at low priority and does not participate in the receive,
decode, publication, notification, or LED tasks.

## Validation and limits

The host `mazda_telemetry_profiling_tests` target covers deterministic bucket
quantiles, sample-count scaling, and interval rollover. A host composition
build can additionally set `-DMAZDA_ENABLE_TELEMETRY_STAGE_PROFILING=ON` to
compile the same fixed accumulator into the service and run deterministic
processed, ignored, malformed, and timeout path checks. Firmware builds with
the option off and on verify that the production and diagnostic compositions
both compile. These checks are software evidence only. A board, CAN bus, and
vehicle workload are required to measure actual ESP scheduling or vehicle
traffic; this feature does not claim hardware or vehicle validation.

The profiler reports bounded wall-clock distributions, not per-stage CPU
attribution, asynchronous notification dispatch latency, or a complete
firmware overhead benchmark. Compare equivalent synthetic workloads on the
target when making a device-specific budget.

## Host overhead evidence

For a software-only overhead check, the existing deterministic baseline was
run once with the host stage option off and once on, using the same Debug
build flags and `--skip-build --repeats 1` runner. The table shows the
`counters-only` medians from the two runs; the host scheduler makes these
measurements noisy.

| Synthetic scenario | Stage off wall ms | Stage on wall ms | Stage off CPU ms | Stage on CPU ms |
| --- | ---: | ---: | ---: | ---: |
| Supported mix | 178.306 | 183.969 | 14.656 | 20.081 |
| Malformed | 81.621 | 82.043 | 3.001 | 3.469 |
| Finite FIFO burst | 6.643 | 6.395 | 0.593 | 1.039 |

This is host scheduling evidence for the added fixed timing reads and does not
substitute for the required identical synthetic workload on an ESP target.
That device comparison, including target-specific CPU and task-runtime
effects, remains deferred until a board or an ESP execution harness is
available.
