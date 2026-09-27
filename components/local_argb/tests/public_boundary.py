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
    frame_root = sink_component_root / "frame_include/local_argb"
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
    if not (frame_root / "pixel_frame.hpp").is_file():
        failures.append("pixel frame contract is missing from its dedicated frame include root")
    if "add_library(local_argb_sink_contract INTERFACE)" not in sink_cmake.read_text(encoding="utf-8"):
        failures.append("authorized local_argb sink contract target is missing")

    # The ordinary target re-exports only the frame contract: on the host via
    # local_argb_pixel_frame, on IDF via exactly one sibling frame root.
    public_links = " ".join(
        re.findall(r"target_link_libraries\(\s*local_argb\s+PUBLIC\s+([^\)]*)\)", cmake_text)
    ).split()
    if "local_argb_pixel_frame" not in public_links:
        failures.append("host local_argb does not publicly link local_argb_pixel_frame")
    if "local_argb_sink_contract" in public_links:
        failures.append("host local_argb publicly exports the sink contract")
    idf_include_dirs = re.search(r"\bINCLUDE_DIRS\s+([^\n]*)", cmake_text)
    if idf_include_dirs is None or re.findall(r'"([^"]*)"', idf_include_dirs.group(1)) != [
        "include",
        "../local_argb_sink_contract/frame_include",
    ]:
        failures.append("IDF local_argb INCLUDE_DIRS must be only include and the frame root")
    if (component_root / "../local_argb_sink_contract/frame_include").resolve() != frame_root.parent:
        failures.append("IDF local_argb frame include path does not resolve to the frame root")

    # Frame consumers take the frame-only target, not the sink contract.
    gvret_text = (root / "lib/gvret/CMakeLists.txt").read_text(encoding="utf-8")
    output_links = re.search(r"target_link_libraries\(\s*gvret_replay_output\s+([^\)]*)\)", gvret_text)
    output_deps = output_links.group(1).split() if output_links else []
    if "local_argb_pixel_frame" not in output_deps:
        failures.append("gvret_replay_output does not link local_argb_pixel_frame")
    if {"local_argb", "local_argb_sink_contract"} & set(output_deps):
        failures.append("gvret_replay_output links more than the pixel frame contract")

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
                str(frame_root.parent),
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

        # A frame consumer implements PixelFrameSink from the frame root alone,
        # without the renderer, vehicle_core, ESP-IDF, or Mazda include roots.
        frame_probe = directory_path / "frame_probe.cpp"
        frame_probe.write_text(
            '#include "local_argb/pixel_frame.hpp"\n'
            "struct Probe final : local_argb::PixelFrameSink {\n"
            "  bool write(const local_argb::PixelFrame &frame) noexcept override {\n"
            "    return frame == local_argb::kBlackFrame && frame[0] == local_argb::kBlack;\n"
            "  }\n"
            "};\n"
            "int main() { Probe probe; return probe.write(local_argb::kBlackFrame) ? 0 : 1; }\n",
            encoding="utf-8",
        )
        frame_result = subprocess.run(
            [
                compiler,
                "-std=c++17",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(frame_root.parent),
                "-c",
                str(frame_probe),
                "-o",
                str(directory_path / "frame_probe.o"),
            ],
            capture_output=True,
            text=True,
        )
        if frame_result.returncode != 0:
            failures.append(f"standalone pixel frame consumer failed: {frame_result.stderr.strip()}")

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
                str(frame_root.parent),
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
                str(frame_root.parent),
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
