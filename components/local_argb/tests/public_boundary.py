#!/usr/bin/env python3
"""Keep the ordinary local_argb include and target boundary generic."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[3])
    parser.add_argument("--core-root", type=Path)
    parser.add_argument("--compiler", default="c++")
    args = parser.parse_args()
    root = args.root.resolve()
    core_root = (args.core_root or root / "third_party/esp32-vehicle-can-core").resolve()
    component_root = root / "components/local_argb"
    public_root = component_root / "include/local_argb"
    compatibility_root = root / "components/local_argb_compat/include/local_argb"
    sink_component_root = root / "components/local_argb_sink_contract"
    sink_root = sink_component_root / "include/local_argb"
    cmake = root / "components/local_argb/CMakeLists.txt"
    sink_cmake = sink_component_root / "CMakeLists.txt"
    failures: list[str] = []

    ordinary_headers = sorted(public_root.glob("*.h"))
    ordinary_headers += sorted(public_root.glob("*.hpp"))
    for header in ordinary_headers:
        public_text = header.read_text(encoding="utf-8")
        for forbidden in (
            "mazda/",
            "SemanticHealth",
            "SemanticSnapshot",
            "VehicleState",
            "decoder",
        ):
            if forbidden in public_text:
                failures.append(
                    "ordinary local_argb header exposes compatibility detail: "
                    f"{header.name}: {forbidden}"
                )

    cmake_text = cmake.read_text(encoding="utf-8")
    if re.search(r"\b(?:REQUIRES|PRIV_REQUIRES)\b[^\n]*\bmazda\b", cmake_text):
        failures.append("IDF local_argb compiles or exports Mazda through the ordinary target")
    if re.search(r"target_link_libraries\(local_argb\s+(?:PUBLIC|PRIVATE)[^\)]*\bmazda\b", cmake_text):
        failures.append("host local_argb target compiles or exports Mazda")
    if (public_root / "legacy_compat.hpp").exists():
        failures.append("legacy compatibility header remains in ordinary local_argb include root")
    if not (compatibility_root / "legacy_compat.hpp").is_file():
        failures.append("firmware compatibility header is missing from its dedicated include root")
    if not (root / "components/local_argb_compat/src/legacy_compat.cpp").is_file():
        failures.append("firmware compatibility source is missing from its dedicated source root")
    if not (sink_root / "lighting_sink.hpp").is_file():
        failures.append("lighting sink contract is missing from its dedicated internal include root")
    if "add_library(local_argb_sink_contract INTERFACE)" not in sink_cmake.read_text(encoding="utf-8"):
        failures.append("authorized local_argb sink contract target is missing")

    compiler = shutil.which(args.compiler) or args.compiler
    with tempfile.TemporaryDirectory(prefix="local-argb-boundary-") as directory:
        directory_path = Path(directory)
        probe = directory_path / "probe.cpp"
        object_file = directory_path / "probe.o"
        probe.write_text(
            '#include "local_argb/local_argb.h"\n'
            "int main() { local_argb::Rgb color{}; return color.red; }\n",
            encoding="utf-8",
        )
        result = subprocess.run(
            [
                compiler,
                "-std=c++17",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(root / "components/local_argb/include"),
                "-I",
                str(core_root / "components/vehicle_core/include"),
                "-c",
                str(probe),
                "-o",
                str(object_file),
            ],
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            failures.append(f"generic local_argb header failed without Mazda: {result.stderr.strip()}")

        # The ordinary consumer must not be able to acquire the temporary
        # Mazda/decoder adapter by including a header outside its declared
        # public include root.
        legacy_probe = directory_path / "legacy_probe.cpp"
        legacy_probe.write_text(
            '#include "local_argb/legacy_compat.hpp"\n'
            "int main() { return 0; }\n",
            encoding="utf-8",
        )
        legacy_result = subprocess.run(
            [
                compiler,
                "-std=c++17",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(root / "components/local_argb/include"),
                "-I",
                str(core_root / "components/vehicle_core/include"),
                "-c",
                str(legacy_probe),
                "-o",
                str(directory_path / "legacy_probe.o"),
            ],
            capture_output=True,
            text=True,
        )
        if legacy_result.returncode == 0:
            failures.append("ordinary local_argb consumer can include firmware compatibility header")

        # The sink contract is intentionally consumable by an explicitly
        # authorized implementation target/path, while the ordinary public
        # root cannot see it and still does not receive renderer.hpp.
        authorized_probe = directory_path / "authorized_sink_probe.cpp"
        authorized_probe.write_text(
            '#include "local_argb/lighting_sink.hpp"\n'
            "int main() { local_argb::internal::LightingCommand command{}; return command.actionable; }\n",
            encoding="utf-8",
        )
        authorized_result = subprocess.run(
            [
                compiler,
                "-std=c++17",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(sink_root.parent),
                "-I",
                str(core_root / "components/vehicle_core/include"),
                "-c",
                str(authorized_probe),
                "-o",
                str(directory_path / "authorized_sink_probe.o"),
            ],
            capture_output=True,
            text=True,
        )
        if authorized_result.returncode != 0:
            failures.append(f"authorized sink consumer failed: {authorized_result.stderr.strip()}")

        ordinary_sink_result = subprocess.run(
            [
                compiler,
                "-std=c++17",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(root / "components/local_argb/include"),
                "-I",
                str(core_root / "components/vehicle_core/include"),
                "-c",
                str(authorized_probe),
                "-o",
                str(directory_path / "ordinary_sink_probe.o"),
            ],
            capture_output=True,
            text=True,
        )
        if ordinary_sink_result.returncode == 0:
            failures.append("ordinary local_argb consumer can include implementation-only sink contract")

    if failures:
        for failure in failures:
            print(f"ERROR: {failure}", file=sys.stderr)
        return 1
    print("local_argb public boundary validated")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
