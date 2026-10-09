"""Check recorded training boundaries with independent NumPy calculations."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any

import numpy as np
import yaml


def decode(value: Any) -> Any:
    """Restore shaped arrays without importing the training implementation."""
    if isinstance(value, dict):
        if set(value) == {"shape", "dtype", "data"}:
            return np.asarray(value["data"]).reshape(value["shape"])
        return {key: decode(item) for key, item in value.items()}
    if isinstance(value, list):
        return [decode(item) for item in value]
    return value


def _same(label: str, actual: Any, expected: Any) -> None:
    if isinstance(expected, Mapping):
        if actual.keys() != expected.keys():
            raise ValueError(f"{label}: field names differ")
        for key in expected:
            _same(f"{label}.{key}", actual[key], expected[key])
        return
    a, b = np.asarray(actual), np.asarray(expected)
    if a.shape != b.shape or not np.allclose(a, b, rtol=2e-5, atol=2e-6, equal_nan=False):
        raise ValueError(f"{label}: recorded values differ")


def audit_policy(values: Mapping[str, Any]) -> None:
    """Reconstruct normalized inputs and diagonal Gaussian action likelihoods."""
    _same("policy slot order", values["slot_ids"], np.arange(len(values["actions"])))
    for name in ("actor", "critic"):
        raw = np.concatenate([values["observations"][key] for key in values["observation_group_order"][name]], axis=-1)
        state = values[f"{name}_normalizer"]
        expected = raw if not state else (raw - state["_mean"]) / (state["_std"] + values["normalizer_epsilon"][name])
        _same(f"{name} normalized input", values["model_inputs"][name], expected)
    actions, mean, std = (values[key] for key in ("actions", "action_mean", "action_std"))
    if not np.all(std > 0):
        raise ValueError("action standard deviation must be positive")
    log_prob = (-0.5 * ((actions - mean) / std) ** 2 - np.log(std) - 0.5 * np.log(2 * np.pi)).sum(axis=-1)
    _same("Gaussian log probability", np.asarray(values["action_log_prob"]).reshape(-1), log_prob)


def audit_actions(plan: Mapping[str, Any], actions: Any, commands: Mapping[str, Any]) -> None:
    """Evaluate the compiled action arithmetic with NumPy, not the plan executor."""
    slots: dict[str, Any] = {}
    for operation in plan["ops"]:
        name, params = operation["op"], operation["params"]
        value: Any = None if not operation["inputs"] else slots[operation["inputs"][0]]
        if name == "policy_action":
            result = actions
        elif name == "slice":
            result = value[:, params["start"] : params["start"] + params["width"]]
        elif name == "clip":
            result = np.clip(value, params["low"], params["high"])
        elif name == "scale":
            result = value * np.asarray(params["factor"])
        elif name == "offset":
            result = value + np.asarray(params["bias"])
        else:
            raise ValueError(f"unsupported action audit operator: {name}")
        slots[operation["output"]] = result
    _same("physical action targets", commands, {key: slots[key] for key in plan["command_fields"]})


def audit_returns(values: Mapping[str, Any]) -> None:
    """Recompute GAE without calling PPO or environment reward helpers."""
    rewards, estimates, dones = (values[key] for key in ("rewards", "values", "dones"))
    returns = np.empty_like(rewards, dtype=np.float64)
    advantage = np.zeros_like(values["last_values"], dtype=np.float64)
    for step in reversed(range(len(rewards))):
        discount, trace = values["time_factors"][step]
        following = values["last_values"] if step == len(rewards) - 1 else estimates[step + 1]
        live = 1 - dones[step]
        delta = rewards[step] + live * discount * following - estimates[step]
        advantage = delta + live * discount * trace * advantage
        returns[step] = advantage + estimates[step]
    _same("GAE returns", values["returns"], returns)
    advantages = returns - estimates
    if values["normalized_advantages"]:
        advantages = (advantages - advantages.mean()) / (advantages.std(ddof=1) + 1e-8)
    _same("GAE advantages", values["advantages"], advantages)


def audit_step(records: Mapping[str, Any], previous_output: Mapping[str, Any] | None) -> None:
    """Check slot identity, policy/environment/storage mapping, and sparse resets."""
    decision, incoming, transition, outgoing, stored = (
        records[key]
        for key in (
            "policy_decision",
            "environment_input",
            "environment_transition",
            "environment_output",
            "ppo_transition",
        )
    )
    audit_policy(decision)
    slots = len(decision["actions"])
    for stage in (incoming, transition, outgoing):
        _same("environment slot order", stage["slot_ids"], np.arange(slots))
    _same("environment policy actions", incoming["policy_actions"], decision["actions"])
    _same("stored policy actions", stored["stored_actions"], decision["actions"])
    _same("decision observations", incoming["observation_groups"], decision["observations"])
    _same("stored observations", stored["stored_observations"], decision["observations"])
    _same("next observations", stored["next_observations"], outgoing["observation_groups"])
    _same("stored values", stored["stored_values"], decision["values"])
    _same(
        "stored log probability",
        np.asarray(stored["stored_log_prob"]).reshape(-1),
        np.asarray(decision["action_log_prob"]).reshape(-1),
    )
    if "velocity" in incoming["commands"]:
        _same("latched velocity command", transition["scored_command_velocity"], incoming["commands"]["velocity"])
    dones = transition["terminated"] | transition["truncated"]
    timeouts = transition["truncated"] & ~transition["terminated"]
    _same("done mapping", stored["dones"], dones)
    _same("timeout mapping", stored["time_outs"], timeouts)
    _same("reset mapping", outgoing["reset_mask"], dones)
    _same("reward mapping", stored["environment_rewards"], transition["rewards"])
    expected_reward = transition["rewards"] + stored["discount"] * decision["values"].reshape(-1) * timeouts
    _same("timeout bootstrap", stored["stored_rewards"].reshape(-1), expected_reward)
    _same("control duration", transition["transition_dt"], transition["physics_dt"] * transition["step_decimation"])
    if stored["transition_dt"] is not None:
        _same("PPO duration", stored["transition_dt"], transition["transition_dt"])
    terms = transition["weighted_reward_terms_compact"]
    ids = transition["valid_slot_ids"].astype(int)
    expected_ids = np.flatnonzero(transition["state_valid"] & (transition["fault_code"] == 0))
    _same("valid slot compaction", ids, expected_ids)
    invalid = np.ones(slots, dtype=bool)
    invalid[ids] = False
    _same(
        "slot fault reward",
        transition["rewards"][invalid],
        np.full(int(invalid.sum()), transition["slot_fault_reward"]),
    )
    if terms is not None and len(ids):
        _same("reward term sum", transition["rewards"][ids], sum(terms.values()))
    expected_actions = decision["actions"].copy()
    expected_actions[dones] = 0
    _same("previous actions after reset", outgoing["previous_policy_actions"], expected_actions)
    for key in ("episode_steps", "episode_solver_steps"):
        _same(f"reset {key}", outgoing[key][dones], np.zeros(int(dones.sum())))
    for key in transition["raw_state"]:
        _same(f"untouched slot state {key}", outgoing["raw_state"][key][~dones], transition["raw_state"][key][~dones])
    if previous_output is not None:
        for key in ("raw_state", "episode_index", "previous_policy_actions", "observation_groups"):
            _same(f"next step {key}", incoming[key], previous_output[key])


def audit_training_trace(path: Path) -> dict[str, int]:
    """Read one control window at a time and reject missing or inconsistent evidence."""
    pending: dict[str, Any] = {}
    previous: Mapping[str, Any] | None = None
    steps = returns = updates = 0
    footer = None
    header = None
    plans = None
    rollout = None
    optimizer_steps = 0
    durations: list[float] = []
    rollout_rows: list[dict[str, Any]] = []
    action_checks = reward_checks = 0
    with path.open(encoding="utf-8") as stream:
        for raw in yaml.safe_load_all(stream):
            record = decode(raw)
            kind = record["kind"]
            if footer is not None:
                raise ValueError("events found after footer")
            if kind == "header":
                if header is not None or steps or pending:
                    raise ValueError("duplicate or misplaced header")
                header = record
                if record["schema_version"] != 1:
                    raise ValueError("unsupported training trace schema")
                continue
            if header is None:
                raise ValueError("missing header")
            if kind == "footer":
                footer = record
                continue
            values = record["values"]
            if kind in {
                "policy_decision",
                "environment_input",
                "environment_transition",
                "environment_output",
                "ppo_transition",
            }:
                if record["control_step"] != steps or kind in pending:
                    raise ValueError("missing, duplicate, or reordered control step")
                pending[kind] = values
                if kind == "ppo_transition":
                    if len(pending) != 5:
                        raise ValueError("incomplete control step")
                    audit_step(pending, previous)
                    if plans is not None and plans["actions"] is not None:
                        audit_actions(
                            plans["actions"],
                            pending["policy_decision"]["actions"],
                            pending["environment_transition"]["physical_commands"],
                        )
                    if plans is not None and plans["actions"] is not None:
                        action_checks += 1
                    if pending["environment_transition"]["weighted_reward_terms_compact"] is not None:
                        reward_checks += 1
                    rollout_rows.append(
                        {key: pending["ppo_transition"][key] for key in ("stored_rewards", "stored_values", "dones")}
                    )
                    if rollout is None:
                        raise ValueError("missing rollout boundary")
                    duration = float(pending["environment_transition"]["transition_dt"])
                    durations.append(duration)
                    exponent = 1 if rollout["reference_dt_s"] is None else duration / rollout["reference_dt_s"]
                    _same("physical discount", pending["ppo_transition"]["discount"], rollout["gamma"] ** exponent)
                    previous = pending["environment_output"]
                    pending = {}
                    steps += 1
            elif kind == "task_plans":
                plans = values
            elif kind == "rollout_start":
                rollout = values
                durations = []
                rollout_rows = []
                optimizer_steps = 0
            elif kind == "ppo_returns":
                if rollout is None or len(durations) != rollout["rollout_length"]:
                    raise ValueError("rollout length mismatch")
                factors = [
                    (rollout["gamma"] ** exponent, rollout["lambda"] ** exponent)
                    for dt in durations
                    for exponent in (1 if rollout["reference_dt_s"] is None else dt / rollout["reference_dt_s"],)
                ]
                _same("physical GAE factors", values["time_factors"], factors)
                for destination, source in (
                    ("rewards", "stored_rewards"),
                    ("values", "stored_values"),
                    ("dones", "dones"),
                ):
                    expected = np.stack([row[source] for row in rollout_rows])
                    if destination == "dones":
                        expected = expected[..., None]
                    _same(f"rollout {destination}", values[destination], expected)
                audit_returns(values)
                returns += 1
            elif kind == "optimizer_step":
                if values["minibatch_update"] != optimizer_steps:
                    raise ValueError("optimizer step sequence mismatch")
                optimizer_steps += 1
                gradients = values["clipped_gradient_statistics"]
                if not gradients or not all(item["finite"] for item in gradients.values()):
                    raise ValueError("missing or non-finite gradients")
            elif kind == "ppo_update":
                if (
                    rollout is None
                    or values["optimizer_steps"] != optimizer_steps
                    or optimizer_steps != rollout["learning_epochs"] * rollout["mini_batches"]
                ):
                    raise ValueError("optimizer update count mismatch")
                updates += 1
    if header is None or footer is None or not footer["complete"] or pending:
        raise ValueError("training capture is incomplete")
    if steps != header["max_control_steps"] or returns != updates or updates < 1:
        raise ValueError("training capture does not contain all requested steps and updates")
    return {
        "control_steps": steps,
        "rollouts": returns,
        "updates": updates,
        "action_plan_checks": action_checks,
        "reward_decomposition_checks": reward_checks,
    }
