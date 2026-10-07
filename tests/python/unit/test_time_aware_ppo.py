"""Verify physical-time PPO targets at the pinned RSL-RL algorithm seam."""

from __future__ import annotations

import pytest
import torch
from rsl_rl.storage import RolloutStorage
from tensordict import TensorDict

from uerl.training.rsl_rl.time_aware_ppo import TimeAwarePPO


class _ConstantModel(torch.nn.Module):
    def __init__(self, value: float) -> None:
        super().__init__()
        self.value = torch.nn.Parameter(torch.tensor(value))
        self._batch_size = 1

    def forward(self, obs: TensorDict, *, stochastic_output: bool = False) -> torch.Tensor:
        del stochastic_output
        self._batch_size = int(obs.batch_size[0])
        return self.value.expand(self._batch_size, 1)

    def get_hidden_state(self) -> None:
        return None

    def reset(self, dones: torch.Tensor | None = None, *, hidden_state: None = None) -> None:
        del dones, hidden_state

    def update_normalization(self, obs: TensorDict) -> None:
        del obs

    def get_output_log_prob(self, actions: torch.Tensor) -> torch.Tensor:
        return torch.zeros(actions.shape[0], 1)

    @property
    def output_distribution_params(self) -> tuple[torch.Tensor, ...]:
        return (torch.ones(self._batch_size, 1),)


def _algorithm(*, value: float, steps: int, slots: int = 1) -> tuple[TimeAwarePPO, TensorDict]:
    obs = TensorDict({"policy": torch.zeros(slots, 1)}, batch_size=[slots])
    storage = RolloutStorage("rl", slots, steps, obs, [1], "cpu")
    algorithm = TimeAwarePPO(
        _ConstantModel(0.0),
        _ConstantModel(value),
        storage,
        gamma=0.81,
        lam=0.64,
        reference_dt_s=0.02,
        device="cpu",
    )
    return algorithm, obs


@pytest.mark.parametrize(
    ("first_duration", "expected_first_return"),
    [(0.005, 4.545584), (0.035, 2.950134)],
)
def test_ac_py_unit_timeppo_001_gae_uses_each_transition_duration(
    first_duration: float, expected_first_return: float
) -> None:
    """Worked examples distinguish short and long physical GAE horizons."""

    algorithm, obs = _algorithm(value=0.0, steps=2)
    for reward, duration, done in ((2.0, first_duration, False), (3.0, 0.02, True)):
        algorithm.act(obs)
        algorithm.process_env_step(
            obs,
            torch.tensor([reward]),
            torch.tensor([done]),
            {"transition_dt": duration, "time_outs": torch.tensor([False])},
        )
    algorithm.compute_returns(obs)

    assert algorithm.storage.returns[:, 0, 0].tolist() == pytest.approx([expected_first_return, 3.0], abs=1e-5)


def test_ac_py_unit_timeppo_002_timeout_bootstrap_uses_current_duration() -> None:
    """A 5 ms timeout uses gamma^(1/4), not gamma for a full 20 ms."""

    algorithm, obs = _algorithm(value=4.0, steps=1)
    algorithm.act(obs)
    algorithm.process_env_step(
        obs,
        torch.tensor([1.0]),
        torch.tensor([True]),
        {"transition_dt": 0.005, "time_outs": torch.tensor([True])},
    )

    assert algorithm.storage.rewards[0, 0, 0].item() == pytest.approx(4.794733, abs=1e-5)


def test_time_aware_ppo_mixed_terminal_timeout_and_continuing_slots() -> None:
    """True termination cuts bootstrap; timeout uses its current physical duration."""

    algorithm, obs = _algorithm(value=4.0, steps=2, slots=3)
    for rewards, dones, timeouts, duration in (
        ([2.0, 3.0, 4.0], [False, True, True], [False, False, True], 0.005),
        ([5.0, 6.0, 7.0], [True, True, True], [True, False, False], 0.035),
    ):
        algorithm.act(obs)
        algorithm.process_env_step(
            obs,
            torch.tensor(rewards),
            torch.tensor(dones),
            {"transition_dt": duration, "time_outs": torch.tensor(timeouts)},
        )
    algorithm.compute_returns(obs)

    # gamma_short = sqrt(0.9); lambda_short = sqrt(0.8); gamma_long = 0.9**3.5.
    # Final timeout return: 5 + gamma_long*4 = 7.7663605.
    # Continuing return: 2 + gamma_short*4 + gamma_short*lambda_short*(7.7663605 - 4).
    # A true terminal has reward 3 only; the short timeout has 4 + gamma_short*4.
    torch.testing.assert_close(
        algorithm.storage.returns[:, :, 0],
        torch.tensor([[8.990596, 3.0, 7.794733], [7.7663605, 6.0, 7.0]]),
        rtol=0,
        atol=1e-5,  # Rounded independent examples and float32 recurrence.
    )
