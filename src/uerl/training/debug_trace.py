"""Stream bounded all-Slot training evidence as YAML documents."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any

import torch
import yaml


def tensor_values(value: Any) -> Any:
    """Copy tensors immediately; preserve dimensions, dtypes, and named fields."""
    if isinstance(value, torch.Tensor):
        return {"shape": list(value.shape), "dtype": str(value.dtype), "data": value.detach().cpu().tolist()}
    if isinstance(value, Mapping):
        return {str(key): tensor_values(item) for key, item in value.items()}
    if isinstance(value, (tuple, list)):
        return [tensor_values(item) for item in value]
    if isinstance(value, Path):
        return str(value)
    if value is None or isinstance(value, (str, bool, int, float)):
        return value
    raise TypeError(f"unsupported debug value: {type(value).__name__}")


class TrainingDebugRecorder:
    """Persist each event separately; never retain the Run's trajectories in RAM."""

    def __init__(self, path: Path, *, max_steps: int, metadata: Mapping[str, object]) -> None:
        if max_steps < 1:
            raise ValueError("debug max_steps must be positive")
        self.path = path
        self.max_steps = max_steps
        self._closed = False
        self._events = 0
        path.parent.mkdir(parents=True, exist_ok=True)
        self._file = path.open("x", encoding="utf-8")
        try:
            self._write({"kind": "header", "schema_version": 1, "max_control_steps": max_steps, "metadata": metadata})
        except BaseException:
            self._file.close()
            raise

    def captures(self, step: int) -> bool:
        return not self._closed and 0 <= step < self.max_steps

    def record(self, stage: str, step: int, values: Mapping[str, object]) -> None:
        if self.captures(step):
            self._write({"kind": stage, "control_step": step, "values": values})
            self._events += 1

    def snapshot(self, name: str, state: Mapping[str, object]) -> str:
        """Save tensor model/optimizer state separately from readable step evidence."""
        path = self.path.with_name(f"{self.path.stem}-{name}.pt")
        with path.open("xb") as output:
            torch.save(state, output)
        return path.name

    def close(self, *, complete: bool) -> None:
        if not self._closed:
            try:
                self._write({"kind": "footer", "complete": complete, "events": self._events})
            finally:
                self._closed = True
                self._file.close()

    def _write(self, record: Mapping[str, object]) -> None:
        yaml.safe_dump(tensor_values(record), self._file, explicit_start=True, sort_keys=False, allow_unicode=True)
        self._file.flush()
