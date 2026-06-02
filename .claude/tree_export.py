import bpy, sys, math

SRC = r"C:\Users\Joe\Downloads\tree_small_02_1k.gltf\tree_small_02_1k.gltf"
OUT = r"A:\Games\mc2-model-override-recon\data\model_overrides\source\trees\tree_small.glb"
TARGET_TRIS = 12000   # runtime-sane: well under MC2 pools (vert 500k / tri 200k per instance)
SCALE = 20.0           # asset ~4.5u tall -> ~36u, roughly MC2 tree scale (tune via screenshot)

# fresh scene
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=SRC)

meshes = [o for o in bpy.context.scene.objects if o.type == 'MESH']
total_tris = 0
for o in meshes:
    o.data.calc_loop_triangles()
    total_tris += len(o.data.loop_triangles)
print("IMPORT total_tris=", total_tris, "objs=", len(meshes), flush=True)

ratio = max(0.002, min(1.0, TARGET_TRIS / float(total_tris)))
print("DECIMATE ratio=", ratio, flush=True)

for o in meshes:
    bpy.context.view_layer.objects.active = o
    m = o.modifiers.new("dec", 'DECIMATE')
    m.ratio = ratio
    bpy.ops.object.modifier_apply(modifier=m.name)

# leaf material -> alpha CLIP (MASK) @0.5, keep double-sided
for mat in bpy.data.materials:
    nm = (mat.name or "").lower()
    if "leaves" in nm or "leaf" in nm:
        mat.blend_method = 'CLIP'
        try: mat.alpha_threshold = 0.5
        except Exception: pass
        print("LEAF mat set CLIP:", mat.name, flush=True)

# join into one object named 'tree_small' (one node, name <=24 chars, importer-safe)
bpy.ops.object.select_all(action='DESELECT')
for o in meshes: o.select_set(True)
bpy.context.view_layer.objects.active = meshes[0]
bpy.ops.object.join()
obj = bpy.context.view_layer.objects.active
obj.name = "tree_small"
obj.data.name = "tree_small_mesh"

# bake scale into geometry so manifest scale stays 1.0
obj.scale = (SCALE, SCALE, SCALE)
bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)

obj.data.calc_loop_triangles()
print("FINAL tris=", len(obj.data.loop_triangles), "node=", obj.name, flush=True)

bpy.ops.export_scene.gltf(
    filepath=OUT, export_format='GLB',
    use_selection=False, export_apply=True,
    export_yup=True)
print("EXPORTED", OUT, flush=True)
