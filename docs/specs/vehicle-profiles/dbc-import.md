# Offline DBC import with explicit mapping

Status: proposed module contract for owner review, revised for explicit
`mapping.json` import (milestone-6 decisions D1-D6, tracked in #310). This
document specifies the `dbc-import` module; it does not add a parser, mapping
loader, dependency, or vehicle support. Importer implementation tickets may
start only after the owner accepts this contract and its acceptance examples.

## Boundary and inputs

The importer runs on the host before a build. It produces the
[normalized version-one profile](v1-format.md), its two computed identities,
and a source/mapping report. The existing `SignalProvider` remains the
consumer boundary. Build selection and the integration of the command line
belong to the later integration contract; runtime storage and handlers belong
to the runtime contract. The importer never opens a CAN interface or emits
transmit code.

The only per-profile inputs are the DBC source files and one user-authored
`mapping.json`. There is no request file, recognition pack, vehicle context or
hand-written registry. The mapping author explicitly maps DBC fields to
signals the controller recognizes; the importer does not guess.

### `mapping.json`

```json
{
  "version": 1,
  "profile_id": "synthetic.demo",
  "selected_bus": "body",
  "sources": [
    {"path": "demo-a.dbc", "sha256": "<64 hex digits>", "bus": "body",
     "license": "Apache-2.0; synthetic fixture", "encoding": "utf-8"}
  ],
  "bindings": [
    {"target": "vehicle.engine_rpm", "message": "Engine", "field": "rpm",
     "unit": "rpm", "conversion": {"scale": "1", "offset": "0"},
     "timeout_ms": 500}
  ]
}
```

| Member | Meaning |
| --- | --- |
| `version` | Mapping format version; `1`. |
| `profile_id` | Stable profile id, `<make>.<model>` for a real profile (see Locations). |
| `selected_bus` | The one logical bus whose fields can bind. Other buses remain inventory. |
| `sources[]` | `path` (relative to the mapping file; also the logical path written to outputs), `sha256` (pins the exact bytes), `bus` (logical bus of that source), `license` (required license/provenance note) and optional `encoding` (`utf-8` default, or `cp1252`). |
| `bindings[]` | `target` (a controller signal key), `message` and `field` (names as they appear in the DBC), `unit` (the target unit; `""` for boolean and enum targets), and the optional members below. |

Optional binding members:

| Member | Meaning |
| --- | --- |
| `conversion` | `{"scale": "<decimal>", "offset": "<decimal>"}`. Both are JSON strings in the source lexer's exact-decimal grammar; JSON numbers are rejected and a zero scale fails. The source unit is the DBC field's unit and the target unit is `unit`. With equal units, `conversion` may be omitted or be an explicit identity (scale 1, offset 0); an explicit non-identity conversion on equal units fails `incompatible_target`. With different units `conversion` is required and never inferred; a missing one fails `incompatible_target`. There is no unit library and no coercion. A boolean or enum target has unit `""`; if its DBC field has a unit, an explicit identity conversion is required, otherwise the binding fails `incompatible_target`. |
| `timeout_ms` | Optional user-declared freshness timeout (D6): a JSON integer (not a boolean) with `0 < timeout_ms <= 4294967295`, otherwise `invalid_freshness`. Absent means the signal stays freshness-unverified. The importer never derives one. |
| `choices` | For enum targets, a map from DBC `VAL_` labels to catalog choice keys. |

The loader is strict: unknown members, duplicate JSON keys, nonfinite numbers
and wrong types fail. Import limits (source count, source bytes, token bytes,
records, fields, total diagnostics) and the `utf-8` encoding default are
constants in importer code (the version-one values) and are not mapping
input; the effective limits still enter artifact identity through
`validation_limits`.

A source's logical bus comes from the mapping. A DBC filename, node name,
sender or `BS_` value does not select a physical CAN interface, bitrate,
vehicle model or bus. Only the selected bus can contribute executable
bindings.

Read source bytes once, verify the pinned SHA-256, then decode with the
source's declared encoding. A missing, changed or wrong file fails
`source_digest_mismatch`. Decoding errors fail import; replacement characters
and guessed encodings are forbidden. Hash the original bytes, before decoding
or newline handling. A `sources[].path` is relative to the mapping file and
may not leave the profile directory (no absolute path, no `..`). Local
absolute paths stay in host diagnostics only and never enter generated
artifacts. A recorded license note is required even for a privately licensed
source. It establishes provenance, not permission to redistribute: committed
DBCs and derived material still follow the repository's
[vehicle-data and licensing policy](../../development/license-and-vehicle-data.md).

### Locations

| What | Where |
| --- | --- |
| Real profiles | `vehicle-profiles/<make>/<model>/`: `mapping.json` plus the DBC files it references, when licensable. Self-contained; `profile_id` is `<make>.<model>` and must match the directory. |
| Test fixtures | `tests/fixtures/vehicle-profiles/dbc/` (synthetic `demo-a.dbc`, `demo-b.dbc`, `mapping-*.json`, `PROVENANCE.md`). |
| Generated output | Under `build/` only, never committed. |

Only the DBC files (when their license allows) and `mapping.json` are
committed. The derived registry, profile, report and any generated code are
build products. Nothing in `vehicle-profiles/` is code, and no repository code
refers to a specific profile; the importer takes the directory as an
argument. The `vehicle-profiles/` directory appears with the first real
profile.

### Derived registry

The importer builds the `ValidationPolicies` registry from the mapping; no
per-profile registry file exists. Targets come from
`tools/controller_signal_catalog.json`, which supplies type, capabilities and
enum choices.

| Registry key | Derived as |
| --- | --- |
| `profile_revisions` | `profile_id` to (`semantic_policy_revision`, `recognition_rule_revision`), each `mapping-v1:<hash>` over a different preimage (see Revision). |
| `handlers` | Empty; imported profiles have no native handlers. |
| `evidence` | Always `("mapping:<profile_id>", "user_mapped", "source_only")`; plus `("mapping:<profile_id>", "user_mapped", "operational")` only when at least one binding declares a `timeout_ms`. |
| `units` | The set of binding `unit` values, including `""` when any boolean or enum binding exists. |
| `unit_conversions` | One row (`field_unit`, `binding_unit`, `scale`, `offset`) per resolved unit pair; an omitted conversion on equal units is scale 1, offset 0. See Conversion pairs. |
| `invalid_value_policies` | `["unavailable"]`, the only value the validator accepts. |
| `freshness_policies` | One row per binding with a `timeout_ms`, keyed by (`profile_id`, `target`): (`profile_id`, `target`, `timeout_ms`, `"reviewed_operational"`, `"mapping:<profile_id>"`). See Freshness. |

The mapping chooses which field feeds which target; the importer and the
existing validator still decide type, unit, conversion and choice
compatibility. Incompatible mappings fail and are never coerced.

#### Conversion pairs

The validator keeps one conversion per (source unit, target unit) per profile,
so the importer groups bindings by their resolved pair and emits exactly one
row per pair, identity pairs included. It compares scale and offset as exact
rationals (`"1.0"` equals `"1"`) and writes the row in canonical decimal text,
so the bytes do not depend on binding order. There is no first-wins rule and
no per-binding row.

Per-binding rules (`incompatible_target`) already force the identity on
equal-unit pairs and on boolean or enum targets, so those pairs cannot
disagree. Only number bindings whose field and target units differ can: if
their conversions in one pair disagree, the import fails with
`conflicting_conversion`, listing every binding target in the group, sorted,
with their mapping locations.

### Deferred

The builder web app, which would write the same `mapping.json`, is deferred to
a later milestone. This contract covers the command-line importer only.

## Supported DBC subset

Version one parses the complete input into a source inventory before applying
the mapping. Whitespace and line endings have no semantic effect; outside
strings, `//` comments end at the next line ending or end of file. Quoted
strings accept escaped quote and backslash; unsupported escapes, unterminated
strings, trailing unparsed tokens and malformed records are errors. Names use
DBC word identifiers (`[A-Za-z_][A-Za-z0-9_]*`); imported text is data and is
never evaluated as Python, C++, a template or an expression.

| Record/form | Version-one treatment |
| --- | --- |
| `VERSION`, `NS_`, `BS_`, `BU_` | Parse as source metadata. Keywords listed inside `NS_` are declarations of vocabulary, not uses of those constructs. `VERSION` is recorded in the report; it does not replace a package revision or byte digest. |
| `BO_ id name : length sender` | Parse exact integer ids and payload length. Standard classic CAN with length 0–8 is executable. A raw id with DBC bit 31 set denotes an extended id; remove only that flag and validate the remaining 29 bits. Standard ids must fit 11 bits. Lengths outside 0–64 fail; lengths above eight and extended/CAN-FD forms are inventory-only as described below. Payload length alone does not identify a protocol. |
| Plain `SG_ name : start\|length@order sign (scale,offset) [min\|max] "unit" receivers` | Support integer fields of 1–64 bits, Intel `@1` and Motorola `@0`, signed and unsigned. Preserve source units, names, declared ranges and exact decimal values. Check every occupied bit against the payload. |
| `VAL_ message signal raw "label" ... ;` | Support direct integer value tables, including signed and unsigned 64-bit values. Preserve labels and exact raw integers. References must resolve; conflicting labels for one raw value fail. |
| `CM_`, `BO_TX_BU_`, `SIG_GROUP_`, standalone `VAL_TABLE_` | Parse as source metadata. Comments, senders, receivers, groups and unreferenced tables create no runtime behavior or canonical bindings. Source bytes retain their attribution through the digest. |
| `BA_DEF_`, `BA_DEF_DEF_`, `BA_` | Parse definitions, defaults and scoped values. Consume only the allowlisted attributes below; retain other well-formed attributes in the report without executing them. Undefined attributes and invalid scope/value types fail. |
| `SIG_VALTYPE_ ... : 0;` | Explicit integer form is supported. Types 1/2 (IEEE float/double) are known unsupported wire forms. Other type codes fail. |
| `SG_` multiplex markers (`M`, `mN`, `mNM`) and `SG_MUL_VAL_` | Recognize and report, but do not generate any field from the affected message. Version one has no executable multiplex representation. |
| Environment variables/data (`EV_`, `ENVVAR_DATA_`, `EV_DATA_`), signal-type templates/references (`SGTYPE_`, `SGTYPE_VAL_`, `SIG_TYPE_REF_`, `SIGTYPE_VALTYPE_`), relationship records, container protocols, LIN/J1939 semantics | Unsupported record families. Fail import with a source location; do not skip lines or guess their effect. |
| Unknown record/keyword, binary extension or invalid numeric syntax | Fail import, including in an inventory-only source. Bus filtering must not hide a parse error. |

Allowlisted attributes are deliberately narrow. `GenMsgCycleTime` accepts a
nonnegative integer number of milliseconds and records source-declared timing
only. `VFrameFormat` is interpreted through its declared enum choices, using
the exact labels `StandardCAN`, `ExtendedCAN`, `StandardCAN_FD` and
`ExtendedCAN_FD`; do not assume vendor enum ordinals. A contradiction with the
encoded id or payload length fails. An unknown format label is unsupported.
`BusType` and `ProtocolType` accept absent metadata or the label `CAN`; other
protocol labels fail as unsupported and cannot configure hardware. Start
values, invalid values, send types and all other attributes do not become availability,
freshness, enum or handler policies. A mapping that needs an
unsupported attribute must report `unsupported_required_binding`.

Source integer tokens match `[+-]?[0-9]+`, with a negative value allowed only
where the field permits it. Identity, start and length values are nonnegative.
Source decimal tokens match the complete token grammar
`[+-]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?`.
Whitespace inside a token, hexadecimal/underscore notation, missing mantissa
or exponent digits and repeated signs fail. This source lexer is intentionally
separate from the stricter normalized-v1 numeric-string grammar:

```text
accept: .5, +01, 1., -1.25e+2  -> exact values 0.5, 1, 1, -125
reject: ., 1e+, --1, 1_0, 0x10, NaN, Inf
```

Parse accepted tokens directly into an exact decimal/rational representation,
never through binary floating point. Emit version-one strings with the same
exact value; source spellings such as `.5` or `+01` normalize to `0.5` or `1`.
Keep the original bytes and token locations in the source inventory. Nonfinite
values, zero scale and inconsistent ranges fail.
The existing normalized validator owns the Intel/Motorola walk and exact
64-bit/range checks; the parser must not introduce a second bit convention.

The importer applies fixed nonnegative integer limits for source count, source
bytes, token bytes, records, fields and total diagnostics. Numeric tokens must
also meet the normalized validator's existing exact-representation limits.
Exceeding any limit is a fatal `resource_overflow`, never truncation. The
effective limits enter artifact identity through `validation_limits`.

## Composition before mapping

Composition keys are `(logical_bus, identifier_format, wire_identifier)`.
Parse and compare selected-bus definitions before dropping unsupported or
unrecognized messages, so filtering cannot hide a contradictory source.
Input order never supplies precedence.

1. Require the same payload length for every fragment of one message key.
   Different message names are retained as provenance and do not select a
   winner.
2. Compare fields by the normalized mechanical signature: occupied bits,
   byte order, signedness, exact scale/offset/range, preserved source unit and
   complete raw-value table. Identical repeats deduplicate, including repeated
   records in one source. Retain every source id and occurrence location.
3. Compose nonoverlapping compatible fields. Partial overlaps, different
   mechanics on the same bits, conflicting raw labels, or incompatible
   definitions of the same source signal name in one message are fatal
   `source_conflict`, with references to both definitions. Unit aliases are
   not silently substituted into source records to make an overlap agree.
4. Generate stable record ids from source id, bus, message identity and source
   signal name, not input-list position or machine path. Choose representatives
   in stable id order. Keep the full original-name/location alias inventory
   beside the strict v1 profile, which has one representative field name.

Use collision-safe ids: `dbc-message-v1:<SHA256>` hashes the compact UTF-8 JSON
array `["dbc-message-id-v1", source_id, bus, identifier_format, wire_id,
message_name]`; `dbc-field-v1:<SHA256>` hashes
`["dbc-field-id-v1", source_id, bus, identifier_format, wire_id, signal_name]`.
Strings retain their decoded exact value, integer ids use JSON integers and
there is no ASCII escaping. Repeated equivalent records in one source share
an id and keep multiple inventory spans. Delimiter concatenation is forbidden.

Known unsupported wire forms (extended id, length above eight, multiplexing,
IEEE payload values) remain in the report, never in executable v1 messages.
Do not decode the plain fields of a multiplexed message as an apparent
nonmultiplexed subset. An unused unsupported message produces an `unsupported`
diagnostic. If a mapping binding names such a message or
field, fail the artifact with `unsupported_required_binding`; a successful
artifact must not imply partial support for that binding.

## Explicit mapping

The mapping author owns everything car-specific; importer code owns only
car-independent rules (the DBC subset, parser, merge and conflict rules, the
target catalog, validator rules, registry derivation, identities and
publication). DBC text is never evaluated. Names, spelling similarity, units
or file order never select a target: only a `bindings` entry does.

Resolve each binding after composition. Look up `message` and `field` by their
DBC names within the selected bus and resolve them to `dbc-message-v1:` and
`dbc-field-v1:` ids; the composed field keeps every source reference. Names
that no longer resolve fail `stale_mapping`. A binding to a message or field
that is an unsupported wire form fails `unsupported_required_binding`; a
successful artifact must not imply partial support for it. Valid fields with
no binding produce `unmapped`.

Mapping errors are fatal, stable-coded and carry the binding's JSON location:

| Code | Cause |
| --- | --- |
| `unknown_target` | `target` is not in the controller signal catalog. |
| `duplicate_target` | Two bindings name one target. This replaces recognition ambiguity: nothing is ranked or tie-broken. |
| `incompatible_target` | The field's type, unit or conversion, or its `VAL_` labels versus the catalog choices, do not fit the target (for example a number field on `vehicle.brake_pressed`, or different units with no `conversion`). |
| `stale_mapping` | `message` or `field` does not exist in the pinned sources. |
| `conflicting_conversion` | Number bindings with different field and target units that resolve to one unit pair declare different exact scale or offset; the report lists every target in the group, sorted. |
| `invalid_freshness` | `timeout_ms` is not an integer, is a boolean, or is outside `0 < timeout_ms <= 4294967295`. |

A structurally malformed `conversion` (a JSON number instead of a string, a
bad decimal, a zero scale or an unknown key) is a mapping-load error: the file
is rejected for its JSON shape before binding resolution. It carries no
per-binding stable code and is reported as a fatal mapping-file error (the
loader raises `MappingError`, a `ValueError` subclass). A non-identity
conversion on equal units, and a boolean or enum target whose DBC field has a
unit but no explicit identity conversion, fail `incompatible_target`.

Brake pressure does not imply `vehicle.brake_pressed`, and indicator lamp
state does not imply a turn request; the mapping author must map fields
explicitly and the type check rejects mismatches.

### Revision

The profile revision is two digests, each `mapping-v1:` followed by the
SHA-256 of a normalized preimage. A preimage is the UTF-8 encoding of
`json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False)`
over the strict-JSON validated mapping. Normalization sorts `sources` by
logical path, `bindings` by target and the keys of each `choices` map. Scale
and offset are written in canonical decimal text, and a `conversion` that is
the identity is omitted, so an explicit identity and an omitted conversion
hash the same. Member order and source order never change a digest.

1. `semantic_policy_revision`: the preimage holds `version`, `profile_id`,
   `selected_bus` and `bindings` sorted by target. Each binding contributes
   `target`, `message`, `field`, `unit`, its canonical `conversion` (omitted
   when identity), `timeout_ms` (omitted when absent) and `choices` with
   sorted keys. It excludes `sources` entirely (path, `sha256`, `bus`,
   `license`, `encoding`).
2. `recognition_rule_revision`: the preimage is the whole normalized mapping,
   sources included, with the same normalization. This replaces the
   recognition-pack and vehicle-context preimage.

The derived `ValidationPolicies.profile_revisions[profile_id]` is the pair
(`semantic_policy_revision`, `recognition_rule_revision`). Source digests
enter artifact identity separately, through the normalized profile's sources;
the generation identity (the import CLI ticket) also covers mapping bytes,
source bytes and importer modules. They never enter the compatibility
preimage.

Consequently a license-only or path-only edit changes artifact identity and
leaves compatibility identity unchanged, while a unit-conversion or freshness
edit changes compatibility identity. Golden vectors pin both preimages
literally.

### Freshness

Freshness is user-declared. A binding may declare a freshness timeout; it is
the mapping author's claim, not reviewed evidence.

Freshness is the optional per-binding `timeout_ms` (see the binding members).
A zero, negative, boolean, non-integer or out-of-range value fails
`invalid_freshness`, the existing validator category.

The derived registry row for each binding with a `timeout_ms` is
(`profile_id`, `target`, `timeout_ms`,
`"reviewed_operational"`, `"mapping:<profile_id>"`), and the importer sets the
same basis and `evidence_ref` on the binding's `freshness` object. The existing
validator's basis literal `reviewed_operational` is satisfied by the evidence
entry `("mapping:<profile_id>", "user_mapped", "operational")`.
That literal does not mean reviewed evidence for imported profiles: the claim
is the mapping author's (D6). No validator code change is required, and the validator's
timeout bounds and basis checks apply unchanged. A signal without a declared
timeout stays freshness-unverified, and features that require `Fresh` stay off for it.
Nothing derives a timeout, for example from `GenMsgCycleTime`. The source
report states that imported-profile freshness is user-declared. Source timing,
operational freshness, confidence and current availability stay distinct.

This applies to imported profiles only. The Mazda compiled catalog, its unset
brake freshness and the owner-approved `fresh_or_unverified` opt-in on
`vehicle.brake_pressed` are unchanged. Imported profiles get no native
handlers and cannot express `fresh_or_unverified`, and import success never
promotes a signal to Fresh without a declared timeout.

The importer emits bindings and diagnostics, rather than factory actions.
Unmapped targets disable their dependent generated factory features in
projection/integration. User-authored invalid mappings remain strict errors.
Zero bindings is a valid explicit `all_features_disabled` profile, with no
fallback to Mazda bindings or policy; the integration contract must preserve
black startup for it.

## Diagnostics and output transaction

The source report uses stable `code`, `severity` (`error`, `warning`, `info`),
`affected`, `source_ids`, token locations and explanatory `reason`. Locations
use source id plus one-based line/column spans, never a private absolute path.
Sort reports by stage, source id, location, code and affected id. Prose is for
people; callers branch on codes.

| Code family | Outcome |
| --- | --- |
| `source_io`, `source_decode`, `source_digest_mismatch`, `source_provenance_missing` | Fatal source acquisition/provenance error. |
| `dbc_syntax_error`, `unsupported_dbc_construct`, `source_conflict`, `unsupported_required_binding` | Fatal parse/composition/support error with source locations. |
| `unknown_target`, `duplicate_target`, `incompatible_target`, `stale_mapping`, `conflicting_conversion`, `invalid_freshness` | Fatal mapping error with the binding's `mapping.json` location. |
| Existing normalized validator/policy/resource error categories | Forward the category and source ids; fail the artifact. |
| `unmapped_field`, `unsupported_message` | Map to strict v1 diagnostic categories `unmapped`, `unsupported`; omit the affected binding. |
| `all_features_disabled` | Required explicit result when no bindings remain. |

Do not extend the strict v1 Diagnostic object with report-only fields. Its
`sources` ids connect it to the richer source report. After mapping, call
`validate_profile` and `identify_profile` with the derived registry,
generator revision and effective limits. The manifest module owns catalog
projection and complete-set publication. A fatal import produces diagnostics
and no usable profile/artifact set; previously published outputs stay usable
as one complete generation. No parser cache keyed only by path/mtime is an
authoritative input.

## Parser evaluation and implementation proposal

Evaluation snapshot: 2026-10-27. These are primary upstream sources, rather
than a dependency installation or a claim about every DBC dialect:

| Candidate | Evidence and decision |
| --- | --- |
| Existing `tools/compare_mazda_dbc.py` | Its [documented Mazda comparison subset](../../protocol/dbc-metadata-verification.md) compares reviewed metadata. Its parser ignores lines outside that subset and has no complete import/mapping contract. Keep it as an independent Mazda regression check. |
| cantools 45.1.0, commit `68131411d777da8024e11ec79ddb7bebe92c2da0` | [Release](https://github.com/cantools/cantools/releases/tag/45.1.0), [MIT license](https://github.com/cantools/cantools/blob/68131411d777da8024e11ec79ddb7bebe92c2da0/LICENSE), [DBC loader](https://github.com/cantools/cantools/blob/68131411d777da8024e11ec79ddb7bebe92c2da0/src/cantools/database/can/formats/dbc/dbc_loader.py), [numeric conversion](https://github.com/cantools/cantools/blob/68131411d777da8024e11ec79ddb7bebe92c2da0/src/cantools/database/can/formats/utils.py). It supports broader DBC syntax, but ordinary numeric loading uses `int`/`float`, which cannot supply this exact-decimal contract by itself. Its private token parser is not adopted as a public API. Suitable as an optional differential oracle for the accepted subset, excluding numeric cases where binary rounding changes the comparison. |
| canmatrix 1.3.3.1, commit `22264038315169b73cbe889d4719a322a1eb0f96` | [Release](https://github.com/ebroecker/canmatrix/releases/tag/1.3.3.1), [BSD-2-Clause license](https://github.com/ebroecker/canmatrix/blob/22264038315169b73cbe889d4719a322a1eb0f96/LICENSE), [DBC loader](https://github.com/ebroecker/canmatrix/blob/22264038315169b73cbe889d4719a322a1eb0f96/src/canmatrix/formats/dbc.py), [package/dependency metadata](https://github.com/ebroecker/canmatrix/blob/22264038315169b73cbe889d4719a322a1eb0f96/pyproject.toml). A viable host conversion-library alternative, but still needs a reviewed adapter proving exact values, complete unsupported-construct detection and these source diagnostics. No such proof is claimed here. |

Propose a small repository-owned tokenizer/parser for the declared subset,
with exact numeric tokens and explicit rejection paths. This avoids coupling
the contract to a library's floating-point model or private AST. The cost is
owning grammar/conformance tests; independent accepted/rejected DBC vectors
and optional upstream differential checks are therefore required. No upstream
parser code or DBC fixture is copied by this specification, and no new
production dependency is added. Any later choice to vendor/adopt a library
requires an exact version/lock, transitive-license review, attribution update
where applicable and tests against this contract before it replaces the
proposed parser.

## Synthetic acceptance and owner-review gate

Use newly authored synthetic DBCs, with source ids `demo-a` and `demo-b`, bus
`body`, explicit Apache-2.0 provenance and a synthetic `mapping.json`. They
contain no vehicle observations, and no Mazda mapping is part of this
contract. The first source has:

```dbc
VERSION "synthetic-a-v1"
NS_ :
BS_:
BU_: ECU Controller
BO_ 256 Engine: 8 ECU
 SG_ rpm : 0|16@1+ (0.25,0) [0|16383.75] "rpm" Controller
 SG_ BrakePressure : 16|16@1+ (0.1,0) [0|6553.5] "kPa" Controller
```

The second source has an equivalent duplicate field and a compatible fragment:

```dbc
VERSION "synthetic-b-v1"
NS_ :
BS_:
BU_: ECU Controller
BO_ 256 EngineExtra: 8 ECU
 SG_ engine_rpm : 0|16@1+ (0.250,0.0) [0|16383.750] "rpm" Controller
 SG_ CoolantRaw : 32|8@1+ (1,-40) [-40|215] "degC" Controller
```

Before importer implementation is accepted, independent fixtures, using these
files and in-memory variants of them, must prove:

1. **Mapping and merge.** `demo-a` plus `demo-b` with a mapping of `rpm` to
   `vehicle.engine_rpm` binds it with both source references (the duplicate
   composes), while `BrakePressure` and `CoolantRaw` are `unmapped`. Reversing
   source order gives identical normalized bytes and identities.
2. **Conflicts.** A changed duplicate scale or DLC fails `source_conflict`
   with both spans, including in a message no mapping uses.
3. **Unsupported forms.** Multiplex, IEEE, extended-id and FD messages are
   reported `unsupported`; mapping one of their fields fails
   `unsupported_required_binding`. Unknown records and malformed strings fail
   with spans. Cross-byte Motorola, signed/unsigned 64-bit and exact decimal
   vectors independently check extraction normalization.
4. **Mapping errors and isolation.** Unknown target, duplicate target,
   unknown message or field (`stale_mapping`), type/unit/choice mismatch,
   different units with no `conversion` (`incompatible_target`), a
   non-identity conversion on equal units (`incompatible_target`), bindings that resolve to one unit
   pair with different conversions (`conflicting_conversion`, listing every
   target) and digest mismatch each fail with a stable code. Mapping `BrakePressure` to
   `vehicle.brake_pressed` fails (number field, boolean target). No imported
   profile can carry `fresh_or_unverified` or a native handler. A declared
   `timeout_ms` yields freshness backed only by `user_mapped` evidence, and
   the `operational` evidence entry exists only when one is declared; a zero,
   boolean or out-of-range `timeout_ms` fails `invalid_freshness`; an
   undeclared timeout leaves the signal unverified. An empty bindings list reports `all_features_disabled`.
5. **Identity and transaction.** Numeric, unit-conversion or freshness
   changes alter compatibility identity; changes confined to source bytes,
   license or path alter artifact identity only. An explicit identity
   conversion and an omitted one hash the same. Repeated runs are
   byte-identical. A late failure preserves the prior complete artifact set.

Host tests, firmware builds, bench measurements and vehicle evidence are
reported separately; this docs-only change claims none of those runtime
results.

Owner review must accept the subset/rejection rules, parser proposal, mapping
and derived-registry contract, evidence boundaries and these acceptance cases.
Only then rework the dependent importer tickets. A real second vehicle and its
licensed DBC package remain unselected.
