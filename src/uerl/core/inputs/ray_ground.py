"""Declare configurable ground rays independently of a training or game map."""

from __future__ import annotations

import math
from dataclasses import replace

from ...errors import ConfigError
from .contracts import InputField, InputSpec, ResolvedInput


class RayGroundFactory:
    """Validate the native uerl.ray_ground version 1 parameter contract."""

    provider_id = "uerl.ray_ground"
    version = 1

    def validate(self, spec: InputSpec) -> ResolvedInput:
        path = f"inputs.{spec.name}"

        def fail(field: str, message: str) -> ConfigError:
            return ConfigError(message, path=f"{path}.{field}", code="RAY_GROUND_INVALID")

        if spec.provider_id != self.provider_id or spec.provider_version != self.version:
            raise fail("provider_id", "expected uerl.ray_ground version 1")
        if spec.attachment is None:
            raise fail("attachment", "ground rays require a logical attachment frame")
        bindings = spec.scene_bindings or ("ground",)
        if len(bindings) != 1:
            raise fail("scene_bindings", "ground rays require exactly one logical ground binding")
        allowed = {"offsets_m", "start_height_m", "end_depth_m", "alignment", "output"}
        unknown = set(spec.parameters) - allowed
        if unknown:
            raise fail("parameters", f"unknown ray parameters: {sorted(unknown)}")
        offsets = spec.parameters.get("offsets_m", ((0.0, 0.0),))
        if not isinstance(offsets, tuple) or not offsets:
            raise fail("parameters.offsets_m", "expected a nonempty list of XY offsets in metres")
        normalized: list[tuple[float, float]] = []
        for index, point in enumerate(offsets):
            if not isinstance(point, tuple) or len(point) != 2:
                raise fail(f"parameters.offsets_m[{index}]", "expected two finite numeric coordinates")
            if any(type(value) not in (int, float) or not math.isfinite(value * 100) for value in point):
                raise fail(f"parameters.offsets_m[{index}]", "expected two finite numeric coordinates")
            normalized.append((float(point[0]), float(point[1])))
        effective: dict[str, object] = {"offsets_m": normalized}
        for key, default in (("start_height_m", 1.0), ("end_depth_m", 2.0)):
            value = spec.parameters.get(key, default)
            if (
                isinstance(value, bool)
                or not isinstance(value, (int, float))
                or not math.isfinite(value * 100)
                or value <= 0
            ):
                raise fail(f"parameters.{key}", "expected a finite positive distance in metres")
            effective[key] = float(value)
        alignment = spec.parameters.get("alignment", "yaw")
        output = spec.parameters.get("output", "height")
        if alignment not in ("yaw", "world"):
            raise fail("parameters.alignment", "expected yaw or world")
        if output not in ("height", "clearance"):
            raise fail("parameters.output", "expected height or clearance")
        effective.update(alignment=alignment, output=output)
        return ResolvedInput(
            replace(spec, parameters=effective, scene_bindings=bindings),
            (
                InputField(
                    f"input.{spec.name}.{output}",
                    (len(normalized),),
                    "m",
                    "world/z-relative",
                    "terrain_height" if output == "height" else "ground_clearance",
                    self.provider_id,
                ),
            ),
        )
