#!/usr/bin/env python3
"""Run project-owned architecture contracts once per host suite.

This host-only gate owns repository-wide checks that cannot live in one
production target: the portable core must build without Mazda or RTOS inputs,
the generic vehicle-signals catalog must expose only value-only dependencies,
the vehicle binding must compile and exercise its project-owned listen-only
contract, and retired capture code must stay absent. The
existing source-safety validators are run here rather than duplicated in
CTest and firmware CI. Public-header positive/negative checks remain the
separate ``public_header_boundary`` and ``public_header_checker_regression``
gates from Stage 1.5.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from typing import Iterable, List, Optional, Sequence, Tuple


class ArchitectureFailure(RuntimeError):
    """A required architecture contract did not pass."""


def _run(command: Sequence[str], *, cwd: Path, timeout: int = 180) -> Tuple[int, str]:
    try:
        result = subprocess.run(
            list(command),
            cwd=str(cwd),
            check=False,
            capture_output=True,
            text=True,
            timeout=timeout,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        return 127, str(error)
    output = "\n".join(part for part in (result.stdout, result.stderr) if part).strip()
    return result.returncode, output


def _compiler_command(requested: Optional[str]) -> Tuple[str, ...]:
    command = tuple(shlex.split(requested or os.environ.get("CXX") or "c++"))
    if not command:
        raise ValueError("the C++ compiler command is empty")
    if shutil.which(command[0]) is None and not Path(command[0]).is_file():
        raise FileNotFoundError(f"C++ compiler not found: {command[0]}")
    return command


def _quoted(path: Path) -> str:
    return json.dumps(path.resolve().as_posix())


def _write_core_probe(probe_dir: Path, core_root: Path) -> Path:
    source = probe_dir / "core_only_consumer.cpp"
    source.write_text(
        '#include "vehicle_core/vehicle_core.hpp"\n'
        '#include "vehicle_core/notification.hpp"\n'
        '#include "vehicle_core/reading.hpp"\n'
        '#include "vehicle_core/telemetry_contracts.hpp"\n'
        "#include <type_traits>\n"
        "static_assert(std::is_trivially_copyable_v<vehicle_core::Reading<float>>);\n"
        "static_assert(std::is_trivially_copyable_v<vehicle_core::Notification<bool>>);\n"
        "int main() {\n"
        "  vehicle_core::RawCanFrame frame{};\n"
        "  return vehicle_core::library_is_available() && frame.is_valid() ? 0 : 1;\n"
        "}\n",
        encoding="utf-8",
    )
    (probe_dir / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.20)\n"
        "project(vehicle_core_only_consumer LANGUAGES CXX)\n"
        "set(CMAKE_CXX_STANDARD 17)\n"
        "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n"
        "set(CMAKE_CXX_EXTENSIONS OFF)\n"
        "set(CMAKE_EXPORT_COMPILE_COMMANDS ON)\n"
        "set(BUILD_TESTING OFF CACHE BOOL \"\" FORCE)\n"
        "add_subdirectory(" + _quoted(core_root / "components/vehicle_core") + " vehicle_core)\n"
        "add_executable(core_only_consumer " + _quoted(source) + ")\n"
        "target_link_libraries(core_only_consumer PRIVATE vehicle_core)\n"
        "target_compile_features(core_only_consumer PRIVATE cxx_std_17)\n",
        encoding="utf-8",
    )
    return source


def _compile_database_source(entry: object) -> Optional[Path]:
    if not isinstance(entry, dict):
        return None
    value = entry.get("file")
    if not isinstance(value, str) or not value:
        return None
    path = Path(value)
    directory = entry.get("directory")
    if not path.is_absolute() and isinstance(directory, str) and directory:
        path = Path(directory) / path
    return path.resolve()


def _command_tokens(entry: object) -> List[str]:
    if not isinstance(entry, dict):
        return []
    arguments = entry.get("arguments")
    if isinstance(arguments, list) and all(isinstance(value, str) for value in arguments):
        return list(arguments)
    command = entry.get("command")
    return shlex.split(command) if isinstance(command, str) else []


def _dependency_paths(depfile: Path, cwd: Path) -> Tuple[Path, ...]:
    try:
        flattened = depfile.read_text(encoding="utf-8").replace("\\\n", " ")
    except OSError:
        return ()
    separator = flattened.find(":")
    if separator < 0:
        return ()
    try:
        tokens = shlex.split(flattened[separator + 1 :], posix=True)
    except ValueError:
        return ()
    paths: List[Path] = []
    for token in tokens:
        path = Path(token)
        if not path.is_absolute():
            path = cwd / path
        resolved = path.resolve()
        if resolved not in paths:
            paths.append(resolved)
    return tuple(paths)


def _core_dependency_violations(
    compile_database: Path,
    consumer_source: Path,
    root: Path,
    work_dir: Path,
    core_root: Path,
) -> List[str]:
    try:
        entries = json.loads(compile_database.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        return [f"core-only compile database is unreadable: {error}"]
    root = root.resolve()
    consumer_source = consumer_source.resolve()
    core_root = core_root.resolve()
    target_source_root = (core_root / "components/vehicle_core/src").resolve()
    matching: List[Tuple[object, Path]] = []
    for entry in entries:
        source = _compile_database_source(entry)
        if source is None:
            continue
        is_consumer = source == consumer_source
        try:
            is_target_source = source.relative_to(target_source_root) is not None
        except ValueError:
            is_target_source = False
        if is_consumer or is_target_source:
            matching.append((entry, source))

    failures: List[str] = []
    if not any(source == consumer_source for _, source in matching):
        failures.append("core-only consumer compile command is missing")
    if not any(source != consumer_source for _, source in matching):
        failures.append("vehicle_core target compile command is missing")
    if failures:
        return failures

    violations: List[str] = []
    forbidden_parts = (
        "lib/mazda",
        "components/",
        "freertos",
        "esp-idf",
        "esp/",
        "driver/twai",
        "sdkconfig",
    )
    for index, (entry, source) in enumerate(matching):
        directory = entry.get("directory") if isinstance(entry, dict) else None
        cwd = Path(directory).resolve() if isinstance(directory, str) and directory else root
        tokens = _command_tokens(entry)
        if source == consumer_source:
            label = "core-only consumer"
        else:
            try:
                source_label = source.relative_to(root).as_posix()
            except ValueError:
                source_label = source.as_posix()
            label = f"vehicle_core target ({source_label})"
        def is_core_path(value: str) -> bool:
            candidate = value
            for prefix in ("-I", "/I", "-isystem", "-iquote", "-idirafter"):
                if candidate.startswith(prefix) and len(candidate) > len(prefix):
                    candidate = candidate[len(prefix) :]
                    break
            try:
                Path(candidate).resolve().relative_to(core_root)
            except (ValueError, OSError):
                return False
            return True

        for token in tokens:
            if is_core_path(token):
                continue
            normalized = token.replace("\\", "/").lower()
            if any(part in normalized for part in forbidden_parts):
                violations.append(f"forbidden {label} compile dependency: {token}")
        command_text = " ".join(tokens).replace("\\", "/").lower()
        if "/lib/mazda/" in command_text or "lib/mazda/" in command_text:
            violations.append(f"{label} compile command mentions Mazda")
        depfile = work_dir / f"core_dependency_{index}.d"
        dependency_probe = _run(
            [*tokens, "-MMD", "-MF", str(depfile), "-MT", str(source)],
            cwd=cwd,
            timeout=120,
        )
        if dependency_probe[0] != 0:
            violations.append(
                f"{label} dependency probe failed\n" + dependency_probe[1][-3000:]
            )
            continue
        dependencies = _dependency_paths(depfile, cwd)
        if not dependencies:
            violations.append(f"{label} dependency probe produced no dependency data")
            continue
        for dependency in dependencies:
            try:
                dependency.resolve().relative_to(core_root)
            except (ValueError, OSError):
                pass
            else:
                continue
            normalized = dependency.as_posix().lower()
            if any(part in normalized for part in forbidden_parts):
                violations.append(f"forbidden {label} dependency: {dependency}")
    return list(dict.fromkeys(violations))


def _check_core_only(
    root: Path,
    cmake: str,
    compiler: Sequence[str],
    work_dir: Path,
    core_root: Optional[Path] = None,
) -> None:
    core_root = (core_root or root / "third_party/esp32-vehicle-can-core").resolve()
    if not core_root.is_dir():
        raise ArchitectureFailure(f"vehicle CAN core source is missing: {core_root}")
    probe_dir = work_dir / "core_only_probe"
    probe_dir.mkdir()
    source = _write_core_probe(probe_dir, core_root)
    configure = [cmake, "-S", str(probe_dir), "-B", str(probe_dir / "build"), "-G", "Ninja"]
    if len(compiler) == 1:
        configure.append(f"-DCMAKE_CXX_COMPILER={compiler[0]}")
    result = _run(configure, cwd=root)
    if result[0] != 0:
        raise ArchitectureFailure("vehicle_core-only configure failed\n" + result[1][-3000:])
    build_dir = probe_dir / "build"
    result = _run([cmake, "--build", str(build_dir), "--target", "core_only_consumer"], cwd=root)
    if result[0] != 0:
        raise ArchitectureFailure("vehicle_core-only build failed\n" + result[1][-3000:])
    executable = build_dir / "core_only_consumer"
    result = _run([str(executable)], cwd=root)
    if result[0] != 0:
        raise ArchitectureFailure("vehicle_core-only consumer execution failed\n" + result[1][-3000:])
    violations = _core_dependency_violations(
        build_dir / "compile_commands.json", source, root, work_dir, core_root
    )
    if violations:
        raise ArchitectureFailure("\n".join(violations))
    print("OK   vehicle_core builds and links without Mazda/RTOS dependencies")


def _write_vehicle_signals_probe(
    probe_dir: Path,
    root: Path,
    core_root: Path,
) -> Path:
    """Create an isolated consumer for the generic vehicle-signals headers."""
    signals_include = root / "lib/vehicle_signals/include"
    headers = sorted(
        path
        for path in signals_include.rglob("*")
        if path.is_file() and path.suffix in {".h", ".hpp"}
    )
    required = (
        signals_include / "vehicle_signals/types.hpp",
        signals_include / "vehicle_signals/catalog.hpp",
    )
    missing = [path.relative_to(root).as_posix() for path in required if not path.is_file()]
    if missing:
        raise ArchitectureFailure(
            "vehicle_signals public headers are missing: " + ", ".join(missing)
        )
    if not headers:
        raise ArchitectureFailure("vehicle_signals has no public headers")

    source = probe_dir / "vehicle_signals_public_headers.cpp"
    includes = []
    for header in headers:
        includes.append(f'#include "{header.relative_to(signals_include).as_posix()}"')
    source.write_text("\n".join((*includes, "int main() { return 0; }", "")), encoding="utf-8")

    cmake = probe_dir / "CMakeLists.txt"
    cmake.write_text(
        "cmake_minimum_required(VERSION 3.20)\n"
        "project(vehicle_signals_architecture_probe LANGUAGES CXX)\n"
        "set(CMAKE_CXX_STANDARD 17)\n"
        "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n"
        "set(CMAKE_CXX_EXTENSIONS OFF)\n"
        "set(CMAKE_EXPORT_COMPILE_COMMANDS ON)\n"
        "set(BUILD_TESTING OFF CACHE BOOL \"\" FORCE)\n"
        "set(VEHICLE_CAN_CORE_BUILD_HOST_TESTS OFF CACHE BOOL \"\" FORCE)\n"
        f"add_subdirectory({_quoted(core_root / 'components/vehicle_core')} vehicle_core)\n"
        # These sentinel targets let the probe report an explicit boundary
        # violation instead of failing first on an unknown target name.
        "foreach(_forbidden_target mazda mazda_contracts mazda_telemetry_contracts "
        "vehicle_lighting_policy vehicle_telemetry can_bus)\n"
        "  if(NOT TARGET ${_forbidden_target})\n"
        "    add_library(${_forbidden_target} INTERFACE IMPORTED GLOBAL)\n"
        "  endif()\n"
        "endforeach()\n"
        f"add_subdirectory({_quoted(root / 'lib/vehicle_signals')} vehicle_signals)\n"
        "if(NOT TARGET vehicle_signals)\n"
        "  message(FATAL_ERROR \"vehicle_signals directory did not define vehicle_signals\")\n"
        "endif()\n"
        "function(_check_vehicle_signals_interface _target)\n"
        "  get_property(_visited GLOBAL PROPERTY _vehicle_signals_checked_targets)\n"
        "  if(_target IN_LIST _visited)\n"
        "    return()\n"
        "  endif()\n"
        "  set_property(GLOBAL APPEND PROPERTY _vehicle_signals_checked_targets ${_target})\n"
        "  get_target_property(_includes ${_target} INTERFACE_INCLUDE_DIRECTORIES)\n"
        "  get_target_property(_links ${_target} INTERFACE_LINK_LIBRARIES)\n"
        r'  string(REPLACE "\\" "/" _includes_normalized "${_includes}")' + "\n"
        '  string(TOLOWER "${_includes_normalized}" _includes_lower)\n'
        '  if(_includes_lower MATCHES "/lib/mazda/|/components/mazda_telemetry/|/components/vehicle_telemetry/|/freertos/|/esp-idf/|/driver/twai")\n'
        '    message(FATAL_ERROR "${_target} exports a forbidden include directory: ${_includes}")\n'
        "  endif()\n"
        "  foreach(_link IN LISTS _links)\n"
        '    string(TOLOWER "${_link}" _link_lower)\n'
        '    if(_link_lower MATCHES "(^|[/:])mazda([_:.]|$)|vehicle_telemetry|vehicle_lighting_policy|can_bus|freertos|esp-idf")\n'
        '      message(FATAL_ERROR "${_target} links forbidden target: ${_link}")\n'
        "    endif()\n"
        "    if(TARGET ${_link})\n"
        "      _check_vehicle_signals_interface(${_link})\n"
        "    endif()\n"
        "  endforeach()\n"
        "endfunction()\n"
        "_check_vehicle_signals_interface(vehicle_signals)\n"
        f"add_executable(vehicle_signals_public_headers {_quoted(source)})\n"
        "target_link_libraries(vehicle_signals_public_headers PRIVATE vehicle_signals)\n"
        "target_compile_features(vehicle_signals_public_headers PRIVATE cxx_std_17)\n",
        encoding="utf-8",
    )
    return source


def _vehicle_signals_dependency_violations(
    dependencies: Sequence[Path],
    root: Path,
    signals_root: Path,
    core_root: Path,
    consumer_source: Path,
) -> List[str]:
    root = root.resolve()
    signals_root = signals_root.resolve()
    core_include = (core_root / "components/vehicle_core/include").resolve()
    consumer_source = consumer_source.resolve()
    allowed_core_headers = {
        "vehicle_core/telemetry_contracts.hpp",
        "vehicle_core/reading.hpp",
        "vehicle_core/notification.hpp",
    }
    violations: List[str] = []
    for dependency in dependencies:
        dependency = dependency.resolve()
        if dependency == consumer_source:
            continue
        try:
            dependency.relative_to(signals_root)
            continue
        except ValueError:
            pass
        try:
            core_relative = dependency.relative_to(core_include).as_posix()
        except ValueError:
            core_relative = None
        if core_relative is not None:
            if core_relative not in allowed_core_headers:
                violations.append(
                    f"non-value vehicle_core dependency: {core_relative}"
                )
            continue
        try:
            project_relative = dependency.relative_to(root).as_posix()
        except ValueError:
            # Compiler and C++ standard-library headers live outside the
            # checkout and are the only other accepted dependency category.
            continue
        violations.append(f"unexpected project dependency: {project_relative}")
    return list(dict.fromkeys(violations))


def _check_vehicle_signals(
    root: Path,
    cmake: str,
    compiler: Sequence[str],
    work_dir: Path,
    core_root: Path,
) -> None:
    target_cmake = root / "lib/vehicle_signals/CMakeLists.txt"
    if not target_cmake.is_file():
        print("SKIP vehicle_signals architecture check (target files are not present)")
        return

    probe_dir = work_dir / "vehicle_signals_probe"
    probe_dir.mkdir()
    try:
        source = _write_vehicle_signals_probe(probe_dir, root, core_root.resolve())
    except ArchitectureFailure:
        raise
    configure = [
        cmake,
        "-S",
        str(probe_dir),
        "-B",
        str(probe_dir / "build"),
        "-G",
        "Ninja",
    ]
    if len(compiler) == 1:
        configure.append(f"-DCMAKE_CXX_COMPILER={compiler[0]}")
    result = _run(configure, cwd=root)
    if result[0] != 0:
        raise ArchitectureFailure(
            "vehicle_signals isolated configure failed\n" + result[1][-3000:]
        )
    build_dir = probe_dir / "build"
    result = _run(
        [cmake, "--build", str(build_dir), "--target", "vehicle_signals_public_headers"],
        cwd=root,
    )
    if result[0] != 0:
        raise ArchitectureFailure(
            "vehicle_signals public-header consumer build failed\n" + result[1][-3000:]
        )

    try:
        entries = json.loads((build_dir / "compile_commands.json").read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ArchitectureFailure(f"vehicle_signals compile database is unreadable: {error}")
    matching = [
        entry
        for entry in entries
        if _compile_database_source(entry) == source.resolve()
    ]
    if len(matching) != 1:
        raise ArchitectureFailure(
            "vehicle_signals public-header compile command is missing or ambiguous"
        )
    tokens = _command_tokens(matching[0])
    command_text = " ".join(tokens).replace("\\", "/").lower()
    forbidden_markers = (
        "/lib/mazda/",
        "/components/mazda_telemetry/",
        "/components/vehicle_telemetry/",
        "/freertos/",
        "/esp-idf/",
        "/driver/twai",
    )
    command_violations = [marker for marker in forbidden_markers if marker in command_text]
    if command_violations:
        raise ArchitectureFailure(
            "vehicle_signals consumer compile command contains forbidden dependencies: "
            + ", ".join(command_violations)
        )

    depfile = work_dir / "vehicle_signals_public_headers.d"
    directory = matching[0].get("directory")
    cwd = Path(directory).resolve() if isinstance(directory, str) and directory else root
    dependency_probe = _run(
        [*tokens, "-MD", "-MF", str(depfile), "-MT", str(source)],
        cwd=cwd,
        timeout=120,
    )
    if dependency_probe[0] != 0:
        raise ArchitectureFailure(
            "vehicle_signals dependency probe failed\n" + dependency_probe[1][-3000:]
        )
    dependencies = _dependency_paths(depfile, cwd)
    if not dependencies:
        raise ArchitectureFailure("vehicle_signals dependency probe produced no dependency data")
    violations = _vehicle_signals_dependency_violations(
        dependencies,
        root,
        root / "lib/vehicle_signals",
        core_root,
        source,
    )
    if violations:
        raise ArchitectureFailure("\n".join(violations))
    print("OK   vehicle_signals exposes only standard and value-only core dependencies")


def _check_dependency_layout(root: Path) -> None:
    required = (
        root / "components/mazda_telemetry",
        root / "components/vehicle_can_rx",
        root / "CMakeLists.txt",
    )
    for path in required:
        if not path.exists():
            raise ArchitectureFailure(f"required controller path is missing: {path}")
    forbidden = (
        root / "lib/vehicle_core",
        root / "components/can_bus",
        root / "components/vehicle_telemetry",
        root / "components/vehicle_telemetry_consumer",
    )
    for path in forbidden:
        if path.exists():
            raise ArchitectureFailure(f"generic component is duplicated in controller: {path}")
    cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    for needle in ("VEHICLE_CAN_CORE_REPOSITORY", "VEHICLE_CAN_CORE_TAG", "FetchContent"):
        if needle not in cmake:
            raise ArchitectureFailure(f"pinned generic dependency contract is missing: {needle}")
    tag_match = re.search(
        r"set\s*\(\s*VEHICLE_CAN_CORE_TAG\s+\"([^\"]+)\"", cmake
    )
    if tag_match is None:
        raise ArchitectureFailure("VEHICLE_CAN_CORE_TAG must name a release tag")
    release_tag = tag_match.group(1)
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", release_tag) or release_tag != "0.1.0":
        raise ArchitectureFailure("VEHICLE_CAN_CORE_TAG must be the 0.1.0 release tag")
    manifest = root / "firmware/weact-can485-v1.1/main/idf_component.yml"
    if not manifest.exists():
        raise ArchitectureFailure(f"firmware dependency manifest is missing: {manifest}")
    manifest_text = manifest.read_text(encoding="utf-8")
    component_refs: List[str] = []
    for component in ("vehicle_core", "can_bus", "vehicle_telemetry"):
        component_match = re.search(
            rf"^  {component}:\s*\n((?:^    .*\n)*)",
            manifest_text,
            re.MULTILINE,
        )
        if component_match is None:
            continue
        version_match = re.search(
            r'^    version:\s+"([^\"]+)"\s*$',
            component_match.group(1),
            re.MULTILINE,
        )
        if version_match is not None:
            component_refs.append(version_match.group(1))
    if len(component_refs) != 3 or set(component_refs) != {release_tag}:
        raise ArchitectureFailure(
            "CMake and all three ESP-IDF generic component refs must use the same 0.1.0 release tag"
        )
    print("OK   generic vehicle-core components are external and pinned")


def _check_adapter(
    root: Path,
    cmake: str,
    compiler: Sequence[str],
    source_dir: Path,
    work_dir: Path,
    core_root: Optional[Path] = None,
) -> None:
    label = source_dir.parent.name
    build_dir = work_dir / f"{label}_adapter_tests"
    configure = [cmake, "-S", str(source_dir), "-B", str(build_dir), "-G", "Ninja"]
    if len(compiler) == 1:
        configure.append(f"-DCMAKE_CXX_COMPILER={compiler[0]}")
    if core_root is not None:
        configure.append(f"-DVEHICLE_CAN_CORE_SOURCE_DIR={core_root.resolve()}")
    result = _run(configure, cwd=root)
    if result[0] != 0:
        raise ArchitectureFailure(f"{label} adapter configure failed\n" + result[1][-3000:])
    result = _run([cmake, "--build", str(build_dir), "--parallel"], cwd=root)
    if result[0] != 0:
        raise ArchitectureFailure(f"{label} adapter build failed\n" + result[1][-3000:])
    result = _run(["ctest", "--test-dir", str(build_dir), "--output-on-failure"], cwd=root)
    if result[0] != 0:
        raise ArchitectureFailure(f"{label} adapter test failed\n" + result[1][-3000:])
    print(f"OK   {label} project-owned mode adapter compiled and passed")


def _run_validator(root: Path, label: str, command: Sequence[str]) -> None:
    result = _run(command, cwd=root)
    if result[0] != 0:
        raise ArchitectureFailure(f"{label} failed\n" + result[1][-3000:])
    print(f"OK   {label}")


def _check_capture_removal(root: Path) -> None:
    patterns = (
        "raw_capture",
        "validate_capture_format.py",
        "capture_reader",
        "capture_writer",
        "replay.hpp",
    )
    roots: Iterable[Path] = (
        root / "CMakeLists.txt",
        root / ".github/workflows/ci.yml",
        root / "components",
        root / "firmware",
        root / "lib",
        root / "tests",
        root / "tools",
    )
    violations: List[str] = []
    for entry in roots:
        paths = [entry] if entry.is_file() else entry.rglob("*")
        for path in paths:
            if not path.is_file() or path.resolve() == Path(__file__).resolve():
                continue
            if path.name != "CMakeLists.txt" and path.suffix not in {
                ".c",
                ".cc",
                ".cpp",
                ".h",
                ".hpp",
                ".py",
                ".cmake",
                ".yml",
                ".yaml",
            }:
                continue
            text = path.read_text(encoding="utf-8")
            for pattern in patterns:
                if pattern in text:
                    violations.append(f"{path.relative_to(root)} contains retired capture marker {pattern}")
    if violations:
        raise ArchitectureFailure("\n".join(violations))
    print("OK   retired raw_capture product has no active code/build dependency")


def _check_validator_ownership(root: Path) -> None:
    workflow = (root / ".github/workflows/ci.yml").read_text(encoding="utf-8")
    host_cmake = (root / "tests/host/CMakeLists.txt").read_text(encoding="utf-8")
    scripts = (
        "validate_can_receive_only.py",
        "validate_weact_vehicle_artifacts.py",
        "validate_local_argb_boundary.py",
    )
    failures: List[str] = []
    for script in scripts:
        if script in workflow:
            failures.append(f"{script} is directly invoked by CI; architecture_contracts must own it")
        if script in host_cmake:
            failures.append(f"{script} is separately registered in host CTest")
    if failures:
        raise ArchitectureFailure("\n".join(failures))
    print("OK   architecture validators have one consolidated CTest owner")


def check(
    root: Path,
    cmake: str,
    compiler: Sequence[str],
    core_root: Optional[Path] = None,
) -> int:
    root = root.resolve()
    core_root = (core_root or root / "third_party/esp32-vehicle-can-core").resolve()
    with tempfile.TemporaryDirectory(prefix="mazda-architecture-") as directory:
        work_dir = Path(directory)
        try:
            _check_dependency_layout(root)
            _check_core_only(root, cmake, compiler, work_dir, core_root)
            _check_vehicle_signals(root, cmake, compiler, work_dir, core_root)
            _check_adapter(
                root,
                cmake,
                compiler,
                root / "components/vehicle_can_rx/tests",
                work_dir,
                core_root,
            )
            _run_validator(
                root,
                "CAN receive-only safety validator",
                (
                    sys.executable,
                    str(root / "tools/validate_can_receive_only.py"),
                    "--public-header",
                    str(core_root / "components/can_bus/include/can_bus/can_bus.h"),
                    "--implementation",
                    str(core_root / "components/can_bus/src/can_bus.cpp"),
                    "--vehicle-binding",
                    str(root / "components/vehicle_can_rx/src/driver_binding.cpp"),
                ),
            )
            _run_validator(
                root,
                "vehicle artifact validator",
                (sys.executable, str(root / "tools/validate_weact_vehicle_artifacts.py"),
                 "--root", str(root), "--core-root",
                 str(core_root)),
            )
            _run_validator(
                root,
                "local ARGB semantic boundary validator",
                (sys.executable, str(root / "tools/validate_local_argb_boundary.py"),
                 "--root", str(root), "--core-root",
                 str(core_root)),
            )
            _check_capture_removal(root)
            _check_validator_ownership(root)
        except (ArchitectureFailure, OSError, ValueError) as error:
            print(f"ERROR: {error}", file=sys.stderr)
            return 1
    print("Architecture contract check passed.")
    return 0


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--core-root", type=Path)
    parser.add_argument("--compiler", help="C++ compiler command (defaults to $CXX or c++)")
    parser.add_argument("--cmake", default="cmake", help="CMake executable (default: cmake)")
    args = parser.parse_args(argv)
    if shutil.which(args.cmake) is None and not Path(args.cmake).is_file():
        parser.error(f"CMake not found: {args.cmake}")
    try:
        compiler = _compiler_command(args.compiler)
    except (FileNotFoundError, ValueError) as error:
        parser.error(str(error))
    return check(args.root, args.cmake, compiler, args.core_root)


if __name__ == "__main__":
    raise SystemExit(main())
