"""Reference deploy inference: our plans + onnxruntime (no training imports).

Used for cross-language deploy parity and offline analysis. Not used on the
training or UE product path. Requires the ``onnxruntime`` package (declared in
``[dependency-groups] dev``).
"""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

import numpy as np
import torch

from uerl.core.mdp.executor import ActionPlanExecutor, PlanExecutor, PlanInputs
from uerl.core.mdp.plan import PlanOp
from uerl.policy.artifact import PolicyArtifact

try:
    import onnxruntime as ort
except ImportError as exc:  # pragma: no cover - exercised when ort missing
    raise ImportError(
        "uerl.policy.reference requires onnxruntime (dev dependency). "
        "Install with: uv sync --group dev"
    ) from exc


def _command_channels_from_plan(ops: tuple[PlanOp, ...]) -> dict[str, int]:
    """Widths for ``command`` ops (Compile-time check against host channels)."""

    channels: dict[str, int] = {}
    for op in ops:
        if op.op != "command":
            continue
        channel = op.params["channel"]
        width = op.params["width"]
        assert isinstance(channel, str)
        assert isinstance(width, int)
        channels[channel] = width
    return channels


def _as_batch1(values: Mapping[str, Any]) -> dict[str, torch.Tensor]:
    """Coerce named float sequences / tensors to rank-2 float32 batch rows."""

    out: dict[str, torch.Tensor] = {}
    for name, value in values.items():
        if isinstance(value, torch.Tensor):
            tensor = value.detach().to(dtype=torch.float32)
        else:
            tensor = torch.as_tensor(value, dtype=torch.float32)
        if tensor.ndim == 1:
            tensor = tensor.unsqueeze(0)
        if tensor.ndim != 2 or tensor.shape[0] != 1:
            raise ValueError(
                f"{name!r} must be shape (1, W) or (W,), got {tuple(tensor.shape)}"
            )
        out[name] = tensor
    return out


class ReferencePolicyRunner:
    """Artifact reference: PlanExecutor + ActionPlanExecutor + onnxruntime.

    Tracks ``previous_action`` as raw policy output (same as UE controller).
    Does not import ``uerl.training``.
    """

    def __init__(self, artifact: PolicyArtifact) -> None:
        artifact.validate()
        command_channels = _command_channels_from_plan(artifact.observation_plan.ops)
        self._artifact = artifact
        self._obs = PlanExecutor(
            artifact.observation_plan,
            command_channels=command_channels or None,
        )
        self._act = ActionPlanExecutor(artifact.action_plan)
        self._session = ort.InferenceSession(
            artifact.onnx,
            providers=["CPUExecutionProvider"],
        )
        inputs = self._session.get_inputs()
        outputs = self._session.get_outputs()
        if len(inputs) != 1 or len(outputs) != 1:
            raise ValueError(
                f"expected single ONNX in/out, got {len(inputs)} inputs / {len(outputs)} outputs"
            )
        self._input_name = inputs[0].name
        self._output_name = outputs[0].name
        self._action_width = artifact.action_plan.policy_width
        self._previous_action = torch.zeros(1, self._action_width, dtype=torch.float32)

    @property
    def artifact(self) -> PolicyArtifact:
        return self._artifact

    def reset(self, previous_action: torch.Tensor | None = None) -> None:
        """Clear history, optionally installing an explicit bootstrap action."""

        if previous_action is None:
            self._previous_action = torch.zeros(1, self._action_width, dtype=torch.float32)
            return
        action = torch.as_tensor(previous_action, dtype=torch.float32)
        if action.ndim == 1:
            action = action.unsqueeze(0)
        if action.shape != (1, self._action_width):
            raise ValueError(
                f"previous_action must be shape (1, {self._action_width}), got {tuple(action.shape)}"
            )
        self._previous_action = action.detach().clone()

    def step(
        self,
        raw_state: Mapping[str, torch.Tensor],
        commands: Mapping[str, torch.Tensor],
        control_frame_dt: torch.Tensor | float | None = None,
        *,
        reset_history: bool = False,
        previous_action: torch.Tensor | None = None,
    ) -> Mapping[str, torch.Tensor]:
        """One deploy step: obs plan -> ONNX -> action plan.

        Returns:
            Mapping with ``action`` (raw policy output, shape ``(1, A)``) and
            each ``action_plan.command_fields`` entry (physical targets).
        """

        if reset_history:
            self.reset()
        if previous_action is not None:
            self.reset(previous_action)
        state = _as_batch1(raw_state)
        cmds = _as_batch1(commands)
        dt = None
        if control_frame_dt is not None:
            dt = torch.as_tensor(control_frame_dt, dtype=torch.float32)
            if dt.ndim == 0:
                dt = dt.reshape(1, 1)
            elif dt.ndim == 1:
                dt = dt.reshape(1, -1)
            if dt.shape != (1, 1):
                raise ValueError(
                    f"control_frame_dt must be scalar or shape (1, 1), got {tuple(dt.shape)}"
                )
        groups = self._obs.execute(
            PlanInputs(
                raw_state=state,
                commands=cmds,
                control_frame_dt=dt,
                previous_action=self._previous_action,
            )
        )
        if "policy" not in groups:
            raise KeyError("observation plan missing 'policy' group")
        observation = groups["policy"].detach().clone()
        obs = observation.cpu().numpy().astype(np.float32, copy=False)
        ort_outs = self._session.run([self._output_name], {self._input_name: obs})
        action_np = np.asarray(ort_outs[0], dtype=np.float32)
        if action_np.ndim == 1:
            action_np = action_np.reshape(1, -1)
        action = torch.from_numpy(np.array(action_np, dtype=np.float32, copy=True))
        if action.shape != (1, self._action_width):
            raise ValueError(
                f"ONNX action shape {tuple(action.shape)} != (1, {self._action_width})"
            )
        targets = self._act.execute(action)
        self._previous_action = action.detach().clone()
        result: dict[str, torch.Tensor] = {
            "observation": observation,
            "action": action,
        }
        for name in self._artifact.action_plan.command_fields:
            result[name] = targets[name]
        return result
