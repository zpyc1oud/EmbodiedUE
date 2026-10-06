"""Generate reviewable skeletons for new Robot declarations and Tasks."""

from __future__ import annotations

import argparse
import json
import keyword
import re
from dataclasses import dataclass
from pathlib import Path
from textwrap import dedent
from typing import cast


@dataclass(frozen=True, slots=True)
class _TaskTemplate:
    """Describe the small set of supported task scaffold starting points."""

    name: str
    summary: str
    default_environment_id: str
    default_map_path: str
    required_semantics: tuple[str, ...]


_TASK_TEMPLATES = {
    "cartpole": _TaskTemplate(
        name="cartpole",
        summary="Direct CartPole-style balance task",
        default_environment_id="uerl.environment.shared_world",
        default_map_path="/Engine/Maps/Entry",
        required_semantics=("joint_position", "joint_velocity"),
    ),
    "phantomx-walk": _TaskTemplate(
        name="phantomx-walk",
        summary="Direct PhantomX-style locomotion task",
        default_environment_id="uerl.environment.shared_world",
        default_map_path="/Engine/Maps/Entry",
        required_semantics=("root_pose", "root_velocity", "joint_position", "joint_velocity"),
    ),
}


class _ScaffoldError(ValueError):
    """Report invalid scaffold input before any output is written."""


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="uerl new", description="Generate UE-RL Robot and Task projects.")
    subparsers = parser.add_subparsers(dest="kind", required=True)

    robot_parser = subparsers.add_parser("robot", help="generate a Robot declaration skeleton")
    robot_parser.add_argument("name", help="Python Robot declaration name, such as phantomx2.")
    robot_parser.add_argument("--asset", required=True, help="UE Skeletal Mesh object path.")
    robot_parser.add_argument(
        "--output-dir",
        type=Path,
        default=Path("."),
        help="Project root where src/ and docs/ are created (default: current directory).",
    )
    robot_parser.add_argument("--json", action="store_true", help="Print a machine-readable result.")

    task_parser = subparsers.add_parser("task", help="generate a DirectTask skeleton")
    task_parser.add_argument("name", help="Python Task package name, such as walk2.")
    task_parser.add_argument("--robot", required=True, help="Robot declaration name or slug.")
    task_parser.add_argument(
        "--template",
        required=True,
        choices=tuple(sorted(_TASK_TEMPLATES)),
        help="Supported starting point for the Task math and config draft.",
    )
    task_parser.add_argument("--task-id", help="Stable Task ID (default: UERL-<Name>-v0).")
    task_parser.add_argument(
        "--environment-id",
        help="UE Environment ID (default comes from the selected template).",
    )
    task_parser.add_argument(
        "--robot-id",
        default="uerl.robot.skeletal_mesh",
        help="UE Robot ID used by the Task registration draft.",
    )
    task_parser.add_argument(
        "--output-dir",
        type=Path,
        default=Path("."),
        help="Project root where src/, configs/, and docs/ are created (default: current directory).",
    )
    task_parser.add_argument("--json", action="store_true", help="Print a machine-readable result.")

    cartpole_parser = subparsers.add_parser(
        "external-cartpole",
        help="generate an independently installable CartPole Task package",
    )
    cartpole_parser.add_argument("name", help="Package name, such as balance-demo.")
    cartpole_parser.add_argument("--task-id", help="Stable Task ID (default: UERL-<Name>-v0).")
    cartpole_parser.add_argument(
        "--output-dir",
        type=Path,
        help="Destination project directory (default: <name> under the current directory).",
    )
    cartpole_parser.add_argument("--json", action="store_true", help="Print a machine-readable result.")

    direct_cartpole_parser = subparsers.add_parser(
        "direct-cartpole",
        help="generate an independently installable CartPole Task using Python hooks",
    )
    direct_cartpole_parser.add_argument("name", help="Package name, such as direct-balance-demo.")
    direct_cartpole_parser.add_argument("--task-id", help="Stable Task ID (default: UERL-<Name>-v0).")
    direct_cartpole_parser.add_argument(
        "--output-dir",
        type=Path,
        help="Destination project directory (default: <name> under the current directory).",
    )
    direct_cartpole_parser.add_argument("--json", action="store_true", help="Print a machine-readable result.")
    return parser


def _slug(value: str, *, label: str) -> str:
    normalized = re.sub(r"[^a-zA-Z0-9]+", "_", value).strip("_").lower()
    if not normalized or not re.fullmatch(r"[a-z][a-z0-9_]*", normalized):
        raise _ScaffoldError(f"{label} must start with a letter and contain only letters, digits, or '_'")
    return normalized


def _class_name(slug: str) -> str:
    return "".join(part.capitalize() for part in slug.split("_"))


def _task_id(slug: str) -> str:
    return f"UERL-{_class_name(slug)}-v0"


def _write_files(files: dict[Path, str]) -> tuple[Path, ...]:
    """Write a complete scaffold only after every destination is confirmed free."""

    conflicts = sorted(path for path in files if path.exists())
    if conflicts:
        rendered = ", ".join(str(path) for path in conflicts)
        raise _ScaffoldError(f"refusing to overwrite existing scaffold file(s): {rendered}")
    for path, content in files.items():
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8", newline="\n")
    return tuple(files)


def _robot_files(root: Path, slug: str, asset_path: str) -> dict[Path, str]:
    class_name = _class_name(slug)
    asset_ref = f"robots/{slug}/robot.yaml"
    declaration = dedent(
        f'''\
        """Robot declaration scaffold for {class_name}.

        TODO: replace every placeholder selector with names from the UE topology
        before adding this declaration to ``ROBOT_ASSETS``.
        """

        from __future__ import annotations

        from ...core.config.robot import ObsType, ResetTargetType
        from .base import ActuatorGroupCfg, ObsSelectorCfg, ResetTargetCfg, RobotAssetCfg, RobotInitStateCfg

        {class_name.upper()}_ASSET_REF = "{asset_ref}"
        {class_name.upper()}_ASSET_PATH = "{asset_path}"

        {class_name.upper()}_CFG = RobotAssetCfg(
            name="{slug}",
            asset_path={class_name.upper()}_ASSET_PATH,
            # TODO: replace placeholders with reflected joint names.
            joint_names=("TODO_joint",),
            # TODO: replace placeholders with reflected body names.
            body_names=("TODO_body",),
            init_state=RobotInitStateCfg(joint_pos={{"TODO_joint": 0.0}}),
            actuators=(
                # TODO: set gains and effort limits from the intended controller.
                ActuatorGroupCfg(
                    joint_names="TODO_joint",
                    target_mode="effort",
                    stiffness=0.0,
                    damping=1.0,
                    effort_limit=1.0,
                    action_scale=1.0,
                ),
            ),
            observations=(
                # TODO: declare every raw state field consumed by the Task.
                ObsSelectorCfg(ObsType.JOINT_POSITION, joint_names="TODO_joint"),
            ),
            reset=(
                # TODO: add reset distributions for all controlled joints and root state.
                ResetTargetCfg(
                    target_type=ResetTargetType.JOINT_POSITION,
                    joint_names="TODO_joint",
                    stream_id="reset.TODO_joint.position",
                ),
            ),
        )

        __all__ = ["{class_name.upper()}_ASSET_PATH", "{class_name.upper()}_ASSET_REF", "{class_name.upper()}_CFG"]
        '''
    )
    readme = dedent(
        f'''\
        # Robot scaffold: `{slug}`

        UE asset path: `{asset_path}`
        Python declaration ref: `{asset_ref}`

        This file is intentionally not registered yet. Before adding it to
        `src/uerl/assets/robots/__init__.py`:

        1. Replace `TODO_*` joint/body selectors with names from UE topology.
        2. Set actuator gains, effort limits, action scales, observations, and reset streams.
        3. Add `{class_name.upper()}_ASSET_REF` / `{class_name.upper()}_CFG` to `ROBOT_ASSETS`.
        4. Add a Task that consumes the declaration and run `uerl check task <TaskID>`.

        The declaration is a reviewable starting point; it does not claim that
        the placeholder topology matches the Skeletal Mesh.
        '''
    )
    return {
        root / "src" / "uerl" / "assets" / "robots" / f"{slug}.py": declaration,
        root / "src" / "uerl" / "assets" / "robots" / f"{slug}.md": readme,
    }


def _task_files(
    root: Path,
    slug: str,
    robot_slug: str,
    template: _TaskTemplate,
    task_id: str,
    environment_id: str,
    robot_id: str,
) -> dict[Path, str]:
    class_name = _class_name(slug)
    constant_prefix = slug.upper()
    robot_ref = f"robots/{robot_slug}/robot.yaml"
    package = dedent(
        f'''\
        """Scaffold package for the `{task_id}` DirectTask."""

        from .config import {constant_prefix}_TASK_ID, {constant_prefix}_TASK_VERSION, {class_name}TaskConfig

        __all__ = ["{constant_prefix}_TASK_ID", "{constant_prefix}_TASK_VERSION", "{class_name}TaskConfig"]
        '''
    )
    config = dedent(
        f'''\
        """Typed Task config scaffold generated from the `{template.name}` template."""

        from __future__ import annotations

        from dataclasses import dataclass

        from ...core.config.models import DirectTaskConfig

        {constant_prefix}_TASK_ID = "{task_id}"
        {constant_prefix}_TASK_VERSION = "1.0.0"
        {constant_prefix}_ENVIRONMENT_ID = "{environment_id}"
        {constant_prefix}_ROBOT_ID = "{robot_id}"
        {constant_prefix}_ROBOT_CONFIG_PATH = "{robot_ref}"


        @dataclass(frozen=True, slots=True)
        class {class_name}TaskConfig(DirectTaskConfig):
            """TODO: replace the placeholder State/Action contract and parameters."""

            state_requirements: tuple[str, ...] = {template.required_semantics!r}
            action_schema: tuple[str, ...] = ("TODO_action",)
            max_episode_steps: int = 1000
            slot_fault_reward: float = -1.0


        __all__ = [
            "{constant_prefix}_ENVIRONMENT_ID",
            "{constant_prefix}_ROBOT_CONFIG_PATH",
            "{constant_prefix}_ROBOT_ID",
            "{constant_prefix}_TASK_ID",
            "{constant_prefix}_TASK_VERSION",
            "{class_name}TaskConfig",
        ]
        '''
    )
    registration = dedent(
        f'''\
        """Explicit registration scaffold for the `{task_id}` DirectTask."""

        from __future__ import annotations

        from pathlib import Path

        from ...core.config import LoggingConfig, RslRlRunnerConfig, SessionConfig, WorkerConfig
        from ...core.config.models import DirectTaskConfig
        from ...core.direct.task import DirectTask
        from ..registry.models import TaskRegistration
        from ..registry.tasks import TaskRegistry
        from .config import (
            {constant_prefix}_ENVIRONMENT_ID,
            {constant_prefix}_ROBOT_ID,
            {constant_prefix}_TASK_ID,
            {constant_prefix}_TASK_VERSION,
            {class_name}TaskConfig,
        )


        def create_{slug}_task_config() -> {class_name}TaskConfig:
            return {class_name}TaskConfig()


        def create_{slug}_worker_config() -> WorkerConfig:
            # TODO: copy the environment-owned defaults from a real Task template.
            return WorkerConfig(
                slot_count=1,
                physics_dt=1.0 / 60.0,
                decimation=(1, 1),
                environment_id={constant_prefix}_ENVIRONMENT_ID,
                robot_id={constant_prefix}_ROBOT_ID,
                robot_config_path=Path("{robot_ref}"),
                robot_asset_path="TODO_REPLACE_WITH_UE_ROBOT_ASSET_PATH",
            )


        def create_{slug}_runner_config() -> RslRlRunnerConfig:
            return RslRlRunnerConfig(rollout_length=16, max_iterations=1, device="cpu")


        def create_{slug}_task(config: DirectTaskConfig, *, robot_spec=None) -> DirectTask:
            raise NotImplementedError(
                "Implement Task math and composed config before registering this scaffold"
            )


        def create_{slug}_registration() -> TaskRegistration:
            return TaskRegistration(
                task_id={constant_prefix}_TASK_ID,
                task_version={constant_prefix}_TASK_VERSION,
                environment_id={constant_prefix}_ENVIRONMENT_ID,
                robot_id={constant_prefix}_ROBOT_ID,
                task_factory=create_{slug}_task,
                worker_config_factory=create_{slug}_worker_config,
                task_config_factory=create_{slug}_task_config,
                runner_config_factory=create_{slug}_runner_config,
                session_config=SessionConfig(map_path="{template.default_map_path}"),
                logging_config=LoggingConfig(),
            )


        def register_{slug}(registry: TaskRegistry) -> None:
            registry.register(create_{slug}_registration())


        __all__ = ["create_{slug}_registration", "register_{slug}"]
        '''
    )
    yaml = dedent(
        f'''\
        # Draft training contract for `{task_id}`.
        # TODO: make the typed loader for this Task consume this document.
        identity:
          task_id: {task_id}
          task_version: 1.0.0
          environment_id: {environment_id}
          robot_id: {robot_id}

        robot:
          asset_path: TODO_REPLACE_WITH_UE_ROBOT_ASSET_PATH
          config_path: {robot_ref}

        task:
          max_episode_steps: 1000
          slot_fault_reward: -1.0
          state_requirements: {list(template.required_semantics)!r}
          action_schema: ['TODO_action']

        worker:
          slot_count: 1
          physics_dt: 0.0166666666666667
          decimation: [1, 1]
          environment:
            id: {environment_id}
          run_seed: 0

        runner:
          rollout_length: 16
          max_iterations: 1
          device: cpu
        '''
    )
    readme = dedent(
        f'''\
        # Task scaffold: `{slug}`

        - Task ID: `{task_id}`
        - Robot declaration: `{robot_slug}` (`{robot_ref}`)
        - Template: `{template.name}` — {template.summary}
        - Environment: `{environment_id}`
        - Robot ID: `{robot_id}`

        Generated files are deliberately unregistered until the contract is
        implemented. Complete these steps in order:

        1. Replace `TODO_*` State/Action fields and parameters in `config.py`.
        2. Implement the DirectTask builder and composed terms in `registration.py`.
        3. Replace the draft YAML with a typed loader/configspec for this Task.
        4. Register the Robot declaration in `ROBOT_ASSETS` and this Task in
           `src/uerl/tasks/registry/defaults.py`.
        5. Add focused unit tests, then run `uerl check task {task_id}`.

        The generator does not edit either registry or overwrite existing files.
        '''
    )
    return {
        root / "src" / "uerl" / "tasks" / slug / "__init__.py": package,
        root / "src" / "uerl" / "tasks" / slug / "config.py": config,
        root / "src" / "uerl" / "tasks" / slug / "registration.py": registration,
        root / "configs" / "tasks" / slug / "training.yaml": yaml,
        root / "src" / "uerl" / "tasks" / slug / "README.md": readme,
    }


def _external_cartpole_files(root: Path, slug: str, task_id: str) -> dict[Path, str]:
    """Build a standalone CartPole reward-variant package using the public entry point."""

    distribution_name = f"uerl-{slug.replace('_', '-')}"
    entry_point = slug.replace("_", "-")
    class_name = _class_name(slug)
    metadata = dedent(
        f'''\
        [build-system]
        requires = ["setuptools>=77"]
        build-backend = "setuptools.build_meta"

        [project]
        name = "{distribution_name}"
        version = "0.1.0"
        requires-python = ">=3.11,<3.12"
        dependencies = ["ue-rl-engine==1.0.0"]

        [project.entry-points."uerl.tasks"]
        {entry_point} = "{slug}:create_registration"

        [tool.setuptools.packages.find]
        where = ["src"]

        [tool.setuptools.package-data]
        {slug} = ["reward.yaml"]
        '''
    )
    package = dedent(
        f'''\
        """Register a CartPole reward variant as an external Task."""

        from __future__ import annotations

        from dataclasses import replace
        from importlib.resources import files

        from uerl.core.config.yaml_loader import load_unique_yaml
        from uerl.tasks.cartpole import CartPoleTaskConfig
        from uerl.tasks.cartpole.registration import create_cartpole_registration, create_cartpole_task_config
        from uerl.tasks.registry import TaskRegistration

        TASK_ID = "{task_id}"


        def create_task_config() -> CartPoleTaskConfig:
            """Change the configured pole-position reward and preserve CartPole semantics."""

            reward = load_unique_yaml(files(__package__).joinpath("reward.yaml").read_text(encoding="utf-8"))
            weight = reward.get("pole_position_weight") if isinstance(reward, dict) else None
            if not isinstance(weight, (int, float)) or isinstance(weight, bool):
                raise ValueError("reward.yaml must define a numeric pole_position_weight")
            return replace(
                create_cartpole_task_config(),
                rew_scale_pole_pos=float(weight),
            )


        def create_registration() -> TaskRegistration:
            """Return fresh metadata that reuses the framework CartPole runtime."""

            return replace(
                create_cartpole_registration(),
                task_id=TASK_ID,
                task_version="0.1.0",
                task_config_factory=create_task_config,
            )


        __all__ = ["TASK_ID", "create_registration", "create_task_config"]
        '''
    )
    readme = dedent(
        f'''\
        # {class_name}: external CartPole Task

        This installable package registers `{task_id}` through the `uerl.tasks`
        entry-point group. It reuses EmbodiedUE's CartPole Task, Robot declaration,
        Worker and runner configurations, and runtime. Its only Task change is the
        pole-position reward weight in `src/{slug}/reward.yaml`.

        ## Requirements

        Use Python 3.11 in an environment with EmbodiedUE `ue-rl-engine==1.0.0`
        and its runtime dependencies installed. Training also requires a configured
        Windows/UE 5.8 host; package installation and the inspection commands below
        do not start UE.

        ## Install and inspect

        From this project directory in PowerShell:

        ```powershell
        uv pip install -e . --no-deps
        $env:UERL_TASK_PLUGINS = '{entry_point}'
        uerl tasks --filter {task_id}
        uerl check task {task_id}
        uerl config --task {task_id} --json
        ```

        In Bash, use `export UERL_TASK_PLUGINS={entry_point}`. After installation,
        these `uerl` commands work from any current directory. `check task` validates
        the Python registration, resolved configuration, and Task construction.

        Train it on a configured Windows/UE host with:

        ```powershell
        uerl train --task {task_id} --num-envs 2 --max-iterations 1 --device cpu
        ```

        ## Change the reward

        Edit `pole_position_weight` in `src/{slug}/reward.yaml` (initially `-2.0`).
        The package reads this resource for each fresh Task configuration. A
        non-editable installation must be reinstalled after the file changes.
        The generated tests in `tests/test_registration.py` check the registration,
        Task construction, and reward-only customization. Run them with
        `python -m pytest tests` in the prepared framework environment.
        '''
    )
    tests = dedent(
        f'''\
        from uerl.core.direct.task import DirectTask
        from uerl.tasks.cartpole.registration import create_cartpole_task_config

        from {slug} import TASK_ID, create_registration


        def test_external_registration_changes_only_pole_position_reward() -> None:
            registration = create_registration()
            config = registration.task_config_factory()
            defaults = create_cartpole_task_config()

            assert registration.task_id == "{task_id}"
            assert registration.task_version == "0.1.0"
            assert config.rew_scale_pole_pos == -2.0
            assert config.rew_scale_alive == defaults.rew_scale_alive
            assert config.rew_scale_terminated == defaults.rew_scale_terminated
            assert config.rew_scale_cart_vel == defaults.rew_scale_cart_vel
            assert config.rew_scale_pole_vel == defaults.rew_scale_pole_vel
            assert isinstance(registration.task_factory(config), DirectTask)
            assert TASK_ID == registration.task_id
        '''
    )
    return {
        root / "pyproject.toml": metadata,
        root / "README.md": readme,
        root / "src" / slug / "__init__.py": package,
        root / "src" / slug / "reward.yaml": "pole_position_weight: -2.0\n",
        root / "tests" / "test_registration.py": tests,
    }


def _emit(result: dict[str, object], *, as_json: bool) -> None:
    if as_json:
        print(json.dumps(result, ensure_ascii=False, indent=2, sort_keys=True))
        return
    print(f"[PASS] generated {result['kind']} scaffold: {result['name']}")
    for path in cast(tuple[Path, ...], result["files"]):
        print(f"[WRITE] {path}")
    print(f"[NEXT] {result['next_step']}")


def _generate_robot(args: argparse.Namespace) -> dict[str, object]:
    slug = _slug(args.name, label="robot name")
    if not args.asset.startswith("/") or "\\" in args.asset:
        raise _ScaffoldError("--asset must be a UE object path beginning with '/' and must not contain '\\'")
    files = _write_files(_robot_files(args.output_dir, slug, args.asset))
    return {
        "kind": "robot",
        "name": slug,
        "files": [str(path) for path in files],
        "next_step": "replace TODO selectors, then add the declaration to ROBOT_ASSETS",
    }


def _generate_task(args: argparse.Namespace) -> dict[str, object]:
    slug = _slug(args.name, label="task name")
    robot_slug = _slug(args.robot, label="robot name")
    template = _TASK_TEMPLATES[args.template]
    task_id = args.task_id or _task_id(slug)
    if not re.fullmatch(r"UERL-[A-Za-z0-9][A-Za-z0-9._-]*-v\d+", task_id):
        raise _ScaffoldError("--task-id must look like UERL-Name-v0")
    environment_id = args.environment_id or template.default_environment_id
    if not environment_id or " " in environment_id:
        raise _ScaffoldError("--environment-id must be a non-empty identifier")
    if not args.robot_id or " " in args.robot_id:
        raise _ScaffoldError("--robot-id must be a non-empty identifier")
    files = _write_files(
        _task_files(
            args.output_dir,
            slug,
            robot_slug,
            template,
            task_id,
            environment_id,
            args.robot_id,
        )
    )
    return {
        "kind": "task",
        "name": slug,
        "task_id": task_id,
        "template": template.name,
        "files": [str(path) for path in files],
        "next_step": f"implement and register the Task, then run `uerl check task {task_id}`",
    }


def _generate_external_cartpole(args: argparse.Namespace) -> dict[str, object]:
    slug = _slug(args.name, label="package name")
    if keyword.iskeyword(slug):
        raise _ScaffoldError("package name must not be a Python keyword")
    task_id = args.task_id or _task_id(slug)
    if not re.fullmatch(r"UERL-[A-Za-z0-9][A-Za-z0-9._-]*-v\d+", task_id):
        raise _ScaffoldError("--task-id must look like UERL-Name-v0")
    root = args.output_dir if args.output_dir is not None else Path(slug.replace("_", "-"))
    files = _write_files(_external_cartpole_files(root, slug, task_id))
    return {
        "kind": "external-cartpole",
        "name": slug,
        "package_name": slug,
        "distribution_name": f"uerl-{slug.replace('_', '-')}",
        "entry_point": slug.replace("_", "-"),
        "task_id": task_id,
        "files": [str(path) for path in files],
        "next_step": f"install the package, enable its entry point, then run `uerl check task {task_id}`",
    }


def _generate_direct_cartpole(args: argparse.Namespace) -> dict[str, object]:
    slug = _slug(args.name, label="package name")
    if keyword.iskeyword(slug):
        raise _ScaffoldError("package name must not be a Python keyword")
    task_id = args.task_id or _task_id(slug)
    if not re.fullmatch(r"UERL-[A-Za-z0-9][A-Za-z0-9._-]*-v\d+", task_id):
        raise _ScaffoldError("--task-id must look like UERL-Name-v0")
    root = args.output_dir if args.output_dir is not None else Path(slug.replace("_", "-"))
    files = _write_files(_direct_cartpole_files(root, slug, task_id))
    return {
        "kind": "direct-cartpole",
        "name": slug,
        "package_name": slug,
        "distribution_name": f"uerl-{slug.replace('_', '-')}-direct",
        "entry_point": slug.replace("_", "-"),
        "task_id": task_id,
        "files": [str(path) for path in files],
        "next_step": f"install the package, enable its entry point, then run `uerl check task {task_id}`",
    }


def _direct_cartpole_files(root: Path, slug: str, task_id: str) -> dict[Path, str]:
    """Build a small external Task that owns CartPole math through DirectTask hooks."""

    distribution_name = f"uerl-{slug.replace('_', '-')}-direct"
    entry_point = slug.replace("_", "-")
    metadata = dedent(
        f'''\
        [build-system]
        requires = ["setuptools>=77"]
        build-backend = "setuptools.build_meta"

        [project]
        name = "{distribution_name}"
        version = "0.1.0"
        requires-python = ">=3.11,<3.12"
        dependencies = ["ue-rl-engine==1.0.0"]

        [project.entry-points."uerl.tasks"]
        {entry_point} = "{slug}:create_registration"

        [tool.setuptools.packages.find]
        where = ["src"]

        [tool.setuptools.package-data]
        {slug} = ["reward.yaml"]
        '''
    )
    package = dedent(
        '''\
        """A minimal CartPole Task implemented with the public Python hooks."""

        from __future__ import annotations

        from collections.abc import Mapping
        from dataclasses import replace
        from importlib.resources import files

        import torch

        from uerl.core.config.models import DirectTaskConfig
        from uerl.core.config.robot import ObsType, RobotSpec
        from uerl.core.config.yaml_loader import load_unique_yaml
        from uerl.core.direct.robot_observation import ObservationShapeTable
        from uerl.core.direct.task import DirectTask
        from uerl.core.direct.types import PhysicalCommandBatch, StepContext, TerminationResult
        from uerl.tasks.cartpole import CARTPOLE_CONTROL_DT, CartPoleTaskConfig
        from uerl.tasks.cartpole.registration import create_cartpole_registration, create_cartpole_task_config
        from uerl.tasks.registry import TaskRegistration

        TASK_ID = "__TASK_ID__"
        _POLE_POSITION = "robot.joint.pole.joint_position"
        _POLE_VELOCITY = "robot.joint.pole.joint_velocity"
        _CART_POSITION = "robot.joint.cart.joint_position"
        _CART_VELOCITY = "robot.joint.cart.joint_velocity"
        _OBSERVATION_FIELDS = (_POLE_POSITION, _POLE_VELOCITY, _CART_POSITION, _CART_VELOCITY)
        _ACTION_FIELD = "robot.actuator.target"
        _CARTPOLE_OBSERVATION_SHAPES = ObservationShapeTable(
            {ObsType.JOINT_POSITION: (1,), ObsType.JOINT_VELOCITY: (1,)}
        )


        class DirectCartPoleTask(DirectTask):
            """Implement actions, observations, rewards, and termination directly."""

            def __init__(self, config: CartPoleTaskConfig, *, robot_spec: RobotSpec | None = None) -> None:
                self.params = config
                direct_config = replace(
                    config,
                    state_requirements=_OBSERVATION_FIELDS,
                    action_schema=(_ACTION_FIELD,),
                )
                super().__init__(
                    direct_config,
                    robot_spec=robot_spec,
                    observation_shapes=(
                        None if robot_spec is None else _CARTPOLE_OBSERVATION_SHAPES
                    ),
                    control_dt=CARTPOLE_CONTROL_DT,
                    batch_size=1,
                    device="cpu",
                )

            def preprocess_actions(
                self,
                policy_actions: torch.Tensor,
                raw_state: Mapping[str, torch.Tensor],
            ) -> PhysicalCommandBatch:
                del raw_state
                if self.robot_spec is None or len(self.robot_spec.actuators) != 1:
                    raise RuntimeError("Direct CartPole requires one bound Robot actuator")
                scale = self.robot_spec.actuators[0].action_scale
                bounded = policy_actions.clamp(-self.params.action_clip, self.params.action_clip)
                return PhysicalCommandBatch({_ACTION_FIELD: bounded * scale})

            def build_observations(
                self,
                raw_state: Mapping[str, torch.Tensor],
                state_valid: torch.Tensor,
                previous_policy_actions: torch.Tensor,
                control_frame_dt: torch.Tensor | None = None,
            ) -> Mapping[str, torch.Tensor]:
                del previous_policy_actions, control_frame_dt
                observations = torch.cat([raw_state[name].reshape(-1, 1) for name in _OBSERVATION_FIELDS], dim=1)
                valid = state_valid.to(device=observations.device, dtype=torch.bool).unsqueeze(1)
                return {"policy": torch.where(valid, observations, torch.zeros_like(observations))}

            def compute_terminations(self, context: StepContext) -> TerminationResult:
                state = context.transition_state
                cart_out = state[_CART_POSITION].reshape(-1).abs() > self.params.max_cart_position
                pole_fell = state[_POLE_POSITION].reshape(-1).abs() > self.params.pole_angle_limit
                terminated = cart_out | pole_fell
                timed_out = (
                    context.episode_steps >= self.params.max_episode_steps - 1
                    if self.params.max_episode_steps > 0
                    else torch.zeros_like(terminated)
                )
                return TerminationResult(
                    terminated,
                    timed_out,
                    {"cart_out_of_bounds": cart_out, "pole_fell": pole_fell, "time_out": timed_out},
                )

            def compute_rewards(
                self,
                context: StepContext,
                terminations: TerminationResult,
            ) -> torch.Tensor:
                state = context.transition_state
                pole_position = state[_POLE_POSITION].reshape(-1)
                cart_velocity = state[_CART_VELOCITY].abs().reshape(-1)
                pole_velocity = state[_POLE_VELOCITY].abs().reshape(-1)
                ended = terminations.terminated.to(dtype=torch.float32)
                return (
                    self.params.rew_scale_alive * (1.0 - ended)
                    + self.params.rew_scale_terminated * ended
                    + self.params.rew_scale_pole_pos * pole_position.square()
                    + self.params.rew_scale_cart_vel * cart_velocity
                    + self.params.rew_scale_pole_vel * pole_velocity
                )


        def create_task_config() -> CartPoleTaskConfig:
            """Read this package's YAML reward override on each fresh Task config."""

            reward = load_unique_yaml(files(__package__).joinpath("reward.yaml").read_text(encoding="utf-8"))
            weight = reward.get("pole_position_weight") if isinstance(reward, dict) else None
            if not isinstance(weight, (int, float)) or isinstance(weight, bool):
                raise ValueError("reward.yaml must define a numeric pole_position_weight")
            return replace(
                create_cartpole_task_config(),
                rew_scale_pole_pos=float(weight),
                state_requirements=_OBSERVATION_FIELDS,
                action_schema=(_ACTION_FIELD,),
            )


        def create_task(
            config: DirectTaskConfig,
            *,
            robot_spec: RobotSpec | None = None,
        ) -> DirectCartPoleTask:
            """Build fresh Python hooks around the resolved CartPole parameters."""

            return DirectCartPoleTask(CartPoleTaskConfig.from_direct(config), robot_spec=robot_spec)


        def create_registration() -> TaskRegistration:
            """Reuse the built-in CartPole Worker, runner, and Session defaults."""

            return replace(
                create_cartpole_registration(),
                task_id=TASK_ID,
                task_version="0.1.0",
                task_factory=create_task,
                task_config_factory=create_task_config,
            )


        __all__ = ["TASK_ID", "DirectCartPoleTask", "create_registration", "create_task", "create_task_config"]
        '''
    ).replace("__TASK_ID__", task_id)
    readme = dedent(
        f'''\
        # Direct CartPole package

        This installable package registers `{task_id}` with the `uerl.tasks`
        entry-point group. Its Task implements the public `DirectTask` hooks for
        actions, observations, rewards, and termination. It uses the existing
        DirectEnv, Session, reset, and valid-Slot lifecycle; it does not add a
        second runtime loop.

        ## Install and inspect

        Use Python 3.11 with the matching EmbodiedUE package installed. Package
        checks do not start UE:

        ```powershell
        uv pip install -e . --no-deps
        $env:UERL_TASK_PLUGINS = '{entry_point}'
        uerl tasks --filter {task_id}
        uerl check task {task_id} --json
        ```

        The check output is a machine-readable preflight report, not a Task config.
        It reports train and evaluate as supported and export as unsupported,
        because this Python Task has no Manager-generated action/observation plans.
        Do not infer ONNX or cross-language exportability from its metadata.

        ## Run on Windows/UE 5.8

        After configuring the host project and CartPole assets, train a short
        smoke Run with:

        ```powershell
        uerl train --task {task_id} --num-envs 2 --max-iterations 1 --device cpu
        ```

        The training smoke verifies Task integration, not learning quality. The
        reward weight is loaded from `src/{slug}/reward.yaml` for each fresh Task
        configuration. Editing this YAML does not change the built-in CartPole
        configuration.
        '''
    )
    tests = dedent(
        '''\
        from __MODULE__ import TASK_ID, create_registration

        from uerl.core.direct.capabilities import CapabilityStatus
        from uerl.core.direct.task import DirectTask


        def test_registration_builds_a_direct_task_and_reports_capabilities() -> None:
            registration = create_registration()
            config = registration.task_config_factory()
            task = registration.task_factory(config)

            assert registration.task_id == "__TASK_ID__"
            assert isinstance(task, DirectTask)
            assert task.capabilities.train.status is CapabilityStatus.SUPPORTED
            assert task.capabilities.evaluate.status is CapabilityStatus.SUPPORTED
            assert task.capabilities.export.status is CapabilityStatus.UNSUPPORTED
            assert task.capabilities.export.reason is not None
            assert "Manager-generated" in task.capabilities.export.reason
            assert TASK_ID == registration.task_id
        '''
    ).replace("__MODULE__", slug).replace("__TASK_ID__", task_id)
    return {
        root / "pyproject.toml": metadata,
        root / "README.md": readme,
        root / "src" / slug / "__init__.py": package,
        root / "src" / slug / "reward.yaml": "pole_position_weight: -2.0\n",
        root / "tests" / "test_registration.py": tests,
    }


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.kind == "robot":
            result = _generate_robot(args)
        elif args.kind == "task":
            result = _generate_task(args)
        elif args.kind == "direct-cartpole":
            result = _generate_direct_cartpole(args)
        else:
            result = _generate_external_cartpole(args)
    except (_ScaffoldError, OSError) as exc:
        print(f"[FAIL] scaffold: {exc}")
        return 1
    _emit(result, as_json=args.json)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
