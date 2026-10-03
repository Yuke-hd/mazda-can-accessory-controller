"""Parity check for the reviewed lock-state controller-config example."""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[2]
COMPILER = REPOSITORY / "tools" / "compile_controller_config.py"
EXAMPLES = REPOSITORY / "docs" / "specs" / "configuration" / "examples"
LOCK_STATE_YAML = EXAMPLES / "lock-state-actions-v1.yaml"
LOCK_STATE_JSON = EXAMPLES / "lock-state-actions-v1.json"


class LockStateExampleTests(unittest.TestCase):
    def test_lock_state_yaml_matches_canonical_json_example(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output_path = Path(directory) / "lock-state-actions.json"
            result = subprocess.run(
                [sys.executable, str(COMPILER), str(LOCK_STATE_YAML), str(output_path)],
                cwd=REPOSITORY,
                check=False,
                capture_output=True,
                text=True,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(
                json.loads(output_path.read_text(encoding="utf-8")),
                json.loads(LOCK_STATE_JSON.read_text(encoding="utf-8")),
            )


if __name__ == "__main__":
    unittest.main()
