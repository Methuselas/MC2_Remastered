"""bake_tree_impostor.py — Blender headless: bake a tree canopy to a 2-card
cross impostor (far-LOD) for the modern-tree-pack.

Ported from the FOLIAGE-IMPOSTOR-PIPELINE-MVP-1 prior art (tree_export_impostor.py).
Renders an ortho front view of the source tree to an alpha-cutout TGA, then builds
a 2-quad cross mesh sized to the tree bounds. The cross collapses dozens of
overlapping leaf cards to 2 quads (kills alpha-card overdraw + the sub-pixel
"dead branches" look at distance) — the real far-LOD.

Convention: exports the cross in the SAME mesh-local frame the engine importer
expects (the cross is built directly in the axisMap0 target frame so it renders
upright + correctly scaled alongside the normalized LOD0, WITHOUT going through
normalize_broadleaf_glb.py). The baked TGA goes straight to the mod's tgl dir so
LoadOverrideRenderShapeTextures resolves it by name (a_ prefix => alpha).

Run:
  "C:/Program Files/Blender Foundation/Blender 5.1/blender.exe" --background \
     --python tools/examples/bake_tree_impostor.py -- \
     --src <source.glb> --tex-name a_poplar_impostor \
     --tex-dir <mod>/data/tgl/128 --out-glb <mod>/.../cooked/pine1_impostor.glb \
     --scale 0.5 --res 512
"""
import bpy, sys, math, argparse
from mathutils import Vector


def parse_args():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True)
    ap.add_argument("--tex-name", required=True, help="e.g. a_poplar_impostor (a_ => alpha)")
    ap.add_argument("--tex-dir", required=True, help="mod data/tgl/<size> dir for the baked TGA")
    ap.add_argument("--out-glb", required=True)
    ap.add_argument("--scale", type=float, default=0.5, help="match the LOD0 TREE_SCALE")
    ap.add_argument("--res", type=int, default=512)
    return ap.parse_args(argv)


def log(*a): print("[IMPOSTOR]", *a, flush=True)


def main():
    args = parse_args()
    import os
    os.makedirs(args.tex_dir, exist_ok=True)
    os.makedirs(os.path.dirname(args.out_glb), exist_ok=True)
    tex_path = os.path.join(args.tex_dir, args.tex_name + ".tga")

    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=args.src)
    meshes = [o for o in bpy.context.scene.objects if o.type == 'MESH']
    log("imported meshes", len(meshes))
    bpy.context.view_layer.update()

    # Combined world-space bbox (node transforms applied via matrix_world).
    mn = Vector((1e18, 1e18, 1e18)); mx = Vector((-1e18, -1e18, -1e18))
    for o in meshes:
        for c in o.bound_box:
            w = o.matrix_world @ Vector(c)
            mn.x = min(mn.x, w.x); mn.y = min(mn.y, w.y); mn.z = min(mn.z, w.z)
            mx.x = max(mx.x, w.x); mx.y = max(mx.y, w.y); mx.z = max(mx.z, w.z)
    ctr = (mn + mx) * 0.5
    H = mx.z - mn.z
    W = max(mx.x - mn.x, mx.y - mn.y)
    log("bbox", mn[:], mx[:], "H", H, "W", W)

    # ---- Render: ortho front view, transparent film, alpha cutout ----
    scn = bpy.context.scene
    scn.render.engine = 'BLENDER_EEVEE'
    scn.view_settings.view_transform = 'Standard'      # true albedo, not AgX
    scn.view_settings.look = 'None'
    scn.render.film_transparent = True
    scn.render.resolution_x = args.res
    scn.render.resolution_y = args.res
    scn.render.image_settings.file_format = 'TARGA'
    scn.render.image_settings.color_mode = 'RGBA'
    scn.render.filepath = tex_path

    world = bpy.data.worlds.new("w"); scn.world = world
    world.use_nodes = True
    bg = world.node_tree.nodes.get("Background")
    if bg:
        bg.inputs[0].default_value = (1, 1, 1, 1); bg.inputs[1].default_value = 1.2

    cam_data = bpy.data.cameras.new("cam"); cam_data.type = 'ORTHO'
    cam_data.ortho_scale = max(W, H) * 1.04
    cam = bpy.data.objects.new("cam", cam_data); scn.collection.objects.link(cam)
    dist = (mx.y - mn.y) + W * 2 + 100.0
    cam.location = (ctr.x, mn.y - dist, ctr.z)
    cam.rotation_euler = (math.radians(90), 0, 0)      # look +Y (front), Z up in image
    scn.camera = cam
    bpy.ops.render.render(write_still=True)
    log("rendered impostor texture ->", tex_path)

    # ---- Build the 2-card cross, baked into the engine's mesh-local frame ----
    # The engine importer (assimp_importer.cpp) applies axisMap0 (X=-x,Y=-y,Z=z)
    # then GROUND=2 to mesh-local verts. We emit verts so that AFTER axisMap0 the
    # card is upright + base-grounded, matching normalize_broadleaf_glb.py's
    # 180-X-flipped LOD0. Source canopy is Blender Z-up (vertical=+Z, base=mn.z).
    # We want engine-up; the LOD0 path bakes (x,-z,y)*flip => here we emit the
    # cross directly in that target frame and scale by --scale.
    for o in list(bpy.context.scene.objects):
        if o.type == 'MESH':
            bpy.data.objects.remove(o, do_unlink=True)

    s = float(args.scale)
    hw = W * 0.5 * s
    # Blender vertical = Z (z0..z1). Emit so that after export (yup) + engine
    # axisMap0 the vertical lands on engine-up. Build the cross in a Y-up frame:
    # map source (x, z_vertical) -> (x, y_up). base at y=0, top at y=H.
    y0, y1 = 0.0, H * s
    cx = ctr.x * s
    cz = ctr.y * s   # depth axis from source Y
    verts = [
        # card A: spans X, vertical Y; at depth cz
        (cx - hw, y0, cz), (cx + hw, y0, cz), (cx + hw, y1, cz), (cx - hw, y1, cz),
        # card B: spans Z(depth), vertical Y; at lateral cx
        (cx, y0, cz - hw), (cx, y0, cz + hw), (cx, y1, cz + hw), (cx, y1, cz - hw),
    ]
    faces = [(0, 1, 2, 3), (4, 5, 6, 7)]
    me = bpy.data.meshes.new("impostor"); me.from_pydata(verts, [], faces); me.update()
    me.uv_layers.new(name="UVMap")
    uvl = me.uv_layers.active.data
    quad_uv = [(0, 0), (1, 0), (1, 1), (0, 1)]
    for fi in range(2):
        for vi in range(4):
            uvl[fi * 4 + vi].uv = quad_uv[vi]
    obj = bpy.data.objects.new(args.tex_name, me)
    bpy.context.scene.collection.objects.link(obj)

    # Material baseColor image NAMED <tex_name> so DeriveMC2TextureName yields
    # <tex_name>.tga (engine loads the deployed TGA from data/tgl; pixels here
    # are irrelevant — only the name drives binding).
    mat = bpy.data.materials.new(args.tex_name); mat.use_nodes = True
    mat.blend_method = 'CLIP'
    nt = mat.node_tree
    bsdf = nt.nodes.get("Principled BSDF")
    img = bpy.data.images.load(tex_path)
    img.name = args.tex_name
    tex = nt.nodes.new("ShaderNodeTexImage"); tex.image = img
    nt.links.new(tex.outputs["Color"], bsdf.inputs["Base Color"])
    nt.links.new(tex.outputs["Color"], bsdf.inputs["Emission Color"])
    bsdf.inputs["Emission Strength"].default_value = 1.0
    nt.links.new(tex.outputs["Alpha"], bsdf.inputs["Alpha"])
    obj.data.materials.append(mat)

    bpy.ops.object.select_all(action='DESELECT')
    obj.select_set(True); bpy.context.view_layer.objects.active = obj
    bpy.ops.export_scene.gltf(filepath=args.out_glb, export_format='GLB',
                              use_selection=True, export_apply=True, export_yup=False,
                              export_image_format='AUTO')
    log("exported impostor GLB ->", args.out_glb, "DONE")


main()
