"""Compile the built-in factory features supported by one selected profile."""

from __future__ import annotations

import argparse
from collections import defaultdict
from collections.abc import Mapping, Sequence
from copy import deepcopy
import json
from pathlib import Path
import sys
import tempfile
from typing import Any

if __package__:
    from . import compile_controller_config as compiler
else:
    import compile_controller_config as compiler


def _profile_fresh_signals(profile: Any) -> set[str]:
    if not isinstance(profile, Mapping) or not isinstance(profile.get("bindings"), list):
        raise ValueError("selected vehicle profile must contain a bindings array")

    fresh: set[str] = set()
    seen: set[str] = set()
    for index, binding in enumerate(profile["bindings"]):
        path = f"bindings[{index}]"
        if not isinstance(binding, Mapping) or not isinstance(binding.get("key"), str):
            raise ValueError(f"selected vehicle profile has an invalid {path}.key")
        key = binding["key"]
        if key in seen:
            raise ValueError(f"selected vehicle profile repeats binding key '{key}'")
        seen.add(key)
        freshness = binding.get("freshness")
        if freshness is None:
            continue
        if not isinstance(freshness, Mapping):
            raise ValueError(f"selected vehicle profile has an invalid {path}.freshness")
        timeout = freshness.get("timeout_ms")
        if isinstance(timeout, bool) or not isinstance(timeout, int) or timeout <= 0:
            raise ValueError(f"selected vehicle profile has an invalid {path}.freshness.timeout_ms")
        fresh.add(key)
    return fresh


def _rule_requirement(rule: Mapping[str, Any]) -> tuple[str, str, str, str | None]:
    rule_type = rule.get("type")
    signal_key = rule.get("signal_key")
    if not isinstance(signal_key, str):
        raise ValueError("built-in factory rule has no signal_key")

    if rule_type in {"range", "sampled_state"}:
        signal_type = "number"
        capability = "read"
        choice = None
    elif rule_type in {"state", "event"}:
        capability = "notify"
        operand = rule.get("operand")
        if isinstance(operand, Mapping) and len(operand) == 1:
            kind, value = next(iter(operand.items()))
            signal_type = {"boolean": "boolean", "number": "number", "choice": "enum"}.get(kind, "")
            choice = value if kind == "choice" and isinstance(value, str) else None
        elif isinstance(operand, bool):
            signal_type, choice = "boolean", None
        elif isinstance(operand, (int, float)):
            signal_type, choice = "number", None
        elif isinstance(operand, str):
            signal_type, choice = "enum", operand
        else:
            signal_type, choice = "", None
        if not signal_type:
            raise ValueError(f"built-in factory rule for '{signal_key}' has an unsupported operand")
    else:
        raise ValueError(f"built-in factory rule has unsupported type '{rule_type}'")
    return signal_key, signal_type, capability, choice


def filter_factory_document(
    document: Any,
    signal_catalog: Mapping[str, Mapping[str, Any]],
    profile: Any,
    *,
    native: bool,
) -> tuple[dict[str, Any], list[str]]:
    """Drop unsupported built-in actions with their rules and output bindings."""
    if not isinstance(document, Mapping):
        raise ValueError("factory configuration must be a mapping")
    result = deepcopy(document)
    if native:
        return result, []

    fresh_signals = _profile_fresh_signals(profile)
    raw_actions = result.get("actions", [])
    raw_rules = result.get("rules", [])
    raw_outputs = result.get("outputs", [])
    if not all(isinstance(value, list) for value in (raw_actions, raw_rules, raw_outputs)):
        # Leave schema failures for the existing strict compiler.
        return result, []

    rules_by_action: dict[str, list[Mapping[str, Any]]] = defaultdict(list)
    for rule in raw_rules:
        if isinstance(rule, Mapping) and isinstance(rule.get("action"), str):
            rules_by_action[rule["action"]].append(rule)

    supported: set[str] = set()
    diagnostics: list[str] = []
    for action in raw_actions:
        if not isinstance(action, Mapping) or not isinstance(action.get("name"), str):
            # The strict compiler will report malformed action records.
            continue
        name = action["name"]
        requirements = rules_by_action.get(name, [])
        if not requirements:
            supported.add(name)
            continue

        reasons: list[str] = []
        checked: list[tuple[Mapping[str, Any], str]] = []
        for rule in requirements:
            key, expected_type, capability, choice = _rule_requirement(rule)
            metadata = signal_catalog.get(key)
            if metadata is None:
                reasons.append(f"missing input signal '{key}'")
                continue
            actual_type = metadata.get("type")
            if actual_type != expected_type:
                reasons.append(f"signal '{key}' requires {expected_type}; selected type is {actual_type}")
                continue
            if choice is not None and choice not in metadata.get("choices", ()):
                reasons.append(f"signal '{key}' is missing choice '{choice}'")
                continue
            if capability not in metadata.get("capabilities", ()):
                reasons.append(f"signal '{key}' is missing required '{capability}' capability")
                continue
            if key not in fresh_signals:
                reasons.append(f"signal '{key}' has no declared Fresh timeout")
                continue
            checked.append((rule, key))

        if reasons:
            diagnostics.extend(f"factory feature '{name}' disabled: {reason}" for reason in reasons)
        else:
            supported.add(name)
            for rule, _ in checked:
                rule["freshness"] = "fresh"

    built_in_actions = {action["name"] for action in raw_actions
                        if isinstance(action, Mapping) and isinstance(action.get("name"), str)}
    result["actions"] = [action for action in raw_actions
                          if not isinstance(action, Mapping) or not isinstance(action.get("name"), str)
                          or action["name"] not in built_in_actions or action["name"] in supported]
    result["rules"] = [rule for rule in raw_rules
                        if not isinstance(rule, Mapping) or not isinstance(rule.get("action"), str)
                        or rule["action"] not in built_in_actions or rule["action"] in supported]
    result["outputs"] = [output for output in raw_outputs
                         if not isinstance(output, Mapping) or not isinstance(output.get("action"), str)
                         or output["action"] not in built_in_actions or output["action"] in supported]
    return result, diagnostics


def generate(
    input_path: Path,
    output_path: Path,
    output_catalog_path: Path,
    signal_catalog_path: Path,
    *,
    profile_path: Path | None,
) -> list[str]:
    document = compiler.load_yaml(input_path)
    signal_catalog = compiler._load_signal_catalog(signal_catalog_path)
    profile = None
    if profile_path is not None:
        try:
            profile = json.loads(profile_path.read_text(encoding="utf-8"))
        except (OSError, UnicodeError, json.JSONDecodeError) as error:
            raise ValueError(f"cannot read selected vehicle profile: {error}") from error
    filtered, diagnostics = filter_factory_document(
        document, signal_catalog, profile, native=profile_path is None
    )

    output_path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="factory-config-", dir=output_path.parent) as temporary:
        filtered_yaml = Path(temporary) / "filtered.yaml"
        filtered_yaml.write_text(json.dumps(filtered, allow_nan=False), encoding="utf-8")
        compiler.compile_file(filtered_yaml, output_path, output_catalog_path, signal_catalog_path)
    return diagnostics


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="built-in factory authoring YAML")
    parser.add_argument("output", type=Path, help="canonical JSON output")
    parser.add_argument("--output-catalog", type=Path, required=True)
    parser.add_argument("--signal-catalog", type=Path, required=True)
    selection = parser.add_mutually_exclusive_group(required=True)
    selection.add_argument("--native", action="store_true")
    selection.add_argument("--profile-json", type=Path)
    args = parser.parse_args(argv)
    try:
        diagnostics = generate(
            args.input, args.output, args.output_catalog, args.signal_catalog,
            profile_path=args.profile_json,
        )
    except (compiler.CompileError, OSError, UnicodeError, ValueError) as error:
        print(f"ERROR: {args.input}: {error}", file=sys.stderr)
        return 2
    for diagnostic in diagnostics:
        print(f"WARNING: {diagnostic}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
