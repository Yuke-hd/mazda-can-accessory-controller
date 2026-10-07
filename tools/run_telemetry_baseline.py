#!/usr/bin/env python3
"""Build and repeat the host synthetic telemetry baseline.

The executable itself owns deterministic accounting checks. This runner adds
release-build repeat variance and records the build command, raw output, and a
small min/median/max summary for engineering evidence.
"""

from __future__ import annotations

import argparse
import re
import statistics
import subprocess
import sys
from collections import defaultdict
from pathlib import Path
from typing import Dict, List, Tuple


HEADER = re.compile(r"^telemetry_baseline\s+(.*)$")
STAGES = re.compile(r"^\s+stage_ns\s+(.*)$")
STAGE_FIELDS = (
    "receive_wait", "enqueue_to_process", "dequeue_to_process", "decode", "diagnostics",
    "publication", "notification_evaluation", "notification_dispatch", "action_sink",
    "callback_delivery",
)


def key_values(text: str) -> Dict[str, str]:
    return {
        match.group(1): match.group(2)
        for match in re.finditer(r"([A-Za-z_]+)=([^\s]+)", text)
    }


def parse_reports(text: str) -> List[Dict[str, str]]:
    reports: List[Dict[str, str]] = []
    current = None
    for line in text.splitlines():
        header = HEADER.match(line)
        if header:
            current = key_values(header.group(1))
            reports.append(current)
            continue
        stages = STAGES.match(line)
        if stages and current is not None:
            current.update(key_values(stages.group(1)))
    return reports


def summarize(reports: List[Dict[str, str]]) -> str:
    if not reports:
        raise ValueError("no telemetry baseline reports")
    for report in reports:
        missing = {"scenario", "profiler", "wall_ns"}.difference(report)
        if missing:
            raise ValueError(f"incomplete telemetry baseline report: missing {sorted(missing)}")
        _measurement(report, "wall_ns")
        for name in STAGE_FIELDS:
            _measurement(report, name)
    groups: Dict[Tuple[str, str], List[Dict[str, str]]] = defaultdict(list)
    for report in reports:
        groups[(report["scenario"], report["profiler"])].append(
            report
        )

    lines = [f"baseline reports={len(reports)} groups={len(groups)}"]
    for (scenario, profiler), entries in sorted(groups.items()):
        lines.append(
            f"scenario={scenario} profiler={profiler} samples={len(entries)} "
            f"wall_ns[min/median/max]={_range(entries, 'wall_ns')} "
            f"process_cpu_ns[min/median/max]={_range(entries, 'process_cpu_ns')} "
            f"receive_wait_ns[min/median/max]={_range(entries, 'receive_wait')} "
            f"enqueue_to_process_ns[min/median/max]={_range(entries, 'enqueue_to_process')} "
            f"dequeue_to_process_ns[min/median/max]={_range(entries, 'dequeue_to_process')} "
            f"decode_ns[min/median/max]={_range(entries, 'decode')} "
            f"diagnostics_ns[min/median/max]={_range(entries, 'diagnostics')} "
            f"publication_ns[min/median/max]={_range(entries, 'publication')} "
            f"notification_eval_ns[min/median/max]={_range(entries, 'notification_evaluation')} "
            f"dispatch_ns[min/median/max]={_range(entries, 'notification_dispatch')} "
            f"action_sink_ns[min/median/max]={_range(entries, 'action_sink')} "
            f"callback_delivery_ns[min/median/max]={_range(entries, 'callback_delivery')}"
        )
    return "\n".join(lines)


def _range(entries: List[Dict[str, str]], name: str) -> str:
    values = [_measurement(entry, name) for entry in entries]
    return "/".join(
        str(value)
        for value in (min(values), statistics.median(values), max(values))
    )


def _measurement(report: Dict[str, str], name: str) -> int:
    value = report.get(name)
    if value is None:
        raise ValueError(f"incomplete telemetry baseline report: missing {name}")
    try:
        parsed = int(value)
    except ValueError as error:
        raise ValueError(f"non-numeric telemetry baseline measurement {name}={value!r}") from error
    if parsed < 0:
        raise ValueError(f"negative telemetry baseline measurement {name}={parsed}")
    return parsed


def run(command: List[str], cwd: Path) -> str:
    completed = subprocess.run(command, cwd=cwd, check=True, text=True, capture_output=True)
    return completed.stdout + completed.stderr


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-dir", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--build-dir", type=Path, default=Path("/tmp/mazda-gh210-baseline-release"))
    parser.add_argument("--core-source-dir", type=Path)
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--output", type=Path, help="Write raw reports and the summary to this file")
    args = parser.parse_args()
    if args.repeats < 1:
        parser.error("--repeats must be positive")

    source_dir = args.source_dir.resolve()
    build_dir = args.build_dir.resolve()
    revision = "working-tree"
    dirty = "unknown"
    try:
        revision = subprocess.run(
            ["git", "rev-parse", "--short", "HEAD"],
            cwd=source_dir,
            check=True,
            text=True,
            capture_output=True,
        ).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        pass
    try:
        dirty = "true" if subprocess.run(
            ["git", "status", "--porcelain", "--untracked-files=all"],
            cwd=source_dir,
            check=True,
            text=True,
            capture_output=True,
        ).stdout else "false"
    except (OSError, subprocess.CalledProcessError):
        pass

    configure = [
        "cmake",
        "-S",
        str(source_dir),
        "-B",
        str(build_dir),
        "-G",
        "Ninja",
        "-DCMAKE_BUILD_TYPE=Release",
        "-DBUILD_TESTING=ON",
        "-DMAZDA_BUILD_HOST_TESTS=ON",
        "-DMAZDA_ENABLE_TELEMETRY_PROFILING=ON",
        f"-DMAZDA_BASELINE_BUILD_REVISION={revision}",
        f"-DMAZDA_BASELINE_SOURCE_DIRTY={dirty}",
    ]
    if args.core_source_dir:
        configure.append(f"-DVEHICLE_CAN_CORE_SOURCE_DIR={args.core_source_dir.resolve()}")

    output: List[str] = []
    if not args.skip_build:
        output.append("$ " + " ".join(configure))
        output.append(run(configure, source_dir).rstrip())
        build = ["cmake", "--build", str(build_dir), "--target", "telemetry_baseline_tests", "--parallel"]
        output.append("$ " + " ".join(build))
        output.append(run(build, source_dir).rstrip())

    executable = build_dir / "tests" / "host" / "telemetry_baseline_tests"
    if not executable.exists():
        raise SystemExit(f"baseline executable not found: {executable}")
    reports: List[Dict[str, str]] = []
    for repeat in range(1, args.repeats + 1):
        command = [str(executable), "--test-case=*", "--no-breaks"]
        output.append(f"$ {' '.join(command)}  # release repeat {repeat}/{args.repeats}")
        raw = run(command, source_dir).rstrip()
        output.append(raw)
        repeat_reports = parse_reports(raw)
        if not repeat_reports:
            raise SystemExit(f"release repeat {repeat} produced no telemetry_baseline reports")
        reports.extend(repeat_reports)
    if not reports:
        raise SystemExit("baseline executable produced no telemetry_baseline reports")
    required = {"scenario", "profiler", "wall_ns"}
    for report in reports:
        missing = required.difference(report)
        if missing:
            raise SystemExit(f"incomplete telemetry baseline report: missing {sorted(missing)}")
    try:
        output.append(summarize(reports))
    except ValueError as error:
        raise SystemExit(str(error)) from error
    result = "\n\n".join(part for part in output if part) + "\n"
    if args.output:
        args.output.resolve().write_text(result, encoding="utf-8")
    print(result, end="")
    return 0


if __name__ == "__main__":
    sys.exit(main())
