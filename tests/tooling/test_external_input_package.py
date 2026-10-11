"""Build and use a separately installed provider without checkout imports."""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]


def _run(command: list[str], cwd: Path, *, env: dict[str, str] | None = None) -> None:
    result = subprocess.run(command, cwd=cwd, env=env, capture_output=True, text=True, timeout=180)
    assert result.returncode == 0, result.stdout + result.stderr


def test_external_input_wheel_contains_configuration_and_uses_public_registry(tmp_path: Path) -> None:
    framework = tmp_path / "framework"
    framework.mkdir()
    shutil.copytree(REPO / "src", framework / "src", ignore=shutil.ignore_patterns("__pycache__", "*.egg-info"))
    for name in ("pyproject.toml", "README.md", "LICENSE", "THIRD_PARTY_NOTICES.md"):
        shutil.copy(REPO / name, framework / name)
    example = tmp_path / "example"
    shutil.copytree(
        REPO / "examples/external-input", example, ignore=shutil.ignore_patterns("__pycache__", "*.egg-info", "build")
    )
    wheels = tmp_path / "wheels"
    wheels.mkdir()
    for source in (framework, example):
        _run(
            [sys.executable, "-c", f"from setuptools.build_meta import build_wheel; build_wheel({str(wheels)!r})"],
            source,
        )
    uv = shutil.which("uv")
    assert uv is not None
    installed = tmp_path / "installed"
    _run(
        [
            uv,
            "--no-cache",
            "pip",
            "install",
            "--python",
            sys.executable,
            "--no-deps",
            "--target",
            str(installed),
            *map(str, wheels.glob("*.whl")),
        ],
        tmp_path,
    )
    shutil.rmtree(framework)
    shutil.rmtree(example)
    probe = tmp_path / "probe.py"
    probe.write_text("""
import sys
from pathlib import Path
sys.path.insert(0, sys.argv[1])
import uerl
import example_input
from uerl.core.inputs import InputRegistry
assert Path(uerl.__file__).is_relative_to(Path(sys.argv[1]))
assert Path(example_input.__file__).is_relative_to(Path(sys.argv[1]))
registry = InputRegistry()
example_input.register_inputs(registry)
result = registry.compile([example_input.example_spec()])
assert result.width == 1
assert result.fields[0].descriptor.name == "input.example.value"
assert result.fields[0].offset == 0
assert result.inputs[0].spec.parameters["value"] == 2.5
assert result.fields[0].descriptor.to_mapping() == {
    "name": "input.example.value", "shape": [1], "dtype": "float32", "unit": "1",
    "frame": "none", "semantic": "constant", "source": "example.constant",
}
""")
    env = os.environ.copy()
    env.pop("PYTHONPATH", None)
    _run([sys.executable, "-I", str(probe), str(installed)], tmp_path, env=env)
