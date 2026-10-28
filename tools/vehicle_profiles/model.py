"""Version-one normalized profile records (host tools only).

Numeric source lexemes remain strings. Mechanical and semantic validation is a
separate stage; these records do not claim that an imported field is executable.
"""

from typing import TypedDict

FORMAT = "vehicle-signal-profile"
VERSION = 1


class Source(TypedDict):
    id: str
    kind: str
    path: str
    sha256: str
    revision: str | None
    license: str
    bus: str


class Value(TypedDict):
    raw: str
    label: str


class Field(TypedDict):
    id: str
    name: str
    sources: list[str]
    start: int
    length: int
    byte_order: str
    signed: bool
    scale: str
    offset: str
    minimum: str
    maximum: str
    unit: str
    value_table: list[Value]


class Message(TypedDict):
    id: str
    bus: str
    identifier_format: str
    identifier: int
    dlc: int
    fields: list[Field]


class Choice(TypedDict):
    key: str
    value: int


class Evidence(TypedDict):
    ref: str
    confidence: str
    eligibility: str


class Freshness(TypedDict):
    timeout_ms: int
    basis: str
    evidence_ref: str


class InvalidRange(TypedDict):
    minimum: str
    maximum: str


class Conversion(TypedDict):
    source_unit: str
    target_unit: str
    scale: str
    offset: str
    target_precision: str
    invalid_ranges: list[InvalidRange]


class EnumMapping(TypedDict):
    raw: str
    choice: str


class NativePolicy(TypedDict):
    handler: str
    revision: str
    invalid_value: str


class DbcPolicy(TypedDict):
    conversion: Conversion
    enum_map: list[EnumMapping]
    invalid_value: str


class Binding(TypedDict):
    key: str
    runtime_id: int
    type: str
    unit: str
    capabilities: list[str]
    choices: list[Choice]
    kind: str
    inputs: list[str]
    policy: NativePolicy | DbcPolicy
    evidence: Evidence
    freshness: Freshness | None


class CapabilityDiagnostic(TypedDict):
    category: str
    affected: str
    sources: list[str]
    reason: str


class Profile(TypedDict):
    format: str
    version: int
    profile_id: str
    semantic_policy_revision: str
    recognition_rule_revision: str
    selected_bus: str
    sources: list[Source]
    messages: list[Message]
    bindings: list[Binding]
    diagnostics: list[CapabilityDiagnostic]
    compatibility_identity: str
    artifact_identity: str
