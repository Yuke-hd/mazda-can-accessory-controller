# Normalized vehicle profile version 1

This is the concrete host/build structural contract for normalized vehicle
profiles. The host API is
`vehicle_profiles.validation.load_profile(text)` or
`validate_structure(document)`. Both return the validated mapping without
changing numeric strings or ordering. `model.py` publishes the record types.
Structural validation alone does not establish executable mechanics, reviewed
policy, resource bounds, or correct computed identities.

Every object has exactly the fields below; none are implicitly optional.
`null` is allowed only where stated. Arrays are JSON arrays, integers exclude
booleans, and text is a JSON string. Identifiers match
`[A-Za-z0-9][A-Za-z0-9_.:-]*`. A digest is 64 lowercase hexadecimal characters.

| Record | Fields and types |
| --- | --- |
| Profile | `format`: literal `vehicle-signal-profile`; `version`: integer `1`; `profile_id`, `semantic_policy_revision`, `recognition_rule_revision`, `selected_bus`: identifiers; `sources`: Source array; `messages`: Message array; `bindings`: Binding array; `diagnostics`: Diagnostic array; `compatibility_identity`, `artifact_identity`: digests. |
| Source | `id`: unique identifier; `kind`: `dbc` or `native`; `path`: nonblank logical package-relative path, with no absolute prefix, backslash or `..` component; `sha256`: digest; `revision`: nonblank text or null; `license`: nonblank provenance/license reference; `bus`: identifier. |
| Message | `id`: unique identifier; `bus`: identifier; `identifier_format`: `standard` or `extended`; `identifier`, `dlc`: integers; `fields`: Field array. |
| Field | `id`: profile-wide unique identifier; `name`: nonblank original source name; `sources`: unique existing source ids; `start`, `length`: integers; `byte_order`: `intel` or `motorola`; `signed`: boolean; `scale`, `offset`, `minimum`, `maximum`: decimal strings; `unit`: text, possibly empty; `value_table`: Value array. |
| Value | `raw`: integer string; `label`: nonblank source text. |
| Binding | `key`: unique canonical identifier; `runtime_id`: unique integer; `type`: `boolean`, `number` or `enum`; `unit`: text, possibly empty; `capabilities`: unique `read`/`notify` strings; `choices`: Choice array; `kind`: `native` or `dbc`; `inputs`: unique existing field ids; `policy`: the matching policy variant below; `evidence`: Evidence; `freshness`: Freshness or null. |
| Choice | `key`: identifier unique within the binding; `value`: integer provider value. |
| Evidence | `ref`, `confidence`, `eligibility`: nonblank text. These are references and policy declarations, not inferred observations. |
| Freshness | `timeout_ms`: integer; `basis`, `evidence_ref`: nonblank text. Null means timing is unset. |
| Diagnostic | `category`: `unmapped`, `ambiguous`, `unsupported` or `all_features_disabled`; `affected`: identifier for the affected canonical input, source field or generated feature; `sources`: unique existing source ids; `reason`: nonblank diagnostic prose. |

Native policy has exactly `handler` and `revision` (reviewed identifiers), and
`invalid_value` (nonblank policy name). It refers to reviewed code and does not
invent source extraction coordinates. Native handlers may reference existing
fields in `inputs` when the handler combines source observations.

DBC policy has exactly `conversion`, `enum_map`, and `invalid_value` (nonblank
policy name). Conversion has `source_unit` and `target_unit` (possibly empty
text), `scale` and `offset` (decimal strings), `target_precision` (nonblank
text), and `invalid_ranges` (an array of inclusive source-physical ranges).
Each invalid range has exactly `minimum` and `maximum` decimal strings in
`source_unit`; `[]` explicitly declares no additional invalid ranges.
Semantic validation checks range order and conversion suitability. Enum-map
records have exactly `raw` (integer string) and `choice` (an
existing choice key in that binding). An empty enum map is represented as `[]`.
These fields describe declared conversion intent; later semantic validation
checks whether it preserves the canonical value contract.

Integer strings match `-?(0|[1-9][0-9]*)`. Decimal strings match
`-?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?`. All source precision is
retained, including exponent spelling and trailing zeroes. Numeric strings
never pass through a floating-point parser. Structural validation does not
establish that two distinct numeric lexemes mean different values; the
identity stage below normalizes their exact values.

Source, message, field, canonical-key and runtime-id identities must be unique
within their stated scopes. References resolve within this document, with no
fallback to another profile or the Mazda catalog. A profile with no bindings
must contain an explicit `all_features_disabled` diagnostic.

`ProfileValidationError` exposes stable `category`, JSON `path`, `reason`, and
applicable `sources`. Structural categories are `malformed_json`,
`duplicate_key`, `unknown_field`, `missing_field`, `invalid_type`,
`invalid_value`, `invalid_identity`, `invalid_reference`, `duplicate_identity`,
`invalid_numeric_lexeme`, `invalid_source_path`, `unsupported_format`,
`unsupported_version`, and `missing_empty_profile_diagnostic`. Duplicate JSON
key errors identify the key and use path `$`, because parsing fails before a
record path can be established. Source and field record errors retain source
ids where their shape allows those ids to be read.

The independently authored `synthetic-river.json` and `synthetic-empty.json`
fixtures contain no vehicle observations or redistributed DBC bytes. River
uses two synthetic source records; Desert deliberately exposes no inputs.
Their repeated-letter digests are structural placeholders, not claims about
source bytes or computed profile identities. They establish shape and profile
isolation only. The identity stage below replaces these placeholders; its
independent golden fixtures contain computed expected digests.

Run the focused structural checks with:

```sh
python3 -m unittest discover -s tests/tools -p vehicle_profile_manifest_test.py
```

## Canonical identity version 1

`vehicle_profiles.canonical.identify_profile(document, policies=registry,
generator_revision="generator.v1", validation_limits={})` first runs mechanical/policy validation
and source composition. It returns `ProfileIdentity`: a validated profile
copy with computed `compatibility_identity` and `artifact_identity`, and the
two exact preimage byte strings. The supplied identity fields are placeholders
and never enter either preimage. The caller supplies an explicit stable
generator revision. Temporary build directories and generation times are
absent from both preimages. Validated logical source paths remain attributable
in the artifact identity.

Both identities are lowercase SHA-256 hex digests. Each preimage is a JSON
object encoded as UTF-8, with keys sorted lexicographically, no insignificant
whitespace, literal Unicode text, and exactly one final LF. JSON strings use
standard JSON escapes; no BOM is emitted. Numeric JSON integers stay integers.
Raw integer strings become base-ten `str(int(value))`, so `-0` becomes `0`.
Decimal strings use exact fixed-point notation: no exponent, unnecessary
leading/trailing zeroes, decimal point for integers, or negative zero. This
conversion uses the coefficient/exponent directly, without floating-point or
decimal-context rounding. For example `1.000e0` becomes `1`, `-0.00` becomes
`0`, and `0.0100000000000000000001` retains every significant digit.

The compatibility preimage has exactly `format` (literal
`vehicle-profile-compatibility-v1`), `profile_id`,
`semantic_policy_revision`, `selected_bus`, and `bindings`. Bindings are
sorted by canonical key. Each binding retains declared canonical metadata,
policies, evidence and freshness. Its input references become the resolved
message `bus`, `identifier_format`, `identifier`, `dlc`, and field mechanics
(`start`, `length`, `byte_order`, `signed`, `scale`, `offset`, `minimum`,
`maximum`, `unit`, `value_table`). Input order remains significant for native
handler arguments. Field ids, source names and provenance references are
excluded from this resolved input representation. Nonpersisted `runtime_id`
assignments are excluded from compatibility and retained in artifact identity.
Unexposed source fields and diagnostics do not affect compatibility.

Changed extraction, physical scale/range, source enum meaning, canonical
choices/provider values, conversion/invalid-value rules, handler revision,
evidence eligibility/confidence or freshness changes compatibility. Native
handler revisions identify reviewed implementations; changed handler meaning
requires a revision change. A profile with no exposed bindings still has its
own profile/policy/bus-scoped compatibility preimage.

The artifact preimage has exactly `format` (literal
`vehicle-profile-artifact-v1`), `generator_revision`, `profile`, and
`validation`. `profile` is the complete validated/composed profile with both
digest fields removed. It retains logical source paths, source ids, byte
digests, revisions, attribution, source names and diagnostic prose.
`recognition_rule_revision` belongs here, while semantic-policy revision also
belongs in compatibility. `validation` includes the complete supplied
`ValidationPolicies` registry, including unused reviewed entries, and the
explicit `validation_limits` mapping. Limit keys are stable identifiers and
values are nonnegative JSON integers (booleans are rejected). The caller
supplies all effective consumer/resource limits; this stage hashes them and
does not introduce resource enforcement. An empty mapping states that no
additional limits were supplied. Changing the validation environment or limits
therefore changes artifact identity while preserving compatibility.

Canonical array ordering is explicit: sources/messages/fields by id,
bindings/choices by canonical key, capabilities/source references
lexicographically, raw value tables/enum maps by exact raw integer, invalid
ranges/diagnostics by canonical JSON bytes. Native input arrays are never
sorted. Registry maps become sorted arrays: profile revisions as
`[profile, semantic_revision, recognition_revision]`; handlers as
`[name, revision, sorted_profiles, value_type, unit, minimum_inputs,
maximum_inputs]`; evidence as triples; unit conversions as
`[source_unit, target_unit, canonical_scale, canonical_offset]`; freshness as
`[profile, key, timeout_ms, basis, evidence_ref]`. Units and invalid-value
policy names are sorted arrays; `limits` is an object with sorted keys.
Equivalent reordering and numeric spellings
therefore produce identical preimages.

Logical source path, license, source bytes, source names or diagnostic-prose
edits may change artifact identity while preserving
compatibility when resolved bindings keep the same meaning. The strict schema
has no comment or generation-timestamp fields. Provenance-only comments in
source bytes affect the supplied byte digest and therefore artifact identity.

`tests/fixtures/vehicle-profiles/identity-golden.json` publishes independently
authored empty/native preimages and digests. Its expected bytes were authored
without the production canonicalizer. Identity tests also cover mechanical,
enum, handler, evidence/freshness, provenance, generator and validation-registry
mutation pairs. Run them with:

```sh
python3 -m unittest discover -s tests/tools -p vehicle_profile_identity_test.py
```
