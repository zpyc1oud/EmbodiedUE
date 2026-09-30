"""Verify reviewable Robot and Task scaffolds."""

from __future__ import annotations

import json
from pathlib import Path

from uerl.cli import new


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
