# Build selection and configuration compatibility

Status: proposed module contract for owner review. This specifies future
`profile-integration` against [DBC import](dbc-import.md), the
[runtime](profile-runtime.md) and the existing
[artifact transaction](../../development/supported-build.md#offline-vehicle-profile-artifact-sets).
It adds no selection option, importer, runtime, storage format or firmware
composition today. Implementation starts only after the owner accepts the
interfaces, migration policy and acceptance examples below.

**Milestone 6 supersession:** this draft predates the accepted explicit-mapping
importer. Its build-request JSON and per-profile `context`, `policy_pack`,
recognition-pack and automatic-recognition concepts are obsolete and must not
be implemented as current importer inputs. The current importer takes DBC
sources plus one user-authored `mapping.json`, which declares the selected bus,
field-to-target bindings, conversions, enum choices and optional user-claimed
freshness; it derives the validation registry from that mapping and the target
catalog. It has no request file, recognition pack, vehicle context or
hand-written registry. `recognition_rule_revision` remains a normalized-profile
identity field, but Milestone 6 derives it from the normalized mapping and
sources; it does not represent recognition rules or a recognition pack. See
[DBC import](dbc-import.md) for the current contract. Milestone 8 owns the
complete integration rewrite, including build selection, factory configuration
and persistence, against the accepted Milestone 6 importer and Milestone 7
runtime.

## One selection at the composition root

Introduce one CMake cache string, `VEHICLE_PROFILE`, shared by host and ESP-IDF
composition. Its default is the reserved value `native:mazda`. Any other
nonempty value names one build-request JSON file, resolved relative to the
top-level project's source directory; absolute host paths are accepted.
Explicit empty values, missing files, unsupported versions and invalid profiles
fail configure. An explicit selection never falls back to Mazda. Changing
selection requires configure and a new image.

These are proposed commands, not commands supported by the current tree:

```sh
cmake -S . -B /tmp/controller-demo -DVEHICLE_PROFILE=/path/to/demo.request.json
cmake --build /tmp/controller-demo --parallel

# After activating the documented ESP-IDF 5.5.4 environment:
idf.py -C firmware/weact-can485-v1.1 -B /tmp/controller-demo-fw \
  -DVEHICLE_PROFILE=/path/to/demo.request.json build
```

The existing WeAct target, pins, bitrate and receive-only CAN input remain the
board contract. A logical bus does not choose another driver, port, bitrate or
physical input. Two sources describe one selected logical bus. No Kconfig
vehicle matrix, runtime upload/switching, automatic detection or extra bus is
introduced.

`native:mazda` selects a reviewed native manifest with stable profile id
`mazda.native.v1` and the existing Mazda decoder/service. A catalog-only export
cannot invent extraction or freshness metadata: reviewed native handler,
evidence and policy revisions must be recorded before computing its semantic
identity. This selector preserves the existing regression baseline without
claiming every Mazda model is supported.

## Build-request contract and limits

A request is strict UTF-8 JSON: reject duplicate/unknown keys and wrong types.
Version one has exactly these root fields:

| Field | Required value |
| --- | --- |
| `format`, `version` | `vehicle-profile-build-request`, integer `1` (not Boolean). |
| `profile_id`, `selected_bus` | Normalized-v1 stable identifiers; integration additionally bounds `profile_id` to 1–63 ASCII bytes. |
| `context` | Exact D1 `make`, `model`, `platform`, `model_year`, `market`, `trim` tuple; explicit `null` means unknown. |
| `policy_pack` | Registered `id`, immutable `revision`, and `path` to reviewed pack data; no executable path or source-supplied expression. |
| `policies` | Path to the explicit normalized validator registry. |
| `sources` | Array of records below, with unique source ids. |
| `factory_recipe` | Registered `id` and immutable `revision` of a reviewed feature recipe. |
| `limits` | Exact nonnegative integer mapping below; reject Booleans. |
| `generator_revision` | Registered stable recipe revision; actual generator/pack bytes are also hashed. |

Every source has exactly `id`, `path`, `logical_path`, `bus`, `revision`,
`license`, `encoding`, `expected_sha256`. These are D1's source inputs;
`revision` and `expected_sha256` may be `null`, encoding is `utf-8` or `cp1252`.
Paths resolve relative to the request file's directory. Resolved machine paths
are not emitted or hashed as provenance; `logical_path` is the normalized
package-relative artifact path and remains attributable in artifact identity.
Read source bytes once and compute/check their digests. Owners choose sources,
context and reviewed recipes rather than assigning individual source fields.

These proposed integration ceilings require owner review and boundary tests.
A request may lower them; raising them needs a reviewed limits revision and
fresh evidence. Every effective value enters explicit `validation_limits`
and artifact identity.

| Limit key | Proposed ceiling |
| --- | ---: |
| `source_count` | 16 |
| `source_bytes_each` / `source_bytes_total` | 4 MiB / 16 MiB |
| `token_bytes` / `source_records` / `source_fields` | 4,096 / 100,000 / 65,536 |
| `diagnostics` / `policy_pack_bytes` | 4,096 / 1 MiB |
| `selected_messages` / `selected_fields` / `bindings` | 64 / 64 / 64 |
| `notify_bindings` / `handler_inputs` | 17 / 8 |
| `enum_choices` / `key_bytes` / `unit_evidence_bytes` | 32 / 63 / 31 |
| `catalog_bytes` | 16,384 |

Host inventory ceilings do not authorize additional runtime messages. D2's
subscriber slots, fixed storage, queue, payload and post-link gates also apply.
Overflow rejects the entire artifact; never truncate, drop user rules or raise
firmware budgets silently.
The profile-id bound is an integration/storage constraint, rather than a
change to normalized-v1's general identifier grammar. Apply it to native and
imported selections before generation, so every selected id fits an envelope.

## One reproducible generation for every consumer

Configure resolves request/pack/recipe, imports sources, validates mechanics
and policies, computes identities, projects metadata, and compiles factory
configuration using that compiler catalog. Future integration adds runtime
tables, canonical factory JSON and a compiled selection descriptor (profile id
and both identities) to A7's complete immutable set. All additions join the
same inventory/transaction before consumers use them.

Request, source, policy/recognition/recipe, board output-catalog and generator
inputs are declared configure/build dependencies. Semantic request contents
and logical provenance enter the appropriate normalized identities; machine
paths, JSON whitespace, unordered records and timestamps do not form a second
identity domain. Context uses D1's immutable resolved recognition revision.
Recipe/board inputs also affect the full artifact inventory.

Resolve A7's generation directory once. Provider runtime tables/metadata, rule
compiler catalog, replay provider, BLE `SignalCatalogExport`, factory document
and embedded selection descriptor use that directory. Neither replay nor BLE
can substitute a global Mazda catalog. Reject a mixed identity set before
compilation; firmware verifies provider/factory descriptors agree before CAN
starts. Corrupt sets use A7's fail-closed/fresh-output-root recovery.

Generation runs on the host. Firmware configure executes no target binary and
does not build the host test suite. Existing companion v1 discovery bytes stay
unchanged; do not put identity bytes into reserved fields or catalog keys. Any
explicit identity characteristic needs a separate versioned protocol contract.

## Factory features and empty profiles

A reviewed recipe declares each feature's complete canonical input, type,
capability, choice, freshness and output requirements. Evaluate a feature as a
unit: missing/ambiguous inputs disable all its dependent actions/rules/outputs
with a stable capability diagnostic. Do not leave dangling output bindings.
This applies only to generated factory features. User-authored unknown keys,
invalid operands, unsupported capabilities and over-capacity rules still fail.

Current Mazda turn/RPM/brake output is the independent baseline. Only its
reviewed Mazda recipe may use the approved `fresh_or_unverified` brake opt-in;
other profiles need their own reviewed policy. Neither source timing nor
recognition confidence promotes brake freshness to `Fresh`.

Zero bindings is a successful explicit `all_features_disabled` selection.
Generate valid canonical v1 factory JSON with empty actions, rules and lighting
outputs plus valid board sink settings. All lighting pixels start/remain black,
discovery is empty, replay observes no inputs, and no Mazda policy is selected
implicitly. A valid empty configuration may start the bounded receive-only
provider. Factory validation/apply failure instead keeps CAN stopped and BLE
repair available under the existing failed-off startup contract.

## Persisted override binding without changing BLE v1

Keep canonical controller documents at version 1 and preserve Config transfer,
read-back, CRC and restart behavior. Do not add fields to today's strict v1
document. The future private storage/composition boundary binds each new save
to the profile used for parsing, semantic validation and never-attached dry-run
apply. One profile lasts for the entire boot, including validation and commit.

New slot blobs use this proposed internal envelope (little-endian integers):

| Offset | Field |
| ---: | --- |
| 0 | Eight ASCII bytes `VPROFOV1`. |
| 8 | `u16` envelope version `1`. |
| 10 | `u16` profile-id byte length, 1–63. |
| 12 | `u32` canonical JSON byte length, 1–4,096. |
| 16 | 32 raw bytes of semantic compatibility SHA-256. |
| 48 | 32 raw bytes of canonical JSON SHA-256. |
| 80 | Profile-id ASCII bytes, then exactly the declared canonical JSON bytes. |

Reject trailing bytes, invalid ids/lengths/version/digests and malformed
payloads. The new blob maximum is 4,239 bytes; canonical JSON writes remain
4 KiB. Envelope overhead has a separate private bound and enters two-slot/NVS
resource tests before implementation. It does not raise the parser's 16 KiB
input/legacy-read bounds or claim guaranteed space in shared 64 KiB NVS.
The digest detects inconsistency, rather than authenticating a document.

At boot require exact profile id and semantic compatibility digest before
exposing payload JSON to the existing parser/apply path. Artifact identity is
not the compatibility key: provenance-only changes can preserve semantics,
while changed extraction, scale, enum meaning, handlers or freshness policy
cannot accidentally reuse an override. Runtime ids are not persisted. Matching
envelopes still undergo current semantic, output-zone, capacity and attach
checks.

The private adapter wraps/unwraps and verifies the full envelope and marker
with the current inactive-slot/read-back/rollback sequence. BLE sees canonical
JSON only; read-back/active CRC exclude the envelope and stay stable until
restart. Validation/dry-run rejection writes nothing, leaves the running
configuration unchanged and does not restart. `StorageFailed` also leaves the
running configuration unchanged without restart, but next-boot selection is
uncertain if the new marker committed and rollback failed: the previous
selection, a verified new envelope, or factory may be loaded. An invalid or
incompatible slot is never applied. Preserve the
[same-session upload/revert recovery](../companion/config-transfer.md#commit)
and bounded reporting behavior; do not promise transaction guarantees beyond
the existing backend. Revert retains the verified active-marker clear; slot
bytes remain and no automatic NVS erase or secure-wipe claim is added.

## Legacy Mazda bridge and fallback

A bare canonical v1 slot is legacy storage, read under the existing 16 KiB
bound. Accept it only when all of these hold:

1. Selection is built-in `native:mazda`, exact profile id `mazda.native.v1`.
2. A checked-in owner-approved bridge allowlist contains the selected exact
   compatibility digest and reviewed native handler/policy baseline. No
   wildcard, key-overlap inference or artifact-id alias is allowed.
3. The document passes current parser, semantic/apply/capacity and attach checks.

The bridge is disabled until its computed digest and migration vectors receive
owner review; placeholders cannot enable it. Other profiles reject legacy bare
storage even if keys agree. Successful legacy boot retains the original slot
without auto-tagging/resaving it. The next explicit successful BLE save creates
an envelope for the current profile. A fresh v1 upload is validated against
the current provider and bound on save, rather than being a legacy exception.

Missing, unreadable, malformed or incompatible overrides select that profile's
factory, retain stored bytes and record diagnostics. Compatibility rejection
uses the existing invalid-override boot flag and `InvalidValue` code; exact
internal `profile_mismatch` / `legacy_profile_unbound` details go to the bounded
firmware log. Hardware reads keep the storage-failure flag. New wire enums
require a separate protocol version decision.

Apply recovery follows separate [PR #306](https://github.com/Yuke-hd/mazda-can-accessory-controller/pull/306):
after override apply failure clear the detached partial engine/sink, put the
factory model in the stable owning active-config object, and apply from clean
state. Report factory active only after success, and retain the override for
diagnostics/retry next boot. If factory parse/apply/attach fails, stay black,
refuse CAN and retain BLE repair/revert. Do not merge/reimplement #306 here;
integration coding rebases onto its accepted behavior and repeats its tests.

## Acceptance and implementation gate

Owner review must accept selection/request syntax, proposed ceilings, envelope
and the exact legacy bridge before integration coding. D1/D2 retain their own
gates. Independently authored synthetic fixtures must prove the following:

| Evidence | Required acceptance |
| --- | --- |
| Host generation | Two profiles with distinct context/policy, each from two sources; reordered records/whitespace reproduce bytes/identities; conflicts preserve old outputs; consumers share one generation. Profile ids of 1/63 ASCII bytes are accepted, while empty/64-byte/non-ASCII ids fail before generation. |
| Host configuration | Changed scale/meaning rejects old envelope; provenance-only changes preserve compatibility; wrong id/digest/malformed envelope falls back; legacy bridge accepts only its exact Mazda baseline; v1 transfer/read-back/CRC stay unchanged. |
| Host lifecycle/output | Missing inputs disable complete factory features; invalid user rules reject; empty profile has no Mazda inputs/policy/lighting; #306 clean fallback and factory failure remain covered. Inject marker-commit/read-back/rollback failures: running config stays unchanged, next boot loads only old/verified-new/factory, and same-session upload/revert recovery remains available. |
| Firmware software | Fresh ESP-IDF 5.5.4 production/profiling builds for native Mazda, populated synthetic and empty selections; architecture/include/storage/IRAM gates and actual RAM/flash/stack deltas against the accepted baseline. |
| Bench / vehicle | Separate owner-authorized follow-up verifying actual board/wiring/port/power. No such evidence is supplied here. |

The current milestone delivers foundation tooling and module contracts. A
general importer, generated runtime, composition selector and real second
vehicle validation remain future implementation after the relevant reviews.
