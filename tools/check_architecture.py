#!/usr/bin/env python3
"""Run project-owned architecture contracts once per host suite.

This host-only gate owns repository-wide checks that cannot live in one
production target: the portable core must build without Mazda or RTOS inputs,
the portable ``vehicle_signals`` contracts must build against that core alone
and reach only its value-only telemetry contracts, the host generic consumer
must include only the public provider and ``vehicle_signals`` headers, the
vehicle binding must compile and exercise its project-owned listen-only
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


VEHICLE_SIGNALS_HEADERS = (
    "vehicle_signals/signal_contracts.hpp",
    "vehicle_signals/signal_catalog.hpp",
)
# The only companion-core header the portable signal contracts may reach.
VEHICLE_SIGNALS_CORE_ALLOWED = ("vehicle_core/telemetry_contracts.hpp",)
# Path segments that identify a make-specific, RTOS, SDK, or CAN-driver input.
_FORBIDDEN_SIGNALS_SEGMENTS = frozenset(
    ("mazda", "mazda_telemetry", "freertos", "esp-idf", "esp_idf", "esp", "sdkconfig")
)
_FORBIDDEN_SIGNALS_DEFINITION = re.compile(
    r"mazda|freertos|esp_platform|esp_idf|sdkconfig|twai", re.IGNORECASE
)
_CPP_SUFFIXES = frozenset((".c", ".cc", ".cpp", ".h", ".hpp", ".inl", ".ipp"))


def _write_vehicle_signals_probe(probe_dir: Path, root: Path, core_root: Path) -> Path:
    source = probe_dir / "vehicle_signals_consumer.cpp"
    includes = "".join(f'#include "{header}"\n' for header in VEHICLE_SIGNALS_HEADERS)
    source.write_text(
        includes + "#include <string_view>\n"
        "namespace {\n"
        "using vehicle_signals::SignalCapability;\n"
        "using vehicle_signals::SignalId;\n"
        "using vehicle_signals::SignalMetadata;\n"
        "constexpr SignalMetadata kProbeCatalog[] = {\n"
        "    {SignalId{1}, \"probe.number\", vehicle_signals::SignalType::Number,\n"
        "     vehicle_signals::SignalUnit::None, vehicle_signals::ValidationStatus::Reference,\n"
        "     SignalCapability::Read},\n"
        "    {SignalId{2}, \"probe.flag\", vehicle_signals::SignalType::Boolean,\n"
        "     vehicle_signals::SignalUnit::None, vehicle_signals::ValidationStatus::Reference,\n"
        "     SignalCapability::Read},\n"
        "};\n"
        "constexpr vehicle_signals::SignalCatalogView kProbeView{kProbeCatalog};\n"
        "static_assert(kProbeView.well_formed());\n"
        "} // namespace\n"
        "int main() {\n"
        "  const SignalMetadata *by_key = kProbeView.find(std::string_view{\"probe.flag\"});\n"
        "  const SignalMetadata *by_id = kProbeView.find(SignalId{2});\n"
        "  const vehicle_signals::SignalReading reading{};\n"
        "  return by_key != nullptr && by_key == by_id && !reading.value.has_value() ? 0 : 1;\n"
        "}\n",
        encoding="utf-8",
    )
    (probe_dir / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.20)\n"
        "project(vehicle_signals_only_consumer LANGUAGES CXX)\n"
        "set(CMAKE_CXX_STANDARD 17)\n"
        "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n"
        "set(CMAKE_CXX_EXTENSIONS OFF)\n"
        "set(CMAKE_EXPORT_COMPILE_COMMANDS ON)\n"
        "set(BUILD_TESTING OFF CACHE BOOL \"\" FORCE)\n"
        "add_subdirectory(" + _quoted(core_root / "components/vehicle_core") + " vehicle_core)\n"
        "add_subdirectory(" + _quoted(root / "lib/vehicle_signals") + " vehicle_signals)\n"
        "add_executable(vehicle_signals_consumer " + _quoted(source) + ")\n"
        "target_link_libraries(vehicle_signals_consumer PRIVATE vehicle_signals)\n"
        "target_compile_features(vehicle_signals_consumer PRIVATE cxx_std_17)\n"
        # Record the evaluated target interface so link targets and exported
        # include directories are checked as CMake resolved them.
        "file(GENERATE OUTPUT \"${CMAKE_BINARY_DIR}/vehicle_signals_interface.txt\" CONTENT\n"
        "  \"include=$<TARGET_PROPERTY:vehicle_signals,INTERFACE_INCLUDE_DIRECTORIES>\\n"
        "link=$<TARGET_PROPERTY:vehicle_signals,INTERFACE_LINK_LIBRARIES>\\n"
        "consumer_link=$<TARGET_PROPERTY:vehicle_signals_consumer,LINK_LIBRARIES>\\n\")\n",
        encoding="utf-8",
    )
    return source


def _include_directories(tokens: Sequence[str], cwd: Path) -> List[Path]:
    directories: List[Path] = []
    flags = ("-I", "/I", "-isystem", "-iquote", "-idirafter")
    index = 0
    while index < len(tokens):
        token = tokens[index]
        value: Optional[str] = None
        if token in flags and index + 1 < len(tokens):
            value = tokens[index + 1]
            index += 1
        else:
            for flag in flags:
                if token.startswith(flag) and len(token) > len(flag):
                    value = token[len(flag) :]
                    break
        if value is not None:
            path = Path(value)
            directories.append((path if path.is_absolute() else cwd / path).resolve())
        index += 1
    return directories


def _is_within(path: Path, parent: Path) -> bool:
    try:
        path.resolve().relative_to(parent.resolve())
    except (ValueError, OSError):
        return False
    return True


def _has_forbidden_signals_segment(path: Path) -> bool:
    for segment in path.as_posix().lower().split("/"):
        if segment in _FORBIDDEN_SIGNALS_SEGMENTS or "twai" in segment:
            return True
    return False


def _vehicle_signals_interface_violations(
    interface_file: Path, root: Path, core_root: Path
) -> List[str]:
    try:
        lines = interface_file.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        return [f"vehicle_signals interface properties are unreadable: {error}"]
    # CMake evaluates INTERFACE_INCLUDE_DIRECTORIES transitively, so the core's
    # include root legitimately appears through the vehicle_core link.
    allowed_includes = (
        (root / "lib/vehicle_signals/include").resolve(),
        (core_root / "components/vehicle_core/include").resolve(),
    )
    allowed_links = {"link": {"vehicle_core"}, "consumer_link": {"vehicle_signals"}}
    labels = {"link": "vehicle_signals target", "consumer_link": "vehicle_signals consumer"}
    violations: List[str] = []
    for line in lines:
        key, _, value = line.partition("=")
        items = [part for part in value.split(";") if part]
        if key == "include":
            for item in items:
                include_dir = Path(item).resolve()
                if include_dir not in allowed_includes:
                    violations.append(
                        f"forbidden vehicle_signals target include directory: {include_dir}"
                    )
        elif key in allowed_links:
            for item in items:
                if item not in allowed_links[key]:
                    violations.append(f"forbidden {labels[key]} link target: {item}")
    return violations


def _strip_cpp_comments(text: str) -> Tuple[str, List[str]]:
    """Return code text without comments, plus its string literal contents."""

    code: List[str] = []
    literals: List[str] = []
    index = 0
    while index < len(text):
        char = text[index]
        pair = text[index : index + 2]
        if pair == "//":
            newline = text.find("\n", index)
            index = len(text) if newline < 0 else newline
            continue
        if pair == "/*":
            end = text.find("*/", index + 2)
            index = len(text) if end < 0 else end + 2
            code.append(" ")
            continue
        if char in "\"'":
            end = index + 1
            while end < len(text) and text[end] != char:
                end += 2 if text[end] == "\\" else 1
            if char == '"':
                literals.append(text[index + 1 : end])
            code.append(text[index : end + 1])
            index = end + 1
            continue
        code.append(char)
        index += 1
    return "".join(code), literals


def _mazda_type_names(root: Path) -> Tuple[str, ...]:
    types_header = root / "lib/mazda/include/mazda/types.hpp"
    if not types_header.is_file():
        return ()
    code, _ = _strip_cpp_comments(types_header.read_text(encoding="utf-8"))
    pattern = r"\b(?:enum\s+class|enum\s+struct|struct|class)\s+([A-Za-z_]\w*)"
    return tuple(sorted(set(re.findall(pattern, code))))


def _vehicle_signals_source_violations(root: Path) -> List[str]:
    signals_root = root / "lib/vehicle_signals"
    type_names = _mazda_type_names(root)
    violations: List[str] = []
    for path in sorted(signals_root.rglob("*")):
        if not path.is_file():
            continue
        label = path.relative_to(root).as_posix()
        text = path.read_text(encoding="utf-8")
        if path.suffix in _CPP_SUFFIXES:
            code, literals = _strip_cpp_comments(text)
            for literal in literals:
                if literal.startswith("vehicle."):
                    violations.append(f"{label} contains a Mazda catalog key literal: {literal}")
            for name in type_names:
                if re.search(rf"\b{re.escape(name)}\b", code):
                    violations.append(f"{label} names Mazda type {name}")
        elif path.name == "CMakeLists.txt" or path.suffix in {".cmake", ".yml", ".yaml"}:
            code = re.sub(r"#.*", "", text)
        else:
            continue
        if re.search("mazda", code, re.IGNORECASE):
            violations.append(f"{label} references Mazda outside comments")
    return violations


def _vehicle_signals_dependency_violations(
    compile_database: Path,
    consumer_source: Path,
    root: Path,
    work_dir: Path,
    core_root: Path,
) -> List[str]:
    try:
        entries = json.loads(compile_database.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        return [f"vehicle_signals compile database is unreadable: {error}"]
    consumer_source = consumer_source.resolve()
    matching = [entry for entry in entries if _compile_database_source(entry) == consumer_source]
    if not matching:
        return ["vehicle_signals consumer compile command is missing"]
    signals_include = (root / "lib/vehicle_signals/include").resolve()
    core_include = (core_root / "components/vehicle_core/include").resolve()
    allowed_core = {(core_include / name).resolve() for name in VEHICLE_SIGNALS_CORE_ALLOWED}
    violations: List[str] = []
    for index, entry in enumerate(matching):
        directory = entry.get("directory") if isinstance(entry, dict) else None
        cwd = Path(directory).resolve() if isinstance(directory, str) and directory else root
        tokens = _command_tokens(entry)
        for include_dir in _include_directories(tokens, cwd):
            if include_dir not in (signals_include, core_include):
                violations.append(
                    f"forbidden vehicle_signals consumer include directory: {include_dir}"
                )
        for token in tokens:
            if token.startswith(("-D", "/D")) and _FORBIDDEN_SIGNALS_DEFINITION.search(token):
                violations.append(f"forbidden vehicle_signals consumer definition: {token}")
        # -MD rather than -MMD: a header reached through -isystem must not be
        # hidden from the closure.
        depfile = work_dir / f"vehicle_signals_dependency_{index}.d"
        probe = _run([*tokens, "-MD", "-MF", str(depfile), "-MT", str(consumer_source)], cwd=cwd)
        if probe[0] != 0:
            violations.append("vehicle_signals dependency probe failed\n" + probe[1][-3000:])
            continue
        dependencies = _dependency_paths(depfile, cwd)
        if not dependencies:
            violations.append("vehicle_signals dependency probe produced no dependency data")
            continue
        for dependency in dependencies:
            if dependency == consumer_source or _is_within(dependency, signals_include):
                continue
            if _is_within(dependency, core_root):
                if dependency not in allowed_core:
                    violations.append(f"forbidden vehicle_signals core dependency: {dependency}")
                continue
            if _is_within(dependency, root) or _has_forbidden_signals_segment(dependency):
                violations.append(f"forbidden vehicle_signals dependency: {dependency}")
    return list(dict.fromkeys(violations))


def _check_vehicle_signals_only(
    root: Path,
    cmake: str,
    compiler: Sequence[str],
    work_dir: Path,
    core_root: Optional[Path] = None,
) -> None:
    root = root.resolve()
    core_root = (core_root or root / "third_party/esp32-vehicle-can-core").resolve()
    if not (root / "lib/vehicle_signals/CMakeLists.txt").is_file():
        raise ArchitectureFailure("vehicle_signals component is missing")
    probe_dir = work_dir / "vehicle_signals_probe"
    probe_dir.mkdir()
    source = _write_vehicle_signals_probe(probe_dir, root, core_root)
    build_dir = probe_dir / "build"
    configure = [cmake, "-S", str(probe_dir), "-B", str(build_dir), "-G", "Ninja"]
    if len(compiler) == 1:
        configure.append(f"-DCMAKE_CXX_COMPILER={compiler[0]}")
    result = _run(configure, cwd=root)
    if result[0] != 0:
        raise ArchitectureFailure("vehicle_signals-only configure failed\n" + result[1][-3000:])
    # Report the resolved interface and source scan alongside any build
    # failure: a Mazda link target otherwise surfaces only as a linker error.
    violations = _vehicle_signals_interface_violations(
        build_dir / "vehicle_signals_interface.txt", root, core_root
    )
    violations.extend(_vehicle_signals_source_violations(root))
    result = _run(
        [cmake, "--build", str(build_dir), "--target", "vehicle_signals_consumer"], cwd=root
    )
    if result[0] != 0:
        violations.append("vehicle_signals-only build failed\n" + result[1][-3000:])
        raise ArchitectureFailure("\n".join(violations))
    result = _run([str(build_dir / "vehicle_signals_consumer")], cwd=root)
    if result[0] != 0:
        violations.append("vehicle_signals-only consumer execution failed\n" + result[1][-3000:])
        raise ArchitectureFailure("\n".join(violations))
    violations.extend(
        _vehicle_signals_dependency_violations(
            build_dir / "compile_commands.json", source, root, work_dir, core_root
        )
    )
    if violations:
        raise ArchitectureFailure("\n".join(violations))
    print(
        "OK   vehicle_signals builds without Mazda/RTOS dependencies and reaches only "
        "value-only core contracts"
    )


GENERIC_CONSUMER_SOURCES = (
    Path("tests/host/generic_signal_consumer.hpp"),
    Path("tests/host/generic_signal_consumer.cpp"),
)
_GENERIC_CONSUMER_ALLOWED_INCLUDE = re.compile(
    r"mazda/signal_provider\.hpp|vehicle_signals/[\w/]+\.hpp|generic_signal_consumer\.hpp"
)


def _check_generic_consumer(root: Path) -> None:
    """Keep the host generic consumer on the public provider interface."""

    violations: List[str] = []
    for relative in GENERIC_CONSUMER_SOURCES:
        path = root / relative
        if not path.is_file():
            violations.append(f"generic consumer source is missing: {relative.as_posix()}")
            continue
        code, _ = _strip_cpp_comments(path.read_text(encoding="utf-8"))
        for delimiter, name in re.findall(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]', code, re.M):
            # Angle includes are limited to standard headers, which have no
            # directory component.
            allowed = (
                "/" not in name
                if delimiter == "<"
                else _GENERIC_CONSUMER_ALLOWED_INCLUDE.fullmatch(name) is not None
            )
            if not allowed:
                violations.append(f"{relative.as_posix()} includes forbidden header {name}")
    host_cmake = root / "tests/host/CMakeLists.txt"
    cmake_code = re.sub(r"#.*", "", host_cmake.read_text(encoding="utf-8"))
    if re.search(r"target_include_directories\s*\(\s*generic_signal_consumer\b", cmake_code):
        violations.append("generic_signal_consumer must not add include directories")
    for match in re.finditer(
        r"target_link_libraries\s*\(\s*generic_signal_consumer\b([^)]*)\)", cmake_code
    ):
        for item in match.group(1).split():
            if item not in {"PUBLIC", "PRIVATE", "INTERFACE", "mazda_telemetry_contracts"}:
                violations.append(f"generic_signal_consumer links forbidden target {item}")
    if violations:
        raise ArchitectureFailure("\n".join(violations))
    print("OK   generic signal consumer uses only the public provider and vehicle_signals headers")


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
            _check_vehicle_signals_only(root, cmake, compiler, work_dir, core_root)
            _check_generic_consumer(root)
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
