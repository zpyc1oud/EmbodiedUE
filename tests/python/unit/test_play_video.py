"""Verify play's UE-internal recording command surface."""

from __future__ import annotations

import json
from pathlib import Path

from uerl.cli.play import _parser, _write_record_evidence
from uerl.presentation import InternalViewportRecorder, raw_ffmpeg_command
from uerl.tasks.evaluation import EvaluationSummary


def test_play_accepts_viewport_recording_options() -> None:
    args = _parser().parse_args(
        [
            "--task",
            "UERL-PhantomX-Walk-v0",
            "--checkpoint",
            "model.pt",
            "--record",
            "walk.mp4",
            "--record-seconds",
            "20",
            "--record-fps",
            "30",
        ]
    )

    assert args.record == Path("walk.mp4")
    assert args.record_seconds == 20.0
    assert args.record_fps == 30


def test_raw_ffmpeg_encodes_the_frames_written_by_ue() -> None:
    command = raw_ffmpeg_command(
        "ffmpeg.exe",
        output=Path("walk.mp4"),
        fps=30,
        raw_path=Path("frames") / "frames.bgra",
        size=(1280, 720),
        frame_count=600,
    )

    assert command[command.index("-i") + 1] == str(Path("frames") / "frames.bgra")
    assert command[command.index("-pixel_format") + 1] == "bgra"
    assert command[command.index("-video_size") + 1] == "1280x720"
    assert command[command.index("-frames:v") + 1] == "600"


def test_recorder_builds_ue_internal_capture_arguments(tmp_path: Path) -> None:
    recorder = InternalViewportRecorder(
        tmp_path / "walk.mp4",
        fps=30,
        seconds=20.0,
        size=(1280, 720),
    )

    recorder.start()
    try:
        arguments = recorder.worker_arguments()
    finally:
        recorder.abort()

    assert any(argument.startswith("-uerlrecorddir=") for argument in arguments)
    assert "-uerlrecordfps=30" in arguments
    assert "-uerlrecordwidth=1280" in arguments
    assert "-uerlrecordheight=720" in arguments
    assert "-uerlrecordmaxframes=600" in arguments


def test_record_evidence_persists_evaluation_and_video_metadata(tmp_path: Path) -> None:
    recorder = InternalViewportRecorder(
        tmp_path / "terrain-level-7.mp4",
        fps=10,
        seconds=4.0,
        size=(640, 360),
    )
    recorder.frame_count = 40
    result = EvaluationSummary(
        task_id="task",
        checkpoint=tmp_path / "model.pt",
        steps=40,
        completed_episodes=1,
        mean_episode_length=40.0,
        mean_reward=0.5,
    )

    evidence_path = _write_record_evidence(recorder, result, terrain_level=7)

    payload = json.loads(evidence_path.read_text(encoding="utf-8"))
    assert payload["evaluation"]["completed_episodes"] == 1
    assert payload["record"] == {
        "capture": "ue-internal",
        "fps": 10,
        "frames": 40,
        "recording": "PASS",
        "resolution": [640, 360],
        "terrain_level": 7,
        "termination_events": [],
        "video": str((tmp_path / "terrain-level-7.mp4").resolve().as_posix()),
    }
    assert set(payload["git_identity"]) == {"commit", "dirty", "ref"}
