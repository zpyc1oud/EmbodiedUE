"""Import a frozen Isaac Lab terrain fixture into a UE 5.8 level."""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import traceback
from pathlib import Path

GROUND_LABEL = "Isaac Lab terrain"
SPAWN_LABEL = "Isaac Lab spawn origin"
EDITOR_ARGUMENTS = "UERL_ISAAC_TERRAIN_ARGUMENTS"


def ue_position(point):
    x, y, z = point
    return 100 * x, -100 * y, 100 * z


def import_or_verify(fixture_path, level_path, report_path, verify, *, world_ready=False):
    import unreal

    fixture = json.loads(Path(fixture_path).read_text(encoding="utf-8"))
    levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    assets = unreal.EditorAssetLibrary
    editor = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
    folder, name = level_path.rsplit("/", 1)
    mesh_path = f"{folder}/SM_{name}"
    material_path = f"{folder}/PM_{name}"
    expected_material = fixture["material"]
    combine_modes = {
        "average": unreal.FrictionCombineMode.AVERAGE,
        "min": unreal.FrictionCombineMode.MIN,
        "multiply": unreal.FrictionCombineMode.MULTIPLY,
        "max": unreal.FrictionCombineMode.MAX,
    }
    properties = {
        "friction": expected_material["dynamic_friction"],
        "static_friction": expected_material["static_friction"],
        "restitution": expected_material["restitution"],
        "friction_combine_mode": combine_modes[expected_material["friction_combine_mode"]],
        "restitution_combine_mode": combine_modes[expected_material["restitution_combine_mode"]],
        "override_friction_combine_mode": True,
        "override_restitution_combine_mode": True,
    }
    if verify and not world_ready:
        if not levels.load_level(level_path):
            raise RuntimeError(f"Could not reopen {level_path}")
        # Let the editor publish physics state for components in the loaded level.
        frames = 0
        unreal.EditorPythonScripting.set_keep_python_script_alive(True)

        def check_after_load(_delta_time):
            nonlocal frames
            frames += 1
            if frames < 3:
                return
            unreal.unregister_slate_post_tick_callback(handle)
            try:
                import_or_verify(fixture_path, level_path, report_path, True, world_ready=True)
            except Exception:
                unreal.log_error(traceback.format_exc())
            finally:
                unreal.EditorPythonScripting.set_keep_python_script_alive(False)

        handle = unreal.register_slate_post_tick_callback(check_after_load)
        return
    if not verify:
        for package in (level_path, mesh_path, material_path):
            if assets.does_asset_exist(package):
                raise ValueError(f"Destination exists: {package}")
        buffers = unreal.GeometryScriptSimpleMeshBuffers()
        vertices = [ue_position(point) for point in fixture["vertices_m"]]
        buffers.set_editor_property("vertices", [unreal.Vector(*point) for point in vertices])
        buffers.set_editor_property("uv0", [unreal.Vector2D(x / 100, y / 100) for x, y, _ in vertices])
        buffers.set_editor_property("triangles", [unreal.IntVector(*face) for face in fixture["triangles"]])
        dynamic_mesh = unreal.DynamicMesh()
        unreal.GeometryScript_MeshEdits.append_buffers_to_mesh(dynamic_mesh, buffers)
        options = unreal.GeometryScriptCreateNewStaticMeshAssetOptions()
        options.set_editor_property("enable_recompute_normals", True)
        options.set_editor_property("enable_recompute_tangents", False)
        options.set_editor_property("collision_mode", unreal.CollisionTraceFlag.CTF_USE_COMPLEX_AS_SIMPLE)
        unreal.GeometryScript_NewAssetUtils.create_new_static_mesh_asset_from_mesh(dynamic_mesh, mesh_path, options)
        mesh = unreal.load_asset(mesh_path)
        if not mesh:
            raise RuntimeError(f"Could not create {mesh_path}")
        material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            f"PM_{name}", folder, unreal.PhysicalMaterial, unreal.PhysicalMaterialFactoryNew()
        )
        if not material:
            raise RuntimeError(f"Could not create {material_path}")
        for field, value in properties.items():
            material.set_editor_property(field, value)
        mesh.get_editor_property("body_setup").set_editor_property("phys_material", material)
        saved_assets = assets.save_loaded_asset(material) and assets.save_loaded_asset(mesh)
        if not saved_assets or not levels.new_level(level_path):
            raise RuntimeError(f"Could not save terrain assets or create {level_path}")
        if "gravity_z_mps2" in fixture:
            settings = editor.get_editor_world().get_world_settings()
            settings.set_editor_property("global_gravity_set", True)
            settings.set_editor_property("global_gravity_z", 100.0 * fixture["gravity_z_mps2"])
        ground = actors.spawn_actor_from_class(unreal.StaticMeshActor, unreal.Vector())
        ground.set_actor_label(GROUND_LABEL)
        component = ground.static_mesh_component
        component.set_static_mesh(mesh)
        component.set_collision_profile_name("BlockAll")
        component.set_phys_material_override(material)
        spawn = actors.spawn_actor_from_class(unreal.TargetPoint, unreal.Vector(*ue_position(fixture["spawn_m"])))
        spawn.set_actor_label(SPAWN_LABEL)
        if not levels.save_current_level():
            raise RuntimeError(f"Could not save {level_path}")
        return

    world = editor.get_editor_world()
    gravity = {}
    if "gravity_z_mps2" in fixture:
        settings = world.get_world_settings()
        value = settings.get_editor_property("global_gravity_z") / 100.0
        assert settings.get_editor_property("global_gravity_set") and abs(value - fixture["gravity_z_mps2"]) < 1e-5
        gravity["gravity_z_mps2"] = value
    level_actors = actors.get_all_level_actors()
    [ground] = [actor for actor in level_actors if actor.get_actor_label() == GROUND_LABEL]
    [spawn] = [actor for actor in level_actors if actor.get_actor_label() == SPAWN_LABEL]
    component = ground.static_mesh_component
    mesh = component.get_editor_property("static_mesh")
    setup = mesh.get_editor_property("body_setup")
    assert setup.get_editor_property("collision_trace_flag") == unreal.CollisionTraceFlag.CTF_USE_COMPLEX_AS_SIMPLE
    material = component.get_editor_property("body_instance").get_editor_property("phys_material_override")
    for field, expected in properties.items():
        value = material.get_editor_property(field)
        assert abs(value - expected) < 1e-6 if isinstance(expected, float) else value == expected, field
    position = spawn.get_actor_location()
    spawn_m = [position.x / 100, -position.y / 100, position.z / 100]
    origin, extent = ground.get_actor_bounds(True)
    center_m = [origin.x / 100, -origin.y / 100, origin.z / 100]
    extent_m = [extent.x / 100, extent.y / 100, extent.z / 100]
    bounds_m = [[center + sign * radius for center, radius in zip(center_m, extent_m, strict=True)] for sign in (-1, 1)]
    assert all(abs(a - b) < 0.001 for a, b in zip(spawn_m, fixture["spawn_m"], strict=True)), spawn_m
    assert all(
        abs(a - b) < 0.001
        for row, ref in zip(bounds_m, fixture["bounds_m"], strict=True)
        for a, b in zip(row, ref, strict=True)
    ), bounds_m
    checks = []
    for probe in fixture["probes"]:
        x, y = probe["xy_m"]
        start = unreal.Vector(*ue_position((x, y, bounds_m[1][2] + 1)))
        end = unreal.Vector(*ue_position((x, y, bounds_m[0][2] - 1)))
        result = unreal.SystemLibrary.line_trace_single(
            world, start, end, unreal.TraceTypeQuery.TRACE_TYPE_QUERY1, False, [], unreal.DrawDebugTrace.NONE, True
        )
        assert result, probe
        hit = result.to_tuple()
        assert hit[9] == ground, hit[9]
        point, normal = hit[5], hit[7]
        height_error = abs(point.z / 100 - probe["z_m"])
        normal_error = max(abs(a - b) for a, b in zip((normal.x, -normal.y, normal.z), probe["normal"], strict=True))
        assert height_error < 0.001 and normal_error < 0.005, (probe, height_error, normal_error)
        checks.append({"xy_m": probe["xy_m"], "height_error_m": height_error, "normal_max_error": normal_error})
    Path(report_path).write_text(json.dumps({
        "level": level_path, "bounds_m": bounds_m, "spawn_m": spawn_m, "material": expected_material, "probes": checks,
        **gravity,
    }, indent=2), encoding="utf-8")
    unreal.log(
        f"[PASS] Reopened {level_path} and verified terrain, spawn origin, material and {len(checks)} collision probes"
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture", required=True, type=Path)
    parser.add_argument("--project", required=True, type=Path)
    parser.add_argument("--ue-executable", required=True, type=Path)
    parser.add_argument("--level", required=True, help="New UE level package under /Game")
    parser.add_argument("--report-dir", required=True, type=Path)
    args = parser.parse_args()
    if not re.fullmatch(r"/Game(?:/[A-Za-z0-9_]+)+", args.level):
        parser.error("--level must be a UE package path under /Game")
    executable, project, fixture = (path.resolve() for path in (args.ue_executable, args.project, args.fixture))
    for path in (executable, project, fixture):
        if not path.is_file():
            parser.error(f"file not found: {path}")
    report_dir = args.report_dir.resolve()
    report_dir.mkdir(parents=True, exist_ok=True)
    name = args.level.rsplit("/", 1)[1]
    report = report_dir / f"{name}.terrain.json"
    if report.exists():
        parser.error(f"report exists: {report}")
    for stage in ("import", "verify"):
        env = {**os.environ, EDITOR_ARGUMENTS: json.dumps((str(fixture), args.level, str(report), stage == "verify"))}
        log = report_dir / f"{name}.{stage}.log"
        result = subprocess.run([
            str(executable), str(project), "-unattended", "-nullrhi", "-nosound", "-nop4",
            "-EnablePlugins=GeometryScripting", f"-ExecutePythonScript={Path(__file__).resolve()}", f"-abslog={log}",
        ], env=env, check=False)
        if result.returncode:
            print(f"{stage} failed. See {log}")
            return result.returncode
    if not report.is_file():
        print(f"Verification did not produce a report. See {log}")
        return 1
    return 0


if __name__ == "__main__":
    if EDITOR_ARGUMENTS in os.environ:
        import_or_verify(*json.loads(os.environ[EDITOR_ARGUMENTS]))
    else:
        raise SystemExit(main())
