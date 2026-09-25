#!/usr/bin/env python3
"""Validate local ARGB isolation and the WeAct engine LED composition root."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys


def _strip_line_comments(text: str) -> str:
    return re.sub(r"//[^\n]*", "", text)


def _setup_failure_blocks(vehicle_main: str) -> list[str]:
    """Return app_main's early-return blocks after local ARGB startup.

    Once the renderer has started, every failure path up to and including the
    telemetry/CAN start must fail the strip off before returning: a stopped
    facade publishes no Unavailable notice and a detached engine sends no
    Deactivate, so a held effect would otherwise stay lit.
    """

    app_main = vehicle_main[max(vehicle_main.find('extern "C" void app_main'), 0) :]
    argb_start = app_main.find("local_argb::start()")
    telemetry_start = app_main.find("telemetry.start()")
    if argb_start < 0 or telemetry_start < 0:
        return []
    blocks: list[str] = []
    for match in re.finditer(r"if\s*\((.*?)\)\s*\{(.*?)\}", app_main, flags=re.S):
        if argb_start < match.start() <= telemetry_start and "return;" in match.group(2):
            blocks.append(match.group(0))
    return blocks


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
        ('"vehicle.turn_state"', "generic turn-state rule signal"),
        ("action_engine::FreshnessRequirement::Fresh", "strict turn freshness"),
        ("engine.add_sink(led_actions)", "LED action sink registration"),
        ("led_actions.bind(", "LED effect binding"),
        ("engine.add_state_rule(", "turn-state rule registration"),
        ("engine.attach()", "engine attachment"),
        ("telemetry.on_turn_state_changed", "typed turn notification registration"),
        ("telemetry.speed_kph()", "speed polling"),
        ("telemetry.engine_rpm()", "engine RPM polling"),
        ("telemetry.start()", "facade-owned startup"),
        ("vTaskDelay(pdMS_TO_TICKS(100))", "application polling cadence"),
    ):
        if needle not in vehicle_main:
            failures.append(f"{label} is missing from vehicle integration: {needle}")

    if vehicle_main.find("board::initialize_safe_defaults()") > vehicle_main.find("local_argb::start()"):
        failures.append("board safe defaults do not precede local ARGB startup")
    if vehicle_main.find("local_argb::start()") > vehicle_main.find("telemetry.start()"):
        failures.append("local ARGB startup does not precede telemetry/CAN startup")
    if vehicle_main.find("local_argb::start()") > vehicle_main.find("engine.attach()"):
        failures.append("local ARGB startup does not precede engine attachment")
    if vehicle_main.find("engine.attach()") > vehicle_main.find("telemetry.start()"):
        failures.append("engine attachment does not precede telemetry/CAN startup")
    for block in _setup_failure_blocks(vehicle_main):
        if "local_argb::fail_off();" not in block:
            failures.append(f"setup failure path does not fail off: {block.splitlines()[0]}")
    for match in re.finditer(r"telemetry\.stop\(\)|engine\.detach\(\)", vehicle_main):
        following = vehicle_main[match.end() :]
        next_return = following.find("return")
        if "local_argb::fail_off();" not in following[: next_return if next_return >= 0 else None]:
            failures.append(f"{match.group(0)} is not followed by local_argb::fail_off()")

    # The strip is mounted mirrored, as in the retired telemetry binding: the
    # vehicle's left indicator lights the renderer's right_turn region and vice
    # versa, and hazard lights both.
    for pattern, label in (
        (r'\{"left",\s*kTurnLeftAction\}', "left turn-state rule"),
        (r'\{"right",\s*kTurnRightAction\}', "right turn-state rule"),
        (r'\{"hazard",\s*kHazardAction\}', "hazard turn-state rule"),
        (r"\{kTurnLeftAction,\s*local_argb_actions::LedEffect::RightTurn\}",
         "mirrored left-turn binding"),
        (r"\{kTurnRightAction,\s*local_argb_actions::LedEffect::LeftTurn\}",
         "mirrored right-turn binding"),
        (r"\{kHazardAction,\s*local_argb_actions::LedEffect::LeftTurn\}",
         "hazard left-region binding"),
        (r"\{kHazardAction,\s*local_argb_actions::LedEffect::RightTurn\}",
         "hazard right-region binding"),
    ):
        if re.search(pattern, vehicle_main) is None:
            failures.append(f"{label} is missing from vehicle integration: {pattern}")
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
        if forbidden in _strip_line_comments(vehicle_main):
            failures.append(f"vehicle application retains {label}: {forbidden}")

    # The LED action sink is an output adapter: it receives engine commands and
    # never subscribes to a provider, the telemetry facade or CAN itself.
    led_sources = "\n".join(
        _strip_line_comments(path.read_text(encoding="utf-8"))
        for path in sorted((root / "components/local_argb_actions").rglob("*"))
        if path.suffix in {".h", ".hpp", ".cpp"}
    )
    if not led_sources:
        failures.append("local LED action sink sources are missing")
    for forbidden in ("subscribe", "on_turn_state_changed", "SignalProvider", "can_bus", "twai"):
        if forbidden in led_sources:
            failures.append(f"local LED action sink subscribes directly: {forbidden}")
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
