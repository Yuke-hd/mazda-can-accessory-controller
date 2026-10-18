#!/usr/bin/env python3
"""Generate the output catalog document and C++ limits header.

Each sink component owns an ``output_catalog.yaml`` descriptor. This host-only
tool validates the selected descriptors, resolves board values, and writes the
compact JSON catalog served to the companion app together with a data-only
C++17 header that the configuration validator uses in place of literals.

The descriptor format, document layout, header layout and limits are specified
in docs/specs/companion/output-catalog.md.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import tempfile
from pathlib import Path
from typing import Any, Callable, Mapping, Sequence

try:
    import yaml
except ModuleNotFoundError:  # pragma: no cover - exercised through the CLI
    yaml = None  # type: ignore[assignment]


DESCRIPTOR_FORMAT = 1
MAX_DESCRIPTORS = 4
MAX_OUTPUT_TYPES = 32
MAX_FIELDS = 16
MAX_LIST_ENTRIES = 32
MAX_NAME_BYTES = 31
MAX_DOCUMENT_BYTES = 16384
INT32_MIN = -(2**31)
INT32_MAX = 2**31 - 1
NAME_PATTERN = re.compile(r"[a-z][a-z0-9_]*")
BOARD_VALUE_PATTERN = re.compile(r"-?[0-9]+")

# Attributes of each field kind, in catalog document order. ``default`` is the
# only optional attribute and is allowed on integer fields alone.
KIND_ATTRIBUTES: Mapping[str, Sequence[str]] = {
    "action": (),
    "integer": ("min", "max"),
    "enum": ("values",),
    "rgb": ("min", "max"),
    "zone": ("pixels", "directions"),
}
LIST_ATTRIBUTES = {"values", "directions"}
DEFAULT_KINDS = {"integer"}

# Output type names become C++ namespaces in the limits header.
CXX_KEYWORDS = frozenset(
    "alignas alignof and and_eq asm auto bitand bitor bool break case catch char char16_t "
    "char32_t class compl const const_cast constexpr continue decltype default delete do "
    "double dynamic_cast else enum explicit export extern false float for friend goto if "
    "inline int long mutable namespace new noexcept not not_eq nullptr operator or or_eq "
    "private protected public register reinterpret_cast return short signed sizeof static "
    "static_assert static_cast struct switch template this thread_local throw true try "
    "typedef typeid typename union unsigned using virtual void volatile wchar_t while xor "
    "xor_eq".split()
)

HEADER_WIDTH = 80


class CatalogError(Exception):
    """A descriptor or argument error, naming its source and entry path."""

    def __init__(self, source: str, path: str, message: str) -> None:
        super().__init__(f"{source}: {path}: {message}")


def _child(path: str, key: str | int) -> str:
    if isinstance(key, int):
        return f"{path}[{key}]" if path != "$" else f"[{key}]"
    return key if path == "$" else f"{path}.{key}"


class BoardValues:
    """Board values passed with ``--board``, tracking which ones are used."""

    def __init__(self, arguments: Sequence[str]) -> None:
        self.values: dict[str, int] = {}
        self.used: set[str] = set()
        for argument in arguments:
            name, separator, text = argument.partition("=")
            if not separator or not NAME_PATTERN.fullmatch(name) or len(name) > MAX_NAME_BYTES:
                raise CatalogError("--board", argument, "expected name=value with a snake_case name")
            if not BOARD_VALUE_PATTERN.fullmatch(text):
                raise CatalogError("--board", argument, "value must be a decimal integer")
            value = int(text)
            if not INT32_MIN <= value <= INT32_MAX:
                raise CatalogError("--board", argument, "value does not fit in int32")
            if name in self.values:
                raise CatalogError("--board", argument, f"board value '{name}' is given more than once")
            self.values[name] = value

    def check_all_used(self) -> None:
        for name in self.values:
            if name not in self.used:
                raise CatalogError("--board", f"{name}={self.values[name]}", "no descriptor uses this board value")


class DescriptorReader:
    """Validates one descriptor and returns its catalog output entries."""

    def __init__(self, source: str, board: BoardValues) -> None:
        self.source = source
        self.board = board

    def fail(self, path: str, message: str) -> CatalogError:
        return CatalogError(self.source, path, message)

    def mapping(self, value: Any, path: str, required: Sequence[str], optional: Sequence[str] = ()) -> dict:
        if not isinstance(value, dict):
            raise self.fail(path, "expected a mapping")
        for key in value:
            if key not in required and key not in optional:
                raise self.fail(_child(path, key), "unknown key")
        for key in required:
            if key not in value:
                raise self.fail(_child(path, key), "missing required key")
        return value

    def name(self, value: Any, path: str) -> str:
        if not isinstance(value, str) or not NAME_PATTERN.fullmatch(value):
            raise self.fail(path, "expected a snake_case ASCII name ([a-z][a-z0-9_]*)")
        if len(value) > MAX_NAME_BYTES:
            raise self.fail(path, f"name is longer than {MAX_NAME_BYTES} bytes")
        return value

    def name_list(self, value: Any, path: str) -> list[str]:
        if not isinstance(value, list):
            raise self.fail(path, "expected a list")
        if not 1 <= len(value) <= MAX_LIST_ENTRIES:
            raise self.fail(path, f"expected 1 to {MAX_LIST_ENTRIES} entries")
        names: list[str] = []
        for index, entry in enumerate(value):
            entry_path = _child(path, index)
            name = self.name(entry, entry_path)
            if name in names:
                raise self.fail(entry_path, f"duplicate entry '{name}'")
            names.append(name)
        return names

    def integer(self, value: Any, path: str) -> int:
        if isinstance(value, dict):
            reference = self.mapping(value, path, ("board",))
            board_name = reference["board"]
            if not isinstance(board_name, str) or not NAME_PATTERN.fullmatch(board_name):
                raise self.fail(path, "board reference must name a snake_case board value")
            if board_name not in self.board.values:
                raise self.fail(path, f"unresolved board value '{board_name}'; pass --board {board_name}=<value>")
            self.board.used.add(board_name)
            return self.board.values[board_name]
        if isinstance(value, bool) or not isinstance(value, int):
            raise self.fail(path, "expected an integer or {board: <name>}")
        if not INT32_MIN <= value <= INT32_MAX:
            raise self.fail(path, "integer does not fit in int32")
        return value

    def read(self, document: Any) -> list[dict[str, Any]]:
        top = self.mapping(document, "$", ("descriptor_format", "sink", "outputs"))
        descriptor_format = top["descriptor_format"]
        if isinstance(descriptor_format, bool) or descriptor_format != DESCRIPTOR_FORMAT:
            raise self.fail("descriptor_format", f"unsupported descriptor format; expected {DESCRIPTOR_FORMAT}")
        sink = self.name(top["sink"], "sink")
        outputs = top["outputs"]
        if not isinstance(outputs, list):
            raise self.fail("outputs", "expected a list")
        return [self.output(entry, _child("outputs", index), sink) for index, entry in enumerate(outputs)]

    def output(self, value: Any, path: str, sink: str) -> dict[str, Any]:
        entry = self.mapping(value, path, ("type", "fields"), ("rule_types",))
        type_path = _child(path, "type")
        output_type = self.name(entry["type"], type_path)
        if output_type in CXX_KEYWORDS:
            raise self.fail(type_path, f"'{output_type}' is a C++ keyword and cannot name a header namespace")
        result: dict[str, Any] = {"type": output_type, "sink": sink}
        if "rule_types" in entry:
            result["rule_types"] = self.name_list(entry["rule_types"], _child(path, "rule_types"))
        fields_path = _child(path, "fields")
        fields = entry["fields"]
        if not isinstance(fields, list):
            raise self.fail(fields_path, "expected a list")
        if len(fields) > MAX_FIELDS:
            raise self.fail(fields_path, f"more than {MAX_FIELDS} fields")
        result["fields"] = []
        for index, field in enumerate(fields):
            field_path = _child(fields_path, index)
            parsed = self.field(field, field_path)
            if any(existing["name"] == parsed["name"] for existing in result["fields"]):
                raise self.fail(_child(field_path, "name"), f"duplicate field name '{parsed['name']}'")
            result["fields"].append(parsed)
        return result

    def field(self, value: Any, path: str) -> dict[str, Any]:
        if not isinstance(value, dict):
            raise self.fail(path, "expected a mapping")
        if "name" not in value:
            raise self.fail(_child(path, "name"), "missing required key")
        name = self.name(value["name"], _child(path, "name"))
        if "kind" not in value:
            raise self.fail(_child(path, "kind"), "missing required key")
        kind = value["kind"]
        if not isinstance(kind, str) or kind not in KIND_ATTRIBUTES:
            raise self.fail(_child(path, "kind"), f"unknown field kind; expected one of {', '.join(KIND_ATTRIBUTES)}")
        attributes = KIND_ATTRIBUTES[kind]
        optional = ("default",) if kind in DEFAULT_KINDS else ()
        self.mapping(value, path, ("name", "kind", "required", *attributes), optional)
        required = value["required"]
        if not isinstance(required, bool):
            raise self.fail(_child(path, "required"), "expected true or false")

        result: dict[str, Any] = {"name": name, "kind": kind, "required": required}
        for attribute in attributes:
            attribute_path = _child(path, attribute)
            if attribute in LIST_ATTRIBUTES:
                result[attribute] = self.name_list(value[attribute], attribute_path)
            else:
                result[attribute] = self.integer(value[attribute], attribute_path)
        if "min" in result and result["min"] > result["max"]:
            raise self.fail(_child(path, "min"), "min is greater than max")
        if "pixels" in result and result["pixels"] < 1:
            raise self.fail(_child(path, "pixels"), "a zone needs at least one pixel")

        default_path = _child(path, "default")
        if required and "default" in value:
            raise self.fail(default_path, "a required field cannot have a default")
        if not required:
            if kind not in DEFAULT_KINDS:
                raise self.fail(default_path, f"an optional field needs a default, which a {kind} field cannot have")
            if "default" not in value:
                raise self.fail(default_path, "an optional field needs a default")
            default = self.integer(value["default"], default_path)
            if not result["min"] <= default <= result["max"]:
                raise self.fail(default_path, "default is outside min and max")
            result["default"] = default
        return result


def _yaml_loader() -> Any:
    class UniqueKeyLoader(yaml.SafeLoader):  # type: ignore[misc,union-attr]
        """SafeLoader that rejects duplicate keys and YAML 1.1 yes/no booleans."""

    bool_tag = "tag:yaml.org,2002:bool"
    UniqueKeyLoader.yaml_implicit_resolvers = {
        key: [(tag, pattern) for tag, pattern in resolvers if tag != bool_tag]
        for key, resolvers in yaml.SafeLoader.yaml_implicit_resolvers.items()  # type: ignore[union-attr]
    }
    UniqueKeyLoader.add_implicit_resolver(
        bool_tag, re.compile(r"^(?:true|True|TRUE|false|False|FALSE)$"), list("tTfF"))

    def construct_mapping(loader: Any, node: Any, deep: bool = False) -> dict[Any, Any]:
        mapping: dict[Any, Any] = {}
        for key_node, value_node in node.value:
            key = loader.construct_object(key_node, deep=deep)
            if key in mapping:
                raise yaml.constructor.ConstructorError(  # type: ignore[union-attr]
                    "while constructing a mapping", node.start_mark, f"duplicate key '{key}'", key_node.start_mark)
            mapping[key] = loader.construct_object(value_node, deep=deep)
        return mapping

    UniqueKeyLoader.add_constructor(
        yaml.resolver.BaseResolver.DEFAULT_MAPPING_TAG, construct_mapping)  # type: ignore[union-attr]
    return UniqueKeyLoader


def load_descriptor(path: str) -> Any:
    if yaml is None:
        raise CatalogError(path, "$", "PyYAML is required; install tools/requirements.txt")
    try:
        source = Path(path).read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        raise CatalogError(path, "$", f"cannot read descriptor: {error}") from error
    try:
        return yaml.load(source, Loader=_yaml_loader())
    except yaml.YAMLError as error:
        raise CatalogError(path, "$", f"malformed YAML: {error}") from error


def build_catalog(descriptors: Sequence[str], board_arguments: Sequence[str]) -> list[dict[str, Any]]:
    if len(descriptors) > MAX_DESCRIPTORS:
        raise CatalogError("--descriptor", "$", f"at most {MAX_DESCRIPTORS} descriptors are allowed")
    board = BoardValues(board_arguments)
    outputs: list[dict[str, Any]] = []
    owners: dict[str, tuple[str, str]] = {}
    for descriptor in descriptors:
        entries = DescriptorReader(descriptor, board).read(load_descriptor(descriptor))
        for index, entry in enumerate(entries):
            path = _child("outputs", index)
            if len(outputs) == MAX_OUTPUT_TYPES:
                raise CatalogError(descriptor, path, f"more than {MAX_OUTPUT_TYPES} output types in the build")
            if entry["type"] in owners:
                first_source, first_path = owners[entry["type"]]
                raise CatalogError(
                    descriptor, _child(path, "type"),
                    f"output type '{entry['type']}' is already defined at {first_source}: {first_path}")
            owners[entry["type"]] = (descriptor, _child(path, "type"))
            outputs.append(entry)
    board.check_all_used()
    return outputs


def render_catalog(outputs: Sequence[Mapping[str, Any]], descriptors: Sequence[str]) -> bytes:
    document = json.dumps({"outputs": list(outputs)}, separators=(",", ":"), ensure_ascii=True).encode("utf-8")
    if not 1 <= len(document) <= MAX_DOCUMENT_BYTES:
        raise CatalogError(", ".join(descriptors), "$",
                           f"catalog document is {len(document)} bytes; the limit is {MAX_DOCUMENT_BYTES}")
    return document


def _pascal(name: str) -> str:
    return "".join(part[:1].upper() + part[1:] for part in name.split("_"))


def _string_array(name: str, values: Sequence[str]) -> list[str]:
    declaration = f"inline constexpr std::array<std::string_view, {len(values)}> {name}{{"
    if not values:
        return [declaration + "};"]
    lines = [declaration]
    line = "   "
    for index, value in enumerate(values):
        item = f' "{value}"' + ("," if index + 1 < len(values) else "};")
        if len(line) + len(item) > HEADER_WIDTH and line.strip():
            lines.append(line)
            line = "   "
        line += item
    lines.append(line)
    return lines


def render_header(outputs: Sequence[Mapping[str, Any]],
                  on_duplicate: Callable[[str, str], CatalogError]) -> bytes:
    lines = [
        "// Generated by tools/generate_output_catalog.py from the output catalog",
        "// descriptors. Do not edit; see docs/specs/companion/output-catalog.md.",
        "#pragma once",
        "",
        "#include <array>",
        "#include <cstdint>",
        "#include <string_view>",
        "",
        "namespace controller_config::generated {",
        "",
        *_string_array("kOutputTypeNames", [output["type"] for output in outputs]),
        "",
    ]
    for output in outputs:
        body: list[str] = []
        constants: set[str] = set()

        def declare(constant: str, declaration: list[str]) -> None:
            if constant in constants:
                raise on_duplicate(output["type"], constant)
            constants.add(constant)
            body.extend(declaration)

        if "rule_types" in output:
            declare("kRuleTypes", _string_array("kRuleTypes", output["rule_types"]))
        for field in output["fields"]:
            prefix = "k" + _pascal(field["name"])
            for attribute in (*KIND_ATTRIBUTES[field["kind"]], "default"):
                if attribute not in field:
                    continue
                constant = prefix + _pascal(attribute)
                if attribute in LIST_ATTRIBUTES:
                    declare(constant, _string_array(constant, field[attribute]))
                else:
                    declare(constant, [f"inline constexpr std::int32_t {constant} = {field[attribute]};"])
        lines += [f"namespace {output['type']} {{", *body, f"}} // namespace {output['type']}", ""]
    lines += ["} // namespace controller_config::generated", ""]
    return "\n".join(lines).encode("utf-8")


def _write_all(files: Sequence[tuple[Path, bytes]]) -> None:
    """Write every file, or none of them if any temporary write fails."""
    staged: list[tuple[str, Path]] = []
    try:
        for path, content in files:
            path.parent.mkdir(parents=True, exist_ok=True)
            handle, temporary = tempfile.mkstemp(dir=path.parent, prefix=f".{path.name}.")
            staged.append((temporary, path))
            with os.fdopen(handle, "wb") as stream:
                stream.write(content)
            # mkstemp creates 0600 files; generated build inputs are ordinary files.
            os.chmod(temporary, 0o644)
        for temporary, path in staged:
            os.replace(temporary, path)
    finally:
        for temporary, _ in staged:
            if os.path.exists(temporary):
                os.unlink(temporary)


def generate(descriptors: Sequence[str], board: Sequence[str]) -> tuple[bytes, bytes]:
    outputs = build_catalog(descriptors, board)
    catalog = render_catalog(outputs, descriptors)

    def duplicate_constant(output_type: str, constant: str) -> CatalogError:
        index = next(i for i, output in enumerate(outputs) if output["type"] == output_type)
        return CatalogError(", ".join(descriptors), f"outputs[{index}]",
                            f"field names of '{output_type}' produce the header constant {constant} twice")

    return catalog, render_header(outputs, duplicate_constant)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--descriptor", action="append", default=[], help="sink descriptor YAML; may repeat")
    parser.add_argument("--board", action="append", default=[], metavar="NAME=VALUE",
                        help="board value referenced by descriptors; may repeat")
    parser.add_argument("--catalog", required=True, type=Path, help="output catalog JSON path")
    parser.add_argument("--limits-header", required=True, type=Path, help="output C++ limits header path")
    arguments = parser.parse_args(argv)
    try:
        catalog, header = generate(arguments.descriptor, arguments.board)
    except CatalogError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    _write_all([(arguments.catalog, catalog), (arguments.limits_header, header)])
    return 0


if __name__ == "__main__":
    sys.exit(main())
