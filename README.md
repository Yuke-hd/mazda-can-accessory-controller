# Mazda CAN Accessory Controller

An ESP32 controller that passively listens to a Mazda's CAN bus, turns what the
car is doing into simple **actions**, and sends them to pluggable **outputs
(sinks)**. The built-in sink drives an addressable LED strip, and you can write
your own. An iOS companion app configures the rules over Bluetooth LE and shows
live signals.

> **Status: engineering starting point.** Host tests pass in software only.
> Nothing here has been validated as a complete installation on a vehicle,
> harness, or power supply. Bench and vehicle validation are required before
> any installation.

## What it does

1. **Read** decoded signals (turn, brake, RPM and more) from the car.
2. **Decide** with configurable rules: "when this signal matches, fire this action".
3. **Output** each action to every registered sink.

The factory profile ([`config/default.yaml`](config/default.yaml)) ships with the
LED strip sink and these rules:

| Car state | LED strip behaviour |
| --- | --- |
| Turn signal / hazard | Amber animation on the matching side, flowing out from the center |
| Brake pressed | Center region solid red |
| RPM above threshold (6000 by default) | Center region solid red |
| Anything unknown, stale, or faulty | **Everything off** |

Rules and thresholds live in a config profile, not in code. See the
[configuration spec](docs/specs/configuration/controller-config.md) and the
[lighting profile](docs/specs/configuration/lighting-profile.md).

## Bring your own sink

The rule engine knows nothing about LEDs. It emits generic action commands
(activate, deactivate, trigger, set level) to any number of sinks, and each sink
decides what an action id means and ignores the ones it does not handle. The LED
strip is just the first sink. To add another output, implement the
`ActionSink` port and register it with the engine. See the
[action engine spec](docs/specs/action-engine.md) and
[module boundaries](docs/architecture/module-boundaries.md).

## Companion app

An iOS companion app talks to the controller over Bluetooth LE to:

- upload and read back a controller config (rules and lighting profile)
- watch live decoded signals

The link cannot transmit on the CAN bus or control pixels directly, and the
lights keep working whether or not a phone is connected. See the
[companion BLE protocol](docs/specs/companion/ble-protocol.md),
[config transfer](docs/specs/companion/config-transfer.md), and
[live signals](docs/specs/companion/live-signals.md).

## Safety first

- **Listen-only.** The controller never transmits on the CAN bus: no frames, no
  diagnostics, no acknowledgements.
- **Fail dark.** If the controller is unsure about the car's state, the strip
  goes black rather than guessing.
- **Not a safety device.** It does not replace factory indicators or brake
  lamps. Do not rely on the LEDs for safety-critical signalling.
- **Brake timing is unverified.** The brake signal has no freshness timeout yet,
  so the red region can stay lit if brake frames stop while other CAN traffic
  continues. See [signal evidence](docs/protocol/signal-evidence.md).
- **Check the electrical side yourself.** Before connecting to a vehicle, verify
  CAN wiring and termination, use a fused and current-limited supply, and
  work out the strip's current draw. WS2812B strips may need level shifting and
  local capacitors.

## How it works

```text
CAN bus (listen-only) -> Mazda decoder -> signals -> rule engine -> actions -> sinks
                                                                     |-> LED strip
                                                                     '-> your sink
```

Each stage has one job and is kept separate, so decoding and rules are portable
C++ with host tests, and only the thin ESP-IDF layer touches hardware. Details:
[module boundaries](docs/architecture/module-boundaries.md) and
[firmware composition](docs/architecture/firmware-composition.md).

Generic CAN and telemetry code lives in the companion project
[`esp32-vehicle-can-core`](https://github.com/Yuke-hd/esp32-vehicle-can-core)
(pinned at `0.2.1`) and is fetched automatically at build time.

## Hardware

- [WeAct Studio CAN485 DevBoard V1.1](docs/architecture/hardware/weact-can485-v1.1.md)
  (ESP32)
- For the built-in LED sink: a 100-pixel WS2812B strip on GPIO16
- A CAN connection to the vehicle (receive only)

Pin assignments and validation requirements are in the hardware record linked
above.

## Quick start

Requirements: CMake 3.20+, a C++17 compiler, Ninja, and Python 3. Firmware work
also needs ESP-IDF 5.5.4.

Build and run the host tests:

```sh
cmake -S . -B build/host -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build/host --parallel
ctest --test-dir build/host --output-on-failure
```

Build the firmware (after activating ESP-IDF, see the
[firmware build guide](docs/development/firmware-build.md)):

```sh
cd firmware/weact-can485-v1.1
idf.py set-target esp32
idf.py build
```

Before flashing or connecting anything, read the safety notes above and the
[firmware build guide](docs/development/firmware-build.md). Every supported build mode (sanitizers, offline,
custom profile, validators) is listed in
[build modes](docs/development/build-modes.md).

## Try it without a car

Replay synthetic CAN logs on your computer and watch the resulting LED frames:

- [`can-replay`](docs/specs/replay/pixel-frame-output.md) renders replayed
  frames as JSONL.
- [`gvret-led-emulator`](docs/specs/replay/web-emulator.md) shows them in a
  local browser page.

Replay output from a real capture can reveal trip details, so only publish
output generated from synthetic fixtures.

## Documentation

Start at [`docs/README.md`](docs/README.md); it routes by task. Common entry
points:

- [Architecture](docs/architecture/) — boundaries and invariants
- [Specs](docs/specs/) — intended behaviour of each component
- [Protocol](docs/protocol/) — Mazda CAN signals, evidence, provenance
- [Development](docs/development/) — build, test, and validation procedures

## Roadmap

- Validate the pinout, CAN electrical path, fuse, and current budget on an
  isolated bench.
- Document a worked example of writing a custom sink.
- Gather reviewed timing evidence for brake freshness.
- Validate strip orientation, logic levels, thermal behaviour, and sustained
  load on hardware.

## Contributing

Read [`CONTRIBUTING.md`](CONTRIBUTING.md). Keep the listen-only and fail-off
guarantees intact, and report hardware and vehicle results separately from host
test results. Never submit raw vehicle captures, VINs, or credentials; see the
[vehicle-data policy](docs/development/license-and-vehicle-data.md).

## License

Apache-2.0; see [`LICENSE`](LICENSE). Third-party material keeps its own
license, listed in [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).

Thanks to WeAct Studio, Espressif (ESP-IDF and `led_strip`), and the
[opendbc](https://github.com/commaai/opendbc) project for signal provenance.
