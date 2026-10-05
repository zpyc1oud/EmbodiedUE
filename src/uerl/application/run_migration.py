"""Explicitly convert one historical Run's configuration into a separate copy.

This is a one-time file migration command, not a reader used by training,
playback, or export. Checkpoints are copied byte-for-byte by default. An
explicit opt-in can embed current YAML after weights-only loading and state
verification; arbitrary pickle loading is never used.
"""

from __future__ import annotations

import argparse
import copy
import filecmp
import hashlib
import json
import os
import shutil
import struct
import sys
import tempfile
from collections import OrderedDict
from collections.abc import Mapping
from dataclasses import dataclass
from pathlib import Path
from typing import Any, cast

import yaml

from ..core.config.canonical import canonical_json, to_jsonable
from ..core.config.snapshot import (
    CHECKPOINT_CONFIG_KEY,
    RESOLVED_CONFIG_SCHEMA_VERSION,
    resolved_config_from_yaml,
    resolved_config_to_yaml,
)
from ..core.config.yaml_loader import load_unique_yaml
from ..errors import ConfigError
from ..tasks.registry import create_default_registry
from ..training.runs import best_checkpoint
from .run_config import _source_from_payload


@dataclass(frozen=True, slots=True)
class _OutputFile:
    path: Path
    contents: bytes | None = None
    copy_from: Path | None = None
    embed_config_yaml: str | None = None
    source_sha256: str | None = None


@dataclass(frozen=True, slots=True)
class RunMigrationPlan:
    """Validated operations for creating an isolated migrated Run copy."""

    source: Path
    output: Path
    files: tuple[_OutputFile, ...]
    already_applied: bool = False


def plan_run_migration(
    source: Path,
    output: Path,
    *,
    embed_checkpoint_config: bool = False,
) -> RunMigrationPlan:
    """Read and validate a historical Run, without writing any files.

    Supported input files are a direct JSON or unversioned YAML resolved config,
    or a JSON/YAML manifest containing ``resolved_config``. A versioned
    ``resolved_config.yaml`` is also accepted so rerunning the command is safe.
    Every required typed field must be present and match an installed Task
    version. Nothing is inferred from current defaults.
    """

    source = Path(source).resolve()
    output = Path(output).resolve()
    if not source.is_dir():
        raise ConfigError("source Run directory does not exist", code="MIGRATION_SOURCE_MISSING", path=str(source))
    if output == source or source in output.parents:
        raise ConfigError("output must be outside the source Run directory", code="MIGRATION_OUTPUT_COLLISION")

    sidecar = _first_existing(source, ("resolved_config.json", "resolved_config.yaml"))
    manifest = _first_existing(source, ("manifest.json", "manifest.yaml"))
    if sidecar is None and manifest is None:
        raise ConfigError(
            "no resolved_config.json, resolved_config.yaml, manifest.json, or manifest.yaml was found",
            code="MIGRATION_CONFIG_MISSING",
            path=str(source),
        )

    registry = create_default_registry()
    candidates: list[tuple[Path, dict[str, object]]] = []
    if sidecar is not None:
        candidates.append((sidecar, _read_sidecar_payload(sidecar)))
    manifest_payload: dict[str, object] | None = None
    if manifest is not None:
        manifest_payload = _read_manifest_payload(manifest)
        manifest_config = _manifest_resolved_config(manifest_payload, path=manifest)
        if manifest_config is not None:
            candidates.append((manifest, manifest_config))
    if not candidates:
        raise ConfigError(
            "Run manifest has no resolved_config; recover a complete config from experiment records",
            code="MIGRATION_CONFIG_MISSING",
            path=str(manifest or source),
        )

    typed_sources = [
        (
            path,
            _source_from_payload(payload, None, strict=True, path=str(path), registry=registry),
        )
        for path, payload in candidates
    ]
    baseline = typed_sources[0][1]
    assert baseline.recorded is not None
    for path, candidate in typed_sources[1:]:
        assert candidate.recorded is not None
        if canonical_json(to_jsonable(baseline.recorded)) != canonical_json(to_jsonable(candidate.recorded)):
            raise ConfigError(
                "saved Run config files disagree; resolve the conflict from experiment records before migration",
                code="RUN_CONFIG_CONFLICT",
                path=str(path),
            )

    config_text = resolved_config_to_yaml(baseline.recorded) + "\n"
    # Validate the exact output format before planning any writes.
    converted_payload = resolved_config_from_yaml(config_text, path=str(output / "resolved_config.yaml"))
    _source_from_payload(
        converted_payload,
        None,
        strict=True,
        path=str(output / "resolved_config.yaml"),
        registry=registry,
    )

    files = [
        _OutputFile(Path("legacy") / original.name, copy_from=original)
        for original in (sidecar, manifest)
        if original is not None
    ]
    files.append(_OutputFile(Path("resolved_config.yaml"), contents=config_text.encode("utf-8")))
    if manifest_payload is not None:
        converted_manifest = dict(manifest_payload)
        if "resolved_config" in converted_manifest:
            converted_manifest["resolved_config"] = to_jsonable(baseline.recorded)
        try:
            manifest_text = yaml.safe_dump(
                converted_manifest,
                allow_unicode=True,
                default_flow_style=False,
                sort_keys=False,
            )
        except yaml.YAMLError as exc:
            raise ConfigError(
                "manifest contains values that cannot be represented as YAML",
                code="INVALID_RUN_CONFIG",
                path=str(manifest),
            ) from exc
        files.append(_OutputFile(Path("manifest.yaml"), contents=(manifest_text + "\n").encode("utf-8")))

    command = source / "command.txt"
    if command.is_file():
        files.append(_OutputFile(Path("command.txt"), copy_from=command))
    checkpoint = best_checkpoint(source)
    if embed_checkpoint_config and checkpoint is None:
        raise ConfigError(
            "no supported checkpoint was found; cannot embed Run config",
            code="MIGRATION_CHECKPOINT_MISSING",
            path=str(source),
        )
    if checkpoint is not None:
        if embed_checkpoint_config:
            _validate_checkpoint_for_embedding(checkpoint, baseline.recorded, config_text, registry)
            digest = _sha256_file(checkpoint)
            files.append(
                _OutputFile(
                    checkpoint.relative_to(source),
                    copy_from=checkpoint,
                    embed_config_yaml=config_text,
                    source_sha256=digest,
                )
            )
            files.append(
                _OutputFile(
                    Path("legacy_checkpoint") / checkpoint.relative_to(source),
                    copy_from=checkpoint,
                    source_sha256=digest,
                )
            )
        else:
            files.append(_OutputFile(checkpoint.relative_to(source), copy_from=checkpoint))

    checkpoint_mode = (
        "weights_only_config_embedded_and_state_verified"
        if embed_checkpoint_config and checkpoint is not None
        else "byte_for_byte_copy_sidecar_only"
        if checkpoint is not None
        else "no_checkpoint"
    )

    report = {
        "migration_schema_version": 1,
        "source_run": str(source),
        "resolved_config_schema_version": RESOLVED_CONFIG_SCHEMA_VERSION,
        "config_sources": [path.name for path, _payload in candidates],
        "checkpoint_mode": checkpoint_mode,
        "checkpoint_embeds_config": bool(embed_checkpoint_config and checkpoint is not None),
        "checkpoint_path": str(checkpoint.relative_to(source)) if checkpoint is not None else None,
    }
    report_text = yaml.safe_dump(report, allow_unicode=True, default_flow_style=False, sort_keys=False)
    files.append(_OutputFile(Path("migration-report.yaml"), contents=report_text.encode("utf-8")))
    plan = RunMigrationPlan(source, output, tuple(files))
    if output.exists():
        if not output.is_dir():
            raise ConfigError(
                "output path exists and is not a directory",
                code="MIGRATION_OUTPUT_COLLISION",
                path=str(output),
            )
        if any(output.iterdir()):
            if _matches_plan(plan):
                return RunMigrationPlan(source, output, tuple(files), already_applied=True)
            raise ConfigError(
                "output directory already contains files that do not match this migration; choose a new output path",
                code="MIGRATION_OUTPUT_COLLISION",
                path=str(output),
            )
    return plan


def apply_run_migration(plan: RunMigrationPlan) -> None:
    """Write a validated copy with a backup of source metadata files."""

    if plan.already_applied:
        if not _matches_plan(plan):
            raise ConfigError(
                "output changed after migration planning; choose a new output path",
                code="MIGRATION_OUTPUT_COLLISION",
                path=str(plan.output),
            )
        return
    if plan.output.exists() and any(plan.output.iterdir()):
        raise ConfigError(
            "output directory changed after planning; choose a new output path",
            code="MIGRATION_OUTPUT_COLLISION",
            path=str(plan.output),
        )
    for item in plan.files:
        if item.embed_config_yaml is not None:
            assert item.copy_from is not None and item.source_sha256 is not None
            if _sha256_file(item.copy_from) != item.source_sha256:
                raise ConfigError(
                    "source checkpoint changed after migration planning; review a new dry-run",
                    code="MIGRATION_SOURCE_CHANGED",
                    path=str(item.copy_from),
                )
    plan.output.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=f".{plan.output.name}.migration-", dir=plan.output.parent))
    try:
        for item in plan.files:
            destination = staging / item.path
            destination.parent.mkdir(parents=True, exist_ok=True)
            if item.copy_from is not None:
                if item.embed_config_yaml is not None:
                    _write_checkpoint_with_config(item.copy_from, destination, item.embed_config_yaml)
                else:
                    shutil.copy2(item.copy_from, destination)
            else:
                assert item.contents is not None
                destination.write_bytes(item.contents)
        _validate_staged_output(staging)
        for item in plan.files:
            if item.copy_from is None:
                continue
            if item.embed_config_yaml is not None:
                if not _checkpoint_matches_embedding(item.copy_from, staging / item.path, item.embed_config_yaml):
                    raise ConfigError(
                        "embedded checkpoint changed training state during serialization",
                        code="MIGRATION_CHECKPOINT_STATE_CHANGED",
                        path=str(item.copy_from),
                    )
            elif not filecmp.cmp(item.copy_from, staging / item.path, shallow=False):
                raise ConfigError(
                    "copied source file did not preserve its original bytes",
                    code="MIGRATION_COPY_MISMATCH",
                    path=str(item.copy_from),
                )
        if plan.output.exists():
            plan.output.rmdir()
        os.replace(staging, plan.output)
    finally:
        if staging.exists():
            shutil.rmtree(staging)


def main(argv: list[str] | None = None) -> int:
    """Plan a migration by default; write only when ``--apply`` is explicit."""

    parser = argparse.ArgumentParser(
        description="Copy one saved Run into a separate directory with current versioned YAML configuration."
    )
    parser.add_argument(
        "--source",
        type=Path,
        required=True,
        help="Historical Run directory; its files are not modified.",
    )
    parser.add_argument("--output", type=Path, required=True, help="New, separate recovery Run directory.")
    parser.add_argument(
        "--apply",
        action="store_true",
        help="Create the copy. Without this flag, print a dry-run plan.",
    )
    parser.add_argument(
        "--embed-checkpoint-config",
        action="store_true",
        help=(
            "opt in to weights-only checkpoint conversion, preserve original bytes under legacy_checkpoint/, "
            "and verify all other state"
        ),
    )
    args = parser.parse_args(argv)
    try:
        plan = plan_run_migration(
            args.source,
            args.output,
            embed_checkpoint_config=args.embed_checkpoint_config,
        )
        if not args.apply:
            print("[DRY RUN] no files written")
            for item in plan.files:
                detail = (
                    f"copy {item.copy_from}"
                    if item.copy_from is not None
                    else f"write {len(item.contents or b'')} bytes"
                )
                print(f"[PLAN] {item.path}: {detail}")
            return 0
        apply_run_migration(plan)
    except (ConfigError, OSError, UnicodeError, ValueError) as exc:
        print(f"[FAIL] {exc}", file=sys.stderr)
        return 1
    if plan.already_applied:
        print(f"[MIGRATED] output already matches: {plan.output}")
    else:
        print(f"[MIGRATED] recovery copy: {plan.output}")
    return 0


def _first_existing(directory: Path, names: tuple[str, ...]) -> Path | None:
    return next((directory / name for name in names if (directory / name).is_file()), None)


def _read_sidecar_payload(path: Path) -> dict[str, object]:
    text = _read_text(path)
    if path.suffix == ".json":
        value = _parse_json(text, path)
    else:
        try:
            value = load_unique_yaml(text)
        except (yaml.YAMLError, TypeError, ValueError) as exc:
            raise ConfigError("cannot parse historical Run YAML", code="INVALID_RUN_CONFIG", path=str(path)) from exc
    return _extract_config_mapping(value, path=path)


def _read_manifest_payload(path: Path) -> dict[str, object]:
    text = _read_text(path)
    if path.suffix == ".json":
        value = _parse_json(text, path)
    else:
        try:
            value = load_unique_yaml(text)
        except (yaml.YAMLError, TypeError, ValueError) as exc:
            raise ConfigError(
                "cannot parse historical Run manifest",
                code="INVALID_RUN_CONFIG",
                path=str(path),
            ) from exc
    if not isinstance(value, Mapping) or any(not isinstance(key, str) for key in value):
        raise ConfigError("Run manifest must be a string-keyed mapping", code="INVALID_RUN_CONFIG", path=str(path))
    return dict(value)


def _manifest_resolved_config(payload: Mapping[str, object], *, path: Path) -> dict[str, object] | None:
    value = payload.get("resolved_config")
    if value is None:
        return None
    return _extract_config_mapping(value, path=path)


def _extract_config_mapping(value: object, *, path: Path) -> dict[str, object]:
    if not isinstance(value, Mapping) or any(not isinstance(key, str) for key in value):
        raise ConfigError("resolved config must be a string-keyed mapping", code="INVALID_RUN_CONFIG", path=str(path))
    if "schema_version" in value or "resolved_config" in value:
        try:
            validated = resolved_config_from_yaml(
                yaml.safe_dump(dict(value), allow_unicode=True, sort_keys=False),
                path=str(path),
            )
        except (yaml.YAMLError, ConfigError) as exc:
            raise ConfigError(
                "unsupported or invalid historical config schema; missing fields cannot be inferred",
                code="UNSUPPORTED_RUN_CONFIG_SCHEMA",
                path=str(path),
            ) from exc
        return validated
    return dict(value)


def _parse_json(text: str, path: Path) -> object:
    try:
        return json.loads(text, parse_constant=_reject_non_finite)
    except (json.JSONDecodeError, TypeError, ValueError) as exc:
        raise ConfigError("cannot parse historical Run JSON", code="INVALID_RUN_CONFIG", path=str(path)) from exc


def _reject_non_finite(value: str) -> Any:
    raise ValueError(f"non-finite JSON number {value}")


def _read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError) as exc:
        raise ConfigError("cannot read historical Run metadata", code="INVALID_RUN_CONFIG", path=str(path)) from exc


def _validate_checkpoint_for_embedding(
    checkpoint: Path,
    resolved_config: object,
    config_text: str,
    registry: object,
) -> None:
    payload = _load_weights_only_checkpoint(checkpoint)
    _require_checkpoint_mapping(payload, checkpoint)
    _validate_safe_checkpoint_value(payload, path="checkpoint", ancestors=set())
    infos = payload.get("infos")
    if infos is not None and type(infos) not in (dict, OrderedDict):
        raise ConfigError(
            "checkpoint infos must be a weights-only dictionary to embed Run config",
            code="UNSUPPORTED_CHECKPOINT_STATE",
            path=f"{checkpoint}:infos",
        )
    if isinstance(infos, Mapping) and CHECKPOINT_CONFIG_KEY in infos:
        existing = infos[CHECKPOINT_CONFIG_KEY]
        if not isinstance(existing, str):
            raise ConfigError(
                "checkpoint has non-text Run config metadata; resolve it before migration",
                code="RUN_CONFIG_CONFLICT",
                path=f"{checkpoint}:infos.{CHECKPOINT_CONFIG_KEY}",
            )
        try:
            previous_payload = resolved_config_from_yaml(
                existing,
                path=f"{checkpoint}:infos.{CHECKPOINT_CONFIG_KEY}",
            )
            previous = _source_from_payload(
                previous_payload,
                None,
                strict=True,
                path=f"{checkpoint}:infos.{CHECKPOINT_CONFIG_KEY}",
                registry=registry,
            )
        except ConfigError as exc:
            raise ConfigError(
                "checkpoint already contains unsupported or incomplete Run config; recover matching metadata first",
                code="RUN_CONFIG_CONFLICT",
                path=f"{checkpoint}:infos.{CHECKPOINT_CONFIG_KEY}",
            ) from exc
        assert previous.recorded is not None
        if canonical_json(to_jsonable(previous.recorded)) != canonical_json(to_jsonable(resolved_config)):
            raise ConfigError(
                "checkpoint and historical Run config disagree; resolve the conflict before migration",
                code="RUN_CONFIG_CONFLICT",
                path=f"{checkpoint}:infos.{CHECKPOINT_CONFIG_KEY}",
            )
    # Ensure the supplied config is itself the current reader's valid schema.
    resolved_config_from_yaml(config_text, path=f"{checkpoint}:new embedded config")


def _load_weights_only_checkpoint(checkpoint: Path) -> Any:
    """Load a checkpoint through PyTorch's restricted unpickler only."""

    import torch

    try:
        payload = torch.load(checkpoint, map_location="cpu", weights_only=True)
    except Exception as exc:
        raise ConfigError(
            "checkpoint cannot be inspected by PyTorch's weights-only loader; it was not modified. "
            "No pickle fallback is available",
            code="UNSAFE_OR_UNREADABLE_CHECKPOINT",
            path=str(checkpoint),
        ) from exc
    _require_checkpoint_mapping(payload, checkpoint)
    _validate_safe_checkpoint_value(payload, path="checkpoint", ancestors=set())
    return payload


def _require_checkpoint_mapping(payload: object, checkpoint: Path) -> None:
    if type(payload) not in (dict, OrderedDict):
        raise ConfigError(
            "checkpoint root must be a weights-only dictionary to embed Run config",
            code="UNSUPPORTED_CHECKPOINT_STATE",
            path=str(checkpoint),
        )
    if any(type(key) is not str for key in cast(Mapping[object, object], payload)):
        raise ConfigError(
            "checkpoint root keys must be strings to embed Run config",
            code="UNSUPPORTED_CHECKPOINT_STATE",
            path=str(checkpoint),
        )


def _validate_safe_checkpoint_value(value: object, *, path: str, ancestors: set[int]) -> None:
    """Restrict rewrites to tensors and ordinary primitive/container state."""

    import torch

    if value is None or type(value) in (str, bool, int, float, complex, bytes):
        return
    if type(value) is torch.Tensor:
        if value.layout != torch.strided or value.is_quantized:
            raise ConfigError(
                "only dense, non-quantized tensors can be verified for checkpoint embedding",
                code="UNSUPPORTED_CHECKPOINT_STATE",
                path=path,
            )
        return
    if type(value) in (dict, OrderedDict):
        mapping = cast(Mapping[object, object], value)
        identity = id(value)
        if identity in ancestors:
            raise ConfigError(
                "checkpoint state contains a recursive container",
                code="UNSUPPORTED_CHECKPOINT_STATE",
                path=path,
            )
        ancestors.add(identity)
        try:
            for key, child in mapping.items():
                if type(key) not in (str, int):
                    raise ConfigError(
                        "checkpoint dictionary keys must be strings or integers for verified embedding",
                        code="UNSUPPORTED_CHECKPOINT_STATE",
                        path=path,
                    )
                _validate_safe_checkpoint_value(child, path=f"{path}.{key}", ancestors=ancestors)
            metadata = getattr(value, "_metadata", None)
            if metadata is not None:
                _validate_safe_checkpoint_value(metadata, path=f"{path}._metadata", ancestors=ancestors)
        finally:
            ancestors.remove(identity)
        return
    if type(value) in (list, tuple):
        sequence = cast(list[object] | tuple[object, ...], value)
        identity = id(value)
        if identity in ancestors:
            raise ConfigError(
                "checkpoint state contains a recursive container",
                code="UNSUPPORTED_CHECKPOINT_STATE",
                path=path,
            )
        ancestors.add(identity)
        try:
            for index, child in enumerate(sequence):
                _validate_safe_checkpoint_value(child, path=f"{path}[{index}]", ancestors=ancestors)
        finally:
            ancestors.remove(identity)
        return
    raise ConfigError(
        f"checkpoint value type {type(value).__name__} cannot be verified for safe embedding",
        code="UNSUPPORTED_CHECKPOINT_STATE",
        path=path,
    )


def _write_checkpoint_with_config(source: Path, destination: Path, config_text: str) -> None:
    import torch

    payload = _load_weights_only_checkpoint(source)
    _require_checkpoint_mapping(payload, source)
    infos = payload.get("infos")
    if infos is None:
        new_infos: dict[str, object] | OrderedDict[str, object] = {}
    elif type(infos) in (dict, OrderedDict):
        new_infos = copy.copy(infos)
    else:
        raise ConfigError(
            "checkpoint infos must be a weights-only dictionary to embed Run config",
            code="UNSUPPORTED_CHECKPOINT_STATE",
            path=f"{source}:infos",
        )
    new_infos[CHECKPOINT_CONFIG_KEY] = config_text
    updated = copy.copy(payload)
    updated["infos"] = new_infos
    try:
        torch.save(updated, destination)
    except Exception as exc:
        raise ConfigError(
            "could not safely serialize the migrated checkpoint; source was not modified",
            code="CHECKPOINT_SERIALIZATION_FAILED",
            path=str(destination),
        ) from exc
    saved = _load_weights_only_checkpoint(destination)
    saved_infos = saved.get("infos")
    if not isinstance(saved_infos, Mapping) or saved_infos.get(CHECKPOINT_CONFIG_KEY) != config_text:
        raise ConfigError(
            "saved checkpoint does not contain the expected Run config",
            code="CHECKPOINT_SERIALIZATION_FAILED",
            path=str(destination),
        )
    if not _checkpoint_state_equal(payload, saved):
        raise ConfigError(
            "checkpoint state changed during safe serialization; source was not modified",
            code="MIGRATION_CHECKPOINT_STATE_CHANGED",
            path=str(destination),
        )


def _checkpoint_matches_embedding(source: Path, destination: Path, config_text: str) -> bool:
    try:
        original = _load_weights_only_checkpoint(source)
        migrated = _load_weights_only_checkpoint(destination)
    except ConfigError:
        return False
    infos = migrated.get("infos")
    return (
        isinstance(infos, Mapping)
        and infos.get(CHECKPOINT_CONFIG_KEY) == config_text
        and _checkpoint_state_equal(original, migrated)
    )


def _checkpoint_state_equal(left: Mapping[str, object], right: Mapping[str, object]) -> bool:
    return _equal_checkpoint_value(_without_embedded_config(left), _without_embedded_config(right))


def _without_embedded_config(payload: Mapping[str, object]) -> object:
    normalized = cast(dict[str, object] | OrderedDict[str, object], copy.copy(payload))
    infos = normalized.get("infos")
    if type(infos) in (dict, OrderedDict):
        infos_without_config = cast(
            dict[object, object] | OrderedDict[object, object],
            copy.copy(infos),
        )
        infos_without_config.pop(CHECKPOINT_CONFIG_KEY, None)
        if not infos_without_config:
            normalized.pop("infos", None)
        else:
            normalized["infos"] = infos_without_config
    elif infos is None:
        normalized.pop("infos", None)
    return normalized


def _equal_checkpoint_value(left: object, right: object) -> bool:
    import torch

    if type(left) is not type(right):
        return False
    if type(left) is torch.Tensor:
        assert isinstance(right, torch.Tensor)
        if (
            left.layout != right.layout
            or left.dtype != right.dtype
            or left.shape != right.shape
            or left.stride() != right.stride()
            or left.storage_offset() != right.storage_offset()
            or left.requires_grad != right.requires_grad
        ):
            return False
        left_bytes = left.detach().contiguous().reshape(-1).view(torch.uint8)
        right_bytes = right.detach().contiguous().reshape(-1).view(torch.uint8)
        return torch.equal(left_bytes, right_bytes)
    if type(left) in (dict, OrderedDict):
        left_mapping = cast(Mapping[object, object], left)
        right_mapping = cast(Mapping[object, object], right)
        if [(type(key), key) for key in left_mapping] != [(type(key), key) for key in right_mapping]:
            return False
        if any(not _equal_checkpoint_value(left_mapping[key], right_mapping[key]) for key in left_mapping):
            return False
        return _equal_checkpoint_value(getattr(left, "_metadata", None), getattr(right, "_metadata", None))
    if type(left) in (list, tuple):
        left_sequence = cast(list[object] | tuple[object, ...], left)
        right_sequence = cast(list[object] | tuple[object, ...], right)
        return len(left_sequence) == len(right_sequence) and all(
            _equal_checkpoint_value(a, b) for a, b in zip(left_sequence, right_sequence, strict=True)
        )
    if type(left) is float:
        return struct.pack("!d", left) == struct.pack("!d", right)
    if type(left) is complex:
        right_complex = cast(complex, right)
        return struct.pack("!dd", left.real, left.imag) == struct.pack(
            "!dd", right_complex.real, right_complex.imag
        )
    return left == right


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as checkpoint_file:
        for block in iter(lambda: checkpoint_file.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _matches_plan(plan: RunMigrationPlan) -> bool:
    expected = {item.path for item in plan.files}
    actual = {path.relative_to(plan.output) for path in plan.output.rglob("*") if path.is_file()}
    if actual != expected:
        return False
    for item in plan.files:
        existing = plan.output / item.path
        if item.embed_config_yaml is not None:
            assert item.copy_from is not None
            if item.source_sha256 != _sha256_file(item.copy_from):
                return False
            if not _checkpoint_matches_embedding(item.copy_from, existing, item.embed_config_yaml):
                return False
        elif item.copy_from is not None:
            if not filecmp.cmp(item.copy_from, existing, shallow=False):
                return False
        elif existing.read_bytes() != item.contents:
            return False
    return True


def _validate_staged_output(directory: Path) -> None:
    config_path = directory / "resolved_config.yaml"
    payload = resolved_config_from_yaml(config_path.read_text(encoding="utf-8"), path=str(config_path))
    _source_from_payload(
        payload,
        None,
        strict=True,
        path=str(config_path),
        registry=create_default_registry(),
    )


if __name__ == "__main__":
    raise SystemExit(main())
