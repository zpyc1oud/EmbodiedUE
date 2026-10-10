"""PhantomX command channels for DirectTask observation ``command`` ops."""

from __future__ import annotations

import math
import random
from collections.abc import Mapping
from dataclasses import dataclass, field

import torch

from uerl.core.direct.task import CommandSource
from uerl.errors import ConfigError
from uerl.tasks.phantomx.config import (
    PHANTOMX_COMMAND_FIELDS,
    PhantomXCommandConfig,
    PhantomXCommandSampling,
    PhantomXTaskConfig,
)
from uerl.tasks.phantomx.pursuit import (
    PHANTOMX_PURSUIT_TARGET_FIELD,
    PhantomXPursuitCommand,
)


def yaw_rate_from_heading_error(
    heading_error: torch.Tensor,
    *,
    stiffness: float,
    max_yaw_rate: float,
) -> torch.Tensor:
    """Body-frame yaw rate shared by walk and pursuit."""

    return torch.clamp(stiffness * heading_error, -max_yaw_rate, max_yaw_rate)


def velocity_command_from_latches(
    *,
    pose: torch.Tensor,
    initial_linear: torch.Tensor,
    heading: torch.Tensor,
    post_turn_linear: torch.Tensor,
    config: PhantomXTaskConfig,
) -> torch.Tensor:
    """Body-frame planar velocity command (matches legacy ``PhantomXTask._velocity_command``)."""

    current_heading = _quat_heading(pose[:, 3:7])
    heading_flat = heading.reshape(pose.shape[0])
    heading_error = torch.atan2(
        torch.sin(heading_flat - current_heading),
        torch.cos(heading_flat - current_heading),
    )
    turn_started = torch.linalg.vector_norm(pose[:, :2], dim=1) >= config.turn_start_distance
    yaw_rate = torch.where(
        turn_started,
        yaw_rate_from_heading_error(
            heading_error,
            stiffness=config.heading_control_stiffness,
            max_yaw_rate=config.max_yaw_rate,
        ),
        torch.zeros_like(heading_error),
    )
    turn_completed = turn_started & (heading_error.abs() <= config.turn_completion_tolerance)
    linear = torch.where(turn_completed.unsqueeze(1), post_turn_linear, initial_linear)
    return torch.cat((linear, yaw_rate.unsqueeze(1)), dim=1)


@dataclass(slots=True)
class PhantomXVelocityCommandSource:
    """Publish ``velocity`` from episode latches owned by this command item."""

    config: PhantomXTaskConfig
    batch_size: int
    device: str | torch.device = "cpu"
    run_seed: int = 0
    _latch_source: CommandSource | None = None
    _curriculum_term: object | None = None
    _velocity: torch.Tensor = field(init=False, repr=False)
    _sampled: torch.Tensor = field(init=False, repr=False)
    _initial_linear: torch.Tensor = field(init=False, repr=False)
    _target_heading: torch.Tensor = field(init=False, repr=False)
    _post_turn_linear: torch.Tensor = field(init=False, repr=False)
    _turn_enabled: torch.Tensor = field(init=False, repr=False)
    _episode_counts: torch.Tensor = field(init=False, repr=False)
    _published_heading: torch.Tensor = field(init=False, repr=False)
    _published_post_turn: torch.Tensor = field(init=False, repr=False)
    _standing: torch.Tensor = field(init=False, repr=False)
    _turn_in_place: torch.Tensor = field(init=False, repr=False)
    _sampled_yaw: torch.Tensor = field(init=False, repr=False)
    _time_left: torch.Tensor = field(init=False, repr=False)
    _resample_counts: torch.Tensor = field(init=False, repr=False)
    _pending_dt: float | None = field(init=False, repr=False, default=None)

    def __post_init__(self) -> None:
        self._velocity = torch.zeros((self.batch_size, 3), dtype=torch.float32, device=self.device)
        self._sampled = torch.zeros(self.batch_size, dtype=torch.bool, device=self.device)
        self._initial_linear = torch.zeros((self.batch_size, 2), dtype=torch.float32, device=self.device)
        self._target_heading = torch.zeros((self.batch_size, 1), dtype=torch.float32, device=self.device)
        self._post_turn_linear = torch.zeros((self.batch_size, 2), dtype=torch.float32, device=self.device)
        self._turn_enabled = torch.zeros(self.batch_size, dtype=torch.bool, device=self.device)
        self._episode_counts = torch.full((self.batch_size,), -1, dtype=torch.long, device=self.device)
        self._published_heading = torch.zeros((self.batch_size, 1), dtype=torch.float32, device=self.device)
        self._published_post_turn = torch.zeros((self.batch_size, 2), dtype=torch.float32, device=self.device)
        self._standing = torch.zeros(self.batch_size, dtype=torch.bool, device=self.device)
        self._turn_in_place = torch.zeros(self.batch_size, dtype=torch.bool, device=self.device)
        self._sampled_yaw = torch.zeros(self.batch_size, dtype=torch.float32, device=self.device)
        self._time_left = torch.zeros(self.batch_size, dtype=torch.float32, device=self.device)
        self._resample_counts = torch.zeros(self.batch_size, dtype=torch.long, device=self.device)
        self._pending_dt = None

    def bind_curriculum(self, source: CommandSource) -> None:
        """Accept latch tensors supplied by a caller that is not the curriculum term."""

        self._latch_source = source

    def bind_curriculum_manager(self, manager: object) -> None:
        """Read turn stage and sampling ranges from the command curriculum term."""

        from uerl.tasks.phantomx.curriculum import PhantomXCommandCurriculum

        terms = getattr(manager, "terms", {})
        for term in terms.values():
            if isinstance(term, PhantomXCommandCurriculum):
                self._curriculum_term = term
                return

    def bind_curriculum_term(self, term: object) -> None:
        self._curriculum_term = term

    def channels(self) -> Mapping[str, int]:
        return {"velocity": 3}

    def note_control_dt(self, dt: float) -> None:
        """Record the completed Control frame length that the next update consumes."""

        if not math.isfinite(dt) or dt < 0.0:
            raise ConfigError(
                f"control frame dt must be finite and non-negative, got {dt}",
                code="CONFIG_OUT_OF_RANGE",
                path="dt",
            )
        self._pending_dt = float(dt)

    def update(self, raw_state: Mapping[str, torch.Tensor]) -> None:
        pose = _require_pose(raw_state)
        batch = int(pose.shape[0])
        sampled = self._sampled
        if bool(sampled.any()) and batch != self.batch_size:
            raise ConfigError(
                "command update requires the full Slot batch",
                code="CONFIG_OUT_OF_RANGE",
                path="raw_state",
            )
        self._advance_timer(pose)
        if not bool(self._sampled.any()):
            latches = self._latches_from_caller(raw_state, batch)
            self._published_heading = latches[PHANTOMX_COMMAND_FIELDS[1]].clone()
            self._published_post_turn = latches[PHANTOMX_COMMAND_FIELDS[2]].clone()
            self._velocity = velocity_command_from_latches(
                pose=pose,
                initial_linear=latches[PHANTOMX_COMMAND_FIELDS[0]],
                heading=latches[PHANTOMX_COMMAND_FIELDS[1]],
                post_turn_linear=latches[PHANTOMX_COMMAND_FIELDS[2]],
                config=self.config,
            )
            return
        own = self._velocity_from_own_samples(pose)
        published = self._publish_sampled(pose)
        if bool((~self._sampled).any()):
            caller = self._latches_from_caller(raw_state, batch)
            caller_velocity = velocity_command_from_latches(
                pose=pose,
                initial_linear=caller[PHANTOMX_COMMAND_FIELDS[0]],
                heading=caller[PHANTOMX_COMMAND_FIELDS[1]],
                post_turn_linear=caller[PHANTOMX_COMMAND_FIELDS[2]],
                config=self.config,
            )
            selected = self._sampled.reshape(-1, 1)
            self._velocity = torch.where(selected, own, caller_velocity)
            self._published_heading = torch.where(
                selected, published[PHANTOMX_COMMAND_FIELDS[1]], caller[PHANTOMX_COMMAND_FIELDS[1]]
            )
            self._published_post_turn = torch.where(
                selected, published[PHANTOMX_COMMAND_FIELDS[2]], caller[PHANTOMX_COMMAND_FIELDS[2]]
            )
            return
        self._velocity = own
        self._published_heading = published[PHANTOMX_COMMAND_FIELDS[1]].clone()
        self._published_post_turn = published[PHANTOMX_COMMAND_FIELDS[2]].clone()

    def refresh(self, raw_state: Mapping[str, torch.Tensor]) -> None:
        self.update(raw_state)

    def reset(self, reset_mask: torch.Tensor, post_reset_state: Mapping[str, torch.Tensor]) -> None:
        """Sample episode latches for selected Slots, then publish velocity."""

        pose = _require_pose(post_reset_state)
        reset_mask = reset_mask.to(device=self.device, dtype=torch.bool).reshape(-1)
        if int(pose.shape[0]) != self.batch_size or int(reset_mask.shape[0]) != self.batch_size:
            raise ConfigError(
                "command reset requires one row per Slot",
                code="CONFIG_OUT_OF_RANGE",
                path="reset_mask",
            )
        self._pending_dt = None
        self._sample_rows(reset_mask, pose, new_episode=True)
        self._sampled[reset_mask] = True
        self.update(post_reset_state)

    def _advance_timer(self, pose: torch.Tensor) -> None:
        dt = self._pending_dt
        self._pending_dt = None
        if dt is None or not bool(self._sampled.any()):
            return
        self._time_left -= dt
        expired = self._sampled & (self._time_left <= 0.0)
        if bool(expired.any()):
            self._sample_rows(expired, pose, new_episode=False)

    def _sample_rows(self, mask: torch.Tensor, pose: torch.Tensor, *, new_episode: bool) -> None:
        ranges = self._ranges()
        uniform_velocity = ranges.sampling == PhantomXCommandSampling.UNIFORM_VELOCITY
        if (
            ranges.lateral_speed_min > ranges.lateral_speed_max
            or (uniform_velocity and ranges.turn_in_place_probability > 0.0)
            or ranges.resampling_time_min_s <= 0.0
            or ranges.resampling_time_min_s > ranges.resampling_time_max_s
            or not 0.0 <= ranges.standing_probability <= 1.0
            or not 0.0 <= ranges.turn_in_place_probability <= 1.0
            or ranges.standing_probability + ranges.turn_in_place_probability > 1.0
            or ranges.yaw_rate_min > ranges.yaw_rate_max
            or (ranges.heading_command and ranges.turn_in_place_probability > 0.0)
        ):
            raise ConfigError(
                "command resampling range or standing probability is invalid",
                code="CONFIG_OUT_OF_RANGE",
                path="command",
            )
        current_heading = _quat_heading(pose[:, 3:7])
        if new_episode:
            self._turn_enabled[mask] = True if uniform_velocity else self._turn_flags()[mask]
            self._episode_counts[mask] += 1
            self._resample_counts[mask] = 0
        else:
            self._resample_counts[mask] += 1
        seed = self._seed()
        for slot_id in torch.nonzero(mask, as_tuple=False).flatten().tolist():
            generator = random.Random(
                seed
                + 1_000_003 * slot_id
                + 97_409 * int(self._episode_counts[slot_id])
                + 17 * int(self._resample_counts[slot_id])
            )
            initial_speed = generator.uniform(ranges.initial_speed_min, ranges.initial_speed_max)
            post_turn_speed = generator.uniform(ranges.post_turn_speed_min, ranges.post_turn_speed_max)
            heading_delta = generator.uniform(ranges.heading_delta_min, ranges.heading_delta_max)
            self._initial_linear[slot_id] = torch.tensor(
                (initial_speed, 0.0), dtype=torch.float32, device=self.device
            )
            self._post_turn_linear[slot_id] = torch.tensor(
                (post_turn_speed, 0.0), dtype=torch.float32, device=self.device
            )
            self._target_heading[slot_id, 0] = torch.atan2(
                torch.sin(current_heading[slot_id] + heading_delta),
                torch.cos(current_heading[slot_id] + heading_delta),
            )
            self._time_left[slot_id] = generator.uniform(
                ranges.resampling_time_min_s, ranges.resampling_time_max_s
            )
            mixture = generator.random()
            self._standing[slot_id] = mixture < ranges.standing_probability
            self._turn_in_place[slot_id] = (
                ranges.standing_probability <= mixture
                < ranges.standing_probability + ranges.turn_in_place_probability
            )
            if not ranges.heading_command:
                self._sampled_yaw[slot_id] = generator.uniform(ranges.yaw_rate_min, ranges.yaw_rate_max)
            if uniform_velocity:
                self._initial_linear[slot_id, 1] = generator.uniform(
                    ranges.lateral_speed_min, ranges.lateral_speed_max
                )
                self._post_turn_linear[slot_id] = self._initial_linear[slot_id]
                # This mode samples an absolute world heading, as UniformVelocityCommand does.
                self._target_heading[slot_id, 0] = heading_delta

    def _velocity_from_own_samples(self, pose: torch.Tensor) -> torch.Tensor:
        if not self._ranges().heading_command:
            # Direct yaw commands are available from the first episode and stay
            # fixed for the sampled physical interval, independent of body pose.
            speed = torch.where(
                self._standing | self._turn_in_place, 0.0, self._initial_linear[:, 0]
            )
            yaw = torch.where(self._standing, 0.0, self._sampled_yaw)
            lateral = torch.where(
                self._standing | self._turn_in_place, 0.0, self._initial_linear[:, 1]
            )
            return torch.stack((speed, lateral, yaw), dim=1)
        current_heading = _quat_heading(pose[:, 3:7])
        target = self._target_heading.reshape(-1)
        heading_error = torch.atan2(
            torch.sin(target - current_heading),
            torch.cos(target - current_heading),
        )
        yaw = torch.clamp(
            self.config.heading_control_stiffness * heading_error,
            -self.config.max_yaw_rate,
            self.config.max_yaw_rate,
        )
        active_turn = self._turn_enabled & ~self._standing
        yaw = torch.where(active_turn, yaw, torch.zeros_like(yaw))
        speed = torch.where(
            self._standing,
            torch.zeros_like(self._initial_linear[:, 0]),
            self._initial_linear[:, 0],
        )
        lateral = torch.where(self._standing, 0.0, self._initial_linear[:, 1])
        return torch.stack((speed, lateral, yaw), dim=1)

    def episode_latches(self) -> Mapping[str, torch.Tensor]:
        """Return the published episode latches, including pose-relative heading."""

        return {
            PHANTOMX_COMMAND_FIELDS[0]: self._initial_linear,
            PHANTOMX_COMMAND_FIELDS[1]: self._published_heading,
            PHANTOMX_COMMAND_FIELDS[2]: self._published_post_turn,
        }

    def current(self) -> Mapping[str, torch.Tensor]:
        return {"velocity": self._velocity}

    def _publish_sampled(self, pose: torch.Tensor) -> Mapping[str, torch.Tensor]:
        current_heading = _quat_heading(pose[:, 3:7]).unsqueeze(1)
        heading = torch.where(self._turn_enabled.unsqueeze(1), self._target_heading, current_heading)
        post_turn = torch.where(
            self._turn_enabled.unsqueeze(1), self._post_turn_linear, self._initial_linear
        )
        self._published_heading = heading
        self._published_post_turn = post_turn
        return {
            PHANTOMX_COMMAND_FIELDS[0]: self._initial_linear,
            PHANTOMX_COMMAND_FIELDS[1]: heading,
            PHANTOMX_COMMAND_FIELDS[2]: post_turn,
        }

    def _latches_from_caller(
        self, raw_state: Mapping[str, torch.Tensor], batch: int
    ) -> Mapping[str, torch.Tensor]:
        latches = (
            self._latch_source.current()
            if self._latch_source is not None
            else {
                name: raw_state[name]
                for name in PHANTOMX_COMMAND_FIELDS
                if name in raw_state
            }
        )
        missing = [name for name in PHANTOMX_COMMAND_FIELDS if name not in latches]
        if missing:
            raise ConfigError(
                f"missing PhantomX command latch fields: {missing}",
                code="CONFIG_MISSING_FIELD",
                path="command_source.latches",
            )
        return {
            PHANTOMX_COMMAND_FIELDS[0]: latches[PHANTOMX_COMMAND_FIELDS[0]].reshape(batch, 2),
            PHANTOMX_COMMAND_FIELDS[1]: latches[PHANTOMX_COMMAND_FIELDS[1]].reshape(batch, 1),
            PHANTOMX_COMMAND_FIELDS[2]: latches[PHANTOMX_COMMAND_FIELDS[2]].reshape(batch, 2),
        }

    def _turn_flags(self) -> torch.Tensor:
        term = self._curriculum_term
        flags = getattr(term, "turn_enabled", None)
        if isinstance(flags, torch.Tensor):
            return flags.to(device=self.device, dtype=torch.bool)
        return torch.zeros(self.batch_size, dtype=torch.bool, device=self.device)

    def _ranges(self) -> PhantomXCommandConfig:
        term = self._curriculum_term
        ranges = getattr(term, "command_config", None)
        if isinstance(ranges, PhantomXCommandConfig):
            return ranges
        return self.config.command

    def _seed(self) -> int:
        term = self._curriculum_term
        seed = getattr(term, "run_seed", None)
        return self.run_seed if seed is None else int(seed)


@dataclass(slots=True)
class PhantomXPursuitCommandSource:
    """Steer the trained walk policy toward an environment pursuit target."""

    config: PhantomXTaskConfig
    pursuit: PhantomXPursuitCommand
    batch_size: int
    device: str | torch.device = "cpu"
    _velocity: torch.Tensor = field(init=False, repr=False)

    def __post_init__(self) -> None:
        self._velocity = torch.zeros(
            (self.batch_size, 3), dtype=torch.float32, device=self.device
        )

    def channels(self) -> Mapping[str, int]:
        return {"velocity": 3}

    def refresh(self, raw_state: Mapping[str, torch.Tensor]) -> None:
        pose_names = [name for name in raw_state if name.endswith(".body_pose")]
        if len(pose_names) != 1:
            raise ConfigError(
                f"expected exactly one body_pose field, found {len(pose_names)}",
                code="CONFIG_MISSING_FIELD",
                path="raw_state.body_pose",
            )
        if PHANTOMX_PURSUIT_TARGET_FIELD not in raw_state:
            raise ConfigError(
                f"missing pursuit target field {PHANTOMX_PURSUIT_TARGET_FIELD!r}",
                code="CONFIG_MISSING_FIELD",
                path=PHANTOMX_PURSUIT_TARGET_FIELD,
            )
        pose = raw_state[pose_names[0]]
        batch = int(pose.shape[0])
        pose = pose.reshape(batch, -1)
        target = raw_state[PHANTOMX_PURSUIT_TARGET_FIELD].reshape(batch, 3)
        delta = target[:, :2] - pose[:, :2]
        distance = torch.linalg.vector_norm(delta, dim=1)
        target_heading = torch.atan2(delta[:, 1], delta[:, 0])
        current_heading = _quat_heading(pose[:, 3:7])
        heading_error = torch.atan2(
            torch.sin(target_heading - current_heading),
            torch.cos(target_heading - current_heading),
        )
        pursuing = distance > self.pursuit.stopping_distance_m
        forward_speed = torch.where(
            pursuing,
            torch.full_like(distance, self.pursuit.speed_mps),
            torch.zeros_like(distance),
        )
        yaw_rate = torch.where(
            pursuing,
            yaw_rate_from_heading_error(
                heading_error,
                stiffness=self.config.heading_control_stiffness,
                max_yaw_rate=self.config.max_yaw_rate,
            ),
            torch.zeros_like(distance),
        )
        self._velocity = torch.stack(
            (forward_speed, torch.zeros_like(forward_speed), yaw_rate), dim=1
        )

    def current(self) -> Mapping[str, torch.Tensor]:
        return {"velocity": self._velocity}

    def update(self, raw_state: Mapping[str, torch.Tensor]) -> None:
        self.refresh(raw_state)

    def reset(
        self,
        reset_mask: torch.Tensor,
        post_reset_state: Mapping[str, torch.Tensor],
    ) -> None:
        del reset_mask
        self.update(post_reset_state)


def _require_pose(raw_state: Mapping[str, torch.Tensor]) -> torch.Tensor:
    pose_names = [name for name in raw_state if name.endswith(".body_pose")]
    if len(pose_names) != 1:
        raise ConfigError(
            f"expected exactly one body_pose field, found {len(pose_names)}",
            code="CONFIG_MISSING_FIELD",
            path="raw_state.body_pose",
        )
    pose = raw_state[pose_names[0]]
    return pose.reshape(int(pose.shape[0]), -1)


def _quat_heading(rotation: torch.Tensor) -> torch.Tensor:
    x, y, z, w = rotation.unbind(dim=1)
    return torch.atan2(2.0 * (w * z + x * y), 1.0 - 2.0 * (y.square() + z.square()))


__all__ = [
    "PhantomXPursuitCommandSource",
    "PhantomXVelocityCommandSource",
    "velocity_command_from_latches",
    "yaw_rate_from_heading_error",
]
