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
PROJECT_CMAKE = Path("firmware/weact-can485-v1.1/CMakeLists.txt")
MAIN_CMAKE = Path("firmware/weact-can485-v1.1/main/CMakeLists.txt")

# The validator reads only these repository paths.
FIXTURE_PATHS = (
    Path("components/board"),
    Path("components/local_argb"),
    PROJECT_CMAKE,
    MAIN_CMAKE,
    MAIN,
)

ATTACH_FAILURE = (
    "    local_argb::fail_off();\n"
    '    ESP_LOGE(kTag, "engine attachment failed; refusing to start CAN");\n'
)
POLL_DELAY = "    vTaskDelay(pdMS_TO_TICKS(100));\n"
BEFORE_CAN_START = '  ESP_LOGI(kTag,\n           "WeAct CAN485 DevBoard V1.1'


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

    def assert_accepted(self) -> None:
        result = run_validator(self.root)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("validated", result.stdout)

    def assert_rejected(self, *messages: str) -> None:
        result = run_validator(self.root)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        for message in messages:
            self.assertIn(message, result.stderr)

    def test_repository_composition_passes(self) -> None:
        self.assert_accepted()

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

    def test_missing_dispatcher_progress_watch_is_rejected(self) -> None:
        self.edit(
            MAIN,
            "local_argb::watch_progress(&notification_dispatch_progress, &telemetry)",
            "true",
        )
        self.assert_rejected("dispatcher progress watch is missing from vehicle integration")

    def test_dispatcher_progress_watch_after_telemetry_start_is_rejected(self) -> None:
        self.edit(
            MAIN,
            "local_argb::watch_progress(&notification_dispatch_progress, &telemetry)",
            "true",
        )
        self.edit(
            MAIN,
            '  ESP_LOGI(kTag, "strict listen-only CAN acquisition started',
            "  (void)local_argb::watch_progress(&notification_dispatch_progress, &telemetry);\n"
            '  ESP_LOGI(kTag, "strict listen-only CAN acquisition started',
        )
        self.assert_rejected("dispatcher progress watch does not precede telemetry/CAN startup")

    def test_supervisor_without_progress_fail_off_is_rejected(self) -> None:
        self.edit(
            Path("components/local_argb/src/local_argb_idf.cpp"),
            "    supervise_progress();\n",
            "",
        )
        self.assert_rejected("dispatcher-stall fail-off supervision is missing")

    def test_progress_stall_without_gated_fail_off_policy_is_rejected(self) -> None:
        self.edit(
            Path("components/local_argb/src/local_argb_idf.cpp"),
            "g_progress_fail_off.apply(sample_progress());",
            "(void)sample_progress();",
        )
        self.assert_rejected("dispatcher-stall fail-off policy is missing")

    def test_progress_stall_without_worker_report_is_rejected(self) -> None:
        self.edit(
            Path("components/local_argb/src/local_argb_idf.cpp"),
            "    report_progress();\n  }\n}",
            "  }\n}",
        )
        self.assert_rejected("worker-side progress stall logging is missing")

    def test_commented_out_engine_attach_is_rejected(self) -> None:
        self.edit(MAIN, "engine.attach()", "vehicle_signals::SignalStatus::Ok")
        self.edit(MAIN, BEFORE_CAN_START, "  // (void)engine.attach();\n" + BEFORE_CAN_START)
        self.assert_rejected("engine attachment is missing from vehicle integration")

    def test_setup_failure_without_fail_off_is_rejected(self) -> None:
        self.edit(
            MAIN,
            "    local_argb::fail_off();\n"
            '    ESP_LOGE(kTag, "engine LED action setup failed',
            '    ESP_LOGE(kTag, "engine LED action setup failed',
        )
        self.assert_rejected("setup failure path does not fail off")

    def test_braceless_setup_failure_return_is_rejected(self) -> None:
        self.edit(
            MAIN,
            BEFORE_CAN_START,
            "  if (application_state.turn_notifications != 0U)\n    return;\n" + BEFORE_CAN_START,
        )
        self.assert_rejected("setup failure path does not fail off")

    def test_commented_out_fail_off_is_rejected(self) -> None:
        self.edit(MAIN, ATTACH_FAILURE, "    // " + ATTACH_FAILURE.lstrip())
        self.assert_rejected("setup failure path does not fail off")

    def test_nested_block_in_failure_path_is_rejected(self) -> None:
        for label, nested in (
            ("nested block without fail_off", '    {\n      ESP_LOGE(kTag, "nested");\n    }\n'),
            (
                "conditional fail_off in a nested block",
                "    if (application_state.turn_notifications == 0U) {\n"
                "      local_argb::fail_off();\n"
                "    }\n",
            ),
        ):
            original = (self.root / MAIN).read_text(encoding="utf-8")
            with self.subTest(label):
                try:
                    self.edit(
                        MAIN,
                        ATTACH_FAILURE,
                        nested
                        + '    ESP_LOGE(kTag, "engine attachment failed; refusing to start CAN");\n',
                    )
                    self.assert_rejected("setup failure path does not fail off")
                finally:
                    (self.root / MAIN).write_text(original, encoding="utf-8")

    def test_telemetry_stop_without_fail_off_is_rejected(self) -> None:
        self.edit(MAIN, POLL_DELAY, POLL_DELAY + "    (void)telemetry.stop();\n")
        self.assert_rejected("telemetry.stop() is not followed by local_argb::fail_off()")

    def test_engine_detach_without_fail_off_is_rejected(self) -> None:
        self.edit(MAIN, POLL_DELAY, POLL_DELAY + "    (void)engine.detach();\n")
        self.assert_rejected("engine.detach() is not followed by local_argb::fail_off()")

    def test_detach_fail_off_only_on_failure_branch_is_rejected(self) -> None:
        self.edit(
            MAIN,
            POLL_DELAY,
            POLL_DELAY + "    if (engine.detach() != vehicle_signals::SignalStatus::Ok) {\n"
            "      local_argb::fail_off();\n"
            "      return;\n"
            "    }\n",
        )
        self.assert_rejected("engine.detach() is not followed by local_argb::fail_off()")

    def test_start_wrapped_in_helper_after_app_main_is_rejected(self) -> None:
        for label, call, helper, message in (
            (
                "renderer start",
                "if (!local_argb::start()) {",
                "bool start_renderer() noexcept { return local_argb::start(); }\n",
                "local_argb::start() is not called in app_main",
            ),
            (
                "telemetry start",
                "if (!telemetry.start().ok()) {",
                "bool start_renderer() noexcept { return telemetry.start().ok(); }\n",
                "telemetry.start() is not called in app_main",
            ),
        ):
            original = (self.root / MAIN).read_text(encoding="utf-8")
            with self.subTest(label):
                try:
                    self.edit(MAIN, call, "if (!start_renderer()) {")
                    path = self.root / MAIN
                    path.write_text(path.read_text(encoding="utf-8") + helper, encoding="utf-8")
                    self.assert_rejected(message)
                finally:
                    (self.root / MAIN).write_text(original, encoding="utf-8")

    def test_decoy_attach_string_before_start_is_rejected(self) -> None:
        self.edit(MAIN, "engine.attach()", "vehicle_signals::SignalStatus::Ok")
        self.edit(
            MAIN,
            BEFORE_CAN_START,
            '  ESP_LOGI(kTag, "engine.attach() skipped");\n' + BEFORE_CAN_START,
        )
        self.edit(
            MAIN,
            '  ESP_LOGI(kTag, "strict listen-only CAN acquisition started',
            "  (void)engine.attach();\n"
            '  ESP_LOGI(kTag, "strict listen-only CAN acquisition started',
        )
        self.assert_rejected("engine attachment does not precede telemetry/CAN startup")

    def test_fail_off_in_another_case_label_is_rejected(self) -> None:
        self.edit(
            MAIN,
            BEFORE_CAN_START,
            "  switch (application_state.turn_notifications) {\n"
            "  case 0U:\n"
            '    ESP_LOGW(kTag, "no turn notice yet");\n'
            "    local_argb::fail_off();\n"
            "    break;\n"
            "  default:\n"
            "    return;\n"
            "  }\n" + BEFORE_CAN_START,
        )
        self.assert_rejected("setup failure path does not fail off")

    def test_fail_off_in_the_returning_case_label_passes(self) -> None:
        self.edit(
            MAIN,
            BEFORE_CAN_START,
            "  switch (application_state.turn_notifications) {\n"
            "  case 0U:\n"
            "    break;\n"
            "  default:\n"
            "    local_argb::fail_off();\n"
            "    return;\n"
            "  }\n" + BEFORE_CAN_START,
        )
        self.assert_accepted()

    def test_nested_return_before_shutdown_fail_off_is_rejected(self) -> None:
        self.edit(
            MAIN,
            POLL_DELAY,
            POLL_DELAY + "    (void)telemetry.stop();\n"
            "    if (application_state.turn_notifications == 0U) {\n"
            "      return;\n"
            "    }\n"
            "    local_argb::fail_off();\n",
        )
        self.assert_rejected("telemetry.stop() is not followed by local_argb::fail_off()")

    def test_detach_followed_by_fail_off_passes(self) -> None:
        self.edit(
            MAIN,
            POLL_DELAY,
            POLL_DELAY + "    if (engine.detach() != vehicle_signals::SignalStatus::Ok) {\n"
            "      local_argb::fail_off();\n"
            "      return;\n"
            "    }\n"
            "    local_argb::fail_off();\n",
        )
        self.assert_accepted()

    def test_duplicate_turn_configuration_in_firmware_is_rejected(self) -> None:
        self.edit(
            MAIN,
            "bool configure_engine_lighting() noexcept {",
            "constexpr int kTurnRules[] = {1};\n"
            "bool configure_engine_lighting() noexcept {",
        )
        self.assert_rejected("vehicle integration retains duplicated turn rules")

    def test_missing_shared_profile_application_is_rejected(self) -> None:
        self.edit(
            MAIN,
            "controller_config::apply_lighting_profile(\n"
            "      controller_config::kDefaultLightingProfile, led_actions, engine);",
            "(void)led_actions;",
        )
        self.assert_rejected("must call controller_config::apply_lighting_profile()")

    def test_unreachable_shared_profile_application_is_rejected(self) -> None:
        self.edit(MAIN, "if (!configure_engine_lighting()) {", "if (false) {")
        self.assert_rejected("configure_engine_lighting() is not called in app_main")

    def test_direct_turn_rule_in_main_is_rejected(self) -> None:
        self.edit(
            MAIN,
            "  return true;\n}\n} // namespace",
            "  (void)engine.add_state_rule({});\n  return true;\n}\n} // namespace",
        )
        self.assert_rejected("vehicle integration retains direct turn-state rule registration")

    def test_direct_led_binding_in_main_is_rejected(self) -> None:
        self.edit(
            MAIN,
            "  return true;\n}\n} // namespace",
            "  (void)led_actions.bind(action_engine::ActionId{6}, "
            "local_argb_actions::LedEffect::Brake);\n"
            "  return true;\n}\n} // namespace",
        )
        self.assert_rejected("vehicle integration retains direct LED effect binding")

    def test_direct_sink_registration_in_main_is_rejected(self) -> None:
        self.edit(
            MAIN,
            "  return true;\n}\n} // namespace",
            "  (void)engine.add_sink(led_actions);\n"
            "  return true;\n}\n} // namespace",
        )
        self.assert_rejected("vehicle integration retains direct LED sink registration")

    def test_direct_event_rule_in_main_is_rejected(self) -> None:
        self.edit(
            MAIN,
            "  return true;\n}\n} // namespace",
            "  (void)engine.add_event_rule({});\n"
            "  return true;\n}\n} // namespace",
        )
        self.assert_rejected("vehicle integration retains direct event rule registration")

    def test_missing_engine_project_component_is_rejected(self) -> None:
        self.edit(PROJECT_CMAKE, '    "${CMAKE_CURRENT_LIST_DIR}/../../lib/action_engine"\n', "")
        self.assert_rejected("vehicle project does not select the lib/action_engine component")

    def test_missing_led_actions_requirement_is_rejected(self) -> None:
        self.edit(MAIN_CMAKE, " local_argb_actions", "")
        self.assert_rejected(
            "vehicle application does not require the local_argb_actions component"
        )

    def test_direct_range_rule_in_main_is_rejected(self) -> None:
        self.edit(
            MAIN,
            "  return true;\n}\n} // namespace",
            "  (void)engine.add_range_rule({});\n"
            "  return true;\n}\n} // namespace",
        )
        self.assert_rejected("vehicle integration retains direct RPM level rule registration")

    def test_direct_sampled_state_rule_in_main_is_rejected(self) -> None:
        self.edit(
            MAIN,
            "  return true;\n}\n} // namespace",
            "  (void)engine.add_sampled_state_rule({});\n"
            "  return true;\n}\n} // namespace",
        )
        self.assert_rejected("vehicle integration retains direct RPM threshold rule registration")

    def test_missing_polled_sampling_is_rejected(self) -> None:
        self.edit(MAIN, "    (void)engine.sample_polled_rules();\n", "")
        self.assert_rejected("the runtime loop after telemetry startup does not call")

    def test_commented_out_polled_sampling_is_rejected(self) -> None:
        self.edit(
            MAIN,
            "    (void)engine.sample_polled_rules();\n",
            "    // (void)engine.sample_polled_rules();\n",
        )
        self.assert_rejected("the runtime loop after telemetry startup does not call")

    def test_polled_sampling_outside_the_runtime_loop_is_rejected(self) -> None:
        self.edit(MAIN, "    (void)engine.sample_polled_rules();\n", "")
        self.edit(
            MAIN,
            "  for (;;) {\n",
            "  (void)engine.sample_polled_rules();\n  for (;;) {\n",
        )
        self.assert_rejected("the runtime loop after telemetry startup does not call")

    def test_missing_controller_config_project_component_is_rejected(self) -> None:
        self.edit(
            PROJECT_CMAKE, '    "${CMAKE_CURRENT_LIST_DIR}/../../components/controller_config"\n', ""
        )
        self.assert_rejected(
            "vehicle project does not select the components/controller_config component"
        )

    def test_missing_controller_config_requirement_is_rejected(self) -> None:
        self.edit(MAIN_CMAKE, " controller_config", "")
        self.assert_rejected("vehicle application does not require the controller_config component")


if __name__ == "__main__":
    unittest.main()
