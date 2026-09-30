"""Verify PhantomX stages through the generic curriculum term interface."""

from __future__ import annotations

from collections.abc import Mapping

import torch

from uerl import CurriculumStep
from uerl.tasks.phantomx.commands import PhantomXVelocityCommandSource
from uerl.tasks.phantomx.config import (
    PHANTOMX_COMMAND_FIELDS,
    PhantomXCommandConfig,
    PhantomXCurriculumConfig,
    PhantomXTaskConfig,
)
from uerl.tasks.phantomx.curriculum import PhantomXCommandCurriculum

POSE_FIELD = "robot.body.base_link.body_pose"


def _state(num_envs: int = 2) -> dict[str, torch.Tensor]:
    pose = torch.tensor([[0.7, 0.0, 0.18, 0.0, 0.0, 0.0, 1.0]])
    return {POSE_FIELD: pose.repeat(num_envs, 1)}


def _walk(
    term: PhantomXCommandCurriculum,
    commands: PhantomXCommandConfig,
    *,
    num_envs: int,
    run_seed: int,
) -> PhantomXVelocityCommandSource:
    command = PhantomXVelocityCommandSource(
        config=PhantomXTaskConfig(command=commands),
        batch_size=num_envs,
        device="cpu",
        run_seed=run_seed,
    )
    command.bind_curriculum_term(term)
    return command


def _sample(
    term: PhantomXCommandCurriculum,
    command: PhantomXVelocityCommandSource,
    mask: torch.Tensor,
    state: Mapping[str, torch.Tensor],
) -> Mapping[str, torch.Tensor]:
    term.reset(mask, state)
    command.reset(mask, state)
    return command.episode_latches()


def _commands() -> PhantomXCommandConfig:
    return PhantomXCommandConfig(
        initial_speed_min=0.4,
        initial_speed_max=0.4,
        post_turn_speed_min=0.8,
        post_turn_speed_max=0.8,
        heading_delta_min=1.0,
        heading_delta_max=1.0,
    )


def _completed_step(*, success: tuple[bool, bool]) -> CurriculumStep:
    return CurriculumStep(
        transition_state=_state(),
        metrics={"phantomx/linear_velocity_error": torch.tensor([0.1, 0.1])},
        state_valid=torch.ones(2, dtype=torch.bool),
        terminated=torch.tensor([not success[0], not success[1]]),
        truncated=torch.tensor([success[0], success[1]]),
        episode_lengths=torch.full((2,), 500),
        transition_dt=0.02,
        command_velocity=torch.zeros(2, 2),
    )


def test_command_curriculum_promotes_only_new_episodes_after_success_threshold() -> None:
    config = PhantomXCurriculumConfig(
        window_episodes=4,
        minimum_episodes=2,
        promotion_success_rate=0.5,
        velocity_error_threshold=0.5,
    )
    commands = _commands()
    term = PhantomXCommandCurriculum(
        config, commands, num_envs=2, device="cpu", run_seed=7
    )
    command = _walk(term, commands, num_envs=2, run_seed=7)
    reset_mask = torch.ones(2, dtype=torch.bool)

    straight = _sample(term, command, reset_mask, _state())
    metrics = term.update(_completed_step(success=(True, False)))
    turning = _sample(term, command, reset_mask, _state())

    assert torch.equal(straight[PHANTOMX_COMMAND_FIELDS[0]], torch.tensor([[0.4, 0.0]] * 2))
    assert torch.equal(straight[PHANTOMX_COMMAND_FIELDS[1]], torch.zeros(2, 1))
    assert torch.equal(straight[PHANTOMX_COMMAND_FIELDS[2]], torch.tensor([[0.4, 0.0]] * 2))
    assert metrics["stage"] == 1.0
    assert metrics["turn_slot_fraction"] == 1.0
    assert torch.allclose(turning[PHANTOMX_COMMAND_FIELDS[1]], torch.ones(2, 1))
    assert torch.equal(turning[PHANTOMX_COMMAND_FIELDS[2]], torch.tensor([[0.8, 0.0]] * 2))


def test_command_curriculum_checkpoint_is_portable_to_another_slot_count() -> None:
    config = PhantomXCurriculumConfig(window_episodes=2, minimum_episodes=2, promotion_success_rate=1.0)
    commands = _commands()
    source = PhantomXCommandCurriculum(
        config, commands, num_envs=2, device="cpu", run_seed=7
    )
    source.reset(torch.ones(2, dtype=torch.bool), _state())
    source.update(_completed_step(success=(True, True)))
    restored = PhantomXCommandCurriculum(
        config, commands, num_envs=1, device="cpu", run_seed=7
    )

    restored.load_state_dict(source.state_dict())
    command = _walk(restored, commands, num_envs=1, run_seed=7)
    transformed = _sample(restored, command, torch.ones(1, dtype=torch.bool), _state(1))

    assert torch.allclose(transformed[PHANTOMX_COMMAND_FIELDS[1]], torch.ones(1, 1))
    assert torch.equal(transformed[PHANTOMX_COMMAND_FIELDS[2]], torch.tensor([[0.8, 0.0]]))


def test_curriculum_checkpoint_does_not_restore_episode_commands() -> None:
    config = PhantomXCurriculumConfig()
    commands = PhantomXCommandConfig()
    term = PhantomXCommandCurriculum(
        config, commands, num_envs=1, device="cpu", run_seed=11
    )
    command = _walk(term, commands, num_envs=1, run_seed=11)
    _sample(term, command, torch.ones(1, dtype=torch.bool), _state(1))
    live = {name: value.clone() for name, value in command.episode_latches().items()}

    saved = term.state_dict()
    term.load_state_dict(saved)

    assert set(saved) == {"stage", "straight_history", "turn_history"}
    for name, value in live.items():
        assert torch.equal(command.episode_latches()[name], value)


def test_command_sampling_changes_only_at_episode_reset() -> None:
    config = PhantomXCurriculumConfig()
    commands = PhantomXCommandConfig()
    term = PhantomXCommandCurriculum(
        config, commands, num_envs=2, device="cpu", run_seed=19
    )
    command = _walk(term, commands, num_envs=2, run_seed=19)
    reset_mask = torch.ones(2, dtype=torch.bool)

    first = {
        name: value.clone() for name, value in _sample(term, command, reset_mask, _state()).items()
    }
    published = command.current()["velocity"].clone()
    stale = _state()
    stale[PHANTOMX_COMMAND_FIELDS[0]] = torch.full((2, 2), 9.0)
    stale[PHANTOMX_COMMAND_FIELDS[1]] = torch.ones(2, 1)
    stale[PHANTOMX_COMMAND_FIELDS[2]] = torch.full((2, 2), 9.0)
    command.update(stale)
    unchanged = {
        name: value.clone() for name, value in command.episode_latches().items()
    }
    assert torch.allclose(command.current()["velocity"], published)
    term.reset(torch.tensor([True, False]), _state())
    command.reset(torch.tensor([True, False]), _state())
    resampled = command.episode_latches()

    assert torch.equal(first[PHANTOMX_COMMAND_FIELDS[0]], unchanged[PHANTOMX_COMMAND_FIELDS[0]])
    assert not torch.equal(
        first[PHANTOMX_COMMAND_FIELDS[0]][0], resampled[PHANTOMX_COMMAND_FIELDS[0]][0]
    )
    assert torch.equal(
        first[PHANTOMX_COMMAND_FIELDS[0]][1], resampled[PHANTOMX_COMMAND_FIELDS[0]][1]
    )


def test_partial_first_reset_samples_only_selected_slot() -> None:
    """A command item may initialize one Slot while another keeps supplied latches."""

    commands = PhantomXCommandConfig(
        initial_speed_min=0.4,
        initial_speed_max=0.4,
        post_turn_speed_min=0.8,
        post_turn_speed_max=0.8,
        heading_delta_min=1.0,
        heading_delta_max=1.0,
    )
    term = PhantomXCommandCurriculum(
        PhantomXCurriculumConfig(), commands, num_envs=2, device="cpu", run_seed=7
    )
    command = _walk(term, commands, num_envs=2, run_seed=7)
    state = _state()
    state[PHANTOMX_COMMAND_FIELDS[0]] = torch.tensor([[0.4, 0.0], [0.9, 0.0]])
    state[PHANTOMX_COMMAND_FIELDS[1]] = torch.tensor([[0.0], [0.25]])
    state[PHANTOMX_COMMAND_FIELDS[2]] = torch.tensor([[0.4, 0.0], [0.9, 0.0]])
    mask = torch.tensor([True, False])

    term.reset(mask, state)
    command.reset(mask, state)

    latches = command.episode_latches()
    assert torch.equal(latches[PHANTOMX_COMMAND_FIELDS[0]][0], torch.tensor([0.4, 0.0]))
    assert torch.equal(latches[PHANTOMX_COMMAND_FIELDS[2]][0], torch.tensor([0.4, 0.0]))
    assert torch.allclose(command.current()["velocity"][0], torch.tensor([0.4, 0.0, 0.0]))

    state[PHANTOMX_COMMAND_FIELDS[0]][1] = torch.tensor([0.7, 0.0])
    command.update(state)
    assert torch.allclose(command.current()["velocity"][0], torch.tensor([0.4, 0.0, 0.0]))
    assert torch.allclose(command.current()["velocity"][1, :2], torch.tensor([0.7, 0.0]))


def test_command_curriculum_does_not_promote_when_timeout_overlaps_failure() -> None:
    config = PhantomXCurriculumConfig(
        window_episodes=2,
        minimum_episodes=1,
        promotion_success_rate=1.0,
        velocity_error_threshold=0.5,
    )
    term = PhantomXCommandCurriculum(
        config, _commands(), num_envs=2, device="cpu", run_seed=7
    )
    term.reset(torch.ones(2, dtype=torch.bool), _state())

    metrics = term.update(
        CurriculumStep(
            transition_state=_state(),
            metrics={"phantomx/linear_velocity_error": torch.tensor([0.1, 0.1])},
            state_valid=torch.ones(2, dtype=torch.bool),
            terminated=torch.tensor([True, False]),
            truncated=torch.tensor([True, False]),
            episode_lengths=torch.full((2,), 500),
            transition_dt=0.02,
            command_velocity=torch.zeros(2, 2),
        )
    )

    assert metrics["stage"] == 0.0
    assert metrics["success_rate"] == 0.0


def test_command_channel_matches_recorded_injection_sequence() -> None:
    """AC_PY_UNIT_CURRMGR_001: channels match independently recorded inject values.

    Expected tensors are produced by the same seeded ``random.Random`` formula the
    curriculum used before CommandSource existed — not by reading ``current()``
    or ``transform_state`` — so a shared publish helper cannot fake parity.
    """

    import math
    import random

    commands = PhantomXCommandConfig(
        initial_speed_min=0.35,
        initial_speed_max=0.55,
        post_turn_speed_min=0.6,
        post_turn_speed_max=0.9,
        heading_delta_min=0.4,
        heading_delta_max=1.2,
    )
    run_seed = 42
    num_envs = 2

    def expected_commands(*, episode_counts: tuple[int, int], turn: bool) -> dict[str, torch.Tensor]:
        # Identity pose → heading 0 (matches ``_state()``).
        current_heading = 0.0
        initials: list[list[float]] = []
        headings: list[list[float]] = []
        posts: list[list[float]] = []
        for slot_id, episode_count in enumerate(episode_counts):
            generator = random.Random(run_seed + 1_000_003 * slot_id + 97_409 * episode_count)
            initial_speed = generator.uniform(0.35, 0.55)
            post_turn_speed = generator.uniform(0.6, 0.9)
            heading_delta = generator.uniform(0.4, 1.2)
            target = math.atan2(
                math.sin(current_heading + heading_delta),
                math.cos(current_heading + heading_delta),
            )
            initials.append([initial_speed, 0.0])
            headings.append([target if turn else current_heading])
            posts.append([post_turn_speed if turn else initial_speed, 0.0])
        return {
            PHANTOMX_COMMAND_FIELDS[0]: torch.tensor(initials, dtype=torch.float32),
            PHANTOMX_COMMAND_FIELDS[1]: torch.tensor(headings, dtype=torch.float32),
            PHANTOMX_COMMAND_FIELDS[2]: torch.tensor(posts, dtype=torch.float32),
        }

    def assert_matches(
        actual: Mapping[str, torch.Tensor], expected: Mapping[str, torch.Tensor]
    ) -> None:
        for name in PHANTOMX_COMMAND_FIELDS:
            assert torch.allclose(actual[name], expected[name], atol=1e-6, rtol=0.0), name

    term = PhantomXCommandCurriculum(
        PhantomXCurriculumConfig(), commands, num_envs=num_envs, device="cpu", run_seed=run_seed
    )
    command = _walk(term, commands, num_envs=num_envs, run_seed=run_seed)
    reset_mask = torch.ones(num_envs, dtype=torch.bool)
    expected = expected_commands(episode_counts=(0, 0), turn=False)
    assert_matches(_sample(term, command, reset_mask, _state()), expected)
    assert command.channels() == {"velocity": 3}

    promote = PhantomXCommandCurriculum(
        PhantomXCurriculumConfig(
            window_episodes=2,
            minimum_episodes=2,
            promotion_success_rate=0.5,
            velocity_error_threshold=0.5,
        ),
        commands,
        num_envs=num_envs,
        device="cpu",
        run_seed=run_seed,
    )
    promote_command = _walk(promote, commands, num_envs=num_envs, run_seed=run_seed)
    promote.reset(reset_mask, _state())
    promote_command.reset(reset_mask, _state())
    for _ in range(2):
        promote.update(_completed_step(success=(True, True)))
    expected_turn = expected_commands(episode_counts=(1, 1), turn=True)
    assert_matches(_sample(promote, promote_command, reset_mask, _state()), expected_turn)
