"""Persist and reload compiled task export contracts for offline .uerlpol2 export."""

from __future__ import annotations

import os
import tempfile
from collections.abc import Mapping
from dataclasses import dataclass
from pathlib import Path
from types import MappingProxyType
from typing import Final, cast

import yaml

from ..core.config.yaml_loader import load_unique_yaml
from ..core.inputs import ResolvedInput
from ..core.inputs.serialization import input_contract, parse_input_contract
from ..core.mdp.plan import ActionPlan, ObservationPlan
from ..errors import ConfigError

EXPORT_CONTRACT_FILENAME: Final[str] = "export_contract.yaml"
EXPORT_CONTRACT_SCHEMA_VERSION: Final[int] = 1
_CONTRACT_KEYS: Final[frozenset[str]] = frozenset(
    {
        "schema_version",
        "task_id",
        "robot_id",
        "observation_plan",
        "action_plan",
        "input_contract",
        "observation_group_widths",
        "num_actions",
    }
)


def _contract_error(message: str, *, code: str = "EXPORT_CONTRACT_INVALID", path: str = "") -> ConfigError:
    return ConfigError(message, code=code, path=path)


def _require_nonempty_str(value: object, *, path: str) -> str:
    if not isinstance(value, str) or not value.strip():
        raise _contract_error(f"{path} must be a non-empty string", path=path)
    return value


def _require_positive_int(value: object, *, path: str) -> int:
    if type(value) is not int or value <= 0:
        raise _contract_error(f"{path} must be a positive integer", path=path)
    return value


@dataclass(frozen=True, slots=True)
class ExportContract:
    """Resolved plans, input contract, and tensor widths needed for offline export."""

    task_id: str
    robot_id: str
    observation_plan: ObservationPlan
    action_plan: ActionPlan
    input_requirements: tuple[ResolvedInput, ...]
    observation_group_widths: Mapping[str, int]
    num_actions: int
    schema_version: int = EXPORT_CONTRACT_SCHEMA_VERSION

    def __post_init__(self) -> None:
        if type(self.schema_version) is not int or self.schema_version != EXPORT_CONTRACT_SCHEMA_VERSION:
            raise _contract_error(
                f"unsupported export contract schema version {self.schema_version!r}",
                code="UNSUPPORTED_EXPORT_CONTRACT_SCHEMA",
                path="schema_version",
            )
        _require_nonempty_str(self.task_id, path="task_id")
        _require_nonempty_str(self.robot_id, path="robot_id")
        num_actions = _require_positive_int(self.num_actions, path="num_actions")
        if num_actions != self.action_plan.policy_width:
            raise _contract_error(
                f"num_actions ({num_actions}) != action_plan.policy_width ({self.action_plan.policy_width})",
                path="num_actions",
            )
        frozen_inputs = tuple(self.input_requirements)
        object.__setattr__(self, "input_requirements", frozen_inputs)

        if not self.observation_group_widths:
            raise _contract_error("observation_group_widths must be non-empty", path="observation_group_widths")
        widths: dict[str, int] = {}
        for name, width in self.observation_group_widths.items():
            key = _require_nonempty_str(name, path="observation_group_widths")
            widths[key] = _require_positive_int(width, path=f"observation_group_widths.{key}")
        for group_name, declared_width in self.observation_plan.group_widths.items():
            if group_name not in widths:
                raise _contract_error(
                    f"observation_group_widths missing plan group {group_name!r}",
                    path=f"observation_group_widths.{group_name}",
                )
            if widths[group_name] != declared_width:
                raise _contract_error(
                    f"observation_group_widths.{group_name} ({widths[group_name]}) != "
                    f"observation_plan.group_widths ({declared_width})",
                    path=f"observation_group_widths.{group_name}",
                )
        object.__setattr__(self, "observation_group_widths", MappingProxyType(widths))


def export_contract_to_yaml(contract: ExportContract) -> str:
    """Serialize one ``ExportContract`` as safe versioned YAML text."""

    payload = {
        "schema_version": contract.schema_version,
        "task_id": contract.task_id,
        "robot_id": contract.robot_id,
        "observation_plan": contract.observation_plan.to_json(),
        "action_plan": contract.action_plan.to_json(),
        "input_contract": input_contract(contract.input_requirements),
        "observation_group_widths": dict(contract.observation_group_widths),
        "num_actions": contract.num_actions,
    }
    try:
        return cast(str, yaml.safe_dump(payload, allow_unicode=True, default_flow_style=False, sort_keys=False))
    except yaml.YAMLError as exc:
        raise _contract_error(f"cannot serialize export contract as YAML: {exc}") from exc


def export_contract_from_yaml(text: str, *, path: str = EXPORT_CONTRACT_FILENAME) -> ExportContract:
    """Parse and validate a persisted export contract from YAML text."""

    try:
        payload = load_unique_yaml(text)
    except (yaml.YAMLError, TypeError, ValueError) as exc:
        raise _contract_error(f"invalid export contract YAML: {exc}", path=path) from exc
    if not isinstance(payload, Mapping):
        raise _contract_error("export contract YAML must be a mapping", path=path)
    unknown = sorted(str(key) for key in (set(payload) - _CONTRACT_KEYS))
    if unknown:
        raise _contract_error(
            f"export contract has unknown fields: {', '.join(unknown)}",
            path=path,
        )
    missing = sorted(_CONTRACT_KEYS - set(payload))
    if missing:
        raise _contract_error(
            f"export contract is missing fields: {', '.join(missing)}",
            path=path,
        )
    schema_version = payload["schema_version"]
    if type(schema_version) is not int:
        raise _contract_error("schema_version must be an integer", path=f"{path}.schema_version")
    if schema_version != EXPORT_CONTRACT_SCHEMA_VERSION:
        raise _contract_error(
            f"unsupported export contract schema version {schema_version}",
            code="UNSUPPORTED_EXPORT_CONTRACT_SCHEMA",
            path=f"{path}.schema_version",
        )
    task_id = _require_nonempty_str(payload["task_id"], path=f"{path}.task_id")
    robot_id = _require_nonempty_str(payload["robot_id"], path=f"{path}.robot_id")

    obs_raw = payload["observation_plan"]
    if not isinstance(obs_raw, Mapping):
        raise _contract_error("observation_plan must be a mapping", path=f"{path}.observation_plan")
    observation_plan = ObservationPlan.from_json(obs_raw)

    action_raw = payload["action_plan"]
    if not isinstance(action_raw, Mapping):
        raise _contract_error("action_plan must be a mapping", path=f"{path}.action_plan")
    action_plan = ActionPlan.from_json(action_raw)

    input_requirements = parse_input_contract(payload["input_contract"])

    widths_raw = payload["observation_group_widths"]
    if not isinstance(widths_raw, Mapping):
        raise _contract_error("observation_group_widths must be a mapping", path=f"{path}.observation_group_widths")
    observation_group_widths: dict[str, int] = {}
    for raw_key, raw_width in widths_raw.items():
        group_name = _require_nonempty_str(raw_key, path=f"{path}.observation_group_widths")
        observation_group_widths[group_name] = _require_positive_int(
            raw_width,
            path=f"{path}.observation_group_widths.{group_name}",
        )

    num_actions = _require_positive_int(payload["num_actions"], path=f"{path}.num_actions")
    return ExportContract(
        task_id=task_id,
        robot_id=robot_id,
        observation_plan=observation_plan,
        action_plan=action_plan,
        input_requirements=input_requirements,
        observation_group_widths=observation_group_widths,
        num_actions=num_actions,
        schema_version=schema_version,
    )


def write_export_contract(path: Path, contract: ExportContract) -> Path:
    """Atomically write an ``export_contract.yaml`` file."""

    serialized = export_contract_to_yaml(contract)
    target = path
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            dir=target.parent,
            prefix=f".{target.name}.",
            suffix=".tmp",
            delete=False,
        ) as stream:
            temporary = Path(stream.name)
            stream.write(serialized)
            if not serialized.endswith("\n"):
                stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, target)
    finally:
        if temporary is not None and temporary.exists():
            temporary.unlink()
    return target


def load_export_contract(path: Path) -> ExportContract:
    """Read and validate a persisted ``export_contract.yaml`` file."""

    try:
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError) as exc:
        raise _contract_error(f"cannot read export contract: {exc}", path=str(path)) from exc
    return export_contract_from_yaml(text, path=str(path))


__all__ = [
    "EXPORT_CONTRACT_FILENAME",
    "EXPORT_CONTRACT_SCHEMA_VERSION",
    "ExportContract",
    "export_contract_from_yaml",
    "export_contract_to_yaml",
    "load_export_contract",
    "write_export_contract",
]
