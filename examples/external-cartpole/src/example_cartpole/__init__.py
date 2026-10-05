"""An independently installed CartPole reward variant using the shared runtime."""

import json
from dataclasses import replace
from importlib.resources import files

from uerl.tasks.cartpole import CartPoleTaskConfig
from uerl.tasks.cartpole.registration import create_cartpole_registration, create_cartpole_task_config
from uerl.tasks.registry import TaskRegistration


def create_task_config() -> CartPoleTaskConfig:
    """Change one reward weight; retain CartPole action and observation semantics."""

    weights = json.loads(files(__package__).joinpath("reward.json").read_text(encoding="utf-8"))
    return replace(create_cartpole_task_config(), rew_scale_pole_pos=float(weights["pole_position_weight"]))


def create_registration() -> TaskRegistration:
    """Return a fresh declaration; factory execution does not start UE."""

    return replace(
        create_cartpole_registration(),
        task_id="Example-CartPole-v0",
        task_version="0.1.0",
        task_config_factory=create_task_config,
    )
