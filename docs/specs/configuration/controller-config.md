# Persisted controller configuration (schema version 1)

This document is the authoritative schema for version 1 persisted controller
configuration, including the YAML authoring form, canonical JSON loader,
validation and production example. It describes the configuration data model;
application to runtime components and board-specific setup are outside this
schema. It does not define storage or a WebUI.

The reusable application helper and RPM feature behavior are specified in
[controller lighting profile](lighting-profile.md).

A configuration describes controller behaviour as:

```text
vehicle signal -> rule -> named action -> output binding
```

It reuses the action engine's persisted-form concepts (see
[action-engine.md](../action-engine.md)) and the local LED adapter's binding
concepts (see [local-led-actions.md](../lighting/local-led-actions.md)). It has
no feature-specific sections: the RPM fill and the red zone are an ordinary
range rule and an ordinary sampled-state rule.

The C++ model is in `components/controller_config/include/controller_config/persisted/`:

| Header | Contents |
| --- | --- |
| `model.hpp` | `ControllerConfig` and its value types. |
| `names.hpp` | Persisted spellings: `name_of()` and `parse_name<Enum>()`. |
| `json_loader.hpp` | `parse_controller_config(std::string_view)` and `serialize_controller_config()`, the JSON boundary. |
| `validation.hpp` | `validate(const ControllerConfig &)`. |
| `production_profile.hpp` | `production_lighting_config()`, the example below in C++. |

The model owns its strings, so a loader can fill it from any serialized form.
It stores persisted integers as `std::int64_t`, which means validation decides
whether a value fits its runtime type; the loader never narrows a value
silently. Numbers are `float`, the action engine's operand type.

## Document

| Field | Type | Required | Meaning |
| --- | --- | --- | --- |
| `version` | integer | yes | Schema version. Only `1` is valid. |
| `actions` | list of action | no (empty) | The named actions. |
| `rules` | list of rule | no (empty) | Signal rules that drive actions. |
| `outputs` | list of output binding | no (empty) | What each action does. |

### Actions

| Field | Type | Required | Meaning |
| --- | --- | --- | --- |
| `name` | string | yes | Stable, non-empty, unique action name. |

Rules and outputs refer to actions by name. The runtime assigns a numeric
`action_engine::ActionId` to each action when it applies the configuration.
ActionIds are never persisted, so renumbering them never changes a document.
An action with no rule or no output is valid, though it has no effect.

### Rules

Every rule has a `type`, the `action` it drives, and an optional `freshness`.

| `type` | Engine form | Behaviour |
| --- | --- | --- |
| `state` | `StateRuleConfig` | Level: on while the condition holds on notified readings. |
| `sampled_state` | `SampledStateRuleConfig` | Level: as `state`, on sampled reads, with optional hysteresis. |
| `event` | `EventRuleConfig` | One-shot pulse on a condition edge. |
| `range` | `RangeRuleConfig` | Level: maps a numeric signal linearly onto an output range. |

These fields are shared by the condition-based rules (`state`, `sampled_state`
and `event`):

| Field | Type | Required | Meaning |
| --- | --- | --- | --- |
| `action` | string | yes | A declared action name. |
| `signal_key` | string | yes | Signal key, such as `vehicle.turn_state`. |
| `comparison` | comparison name | yes | See below. |
| `operand` | operand | yes | Exactly one of `boolean`, `number` or `choice`. |
| `freshness` | freshness name | no (`fresh`) | Freshness a reading needs to count. |
| `release_threshold` | number | no | `sampled_state` only: the hysteresis release value. |
| `edge` | edge name | yes, for `event` | `becomes_true` or `becomes_false`. |

An operand is written as a one-key map: `{boolean: true}`, `{number: 6000}`,
or `{choice: left}`. A choice is one of the signal's enum choice keys.

A `range` rule has these fields:

| Field | Type | Required | Meaning |
| --- | --- | --- | --- |
| `action` | string | yes | A declared action name. |
| `signal_key` | string | yes | A numeric signal key. |
| `input` | `{from, to}` numbers | yes | The signal range. `from` < `to`. |
| `output` | `{from, to}` numbers | yes | The level range. It may descend or be constant. |
| `freshness` | freshness name | no (`fresh`) | Freshness a reading needs to count. |

### Output bindings

Every binding has a `type` and the `action` it follows.

`led_effect` binds the action to a discrete local LED effect:

| Field | Type | Required | Meaning |
| --- | --- | --- | --- |
| `action` | string | yes | A declared action name. |
| `effect` | effect name | yes | `left_turn`, `right_turn` or `brake`. |
| `priority` | integer 0..255 | no (`100`) | Higher wins where effects overlap. |

`led_fill` binds the action's level to a fill of part of the strip:

| Field | Type | Required | Meaning |
| --- | --- | --- | --- |
| `action` | string | yes | A declared action name. |
| `zone.start` | integer | yes | First logical pixel, from 0. |
| `zone.length` | integer | yes | Pixel count; the zone must fit the strip. |
| `zone.direction` | direction name | yes | `start_to_end`, `end_to_start` or `center_out`. |
| `color` | `{red, green, blue}` integers 0..255 | yes | Colour at full level. |
| `priority` | integer 0..255 | no (`100`) | Drawing order among fills. |

`led_transient` binds an action's `Trigger` to a solid zone for a bounded
lifetime. It shares `action`, `zone`, `color`, and `priority` fields with
`led_fill`, plus required `duration_ms` (integer `1..5000`). The five-second
maximum is the renderer contract's `local_argb::internal::kMaxTransientDurationUs`
converted to milliseconds; the validator and YAML compiler derive their bound
from that contract. It limits an event accent to a short lifetime even if
telemetry becomes stale immediately after its `Trigger`: `Deactivate` cannot
extend or restart the accent, and it expires within five seconds of the source
trigger. This software policy limit is not measured vehicle timing evidence.
The renderer owns elapsed-time expiry and its brightness ceiling; driver
fail-off clears active layers immediately.

JSON `duration_ms` must use an integer token: `800` is accepted, while `800.0`
and `8e2` are rejected. The loader checks the original token before cJSON's
double conversion can silently round a fractional or oversized duration.
YAML authoring normalizes exactly integral numeric values to integer JSON.

```yaml
outputs:
  - type: led_transient
    action: door_open_event # Must be declared under actions.
    zone: {start: 20, length: 20, direction: start_to_end}
    color: {red: 32, green: 16, blue: 0}
    duration_ms: 800
    priority: 120
```

Multiple transient zones may follow one action. A duplicate is the same
binding kind, action, zone start, and length; direction does not change the
physical target of a solid transient. Changing colour, duration, or priority
does not make a new target. Fill targets continue to include direction because
it changes their partial-level rendering. A fill and transient may
share a zone. `Activate`, `Deactivate`, and `SetLevel` leave transient timing
unchanged; only `Trigger` starts or restarts a transient. Renderer expiry and
explicit fail-off cancel active layers. Applying a configuration installs
bindings through `LedActionSink`; it does not bypass trigger/fail-off behavior. No transient
rule is added to the factory configuration.

### Persisted names

Names are snake_case and case-sensitive. `names.hpp` holds the single table
for each enum. A runtime enumerator that has no entry there cannot be
persisted.

| Value | Names |
| --- | --- |
| rule `type` | `state`, `sampled_state`, `event`, `range` |
| output `type` | `led_effect`, `led_fill`, `led_transient` |
| `comparison` | `equal`, `not_equal`, `less`, `less_or_equal`, `greater`, `greater_or_equal` |
| `freshness` | `fresh`, `fresh_or_unverified` |
| `edge` | `becomes_true`, `becomes_false` |
| `effect` | `left_turn`, `right_turn`, `brake` |
| `direction` | `start_to_end`, `end_to_start`, `center_out` |

## Validation

A loader works in two steps.

1. It rejects anything the model cannot hold: a missing required field, an
   unrecognized `type` or other name (`parse_name` returns `std::nullopt`),
   an operand that is not exactly one of `boolean`, `number` or `choice`, or a
   value of the wrong kind.
2. It builds the model and calls `validate()`.

`validate()` needs no parser, signal catalog or runtime. It returns the first
error as `{error, section, index}`. It checks the version first, then
`actions`, `rules` and `outputs`, each in document order. Where a constraint
is one the action engine or LED adapter already enforces, the error has the
same name and meaning.

| Error | Rejected when |
| --- | --- |
| `UnsupportedVersion` | `version` is not `1`. |
| `EmptyActionName` | An action name is empty. |
| `DuplicateActionName` | An action name is declared twice. |
| `UndeclaredAction` | A rule or output names an action that is not declared. |
| `DuplicateAction` | A second level rule (`state`, `sampled_state` or `range`) drives the same action. Event rules may share. |
| `EmptySignalKey` | A rule's signal key is empty. |
| `UnknownComparison`, `UnknownFreshness`, `UnknownEventEdge` | The value is not a recognized enumerator. |
| `EmptyChoice` | A choice operand is empty. |
| `InvalidOperand` | A number operand is NaN or infinite. |
| `UnsupportedComparison` | An ordered comparison (`less`, `greater`, ...) has a boolean or choice operand. |
| `InvalidHysteresis` | A release threshold is not finite, or the condition is not an ordered numeric one, or the value is not on the inactive side: below the activation for `greater*`, above it for `less*`. |
| `InvalidRange` | A range bound or span is not finite, or `input.from >= input.to`. |
| `UnknownLedEffect`, `UnknownFillDirection` | The value is not a recognized enumerator. |
| `EmptyZone` | `zone.length` is 0. |
| `ZoneOutOfRange` | The zone does not fit the 100-pixel logical strip (`local_argb::kLedCount`), or has a negative start or length. |
| `InvalidColor` | A colour channel is outside 0..255. |
| `InvalidPriority` | A priority is outside 0..255. |
| `DuplicateBinding` | The same binding kind and action already drive this effect or zone. Fill zones include start, length and direction; solid transient zones include only start and length. |
| `InvalidDuration` | Transient `duration_ms` is outside `1..5000`, derived from the renderer contract maximum. |

A validated configuration can still fail when the runtime applies it. These
checks need the provider's signal catalog or the runtime's fixed capacities,
so they remain with the runtime apply step and keep their existing
`action_engine::ConfigStatus` and `local_argb_actions::BindingStatus` results:

- an unknown signal key;
- an operand type that does not match the signal's type, or an unknown choice key;
- a signal whose capabilities do not support the rule type;
- rule or binding capacity (for example, 8 effect, 8 fill and 8 transient bindings).

## Versioning

- Every document states `version`. The runtime loader rejects any version
  other than `1`.
- The meaning of a version 1 document never changes. A later change that
  alters meaning, adds a required field or renames a value uses a new
  version.
- A future loader may accept older versions by migrating them into the
  current model before validation. Version 1 needs no migrations.

## YAML compiler

YAML is an authoring format only. The host-side compiler parses it with the
PyYAML library, applies the schema checks above, and emits deterministic,
compact JSON using the persisted field names. The ESP32 has no YAML parser.
Before emitting JSON, it also checks each rule against the checked-in host
signal catalog manifest at `tools/controller_signal_catalog.json`: signal
existence, delivery capability, operand type, and enum choice keys must match
the catalog exposed by the Mazda telemetry provider.

`vehicle.brake_pressed` is a Boolean Read + Notify signal. It can be used
with Boolean equality or inequality conditions in `state`, `event` and
`sampled_state` rules; numeric ranges and enum choices are invalid. Compiling
or loading such a rule does not make its observation fresh. The default
`fresh` requirement excludes a `FreshnessUnverified` brake observation.

The project owner explicitly approved `fresh_or_unverified` as an opt-in for
`vehicle.brake_pressed`. The compiler, persisted validator and canonical JSON
loader accept that policy for Boolean state, event and sampled-state rules,
including rules with LED output bindings. With the brake timeout unset, the
last decoded brake value can remain eligible when brake frames stop arriving
while other CAN traffic keeps transport health available. This opt-in does not
turn the observation into `Fresh` or establish brake timing evidence; normal
malformed, faulted, stopped and unavailable fail-off behavior still applies.
The schema does not configure telemetry freshness timeouts. Other signals
retain their existing freshness policies. The production profile uses this
opt-in for its `brake` action; see [Production lighting profile](#production-lighting-profile).

Install the host-only dependency and compile a profile with:

```sh
python3 -m pip install --user -r tools/requirements.txt
python3 tools/compile_controller_config.py \
  config/default.yaml \
  /tmp/controller-config-v1.json
```

The generated JSON can be inspected or passed to host tooling. It remains
subject to the normal runtime loader validation when applied to firmware.

Omitted `actions`, `rules` and `outputs` become empty lists. Integer fields
are checked before float normalization: exactly integral forms such as `1.0`
are accepted, while fractional versions, priorities, zone coordinates and
RGB channels are rejected. Numeric rule fields are rounded to the model's
float precision before validating range and hysteresis constraints. YAML
constructor failures, including invalid unquoted dates, use the compiler's
`ERROR` diagnostic and exit code 2; quote date-like strings when used as names.

The host CTest `controller_config_yaml_loader` compiles the production example
and valid regression profiles, loads the emitted bytes through the canonical
C++ loader, and resolves each rule against the real Mazda catalog. It also
compares the complete catalog metadata with the host manifest. Compiler enum
spellings are checked against the C++ persisted name tables. Brake regression
profiles cover Boolean equality and inequality notify and read rules without
output bindings. Owner-approved unverified-brake LED profiles compile and
resolve through the canonical loader/catalog harness. Direct C++ validator
and JSON-loader regressions independently retain `FreshOrUnverified` on
accepted Boolean brake rules rather than converting it to `Fresh`.
Type-mismatch and numeric-range profiles still fail before JSON is emitted,
including numeric brake ranges with either freshness policy.

## Not configurable

The schema describes controller behaviour only. Board and platform safety
settings stay compile-time and platform concerns, and no version of this
schema exposes them:

- GPIO assignments, including the LED data pin;
- CAN TX/RX pins and bit rate;
- CAN listen-only enforcement and CAN transmit capability;
- board revision and transceiver electrical behaviour;
- hardware capability declarations;
- renderer worker, supervisor, frame timing and other internals;
- the logical strip length (`kLedCount`), which bounds `zone` but is not set by it.

## Production lighting profile

`config/default.yaml` is the factory profile for the firmware; the version 1
example in `examples/controller-config-v1.yaml` documents the same profile.
It covers the mirrored turn signals, the hazard, the RPM level fill, the
RPM red zone and the brake pedal. The `brake` action is a `state` rule on
`vehicle.brake_pressed` equal to `true` with `fresh_or_unverified` freshness
and no telemetry timeout; it is bound to the `brake` LED effect at priority
200. The RPM red zone binds the same effect at priority 150, so the region is
lit while either action is active and takes the higher active priority.
`production_lighting_config()` builds the same document in C++ for host
parity tests; firmware loads this YAML's generated JSON through the canonical
persisted configuration loader.
`tests/host/controller_config_schema_tests.cpp` checks that the C++ builder
validates and matches the default lighting profile field by field.
`tests/host/controller_config_application_tests.cpp` loads JSON generated from
the factory YAML at host configure time through the canonical parser, compares
every persisted field with that C++ builder, and checks full board-strip RPM
coverage, mirrored turns, the RPM fill range and red-zone threshold, the brake
pedal and its coexistence with the red zone, and fail-off.

## Boot-time override storage

The firmware exposes a configuration storage boundary with `load_override()`,
`save_override()` and `clear_override()`. The ESP-IDF adapter keeps its NVS
handles and APIs private and uses the dedicated `mazda_config` namespace. The
stored slot values contain only the canonical JSON document; they never contain
YAML, parser objects or runtime `action_engine::ActionId` values.

`save_override()` parses and semantically validates the candidate with the same
`parse_controller_config()` loader used for the embedded factory document, then
serializes the owning `ControllerConfig` into deterministic JSON before writing
it. New overrides are limited to **4 KiB of canonical JSON**, separately from
the parser's 16 KiB input limit. Canonicalization can expand a candidate; an
oversized result returns `InvalidCandidate` with an `InputTooLarge` diagnostic
before writing. The NVS adapter enforces the same write bound. The existing
16 KiB read bound is retained so previously stored documents can still load.

The default ESP-IDF single-app partition table provides a 24 KiB `nvs`
partition. A 4 KiB write limit leaves conservative room for the two slot blobs,
an in-flight replacement of the inactive slot, NVS metadata and garbage
collection. This is a software storage bound, not an on-target capacity or
endurance guarantee. Other namespaces share the partition and may exhaust it;
the adapter reports the specific ESP-IDF error, including
`ESP_ERR_NVS_NOT_ENOUGH_SPACE`.

A rejected candidate does not reach the backend, and a failed backend write
does not replace the active override. The adapter verifies the written slot and
active marker after each NVS commit and restores the previous marker when a
commit or read-back check fails.

At boot, a present override is parsed through that same loader. A malformed or
semantically invalid override is retained for diagnostics, logged, and ignored
for the current boot; the embedded factory JSON is selected instead. A missing
override and a storage read failure also use the factory document, with the
failure reported. Clearing the active marker restores factory selection on the
next boot, including when the marker has the wrong type, is out of range, or
cannot be read. Clear reports success only when the marker is absent; if
clearing fails, it attempts to restore a previously readable valid marker.
Clear deactivates the override: **`slot_a` and `slot_b` remain in flash**. It is
not a secure wipe of user configuration or an erase of the NVS partition.

The configuration component currently owns global `nvs_flash_init()` during
boot. Initialization failures, including `ESP_ERR_NVS_NO_FREE_PAGES` and
`ESP_ERR_NVS_NEW_VERSION_FOUND`, are logged with the specific error and leave
the partition intact. The firmware uses the factory document and provides no
store for save or clear during that boot. Recovery requires deliberate
partition maintenance and a reboot; automatic partition erasure is excluded
because it would also destroy other namespaces. Future Wi-Fi/WebUI work must
coordinate global initialization and recovery through a shared NVS owner.

Configuration is loaded and applied once during startup; this
iteration has no WebUI, hot reload, or live action-engine mutation.

```yaml
version: 1

actions:
  - name: left_turn
  - name: right_turn
  - name: hazard
  - name: rpm_fill
  - name: red_zone

rules:
  - type: state
    action: left_turn
    signal_key: vehicle.turn_state
    comparison: equal
    operand: {choice: left}
    freshness: fresh
  - type: state
    action: right_turn
    signal_key: vehicle.turn_state
    comparison: equal
    operand: {choice: right}
    freshness: fresh
  - type: state
    action: hazard
    signal_key: vehicle.turn_state
    comparison: equal
    operand: {choice: hazard}
    freshness: fresh
  # Engine speed is reported without freshness verification, so the RPM
  # rules accept unverified readings. Stale or unavailable readings still
  # turn them off.
  - type: range
    action: rpm_fill
    signal_key: vehicle.engine_rpm
    input: {from: 0, to: 6500}
    output: {from: 0, to: 1}
    freshness: fresh_or_unverified
  - type: sampled_state
    action: red_zone
    signal_key: vehicle.engine_rpm
    comparison: greater
    operand: {number: 6000}
    freshness: fresh_or_unverified

outputs:
  # The strip is mounted mirrored: a left turn animates the right-hand effect.
  - type: led_effect
    action: left_turn
    effect: right_turn
    priority: 100
  - type: led_effect
    action: right_turn
    effect: left_turn
    priority: 100
  - type: led_effect
    action: hazard
    effect: left_turn
    priority: 100
  - type: led_effect
    action: hazard
    effect: right_turn
    priority: 100
  - type: led_fill
    action: rpm_fill
    zone: {start: 0, length: 100, direction: center_out}
    color: {red: 0, green: 16, blue: 32}
    priority: 50
  - type: led_effect
    action: red_zone
    effect: brake
    priority: 150
```

The same document as canonical JSON uses the same field names and values.
For example, a rule is
`{"type": "range", "action": "rpm_fill", "signal_key": "vehicle.engine_rpm", ...}`.

## Canonical JSON loading

`serialize_controller_config()` emits deterministic JSON for a validated model,
including transient durations. Its existing caller precondition is to call
`validate()` first. Serialization preserves every rule/output alternative and
escapes string values; transient outputs are optional additions to schema 1,
so existing version-1 documents retain their behavior.

`controller_config::persisted::parse_controller_config()` parses version-one
JSON directly into the owning `ControllerConfig` above and calls its existing
`validate()` function. A successful result contains the model; a failed result
contains a categorized diagnostic with a path, array index and message, and
never a partial model. Parser types remain private to the implementation.

The loader rejects duplicate or unknown fields, invalid enum names, non-integer
integer fields, invalid operand unions and schema constraints. Omitted lists,
freshness and priority use the defaults documented above. Numeric operands
and range bounds use the model's float precision; integers retain their
precision through validation. Input is bounded to 16 KiB, 16 container levels
and 512 structural elements during parser preflight. One terminal C-string NUL
is accepted; embedded NUL bytes and escaped NUL strings are rejected.
Documents that exceed the parser resource budget return a `ResourceExhausted`
diagnostic before cJSON allocates its parse tree; a cJSON allocation failure
during parsing uses the same diagnostic code.

Host builds import cJSON v1.7.19 at its pinned immutable commit. Offline builds
can set `CONTROLLER_CONFIG_CJSON_SOURCE_DIR` to a source checkout containing
`cJSON.c` and `cJSON.h`. ESP-IDF uses its built-in `json` component. Signal
catalog compatibility, runtime capacities and numeric action ID assignment
remain the application step's responsibility, as specified by the persisted
model. The loader does not apply a profile to firmware or perform driver work.
