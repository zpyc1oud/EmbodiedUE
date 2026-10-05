"""Verify reviewable Robot and Task scaffolds."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from uerl.cli import new
from uerl.core.config.yaml_loader import load_unique_yaml


def test_robot_scaffold_writes_declaration_and_checklist(tmp_path: Path, capsys) -> None:  # type: ignore[no-untyped-def]
    assert (
        new.main(
            [
                "robot",
                "phantomx2",
                "--asset",
                "/Game/Robots/PhantomX2/SK_PhantomX2",
                "--output-dir",
                str(tmp_path),
            ]
        )
        == 0
    )

    declaration = tmp_path / "src" / "uerl" / "assets" / "robots" / "phantomx2.py"
    readme = declaration.with_suffix(".md")
    assert declaration.exists()
    assert readme.exists()
    declaration_text = declaration.read_text()
    assert 'PHANTOMX2_ASSET_PATH = "/Game/Robots/PhantomX2/SK_PhantomX2"' in declaration_text
    assert "TODO_joint" in declaration_text
    compile(declaration_text, str(declaration), "exec")
    assert "not registered yet" in readme.read_text()
    assert "[PASS] generated robot scaffold" in capsys.readouterr().out


def test_task_scaffold_json_contains_template_contract(tmp_path: Path, capsys) -> None:  # type: ignore[no-untyped-def]
    assert (
        new.main(
            [
                "task",
                "walk2",
                "--robot",
                "phantomx2",
                "--template",
                "phantomx-walk",
                "--output-dir",
                str(tmp_path),
                "--json",
            ]
        )
        == 0
    )

    result = json.loads(capsys.readouterr().out)
    assert result["task_id"] == "UERL-Walk2-v0"
    assert result["template"] == "phantomx-walk"
    assert len(result["files"]) == 5
    config = tmp_path / "src" / "uerl" / "tasks" / "walk2" / "config.py"
    yaml = tmp_path / "configs" / "tasks" / "walk2" / "training.yaml"
    registration = tmp_path / "src" / "uerl" / "tasks" / "walk2" / "registration.py"
    config_text = config.read_text()
    yaml_text = yaml.read_text()
    registration_text = registration.read_text()
    assert 'WALK2_TASK_ID = "UERL-Walk2-v0"' in config_text
    assert "robots/phantomx2/robot.yaml" in yaml_text
    assert "NotImplementedError" in registration_text
    compile(config_text, str(config), "exec")
    compile(registration_text, str(registration), "exec")


def test_scaffold_refuses_conflicts_before_writing_other_files(tmp_path: Path, capsys) -> None:  # type: ignore[no-untyped-def]
    declaration = tmp_path / "src" / "uerl" / "assets" / "robots" / "cartpole2.py"
    declaration.parent.mkdir(parents=True)
    declaration.write_text("existing", encoding="utf-8")

    assert (
        new.main(
            [
                "robot",
                "cartpole2",
                "--asset",
                "/Game/Robots/CartPole2/SK_CartPole2",
                "--output-dir",
                str(tmp_path),
            ]
        )
        == 1
    )

    assert declaration.read_text() == "existing"
    assert not declaration.with_suffix(".md").exists()
    assert "refusing to overwrite" in capsys.readouterr().out


def test_external_cartpole_generator_writes_an_installable_package(tmp_path: Path, capsys) -> None:  # type: ignore[no-untyped-def]
    project = tmp_path / "balance-demo"

    assert new.main(["external-cartpole", "balance-demo", "--output-dir", str(project), "--json"]) == 0

    result = json.loads(capsys.readouterr().out)
    assert result["kind"] == "external-cartpole"
    assert result["task_id"] == "UERL-BalanceDemo-v0"
    assert result["entry_point"] == "balance-demo"
    assert len(result["files"]) == 5

    metadata = (project / "pyproject.toml").read_text(encoding="utf-8")
    assert 'name = "uerl-balance-demo"' in metadata
    assert 'balance-demo = "balance_demo:create_registration"' in metadata

    package = project / "src" / "balance_demo"
    registration = (package / "__init__.py").read_text(encoding="utf-8")
    test_source = (project / "tests" / "test_registration.py").read_text(encoding="utf-8")
    readme = (project / "README.md").read_text(encoding="utf-8")
    assert 'TASK_ID = "UERL-BalanceDemo-v0"' in registration
    assert "create_cartpole_registration" in registration
    assert "importlib.resources" in registration
    assert "NotImplementedError" not in registration
    assert "TODO" not in registration
    assert "DirectTask" in test_source
    assert "UERL-BalanceDemo-v0" in readme
    assert "UERL_TASK_PLUGINS" in readme
    assert 'balance_demo = ["reward.yaml"]' in metadata
    assert "reward.yaml" in registration
    assert not (package / "reward.json").exists()

    reward = load_unique_yaml((package / "reward.yaml").read_text(encoding="utf-8"))
    assert reward == {"pole_position_weight": -2.0}
    compile(registration, str(package / "__init__.py"), "exec")


def test_external_cartpole_generator_refuses_existing_project_files(tmp_path: Path, capsys) -> None:  # type: ignore[no-untyped-def]
    project = tmp_path / "balance-demo"
    project.mkdir()
    existing = project / "README.md"
    existing.write_text("keep", encoding="utf-8")

    assert new.main(["external-cartpole", "balance-demo", "--output-dir", str(project)]) == 1

    assert existing.read_text(encoding="utf-8") == "keep"
    assert not (project / "pyproject.toml").exists()
    assert "refusing to overwrite" in capsys.readouterr().out


def test_direct_cartpole_generator_writes_yaml_package_with_known_capability_boundary(
    tmp_path: Path,
    capsys: pytest.CaptureFixture[str],
) -> None:
    project = tmp_path / "direct-balance-demo"

    assert new.main(["direct-cartpole", "direct-balance-demo", "--output-dir", str(project), "--json"]) == 0

    result = json.loads(capsys.readouterr().out)
    assert result["kind"] == "direct-cartpole"
    assert result["task_id"] == "UERL-DirectBalanceDemo-v0"
    assert result["entry_point"] == "direct-balance-demo"
    assert len(result["files"]) == 5

    metadata = (project / "pyproject.toml").read_text(encoding="utf-8")
    package_dir = project / "src" / "direct_balance_demo"
    package = package_dir / "__init__.py"
    tests = project / "tests" / "test_registration.py"
    generated = package.read_text(encoding="utf-8")
    assert 'name = "uerl-direct-balance-demo-direct"' in metadata
    assert 'direct-balance-demo = "direct_balance_demo:create_registration"' in metadata
    assert "class DirectCartPoleTask(DirectTask)" in generated
    assert "def compute_rewards(" in generated
    assert "def compute_terminations(" in generated
    assert "UERLDirectEnv" not in generated
    assert "Manager-generated" in tests.read_text(encoding="utf-8")
    assert load_unique_yaml((package_dir / "reward.yaml").read_text(encoding="utf-8")) == {
        "pole_position_weight": -2.0
    }
    compile(generated, str(package), "exec")


def test_checked_in_direct_example_matches_generator_output(
    tmp_path: Path,
    capsys: pytest.CaptureFixture[str],
) -> None:
    project = tmp_path / "example-direct-cartpole"
    assert (
        new.main(
            [
                "direct-cartpole",
                "example-direct-cartpole",
                "--output-dir",
                str(project),
                "--task-id",
                "UERL-DirectCartPole-v0",
            ]
        )
        == 0
    )
    capsys.readouterr()

    sample = Path(__file__).resolve().parents[3] / "examples" / "external-direct-cartpole"
    generated_files = (
        Path("pyproject.toml"),
        Path("README.md"),
        Path("src/example_direct_cartpole/__init__.py"),
        Path("src/example_direct_cartpole/reward.yaml"),
        Path("tests/test_registration.py"),
    )
    for relative_path in generated_files:
        assert (project / relative_path).read_bytes() == (sample / relative_path).read_bytes()
