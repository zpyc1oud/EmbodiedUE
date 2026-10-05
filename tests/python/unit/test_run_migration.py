"""Run-file migration creates a verified copy without loading checkpoint data."""

from __future__ import annotations

import json
from pathlib import Path

import pytest
import torch
import yaml

from uerl.application.run_migration import apply_run_migration, plan_run_migration
from uerl.core.config.canonical import to_jsonable
from uerl.core.config.snapshot import resolved_config_from_yaml
from uerl.errors import ConfigError
from uerl.tasks.cartpole import CARTPOLE_TASK_ID
from uerl.training import build_run_config


def _legacy_json_run(directory: Path) -> tuple[dict[str, object], Path]:
    config = to_jsonable(build_run_config(CARTPOLE_TASK_ID))
    directory.mkdir(parents=True)
    sidecar = directory / "resolved_config.json"
    sidecar.write_text(json.dumps(config), encoding="utf-8")
    return config, sidecar


def test_migration_dry_run_plans_versioned_yaml_and_leaves_sources_untouched(
    tmp_path: Path,
    capsys: pytest.CaptureFixture[str],
) -> None:
    from uerl.application.run_migration import main

    source = tmp_path / "legacy-run"
    _config, original = _legacy_json_run(source)
    original_bytes = original.read_bytes()
    checkpoint = source / "rsl_rl" / "model_10.pt"
    checkpoint.parent.mkdir()
    checkpoint.write_bytes(b"checkpoint bytes are copied, never unpickled")
    output = tmp_path / "recovered-run"

    assert main(["--source", str(source), "--output", str(output)]) == 0

    assert "DRY RUN" in capsys.readouterr().out
    assert not output.exists()
    assert original.read_bytes() == original_bytes


def test_migration_copies_complete_config_and_checkpoint_and_is_idempotent(tmp_path: Path) -> None:
    source = tmp_path / "legacy-run"
    expected_config, original = _legacy_json_run(source)
    original_bytes = original.read_bytes()
    checkpoint = source / "rsl_rl" / "model_10.pt"
    checkpoint.parent.mkdir()
    checkpoint.write_bytes(b"checkpoint bytes must remain byte-identical")
    checkpoint_bytes = checkpoint.read_bytes()
    command = source / "command.txt"
    command.write_text("uerl train --task example\n", encoding="utf-8")
    output = tmp_path / "recovered-run"

    plan = plan_run_migration(source, output)
    apply_run_migration(plan)

    restored = resolved_config_from_yaml((output / "resolved_config.yaml").read_text(encoding="utf-8"))
    assert restored == expected_config
    assert (output / "legacy" / "resolved_config.json").read_bytes() == original_bytes
    assert (output / "rsl_rl" / "model_10.pt").read_bytes() == checkpoint_bytes
    assert (output / "command.txt").read_bytes() == command.read_bytes()
    assert original.read_bytes() == original_bytes
    assert checkpoint.read_bytes() == checkpoint_bytes

    repeated = plan_run_migration(source, output)
    assert repeated.already_applied
    apply_run_migration(repeated)
    assert (output / "resolved_config.yaml").is_file()


def test_migration_rejects_missing_config_fields_before_creating_output(tmp_path: Path) -> None:
    source = tmp_path / "legacy-run"
    config, _original = _legacy_json_run(source)
    task = config["task"]
    assert isinstance(task, dict)
    del task["rew_scale_alive"]
    (source / "resolved_config.json").write_text(json.dumps(config), encoding="utf-8")
    output = tmp_path / "recovered-run"

    with pytest.raises(ConfigError):
        plan_run_migration(source, output)

    assert not output.exists()


def test_migration_rejects_disagreeing_config_and_manifest(tmp_path: Path) -> None:
    source = tmp_path / "legacy-run"
    config, _original = _legacy_json_run(source)
    manifest_config = json.loads(json.dumps(config))
    assert isinstance(manifest_config["worker"], dict)
    manifest_config["worker"]["decimation"] = [5, 5]
    (source / "manifest.json").write_text(
        json.dumps({"resolved_config": manifest_config}),
        encoding="utf-8",
    )

    with pytest.raises(ConfigError, match="disagree"):
        plan_run_migration(source, tmp_path / "recovered-run")


def test_migration_refuses_output_collisions(tmp_path: Path) -> None:
    source = tmp_path / "legacy-run"
    _legacy_json_run(source)
    output = tmp_path / "recovered-run"
    output.mkdir()
    (output / "notes.txt").write_text("keep", encoding="utf-8")

    with pytest.raises(ConfigError, match="already contains"):
        plan_run_migration(source, output)

    assert (output / "notes.txt").read_text(encoding="utf-8") == "keep"


def test_migration_adapts_unversioned_yaml_sidecar_and_manifest(tmp_path: Path) -> None:
    config = to_jsonable(build_run_config(CARTPOLE_TASK_ID))
    source = tmp_path / "legacy-run"
    source.mkdir()
    (source / "resolved_config.yaml").write_text(
        yaml.safe_dump(config, sort_keys=False),
        encoding="utf-8",
    )
    (source / "manifest.json").write_text(json.dumps({"resolved_config": config}), encoding="utf-8")
    output = tmp_path / "recovered-run"

    apply_run_migration(plan_run_migration(source, output))

    assert resolved_config_from_yaml((output / "resolved_config.yaml").read_text(encoding="utf-8")) == config
    assert (output / "legacy" / "resolved_config.yaml").is_file()
    assert (output / "manifest.yaml").is_file()


def test_migration_can_embed_config_in_a_safe_checkpoint_copy(tmp_path: Path) -> None:
    source = tmp_path / "legacy-run"
    config, _original = _legacy_json_run(source)
    checkpoint = source / "rsl_rl" / "model_10.pt"
    checkpoint.parent.mkdir()
    torch.save(
        {
            "model_state_dict": {"weight": torch.tensor([[1.25, -2.5]])},
            "optimizer_state_dict": {
                "state": {0: {"step": torch.tensor(11), "exp_avg": torch.tensor([0.2, 0.4])}},
                "param_groups": [{"params": [0], "lr": 0.001}],
            },
            "iter": 11,
            "infos": {"existing": {"seed": 17}},
        },
        checkpoint,
    )
    checkpoint_bytes = checkpoint.read_bytes()
    output = tmp_path / "recovered-run"

    plan = plan_run_migration(source, output, embed_checkpoint_config=True)
    apply_run_migration(plan)

    original_state = torch.load(checkpoint, map_location="cpu", weights_only=True)
    migrated_checkpoint = output / "rsl_rl" / "model_10.pt"
    migrated_state = torch.load(migrated_checkpoint, map_location="cpu", weights_only=True)
    assert migrated_state["model_state_dict"].keys() == original_state["model_state_dict"].keys()
    assert torch.equal(
        migrated_state["model_state_dict"]["weight"],
        original_state["model_state_dict"]["weight"],
    )
    assert torch.equal(
        migrated_state["optimizer_state_dict"]["state"][0]["step"],
        original_state["optimizer_state_dict"]["state"][0]["step"],
    )
    assert torch.equal(
        migrated_state["optimizer_state_dict"]["state"][0]["exp_avg"],
        original_state["optimizer_state_dict"]["state"][0]["exp_avg"],
    )
    assert (
        migrated_state["optimizer_state_dict"]["param_groups"] == original_state["optimizer_state_dict"]["param_groups"]
    )
    assert migrated_state["iter"] == original_state["iter"] == 11
    assert migrated_state["infos"]["existing"] == original_state["infos"]["existing"]
    assert migrated_state["infos"]["uerl_resolved_config_yaml"]
    assert (output / "legacy_checkpoint" / "rsl_rl" / "model_10.pt").read_bytes() == checkpoint_bytes
    assert checkpoint.read_bytes() == checkpoint_bytes
    repeated = plan_run_migration(source, output, embed_checkpoint_config=True)
    assert repeated.already_applied
    apply_run_migration(repeated)


def test_migration_embed_rejects_conflicting_checkpoint_identity(tmp_path: Path) -> None:
    source = tmp_path / "legacy-run"
    config, _original = _legacy_json_run(source)
    checkpoint = source / "model_final.pt"
    changed_config = json.loads(json.dumps(config))
    assert isinstance(changed_config["task"], dict)
    changed_config["task"]["rew_scale_alive"] = 0.25
    torch.save(
        {
            "model_state_dict": {"weight": torch.tensor([1.0])},
            "infos": {
                "uerl_resolved_config_yaml": yaml.safe_dump(
                    {
                        "schema_version": 1,
                        "resolved_config": changed_config,
                    }
                )
            },
        },
        checkpoint,
    )
    before = checkpoint.read_bytes()

    with pytest.raises(ConfigError, match="disagree"):
        plan_run_migration(source, tmp_path / "recovered-run", embed_checkpoint_config=True)

    assert checkpoint.read_bytes() == before
    assert not (tmp_path / "recovered-run").exists()


def test_migration_embed_rejects_unreadable_checkpoint_without_pickle_fallback(tmp_path: Path) -> None:
    source = tmp_path / "legacy-run"
    _legacy_json_run(source)
    checkpoint = source / "model_final.pt"
    checkpoint.write_bytes(b"not a checkpoint")
    before = checkpoint.read_bytes()
    output = tmp_path / "recovered-run"

    with pytest.raises(ConfigError, match="weights-only"):
        plan_run_migration(source, output, embed_checkpoint_config=True)

    assert checkpoint.read_bytes() == before
    assert not output.exists()
