# Vehicle profiles: signal manifest

Status: requirements approved by the owner on 2026-10-27. Implementation is
tracked by [milestone 5](https://github.com/Yuke-hd/mazda-can-accessory-controller/milestone/5)
(#311–#320) under the umbrella
[#310](https://github.com/Yuke-hd/mazda-can-accessory-controller/issues/310).
Module id: `signal-manifest`.
Scope decision: build-time profiles with multiple DBC sources, selected by the
owner on 2026-10-27. The other components, `dbc-import`, `profile-runtime` and
`profile-integration`, have their own milestones and specifications.

## Objective and ownership

Define one normalized profile contract between host-side DBC import and the
selected firmware provider. Derive repeated catalog and registration artifacts
from that contract so adding an ordinary supported signal does not require
hand-editing each consumer, tuple list, counter, and compiler manifest.

This module owns the versioned representation, validation rules, catalog
projections, deterministic artifact interfaces, and compatibility expectations.
It does not implement DBC recognition, generic extraction, runtime lifecycle,
profile selection in CMake, or configuration persistence. Those belong to
`dbc-import`, `profile-runtime`, and `profile-integration` respectively.

The existing `vehicle_signals::SignalProvider` remains the consumer boundary.
Do not introduce a second public vehicle abstraction or modify the companion
core dependency. The existing Mazda decoder remains authoritative until an
explicitly reviewed migration replaces a particular responsibility. Preserve
custom Mazda semantic handlers and the current public facade.

## Profile and source model

The normalized representation is UTF-8 JSON with an explicit format name and
integer schema version. Version 1 uses format `vehicle-signal-profile`.
Unknown versions, unknown object fields, duplicate JSON keys, missing required
fields, and invalid field types are errors. This is a host/build artifact,
not a new BLE wire format or an ESP32 JSON parser requirement.

| Record | Required information |
| --- | --- |
| Profile | `profile_id`, schema version, semantic-policy revision, recognition-rule revision, selected logical bus, source records, message records, canonical bindings, capability diagnostics, compatibility identity, and artifact identity. |
| Source | Stable source id, source kind, logical path or document reference, exact byte SHA-256, source revision when available, license/provenance reference, and logical bus association. |
| Message | Selected bus, identifier format, numeric identifier, classic-CAN DLC, and source-field records. File names do not identify frames. |
| Canonical binding | Canonical key, explicit per-profile runtime id, value type, unit, capabilities, enum choices where applicable, input references, conversion/handler policy, evidence reference, and explicit optional freshness policy. |
| Capability diagnostic | Stable category, affected canonical input or feature, source references, and reason. Diagnostic prose is not part of the machine contract. |

Each profile has one selected physical CAN input. Multiple source files may
contribute to its selected logical bus; other buses are not implicitly merged
into it. The initial runtime supports the existing standard-identifier classic
CAN boundary with payloads of at most eight bytes. A broader source inventory
does not authorize decoding unsupported selected traffic.

Source paths in generated artifacts are logical or package-relative. Exclude
machine-specific absolute paths, wall-clock generation timestamps, private
capture paths, and vehicle identifiers. DBC redistribution still requires
reviewed permission and attribution; accepting a source path is not permission
to commit that file.

## Mechanical fields and semantic bindings

DBC field records preserve the original DBC start coordinate, bit length,
byte order, signedness, scale, offset, declared range, unit, source name, and
source value table. Do not replace source units or enumerations with canonical
ones; the binding records the reviewed conversion between them.

- Raw integer literals and raw enum values use base-10 integer strings where
  JSON numeric precision would be insufficient. The format accommodates
  unsigned and signed fields up to 64 bits without a floating-point round trip.
- Scale, offset, and declared physical bounds use finite decimal strings.
  Preserve their exact decimal value during normalization. Reject NaN,
  infinity, invalid numeric syntax, and inconsistent ranges.
- Intel bit `i` is at `start + i`, with increasing significance. Motorola
  starts at its most-significant DBC bit; successive bits use `b - 1` when
  `b % 8 != 0`, otherwise `b + 15`, with decreasing significance. Validate
  every occupied bit against the message payload. Do not confuse the Motorola
  start coordinate with the lowest ordinary payload bit.
- The importer specification must declare its supported DBC constructs.
  Representability in this format does not imply importer/runtime support.
  Unsupported selected constructs require diagnostics, never guessed decoding.

Canonical bindings use existing Boolean, Number, and Enum signal contracts.
Numeric conversion must declare units, normalization, invalid ranges, and
target precision. A conversion that cannot preserve the declared contract
fails validation. Enum bindings map raw values to explicit canonical choice
keys and provider values; provider values must fit the existing enum contract.
Unknown or reserved raw values require an explicit invalid-value policy.

Bindings reference reviewed handler identifiers and revisions for derived or
exceptional behavior. Examples include combined turn state, switch-versus-lamp
distinctions, disabled flags, and reserved-status handling. Artifacts must not
contain arbitrary executable expressions or C++ supplied by a DBC source.

The contract distinguishes DBC extraction from native reviewed handlers.
A native binding references its handler and evidence rather than inventing
DBC coordinates. The existing C++ catalog export is a catalog-only projection:
it must not fabricate a complete executable profile from missing extraction
metadata or infer freshness policy from current readings.

## Automatic recognition and conflict classification

The `dbc-import` module will populate canonical bindings through versioned,
reviewed recognition rules. Owners do not manually map every imported field.
Names, units, enumerations, and source context are recognition evidence;
spelling similarity alone does not establish controller meaning. Recognition
must not turn brake pressure into brake pressed or indicator lamps into turn
requests without a reviewed semantic policy.

| Condition | Required outcome |
| --- | --- |
| Identical repeated message/field definitions on the selected bus | Deduplicate by normalized mechanical meaning and retain all provenance references. |
| Incompatible definitions for the same selected bus, identifier format, and identifier | Reject generation with references to both sources. Source order never chooses a winner. Compatible fragments of one message may compose; contradictory fields, message sizes, or unsupported overlaps do not. |
| Multiple competing interpretations of one canonical input | Exclude that binding and report ambiguity; never choose by source order or heuristic score alone. |
| Valid but unrecognized source field | Report it as unmapped; do not invent a controller input. |
| Unsupported construct required for a selected binding | Reject the selected binding/artifact with an explicit unsupported-construct error; never emit a partial executable interpretation. |
| Malformed artifact, invalid policy/handler reference, or resource overflow | Reject generation. Existing output files must not be replaced by partial results. |

Ambiguity and missing inputs disable only their dependent generated factory
features. Structurally contradictory sources are generation errors rather
than uncertain recognition. User-authored unknown keys, invalid rules, and
unsupported capabilities remain strict configuration errors; do not silently
discard arbitrary rules to make an incompatible configuration load.

A profile with zero exposed inputs is valid only as an explicitly reported
all-features-disabled result. It generates no dependent factory actions and
cannot cause fallback to Mazda inputs or policies. The later integration
specification must preserve startup black for that profile.

## Evidence, freshness, and profile compatibility

Evidence confidence, expected source timing, operational freshness policy,
and runtime availability are distinct. Every binding cites its evidence scope.
Source-only imports remain Reference. Timing stays unset unless supported by
reviewed evidence or a separately recorded owner-approved operational default.
Never promote a signal to Fresh merely because import or synthetic tests pass.
The Mazda brake `fresh_or_unverified` exception is profile-specific and must
not propagate to a different profile.

Use two identities with separate purposes:

- **Compatibility identity:** a digest of the resolved semantics, including
  profile/bus identity, extraction coordinates, signedness, scale/offset,
  units, ranges, canonical bindings, enum meaning, capabilities, evidence
  eligibility, semantic-handler revisions, freshness and invalid/availability
  policies. A scaling or interpretation change must change this identity even
  when the visible catalog keys and types are identical. Runtime ids are not
  persisted and do not establish compatibility.
- **Artifact identity:** a digest of deterministic normalized input content,
  exact source byte digests, recognition/generator revisions, policies, and
  validation-relevant limits. It identifies reproducible build inputs rather
  than a generation time or output directory. Source path, comment, or license
  changes alone do not imply changed signal semantics, but provenance changes
  remain attributable in the artifact identity.

Canonical serialization for each digest uses defined field order, numeric
encoding, and array ordering. Exclude the computed identity fields themselves
from their hash preimages. The implementation must publish golden preimages
and digests so independent tools can reproduce them.

The later integration module must bind persisted overrides to compatible
profile semantics, with explicit treatment of legacy untagged Mazda overrides.
This module does not change the persisted configuration format or weaken its
validator. Legacy BLE layouts, canonical keys, and enum choices remain stable.

## Generated projections and bounded resources

The same resolved canonical metadata supplies C++ `SignalMetadata`, the YAML
compiler's signal-catalog JSON, a capability report, and reference inventories.
Use the existing compiler manifest shape for its catalog projection; do not
make the compiler consume mechanical CAN definitions or provenance internals.

Generate private registration declarations and descriptors from one binding
description where applicable, rather than hand-maintaining separate channel
aliases, members, host/ESP tuple type lists, values, counts, and positional
subscription wrappers. New generic-only signals need not add named public
facade callbacks. Keep public include boundaries intact.

As a first migration slice, reuse the existing C++ catalog serialization to
derive the compiler manifest without replacing Mazda decoding. Generated
profiles later own their own catalog projection; no signal may require editing
both a generated projection and an independent handwritten copy.

Only the selected profile's exposed signals consume generated runtime storage.
Enforce the actual consumer bounds, including the current BLE catalog limits:
64 signals, 32 choices per enum, 63-byte keys, 31-byte tokens, and a 16 KiB
catalog document. Validate the encoded document using the existing exporter.
Do not truncate a catalog or automatically raise budgets. Runtime-specific
message/channel/subscriber limits remain explicit inputs from the runtime
contract; this schema must not guess new capacities.

Generation is deterministic, escapes source text as data, uses reviewed
identifiers, and never evaluates imported text. Validate all outputs before
publishing them. An error leaves no usable mixed set of old and new artifacts.
Build-generated files live outside source directories and declare their inputs
for incremental rebuilding. This contract does not prescribe a new parser
dependency or a new firmware allocation strategy.

## Locations, commands, and validation

Expected module-owned locations are `tools/vehicle_profiles/` for host schema,
validation and projections, `tests/tools/` for unit tests, and
`tests/fixtures/vehicle-profiles/` for reviewed synthetic data. Generated files
belong under the selected build directory. These locations describe proposed
implementation, not files already available in this revision.

Follow existing Python tooling style and C++17 conventions. In particular,
keep canonical keys such as `vehicle.engine_rpm` and enum choice keys such as
`warning` as data; do not derive persistent keys from C++ symbols or ordinals.
No additional production dependency is proposed by this specification.

Existing validation commands to use when implementing this module:

```sh
python3 -m unittest discover -s tests/tools -p '*_test.py'
cmake -S . -B /tmp/mazda-profile-manifest-host -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DMAZDA_BUILD_HOST_TESTS=ON
cmake --build /tmp/mazda-profile-manifest-host --parallel
ctest --test-dir /tmp/mazda-profile-manifest-host --output-on-failure
git diff --check
```

Use a fresh host directory and the pinned companion core as described in
[supported builds](../../development/supported-build.md). Public-header or
component changes also require [architecture validation](../../development/architecture-validation.md).
Later firmware integration requires the production and profiling ESP-IDF
5.5.4 builds, IRAM-placement checks, and RAM/IRAM reports from
[firmware builds](../../development/firmware-build.md) and
[flash-cache layout](../../development/flash-cache-layout.md). Those
measurements belong to implementation; this document claims no new headroom.

## Acceptance criteria

1. A versioned validator accepts the supported normalized representation and
   rejects malformed, conflicting, unsupported selected, and over-budget
   artifacts with stable machine categories and source-specific diagnostics.
2. Catalog projections agree with the existing C++ exporter and compiler
   metadata, preserve the Mazda compatibility baseline, and need no manual
   counts or edits in BLE, replay, or rule-engine implementation for ordinary
   signal additions.
3. Independently authored fixtures exercise two distinct profiles and one
   profile composed from two DBC sources, source-order independence, identical
   duplicates, structural conflicts, ambiguous recognition outcomes, zero
   usable inputs, and profile isolation. Full importer recognition is tested
   by its later module, not claimed by manifest fixtures.
4. Independent numeric/bit-coordinate and compatibility expectations cover
   Motorola crossing bytes, signed and unsigned full-width values, changed
   scale with unchanged keys, enum policy, unset freshness, changed handlers,
   and non-semantic provenance changes. Expected values must not be generated
   solely from the production manifest.
5. Repeated generation from identical inputs produces identical bytes and
   identities. Changed source bytes or generator revision affect artifact
   identity; changed resolved meaning affects compatibility identity. Failed
   generation cannot leave a consumable partial artifact set.

## Boundaries and remaining decisions

Always retain source attribution, independent compatibility expectations,
strict validation, generic include boundaries, and bounded resource checks.
Never import vehicle transmit operations, propagate the Mazda brake exception,
or represent host checks as hardware/vehicle evidence. Retain listen-only CAN,
startup black, fault fail-off, bounded retries/watchdogs, and recovery ordering.

Implementation tasks are tracked in
[#310](https://github.com/Yuke-hd/mazda-can-accessory-controller/issues/310)
and milestone 5. The remaining module
specifications will choose the supported DBC syntax/recognition set,
runtime storage layout, concrete build selection command, and treatment of
legacy untagged configuration. A second real vehicle is intentionally
unselected; synthetic alternative profiles establish software behavior only.
