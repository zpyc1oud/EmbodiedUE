"""An independently installed CartPole reward variant using the shared runtime."""

from dataclasses import replace
from importlib.resources import files

from uerl.core.config.yaml_loader import load_unique_yaml
from uerl.tasks.cartpole import CartPoleTaskConfig
from uerl.tasks.cartpole.registration import create_cartpole_registration, create_cartpole_task_config
from uerl.tasks.registry import TaskRegistration


def create_task_config() -> CartPoleTaskConfig:
    """Change one reward weight; retain CartPole action and observation semantics."""

    weights = load_unique_yaml(files(__package__).joinpath("reward.yaml").read_text(encoding="utf-8"))
    weight = weights.get("pole_position_weight") if isinstance(weights, dict) else None
    if not isinstance(weight, (int, float)) or isinstance(weight, bool):
        raise ValueError("reward.yaml must define a numeric pole_position_weight")
    return replace(create_cartpole_task_config(), rew_scale_pole_pos=float(weight))


def create_registration() -> TaskRegistration:
    """Return a fresh declaration; factory execution does not start UE."""

    return replace(
        create_cartpole_registration(),
        task_id="Example-CartPole-v0",
        task_version="0.1.0",
        task_config_factory=create_task_config,
    )
