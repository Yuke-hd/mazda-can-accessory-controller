#!/usr/bin/env python3
"""Validate local ARGB isolation and the WeAct engine LED composition root."""

from __future__ import annotations

import argparse
from collections import Counter
from pathlib import Path
import re
import sys
from typing import List, Optional

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_architecture import _strip_cpp_comments  # noqa: E402

# Composition-root checks run on main.cpp with comments removed and string
# literal contents blanked, so commented-out code never satisfies a rule and
# braces or semicolons inside log strings never change the structure.
_STRING_LITERAL = re.compile(r'"(?:\\.|[^"\\\n])*"')
_FAIL_OFF = re.compile(r"\blocal_argb::fail_off\s*\(\s*\)\s*;")
_SHUTDOWN_CALL = re.compile(r"\b(?:telemetry\.stop|engine\.detach)\s*\(\s*\)")

# The strip is mounted mirrored, as in the retired telemetry binding: the
# vehicle's left indicator lights the renderer's right_turn region and vice
# versa, and hazard lights both. The tables must hold exactly these entries.
_TURN_RULES = ('"left",kTurnLeftAction', '"right",kTurnRightAction', '"hazard",kHazardAction')
_EFFECT_BINDINGS = (
    "kTurnLeftAction,local_argb_actions::LedEffect::RightTurn",
    "kTurnRightAction,local_argb_actions::LedEffect::LeftTurn",
    "kHazardAction,local_argb_actions::LedEffect::LeftTurn",
    "kHazardAction,local_argb_actions::LedEffect::RightTurn",
)


def _squash(text: str) -> str:
    return re.sub(r"\s+", "", text)


def _matching_close(text: str, open_index: int) -> int:
    depth = 0
    for index in range(open_index, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return index
    return len(text)


def _enclosing_open(text: str, index: int) -> int:
    """Index of the unmatched `{` that encloses `index`, or -1."""

    depth = 0
    for position in range(index - 1, -1, -1):
        if text[position] == "}":
            depth += 1
        elif text[position] == "{":
            if depth == 0:
                return position
            depth -= 1
    return -1


def _flatten(text: str) -> str:
    """Keep `text` at its own nesting level; nested blocks collapse to `{}`."""

    kept: List[str] = []
    depth = 0
    for char in text:
        if char == "{":
            if depth == 0:
                kept.append(char)
            depth += 1
        elif char == "}":
            depth -= 1
            if depth < 0:
                break
            if depth == 0:
                kept.append(char)
        elif depth == 0:
            kept.append(char)
    return "".join(kept)


def _has_unconditional_fail_off(flat: str) -> bool:
    """True if `flat` holds a fail_off() statement that no `if`/`else` controls."""

    for match in _FAIL_OFF.finditer(flat):
        before = flat[: match.start()].rstrip()
        if not before or before[-1] in ";{}":
            return True
    return False


def _statement_end(body: str, index: int) -> int:
    """End of the top-level statement of `body` that contains `index`."""

    braces = parens = 0
    for position, char in enumerate(body):
        if char == "(":
            parens += 1
        elif char == ")":
            parens -= 1
        elif char == "{":
            braces += 1
        elif char == "}":
            braces -= 1
            if braces == 0 and parens == 0 and position >= index:
                return position + 1
        elif char == ";" and braces == 0 and parens == 0 and position >= index:
            return position + 1
    return len(body)


def _context(text: str, end: int) -> str:
    return " ".join(text[max(0, end - 80) : end].split())


def _fail_off_failures(structure: str) -> List[str]:
    """Return fail-off violations in the composition root.

    Once the renderer has started, every `return` up to and including the
    telemetry/CAN start must be preceded, in its own block and on its own path,
    by `local_argb::fail_off();`. A stopped facade publishes no Unavailable
    notice and a detached engine sends no Deactivate, so every stop or detach
    must also fail off on its success path.
    """

    failures: List[str] = []
    app_main = re.search(r"\bvoid\s+app_main\s*\(", structure)
    open_index = structure.find("{", app_main.end()) if app_main is not None else -1
    if app_main is None:
        failures.append("app_main is missing from vehicle integration")
    if open_index >= 0:
        body = structure[open_index + 1 : _matching_close(structure, open_index)]
        argb_start = body.find("local_argb::start()")
        telemetry_start = body.find("telemetry.start()")
        if argb_start >= 0 and telemetry_start >= 0:
            region_start = _statement_end(body, argb_start)
            region_end = _statement_end(body, telemetry_start)
            for match in re.finditer(r"\breturn\b", body[region_start:region_end]):
                position = region_start + match.start()
                before = body[:position].rstrip()
                block_open = _enclosing_open(body, position)
                path = _flatten(body[max(block_open + 1, region_start) : position])
                braceless = before.endswith(")") or re.search(r"\belse$", before) is not None
                if braceless or not _has_unconditional_fail_off(path):
                    failures.append(
                        "setup failure path does not fail off before returning: "
                        + _context(body, position + len("return"))
                    )
    for match in _SHUTDOWN_CALL.finditer(structure):
        block_open = _enclosing_open(structure, match.start())
        block_close = _matching_close(structure, block_open) if block_open >= 0 else len(structure)
        success_path = _flatten(structure[match.end() : block_close])
        next_return = re.search(r"\breturn\b", success_path)
        if next_return is not None:
            success_path = success_path[: next_return.start()]
        if not _has_unconditional_fail_off(success_path):
            call = _squash(match.group(0))
            failures.append(f"{call} is not followed by local_argb::fail_off() on its success path")
    return failures


def _table_entries(structure: str, name: str) -> Optional[List[str]]:
    match = re.search(rf"\b{name}\s*\[\s*\]\s*=\s*\{{", structure)
    if match is None:
        return None
    open_index = match.end() - 1
    table = structure[open_index + 1 : _matching_close(structure, open_index)]
    return [_squash(entry) for entry in re.findall(r"\{([^{}]*)\}", table)]


def _loop_body(structure: str, pattern: str) -> Optional[str]:
    match = re.search(pattern, structure)
    if match is None:
        return None
    open_index = structure.find("{", match.end())
    if open_index < 0 or structure[match.end() : open_index].strip():
        return None
    return _squash(structure[open_index + 1 : _matching_close(structure, open_index)])


def _binding_failures(code: str, structure: str) -> List[str]:
    """Require the mirrored tables and loops that apply exactly those tables."""

    failures: List[str] = []
    for name, expected in (("kTurnRules", _TURN_RULES), ("kEffectBindings", _EFFECT_BINDINGS)):
        entries = _table_entries(code, name)
        if entries is None or Counter(entries) != Counter(expected):
            failures.append(f"{name} must hold exactly the mirrored entries {list(expected)}")
    actions = dict(re.findall(r"\bActionId\s+(k\w+Action)\s*\{\s*(\d+)\s*\}", structure))
    values = [actions.get(name) for name in ("kTurnLeftAction", "kTurnRightAction", "kHazardAction")]
    if None in values or "0" in values or len(set(values)) != len(values):
        failures.append("turn actions must be three distinct nonzero ActionIds")
    if re.search(r'\bkTurnStateSignal\s*\{\s*"vehicle\.turn_state"\s*\}', code) is None:
        failures.append('kTurnStateSignal must name the "vehicle.turn_state" signal')

    binding_loop = _loop_body(
        structure, r"\bfor\s*\(\s*const\s+auto\s*&\s*binding\s*:\s*kEffectBindings\s*\)"
    )
    if binding_loop is None or "led_actions.bind(binding.action,binding.effect)" not in binding_loop:
        failures.append("LED effect binding loop does not bind every kEffectBindings entry")
    rule_loop = _loop_body(
        structure, r"\bfor\s*\(\s*const\s+auto\s*&\s*rule\s*:\s*kTurnRules\s*\)"
    )
    if rule_loop is None or not all(
        needle in rule_loop
        for needle in (
            "{kTurnStateSignal,action_engine::Comparison::Equal,"
            "action_engine::RuleOperand::choice(rule.choice)},rule.action,"
            "action_engine::FreshnessRequirement::Fresh}",
            "engine.add_state_rule(config)",
        )
    ):
        failures.append("turn-state rule loop does not add a strict rule for every kTurnRules entry")
    counts = {
        "led_actions.bind(": 1,
        "engine.add_state_rule(": 1,
        "engine.add_sink(": 1,
        "engine.add_event_rule(": 0,
        "engine.add_range_rule(": 0,
    }
    squashed = _squash(structure)
    for needle, expected_count in counts.items():
        if squashed.count(needle) != expected_count:
            failures.append(
                f"vehicle integration must call {needle}) exactly {expected_count} time(s)"
            )
    return failures


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--core-root", type=Path)
    args = parser.parse_args()
    root = args.root.resolve()
    core_root = (args.core_root or root / "third_party/esp32-vehicle-can-core").resolve()
    failures: list[str] = []

    component = root / "components/local_argb"
    sources = "\n".join(
        path.read_text(encoding="utf-8")
        for path in component.rglob("*")
        if path.suffix in {".h", ".hpp", ".cpp"}
    )
    public_header = (component / "include/local_argb/local_argb.h").read_text(encoding="utf-8")
    idf_source = (component / "src/local_argb_idf.cpp").read_text(encoding="utf-8")
    board_header = (root / "components/board/include/board/board_config.h").read_text(
        encoding="utf-8"
    )
    vehicle_cmake = (root / "firmware/weact-can485-v1.1/main/CMakeLists.txt").read_text(
        encoding="utf-8"
    )
    vehicle_main = (root / "firmware/weact-can485-v1.1/main/main.cpp").read_text(
        encoding="utf-8"
    )
    vehicle_project_cmake = (root / "firmware/weact-can485-v1.1/CMakeLists.txt").read_text(
        encoding="utf-8"
    )

    for forbidden in (
        "RawCanFrame",
        "mazda_candidate",
        "kTurnSwitchId",
        "can_bus/",
        "driver/twai",
        "twai_",
    ):
        if forbidden in sources:
            failures.append(f"local_argb contains forbidden transport/decoder dependency: {forbidden}")
    if '"board/board_config.h"' in public_header or '"led_strip.h"' in public_header:
        failures.append("portable local_argb public API exposes hardware dependencies")

    requirements = {
        "board::kWeActCan485V11.onboard_rgb.data": "central GPIO binding",
        "board::kWeActCan485V11.onboard_rgb.pixel_count": "central pixel-count binding",
        "board::kWeActCan485V11.vehicle_light_strip.data": "vehicle strip GPIO binding",
        "board::kWeActCan485V11.vehicle_light_strip.pixel_count": "vehicle strip pixel-count binding",
        "LED_MODEL_WS2812": "WS2812 model",
        "LED_STRIP_COLOR_COMPONENT_FMT_GRB": "GRB component order",
        "kRmtResolutionHz = 10'000'000": "10 MHz RMT resolution",
        "rmt_config.flags.with_dma = false": "DMA-disabled RMT",
        "xQueueCreateStatic(1": "length-one queue",
        "xQueueOverwrite": "nonblocking overwrite submission",
        "kWorkerPriority = tskIDLE_PRIORITY + 2": "lower-priority worker",
        "g_controller.tick": "independent timeout tick",
        "g_controller.start": "explicit startup black frame",
        "xTaskCreate(supervisor": "independent driver-hang supervisor",
        "kSupervisorPriority = configMAX_PRIORITIES - 1": "supervisor above can_rx",
        "g_driver_watchdog.restart_due": "bounded driver timeout",
        "g_worker_lease.restart_due": "bounded worker-progress timeout",
        "heartbeat_worker()": "worker progress heartbeat",
        "if (worker_created == pdPASS)": "post-creation lease arm",
        "disarm_worker_lease()": "startup-failure lease disarm",
        "esp_restart()": "supervised stall reset recovery",
    }
    for needle, label in requirements.items():
        if needle not in idf_source:
            failures.append(f"{label} is missing: {needle}")
    for needle, label in (
        ("OnboardRgb{4, kOnboardRgbPixelCount}", "GPIO4/single-pixel onboard board record"),
        ("VehicleLightStrip{16, kVehicleLightStripPixelCount}", "GPIO16/100-pixel vehicle board record"),
        ("kVehicleLightStripPixelCount = 100", "100-pixel logical renderer record"),
    ):
        if needle not in board_header:
            failures.append(f"{label} is missing: {needle}")
    # Strip comments once; every composition-root rule runs on this code.
    code, _ = _strip_cpp_comments(vehicle_main)
    structure = _STRING_LITERAL.sub('""', code)
    for needle, label in (
        ('#include "mazda/vehicle_telemetry.hpp"', "public telemetry facade include"),
        ('#include "mazda/signal_provider.hpp"', "generic signal provider include"),
        ('#include "action_engine/engine.hpp"', "generic action engine include"),
        ('#include "local_argb_actions/led_action_sink.hpp"', "local LED action sink include"),
        ("static mazda::VehicleTelemetry telemetry{}", "static facade instance"),
        ("static mazda::MazdaSignalProvider signal_provider{telemetry}",
         "static generic provider over the facade"),
        ("static action_engine::ActionEngine engine{signal_provider}", "static action engine"),
        ("static local_argb_actions::LedActionSink led_actions{local_argb::internal::sink()}",
         "static LED action sink bound to the renderer queue"),
        ("engine.add_sink(led_actions)", "LED action sink registration"),
        ("engine.attach()", "engine attachment"),
        ("telemetry.on_turn_state_changed", "typed turn notification registration"),
        ("telemetry.speed_kph()", "speed polling"),
        ("telemetry.engine_rpm()", "engine RPM polling"),
        ("telemetry.start()", "facade-owned startup"),
        ("vTaskDelay(pdMS_TO_TICKS(100))", "application polling cadence"),
    ):
        if needle not in code:
            failures.append(f"{label} is missing from vehicle integration: {needle}")

    for earlier, later, label in (
        ("board::initialize_safe_defaults()", "local_argb::start()",
         "board safe defaults do not precede local ARGB startup"),
        ("local_argb::start()", "engine.attach()",
         "local ARGB startup does not precede engine attachment"),
        ("engine.attach()", "telemetry.start()",
         "engine attachment does not precede telemetry/CAN startup"),
        ("local_argb::start()", "telemetry.start()",
         "local ARGB startup does not precede telemetry/CAN startup"),
    ):
        earlier_index, later_index = code.find(earlier), code.find(later)
        if earlier_index >= 0 and later_index >= 0 and earlier_index > later_index:
            failures.append(label)
    failures.extend(_fail_off_failures(structure))
    failures.extend(_binding_failures(code, structure))
    for forbidden, label in (
        ("semantic_led_policy", "legacy semantic application adapter"),
        ("process_received_frame", "manual decoder loop"),
        ("PublicationPolicy", "manual lighting publication policy"),
        ("can_bus::receive", "manual CAN receive loop"),
        ("local_argb::submit", "manual lighting submission loop"),
        # The renderer queue is a one-slot overwrite queue: the LED action sink
        # must be its only publisher.
        ("bind_local_argb_sink", "legacy telemetry lighting binding"),
        ("mazda/accessory_telemetry.hpp", "legacy telemetry lighting binding include"),
        ("FreshOrUnverified", "weakened turn freshness"),
    ):
        if forbidden in code:
            failures.append(f"vehicle application retains {label}: {forbidden}")

    for component in ("lib/action_engine", "components/local_argb_actions"):
        if f'/../../{component}"' not in vehicle_project_cmake:
            failures.append(f"vehicle project does not select the {component} component")
    requires = re.search(r"\bREQUIRES\b([^)]*)", vehicle_cmake)
    for component in ("action_engine", "local_argb_actions", "local_argb_sink_contract"):
        if requires is None or component not in requires.group(1).split():
            failures.append(f"vehicle application does not require the {component} component")
    if "local_argb_compat" in vehicle_cmake:
        failures.append("vehicle application still selects the retired local_argb compatibility target")
    if failures:
        for failure in failures:
            print(f"ERROR: {failure}", file=sys.stderr)
        return 1
    print("local ARGB semantic and hardware boundary validated")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
