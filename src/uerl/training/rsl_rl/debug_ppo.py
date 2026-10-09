"""Observe the pinned PPO implementation without replacing its mathematics."""

from __future__ import annotations

from typing import Any, cast

import torch
from rsl_rl.algorithms import PPO
from tensordict import TensorDict

from ..debug_trace import TrainingDebugRecorder
from .time_aware_ppo import TimeAwarePPO


class _DebugMixin:
    debug_recorder: TrainingDebugRecorder | None = None
    _debug_step: int = 0

    def configure_debug(self, recorder: TrainingDebugRecorder) -> None:
        self.debug_recorder = recorder
        self._debug_step = 0

    def act(self, obs: TensorDict) -> torch.Tensor:
        model = cast(Any, self)
        recorder = self.debug_recorder
        step = self._debug_step
        if recorder is None or not recorder.captures(step):
            return cast(torch.Tensor, cast(Any, super()).act(obs))
        if step % model.storage.num_transitions_per_env == 0:
            snapshot = recorder.snapshot(f"step-{step}-before", model.save())
            recorder.record(
                "rollout_start",
                step,
                {
                    "snapshot": snapshot,
                    "gamma": model.gamma,
                    "lambda": model.lam,
                    "reference_dt_s": getattr(model, "reference_dt_s", None),
                    "rollout_length": model.storage.num_transitions_per_env,
                    "learning_epochs": model.num_learning_epochs,
                    "mini_batches": model.num_mini_batches,
                },
            )
        inputs: dict[str, torch.Tensor] = {}

        def capture(name: str) -> Any:
            def hook(module: Any, args: tuple[torch.Tensor, ...]) -> None:
                del module
                inputs[name] = args[0].detach().clone()

            return hook

        hooks = [
            model.actor.mlp.register_forward_pre_hook(capture("actor")),
            model.critic.mlp.register_forward_pre_hook(capture("critic")),
        ]
        try:
            actions = cast(torch.Tensor, cast(Any, super()).act(obs))
        finally:
            for hook in hooks:
                hook.remove()
        recorder.record(
            "policy_decision",
            step,
            {
                "slot_ids": torch.arange(actions.shape[0]),
                "observations": dict(obs.items()),
                "model_inputs": inputs,
                "actions": actions,
                "values": model.transition.values,
                "action_log_prob": model.transition.actions_log_prob,
                "action_mean": model.actor.output_mean,
                "action_std": model.actor.output_std,
                "observation_group_order": {"actor": model.actor.obs_groups, "critic": model.critic.obs_groups},
                "normalizer_epsilon": {
                    name: getattr(network.obs_normalizer, "eps", None)
                    for name, network in (("actor", model.actor), ("critic", model.critic))
                },
                "actor_normalizer": model.actor.obs_normalizer.state_dict(),
                "critic_normalizer": model.critic.obs_normalizer.state_dict(),
            },
        )
        return actions

    def process_env_step(
        self,
        obs: TensorDict,
        rewards: torch.Tensor,
        dones: torch.Tensor,
        extras: dict[str, Any],
    ) -> None:
        cast(Any, super()).process_env_step(obs, rewards, dones, extras)
        recorder = self.debug_recorder
        model = cast(Any, self)
        if recorder is not None and recorder.captures(self._debug_step):
            index = model.storage.step - 1
            recorder.record(
                "ppo_transition",
                self._debug_step,
                {
                    "storage_index": index,
                    "next_observations": dict(obs.items()),
                    "environment_rewards": rewards,
                    "dones": dones,
                    "time_outs": extras.get("time_outs"),
                    "transition_dt": extras.get("transition_dt"),
                    "stored_rewards": model.storage.rewards[index],
                    "stored_values": model.storage.values[index],
                    "stored_actions": model.storage.actions[index],
                    "stored_observations": dict(model.storage.observations[index].items()),
                    "stored_log_prob": model.storage.actions_log_prob[index],
                    "discount": (model._time_factors[-1][0] if isinstance(self, TimeAwarePPO) else model.gamma),
                },
            )
        self._debug_step += 1

    def compute_returns(self, obs: TensorDict) -> None:
        model = cast(Any, self)
        recorder = self.debug_recorder
        step = self._debug_step - 1
        if recorder is None or not recorder.captures(step):
            cast(Any, super()).compute_returns(obs)
            return
        bootstrap: dict[str, torch.Tensor] = {}

        def capture(module: Any, args: Any, output: torch.Tensor) -> None:
            del module, args
            bootstrap["last_values"] = output.detach().clone()

        hook = model.critic.register_forward_hook(capture)
        try:
            cast(Any, super()).compute_returns(obs)
        finally:
            hook.remove()
        storage = model.storage
        recorder.record(
            "ppo_returns",
            step,
            {
                "rewards": storage.rewards,
                "dones": storage.dones,
                "values": storage.values,
                "returns": storage.returns,
                "advantages": storage.advantages,
                **bootstrap,
                "time_factors": (
                    model._time_factors
                    if isinstance(self, TimeAwarePPO)
                    else [(model.gamma, model.lam)] * storage.num_transitions_per_env
                ),
                "normalized_advantages": not model.normalize_advantage_per_mini_batch,
            },
        )

    def update(self) -> dict[str, float]:
        model = cast(Any, self)
        recorder = self.debug_recorder
        step = self._debug_step - 1
        if recorder is None or not recorder.captures(step):
            return cast(dict[str, float], cast(Any, super()).update())
        updates = 0

        def before_optimizer(optimizer: Any, args: Any, kwargs: Any) -> None:
            nonlocal updates
            del args, kwargs
            gradients = {
                name: {
                    "norm": float(torch.linalg.vector_norm(parameter.grad).item()),
                    "finite": bool(torch.isfinite(parameter.grad).all().item()),
                }
                for prefix, network in (("actor", model.actor), ("critic", model.critic))
                for key, parameter in network.named_parameters()
                if parameter.grad is not None
                for name in (f"{prefix}.{key}",)
            }
            recorder.record(
                "optimizer_step",
                step,
                {
                    "minibatch_update": updates,
                    "learning_rates": [group["lr"] for group in optimizer.param_groups],
                    "clipped_gradient_statistics": gradients,
                },
            )
            updates += 1

        hook = model.optimizer.register_step_pre_hook(before_optimizer)
        try:
            losses = cast(dict[str, float], cast(Any, super()).update())
        finally:
            hook.remove()
        snapshot = recorder.snapshot(f"step-{step}-after", model.save())
        recorder.record(
            "ppo_update",
            step,
            {
                "losses": losses,
                "optimizer_steps": updates,
                "snapshot": snapshot,
            },
        )
        return losses


class DebugPPO(_DebugMixin, PPO):  # type: ignore[misc]
    """Bounded tracing of the standard pinned PPO."""


class DebugTimeAwarePPO(_DebugMixin, TimeAwarePPO):
    """Bounded tracing of the physical-time PPO."""
