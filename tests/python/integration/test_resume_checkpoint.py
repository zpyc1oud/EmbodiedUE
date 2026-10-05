"""Exercise the real CPU RSL checkpoint loader without a UE Session."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any

import torch
from tensordict import TensorDict

from uerl.application.run_config import CHECKPOINT_CONFIG_KEY, load_run_config_source
from uerl.core.config.snapshot import resolved_config_from_yaml
from uerl.core.direct.curriculum import CurriculumManager
from uerl.core.mdp.lib.curriculum import TerrainLevelTerm
from uerl.tasks.cartpole import CARTPOLE_TASK_ID
from uerl.training import build_rsl_rl_train_config, build_run_config
from uerl.training.checkpoint import TrainingOptions
from uerl.training.rsl_rl.runner import TRAINING_OPTIONS_KEY, UERLOnPolicyRunner


class _CPUEnv:
    """Supply observations and existing Python state owners, with no Worker."""

    num_envs = 2
    num_actions = 1
    cfg: dict[str, object] = {}

    def __init__(self) -> None:
        self.direct_env = self
        self.generator = torch.Generator().manual_seed(21)
        self.curriculum = CurriculumManager(
            {
                "terrain": TerrainLevelTerm(num_levels=3, num_envs=2, terrain_size_x=30.0),
            }
        )

    def get_observations(self) -> TensorDict:
        return TensorDict({"policy": torch.tensor([[1.0, 2.0, 3.0, 4.0], [5.0, 6.0, 7.0, 8.0]])}, batch_size=[2])

    def curriculum_state_dict(self) -> dict[str, dict[str, object]]:
        return self.curriculum.state_dict()

    def load_curriculum_state_dict(self, state: Mapping[str, object]) -> None:
        self.curriculum.load_state_dict(state)

    def random_state_dict(self) -> dict[str, object]:
        return {"step_decimation_generator": self.generator.get_state()}

    def load_random_state_dict(self, state: Mapping[str, Any]) -> None:
        self.generator.set_state(state["step_decimation_generator"])


def _runner(env: _CPUEnv) -> UERLOnPolicyRunner:
    config = build_run_config(
        CARTPOLE_TASK_ID,
        overrides={
            "worker.slot_count": "2",
            "runner.device": "cpu",
            "runner.parameters.hidden_dims": "[8,8]",
            "runner.parameters.obs_normalization": "true",
        },
    )
    runner = UERLOnPolicyRunner(env, build_rsl_rl_train_config(config), log_dir=None, device="cpu")
    runner.resolved_config = config
    return runner


def test_real_rsl_checkpoint_restores_models_statistics_optimizer_iteration_and_python_state(tmp_path: Path) -> None:
    source_env = _CPUEnv()
    source = _runner(source_env)
    obs = source_env.get_observations()
    source.alg.actor.update_normalization(obs)
    source.alg.critic.update_normalization(obs)
    loss = source.alg.actor(obs).sum() + source.alg.critic(obs).sum()
    loss.backward()
    source.alg.optimizer.step()  # Materialize Adam moments rather than testing empty state.
    source.current_learning_iteration = 17
    source.training_options = TrainingOptions(terrain_level=2, freeze_observation_normalization=True).to_dict()
    curriculum = source_env.curriculum_state_dict()
    curriculum["terrain"]["levels"] = [1, 2]
    source_env.load_curriculum_state_dict(curriculum)
    torch.randint(1, 8, (9,), generator=source_env.generator)
    checkpoint = tmp_path / "model.pt"
    source.save(str(checkpoint))
    safe_payload = torch.load(checkpoint, map_location="cpu", weights_only=True)
    saved_config_text = safe_payload["infos"][CHECKPOINT_CONFIG_KEY]
    saved_config = resolved_config_from_yaml(saved_config_text)
    assert saved_config["task_id"] == CARTPOLE_TASK_ID
    assert saved_config["worker"]["slot_count"] == 2
    detached_restore = load_run_config_source(None, None, strict=True, checkpoint=checkpoint)
    assert detached_restore.recorded == source.resolved_config
    expected_draws = torch.randint(1, 8, (8,), generator=source_env.generator)
    expected_actor = source.alg.actor(obs).detach().clone()
    expected_critic = source.alg.critic(obs).detach().clone()

    target_env = _CPUEnv()
    target = _runner(target_env)
    assert not target.alg.optimizer.state
    infos = target.load(str(checkpoint), map_location="cpu")

    torch.testing.assert_close(target.alg.actor(obs), expected_actor, rtol=0, atol=0)
    torch.testing.assert_close(target.alg.critic(obs), expected_critic, rtol=0, atol=0)
    assert target.current_learning_iteration == 17
    assert target_env.curriculum_state_dict()["terrain"]["levels"] == [1, 2]
    assert torch.equal(torch.randint(1, 8, (8,), generator=target_env.generator), expected_draws)
    assert infos[TRAINING_OPTIONS_KEY] == {"terrain_level": 2, "freeze_observation_normalization": True}
    source_optimizer = source.alg.optimizer.state_dict()
    target_optimizer = target.alg.optimizer.state_dict()
    assert target_optimizer["param_groups"] == source_optimizer["param_groups"]
    assert target_optimizer["state"]
    for key, state in source_optimizer["state"].items():
        for name, value in state.items():
            torch.testing.assert_close(target_optimizer["state"][key][name], value, rtol=0, atol=0)
    target.freeze_observation_normalization()
    before = target.alg.actor.obs_normalizer.state_dict()["_mean"].clone()
    target.alg.actor.update_normalization(TensorDict({"policy": torch.ones(2, 4) * 100}, batch_size=[2]))
    assert torch.equal(before, target.alg.actor.obs_normalizer.state_dict()["_mean"])
