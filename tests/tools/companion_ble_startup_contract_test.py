"""Regression guards for the companion GATT upgrade and telemetry startup path."""

from __future__ import annotations

from pathlib import Path
import re
import unittest


REPOSITORY = Path(__file__).resolve().parents[2]
SOURCE = (REPOSITORY / "components/companion_ble/src/companion_ble.cpp").read_text(
    encoding="utf-8"
)


def function_body(name: str) -> str:
    signature = re.search(
        rf"\b{re.escape(name)}\s*\([^;{{}}]*\)\s*(?:noexcept\s*)?\{{",
        SOURCE,
        re.DOTALL,
    )
    if signature is None:
        raise AssertionError(f"function {name} was not found")

    opening = signature.end() - 1
    depth = 0
    for index in range(opening, len(SOURCE)):
        if SOURCE[index] == "{":
            depth += 1
        elif SOURCE[index] == "}":
            depth -= 1
            if depth == 0:
                return SOURCE[opening + 1 : index]
    raise AssertionError(f"function {name} has an unterminated body")


class CompanionBleStartupContractTests(unittest.TestCase):
    def test_telemetry_start_reaches_both_live_streams(self) -> None:
        body = function_body("mark_telemetry_started")
        self.assertIn("internal::mark_live_signals_telemetry_started()", body)
        self.assertIn("internal::mark_extensible_live_signals_telemetry_started()", body)

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
