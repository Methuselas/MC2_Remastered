# MODEL-OVERRIDE — Full GPU Instancing of Static Props (proposal)

**Status:** READ-ONLY investigation + proposal. No code changed.
**Branch:** `claude/model-override-system-recon-1`
**Question:** Are MC2 static-prop instances fully GPU-instanced? If not, what
allocates per-instance CPU pool memory that scales with mesh×instances, and
what is the concrete path to make them fully GPU so a heavy (700k-tri) override
mesh renders on a whole forest at negligible per-instance pool cost?

All file:line references verified against the working tree on this branch.

---

## 0. TL;DR verdict

- **Geometry: YES, fully GPU.** Each *type* mesh is uploaded **once** into an
  immutable VBO/IBO at `finalizeGeometry()`, keyed by `TG_TypeShape*` in
  `s_typeIndex` (`gos_static_prop_batcher.cpp:372`, build at `:1953`). Instances
  are drawn from a per-instance SSBO record (transform + light index + fog +
  highlight) via the coalesce/indirect multidraw. **No per-instance vertex copy
  exists on the GPU draw path's geometry** — `submit()` stores only a 16-float
  matrix + a few scalars per instance (`:3917-3935`).

- **Lighting: YES, already GPU.** Per-instance lighting is a single
  `lightDataIndex` into an UNBOUNDED `LightsData light[]` SSBO (grow-on-demand,
  b41baec lifted the old UBO cap — earlier "[32]" wording here was STALE; see
  docs/model-override-lighting-lod-recon.md), consumed per-vertex by
  `static_prop.vert` `calc_light(... inst.lightDataIndex ...)`
  (`shaders/static_prop.vert:41, 314-316`). Normals/positions come from the
  immutable VBO transformed by `inst.modelMatrix`. **No per-vertex CPU lighting
  is needed for the lit GPU image.**

- **So what scales with mesh×instances?** Two distinct per-instance CPU costs,
  both proportional to the mesh's `numTypeVertices`/`numTypeTriangles`:

  1. **Per-frame TGL frame-pool allocation** (the 98% peak). On the GPU path,
     each *visible* instance still runs `TransformMultiShape_PositionsOnly`
     every frame (`bdactor.cpp:4541` trees, `:2343` bldgs), which
     **unconditionally allocates all six frame pools** sized to the mesh
     (`tgl.cpp:2766-2774`). Pools are bump allocators reset only at end of frame
     (`mission.cpp:899-903`), so **peak = Σ over all visible instances of
     numVertices/numTriangles**. This is retained-for-frame, not transient.
  2. **Per-instance retained shadow-vertex heap** (`tgl.cpp:427`):
     `CreateFrom` `Malloc`s `numTypeVertices * MAX_SHADOWS * sizeof(TG_ShadowVertex)`
     on the tglHeap for the *lifetime* of the instance (MAX_SHADOWS=1,
     `tgl.h:195`). Smaller per item than the frame pools but permanent.

- **Is the per-instance pool alloc NEEDED for the GPU draw?** **No — the
  *content* is vestigial, but the *allocation* is currently a hard precondition.**
  `submitMultiShape` skips any child whose `listOfVertices`/`listOfColors` is NULL
  (`gos_static_prop_batcher.cpp:4359`), and `submit()` copies a per-vertex ARGB
  block from `listOfVertices[v].argb` into `bucket.colors` (`:4127-4131`). But
  `_PositionsOnly` leaves that argb **stale/zero** (lighting kernel stripped,
  `tgl.cpp:2837, 2839-2844`), and the Colors SSBO is read by the shader **only
  for legacy debug modes** (`static_prop.vert:5`). The real lighting is the GPU
  `lightDataIndex` path. So the pools are allocated and filled with positions
  that the GPU draw never consumes — pure overhead kept alive by a non-null
  gate + a vestigial color copy.

**Conclusion:** static props are already GPU-instanced for geometry AND lighting.
The only thing forcing mesh×instance CPU/GPU-color memory is a legacy
per-instance `_PositionsOnly` transform whose output the GPU draw does not use.
Removing that dependency for registered (GPU-batched) types is the fix.

---

## 1. The GPU draw path (geometry once per type)

| Stage | Where | What |
|---|---|---|
| Register render shape | `bdactor.cpp` `BldgAppearance::registerStatic` (~`:2782`), `TreeAppearance::registerStatic` (~`:4779`) → `TransformMultiShape_BuildRecipe` | Hierarchy walk only; **explicitly skips all pool alloc** (`msl.cpp:1993` → flag → `msl.cpp:1923 continue`). Runs once at mission load. |
| Type geometry upload | `gos_static_prop_batcher.cpp` `registerMultiShape` (`:1905`) → `finalizeGeometry` (`:1953`) | Per *type* `TG_TypeShape` mesh copied **once** into immutable VBO/IBO; recorded in `s_typeIndex` (`:372, :1797`). flags=0 fully immutable (`:1975`). |
| Per-frame instance submit | `gos_static_prop_batcher.cpp` `submitMultiShape` (`:4151`) → `submit` (`:3858`) | Pushes one `GpuStaticPropInstance` = `modelMatrix[16]` + `typeID` + `lightDataIndex` + fog/highlight (`:3917-3935`). **No geometry per instance.** |
| Cull + draw | substrate `GpuActorRecord` (`:3971-4019`) → compute cull → coalesce multidraw (`[GPU_CULL] event=indirect_draw`) | One draw range per type; instance count from GPU-authoritative `bucketCountData[typeID]`. |

**Geometry instancing is real and complete.** A 700k-tri mesh occupies the VBO
**once**; 148 instances add 148 × ~80 bytes of instance record. That part already
scales O(1) in mesh size per instance.

---

## 2. The per-instance CPU pool path (the cost that scales)

### 2.1 The pools
`code/mission.cpp:3309-3322` allocates (note: raised from the 16M/8M in the
MVP notes to **32M verts/colors/shadows, 16M faces/triangles** — these are the
"artificial caps"):

```
colorPool    32,000,000   (TG_VertexPool)
vertexPool   32,000,000   (TG_GOSVertexPool)
facePool     16,000,000   (TG_DWORDPool)   // used twice per shape (faces+shadows)
shadowPool   32,000,000   (TG_ShadowPool)
trianglePool 16,000,000   (TG_TrianglePool)
```

Each is a **bump allocator** (`tgl.h:1210-1223`): `getVerticesFromPool` advances
`nextVertex` and bumps `numVertices`; returns NULL once `numVertices >=
totalVertices`. **Reset only once per frame**, at the END of render
(`mission.cpp:896-904`). Therefore **frame peak = Σ over every shape transformed
that frame of its vertex/triangle count** — fully retained within the frame.

### 2.2 The two allocation sites (both per visible instance per frame)
`tgl.cpp:1775-1783` (`MultiTransformShape`, full bake) and
`tgl.cpp:2766-2774` (`MultiTransformShape_PositionsOnly`, GPU path) — identical
six allocations sized to `numVertices`/`numTriangles`:

```
listOfVertices       = vertexPool   (numVertices)
listOfColors         = colorPool    (numVertices)
listOfShadowTVertices= shadowPool   (numVertices)
listOfTriangles      = trianglePool (numTriangles)
listOfVisibleFaces   = facePool     (numTriangles)
listOfVisibleShadows = facePool     (numTriangles)
```

### 2.3 Which path runs for a GPU-batched tree/bldg
Per visible instance, per frame, in the GPU-eligible branch:

- **Trees:** `bdactor.cpp:4532-4549` — `gpuEligible` → `TransformMultiShape_PositionsOnly` (`:4541`). If parity dual-emit armed, ALSO full `TransformMultiShape` (`:4547`) → *doubles* the alloc.
- **Buildings:** `bdactor.cpp:2329-2361` — identical shape (`_PositionsOnly` at `:2343`, dual-emit full at `:2360`).

So **even a fully GPU-instanced tree allocates `numVertices` colors + `numVertices`
positions + `numTriangles` triangles/faces from the frame pools, every frame, for
every on-screen instance.**

### 2.4 The retained per-instance shadow heap
`tgl.cpp:399 CreateFrom` (the per-instance clone made at
`bdactor.cpp:3837` tree / `:803` bldg) `Malloc`s
`sizeof(TG_ShadowVertex) * numTypeVertices * MAX_SHADOWS` on `tglHeap`
(`tgl.cpp:427`) and `memset`s it (`:430`). MAX_SHADOWS=1 (`tgl.h:195`). This is
**retained for the instance's lifetime**, separate from the frame pools. Smaller
per item but it is the other mesh×instance term.

### 2.5 Why the alloc is currently load-bearing for the GPU draw (but its content is not)
`submitMultiShape` enforces a hard precondition and a vestigial copy:

- **Non-null gate** — `gos_static_prop_batcher.cpp:4359`:
  `if (!child->listOfVertices || !child->listOfColors) { skip; }`. A NULL
  (pool-overflow) instance is silently dropped → **the vanishing trees / 98%
  peak symptom.**
- **Vestigial color copy** — `:4126-4131`: copies `listOfVertices[v].argb`
  (per vertex) into `bucket.colors`. But on the `_PositionsOnly` path that argb
  is **stale/zero** (`tgl.cpp:2837` memsets colors to 0; the lighting kernel that
  would write `.argb` is stripped, `:2839-2844`). And the Colors SSBO is read by
  the shader **only for legacy debug modes** (`static_prop.vert:5`).
- **Real lighting is GPU** — `static_prop.vert:314-316` lights per-vertex from
  `inst.lightDataIndex` into the unbounded `LightsData light[]` SSBO (NOT a 32-slot
  UBO — stale wording corrected per recon). `lightDataIndex` is gathered once
  per actor by `GatherGpuObjectLightDataOnly()` (`tgl.cpp:2910`,
  batcher `:4332-4338`) — **O(1) per instance, not per vertex.**

**Net:** the GPU draw needs `listOfVertices` only as a non-null pointer and
`bucket.colors` only as a (debug-only, currently zeroed) block. It does **not**
need the mesh-sized CPU vertex transform. The allocation is therefore removable
for registered types without changing the lit image.

---

## 3. Why raising the caps alone cannot reach a full forest (RAM math)

The user confirmed the pools are artificial caps and can be raised. They cannot
scale to a forest of a heavy mesh:

- 706k-tri lush tree ≈ **~350k vertices** per instance (tri≈2×vert for closed
  meshes; even at the importer's actual vert count the order holds).
- Per visible instance per frame the pools consume, sized to that mesh:
  `vertexPool + colorPool + shadowPool` each ≈ 350k entries,
  `trianglePool + 2×facePool` ≈ 706k entries.
- Byte cost per instance per frame (approx, from the pool element sizes):
  vertex pool `gos_VERTEX`=32B → ~11 MB; color/shadow pools similar order;
  triangle/face pools millions of entries. **Order ~30–50 MB of frame-pool
  traffic per visible instance.**
- A "forest" = O(100–1000) visible instances → **gigabytes per frame** of
  bump-allocator traffic against a single contiguous reset-per-frame arena.
- The arena is one flat allocation (`init(N)`); raising N to cover 1000 ×
  350k verts = 3.5e8 entries × 32B ≈ **11 GB for the vertex pool alone**, ×5
  pools. Infeasible, and it would be re-`memset`/touched every frame.

Even the current 32M vertex cap overflows at ~90 instances of the 350k-vert
mesh (32M / 350k ≈ 91), which matches the observed "148 instances peaks ~98%
then trees drop." **Raising caps buys a few more instances linearly; GPU
instancing makes the per-instance pool cost ~0 regardless of mesh size.**

---

## 4. The path to full GPU (skip per-instance pool work for registered types)

The geometry and lighting are already GPU. The remaining work is to **stop
running the per-instance mesh-sized CPU transform for instances whose type is
registered in the batcher**, and to satisfy `submit`'s two soft dependencies
without it.

### Candidate A — Skip `_PositionsOnly` for registered GPU types; submit directly *(RECOMMENDED)*
**Change:**
1. In the GPU-eligible branch (`bdactor.cpp:4532` trees, `:2329` bldgs), when the
   type is registered in the batcher, **do not call
   `TransformMultiShape_PositionsOnly`**. Run only the hierarchy walk needed for
   per-leaf `shapeToWorld` — that already exists as
   `TransformMultiShape_HierarchyOnly` / the `_BuildRecipe` mode
   (`msl.cpp:2010` / `:1993`, both hit the `s_buildRecipeOnly continue` at
   `msl.cpp:1923` and allocate **zero pools**).
2. Relax `submitMultiShape`'s non-null gate (`gos_static_prop_batcher.cpp:4359`)
   so a registered child with NULL `listOfVertices`/`listOfColors` is **admitted,
   not skipped** — its transform comes from `rec.shapeToWorld` (already populated
   by the hierarchy walk, used at `:4391`), not from `listOfVertices`.
3. In `submit` (`:4126-4137`), when `listOfVertices` is NULL, take the existing
   zero-pad branch (`:4136 bucket.colors.insert(..., 0u)`) — already present.
   The shader ignores the Colors SSBO outside debug modes
   (`static_prop.vert:5`), so a zeroed block is correct for the lit path.

**What breaks / must be checked:**
- **PerPolySelect / picking** (`tglpp.cpp`, referenced by the `:2764` comment):
  per-poly mouse picking reads `listOfVertices`. Static props use
  rectangle/screen-space object picking (cf. the mech `findMoverByMouse`
  rationale at `msl.cpp:2005-2008`); confirm static-prop selection does not rely
  on per-poly pick. If it does, gate the skip behind "not currently the
  pick-hovered actor," or fall back to a full transform only for the single
  hovered instance.
- **Parity dual-emit** (`bdactor.cpp:4547/2360`): only fires for the sampled
  actor under `IsDualEmitArmedForActor`; leave that one actor on the full path
  (negligible — one instance). Otherwise it would re-introduce the alloc.
- **Shadows:** `MultiTransformShadows` (`msl.cpp:1953`) and `RenderShadows` read
  per-instance shadow buffers. Static-prop shadow casting is a separate concern;
  verify the GPU shadow path (caster feed) does not depend on the per-frame
  `listOfShadowTVertices` pool block. If it does, restrict the skip to the
  non-shadow population first.
- **Collision/bounds:** unaffected — collision reads the **stock type** shape
  (dual-shape, diff-proven in Slice 2); bounds (`OBBRadius`/`highZ`) are computed
  at init from the (per-instance render) shape's type metadata, not from the
  per-frame pool fill.

**Effort:** medium (3 edit sites + a gate relax + picking audit).
**Risk:** medium — the picking/shadow audits are the real risk; geometry+lighting
are provably independent of the skipped data.
**Payoff:** per-instance frame-pool cost → **0** for registered types. A 700k-tri
mesh costs one VBO upload + 80 bytes/instance/frame. Forest-scalable.

### Candidate B — Move the (already-GPU) lighting note to its logical end: delete the per-vertex Colors copy
Independent cleanup that de-risks A: since the Colors SSBO is debug-only
(`static_prop.vert:5`) and `_PositionsOnly` zeroes it anyway, the per-vertex
`bucket.colors.push_back` loop (`:4127-4131`) can be replaced by the zero-pad
(`:4136`) unconditionally for the lit path, removing the last reason `submit`
touches `listOfVertices`. Effort: small. Risk: low (only legacy debug
visualization changes). Best landed *with* A so the non-null gate relax is safe.

### Candidate C — Free / shrink the retained shadow heap for registered types
`CreateFrom`'s `numTypeVertices*MAX_SHADOWS` shadow Malloc (`tgl.cpp:427`) is
retained per instance. For registered GPU types whose shadows are GPU-fed, this
buffer is unused on the draw path. Option: skip the shadow Malloc in `CreateFrom`
when the type is batcher-registered, or lazily allocate only if the legacy
shadow path runs. Effort: small–medium. Risk: medium (must confirm no legacy
shadow consumer). Smaller win than A but removes the only *retained* mesh×instance
term.

### Candidate D — Eliminate the per-instance `TG_Shape` clone entirely *(future)*
The endgame: registered types need only `{typeID, shapeToWorld, lightDataIndex}`
per instance — no `TG_Shape`/`CreateFrom` at all. The per-instance shape exists
today only to (a) carry `shapeToWorld` and (b) gate submit. Both can be sourced
from the recipe/registry record. This deletes both the frame-pool churn (A) and
the retained shadow heap (C) at once, but is a larger refactor of the appearance
update loop. Defer until A+B+C prove the data-flow assumptions.

---

## 5. Recommended slice

**Slice: GPU-INSTANCE-SKIP-POOLS-1 — skip per-instance `_PositionsOnly` for
registered types.** (Candidate A + B together.)

**Steps:**
1. Land B first (zero-pad the Colors block unconditionally for the lit path;
   `gos_static_prop_batcher.cpp:4126-4137`). Verify no visual change with caps
   unchanged.
2. Relax the non-null gate at `gos_static_prop_batcher.cpp:4359` to admit
   registered children with NULL `listOfVertices` (source transform from
   `rec.shapeToWorld`).
3. In `bdactor.cpp` GPU-eligible branches (`:4532` tree, `:2329` bldg), replace
   `TransformMultiShape_PositionsOnly` with the hierarchy-only walk
   (`TransformMultiShape_HierarchyOnly`, `msl.cpp:2010`) for instances whose type
   is in `s_typeIndex`. Keep the dual-emit full-bake only for the parity-sampled
   actor.
4. Audit static-prop picking (`tglpp.cpp` / `findObjectByMouse`) and the GPU
   shadow caster feed for any dependence on the skipped per-frame buffers; gate
   the skip narrower if found.

**Verification:**
- Override a dead-tree type with the 706k-tri lush mesh on its full instance
  count (the 148-instance case). Run `--validate --frames 30 -mission mc2_01`.
- **Pass criteria:** all override-tree instances render (no vanishing), exit 0,
  `gl_errors=[]`; and the TGL pool peak (`vertexPool/facePool peakUsedThisMission`,
  dumped at `tgl.cpp:3913-3922`) stays **low and flat** — proportional to the
  *non-registered* (legacy CPU) population only, NOT to override mesh×instances.
- A/B the pool peak: before the slice ≈ 98% of 32M; after ≈ a few %.
- Confirm lit appearance unchanged on stock props (lighting still flows through
  `lightDataIndex`), and confirm picking/selection of a static prop still works.

**Interim (until the slice lands):** the caps are artificial and can be raised
(`mission.cpp:3309-3322`), buying ~`cap/meshVerts` instances linearly — but §3
shows this cannot reach a forest of a heavy mesh (gigabytes/frame). GPU
instancing (this slice) is the real fix; cap-raising is only a stopgap for a
handful of instances.

---

## 6. Evidence index (file:line)

- Pools defined / sized: `code/mission.cpp:3309-3322`; reset per frame `:896-904`.
- Bump allocator + NULL-on-full: `mclib/tgl.h:1210-1223` (verts; siblings follow).
- Pool alloc sites: `mclib/tgl.cpp:1775-1783` (full), `:2766-2774` (`_PositionsOnly`).
- `_PositionsOnly` strips lighting / zeroes color: `mclib/tgl.cpp:2837, 2839-2844`.
- GPU-eligible per-instance dispatch: `mclib/bdactor.cpp:4532-4549` (tree), `:2329-2361` (bldg).
- `_BuildRecipe`/`_HierarchyOnly` skip pools: `mclib/msl.cpp:1923-1924, 1993, 2010`.
- Per-instance clone + retained shadow heap: `mclib/tgl.cpp:399, 427, 430`; MAX_SHADOWS `mclib/tgl.h:195`.
- Immutable per-type geometry: `GameOS/gameos/gos_static_prop_batcher.cpp:372, 1797, 1953, 1975`.
- Instance record (no geometry): `:3858, 3917-3935`.
- submit non-null gate / color copy: `:4359, 4126-4137`.
- GPU per-instance light index: `mclib/tgl.cpp:2910`; batcher `:4332-4338`; shader `shaders/static_prop.vert:5, 41, 314-316`.
