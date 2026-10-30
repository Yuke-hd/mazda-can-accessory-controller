"""Generate allocation-free C++ decoders from checked normalized profiles."""

from collections import defaultdict
from fractions import Fraction
import math
from types import MappingProxyType
from typing import Any

from .validation import ProfileValidationError

RUNTIME_LIMITS = MappingProxyType({
    "max_selected_messages": 64,
    "max_extracted_fields": 64,
    "max_runtime_bindings": 64,
    "max_runtime_notify_bindings": 17,
})


def _fail(category: str, path: str, reason: str) -> None:
    raise ProfileValidationError(category, path, reason)


def _ceil(value: Fraction) -> int:
    return -(-value.numerator // value.denominator)


def _raw_limits(field: dict[str, Any]) -> tuple[int, int]:
    width = field["length"]
    if field["signed"]:
        return -(1 << (width - 1)), (1 << (width - 1)) - 1
    return 0, (1 << width) - 1


def _raw_interval(field: dict[str, Any], minimum: Fraction,
                  maximum: Fraction) -> tuple[int, int] | None:
    """Invert an inclusive physical interval into exact representable raw values."""
    scale = Fraction(field["scale"])
    offset = Fraction(field["offset"])
    endpoints = ((minimum - offset) / scale, (maximum - offset) / scale)
    lower = _ceil(min(endpoints))
    upper = max(endpoints).numerator // max(endpoints).denominator
    raw_minimum, raw_maximum = _raw_limits(field)
    lower = max(lower, raw_minimum)
    upper = min(upper, raw_maximum)
    return (lower, upper) if lower <= upper else None


def _permitted_interval(field: dict[str, Any]) -> tuple[int, int]:
    interval = _raw_interval(field, Fraction(field["minimum"]), Fraction(field["maximum"]))
    if interval is None:
        _fail("invalid_runtime_range", "$.messages", "field has no representable raw value in its declared bounds")
    return interval


def _invalid_intervals(binding: dict[str, Any], field: dict[str, Any],
                       permitted: tuple[int, int]) -> list[tuple[int, int]]:
    intervals = []
    for invalid in binding["policy"]["conversion"]["invalid_ranges"]:
        interval = _raw_interval(field, Fraction(invalid["minimum"]), Fraction(invalid["maximum"]))
        if interval is None:
            continue
        lower = max(interval[0], permitted[0])
        upper = min(interval[1], permitted[1])
        if lower <= upper:
            intervals.append((lower, upper))

    merged: list[list[int]] = []
    for lower, upper in sorted(intervals):
        if merged and lower <= merged[-1][1] + 1:
            merged[-1][1] = max(merged[-1][1], upper)
        else:
            merged.append([lower, upper])
    return [(lower, upper) for lower, upper in merged]


def _bits(raw: int, width: int) -> int:
    return raw & ((1 << width) - 1)


def _raw_value(raw: int, field: dict[str, Any]) -> str:
    width = field["length"]
    signed = "true" if field["signed"] else "false"
    return (f"vehicle_profile_runtime::RawValue{{UINT64_C(0x{_bits(raw, width):016x}), "
            f"{width}U, {signed}}}")


def _raw_range(lower: int, upper: int, field: dict[str, Any]) -> str:
    return ("vehicle_profile_runtime::RawRange{" + _raw_value(lower, field) + ", " +
            _raw_value(upper, field) + "}")


def _extended(value: Fraction) -> str:
    """Emit a two-limb binary constant for a validated exact decimal value.

    Each limb is an exactly representable binary64 value. The runtime adds and
    multiplies these as an ExtendedValue, preserving cancellation without any
    decimal parsing or allocation in firmware.
    """
    try:
        high = float(value)
    except OverflowError:
        _fail("invalid_runtime_conversion", "$.bindings", "affine constant is outside the finite binary64 range")
    if not math.isfinite(high):
        _fail("invalid_runtime_conversion", "$.bindings", "affine constant is not finite")
    residual = value - Fraction.from_float(high)
    low = float(residual)
    if not math.isfinite(low):
        _fail("invalid_runtime_conversion", "$.bindings", "affine residual is not finite")
    return f"vehicle_profile_runtime::ExtendedValue{{{high.hex()}L, {low.hex()}L}}"


def _type_check(profile: dict[str, Any]) -> tuple[dict[str, tuple[dict[str, Any], dict[str, Any]]], dict[str, dict[str, Any]]]:
    fields: dict[str, tuple[dict[str, Any], dict[str, Any]]] = {}
    messages: dict[str, dict[str, Any]] = {}
    for message in profile["messages"]:
        if message["bus"] != profile["selected_bus"]:
            continue
        if (message["identifier_format"] != "standard" or
                type(message["identifier"]) is not int or not 0 <= message["identifier"] <= 0x7FF or
                type(message["dlc"]) is not int or not 0 <= message["dlc"] <= 8):
            _fail("unsupported_selected_traffic", "$.messages", "generated runtime supports standard classic CAN data frames only")
        messages[message["id"]] = message
        for field in message["fields"]:
            fields[field["id"]] = (message, field)
    for binding in profile["bindings"]:
        if binding["kind"] != "dbc":
            _fail("unsupported_runtime_binding", "$.bindings", "generated runtime does not support native handlers")
    return fields, messages


def _binding_conversion(binding: dict[str, Any], field: dict[str, Any],
                        pivot: int) -> tuple[Fraction, Fraction]:
    field_scale = Fraction(field["scale"])
    field_offset = Fraction(field["offset"])
    policy = binding["policy"]["conversion"]
    target_scale = field_scale * Fraction(policy["scale"])
    target_offset = field_offset * Fraction(policy["scale"]) + Fraction(policy["offset"])
    target_at_pivot = pivot * target_scale + target_offset
    return target_scale, target_at_pivot


def _conversion_pivot(binding: dict[str, Any], field: dict[str, Any],
                      permitted: tuple[int, int]) -> int:
    """Choose the permitted integer raw value nearest the target affine zero."""
    scale, target_offset = _binding_conversion(binding, field, 0)
    if scale == 0:
        return permitted[0]
    zero = round(-target_offset / scale)
    return min(permitted[1], max(permitted[0], zero))


def _array(name: str, element_type: str, items: list[str]) -> list[str]:
    lines = [f"inline constexpr std::array<{element_type}, {len(items)}> {name}{{{{"]
    lines.extend(f"  {item}," for item in items)
    lines.append("}};")
    return lines


def _binding_code(index: int, binding: dict[str, Any], field: dict[str, Any]) -> list[str]:
    permitted = _permitted_interval(field)
    invalid = _invalid_intervals(binding, field, permitted)
    ranges = [_raw_range(lower, upper, field) for lower, upper in invalid]
    lines = [f"inline constexpr auto kPermitted{index} = {_raw_range(*permitted, field)};"]
    lines += _array(f"kInvalidRanges{index}", "vehicle_profile_runtime::RawRange", ranges)

    if binding["type"] in ("number", "boolean"):
        pivot = _conversion_pivot(binding, field, permitted)
        scale, target_at_pivot = _binding_conversion(binding, field, pivot)
        lines.append(
            f"inline constexpr auto kConversion{index} = vehicle_profile_runtime::NumberConversion{{"
            f"{_raw_value(pivot, field)}, {_extended(scale)}, {_extended(target_at_pivot)}}};")
    elif binding["type"] == "enum":
        choices = {choice["key"]: choice["value"] for choice in binding["choices"]}
        mappings = []
        for entry in sorted(binding["policy"]["enum_map"], key=lambda item: int(item["raw"])):
            raw = int(entry["raw"])
            mappings.append(
                f"vehicle_profile_runtime::EnumMapping{{UINT64_C(0x{_bits(raw, field['length']):016x}), "
                f"{choices[entry['choice']]}U}}")
        lines += _array(f"kEnumMappings{index}", "vehicle_profile_runtime::EnumMapping", mappings)
    else:
        _fail("unsupported_runtime_binding", "$.bindings", f"unsupported direct binding type {binding['type']!r}")

    lines.append("")
    return lines


def _decoder_code(function_index: int, message: dict[str, Any],
                  bindings: list[tuple[int, dict[str, Any], dict[str, Any]]]) -> list[str]:
    lines = [
        f"inline void decode_message_{function_index}(const vehicle_core::RawCanFrame &frame,",
        "                               vehicle_profile_runtime::DecodeResult *results,",
        "                               std::size_t result_capacity, std::size_t &result_count) {",
    ]
    for index, binding, field in bindings:
        signal_id = binding["runtime_id"]
        order = "Intel" if field["byte_order"] == "intel" else "Motorola"
        signed = "true" if field["signed"] else "false"
        lines += [
            "  {",
            "    vehicle_profile_runtime::RawValue raw{};",
            f"    bool valid = vehicle_profile_runtime::extract_raw_value(frame, {field['start']}U, "
            f"{field['length']}U, vehicle_profile_runtime::ByteOrder::{order}, {signed}, raw);",
            "    vehicle_signals::SignalValue value{};",
        ]
        if binding["type"] == "number":
            lines += [
                "    float converted = 0.0F;",
                f"    valid = valid && vehicle_profile_runtime::convert_number(raw, kConversion{index}, "
                f"kPermitted{index}, kInvalidRanges{index}.data(), kInvalidRanges{index}.size(), converted);",
                "    value = vehicle_signals::SignalValue::number(converted);",
            ]
        elif binding["type"] == "boolean":
            lines += [
                "    bool converted = false;",
                f"    valid = valid && vehicle_profile_runtime::convert_boolean(raw, kConversion{index}, "
                f"kPermitted{index}, kInvalidRanges{index}.data(), kInvalidRanges{index}.size(), converted);",
                "    value = vehicle_signals::SignalValue::boolean(converted);",
            ]
        else:
            lines += [
                "    vehicle_signals::SignalValue converted{};",
                f"    valid = valid && vehicle_profile_runtime::map_enum(raw, kPermitted{index}, "
                f"kInvalidRanges{index}.data(), kInvalidRanges{index}.size(), "
                f"kEnumMappings{index}.data(), kEnumMappings{index}.size(), converted);",
                "    value = converted;",
            ]
        lines += [
            f"    const auto id = vehicle_signals::SignalId{{{signal_id}U}};",
            "    const auto result = valid ? vehicle_profile_runtime::DecodeResult::success(id, value)"
            ": vehicle_profile_runtime::DecodeResult::invalid(id);",
            "    if (!vehicle_profile_runtime::append_result(results, result_capacity, result_count, result)) {",
            "      return;",
            "    }",
            "  }",
        ]
    lines += ["}", ""]
    return lines


def generate_runtime_header(profile: dict[str, Any]) -> bytes:
    """Project an identified, validated profile into static decoder data."""
    fields, messages_by_id = _type_check(profile)
    bindings_by_message: dict[str, list[tuple[int, dict[str, Any], dict[str, Any]]]] = defaultdict(list)
    used_fields: set[str] = set()
    for index, binding in enumerate(sorted(profile["bindings"], key=lambda item: item["runtime_id"])):
        if len(binding["inputs"]) != 1:
            _fail("unsupported_runtime_binding", "$.bindings", "DBC bindings require exactly one field")
        field_id = binding["inputs"][0]
        if field_id not in fields:
            _fail("invalid_runtime_binding", "$.bindings", "binding field is not part of selected runtime traffic")
        message, field = fields[field_id]
        bindings_by_message[message["id"]].append((index, binding, field))
        used_fields.add(field_id)

    selected_messages = [messages_by_id[key] for key in bindings_by_message]
    selected_messages.sort(key=lambda item: (item["identifier"], item["id"]))
    binding_count = len(profile["bindings"])
    notify_count = sum("notify" in binding["capabilities"] for binding in profile["bindings"])
    counts = {
        "max_selected_messages": len(selected_messages),
        "max_extracted_fields": len(used_fields),
        "max_runtime_bindings": binding_count,
        "max_runtime_notify_bindings": notify_count,
    }
    for limit, actual in counts.items():
        if actual > RUNTIME_LIMITS[limit]:
            _fail("runtime_limit", "$.bindings", f"{limit} exceeds the declared runtime cap")

    lines = [
        "// Generated profile decoder table. All referenced data has static lifetime.",
        "#pragma once", "", "#include <array>", "#include <cstddef>", "#include <cstdint>",
        "#include <string_view>", "", '#include "vehicle_profile_metadata.hpp"',
        '#include "vehicle_profile_runtime/tables.hpp"', "",
        "namespace vehicle_profile {", "",
        f'inline constexpr std::string_view kCompatibilityIdentity = "{profile["compatibility_identity"]}";',
        f'inline constexpr std::string_view kArtifactIdentity = "{profile["artifact_identity"]}";',
        f"inline constexpr std::size_t kSelectedMessageCount = {len(selected_messages)}U;",
        f"inline constexpr std::size_t kExtractedFieldCount = {len(used_fields)}U;",
        f"inline constexpr std::size_t kBindingCount = {binding_count}U;",
        f"inline constexpr std::size_t kNotifyBindingCount = {notify_count}U;",
        f"inline constexpr std::size_t kMaxDecodeResults = {max((len(v) for v in bindings_by_message.values()), default=0)}U;",
        f"inline constexpr std::size_t kLimitSelectedMessages = {RUNTIME_LIMITS['max_selected_messages']}U;",
        f"inline constexpr std::size_t kLimitExtractedFields = {RUNTIME_LIMITS['max_extracted_fields']}U;",
        f"inline constexpr std::size_t kLimitBindings = {RUNTIME_LIMITS['max_runtime_bindings']}U;",
        f"inline constexpr std::size_t kLimitNotifyBindings = {RUNTIME_LIMITS['max_runtime_notify_bindings']}U;",
        "",
        "inline constexpr std::array<vehicle_profile_runtime::SignalFreshnessPolicy, kBindingCount> kFreshnessPolicies{{",
    ]
    for binding in sorted(profile["bindings"], key=lambda item: item["runtime_id"]):
        freshness = binding["freshness"]
        declared = freshness is not None
        timeout = freshness["timeout_ms"] if declared else 0
        lines.append(
            f"  {{vehicle_signals::SignalId{{{binding['runtime_id']}U}}, "
            f"{'true' if declared else 'false'}, {timeout}U}},")
    lines += ["}};", "", "namespace detail {", ""]

    function_indices: dict[str, int] = {}
    for function_index, message in enumerate(selected_messages):
        bindings = sorted(bindings_by_message[message["id"]], key=lambda entry: entry[1]["runtime_id"])
        function_indices[message["id"]] = function_index
        for index, binding, field in bindings:
            lines += _binding_code(index, binding, field)
        lines += _decoder_code(function_index, message, bindings)

    lines += ["} // namespace detail", "", f"inline constexpr std::array<vehicle_profile_runtime::DispatchEntry, {len(selected_messages)}> kDispatchTable{{{{"]
    for message in selected_messages:
        lines.append(
            f"  {{{message['identifier']}U, {message['dlc']}U, "
            f"&detail::decode_message_{function_indices[message['id']]}}},")
    lines += [
        "}};",
        "",
        "static_assert(kCatalog.well_formed(), \"generated catalog must be well formed\");",
        "static_assert(kFreshnessPolicies.size() == kCatalog.size(), \"every catalog entry needs one freshness row\");",
        "static_assert(kDispatchTable.size() == kSelectedMessageCount, \"dispatch count must match selected messages\");",
        "",
        "} // namespace vehicle_profile",
        "",
    ]
    return "\n".join(lines).encode("utf-8")
