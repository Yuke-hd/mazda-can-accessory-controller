#!/usr/bin/env python3
"""Check the tools required by the supported host and firmware builds.

This checker intentionally does not install anything.  It is safe to run in
CI and before a build, and reports every missing or incompatible executable in
one pass so that a developer can fix the environment without guesswork.
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, FrozenSet, List, Optional, Sequence, Tuple


Version = Tuple[int, ...]


# Keep this policy in one place so checker output, documentation, and CI use
# the same supported host-CMake floor. CMake 4.4 is the modern compatibility
# leg exercised by CI; newer versions remain valid when they preserve the
# project's declared policy range.
SUPPORTED_HOST_CMAKE_MINIMUM: Version = (3, 20)
SUPPORTED_HOST_CMAKE_MODERN: Version = (4, 4)


@dataclass(frozen=True)
class Requirement:
    name: str
    executable: str
    arguments: Tuple[str, ...]
    scopes: FrozenSet[str]
    check: Callable[[str], Tuple[bool, str]]
    install_hint: str


_VERSION_RE = re.compile(
    r"(?<!\d)(?P<version>\d+(?:\.\d+)+)(?P<suffix>[-+A-Za-z][A-Za-z0-9.-]*)?"
)


def _version_details(text: str) -> Optional[Tuple[Version, str]]:
    match = _VERSION_RE.search(text)
    if match is None:
        return None
    version = tuple(int(part) for part in match.group("version").split("."))
    return version, match.group("suffix") or ""


def _version(text: str) -> Optional[Version]:
    details = _version_details(text)
    return details[0] if details is not None else None


def _at_least(expected: Version) -> Callable[[str], Tuple[bool, str]]:
    def check(output: str) -> Tuple[bool, str]:
        actual = _version(output)
        if actual is None:
            return False, f"could not parse a version (need >= {'.'.join(map(str, expected))})"
        return actual >= expected, f"reported {'.'.join(map(str, actual))}; need >= {'.'.join(map(str, expected))}"

    return check


def _exact(expected: Version) -> Callable[[str], Tuple[bool, str]]:
    def check(output: str) -> Tuple[bool, str]:
        details = _version_details(output)
        expected_text = ".".join(map(str, expected))
        if details is None:
            return False, f"could not parse a version (need exactly {expected_text})"
        actual, suffix = details
        actual_text = ".".join(map(str, actual)) + suffix
        valid = actual == expected and not suffix
        return valid, f"reported {actual_text}; need exactly {expected_text}"

    return check


def _major_exact(expected: int) -> Callable[[str], Tuple[bool, str]]:
    def check(output: str) -> Tuple[bool, str]:
        actual = _version(output)
        if actual is None:
            return False, f"could not parse a version (need major version {expected})"
        return actual[0] == expected, f"reported {'.'.join(map(str, actual))}; need major version {expected}"

    return check


def _present(output: str) -> Tuple[bool, str]:
    return True, output.strip().splitlines()[0] if output.strip() else "executable responded"


REQUIREMENTS = (
    Requirement("Git", "git", ("--version",), frozenset({"host", "firmware"}), _present, "https://git-scm.com/book/en/v2/Getting-Started-Installing-Git"),
    Requirement("Bash", "bash", ("--version",), frozenset({"host", "firmware"}), _present, "https://www.gnu.org/software/bash/"),
    Requirement(
        "CMake",
        "cmake",
        ("--version",),
        frozenset({"host"}),
        _at_least(SUPPORTED_HOST_CMAKE_MINIMUM),
        "https://cmake.org/download/",
    ),
    Requirement("Ninja", "ninja", ("--version",), frozenset({"host"}), _present, "https://ninja-build.org/"),
    Requirement("C++ compiler", "c++", ("--version",), frozenset({"host"}), _present, "https://gcc.gnu.org/install/"),
    Requirement("clang-format", "clang-format-14", ("--version",), frozenset({"host"}), _major_exact(14), "https://clang.llvm.org/docs/ClangFormat.html"),
    Requirement("Python", "python3", ("--version",), frozenset({"host", "firmware"}), _at_least((3, 8)), "https://www.python.org/downloads/"),
    Requirement("ripgrep", "rg", ("--version",), frozenset({"host"}), _present, "https://github.com/BurntSushi/ripgrep#installation"),
    Requirement("ESP-IDF", "idf.py", ("--version",), frozenset({"firmware"}), _exact((5, 5, 4)), "https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32/get-started/"),
)


def _run(executable: str, arguments: Sequence[str]) -> Tuple[int, str]:
    try:
        result = subprocess.run(
            [executable, *arguments],
            check=False,
            capture_output=True,
            text=True,
            timeout=15,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        return 127, str(error)
    return result.returncode, f"{result.stdout}\n{result.stderr}".strip()


def _executable_candidates(executable: str) -> Tuple[str, ...]:
    """Return the active Python environment and PATH tool locations.

    A virtual environment can be used without activating it by invoking its
    Python directly (for example, ``.ci-venv/bin/python checker.py``). In that
    mode Python packages install their console scripts next to the interpreter,
    while PATH still points at the caller's shell. Prefer those locations when
    a virtual environment is explicit; for system Python, preserve PATH
    precedence.
    """

    path_entry = shutil.which(executable)
    interpreter_dir = Path(sys.executable).resolve().parent
    environment_dirs = (interpreter_dir, Path(sys.prefix) / "bin", Path(sys.prefix) / "Scripts")
    is_virtual_environment = sys.prefix != getattr(sys, "base_prefix", sys.prefix) or hasattr(sys, "real_prefix")
    candidates: List[str] = []

    def add_directory_tools(directories: Tuple[Path, ...]) -> None:
        for directory in directories:
            candidate = directory / executable
            if candidate.is_file() and candidate.stat().st_mode & 0o111:
                candidate_text = str(candidate)
                if candidate_text not in candidates:
                    candidates.append(candidate_text)

    if is_virtual_environment:
        add_directory_tools(environment_dirs)
    if path_entry is not None and path_entry not in candidates:
        candidates.append(path_entry)
    if not is_virtual_environment:
        add_directory_tools(environment_dirs)
    return tuple(candidates)


def _find_executable(executable: str) -> Optional[str]:
    candidates = _executable_candidates(executable)
    return candidates[0] if candidates else None


def check(scope: str) -> int:
    failures = 0
    print(f"MCAN toolchain check: {scope}")
    for requirement in REQUIREMENTS:
        if scope not in requirement.scopes:
            continue
        path = _find_executable(requirement.executable)
        if path is None:
            print(f"FAIL {requirement.name}: '{requirement.executable}' not found")
            print(f"     Install: {requirement.install_hint}")
            failures += 1
            continue
        return_code, output = _run(path, requirement.arguments)
        if return_code != 0:
            print(f"FAIL {requirement.name}: {output or 'command failed'}")
            print(f"     Install: {requirement.install_hint}")
            failures += 1
            continue
        valid, detail = requirement.check(output)
        marker = "OK" if valid else "FAIL"
        print(f"{marker}   {requirement.name}: {detail}")
        if not valid:
            print(f"     Install: {requirement.install_hint}")
            failures += 1
    if failures:
        print(f"Toolchain check failed: {failures} requirement(s) need attention.", file=sys.stderr)
        return 1
    print("Toolchain check passed.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--scope",
        choices=("host", "firmware", "all"),
        default="all",
        help="requirements to check (default: all)",
    )
    args = parser.parse_args()
    scopes = ("host", "firmware") if args.scope == "all" else (args.scope,)
    return max(check(scope) for scope in scopes)


if __name__ == "__main__":
    raise SystemExit(main())
