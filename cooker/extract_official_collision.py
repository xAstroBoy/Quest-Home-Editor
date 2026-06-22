#!/usr/bin/env python
"""Extract the ENTIRE layer data & collision from an official Meta home APK (V203).

The official homes carry the proper, DEVICE-COMPATIBLE collision the cook can't yet make:
  * collider ENTITIES (the "layer data"): Transform + ColliderMeshPlatformComponent (-> a *_col_mesh.usda)
    + PhysicsBodyPlatformComponent {type: StaticCollision}.  (in home_simple_colliders.hstf / augment_collisions.hstf)
  * collision GEOMETRY: each *_col_mesh.usda's __mesh_sub_targets__ entry = a "MESH"/"MATL" FlatBuffer.
  * cooked PhysX: each *_col_mesh.usda's __phys_mesh_sub_targets__ entry = an "SEBD" blob (the Meta PhysX
    cooked-trimesh format — the goldmine reference for fixing our SEBD, see project_hsl_physx_sebd_format).

Usage:  python cooker/extract_official_collision.py "Envs To check/v203 Ufficial Envs/haven2025.apk" [outdir]
Dumps:  <outdir>/<env>/collision_layers.json   (every collider entity: transform + mesh ref + physics type)
        <outdir>/<env>/col_mesh/<name>.mesh     (the collision geometry FlatBuffers)
        <outdir>/<env>/phys_sebd/<name>.sebd     (the device PhysX blobs)
        <outdir>/<env>/SUMMARY.txt
"""
import zipfile, io, json, os, sys, re

def load_scene(apk):
    z = zipfile.ZipFile(apk)
    if "assets/scene.zip" not in z.namelist():
        raise SystemExit("no assets/scene.zip in " + apk)
    return zipfile.ZipFile(io.BytesIO(z.read("assets/scene.zip")))

def main():
    if len(sys.argv) < 2:
        print(__doc__); return 1
    apk = sys.argv[1]
    outroot = sys.argv[2] if len(sys.argv) > 2 else "cooker/_official_collision"
    env = os.path.splitext(os.path.basename(apk))[0]
    out = os.path.join(outroot, env)
    sz = load_scene(apk); names = sz.namelist()

    # 1) layer data — every collider/physics ENTITY from the collider templates
    layers = []
    for n in names:
        if ".hstf/template" not in n: continue          # ANY entity template (collider files are named per-env)
        try: j = json.loads(sz.read(n))
        except Exception: continue
        if not isinstance(j, dict) or "entities" not in j: continue
        for e in j["entities"]:
            comps = e.get("components", [])
            classes = [c.get("data", {}).get("class", "").split("::")[-1] for c in comps]
            if not any("Collider" in k or "Physics" in k for k in classes): continue
            rec = {"name": e.get("name"), "id": e.get("id"), "from": n.split("/")[-2]}
            for c in comps:
                d = c.get("data", {}); cls = d.get("class", "").split("::")[-1]
                rec[cls] = d.get("data")
            layers.append(rec)

    # 2) the collision MESH geometry + 3) the SEBD PhysX blobs
    meshes = [n for n in names if "__mesh_sub_targets__" in n and "col_mesh" in n and "material" not in n]
    physs  = [n for n in names if "__phys_mesh_sub_targets__" in n]

    os.makedirs(out, exist_ok=True)
    json.dump(layers, open(os.path.join(out, "collision_layers.json"), "w"), indent=1)
    def dump(sub, lst, ext):
        d = os.path.join(out, sub); os.makedirs(d, exist_ok=True)
        for n in lst:
            nm = re.sub(r'[^A-Za-z0-9_]+', '_', n.split("/")[-1])
            open(os.path.join(d, nm + ext), "wb").write(sz.read(n))
    dump("col_mesh", meshes, ".mesh")
    dump("phys_sebd", physs, ".sebd")

    sebd_ok = sum(1 for n in physs if sz.read(n)[:4] == b"SEBD")
    summary = ("env: %s\ncollider entities (layer data): %d\ncol_mesh geometry files: %d\nphys SEBD blobs: %d (%d valid SEBD)\n"
               % (env, len(layers), len(meshes), len(physs), sebd_ok))
    if layers:
        pt = {}
        for L in layers:
            t = (L.get("PhysicsBodyPlatformComponent") or {}).get("type", "?")
            pt[t] = pt.get(t, 0) + 1
        summary += "PhysicsBody types: %s\n" % pt
    open(os.path.join(out, "SUMMARY.txt"), "w").write(summary)
    print(summary + "-> " + out)
    return 0

if __name__ == "__main__":
    sys.exit(main())
