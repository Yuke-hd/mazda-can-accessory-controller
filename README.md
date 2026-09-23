# Mazda CAN Accessory Controller

An ESP32 accessory controller that listens to Mazda CAN traffic and drives a
separate addressable-light strip from decoded turn and brake state. The assumed
future repository URL is [Yuke-hd/mazda-can-accessory-controller](https://github.com/Yuke-hd/mazda-can-accessory-controller);
the URL is documented for orientation only and does not imply that a public
repository, release, or hardware validation exists today.

## Table of Contents

- [About](#about)
- [Safety Boundary](#safety-boundary)
- [Built With](#built-with)
- [Hardware](#hardware)
- [Getting Started](#getting-started)
- [Usage](#usage)
- [Architecture and Data Flow](#architecture-and-data-flow)
- [Actions and Animations](#actions-and-animations)
- [Repository Layout](#repository-layout)
- [Roadmap](#roadmap)
- [Contributing](#contributing)
- [License and Data Policy](#license-and-data-policy)
- [Acknowledgments](#acknowledgments)

## About

The project turns reviewed, decoded vehicle signals into a bounded accessory
lighting command. The pinned companion core owns generic frame validation, CAN
acquisition, and the receive/runtime lifecycle. This controller owns Mazda
candidate decoding, signal freshness, message health, fail-safe availability,
board startup, strict listen-only CAN receive, and the LED sink.

This repository is an engineering starting point. Host tests and structural
validators do not demonstrate correct behavior on a vehicle, in a harness, or
under a particular power supply. Hardware and vehicle validation are still
required before any installation or release decision.

## Safety Boundary

- The vehicle firmware uses strict classic-CAN `TWAI_MODE_LISTEN_ONLY` with a
  zero-length transmit queue. It must not transmit CAN data frames, inject
  diagnostics, acknowledge as an application, or poll the vehicle.
- Unknown, stale, malformed, unavailable, stopped, or faulted telemetry is
  mapped to a black LED frame. A decoder error is not cleared by unrelated
  traffic; recovery requires a newer valid observation.
- Brake decoding is present, but its freshness timeout is intentionally unset
  until timing evidence is established. The production lighting adapter
  therefore requires `Fresh` brake availability and fails off for an
  unverified brake sample.
- This is not a replacement for factory indicators, brake lamps, a dashboard,
  or any certified safety system. Do not use LED output as a safety-critical
  indication.
- Verify CAN wiring, termination, polarity, ground, connector pinout, and
  transceiver behavior before connection. Use an appropriate fuse and
  current-limited, protected power source. Calculate strip current for the
  chosen LEDs and brightness; do not assume the controller or vehicle circuit
  can supply it.
- WS2812B-class strips may require level shifting and local bulk/decoupling
  capacitors. Confirm logic levels, ground reference, connector protection,
  thermal behavior, and enclosure strain relief on the actual assembly.

## Built With

- C++17 portable libraries and CMake host tests
- ESP-IDF **5.5.4**, target `esp32`
- Espressif `led_strip` **3.0.3** (pinned in the component manifest; the ESP-IDF configure step generates the lock)
- WeAct Studio CAN485 DevBoard V1.1
- Classic CAN receive through ESP-IDF TWAI in listen-only mode

## Hardware

The checked-in board capability record is in
[`components/board/include/board/board_config.h`](components/board/include/board/board_config.h).
The intended board is WeAct Studio CAN485 DevBoard V1.1; verify the physical
revision and assembly before use.

| Function | Pin | Role |
| --- | ---: | --- |
| CAN RX | GPIO26 | listen-only input |
| CAN TX | GPIO27 | held recessive; no application transmit API |
| Onboard WS2812 status pixel | GPIO4 | one-pixel status projection |
| Vehicle WS2812B strip | GPIO16 | 100-pixel accessory output |
| RS485 DE / RO / DI | GPIO17 / GPIO21 / GPIO22 | disabled auxiliary interface |

The strip orientation is a wiring decision: the renderer preserves the
left/right region mapping used by the current accessory design, but the
physical direction must be confirmed during hardware validation.

## Getting Started

### Prerequisites

- CMake 3.20 or newer and a C++17 compiler
- Ninja (recommended)
- Python 3 for validators
- ESP-IDF 5.5.4 and `idf.py` for firmware work
- A correctly fused, current-limited bench supply for hardware work

The generic CAN/frame/runtime code is consumed from the pinned vehicle-core
dependency through CMake `FetchContent` at
`https://github.com/Yuke-hd/esp32-vehicle-can-core`. `VEHICLE_CAN_CORE_COMMIT`
must always be a full immutable source commit SHA. For offline work, set
`VEHICLE_CAN_CORE_SOURCE_DIR` to a checkout of that exact source commit. Do not
copy generic components back into this repository.

### Host build, test, and validation

Use a new build directory so an older configuration cannot hide a change:

```sh
cmake -S . -B /tmp/mazda-accessory-controller-host -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build /tmp/mazda-accessory-controller-host --parallel
ctest --test-dir /tmp/mazda-accessory-controller-host --output-on-failure
python3 tools/check_architecture.py --root . \
  --core-root /path/to/esp32-vehicle-can-core
```

Run the host formatter used by CI when available:

```sh
clang-format --dry-run --Werror $(find lib components firmware tests -type f \
  \( -name '*.h' -o -name '*.hpp' -o -name '*.cpp' \) -print)
```

### Firmware build and flash

The firmware job builds only `firmware/weact-can485-v1.1` with ESP-IDF 5.5.4:

```sh
cd firmware/weact-can485-v1.1
idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

Check the port, power, fuse, CAN wiring, and strip wiring for the actual host
before flashing. A successful compile or flash is not vehicle validation.

## Usage

The application starts board safe defaults, clears both LED surfaces to black,
starts the listen-only CAN receiver, starts the bounded telemetry service, and
publishes only a private lighting command to the renderer. The renderer owns
the LED driver task; the telemetry producer never performs LED or RMT/SPI work.

Keep the controller disconnected from a vehicle while checking the image,
startup logs, polarity, strip current, and fail-off behavior. Public evidence
must not contain raw captures, VINs, credentials, precise location, or
reconstructable trip data.

## Architecture and Data Flow

```text
CAN transceiver (listen-only)
        -> vehicle_can_rx / can_bus
        -> vehicle_telemetry service
        -> Mazda candidate decoder + freshness/health state
        -> private LightingUpdate
        -> local_argb bounded mailbox
        -> 100-pixel GPIO16 strip + GPIO4 status projection
```

The vehicle target selects `vehicle_can_rx`; no bench ACK target is part of
this repository. Decoder and policy libraries are hardware independent. A
stale or faulted semantic state expires to black, and driver failure attempts
an immediate black frame before retrying under the worker watchdog.

## Actions and Animations

- **Off / fail-off:** all 100 logical pixels are black for startup, unknown,
  stale, unavailable, stopped, malformed, or expired state.
- **Turn:** the left and right regions each contain 35 pixels. The preserved
  running-flow strategy uses an amber five-pixel tail that travels from the
  center toward the corresponding outer edge. The preserved center-out-fill
  strategy fills each region from the center outward and is the current WeAct
  runtime strategy.
- **Brake:** the center 30-pixel region is solid red when a brake sample is
  fresh and valid. Unverified brake freshness fails off by design.
- **Overlap:** turn animation and the brake region may coexist; the single
  GPIO4 status pixel prioritizes red brake status, otherwise amber turn status,
  otherwise black.
- **Brightness:** the generic solid-color path caps each RGB component at its
  configured ceiling. Animation palettes use explicit bounded colors, and the
  renderer suppresses redundant physical refreshes.

The strip's left/right physical orientation and electrical behavior must be
verified on the assembled hardware; software tests do not establish those
facts.

## Repository Layout

- `firmware/weact-can485-v1.1/` — the sole ESP-IDF accessory firmware target
- `components/board/` — WeAct pin capabilities and safe startup defaults
- `components/vehicle_can_rx/` — the WeAct receive binding
- `components/mazda_telemetry/` — Mazda decoder, publication, subscriptions,
  and facade adaptation over the generic runtime
- `third_party/esp32-vehicle-can-core/` — optional local checkout location for
  the pinned generic dependency (not vendored in this repository)
- `components/local_argb/` — renderer, animations, watchdog, and GPIO4/GPIO16
  LED sinks
- `lib/mazda/` — Mazda definitions, decoder, health, and state
- `vehicle-can-core` (FetchContent/ESP-IDF dependency) — portable frames,
  signals, receive-only CAN, runtime, and notification contracts
- `tests/host/` and component tests — deterministic host coverage
- `docs/` — development, hardware, protocol, and policy notes
- `tools/` — architecture, receive-only, boundary, and artifact validators

## Roadmap

- Validate the WeAct V1.1 pinout, CAN electrical path, fuse, termination, and
  current budget on an isolated bench.
- Establish reviewed brake freshness evidence before enabling a fresh brake
  action in a deployment configuration.
- Validate strip orientation, logic-level margins, thermal behavior, warm-reset
  clearing, and sustained CAN/LED load on hardware.
- Keep signal provenance, privacy review, and vehicle-specific compatibility
  evidence current before any installation decision.
- Update the core dependency only by reviewing its source commit, updating the
  full SHA in CMake and ESP-IDF manifests/lock data, and running the architecture
  validator plus host tests against that exact checkout.

## Contributing

Read [`CONTRIBUTING.md`](CONTRIBUTING.md), preserve the strict listen-only and
fail-off boundary, and run host tests plus relevant Python validators. Do not
submit raw vehicle captures or credentials. Hardware and vehicle results must
be reported separately from deterministic host evidence.

## License and Data Policy

Project-authored source, documentation, tests, and tooling are licensed under
Apache-2.0; see [`LICENSE`](LICENSE). Third-party material retains its own
license and attribution in [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).
Vehicle data follows [`docs/policies/license-and-vehicle-data.md`](docs/policies/license-and-vehicle-data.md):
use synthetic, reviewed, anonymized fixtures only, and never commit raw
captures, VINs, credentials, precise locations, or absolute trip timestamps.

## Acknowledgments

- WeAct Studio board documentation for the CAN485 V1.1 hardware record.
- Espressif ESP-IDF and the `led_strip` 3.0.3 component.
- The opendbc project for candidate signal provenance, with attribution kept
  in the repository notices and signal-evidence documentation.
