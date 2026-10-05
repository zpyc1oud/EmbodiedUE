"""Keep the checked-in external CartPole package's YAML resource working."""

from __future__ import annotations

import importlib
import sys
from pathlib import Path

import pytest

from uerl.tasks.cartpole.registration import create_cartpole_task_config


def test_external_cartpole_example_loads_its_yaml_reward(monkeypatch: pytest.MonkeyPatch) -> None:
    package_root = Path(__file__).resolve().parents[3] / "examples" / "external-cartpole" / "src"
    monkeypatch.syspath_prepend(str(package_root))
    sys.modules.pop("example_cartpole", None)
    try:
        external = importlib.import_module("example_cartpole")
        config = external.create_task_config()
    finally:
        sys.modules.pop("example_cartpole", None)

    defaults = create_cartpole_task_config()
    assert config.rew_scale_pole_pos == -2.0
    assert config.rew_scale_alive == defaults.rew_scale_alive
    assert config.rew_scale_terminated == defaults.rew_scale_terminated
    assert config.rew_scale_cart_vel == defaults.rew_scale_cart_vel
    assert config.rew_scale_pole_vel == defaults.rew_scale_pole_vel
