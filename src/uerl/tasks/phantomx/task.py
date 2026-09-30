"""PhantomX DirectTask with velocity-command refresh and task metrics."""

from __future__ import annotations

from collections.abc import Callable, Mapping, Sequence
from typing import Any, cast

import torch

from uerl.core.config.models import DirectTaskConfig
from uerl.core.config.robot import RobotSpec
from uerl.core.direct.robot_observation import ObservationShapeTable, robot_observation_schema
from uerl.core.direct.task import CommandSource, DirectTask, DirectTaskCfg
from uerl.core.direct.types import SessionSchema, StepContext, TerminationResult
from uerl.errors import ConfigError
from uerl.tasks.phantomx.commands import (
    PhantomXPursuitCommandSource,
    PhantomXVelocityCommandSource,
)
from uerl.tasks.phantomx.composed import (
    build_phantomx_continuous_terrain_cfg,
    build_phantomx_discrete_terrain_cfg,
    build_phantomx_walk_cfg,
)
from uerl.tasks.phantomx.config import PhantomXTaskConfig, load_phantomx_training_config
from uerl.tasks.phantomx.generalization import collect_generalization_metrics
from uerl.tasks.phantomx.mdp_terms import collect_phantomx_metrics
from uerl.tasks.phantomx.pursuit import (
    DEFAULT_PHANTOMX_PURSUIT_COMMAND,
    PHANTOMX_PURSUIT_TARGET_DESCRIPTOR,
    PhantomXPursuitCommand,
)

_VelocitySource = PhantomXVelocityCommandSource | PhantomXPursuitCommandSource
_CfgBuilder = Callable[[PhantomXTaskConfig, RobotSpec], DirectTaskCfg]


class PhantomXTask(DirectTask):
    """DirectTask that refreshes the ``velocity`` command channel each step."""

    def __init__(
        self,
        params: PhantomXTaskConfig | DirectTaskConfig | None = None,
        *,
        control_dt: float | None = None,
        batch_size: int = 1,
        device: str | torch.device = "cpu",
        robot_spec: RobotSpec | None = None,
        command_source: _VelocitySource | None = None,
        cfg_builder: _CfgBuilder | None = None,
        extra_state: Sequence[Mapping[str, Any]] = (),
        observation_shapes: ObservationShapeTable | None = None,
    ) -> None:
        params = PhantomXTaskConfig.from_direct(
            PhantomXTaskConfig() if params is None else params
        )
        if control_dt is None:
            # Fallback for direct callers that omit dt: the bootstrap contract
            # starts observations at DtMin (spec section 3).
            training = load_phantomx_training_config()
            control_dt = training.worker.physics_dt * training.worker.decimation[0]
        if control_dt <= 0.0:
            raise ValueError(f"control_dt must be positive, got {control_dt!r}")
        self._control_dt = float(control_dt)
        self.phantomx_config = params
        self._velocity_source = command_source or PhantomXVelocityCommandSource(
            config=params,
            batch_size=batch_size,
            device=device,
        )
        self._extra_state = tuple(extra_state)
        self._noise_generator: torch.Generator | None = None
        builder = cfg_builder or (
            lambda task_params, spec: build_phantomx_walk_cfg(
                task_params,
                spec,
                control_dt=control_dt,
            )
        )
        super().__init__(
            robot_spec=robot_spec,
            control_dt=control_dt,
            batch_size=batch_size,
            device=device,
            command_source=self._velocity_source,
            cfg_factory=lambda spec: builder(params, spec),
            task_config=params,
            observation_shapes=observation_shapes,
        )

    @property
    def command_source(self) -> CommandSource:
        return super().command_source

    @property
    def schema(self) -> SessionSchema:
        base = super().schema
        if not self._extra_state:
            return base
        return SessionSchema(
            (*base.state_requirements, *self._extra_state),
            base.action_schema,
        )

    def align_batch(self, batch_size: int) -> None:
        """Rebuild the command publisher when the Session Slot count differs."""

        super().align_batch(batch_size)
        source = self._command_source
        if isinstance(source, PhantomXVelocityCommandSource):
            if source.batch_size == batch_size:
                return
            self._velocity_source = PhantomXVelocityCommandSource(
                config=source.config,
                batch_size=batch_size,
                device=source.device,
                run_seed=source.run_seed,
            )
        elif isinstance(source, PhantomXPursuitCommandSource):
            if source.batch_size == batch_size:
                return
            self._velocity_source = PhantomXPursuitCommandSource(
                config=source.config,
                pursuit=source.pursuit,
                batch_size=batch_size,
                device=source.device,
            )
        else:
            return
        self._command_source = self._velocity_source

    def use_command_source(self, source: CommandSource) -> None:
        super().use_command_source(source)
        if isinstance(source, (PhantomXVelocityCommandSource, PhantomXPursuitCommandSource)):
            self._velocity_source = source

    def build_observations(
        self,
        raw_state: Mapping[str, torch.Tensor],
        state_valid: torch.Tensor,
        previous_policy_actions: torch.Tensor,
        control_frame_dt: torch.Tensor | None = None,
    ) -> Mapping[str, torch.Tensor]:
        # Direct task callers (unit tests and offline plan probes) historically
        # omitted dt.  The environment supplies the actual completed interval;
        # this fallback keeps those callers on the valid bootstrap contract.
        if control_frame_dt is None:
            control_frame_dt = torch.full(
                (previous_policy_actions.shape[0],),
                self._control_dt,
                dtype=previous_policy_actions.dtype,
                device=previous_policy_actions.device,
            )
        groups = dict(
            super().build_observations(
                raw_state, state_valid, previous_policy_actions, control_frame_dt
            )
        )
        policy = groups["policy"]
        groups["critic"] = torch.cat((policy, self._contact_force_block(raw_state)), dim=1)
        groups["policy"] = self._corrupt_policy(policy)
        return groups

    def enable_observation_corruption(self, generator: torch.Generator) -> None:
        """Add training-only noise to the actor Observation. The plan stays clean."""

        self._noise_generator = generator

    def _contact_force_block(self, raw_state: Mapping[str, torch.Tensor]) -> torch.Tensor:
        if self.robot_spec is None:
            raise ConfigError(
                "contact-force critic block requires a bound RobotSpec",
                code="CONFIG_MISSING_FIELD",
                path="robot_spec",
            )
        names = [
            cast(str, item["name"])
            for item in robot_observation_schema(self.robot_spec, self._require_observation_shapes())
            if item["semantic"] == "contact_force"
        ]
        columns = []
        for name in names:
            if name not in raw_state:
                raise ConfigError(
                    f"missing contact-force field {name!r}",
                    code="CONFIG_MISSING_FIELD",
                    path=name,
                )
            value = raw_state[name]
            columns.append(value.reshape(value.shape[0], -1))
        return torch.cat(columns, dim=1)

    def _corrupt_policy(self, policy: torch.Tensor) -> torch.Tensor:
        generator = self._noise_generator
        if generator is None:
            return policy
        noise = self.phantomx_config.observation_noise
        std_by_member = {
            "lin_vel_b": noise.lin_vel_b,
            "ang_vel_b": noise.ang_vel_b,
            "gravity_b": noise.gravity_b,
            "joint_pos_rel": noise.joint_pos_rel,
            "joint_vel": noise.joint_vel,
        }
        plan = self.observation_plan
        produced = {op.output: op.width for op in plan.ops}
        corrupted = policy.clone()
        offset = 0
        for member in plan.groups["policy"]:
            width = produced[member]
            std = std_by_member.get(member, 0.0)
            if std > 0.0:
                draw = torch.empty((policy.shape[0], width), dtype=policy.dtype)
                draw.normal_(mean=0.0, std=float(std), generator=generator)
                corrupted[:, offset : offset + width] += draw.to(device=policy.device)
            offset += width
        return corrupted

    def collect_metrics(
        self,
        context: StepContext,
        terminations: TerminationResult,
    ) -> Mapping[str, torch.Tensor | float]:
        context = self.context_with_command(context)
        metrics: dict[str, torch.Tensor | float] = dict(super().collect_metrics(context, terminations))
        if self.robot_spec is None:
            return metrics
        metrics.update(
            collect_phantomx_metrics(
                context,
                terminations,
                config=self.phantomx_config,
                robot_spec=self.robot_spec,
            )
        )
        if isinstance(self._velocity_source, PhantomXPursuitCommandSource):
            metrics.update(
                collect_generalization_metrics(
                    context,
                    robot_spec=self.robot_spec,
                    pursuit=True,
                )
            )
        return metrics


def create_phantomx_direct_task(
    config: DirectTaskConfig,
    *,
    robot_spec: RobotSpec | None = None,
    control_dt: float,
    batch_size: int,
    device: str | torch.device,
    cfg_builder: _CfgBuilder | None = None,
    observation_shapes: ObservationShapeTable | None = None,
) -> PhantomXTask:
    """Create the generic PhantomX walking DirectTask."""

    params = PhantomXTaskConfig.from_direct(config)
    source = PhantomXVelocityCommandSource(
        config=params,
        batch_size=batch_size,
        device=device,
    )
    return PhantomXTask(
        params=params,
        control_dt=control_dt,
        batch_size=batch_size,
        device=device,
        robot_spec=robot_spec,
        command_source=source,
        cfg_builder=cfg_builder,
        observation_shapes=observation_shapes,
    )


def create_phantomx_continuous_terrain_direct_task(
    config: DirectTaskConfig,
    *,
    robot_spec: RobotSpec | None = None,
    control_dt: float,
    batch_size: int,
    device: str | torch.device,
    observation_shapes: ObservationShapeTable | None = None,
) -> PhantomXTask:
    """Create PhantomX DirectTask with continuous terrain declarations."""

    return create_phantomx_direct_task(
        config,
        robot_spec=robot_spec,
        control_dt=control_dt,
        batch_size=batch_size,
        device=device,
        cfg_builder=lambda params, spec: build_phantomx_continuous_terrain_cfg(
            params,
            spec,
            control_dt=control_dt,
        ),
        observation_shapes=observation_shapes,
    )


def create_phantomx_discrete_terrain_direct_task(
    config: DirectTaskConfig,
    *,
    robot_spec: RobotSpec | None = None,
    control_dt: float,
    batch_size: int,
    device: str | torch.device,
    observation_shapes: ObservationShapeTable | None = None,
) -> PhantomXTask:
    """Create PhantomX DirectTask with discrete terrain declarations."""

    return create_phantomx_direct_task(
        config,
        robot_spec=robot_spec,
        control_dt=control_dt,
        batch_size=batch_size,
        device=device,
        cfg_builder=lambda params, spec: build_phantomx_discrete_terrain_cfg(
            params,
            spec,
            control_dt=control_dt,
        ),
        observation_shapes=observation_shapes,
    )


def create_phantomx_pursuit_direct_task(
    config: DirectTaskConfig,
    *,
    robot_spec: RobotSpec | None = None,
    control_dt: float,
    batch_size: int = 1,
    device: str | torch.device = "cpu",
    pursuit: PhantomXPursuitCommand = DEFAULT_PHANTOMX_PURSUIT_COMMAND,
    observation_shapes: ObservationShapeTable | None = None,
) -> PhantomXTask:
    """Create PhantomX DirectTask with the authored pursuit command source."""

    params = PhantomXTaskConfig.from_direct(config)
    source = PhantomXPursuitCommandSource(
        config=params,
        pursuit=pursuit,
        batch_size=batch_size,
        device=device,
    )
    return PhantomXTask(
        params=params,
        control_dt=control_dt,
        batch_size=batch_size,
        device=device,
        robot_spec=robot_spec,
        command_source=source,
        extra_state=(PHANTOMX_PURSUIT_TARGET_DESCRIPTOR,),
        observation_shapes=observation_shapes,
    )


__all__ = [
    "PhantomXTask",
    "create_phantomx_continuous_terrain_direct_task",
    "create_phantomx_direct_task",
    "create_phantomx_discrete_terrain_direct_task",
    "create_phantomx_pursuit_direct_task",
]
