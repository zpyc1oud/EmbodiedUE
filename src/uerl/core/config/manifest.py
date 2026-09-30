"""Persist one resolved Run Manifest at the filesystem boundary."""

from __future__ import annotations

import os
import subprocess
import tempfile
from collections.abc import Mapping
from dataclasses import dataclass
from pathlib import Path

from .canonical import canonical_json, sha256_hex, to_jsonable
from .models import ResolvedRunConfig, _freeze_value

_SOURCE_REPOSITORY_ROOT = Path(__file__).resolve().parents[4]


class GitIdentityError(RuntimeError):
    """Report a failed source-checkout identity lookup."""


@dataclass(frozen=True, slots=True)
class RunManifest:
    """Describe the resolved Config and external Worker negotiation for one Run.

    The manifest is an audit snapshot, not a live control object. Mapping
    fields are recursively frozen during construction so the identity written
    before Ready cannot drift while the Session is running.
    """

    resolved_config: ResolvedRunConfig
    run_seed: int
    run_directory: Path
    effective_worker_config: Mapping[str, object]
    build_identity: Mapping[str, object]
    selected_protocol: Mapping[str, object]
    schema_hashes: Mapping[str, str]
    layout_hashes: Mapping[str, str]
    seed_derivation_version: str
    git_identity: Mapping[str, object]

    def __post_init__(self) -> None:
        required_identity = {"commit", "ref", "dirty"}
        missing_identity = required_identity.difference(self.git_identity)
        if missing_identity:
            raise ValueError(f"git identity is missing required fields: {sorted(missing_identity)}")
        if not isinstance(self.git_identity["commit"], str) or not self.git_identity["commit"]:
            raise ValueError("git identity commit must be a non-empty string")
        if not isinstance(self.git_identity["ref"], str) or not self.git_identity["ref"]:
            raise ValueError("git identity ref must be a non-empty string")
        if not isinstance(self.git_identity["dirty"], bool):
            raise ValueError("git identity dirty must be a boolean")
        object.__setattr__(self, "effective_worker_config", _freeze_value(self.effective_worker_config))
        object.__setattr__(self, "build_identity", _freeze_value(self.build_identity))
        object.__setattr__(self, "selected_protocol", _freeze_value(self.selected_protocol))
        object.__setattr__(self, "schema_hashes", _freeze_value(self.schema_hashes))
        object.__setattr__(self, "layout_hashes", _freeze_value(self.layout_hashes))
        object.__setattr__(self, "git_identity", _freeze_value(self.git_identity))


def capture_git_identity(repo_root: Path | None = None) -> Mapping[str, object]:
    """Capture the source checkout identity at the Run filesystem boundary."""

    root = _SOURCE_REPOSITORY_ROOT if repo_root is None else repo_root
    commit = _git_text(root, "rev-parse", "HEAD")
    ref = _git_text(root, "branch", "--show-current") or "HEAD"
    dirty = bool(_git_text(root, "status", "--porcelain=v1"))
    return {"commit": commit, "ref": ref, "dirty": dirty}


def _git_text(repo_root: Path, *arguments: str) -> str:
    completed = subprocess.run(
        ["git", *arguments],
        cwd=repo_root,
        stdin=subprocess.DEVNULL,
        capture_output=True,
        text=True,
        check=False,
    )
    if completed.returncode != 0:
        detail = completed.stderr.strip() or "git command failed"
        raise GitIdentityError(f"cannot capture Git identity: {detail}")
    return completed.stdout.strip()


class RunRecorder:
    """Write resolved Config and Manifest files before ReadyAck."""

    def __init__(self, run_directory: Path) -> None:
        """Create a recorder rooted at one Run directory.

        Args:
            run_directory: Directory receiving ``resolved_config.json`` and
                ``manifest.json``. Parent directories are created on first write.
        """

        self._run_directory = run_directory

    def write_resolved_config(self, config: ResolvedRunConfig) -> Path:
        """Atomically write the resolved Config used by the Run.

        Args:
            config: The immutable resolved configuration that was used to build
                the Worker projection.

        Returns:
            The path of the replaced ``resolved_config.json`` file.

        Side effects:
            Create the Run directory, write a temporary canonical JSON file,
            flush it to disk, and replace the target without exposing a partial
            file.
        """

        return self._write_json("resolved_config.json", to_jsonable(config))

    def write_manifest_atomic(self, manifest: RunManifest) -> str:
        """Atomically write the Manifest and return its audit hash.

        Args:
            manifest: The resolved Config and external negotiation snapshot
                captured before Ready acknowledgement.

        Returns:
            A SHA-256 hash of the canonical manifest identity excluding the local
            ``run_directory`` path.

        Side effects:
            Atomically replace ``manifest.json`` after flushing the complete
            canonical payload. The returned hash is the identity later sent in
            ReadyAck.
        """

        payload = to_jsonable(manifest)
        manifest_json = canonical_json(payload)
        identity_payload = dict(payload)
        identity_payload.pop("run_directory", None)
        manifest_hash = sha256_hex(canonical_json(identity_payload))
        self._write_json("manifest.json", payload, serialized=manifest_json)
        return manifest_hash

    def _write_json(self, filename: str, payload: object, *, serialized: str | None = None) -> Path:
        self._run_directory.mkdir(parents=True, exist_ok=True)
        target = self._run_directory / filename
        temporary: Path | None = None
        try:
            with tempfile.NamedTemporaryFile(
                mode="w",
                encoding="utf-8",
                dir=self._run_directory,
                prefix=f".{filename}.",
                suffix=".tmp",
                delete=False,
            ) as stream:
                temporary = Path(stream.name)
                stream.write(serialized if serialized is not None else canonical_json(payload))
                stream.write("\n")
                # Flush before replace so Ready cannot acknowledge a manifest
                # whose durable contents are only present in Python buffers.
                stream.flush()
                os.fsync(stream.fileno())
            # Replace is atomic within the Run directory, so readers see either
            # the previous complete file or the new complete file.
            os.replace(temporary, target)
        finally:
            if temporary is not None and temporary.exists():
                temporary.unlink()
        return target
