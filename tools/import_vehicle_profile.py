#!/usr/bin/env python3
"""Import a mapping.json and its pinned DBC sources into one complete artifact set."""

import argparse
import hashlib
import json
from pathlib import Path
import sys

from vehicle_profiles.artifact_set import check, contents, generation, json_bytes, publish
from vehicle_profiles.canonical import identify_profile
from vehicle_profiles.dbc_import import LIMITS, import_profile
from vehicle_profiles.dbc_mapping import MappingError, load_mapping
from vehicle_profiles.dbc_registry import derive_registry
from vehicle_profiles.projections import CATALOG_LIMITS, project_profile

TOOLS = Path(__file__).resolve().parent
ROOT = TOOLS.parent
REVISION = "vehicle-profile-import.v1"


class ImportFailed(Exception):
    """A fatal import; carries the source-report diagnostics."""

    def __init__(self, diagnostics):
        super().__init__("import failed")
        self.diagnostics = diagnostics


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def generator_files():
    """Every importer module, keyed by a stable relative name; all enter the identity."""
    files = {"import_vehicle_profile.py": Path(__file__).resolve()}
    for path in (TOOLS / "vehicle_profiles").glob("*.py"):
        files[path.relative_to(TOOLS).as_posix()] = path
    return files


def generator_hashes(mapping_data, files):
    hashes = {"revision": sha256(REVISION.encode()), "mapping": sha256(mapping_data)}
    for name, path in sorted(files.items()):
        hashes["code:" + name] = sha256(path.read_bytes())
    return hashes


def generator_identity(mapping_data, files):
    return "artifact-set-v1:" + sha256(json_bytes(generator_hashes(mapping_data, files)))


def registry_bytes(policies):
    """Serialize the derived registry in the explicit version-1 policy format."""
    return json_bytes({
        "version": 1,
        "profile_revisions": sorted([k, *v] for k, v in policies.profile_revisions.items()),
        "handlers": [],
        "evidence": sorted(list(row) for row in policies.evidence),
        "units": sorted(policies.units),
        "unit_conversions": sorted([*k, *v] for k, v in policies.unit_conversions.items()),
        "invalid_value_policies": sorted(policies.invalid_value_policies),
        "freshness_policies": sorted([*k, *v] for k, v in policies.freshness_policies.items())})


def generate(mapping_path):
    mapping_data = mapping_path.read_bytes()
    mapping = load_mapping(mapping_data.decode("utf-8"))
    files = generator_files()
    hashes = generator_hashes(mapping_data, files)
    effective = generator_identity(mapping_data, files)
    result = import_profile(mapping_path)
    if result.profile is None:
        raise ImportFailed(result.report["diagnostics"])
    field_units = {b["key"]: b["policy"]["conversion"]["source_unit"]
                   for b in result.profile["bindings"]}
    policies = derive_registry(mapping, field_units)
    limits = {**CATALOG_LIMITS, **LIMITS}  # import limits enter the artifact identity
    projected = project_profile(result.profile, policies=policies, generator_revision=effective,
                                validation_limits=limits)
    profile = identify_profile(result.profile, policies=policies, generator_revision=effective,
                               validation_limits=limits).profile
    extras = {"vehicle_profile.json": json_bytes(profile),
              "validation_policies.json": registry_bytes(policies),
              "source_report.json": json_bytes(result.report)}
    inputs = dict(hashes)
    for source in result.report["sources"]:
        inputs["source:" + source["id"]] = source["sha256"]
    inputs["source-report"] = sha256(extras["source_report.json"])
    inputs["vehicle-profile"] = sha256(extras["vehicle_profile.json"])
    inputs["validation-policies"] = sha256(extras["validation_policies.json"])
    inputs["normalized-manifest-and-policies"] = json.loads(
        projected.capability_report)["artifact_identity"]
    outputs = contents(projected, inputs)
    manifest = json.loads(outputs["artifact_set.json"])
    manifest["outputs"].update({name: sha256(data) for name, data in extras.items()})
    outputs["artifact_set.json"] = json_bytes(manifest)
    outputs.update(extras)
    return outputs


def check_output_dir(directory):
    """Generated files never land in tracked repository paths: inside the repo, only build/."""
    resolved = directory.resolve()
    if resolved.is_relative_to(ROOT) and not resolved.is_relative_to(ROOT / "build"):
        raise ValueError("output directory inside the repository must be under build/")


def report_failure(diagnostics):
    for item in diagnostics:
        if item["severity"] != "error":
            continue
        places = ", ".join(f"{l['source_id']}:{l['line']}:{l['column']}"
                           for l in item["locations"])
        print(f"vehicle profile import failed: {item['code']} {item['affected']}"
              f"{' at ' + places if places else ''}: {item['reason']}", file=sys.stderr)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mapping", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--check", action="store_true",
                        help="verify the current set without writing")
    parser.add_argument("--print-directory", action="store_true")
    args = parser.parse_args(argv)
    try:
        check_output_dir(args.output_dir)
        outputs = generate(args.mapping)
        if args.check:
            if not check(args.output_dir, outputs):
                print("vehicle profile artifact set is missing, corrupt or stale",
                      file=sys.stderr)
                return 1
            directory = args.output_dir / "sets" / generation(outputs)
        else:
            directory = publish(args.output_dir, outputs)
        if args.print_directory:
            print(directory.resolve())
        return 0
    except ImportFailed as failure:
        report_failure(failure.diagnostics)
        return 1
    except MappingError as error:
        print(f"vehicle profile import failed: mapping_error: {error}", file=sys.stderr)
        return 1
    except (OSError, ValueError, TypeError, OverflowError) as error:
        print(f"vehicle profile import failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
