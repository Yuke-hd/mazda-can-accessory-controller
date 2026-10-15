"""Regression guards for the companion GATT upgrade and telemetry startup path."""

from __future__ import annotations

from pathlib import Path
import re
import unittest


REPOSITORY = Path(__file__).resolve().parents[2]
SOURCE = (REPOSITORY / "components/companion_ble/src/companion_ble.cpp").read_text(
    encoding="utf-8"
)


def function_body_in(source: str, name: str) -> str:
    signature = re.search(
        rf"\b{re.escape(name)}\s*\([^;{{}}]*\)\s*(?:noexcept\s*)?\{{",
        source,
        re.DOTALL,
    )
    if signature is None:
        raise AssertionError(f"function {name} was not found")

    opening = signature.end() - 1
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1 : index]
    raise AssertionError(f"function {name} has an unterminated body")


def function_body(name: str) -> str:
    return function_body_in(SOURCE, name)


class CompanionBleStartupContractTests(unittest.TestCase):
    def test_telemetry_start_reaches_both_live_streams(self) -> None:
        body = function_body("mark_telemetry_started")
        self.assertIn("internal::mark_live_signals_telemetry_started()", body)
        self.assertIn("internal::mark_extensible_live_signals_telemetry_started()", body)

    def test_device_info_capability_tracks_export_and_complete_service_binding(self) -> None:
        start = function_body("start")
        self.assertIn("signal_catalog.emplace(inputs.live_signals->catalog())", start)
        self.assertIn("signal_catalog->available()", start)
        self.assertIn("DeviceInfoExtensionStatus::Usable", start)
        self.assertIn("DeviceInfoExtensionStatus::Unusable", start)
        self.assertEqual(start.count("encode_device_info(served, extension_status)"), 2)

        register = function_body("register_services")
        for characteristic in (
            "internal::signal_catalog_characteristic()",
            "internal::extensible_live_signals_characteristic()",
        ):
            self.assertIn(characteristic, register)
        bind_catalog = register.index("internal::bind_signal_catalog(*signal_catalog)")
        bind_stream = register.index("internal::bind_extensible_live_signals(*signal_catalog")
        self.assertLess(bind_catalog, bind_stream)

        startup = function_body("bring_up_nimble")
        bind_result = startup.index("const int rc = register_services()")
        failed_registration = startup.index("if (rc != 0)", bind_result)
        host_start = startup.index("nimble_port_freertos_init(host_task)")
        self.assertLess(bind_result, failed_registration)
        self.assertLess(failed_registration, host_start)

        device_info = (
            REPOSITORY / "components/companion_ble/src/device_info_characteristic.cpp"
        ).read_text(encoding="utf-8")
        access = function_body_in(device_info, "access_device_info")
        self.assertIn(
            "const auto &value = pairing_window_open() ? values.window_open : values.window_closed;",
            access,
        )
        self.assertIn("const auto bytes = value.view();", access)
        self.assertIn(
            "os_mbuf_append(context->om, bytes.data(), static_cast<std::uint16_t>(bytes.size()))",
            access,
        )
        self.assertIn("NimBLE applies Read Blob offsets itself", device_info)
        access_code = re.sub(r"//[^\n]*|/\*.*?\*/", "", access, flags=re.DOTALL)
        self.assertNotRegex(access_code, r"\boffsets?\b")

    def test_host_sync_queues_whole_table_service_changed_before_advertising(self) -> None:
        body = function_body("on_host_sync")
        ensure_address = body.index("ble_hs_util_ensure_addr(0)")
        address_failure_return = body.index("return;", ensure_address)
        service_changed = body.index(
            "ble_svc_gatt_changed(1U, std::numeric_limits<std::uint16_t>::max())"
        )
        advertise = body.index("internal::start_advertising()")

        self.assertLess(ensure_address, service_changed)
        self.assertLess(address_failure_return, service_changed)
        self.assertLess(service_changed, advertise)
        self.assertIn("if (rc != 0)", body[ensure_address:service_changed])
        self.assertIn('#include "services/gatt/ble_svc_gatt.h"', SOURCE)


if __name__ == "__main__":
    unittest.main()
