# LightsData UBO -> SSBO Conversion - Recon (READ-ONLY)

- **Branch / worktree:** `claude/gpu-driven-rendering` @ HEAD `5bffaf3`
  (`git rev-parse HEAD`). NOTE: the dispatch prompt cited HEAD `2dca942`;
  that is the D2 LIGHTBRIDGE commit, now 3 commits back. Commits
  `158c229` (LIGHTBRIDGE recon), `2dca942` (D2 FNV/memcmp retire),
  then three water-only commits (`99a4c23`, `e925dfb`, `5bffaf3`) sit on
  top. **No light-path code changed since `2dca942`** - all light:line
  citations below grep-verified at `5bffaf3` this invocation and are
  unaffected by the water commits.
- **Scope:** convert the `LightsData` block from `std140 uniform { ObjectLights
  light[64]; }` to `std430 buffer { ObjectLights light[]; }` (SSBO, unbounded).
  Remove the 64-slot window. READ-ONLY recon, no code written.
- **Why (settled, not relitigated):** unblocks the static-lighting bake AND
  fixes a pre-existing latent ceiling - `lighting.hglsl:41-43` records
  mc2_17 already observed `maxIdx=57/64` combined mech+static; the game is
  one dense mission from silent light-index OOB corruption today,
  independent of any bake.

---

## Q1. Full consumer inventory (grep-verified @ `5bffaf3`)

### Shader side - every reader of `LightsData` / `light[]` / the slot constant

The block is declared ONCE and `#include`d everywhere:

- **`shaders/include/lighting.hglsl:39-55`** - the sole declaration:
  `layout (binding = LIGHT_DATA_ATTACHMENT_SLOT, std140) uniform LightsData {
  ObjectLights light[64]; };`. `LIGHT_DATA_ATTACHMENT_SLOT` is `#define`d
  to `0` at `lighting.hglsl:11`. The mirror `struct ObjectLights` is
  `lighting.hglsl:31-37`. The sole subscript site inside the header is
  `calc_light()` at `lighting.hglsl:206`: `ObjectLights ld =
  light[lights_index];` where `lights_index` is a plain `in int` parameter.

- **`shaders/static_prop.vert`** - `#include <include/lighting.hglsl>`
  (`:21`). Indexes via `calc_light(int(inst.lightDataIndex), ...)` (called
  from the lighting path; diagnostic-only direct subscript at `:306`
  `ObjectLights ld = light[int(inst.lightDataIndex)];` under
  `u_parityNumLightsDebugMode`). Pure integer subscript.

- **`shaders/mech.vert`** - `#include <include/lighting.hglsl>` (`:17`,
  with `#define MC2_STATIC_PROP_LIGHTING` at `:9` before the include).
  Indexes via `calc_light(int(inst.lightDataIndex), worldNormal,
  worldMC2, base)` at `mech.vert:162`. Pure integer subscript.

- **`shaders/gos_tex_vertex_lighted.vert`** - `#include
  <include/lighting.hglsl>` (`:5`). `uniform vec4 light_offset_;` (`:18`);
  `const int lights_index = int(light_offset_.x);` (`:79`); then
  `calc_light(lights_index, Normal, WorldPos, base_light)`.

- **`shaders/gos_tex_vertex_lighted.frag`** - `#include
  <include/lighting.hglsl>` (`:7`). `uniform vec4 light_offset_;` (`:13`);
  `const int lights_index = int(light_offset_.x);` (`:49`); then
  `calc_light(...)` (only in the `#else` of `ENABLE_VERTEX_LIGHTING`,
  which is force-on globally, so this branch is normally dead).

- **`shaders/static_prop.frag` / `shaders/mech.frag`** - DO NOT declare or
  read `LightsData`; lighting is computed in the VS and passed as a
  varying (`VertexLight` / `baseLight`). Confirmed by the Q1 cross-grep:
  zero `LightsData`/`light[`/`LIGHT_DATA_ATTACHMENT_SLOT` hits in either
  frag (grep over whole `shaders/` tree returned only the files above).

**Negative claim (shaders): no other reader.** Grep
`LightsData|LIGHT_DATA_ATTACHMENT_SLOT|light\[|lighting\.hglsl|light_offset_`
over the entire `shaders/` tree returns only: `lighting.hglsl`,
`static_prop.vert`, `mech.vert`, `gos_tex_vertex_lighted.{vert,frag}`.
No `.comp`, no shadow shader, no terrain shader reads it.

### C++ side - every create / bind / upload / binding-point site

All in `mclib/txmmgr.cpp` (the ONLY .cpp that touches the buffer object):

- **Alloc + create + bind (init):** `txmmgr.cpp:320`
  `lightData_ = new TG_HWLightsData[lightDataStructuresCapacity];`
  (`lightDataStructuresCapacity = 128` initial - grep `lightDataStructuresCapacity`
  init site); `:321` `lightDataBuffer_ =
  gos_CreateBuffer(gosBUFFER_TYPE::UNIFORM, gosBUFFER_USAGE::STATIC_DRAW,
  sizeof(TG_HWLightsData)*lightDataStructuresCapacity, 1, NULL);`; `:322`
  `gos_BindBufferBase(lightDataBuffer_, LIGHT_DATA_ATTACHMENT_SLOT);`.
- **Destroy:** `txmmgr.cpp:386-388` (`gos_DestroyBuffer`).
- **Per-frame upload + recreate-on-grow:** `txmmgr.cpp:1552-1570`.
  `gpu_buf_size = gos_GetBufferSizeBytes(...)` (`:1552`);
  `kGlslUboMinBytes = 64u*sizeof(TG_HWLightsData)` (`:1558`); `cpu_buf_size
  = max(lightDataStructuresCount*sizeof(...), kGlslUboMinBytes)`
  (`:1559-1561`); if `gpu_buf_size < cpu_buf_size` ->
  `gos_DestroyBuffer`+`gos_CreateBuffer(...UNIFORM...)`+`gos_BindBufferBase`
  (`:1563-1565`); else `gos_UpdateBuffer(lightDataBuffer_, lightData_, 0,
  cpu_buf_size)`+`gos_BindBufferBase` (`:1568-1569`). Commented-out
  redundant rebind at `:1593`.
- **Slot population (writers, not bindings):** `addLightDataStructure`
  `txmmgr.cpp:1146-1194` (FNV `:1153`, memcmp `:1159`, grow-by-128
  `:1170-1171`, append `lightData_[count]` then `rv=count; count++`
  `:1177-1179`); `addLightDataStructureWithPerActorColor`
  `txmmgr.cpp:1196-1264`; `peekLightSlot` reads `lightData_[idx]` `:1287`.
  These touch `lightData_` (CPU array) only; no GL binding.
- **Material-cache binding-point set:** `txmmgr.cpp:1364`
  `gos_SetRenderMaterialUniformBlockBindingPoint(mat, "LightsData",
  LIGHT_DATA_ATTACHMENT_SLOT);` - **legacy material-cache path only**
  (the `mat` at `:1352` is `gos_getRenderMaterial("gos_vertex_lighted"
  | "gos_tex_vertex_lighted")`). The static_prop / mech batchers do
  NOT use this; they `glsl_program::makeProgram` directly
  (`gos_static_prop_batcher.cpp:485`, `gos_mech_batcher.cpp:201`) and
  rely on the in-shader `layout(binding=0)` qualifier + the single
  `gos_BindBufferBase` in txmmgr.cpp. This split is load-bearing for Q2.
- **`LIGHT_DATA_ATTACHMENT_SLOT` C++ definition:** grep returns it used
  at `txmmgr.cpp:322/1364/1365/1565/1569/1593` and declared in
  `GameOS/include/gameos.hpp` / `mclib/txmmgr.h` region (grep
  `LIGHT_DATA_ATTACHMENT_SLOT` - the `#define`/const lives with the other
  attachment-slot constants; value `0`, matching `lighting.hglsl:11`).

**Negative claim (C++): no other reader.** Grep
`LightsData|light_offset_|LIGHT_DATA_ATTACHMENT_SLOT|lightDataBuffer_`
over `mclib/ code/ GameOS/` (excluding `.claude/`) -> 50 hits across
exactly 9 files: `mclib/{mech3d.cpp,tgl.h,txmmgr.cpp,txmmgr.h}`,
`GameOS/gameos/{gos_mech_batcher.cpp,gos_mech_batcher.h,gos_object_parity.h,
gos_static_prop_batcher.cpp}`, `GameOS/include/gameos.hpp`.
- `mech3d.cpp:4496` - sets a per-actor `lightDataIndex` (producer, writes
  the index, does not bind the buffer).
- `gos_mech_batcher.cpp:215,890-908` - `kUboLightSlotCap = 64u`
  diagnostic (`[MECHLIGHT v1] event=cache_full` when
  `lightDataIndex >= 64`). NOT a buffer binding - a soft telemetry guard
  that becomes obsolete when the window is unbounded (Q4).
- `gos_static_prop_batcher.cpp:2164` - comment only ("LightsData[32]"
  stale comment); the index pack at `:2166` is the producer.
- `gos_object_parity.h` - parity harness comment.
- No other site creates / binds / uploads the buffer. The buffer object
  is owned entirely by `txmmgr.cpp`.

---

## Q2. GameOS buffer API for SSBO - **THE central structural finding**

### There is NO `gosBUFFER_TYPE::STORAGE` - the gos buffer API cannot create an SSBO

`GameOS/include/gameos.hpp:2739-2744`:
```
enum class gosBUFFER_TYPE { VERTEX = 0, INDEX, UNIFORM, NUM_BUFFER_TYPES };
```
`getGLBufferType()` (`gameos_graphics.cpp:6303-6316`) maps only
VERTEX->`GL_ARRAY_BUFFER`, INDEX->`GL_ELEMENT_ARRAY_BUFFER`,
UNIFORM->`GL_UNIFORM_BUFFER`; `default` is `gosASSERT(0 && "unknows
buffer type")`. **`gos_CreateBuffer`/`gos_BindBufferBase`/`gos_UpdateBuffer`
physically cannot target `GL_SHADER_STORAGE_BUFFER`.** Conclusion:
`lightDataBuffer_` (today a `HGOSBUFFER` of type UNIFORM) cannot become
an SSBO by changing the `gosBUFFER_TYPE` argument - that enumerator does
not exist.

### How SSBOs are actually done in this worktree (the precedent to reuse)

Every existing SSBO bypasses the gos buffer API entirely and uses **raw
GL** (`glGenBuffers` + `glBindBuffer(GL_SHADER_STORAGE_BUFFER,...)` +
`glBindBufferBase(GL_SHADER_STORAGE_BUFFER, slot, id)` +
`glBufferData`/`glBufferSubData`). Grep-verified precedents in
`gameos_graphics.cpp`:
- GPU-driven terrain/recipe SSBOs: `glBindBufferBase(GL_SHADER_STORAGE_BUFFER,
  5, recipeBuf)` (`:2319`), `... 7, s_perCmdSsbo` (`:2321`),
  `s_perCmdSsbo` created at `:2100-2101` (`glGenBuffers`-style raw path,
  `glBufferData(GL_SHADER_STORAGE_BUFFER, ..., GL_DYNAMIC_DRAW)`),
  updated via `glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, ...)`
  (`:2268`).
- Mech/static recipe + thin-record SSBOs: `glBindBufferBase(
  GL_SHADER_STORAGE_BUFFER, 1, recipeSSBO)` (`:2666`),
  `glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 2, thinRecordSSBO,...)`
  (`:2689`), lighting SSBO at slot 2 (`:2958/3109`), solid/water mask
  SSBOs at 17/18/19 (`:2955-2956/3106-3107`).
- Static-prop and mech batchers bind their instance/bone/color SSBOs
  raw: `static_prop.vert:55-67` declares `Instances`(binding 0),
  `Colors`(1), `PerType`(2), `ParityOut`(3); `static_prop.frag:47`
  `PerDrawData`(4); `mech.vert:38/44` `InstanceBuffer`(0),
  `BoneBuffer`(1) - all bound via raw `glBindBufferBase(
  GL_SHADER_STORAGE_BUFFER, N, ...)` in the batchers.

**`LightsData`-as-SSBO MUST follow this raw-GL precedent**, NOT the gos
buffer API. Cite `s_perCmdSsbo` (`gameos_graphics.cpp:2100-2101,2267-2269`)
as the cleanest create+update pattern (glBufferData create, glBufferSubData
update, GL_DYNAMIC_DRAW). The conversion is therefore NOT
"change one enum + one qualifier" - it requires replacing the
`gos_CreateBuffer`/`gos_BindBufferBase`/`gos_UpdateBuffer` trio in
`txmmgr.cpp` (`:321-322`, `:1563-1569`) with the raw-GL SSBO equivalents
(or adding a `gosBUFFER_TYPE::STORAGE` enumerator + `getGLBufferType`
case + an `glShaderStorageBlockBinding`-aware bind path - a GameOS-layer
API addition; defer the API-shape decision to `mc2-gameos-expert`).

### Binding-point scheme + collision risk - **REAL, MUST be designed around**

UBO and SSBO binding points are **separate GL namespaces**:
`glBindBufferBase(GL_UNIFORM_BUFFER, 0, ...)` and
`glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, ...)` do NOT collide at
the GL level. **BUT** within `static_prop.vert` and `mech.vert`, SSBO
**binding point 0 is already occupied**:
- `static_prop.vert:55` `layout(std430, binding = 0) ... buffer Instances`
- `mech.vert:38` `layout(std430, binding=0) ... buffer InstanceBuffer`

If `LightsData` is declared `layout(std430, binding = 0) buffer`
(reusing the old `LIGHT_DATA_ATTACHMENT_SLOT=0`), it **collides with the
per-shader instance SSBO at SSBO binding 0** -> two storage blocks bound
to the same SSBO binding point in the same program = the instance data
and the light data alias; catastrophic silent corruption of BOTH static
and mech rendering. **`LightsData`-as-SSBO needs a NEW, unused SSBO
binding point**, not the recycled UBO slot 0. Grep of all
`layout(std430, binding=N)` across `shaders/*.{vert,comp,frag}` shows
SSBO bindings 0-19 are in active use across the GPU-driven pipeline; the
plan must pick a binding free in EVERY program that includes
`lighting.hglsl` (static_prop, mech, gos_tex_vertex_lighted) AND free in
the compute/terrain dispatch set if any of those shaders ever co-bind it
(they do not include lighting.hglsl - verified Q1 - so the constraint is
just the 3 lighted programs, but pick a high unused index e.g. >=20 to
be safe and defer the exact number to `mc2-gameos-expert`).
`LIGHT_DATA_ATTACHMENT_SLOT` must be repurposed/renamed to the new SSBO
slot value and threaded to BOTH the `lighting.hglsl:11` `#define` AND
every C++ `glBindBufferBase(GL_SHADER_STORAGE_BUFFER, NEW_SLOT, ...)`
site in lockstep.

---

## Q3. std140 -> std430 layout lockstep - **THE highest-risk item: VERDICT**

### The struct (grep-verified `mclib/tgl.h:282,304-322`)

`#define MAX_HW_LIGHTS_IN_WORLD 16` (`tgl.h:282`). The GLSL mirror uses
`#define MAX_LIGHTS_IN_WORLD 16` (`lighting.hglsl:19`) - same value.

```c
struct TG_HWLightsData {                    // tgl.h:304
    float lightToWorld[16][16];   // 16 mat4   = 1024 B,  off    0
    float lightDir    [16][4];    // 16 vec4   =  256 B,  off 1024
    float lightColor  [16][4];    // 16 vec4   =  256 B,  off 1280
    float lightFalloff[16][4];    // 16 vec4   =  256 B,  off 1536
    int   numLights_;             //              4 B,    off 1792
    int   pad[3];                 //             12 B,    off 1796
};                                // sizeof = 1808 B
```
The inline layout comment at `tgl.h:291-296` documents exactly these
offsets. GLSL `struct ObjectLights` (`lighting.hglsl:31-37`):
`mat4 light_to_world[16]; vec4 light_dir[16]; vec4 light_color[16];
vec4 light_falloff[16]; ivec4 numLights;`.

### VERDICT: **BIT-IDENTICAL. std430 == std140 for THIS struct. No repack needed.**

Reasoning (the std140-vs-std430 rule from `cpp_glsl_ubo_struct_lockstep.md`:
std140 rounds array-element stride UP to a vec4 (16 B); std430 packs
scalars/vec2 more tightly - but **only** for members that are NOT already
vec4-aligned). Member-by-member:

- `mat4 light_to_world[16]`: a `mat4` is 4x `vec4`. Array-of-mat4 stride
  is 64 B in BOTH std140 and std430 (mat4 is already a multiple of 16).
  Identical.
- `vec4 light_dir[16]` / `light_color[16]` / `light_falloff[16]`: array
  of `vec4`. std140 array stride = 16 B (rounded up to vec4); std430
  array stride for `vec4` = 16 B (vec4 is already 16). Identical.
- `ivec4 numLights`: a 16-B vector in both layouts; the C++ side spends
  `int numLights_ + int pad[3]` = exactly 16 B at the same offset
  (1792). The `pad[3]` exists specifically so the C++ tail matches the
  `ivec4` GLSL tail. Identical.

There is **no scalar, vec2, vec3, or sub-16-B member anywhere** - every
member is a vec4 or an array of vec4 / mat4, all of which have identical
stride and offset under std140 and std430. The std430 "tighter packing"
divergence only bites scalars/vec2/vec3 and arrays thereof; this struct
has none. Total size 1808 B is a multiple of 16 in both layouts.

**Conclusion: the conversion does NOT require any C++ struct repack or
explicit padding change.** This downgrades the highest-theoretical-risk
item (the mc2_24-class lockstep regression) to **NOT APPLICABLE for the
layout itself** - the `cpp_glsl_ubo_struct_lockstep.md` hazard is about
the array-element-count / member-set being changed without the GLSL
mirror; it does NOT fire on a pure std140->std430 *qualifier* change for
an all-vec4/mat4 struct. The lockstep rule still applies in spirit: the
qualifier change in `lighting.hglsl:39` MUST be a single atomic commit
and the parity probe (Q6) must still prove byte-equality empirically
(verdict is a layout-rules derivation, not an observed RenderDoc dump).

**Residual caveat:** std430 *can* differ from std140 if a future edit
adds a scalar/vec3 member; if this conversion is bundled with ANY
`ObjectLights` field addition, the verdict is void and the full lockstep
repack discipline re-applies. Keep the SSBO conversion a *pure qualifier
+ binding* change with zero struct edits in the same commit.

---

## Q4. Unbounded array + indexing

- **Index path is a plain integer subscript - works unchanged on an
  unbounded SSBO array.** `lighting.hglsl:206` `ObjectLights ld =
  light[lights_index];` (`lights_index` an `in int`). Callers:
  `static_prop.vert` `calc_light(int(inst.lightDataIndex),...)` (and the
  diagnostic `light[int(inst.lightDataIndex)]` at `:306`),
  `mech.vert:162` `calc_light(int(inst.lightDataIndex),...)`,
  `gos_tex_vertex_lighted.{vert:79,frag:49}` `int(light_offset_.x)`.
  `inst.lightDataIndex` is packed at `gos_static_prop_batcher.cpp:2166`
  (from `submit(...,lightDataIndex)`); for mechs from
  `mech3d.cpp:4496`. None of these is a bounds-checked or
  `light[64]`-literal access. GLSL runtime-sized SSBO array
  (`buffer { ObjectLights light[]; }`) supports identical integer
  subscript with no syntax change at the call sites.
- **The only `64` literals that must change:**
  - `lighting.hglsl:54` `ObjectLights light[64];` -> `ObjectLights
    light[];` (the array bound is the conversion itself).
  - `gos_mech_batcher.cpp:897` `const uint32_t kUboLightSlotCap = 64u;`
    and the `>= kUboLightSlotCap` overflow telemetry at `:902-908`
    (`[MECHLIGHT v1] event=cache_full`). This is the *latent ceiling
    fix*: with an unbounded SSBO the cap is gone, so the diagnostic
    should be removed or repurposed (e.g. report distinct-slot count for
    sizing the static bake) - it must NOT keep asserting a 64 cap that
    no longer exists, or it will spam false `cache_full` for exactly the
    dense missions this conversion is meant to support.
  - `txmmgr.cpp:1558` `kGlslUboMinBytes = 64u*sizeof(TG_HWLightsData)`
    (the upload floor; see Q5 - the SSBO has no fixed-window
    GL-undefined-OOB requirement, but keeping a sane floor is harmless
    and the recreate path still needs `cpu_buf_size` correct).
  - Stale comments: `gos_static_prop_batcher.cpp:2164`
    "LightsData[32]", `static_prop.vert:39,227` "LightsData[32]" - cosmetic, fix in
    the same commit for grep-honesty.
- **GL version: SSBO + std430 needs GLSL 4.30 - CONFIRMED present.**
  Every program that includes `lighting.hglsl` is compiled with
  `"#version 430\n"`: `gos_static_prop_batcher.cpp:480-482`
  (`kShaderPrefixLegacy = "#version 430\n"`, `makeProgram("static_prop",
  ...)` `:485`), `gos_mech_batcher.cpp:201-202`
  (`makeProgram("mech",...,"#version 430\n")`). The legacy
  `gos_tex_vertex_lighted` material is loaded via `gosRenderMaterial`;
  its prefix is `"#version 430\n"` too (`gameos_graphics.cpp:210`
  `defines_str = "#version 430\n"` in the material-load path). The
  worktree CLAUDE.md confirms 4.3 context (`#version 430` rule).
- **AMD vertex-shader SSBO read (RX 7900 XTX) - safe precedent exists.**
  `static_prop.vert` and `mech.vert` ALREADY read multiple std430 SSBOs
  in the vertex stage (`Instances`/`InstanceBuffer` binding 0,
  `Colors`/`BoneBuffer` 1, `PerType` 2) on this exact GPU in the shipped
  GPU-driven path. `docs/amd-driver-rules.md` lists six RX 7900 XTX
  driver rules; **none restricts vertex-stage SSBO reads** (the rules are
  attribute-0, gl_FragDepth, depth-only color attachment, feedback
  loops, matrix transpose, deferred-uniform ordering). Vertex SSBO read
  is already in production here, so a new read-only `LightsData` SSBO in
  the same VS is AMD-safe by existing precedent. No new AMD rule applies.

---

## Q5. Upload path

- **Same GL upload primitives work** - an SSBO is still a GL buffer.
  `glBufferData`/`glBufferSubData` on `GL_SHADER_STORAGE_BUFFER` is the
  established pattern (`s_perCmdSsbo` create `gameos_graphics.cpp:2101`
  `glBufferData(GL_SHADER_STORAGE_BUFFER,...,GL_DYNAMIC_DRAW)`, update
  `:2268` `glBufferSubData(GL_SHADER_STORAGE_BUFFER,0,...)`). The current
  `gos_UpdateBuffer` (`gameos_graphics.cpp:6383-6391`) is a
  full-orphan `glBufferData(GL_DYNAMIC_DRAW)` with `offset` declared but
  IGNORED - that exact behavior is reproducible against
  `GL_SHADER_STORAGE_BUFFER`. Because the gos API can't target SSBO
  (Q2), the txmmgr upload trio (`:1563-1569`) must be replaced with raw
  `glBindBuffer(GL_SHADER_STORAGE_BUFFER,id)` +
  `glBufferData(...,cpu_buf_size,lightData_,GL_DYNAMIC_DRAW)` +
  `glBindBufferBase(GL_SHADER_STORAGE_BUFFER, NEW_SLOT, id)` (or the new
  gosBUFFER_TYPE::STORAGE path if that API is added).
- **Removing the 64 cap DOES make the recreate-on-grow path actually
  fire.** Today `cpu_buf_size` is floored to `64*sizeof` (`:1558-1561`)
  and the initial buffer is `128*sizeof` (`:321`), so
  `gpu_buf_size(=128*sizeof) < cpu_buf_size` is **never true until
  >128 distinct slots** - the recreate branch (`:1563-1565`) is
  effectively dead today (distinct count observed <=57). After the cap
  removal, dense missions can legitimately produce >128 distinct
  `TG_HWLightsData` (the whole point of the conversion), so the
  destroy+recreate+rebind path will fire for the first time in anger.
  **This is a real new code-exercise risk:** `gos_DestroyBuffer` of an
  SSBO + recreate must re-`glBindBufferBase(GL_SHADER_STORAGE_BUFFER,
  NEW_SLOT,...)` (the current `:1565` rebinds via gos to UBO slot 0; the
  raw-SSBO equivalent must rebind the SSBO slot). The grow-by-128
  `addLightDataStructure` realloc (`:1170-1171`) also now actually grows
  the CPU array past 128 - previously dead, now live; verify the
  `memcpy(new, old, count*sizeof)` (`:1171`) and `count`-based bounds
  are correct for `count > 128` (they appear correct by inspection but
  were never exercised).
- **Per-frame size implication:** with the cap gone, `cpu_buf_size`
  scales with distinct-light count (could be hundreds of x1808 B). The
  per-frame whole-buffer `glBufferData` orphan now memcpys a larger
  region; still O(distinct-lights), bounded, and the substitutive-win
  framing (Q6) makes the per-frame *recompute* the lever, not the
  upload. A `glBufferSubData` sub-range upload is a deferred follow-on,
  not required here.

---

## Q6. Parity + substitutive framing + biggest risk

### Substitutive framing (honest, per `feedback_offload_must_be_substitutive_not_additive.md`)

The SSBO conversion is **enabling-infrastructure / "remove the wall,"
NOT a substitutive offload slice.** By itself it RETIRES no CPU zone:
the per-frame `CacheGpuLightData -> GatherGpuObjectLightDataOnly ->
addLightDataStructure*` chain still runs identically; only the storage
class of the destination buffer changes. Its value is (a) a
correctness/ceiling fix - removes the silent `maxIdx>=64` OOB
corruption mc2_17 is already one dense mission away from
(`lighting.hglsl:41-43`), and (b) an enabler - it unblocks the
static-lighting persistent-partition bake (that bake is the follow-on
*substitutive* slice; it is the thing that takes the static-class CPU
chain to zero). Frame it exactly so in the plan: "SSBO conversion =
remove the 64-slot wall + the latent OOB hazard; the static bake on top
is the substitutive win." Do NOT claim a frame-time gain for the
conversion itself; the parity gate must show ZERO behavior change (it is
a pure storage-class refactor).

### Single biggest risk - RANKED

1. **HIGHEST: SSBO binding-point collision with the per-shader instance
   SSBO at binding 0** (Q2). `static_prop.vert:55` and `mech.vert:38`
   already bind `Instances`/`InstanceBuffer` at SSBO binding 0. Recycling
   `LIGHT_DATA_ATTACHMENT_SLOT=0` for the SSBO aliases light data over
   instance data in the SAME program -> silent catastrophic corruption
   of ALL static + mech rendering (every transform/instance read becomes
   garbage). This is a *new* failure mode the UBO version cannot have
   (UBO binding 0 is a different namespace from SSBO binding 0). It is
   ranked above the std430 layout because the layout verdict (Q3) is
   bit-identical / not-applicable, whereas the binding collision is a
   live, easy-to-make, hard-to-see mistake. **Mitigation: pick a NEW
   SSBO binding index unused in all 3 lighted programs (recommend >=20),
   rename the constant, thread it lockstep C++/GLSL.**
2. **MEDIUM: the recreate-on-grow path firing for the first time**
   (Q5). Dead today (<=57 distinct, floor 64, initial 128); live after
   the cap removal. Destroy+recreate must correctly re-bind the SSBO
   slot every grow, and the >128 CPU realloc path
   (`txmmgr.cpp:1170-1171`) is now exercised. Off-by-one or a missed
   rebind = light data points at a stale/destroyed buffer on the frame a
   dense mission crosses the threshold.
3. **LOW: AMD vertex-stage SSBO read.** Already in production on RX 7900
   XTX via the instance/bone SSBOs in the same shaders; no AMD rule
   restricts it. Adding one more read-only SSBO read is covered by
   existing precedent. Residual risk near zero.
4. **NOT APPLICABLE / NEUTRALIZED: std430-vs-std140 struct layout
   divergence.** Q3 verdict: bit-identical for this all-vec4/mat4 struct;
   no repack. The mc2_24-class lockstep regression does NOT fire on a
   pure qualifier change with zero struct-field edits. (Stays a CRITICAL
   *review* item only to enforce "no bundled struct edit in the same
   commit.")

### Parity gate (env-gated probe + visual canary, soak waived per `feedback_soak_waiver_with_probes_and_reviews_validated`)

- **Bit-equality probe:** env-gated (`MC2_LIGHTSSBO=0` -> unchanged UBO
  path bit-for-bit; default-ON per "full send" but the kill-switch is
  the reversibility safety for a GPU-buffer-contract change). With both
  paths compilable, a 1/N-sampled assert that the bytes uploaded to the
  SSBO equal the bytes the UBO path would have uploaded for the same
  frame (`memcmp(lightData_, ...)` is trivially equal since only the
  storage class changed - the real probe is *downstream*: a parity
  capture of the lit ARGB / `numLights` via the existing
  `parityOut_`/`u_parityNumLightsDebugMode` harness
  (`static_prop.vert:304-308`, `gos_object_parity.h`) showing
  UBO-vs-SSBO produce identical `light[idx].numLights` and lit color
  for a sampled actor).
- **Visual canary:** side-by-side `MC2_LIGHTSSBO` on/off on the heaviest
  mission AND mc2_17 (the maxIdx=57 mission - the one closest to the old
  cap, where the conversion's correctness benefit is observable) AND a
  deliberately dense mission if one exists: mech + static + object
  lighting bit-visually identical. The aRGB landmine
  (`feedback_offload...`) = wrong/hot colors if the binding collision
  (risk 1) is present -> the canary is specifically a binding-collision
  detector, not just a layout check.
- **Smoke:** tier1 5/5 (`mc2_01,03,10,17,24`) with `MC2_LIGHTSSBO`
  default-ON, `GL_INVALID_*`=0 (a wrong SSBO bind / missing
  `glShaderStorageBlockBinding` shows as `GL_INVALID_OPERATION` on
  draw), `+0` destroys. Verify `[MECHLIGHT v1] event=cache_full` does
  NOT fire (it must be removed/repurposed - Q4 - or it will false-spam).

---

## Open questions for the plan / adversarial review

1. **New SSBO binding index (BLOCKING).** Pick a value unused in
   static_prop.vert, mech.vert, gos_tex_vertex_lighted.{vert,frag} AND
   safe vs the GPU-driven compute/terrain SSBO set (0-19 in active use;
   recommend >=20). Decide whether to add a GameOS
   `gosBUFFER_TYPE::STORAGE` + `glShaderStorageBlockBinding`-aware bind
   API (clean, Vulkan-prep-aligned, named-contract) vs raw-GL inline in
   txmmgr (smaller blast radius, matches existing batcher precedent).
   Defer the API-shape ruling to `mc2-gameos-expert`.
2. **Material-cache path for `gos_tex_vertex_lighted`.** That legacy
   material binds via `gos_SetRenderMaterialUniformBlockBindingPoint(mat,
   "LightsData",0)` (`txmmgr.cpp:1364`) -> `setUniformBlock`
   (`gameos_graphics.cpp:384-392`) -> `glUniformBlockBinding`. SSBO
   blocks are NOT enumerated by `parse_uniform_blocks`
   (`shader_builder.cpp:563-601`, UBO-only via `GL_ACTIVE_UNIFORM_BLOCKS`),
   so `setUniformBlock("LightsData",...)` silently returns false and
   `glUniformBlockBinding` is the wrong call for an SSBO anyway. The
   legacy material path needs an SSBO-aware bind
   (`glShaderStorageBlockBinding` + a `parse_storage_blocks` reflection
   addition) OR `gos_tex_vertex_lighted` must be excluded from the SSBO
   conversion (kept on a separate UBO?) - **this is a second blocking
   design fork**: is `gos_tex_vertex_lighted` still a live path? (It is
   the legacy non-batched lit-mesh material; confirm whether any actor
   still routes through it post-GPU-driven, or whether its
   `glBindBufferBase` reliance is already covered by the single txmmgr
   bind.) Defer the "is the legacy material path still alive" question to
   `mc2-render-expert`.
3. **`kUboLightSlotCap` / `[MECHLIGHT v1] event=cache_full` disposition.**
   Remove vs repurpose as a distinct-slot-count sizer for the downstream
   static bake. Must not assert a 64 cap that no longer exists.
4. **Recreate-path correctness under first real exercise.** The
   destroy+recreate+rebind (`txmmgr.cpp:1563-1565`) and the >128 CPU
   realloc (`:1170-1171`) are dead today; add an env-gated
   `[LIGHTSSBO v1] event=buffer_grow old= new=` lifecycle print in the
   same commit (debug-instrumentation rule) so the first dense mission
   that crosses the threshold is observable, not silent.
5. **Atomic-commit discipline.** The qualifier change
   (`lighting.hglsl:39`), the array-bound change (`:54`), the constant
   rename, every `glBindBufferBase(GL_SHADER_STORAGE_BUFFER,...)` C++
   site, and the upload-path swap MUST land in ONE commit with ZERO
   `ObjectLights`/`TG_HWLightsData` field edits (Q3 caveat - any bundled
   field add voids the bit-identical verdict and re-arms the mc2_24
   lockstep hazard).
6. **`glMemoryBarrier` need.** The SSBO is CPU-written (glBufferData)
   then VS-read - no GPU compute writes it, so no
   `GL_SHADER_STORAGE_BARRIER_BIT` is needed (unlike the GPU-driven
   recipe SSBOs). Confirm no path has a compute shader writing
   `LightsData` (Q1 negative claim says none) so the plan does not
   cargo-cult a barrier.

---

*Recon only. No code written. All citations grep-verified at `5bffaf3`
this invocation (prompt-cited `2dca942` is 3 commits back; no light-path
code changed between - water-only commits).*
