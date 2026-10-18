# ADR-0006: Generate an output catalog from per-sink YAML descriptors

Status: Accepted
Date: 2026-10-18
Retrospective: no

## Context

The companion app edits the controller configuration's `outputs` list. To do
that today it must hard-code each output type (`led_effect`, `led_fill`,
`led_transient`, `led_solid`), the effect names, the zone directions and every
field limit. Firmware already defines the same facts in three places that are
kept in step by hand:

- the C++ loader and validator in `controller_config`, partly derived from
  renderer constants such as `local_argb::kLedCount` and
  `local_argb::internal::kMaxTransientDurationUs`;
- the persisted name tables in `controller_config/src/persisted/names.cpp`;
- the YAML compiler `tools/compile_controller_config.py`, which repeats the
  pixel count and reads the transient limit from a C++ header with a regular
  expression.

The owner wants the app to discover the available outputs from the device so
that a new effect or output type does not need a coordinated app release. The
app will render generic forms from what it discovers.

The BLE service must stay independent of any sink. `companion_ble` depends
only on `companion_protocol` and `vehicle_signals`, and moves configuration
as opaque bytes. The signal catalog
([ADR-0005](0005-discoverable-live-signals.md)) already shows how the device
can describe itself through a paged, protected characteristic. A build may
combine several sinks through `action_engine::SinkFanOut`, and a later build
may have no LED sink at all.

The firmware build already runs a Python step with PyYAML at configure and
build time to compile `config/default.yaml` into the embedded factory
configuration. A second generator adds no new tool dependency.

## Decision

- Each sink component that contributes output types owns one YAML descriptor
  file. The descriptor declares its output types, their fields, and each
  field's kind and limits. It is the single source of those facts.
- A build-time generator merges the descriptors that a firmware project
  selects, together with board values that the project passes (such as the
  strip pixel count), and emits two outputs into the build directory:
  1. the output catalog document, which the device serves over BLE and the
     YAML compiler reads in place of its own copies of those facts;
  2. a C++17 header of `constexpr` limits and names that the configuration
     loader and validator include.
- Generate data, not logic. Rendering, parsing and validation code stay
  hand-written. The generated header replaces literal limits only, and
  compile-time checks tie it to the renderer's own constants.
- The catalog contains only facts that the configuration validator enforces:
  output types, their fields, field kinds, enum values, numeric ranges,
  defaults and the rule types that may drive a binding. If the validator does
  not check a field, the field does not go in the catalog. Runtime capacities
  and cross-binding duplicate rules remain enforced by the firmware alone.
- Generate during the build, as the factory configuration is generated. Do not
  check generated files into the repository.
- The composition root embeds the merged document and passes it to
  `companion_ble` as opaque bytes. `companion_ble` serves the bytes through a
  new protected characteristic that reuses the signal catalog's page selector
  grammar. The per-connection discovery token moves into `companion_ble`, so
  the output catalog works whether or not the signal catalog is usable.
  It does not parse the document and gains no sink dependency.
- One catalog describes every sink in the build. Output type names are unique
  across sinks; each entry names the sink that owns it.
- Versioning: adding an output type, field or effect is an additive content
  change and changes no version. The catalog format version changes only when
  the document or paging encoding changes incompatibly. The configuration
  schema version changes only when the persisted document changes, as today.
- Board limits come from each firmware project, not from a configuration
  flag that selects a board model.

The [output catalog spec](../specs/companion/output-catalog.md) owns the
descriptor grammar, generator interface, document format, UUID and limits.
This record does not claim implementation, phone, bench or vehicle
validation.

## Consequences

Easier:

- A new effect or output type is added in one descriptor plus its hand-written
  parsing and rendering code. The app, the validator limits and the YAML
  compiler pick up the new facts from the same build.
- Drift between the C++ validator and the Python compiler becomes a build
  failure or a test failure instead of a silent mismatch.
- A build with different sinks or a different strip length serves a different
  catalog without any change to `companion_ble`.

Harder:

- The firmware and host builds gain a second generation step, which must run
  before `controller_config` compiles and before the factory configuration is
  compiled.
- The generator becomes a build dependency of the configuration component.
  Its errors must name the descriptor file and field so that a broken
  descriptor is easy to fix.
- The app needs a generic form renderer and must handle field kinds it does
  not know. A curated editor for a specific effect remains separate app work.
- Released apps decode output bindings into closed enums and fail on any
  unknown output type or effect. They also drop unknown keys on write-back.
  The iOS app must ship a tolerant configuration model before the firmware
  adds any new output content. Until then the catalog gives no
  release-free path. The Android app is deferred and stays bound by the same
  rule.
- `extension_version` must stay `1`. Released apps enable extensible live
  signals only for that exact value, so the catalog is signalled by a new
  `extension_flags` bit instead.
- The catalog describes what the validator accepts, not what fits at runtime.
  A configuration that matches the catalog can still fail at apply when it
  exceeds a binding capacity, and the firmware then fails off to black.

## Alternatives

- **Build the catalog at runtime in C++.** Each sink would register
  descriptors in code. This keeps everything in one language but adds
  descriptor tables and serialisation to the firmware, and leaves the Python
  compiler with its own copy of the facts.
- **Keep an app-side table keyed by schema version and hardware ID.** This
  needs no firmware work, but every new effect still needs an app release,
  and the table drifts from the firmware.
- **Check the generated files into the repository.** This avoids a build step
  but lets the checked-in output fall out of step with its source. The build
  already depends on Python and PyYAML, so the step costs nothing new.
- **Serve one catalog characteristic per sink.** This scales GATT attributes
  with the number of sinks and makes the app merge several documents. A
  single merged document keeps one discovery path.
- **Version the catalog by content.** Rejected: an additive change would force
  the app to treat a newer catalog as incompatible, which defeats discovery.

## References

- [ADR-0005: Discoverable live signals](0005-discoverable-live-signals.md)
- [Output catalog spec](../specs/companion/output-catalog.md)
- [BLE protocol](../specs/companion/ble-protocol.md)
- [Controller configuration](../specs/configuration/controller-config.md)
- [Module boundaries](../architecture/module-boundaries.md)
