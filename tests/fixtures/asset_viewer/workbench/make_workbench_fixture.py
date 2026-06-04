#!/usr/bin/env python3
# Minimal valid glTF 2.0 triangle, embedded base64 buffer, deterministic.
# Source verts use distinct nonzero Y and Z so a wrong/half axis swap can't pass.
import base64, json, struct, os
P  = [(0.0, 0.0, 0.0), (2.0, 0.0, 5.0), (0.0, 3.0, 7.0)]
UV = [(0.0, 0.0), (1.0, 0.0), (0.0, 1.0)]
IDX = [0, 1, 2]
pos = b"".join(struct.pack("<3f", *p) for p in P)
uv  = b"".join(struct.pack("<2f", *u) for u in UV)
idx = struct.pack("<3H", *IDX)
buf = pos + uv + idx
mn = lambda vs,n:[min(v[i] for v in vs) for i in range(n)]
mx = lambda vs,n:[max(v[i] for v in vs) for i in range(n)]
gltf = {
 "asset":{"version":"2.0","generator":"make_workbench_fixture"},
 "buffers":[{"byteLength":len(buf),"uri":"data:application/octet-stream;base64,"+base64.b64encode(buf).decode()}],
 "bufferViews":[
   {"buffer":0,"byteOffset":0,"byteLength":len(pos),"target":34962},
   {"buffer":0,"byteOffset":len(pos),"byteLength":len(uv),"target":34962},
   {"buffer":0,"byteOffset":len(pos)+len(uv),"byteLength":len(idx),"target":34963}],
 "accessors":[
   {"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":mn(P,3),"max":mx(P,3)},
   {"bufferView":1,"componentType":5126,"count":3,"type":"VEC2","min":mn(UV,2),"max":mx(UV,2)},
   {"bufferView":2,"componentType":5123,"count":3,"type":"SCALAR"}],
 "meshes":[{"primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1},"indices":2,"mode":4}]}],
 "nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0}
out=os.path.join(os.path.dirname(__file__),"unit_tri.gltf")
open(out,"w",encoding="utf-8").write(json.dumps(gltf,indent=1)); print("wrote",out)
