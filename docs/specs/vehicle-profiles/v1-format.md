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
never pass through a floating-point parser. Canonical identity serialization
and mechanical range/bit validation are later stages; structural validation
does not establish that two distinct numeric lexemes mean different values.

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
isolation only. Later identity validation must use computed golden digests.

Run the focused structural checks with:

```sh
python3 -m unittest discover -s tests/tools -p vehicle_profile_manifest_test.py
```
