"""Capture UE-rendered frames and encode them as a video."""

from __future__ import annotations

import math
import shutil
import subprocess
import tempfile
import time
from pathlib import Path


def raw_ffmpeg_command(
    executable: str,
    *,
    output: Path,
    fps: int,
    raw_path: Path,
    size: tuple[int, int],
    frame_count: int,
) -> list[str]:
    """Build an encoder command for UE's post-simulation BGRA frame stream."""

    width, height = size
    return [
        executable,
        "-y",
        "-f",
        "rawvideo",
        "-pixel_format",
        "bgra",
        "-video_size",
        f"{width}x{height}",
        "-framerate",
        str(fps),
        "-i",
        str(raw_path),
        "-frames:v",
        str(frame_count),
        "-an",
        "-c:v",
        "libx264",
        "-preset",
        "veryfast",
        "-crf",
        "20",
        "-pix_fmt",
        "yuv420p",
        "-movflags",
        "+faststart",
        str(output),
    ]


class InternalViewportRecorder:
    """Capture UE-rendered frames and encode them without reading the desktop."""

    def __init__(
        self,
        output: Path,
        *,
        fps: int,
        seconds: float,
        size: tuple[int, int],
    ) -> None:
        self.output = output.resolve()
        self.fps = fps
        self.seconds = seconds
        self.size = size
        self.frames_directory: Path | None = None
        self.frame_count = 0
        self._error: BaseException | None = None

    @property
    def max_frames(self) -> int:
        return max(1, math.ceil(self.seconds * self.fps))

    def start(self) -> None:
        """Create a private frame directory before the UE process is launched."""

        self.output.parent.mkdir(parents=True, exist_ok=True)
        self.frames_directory = Path(
            tempfile.mkdtemp(
                prefix=f".{self.output.stem}.internal-frames-",
                dir=str(self.output.parent),
            )
        )

    def worker_arguments(self) -> list[str]:
        """Return the UE command-line contract for internal capture."""

        if self.frames_directory is None:
            raise RuntimeError("internal recorder must be started before building worker arguments")
        width, height = self.size
        return [
            f"-uerlrecorddir={self.frames_directory}",
            f"-uerlrecordfps={self.fps}",
            f"-uerlrecordwidth={width}",
            f"-uerlrecordheight={height}",
            f"-uerlrecordmaxframes={self.max_frames}",
        ]

    def stop(self, *, raise_on_error: bool = True) -> None:
        """Wait for UE's completion marker, encode the frame sequence, and clean it up."""

        if self.frames_directory is None:
            return
        try:
            status = self._wait_for_completion()
            fields = dict(line.split("=", 1) for line in status.splitlines() if "=" in line)
            try:
                self.frame_count = int(fields["frames"])
            except (KeyError, ValueError) as exc:
                raise RuntimeError("UE internal recorder completion marker has no valid frame count") from exc
            if self.frame_count < 1 or self.frame_count > self.max_frames:
                raise RuntimeError(
                    f"UE internal recorder returned invalid frame count {self.frame_count}; "
                    f"expected 1..{self.max_frames}"
                )
            if fields.get("format") != "bgra8" or fields.get("file") != "frames.bgra":
                raise RuntimeError("UE internal recorder completion marker has an unsupported frame stream")
            raw_path = self.frames_directory / "frames.bgra"
            if not raw_path.is_file():
                raise RuntimeError(f"UE internal recorder produced no raw frame stream at {raw_path}")
            frame_bytes = self.size[0] * self.size[1] * 4
            raw_bytes = raw_path.stat().st_size
            if frame_bytes < 1 or raw_bytes != self.frame_count * frame_bytes:
                raise RuntimeError(
                    f"UE internal recorder produced {raw_bytes} raw bytes for "
                    f"{self.frame_count} frames at {raw_path}"
                )
            self._run_ffmpeg_raw(raw_path, self.frame_count)
            if not self.output.is_file() or self.output.stat().st_size == 0:
                raise RuntimeError(f"internal recorder produced no video at {self.output}")
            shutil.rmtree(self.frames_directory)
            self.frames_directory = None
        except BaseException as exc:
            self._error = exc
            if raise_on_error:
                raise

    def abort(self) -> None:
        """Release a prepared directory when UE was never launched."""

        if self.frames_directory is not None:
            shutil.rmtree(self.frames_directory)
            self.frames_directory = None

    def _wait_for_completion(self) -> str:
        assert self.frames_directory is not None
        done = self.frames_directory / "recording.done"
        error = self.frames_directory / "recording.error"
        deadline = time.monotonic() + 30.0
        while time.monotonic() < deadline:
            if error.is_file():
                raise RuntimeError(f"UE internal recorder failed: {error.read_text(encoding='utf-8')}")
            if done.is_file():
                return done.read_text(encoding="utf-8")
            time.sleep(0.1)
        raise TimeoutError(f"timed out waiting for UE internal recorder completion in {self.frames_directory}")

    def _run_ffmpeg_raw(self, raw_path: Path, frame_count: int) -> None:
        import imageio_ffmpeg

        assert self.frames_directory is not None
        log_path = self.output.with_suffix(f"{self.output.suffix}.ffmpeg.log")
        command = raw_ffmpeg_command(
            imageio_ffmpeg.get_ffmpeg_exe(),
            output=self.output,
            fps=self.fps,
            raw_path=raw_path,
            size=self.size,
            frame_count=frame_count,
        )
        with log_path.open("wb") as log:
            return_code = subprocess.run(
                command,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=log,
                check=False,
            ).returncode
        if return_code != 0:
            raise RuntimeError(
                f"ffmpeg raw internal frame encoding failed with exit code {return_code}; see {log_path}"
            )
