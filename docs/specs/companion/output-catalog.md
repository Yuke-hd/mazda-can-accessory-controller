# Spec: Companion output catalog

Status: **Accepted contract for companion protocol 1.2.** Not implemented. This
document does not claim firmware, phone, bench or vehicle validation.

[ADR-0006](../../decisions/0006-generated-output-catalog.md) records the
architectural decision and its tradeoffs. This document specifies the
descriptor format, the generator, the catalog document, its BLE transport and
the implementation plan.

## Objective

Let the companion app discover which output types the firmware accepts, their
fields and the limits the validator enforces, so the app can render generic
editing forms without hard-coded effect tables. Adding an effect or an output
type must not need an app release, a protocol version change or a BLE change,
for an app that has the tolerant configuration model and the catalog renderer
(see [Implementation plan](#implementation-plan)).

Owner-confirmed decisions:

- The app renders generic forms driven by the catalog.
- The catalog carries output types, their fields, enum values and per-field
  limits: only facts that the configuration validator checks.
- The C++ limits header is generated from the same source in the first
  implementation.
- Adding an output type, field or effect changes no version. The catalog
  format changes only when its encoding changes; the configuration schema
  version changes only when the persisted document changes.
- Each firmware project passes its own board values to the generator.
- Generated files are produced during the build, as the factory configuration
  is, and are not checked in.

## Scope

In scope:

- a per-sink YAML descriptor and the descriptor for the `local_argb` sink;
- a generator that emits the catalog document and a C++ limits header;
- the configuration loader, validator and YAML compiler consuming the
  generated facts;
- a protected, paged BLE characteristic that serves the document;
- a Device info capability bit.

Out of scope:

- the app work itself: the tolerant configuration model and the form
  renderer in `Yuke-hd/esp-can-companion` (iOS), tracked as tickets 7 and 8;
- the Android app (`esp-can-companion-android`), deferred by the owner. Until
  it gains the same tolerant model, the
  [rollout rule](#implementation-plan) applies to it as well;
- rules, actions, signals and capacities in the catalog;
- a relay or other non-LED sink, and builds without the LED sink;
- moving `local_argb::kLedCount` from a global constant to a board value;
- changing the persisted configuration schema.

## Rule for catalog content

A fact goes in the catalog only if the configuration validator, loader or YAML
compiler rejects a document that breaks it. This keeps the catalog an exact
description of what the firmware accepts, and keeps it small.

| In the catalog | Not in the catalog |
| --- | --- |
| Output type names and the sink that owns each one. | Binding capacities (8 effect, 8 fill, 8 transient); they are checked at apply. |
| Field names, kinds and whether they are required. | Duplicate-target rules between bindings. |
| Integer ranges and defaults. | Labels, descriptions, units, icons or ordering hints. |
| Enum values, such as effect names and zone directions. | Rendering behaviour, such as priority tie-breaking. |
| Zone pixel count and colour channel range. | Signal, rule or action definitions. |
| Rule types that may drive a binding. | Vehicle or timing evidence. |

The app shows display names derived from the snake_case keys, or its own
curated names keyed by them. Curated names are not protocol requirements.

## Descriptor source

Each sink component that contributes output types owns one descriptor,
`output_catalog.yaml`, at the component root. The first descriptor is
`components/local_argb_actions/output_catalog.yaml`, beside the adapter that
installs the bindings.

```yaml
descriptor_format: 1
sink: local_argb
outputs:
  - type: led_effect
    fields:
      - &action {name: action, kind: action, required: true}
      - {name: effect, kind: enum, required: true,
         values: [left_turn, right_turn, brake]}
      - &priority {name: priority, kind: integer, required: false,
                   min: 0, max: 255, default: 100}
  - type: led_fill
    fields:
      - *action
      - &zone {name: zone, kind: zone, required: true,
               pixels: {board: strip_pixels},
               directions: [start_to_end, end_to_start, center_out]}
      - &color {name: color, kind: rgb, required: true, min: 0, max: 255}
      - *priority
  - type: led_transient
    fields:
      - *action
      - *zone
      - *color
      - {name: duration_ms, kind: integer, required: true, min: 1, max: 5000}
      - *priority
  - type: led_solid
    rule_types: [state, sampled_state]
    fields: [*action, *zone, *color, *priority]
```

Descriptor rules:

- `descriptor_format` is `1`. The generator rejects any other value.
- `sink` and every `type`, field `name`, enum value and rule type are
  snake_case ASCII (`[a-z][a-z0-9_]*`), at most 31 bytes.
- `rule_types` is optional. When present, only an action driven exclusively by
  those rule types may be bound. Absent means any rule type.
- Field kinds and their attributes are listed in [Field kinds](#field-kinds).
  Each kind's attributes are all required, except `default`.
- An integer attribute may be a literal or `{board: <name>}`. The generator
  replaces a board reference with the value the firmware project passes. An
  unresolved reference is an error.
- `default` is allowed only on a field with `required: false`, and must lie in
  its range. A field with `required: false` must have a `default`.
- YAML anchors and aliases are allowed; the generator sees the expanded data.
- Unknown keys are errors, so a misspelled attribute cannot be ignored
  silently.

## Field kinds

Catalog format 1 defines five field kinds. A new kind is a new feature of the
document; see [Compatibility](#compatibility).

| Kind | Attributes | Accepted value in the configuration |
| --- | --- | --- |
| `action` | none | A string naming a declared action. |
| `integer` | `min`, `max`, optional `default` | A JSON integer token from `min` to `max` inclusive. |
| `enum` | `values` (1–32 entries) | One of `values`. |
| `rgb` | `min`, `max` | `{red, green, blue}`, each an integer from `min` to `max`. |
| `zone` | `pixels`, `directions` (1–32 entries) | `{start, length, direction}` with `start >= 0`, `length >= 1`, `start + length <= pixels`, and `direction` one of `directions`. |

`min <= max` for every range. Integer attributes are signed and fit in
`int32`.

## Generator

`tools/generate_output_catalog.py` reads the selected descriptors and writes
two outputs. It uses Python 3 and PyYAML, which the firmware build already
requires for the factory configuration.

```sh
python3 tools/generate_output_catalog.py \
  --descriptor components/local_argb_actions/output_catalog.yaml \
  --board strip_pixels=100 \
  --catalog <build>/output_catalog.json \
  --limits-header <build>/generated/controller_config/generated/output_limits.hpp
```

- `--descriptor` may repeat. Outputs keep descriptor order, then the order
  within each descriptor.
- `--board name=value` may repeat. Each value is a decimal integer. A board
  value no descriptor uses is an error, so a stale value cannot linger.
- An output type name that appears twice, in one descriptor or across
  descriptors, is an error.
- Every error names the descriptor file and the path of the offending entry,
  and the generator exits non-zero without writing either output.
- Output is deterministic: the same inputs give byte-identical files. The
  generator writes no timestamps, absolute paths or host names.
- The generator enforces the [limits](#limits) and fails if the document is
  too large.

### Catalog document

The document is UTF-8 JSON with no whitespace between tokens. Keys appear in
the order shown. The device serves it unchanged, and the YAML compiler reads
the same file.

```json
{"outputs":[{"type":"led_solid","sink":"local_argb","rule_types":["state","sampled_state"],"fields":[{"name":"action","kind":"action","required":true}]}]}
```

- Top level: `outputs`, an array of output entries.
- Output entry: `type`, `sink`, `rule_types` when the descriptor has it, then
  `fields`.
- Field: `name`, `kind`, `required`, then the kind's attributes in the order of
  the [Field kinds](#field-kinds) table, with `default` last.
- Board references are resolved; the document contains only literals.
- `{"outputs":[]}` is a valid document for a build with no output sinks.

The full document for the WeAct CAN485 V1.1 build is in
[Synthetic reference vectors](#synthetic-reference-vectors).

### Limits header

The generator writes a C++17 header in the build directory, included as
`controller_config/generated/output_limits.hpp`. It holds data only:

```cpp
namespace controller_config::generated {

inline constexpr std::array<std::string_view, 4> kOutputTypeNames{
    "led_effect", "led_fill", "led_transient", "led_solid"};

namespace led_transient {
inline constexpr std::int32_t kZonePixels = 100;
inline constexpr std::array<std::string_view, 3> kZoneDirections{
    "start_to_end", "end_to_start", "center_out"};
inline constexpr std::int32_t kColorMin = 0;
inline constexpr std::int32_t kColorMax = 255;
inline constexpr std::int32_t kDurationMsMin = 1;
inline constexpr std::int32_t kDurationMsMax = 5000;
inline constexpr std::int32_t kPriorityMin = 0;
inline constexpr std::int32_t kPriorityMax = 255;
inline constexpr std::int32_t kPriorityDefault = 100;
} // namespace led_transient

} // namespace controller_config::generated
```

- One namespace per output type, named after the type.
- Constants are `k` + PascalCase field name + PascalCase attribute: `Min`,
  `Max`, `Default`, `Values`, `Pixels`, `Directions`.
- An output with `rule_types` gets `kRuleTypes`.
- The header includes only `<array>`, `<cstdint>` and `<string_view>`.

The loader, validator and their tests use these constants in place of
literals. Hand-written code keeps its parsing, enumerator mapping and
validation logic. Compile-time checks in `controller_config` tie the generated
values to the renderer contract, so a mismatched board value or descriptor
fails the build:

- every `kZonePixels` equals `local_argb::kLedCount`;
- `led_transient::kDurationMsMax * 1000` equals
  `local_argb::internal::kMaxTransientDurationUs`;
- every `kPriorityDefault` equals `local_argb::internal::EffectPriority::kDefault`.

A host test checks that the persisted name tables in `names.cpp` list exactly
the generated output type, effect and direction names, in any order.

### Build integration

The generator runs at configure time and as a build rule, following the
factory configuration in `firmware/weact-can485-v1.1/CMakeLists.txt`:

- `firmware/weact-can485-v1.1/output_catalog.cmake` sets
  `OUTPUT_CATALOG_DESCRIPTORS` and `OUTPUT_CATALOG_BOARD` for that board
  (`strip_pixels=100`). The firmware project and the host build both include
  it, so host tests compile the same header the firmware uses.
- A configure-time run makes a malformed descriptor fail configuration
  immediately. The build rule regenerates when a descriptor, the board file or
  the generator changes.
- The catalog is generated before the factory configuration is compiled.
  `tools/compile_controller_config.py` takes `--output-catalog <json>` and
  reads the pixel count, names and ranges from it. It drops `PIXEL_COUNT` and
  its regular-expression read of `lighting_sink.hpp`.
- `controller_config` adds the generated include directory and depends on the
  generation target.
- The firmware `main` component embeds the catalog with `EMBED_FILES` and
  passes it to `companion_ble`.

## BLE transport

### Characteristic

| Attribute | `xxxx` | UUID | Properties | Security |
| --- | --- | --- | --- | --- |
| Output catalog | `0008` | `ab490008-09b6-4509-bf8e-2790253baf98` | Read, Write | Encrypted, bonded |

`companion_ble` receives the document as opaque bytes in a new
`ServiceInputs` field. It checks only the length bounds and never parses the
JSON, so it gains no dependency on `controller_config` or any sink. The
architecture check keeps `companion_ble`'s allowed dependencies unchanged.

### Paging

Paging uses the [signal catalog selector](extensible-live-signals.md#catalog-discovery)
grammar, with its own selector state and its own format constant:

- Page response: `catalog_format` u8 (`1`), `token` u32, `total_bytes` u16,
  `offset` u16, `data_length` u16, then up to 200 document bytes. At most 211
  bytes. All integers are little-endian.
- Selector Write Request: exactly seven bytes, `catalog_format` u8, `token`
  u32, `offset` u16.
- Errors, first match wins: wrong length `0x0D`; unknown format
  `UnsupportedOperation` (`0x81`); unusable catalog or wrong token
  `InvalidState` (`0x83`); offset beyond the document `InvalidPdu` (`0x80`).
  Offset equal to `total_bytes` returns an empty terminal page. A failed write
  leaves the selection unchanged.
- The token is the connection's discovery token: the same nonzero value that
  the signal catalog serves on that connection. It changes on every new
  connection in a firmware run.
- The initial Read on a connection selects offset zero. The selector resets
  on disconnect. A selected page stays byte-stable across Read Blob requests.
- Reads and writes have no configuration, storage, CAN or lighting side
  effect.

The page `catalog_format` versions both the page header and the JSON
document. It is independent of the signal catalog's format.

### Device info

Protocol 1.2 defines Device info `extension_flags` bit 1: the output catalog
is usable. It is set when the build passed a document of 1 to 16384 bytes;
an empty catalog document (`{"outputs":[]}`) still sets it. When the bit is
clear, the characteristic behaves as an unusable signal catalog does.
`extension_version` stays `1`. Bit 0 keeps its meaning.

`extension_version` must not change for this feature. Released iOS and Android
apps enable extensible live signals only when `extension_version` equals `1`
exactly, so raising it would switch off live signals in every released app.
New capabilities are added as new `extension_flags` bits.

## Client behaviour

Released apps do not yet behave this way. Today the iOS and Android
configuration models are closed enums: an unknown output type, effect or
direction fails decoding of the whole configuration, and keys the app does not
know are dropped when it writes the configuration back. Ticket 7 makes the iOS
model tolerant; the rules below assume it.

- Use the output catalog only when `protocol_major` is 1, `protocol_minor` is
  at least 2 and `extension_flags` bit 1 is set. Otherwise fall back to the
  app's built-in knowledge of schema version 1, or offer only opaque JSON
  editing.
- Read every page from offset zero, verifying the token, contiguous offsets,
  stable `total_bytes` and page lengths. Discard a partial or inconsistent
  download, and download again after every reconnect.
- Reject the document if it is not valid JSON, `outputs` is missing or not an
  array, or two entries share a `type`.
- Ignore unknown keys at every level.
- An output entry with an unknown field kind, or with a known kind missing a
  required attribute, is not editable. The app still shows existing bindings
  of that type as read-only JSON, and keeps them when it writes the
  configuration back.
- An output type in the read-back configuration that the catalog does not list
  is treated the same way.
- Keep unknown keys on known output bindings when writing the configuration
  back, and keep unknown output types, effects and enum values as their
  original JSON. An unknown value must never fail decoding of the whole
  configuration.
- The catalog does not replace firmware validation. A configuration that
  matches the catalog can still fail validation for duplicates, or fail at
  apply for capacity; the app reports the firmware's error.

## Compatibility

| Change | Effect |
| --- | --- |
| New output type, field or enum value | Content only. No version changes. Apps with the tolerant model show what they cannot edit read-only. Apps without it fail to decode a configuration that uses the new value; see the rollout rule. |
| New field kind | Content only. Apps with the tolerant model show the affected types read-only. |
| New attribute on a known kind | Content only, if optional. A new required attribute needs a catalog format change. |
| Changed meaning of an existing key or attribute | Catalog format change. |
| Page header change | Catalog format change. |
| Persisted configuration change | Configuration schema version change, as today. |

## Limits

| Limit | Value |
| --- | --- |
| Document size | 1–16384 bytes |
| Output types | 32 |
| Fields per output type | 16 |
| Enum values, directions or rule types per field | 32 |
| Name length | 31 bytes |
| Descriptors per build | 4, matching `SinkFanOut::kCapacity` |

The generator fails the build when any limit is exceeded; the device never
serves a truncated catalog.

## Tech stack and project structure

C++17, Python 3 with PyYAML, ESP-IDF 5.5.4 (`esp32`) and the existing NimBLE
binding. No new dependency.

| Location | Responsibility |
| --- | --- |
| `components/local_argb_actions/output_catalog.yaml` | The LED sink's descriptor. |
| `tools/generate_output_catalog.py` | Descriptor validation, merging, document and header generation. |
| `firmware/weact-can485-v1.1/output_catalog.cmake` | The board's descriptor list and board values. |
| `components/controller_config/` | Uses the generated header; compile-time parity checks. |
| `tools/compile_controller_config.py` | Reads the generated catalog for its limits and names. |
| `lib/companion_protocol/` | Page encoding for a static document. |
| `components/companion_ble/` | The protected characteristic and the Device info bit. |
| `tests/tools/`, `tests/host/` | Generator, compiler, parity and paging tests. |

## Commands and testing strategy

Use a fresh host build directory, as in
[supported builds](../../development/supported-build.md):

```sh
python3 tools/check_toolchain.py --scope host
cmake -S . -B /tmp/mazda-output-catalog-host -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DMAZDA_BUILD_HOST_TESTS=ON
cmake --build /tmp/mazda-output-catalog-host --parallel
ctest --test-dir /tmp/mazda-output-catalog-host --output-on-failure
python3 tools/check_architecture.py --root . --compiler c++ --cmake cmake
```

Firmware compilation and size, with no flashing, follow
[firmware build](../../development/firmware-build.md).

Tests must cover:

1. Generator: each descriptor rule and limit, duplicate types across two
   descriptors, unresolved and unused board values, deterministic bytes, and
   no output on error.
2. The generated WeAct document equals the reference vector byte for byte.
3. Compile-time parity with the renderer constants, and the `names.cpp`
   parity test.
4. Existing configuration loader, validator and YAML compiler tests pass
   unchanged, and the factory configuration output is byte-identical before
   and after the compiler switch.
5. Paging at offsets 0, 200, the last page and `total_bytes`, plus every
   selector error in order, wrong tokens across reconnect, and an unusable
   catalog.
6. Device info bit 1 set and clear, and protocol minor 2.
7. The architecture check proves `companion_ble` gained no dependency.

Phone discovery, Service Changed after upgrade and on-device paging need a
separately scoped bench session. Host tests and a firmware build do not
establish them.

## Boundaries

- **Always:** keep `companion_ble` sink-agnostic; derive every catalog fact
  from a descriptor; keep the validator authoritative; fail the build on any
  descriptor error; preserve receive-only CAN and fail-off behaviour.
- **Ask first:** put a fact in the catalog that the validator does not check;
  change the persisted schema; raise a published limit; move `kLedCount` to a
  board value; add a dependency; flash or interact with hardware.
- **Never:** check generated files in; generate parsing, validation or
  rendering logic; let a catalog read change configuration, storage or
  lighting; serve a partial catalog.

## Success criteria

- Adding a fourth LED effect touches its descriptor, `names.cpp`, the
  enumerator and its renderer. The validator limits, YAML compiler and served
  catalog pick it up with no BLE or protocol change, and an iOS app with
  tickets 7 and 8 can edit it without a release.
- `extension_version` remains `1`, and extensible live signals keep working
  in released apps.
- Changing a limit in the descriptor without the matching renderer change
  fails the build.
- The served document matches the generated file and the reference vector.
- `companion_ble` has the same allowed dependencies as before.
- Host tests, the architecture check and the firmware build pass. Unrun bench
  and phone checks are stated plainly.

## Implementation plan

Stacked pull requests, one per ticket. Tracked by
[#279](https://github.com/Yuke-hd/mazda-can-accessory-controller/issues/279).

| # | Ticket | Depends on | Touches |
| --- | --- | --- | --- |
| 1 ([#280](https://github.com/Yuke-hd/mazda-can-accessory-controller/issues/280)) | Docs: ADR-0006, this spec, BLE protocol 1.2 updates. | — | `docs/` |
| 2 ([#281](https://github.com/Yuke-hd/mazda-can-accessory-controller/issues/281)) | Generator, LED descriptor, board file, CMake generation targets for host and firmware (outputs not yet consumed), generator tests. | 1 | `tools/`, `components/local_argb_actions/`, `firmware/.../output_catalog.cmake`, CMake, `tests/tools/` |
| 3 ([#282](https://github.com/Yuke-hd/mazda-can-accessory-controller/issues/282)) | `controller_config` uses the generated header; parity checks and `names.cpp` test. | 2 | `components/controller_config/`, `tests/host/` |
| 4 ([#283](https://github.com/Yuke-hd/mazda-can-accessory-controller/issues/283)) | YAML compiler reads the generated catalog; factory build guard updated. | 2 | `tools/compile_controller_config.py`, firmware CMake compiler arguments, `tests/tools/` |
| 5 ([#284](https://github.com/Yuke-hd/mazda-can-accessory-controller/issues/284)) | Static document page encoder with synthetic vectors. | 1 | `lib/companion_protocol/`, `tests/host/` |
| 6 ([#285](https://github.com/Yuke-hd/mazda-can-accessory-controller/issues/285)) | Characteristic `0008`, `ServiceInputs` field, Device info bit 1, protocol 1.2, firmware embed. | 2, 5 | `components/companion_ble/`, `firmware/.../main/` |
| 7 ([esp-can-companion#82](https://github.com/Yuke-hd/esp-can-companion/issues/82)) | iOS tolerant configuration model: keep unknown output types, effects, enum values and keys as raw JSON, and write them back unchanged. | — | `Yuke-hd/esp-can-companion` (`CompanionProtocol` configuration model) |
| 8 ([esp-can-companion#83](https://github.com/Yuke-hd/esp-can-companion/issues/83)) | iOS output catalog client and generic form renderer: characteristic `0008` in the BLE transport, paged download reusing the signal catalog client, and catalog-driven forms. | 1 for vectors, 6 for devices, 7 | `Yuke-hd/esp-can-companion` |

Tickets 3, 4 and 5 can run in parallel once their dependencies merge; they
touch separate files. Ticket 7 does not depend on the catalog and can start
at any time.

There is no generic output editor in the iOS app to replace. Setup today
edits only the zone, colour and priority of the existing `rpm_fill`,
`red_zone` and `brake` bindings, so ticket 8 is a new feature. In the
transport, `0008` needs a `CompanionCharacteristic` case, a UUID constant, an
entry in the readable and writable lists, and the same read-and-write check
that the usable-characteristic filter applies to the signal catalog.

Rollout rule: the firmware must not add a new output type, effect or enum
value to a descriptor until ticket 7 has shipped in a released app. A device
whose configuration uses such a value would otherwise make older apps fail to
read its configuration at all. The Android app is deferred and has the same
closed model, so the rule also holds for Android until it gains a tolerant
model. Tickets 1–6 add no new content and are safe to ship first.

## Synthetic reference vectors

The WeAct CAN485 V1.1 catalog, generated from the descriptor above with
`strip_pixels=100`, is these 1518 bytes, on one line:

```json
{"outputs":[{"type":"led_effect","sink":"local_argb","fields":[{"name":"action","kind":"action","required":true},{"name":"effect","kind":"enum","required":true,"values":["left_turn","right_turn","brake"]},{"name":"priority","kind":"integer","required":false,"min":0,"max":255,"default":100}]},{"type":"led_fill","sink":"local_argb","fields":[{"name":"action","kind":"action","required":true},{"name":"zone","kind":"zone","required":true,"pixels":100,"directions":["start_to_end","end_to_start","center_out"]},{"name":"color","kind":"rgb","required":true,"min":0,"max":255},{"name":"priority","kind":"integer","required":false,"min":0,"max":255,"default":100}]},{"type":"led_transient","sink":"local_argb","fields":[{"name":"action","kind":"action","required":true},{"name":"zone","kind":"zone","required":true,"pixels":100,"directions":["start_to_end","end_to_start","center_out"]},{"name":"color","kind":"rgb","required":true,"min":0,"max":255},{"name":"duration_ms","kind":"integer","required":true,"min":1,"max":5000},{"name":"priority","kind":"integer","required":false,"min":0,"max":255,"default":100}]},{"type":"led_solid","sink":"local_argb","rule_types":["state","sampled_state"],"fields":[{"name":"action","kind":"action","required":true},{"name":"zone","kind":"zone","required":true,"pixels":100,"directions":["start_to_end","end_to_start","center_out"]},{"name":"color","kind":"rgb","required":true,"min":0,"max":255},{"name":"priority","kind":"integer","required":false,"min":0,"max":255,"default":100}]}]}
```

Page vectors use token `0x01020304`:

| Vector | Bytes (hex) |
| --- | --- |
| First page header for the WeAct document | `01 04 03 02 01 ee 05 00 00 c8 00`, then document bytes 0–199 |
| Selector for offset 200 | `01 04 03 02 01 c8 00` |
| Terminal page for the WeAct document | `01 04 03 02 01 ee 05 ee 05 00 00` |
| Whole page for the empty document | `01 04 03 02 01 0e 00 00 00 0e 00 7b 22 6f 75 74 70 75 74 73 22 3a 5b 5d 7d` |
