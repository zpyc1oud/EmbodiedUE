"""Deterministic PhantomX scene-generalization evaluation primitives."""

from __future__ import annotations

from collections.abc import Collection, Mapping
from dataclasses import dataclass, replace
from pathlib import Path
from typing import Final, Literal, cast, final

import torch
from typing_extensions import override

from ...core.config.models import ResolvedRunConfig
from ...core.config.robot import ObservationConfig, ObsType, RobotSpec
from ...core.direct.types import StepContext
from ...core.mdp.lib.events import (
    push_root,
    randomize_ground_friction,
    randomize_terrain_tier_params,
)
from ...core.mdp.managers.event import EventManager
from ...core.mdp.terms import EventCfg, EventTermCfg
from ...errors import ConfigError
from ..evaluation import EvaluationSummary, TaskEvaluator
from .config import PHANTOMX_FEET
from .pursuit import (
    DEFAULT_PHANTOMX_PURSUIT_COMMAND,
    PHANTOMX_PURSUIT_TARGET_FIELD,
)

GENERALIZATION_ROOT_HEIGHT_METRIC: Final = "phantomx/generalization/root_height"
GENERALIZATION_PURSUIT_DISTANCE_METRIC: Final = "phantomx/generalization/pursuit_distance"
GENERALIZATION_NON_FOOT_CONTACT_METRIC: Final = "phantomx/generalization/non_foot_contact"

GeneralizationEventName = Literal["ground_friction", "terrain_tier_params", "root_push"]
GENERALIZATION_EVENT_NAMES: Final[tuple[GeneralizationEventName, ...]] = (
    "ground_friction",
    "terrain_tier_params",
    "root_push",
)
GENERALIZATION_FRICTION_RANGE: Final = ((0.7, 1.1), (0.5, 0.9))
GENERALIZATION_ROUGHNESS_RANGE: Final = (0.0, 1.0)
GENERALIZATION_PUSH_RANGE_MPS: Final = (-0.2, 0.2)
GENERALIZATION_PUSH_INTERVAL_RANGE_S: Final = (1.0, 1.5)


@dataclass(frozen=True, slots=True)
class GeneralizationEvaluationResult:
    """Report the four fixed metrics used by ticket35."""

    task_id: str
    checkpoint: Path
    steps: int
    completed_episodes: int
    survival_seconds: float
    pursuit_success_rate: float
    mean_root_height: float
    contact_violation_rate: float


def create_generalization_event_manager(
    spec: RobotSpec,
    *,
    batch_size: int,
    device: str | torch.device,
    run_seed: int,
    enabled_terms: Collection[str] | None = None,
) -> EventManager:
    """Build selected ticket35 randomization terms on the existing EventManager path."""

    generator = torch.Generator(device="cpu")
    generator.manual_seed(run_seed)
    term_cfgs: dict[str, EventTermCfg] = {
        "ground_friction": EventTermCfg(
            func=randomize_ground_friction,
            mode="startup",
            params={
                "static_range": GENERALIZATION_FRICTION_RANGE[0],
                "dynamic_range": GENERALIZATION_FRICTION_RANGE[1],
            },
        ),
        "terrain_tier_params": EventTermCfg(
            func=randomize_terrain_tier_params,
            mode="reset",
            params={"roughness_scale_range": GENERALIZATION_ROUGHNESS_RANGE},
        ),
        "root_push": EventTermCfg(
            func=push_root,
            mode="interval",
            interval_range_s=GENERALIZATION_PUSH_INTERVAL_RANGE_S,
            params={"velocity_range_mps": GENERALIZATION_PUSH_RANGE_MPS},
        ),
    }
    selected = set(GENERALIZATION_EVENT_NAMES if enabled_terms is None else enabled_terms)
    unknown = selected.difference(GENERALIZATION_EVENT_NAMES)
    if unknown:
        raise ConfigError(
            f"unknown generalization event terms: {sorted(unknown)}",
            code="CONFIG_OUT_OF_RANGE",
            path="enabled_terms",
        )
    if not selected:
        raise ConfigError(
            "at least one generalization event term must be enabled",
            code="CONFIG_OUT_OF_RANGE",
            path="enabled_terms",
        )
    return EventManager(
        EventCfg(terms={name: term_cfgs[name] for name in GENERALIZATION_EVENT_NAMES if name in selected}),
        spec,
        batch_size=batch_size,
        device=device,
        generator=generator,
    )


def select_generalization_event_names(
    disabled_terms: Collection[str] = (),
) -> tuple[GeneralizationEventName, ...]:
    """Return the stable event order after validating an ablation selection."""

    disabled = set(disabled_terms)
    unknown = disabled.difference(GENERALIZATION_EVENT_NAMES)
    if unknown:
        raise ConfigError(
            f"unknown generalization event terms: {sorted(unknown)}",
            code="CONFIG_OUT_OF_RANGE",
            path="disabled_terms",
        )
    return tuple(name for name in GENERALIZATION_EVENT_NAMES if name not in disabled)


def with_generalization_contact_telemetry(config: ResolvedRunConfig) -> ResolvedRunConfig:
    """Add evaluator-only contact-force fields without changing the asset contract."""

    semantics = config.worker.robot_semantics
    if semantics is None:
        raise ConfigError(
            "generalization evaluation requires materialised Robot semantics",
            code="ROBOT_CONFIG_MISSING",
            path="worker.robot_semantics",
        )
    declared = {(item.type, item.target, item.target_kind) for item in semantics.observations}
    additions = tuple(
        ObservationConfig(ObsType.CONTACT_FORCE, body_name, "body")
        for body_name in _phantomx_non_foot_bodies()
        if (ObsType.CONTACT_FORCE, body_name, "body") not in declared
    )
    if not additions:
        return config
    return replace(
        config,
        worker=replace(
            config.worker,
            robot_semantics=replace(
                semantics,
                observations=(*semantics.observations, *additions),
            ),
        ),
    )


def collect_generalization_metrics(
    context: StepContext,
    *,
    robot_spec: RobotSpec,
    pursuit: bool,
) -> Mapping[str, torch.Tensor]:
    """Compute evaluator-only physical metrics from the pre-reset transition."""

    pose_name = _body_field_name(robot_spec, ObsType.BODY_POSE, "base_link")
    pose = context.transition_state[pose_name].reshape(context.transition_state[pose_name].shape[0], -1)
    metrics: dict[str, torch.Tensor] = {
        GENERALIZATION_ROOT_HEIGHT_METRIC: pose[:, 2],
        GENERALIZATION_NON_FOOT_CONTACT_METRIC: _non_foot_contact(
            context.transition_state,
            robot_spec=robot_spec,
        ),
    }
    if pursuit:
        if PHANTOMX_PURSUIT_TARGET_FIELD not in context.transition_state:
            raise ConfigError(
                f"missing pursuit target field {PHANTOMX_PURSUIT_TARGET_FIELD!r}",
                code="CONFIG_MISSING_FIELD",
                path=PHANTOMX_PURSUIT_TARGET_FIELD,
            )
        target = context.transition_state[PHANTOMX_PURSUIT_TARGET_FIELD].reshape(pose.shape[0], 3)
        metrics[GENERALIZATION_PURSUIT_DISTANCE_METRIC] = torch.linalg.vector_norm(
            target[:, :2] - pose[:, :2],
            dim=1,
        )
    return metrics


@final
class GeneralizationEvaluator(TaskEvaluator):
    """Aggregate fixed-horizon survival, pursuit, height, and contact metrics."""

    def __init__(
        self,
        sample_hz: float,
        *,
        stopping_distance_m: float = DEFAULT_PHANTOMX_PURSUIT_COMMAND.stopping_distance_m,
    ) -> None:
        if not sample_hz > 0.0:
            raise ValueError("sample_hz must be positive")
        if not stopping_distance_m > 0.0:
            raise ValueError("stopping_distance_m must be positive")
        self._sample_hz = float(sample_hz)
        self._stopping_distance_m = float(stopping_distance_m)
        self._episode_steps: torch.Tensor | None = None
        self._pursuit_reached: torch.Tensor | None = None
        self._survival_sum_seconds = 0.0
        self._survival_count = 0
        self._pursuit_successes = 0
        self._completed_episodes = 0
        self._root_height_sum = 0.0
        self._root_height_samples = 0
        self._contact_violations = 0.0
        self._contact_samples = 0

    @override
    def observe(
        self,
        observations: Mapping[str, torch.Tensor],
        actions: torch.Tensor,
        metrics: Mapping[str, object],
        rewards: torch.Tensor,
        dones: torch.Tensor,
        terminal_episode_lengths: torch.Tensor,
    ) -> None:
        del observations, actions, rewards
        root_height = _metric(metrics, GENERALIZATION_ROOT_HEIGHT_METRIC)
        pursuit_distance = _metric(metrics, GENERALIZATION_PURSUIT_DISTANCE_METRIC)
        non_foot_contact = _metric(metrics, GENERALIZATION_NON_FOOT_CONTACT_METRIC)
        batch_size = int(root_height.numel())
        if batch_size < 1 or any(int(value.numel()) != batch_size for value in (pursuit_distance, non_foot_contact)):
            raise RuntimeError("generalization metrics must have one value per Slot")
        if not all(bool(torch.isfinite(value).all()) for value in (root_height, pursuit_distance, non_foot_contact)):
            raise RuntimeError("generalization metrics must be finite")
        if self._episode_steps is None:
            self._episode_steps = torch.zeros(batch_size, dtype=torch.long, device=root_height.device)
            self._pursuit_reached = torch.zeros(batch_size, dtype=torch.bool, device=root_height.device)
        if self._episode_steps.numel() != batch_size:
            raise RuntimeError("generalization Slot count changed during evaluation")
        reached = cast(torch.Tensor, self._pursuit_reached)
        self._episode_steps += 1
        reached |= pursuit_distance <= self._stopping_distance_m
        self._root_height_sum += float(root_height.sum().item())
        self._root_height_samples += batch_size
        self._contact_violations += float(non_foot_contact.gt(0.0).sum().item())
        self._contact_samples += batch_size

        done = dones.reshape(-1).to(device=root_height.device, dtype=torch.bool)
        lengths = terminal_episode_lengths.reshape(-1).to(device=root_height.device, dtype=torch.long)
        if done.numel() != batch_size or lengths.numel() != batch_size:
            raise RuntimeError("evaluation done metadata must align with metric Slots")
        for slot_id in torch.nonzero(done, as_tuple=False).flatten().tolist():
            length = int(lengths[slot_id].item())
            if length <= 0:
                length = int(self._episode_steps[slot_id].item())
            if length <= 0:
                raise RuntimeError("completed evaluation episode has no positive length")
            self._survival_sum_seconds += length / self._sample_hz
            self._survival_count += 1
            self._pursuit_successes += int(reached[slot_id].item())
            self._completed_episodes += 1
            self._episode_steps[slot_id] = 0
            reached[slot_id] = False

    @override
    def finish(self, summary: EvaluationSummary) -> GeneralizationEvaluationResult:
        active_steps = 0
        active_count = 0
        if self._episode_steps is not None:
            active = self._episode_steps > 0
            active_steps = int(self._episode_steps[active].sum().item())
            active_count = int(active.sum().item())
        survival_count = self._survival_count + active_count
        survival_sum = self._survival_sum_seconds + active_steps / self._sample_hz
        return GeneralizationEvaluationResult(
            task_id=summary.task_id,
            checkpoint=summary.checkpoint,
            steps=summary.steps,
            completed_episodes=summary.completed_episodes,
            survival_seconds=survival_sum / survival_count if survival_count else 0.0,
            pursuit_success_rate=(
                self._pursuit_successes / self._completed_episodes
                if self._completed_episodes
                else 0.0
            ),
            mean_root_height=(
                self._root_height_sum / self._root_height_samples
                if self._root_height_samples
                else 0.0
            ),
            contact_violation_rate=(
                self._contact_violations / self._contact_samples
                if self._contact_samples
                else 0.0
            ),
        )


def _metric(metrics: Mapping[str, object], name: str) -> torch.Tensor:
    matches = [value for key, value in metrics.items() if key == name]
    if len(matches) != 1:
        raise RuntimeError(f"generalization metric {name!r} is missing or duplicated")
    value = matches[0]
    tensor = value if isinstance(value, torch.Tensor) else torch.as_tensor(value)
    return tensor.reshape(-1)


def _body_field_name(robot_spec: RobotSpec, observation_type: ObsType, target: str) -> str:
    for observation in robot_spec.observations:
        if (
            observation.type is observation_type
            and observation.target_kind == "body"
            and observation.target == target
        ):
            return f"robot.body.{target}.{observation_type.value}"
    raise ConfigError(
        f"Robot observation {observation_type.value!r} for body {target!r} is missing",
        code="CONFIG_MISSING_FIELD",
        path=f"robot.body.{target}.{observation_type.value}",
    )


def _non_foot_contact(
    state: Mapping[str, torch.Tensor],
    *,
    robot_spec: RobotSpec,
) -> torch.Tensor:
    fields = [
        f"robot.body.{observation.target}.contact_force"
        for observation in robot_spec.observations
        if observation.type is ObsType.CONTACT_FORCE
        and observation.target_kind == "body"
        and observation.target not in PHANTOMX_FEET
    ]
    if not fields:
        raise ConfigError(
            "generalization evaluation requires non-foot contact-force observations",
            code="CONFIG_MISSING_FIELD",
            path="robot.body.*.contact_force",
        )
    values: list[torch.Tensor] = []
    for field_name in fields:
        if field_name not in state:
            raise ConfigError(
                f"missing state field {field_name!r}",
                code="CONFIG_MISSING_FIELD",
                path=field_name,
            )
        value = state[field_name]
        values.append(value.reshape(value.shape[0], -1).abs().amax(dim=1))
    return torch.stack(values, dim=1).gt(0.0).any(dim=1).to(dtype=torch.float32)


def _phantomx_non_foot_bodies() -> tuple[str, ...]:
    from ...assets.robots.phantomx import PHANTOMX_CFG

    return tuple(body_name for body_name in PHANTOMX_CFG.body_names if body_name not in PHANTOMX_FEET)


__all__ = [
    "GENERALIZATION_EVENT_NAMES",
    "GENERALIZATION_NON_FOOT_CONTACT_METRIC",
    "GENERALIZATION_FRICTION_RANGE",
    "GENERALIZATION_PUSH_INTERVAL_RANGE_S",
    "GENERALIZATION_PUSH_RANGE_MPS",
    "GENERALIZATION_PURSUIT_DISTANCE_METRIC",
    "GENERALIZATION_ROUGHNESS_RANGE",
    "GENERALIZATION_ROOT_HEIGHT_METRIC",
    "GeneralizationEvaluationResult",
    "GeneralizationEvaluator",
    "collect_generalization_metrics",
    "create_generalization_event_manager",
    "select_generalization_event_names",
    "with_generalization_contact_telemetry",
]
