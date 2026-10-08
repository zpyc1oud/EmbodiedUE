"""Verify the product RSL-RL adapter without starting a UE process."""

from __future__ import annotations

from typing import Any, cast

import torch

from uerl.core.direct.env import UERLDirectEnv
from uerl.training.rsl_rl import UERLVecEnvWrapper


class _FakeTask:
    """Provide the generic task metadata consumed by the adapter constructor."""

    num_actions = 1
    max_episode_steps = 8
    max_episode_duration_s = None


class _FakeDirectEnv:
    """Record delegated calls while returning DirectEnv-shaped batches."""

    def __init__(self) -> None:
        self.num_envs = 2
        self.device = torch.device("cpu")
        self.min_control_dt = 0.02
        self.task = _FakeTask()
        self.episode_length_buf = torch.zeros(2, dtype=torch.long)
        self.step_actions: list[torch.Tensor] = []
        self.reset_seeds: list[int | None] = []
        self.close_reasons: list[str] = []

    def get_observations(self) -> dict[str, torch.Tensor]:
        """Return a four-feature policy group."""

        return {"policy": torch.zeros(2, 4)}

    def step(
        self, actions: torch.Tensor
    ) -> tuple[dict[str, torch.Tensor], torch.Tensor, torch.Tensor, torch.Tensor, dict[str, Any]]:
        """Return one mixed termination/timeout transition."""

        self.step_actions.append(actions.clone())
        return (
            {"policy": torch.ones(2, 4)},
            torch.tensor([1.0, 2.0]),
            torch.tensor([True, False]),
            torch.tensor([False, True]),
            {
                "episode_metrics": {"/cartpole/alive": torch.ones(2)},
                "terminal_observation_valid": torch.tensor([False, True]),
                "slot_fault_code": torch.tensor([2, 0]),
                "terminal_episode_length": torch.tensor([3, 4]),
                "transition_dt": torch.tensor([0.02, 0.02]),
            },
        )

    def reset(self, *, seed: int | None = None) -> tuple[dict[str, torch.Tensor], dict[str, Any]]:
        """Record an explicit all-Slot reset."""

        self.reset_seeds.append(seed)
        return {"policy": torch.full((2, 4), 3.0)}, {"episode_index": torch.zeros(2, dtype=torch.uint64)}

    def close(self, reason: str) -> None:
        """Record the delegated close reason."""

        self.close_reasons.append(reason)


def test_wrapper_maps_observations_done_and_timeout_without_resetting() -> None:
    """Prove RSL-RL sees Direct observations and combined done masks."""

    fake = _FakeDirectEnv()
    wrapper = UERLVecEnvWrapper(cast(UERLDirectEnv, fake), cfg={"name": "test"})

    observations = wrapper.get_observations()
    next_observations, rewards, dones, extras = wrapper.step(torch.tensor([[0.25], [-0.5]]))

    assert tuple(observations.batch_size) == (2,)
    assert tuple(observations["policy"].shape) == (2, 4)
    assert tuple(next_observations["policy"].shape) == (2, 4)
    assert torch.equal(rewards, torch.tensor([1.0, 2.0]))
    assert torch.equal(dones, torch.tensor([True, True]))
    assert torch.equal(extras["time_outs"], torch.tensor([False, True]))
    assert torch.equal(extras["terminal_episode_length"], torch.tensor([3, 4]))
    assert torch.equal(extras["transition_dt"], torch.tensor([0.02, 0.02]))
    assert extras["state_valid"].tolist() == [False, True]
    assert extras["slot_fault_code"].tolist() == [2, 0]
    assert fake.reset_seeds == []
    assert torch.equal(fake.step_actions[0], torch.tensor([[0.25], [-0.5]]))


def test_wrapper_forwards_episode_buffer_explicit_reset_and_close() -> None:
    """Prove runner setup and lifecycle calls remain delegated to DirectEnv."""

    fake = _FakeDirectEnv()
    wrapper = UERLVecEnvWrapper(cast(UERLDirectEnv, fake))
    replacement = torch.tensor([3, 4], dtype=torch.long)

    wrapper.episode_length_buf = replacement
    reset_observations, reset_info = wrapper.reset(seed=17)
    wrapper.close("test_close")

    assert wrapper.episode_length_buf is replacement
    assert tuple(reset_observations["policy"].shape) == (2, 4)
    assert reset_info["episode_index"].shape == (2,)
    assert fake.reset_seeds == [17]
    assert fake.close_reasons == ["test_close"]


def test_wrapper_bootstraps_only_timeouts_without_physical_termination() -> None:
    """A fall on the time limit remains terminal for PPO value targets."""

    class _OverlappingTerminationEnv(_FakeDirectEnv):
        def step(
            self, actions: torch.Tensor
        ) -> tuple[dict[str, torch.Tensor], torch.Tensor, torch.Tensor, torch.Tensor, dict[str, Any]]:
            observations, rewards, terminated, _truncated, info = super().step(actions)
            return observations, rewards, terminated, torch.tensor([True, True]), info

    wrapper = UERLVecEnvWrapper(cast(UERLDirectEnv, _OverlappingTerminationEnv()))
    _observations, _rewards, dones, extras = wrapper.step(torch.zeros(2, 1))

    assert dones.tolist() == [True, True]
    assert extras["time_outs"].tolist() == [False, True]
