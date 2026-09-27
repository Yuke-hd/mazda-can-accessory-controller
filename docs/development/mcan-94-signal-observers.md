# Replay Signal Observers

Host replay can report the decoded vehicle signals it produces, alongside the
output stage, so a replay can be inspected at the signal layer instead of only
through rendered pixels. This is a host-only facility; it adds no dashboard, no
network transport, and nothing to firmware.

## Observer contract

`replay::SignalObserver` (`replay/signal_observer.hpp`) names only portable
`vehicle_signals` types:

- `on_catalog(SignalCatalogView)` is called exactly once per replay, once the
  controller start has succeeded and before any reading. A start that fails
  (for example on an output fault) calls neither method.
- `on_reading(time_us, metadata, reading)` is called for each reading.
  `time_us` is the replay clock time at which the reading was produced.
  Readings are passed through unchanged, including `availability`,
  `freshness_timeout_us` and `validation`.

Zero or more observers are injected with `replay::SignalObservers` into
`ReplayController` or `run_replay`, next to the output stage. With no observer,
the controller subscribes to nothing extra and schedules no signal sample, so
pixel output is byte-identical to a replay without observer support. A null
observer is a configuration failure.

Signals reach observers by their catalog capability:

- **Notified signals** (`SignalCapability::Notify`, for example
  `vehicle.turn_state`) are delivered through `SignalProvider::subscribe`. The
  replay subscribes once for all observers, before telemetry starts, so the
  initial reading is produced at the start time; it is held until the start
  succeeds and then delivered, with that start time, right after the catalog.
  Every later notification arrives when the frame, the end of stream, or the
  availability timeout that caused it is processed.
- **Polled signals** (readable but not notified, for example
  `vehicle.engine_rpm` and `vehicle.speed_kph`) are read on a replay-time signal
  sample event, in catalog order. The cadence is
  `ReplayScheduleOptions::signal_sample_period_us` and defaults to the polled
  rule cadence (`poll_period_us`). The first sample is at time zero. A zero
  cadence is rejected as invalid options, with or without observers.

## Event order

Events with the same replay timestamp are processed in this fixed order:

1. all CAN frames at that timestamp, in source order;
2. end of stream;
3. availability timeout;
4. polled-rule sample;
5. signal sample (only when observers are injected);
6. output-stage tick.

A signal sample therefore sees every frame and freshness transition at its own
timestamp and the same state the polled rules just evaluated, and it runs before
the output stage renders that timestamp. Notified readings are delivered inside
steps 1 to 3 (frames, end of stream and availability timeout), so they precede
that timestamp's signal sample. Signal samples count towards the scheduler's event budget, so an
implausibly dense cadence is rejected as invalid options.

## JSONL signal records

`replay::JsonlSignalRecordWriter` (`replay/signal_record_output.hpp`) is an
observer that writes one record per reading:

```json
{"type":"signal","timestamp_us":0,"signal":"vehicle.engine_rpm","value":3250,"unit":"rpm","freshness":"unverified","availability":"freshness_unverified","validation":"confirmed"}
```

| Field          | Content                                                                                                  |
| -------------- | -------------------------------------------------------------------------------------------------------- |
| `timestamp_us` | Replay-relative time of the reading.                                                                     |
| `signal`       | Catalog key, JSON-escaped.                                                                               |
| `value`        | `true`/`false`, a shortest round-trip number, the enum choice key, or the raw number for an unknown enum value. `null` without a value or for a non-finite number. |
| `unit`         | `"rpm"`, `"km/h"` or `null`.                                                                             |
| `freshness`    | `"fresh"`, `"stale"` or `"unverified"`; `null` when the reading has no freshness (`no_data`, `unavailable`). |
| `availability` | `no_data`, `fresh`, `stale`, `freshness_unverified` or `unavailable`.                                    |
| `validation`   | `reference`, `observed` or `confirmed`.                                                                  |

Formatting is locale-independent, so repeated replays of the same input give
byte-identical records.

`can-replay render` writes these records into its pixel stream only when
`--signals` is given. `--signal-sample-us <number>` overrides the sample
cadence and is accepted only together with `--signals`. The records use the
existing stream version: consumers already ignore record types they do not
recognize. With `--signals`, the header is written only once the replay has started
successfully (the catalog marks that point), and signal and pixel records are
interleaved in the order they are produced. A rejected replay or a failed
controller start writes nothing from the signal path.

## Privacy

Signal records are decoded vehicle telemetry. From a real capture they expose
RPM, speed and turn timing more directly than the rendered pixels do. They
carry no CAN identifier, payload, source timestamp, vehicle identifier or
filename, but they are still reconstructable trip data. Publish only records
generated from synthetic fixtures.

## Tests

`tests/host/replay_signal_observer_tests.cpp` covers:

- the observer fan-out against a fake provider, including readings held until
  `open()` and dropped by an unopened `detach()`;
- subscription release on the real Mazda provider, and after a failed start or
  a `stop()`, with nothing delivered from a failed start;
- catalog-first delivery, and notified and polled readings, in a production
  replay;
- the default and configured sample cadence;
- the equal-time event order, including a notified record before the
  same-timestamp sample records;
- a polled signal turning `unavailable` without frames on the production path;
- invalid options, including a zero sample cadence without observers;
- byte-identical pixel output with and without observers;
- byte-identical signal records across runs;
- the exact record format.

`tests/host/can_replay_cli_tests.cpp` covers the CLI flags. These tests are
software evidence only.
