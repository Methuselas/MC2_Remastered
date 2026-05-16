# GPU-Driven Dynamic Sun Shadow - Phase 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development (recommended) or superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.

**Goal:** GPU-batched mechs and buildings cast a crisp, camera-tracked sun shadow by rendering the existing camera-visible batcher instance sets into a frustum-fit dynamic shadow FBO.

**Architecture:** Replace the legacy CPU camera-raycast/hand-conversion shim with a frustum-fit sun ortho (unproject the 8 NDC corners through `Camera::clipToWorld`, apply the Stuff->MC2 swizzle, clamp to a fixed elevation slab and the static map-bounds radius, power-of-two anti-shimmer). Implement the two empty batcher `flushShadow()` stubs to draw their already-camera-windowed instance sets, depth-only, with two new instanced(/skinned) shadow vertex shaders, into the dynamic shadow FBO via a new GPU-config-gated region in `txmmgr.cpp` placed before the GPU cull dispatch. Static terrain shadow path (commit `0c421d1`) is untouched.

**Tech Stack:** C++ (GameOS/mclib), GLSL 4.30 (SSBO/std430), OpenGL 4.3, RelWithDebInfo MSVC build, env-gated stderr parity probes (project's TDD-equivalent; soak waived per `feedback_soak_waiver_with_probes_and_reviews_validated`).

**Scope:** Phase 1 only. Off-screen-caster shadows at low sun are a documented gap; Phase 2 (light-volume caster cull) is a separate tracked slice. Spec: `docs/superpowers/specs/2026-05-16-gpu-driven-dynamic-sun-shadow-design.md` (rev 3).

**Verification model:** Each task ends with build (RelWithDebInfo, full relink when load-bearing) + the relevant env-gated probe assertion and/or tier1 smoke + an atomic commit. Final task is user-driven visual. No unit-test harness exists for this subsystem; the env-gated `[SHADOWFIT v1]` probe is the executable assertion.

---

## File structure

- Create: `shaders/shadow_mech.vert` - instanced + skinned depth-only, mirrors `mech.vert` SSBO/bone path, emits `lightSpaceMatrix * mc2Pos`.
- Create: `shaders/shadow_static_prop.vert` - instanced depth-only, mirrors `static_prop.vert` SSBO modelMatrix path + Stuff->MC2 swizzle.
- Create: `shaders/shadow_instanced.frag` - trivial depth-only fragment (no color output) shared by both.
- Modify: `GameOS/gameos/gameos_graphics.cpp` - register the two new programs in `gosRenderer::init`; expose getters.
- Modify: `GameOS/gameos/gos_postprocess.cpp` - replace `buildDynamicLightMatrix` body with frustum-fit; add `[SHADOWFIT v1]` probe.
- Modify: `GameOS/gameos/gos_postprocess.h` - signature change for `buildDynamicLightMatrix` (pass the unprojected corners / camera matrix in).
- Modify: `mclib/txmmgr.cpp` - delete the dead `g_numShadowShapes>0` region's shim; add the new GPU-config-gated shadow region before `gpu_cull::compute_dispatch()`.
- Modify: `GameOS/gameos/gos_mech_batcher.cpp` / `.h` - implement `GpuMechBatcher::flushShadow()`.
- Modify: `GameOS/gameos/gos_static_prop_batcher.cpp` / `.h` - implement `GpuStaticPropBatcher::flushShadow()`.

---

### Task 1: Depth-only shadow shaders + loader registration

**Files:**
- Create: `shaders/shadow_mech.vert`
- Create: `shaders/shadow_static_prop.vert`
- Create: `shaders/shadow_instanced.frag`
- Modify: `GameOS/gameos/gameos_graphics.cpp` (`gosRenderer::init`, near the existing `shadow_object_material_` load ~:3399-3408; members near ~:1625-1626)

- [ ] **Step 1: Grep-confirm the exact SSBO/attribute layout to mirror**

Run (record exact current lines; do not trust the numbers below):
```
grep -n "GpuMechInstance\|GpuMechBone\|a_position\|a_boneIndices\|a_boneWeights\|baseBoneOffset\|u_instanceBase\|u_skinningMode" shaders/mech.vert
grep -n "struct Instance\|modelMatrix\|instances_.i\|a_position\|world_stuff\|world_mc2\|gl_BaseInstanceARB\|MC2_COALESCE" shaders/static_prop.vert
grep -n "stuffPos\|mc2Pos\|lightOffset\|lightSpaceMatrix\|worldMatrix" shaders/shadow_object.vert
```
Expected: confirms `mech.vert` binding 0 `InstanceBuffer{GpuMechInstance instances[]}` (fields `typeLodRecordIndex,baseBoneOffset,lightDataIndex,renderFlags,aRGBHighlight,fogRGB`), binding 1 `BoneBuffer{GpuMechBone bones[]}` (`row0..row3`), attrs loc0 `vec3 a_position`, loc3 `uvec4 a_boneIndices`, loc4 `vec4 a_boneWeights`, `u_instanceBase`, skinning `boneT = mat4(b.row0..row3)`, world `vec3(-s.x,s.z,s.y)`; `static_prop.vert` binding 0 `Instances{Instance i[]}` `i[].modelMatrix`, world `vec4(a_position,1)*inst.modelMatrix` then `(-x,z,y)`.

- [ ] **Step 2: Write `shaders/shadow_instanced.frag`**

```glsl
// Depth-only shadow fragment. No color attachment write; depth is implicit.
void main() {}
```

- [ ] **Step 3: Write `shaders/shadow_mech.vert`** (mirror mech.vert position-only path; NO #version line - prefix is injected by makeProgram)

```glsl
// Depth-only instanced+skinned mech shadow. Mirrors mech.vert SSBO/bone
// math for POSITION ONLY. mat4(row0..row3) is intentionally the transpose
// of the Stuff bone matrix so boneT*v == row-vec v*M (see mech.vert) -
// do NOT "fix" it.
layout(location=0) in vec3 a_position;
layout(location=3) in uvec4 a_boneIndices;
layout(location=4) in vec4 a_boneWeights;

struct GpuMechInstance { uint typeLodRecordIndex; uint baseBoneOffset; uint lightDataIndex; uint renderFlags; vec4 aRGBHighlight; vec4 fogRGB; };
struct GpuMechBone { vec4 row0, row1, row2, row3; };
layout(std430, binding=0) readonly buffer InstanceBuffer { GpuMechInstance instances[]; };
layout(std430, binding=1) readonly buffer BoneBuffer { GpuMechBone bones[]; };

uniform int  u_instanceBase;
uniform int  u_skinningMode;
uniform mat4 lightSpaceMatrix;

void main() {
    uint instIdx = uint(u_instanceBase) + uint(gl_InstanceID);
    GpuMechInstance inst = instances[instIdx];
    vec4 worldStuff;
    if (u_skinningMode == 0) {
        GpuMechBone b = bones[a_boneIndices.x + inst.baseBoneOffset];
        mat4 boneT = mat4(b.row0, b.row1, b.row2, b.row3);
        worldStuff = boneT * vec4(a_position, 1.0);
    } else {
        worldStuff = vec4(0.0);
        for (int i = 0; i < 4; ++i) {
            float w = a_boneWeights[i];
            if (w <= 0.0) continue;
            GpuMechBone b = bones[a_boneIndices[i] + inst.baseBoneOffset];
            mat4 boneT = mat4(b.row0, b.row1, b.row2, b.row3);
            worldStuff += w * (boneT * vec4(a_position, 1.0));
        }
    }
    vec3 worldMC2 = vec3(-worldStuff.x, worldStuff.z, worldStuff.y);
    gl_Position = lightSpaceMatrix * vec4(worldMC2, 1.0);
}
```
(In Step 1 confirm `a_boneWeights`/`u_skinningMode` names/locations match `mech.vert` exactly; adjust verbatim if the grep shows different identifiers.)

- [ ] **Step 4: Write `shaders/shadow_static_prop.vert`** (mirror static_prop.vert legacy non-coalesce path, position only)

```glsl
// Depth-only instanced static-prop shadow. Mirrors static_prop.vert
// legacy (non-coalesce) SSBO path. v*M row-vector order (Stuff std430
// col-major), then Stuff->MC2 swizzle - matches static_prop.vert, NOT
// shadow_object.vert's column-vector convention.
layout(location=0) in vec3 a_position;

struct Instance { mat4 modelMatrix; uint typeID; uint firstColorOffset; uint flags; uint lightDataIndex; vec4 aRGBHighlight; vec4 fogRGB; };
layout(std430, binding=0) readonly buffer Instances { Instance i[]; } instances_;

uniform int  u_instanceBase;
uniform mat4 lightSpaceMatrix;

void main() {
    Instance inst = instances_.i[uint(u_instanceBase) + uint(gl_InstanceID)];
    vec4 worldStuff = vec4(a_position, 1.0) * inst.modelMatrix;
    vec3 worldMC2 = vec3(-worldStuff.x, worldStuff.z, worldStuff.y);
    gl_Position = lightSpaceMatrix * vec4(worldMC2, 1.0);
}
```
(Step 1 confirms whether the legacy path indexes `instances_.i[gl_InstanceID]` or `[u_instanceBase+gl_InstanceID]`; match the legacy non-coalesce branch exactly. If legacy uses bare `gl_InstanceID`, drop `u_instanceBase` here and bind the SSBO range per-type instead - decide from the grep, not assumed.)

- [ ] **Step 5: Register the two programs in `gosRenderer::init`**

Grep first: `grep -n "makeProgram\|kThinPrefix\|#version 430\|shadow_object_material_\|materialList_.push_back" GameOS/gameos/gameos_graphics.cpp | head -60` to copy the exact SSBO-program registration pattern used by the static-prop/mech color programs (they use `glsl_program::makeProgram(name, "#version 430\n", ...)`, NOT `gosRenderMaterial::load`). Add, mirroring that pattern, after the `shadow_object_material_` block (~:3408):

```cpp
// Depth-only instanced shadow programs for GPU-batched casters (Phase 1).
static const char* kShadowPrefix = "#version 430\n";
shadow_mech_prog_ = glsl_program::makeProgram(
    "shadow_mech", kShadowPrefix, "shaders/shadow_mech.vert", "shaders/shadow_instanced.frag");
shadow_static_prop_prog_ = glsl_program::makeProgram(
    "shadow_static_prop", kShadowPrefix, "shaders/shadow_static_prop.vert", "shaders/shadow_instanced.frag");
```
Match the EXACT `makeProgram` signature found in the grep (arg order/types may differ - use the grepped form verbatim). Declare members near the other shadow program members (`grep -n "shadow_object_material_\|thin_terrain_prog_" GameOS/gameos/gameos_graphics.cpp` for the decl site) and add public getters `glsl_program* getShadowMechProg()` / `getShadowStaticPropProg()` mirroring an existing prog getter.

- [ ] **Step 6: Build (full relink - new shaders + gameos_graphics.cpp)**

Run:
```
rm -f build64/RelWithDebInfo/mc2.exe "build64/out/GameOS/gameos/gameos.dir/RelWithDebInfo/gameos_graphics.obj"
"/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -20
```
Expected: exit 0, `mc2.exe` relinked. Shaders are loaded at runtime (not compiled into exe) so a clean compile here only proves the C++ registration compiles.

- [ ] **Step 7: Commit**

```
git add shaders/shadow_mech.vert shaders/shadow_static_prop.vert shaders/shadow_instanced.frag GameOS/gameos/gameos_graphics.cpp
git commit -m "feat(shadow): depth-only instanced mech/static-prop shadow shaders + loader"
```

---

### Task 2: Frustum-fit dynamic light matrix

**Files:**
- Modify: `GameOS/gameos/gos_postprocess.cpp` (`buildDynamicLightMatrix` ~:1364-1432; texel snap ~:1382-1384; up-hint guard ~:1391-1392; z-row ~:1417-1423)
- Modify: `GameOS/gameos/gos_postprocess.h` (`buildDynamicLightMatrix` signature)
- Modify: `mclib/txmmgr.cpp` (delete shim ~:1559-1574; the new region in Task 5 calls the new builder)

- [ ] **Step 1: Re-grep all anchors**

```
grep -n "buildDynamicLightMatrix\|xyRadius\|depthDist\|fabsf(fz)\|worldUnitsPerTexel\|dynShadowMapSize_\|nearP = 1.0f\|farP = 2.0f" GameOS/gameos/gos_postprocess.cpp
grep -n "clipToWorld\|getLookVector\|getCameraOrigin\|Invert(worldToClip" mclib/camera.cpp mclib/camera.h
grep -n "mapHalfExtent\|setMapHalfExtent\|1.05f" GameOS/gameos/gos_postprocess.cpp GameOS/gameos/gos_postprocess.h
```
Expected: confirms `buildDynamicLightMatrix(float sunDirX,Y,Z,float camX,Y,Z)`, `xyRadius=2400`, `depthDist=5000`, the `fabsf(fz)>0.9f` up-hint guard, the `floorf` snap, `dynShadowMapSize_` (live 4096 via init `~:1312`), static `r = mapHalfExtent*sqrtf(2.0f)*1.05f` (~:1207), `clipToWorld` = `worldToClip.inverse()` set `camera.cpp:~2291`, `eye->clipToWorld` public `Stuff::Matrix4D`.

- [ ] **Step 2: Change `buildDynamicLightMatrix` signature to take the fit footprint**

In `gos_postprocess.h` change the declaration to:
```cpp
// camFitCornersMC2 = 8 raw-MC2 frustum corners (clipToWorld-unprojected
// + Stuff->MC2 swizzled by the caller). Builder clamps + fits the ortho.
void buildDynamicLightMatrix(float sunDirX, float sunDirY, float sunDirZ,
                             const float camFitCornersMC2[8][3]);
```
(The CPU-shim camX/Y/Z form is removed; the caller in Task 5 computes corners. Keep the old symbol name so consumers/probe references stay.)

- [ ] **Step 3: Replace the fixed-extent body with the frustum-fit**

Replace the `xyRadius`/`depthDist`/`floorf`-snap region (everything from the old `float xyRadius = 2400.0f;` down to just before the view-basis `fabsf(fz)` block) with:

```cpp
// --- Frustum-fit XY extent in raw-MC2 (corners supplied by caller) ---
// Fixed conservative elevation slab + caster margin (no global terrain
// min/max constant exists; the real safety net is the map-bounds clamp).
const float kSlabMinZ = -512.0f - 512.0f;   // floor - caster margin
const float kSlabMaxZ = 4096.0f + 512.0f;   // ceil + caster margin
float minX =  1e30f, maxX = -1e30f, minY = 1e30f, maxY = -1e30f;
for (int c = 0; c < 8; ++c) {
    float x = camFitCornersMC2[c][0];
    float y = camFitCornersMC2[c][1];
    float z = camFitCornersMC2[c][2];
    z = (z < kSlabMinZ) ? kSlabMinZ : (z > kSlabMaxZ ? kSlabMaxZ : z);
    if (x < minX) minX = x; if (x > maxX) maxX = x;
    if (y < minY) minY = y; if (y > maxY) maxY = y;
    (void)z; // slab clamp bounds the corners conceptually; XY AABB is the fit
}
// Map-bounds clamp (the real safety net) - mirrors static path's r.
float r = mapHalfExtent_ * 1.41421356f * 1.05f;
if (minX < -r) minX = -r; if (maxX > r) maxX = r;
if (minY < -r) minY = -r; if (maxY > r) maxY = r;
float cx = 0.5f * (minX + maxX);
float cy = 0.5f * (minY + maxY);
float halfX = 0.5f * (maxX - minX);
float halfY = 0.5f * (maxY - minY);
float fitRadius = (halfX > halfY ? halfX : halfY);
if (fitRadius < 64.0f) fitRadius = 64.0f;          // sane floor
if (fitRadius > r)      fitRadius = r;              // never exceed map
// Power-of-two extent quantization (anti-shimmer) AFTER map clamp.
float xyRadius = 64.0f;
while (xyRadius < fitRadius) xyRadius *= 2.0f;
if (xyRadius > r) xyRadius = r;
// Texel snap the center using the SNAPPED extent.
float worldUnitsPerTexel = (2.0f * xyRadius) / (float)dynShadowMapSize_;
float camX = floorf(cx / worldUnitsPerTexel) * worldUnitsPerTexel;
float camY = floorf(cy / worldUnitsPerTexel) * worldUnitsPerTexel;
float camZ = 0.0f;
float depthDist = 5000.0f;
```
Leave the existing view-basis block (INCLUDING the `fabsf(fz)>0.9f` up-hint guard) and the `[0,1]` ortho z-row (`nearP=1.0f, farP=2.0f*depthDist`, `-1/(farP-nearP)`, `-nearP/(farP-nearP)`) and the composite multiply EXACTLY as-is. They now consume the fit `xyRadius`/`camX`/`camY`/`camZ`. Verify by grep that nothing else referenced the removed fixed `xyRadius=2400`.

- [ ] **Step 4: Delete the legacy shim in `txmmgr.cpp`**

Remove the `~:1559-1574` block (`eye->getLookVector()` raycast + `focusX/focusZ` hand-conversion + the old `gos_BuildDynamicLightMatrix(-lx,-ly,-lz,focusX,focusZ,0.0f)` call) AND the dead `if (... g_numShadowShapes > 0)` region body `~:1549-1581` (keep `g_numShadowShapes = 0;` reset if any other code still increments it - grep `g_numShadowShapes` first; per gate findings the sole producer `tgl.cpp:3064` is fallback-only, but leave the variable + reset intact to avoid unrelated breakage). The new region (Task 5) supersedes it.

- [ ] **Step 5: Build full relink**

```
rm -f build64/RelWithDebInfo/mc2.exe "build64/out/GameOS/gameos/gameos.dir/RelWithDebInfo/gos_postprocess.obj" "build64/out/mclib/mclib.dir/RelWithDebInfo/txmmgr.obj"
"/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -20
```
Expected: exit 0. (Visual result not yet observable - the new builder has no caller until Task 5; this step only proves it compiles.)

- [ ] **Step 6: Commit**

```
git add GameOS/gameos/gos_postprocess.cpp GameOS/gameos/gos_postprocess.h mclib/txmmgr.cpp
git commit -m "feat(shadow): frustum-fit dynamic light ortho (clipToWorld fit, replaces CPU shim)"
```

---

### Task 3: GpuStaticPropBatcher::flushShadow()

**Files:**
- Modify: `GameOS/gameos/gos_static_prop_batcher.cpp` (stub ~:3589; legacy non-indirect draw pattern ~:3299-3475; upload ~:2600-2607)
- Modify: `GameOS/gameos/gos_static_prop_batcher.h` (already declares `flushShadow()` ~:208 - verify)

- [ ] **Step 1: Re-grep the legacy non-indirect draw + upload**

```
grep -n "flushShadow\|uploadAllBucketsIfNeeded\|s_lastUploadedSlot\|s_typeRanges\|glDrawElementsInstancedBaseVertex\|useC1bIndirect\|compute_getIndirectCmdBuf\|s_instanceSsbo\|s_staticPropProgram\b\|instanceByteOffset\|instanceCount" GameOS/gameos/gos_static_prop_batcher.cpp
```
Expected: confirms the legacy branch loops `s_typeRanges`, `glBindBufferRange(...,0,s_instanceSsbo,r.instanceByteOffset,...)`, per-packet `glDrawElementsInstancedBaseVertex(GL_TRIANGLES,pkt.indexCount,GL_UNSIGNED_INT,(void*)(pkt.firstIndex*4),r.instanceCount,pkt.baseVertex)`; indirect branch reads `compute_getIndirectCmdBuf()`.

- [ ] **Step 2: Implement `flushShadow()` - non-indirect full per-type ranges, shadow program**

Replace the empty stub body with (adapt identifiers to the Step-1 grep verbatim):
```cpp
void GpuStaticPropBatcher::flushShadow() {
    if (!uploadAllBucketsIfNeeded()) return;          // shared upload (fence-safe)
    glsl_program* prog = g_gos_renderer ? g_gos_renderer->getShadowStaticPropProg() : nullptr;
    if (!prog || s_typeRanges.empty()) return;
    prog->bind();
    GLint lsLoc = prog->getUniformLocation("lightSpaceMatrix");
    if (lsLoc >= 0) glUniformMatrix4fv(lsLoc, 1, GL_FALSE,
        g_gos_renderer->getPostProcess()->getDynamicLightSpaceMatrix());
    glBindVertexArray(s_sharedVao);                   // same VAO as flush()
    int typesDrawn = 0, instDrawn = 0;
    for (auto& kv : s_typeRanges) {
        const TypeRangeSsbo& rr = kv.second;          // exact type name from grep
        glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 0, s_instanceSsbo,
                          (GLintptr)rr.instanceByteOffset,
                          (GLsizeiptr)(rr.instanceCount * sizeof(/*Instance struct*/)));
        // per-type packet loop exactly as the legacy non-indirect branch
        for (const auto& pkt : /* packets for kv.first per grep */) {
            GLint baseLoc = prog->getUniformLocation("u_instanceBase");
            if (baseLoc >= 0) glUniform1i(baseLoc, 0); // SSBO range is per-type-based
            glDrawElementsInstancedBaseVertex(
                GL_TRIANGLES, (GLsizei)pkt.indexCount, GL_UNSIGNED_INT,
                (void*)(uintptr_t)(pkt.firstIndex * sizeof(uint32_t)),
                (GLsizei)rr.instanceCount, pkt.baseVertex);
        }
        ++typesDrawn; instDrawn += (int)rr.instanceCount;
    }
    s_shadowTypesDrawn = typesDrawn;                  // for the [SHADOWFIT v1] probe (Task 6)
    s_shadowInstDrawn  = instDrawn;
}
```
EXACT struct size, packet container, `u_instanceBase`-vs-bare-`gl_InstanceID` semantics, and VAO handle MUST come from the Step-1 grep (this scaffold names them; the executor fills the grepped types verbatim, no guessing). It MUST use the legacy non-indirect path, NEVER `compute_getIndirectCmdBuf()` (camera-cull narrowed). Add the two `static int s_shadowTypesDrawn/InstDrawn` (file-scope) for the probe.

- [ ] **Step 3: Build full relink + tier1 smoke (no visual yet - not wired)**

```
rm -f build64/RelWithDebInfo/mc2.exe "build64/out/GameOS/gameos/gameos.dir/RelWithDebInfo/gos_static_prop_batcher.obj"
"/c/Program Files (x86)/.../cmake.exe" --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -20
```
Expected: exit 0. (flushShadow has no caller until Task 5.)

- [ ] **Step 4: Commit**

```
git add GameOS/gameos/gos_static_prop_batcher.cpp GameOS/gameos/gos_static_prop_batcher.h
git commit -m "feat(shadow): GpuStaticPropBatcher::flushShadow non-indirect full-range depth draw"
```

---

### Task 4: GpuMechBatcher::flushShadow()

**Files:**
- Modify: `GameOS/gameos/gos_mech_batcher.cpp` (stub ~:341; per-bucket draw ~:1068; ring `s_frameSlot` ~:149/820/827; fence ~:1097; bindings ~:885-890; `u_instanceBase` ~:994)
- Modify: `GameOS/gameos/gos_mech_batcher.h` (verify `flushShadow()` decl + `MECH_RING_FRAMES=3`)

- [ ] **Step 1: Re-grep ring/draw + DERIVE the read slot**

```
grep -n "flushShadow\|s_frameSlot\|MECH_RING_FRAMES\|s_fence\|glFenceSync\|glClientWaitSync\|s_instanceSsbo\|s_boneSsbo\|glBindBufferRange\|glDrawElementsInstancedBaseVertex\|u_instanceBase\|s_mechProgram\|s_sharedVao\|s_instanceCapacity\|s_boneCapacity\|drawCalls\|instanceBase\|GpuMechInstance\|GpuMechBone" GameOS/gameos/gos_mech_batcher.cpp
```
Derive the read slot precisely: `flush()` advances `s_frameSlot` at the top (~:820), writes that slot (~:827), fences it (~:1097). `flushShadow()` runs in the new region BEFORE this frame's mech `flush()` (Task 5), so `s_frameSlot` still holds the PREVIOUS frame's post-advance value and the previous frame's data+fence are at `s_frameSlot` itself. **Read slot = `s_frameSlot`** (NOT `s_frameSlot-1`). Confirm by reading the advance/write/fence ordering in the grep; assert with the probe (Task 6).

- [ ] **Step 2: Implement `flushShadow()` - previous fenced slot, per-bucket loop, shadow_mech program**

Replace the empty stub. Mirror the `flush()` per-bucket draw (`~:991-1074`) but: bind SSBO ranges at `s_frameSlot` (already fenced, do NOT advance, do NOT write), use `shadow_mech` program + `lightSpaceMatrix`:
```cpp
void GpuMechBatcher::flushShadow() {
    if (s_pendingSubmits.empty()) return;             // nothing batched this frame
    // Reuse the buckets/drawCalls built for this frame's color flush.
    // (Re-derive buckets exactly as flush() does up to the draw loop, OR
    //  factor flush()'s bucket-build into a shared helper called here
    //  first and by flush() - see Step 3; pick the lower-risk option.)
    glsl_program* prog = g_gos_renderer ? g_gos_renderer->getShadowMechProg() : nullptr;
    if (!prog) return;
    prog->bind();
    GLint lsLoc = prog->getUniformLocation("lightSpaceMatrix");
    if (lsLoc >= 0) glUniformMatrix4fv(lsLoc, 1, GL_FALSE,
        g_gos_renderer->getPostProcess()->getDynamicLightSpaceMatrix());
    GLint smLoc = prog->getUniformLocation("u_skinningMode");
    glBindVertexArray(s_sharedVao);
    uint32_t slot = s_frameSlot;                      // already-fenced previous data
    glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 0, s_instanceSsbo,
        (GLintptr)(slot * s_instanceCapacity * sizeof(GpuMechInstance)),
        (GLsizeiptr)(s_lastTotalInstances * sizeof(GpuMechInstance)));
    glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 1, s_boneSsbo,
        (GLintptr)(slot * s_boneCapacity * sizeof(GpuMechBone)),
        (GLsizeiptr)(s_lastTotalBones * sizeof(GpuMechBone)));
    int typesDrawn = 0, instDrawn = 0;
    for (/* each bucket/drawCall exactly as flush() ~:991 */) {
        if (smLoc >= 0) glUniform1i(smLoc, /* s_skinningMode as flush() sets it */);
        GLint baseLoc = prog->getUniformLocation("u_instanceBase");
        if (baseLoc >= 0) glUniform1i(baseLoc, (int)dc.instanceBase);
        glDrawElementsInstancedBaseVertex(GL_TRIANGLES, (GLsizei)pkt.indexCount,
            GL_UNSIGNED_INT, (void*)(uintptr_t)(pkt.firstIndex * sizeof(uint32_t)),
            (GLsizei)dc.instanceCount, pkt.baseVertex);
        ++typesDrawn; instDrawn += (int)dc.instanceCount;
    }
    s_shadowTypesDrawn = typesDrawn; s_shadowInstDrawn = instDrawn; // probe (Task 6)
}
```
`s_lastTotalInstances/Bones` = the instance/bone counts written for `slot` by the previous `flush()` - grep for the existing variables that hold the last upload's totals; if none exist, capture them into new file-scope statics at the end of `flush()` (same commit). EXACT bucket/drawCall iteration, `dc.instanceBase`, skinning-mode source MUST be the grepped `flush()` shapes.

- [ ] **Step 3: Decide bucket-build reuse (lower-risk)**

`flushShadow` needs the same per-frame buckets `flush()` builds. Grep where `flush()` builds `drawCalls`/buckets vs where it advances the ring (`~:820`). If bucket-build is BEFORE the ring advance and side-effect-free, factor it into a private `buildFrameBuckets()` called by both (idempotent via a per-frame latch) - this is the spec's prescribed shape. If bucket-build is entangled with the ring write, instead have `flushShadow` read the PREVIOUS frame's persisted drawCalls (capture them into a file-scope vector at end of `flush()`); choose whichever the grep shows is lower blast radius and document the choice in the commit message. Do NOT advance/write the ring in `flushShadow` either way.

- [ ] **Step 4: Build full relink**

```
rm -f build64/RelWithDebInfo/mc2.exe "build64/out/GameOS/gameos/gameos.dir/RelWithDebInfo/gos_mech_batcher.obj"
"/c/Program Files (x86)/.../cmake.exe" --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -20
```
Expected: exit 0.

- [ ] **Step 5: Commit**

```
git add GameOS/gameos/gos_mech_batcher.cpp GameOS/gameos/gos_mech_batcher.h
git commit -m "feat(shadow): GpuMechBatcher::flushShadow previous-fenced-slot depth draw"
```

---

### Task 5: New GPU-config-gated dynamic shadow region + frustum-corner unproject

**Files:**
- Modify: `mclib/txmmgr.cpp` (insert new region after the old dead region `~:1581`, BEFORE `gpu_cull::compute_dispatch()` `~:1737`)

- [ ] **Step 1: Re-grep region structure + clipToWorld access**

```
grep -n "g_numShadowShapes\|gos_IsTerrainTessellationActive\|g_useGpuObjects\|g_useGpuMechs\|gpu_cull::compute_dispatch\|Render.GpuStaticProps\|gos_BeginDynamicShadowPass\|gos_EndDynamicShadowPass\|GpuStaticPropBatcher::instance\|GpuMechBatcher::instance\|eye->\|clipToWorld" mclib/txmmgr.cpp
grep -n "Vector4D::Multiply\|clipToWorld" mclib/stuff/vector4d.cpp mclib/camera.h
```
Expected: confirms insertion point (after `g_numShadowShapes = 0;` ~:1581, before `Render.TerrainSolid`), `compute_dispatch()` ~:1737, `g_useGpuObjects/g_useGpuMechs` extern ~:1714-1715, `eye->clipToWorld` public `Stuff::Matrix4D`, `Vector4D::Multiply(v,m)` row-vector form.

- [ ] **Step 2: Add the new region**

Insert after the old region's `g_numShadowShapes = 0;` (and after deleting the old shim per Task 2 Step 4):
```cpp
// --- GPU-driven dynamic sun shadow (Phase 1): frustum-fit + flushShadow.
// Runs BEFORE gpu_cull::compute_dispatch so static-prop shadow uses the
// full camera-visible per-type ranges (not the cull-narrowed indirect).
// Casters = the camera-visible (inView) batched set (Phase 1 scope;
// off-screen-caster low-sun shadow is the documented Phase-2 gap).
if (gos_IsTerrainTessellationActive() && (g_useGpuObjects || g_useGpuMechs)) {
    // Unproject 8 NDC corners through clipToWorld (Stuff space) then
    // swizzle Stuff->MC2 (-x, z, y) - matches shadow_object.vert:14.
    const float ndc[8][3] = {
        {-1,-1,0},{ 1,-1,0},{-1, 1,0},{ 1, 1,0},
        {-1,-1,1},{ 1,-1,1},{-1, 1,1},{ 1, 1,1} };
    float cornersMC2[8][3];
    for (int c = 0; c < 8; ++c) {
        Stuff::Vector4D in(ndc[c][0], ndc[c][1], ndc[c][2], 1.0f), out;
        out.Multiply(in, eye->clipToWorld);           // row-vector convention
        float w = out.w;
        if (w < 0.0f) { out.x=-out.x; out.y=-out.y; out.z=-out.z; w=-w; }
        float inv = (fabsf(w) > 1e-6f) ? 1.0f / w : 0.0f;
        float sx = out.x*inv, sy = out.y*inv, sz = out.z*inv;
        cornersMC2[c][0] = -sx;                        // Stuff->MC2
        cornersMC2[c][1] =  sz;
        cornersMC2[c][2] =  sy;
    }
    float lx, ly, lz; gos_GetTerrainLightDir(&lx, &ly, &lz); // exact accessor per grep
    gos_BuildDynamicLightMatrix(-lx, -ly, -lz, cornersMC2);  // new signature
    gos_BeginDynamicShadowPass();                      // internally no-ops if !shadowsEnabled_
    GpuStaticPropBatcher::instance().flushShadow();
    GpuMechBatcher::instance().flushShadow();
    gos_EndDynamicShadowPass();
}
```
Use the EXACT light-dir accessor the old shim used (grep the deleted `-lx,-ly,-lz` source - likely `gos_GetTerrainLightDir` or the `eye->`/sun accessor at the old `~:1553`); `Stuff::Vector4D`/`Multiply` exact API from the grep. `gos_BuildDynamicLightMatrix` is the C wrapper for the new `buildDynamicLightMatrix(sun, corners[8][3])` - update the wrapper signature in `gameos_graphics.cpp`/`gos_postprocess.h` accordingly (grep `gos_BuildDynamicLightMatrix` for the wrapper site, change it in this task).

- [ ] **Step 3: Build full relink + tier1 smoke (now wired - first observable)**

```
rm -f build64/RelWithDebInfo/mc2.exe "build64/out/mclib/mclib.dir/RelWithDebInfo/txmmgr.obj" "build64/out/GameOS/gameos/gameos.dir/RelWithDebInfo/gos_postprocess.obj"
"/c/Program Files (x86)/.../cmake.exe" --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -20
cp -f build64/RelWithDebInfo/mc2.exe /a/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe && diff -q build64/RelWithDebInfo/mc2.exe /a/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe
cp -f shaders/shadow_mech.vert shaders/shadow_static_prop.vert shaders/shadow_instanced.frag /a/Games/mc2-opengl/mc2-win64-v0.4/shaders/ && for f in shadow_mech.vert shadow_static_prop.vert shadow_instanced.frag; do diff -q shaders/$f /a/Games/mc2-opengl/mc2-win64-v0.4/shaders/$f; done
py -3 scripts/run_smoke.py --tier tier1 --duration 30 --kill-existing
```
Expected: build exit 0; deploy in sync; smoke exit 0 (no crash/regression). Check the console for shader compile errors (`grep -i "compile error\|link error" tests/smoke/artifacts/<latest>/*.log`) - hot-reload fails silently so a bad shadow shader leaves no shadow but must not crash.

- [ ] **Step 4: Commit**

```
git add mclib/txmmgr.cpp GameOS/gameos/gos_postprocess.h GameOS/gameos/gameos_graphics.cpp
git commit -m "feat(shadow): GPU-config dynamic shadow region (frustum-fit + flushShadow) pre-cull"
```

---

### Task 6: [SHADOWFIT v1] env-gated parity probe

**Files:**
- Modify: `GameOS/gameos/gos_postprocess.cpp` (in `buildDynamicLightMatrix`, mirror the `[SHADOWZRANGE v1]` pattern ~:1438)
- Modify: `GameOS/gameos/gos_static_prop_batcher.cpp` / `gos_mech_batcher.cpp` (emit the `s_shadowTypesDrawn/InstDrawn` captured in Tasks 3-4)

- [ ] **Step 1: Add the probe in `buildDynamicLightMatrix`**

After the composite multiply, mirroring `[SHADOWZRANGE v1]` (unconditional `fprintf(stderr)`, NOT assert, per `assert_is_noop_in_relwithdebinfo`):
```cpp
if (getenv("MC2_DEBUG_SHADOW_FIT") != nullptr) {
    static int s_fitN = 0;
    if (++s_fitN <= 1 || (s_fitN % 600) == 0) {       // one-shot + periodic
        fprintf(stderr, "[SHADOWFIT v1] event=fit n=%d "
            "cMC2_0=(%.1f,%.1f,%.1f) cMC2_6=(%.1f,%.1f,%.1f) "
            "fitC=(%.1f,%.1f) xyRadius=%.1f mapR=%.1f wuPerTexel=%.3f\n",
            s_fitN, camFitCornersMC2[0][0],camFitCornersMC2[0][1],camFitCornersMC2[0][2],
            camFitCornersMC2[6][0],camFitCornersMC2[6][1],camFitCornersMC2[6][2],
            camX, camY, xyRadius, r, worldUnitsPerTexel);
        fflush(stderr);
    }
}
```

- [ ] **Step 2: Add the per-batcher instance-count emit**

In each `flushShadow()` tail, gated identically:
```cpp
if (getenv("MC2_DEBUG_SHADOW_FIT") != nullptr) {
    static int n = 0;
    if (++n <= 1 || (n % 600) == 0) {
        fprintf(stderr, "[SHADOWFIT v1] event=flush batcher=%s types=%d inst=%d slot=%u fbo=%d\n",
            /*"staticprop"|"mech"*/, s_shadowTypesDrawn, s_shadowInstDrawn,
            (unsigned)/*slot or 0*/, (int)/*current GL_FRAMEBUFFER_BINDING or pp fbo*/);
        fflush(stderr);
    }
}
```

- [ ] **Step 3: Build, deploy, run smoke WITH the probe**

```
rm -f build64/RelWithDebInfo/mc2.exe build64/out/GameOS/gameos/gameos.dir/RelWithDebInfo/gos_postprocess.obj build64/out/GameOS/gameos/gameos.dir/RelWithDebInfo/gos_mech_batcher.obj build64/out/GameOS/gameos/gameos.dir/RelWithDebInfo/gos_static_prop_batcher.obj
"/c/Program Files (x86)/.../cmake.exe" --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -20
cp -f build64/RelWithDebInfo/mc2.exe /a/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe && diff -q build64/RelWithDebInfo/mc2.exe /a/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe
MC2_DEBUG_SHADOW_FIT=1 py -3 scripts/run_smoke.py --tier tier1 --duration 30 --kill-existing
grep -h "\[SHADOWFIT v1\]" tests/smoke/artifacts/$(ls -t tests/smoke/artifacts | head -1)/*.log | head -40
```
Expected: `event=fit` lines with `xyRadius` a power-of-two, `<= mapR`, `wuPerTexel` finite >0, corners finite (not 1e30/NaN); `event=flush` for BOTH `staticprop` and `mech` with `inst>0` on missions that have buildings/mechs (mc2_01/mc2_10). `inst=0` everywhere => casters not reaching flushShadow (a bug to fix before proceeding, not a pass).

- [ ] **Step 4: Commit**

```
git add GameOS/gameos/gos_postprocess.cpp GameOS/gameos/gos_mech_batcher.cpp GameOS/gameos/gos_static_prop_batcher.cpp
git commit -m "feat(shadow): [SHADOWFIT v1] env-gated parity probe (fit AABB + caster counts)"
```

---

### Task 7: User-visual verification + Phase 2 follow-up filing

- [ ] **Step 1: Final full relink + deploy + tier1 smoke**

```
rm -f build64/RelWithDebInfo/mc2.exe
"/c/Program Files (x86)/.../cmake.exe" --build build64 --config RelWithDebInfo --target mc2 --clean-first 2>&1 | tail -20
cp -f build64/RelWithDebInfo/mc2.exe /a/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe && diff -q build64/RelWithDebInfo/mc2.exe /a/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe
for f in shadow_mech.vert shadow_static_prop.vert shadow_instanced.frag; do cp -f shaders/$f /a/Games/mc2-opengl/mc2-win64-v0.4/shaders/$f && diff -q shaders/$f /a/Games/mc2-opengl/mc2-win64-v0.4/shaders/$f; done
py -3 scripts/run_smoke.py --tier tier1 --duration 30 --kill-existing
```
Expected: clean build, in-sync deploy, smoke exit 0.

- [ ] **Step 2: User-driven visual (hand back; this is the acceptance gate)**

Report to the user to verify in-game (smoke is user-driven): mech AND building shadows now present, sharp, and camera-tracked across 360deg azimuth / +10..90deg elevation; Alt+F2 dynamic atlas is full and tight (no clipped wedge); shadow edges quantize in discrete steps on zoom (no continuous crawl); the static terrain shadow (artifact A, commit 0c421d1) is NOT regressed; `MC2_DEBUG_SHADOW_ZRANGE=1` still `inRange=1`. Known/expected: off-screen casters at low sun do not shadow into view (documented Phase-1 gap). Do NOT claim success before user confirmation (`verification-before-completion`).

- [ ] **Step 3: File Phase 2 as a tracked follow-up**

Append a Phase-2 entry to `docs/superpowers/VPL-RETIREMENT-DEFERRED.md` (item #10 family): "Phase 2 - light-volume dynamic-shadow caster cull: admit casters whose shadow volume intersects the view WITHOUT touching the load-bearing `inView` cull cascade (`cull_gates_are_load_bearing`); fixes the documented Phase-1 off-screen-caster low-sun gap. Non-deferrable follow-up, own slice, mandated review." Commit:
```
git add docs/superpowers/VPL-RETIREMENT-DEFERRED.md
git commit -m "docs: track Phase 2 (light-volume shadow caster cull) follow-up"
```

---

## Self-review

- **Spec coverage:** frustum-fit (Task 2) / clipToWorld+swizzle (Task 5 Step 2) / fixed elevation slab + map clamp (Task 2 Step 3) / pow2 anti-shimmer + center snap order (Task 2 Step 3) / [0,1] z-row + up-hint guard preserved (Task 2 Step 3) / two new depth shaders (Task 1) / non-indirect static-prop draw (Task 3) / mech previous-fenced-slot (Task 4) / new pre-cull GPU-gated region (Task 5) / [SHADOWFIT v1] probe (Task 6) / phased scope + Phase-2 filing (Task 7) - all mapped.
- **Placeholder scan:** the `/* ... per grep */` markers are deliberate "fill from the Step-1 grep verbatim" instructions with the surrounding real code given, not TBDs - each is preceded by the exact grep that resolves it. No "add error handling"/"similar to" placeholders.
- **Type consistency:** `buildDynamicLightMatrix(sun, corners[8][3])` signature consistent Task 2<->5; `getShadowMechProg`/`getShadowStaticPropProg` consistent Task 1<->3<->4; `s_shadowTypesDrawn/InstDrawn` consistent Task 3/4<->6; read slot = `s_frameSlot` consistent Task 4<->spec rev3.
- **Open execute-time precision items (not placeholders - explicit verify steps):** exact `makeProgram` signature (Task 1 S5), legacy static-prop `u_instanceBase`-vs-bare-`gl_InstanceID` (Task 3 S2), mech bucket-build reuse shape (Task 4 S3), light-dir accessor name (Task 5 S2). Each has a grep step that resolves it deterministically before the code step.
