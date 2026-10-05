"""Check safe, versioned YAML snapshots for resolved Run configuration."""

from __future__ import annotations

import pytest
import yaml

from uerl.core.config.canonical import to_jsonable
from uerl.core.config.snapshot import (
    RESOLVED_CONFIG_SCHEMA_VERSION,
    decode_worker_args,
    encode_worker_args,
    resolved_config_from_yaml,
    resolved_config_to_yaml,
)
from uerl.errors import ConfigError
from uerl.tasks.cartpole import CARTPOLE_TASK_ID
from uerl.training import build_run_config


def test_resolved_config_snapshot_round_trips_as_versioned_yaml() -> None:
    config = build_run_config(
        CARTPOLE_TASK_ID,
        overrides={
            "worker.decimation": "[2, 5]",
            "worker.run_seed": "42",
            "task.rew_scale_alive": "0.75",
            "runner.parameters.hidden_dims": "[32, 16]",
            "session.worker_args": '["-project=demo.uproject", "-windowed"]',
        },
    )

    snapshot = resolved_config_to_yaml(config)
    envelope = yaml.safe_load(snapshot)

    assert envelope["schema_version"] == RESOLVED_CONFIG_SCHEMA_VERSION
    assert "resolved_config:" in snapshot
    assert resolved_config_from_yaml(snapshot) == to_jsonable(config)


@pytest.mark.parametrize(
    ("snapshot", "code"),
    [
        ("schema_version: 999\nresolved_config: {}\n", "UNSUPPORTED_RUN_CONFIG_SCHEMA"),
        ("schema_version: 1\nresolved_config: {}\nextra: true\n", "INVALID_RUN_CONFIG"),
        ("schema_version: 1\nresolved_config: {}\nschema_version: 1\n", "INVALID_RUN_CONFIG"),
        ("!!python/object/apply:os.system ['echo unsafe']", "INVALID_RUN_CONFIG"),
    ],
)
def test_resolved_config_yaml_rejects_unknown_or_unsafe_snapshot_shape(
    snapshot: str,
    code: str,
) -> None:
    with pytest.raises(ConfigError) as error:
        resolved_config_from_yaml(snapshot)

    assert error.value.code == code


def test_worker_argument_yaml_keeps_string_list_semantics_and_reads_legacy_json() -> None:
    encoded = encode_worker_args(["/tmp/Project.uproject", "-key=value with spaces"])

    assert decode_worker_args(encoded) == ["/tmp/Project.uproject", "-key=value with spaces"]
    assert decode_worker_args('["-legacy", "-args"]') == ["-legacy", "-args"]


@pytest.mark.parametrize("invalid", ["{}", "[1, true]", "[one, null]"])
def test_worker_argument_yaml_rejects_non_string_sequences(invalid: str) -> None:
    with pytest.raises(ConfigError, match="list of strings"):
        decode_worker_args(invalid)
