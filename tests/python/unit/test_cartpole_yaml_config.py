"""Verify CartPole training semantics are loaded from the dedicated YAML folder."""

from __future__ import annotations

from pathlib import Path

import pytest

from uerl import ConfigError
from uerl.runtime.session.worker import _build_initialize_request
from uerl.tasks.cartpole import (
    CARTPOLE_ENVIRONMENT_ID,
    CARTPOLE_ROBOT_ID,
    CARTPOLE_TASK_ID,
    DEFAULT_CARTPOLE_TRAINING_CONFIG,
    load_cartpole_training_config,
    parse_cartpole_training_config,
)
from uerl.training import build_rsl_rl_train_config, build_run_config


def test_default_cartpole_config_is_in_dedicated_task_folder() -> None:
    """Load canonical Task/Worker/runner values from configs/tasks/cartpole."""

    config = load_cartpole_training_config()

    assert DEFAULT_CARTPOLE_TRAINING_CONFIG == Path("src/uerl/configs/tasks/cartpole/training.yaml").resolve()
    assert config.worker.environment_id == CARTPOLE_ENVIRONMENT_ID
    assert config.worker.robot_id == CARTPOLE_ROBOT_ID
    assert config.worker.robot_asset_path == "/Game/Robots/CartPole/SKM_CartPole"
    assert config.worker.robot_config_path == Path("robots/cartpole/robot.yaml")
    assert config.robot_config.actuators
    assert config.robot_config.reset.distributions[2].wire_key == "joint_position:pole"


def test_cartpole_yaml_merges_with_reflected_topology() -> None:
    config = load_cartpole_training_config()
    topology = {
        "asset_path": "/Game/Robots/CartPole/SKM_CartPole",
        "body_names": ["base", "cart", "pole"],
        "body_motion_types": ["kinematic", "simulated", "simulated"],
        "root_body_index": 0,
        "fixed_base": True,
        "joints": [
            {
                "name": "cart",
                "parent_body_index": 0,
                "child_body_index": 1,
                "degrees_of_freedom": 1,
                "coordinate": "linear_x",
                "coordinate_type": "prismatic",
                "unit": "m",
                "default_position": 0.0,
                "lower_limit": None,
                "upper_limit": None,
                "child_frame": {
                    "position_metres": [0.0, 0.0, 0.0],
                    "rotation_xyzw": [0.0, 0.0, 0.0, 1.0],
                },
                "parent_frame": {
                    "position_metres": [0.0, 0.0, 0.0],
                    "rotation_xyzw": [0.0, 0.0, 0.0, 1.0],
                },
            },
            {
                "name": "pole",
                "parent_body_index": 1,
                "child_body_index": 2,
                "degrees_of_freedom": 1,
                "coordinate": "twist",
                "coordinate_type": "revolute",
                "unit": "rad",
                "default_position": 0.0,
                "lower_limit": -3.14,
                "upper_limit": 3.14,
                "child_frame": {
                    "position_metres": [0.0, 0.0, 0.1],
                    "rotation_xyzw": [0.0, 0.0, 0.0, 1.0],
                },
                "parent_frame": {
                    "position_metres": [0.0, 0.0, 0.1],
                    "rotation_xyzw": [0.0, 0.0, 0.0, 1.0],
                },
            },
        ],
    }

    spec = config.resolve_robot_spec(topology)

    assert (spec.actuators[0].joint, spec.actuators[0].joint_index) == ("cart", 0)
    assert spec.actuators[0].coordinate_type == "prismatic"
    assert spec.actuators[0].target_unit == "N"
    assert [(item.index, item.target, item.joint_index, item.unit) for item in spec.observations] == [
        (0, "pole", 1, "rad"),
        (1, "pole", 1, "rad/s"),
        (2, "cart", 0, "m"),
        (3, "cart", 0, "m/s"),
    ]
    assert [item.wire_key for item in spec.reset] == [
        "joint_position:cart",
        "joint_velocity:cart",
        "joint_position:pole",
        "joint_velocity:pole",
    ]


def test_cartpole_yaml_identity_and_robot_reset_stays_python_owned() -> None:
    """Keep reset distributions in the Python-owned RobotSpec projection."""

    resolved = build_run_config(CARTPOLE_TASK_ID)
    request = _build_initialize_request(
        resolved,
        {"state_requirements": [], "action_schema": []},
    )

    worker = request["worker_config"]
    robot = worker["robot"]
    assert robot["id"] == CARTPOLE_ROBOT_ID
    assert robot["scalars"] == {}
    assert robot["asset_path"] == "/Game/Robots/CartPole/SKM_CartPole"
    assert "reset_distributions" not in robot
    assert "reset_distributions" not in worker["environment"]
    assert "safety" not in worker


def test_cartpole_runner_settings_are_consumed_without_hardcoded_defaults() -> None:
    """Translate every RSL-RL setting from YAML into the runner config."""

    resolved = build_run_config(CARTPOLE_TASK_ID)
    train_config = build_rsl_rl_train_config(resolved)

    assert train_config["obs_groups"] == {"actor": ["policy"], "critic": ["policy"]}
    assert train_config["actor"]["class_name"] == "rsl_rl.models.mlp_model:MLPModel"
    assert train_config["critic"]["class_name"] == "rsl_rl.models.mlp_model:MLPModel"
    assert train_config["algorithm"]["class_name"] == "rsl_rl.algorithms.ppo:PPO"
    assert train_config["check_for_nan"] is True
    assert train_config["algorithm"]["rnd_cfg"] is None
    assert train_config["algorithm"]["symmetry_cfg"] is None


def test_cartpole_yaml_rejects_unknown_task_key() -> None:
    """Keep the dedicated file strict so C++/Python drift fails at the boundary."""

    document = """
identity:
  task_id: UERL-CartPole-Direct-v0
  task_version: 1.0.0
  environment_id: uerl.environment.shared_world
  robot_id: uerl.robot.skeletal_mesh
robot:
  asset_path: /Game/Robots/CartPole/SKM_CartPole
  config_path: robots/cartpole/robot.yaml
task:

  max_episode_steps: 300
  slot_fault_reward: -2.0
  max_cart_position: 3.0
  pole_angle_limit: 1.57
  action_clip: 1.0
  reward:
    alive: 1.0
    terminated: -2.0
    pole_position: -1.0
    cart_velocity: -0.01
    pole_velocity: -0.005
  typo: true
worker: {}
runner: {}
"""

    with pytest.raises(ConfigError) as raised:
        parse_cartpole_training_config(document)

    assert raised.value.code == "CONFIG_UNKNOWN_KEY"
    assert raised.value.path == "task.typo"


def test_cartpole_yaml_rejects_duplicate_keys() -> None:
    """Reject duplicate YAML keys instead of silently taking the last value."""

    document = """
identity:
  task_id: UERL-CartPole-Direct-v0
  task_id: wrong
"""

    with pytest.raises(ConfigError) as raised:
        parse_cartpole_training_config(document)

    assert raised.value.code == "CONFIG_INVALID_YAML"
    assert raised.value.path == "<string>"


def test_cartpole_yaml_rejects_unregistered_robot_asset_reference() -> None:
    """Resolve Robot semantics only from the built-in asset declaration registry."""

    document = DEFAULT_CARTPOLE_TRAINING_CONFIG.read_text(encoding="utf-8").replace(
        "config_path: robots/cartpole/robot.yaml", 'config_path: "C:/outside/robot.yaml"'
    )

    with pytest.raises(ConfigError) as raised:
        parse_cartpole_training_config(document, source=str(DEFAULT_CARTPOLE_TRAINING_CONFIG))

    assert raised.value.code == "ROBOT_ASSET_NOT_FOUND"
    assert raised.value.path == "robot.config_path"


@pytest.mark.parametrize(
    ("injected", "path"),
    [
        ("    reset_distributions: {}\n", "worker.environment.reset_distributions"),
        ("  safety:\n    finite_fallback: true\n", "worker.safety"),
    ],
)
def test_cartpole_yaml_rejects_removed_worker_config(injected: str, path: str) -> None:
    """Reject removed config surfaces instead of silently accepting them."""

    document = DEFAULT_CARTPOLE_TRAINING_CONFIG.read_text(encoding="utf-8").replace(
        "      environment.trace_depth_m: 20.0\n",
        "      environment.trace_depth_m: 20.0\n" + injected,
    )

    with pytest.raises(ConfigError) as raised:
        parse_cartpole_training_config(document, source=str(DEFAULT_CARTPOLE_TRAINING_CONFIG))

    assert raised.value.code == "CONFIG_UNKNOWN_KEY"
    assert raised.value.path == path


def test_cartpole_yaml_rejects_uint64_overflow_seed() -> None:
    """Reject a seed that UE's ReadU64 cannot consume."""

    document = DEFAULT_CARTPOLE_TRAINING_CONFIG.read_text(encoding="utf-8").replace(
        "run_seed: 0", "run_seed: 18446744073709551616"
    )

    with pytest.raises(ConfigError) as raised:
        parse_cartpole_training_config(document, source=str(DEFAULT_CARTPOLE_TRAINING_CONFIG))

    assert raised.value.code == "CONFIG_OUT_OF_RANGE"
    assert raised.value.path == "worker.run_seed"


def test_cartpole_yaml_rejects_non_integer_shared_world_columns() -> None:
    """Shared-world columns must be an integer Slot-grid count."""

    document = DEFAULT_CARTPOLE_TRAINING_CONFIG.read_text(encoding="utf-8").replace(
        "environment.columns: 8", "environment.columns: 8.5"
    )

    with pytest.raises(ConfigError) as raised:
        parse_cartpole_training_config(document, source=str(DEFAULT_CARTPOLE_TRAINING_CONFIG))

    assert raised.value.code == "CONFIG_OUT_OF_RANGE"
    assert raised.value.path == "worker.environment.scalars.columns"


@pytest.mark.parametrize(
    ("field", "value", "path", "code"),
    [
        ("slot_count", "65537", "worker.slot_count", "CONFIG_OUT_OF_RANGE"),
        ("physics_dt", "1.1", "worker.physics_dt", "CONFIG_OUT_OF_RANGE"),
        ("decimation", "[1025, 1025]", "worker.decimation", "CONFIG_OUT_OF_RANGE"),
    ],
)
def test_cartpole_yaml_matches_ue_worker_bounds(field: str, value: str, path: str, code: str) -> None:
    """Reject values that UE ParseWorkerConfig cannot consume."""

    original_values = {
        "slot_count": "64",
        "physics_dt": "0.00833333333333333",
            "decimation": "[2, 2]",
    }
    document = DEFAULT_CARTPOLE_TRAINING_CONFIG.read_text(encoding="utf-8").replace(
        f"  {field}: {original_values[field]}",
        f"  {field}: {value}",
    )

    with pytest.raises(ConfigError) as raised:
        parse_cartpole_training_config(document, source=str(DEFAULT_CARTPOLE_TRAINING_CONFIG))

    assert raised.value.code == code
    assert raised.value.path == path


@pytest.mark.parametrize(
    ("field", "value", "path"),
    [
        ("physics_dt", "0.0166666666666667", "worker.physics_dt"),
            ("decimation", "[1, 1]", "worker.decimation"),
        ("max_episode_steps", "150", "task.max_episode_steps"),
    ],
)
def test_cartpole_yaml_requires_fixed_training_step_baseline(field: str, value: str, path: str) -> None:
    """Keep the five-second, 60 Hz Cart-Pole baseline explicit at the config boundary."""

    original_values = {
        "physics_dt": "0.00833333333333333",
            "decimation": "[2, 2]",
        "max_episode_steps": "300",
    }
    document = DEFAULT_CARTPOLE_TRAINING_CONFIG.read_text(encoding="utf-8").replace(
        f"  {field}: {original_values[field]}",
        f"  {field}: {value}",
    )

    with pytest.raises(ConfigError) as raised:
        parse_cartpole_training_config(document, source=str(DEFAULT_CARTPOLE_TRAINING_CONFIG))

    assert raised.value.code == "CONFIG_OUT_OF_RANGE"
    assert raised.value.path == path


@pytest.mark.parametrize(
    ("replacement", "path", "code"),
    [
        ("pole_angle_limit: 0.0", "task.pole_angle_limit", "CONFIG_OUT_OF_RANGE"),
        ("pole_angle_limit: .nan", "task.pole_angle_limit", "CONFIG_NOT_FINITE"),
        ("max_cart_position: -1.0", "task.max_cart_position", "CONFIG_OUT_OF_RANGE"),
        ("action_clip: 0", "task.action_clip", "CONFIG_OUT_OF_RANGE"),
        ("slot_fault_reward: .inf", "task.slot_fault_reward", "CONFIG_NOT_FINITE"),
        ("learning_rate: -1", "runner.parameters.learning_rate", "CONFIG_OUT_OF_RANGE"),
        ("gamma: 1.5", "runner.parameters.gamma", "CONFIG_OUT_OF_RANGE"),
        ("lam: -0.1", "runner.parameters.lam", "CONFIG_OUT_OF_RANGE"),
        ("hidden_dims: []", "runner.parameters.hidden_dims", "CONFIG_OUT_OF_RANGE"),
        ("hidden_dims: [32, 0]", "runner.parameters.hidden_dims[1]", "CONFIG_OUT_OF_RANGE"),
        ("hidden_dims: 32", "runner.parameters.hidden_dims", "CONFIG_TYPE_MISMATCH"),
        ("device: ''", "runner.device", "CONFIG_OUT_OF_RANGE"),
        ("obs_normalization: 1", "runner.parameters.obs_normalization", "CONFIG_TYPE_MISMATCH"),
        ("check_for_nan: 'true'", "runner.parameters.check_for_nan", "CONFIG_TYPE_MISMATCH"),
        ("rnd_cfg: {}", "runner.parameters.rnd_cfg", "CONFIG_OUT_OF_RANGE"),
        ("symmetry_cfg: false", "runner.parameters.symmetry_cfg", "CONFIG_OUT_OF_RANGE"),
        ("init_std: 0", "runner.parameters.init_std", "CONFIG_OUT_OF_RANGE"),
        ("clip_param: -0.1", "runner.parameters.clip_param", "CONFIG_OUT_OF_RANGE"),
        ("save_interval: 0", "runner.parameters.save_interval", "CONFIG_OUT_OF_RANGE"),
        ("entropy_coef: .nan", "runner.parameters.entropy_coef", "CONFIG_NOT_FINITE"),
        ("rollout_length: 0", "runner.rollout_length", "CONFIG_OUT_OF_RANGE"),
        ("max_iterations: -1", "runner.max_iterations", "CONFIG_OUT_OF_RANGE"),
        ("experiment_name: ''", "runner.parameters.experiment_name", "CONFIG_OUT_OF_RANGE"),
        ("run_name: ''", "runner.parameters.run_name", "CONFIG_OUT_OF_RANGE"),
        ("config_path: ''", "robot.config_path", "CONFIG_OUT_OF_RANGE"),
        (
            "task_id: UERL-CartPole-Direct-v1",
            "identity.task_id",
            "CONFIG_OUT_OF_RANGE",
        ),
        (
            "asset_path: /Engine/NotGame",
            "robot.asset_path",
            "CONFIG_OUT_OF_RANGE",
        ),
    ],
)
def test_cartpole_yaml_rejection_paths(replacement: str, path: str, code: str) -> None:
    """Cover former hand-validator rejection paths with generic configspec codes."""

    original_keys = {
        "pole_angle_limit: 0.0": "pole_angle_limit: 1.5707963267948966",
        "pole_angle_limit: .nan": "pole_angle_limit: 1.5707963267948966",
        "max_cart_position: -1.0": "max_cart_position: 3.0",
        "action_clip: 0": "action_clip: 1.0",
        "slot_fault_reward: .inf": "slot_fault_reward: -2.0",
        "learning_rate: -1": "learning_rate: 0.001",
        "gamma: 1.5": "gamma: 0.99",
        "lam: -0.1": "lam: 0.95",
        "hidden_dims: []": "hidden_dims: [32, 32]",
        "hidden_dims: [32, 0]": "hidden_dims: [32, 32]",
        "hidden_dims: 32": "hidden_dims: [32, 32]",
        "device: ''": "device: cpu",
        "obs_normalization: 1": "obs_normalization: false",
        "check_for_nan: 'true'": "check_for_nan: true",
        "rnd_cfg: {}": "rnd_cfg: null",
        "symmetry_cfg: false": "symmetry_cfg: null",
        "init_std: 0": "init_std: 1.0",
        "clip_param: -0.1": "clip_param: 0.2",
        "save_interval: 0": "save_interval: 50",
        "entropy_coef: .nan": "entropy_coef: 0.01",
        "rollout_length: 0": "rollout_length: 16",
        "max_iterations: -1": "max_iterations: 150",
        "experiment_name: ''": "experiment_name: cartpole_direct",
        "run_name: ''": "run_name: cartpole",
        "config_path: ''": "config_path: robots/cartpole/robot.yaml",
        "task_id: UERL-CartPole-Direct-v1": "task_id: UERL-CartPole-Direct-v0",
        "asset_path: /Engine/NotGame": "asset_path: /Game/Robots/CartPole/SKM_CartPole",
    }
    document = DEFAULT_CARTPOLE_TRAINING_CONFIG.read_text(encoding="utf-8").replace(
        original_keys[replacement],
        replacement,
    )

    with pytest.raises(ConfigError) as raised:
        parse_cartpole_training_config(document, source=str(DEFAULT_CARTPOLE_TRAINING_CONFIG))

    assert raised.value.code == code
    assert raised.value.path == path


def test_cartpole_yaml_rejects_missing_config_file(tmp_path: Path) -> None:
    missing = tmp_path / "no_such_cartpole_training.yaml"
    with pytest.raises(ConfigError) as raised:
        load_cartpole_training_config(missing)
    assert raised.value.code == "CONFIG_NOT_FOUND"
    assert raised.value.path == str(missing)


def test_cartpole_yaml_rejects_root_non_mapping() -> None:
    with pytest.raises(ConfigError) as raised:
        parse_cartpole_training_config("[]\n", source="cartpole.yaml")
    assert raised.value.code == "CONFIG_NOT_MAPPING"


def test_cartpole_yaml_rejects_root_unknown_key() -> None:
    document = DEFAULT_CARTPOLE_TRAINING_CONFIG.read_text(encoding="utf-8") + "\nextra: 1\n"
    with pytest.raises(ConfigError) as raised:
        parse_cartpole_training_config(document, source=str(DEFAULT_CARTPOLE_TRAINING_CONFIG))
    assert raised.value.code == "CONFIG_UNKNOWN_KEY"
    assert raised.value.path == "extra"


def test_cartpole_yaml_rejects_missing_reward_alive() -> None:
    document = DEFAULT_CARTPOLE_TRAINING_CONFIG.read_text(encoding="utf-8").replace(
        "    alive: 1.0\n",
        "",
    )
    with pytest.raises(ConfigError) as raised:
        parse_cartpole_training_config(document, source=str(DEFAULT_CARTPOLE_TRAINING_CONFIG))
    assert raised.value.code == "CONFIG_MISSING_FIELD"
    assert raised.value.path == "task.reward.alive"


def test_cartpole_yaml_rejects_missing_rnd_cfg() -> None:
    document = DEFAULT_CARTPOLE_TRAINING_CONFIG.read_text(encoding="utf-8").replace(
        "    rnd_cfg: null\n",
        "",
    )
    with pytest.raises(ConfigError) as raised:
        parse_cartpole_training_config(document, source=str(DEFAULT_CARTPOLE_TRAINING_CONFIG))
    assert raised.value.code == "CONFIG_MISSING_FIELD"
    assert raised.value.path == "runner.parameters.rnd_cfg"


def test_cartpole_yaml_rejects_blank_obs_group_name() -> None:
    document = DEFAULT_CARTPOLE_TRAINING_CONFIG.read_text(encoding="utf-8").replace(
        "      actor: [policy]",
        "      actor: ['']",
    )
    with pytest.raises(ConfigError) as raised:
        parse_cartpole_training_config(document, source=str(DEFAULT_CARTPOLE_TRAINING_CONFIG))
    assert raised.value.code == "CONFIG_OUT_OF_RANGE"
    assert raised.value.path == "runner.parameters.obs_groups.actor[0]"


def test_cartpole_yaml_rejects_empty_obs_group() -> None:
    document = DEFAULT_CARTPOLE_TRAINING_CONFIG.read_text(encoding="utf-8").replace(
        "      actor: [policy]",
        "      actor: []",
    )
    with pytest.raises(ConfigError) as raised:
        parse_cartpole_training_config(document, source=str(DEFAULT_CARTPOLE_TRAINING_CONFIG))
    assert raised.value.code == "CONFIG_OUT_OF_RANGE"
    assert raised.value.path == "runner.parameters.obs_groups.actor"
