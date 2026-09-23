#!/usr/bin/env python3
"""Validate local ARGB isolation and final WeAct facade composition."""

from __future__ import annotations

import argparse
from pathlib import Path
import sys


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
        ('#include "mazda/accessory_telemetry.hpp"',
         "explicit vehicle application binding include"),
        ("mazda::VehicleTelemetry telemetry{}", "facade instance"),
        ("mazda::application::bind_local_argb_sink", "private ARGB sink binding"),
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
    for forbidden, label in (
        ("semantic_led_policy", "legacy semantic application adapter"),
        ("process_received_frame", "manual decoder loop"),
        ("PublicationPolicy", "manual lighting publication policy"),
        ("can_bus::receive", "manual CAN receive loop"),
        ("local_argb::submit", "manual lighting submission loop"),
    ):
        if forbidden in vehicle_main:
            failures.append(f"vehicle application retains {label}: {forbidden}")
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
