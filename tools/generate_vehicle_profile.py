#!/usr/bin/env python3
"""Generate/check one complete offline normalized-profile artifact set."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

from vehicle_profiles.artifact_set import check, contents, generation, json_bytes, publish
from vehicle_profiles.projections import project_runtime_profile
from vehicle_profiles.validation import HandlerPolicy, ValidationPolicies, load_profile


def read_json(data):
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ValueError(f"duplicate policy key {key}")
            result[key] = value
        return result
    def invalid(value):
        raise ValueError(f"non-finite policy number {value}")
    return json.loads(data, object_pairs_hook=pairs,
                      parse_constant=invalid)


def load_policies(data):
    """Read explicit v1 reviewed registry data; never import executable policy."""
    value = read_json(data)
    keys = {"version", "profile_revisions", "handlers", "evidence", "units",
            "unit_conversions", "invalid_value_policies", "freshness_policies"}
    if not isinstance(value, dict) or set(value) != keys or type(value["version"]) is not int or value["version"] != 1:
        raise ValueError("expected exact version-1 policy registry fields")
    def rows(name, types):
        result = value[name]
        if not isinstance(result, list):
            raise ValueError(f"{name} must be an array")
        for row in result:
            if not isinstance(row, list) or len(row) != len(types) or any(type(v) is not t for v, t in zip(row, types)):
                raise ValueError(f"invalid {name} record")
        return result
    def strings(items, name):
        if not isinstance(items, list) or any(type(v) is not str for v in items) or len(set(items)) != len(items):
            raise ValueError(f"{name} must be unique strings")
        return frozenset(items)
    def mapping(items, count, name):
        result = {}
        for row in items:
            key = row[0] if count == 1 else tuple(row[:count])
            if key in result:
                raise ValueError(f"duplicate {name} record")
            result[key] = tuple(row[count:])
        return result
    handlers = {}
    for name, revision, profiles, kind, unit, minimum, maximum in rows("handlers", (str, str, list, str, str, int, int)):
        if (name, revision) in handlers or minimum < 0 or maximum < minimum:
            raise ValueError("invalid or duplicate handler signature")
        handlers[name, revision] = HandlerPolicy(strings(profiles, "handler profiles"), kind, unit, minimum, maximum)
    evidence_rows = rows("evidence", (str, str, str))
    evidence = frozenset(tuple(row) for row in evidence_rows)
    if len(evidence) != len(evidence_rows):
        raise ValueError("duplicate evidence record")
    return ValidationPolicies(
        mapping(rows("profile_revisions", (str, str, str)), 1, "profile revision"), handlers, evidence,
        strings(value["units"], "units"),
        mapping(rows("unit_conversions", (str, str, str, str)), 2, "unit conversion"),
        strings(value["invalid_value_policies"], "invalid value policies"),
        mapping(rows("freshness_policies", (str, str, int, str, str)), 2, "freshness policy"))


def named_paths(values):
    result = {}
    for value in values:
        name, separator, path = value.partition("=")
        if not separator or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.:-]*", name) or not path or name in result:
            raise ValueError("inputs must use unique stable NAME=PATH assignments")
        result[name] = Path(path)
    return result


def generate(manifest, policies, sources, generator_inputs, revision):
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.:-]*", revision):
        raise ValueError("generator revision must be a stable identifier")
    manifest_data = manifest.read_bytes()
    policy_data = policies.read_bytes()
    profile = load_profile(manifest_data.decode("utf-8"))
    expected = {source["id"]: source["sha256"] for source in profile["sources"]}
    if set(sources) != set(expected):
        raise ValueError("explicit source assignments must match every profile source id")
    inputs = {}
    for name, path in sources.items():
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        if digest != expected[name]:
            raise ValueError(f"source digest mismatch: {name}")
        inputs["source:" + name] = digest
    tools = Path(__file__).resolve().parent
    code = {"generate_vehicle_profile.py": Path(__file__).resolve(),
            **{path.relative_to(tools).as_posix(): path for path in (tools / "vehicle_profiles").glob("*.py")}}
    generator_hashes = {"revision": hashlib.sha256(revision.encode()).hexdigest()}
    for name, path in code.items():
        generator_hashes["code:" + name] = hashlib.sha256(path.read_bytes()).hexdigest()
    for name, path in generator_inputs.items():
        generator_hashes["extra:" + name] = hashlib.sha256(path.read_bytes()).hexdigest()
    inputs.update(generator_hashes)
    effective = "artifact-set-v1:" + hashlib.sha256(json_bytes(generator_hashes)).hexdigest()
    projected = project_runtime_profile(profile, policies=load_policies(policy_data),
                                        generator_revision=effective)
    # A5's normalized identity covers both records and the validation registry.
    # Raw JSON spelling/order is not a new artifact-generation domain.
    inputs["normalized-manifest-and-policies"] = json.loads(projected.capability_report)["artifact_identity"]
    return contents(projected, inputs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--policies", type=Path, required=True)
    parser.add_argument("--source", action="append", default=[], metavar="ID=PATH")
    parser.add_argument("--generator-input", action="append", default=[], metavar="NAME=PATH")
    parser.add_argument("--generator-revision", required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--print-directory", action="store_true")
    args = parser.parse_args()
    try:
        outputs = generate(args.manifest, args.policies, named_paths(args.source),
                           named_paths(args.generator_input), args.generator_revision)
        if args.check:
            if not check(args.output_dir, outputs):
                print("vehicle profile artifact set is missing, corrupt or stale", file=sys.stderr)
                return 1
            directory = args.output_dir / "sets" / generation(outputs)
        else:
            directory = publish(args.output_dir, outputs)
        if args.print_directory:
            print(directory.resolve())
        return 0
    except (OSError, ValueError, TypeError, OverflowError) as error:
        print(f"vehicle profile generation failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
