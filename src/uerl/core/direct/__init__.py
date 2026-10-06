"""Expose framework-independent Direct runtime primitives."""

from .capabilities import CapabilityOperation, CapabilityStatus, TaskCapabilities, TaskCapability
from .curriculum import CurriculumManager, CurriculumStep, CurriculumTerm, TerrainCurriculum
from .env import UERLDirectEnv
from .robot_action import ROBOT_ACTUATOR_TARGET_FIELD, resolve_robot_actions, robot_actuator_action_schema
from .robot_observation import (
    ROBOT_CONTACT_OBSERVATION_FRAME,
    ROBOT_JOINT_OBSERVATION_FRAME,
    ROBOT_OBSERVATION_FRAME,
    ROBOT_OBSERVATION_SOURCE,
    ObservationShapeTable,
    robot_observation_schema,
    validate_robot_shape_agreement,
)
from .session import DirectSession
from .task import (
    CommandSource,
    CurriculumBindableCommandSource,
    DirectTask,
    DirectTaskCfg,
    EmptyCommandSource,
)
from .termination import TerminationEvaluator, TerminationTerm, combine_termination_terms
from .types import (
    InitialState,
    PhysicalCommandBatch,
    PostResetState,
    SessionSchema,
    StateBatch,
    StepContext,
    TerminationResult,
    TransitionState,
)

__all__ = [
    "CommandSource",
    "CurriculumBindableCommandSource",
    "CapabilityOperation",
    "CapabilityStatus",
    "DirectSession",
    "EmptyCommandSource",
    "ROBOT_ACTUATOR_TARGET_FIELD",
    "ObservationShapeTable",
    "ROBOT_CONTACT_OBSERVATION_FRAME",
    "ROBOT_JOINT_OBSERVATION_FRAME",
    "resolve_robot_actions",
    "robot_actuator_action_schema",
    "ROBOT_OBSERVATION_FRAME",
    "ROBOT_OBSERVATION_SOURCE",
    "robot_observation_schema",
    "validate_robot_shape_agreement",
    "DirectTask",
    "DirectTaskCfg",
    "TaskCapabilities",
    "TaskCapability",
    "TerminationEvaluator",
    "TerminationTerm",
    "combine_termination_terms",
    "CurriculumManager",
    "CurriculumStep",
    "CurriculumTerm",
    "TerrainCurriculum",
    "UERLDirectEnv",
    "InitialState",
    "PhysicalCommandBatch",
    "PostResetState",
    "SessionSchema",
    "StateBatch",
    "StepContext",
    "TerminationResult",
    "TransitionState",
]
