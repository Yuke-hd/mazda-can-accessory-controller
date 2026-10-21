#!/usr/bin/env python3
"""Compile a YAML controller profile into canonical runtime JSON.

The compiler is intentionally host-only. PyYAML performs YAML parsing while
this module validates and normalizes the persisted controller-config schema;
the ESP32 consumes the resulting JSON through its loader.
"""

from __future__ import annotations

import argparse
import json
import math
import re
import struct
import sys
from decimal import Decimal, InvalidOperation
from pathlib import Path
from typing import Any, Iterable, Mapping, Sequence

try:
    import yaml
except ModuleNotFoundError:  # pragma: no cover - exercised through the CLI
    yaml = None  # type: ignore[assignment]


VERSION = 1
DEFAULT_FRESHNESS = "fresh"
MAX_JSON_BYTES = 16 * 1024
# The persisted C++ model stores numeric fields as float. Reject values that
# the runtime model would convert to infinity even when Python can represent
# them as a finite double.
FLOAT_MAX = 3.4028234663852886e38
SIGNAL_CATALOG_PATH = Path(__file__).with_name("controller_signal_catalog.json")
SIGNAL_TYPES = {"boolean", "number", "enum"}
RULE_CAPABILITIES = {
    "state": "notify",
    "event": "notify",
    "sampled_state": "read",
    "range": "read",
}

ENUMS = {
    "type": {
        "state": "state",
        "State": "state",
        "sampled_state": "sampled_state",
        "SampledState": "sampled_state",
        "event": "event",
        "Event": "event",
        "range": "range",
        "Range": "range",
    },
    "comparison": {
        "equal": "equal",
        "Equal": "equal",
        "not_equal": "not_equal",
        "NotEqual": "not_equal",
        "less": "less",
        "Less": "less",
        "less_or_equal": "less_or_equal",
        "LessOrEqual": "less_or_equal",
        "greater": "greater",
        "Greater": "greater",
        "greater_or_equal": "greater_or_equal",
        "GreaterOrEqual": "greater_or_equal",
    },
    "freshness": {
        "fresh": "fresh",
        "Fresh": "fresh",
        "fresh_or_unverified": "fresh_or_unverified",
        "FreshOrUnverified": "fresh_or_unverified",
    },
    "edge": {
        "becomes_true": "becomes_true",
        "BecomesTrue": "becomes_true",
        "becomes_false": "becomes_false",
        "BecomesFalse": "becomes_false",
    },
}


class CompileError(ValueError):
    """A user-facing schema or normalization failure."""

    def __init__(self, path: str, message: str) -> None:
        super().__init__(message)
        self.path = path
        self.message = message


class _YamlDurationScalar(str):
    """A YAML numeric duration lexeme retained before double rounding."""


def _path(parent: str, child: str) -> str:
    return f"{parent}.{child}" if parent else child


def _index(parent: str, index: int) -> str:
    return f"{parent}[{index}]"


def _mapping(value: Any, path: str) -> Mapping[str, Any]:
    if not isinstance(value, dict):
        raise CompileError(path, "expected a mapping")
    if any(not isinstance(key, str) for key in value):
        raise CompileError(path, "mapping keys must be strings")
    return value


def _sequence(value: Any, path: str) -> Sequence[Any]:
    if not isinstance(value, list):
        raise CompileError(path, "expected a sequence")
    return value


def _fields(
    value: Mapping[str, Any],
    path: str,
    required: Iterable[str],
    optional: Iterable[str] = (),
) -> None:
    allowed = set(required) | set(optional)
    unknown = sorted(set(value) - allowed)
    if unknown:
        raise CompileError(_path(path, unknown[0]), f"unknown field '{unknown[0]}'")
    missing = sorted(set(required) - set(value))
    if missing:
        raise CompileError(_path(path, missing[0]), "required field is missing")


def _string(value: Any, path: str) -> str:
    if not isinstance(value, str):
        raise CompileError(path, "expected a string")
    if "\0" in value:
        raise CompileError(path, "strings must not contain NUL characters")
    return value


def _non_blank_string(value: Any, path: str, description: str) -> str:
    text = _string(value, path)
    if not text.strip():
        raise CompileError(path, f"{description} must not be empty")
    return text


def _runtime_float(value: int | float, path: str) -> float:
    """Round a number exactly as the persisted C++ model does."""
    try:
        converted = struct.unpack("<f", struct.pack("<f", float(value)))[0]
    except (OverflowError, ValueError):
        raise CompileError(path, "number must be finite") from None
    if not math.isfinite(converted):
        raise CompileError(path, "number must be finite")
    return converted


def _finite_number(value: Any, path: str) -> int | float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise CompileError(path, "expected a finite number")
    converted = _runtime_float(value, path)
    if abs(converted) > FLOAT_MAX:
        raise CompileError(path, "number must be finite")
    if converted.is_integer():
        return int(converted)
    return converted


def _integer(value: Any, path: str, description: str = "integer") -> int:
    # Integer fields retain the source precision; float32 normalization is only
    # appropriate for fields stored as float in the persisted runtime model.
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise CompileError(path, f"{description} must be an integer")
    if isinstance(value, float) and (not math.isfinite(value) or not value.is_integer()):
        raise CompileError(path, f"{description} must be an integer")
    return int(value)


def _normalize_duration(value: Any, path: str, field: Mapping[str, Any]) -> int:
    if isinstance(value, _YamlDurationScalar):
        try:
            exact = Decimal(value.replace("_", ""))
        except InvalidOperation:
            raise CompileError(path, "duration_ms must be an integer") from None
        if not exact.is_finite() or exact != exact.to_integral_value():
            raise CompileError(path, "duration_ms must be an integer")
        duration = exact
    else:
        duration = _integer(value, path, "duration_ms")
    # Check before int conversion to avoid allocating huge exponent values.
    # This bound stays exact in JSON's double parser and fits uint64_t after
    # the runtime millisecond-to-microsecond conversion.
    minimum_ms, maximum_ms = field["min"], field["max"]
    if not minimum_ms <= duration <= maximum_ms:
        raise CompileError(path, f"duration_ms must be in {minimum_ms}..{maximum_ms}")
    return int(duration)


def _enum(value: Any, path: str, kind: str) -> str:
    name = _string(value, path)
    normalized = ENUMS[kind].get(name)
    if normalized is None:
        raise CompileError(path, f"unsupported {kind} '{name}'")
    return normalized


def _load_signal_catalog() -> Mapping[str, Mapping[str, Any]]:
    try:
        source = SIGNAL_CATALOG_PATH.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        raise CompileError("$", f"cannot read signal catalog: {error}") from error
    try:
        document = json.loads(source)
    except json.JSONDecodeError as error:
        raise CompileError("$", f"malformed signal catalog: {error.msg}") from error

    root = _mapping(document, "signal_catalog")
    _fields(root, "signal_catalog", ("version", "signals"), ("source",))
    version = _integer(root["version"], "signal_catalog.version", "catalog version")
    if version != VERSION:
        raise CompileError(
            "signal_catalog.version",
            f"unsupported signal catalog version {version}; expected {VERSION}",
        )

    signals: dict[str, Mapping[str, Any]] = {}
    raw_signals = _mapping(root["signals"], "signal_catalog.signals")
    for key, raw_signal in raw_signals.items():
        path = _path("signal_catalog.signals", key)
        metadata = _mapping(raw_signal, path)
        _fields(metadata, path, ("type", "capabilities"), ("choices",))
        signal_type = _string(metadata["type"], _path(path, "type"))
        if signal_type not in SIGNAL_TYPES:
            raise CompileError(_path(path, "type"), f"unsupported signal type '{signal_type}'")
        capabilities = _sequence(metadata["capabilities"], _path(path, "capabilities"))
        normalized_capabilities: set[str] = set()
        for index, capability in enumerate(capabilities):
            capability_path = _index(_path(path, "capabilities"), index)
            name = _string(capability, capability_path)
            if name not in {"read", "notify"}:
                raise CompileError(capability_path, f"unsupported signal capability '{name}'")
            normalized_capabilities.add(name)
        if not normalized_capabilities:
            raise CompileError(_path(path, "capabilities"), "signal must expose a capability")

        choices: tuple[str, ...] = ()
        if signal_type == "enum":
            if "choices" not in metadata:
                raise CompileError(_path(path, "choices"), "enum signal choices are required")
            raw_choices = _sequence(metadata["choices"], _path(path, "choices"))
            normalized_choices: list[str] = []
            for index, choice in enumerate(raw_choices):
                choice_path = _index(_path(path, "choices"), index)
                name = _non_blank_string(choice, choice_path, "signal choice")
                if name in normalized_choices:
                    raise CompileError(choice_path, "signal choices must be unique")
                normalized_choices.append(name)
            if not normalized_choices:
                raise CompileError(_path(path, "choices"), "enum signal choices must not be empty")
            choices = tuple(normalized_choices)
        elif "choices" in metadata:
            raise CompileError(_path(path, "choices"), "only enum signals may declare choices")

        signals[key] = {
            "type": signal_type,
            "capabilities": frozenset(normalized_capabilities),
            "choices": choices,
        }
    return signals


def _validate_rule_signal(rule: Mapping[str, Any], path: str,
                          catalog: Mapping[str, Mapping[str, Any]]) -> None:
    signal_key = rule["signal_key"]
    metadata = catalog.get(signal_key)
    if metadata is None:
        raise CompileError(_path(path, "signal_key"), f"unknown signal '{signal_key}'")
    required_capability = RULE_CAPABILITIES[rule["type"]]
    if required_capability not in metadata["capabilities"]:
        raise CompileError(
            _path(path, "type"),
            f"signal '{signal_key}' does not support {required_capability} delivery",
        )
    if rule["type"] == "range":
        if metadata["type"] != "number":
            raise CompileError(
                _path(path, "signal_key"),
                f"range signal '{signal_key}' must be numeric",
            )
        return

    operand = rule["operand"]
    operand_kind = next(iter(operand))
    expected_type = {"boolean": "boolean", "number": "number", "choice": "enum"}[operand_kind]
    if metadata["type"] != expected_type:
        raise CompileError(
            _path(path, "operand"),
            f"operand type '{operand_kind}' does not match signal '{signal_key}'",
        )
    if operand_kind == "choice" and operand["choice"] not in metadata["choices"]:
        raise CompileError(
            _path(path, "operand.choice"),
            f"unknown choice '{operand['choice']}' for signal '{signal_key}'",
        )


def _normalize_actions(value: Any) -> tuple[list[dict[str, str]], set[str]]:
    raw_actions = _sequence(value, "actions")
    actions: list[dict[str, str]] = []
    names: set[str] = set()
    for index, raw_action in enumerate(raw_actions):
        path = _index("actions", index)
        action = _mapping(raw_action, path)
        _fields(action, path, ("name",))
        name = _non_blank_string(action["name"], _path(path, "name"), "action name")
        if name in names:
            raise CompileError(_path(path, "name"), "action name must be unique")
        names.add(name)
        actions.append({"name": name})
    return actions, names


def _normalize_operand(value: Any, path: str) -> dict[str, Any]:
    """Normalize the persisted one-key operand union."""
    operand = _mapping(value, path)
    _fields(operand, path, (), ("boolean", "number", "choice"))
    if len(operand) != 1:
        raise CompileError(path, "operand must contain exactly one of boolean, number, or choice")
    kind, operand_value = next(iter(operand.items()))
    value_path = _path(path, kind)
    if kind == "boolean":
        if not isinstance(operand_value, bool):
            raise CompileError(value_path, "boolean operand must be true or false")
        normalized_value: Any = operand_value
    elif kind == "number":
        normalized_value = _finite_number(operand_value, value_path)
    else:
        normalized_value = _non_blank_string(operand_value, value_path, "choice operand")
    return {kind: normalized_value}


def _infer_operand(value: Any, path: str) -> dict[str, Any]:
    if isinstance(value, bool):
        return {"boolean": value}
    if isinstance(value, (int, float)):
        return {"number": _finite_number(value, path)}
    if isinstance(value, str):
        return {"choice": _non_blank_string(value, path, "choice operand")}
    raise CompileError(path, "shorthand rule value must be a boolean, number, or string")


def _normalize_range(value: Any, path: str) -> dict[str, int | float]:
    range_value = _mapping(value, path)
    _fields(range_value, path, ("from", "to"))
    return {
        "from": _finite_number(range_value["from"], _path(path, "from")),
        "to": _finite_number(range_value["to"], _path(path, "to")),
    }


def _validate_range_span(value: Mapping[str, int | float], path: str) -> None:
    span = float(value["to"]) - float(value["from"])
    try:
        _runtime_float(span, path)
    except CompileError as error:
        raise CompileError(path, "range span must be finite") from error


def _normalize_rule(value: Any, path: str, actions: set[str]) -> tuple[dict[str, Any], bool]:
    rule = _mapping(value, path)
    _fields(
        rule,
        path,
        ("type", "action", "signal_key"),
        (
            "comparison",
            "operand",
            "value",
            "freshness",
            "edge",
            "release_threshold",
            "input",
            "output",
        ),
    )
    rule_type = _enum(rule["type"], _path(path, "type"), "type")
    if rule_type not in {"state", "sampled_state", "event", "range"}:
        raise CompileError(_path(path, "type"), f"unsupported rule type '{rule_type}'")
    action = _non_blank_string(rule["action"], _path(path, "action"), "action reference")
    if action not in actions:
        raise CompileError(_path(path, "action"), f"action '{action}' is not declared")
    signal_key = _non_blank_string(rule["signal_key"], _path(path, "signal_key"), "signal key")
    freshness = _enum(rule.get("freshness", DEFAULT_FRESHNESS), _path(path, "freshness"), "freshness")
    normalized: dict[str, Any] = {
        "type": rule_type,
        "action": action,
        "signal_key": signal_key,
        "freshness": freshness,
    }

    if rule_type == "range":
        _fields(rule, path, ("type", "action", "signal_key", "input", "output"), ("freshness",))
        input_range = _normalize_range(rule["input"], _path(path, "input"))
        output_range = _normalize_range(rule["output"], _path(path, "output"))
        _validate_range_span(input_range, _path(path, "input"))
        _validate_range_span(output_range, _path(path, "output"))
        if not input_range["from"] < input_range["to"]:
            raise CompileError(_path(path, "input"), "input range 'from' must be less than 'to'")
        normalized["input"] = input_range
        normalized["output"] = output_range
        return normalized, True

    if "operand" in rule and "value" in rule:
        raise CompileError(path, "use either 'operand' or shorthand 'value', not both")
    if "operand" not in rule and "value" not in rule:
        raise CompileError(_path(path, "operand"), "required field is missing")
    if "comparison" not in rule:
        raise CompileError(_path(path, "comparison"), "required field is missing")

    allowed = {"type", "action", "signal_key", "comparison", "freshness", "operand", "value"}
    if rule_type == "event":
        allowed.add("edge")
    if rule_type == "sampled_state":
        allowed.add("release_threshold")
    _fields(rule, path, ("type", "action", "signal_key", "comparison"), allowed - {"type", "action", "signal_key", "comparison"})

    normalized["comparison"] = _enum(rule["comparison"], _path(path, "comparison"), "comparison")
    operand_path = _path(path, "operand")
    normalized["operand"] = (
        _normalize_operand(rule["operand"], operand_path)
        if "operand" in rule
        else _infer_operand(rule["value"], _path(path, "value"))
    )
    if normalized["comparison"] not in {"equal", "not_equal"} and "number" not in normalized["operand"]:
        raise CompileError(operand_path, "ordered comparisons require a numeric operand")

    if rule_type == "event":
        if "edge" not in rule:
            raise CompileError(_path(path, "edge"), "required field is missing")
        normalized["edge"] = _enum(rule["edge"], _path(path, "edge"), "edge")
    if rule_type == "sampled_state" and "release_threshold" in rule:
        if "number" not in normalized["operand"] or normalized["comparison"] in {"equal", "not_equal"}:
            raise CompileError(
                _path(path, "release_threshold"),
                "release threshold requires an ordered numeric comparison",
            )
        release = _finite_number(rule["release_threshold"], _path(path, "release_threshold"))
        activation = normalized["operand"]["number"]
        if normalized["comparison"] in {"greater", "greater_or_equal"} and not release < activation:
            raise CompileError(_path(path, "release_threshold"), "release threshold must be below activation")
        if normalized["comparison"] in {"less", "less_or_equal"} and not release > activation:
            raise CompileError(_path(path, "release_threshold"), "release threshold must be above activation")
        normalized["release_threshold"] = release
    return normalized, rule_type != "event"


def _catalog_enum(value: Any, path: str, kind: str, names: Sequence[str]) -> str:
    name = _string(value, path)
    for canonical in names:
        if name in (canonical, "".join(part.capitalize() for part in canonical.split("_"))):
            return canonical
    raise CompileError(path, f"unsupported {kind} '{name}'")


def load_output_catalog(path: Path) -> Mapping[str, Any]:
    """Read the generated catalog; descriptor validation belongs to its generator."""
    try:
        root = _mapping(json.loads(path.read_text(encoding="utf-8")), "output_catalog")
        _fields(root, "output_catalog", ("outputs",), ("sinks",))
        _sequence(root["outputs"], "output_catalog.outputs")
        return root
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise CompileError("output_catalog", f"cannot read output catalog: {error}") from error


def _bounded_integer(value: Any, path: str, field: Mapping[str, Any], description: str) -> int:
    number = _integer(value, path, description)
    if not field["min"] <= number <= field["max"]:
        raise CompileError(path, f"{description} must be in {field['min']}..{field['max']}")
    return number


def _normalize_zone(value: Any, path: str, field: Mapping[str, Any],
                    settings: Mapping[str, int]) -> dict[str, Any]:
    zone = _mapping(value, path)
    _fields(zone, path, ("start", "length", "direction"))
    start = _integer(zone["start"], _path(path, "start"), "zone start")
    length = _integer(zone["length"], _path(path, "length"), "zone length")
    direction = _catalog_enum(zone["direction"], _path(path, "direction"),
                              "direction", field["directions"])
    pixels = field["pixels"]
    if "pixels_setting" in field:
        pixels = min(pixels, settings[field["pixels_setting"]])
    if start < 0 or length <= 0 or start >= pixels or length > pixels - start:
        raise CompileError(path, "LED zone must be non-empty and fit within the strip")
    return {"start": start, "length": length, "direction": direction}


def _normalize_color(value: Any, path: str, field: Mapping[str, Any]) -> dict[str, int]:
    color = _mapping(value, path)
    _fields(color, path, ("red", "green", "blue"))
    return {channel: _bounded_integer(color[channel], _path(path, channel), field, "RGB channel")
            for channel in ("red", "green", "blue")}


def _normalize_sinks(value: Any, catalog: Mapping[str, Any]) -> tuple[dict[str, Any], dict[str, Any]]:
    sinks = _mapping(value, "sinks")
    descriptors = {entry["sink"]: entry["settings"] for entry in catalog.get("sinks", [])}
    _fields(sinks, "sinks", (), descriptors)
    normalized: dict[str, Any] = {}
    resolved: dict[str, Any] = {}
    for sink, fields in descriptors.items():
        path = _path("sinks", sink)
        raw = _mapping(sinks.get(sink, {}), path)
        _fields(raw, path, (), (field["name"] for field in fields))
        resolved[sink] = {
            field["name"]: _bounded_integer(raw.get(field["name"], field["default"]),
                _path(path, field["name"]), field, field["name"])
            for field in fields
        }
        if sink in sinks:
            normalized[sink] = {name: resolved[sink][name] for name in raw}
    return normalized, resolved


def _output_targets(output: dict[str, Any]) -> tuple[tuple[Any, ...], ...]:
    """Return the physical targets an output occupies, mirroring the C++ validator.

    Solid zones are bound as fills, so a solid shares the fill identity (direction
    included) while solids compare among themselves by action and zone alone.
    """
    output_type = output["type"]
    action = output["action"]
    if "effect" in output:
        return ((output_type, action, output["effect"]),)
    if "zone" not in output:
        return ((output_type, action),)
    zone = output["zone"]
    placement = (action, zone["start"], zone["length"])
    fill_slot = ("fill_slot",) + placement + (zone["direction"],)
    if output_type == "led_fill":
        return (fill_slot,)
    if output_type == "led_solid":
        return ((output_type,) + placement, fill_slot)
    return ((output_type,) + placement,)


def _normalize_output(value: Any, path: str, actions: set[str], rules: Sequence[Mapping[str, Any]],
                      catalog: Mapping[str, Any], settings: Mapping[str, Any]) -> dict[str, Any]:
    output = _mapping(value, path)
    if "type" not in output:
        raise CompileError(_path(path, "type"), "required field is missing")
    descriptors = {entry["type"]: entry for entry in catalog["outputs"]}
    try:
        output_type = _catalog_enum(output["type"], _path(path, "type"), "type", list(descriptors))
    except CompileError:
        if isinstance(output["type"], str) and output["type"] in ENUMS["type"]:
            raise CompileError(_path(path, "type"), f"unsupported output type '{output['type']}'") from None
        raise
    descriptor = descriptors[output_type]
    fields = descriptor["fields"]
    _fields(output, path, ("type", *(field["name"] for field in fields if field["required"])),
            (field["name"] for field in fields if not field["required"]))
    normalized: dict[str, Any] = {"type": output_type}
    # Preserve the established diagnostic order: action, optional integers, then payload.
    ordered_fields = sorted(fields, key=lambda field: (
        0 if field["kind"] == "action" else
        1 if field["kind"] == "integer" and not field["required"] else 2))
    for field in ordered_fields:
        name, kind = field["name"], field["kind"]
        field_path = _path(path, name)
        raw = output[name] if name in output else field["default"]
        if kind == "action":
            result = _non_blank_string(raw, field_path, "action reference")
            if result not in actions:
                raise CompileError(field_path, f"action '{result}' is not declared")
            if "rule_types" in descriptor and any(
                rule["action"] == result and rule["type"] not in descriptor["rule_types"] for rule in rules
            ):
                allowed = " or ".join(descriptor["rule_types"])
                raise CompileError(field_path, f"{output_type} action must be driven by a {allowed} rule")
        elif kind == "integer":
            result = (_normalize_duration(raw, field_path, field) if name == "duration_ms" else
                      _bounded_integer(raw, field_path, field, name))
        elif kind == "enum":
            result = _catalog_enum(raw, field_path, name, field["values"])
        elif kind == "zone":
            result = _normalize_zone(raw, field_path, field, settings.get(descriptor["sink"], {}))
        elif kind == "rgb":
            result = _normalize_color(raw, field_path, field)
        else:
            raise CompileError(field_path, f"unsupported catalog field kind '{kind}'")
        normalized[name] = result
    return normalized


def normalize_document(document: Any, output_catalog: Mapping[str, Any]) -> dict[str, Any]:
    """Validate and normalize a loaded YAML document to persisted JSON data."""
    root = _mapping(document, "$")
    _fields(root, "", ("version",), ("actions", "rules", "outputs", "sinks"))
    version = _integer(root["version"], "version", "version")
    if version != VERSION:
        raise CompileError("version", f"unsupported configuration version {version}; expected {VERSION}")

    actions, action_names = _normalize_actions(root.get("actions", []))
    signal_catalog = _load_signal_catalog()
    rules: list[dict[str, Any]] = []
    level_actions: set[str] = set()
    for index, raw_rule in enumerate(_sequence(root.get("rules", []), "rules")):
        path = _index("rules", index)
        rule, is_level = _normalize_rule(raw_rule, path, action_names)
        _validate_rule_signal(rule, path, signal_catalog)
        if is_level and rule["action"] in level_actions:
            raise CompileError(_path(path, "action"), "an action may have only one level rule")
        if is_level:
            level_actions.add(rule["action"])
        rules.append(rule)

    sinks, settings = _normalize_sinks(root.get("sinks", {}), output_catalog)
    outputs: list[dict[str, Any]] = []
    output_targets: set[tuple[Any, ...]] = set()
    for index, raw_output in enumerate(_sequence(root.get("outputs", []), "outputs")):
        path = _index("outputs", index)
        output = _normalize_output(raw_output, path, action_names, rules, output_catalog, settings)
        targets = _output_targets(output)
        if any(target in output_targets for target in targets):
            raise CompileError(path, "output binding target must be unique")
        output_targets.update(targets)
        outputs.append(output)

    normalized = {"version": version, "actions": actions, "rules": rules, "outputs": outputs}
    if "sinks" in root:
        normalized["sinks"] = sinks
    return normalized


def _yaml_loader() -> Any:
    if yaml is None:
        raise CompileError("$", "PyYAML is required; install tools/requirements.txt")

    class UniqueKeyLoader(yaml.SafeLoader):  # type: ignore[misc]
        """SafeLoader variant that rejects silently overwritten YAML keys."""

    # PyYAML's default YAML 1.1 resolver turns ``off``/``on``/``yes``/``no``
    # into booleans. Controller signal choices use those words legitimately,
    # so keep only the JSON/YAML 1.2 boolean spellings.
    UniqueKeyLoader.yaml_implicit_resolvers = {
        key: list(resolvers)
        for key, resolvers in yaml.SafeLoader.yaml_implicit_resolvers.items()
    }
    bool_tag = "tag:yaml.org,2002:bool"
    for key, resolvers in UniqueKeyLoader.yaml_implicit_resolvers.items():
        UniqueKeyLoader.yaml_implicit_resolvers[key] = [
            (tag, pattern) for tag, pattern in resolvers if tag != bool_tag
        ]
    UniqueKeyLoader.add_implicit_resolver(
        bool_tag,
        re.compile(r"^(?:true|True|TRUE|false|False|FALSE)$"),
        list("tTfF"),
    )

    def construct_mapping(loader: Any, node: Any, deep: bool = False) -> dict[str, Any]:
        mapping: dict[str, Any] = {}
        for key_node, value_node in node.value:
            key = loader.construct_object(key_node, deep=deep)
            if not isinstance(key, str):
                raise yaml.constructor.ConstructorError(
                    "while constructing a mapping", node.start_mark, "keys must be strings", key_node.start_mark
                )
            if key in mapping:
                raise yaml.constructor.ConstructorError(
                    "while constructing a mapping", node.start_mark, f"duplicate key '{key}'", key_node.start_mark
                )
            # Retain numeric lexemes for every duration_ms mapping key. Schema
            # validation currently permits that field only in transient outputs;
            # a future duration_ms field must opt into _normalize_duration or
            # narrow this interception before accepting ordinary float values.
            if key == "duration_ms" and value_node.tag == "tag:yaml.org,2002:float":
                mapping[key] = _YamlDurationScalar(value_node.value)
            else:
                mapping[key] = loader.construct_object(value_node, deep=deep)
        return mapping

    UniqueKeyLoader.add_constructor(yaml.resolver.BaseResolver.DEFAULT_MAPPING_TAG, construct_mapping)
    return UniqueKeyLoader


def load_yaml(path: Path) -> Any:
    if yaml is None:
        _yaml_loader()
    try:
        source = path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        raise CompileError("$", f"cannot read input: {error}") from error
    try:
        return yaml.load(source, Loader=_yaml_loader())
    except RecursionError as error:
        raise CompileError("$", "YAML nesting exceeds the supported parser depth") from error
    except ValueError as error:
        raise CompileError("$", f"malformed YAML scalar: {error}") from error
    except yaml.YAMLError as error:  # type: ignore[union-attr]
        mark = getattr(error, "problem_mark", None)
        location = ""
        if mark is not None:
            location = f" at line {mark.line + 1}, column {mark.column + 1}"
        detail = getattr(error, "problem", None) or str(error)
        raise CompileError("$", f"malformed YAML{location}: {detail}") from error


def compile_file(input_path: Path, output_path: Path, output_catalog_path: Path) -> None:
    try:
        document = load_yaml(input_path)
        normalized = normalize_document(document, load_output_catalog(output_catalog_path))
    except RecursionError as error:
        raise CompileError("$", "configuration nesting exceeds the supported depth") from error
    payload = json.dumps(normalized, sort_keys=True, separators=(",", ":"), allow_nan=False) + "\n"
    if len(payload.encode("utf-8")) > MAX_JSON_BYTES:
        raise CompileError("$", f"compiled JSON exceeds the maximum supported size of {MAX_JSON_BYTES} bytes")
    try:
        output_path.write_text(payload, encoding="utf-8")
    except OSError as error:
        raise CompileError("$", f"cannot write output: {error}") from error


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="authoring YAML configuration")
    parser.add_argument("output", type=Path, help="canonical JSON output")
    parser.add_argument("--output-catalog", type=Path, required=True, help="generated output catalog JSON")
    args = parser.parse_args(argv)
    try:
        compile_file(args.input, args.output, args.output_catalog)
    except CompileError as error:
        print(f"ERROR: {args.input}: {error.path}: {error.message}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
