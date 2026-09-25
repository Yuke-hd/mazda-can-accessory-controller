#!/usr/bin/env python3
"""Run project-owned architecture contracts once per host suite.

This host-only gate owns repository-wide checks that cannot live in one
production target: the portable core must build without Mazda or RTOS inputs,
the portable ``vehicle_signals`` contracts must build against that core alone
and reach only its value-only telemetry contracts, the generic
``action_engine`` must build on those contracts alone, the host generic consumer
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
from dataclasses import dataclass
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


@dataclass(frozen=True)
class PortableLayer:
    """A make-independent library that must build against the core alone.

    Each layer is built in a generated probe project that adds only the core's
    ``vehicle_core`` component and the listed repository directories. The
    resolved CMake interface, a comment-stripped source scan and the real
    compile commands' dependency closure must stay inside the layer.
    """

    name: str
    # Repository directories added to the probe, dependencies first.
    directories: Tuple[str, ...]
    # Public headers the probe consumer includes.
    headers: Tuple[str, ...]
    # C++ probe body after the includes; it must define main() returning 0.
    probe_body: str
    # Allowed resolved link items of the layer target and the probe consumer.
    target_links: Tuple[str, ...]
    # Repository include roots the target may export (the core root is implied).
    include_roots: Tuple[str, ...]
    # Repository directory whose compiled sources are checked, if any.
    source_dir: Optional[str] = None


VEHICLE_SIGNALS_HEADERS = (
    "vehicle_signals/signal_contracts.hpp",
    "vehicle_signals/signal_catalog.hpp",
    "vehicle_signals/signal_provider.hpp",
)
ACTION_ENGINE_HEADERS = (
    "action_engine/action.hpp",
    "action_engine/actionability.hpp",
    "action_engine/condition.hpp",
    "action_engine/config_status.hpp",
    "action_engine/engine.hpp",
    "action_engine/range_rule.hpp",
    "action_engine/range_rule_set.hpp",
    "action_engine/rule_config.hpp",
    "action_engine/rule_set.hpp",
    "action_engine/rules.hpp",
    "action_engine/sink_fan_out.hpp",
    "action_engine/subscription_set.hpp",
)
# The only companion-core header the portable layers may reach.
VEHICLE_SIGNALS_CORE_ALLOWED = ("vehicle_core/telemetry_contracts.hpp",)
# Path segments that identify a make-specific, RTOS, SDK, or CAN-driver input.
_FORBIDDEN_SIGNALS_SEGMENTS = frozenset(
    ("mazda", "mazda_telemetry", "freertos", "esp-idf", "esp_idf", "esp", "sdkconfig")
)
_FORBIDDEN_SIGNALS_DEFINITION = re.compile(
    r"mazda|freertos|esp_platform|esp_idf|sdkconfig|twai", re.IGNORECASE
)
# Output, transport, CAN-driver and RTOS names a portable layer never uses.
_FORBIDDEN_LAYER_TOKENS = re.compile(r"wled|argb|twai|can_bus|freertos", re.IGNORECASE)
_CPP_SUFFIXES = frozenset((".c", ".cc", ".cpp", ".h", ".hpp", ".inl", ".ipp"))

_VEHICLE_SIGNALS_PROBE_BODY = (
    "#include <string_view>\n"
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
    "}\n"
)

# Drives one state rule and one range rule through a probe-local provider and
# sink, so the probe links and runs the engine's compiled sources.
_ACTION_ENGINE_PROBE_BODY = (
    "namespace {\n"
    "using namespace vehicle_signals;\n"
    "constexpr SignalMetadata kProbeCatalog[] = {\n"
    "    {SignalId{1}, \"probe.flag\", SignalType::Boolean, SignalUnit::None,\n"
    "     ValidationStatus::Reference, SignalCapability::Read | SignalCapability::Notify},\n"
    "    {SignalId{2}, \"probe.number\", SignalType::Number, SignalUnit::None,\n"
    "     ValidationStatus::Reference, SignalCapability::Read},\n"
    "};\n"
    "class ProbeProvider final : public SignalProvider {\n"
    "public:\n"
    "  SignalCatalogView catalog() const noexcept override { return kProbeCatalog; }\n"
    "  SignalResult<SignalReading> read(SignalId) const noexcept override {\n"
    "    return SignalResult<SignalReading>::success(\n"
    "        SignalReading{SignalValue::number(50.0F), Availability::Fresh});\n"
    "  }\n"
    "  SignalResult<SignalSubscription> subscribe(SignalId, SignalCallback callback,\n"
    "                                             void *context) noexcept override {\n"
    "    callback_ = callback;\n"
    "    context_ = context;\n"
    "    return SignalResult<SignalSubscription>::success(\n"
    "        SignalSubscription::from_provider_bits(1));\n"
    "  }\n"
    "  SignalStatusResult unsubscribe(SignalSubscription) noexcept override {\n"
    "    return SignalStatusResult::success();\n"
    "  }\n"
    "  void publish(const SignalNotification &notice) const noexcept {\n"
    "    callback_(context_, notice);\n"
    "  }\n"
    "private:\n"
    "  SignalCallback callback_{nullptr};\n"
    "  void *context_{nullptr};\n"
    "};\n"
    "class ProbeSink final : public action_engine::ActionSink {\n"
    "public:\n"
    "  void execute(const action_engine::ActionCommand &command) noexcept override {\n"
    "    last = command;\n"
    "  }\n"
    "  action_engine::ActionCommand last{};\n"
    "};\n"
    "} // namespace\n"
    "int main() {\n"
    "  ProbeProvider provider{};\n"
    "  ProbeSink sink{};\n"
    "  action_engine::ActionEngine engine{provider};\n"
    "  const action_engine::StateRuleConfig rule{\n"
    "      {\"probe.flag\", action_engine::Comparison::Equal,\n"
    "       action_engine::RuleOperand::boolean(true)},\n"
    "      action_engine::ActionId{1}};\n"
    "  const action_engine::RangeRuleConfig range{\"probe.number\", {0.0F, 100.0F},\n"
    "                                              {0.0F, 1.0F}, action_engine::ActionId{2}};\n"
    "  if (engine.add_sink(sink) != action_engine::ConfigStatus::Ok ||\n"
    "      engine.add_state_rule(rule) != action_engine::ConfigStatus::Ok ||\n"
    "      engine.add_range_rule(range) != action_engine::ConfigStatus::Ok ||\n"
    "      engine.attach() != SignalStatus::Ok) {\n"
    "    return 1;\n"
    "  }\n"
    "  SignalNotification notice{};\n"
    "  notice.id = SignalId{1};\n"
    "  notice.current.value = SignalValue::boolean(true);\n"
    "  notice.current.availability = Availability::Fresh;\n"
    "  notice.initial = true;\n"
    "  provider.publish(notice);\n"
    "  const bool activated = sink.last.kind == action_engine::ActionCommandKind::Activate;\n"
    "  const bool sampled = engine.sample_range_rules() == SignalStatus::Ok &&\n"
    "                       sink.last.kind == action_engine::ActionCommandKind::SetLevel &&\n"
    "                       sink.last.level == 0.5F;\n"
    "  return activated && sampled && engine.detach() == SignalStatus::Ok ? 0 : 1;\n"
    "}\n"
)

VEHICLE_SIGNALS_LAYER = PortableLayer(
    name="vehicle_signals",
    directories=("lib/vehicle_signals",),
    headers=VEHICLE_SIGNALS_HEADERS,
    probe_body=_VEHICLE_SIGNALS_PROBE_BODY,
    target_links=("vehicle_core",),
    include_roots=("lib/vehicle_signals/include",),
)
ACTION_ENGINE_LAYER = PortableLayer(
    name="action_engine",
    directories=("lib/vehicle_signals", "lib/action_engine"),
    headers=ACTION_ENGINE_HEADERS,
    probe_body=_ACTION_ENGINE_PROBE_BODY,
    target_links=("vehicle_signals",),
    include_roots=("lib/action_engine/include", "lib/vehicle_signals/include"),
    source_dir="lib/action_engine/src",
)


def _consumer_name(layer: PortableLayer) -> str:
    return f"{layer.name}_consumer"


def _write_layer_probe(
    probe_dir: Path, root: Path, core_root: Path, layer: PortableLayer
) -> Path:
    consumer = _consumer_name(layer)
    source = probe_dir / f"{consumer}.cpp"
    includes = "".join(f'#include "{header}"\n' for header in layer.headers)
    source.write_text(includes + layer.probe_body, encoding="utf-8")
    subdirectories = "".join(
        "add_subdirectory(" + _quoted(root / directory) + f" {Path(directory).name})\n"
        for directory in layer.directories
    )
    (probe_dir / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.20)\n"
        f"project({layer.name}_only_consumer LANGUAGES CXX)\n"
        "set(CMAKE_CXX_STANDARD 17)\n"
        "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n"
        "set(CMAKE_CXX_EXTENSIONS OFF)\n"
        "set(CMAKE_EXPORT_COMPILE_COMMANDS ON)\n"
        "set(BUILD_TESTING OFF CACHE BOOL \"\" FORCE)\n"
        "add_subdirectory(" + _quoted(core_root / "components/vehicle_core") + " vehicle_core)\n"
        + subdirectories
        + f"add_executable({consumer} " + _quoted(source) + ")\n"
        f"target_link_libraries({consumer} PRIVATE {layer.name})\n"
        f"target_compile_features({consumer} PRIVATE cxx_std_17)\n"
        # Record the evaluated target interface so link targets and exported
        # include directories are checked as CMake resolved them.
        f"file(GENERATE OUTPUT \"${{CMAKE_BINARY_DIR}}/{layer.name}_interface.txt\" CONTENT\n"
        f"  \"include=$<TARGET_PROPERTY:{layer.name},INTERFACE_INCLUDE_DIRECTORIES>\\n"
        f"link=$<TARGET_PROPERTY:{layer.name},INTERFACE_LINK_LIBRARIES>\\n"
        f"consumer_link=$<TARGET_PROPERTY:{consumer},LINK_LIBRARIES>\\n\")\n",
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


def _layer_include_roots(root: Path, core_root: Path, layer: PortableLayer) -> Tuple[Path, ...]:
    # CMake evaluates INTERFACE_INCLUDE_DIRECTORIES transitively, so the core's
    # include root legitimately appears through the vehicle_core link.
    return tuple((root / include).resolve() for include in layer.include_roots) + (
        (core_root / "components/vehicle_core/include").resolve(),
    )


def _layer_interface_violations(
    interface_file: Path, root: Path, core_root: Path, layer: PortableLayer
) -> List[str]:
    try:
        lines = interface_file.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        return [f"{layer.name} interface properties are unreadable: {error}"]
    allowed_includes = _layer_include_roots(root, core_root, layer)
    allowed_links = {"link": set(layer.target_links), "consumer_link": {layer.name}}
    labels = {"link": f"{layer.name} target", "consumer_link": f"{layer.name} consumer"}
    violations: List[str] = []
    for line in lines:
        key, _, value = line.partition("=")
        items = [part for part in value.split(";") if part]
        if key == "include":
            for item in items:
                include_dir = Path(item).resolve()
                if include_dir not in allowed_includes:
                    violations.append(
                        f"forbidden {layer.name} target include directory: {include_dir}"
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


def _layer_source_violations(root: Path, layer: PortableLayer) -> List[str]:
    layer_root = root / layer.directories[-1]
    type_names = _mazda_type_names(root)
    violations: List[str] = []
    for path in sorted(layer_root.rglob("*")):
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
        for token in sorted({match.lower() for match in _FORBIDDEN_LAYER_TOKENS.findall(code)}):
            violations.append(f"{label} references forbidden {token} outside comments")
    return violations


def _layer_compile_entries(
    entries: object, consumer_source: Path, root: Path, layer: PortableLayer
) -> Tuple[List[Tuple[str, object, Path]], List[str]]:
    consumer: List[Tuple[str, object, Path]] = []
    target: List[Tuple[str, object, Path]] = []
    source_dir = (root / layer.source_dir).resolve() if layer.source_dir else None
    for entry in entries if isinstance(entries, list) else []:
        source = _compile_database_source(entry)
        if source is None:
            continue
        if source == consumer_source:
            consumer.append((f"{layer.name} consumer", entry, source))
        elif source_dir is not None and _is_within(source, source_dir):
            target.append((f"{layer.name} target ({source.name})", entry, source))
    missing: List[str] = []
    if not consumer:
        missing.append(f"{layer.name} consumer compile command is missing")
    if source_dir is not None and not target:
        missing.append(f"{layer.name} target compile command is missing")
    return consumer + target, missing


def _layer_dependency_violations(
    compile_database: Path,
    consumer_source: Path,
    root: Path,
    work_dir: Path,
    core_root: Path,
    layer: PortableLayer,
) -> List[str]:
    try:
        entries = json.loads(compile_database.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        return [f"{layer.name} compile database is unreadable: {error}"]
    consumer_source = consumer_source.resolve()
    matching, missing = _layer_compile_entries(entries, consumer_source, root, layer)
    if missing:
        return missing
    include_roots = _layer_include_roots(root, core_root, layer)
    core_include = include_roots[-1]
    allowed_core = {(core_include / name).resolve() for name in VEHICLE_SIGNALS_CORE_ALLOWED}
    violations: List[str] = []
    for index, (label, entry, source) in enumerate(matching):
        directory = entry.get("directory") if isinstance(entry, dict) else None
        cwd = Path(directory).resolve() if isinstance(directory, str) and directory else root
        tokens = _command_tokens(entry)
        for include_dir in _include_directories(tokens, cwd):
            if include_dir not in include_roots:
                violations.append(f"forbidden {label} include directory: {include_dir}")
        for token in tokens:
            if token.startswith(("-D", "/D")) and _FORBIDDEN_SIGNALS_DEFINITION.search(token):
                violations.append(f"forbidden {label} definition: {token}")
        # -MD rather than -MMD: a header reached through -isystem must not be
        # hidden from the closure.
        depfile = work_dir / f"{layer.name}_dependency_{index}.d"
        probe = _run([*tokens, "-MD", "-MF", str(depfile), "-MT", str(source)], cwd=cwd)
        if probe[0] != 0:
            violations.append(f"{label} dependency probe failed\n" + probe[1][-3000:])
            continue
        dependencies = _dependency_paths(depfile, cwd)
        if not dependencies:
            violations.append(f"{label} dependency probe produced no dependency data")
            continue
        for dependency in dependencies:
            if dependency == source or any(
                _is_within(dependency, include) for include in include_roots[:-1]
            ):
                continue
            if _is_within(dependency, core_root):
                if dependency not in allowed_core:
                    violations.append(f"forbidden {layer.name} core dependency: {dependency}")
                continue
            if _is_within(dependency, root) or _has_forbidden_signals_segment(dependency):
                violations.append(f"forbidden {layer.name} dependency: {dependency}")
    return list(dict.fromkeys(violations))


def _check_portable_layer(
    root: Path,
    cmake: str,
    compiler: Sequence[str],
    work_dir: Path,
    layer: PortableLayer,
    core_root: Optional[Path] = None,
) -> None:
    root = root.resolve()
    core_root = (core_root or root / "third_party/esp32-vehicle-can-core").resolve()
    for directory in layer.directories:
        if not (root / directory / "CMakeLists.txt").is_file():
            raise ArchitectureFailure(f"{Path(directory).name} component is missing")
    probe_dir = work_dir / f"{layer.name}_probe"
    probe_dir.mkdir()
    source = _write_layer_probe(probe_dir, root, core_root, layer)
    build_dir = probe_dir / "build"
    configure = [cmake, "-S", str(probe_dir), "-B", str(build_dir), "-G", "Ninja"]
    if len(compiler) == 1:
        configure.append(f"-DCMAKE_CXX_COMPILER={compiler[0]}")
    result = _run(configure, cwd=root)
    if result[0] != 0:
        raise ArchitectureFailure(f"{layer.name}-only configure failed\n" + result[1][-3000:])
    # Report the resolved interface and source scan alongside any build
    # failure: a Mazda link target otherwise surfaces only as a linker error.
    violations = _layer_interface_violations(
        build_dir / f"{layer.name}_interface.txt", root, core_root, layer
    )
    violations.extend(_layer_source_violations(root, layer))
    consumer = _consumer_name(layer)
    result = _run([cmake, "--build", str(build_dir), "--target", consumer], cwd=root)
    if result[0] != 0:
        violations.append(f"{layer.name}-only build failed\n" + result[1][-3000:])
        raise ArchitectureFailure("\n".join(violations))
    result = _run([str(build_dir / consumer)], cwd=root)
    if result[0] != 0:
        violations.append(f"{layer.name}-only consumer execution failed\n" + result[1][-3000:])
        raise ArchitectureFailure("\n".join(violations))
    violations.extend(
        _layer_dependency_violations(
            build_dir / "compile_commands.json", source, root, work_dir, core_root, layer
        )
    )
    if violations:
        raise ArchitectureFailure("\n".join(violations))


def _check_vehicle_signals_only(
    root: Path,
    cmake: str,
    compiler: Sequence[str],
    work_dir: Path,
    core_root: Optional[Path] = None,
) -> None:
    _check_portable_layer(root, cmake, compiler, work_dir, VEHICLE_SIGNALS_LAYER, core_root)
    print(
        "OK   vehicle_signals builds without Mazda/RTOS dependencies and reaches only "
        "value-only core contracts"
    )


def _check_action_engine_only(
    root: Path,
    cmake: str,
    compiler: Sequence[str],
    work_dir: Path,
    core_root: Optional[Path] = None,
) -> None:
    _check_portable_layer(root, cmake, compiler, work_dir, ACTION_ENGINE_LAYER, core_root)
    print(
        "OK   action_engine builds on vehicle_signals alone without Mazda, CAN, RTOS or "
        "output dependencies"
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


LED_ACTIONS_COMPONENT = Path("components/local_argb_actions")
# The adapter sees the engine's output port, the renderer's private handoff and
# its own headers; never a provider, a vehicle make, CAN or the RTOS.
_LED_ACTIONS_ALLOWED_INCLUDE = re.compile(
    r"action_engine/action\.hpp|local_argb/lighting_sink\.hpp|local_argb_actions/[\w/]+\.hpp"
)
_LED_ACTIONS_ALLOWED_LINKS = frozenset(
    ("PUBLIC", "PRIVATE", "INTERFACE", "action_engine", "local_argb_sink_contract")
)
_LED_ACTIONS_FORBIDDEN_NAMES = re.compile(r"mazda|twai|can_bus|freertos|wled", re.IGNORECASE)


def _check_led_action_adapter(root: Path) -> None:
    """Keep the local LED action sink a pure engine-to-renderer adapter."""

    component = root / LED_ACTIONS_COMPONENT
    violations: List[str] = []
    sources = sorted(
        path for path in component.rglob("*") if path.suffix in _CPP_SUFFIXES and path.is_file()
    )
    if not sources:
        violations.append(f"local LED action adapter sources are missing: {LED_ACTIONS_COMPONENT}")
    for path in sources:
        relative = path.relative_to(root).as_posix()
        code, _ = _strip_cpp_comments(path.read_text(encoding="utf-8"))
        for delimiter, name in re.findall(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]', code, re.M):
            # Angle includes are limited to standard headers, which have no
            # directory component.
            allowed = (
                "/" not in name
                if delimiter == "<"
                else _LED_ACTIONS_ALLOWED_INCLUDE.fullmatch(name) is not None
            )
            if not allowed:
                violations.append(f"{relative} includes forbidden header {name}")
        body = re.sub(r'^\s*#\s*include[^\n]*', "", code, flags=re.M)
        for name in sorted(set(m.lower() for m in _LED_ACTIONS_FORBIDDEN_NAMES.findall(body))):
            violations.append(f"{relative} uses forbidden name {name}")
    cmake = component / "CMakeLists.txt"
    cmake_code = re.sub(r"#.*", "", cmake.read_text(encoding="utf-8")) if cmake.is_file() else ""
    for match in re.finditer(
        r"target_link_libraries\s*\(\s*local_argb_actions\b([^)]*)\)", cmake_code
    ):
        for item in match.group(1).split():
            if item not in _LED_ACTIONS_ALLOWED_LINKS:
                violations.append(f"local_argb_actions links forbidden target {item}")
    if violations:
        raise ArchitectureFailure("\n".join(violations))
    print(
        "OK   local LED action adapter uses only the engine action port and the renderer "
        "sink contract"
    )


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
            _check_action_engine_only(root, cmake, compiler, work_dir, core_root)
            _check_generic_consumer(root)
            _check_led_action_adapter(root)
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
