# Persisted controller configuration schema

`controller_config::Configuration` is the parser-independent model for a
versioned controller profile. It owns every persisted string with
`std::string`; `controller_config::parse_controller_config()` populates it
without making the action engine or LED adapter depend on a JSON library.
`to_action_engine_rule()` creates the existing action-engine persisted rule
types using string views into the owning configuration, so the configuration
must remain alive and must not be mutated while those rules are registered.

The loader rejects malformed JSON, missing or mistyped fields, unknown fields,
unsupported enum names, duplicate actions, unresolved action references, and
semantic range/zone/priority errors. `ConfigLoadResult::diagnostic` contains a
parse/structural/semantic category, a path such as `rules[3].input`, an
optional `SchemaError`, an array index where applicable, and a human-readable
message. A failed load never returns a partial `Configuration`. The overload accepting a
`vehicle_signals::SignalCatalogView` additionally delegates signal type,
capability, and enum-choice checks to the action engine.

## Version 1

The top-level `version` is required and must be `1`. Version 1 contains only
the following portable fields:

| Field | Type | Rules |
| --- | --- | --- |
| `version` | mathematically integral JSON number | Exactly `1`; JSON spellings such as `1.0` are accepted. |
| `actions` | array of `{name}` | Names are non-empty and unique. Runtime `ActionId`s are assigned one-based in declaration order; numeric IDs are not persisted. |
| `rules` | array | `type` is `state`, `sampled_state`, `event`, or `range`; `action` must name an action; `signal` is a vehicle signal catalog key. |
| `effect_bindings` | array | `action`, `effect` (`left_turn`, `right_turn`, `brake`), and integer `priority` in `0..255`. |
| `fill_bindings` | array | `action`, a zone, RGB color, and integer `priority` in `0..255`. |

State, sampled-state, and event rules use the action engine's persisted
condition concepts: `comparison` is `equal`, `not_equal`, `less`,
`less_or_equal`, `greater`, or `greater_or_equal`; `freshness` is `fresh` or
`fresh_or_unverified`; and operands are a `boolean`, finite `number`, or
named enum `choice`. Ordered comparisons require a numeric operand, and the
catalog-aware validator delegates signal type, capability, and enum-choice
checks to `action_engine`. Event rules additionally use `becomes_true` or
`becomes_false`. Sampled-state rules may set a finite `release_threshold` on
an ordered numeric condition; it must be on the release side of the
activation threshold.

Range rules contain finite `input` and `output` numeric ranges. `input.from`
must be strictly less than `input.to`; output ranges may be ascending,
descending, or constant. Non-finite values and overflowed spans are rejected.

Fill zones use `zone.start`, `zone.length`, and `zone.direction`, where the
direction is `start_to_end`, `end_to_start`, or `center_out`. The logical strip
has exactly 100 pixels for schema validation: a zone must have positive length
and fit entirely within `[0, 100)`. RGB channels are integers in `0..255`.

The loader accepts at most 16 KiB of JSON payload and 16 nested arrays/objects.
One terminal NUL byte is allowed in addition to that payload when a
configuration comes from a NUL-terminated NVS blob. Embedded NUL bytes and
`\\u0000` string escapes are rejected with a dedicated `EmbeddedNul` diagnostic.

## Production profile example

The complete profile currently represented by
`controller_config::default_configuration()` is available as
[`controller-config-v1.json`](../examples/controller-config-v1.json). It
contains strict turn-state rules for left, right, and hazard; an RPM range
from `0` to `6500` mapped to `0..1` with `fresh_or_unverified` freshness; a
sampled red-zone rule above `6000` RPM; mirrored turn bindings; a full-strip
center-out blue fill; and the brake warning effect.

## Deliberately absent hardware configuration

This schema describes signals, action semantics, and local LED effects only.
It has no CAN GPIO, bitrate, TWAI mode, LED GPIO, strip driver, pixel count,
task, queue, watchdog, renderer lifecycle, or power settings. Those remain
reviewed firmware and board composition concerns. In particular, persisted
configuration cannot enable CAN transmission or change the receive-only
vehicle boundary.
