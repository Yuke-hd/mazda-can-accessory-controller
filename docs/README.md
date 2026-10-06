# Documentation

Consult the documents relevant to the change. Current contracts and intended
behaviour live in architecture and specs; protocol documents distinguish source
evidence from software behaviour. Work items preserve history and do not define
current requirements unless a task explicitly references them.

| Category | Purpose |
| --- | --- |
| [Architecture](architecture/) | System boundaries, domain model, dependency direction, component responsibilities and stable invariants. |
| [Specs](specs/) | Current component and feature semantics, formats, lifecycle, edge cases and constraints. |
| [Protocol](protocol/) | Mazda CAN/DBC mappings, signal evidence, provenance and verification. |
| [Development](development/) | Build, test, toolchain and validation procedures. |
| [Decisions](decisions/) | Architecture decision records: why a significant choice was made and what it rules out. |
| [Work items](work-items/) | Historical implementation, milestone and superseded design records. |

## Find context by task

| Change | Starting points |
| --- | --- |
| Component ownership or public headers | [Module boundaries](architecture/module-boundaries.md), [signal layering](architecture/signal-layering.md), [architecture validation](development/architecture-validation.md). |
| Frame, signal or state model | [Domain model](architecture/domain-model.md), [telemetry contracts](specs/telemetry/telemetry-contracts.md). |
| CAN acquisition or safety boundary | [Receive-only boundary](architecture/receive-only-boundary.md), [acquisition](specs/can/acquisition.md). |
| Mazda decoder or signal confidence | [Decoder behaviour](specs/telemetry/decoder-behaviour.md), [decoder mappings](protocol/decoder-mappings.md), [signal evidence](protocol/signal-evidence.md), [turn state](specs/telemetry/turn-state.md). |
| DBC/provenance verification | [DBC metadata verification](protocol/dbc-metadata-verification.md), [opendbc provenance](protocol/opendbc-provenance.md). |
| Generic rules or local lighting | [Action engine](specs/action-engine.md), [local LED actions](specs/lighting/local-led-actions.md), [renderer runtime](specs/lighting/renderer-runtime.md). |
| Configuration formats/loaders or lighting profile | [Controller configuration](specs/configuration/controller-config.md), [lighting profile application and RPM features](specs/configuration/lighting-profile.md), [YAML and JSON examples](specs/configuration/examples/). |
| Companion app BLE link | [ADR-0001 to ADR-0003](decisions/), [companion BLE protocol](specs/companion/ble-protocol.md), [config transfer](specs/companion/config-transfer.md), [live signals](specs/companion/live-signals.md), [NimBLE resource budget](development/ble-resource-budget.md), [firmware composition](architecture/firmware-composition.md#companion-ble), [controller configuration](specs/configuration/controller-config.md), [signal evidence](protocol/signal-evidence.md). |
| Firmware wiring or startup | [Firmware composition](architecture/firmware-composition.md), [WeAct hardware record](architecture/hardware/weact-can485-v1.1.md). |
| Host ingestion or replay output | [GVRET ingestion](specs/replay/gvret-ingestion.md), [pixel-frame output](specs/replay/pixel-frame-output.md), [signal observers](specs/replay/signal-observers.md). |
| Browser emulator or playback | [Web emulator](specs/replay/web-emulator.md), [browser renderer](specs/replay/browser-renderer.md), [playback](specs/replay/playback.md). |
| Build, test or CI | [Supported host builds](development/supported-build.md), [firmware builds](development/firmware-build.md), [architecture validation](development/architecture-validation.md). |
| Fixtures or third-party material | [License and vehicle-data policy](development/license-and-vehicle-data.md), [third-party notices](../THIRD_PARTY_NOTICES.md). |

## Authority and history

- [Signal evidence](protocol/signal-evidence.md) owns current per-channel
  confidence assignments. [The reviewed DBC](protocol/mazda_custom.dbc) is
  source evidence; compiled constexpr definitions are executable metadata.
  Synthetic tests verify software consistency and do not promote confidence.
- [Firmware composition](architecture/firmware-composition.md) owns current
  application wiring. The action-engine and lighting specs own their respective
  reusable semantics.
- [The retired raw-capture format](work-items/retired-raw-capture-format.md)
  and older signal/lighting review records are historical. Supported host
  ingestion is defined by the GVRET spec.
- Historical test results record what was run at the documented revision and
  time. They are not current CI status, bench acceptance or vehicle validation.

Contributor workflow remains in [CONTRIBUTING.md](../CONTRIBUTING.md). Project
orientation and the system overview remain in [README.md](../README.md).
