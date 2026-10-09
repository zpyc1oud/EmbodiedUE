"""Prove that tracing observes real PPO without changing its numerical result."""

from pathlib import Path
from typing import Any

import pytest
import torch
import yaml
from rsl_rl.algorithms import PPO
from rsl_rl.models import MLPModel
from rsl_rl.storage import RolloutStorage
from tensordict import TensorDict

from uerl.training.debug_trace import TrainingDebugRecorder
from uerl.training.rsl_rl.debug_ppo import DebugPPO, DebugTimeAwarePPO
from uerl.training.rsl_rl.time_aware_ppo import TimeAwarePPO


def _run(kind: Any, recorder: TrainingDebugRecorder | None) -> tuple[Any, torch.Tensor]:
    torch.manual_seed(17)
    obs = TensorDict({"policy": torch.arange(12, dtype=torch.float32).reshape(4, 3) / 10}, [4])
    groups = {"actor": ["policy"], "critic": ["policy"]}
    actor = MLPModel(
        obs,
        groups,
        "actor",
        2,
        hidden_dims=[8],
        obs_normalization=True,
        distribution_cfg={"class_name": "rsl_rl.modules.distribution:GaussianDistribution", "init_std": 0.3},
    )
    critic = MLPModel(obs, groups, "critic", 1, hidden_dims=[8], obs_normalization=True)
    storage = RolloutStorage("rl", 4, 3, obs, [2], "cpu")
    options = {"reference_dt_s": 0.02} if issubclass(kind, TimeAwarePPO) else {}
    alg = kind(actor, critic, storage, num_learning_epochs=2, num_mini_batches=2, **options)
    if recorder is not None:
        alg.configure_debug(recorder)
    for _ in range(2):
        with torch.inference_mode():
            for step in range(3):
                actions = alg.act(obs)
                obs = TensorDict({"policy": obs["policy"] + 0.1}, [4])
                alg.process_env_step(
                    obs,
                    actions.sum(-1),
                    torch.tensor([False, False, True, True]),
                    {
                        "time_outs": torch.tensor([False, False, False, True]),
                        "transition_dt": [0.005, 0.02, 0.035][step],
                    },
                )
            alg.compute_returns(obs)
        alg.update()
    return alg.save(), torch.get_rng_state()


def _equal(left: Any, right: Any) -> None:
    if isinstance(left, torch.Tensor):
        torch.testing.assert_close(left, right, rtol=0, atol=0)
    elif isinstance(left, dict):
        assert left.keys() == right.keys()
        for key in left:
            _equal(left[key], right[key])
    elif isinstance(left, (list, tuple)):
        assert len(left) == len(right)
        for a, b in zip(left, right, strict=True):
            _equal(a, b)
    else:
        assert left == right


@pytest.mark.parametrize(("plain", "traced"), [(PPO, DebugPPO), (TimeAwarePPO, DebugTimeAwarePPO)])
def test_training_trace_preserves_parameters_optimizer_and_rng(tmp_path: Path, plain: Any, traced: Any) -> None:
    expected, expected_rng = _run(plain, None)
    path = tmp_path / "steps.yaml"
    recorder = TrainingDebugRecorder(path, max_steps=3, metadata={})
    actual, actual_rng = _run(traced, recorder)
    recorder.close(complete=True)
    _equal(expected, actual)
    _equal(expected_rng, actual_rng)
    records = list(yaml.safe_load_all(path.read_text()))
    decisions = [r for r in records if r["kind"] == "policy_decision"]
    assert [r["control_step"] for r in decisions] == [0, 1, 2]
    assert decisions[0]["values"]["slot_ids"]["data"] == [0, 1, 2, 3]
    assert len([r for r in records if r["kind"] == "optimizer_step"]) == 4
    assert len(list(tmp_path.glob("*.pt"))) == 2
    assert records[-1]["complete"] is True


def test_training_trace_copies_before_mutation_and_refuses_overwrite(tmp_path: Path) -> None:
    path = tmp_path / "steps.yaml"
    recorder = TrainingDebugRecorder(path, max_steps=1, metadata={})
    values = torch.tensor([[1.0, 2.0], [3.0, 4.0]])
    recorder.record("state", 0, {"state": values})
    values.zero_()
    recorder.record("state", 1, {"state": values})
    recorder.close(complete=False)
    records = list(yaml.safe_load_all(path.read_text()))
    assert records[1]["values"]["state"]["data"] == [[1.0, 2.0], [3.0, 4.0]]
    assert records[-1] == {"kind": "footer", "complete": False, "events": 1}
    with pytest.raises(FileExistsError):
        TrainingDebugRecorder(path, max_steps=1, metadata={})


def test_independent_audit_reconstructs_real_ppo_targets(tmp_path: Path) -> None:
    from uerl.training.debug_audit import audit_policy, audit_returns, decode

    path = tmp_path / "steps.yaml"
    recorder = TrainingDebugRecorder(path, max_steps=3, metadata={})
    _run(DebugTimeAwarePPO, recorder)
    recorder.close(complete=True)
    records = [decode(r) for r in yaml.safe_load_all(path.read_text())]
    for record in records:
        if record["kind"] == "policy_decision":
            audit_policy(record["values"])
        if record["kind"] == "ppo_returns":
            audit_returns(record["values"])
            record["values"]["returns"][1, 2, 0] += 0.1
            with pytest.raises(ValueError, match="GAE returns"):
                audit_returns(record["values"])
    decision = next(r["values"] for r in records if r["kind"] == "policy_decision")
    decision["model_inputs"]["actor"][1, 0] += 0.5
    with pytest.raises(ValueError, match="actor normalized input"):
        audit_policy(decision)


def test_independent_action_audit_catches_clip_scale_and_offset_errors() -> None:
    import numpy as np

    from uerl.training.debug_audit import audit_actions

    plan = {
        "ops": [
            {"op": "policy_action", "inputs": [], "output": "raw", "params": {}},
            {"op": "slice", "inputs": ["raw"], "output": "slice", "params": {"start": 0, "width": 2}},
            {"op": "clip", "inputs": ["slice"], "output": "clip", "params": {"low": -1, "high": 1}},
            {"op": "scale", "inputs": ["clip"], "output": "scale", "params": {"factor": [0.5, 2]}},
            {"op": "offset", "inputs": ["scale"], "output": "target", "params": {"bias": [0.1, -0.2]}},
        ],
        "command_fields": ["target"],
    }
    actions = np.array([[3.0, -2.0], [0.2, 0.3]])
    commands = {"target": np.array([[0.6, -2.2], [0.2, 0.4]])}
    audit_actions(plan, actions, commands)
    commands["target"][0, 0] = 1.6
    with pytest.raises(ValueError, match="physical action targets"):
        audit_actions(plan, actions, commands)
