#!/usr/bin/env python3
"""Check the public C++ header and target boundaries.

The check is intentionally independent from the project's normal host test
build.  Each public entry point is compiled in its own translation unit and
the compiler's dependency file is inspected; this catches transitive
dependencies even when a header happens to compile successfully.  A small
CMake consumer project then exercises the exported interface of the real
``mazda_telemetry_contracts`` target, including a provider-only consumer that
is held to the generic provider's stricter dependency rules.

The checker is a required Stage 1.5 host regression gate. It is registered
once by the host CTest flow after the public/private header split and returns a
failure for any boundary violation.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from typing import Iterable, List, Optional, Sequence, Tuple


DependencyRules = Tuple[Tuple[str, Tuple[str, ...]], ...]


@dataclass(frozen=True)
class PublicHeader:
    include: str
    label: str
    # Rules applied to this header's closure on top of FORBIDDEN_DEPENDENCIES.
    extra_forbidden: DependencyRules = ()


# The generic provider exposes value-only vehicle_signals contracts. Unlike the
# typed facade, whose value types legitimately live in mazda/types.hpp, it must
# not reach Mazda value types, state, definitions, the service, publication
# store, or the private catalog/descriptor bindings.
PROVIDER_FORBIDDEN_DEPENDENCIES: DependencyRules = (
    (
        "provider service/state/publication/catalog",
        (
            "mazda/vehicle_telemetry_service.hpp",
            "mazda/vehicle_telemetry_internal.hpp",
            "mazda/publication_store.hpp",
            "mazda/signal_catalog.hpp",
            "mazda/state.hpp",
            "mazda/types.hpp",
            "mazda/definitions.hpp",
        ),
    ),
)


# The portable signal contracts know no vehicle make. Every Mazda header,
# public or private, lives below an include*/mazda/ directory.
VEHICLE_SIGNALS_FORBIDDEN_DEPENDENCIES: DependencyRules = (
    ("Mazda dependency", ("include/mazda/",)),
)


# These are the application-facing entry points, including the frozen Mazda
# compatibility umbrella.  The compatibility path remains public even while
# its internal handoff includes are being moved behind an internal target.
PUBLIC_HEADERS: Tuple[PublicHeader, ...] = (
    PublicHeader("mazda/vehicle_telemetry.hpp", "vehicle telemetry facade"),
    PublicHeader("mazda/facade_contracts.hpp", "facade contracts"),
    PublicHeader("mazda/reading.hpp", "reading contract"),
    PublicHeader("mazda/notification.hpp", "notification contract"),
    PublicHeader("mazda/telemetry_contracts.hpp", "Mazda compatibility contracts"),
    PublicHeader("vehicle_core/telemetry_contracts.hpp", "public telemetry contracts"),
    PublicHeader(
        "mazda/signal_provider.hpp", "generic signal provider", PROVIDER_FORBIDDEN_DEPENDENCIES
    ),
    PublicHeader(
        "vehicle_signals/signal_contracts.hpp",
        "portable signal contracts",
        VEHICLE_SIGNALS_FORBIDDEN_DEPENDENCIES,
    ),
    PublicHeader(
        "vehicle_signals/signal_catalog.hpp",
        "portable signal catalog view",
        VEHICLE_SIGNALS_FORBIDDEN_DEPENDENCIES,
    ),
)


# Match path names rather than source spellings.  These are the boundaries
# that must remain outside every application-facing header's include closure.
# A suffix match keeps the checker usable for a temporary copied fixture root.
FORBIDDEN_DEPENDENCIES: DependencyRules = (
    ("raw frame", ("vehicle_core/frame.hpp",)),
    (
        "decoder contracts/definitions",
        ("vehicle_core/decoder_contracts.hpp", "mazda/decoder.hpp", "mazda/definitions.hpp"),
    ),
    ("mutable signal/state", ("vehicle_core/signal.hpp", "mazda/state.hpp")),
    ("notification implementation", ("vehicle_core/notification_channel.hpp",)),
    ("lighting implementation", ("lighting_sink.hpp",)),
    ("board dependency", ("board/board_config.h",)),
    (
        "CAN driver dependency",
        ("can_bus.h", "driver_binding.hpp", "driver/twai.h", "twai.h"),
    ),
    (
        "RTOS/SDK dependency",
        ("freertos/", "freertos.h", "esp_idf/", "esp/", "esp_system.h", "sdkconfig.h"),
    ),
)


@dataclass
class CommandResult:
    returncode: int
    output: str


REQUIRED_INTERNAL_HEADER = Path("lib/mazda/internal_include/mazda/internal_contracts.hpp")
PUBLIC_INTERNAL_HEADER = Path("lib/mazda/include/mazda/internal_contracts.hpp")


def _run(command: Sequence[str], *, cwd: Path, timeout: int = 90) -> CommandResult:
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
        return CommandResult(127, str(error))
    output = "\n".join(part for part in (result.stdout, result.stderr) if part).strip()
    return CommandResult(result.returncode, output)


def _compiler_command(requested: Optional[str]) -> Tuple[str, ...]:
    value = requested or os.environ.get("CXX") or "c++"
    command = tuple(shlex.split(value))
    if not command:
        raise ValueError("the C++ compiler command is empty")
    if shutil.which(command[0]) is None and not Path(command[0]).is_file():
        raise FileNotFoundError(f"C++ compiler not found: {command[0]}")
    return command


def _include_dirs(root: Path, core_root: Optional[Path] = None) -> Tuple[Path, ...]:
    resolved_core_root = (core_root or root / "third_party/esp32-vehicle-can-core").resolve()
    candidates = (
        root / "components/mazda_telemetry/include",
        root / "components/vehicle_telemetry/include",  # legacy isolated fixture
        root / "lib/mazda/include",
        root / "lib/vehicle_signals/include",
        resolved_core_root / "components/vehicle_core/include",
        root / "lib/vehicle_core/include",  # legacy isolated fixture
    )
    return tuple(path.resolve() for path in candidates if path.is_dir())


def _compiler_is_msvc(compiler: Sequence[str]) -> bool:
    compiler_name = Path(compiler[0]).name.lower()
    if compiler_name in {"cl", "cl.exe", "clang-cl", "clang-cl.exe"}:
        return True
    result = _run((*compiler, "--version"), cwd=Path.cwd(), timeout=15)
    text = result.output.lower()
    return "microsoft" in text or "msvc" in text


def _make_tokens(text: str) -> List[str]:
    """Parse a GCC/Clang make-style dependency file.

    Paths containing spaces are escaped by the compiler.  ``shlex`` handles
    those escapes after line continuations are removed.  The target is
    discarded before tokenization so a Windows drive colon cannot be mistaken
    for a dependency separator.
    """

    flattened = text.replace("\\\n", " ")
    separator = -1
    escaped = False
    for index, char in enumerate(flattened):
        if char == ":" and not escaped:
            separator = index
            break
        if char == "\\" and not escaped:
            escaped = True
        else:
            escaped = False
    if separator < 0:
        return []
    try:
        return shlex.split(flattened[separator + 1 :], posix=True)
    except ValueError:
        # A malformed depfile is a boundary failure, not a reason to silently
        # fall back to source-text inspection.
        return []


def _dependency_paths(depfile: Path, cwd: Path) -> Tuple[Path, ...]:
    if not depfile.is_file():
        return ()
    paths: List[Path] = []
    for token in _make_tokens(depfile.read_text(encoding="utf-8")):
        path = Path(token)
        if not path.is_absolute():
            path = cwd / path
        resolved = path.resolve()
        if resolved not in paths:
            paths.append(resolved)
    return tuple(paths)


def _relative_name(path: Path, root: Path) -> str:
    try:
        return path.resolve().relative_to(root.resolve()).as_posix().lower()
    except ValueError:
        return path.as_posix().lower()


def _dependency_violation(
    path: Path, root: Path, extra_forbidden: DependencyRules = ()
) -> Optional[str]:
    name = _relative_name(path, root)
    # Do not treat the host's temporary directory (often /private/var on
    # macOS) as a repository-private include.  Only the deliberate component
    # marker is a boundary signal.
    try:
        relative_parts = {part.lower() for part in path.resolve().relative_to(root.resolve()).parts}
    except ValueError:
        relative_parts = set()
    if (
        "private_include" in relative_parts
        or "internal_include" in relative_parts
        or "private" in relative_parts
    ):
        return "private/internal include path"
    if name.endswith("mazda/internal_contracts.hpp"):
        return "private internal contract"
    for category, suffixes in (*FORBIDDEN_DEPENDENCIES, *extra_forbidden):
        if any(name.endswith(suffix) or suffix in name for suffix in suffixes):
            return category
    return None


def _compile_translation_unit(
    compiler: Sequence[str],
    source: Path,
    root: Path,
    include_dirs: Iterable[Path],
    work_dir: Path,
    *,
    depfile_name: str,
    msvc: bool = False,
) -> CommandResult:
    output = work_dir / (source.stem + ".o")
    depfile = work_dir / depfile_name
    if msvc:
        command = [*compiler, "/nologo", "/std:c++17", "/c", str(source), f"/Fo{output}"]
        command.extend(f"/I{path}" for path in include_dirs)
    else:
        command = [
            *compiler,
            "-std=c++17",
            "-fsyntax-only",
            "-MD",
            "-MF",
            str(depfile),
            "-MT",
            str(output),
        ]
        command.extend(f"-I{path}" for path in include_dirs)
        command.append(str(source))
    result = _run(command, cwd=root)
    if not msvc and result.returncode == 0 and not depfile.is_file():
        return CommandResult(1, "compiler succeeded without producing a dependency file")
    return result


def _header_source(header: PublicHeader, directory: Path) -> Path:
    source = directory / (header.include.replace("/", "_") + ".cpp")
    source.write_text(f'#include "{header.include}"\nint main() {{ return 0; }}\n', encoding="utf-8")
    return source


def _unsupported_msvc_failure() -> List[str]:
    message = "MSVC dependency inspection is unsupported; use GCC or Clang"
    print(f"FAIL public header boundary check: {message}")
    return [message]


def check_public_headers(
    root: Path,
    compiler: Sequence[str],
    work_dir: Path,
    core_root: Optional[Path] = None,
) -> List[str]:
    failures: List[str] = []
    include_dirs = _include_dirs(root, core_root)
    if not include_dirs:
        return ["no public include directories were found"]
    msvc = _compiler_is_msvc(compiler)
    if msvc:
        return _unsupported_msvc_failure()
    for index, header in enumerate(PUBLIC_HEADERS):
        source = _header_source(header, work_dir)
        depfile = work_dir / f"header_{index}.d"
        result = _compile_translation_unit(
            compiler,
            source,
            root,
            include_dirs,
            work_dir,
            depfile_name=depfile.name,
            msvc=msvc,
        )
        prefix = f"{header.include} ({header.label})"
        if result.returncode != 0:
            detail = result.output[-3000:] if result.output else "compiler failed"
            failures.append(f"{prefix}: compile failed\n{detail}")
            print(f"FAIL {prefix}: compile failed")
            if detail:
                print(f"      {detail}")
            continue
        dependencies = _dependency_paths(depfile, root)
        if not dependencies:
            failures.append(f"{prefix}: compiler dependency output was empty or malformed")
            print(f"FAIL {prefix}: dependency output was empty or malformed")
            continue
        violations = []
        for dependency in dependencies:
            category = _dependency_violation(dependency, root, header.extra_forbidden)
            if category is not None:
                violations.append((category, _relative_name(dependency, root)))
        if violations:
            unique = list(dict.fromkeys(violations))
            detail = "; ".join(f"{category}: {name}" for category, name in unique)
            failures.append(f"{prefix}: forbidden transitive dependency ({detail})")
            print(f"FAIL {prefix}: forbidden transitive dependency")
            print(f"      {detail}")
        else:
            print(f"OK   {prefix}: compiled with a clean dependency closure")
    return failures


def _cmake_probe_files(probe_dir: Path, root: Path) -> Tuple[Path, Path, Path, Path]:
    # The probes use only the provider's constructor, catalog() and read(), so
    # linking the public target proves those members are exported.
    facade_source = probe_dir / "facade_consumer.cpp"
    facade_source.write_text(
        '#include "mazda/vehicle_telemetry.hpp"\n'
        '#include "mazda/facade_contracts.hpp"\n'
        '#include "mazda/reading.hpp"\n'
        '#include "mazda/notification.hpp"\n'
        '#include "mazda/signal_provider.hpp"\n'
        '#include "mazda/telemetry_contracts.hpp"\n'
        '#include "vehicle_core/telemetry_contracts.hpp"\n'
        '#include "vehicle_signals/signal_catalog.hpp"\n'
        '#include "vehicle_signals/signal_contracts.hpp"\n'
        "int main() {\n"
        "  mazda::VehicleTelemetry telemetry;\n"
        "  const mazda::MazdaSignalProvider provider{telemetry};\n"
        "  (void)provider.catalog();\n"
        "  return 0;\n"
        "}\n",
        encoding="utf-8",
    )
    provider_source = probe_dir / "provider_consumer.cpp"
    provider_source.write_text(
        '#include "mazda/signal_provider.hpp"\n'
        "vehicle_signals::SignalCatalogView\n"
        "provider_catalog(const mazda::MazdaSignalProvider &provider) noexcept {\n"
        "  return provider.catalog();\n"
        "}\n"
        "bool provider_reads(const mazda::MazdaSignalProvider &provider) noexcept {\n"
        "  return provider.read(vehicle_signals::SignalId{}).ok();\n"
        "}\n"
        "int main() { return 0; }\n",
        encoding="utf-8",
    )
    lower_level_source = probe_dir / "lower_level_consumer.cpp"
    lower_level_source.write_text(
        '#include "vehicle_core/frame.hpp"\n'
        "int main() {\n"
        "  vehicle_core::RawCanFrame frame{};\n"
        "  (void)frame;\n"
        "  return 0;\n"
        "}\n",
        encoding="utf-8",
    )
    cmake = probe_dir / "CMakeLists.txt"
    target_cmake = root / "components/mazda_telemetry/CMakeLists.txt"
    project_cmake = (
        target_cmake if target_cmake.is_file() else root / "CMakeLists.txt"
    ).read_text(encoding="utf-8")
    contracts_target = (
        "mazda_telemetry_contracts"
        if "add_library(mazda_telemetry_contracts" in project_cmake
        else "vehicle_telemetry_contracts"
    )
    cmake.write_text(
        "cmake_minimum_required(VERSION 3.20)\n"
        "project(public_header_consumer LANGUAGES CXX)\n"
        "set(CMAKE_EXPORT_COMPILE_COMMANDS ON)\n"
        "set(BUILD_TESTING OFF CACHE BOOL \"\" FORCE)\n"
        "set(MAZDA_BUILD_HOST_TESTS OFF CACHE BOOL \"\" FORCE)\n"
        f'add_subdirectory({json.dumps(root.as_posix())} project)\n'
        f'add_executable(public_header_facade_consumer {json.dumps(facade_source.as_posix())})\n'
        f"target_link_libraries(public_header_facade_consumer PRIVATE {contracts_target})\n"
        f'add_executable(public_header_lower_level_consumer {json.dumps(lower_level_source.as_posix())})\n'
        f"target_link_libraries(public_header_lower_level_consumer PRIVATE {contracts_target})\n"
        f'add_executable(public_header_provider_consumer {json.dumps(provider_source.as_posix())})\n'
        f"target_link_libraries(public_header_provider_consumer PRIVATE {contracts_target})\n",
        encoding="utf-8",
    )
    return cmake, facade_source, lower_level_source, provider_source


def _compile_database_entry_source(entry: object) -> Optional[Path]:
    if not isinstance(entry, dict):
        return None
    file_name = entry.get("file")
    if not isinstance(file_name, str) or not file_name:
        return None
    path = Path(file_name)
    if not path.is_absolute():
        directory = entry.get("directory")
        if isinstance(directory, str) and directory:
            path = Path(directory) / path
    return path.resolve()


def _split_command(command: str) -> List[str]:
    try:
        return shlex.split(command, posix=os.name != "nt")
    except ValueError:
        return []


def _expand_response_files(
    tokens: Sequence[str], base_dir: Path, seen: Optional[Tuple[Path, ...]] = None
) -> Tuple[List[str], List[str]]:
    expanded: List[str] = []
    errors: List[str] = []
    active = seen or ()
    for token in tokens:
        if not token.startswith("@") or len(token) == 1:
            expanded.append(token)
            continue
        response_file = Path(token[1:])
        if not response_file.is_absolute():
            response_file = base_dir / response_file
        response_file = response_file.resolve()
        if response_file in active:
            errors.append(f"response file cycle: {response_file}")
            continue
        try:
            response_text = response_file.read_text(encoding="utf-8")
        except OSError as error:
            errors.append(f"unable to read response file {response_file}: {error}")
            continue
        response_tokens = _split_command(response_text)
        if not response_tokens and response_text.strip():
            errors.append(f"malformed response file: {response_file}")
            continue
        nested, nested_errors = _expand_response_files(
            response_tokens, response_file.parent, (*active, response_file)
        )
        expanded.extend(nested)
        errors.extend(nested_errors)
    return expanded, errors


def _compile_database_command(entry: object) -> Tuple[List[str], List[str]]:
    if not isinstance(entry, dict):
        return [], ["compile database entry is not an object"]
    arguments = entry.get("arguments")
    if isinstance(arguments, list) and all(isinstance(argument, str) for argument in arguments):
        tokens = list(arguments)
    else:
        command = entry.get("command")
        if not isinstance(command, str):
            return [], ["compile database entry has neither arguments nor command"]
        tokens = _split_command(command)
        if not tokens and command.strip():
            return [], ["compile database command is malformed"]
    directory = entry.get("directory")
    base_dir = Path(directory).resolve() if isinstance(directory, str) and directory else Path.cwd()
    return _expand_response_files(tokens, base_dir)


def _include_directory_arguments(tokens: Sequence[str], cwd: Path) -> Tuple[Path, ...]:
    directories: List[Path] = []
    separate_flags = {"-I", "/I", "-isystem", "-iquote", "-idirafter"}

    def add(value: str) -> None:
        path = Path(value)
        if not path.is_absolute():
            path = cwd / path
        resolved = path.resolve()
        if resolved not in directories:
            directories.append(resolved)

    index = 0
    while index < len(tokens):
        token = tokens[index]
        if token in separate_flags:
            if index + 1 < len(tokens):
                add(tokens[index + 1])
                index += 2
                continue
        elif token.startswith("--include-directory="):
            add(token.split("=", 1)[1])
        elif token.startswith("-I") and len(token) > 2:
            add(token[2:])
        elif token.startswith("/I") and len(token) > 2:
            add(token[2:])
        elif token.startswith("-isystem") and len(token) > len("-isystem"):
            add(token[len("-isystem") :])
        elif token.startswith("-iquote") and len(token) > len("-iquote"):
            add(token[len("-iquote") :])
        index += 1
    return tuple(directories)


def _consumer_dependency_probe(
    entry: object, source: Path, work_dir: Path, index: int, label: str = "facade"
) -> Tuple[CommandResult, Path]:
    """Re-run an exact CMake consumer command with an explicit depfile.

    Some CMake versions/generators omit automatic dependency flags from
    ``compile_commands.json`` and do not leave a usable ``<output>.d`` file.
    Appending dependency-only flags to the actual command preserves all
    target-provided definitions, include paths, and warning/options while
    making the dependency observation deterministic.
    """

    tokens, parse_errors = _compile_database_command(entry)
    depfile = work_dir / f"{label}_consumer_{index}.d"
    if parse_errors:
        return CommandResult(1, "; ".join(parse_errors)), depfile
    directory = entry.get("directory") if isinstance(entry, dict) else None
    command_cwd = Path(directory).resolve() if isinstance(directory, str) and directory else source.parent
    command = [*tokens, "-MD", "-MF", str(depfile), "-MT", str(source)]
    return _run(command, cwd=command_cwd, timeout=120), depfile


def _private_include_argument(path: Path, root: Path) -> Optional[str]:
    try:
        relative_parts = path.resolve().relative_to(root.resolve()).parts
    except ValueError:
        return None
    for marker in ("private_include", "internal_include"):
        if marker in {part.lower() for part in relative_parts}:
            return _relative_name(path, root)
    return None


def _consumer_dependency_failures(
    entries: Sequence[object],
    source: Path,
    label: str,
    root: Path,
    work_dir: Path,
    extra_forbidden: DependencyRules = (),
) -> List[str]:
    failures: List[str] = []
    for index, entry in enumerate(entries):
        probe, depfile = _consumer_dependency_probe(entry, source, work_dir, index, label)
        if probe.returncode != 0:
            detail = probe.output[-3000:] if probe.output else "compiler failed"
            failures.append(f"{label} consumer dependency probe compile failed: {detail}")
            continue
        if not depfile.is_file():
            failures.append(f"{label} consumer dependency probe did not produce a dependency file")
            continue
        directory = entry.get("directory") if isinstance(entry, dict) else None
        command_cwd = Path(directory).resolve() if isinstance(directory, str) and directory else root
        dependencies = _dependency_paths(depfile, command_cwd)
        if not dependencies:
            failures.append(f"{label} consumer dependency output was empty or malformed")
            continue
        for dependency in dependencies:
            category = _dependency_violation(dependency, root, extra_forbidden)
            if category is not None:
                failures.append(f"{category}: {_relative_name(dependency, root)}")
    return list(dict.fromkeys(failures))


def check_consumer_target(
    root: Path,
    cmake: str,
    compiler: Sequence[str],
    work_dir: Path,
    core_root: Optional[Path] = None,
) -> List[str]:
    failures: List[str] = []
    probe_dir = work_dir / "cmake_probe"
    build_dir = work_dir / "cmake_build"
    probe_dir.mkdir()
    _, facade_source, lower_level_source, provider_source = _cmake_probe_files(probe_dir, root)
    configure = [cmake, "-S", str(probe_dir), "-B", str(build_dir), "-DBUILD_TESTING=OFF"]
    if len(compiler) == 1:
        configure.append(f"-DCMAKE_CXX_COMPILER={compiler[0]}")
    if core_root is not None:
        configure.append(f"-DVEHICLE_CAN_CORE_SOURCE_DIR={core_root.resolve()}")
    configured = _run(configure, cwd=root, timeout=120)
    if configured.returncode != 0:
        detail = configured.output[-3000:] if configured.output else "CMake configure failed"
        failures.append(f"CMake consumer probe configure failed\n{detail}")
        print("FAIL CMake consumer target: configure failed")
        if detail:
            print(f"      {detail}")
        return failures
    built = _run(
        [
            cmake,
            "--build",
            str(build_dir),
            "--target",
            "public_header_facade_consumer",
            "public_header_lower_level_consumer",
            "public_header_provider_consumer",
        ],
        cwd=root,
        timeout=120,
    )
    if built.returncode != 0:
        detail = built.output[-3000:] if built.output else "CMake build failed"
        failures.append(f"CMake consumer probe build failed\n{detail}")
        print("FAIL CMake consumer target: build failed")
        if detail:
            print(f"      {detail}")
        return failures

    compile_commands = build_dir / "compile_commands.json"
    if not compile_commands.is_file():
        failures.append("CMake consumer probe did not emit compile_commands.json")
        print("FAIL CMake consumer target: no compile_commands.json")
        return failures
    try:
        entries = json.loads(compile_commands.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        failures.append(f"CMake consumer probe compile database is unreadable: {error}")
        print("FAIL CMake consumer target: compile database unreadable")
        return failures
    facade_entries = [
        entry for entry in entries if _compile_database_entry_source(entry) == facade_source.resolve()
    ]
    lower_level_entries = [
        entry
        for entry in entries
        if _compile_database_entry_source(entry) == lower_level_source.resolve()
    ]
    provider_entries = [
        entry
        for entry in entries
        if _compile_database_entry_source(entry) == provider_source.resolve()
    ]
    if not facade_entries or not lower_level_entries or not provider_entries:
        failures.append("CMake consumer probe did not expose every consumer compile command")
        print("FAIL CMake consumer target: consumer compile command missing")
        return failures
    forbidden: List[str] = []
    for entry in [*facade_entries, *lower_level_entries, *provider_entries]:
        tokens, parse_errors = _compile_database_command(entry)
        if parse_errors:
            detail = "; ".join(parse_errors)
            failures.append(f"CMake consumer probe compile command is unreadable: {detail}")
            print("FAIL CMake consumer target: compile command unreadable")
            continue
        directory = entry.get("directory") if isinstance(entry, dict) else None
        command_cwd = Path(directory).resolve() if isinstance(directory, str) and directory else root
        for include_dir in _include_directory_arguments(tokens, command_cwd):
            marker = _private_include_argument(include_dir, root)
            if marker is not None:
                forbidden.append(marker)
    # The provider-only consumer is held to the provider header's stricter
    # rules; the facade consumer legitimately reaches Mazda value types.
    consumers = (
        ("facade", facade_entries, facade_source, ()),
        ("provider", provider_entries, provider_source, PROVIDER_FORBIDDEN_DEPENDENCIES),
    )
    for label, consumer_entries, source, extra_forbidden in consumers:
        dependency_failures = _consumer_dependency_failures(
            consumer_entries, source, label, root, work_dir, extra_forbidden
        )
        if dependency_failures:
            detail = "; ".join(dependency_failures)
            failures.append(
                f"CMake {label} consumer has a forbidden dependency under its actual compile "
                "flags: " + detail
            )
            print(f"FAIL CMake {label} consumer: forbidden dependency under actual compile flags")
            print(f"      {detail}")
    if forbidden:
        unique = list(dict.fromkeys(forbidden))
        detail = ", ".join(unique)
        failures.append(f"CMake consumer target exports a private/internal include path: {detail}")
        print("FAIL CMake consumer target: exported private/internal include path")
        print(f"      {detail}")
    elif not failures:
        # Check the exact consumer command rather than CMake source spelling:
        # this is the actual interface consumed by a downstream target. The
        # facade dependency file comes from that same command, including any
        # target-controlled definitions or options.
        print("OK   CMake consumer target: isolated public interface compiled")
    return failures


def check_internal_access(
    root: Path,
    compiler: Sequence[str],
    work_dir: Path,
    core_root: Optional[Path] = None,
) -> List[str]:
    failures: List[str] = []
    public_internal = root / PUBLIC_INTERNAL_HEADER
    authorized_internal = root / REQUIRED_INTERNAL_HEADER
    if not authorized_internal.is_file():
        message = f"missing required internal header: {REQUIRED_INTERNAL_HEADER.as_posix()}"
        failures.append(message)
        print(f"FAIL internal contract probe: {message}")
    if public_internal.is_file():
        message = (
            "public mazda/internal_contracts.hpp is top-level accessible; "
            "internal access must be denied by top-level inaccessibility"
        )
        failures.append(message)
        print(f"FAIL internal contract probe: {message}")
    if failures:
        return failures
    normal_source = work_dir / "internal_normal.cpp"
    normal_source.write_text('#include "mazda/internal_contracts.hpp"\nint main() { return 0; }\n', encoding="utf-8")
    include_dirs = _include_dirs(root, core_root)
    normal = _compile_translation_unit(
        compiler,
        normal_source,
        root,
        include_dirs,
        work_dir,
        depfile_name="internal_normal.d",
        msvc=_compiler_is_msvc(compiler),
    )
    if normal.returncode == 0:
        failures.append("normal consumer access to mazda/internal_contracts.hpp unexpectedly succeeded")
        print("FAIL internal contract probe: normal access unexpectedly succeeded")
    else:
        print("OK   internal contract probe: normal access failed as expected")

    authorized_source = work_dir / "internal_authorized.cpp"
    authorized_source.write_text(
        '#include "mazda/internal_contracts.hpp"\nint main() { return 0; }\n', encoding="utf-8"
    )
    authorized_dirs = list(include_dirs)
    # The include argument is mazda/internal_contracts.hpp, so the include
    # directory is the parent of the mazda/ directory, not mazda/ itself.
    explicit_private = authorized_internal.parent.parent
    if explicit_private not in authorized_dirs:
        authorized_dirs.append(explicit_private)
    authorized = _compile_translation_unit(
        compiler,
        authorized_source,
        root,
        authorized_dirs,
        work_dir,
        depfile_name="internal_authorized.d",
        msvc=_compiler_is_msvc(compiler),
    )
    if authorized.returncode != 0:
        detail = authorized.output[-3000:] if authorized.output else "compiler failed"
        failures.append(f"authorized internal contract probe failed\n{detail}")
        print("FAIL internal contract probe: authorized access failed")
    else:
        print("OK   internal contract probe: authorized access succeeded (internal include path)")
    return failures


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--core-root", type=Path)
    parser.add_argument("--compiler", help="C++ compiler command (defaults to $CXX or c++)")
    parser.add_argument("--cmake", default="cmake", help="CMake executable (default: cmake)")
    parser.add_argument(
        "--keep-temp",
        action="store_true",
        help="keep generated probe files and print their directory",
    )
    args = parser.parse_args(argv)
    root = args.root.resolve()
    if not root.is_dir():
        parser.error(f"root is not a directory: {root}")
    if shutil.which(args.cmake) is None and not Path(args.cmake).is_file():
        parser.error(f"CMake not found: {args.cmake}")
    try:
        compiler = _compiler_command(args.compiler)
    except (FileNotFoundError, ValueError) as error:
        parser.error(str(error))

    if args.keep_temp:
        temp_context = None
        work_dir = Path(tempfile.mkdtemp(prefix="mazda-header-boundary-"))
    else:
        temp_context = tempfile.TemporaryDirectory(prefix="mazda-header-boundary-")
        work_dir = Path(temp_context.name)
    try:
        print(f"Public header boundary check: {root}")
        if _compiler_is_msvc(compiler):
            failures = _unsupported_msvc_failure()
        else:
            failures = check_public_headers(root, compiler, work_dir, args.core_root)
            failures.extend(
                check_consumer_target(root, args.cmake, compiler, work_dir, args.core_root)
            )
            failures.extend(check_internal_access(root, compiler, work_dir, args.core_root))
        if args.keep_temp:
            print(f"Probe files: {work_dir}")
        if failures:
            print(f"Header boundary check found {len(failures)} violation(s).", file=sys.stderr)
            for failure in failures:
                print(f"ERROR: {failure}", file=sys.stderr)
            return 1
        print("Header boundary check passed.")
        return 0
    finally:
        if temp_context is not None:
            temp_context.cleanup()


if __name__ == "__main__":
    raise SystemExit(main())
