# PatchStream M1g: Thin-Record VS-Only Draw Path

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the thin-record terrain draw from GL_PATCHES + TCS + TES (~21ms GPU, causes CPU fence stall) to a dedicated GL_TRIANGLES vertex shader path (~3-5ms GPU expected), eliminating the performance regression introduced by M1f.

**Architecture:** Create `gos_terrain_thin.vert` — a vertex shader that reads ThinRecordBuf and RecipeBuf SSBOs via `gl_VertexID`, decodes corner attributes, and applies the same double-projection as TES. Link it with the existing `gos_terrain.frag` (no tessellation stages). Add bridge accessors in `gameos_graphics.cpp` to load the program and set its uniforms. Switch the patchstream thin draw path from `gos_terrain_bridge_drawSingleBucket` (GL_PATCHES) to a new `gos_terrain_bridge_drawSingleBucketTriangles` (GL_TRIANGLES).

**Tech Stack:** OpenGL 4.3, GLSL 430, C++17, existing bridge pattern in `gos_terrain_bridge.h`/`gameos_graphics.cpp`.

---

## Context for the implementer

You are working in the nifty-mendeleev worktree at:
`A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/`

The relevant background:

**M1d** added a "thin record" GPU draw path for terrain: the CPU emits compact 48-byte `TerrainQuadThinRecord`s per quad (lighting, fog, recipe index), plus a 144-byte `TerrainQuadRecipe` per unique quad (world positions, normals, UVs). The TCS (`gos_terrain.tesc`) reads these SSBOs via `gl_PrimitiveID` to reconstruct vertices for the tessellation pipeline.

**M1f** (already merged) gates the CPU's `addVertices(DRAWSOLID)` calls in `quad.cpp` behind `TerrainPatchStream::isFastPathActive()`, so when the thin-record GPU draw is active those CPU VBO copies are skipped.

**M1g problem:** The current thin-record draw issues `glDrawArrays(GL_PATCHES, 0, N)` through the TCS + TES pipeline. On AMD RX 7900 XTX, this takes ~21ms GPU time for 5938 quads (11876 patches), which is 5× slower than the pre-existing expanded-vertex path. The CPU then blocks on the slot fence in `beginFrame()` for ~21ms each frame. The tessellation overhead comes from: divergent SSBO reads in TCS, scatter-gather memory access patterns, and tessellation unit throughput limitations on RDNA3.

**M1g fix:** A dedicated vertex shader (`gos_terrain_thin.vert`) reads the same SSBOs but using `gl_VertexID` arithmetic, bypassing the tessellation pipeline entirely. The fragment shader (`gos_terrain.frag`) is reused unchanged — same PBR splatting, same shadows, same fog.

**Known limitation of the thin path (vs tessellation):** Phong tessellation smoothing (`tessDisplace.x`) and texture-based vertex displacement (`tessDisplace.y`) are TES features. The thin VS skips them. At `tessLevel=1` (current default), Phong smoothing has zero effect. Texture displacement may produce subtle terrain surface differences on dirt-heavy missions.

**Critical rules (from CLAUDE.md):**
- Build: ALWAYS `--config RelWithDebInfo`
- Shader `#version`: NEVER in shader files; the material loader prepends `#version 430\n`
- `setFloat`/`setInt` BEFORE `apply()`, not after
- `terrainMVP` uploaded with `GL_FALSE` (row-major cancels to correct math)
- Shader hot-reload fails silently: bad compile = old shader stays active; always check console

**Build command:**
```
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build --config RelWithDebInfo --target mc2 -j8
```

**Deploy:** Use `/mc2-deploy` skill (or read `.claude/skills/mc2-deploy.md` and follow manually). Deploy dir: `A:/Games/mc2-opengl/mc2-win64-v0.2/`.

**Smoke test:**
```
py -3 A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/scripts/run_smoke.py --tier tier1 --kill-existing
```
(skip `--with-menu-canary` — it has a pre-existing timeout bug unrelated to this feature)

---

## Files

- **Create:** `shaders/gos_terrain_thin.vert`
- **Modify:** `GameOS/gameos/gos_terrain_bridge.h` — add 3 bridge function declarations
- **Modify:** `GameOS/gameos/gameos_graphics.cpp` — add thin program load, uniform struct, bind function, bridge impls
- **Modify:** `GameOS/gameos/gos_terrain_patch_stream.cpp` — switch thin draw to GL_TRIANGLES
- **Modify:** `shaders/gos_terrain.tesc` — remove dead `useQuadRecords==2` branch

---

## Task 1: Create gos_terrain_thin.vert

**Files:**
- Create: `shaders/gos_terrain_thin.vert`

- [ ] **Step 1: Create the file**

```glsl
//#version 430 (version provided by material prefix)

// --- SSBO bindings (must match TerrainQuadThinRecord / TerrainQuadRecipe in gos_terrain_patch_stream.h) ---
struct TerrainQuadThinRecord {
    uvec4 control;    // x=recipeIdx, y=terrainHandle, z=flags(bit0=uvMode,bit1=pzTri1,bit2=pzTri2), w=_pad0
    uvec4 lightRGBs;  // corners 0-3, packed ARGB
    uvec4 fogRGBs;    // corners 0-3, packed frgb
};
layout(std430, binding = 2) readonly buffer ThinRecordBuf {
    TerrainQuadThinRecord thinRecs[];
};

struct TerrainQuadRecipe {
    vec4 worldPos0, worldPos1, worldPos2, worldPos3;
    vec4 worldNorm0, worldNorm1, worldNorm2, worldNorm3;
    vec4 uvData;  // minU, minV, maxU, maxV
};
layout(std430, binding = 1) readonly buffer RecipeBuf {
    TerrainQuadRecipe recipes[];
};

// Output varyings — names MUST match gos_terrain.frag `in` declarations exactly.
out vec4  Color;
out float FogValue;
out vec2  Texcoord;
out float TerrainType;
out vec3  WorldNorm;
out vec3  WorldPos;
out float UndisplacedDepth;

// Uniforms used by this shader
uniform int  ssboRecordBase;     // global record index offset for this draw call
uniform mat4 terrainMVP;         // axisSwap * worldToClip
uniform vec4 terrainViewport;    // (vmx, vmy, vax, vay) for perspective projection
uniform mat4 mvp;                // projection_: screen pixels -> NDC

// Unpack ARGB uint to vec4 each component 0..255 -> 0..1.
vec4 unpackARGB(uint packed) {
    return vec4(
        float((packed >> 16u) & 0xFFu) / 255.0,  // R
        float((packed >>  8u) & 0xFFu) / 255.0,  // G
        float((packed       ) & 0xFFu) / 255.0,  // B
        float((packed >> 24u) & 0xFFu) / 255.0   // A
    );
}

// Get uvec4 component by index 0-3.
uint uvec4Idx(uvec4 v, uint idx) {
    if (idx == 0u) return v.x;
    if (idx == 1u) return v.y;
    if (idx == 2u) return v.z;
    return v.w;
}

void main() {
    uint vid          = uint(gl_VertexID);
    uint vertInRecord = vid % 6u;
    uint triIdx       = vertInRecord / 3u;
    uint id           = vertInRecord % 3u;
    uint recordIdx    = uint(ssboRecordBase) + vid / 6u;

    TerrainQuadThinRecord tr = thinRecs[recordIdx];
    uint flags   = tr.control.z;
    uint uvMode  = flags & 1u;
    uint pzTri1  = (flags >> 1u) & 1u;
    uint pzTri2  = (flags >> 2u) & 1u;
    uint pzValid = (triIdx == 0u) ? pzTri1 : pzTri2;

    // pz-culled triangles: degenerate position (behind near clip, never rasterized).
    if (pzValid == 0u) {
        gl_Position    = vec4(0.0, 0.0, -2.0, 1.0);
        Color          = vec4(0.0);
        FogValue       = 0.0;
        Texcoord       = vec2(0.0);
        TerrainType    = 0.0;
        WorldNorm      = vec3(0.0, 0.0, 1.0);
        WorldPos       = vec3(0.0);
        UndisplacedDepth = 0.0;
        return;
    }

    uint recipeIdx = tr.control.x;
    TerrainQuadRecipe rec = recipes[recipeIdx];

    // Corner index table — same convention as gos_terrain.tesc thin path.
    // TOPRIGHT  (uvMode=0): tri0=corners[0,1,2], tri1=corners[0,2,3]
    // BOTTOMLEFT(uvMode=1): tri0=corners[0,1,3], tri1=corners[1,2,3]
    uint cornerIdx;
    if (uvMode == 0u) {
        if (triIdx == 0u) {
            cornerIdx = (id == 0u) ? 0u : (id == 1u) ? 1u : 2u;
        } else {
            cornerIdx = (id == 0u) ? 0u : (id == 1u) ? 2u : 3u;
        }
    } else {
        if (triIdx == 0u) {
            cornerIdx = (id == 0u) ? 0u : (id == 1u) ? 1u : 3u;
        } else {
            cornerIdx = (id == 0u) ? 1u : (id == 1u) ? 2u : 3u;
        }
    }

    // World position and normal from recipe corners.
    vec4 wp = (cornerIdx == 0u) ? rec.worldPos0
             :(cornerIdx == 1u) ? rec.worldPos1
             :(cornerIdx == 2u) ? rec.worldPos2
             :                    rec.worldPos3;
    vec4 wn = (cornerIdx == 0u) ? rec.worldNorm0
             :(cornerIdx == 1u) ? rec.worldNorm1
             :(cornerIdx == 2u) ? rec.worldNorm2
             :                    rec.worldNorm3;
    vec3 worldPos  = wp.xyz;
    vec3 worldNorm = normalize(wn.xyz);

    // UV reconstruction (verified against quad.cpp actual UV assignment):
    //   corner 0=(minU,minV), corner 1=(maxU,minV), corner 2=(maxU,maxV), corner 3=(minU,maxV)
    float u = (cornerIdx == 1u || cornerIdx == 2u) ? rec.uvData.z : rec.uvData.x;
    float v = (cornerIdx == 0u || cornerIdx == 1u) ? rec.uvData.y : rec.uvData.w;

    // Lighting and fog per corner.
    uint lrgb = uvec4Idx(tr.lightRGBs, cornerIdx);
    uint frgb = uvec4Idx(tr.fogRGBs,   cornerIdx);

    Color       = unpackARGB(lrgb);
    FogValue    = float((frgb >> 24u) & 0xFFu) / 255.0;
    Texcoord    = vec2(u, v);
    TerrainType = float(frgb & 0xFFu);
    WorldNorm   = worldNorm;
    WorldPos    = worldPos;

    // Double-projection — identical to TES, minus displacement (thin path skips it).
    // No displacement => UndisplacedDepth == actual depth.
    vec4 clip = terrainMVP * vec4(worldPos, 1.0);
    float rhw = 1.0 / clip.w;
    vec3 screen;
    screen.x = clip.x * rhw * terrainViewport.x + terrainViewport.z;
    screen.y = clip.y * rhw * terrainViewport.y + terrainViewport.w;
    screen.z = clip.z * rhw;
    vec4 ndc = mvp * vec4(screen, 1.0);
    float absW = abs(clip.w);
    gl_Position      = vec4(ndc.xyz * absW, absW);
    UndisplacedDepth = screen.z * 0.5 + 0.5;
}
```

- [ ] **Step 2: Verify no `#version` directive is in the file**

Run:
```
grep -n "^#version" shaders/gos_terrain_thin.vert
```
Expected: no output (the comment line `// #version 430` is fine, `^#version` won't match it).

- [ ] **Step 3: Commit**

```bash
git add shaders/gos_terrain_thin.vert
git commit -m "feat(patchstream): M1g — add gos_terrain_thin.vert VS-only thin draw shader"
```

---

## Task 2: gosRenderer — thin program member + load + uniform bind

**Files:**
- Modify: `GameOS/gameos/gameos_graphics.cpp`

All edits are in `gameos_graphics.cpp`. There is no separate `.h` for gosRenderer — the class is defined entirely in that file.

- [ ] **Step 1: Add ThinTerrainUniformLocs struct and members to gosRenderer**

Find the `TerrainUniformLocs` struct definition (around line 1553):
```cpp
        struct TerrainUniformLocs {
            GLint tessLevel = -1, tessDistanceRange = -1, tessDisplace = -1;
```

Add AFTER the closing `} terrainLocs_;` line (after line 1564):

```cpp
        struct ThinTerrainUniformLocs {
            GLint terrainMVP = -1, terrainViewport = -1, mvp = -1;
            GLint cameraPos = -1, terrainLightDir = -1;
            GLint detailNormalTiling = -1, detailNormalStrength = -1;
            GLint pomParams = -1, terrainWorldScale = -1, cellBombParams = -1;
            GLint matNormal[5] = {-1,-1,-1,-1,-1};
            GLint tex1 = -1;
            GLint lightSpaceMatrix = -1, enableShadows = -1, shadowSoftness = -1, shadowMap = -1;
            GLint dynamicLightSpaceMatrix = -1, enableDynamicShadows = -1, dynamicShadowMap = -1;
            GLint time = -1, mapHalfExtent = -1;
            GLint ssboRecordBase = -1;
            GLuint program = 0;
        } thinTerrainLocs_;
```

- [ ] **Step 2: Add thin_terrain_prog_ member**

Find `gosRenderMaterial* terrain_material_ = nullptr;` (around line 1528).

Add immediately after it:
```cpp
        glsl_program* thin_terrain_prog_ = nullptr;  // gos_terrain_thin.vert + gos_terrain.frag
```

- [ ] **Step 3: Add method declarations to gosRenderer**

Find `void terrainBindUniformsForPatchStream(gosRenderMaterial* material);` (around line 1286).

Add immediately after it:
```cpp
        // Returns ssboRecordBase uniform location in the thin program, or -1.
        int terrainBindThinUniformsForPatchStream();
        void cacheThinTerrainUniformLocations(GLuint shp);
```

- [ ] **Step 4: Implement cacheThinTerrainUniformLocations**

Find `void cacheTerrainUniformLocations(GLuint shp) {` (around line 1574).

Add after its closing `}`:

```cpp
        void cacheThinTerrainUniformLocations(GLuint shp) {
            if (thinTerrainLocs_.program == shp) return;
            thinTerrainLocs_.program            = shp;
            thinTerrainLocs_.terrainMVP         = glGetUniformLocation(shp, "terrainMVP");
            thinTerrainLocs_.terrainViewport    = glGetUniformLocation(shp, "terrainViewport");
            thinTerrainLocs_.mvp                = glGetUniformLocation(shp, "mvp");
            thinTerrainLocs_.cameraPos          = glGetUniformLocation(shp, "cameraPos");
            thinTerrainLocs_.terrainLightDir    = glGetUniformLocation(shp, "terrainLightDir");
            thinTerrainLocs_.detailNormalTiling   = glGetUniformLocation(shp, "detailNormalTiling");
            thinTerrainLocs_.detailNormalStrength = glGetUniformLocation(shp, "detailNormalStrength");
            thinTerrainLocs_.pomParams          = glGetUniformLocation(shp, "pomParams");
            thinTerrainLocs_.terrainWorldScale  = glGetUniformLocation(shp, "terrainWorldScale");
            thinTerrainLocs_.cellBombParams     = glGetUniformLocation(shp, "cellBombParams");
            thinTerrainLocs_.matNormal[0]       = glGetUniformLocation(shp, "matNormal0");
            thinTerrainLocs_.matNormal[1]       = glGetUniformLocation(shp, "matNormal1");
            thinTerrainLocs_.matNormal[2]       = glGetUniformLocation(shp, "matNormal2");
            thinTerrainLocs_.matNormal[3]       = glGetUniformLocation(shp, "matNormal3");
            thinTerrainLocs_.matNormal[4]       = glGetUniformLocation(shp, "matNormal4");
            thinTerrainLocs_.tex1               = glGetUniformLocation(shp, "tex1");
            thinTerrainLocs_.lightSpaceMatrix   = glGetUniformLocation(shp, "lightSpaceMatrix");
            thinTerrainLocs_.enableShadows      = glGetUniformLocation(shp, "enableShadows");
            thinTerrainLocs_.shadowSoftness     = glGetUniformLocation(shp, "shadowSoftness");
            thinTerrainLocs_.shadowMap          = glGetUniformLocation(shp, "shadowMap");
            thinTerrainLocs_.dynamicLightSpaceMatrix = glGetUniformLocation(shp, "dynamicLightSpaceMatrix");
            thinTerrainLocs_.enableDynamicShadows    = glGetUniformLocation(shp, "enableDynamicShadows");
            thinTerrainLocs_.dynamicShadowMap        = glGetUniformLocation(shp, "dynamicShadowMap");
            thinTerrainLocs_.time               = glGetUniformLocation(shp, "time");
            thinTerrainLocs_.mapHalfExtent      = glGetUniformLocation(shp, "mapHalfExtent");
            thinTerrainLocs_.ssboRecordBase     = glGetUniformLocation(shp, "ssboRecordBase");
        }
```

- [ ] **Step 5: Implement terrainBindThinUniformsForPatchStream**

Find `void gosRenderer::terrainBindUniformsForPatchStream(gosRenderMaterial* material)` (around line 2831). Add a new function AFTER its closing `}` (after line 2920):

```cpp
int gosRenderer::terrainBindThinUniformsForPatchStream()
{
    if (!thin_terrain_prog_ || !thin_terrain_prog_->shp_) return -1;
    GLuint shp = thin_terrain_prog_->shp_;
    glUseProgram(shp);
    cacheThinTerrainUniformLocations(shp);
    const auto& tl = thinTerrainLocs_;

    // VS uniforms: projection chain
    if (terrain_mvp_valid_ && tl.terrainMVP >= 0)
        glUniformMatrix4fv(tl.terrainMVP, 1, GL_FALSE, (const float*)&terrain_mvp_);
    if (tl.terrainViewport >= 0)
        glUniform4fv(tl.terrainViewport, 1, (const float*)&terrain_viewport_);
    if (tl.mvp >= 0)
        glUniformMatrix4fv(tl.mvp, 1, GL_FALSE, (const float*)&projection_);

    // FS uniforms (same as terrainBindUniformsForPatchStream, minus tess-only params)
    if (tl.cameraPos >= 0)        glUniform4fv(tl.cameraPos, 1, (const float*)&terrain_camera_pos_);
    if (tl.terrainLightDir >= 0)  glUniform4fv(tl.terrainLightDir, 1, (const float*)&terrain_light_dir_);
    if (tl.mapHalfExtent >= 0) {
        gosPostProcess* pp = getGosPostProcess();
        float halfExt = pp ? pp->getMapHalfExtent() : 0.0f;
        glUniform1f(tl.mapHalfExtent, halfExt);
    }
    float tiling[4]      = { terrain_detail_tiling_, 0.0f, 0.0f, 0.0f };
    float strength[4]    = { terrain_detail_strength_, 0.0f, 0.0f, 0.0f };
    float pomP[4]        = { terrain_pom_scale_, 8.0f, 32.0f, 0.0f };
    float worldScaleV[4] = { terrain_world_scale_, 0.0f, 0.0f, 0.0f };
    float cellP[4]       = { terrain_cell_scale_, terrain_cell_jitter_, terrain_cell_rotation_, 0.0f };
    if (tl.detailNormalTiling >= 0)   glUniform4fv(tl.detailNormalTiling, 1, tiling);
    if (tl.detailNormalStrength >= 0) glUniform4fv(tl.detailNormalStrength, 1, strength);
    if (tl.pomParams >= 0)            glUniform4fv(tl.pomParams, 1, pomP);
    if (tl.terrainWorldScale >= 0)    glUniform4fv(tl.terrainWorldScale, 1, worldScaleV);
    if (tl.cellBombParams >= 0)       glUniform4fv(tl.cellBombParams, 1, cellP);
    if (tl.time >= 0) {
        float elapsed = (float)(timing::get_wall_time_ms() - timeStart_) / 1000.0f;
        glUniform1f(tl.time, elapsed);
    }
    if (tl.tex1 >= 0) glUniform1i(tl.tex1, 0);
    for (int i = 0; i < 5; i++) {
        if (terrain_mat_normal_[i] != 0 && tl.matNormal[i] >= 0) {
            glUniform1i(tl.matNormal[i], 5 + i);
            glActiveTexture(GL_TEXTURE5 + i);
            glBindTexture(GL_TEXTURE_2D, terrain_mat_normal_[i]);
        }
    }
    glActiveTexture(GL_TEXTURE0);

    gosPostProcess* pp = getGosPostProcess();
    if (pp && pp->shadowsEnabled_) {
        if (tl.lightSpaceMatrix >= 0)
            glUniformMatrix4fv(tl.lightSpaceMatrix, 1, GL_FALSE, pp->getLightSpaceMatrix());
        if (tl.enableShadows >= 0)  glUniform1i(tl.enableShadows, 1);
        if (tl.shadowSoftness >= 0) glUniform1f(tl.shadowSoftness, terrain_shadow_softness_);
        if (tl.shadowMap >= 0) {
            glUniform1i(tl.shadowMap, 9);
            glActiveTexture(GL_TEXTURE9);
            glBindTexture(GL_TEXTURE_2D, pp->getShadowTexture());
            glActiveTexture(GL_TEXTURE0);
        }
        if (pp->getDynamicShadowFBO()) {
            if (tl.dynamicLightSpaceMatrix >= 0)
                glUniformMatrix4fv(tl.dynamicLightSpaceMatrix, 1, GL_FALSE,
                                   pp->getDynamicLightSpaceMatrix());
            if (tl.enableDynamicShadows >= 0) glUniform1i(tl.enableDynamicShadows, 1);
            if (tl.dynamicShadowMap >= 0) {
                glUniform1i(tl.dynamicShadowMap, 10);
                glActiveTexture(GL_TEXTURE10);
                glBindTexture(GL_TEXTURE_2D, pp->getDynamicShadowTexture());
                glActiveTexture(GL_TEXTURE0);
            }
        } else {
            if (tl.enableDynamicShadows >= 0) glUniform1i(tl.enableDynamicShadows, 0);
        }
    } else {
        if (tl.enableShadows >= 0)        glUniform1i(tl.enableShadows, 0);
        if (tl.enableDynamicShadows >= 0) glUniform1i(tl.enableDynamicShadows, 0);
    }

    return tl.ssboRecordBase;
}
```

- [ ] **Step 6: Load thin program in gosRenderer::init()**

Find the block that loads `shadow_terrain_material_` (around line 1935):
```cpp
    // Load shadow terrain material (VS+FS only, no tessellation)
    {
```

Add a new block AFTER the closing `}` of the shadow_terrain_material_ load:

```cpp
    // Load thin-record terrain program (gos_terrain_thin.vert + gos_terrain.frag, no tess).
    // Used by PatchStream M1g to draw thin records via GL_TRIANGLES, avoiding tessellation overhead.
    {
        ZoneScopedN("gosRenderer::init thinTerrainProg");
        static const char* kThinPrefix = "#version 430\n";
        thin_terrain_prog_ = glsl_program::makeProgram(
            "gos_terrain_thin",
            "shaders/gos_terrain_thin.vert",
            "shaders/gos_terrain.frag",
            kThinPrefix);
        if (!thin_terrain_prog_ || !thin_terrain_prog_->shp_)
            fprintf(stderr, "[THIN_TERRAIN] WARNING: failed to compile thin terrain shader"
                            " — thin draw path disabled\n");
        else
            printf("[THIN_TERRAIN] Thin terrain shader loaded: prog=%u\n",
                   (unsigned)thin_terrain_prog_->shp_);
        fflush(stdout);
    }
```

- [ ] **Step 7: Build and verify thin program loads**

```
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build --config RelWithDebInfo --target mc2 -j8 2>&1 | tail -5
```
Expected: build succeeds (exit 0).

Start the game briefly (`A:/Games/mc2-opengl/mc2-win64-v0.2/mc2.exe`) and check console for:
```
[THIN_TERRAIN] Thin terrain shader loaded: prog=N
```
If it shows `WARNING: failed to compile`, the shader has a compile error — check console for the GLSL error.

- [ ] **Step 8: Commit**

```bash
git add GameOS/gameos/gameos_graphics.cpp
git commit -m "feat(patchstream): M1g — add thin terrain program load and uniform bind in gosRenderer"
```

---

## Task 3: Bridge exports for thin program

**Files:**
- Modify: `GameOS/gameos/gos_terrain_bridge.h`
- Modify: `GameOS/gameos/gameos_graphics.cpp`

- [ ] **Step 1: Add declarations to gos_terrain_bridge.h**

Find the end of the file (after the `gos_terrain_bridge_endBucketLoop` declaration):

Add:
```cpp
// Returns the GL program ID of the thin-record VS-only terrain shader (gos_terrain_thin.vert +
// gos_terrain.frag). Returns 0 if not yet loaded or failed to compile.
unsigned int gos_terrain_bridge_getThinShaderProgram();

// Switches the active GL program to the thin terrain program and sets all uniforms
// (projection, cameraPos, shadow maps, PBR params — same as the tessellation path minus tess-only).
// Returns the ssboRecordBase uniform location in the thin program (for per-bucket setting),
// or -1 if the thin program is not ready.
// IMPORTANT: call gos_terrain_bridge_beginBucketLoop() first to dirty the Z/blend states.
int gos_terrain_bridge_bindThinUniforms();

// Like gos_terrain_bridge_drawSingleBucket but issues glDrawArrays(GL_TRIANGLES, ...).
// Binds gosHandle texture, calls applyRenderStates(), then draws. Does NOT change glUseProgram —
// assumes gos_terrain_bridge_bindThinUniforms() was already called.
void gos_terrain_bridge_drawSingleBucketTriangles(
    unsigned int gosHandle,
    unsigned int firstVertex,
    unsigned int vertexCount);
```

- [ ] **Step 2: Implement the three bridge functions in gameos_graphics.cpp**

Find `void gos_terrain_bridge_endBucketLoop(unsigned int lastGosHandle)` and its implementation block. Add after it:

```cpp
unsigned int gos_terrain_bridge_getThinShaderProgram() {
    if (!g_gos_renderer) return 0;
    glsl_program* p = g_gos_renderer->thin_terrain_prog_;
    return (p && p->shp_) ? (unsigned int)p->shp_ : 0u;
}

int gos_terrain_bridge_bindThinUniforms() {
    if (!g_gos_renderer) return -1;
    return g_gos_renderer->terrainBindThinUniformsForPatchStream();
}

void gos_terrain_bridge_drawSingleBucketTriangles(
    unsigned int gosHandle,
    unsigned int firstVertex,
    unsigned int vertexCount)
{
    if (!g_gos_renderer || vertexCount == 0) return;
    g_gos_renderer->setRenderState(gos_State_Texture, (int)gosHandle);
    g_gos_renderer->applyRenderStates();
    glActiveTexture(GL_TEXTURE0);
    glDrawArrays(GL_TRIANGLES, (GLint)firstVertex, (GLsizei)vertexCount);
}
```

**Note on `thin_terrain_prog_` access:** `gos_terrain_bridge_getThinShaderProgram` accesses `g_gos_renderer->thin_terrain_prog_` directly. Since the bridge functions are implemented in `gameos_graphics.cpp` (where `gosRenderer` is fully defined), this compiles. The field `thin_terrain_prog_` is `public` if declared in the public section, or the bridge function must be declared `friend`. The existing `getTerrainMaterial()` accessor pattern shows how — just add `glsl_program* getThinTerrainProg() const { return thin_terrain_prog_; }` in the gosRenderer class if direct access fails. Prefer the inline accessor form to stay consistent with existing patterns.

- [ ] **Step 3: Build**

```
cmake.exe --build build --config RelWithDebInfo --target mc2 -j8 2>&1 | tail -5
```
Expected: builds cleanly.

- [ ] **Step 4: Commit**

```bash
git add GameOS/gameos/gos_terrain_bridge.h GameOS/gameos/gameos_graphics.cpp
git commit -m "feat(patchstream): M1g — add thin terrain bridge exports (getThinProgram, bindThinUniforms, drawSingleBucketTriangles)"
```

---

## Task 4: Switch thin draw path in patchstream.cpp to GL_TRIANGLES

**Files:**
- Modify: `GameOS/gameos/gos_terrain_patch_stream.cpp`

The section to replace is the `// --- M1d thin-record draw path ---` block inside `flush()`. It currently:
1. Resolves `s_useQuadRecordsLoc2` and `s_ssboRecordBaseLoc2` from the tessellation program
2. Sets `useQuadRecords = 2` on the tessellation program
3. Calls `gos_terrain_bridge_drawSingleBucket(...)` which issues `glDrawArrays(GL_PATCHES, ...)`

Replace that section entirely.

- [ ] **Step 1: Read the current thin draw section**

Find the `// --- M1d thin-record draw path ---` comment in `flush()` (around line 1313). Read from there to the closing `}` of the thin draw `if` block (around line 1446 — just before the parity log).

The block starts with:
```cpp
    // --- M1d thin-record draw path ---
    if (s_thinRecordsOn && s_thinRecordsDrawOn && s_thinRecordBuf && s_recipeBuf
            && s_thinRecordCount > 0) {
```

And includes the sort, gather, upload-to-SSBO, then the conditional draw logic.

The sort/gather/upload section (lines 1316–1363) stays unchanged. Only the draw section changes.

- [ ] **Step 2: Replace the draw section**

Find the "Retrieve useQuadRecords / ssboRecordBase" comment block and everything after it (starting at the static GLint declarations, through the closing `}` of the `if (s_useQuadRecordsLoc2 >= 0)` block). Replace it with:

```cpp
        // M1g: draw via dedicated thin VS + GL_TRIANGLES (no tessellation overhead).
        unsigned int thinProg = gos_terrain_bridge_getThinShaderProgram();
        if (thinProg != 0) {
            ZoneScopedN("PatchStream.DrawThinRecords.Buckets");

            // Mark invariant GL states dirty (Z, blend, etc.). applyRenderStates in
            // drawSingleBucketTriangles will flush them without touching glUseProgram.
            gos_terrain_bridge_beginBucketLoop();

            // Switch to thin VS program and set all uniforms.
            GLint ssboBaseLoc = gos_terrain_bridge_bindThinUniforms();

            // Bind SSBOs once (same across all buckets — slot offset applied per bucket
            // via ssboRecordBase).
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, s_thinRecordBuf);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, s_recipeBuf);

            for (uint32_t b = 0; b < thinRecDrawBucketCount; ++b) {
                const ThinRecBucket& rb = s_thinRecDrawBuckets[b];
                if (ssboBaseLoc >= 0)
                    glUniform1i(ssboBaseLoc,
                                (GLint)(slotFirstThinRec + rb.firstRecord));
                const GLsizei vertCount = (GLsizei)(rb.recordCount * 6u);
                gos_terrain_bridge_drawSingleBucketTriangles(
                    (unsigned int)rb.gosHandle, 0u, (unsigned int)vertCount);
            }

            gos_terrain_bridge_endBucketLoop(0xFFFFFFFFu);

            // Unbind SSBOs and restore neutral program.
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, 0);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, 0);
            glUseProgram(0);
        } else {
            fprintf(stderr,
                "[PATCH_STREAM v1] event=thin_record_draw_skip reason=no_thin_program\n");
            fflush(stderr);
        }
```

- [ ] **Step 3: Build**

```
cmake.exe --build build --config RelWithDebInfo --target mc2 -j8 2>&1 | tail -5
```
Expected: builds cleanly. If there are errors about `thinRecDrawBucketCount` scope or `ThinRecBucket` visibility, move the declaration earlier in the block or verify you're editing inside the correct `if` block.

- [ ] **Step 4: Smoke-test thin draw (standard path only, no fast path)**

```
set MC2_PATCHSTREAM_THIN_RECORDS=1& set MC2_PATCHSTREAM_THIN_RECORDS_DRAW=1& "A:\Games\mc2-opengl\mc2-win64-v0.2\mc2.exe"
```

Load mc2_01. Terrain should render correctly (same appearance as standard path). Check console:
- `[THIN_TERRAIN] Thin terrain shader loaded:` — present at startup
- NO `[PATCH_STREAM v1] event=thin_record_draw_skip reason=no_thin_program` lines
- No GL error lines (`event=thin_record_post_glerror`)

- [ ] **Step 5: Commit**

```bash
git add GameOS/gameos/gos_terrain_patch_stream.cpp
git commit -m "feat(patchstream): M1g — switch thin-record draw from GL_PATCHES+TCS+TES to GL_TRIANGLES via thin VS"
```

---

## Task 5: TCS cleanup — remove dead useQuadRecords==2 branch

**Files:**
- Modify: `shaders/gos_terrain.tesc`

The `useQuadRecords == 2` branch in `gos_terrain.tesc` is now dead code — patchstream.cpp no longer sets `useQuadRecords=2` anywhere.

- [ ] **Step 1: Remove the useQuadRecords==2 block from gos_terrain.tesc**

Find the block starting with:
```glsl
    // --- Thin record path (MC2_PATCHSTREAM_THIN_RECORDS_DRAW=1) ---
    if (useQuadRecords == 2) {
```

Through its closing `return;` and `}`.

Delete the entire block (from the comment line through the closing `}`). Also remove the `ThinRecordBuf` and `RecipeBuf` SSBO layout declarations and their struct definitions (they moved to `gos_terrain_thin.vert`).

After removal, verify the file still has:
- The `TerrainQuadRecord` struct and `QuadRecordBuf` at binding=0 — **keep these** (fat record path still uses them)
- The `useQuadRecords == 0` passthrough block — **keep**
- The `useQuadRecords == 1` fat-record draw block — **keep**

The `ThinRecordBuf` and `RecipeBuf` struct definitions and layout declarations can be removed since they're no longer needed in the TCS.

- [ ] **Step 2: Build and verify TCS compiles**

```
cmake.exe --build build --config RelWithDebInfo --target mc2 -j8 2>&1 | tail -5
```

- [ ] **Step 3: Deploy**

Use the `/mc2-deploy` skill or read `.claude/skills/mc2-deploy.md` and follow manually.

The deploy copies `mc2.exe` and all shaders (including `gos_terrain.tesc` and new `gos_terrain_thin.vert`) to `A:/Games/mc2-opengl/mc2-win64-v0.2/`.

Verify the thin shader is deployed:
```bash
ls "A:/Games/mc2-opengl/mc2-win64-v0.2/shaders/gos_terrain_thin.vert"
```
Expected: file exists.

- [ ] **Step 4: Standard-path smoke (5/5)**

```
py -3 A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/scripts/run_smoke.py --tier tier1 --kill-existing
```
Expected: exit 0, all 5 tier1 missions pass.

- [ ] **Step 5: Thin-path visual verification**

```
set MC2_PATCHSTREAM_THIN_RECORDS=1
set MC2_PATCHSTREAM_THIN_RECORDS_DRAW=1
"A:\Games\mc2-opengl\mc2-win64-v0.2\mc2.exe"
```

Load mc2_01. Verify:
- Terrain renders correctly (PBR splatting, shadows visible)
- No visual corruption or missing terrain
- Check console: `[THIN_TERRAIN] Thin terrain shader loaded:` present, no GL errors

- [ ] **Step 6: Fast-path + thin smoke (perf regression gate)**

```
set MC2_PATCHSTREAM_THIN_RECORDS=1
set MC2_PATCHSTREAM_THIN_RECORDS_DRAW=1
set MC2_PATCHSTREAM_THIN_RECORD_FASTPATH=1
py -3 A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/scripts/run_smoke.py --tier tier1 --kill-existing
```
Expected: exit 0. Also note the FPS reported in smoke artifacts — it should be ≥ the standard-path baseline (not the 25fps regression of the old thin+tess path).

- [ ] **Step 7: Commit cleanup + results note**

```bash
git add shaders/gos_terrain.tesc
git commit -m "chore(patchstream): M1g — remove dead useQuadRecords==2 branch from TCS (replaced by thin VS path)"
```

---

## Self-review checklist

**Spec coverage:**
- [ ] `gos_terrain_thin.vert` reads ThinRecordBuf(binding=2) and RecipeBuf(binding=1) via gl_VertexID
- [ ] Corner decode matches TCS thin path corner table exactly (TOPRIGHT vs BOTTOMLEFT)
- [ ] pz-culled triangles get degenerate position (not rendered)
- [ ] Double-projection (terrainMVP → screen → mvp) matches TES
- [ ] UndisplacedDepth = screen.z * 0.5 + 0.5 (no displacement in VS → always correct)
- [ ] Output varyings match gos_terrain.frag inputs exactly (Color, FogValue, Texcoord, TerrainType, WorldNorm, WorldPos, UndisplacedDepth)
- [ ] Thin program loaded in gosRenderer::init() with `#version 430\n` prefix (no #version in shader file)
- [ ] All FS uniforms set in terrainBindThinUniformsForPatchStream (shadows, PBR, fog)
- [ ] glUseProgram(0) called after thin draw loop
- [ ] SSBOs unbound after thin draw (binding 1 and 2)
- [ ] gos_terrain.tesc useQuadRecords==2 block removed
- [ ] Standard-path smoke 5/5

**Placeholder scan:** No TBD, no "similar to Task N", all code blocks complete.

**Type consistency:** `ThinRecBucket` type used in Task 4 is the same as defined in the existing flush() thin section (it's a `struct ThinRecBucket { DWORD gosHandle; uint32_t firstRecord; uint32_t recordCount; }` defined as a local static in that block). Verify the name matches what's actually in the file before editing.
