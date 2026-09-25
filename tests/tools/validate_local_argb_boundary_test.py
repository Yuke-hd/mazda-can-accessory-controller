"""Regression tests for the local ARGB boundary and firmware composition validator."""

from __future__ import annotations

from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


REPOSITORY = Path(__file__).resolve().parents[2]
VALIDATOR = REPOSITORY / "tools/validate_local_argb_boundary.py"
MAIN = Path("firmware/weact-can485-v1.1/main/main.cpp")
LED_SINK_SOURCE = Path("components/local_argb_actions/src/led_action_sink.cpp")

# The validator reads only these repository paths.
FIXTURE_PATHS = (
    Path("components/board"),
    Path("components/local_argb"),
    Path("components/local_argb_actions"),
    Path("firmware/weact-can485-v1.1/CMakeLists.txt"),
    Path("firmware/weact-can485-v1.1/main/CMakeLists.txt"),
    MAIN,
)


def copy_fixture(root: Path) -> None:
    for relative in FIXTURE_PATHS:
        source = REPOSITORY / relative
        target = root / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        if source.is_dir():
            shutil.copytree(source, target)
        else:
            shutil.copy2(source, target)


def run_validator(root: Path) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, str(VALIDATOR), "--root", str(root)],
        capture_output=True,
        text=True,
        check=False,
    )


class LocalArgbBoundaryValidatorTests(unittest.TestCase):
    def setUp(self) -> None:
        self._directory = tempfile.TemporaryDirectory(prefix="local-argb-boundary-fixture-")
        self.root = Path(self._directory.name)
        copy_fixture(self.root)

    def tearDown(self) -> None:
        self._directory.cleanup()

    def edit(self, relative: Path, old: str, new: str) -> None:
        path = self.root / relative
        text = path.read_text(encoding="utf-8")
        self.assertIn(old, text, f"fixture anchor is missing from {relative}")
        path.write_text(text.replace(old, new, 1), encoding="utf-8")

    def assert_rejected(self, *messages: str) -> None:
        result = run_validator(self.root)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        for message in messages:
            self.assertIn(message, result.stderr)

    def test_repository_composition_passes(self) -> None:
        result = run_validator(self.root)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("validated", result.stdout)

    def test_legacy_sink_binding_is_rejected(self) -> None:
        self.edit(
            MAIN,
            "  const auto turn_subscription =",
            "  (void)mazda::application::bind_local_argb_sink(telemetry);\n"
            "  const auto turn_subscription =",
        )
        self.assert_rejected("legacy telemetry lighting binding")

    def test_engine_attach_after_telemetry_start_is_rejected(self) -> None:
        self.edit(MAIN, "engine.attach()", "vehicle_signals::SignalStatus::Ok")
        self.edit(
            MAIN,
            '  ESP_LOGI(kTag, "strict listen-only CAN acquisition started',
            "  (void)engine.attach();\n"
            '  ESP_LOGI(kTag, "strict listen-only CAN acquisition started',
        )
        self.assert_rejected("engine attachment does not precede telemetry/CAN startup")

    def test_setup_failure_without_fail_off_is_rejected(self) -> None:
        self.edit(
            MAIN,
            "    local_argb::fail_off();\n"
            '    ESP_LOGE(kTag, "engine LED action setup failed',
            '    ESP_LOGE(kTag, "engine LED action setup failed',
        )
        self.assert_rejected("setup failure path does not fail off")

    def test_stop_or_detach_without_fail_off_is_rejected(self) -> None:
        # Stopping the facade publishes no Unavailable notice and detaching the
        # engine sends no Deactivate, so both must be followed by fail-off.
        self.edit(
            MAIN,
            "    vTaskDelay(pdMS_TO_TICKS(100));\n",
            "    vTaskDelay(pdMS_TO_TICKS(100));\n"
            "    (void)telemetry.stop();\n"
            "    (void)engine.detach();\n",
        )
        self.assert_rejected(
            "telemetry.stop() is not followed by local_argb::fail_off()",
            "engine.detach() is not followed by local_argb::fail_off()",
        )

    def test_unmirrored_turn_binding_is_rejected(self) -> None:
        self.edit(
            MAIN,
            "{kTurnRightAction, local_argb_actions::LedEffect::LeftTurn}",
            "{kTurnRightAction, local_argb_actions::LedEffect::RightTurn}",
        )
        self.assert_rejected("mirrored right-turn binding")

    def test_weakened_turn_freshness_is_rejected(self) -> None:
        self.edit(
            MAIN,
            "action_engine::FreshnessRequirement::Fresh",
            "action_engine::FreshnessRequirement::FreshOrUnverified",
        )
        self.assert_rejected("weakened turn freshness")

    def test_missing_engine_components_are_rejected(self) -> None:
        self.edit(
            Path("firmware/weact-can485-v1.1/CMakeLists.txt"),
            '    "${CMAKE_CURRENT_LIST_DIR}/../../lib/action_engine"\n',
            "",
        )
        self.edit(
            Path("firmware/weact-can485-v1.1/main/CMakeLists.txt"), " local_argb_actions", ""
        )
        self.assert_rejected(
            "vehicle project does not select the lib/action_engine component",
            "vehicle application does not require the local_argb_actions component",
        )

    def test_led_sink_subscription_is_rejected(self) -> None:
        self.edit(
            LED_SINK_SOURCE,
            "namespace local_argb_actions {\n",
            "namespace local_argb_actions {\nvoid probe() { provider.subscribe(); }\n",
        )
        self.assert_rejected("local LED action sink subscribes directly: subscribe")


if __name__ == "__main__":
    unittest.main()
