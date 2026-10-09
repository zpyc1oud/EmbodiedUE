"""Configuration-only callers must not load the policy-learning stack."""

import os
import subprocess
import sys
from pathlib import Path


def test_runtime_configuration_does_not_import_policy_learning() -> None:
    root = Path(__file__).resolve().parents[2]
    code = """
import sys
from pathlib import Path
from uerl.training import build_launch_overrides, build_run_config
config = build_run_config('UERL-PhantomX-Walk-v0', overrides={'worker.slot_count': '64'})
assert config.worker.slot_count == 64
launch = build_launch_overrides(ue_executable=Path('UE.exe'), project=Path('Host.uproject'),
                               map_name='/Game/Maps/NewMap', port=60001)
assert launch['session.port'] == '60001'
assert launch['session.map_path'] == '/Game/Maps/NewMap'
assert 'rsl_rl' not in sys.modules
assert 'tensorboard' not in sys.modules
from uerl.training import run_training, run_evaluation
from uerl.training import runner
assert run_training is runner.run_training
assert run_evaluation is runner.run_evaluation
assert 'rsl_rl' in sys.modules
"""
    result = subprocess.run(
        [sys.executable, "-c", code], cwd=root,
        env={**os.environ, "PYTHONPATH": str(root / "src")},
        capture_output=True, text=True, timeout=90,
    )
    assert result.returncode == 0, result.stdout + result.stderr
