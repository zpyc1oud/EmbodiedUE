"""Build and install real wheels, then use Tasks outside the source checkout."""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path

from uerl.cli import new

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
    generated = tmp_path / "balance-demo"
    assert new.main(["external-cartpole", "balance-demo", "--output-dir", str(generated)]) == 0
    generated_test_env = os.environ.copy()
    generated_test_env["PYTHONPATH"] = os.pathsep.join(
        (str(generated / "src"), str(REPO / "src"), generated_test_env.get("PYTHONPATH", ""))
    )
    _run([sys.executable, "-m", "pytest", "-q", "tests"], generated, env=generated_test_env)
    wheels = tmp_path / "wheels"
    wheels.mkdir()
    # Build from an sdist too, so both release formats must carry the YAML resources.
    _run([sys.executable, "-c", "from setuptools.build_meta import build_sdist; build_sdist('dist')"], project)
    source = next((project / "dist").glob("*.tar.gz"))
    unpacked = tmp_path / "unpacked"
    shutil.unpack_archive(source, unpacked)
    source_root = next(unpacked.iterdir())
    for root in (source_root, example, generated):
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
    shutil.rmtree(unpacked)
    probe = tmp_path / "probe.py"
    probe.write_text('''
import os
import sys
from pathlib import Path
sys.path.insert(0, sys.argv[1])
import uerl
assert Path(uerl.__file__).is_relative_to(Path(sys.argv[1]))
from uerl.tasks.registry import create_default_registry
from uerl.core.config.paths import repository_config_root
from uerl.cli.main import main
from uerl.training import build_run_config
from uerl.core.direct.task import DirectTask
assert repository_config_root().is_relative_to(Path(sys.argv[1]))
registry = create_default_registry(external_tasks=())
assert len(registry.list()) == 5
for entry in registry.list():
    build_run_config(entry.task_id)
os.environ['UERL_TASK_PLUGINS'] = 'example-cartpole,balance-demo'
registry = create_default_registry()
assert len(registry.list()) == 7
example_config = registry.create_task_config('Example-CartPole-v0')
generated_config = registry.create_task_config('UERL-BalanceDemo-v0')
assert example_config.rew_scale_pole_pos == -2.0
assert generated_config.rew_scale_pole_pos == -2.0
assert registry.create_task_config('UERL-CartPole-Direct-v0').rew_scale_pole_pos == -1.0
assert isinstance(registry.create_task('Example-CartPole-v0', example_config), DirectTask)
assert isinstance(registry.create_task('UERL-BalanceDemo-v0', generated_config), DirectTask)
assert main(['check', 'task', 'Example-CartPole-v0']) == 0
assert main(['check', 'task', 'UERL-BalanceDemo-v0']) == 0
assert main(['config', '--task', 'Example-CartPole-v0', '--json']) == 0
assert main(['config', '--task', 'UERL-BalanceDemo-v0', '--json']) == 0
assert main(['tasks', '--filter', 'Example-CartPole-v0']) == 0
assert main(['tasks', '--filter', 'UERL-BalanceDemo-v0']) == 0
# Package-owned resource changes feed the next fresh config without affecting built-ins.
example_resource = Path(sys.argv[1]) / 'example_cartpole' / 'reward.yaml'
example_resource.write_text('pole_position_weight: -3.0\\n', encoding='utf-8')
assert registry.create_task_config('Example-CartPole-v0').rew_scale_pole_pos == -3.0
generated_resource = Path(sys.argv[1]) / 'balance_demo' / 'reward.yaml'
generated_resource.write_text('pole_position_weight: -4.0\\n', encoding='utf-8')
assert registry.create_task_config('UERL-BalanceDemo-v0').rew_scale_pole_pos == -4.0
assert registry.create_task_config('UERL-CartPole-Direct-v0').rew_scale_pole_pos == -1.0
os.environ['UERL_TASK_PLUGINS'] = 'example-cartpole,example-cartpole'
assert main(['check', 'task', 'Example-CartPole-v0']) == 1
''', encoding="utf-8")
    # Isolated mode ignores PYTHONPATH and the working directory. Existing numerical
    # dependencies are reused; the installed target must own all product imports.
    env = os.environ.copy()
    env.pop("UERL_TASK_PLUGINS", None)
    completed = subprocess.run([sys.executable, "-I", str(probe), str(installed)], cwd=tmp_path,
                               env=env, text=True, capture_output=True, check=False)
    assert completed.returncode == 0, completed.stdout + completed.stderr
    assert "already registered" in completed.stdout
