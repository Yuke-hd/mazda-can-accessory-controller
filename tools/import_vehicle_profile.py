#!/usr/bin/env python3
"""Import DBC sources into one complete offline normalized-profile artifact set."""

import argparse
import hashlib
import json
from pathlib import Path
import sys

from generate_vehicle_profile import load_policies
from vehicle_profiles.artifact_set import check, contents, generation, json_bytes, publish
from vehicle_profiles.dbc_import import import_profile, load_request
from vehicle_profiles.projections import project_profile
from vehicle_profiles.recognition import load_pack


class ImportFailed(Exception):
    """A fatal import; carries the source-report diagnostics."""

    def __init__(self, diagnostics):
        super().__init__("import failed")
        self.diagnostics = diagnostics


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def generate(request_path, pack_path, policies_path):
    request_data = request_path.read_bytes()
    pack_data = pack_path.read_bytes()
    policy_data = policies_path.read_bytes()
    request = load_request(request_data.decode("utf-8"))
    pack = load_pack(pack_data.decode("utf-8"))
    policies = load_policies(policy_data)
    tools = Path(__file__).resolve().parent
    code = {"import_vehicle_profile.py": Path(__file__).resolve(),
            "generate_vehicle_profile.py": tools / "generate_vehicle_profile.py",
            **{path.relative_to(tools).as_posix(): path
               for path in (tools / "vehicle_profiles").glob("*.py")}}
    generator = {"revision": sha256(request["generator_revision"].encode()),
                 "request": sha256(request_data), "pack": sha256(pack_data),
                 "policies": sha256(policy_data)}
    for name, path in sorted(code.items()):
        generator["code:" + name] = sha256(path.read_bytes())
    effective = "artifact-set-v1:" + sha256(json_bytes(generator))
    # Source bytes enter through each profile source digest and the report below.
    result = import_profile(dict(request, generator_revision=effective), pack=pack,
                            policies=policies, base_dir=request_path.resolve().parent)
    if result.profile is None:
        raise ImportFailed(result.report["diagnostics"])
    projected = project_profile(result.profile, policies=policies, generator_revision=effective)
    report = json_bytes(result.report)
    inputs = dict(generator)
    for source in result.report["sources"]:
        inputs["source:" + source["id"]] = source["sha256"]
    inputs["source-report"] = sha256(report)
    inputs["normalized-manifest-and-policies"] = json.loads(
        projected.capability_report)["artifact_identity"]
    outputs = contents(projected, inputs)
    outputs["source_report.json"] = report
    return outputs


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--request", type=Path, required=True)
    parser.add_argument("--pack", type=Path, required=True)
    parser.add_argument("--policies", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--print-directory", action="store_true")
    args = parser.parse_args()
    try:
        outputs = generate(args.request, args.pack, args.policies)
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
    except ImportFailed as failure:
        for item in failure.diagnostics:
            if item["severity"] == "error":
                print(f"vehicle profile import failed: {item['code']} "
                      f"{item['affected']}: {item['reason']}", file=sys.stderr)
        return 1
    except (OSError, ValueError, TypeError, OverflowError) as error:
        print(f"vehicle profile import failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
