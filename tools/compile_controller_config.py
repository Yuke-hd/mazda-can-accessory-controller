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
import sys
from pathlib import Path
from typing import Any, Iterable, Mapping, Sequence

try:
    import yaml
except ModuleNotFoundError:  # pragma: no cover - exercised through the CLI
    yaml = None  # type: ignore[assignment]


VERSION = 1
PIXEL_COUNT = 100
DEFAULT_FRESHNESS = "fresh"
DEFAULT_PRIORITY = 100
# The persisted C++ model stores numeric fields as float. Reject values that
# the runtime model would convert to infinity even when Python can represent
# them as a finite double.
FLOAT_MAX = 3.4028234663852886e38

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
        "led_effect": "led_effect",
        "LedEffect": "led_effect",
        "led_fill": "led_fill",
        "LedFill": "led_fill",
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
    "effect": {
        "left_turn": "left_turn",
        "LeftTurn": "left_turn",
        "right_turn": "right_turn",
        "RightTurn": "right_turn",
        "brake": "brake",
        "Brake": "brake",
    },
    "direction": {
        "start_to_end": "start_to_end",
        "StartToEnd": "start_to_end",
        "end_to_start": "end_to_start",
        "EndToStart": "end_to_start",
        "center_out": "center_out",
        "CenterOut": "center_out",
    },
}


class CompileError(ValueError):
    """A user-facing schema or normalization failure."""

    def __init__(self, path: str, message: str) -> None:
        super().__init__(message)
        self.path = path
        self.message = message


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
    return value


def _non_blank_string(value: Any, path: str, description: str) -> str:
    text = _string(value, path)
    if not text.strip():
        raise CompileError(path, f"{description} must not be empty")
    return text


def _finite_number(value: Any, path: str) -> int | float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise CompileError(path, "expected a finite number")
    try:
        converted = float(value)
    except (OverflowError, ValueError):
        raise CompileError(path, "number must be finite") from None
    if not math.isfinite(converted) or abs(converted) > FLOAT_MAX:
        raise CompileError(path, "number must be finite")
    if converted.is_integer():
        return int(converted)
    return converted


def _integer(value: Any, path: str, description: str = "integer") -> int:
    normalized = _finite_number(value, path)
    if not isinstance(normalized, int):
        raise CompileError(path, f"{description} must be an integer")
    return normalized


def _enum(value: Any, path: str, kind: str) -> str:
    name = _string(value, path)
    normalized = ENUMS[kind].get(name)
    if normalized is None:
        raise CompileError(path, f"unsupported {kind} '{name}'")
    return normalized


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
    if not math.isfinite(span) or abs(span) > FLOAT_MAX:
        raise CompileError(path, "range span must be finite")


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


def _normalize_zone(value: Any, path: str) -> dict[str, Any]:
    zone = _mapping(value, path)
    _fields(zone, path, ("start", "length", "direction"))
    start = _integer(zone["start"], _path(path, "start"), "zone start")
    length = _integer(zone["length"], _path(path, "length"), "zone length")
    direction = _enum(zone["direction"], _path(path, "direction"), "direction")
    if start < 0 or length <= 0 or start >= PIXEL_COUNT or length > PIXEL_COUNT - start:
        raise CompileError(path, "LED zone must be non-empty and fit within the strip")
    return {"start": start, "length": length, "direction": direction}


def _normalize_color(value: Any, path: str) -> dict[str, int]:
    color = _mapping(value, path)
    _fields(color, path, ("red", "green", "blue"))
    channels: dict[str, int] = {}
    for channel in ("red", "green", "blue"):
        channel_path = _path(path, channel)
        channel_value = _integer(color[channel], channel_path, "RGB channel")
        if not 0 <= channel_value <= 255:
            raise CompileError(channel_path, "RGB channel must be in 0..255")
        channels[channel] = channel_value
    return channels


def _normalize_output(value: Any, path: str, actions: set[str]) -> tuple[dict[str, Any], tuple[Any, ...]]:
    output = _mapping(value, path)
    _fields(output, path, ("type", "action"), ("effect", "zone", "color", "priority"))
    output_type = _enum(output["type"], _path(path, "type"), "type")
    if output_type not in {"led_effect", "led_fill"}:
        raise CompileError(_path(path, "type"), f"unsupported output type '{output_type}'")
    action = _non_blank_string(output["action"], _path(path, "action"), "action reference")
    if action not in actions:
        raise CompileError(_path(path, "action"), f"action '{action}' is not declared")
    priority = _integer(output.get("priority", DEFAULT_PRIORITY), _path(path, "priority"), "priority")
    if not 0 <= priority <= 255:
        raise CompileError(_path(path, "priority"), "priority must be in 0..255")

    if output_type == "led_effect":
        _fields(output, path, ("type", "action", "effect"), ("priority",))
        effect = _enum(output["effect"], _path(path, "effect"), "effect")
        return (
            {"type": output_type, "action": action, "effect": effect, "priority": priority},
            (output_type, action, effect),
        )

    _fields(output, path, ("type", "action", "zone", "color"), ("priority",))
    zone = _normalize_zone(output["zone"], _path(path, "zone"))
    color = _normalize_color(output["color"], _path(path, "color"))
    return (
        {"type": output_type, "action": action, "zone": zone, "color": color, "priority": priority},
        (output_type, action, zone["start"], zone["length"], zone["direction"]),
    )


def normalize_document(document: Any) -> dict[str, Any]:
    """Validate and normalize a loaded YAML document to persisted JSON data."""
    root = _mapping(document, "$")
    _fields(root, "", ("version", "actions", "rules", "outputs"))
    version = _integer(root["version"], "version", "version")
    if version != VERSION:
        raise CompileError("version", f"unsupported configuration version {version}; expected {VERSION}")

    actions, action_names = _normalize_actions(root["actions"])
    rules: list[dict[str, Any]] = []
    level_actions: set[str] = set()
    for index, raw_rule in enumerate(_sequence(root["rules"], "rules")):
        path = _index("rules", index)
        rule, is_level = _normalize_rule(raw_rule, path, action_names)
        if is_level and rule["action"] in level_actions:
            raise CompileError(_path(path, "action"), "an action may have only one level rule")
        if is_level:
            level_actions.add(rule["action"])
        rules.append(rule)

    outputs: list[dict[str, Any]] = []
    output_targets: set[tuple[Any, ...]] = set()
    for index, raw_output in enumerate(_sequence(root["outputs"], "outputs")):
        path = _index("outputs", index)
        output, target = _normalize_output(raw_output, path, action_names)
        if target in output_targets:
            raise CompileError(path, "output binding target must be unique")
        output_targets.add(target)
        outputs.append(output)

    return {"version": version, "actions": actions, "rules": rules, "outputs": outputs}


def _yaml_loader() -> Any:
    if yaml is None:
        raise CompileError("$", "PyYAML is required; install tools/requirements.txt")

    class UniqueKeyLoader(yaml.SafeLoader):  # type: ignore[misc]
        """SafeLoader variant that rejects silently overwritten YAML keys."""

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
    except yaml.YAMLError as error:  # type: ignore[union-attr]
        mark = getattr(error, "problem_mark", None)
        location = ""
        if mark is not None:
            location = f" at line {mark.line + 1}, column {mark.column + 1}"
        detail = getattr(error, "problem", None) or str(error)
        raise CompileError("$", f"malformed YAML{location}: {detail}") from error


def compile_file(input_path: Path, output_path: Path) -> None:
    document = load_yaml(input_path)
    normalized = normalize_document(document)
    payload = json.dumps(normalized, sort_keys=True, separators=(",", ":"), allow_nan=False) + "\n"
    try:
        output_path.write_text(payload, encoding="utf-8")
    except OSError as error:
        raise CompileError("$", f"cannot write output: {error}") from error


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="authoring YAML configuration")
    parser.add_argument("output", type=Path, help="canonical JSON output")
    args = parser.parse_args(argv)
    try:
        compile_file(args.input, args.output)
    except CompileError as error:
        print(f"ERROR: {args.input}: {error.path}: {error.message}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
