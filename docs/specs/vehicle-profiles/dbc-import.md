# Offline DBC import and automatic recognition

Status: proposed module contract for owner review. This document specifies the
`dbc-import` module; it does not add a parser, recognition engine, dependency,
or vehicle support. Importer implementation tickets may start only after the
owner accepts this contract and its acceptance examples.

## Boundary and inputs

The importer runs on the host before a build. It produces the
[normalized version-one profile](v1-format.md), its two computed identities,
and a source/recognition report. The existing `SignalProvider` remains the
consumer boundary. Build selection and command-line syntax belong to the
later integration contract; runtime storage and handlers belong to the runtime
contract. The importer never opens a CAN interface or emits transmit code.

A request supplies these explicit inputs:

- Stable `profile_id`, one `selected_bus`, and one reviewed recognition/model
  policy-pack id and revision.
- Vehicle context: make, model/platform, model year, market and trim. Unknown
  values are explicit; a missing value cannot satisfy a restricted rule.
- Source records: stable source id, local input path, logical package-relative
  artifact path, logical bus, source revision or `null`, and a recorded
  license/provenance reference. An expected byte digest is optional for a
  pinned source package; the tool always computes the actual SHA-256 itself.
- The explicit `ValidationPolicies` registry, generator revision and effective
  parser/consumer limits required by `identify_profile`. No Mazda registry or
  policy is inherited implicitly.

The owner chooses sources and context, rather than assigning individual DBC
fields to controller inputs. A DBC filename, node name, sender or `BS_` value
does not select a physical CAN interface, bitrate, vehicle model or bus.
Each source's logical bus comes from the request. Only the selected bus can
contribute executable bindings; other buses remain inventory.

Read source bytes once, verify any expected digest, then decode with the
source's explicitly declared encoding (`utf-8` or `cp1252` in version one).
Decoding errors fail import; replacement characters and guessed encodings are
forbidden. Hash the original bytes, before decoding or newline handling.
Local absolute paths stay in host diagnostics only and never enter generated
artifacts. A recorded license reference is required even for a privately
licensed source. It establishes provenance, not permission to redistribute:
committed DBCs and derived material still follow the repository's
[vehicle-data and licensing policy](../../development/license-and-vehicle-data.md).

## Supported DBC subset

Version one parses the complete input into a source inventory before applying
recognition. Whitespace and line endings have no semantic effect; outside
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
freshness, enum or handler policies. A reviewed model rule that needs an
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

Every request supplies nonnegative integer limits for source count, source
bytes, token bytes, records, fields and total diagnostics. Numeric tokens must
also meet the normalized validator's existing exact-representation limits.
Exceeding any limit is a fatal `resource_overflow`, never truncation. The
effective limits enter artifact identity through `validation_limits`; concrete
build defaults are an integration decision.

## Composition before recognition

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
diagnostic. If a selected recognition/policy rule requires such a message or
field, fail the artifact with `unsupported_required_binding`; a successful
artifact must not imply partial support for that binding.

## Reviewed automatic recognition

A policy pack is versioned repository data/code reviewed for its exact profile
scope. It declares context predicates, source/evidence references, aliases,
mechanical constraints, canonical types/units/choices, conversion and invalid
value rules, handler id/revision, and explicit optional freshness. Its revision
must agree with the supplied `ValidationPolicies.profile_revisions` entry.
Unknown packs, revisions, handlers or policy references fail validation.
No pack can supply arbitrary executable expressions from DBC text.

The resolved `recognition_rule_revision` must identify the pack and exact
context inputs, rather than only a shared parser/library version. Define it
as `recognition-v1:` followed by SHA-256 of this recognition-input preimage:
`format` is `dbc-recognition-input-v1`, `pack_id` and `pack_revision` are
registered identifiers, and `context` has exactly `make`, `model`, `platform`,
`model_year`, `market` and `trim`. Text values use the pack's exact registered
ASCII codes or `null`; `model_year` is an integer (excluding booleans) or
`null`. Serialize as UTF-8 JSON, sorted object keys, compact separators, no ASCII escaping and no nonfinite
numbers. The resolver supplies the matching expected profile revision pair to
the validator and retains this preimage in the source report. Thus a context
or recognition-pack change remains attributable in artifact identity without
changing semantic compatibility when the resolved bindings agree.

Rules operate on composed fields plus their original source-name inventory.
Aliases are explicit ASCII case-insensitive spellings; for example a reviewed
engine rule can accept `rpm`, `engine_rpm` and `EngineRPM`. The name match must
also satisfy that rule's unit, mechanical and source/model-context predicates.
Spelling similarity, a substring, file order or a confidence score alone
cannot select a canonical input. Unknown context fails the restricted
predicate; it never widens a rule to all models or years.

Each complete rule match proposes a binding. Mechanically equivalent aliases
of the same input normalize before signature/arity validation. Native-handler
arguments follow the rule's declared named-slot order; deduplication must not
sort or reorder those inputs. Equivalent complete candidates deduplicate only
when their resolved semantics and policies agree. Competing candidates for
one canonical key produce `ambiguous`, exclude that binding, and identify
all candidate sources. There is no best-score or last-file tie breaker.
Valid fields with no reviewed match produce `unmapped`.

Model policy supplies meaning as well as names. Brake pressure does not imply
`vehicle.brake_pressed`; indicator lamp state does not imply a turn request.
Reserved/unknown enum values need an explicit invalid-value policy, and
combined turn state or disable-bit behavior uses a reviewed native handler.
The Mazda brake opt-in remains scoped to its approved Mazda profile. Other
profiles cannot inherit its `fresh_or_unverified` policy. Import success and
synthetic tests never promote Reference evidence to Fresh; source timing,
operational freshness, confidence and current availability stay distinct.

The importer emits bindings and diagnostics, rather than factory actions.
Missing or ambiguous canonical inputs disable their dependent generated
factory features in projection/integration. User-authored invalid rules remain
strict errors. Zero exposed bindings is a valid explicit
`all_features_disabled` profile, with no fallback to Mazda bindings or policy;
the integration contract must preserve black startup for it.

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
| Existing normalized validator/policy/resource error categories | Forward the category and source ids; fail the artifact. |
| `unmapped_field`, `ambiguous_binding`, `unsupported_message` | Map to strict v1 diagnostic categories `unmapped`, `ambiguous`, `unsupported`; omit the affected binding. |
| `all_features_disabled` | Required explicit result when no bindings remain. |

Do not extend the strict v1 Diagnostic object with report-only fields. Its
`sources` ids connect it to the richer source report. After recognition, call
`validate_profile` and `identify_profile` with the reviewed registries,
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
| Existing `tools/compare_mazda_dbc.py` | Its [documented Mazda comparison subset](../../protocol/dbc-metadata-verification.md) compares reviewed metadata. Its parser ignores lines outside that subset and has no complete import/recognition contract. Keep it as an independent Mazda regression check. |
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
`body`, explicit Apache-2.0 provenance and a synthetic model context. They
contain no vehicle observations. The first source has:

```dbc
VERSION "synthetic-a-v1"
NS_ :
BS_:
BU_: ECU Controller
BO_ 256 Engine: 8 ECU
 SG_ rpm : 0|16@1+ (0.25,0) [0|16383.75] "rpm" Controller
 SG_ BrakePressure : 16|16@1+ (0.1,0) [0|6553.5] "kPa" Controller
```

The second source has an equivalent alias and a compatible fragment:

```dbc
VERSION "synthetic-b-v1"
NS_ :
BS_:
BU_: ECU Controller
BO_ 256 EngineExtra: 8 ECU
 SG_ engine_rpm : 0|16@1+ (0.250,0.0) [0|16383.750] "rpm" Controller
 SG_ CoolantRaw : 32|8@1+ (1,-40) [-40|215] "degC" Controller
```

Before importer implementation is accepted, independent fixtures must prove:

1. The reviewed synthetic pack automatically binds `vehicle.engine_rpm` from
   the aliases; the duplicate field has both source references, while
   pressure/coolant remain unmapped. Reversing source order gives identical
   normalized bytes and identities. No per-field owner assignment is needed.
2. A changed duplicate scale or DLC fails `source_conflict` with both spans.
   A distinct matching rpm field on another message creates ambiguity and
   removes only that canonical binding. Brake pressure alone never produces
   brake-pressed or Mazda freshness policy.
3. A multiplex/IEEE/extended/FD example is inventoried as unsupported; a
   reviewed binding requiring it fails. Unknown records and malformed strings
   fail rather than disappear. Cross-byte Motorola, signed/unsigned 64-bit and
   exact decimal vectors independently check extraction normalization.
4. Wrong/unknown model, year, market, trim or policy revision cannot borrow a
   different profile's rules. An explicitly empty synthetic profile reports
   all features disabled. Numeric/handler/freshness changes alter compatibility;
   source-byte/provenance-only changes remain attributable in artifact identity.
5. A late failure preserves the prior complete artifact set. Host tests,
   firmware builds, bench measurements and vehicle evidence are reported
   separately; this docs-only change claims none of those runtime results.

Owner review must accept the subset/rejection rules, parser proposal,
recognition/context contract, evidence boundaries and these acceptance cases.
Only then open bounded importer implementation tickets. A real second vehicle
and its licensed DBC package remain unselected.
