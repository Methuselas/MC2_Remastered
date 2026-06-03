import bpy, sys, math, random

SRC = r"C:\Users\Joe\Downloads\tree_small_02_1k.gltf\tree_small_02_1k.gltf"
OUT = r"A:\Games\mc2-model-override-recon\data\model_overrides\source\trees\tree_lush.glb"

# Leaf-card-PRESERVING reduction:
#  - trunk + branches kept FULLY intact (they're cheap: ~28k + ~94k tris)
#  - leaves: each leaf CARD is a loose-part island (a quad/few quads). We KEEP
#    whole cards intact (so each still looks like a leaf) and DELETE a fraction
#    of whole cards at random to hit a tri budget. NO collapse-decimate on leaves.
LEAF_KEEP = float(sys.argv[sys.argv.index('--keep')+1]) if '--keep' in sys.argv else 0.20
SCALE = float(sys.argv[sys.argv.index('--scale')+1]) if '--scale' in sys.argv else 32.0

print(f"PARAMS LEAF_KEEP={LEAF_KEEP} SCALE={SCALE}", flush=True)

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=SRC)

meshes = [o for o in bpy.context.scene.objects if o.type == 'MESH']
print("IMPORT objs=", len(meshes), flush=True)

# After gltf import, one object with 3 material slots. Separate by material so we
# can operate on the leaf faces only.
obj = meshes[0]
bpy.context.view_layer.objects.active = obj
bpy.ops.object.select_all(action='DESELECT')
obj.select_set(True)
bpy.ops.object.mode_set(mode='EDIT')
bpy.ops.mesh.separate(type='MATERIAL')
bpy.ops.object.mode_set(mode='OBJECT')

parts = [o for o in bpy.context.scene.objects if o.type == 'MESH']
for o in parts:
    o.data.calc_loop_triangles()
    matname = o.material_slots[0].name if o.material_slots else '?'
    print(f"PART {o.name!r} mat={matname!r} tris={len(o.data.loop_triangles)}", flush=True)

def is_leaf(o):
    nm = (o.material_slots[0].name if o.material_slots else '').lower()
    return 'leaf' in nm or 'leaves' in nm

leaf_objs = [o for o in parts if is_leaf(o)]
keep_objs = [o for o in parts if not is_leaf(o)]

# Reduce leaves by deleting whole loose-part islands (cards), not collapsing.
import bmesh
for lo in leaf_objs:
    bpy.context.view_layer.objects.active = lo
    bpy.ops.object.select_all(action='DESELECT')
    lo.select_set(True)
    # Split into loose parts to identify cards
    bpy.ops.object.mode_set(mode='EDIT')
    bm = bmesh.from_edit_mesh(lo.data)
    # group faces into islands via linked
    bpy.ops.mesh.select_all(action='DESELECT')
    bpy.ops.object.mode_set(mode='OBJECT')

    # Build islands using face-vertex connectivity
    me = lo.data
    nf = len(me.polygons)
    # union-find over faces sharing verts
    parent = list(range(nf))
    def find(a):
        while parent[a]!=a:
            parent[a]=parent[parent[a]]; a=parent[a]
        return a
    def union(a,b):
        ra,rb=find(a),find(b)
        if ra!=rb: parent[ra]=rb
    vert_to_face={}
    for fi,poly in enumerate(me.polygons):
        for vi in poly.vertices:
            if vi in vert_to_face:
                union(fi, vert_to_face[vi])
            else:
                vert_to_face[vi]=fi
    islands={}
    for fi in range(nf):
        islands.setdefault(find(fi),[]).append(fi)
    island_list=list(islands.values())
    print(f"LEAF islands={len(island_list)} faces={nf}", flush=True)

    random.seed(1234)
    random.shuffle(island_list)
    nkeep=int(len(island_list)*LEAF_KEEP)
    delete_islands=island_list[nkeep:]
    del_faces=set()
    for isl in delete_islands:
        del_faces.update(isl)

    bpy.ops.object.mode_set(mode='EDIT')
    bm = bmesh.from_edit_mesh(lo.data)
    bm.faces.ensure_lookup_table()
    bpy.ops.mesh.select_all(action='DESELECT')
    for fi in del_faces:
        bm.faces[fi].select=True
    bmesh.update_edit_mesh(lo.data)
    bpy.ops.mesh.delete(type='FACE')
    bpy.ops.object.mode_set(mode='OBJECT')
    lo.data.calc_loop_triangles()
    print(f"LEAF kept {nkeep}/{len(island_list)} cards -> tris={len(lo.data.loop_triangles)}", flush=True)

# leaf material -> alpha CLIP
for mat in bpy.data.materials:
    nm=(mat.name or "").lower()
    if "leaf" in nm or "leaves" in nm:
        mat.blend_method='CLIP'
        try: mat.alpha_threshold=0.5
        except Exception: pass

# join all parts back into one object 'tree_small'
allparts=[o for o in bpy.context.scene.objects if o.type=='MESH']
bpy.ops.object.select_all(action='DESELECT')
for o in allparts: o.select_set(True)
bpy.context.view_layer.objects.active=allparts[0]
bpy.ops.object.join()
obj=bpy.context.view_layer.objects.active
obj.name="tree_small"
obj.data.name="tree_small_mesh"

obj.scale=(SCALE,SCALE,SCALE)
bpy.ops.object.transform_apply(location=False,rotation=False,scale=True)

obj.data.calc_loop_triangles()
print("FINAL tris=", len(obj.data.loop_triangles), "node=", obj.name, flush=True)

bpy.ops.export_scene.gltf(filepath=OUT, export_format='GLB',
    use_selection=False, export_apply=True, export_yup=True)
print("EXPORTED", OUT, flush=True)
