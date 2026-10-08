"""Reject-path coverage for PhantomX training and terrain configspec migration."""

from __future__ import annotations

from pathlib import Path

import pytest

from uerl import ConfigError
from uerl.core.config.terrain_spec import parse_terrain_config
from uerl.tasks.phantomx.config import (
    DEFAULT_PHANTOMX_TRAINING_CONFIG,
    load_phantomx_discrete_terrain_config,
    load_phantomx_terrain_config,
    load_phantomx_training_config,
    parse_phantomx_training_config,
)


def test_terrain_parser_materializes_walkable_collision_default() -> None:
    """Canonical terrain config includes the default collision policy."""

    parsed = parse_terrain_config(
        {
            "num_levels": 1,
            "cell_size": [2.0, 2.0],
            "border_width": 0.0,
            "tiers": [
                {
                    "level": 0,
                    "primitive": "plane",
                    "seed": "0",
                    "platform_width": 0.0,
                    "params": {},
                }
            ],
        }
    )

    assert parsed["physics_collision"] is True


@pytest.mark.parametrize(
    ("replacement", "path", "code"),
    [
        ("learning_rate: -1", "runner.parameters.learning_rate", "CONFIG_OUT_OF_RANGE"),
        ("rollout_length: 0", "runner.rollout_length", "CONFIG_OUT_OF_RANGE"),
        ("init_std: 0", "runner.parameters.init_std", "CONFIG_OUT_OF_RANGE"),
        ("device: ''", "runner.device", "CONFIG_OUT_OF_RANGE"),
        ("initial_speed_min: -0.1", "task.command.initial_speed_min", "CONFIG_OUT_OF_RANGE"),
        ("promotion_success_rate: 1.5", "task.curriculum.promotion_success_rate", "CONFIG_OUT_OF_RANGE"),
        ("action_clip: 0", "task.action_clip", "CONFIG_OUT_OF_RANGE"),
        ("spacing_x_m: 0", "worker.environment.scalars.spacing_x_m", "CONFIG_OUT_OF_RANGE"),
        ("hidden_dims: []", "runner.parameters.hidden_dims", "CONFIG_OUT_OF_RANGE"),
        ("obs_normalization: 1", "runner.parameters.obs_normalization", "CONFIG_TYPE_MISMATCH"),
    ],
)
def test_phantomx_yaml_rejection_paths(replacement: str, path: str, code: str) -> None:
    originals = {
        "learning_rate: -1": "learning_rate: 0.0003",
        "rollout_length: 0": "rollout_length: 40",
        "init_std: 0": "init_std: 0.5",
        "device: ''": "device: cuda:0",
        "initial_speed_min: -0.1": "initial_speed_min: 0.05",
        "promotion_success_rate: 1.5": "promotion_success_rate: 0.6",
        "action_clip: 0": "action_clip: 1.0",
        "spacing_x_m: 0": "environment.spacing_x_m: 1.5",
        "hidden_dims: []": "hidden_dims: [128, 128, 128]",
        "obs_normalization: 1": "obs_normalization: true",
    }
    # spacing rewrite uses field name after prepare; inject via dotted YAML key.
    if replacement == "spacing_x_m: 0":
        document = DEFAULT_PHANTOMX_TRAINING_CONFIG.read_text(encoding="utf-8").replace(
            originals[replacement],
            "environment.spacing_x_m: 0",
        )
    else:
        document = DEFAULT_PHANTOMX_TRAINING_CONFIG.read_text(encoding="utf-8").replace(
            originals[replacement],
            replacement,
        )

    with pytest.raises(ConfigError) as raised:
        parse_phantomx_training_config(document, source=str(DEFAULT_PHANTOMX_TRAINING_CONFIG))

    assert raised.value.code == code
    assert raised.value.path == path


def test_phantomx_yaml_rejects_inverted_command_range() -> None:
    document = DEFAULT_PHANTOMX_TRAINING_CONFIG.read_text(encoding="utf-8").replace(
        "initial_speed_min: 0.05",
        "initial_speed_min: 0.9",
    ).replace(
        "initial_speed_max: 0.5",
        "initial_speed_max: 0.5",
    )
    with pytest.raises(ConfigError) as raised:
        parse_phantomx_training_config(document, source=str(DEFAULT_PHANTOMX_TRAINING_CONFIG))
    assert raised.value.code == "CONFIG_OUT_OF_RANGE"
    assert raised.value.path == "task.command"


def test_phantomx_yaml_rejects_curriculum_window_order() -> None:
    document = DEFAULT_PHANTOMX_TRAINING_CONFIG.read_text(encoding="utf-8").replace(
        "minimum_episodes: 128",
        "minimum_episodes: 600",
    )
    with pytest.raises(ConfigError) as raised:
        parse_phantomx_training_config(document, source=str(DEFAULT_PHANTOMX_TRAINING_CONFIG))
    assert raised.value.code == "CONFIG_OUT_OF_RANGE"
    assert raised.value.path == "task.curriculum.minimum_episodes"


def test_phantomx_yaml_rejects_unknown_task_key() -> None:
    document = DEFAULT_PHANTOMX_TRAINING_CONFIG.read_text(encoding="utf-8").replace(
        "  action_clip: 1.0\n",
        "  action_clip: 1.0\n  typo: true\n",
    )
    with pytest.raises(ConfigError) as raised:
        parse_phantomx_training_config(document, source=str(DEFAULT_PHANTOMX_TRAINING_CONFIG))
    assert raised.value.code == "CONFIG_UNKNOWN_KEY"
    assert raised.value.path == "task.typo"


def test_phantomx_yaml_rejects_root_extra_and_missing_file(tmp_path: Path) -> None:
    document = DEFAULT_PHANTOMX_TRAINING_CONFIG.read_text(encoding="utf-8") + "\nextra: 1\n"
    with pytest.raises(ConfigError) as raised:
        parse_phantomx_training_config(document, source=str(DEFAULT_PHANTOMX_TRAINING_CONFIG))
    assert raised.value.code == "CONFIG_UNKNOWN_KEY"
    assert raised.value.path == "extra"

    missing = tmp_path / "missing_phantomx.yaml"
    with pytest.raises(ConfigError) as missing_raised:
        load_phantomx_training_config(missing)
    assert missing_raised.value.code == "CONFIG_NOT_FOUND"


def test_phantomx_terrain_rejects_inverted_noise_range(tmp_path: Path) -> None:
    path = tmp_path / "bad_terrain.yaml"
    path.write_text(
        "num_levels: 1\n"
        "cell_size: [2.0, 2.0]\n"
        "border_width: 0.0\n"
        "tiers:\n"
        "  - level: 0\n"
        "    primitive: heightfield\n"
        "    seed: '1'\n"
        "    platform_width: 0.5\n"
        "    params:\n"
        "      noise_range: [0.2, -0.1]\n"
        "      noise_step: 0.01\n"
        "      horizontal_scale: 0.4\n"
        "      vertical_scale: 0.01\n"
        "      downsampled_scale: 0.9\n",
        encoding="utf-8",
    )
    with pytest.raises(ConfigError) as raised:
        load_phantomx_terrain_config(path)
    assert raised.value.code == "CONFIG_OUT_OF_RANGE"
    assert raised.value.path == "tiers[0].params.noise_range"


def test_phantomx_discrete_terrain_rejects_inverted_grid_height_range(tmp_path: Path) -> None:
    path = tmp_path / "bad_boxes.yaml"
    path.write_text(
        "num_levels: 1\n"
        "cell_size: [2.0, 2.0]\n"
        "border_width: 0.0\n"
        "tiers:\n"
        "  - level: 0\n"
        "    primitive: boxes\n"
        "    seed: '1'\n"
        "    platform_width: 0.5\n"
        "    params:\n"
        "      grid_width: 0.4\n"
        "      grid_height_range: [0.2, 0.01]\n"
        "      holes: false\n"
        "      generator: random_grid\n"
        "      difficulty: 0.0\n",
        encoding="utf-8",
    )
    with pytest.raises(ConfigError) as raised:
        load_phantomx_discrete_terrain_config(path)
    assert raised.value.code == "CONFIG_OUT_OF_RANGE"
    assert raised.value.path == "tiers[0].params.grid_height_range"


@pytest.mark.parametrize(("old", "new", "path"), [
    ("turn_in_place_probability: 0.2", "turn_in_place_probability: 1.1", "task.command.turn_in_place_probability"),
    ("yaw_rate_error_threshold: 0.25", "yaw_rate_error_threshold: 0", "task.curriculum.yaw_rate_error_threshold"),
    ("standing_velocity_error_threshold: 0.03", "standing_velocity_error_threshold: 0.3",
     "task.curriculum.standing_velocity_error_threshold"),
])
def test_learning_quality_thresholds_reject_invalid_configuration(old: str, new: str, path: str) -> None:
    text = DEFAULT_PHANTOMX_TRAINING_CONFIG.read_text(encoding="utf-8")
    assert old in text
    with pytest.raises(ConfigError) as raised:
        parse_phantomx_training_config(text.replace(old, new))
    assert raised.value.code == "CONFIG_OUT_OF_RANGE"
    assert raised.value.path == path
