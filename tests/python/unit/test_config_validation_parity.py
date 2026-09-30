"""Prove CartPole and PhantomX reject the same illegal runner inputs equivalently."""

from __future__ import annotations

import pytest

from uerl import ConfigError
from uerl.tasks.cartpole.config import (
    DEFAULT_CARTPOLE_TRAINING_CONFIG,
    parse_cartpole_training_config,
)
from uerl.tasks.phantomx.config import (
    DEFAULT_PHANTOMX_TRAINING_CONFIG,
    parse_phantomx_training_config,
)


def _inject(document: str, *, original: str, replacement: str) -> str:
    assert original in document, original
    return document.replace(original, replacement)


@pytest.mark.parametrize(
    ("cartpole_original", "phantomx_original", "replacement", "path", "code"),
    [
        (
            "learning_rate: 0.001",
            "learning_rate: 0.0003",
            "learning_rate: -1",
            "runner.parameters.learning_rate",
            "CONFIG_OUT_OF_RANGE",
        ),
        (
            "rollout_length: 16",
            "rollout_length: 40",
            "rollout_length: 0",
            "runner.rollout_length",
            "CONFIG_OUT_OF_RANGE",
        ),
        (
            "hidden_dims: [32, 32]",
            "hidden_dims: [128, 128, 128]",
            "hidden_dims: []",
            "runner.parameters.hidden_dims",
            "CONFIG_OUT_OF_RANGE",
        ),
        (
            "device: cpu",
            "device: cuda:0",
            "device: ''",
            "runner.device",
            "CONFIG_OUT_OF_RANGE",
        ),
        (
            "check_for_nan: true",
            "check_for_nan: true",
            "bogus_key: 1\n    check_for_nan: true",
            "runner.parameters.bogus_key",
            "CONFIG_UNKNOWN_KEY",
        ),
    ],
)
def test_runner_rejection_parity_across_tasks(
    cartpole_original: str,
    phantomx_original: str,
    replacement: str,
    path: str,
    code: str,
) -> None:
    """Same illegal runner inputs must produce the same code and path shape."""

    cartpole_doc = _inject(
        DEFAULT_CARTPOLE_TRAINING_CONFIG.read_text(encoding="utf-8"),
        original=cartpole_original,
        replacement=replacement,
    )
    phantomx_doc = _inject(
        DEFAULT_PHANTOMX_TRAINING_CONFIG.read_text(encoding="utf-8"),
        original=phantomx_original,
        replacement=replacement,
    )

    with pytest.raises(ConfigError) as cartpole_error:
        parse_cartpole_training_config(cartpole_doc, source=str(DEFAULT_CARTPOLE_TRAINING_CONFIG))
    with pytest.raises(ConfigError) as phantomx_error:
        parse_phantomx_training_config(phantomx_doc, source=str(DEFAULT_PHANTOMX_TRAINING_CONFIG))

    assert cartpole_error.value.code == phantomx_error.value.code == code
    assert cartpole_error.value.path == phantomx_error.value.path == path
