"""Verify curriculum state rides inside every RSL-RL checkpoint."""

from __future__ import annotations

from types import SimpleNamespace
from unittest.mock import Mock

import pytest
from rsl_rl.runners import OnPolicyRunner

from uerl.core.direct.curriculum import CurriculumManager
from uerl.core.mdp.lib.curriculum import TerrainLevelTerm
from uerl.tasks.phantomx.config import PhantomXCommandConfig, PhantomXCurriculumConfig
from uerl.tasks.phantomx.curriculum import PhantomXCommandCurriculum
from uerl.training.rsl_rl.runner import UERLOnPolicyRunner


def test_runner_saves_and_restores_curriculum_state(monkeypatch: pytest.MonkeyPatch) -> None:
    runner = object.__new__(UERLOnPolicyRunner)
    runner.alg = Mock()
    direct_env = Mock()
    direct_env.curriculum_state_dict.return_value = {"command": {"stage": 1}}
    runner.env = Mock(direct_env=direct_env)
    saved: dict[str, object] = {}

    def fake_save(_runner: object, path: str, infos: dict[str, object] | None = None) -> None:
        saved["path"] = path
        saved["infos"] = infos

    def fake_load(
        _runner: object,
        path: str,
        load_cfg: dict[str, object] | None = None,
        strict: bool = True,
        map_location: str | None = None,
    ) -> dict[str, object]:
        del path, load_cfg, strict, map_location
        return {"uerl_curriculum": {"command": {"stage": 1}}}

    monkeypatch.setattr(OnPolicyRunner, "save", fake_save)
    monkeypatch.setattr(OnPolicyRunner, "load", fake_load)

    runner.save("model.pt", {"caller": "kept"})
    infos = runner.load("model.pt", map_location="cpu")

    assert saved == {
        "path": "model.pt",
        "infos": {
            "caller": "kept",
            "uerl_curriculum": {"command": {"stage": 1}},
        },
    }
    assert infos["uerl_curriculum"] == {"command": {"stage": 1}}
    direct_env.load_curriculum_state_dict.assert_called_once_with({"command": {"stage": 1}})


def test_fixed_terrain_level_resume_restores_command_stage_without_terrain_term(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """A fixed-level Run registers no terrain term, yet resumes the command stage."""

    def command_term() -> PhantomXCommandCurriculum:
        return PhantomXCommandCurriculum(
            PhantomXCurriculumConfig(),
            PhantomXCommandConfig(),
            num_envs=2,
            device="cpu",
            run_seed=0,
        )

    trained_command = command_term()
    trained_command.load_state_dict(
        {"stage": 1, "straight_history": [True] * 128, "turn_history": [False]}
    )
    adaptive = CurriculumManager(
        {
            "command": trained_command,
            "terrain": TerrainLevelTerm(num_levels=4, num_envs=2, terrain_size_x=30.0),
        }
    )
    checkpoint_curriculum = adaptive.state_dict()
    fixed_command = command_term()
    fixed = CurriculumManager({"command": fixed_command})
    runner = object.__new__(UERLOnPolicyRunner)
    runner.env = Mock(direct_env=SimpleNamespace(load_curriculum_state_dict=fixed.load_state_dict))

    def fake_load(*_args: object, **_kwargs: object) -> dict[str, object]:
        return {"uerl_curriculum": checkpoint_curriculum}

    monkeypatch.setattr(OnPolicyRunner, "load", fake_load)

    with pytest.raises(ValueError, match="do not match the registered terms"):
        runner.load("model.pt")
    runner.load("model.pt", skip_curriculum_terms=("terrain",))

    assert fixed_command.state_dict() == trained_command.state_dict()
    assert bool(fixed_command.turn_enabled.all())


def test_runner_freezes_loaded_observation_normalization() -> None:
    """Disable updates while preserving each model's loaded normalizer object."""

    runner = object.__new__(UERLOnPolicyRunner)
    actor = SimpleNamespace(obs_normalization=True)
    critic = SimpleNamespace(obs_normalization=True)
    runner.alg = SimpleNamespace(actor=actor, critic=critic)

    runner.freeze_observation_normalization()

    assert actor.obs_normalization is False
    assert critic.obs_normalization is False
