"""Export the flat Go2 fixture and one frozen Isaac Lab rough tile."""

import argparse
import json
import traceback
from pathlib import Path


def export_terrains(output):
    import numpy as np
    import omni.physx
    from isaaclab import sim as sim_utils
    from isaaclab.terrains import TerrainGenerator
    from isaaclab.terrains.utils import create_prim_from_mesh
    from isaaclab_tasks.manager_based.locomotion.velocity.config.go2.flat_env_cfg import UnitreeGo2FlatEnvCfg
    from isaaclab_tasks.manager_based.locomotion.velocity.config.go2.rough_env_cfg import UnitreeGo2RoughEnvCfg

    flat = UnitreeGo2FlatEnvCfg()
    sim = sim_utils.SimulationContext(flat.sim.replace(device="cpu", enable_scene_query_support=True))
    ground = sim_utils.GroundPlaneCfg(physics_material=flat.scene.terrain.physics_material)
    ground.func("/World/Ground", ground)
    terrain = UnitreeGo2RoughEnvCfg().scene.terrain.terrain_generator
    seed, difficulty = 42, 0.65
    slope = TerrainGenerator(terrain.replace(
        seed=seed, curriculum=False, num_cols=1, num_rows=1, difficulty_range=(difficulty, difficulty),
        border_width=0.0, sub_terrains={"hf_pyramid_slope": terrain.sub_terrains["hf_pyramid_slope"]},
    ), device="cpu")
    geometry = slope.terrain_mesh.copy()
    geometry.apply_translation(-slope.terrain_origins[0, 0])
    offset = np.array([14.0, 0.0, 3.0])
    physics_mesh = geometry.copy()
    physics_mesh.apply_translation(offset)
    create_prim_from_mesh("/World/FrozenSlope", physics_mesh, physics_material=ground.physics_material)
    sim.reset()
    sim.step(render=False)
    query = omni.physx.get_physx_scene_query_interface()
    material = {field: getattr(ground.physics_material, field) for field in (
        "static_friction", "dynamic_friction", "restitution", "friction_combine_mode", "restitution_combine_mode",
    )}
    for name, vertices, triangles, probes, origin, collider in (
        ("flat", [[-12, -12, 0], [12, -12, 0], [12, 12, 0], [-12, 12, 0]], [[0, 1, 2], [0, 2, 3]],
         ((0, 0), (-1, 3), (5.2, -2), (11, 9)), (0, 0, 0), "/World/Ground"),
        ("slope", geometry.vertices.tolist(), geometry.faces.tolist(),
         ((0, 0), (2.81, 0.07), (-2.81, 0.07), (0.07, -2.81), (0.07, 2.81)), offset, "/World/FrozenSlope"),
    ):
        samples = []
        for x, y in probes:
            hit = query.raycast_closest((x + origin[0], y + origin[1], origin[2] + 2), (0, 0, -1), 4)
            if not hit["hit"] or collider not in str(hit["collision"]):
                raise RuntimeError(f"{name}: PhysX missed {collider} at {(x, y)}")
            samples.append({
                "xy_m": [x, y], "z_m": float(hit["position"][2] - origin[2]), "normal": list(hit["normal"]),
            })
        surface = np.asarray(vertices)[np.unique(triangles)]
        bounds = [surface.min(axis=0).tolist(), surface.max(axis=0).tolist()]
        result = {
            "source": "Isaac-Velocity-Flat-Unitree-Go2-v0" if name == "flat" else "hf_pyramid_slope",
            "spawn_m": [0, 0, 0], "bounds_m": bounds, "material": material, "probes": samples,
            "vertices_m": vertices, "triangles": triangles,
        }
        if name == "slope":
            result.update(seed=seed, difficulty=difficulty)
        path = output / f"{name}.json"
        path.write_text(json.dumps(result, allow_nan=False), encoding="utf-8")
        print(f"[PASS] Exported {path}", flush=True)
    type(sim).clear_instance()
    sim.stop()


def main():
    from isaaclab.app import AppLauncher

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    output = parser.parse_args().output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    app = AppLauncher(headless=True, device="cpu").app
    try:
        export_terrains(output)
    except Exception:
        traceback.print_exc()
        raise SystemExit(1) from None
    finally:
        app.close(wait_for_replicator=False)


if __name__ == "__main__":
    main()
