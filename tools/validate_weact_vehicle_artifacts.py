#!/usr/bin/env python3
"""Validate the WeAct V1.1 accessory firmware identity and safety boundaries."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys


def require(text: str, needle: str, label: str, failures: list[str]) -> None:
    if needle not in text:
        failures.append(f"{label} is missing: {needle}")


def forbid(text: str, needle: str, label: str, failures: list[str]) -> None:
    if needle in text:
        failures.append(f"{label} contains forbidden text: {needle}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--core-root", type=Path)
    args = parser.parse_args()
    root = args.root.resolve()
    core_root = (args.core_root or root / "third_party/esp32-vehicle-can-core").resolve()
    failures: list[str] = []

    vehicle_dir = root / "firmware/weact-can485-v1.1"
    retired_vehicle_dir = root / "firmware/tcan485"
    if not vehicle_dir.is_dir():
        failures.append("WeAct CAN485 V1.1 vehicle project is missing")
    if retired_vehicle_dir.exists() and any(path.is_file() for path in retired_vehicle_dir.rglob("*")):
        failures.append("retired firmware/tcan485 vehicle project still exists")

    vehicle_cmake = (vehicle_dir / "CMakeLists.txt").read_text(encoding="utf-8")
    vehicle_main = (vehicle_dir / "main/main.cpp").read_text(encoding="utf-8")
    vehicle_component = (vehicle_dir / "main/idf_component.yml").read_text(encoding="utf-8")
    vehicle_binding = (
        root / "components/vehicle_can_rx/src/driver_binding.cpp"
    ).read_text(encoding="utf-8")
    board_header = (root / "components/board/include/board/board_config.h").read_text(
        encoding="utf-8"
    )
    board_source = (root / "components/board/src/board.cpp").read_text(encoding="utf-8")
    can_header = (core_root / "components/can_bus/include/can_bus/can_bus.h").read_text(
        encoding="utf-8"
    )
    can_source = (core_root / "components/can_bus/src/can_bus.cpp").read_text(encoding="utf-8")
    telemetry_source = (
        root / "components/mazda_telemetry/src/vehicle_telemetry.cpp"
    ).read_text(encoding="utf-8")

    require(
        vehicle_cmake,
        "project(weact_can485_v11_vehicle_listen_only)",
        "vehicle project name",
        failures,
    )
    require(
        vehicle_cmake,
        "components/vehicle_can_rx",
        "vehicle binding component selection",
        failures,
    )
    require(vehicle_main, "WeAct CAN485 DevBoard V1.1", "vehicle board identity", failures)
    require(vehicle_main, "STRICT LISTEN-ONLY", "vehicle startup warning", failures)
    require(
        vehicle_component,
        "WeAct CAN485 V1.1 Mazda accessory-controller firmware",
        "vehicle artifact label",
        failures,
    )

    board_requirements = {
        "kWeActCan485DevBoardV11": "hardware revision",
        "kWeActCan485V11": "board capability name",
        "CanPins{27, 26}": "CAN pin map",
        "Rs485Pins{17, 21, 22}": "RS485 pin map",
        "MicroSdPins{2, 15, 14, 13}": "microSD pin map",
        "OnboardRgb{4, kOnboardRgbPixelCount}": "single-pixel onboard WS2812B status LED",
        "VehicleLightStrip{16, kVehicleLightStripPixelCount}": "100-pixel GPIO16 vehicle WS2812B strip",
        "kVehicleLightStripPixelCount = 100": "100-pixel logical vehicle light layout",
        "AuxiliaryPins{36, 0}": "VIN sense and user-key pins",
        'UsbSerialInterface{"native USB serial", "CH343P"}': "USB serial identity",
        "can_transceiver_always_powered": "CA-IS2062A power model",
    }
    for needle, label in board_requirements.items():
        require(board_header, needle, label, failures)
    for forbidden in ("kTcan485", "speed_mode", "boost_enable", "set_can_transceiver_power"):
        forbid(
            board_header + board_source + can_source + vehicle_binding,
            forbidden,
            "vehicle/shared source",
            failures,
        )
    require(board_source, "onboard_rgb.data, 0", "GPIO4 data-line-low startup", failures)
    require(board_source, "vehicle_light_strip.data, 0", "GPIO16 data-line-low startup", failures)
    require(board_source, "rs485.driver_enable, 0", "RS485 disabled startup", failures)
    require(board_source, "can.tx, 1", "CAN TX recessive startup", failures)

    require(vehicle_binding, "board::kWeActCan485V11.can.tx", "vehicle CAN TX binding", failures)
    require(vehicle_binding, "board::kWeActCan485V11.can.rx", "vehicle CAN RX binding", failures)
    require(vehicle_binding, "TWAI_MODE_LISTEN_ONLY", "vehicle CAN mode", failures)
    if not re.search(r"configuration\.tx_queue_len\s*=\s*0\s*;", vehicle_binding):
        failures.append("CAN TX queue is not explicitly disabled")
    for source_name, source in (
        ("CAN public header", can_header),
        ("CAN implementation and vehicle binding", can_source + vehicle_binding),
        ("vehicle main", vehicle_main),
    ):
        for forbidden in ("twai_transmit", "twai_transmit_v2", "TWAI_MODE_NORMAL", "TWAI_MODE_NO_ACK"):
            forbid(source, forbidden, source_name, failures)

    require(
        telemetry_source,
        "constexpr TickType_t kMinimumTaskDelayTicks",
        "telemetry ESP task delay guard",
        failures,
    )
    forbid(
        telemetry_source,
        "vTaskDelay(pdMS_TO_TICKS(1));",
        "telemetry ESP task delay guard",
        failures,
    )
    public_functions = re.findall(
        r"^(?:Result|Statistics)\s+([a-zA-Z_][a-zA-Z0-9_]*)\s*\(",
        can_header,
        flags=re.MULTILINE,
    )
    if public_functions != ["start", "stop", "receive", "statistics"]:
        failures.append(f"unexpected public CAN functions: {public_functions}")

    if failures:
        for failure in failures:
            print(f"ERROR: {failure}", file=sys.stderr)
        return 1
    print("WeAct V1.1 accessory firmware artifacts validated")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
