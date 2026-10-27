"""Immutable generated sets, published through one atomic generation index."""

import hashlib
import json
import os
from pathlib import Path
import shutil
import tempfile
from typing import Callable

from .projections import ProfileProjections

OUTPUT_NAMES = ("controller_signal_catalog.json", "vehicle_profile_metadata.hpp",
                "capability_report.json")
INDEX_NAME = "current.json"


def json_bytes(value: object) -> bytes:
    return (json.dumps(value, sort_keys=True, separators=(",", ":"),
                       ensure_ascii=True, allow_nan=False) + "\n").encode()


def contents(projections: ProfileProjections, inputs: dict[str, str]) -> dict[str, bytes]:
    """Materialize and fingerprint the complete set before touching output files."""
    outputs = dict(zip(OUTPUT_NAMES, (projections.compiler_catalog,
                                     projections.metadata_header,
                                     projections.capability_report)))
    outputs["artifact_set.json"] = json_bytes({
        "format": "vehicle-profile-artifact-set-v1", "inputs": inputs,
        "outputs": {name: hashlib.sha256(data).hexdigest()
                    for name, data in outputs.items()}})
    return outputs


def generation(outputs: dict[str, bytes]) -> str:
    return hashlib.sha256(json_bytes({name: hashlib.sha256(data).hexdigest()
                                     for name, data in outputs.items()})).hexdigest()


def _write(path: Path, data: bytes) -> None:
    with path.open("xb") as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())


def _matches(directory: Path, outputs: dict[str, bytes]) -> bool:
    try:
        return (directory.is_dir() and not directory.is_symlink()
                and {p.name for p in directory.iterdir()} == set(outputs)
                and all(not (directory / name).is_symlink()
                        and (directory / name).read_bytes() == data
                        for name, data in outputs.items()))
    except OSError:
        return False


def _index(digest: str) -> bytes:
    return json_bytes({"format": "vehicle-profile-current-v1",
                       "directory": "sets/" + digest})


def check(root: Path, outputs: dict[str, bytes]) -> bool:
    """Non-mutating byte check, including missing/malformed/corrupt old sets."""
    digest = generation(outputs)
    try:
        return ((root / INDEX_NAME).read_bytes() == _index(digest)
                and _matches(root / "sets" / digest, outputs))
    except OSError:
        return False


def publish(root: Path, outputs: dict[str, bytes], *,
            write: Callable[[Path, bytes], None] = _write) -> Path:
    """Publish a whole set or leave the previous index and readers unchanged.

    Old immutable directories are deliberately retained. Consumers resolve the
    returned path once, rather than reading multiple files through the index.
    Concurrent writers are last-successful-index-wins; every index names a full
    verified generation. The writer seam is for late-output fault injection.
    """
    digest = generation(outputs)
    if check(root, outputs):
        return root / "sets" / digest
    sets = root / "sets"
    sets.mkdir(parents=True, exist_ok=True)
    destination = sets / digest
    staging = Path(tempfile.mkdtemp(prefix=".staging-", dir=sets))
    pointer: Path | None = None
    try:
        for name, data in outputs.items():
            if Path(name).name != name or name in (".", ".."):
                raise ValueError("artifact filenames must be plain basenames")
            write(staging / name, data)
        if not _matches(staging, outputs):
            raise OSError("staged artifact set failed byte verification")
        try:
            os.rename(staging, destination)
        except OSError as error:
            # Another successful writer can have published this same immutable
            # generation. A corrupt existing directory must never be reused.
            if not _matches(destination, outputs):
                raise OSError("existing generation is corrupt; use a fresh output root after investigating corruption") from error
        descriptor, name = tempfile.mkstemp(prefix=".current-", dir=root)
        os.close(descriptor)
        pointer = Path(name)
        pointer.unlink()
        _write(pointer, _index(digest))
        os.replace(pointer, root / INDEX_NAME)
        return destination
    finally:
        shutil.rmtree(staging, ignore_errors=True)
        if pointer is not None:
            pointer.unlink(missing_ok=True)
