import bpy
OUT = r"A:\Games\mc2-model-override-recon\data\model_overrides\source\props\bigbox.glb"
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.mesh.primitive_cube_add(size=12.0)   # 12-unit cube, clearly visible vs a hangar
obj = bpy.context.active_object
obj.name = "bigbox"
obj.data.name = "bigbox_mesh"
# bright unlit-ish material so it stands out
mat = bpy.data.materials.new("bigbox_mat")
mat.use_nodes = True
bsdf = mat.node_tree.nodes.get("Principled BSDF")
if bsdf: bsdf.inputs["Base Color"].default_value = (1.0, 0.1, 0.8, 1.0)  # magenta
obj.data.materials.append(mat)
bpy.ops.export_scene.gltf(filepath=OUT, export_format='GLB', use_selection=False, export_apply=True, export_yup=True)
print("EXPORTED", OUT)
