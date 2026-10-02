"""Adapt the typed Direct environment to the pinned RSL-RL VecEnv contract."""

from __future__ import annotations

import math
from collections.abc import Mapping
from typing import Any

import torch
from rsl_rl.env import VecEnv
from tensordict import TensorDict

from ...core.direct.env import UERLDirectEnv


class UERLVecEnvWrapper(VecEnv):  # type: ignore[misc]
    """Expose one ``UERLDirectEnv`` as a synchronized RSL-RL vector environment.

    The wrapper owns no Session or reset policy. ``UERLDirectEnv`` remains the
    only component that advances physics, computes task math, and performs
    sparse per-Slot reset; this class only changes the external tensor envelope.
    """

    def __init__(self, direct_env: UERLDirectEnv, cfg: dict[str, Any] | object | None = None) -> None:
        """Bind the adapter to one ready Direct environment.

        Args:
            direct_env: Initialized typed Direct environment whose stable Slot
                batch is used for every RSL-RL call.
            cfg: Python-only runner configuration exposed through the RSL-RL
                ``VecEnv.cfg`` field. It is not sent to UE.
        """

        self._direct_env = direct_env
        self.cfg = cfg if cfg is not None else {}
        self.num_envs = direct_env.num_envs
        self.num_actions = direct_env.task.num_actions
        duration = direct_env.task.max_episode_duration_s
        self._timeout_counts_solver_steps = duration is not None and duration > 0.0
        self.max_episode_length = (
            math.ceil(duration / direct_env.physics_dt)
            if duration is not None and duration > 0.0
            else direct_env.task.max_episode_steps
        )
        self.device = direct_env.device

    @property
    def direct_env(self) -> UERLDirectEnv:
        """Return the delegated Direct environment for lifecycle ownership."""

        return self._direct_env

    @property
    def episode_length_buf(self) -> torch.Tensor:
        """Return the live episode clock that ``max_episode_length`` bounds.

        A simulated-time timeout reads completed solver steps; a step timeout
        reads control steps. RSL-RL's ``init_at_random_ep_len`` writes this
        clock, so it staggers whichever counter the timeout term consumes.
        """

        if self._timeout_counts_solver_steps:
            return self._direct_env.episode_solver_steps
        return self._direct_env.episode_length_buf

    @episode_length_buf.setter
    def episode_length_buf(self, value: torch.Tensor) -> None:
        """Replace the live episode clock for runner setup."""

        if self._timeout_counts_solver_steps:
            self._direct_env.episode_solver_steps = value
        else:
            self._direct_env.episode_length_buf = value

    def get_observations(self) -> TensorDict:
        """Return current Direct observations as an RSL-RL TensorDict."""

        return self._to_tensor_dict(self._direct_env.get_observations())

    def step(self, actions: torch.Tensor) -> tuple[TensorDict, torch.Tensor, torch.Tensor, dict[str, Any]]:
        """Delegate one transition and map Gymnasium masks to RSL-RL semantics.

        Args:
            actions: Batch-first policy actions with shape
                ``(num_envs, num_actions)`` in normalized Task units.

        Returns:
            A four-tuple containing the next observations, rewards, the combined
            ``terminated OR truncated`` done mask, and extras. ``extras`` carries
            pure timeouts (without physical termination) under ``time_outs`` and task metrics under
            ``log``.
        """

        observations, rewards, terminated, truncated, info = self._direct_env.step(actions)
        episode_metrics = info.get("episode_metrics", {})
        extras = {
            "time_outs": truncated & ~terminated,
            "transition_dt": info["transition_dt"],
            "terminal_episode_length": info["terminal_episode_length"],
            "log": dict(episode_metrics) if isinstance(episode_metrics, Mapping) else {},
        }
        return self._to_tensor_dict(observations), rewards, terminated | truncated, extras

    def reset(self, *, seed: int | None = None) -> tuple[TensorDict, dict[str, Any]]:
        """Reset all Direct slots when an external caller explicitly requests it."""

        observations, info = self._direct_env.reset(seed=seed)
        return self._to_tensor_dict(observations), info

    def close(self, reason: str = "rsl_rl_vecenv_close") -> None:
        """Close the delegated Direct environment exactly once."""

        self._direct_env.close(reason)

    def _to_tensor_dict(self, observations: Mapping[str, torch.Tensor]) -> TensorDict:
        """Wrap full-batch observation groups without changing their feature order."""

        return TensorDict(dict(observations), batch_size=[self.num_envs], device=self.device)


__all__ = ["UERLVecEnvWrapper"]
