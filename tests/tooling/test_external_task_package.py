"""Build and install real wheels, then use Tasks outside the source checkout."""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

from uerl.cli import new
from uerl.core.config.manifest import GitIdentityError, capture_git_identity

REPO = Path(__file__).resolve().parents[2]


def _run(command: list[str], cwd: Path, *, env: dict[str, str] | None = None) -> None:
    completed = subprocess.run(command, cwd=cwd, env=env, text=True, capture_output=True, check=False)
    assert completed.returncode == 0, completed.stdout + completed.stderr


def test_installed_wheels_discover_external_task_and_load_resources(tmp_path: Path) -> None:
    # Stage only Python distribution inputs: the checkout/UE content cannot mask missing resources.
    project = tmp_path / "framework"
    project.mkdir()
    shutil.copytree(REPO / "src", project / "src", ignore=shutil.ignore_patterns("__pycache__", "*.egg-info"))
    for name in ("pyproject.toml", "README.md", "LICENSE", "THIRD_PARTY_NOTICES.md"):
        shutil.copy(REPO / name, project / name)
    example = tmp_path / "example"
    shutil.copytree(REPO / "examples" / "external-cartpole", example,
                    ignore=shutil.ignore_patterns("__pycache__", "*.egg-info", "build"))
    direct_example = tmp_path / "direct-example"
    shutil.copytree(
        REPO / "examples" / "external-direct-cartpole",
        direct_example,
        ignore=shutil.ignore_patterns("__pycache__", "*.egg-info", "build"),
    )
    generated = tmp_path / "balance-demo"
    assert new.main(["external-cartpole", "balance-demo", "--output-dir", str(generated)]) == 0
    generated_test_env = os.environ.copy()
    generated_test_env["PYTHONPATH"] = os.pathsep.join(
        (str(generated / "src"), str(REPO / "src"), generated_test_env.get("PYTHONPATH", ""))
    )
    _run([sys.executable, "-m", "pytest", "-q", "tests"], generated, env=generated_test_env)
    generated_direct = tmp_path / "direct-balance-demo"
    assert new.main(["direct-cartpole", "direct-balance-demo", "--output-dir", str(generated_direct)]) == 0
    generated_direct_test_env = os.environ.copy()
    generated_direct_test_env["PYTHONPATH"] = os.pathsep.join(
        (str(generated_direct / "src"), str(REPO / "src"), generated_direct_test_env.get("PYTHONPATH", ""))
    )
    _run([sys.executable, "-m", "pytest", "-q", "tests"], generated_direct, env=generated_direct_test_env)
    wheels = tmp_path / "wheels"
    wheels.mkdir()
    # Build from an sdist too, so both release formats must carry the YAML resources.
    _run([sys.executable, "-c", "from setuptools.build_meta import build_sdist; build_sdist('dist')"], project)
    source = next((project / "dist").glob("*.tar.gz"))
    unpacked = tmp_path / "unpacked"
    shutil.unpack_archive(source, unpacked)
    source_root = next(unpacked.iterdir())
    for root in (source_root, example, generated, direct_example, generated_direct):
        _run([sys.executable, "-c",
              f"from setuptools.build_meta import build_wheel; build_wheel({str(wheels)!r})"], root)
    installed = tmp_path / "installed"
    uv = shutil.which("uv")
    assert uv is not None, "Packaging tests require uv (the documented project installer)"
    _run([uv, "--no-cache", "pip", "install", "--python", sys.executable, "--no-deps", "--target", str(installed),
          *map(str, wheels.glob("*.whl"))], tmp_path)
    shutil.rmtree(project)
    shutil.rmtree(example)
    shutil.rmtree(generated)
    shutil.rmtree(direct_example)
    shutil.rmtree(generated_direct)
    shutil.rmtree(unpacked)
    probe = tmp_path / "probe.py"
    probe.write_text('''
import contextlib
import io
import json
import os
import sys
from pathlib import Path
sys.path.insert(0, sys.argv[1])
import uerl
assert Path(uerl.__file__).is_relative_to(Path(sys.argv[1]))
from uerl.tasks.registry import create_default_registry
from uerl.core.config.paths import repository_config_root
from uerl.core.config.manifest import capture_git_identity
original_path = os.environ.get('PATH', '')
os.environ['PATH'] = ''
try:
    assert capture_git_identity() == {"commit": "unavailable", "ref": "package:ue-rl-engine==1.0.0", "dirty": False}
finally:
    os.environ['PATH'] = original_path
from uerl.cli.main import main
from uerl.training import build_run_config
from uerl.core.direct.task import DirectTask
from uerl.core.direct.capabilities import CapabilityStatus
assert repository_config_root().is_relative_to(Path(sys.argv[1]))
registry = create_default_registry(external_tasks=())
assert len(registry.list()) == 5
for entry in registry.list():
    build_run_config(entry.task_id)
os.environ['UERL_TASK_PLUGINS'] = 'example-cartpole,balance-demo,example-direct-cartpole,direct-balance-demo'
registry = create_default_registry()
assert len(registry.list()) == 9
example_config = registry.create_task_config('Example-CartPole-v0')
generated_config = registry.create_task_config('UERL-BalanceDemo-v0')
direct_config = registry.create_task_config('UERL-DirectCartPole-v0')
generated_direct_config = registry.create_task_config('UERL-DirectBalanceDemo-v0')
assert example_config.rew_scale_pole_pos == -2.0
assert generated_config.rew_scale_pole_pos == -2.0
assert direct_config.rew_scale_pole_pos == -2.0
assert generated_direct_config.rew_scale_pole_pos == -2.0
assert registry.create_task_config('UERL-CartPole-Direct-v0').rew_scale_pole_pos == -1.0
assert isinstance(registry.create_task('Example-CartPole-v0', example_config), DirectTask)
assert isinstance(registry.create_task('UERL-BalanceDemo-v0', generated_config), DirectTask)
direct_task = registry.create_task('UERL-DirectCartPole-v0', direct_config)
generated_direct_task = registry.create_task('UERL-DirectBalanceDemo-v0', generated_direct_config)
assert isinstance(direct_task, DirectTask)
assert isinstance(generated_direct_task, DirectTask)
for task in (direct_task, generated_direct_task):
    assert task.capabilities.train.status is CapabilityStatus.SUPPORTED
    assert task.capabilities.evaluate.status is CapabilityStatus.SUPPORTED
    assert task.capabilities.export.status is CapabilityStatus.UNSUPPORTED
assert main(['check', 'task', 'Example-CartPole-v0']) == 0
assert main(['check', 'task', 'UERL-BalanceDemo-v0']) == 0
direct_report = io.StringIO()
with contextlib.redirect_stdout(direct_report):
    assert main(['check', 'task', 'UERL-DirectCartPole-v0', '--json']) == 0
direct_capabilities = json.loads(direct_report.getvalue())['capabilities']
assert direct_capabilities['train']['status'] == 'supported'
assert direct_capabilities['evaluate']['status'] == 'supported'
assert direct_capabilities['export']['status'] == 'unsupported'
assert 'Manager-generated' in direct_capabilities['export']['reason']
manager_report = io.StringIO()
with contextlib.redirect_stdout(manager_report):
    assert main(['check', 'task', 'Example-CartPole-v0', '--json']) == 0
assert json.loads(manager_report.getvalue())['capabilities']['export']['status'] == 'unknown'
assert main(['config', '--task', 'Example-CartPole-v0', '--json']) == 0
assert main(['config', '--task', 'UERL-BalanceDemo-v0', '--json']) == 0
assert main(['config', '--task', 'UERL-DirectCartPole-v0', '--json']) == 0
assert main(['config', '--task', 'UERL-DirectBalanceDemo-v0', '--json']) == 0
assert main(['tasks', '--filter', 'Example-CartPole-v0']) == 0
assert main(['tasks', '--filter', 'UERL-BalanceDemo-v0']) == 0
assert main(['tasks', '--filter', 'UERL-DirectCartPole-v0']) == 0
assert main(['tasks', '--filter', 'UERL-DirectBalanceDemo-v0']) == 0
# Package-owned resource changes feed the next fresh config without affecting built-ins.
example_resource = Path(sys.argv[1]) / 'example_cartpole' / 'reward.yaml'
example_resource.write_text('pole_position_weight: -3.0\\n', encoding='utf-8')
assert registry.create_task_config('Example-CartPole-v0').rew_scale_pole_pos == -3.0
generated_resource = Path(sys.argv[1]) / 'balance_demo' / 'reward.yaml'
generated_resource.write_text('pole_position_weight: -4.0\\n', encoding='utf-8')
assert registry.create_task_config('UERL-BalanceDemo-v0').rew_scale_pole_pos == -4.0
direct_resource = Path(sys.argv[1]) / 'example_direct_cartpole' / 'reward.yaml'
direct_resource.write_text('pole_position_weight: -5.0\\n', encoding='utf-8')
assert registry.create_task_config('UERL-DirectCartPole-v0').rew_scale_pole_pos == -5.0
generated_direct_resource = Path(sys.argv[1]) / 'direct_balance_demo' / 'reward.yaml'
generated_direct_resource.write_text('pole_position_weight: -6.0\\n', encoding='utf-8')
assert registry.create_task_config('UERL-DirectBalanceDemo-v0').rew_scale_pole_pos == -6.0
assert registry.create_task_config('UERL-CartPole-Direct-v0').rew_scale_pole_pos == -1.0
os.environ['UERL_TASK_PLUGINS'] = 'example-cartpole,example-cartpole'
assert main(['check', 'task', 'Example-CartPole-v0']) == 1
''', encoding="utf-8")
    # Isolated mode ignores PYTHONPATH and the working directory. Existing numerical
    # dependencies are reused; the installed target must own all product imports.
    env = os.environ.copy()
    env.pop("UERL_TASK_PLUGINS", None)
    direct_task_probe = REPO / "tests" / "tooling" / "support" / "installed_direct_task_probe.py"
    direct_task_result = subprocess.run(
        [sys.executable, "-I", str(direct_task_probe), str(installed)],
        cwd=tmp_path,
        env=env,
        text=True,
        capture_output=True,
        check=False,
    )
    assert direct_task_result.returncode == 0, direct_task_result.stdout + direct_task_result.stderr
    assert "installed Direct Tasks stepped through DirectEnv" in direct_task_result.stdout
    completed = subprocess.run([sys.executable, "-I", str(probe), str(installed)], cwd=tmp_path,
                               env=env, text=True, capture_output=True, check=False)
    assert completed.returncode == 0, completed.stdout + completed.stderr
    assert "already registered" in completed.stdout


def test_explicit_source_repository_git_error_is_preserved(tmp_path: Path) -> None:
    with pytest.raises(GitIdentityError, match="cannot capture Git identity"):
        capture_git_identity(tmp_path)
