"""PPO rollout targets for PhantomX's variable physical control interval."""

from __future__ import annotations

import math
from typing import Any, cast

import torch
from rsl_rl.algorithms import PPO
from tensordict import TensorDict

PHANTOMX_PHYSICAL_TIME_OBJECTIVE = "phantomx_physical_time_v1"


class TimeAwarePPO(PPO):  # type: ignore[misc]
    """Use elapsed simulated time for bootstrap discount and GAE traces."""

    gamma: float
    lam: float

    def __init__(self, *args: Any, reference_dt_s: float, **kwargs: Any) -> None:
        if not math.isfinite(reference_dt_s) or reference_dt_s <= 0.0:
            raise ValueError("reference_dt_s must be finite and positive")
        super().__init__(*args, **kwargs)
        self.reference_dt_s = reference_dt_s
        self._time_factors: list[tuple[float, float]] = []

    def process_env_step(
        self,
        obs: TensorDict,
        rewards: torch.Tensor,
        dones: torch.Tensor,
        extras: dict[str, Any],
    ) -> None:
        """Record the transition and use its physical-time discount for timeout bootstrap."""

        transition_dt = float(extras["transition_dt"])
        if not math.isfinite(transition_dt) or transition_dt <= 0.0:
            raise ValueError("transition_dt must be finite and positive")
        exponent = transition_dt / self.reference_dt_s
        reference_gamma = float(self.gamma)
        discount = reference_gamma**exponent
        trace = float(self.lam) ** exponent
        self.gamma = discount
        try:
            super().process_env_step(obs, rewards, dones, extras)
        finally:
            self.gamma = reference_gamma
        self._time_factors.append((discount, trace))

    def compute_returns(self, obs: TensorDict) -> None:
        """Compute PPO targets with each completed transition's discount and trace."""

        storage = self.storage
        if len(self._time_factors) != storage.num_transitions_per_env:
            raise ValueError("rollout transition times do not match storage length")
        critic_hidden_state = self.critic.get_hidden_state()
        last_values = self.critic(obs).detach()
        self.critic.reset(hidden_state=critic_hidden_state)
        advantage = torch.zeros_like(last_values)
        for step in reversed(range(storage.num_transitions_per_env)):
            discount, trace = self._time_factors[step]
            next_values = last_values if step == storage.num_transitions_per_env - 1 else storage.values[step + 1]
            next_is_not_terminal = 1.0 - storage.dones[step].float()
            delta = storage.rewards[step] + next_is_not_terminal * discount * next_values - storage.values[step]
            advantage = delta + next_is_not_terminal * discount * trace * advantage
            storage.returns[step] = advantage + storage.values[step]
        storage.advantages = storage.returns - storage.values
        if not self.normalize_advantage_per_mini_batch:
            storage.advantages = (storage.advantages - storage.advantages.mean()) / (
                storage.advantages.std() + 1e-8
            )

    def update(self) -> dict[str, float]:
        """Release time factors together with RSL-RL's rollout storage."""

        losses = super().update()
        self._time_factors.clear()
        return cast(dict[str, float], losses)


__all__ = ["PHANTOMX_PHYSICAL_TIME_OBJECTIVE", "TimeAwarePPO"]
