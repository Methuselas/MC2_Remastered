# FOLIAGE-STATICPROP-DEPTH-PREPASS-1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a gated camera depth-prepass for static props so the expensive
color/lighting fragment shader runs ~once per visible pixel instead of once per
overlapping leaf/canopy layer (kills the foliage overdraw multiplier).

**Architecture:** Two-pass static-prop render. A new cheap depth-only program
(`static_prop.vert` reused verbatim + new `static_prop_depth.frag` that does only
the alpha-test discard) lays down depth (GEQUAL, write-on, color masked off).
Then the existing color `flush()` switches to `GL_EQUAL` + depthWrite-off so
early-Z rejects all hidden fragments. The prepass mirrors the default v6/coalesced
dispatch by iterating the SAME dispatch lists the color pass uses (identical draw
set by construction). Everything is behind `MC2_STATIC_PROP_DEPTH_PREPASS`,
default OFF.

**Tech Stack:** C++ (GameOS/RenderCore), GLSL (OpenGL 4.x, reverse-Z), CMake
RelWithDebInfo build, `run_smoke.py` integration validation, Tracy GPU zones,
screenshot parity diff.

**Spec:** `docs/superpowers/specs/2026-06-04-foliage-depth-prepass-overdraw-design.md`

---

## Validation model (read first — this codebase has no unit-test harness)

There is **no pytest/gtest** rig for the GL renderer. "Tests" here are:
- **Build:** the `mc2-build` skill — `cmake --build build64 --config RelWithDebInfo --target mc2` (exit 0, no new errors).
- **Smoke:** `python scripts/run_smoke.py --exe "A:/Games/mc2-opengl/mc2-win64-v0.3/mc2.exe" --mission <m>` → must report `PASS` and `Δ destroys +0`, GL-clean.
- **Parity (THE gate):** screenshot of a static camera with prepass OFF vs ON — no prop may vanish, lighting identical for surviving fragments.
- **Tracy:** `Render.GpuStaticProps` GPU self-time + the new prepass/color zones, OFF vs ON, on a near-foliage camera.

Deploy after each build: copy `build64/RelWithDebInfo/mc2.exe` + changed shaders to
`A:/Games/mc2-opengl/mc2-win64-v0.3/` (use the `mc2-deploy` flow but TARGET v0.3,
copying shaders one-at-a-time with `cp -f`, never `cp -r`).

**Commit after every task.** Default-OFF means every intermediate commit is safe
(the prepass code is dead until the gate flips).

---

## File Structure

| File | Responsibility | Action |
|---|---|---|
| `shaders/static_prop_depth.frag` | Depth-only fragment: alpha-test discard only, no color/lighting/object-id | Create |
| `shaders/static_prop.vert` | Shared VS — add `invariant gl_Position;` | Modify |
| `RenderCore/PipelineRegistry.h` | Add `StaticPropDepth` enum value | Modify |
| `RenderCore/PipelineRegistry.cpp` | Add `StaticPropDepth` desc row | Modify |
| `GameOS/gameos/gos_static_prop_batcher.cpp` | Depth program creation, `s_locsDepthCoalesce`, `flushDepthPrepass()`, color-pass EQUAL override, gate, Tracy zones | Modify |
| `GameOS/gameos/gos_static_prop_batcher.h` | Declare `flushDepthPrepass()` | Modify |
| `mclib/txmmgr.cpp` | Call `flushDepthPrepass()` between `compute_dispatch()` and `flush()` | Modify |
| `RenderCore/RendererFeatureRegistry.h` | Register `MC2_STATIC_PROP_DEPTH_PREPASS` gate (allowlist) | Modify |

---

## Task 1: Depth-only fragment shader

**Files:**
- Create: `shaders/static_prop_depth.frag`
- Reference (copy the predicate + texture path EXACTLY): `shaders/static_prop.frag:60-216`

The predicate that MUST be byte-identical to the color shader (`static_prop.frag:215`):
```glsl
if (u_debugAddrMode != 8 && (materialFlags & ALPHA_TEST_BIT) != 0 && tex_color.a < 0.5)
    discard;
```

- [ ] **Step 1: Read the color shader's texture/predicate region**

Read `shaders/static_prop.frag` lines ~60-220. Identify: how `materialFlags` is
obtained for BOTH paths (coalesce: `PerDrawEntry[...].materialFlags` via
`gl_DrawIDARB`/`u_drawIDBase`; legacy: `uniform u_materialFlags`), how `tex_color`
is sampled (coalesce: `u_texArr` with the layer; legacy: `u_tex`), the UV varying
name, `ALPHA_TEST_BIT` constant, `u_debugAddrMode` uniform. Note the `#version`,
extensions, and any `include` directives at the top.

- [ ] **Step 2: Write `static_prop_depth.frag`**

Create a fragment shader that:
- Uses the SAME `#version`, extensions, includes, and the SAME inputs/SSBO/uniform
  declarations needed to reproduce `materialFlags` + `tex_color.a` + the same UV
  for BOTH the coalesce and legacy paths (mirror exactly what `static_prop.frag`
  does to derive them — same SSBO bindings, same `u_texArr`/`u_tex`, same
  `u_drawIDBase`, same `u_materialFlags`, same `u_debugAddrMode`).
- Body: sample alpha, run the **identical** predicate above, `discard` on the
  same condition. Otherwise return.
- Declares **no** `out` color attachments (no `layout(location=0/1/2)` outputs).
  No lighting, no fog, no SH-L2, no PBR, no object-id write.

Keep everything not needed for the alpha decision OUT. The whole point is this
shader is cheap.

- [ ] **Step 3: Verify it compiles standalone (build gate)**

Wire it via Task 4 first if the engine only compiles referenced shaders; otherwise
add a temporary reference. Simplest: defer compile-check to Task 4's
`makeProgram` (which links vert+frag) and run the build there. Mark this step done
once Task 4 builds clean.

- [ ] **Step 4: Commit**
```bash
git add shaders/static_prop_depth.frag
git commit -m "feat(shader): static_prop_depth.frag — alpha-test discard only, no color"
```

---

## Task 2: `invariant gl_Position` in the shared vertex shader

**Files:**
- Modify: `shaders/static_prop.vert`

`invariant gl_Position` guarantees the position output is bit-identical across the
two program objects (color + depth) that link this same VS — the precondition for
`GL_EQUAL` to match.

- [ ] **Step 1: Add the invariant qualifier**

After the `#version`/extension lines and before/at `main`, add:
```glsl
invariant gl_Position;
```
(GLSL allows a global `invariant gl_Position;` redeclaration. Place it at global
scope after `gl_Position` is implicitly available — i.e. among the out/varying
declarations, before `main()`.)

- [ ] **Step 2: Build to confirm the VS still compiles in the existing color program**

Run the `mc2-build` flow (`cmake --build build64 --config RelWithDebInfo --target mc2`).
Expected: exit 0. (The shader is compiled at runtime, so a clean C++ build only
proves no C++ breakage; the real shader-compile check is the smoke run in Task 9.
Do a quick smoke of mc2_01 here to confirm static_prop still links with the
invariant qualifier — no `[GPUPROPS] failed to compile/link` in stderr.)

Run: `python scripts/run_smoke.py --exe "A:/Games/mc2-opengl/mc2-win64-v0.3/mc2.exe" --mission mc2_01`
Expected: PASS, and no static_prop link-failure line.

- [ ] **Step 3: Commit**
```bash
git add shaders/static_prop.vert
git commit -m "feat(shader): invariant gl_Position on static_prop.vert (depth-prepass parity precondition)"
```

---

## Task 3: `StaticPropDepth` pipeline registry entry

**Files:**
- Modify: `RenderCore/PipelineRegistry.h:25-32`
- Modify: `RenderCore/PipelineRegistry.cpp:39-96`

- [ ] **Step 1: Add the enum value**

In `PipelineRegistry.h`, insert before `Count_` and bump it:
```cpp
enum class PipelineId : uint32_t {
    Invalid             = 0,
    StaticPropOpaque    = 1,
    StaticPropAlphaTest = 2,
    MechOpaque          = 3,
    StaticPropDepth     = 4,   // camera depth-prepass (depth-only, alpha discard)
    Count_              = 5,   // sentinel — do not use as an ID
};
```

- [ ] **Step 2: Add the desc row**

In `PipelineRegistry.cpp`, add a row `[4]` to the `s_descs` initializer (after the
MechOpaque `[3]` row, before the closing `}}`):
```cpp
    // [4] StaticPropDepth — camera depth-prepass. Depth-only: GEQUAL + write so
    // it lays the nearest reverse-Z depth; alpha discard in static_prop_depth.frag.
    // Color writes are masked off by the caller (glColorMask), NOT by attachment
    // changes — same FBO stays bound. Shares the static-prop SSBOs (instance /
    // per-type / per-draw) because it reuses static_prop.vert.
    {
        /* glProgramName       */ 0u,              // filled by bindProgram()
        /* blend               */ BlendMode::AlphaTest, // discard path; GL_BLEND off
        /* depthTestEnable     */ true,
        /* depthWriteEnable    */ true,
        /* depthFunc           */ DepthFunc::GreaterEqual, // reverse-Z, lay nearest
        /* cullMode            */ CullMode::Back,
        /* colorAttachments    */ { true, true, false },
        /* objectIdWriteEnabled*/ false,
        /* ssboBindingsMask    */ kStaticPropSsbos,
    },
```
(`kStaticPropSsbos` is the same constant the other static-prop rows use — confirm
its name in the file's top constants and reuse it verbatim.)

- [ ] **Step 3: Build to confirm the static_assert (row count == Count_) passes**

Run: `cmake --build build64 --config RelWithDebInfo --target mc2`
Expected: exit 0. The `static_assert(s_descs.size() == Count_)` in
`PipelineRegistry.cpp:98` proves the enum and table stay in sync.

- [ ] **Step 4: Commit**
```bash
git add RenderCore/PipelineRegistry.h RenderCore/PipelineRegistry.cpp
git commit -m "feat(rendercore): StaticPropDepth pipeline desc (depth-prepass, GEQUAL+write)"
```

---

## Task 4: Create the depth program + per-program uniform locations

**Files:**
- Modify: `GameOS/gameos/gos_static_prop_batcher.cpp:495-497` (statics), `:1081-1101` (program creation), `:1160-1175` (coalesce uniform locations)

The depth program is a DISTINCT GL program object (same VS, different frag), so its
uniform locations differ from the color program's. The v6 draw loop uses
`u_drawIDBase`; the prepass must use the **depth program's** location, not the
color program's.

- [ ] **Step 1: Add file-scope statics for the depth program + its locations**

Near the existing program statics (`s_staticPropProgram` ~line 496, and
`s_locsCoalesce` struct used at ~1169), add:
```cpp
GLuint s_staticPropDepthProgram = 0;   // depth-prepass program (static_prop.vert + static_prop_depth.frag)
struct DepthCoalesceLocs { GLint drawIDBase = -1; };
static DepthCoalesceLocs s_locsDepthCoalesce;
```

- [ ] **Step 2: Create + link the depth program next to the color program**

After the color program is created and validated (`gos_static_prop_batcher.cpp:1096`),
add:
```cpp
    // Depth-prepass program: same VS (invariant gl_Position), cheap depth-only frag.
    glsl_program* depthObj = glsl_program::makeProgram(
        "static_prop_depth",
        "shaders/static_prop.vert",
        "shaders/static_prop_depth.frag",
        legacyPrefix.c_str());
    if (depthObj && depthObj->is_valid()) {
        s_staticPropDepthProgram = depthObj->shp_;
        RenderCore::bindProgram(RenderCore::PipelineId::StaticPropDepth, s_staticPropDepthProgram);
        s_locsDepthCoalesce.drawIDBase =
            glGetUniformLocation(s_staticPropDepthProgram, "u_drawIDBase");
    } else {
        std::fprintf(stderr,
            "[GPUPROPS] static_prop_depth program failed to compile/link — "
            "depth-prepass disabled this session (color path unaffected)\n");
        s_staticPropDepthProgram = 0;   // flushDepthPrepass() must no-op when 0
    }
```
(Match the exact `makeProgram` signature + `legacyPrefix` usage from the color
program call above it. If the depth frag references coalesce SSBOs/uniforms the
color program defines, the same `legacyPrefix`/defines must be passed.)

- [ ] **Step 3: Build + smoke to confirm the depth program links**

Build (`--target mc2`), deploy exe + both shaders to v0.3, then:
Run: `python scripts/run_smoke.py --exe "A:/Games/mc2-opengl/mc2-win64-v0.3/mc2.exe" --mission mc2_01`
Expected: PASS, stderr shows NO `static_prop_depth program failed` line (i.e. the
new frag compiled + linked against the shared VS). The prepass isn't called yet,
so behavior is unchanged.

- [ ] **Step 4: Commit**
```bash
git add GameOS/gameos/gos_static_prop_batcher.cpp
git commit -m "feat(batcher): create static_prop_depth program + per-program u_drawIDBase loc"
```

---

## Task 5: `flushDepthPrepass()` — isolated mirror of the v6 dispatch

**Files:**
- Modify: `GameOS/gameos/gos_static_prop_batcher.h` (declare method)
- Modify: `GameOS/gameos/gos_static_prop_batcher.cpp` (implement; mirror the v6 loop at `:5891-5974`)

Design: a standalone method that reuses the SAME dispatch lists
(`pDispatchPackets`/`pDispatchMeta` = the v6Packets/v6Meta or snapshot variants the
color flush iterates) so the draw set is identical by construction. It binds the
depth program via the `StaticPropDepth` pipeline, masks color writes, runs the
minimal per-packet draw loop, then restores color mask + GL state. It is a NO-OP
(returns early, logs once) when the depth program is 0 OR when the active dispatch
path is not v6 (legacy/v5 unsupported in this slice).

- [ ] **Step 1: Declare the method**

In `gos_static_prop_batcher.h`, in the same class/namespace as `flush()`:
```cpp
// Camera depth-prepass: depth-only draw of the v6 static-prop dispatch set
// (alpha-test discard, no color). No-op unless MC2_STATIC_PROP_DEPTH_PREPASS
// is set, the depth program linked, and the v6 path is active.
void flushDepthPrepass();
```

- [ ] **Step 2: Expose the dispatch lists the prepass must reuse**

The color `flush()` builds `pDispatchPackets`/`pDispatchMeta` (local, `:5891`). To
guarantee the prepass draws the identical set, hoist references to those lists to
file scope at the point they're finalized, e.g.:
```cpp
// Set in flush() right where pDispatchPackets/pDispatchMeta are resolved (:5891-5894),
// consumed by flushDepthPrepass() which runs IMMEDIATELY BEFORE flush()'s draw loop
// in the same frame (txmmgr order: prepass -> flush). Cleared at flush() end.
static const std::vector<RenderCore::DrawPacket>*    s_lastV6DispatchPackets = nullptr;
static const std::vector<StaticPropDispatchMeta>*    s_lastV6DispatchMeta    = nullptr;
static uint32_t                                      s_lastV6TotalCmds       = 0u;
static bool                                          s_lastV6Valid           = false;
```
IMPORTANT ordering: the prepass runs BEFORE `flush()` (txmmgr Task 7), but these
lists are built INSIDE `flush()`. Resolve by having `flushDepthPrepass()` build the
SAME lists the same way, OR refactor list-building into a shared helper
`bool buildV6DispatchLists()` that both call. **Pick the shared-helper refactor**:
extract the list resolution (`:5800-5894` region that produces
`pDispatchPackets`/`pDispatchMeta`/`totalCmds`/`skipDispatch`/`useSnapshot`) into
a private helper returning a small struct; call it from both `flushDepthPrepass()`
and `flush()`. This keeps ONE source of truth for the draw set.

(If the refactor is too large for one task, fall back to: prepass reads the lists
that `compute_dispatch()` + the sort already produced — `s_sortedPacketOrder`,
`s_alphaOffCmdCount`/`s_alphaOnCmdCount`, the v6 packet/meta vectors — which are
file-scope and valid after compute_dispatch(). Document which source you used.)

- [ ] **Step 3: Implement `flushDepthPrepass()`**

```cpp
void GpuStaticPropBatcher::flushDepthPrepass()
{
    ZoneScopedN("GpuSP.DepthPrepass");
    // Gate + capability guards. Any false -> no-op; color path stays single-pass.
    static const bool s_prepassEnabled =
        (std::getenv("MC2_STATIC_PROP_DEPTH_PREPASS") != nullptr);
    if (!s_prepassEnabled) return;
    if (s_staticPropDepthProgram == 0) return;          // program didn't link
    if (!s_v6Enabled || !s_v6Armed) {                   // only v6 supported this slice
        static bool warned = false;
        if (!warned) { warned = true;
            std::fprintf(stderr, "[GPUPROPS] depth-prepass skipped: v6 path inactive "
                                 "(legacy/v5 unsupported in this slice)\n"); }
        return;
    }

    // Resolve the SAME dispatch lists the color flush will draw (Step 2 helper).
    const auto disp = buildV6DispatchLists();           // shared with flush()
    if (!disp.valid || disp.totalCmds == 0u) return;

    // Save GL state we touch (color flush saves its own; we restore ours).
    GLboolean prevColorMask[4];
    glGetBooleanv(GL_COLOR_WRITEMASK, prevColorMask);

    // Depth-only: GEQUAL + write via the StaticPropDepth desc; color masked off.
    pipeline_binder::applyPipeline(
        RenderCore::getPipelineDesc(RenderCore::PipelineId::StaticPropDepth));
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glBindVertexArray(s_sharedVao);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_sharedIbo);
    if (s_perTypeSsbo) glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, s_perTypeSsbo);
    glBindTexture(GL_TEXTURE_2D_ARRAY, s_texArrayOff);   // alpha-OFF group default

    bool enteredOnGroup = false;
    for (uint32_t i = 0u; i < disp.totalCmds; ++i) {
        const StaticPropDispatchMeta& m  = (*disp.meta)[i];
        const RenderCore::DrawPacket&  dp = (*disp.packets)[i];
        if (m.instanceCount == 0u) continue;
        // Texture array switch at the alpha-ON boundary (mirror flush :5945-5948).
        if (m.group == 1u && !enteredOnGroup) {
            glBindTexture(GL_TEXTURE_2D_ARRAY, s_texArrayOn);
            enteredOnGroup = true;
        }
        // Depth PROGRAM's own drawIDBase location.
        glUniform1i(s_locsDepthCoalesce.drawIDBase, static_cast<GLint>(m.drawIDBase));
        glDrawElementsInstancedBaseVertexBaseInstance(
            GL_TRIANGLES,
            static_cast<GLsizei>(dp.indexCount), GL_UNSIGNED_INT,
            reinterpret_cast<const void*>(
                static_cast<uintptr_t>(dp.firstIndex) * sizeof(uint32_t)),
            static_cast<GLsizei>(m.instanceCount), m.baseVertex, m.baseInstance);
    }
    if (enteredOnGroup) glBindTexture(GL_TEXTURE_2D_ARRAY, s_texArrayOff);

    // Restore color mask so the color flush writes normally.
    glColorMask(prevColorMask[0], prevColorMask[1], prevColorMask[2], prevColorMask[3]);
    // Mark that the prepass ran this frame so flush() flips to EQUAL/no-write.
    s_depthPrepassRanThisFrame = true;
}
```
Add the file-scope flag `static bool s_depthPrepassRanThisFrame = false;` near the
other v6 statics. (The bc7Buckets / ORM binding path is intentionally OMITTED — the
depth shader does not sample ORM. If `bc7Buckets` is active the per-bucket texture
array bind differs from the simple OFF/ON bind above; in that case the depth shader
must bind the same bucket arrays for alpha sampling. **If `bc7Buckets` is true,
skip+log like the legacy guard** — handle bc7 in a follow-up; alpha sampling needs
the right array. Add: `if (bc7Buckets) { skip+log; return; }`.)

- [ ] **Step 4: Build**

Run: `cmake --build build64 --config RelWithDebInfo --target mc2`
Expected: exit 0. (Method exists but isn't called yet — Task 7 wires it. Behavior
unchanged.)

- [ ] **Step 5: Commit**
```bash
git add GameOS/gameos/gos_static_prop_batcher.cpp GameOS/gameos/gos_static_prop_batcher.h
git commit -m "feat(batcher): flushDepthPrepass() — isolated v6 depth-only mirror (gated, no-op)"
```

---

## Task 6: Color-pass EQUAL / depthWrite-off override

**Files:**
- Modify: `GameOS/gameos/gos_static_prop_batcher.cpp:4958-4959` (the color-pass applyPipeline site)

When the prepass ran this frame, the color pass must use `GL_EQUAL` + depthWrite
OFF so early-Z keeps only the front-most fragment the prepass recorded.

- [ ] **Step 1: Override the desc locally at the color applyPipeline site**

Replace the existing call:
```cpp
    pipeline_binder::applyPipeline(
        RenderCore::getPipelineDesc(RenderCore::PipelineId::StaticPropOpaque));
```
with:
```cpp
    {
        RenderCore::PipelineDesc colorDesc =
            RenderCore::getPipelineDesc(RenderCore::PipelineId::StaticPropOpaque);
        if (s_depthPrepassRanThisFrame) {
            // Prepass already laid the nearest reverse-Z depth. Keep only the
            // exact front-most fragment; do not re-write depth.
            colorDesc.depthFunc        = RenderCore::DepthFunc::Equal;
            colorDesc.depthWriteEnable = false;
        }
        pipeline_binder::applyPipeline(colorDesc);
    }
```
`flush()` already snapshots prev depth mask/func (`:4945-4946`) and restores them at
the end, so the override does not leak to other passes. **Object-ID write is
unaffected** — that is the frag's `layout(location=2)` output, not a depth-state
field; the color program keeps writing it for the surviving fragment.

- [ ] **Step 2: Reset the per-frame flag at the end of flush()**

At the end of `flush()` (after the draw loop, near the state restore ~`:5973+`), add:
```cpp
    s_depthPrepassRanThisFrame = false;   // consume; next frame re-arms in prepass
```

- [ ] **Step 3: Build**

Run: `cmake --build build64 --config RelWithDebInfo --target mc2`
Expected: exit 0. Still gated OFF (prepass not wired until Task 7), so the override
branch is never taken yet — behavior unchanged.

- [ ] **Step 4: Commit**
```bash
git add GameOS/gameos/gos_static_prop_batcher.cpp
git commit -m "feat(batcher): color pass flips to EQUAL+no-write when depth-prepass ran"
```

---

## Task 7: Wire the prepass into the frame (txmmgr)

**Files:**
- Modify: `mclib/txmmgr.cpp` — between `compute_dispatch()` (~:2528) and `flush()` (~:2578)

- [ ] **Step 1: Insert the prepass call**

In the `Render.GpuStaticProps` block, AFTER `gpu_cull::compute_dispatch()` writes
the indirect counts and BEFORE `GpuStaticPropBatcher::...flush()`:
```cpp
    // Depth-prepass (FOLIAGE-STATICPROP-DEPTH-PREPASS-1): lay static-prop depth
    // with a cheap alpha-test-only shader so the color flush() below can run
    // GL_EQUAL early-Z and skip the overdraw shading. Gated, no-op by default.
    GpuStaticPropBatcher::instance().flushDepthPrepass();
```
(Match the exact singleton/access form used for the `flush()` call right below it —
e.g. `GpuStaticPropBatcher::instance().flush()` or the file's local accessor.)

- [ ] **Step 2: Build + deploy**

Build `--target mc2`; deploy exe + `static_prop.vert` + `static_prop_depth.frag` +
`static_prop.frag` to v0.3.

- [ ] **Step 3: Smoke with the gate OFF (regression guard)**

Run: `python scripts/run_smoke.py --exe "A:/Games/mc2-opengl/mc2-win64-v0.3/mc2.exe" --mission mc2_24`
Expected: PASS, Δ destroys +0 (gate off → prepass returns immediately → identical
to pre-task behavior).

- [ ] **Step 4: Commit**
```bash
git add mclib/txmmgr.cpp
git commit -m "feat(txmmgr): call flushDepthPrepass() before static-prop color flush (gated)"
```

---

## Task 8: Register the gate + the color-after-prepass Tracy zone

**Files:**
- Modify: `RenderCore/RendererFeatureRegistry.h` (allowlist `MC2_STATIC_PROP_DEPTH_PREPASS`)
- Modify: `GameOS/gameos/gos_static_prop_batcher.cpp` (add `ZoneScopedN("GpuSP.ColorAfterPrepass")` around the color draw loop when prepass ran)

- [ ] **Step 1: Allowlist the env gate**

Add a `MC2_STATIC_PROP_DEPTH_PREPASS` entry to the feature/env registry following
the pattern of an existing `MC2_*` static-prop gate in
`RendererFeatureRegistry.h` (default OFF; description: "Camera depth-prepass for
static props — lays cheap alpha-test depth so the color pass uses GL_EQUAL early-Z
to skip foliage overdraw. =1 enables. Default OFF; color path byte-identical when
OFF."). If this engine validates env vars against an allowlist at startup (it
does — see the handoff's "All registered/allowlisted" note), an unregistered var
asserts; this step prevents that.

- [ ] **Step 2: Add the color-pass Tracy zone (measurement)**

Wrap the v6 color draw loop region (`:5896-5971`) — or add a nested zone — with:
```cpp
    ZoneScopedN("GpuSP.ColorAfterPrepass");
```
so Tracy shows the color-pass GPU cost separately when comparing prepass ON/OFF.
(`GpuSP.DepthPrepass` already added in Task 5.)

- [ ] **Step 3: Build + deploy + smoke**

Build, deploy, then:
Run: `python scripts/run_smoke.py --exe "A:/Games/mc2-opengl/mc2-win64-v0.3/mc2.exe" --mission mc2_24`
Expected: PASS (gate still OFF by default).

- [ ] **Step 4: Commit**
```bash
git add RenderCore/RendererFeatureRegistry.h GameOS/gameos/gos_static_prop_batcher.cpp
git commit -m "feat: register MC2_STATIC_PROP_DEPTH_PREPASS gate + GpuSP.ColorAfterPrepass zone"
```

---

## Task 9: Validation gate (parity + measurement)

**Files:** none (validation only). Produces evidence; no code unless a defect is found.

- [ ] **Step 1: GL_EQUAL parity — the load-bearing check**

With the build deployed to v0.3, capture the SAME static camera twice:
- Prepass OFF: launch without the env, screenshot.
- Prepass ON: `MC2_STATIC_PROP_DEPTH_PREPASS=1`, same mission/camera, screenshot.

Use the smoke runner / the project's screenshot mechanism (the `--screenshot`
shoots the fixed start camera — acceptable here since we compare ON vs OFF of the
SAME fixed camera). Compare:
- **No prop may vanish** (the GL_EQUAL failure mode). Diff the two PNGs; foliage
  must be present and lit identically in both.
- If exact byte-diff is too strict (temporal/dither effects), use a visual diff +
  an explicit "are the trees still there / same silhouette" check.

Missions: **mc2_24** (override trees: old alpha-card AND new trees9 solid canopy if
deployed) and **mc2_01** (baseline). Expected: visually identical ON vs OFF.

If props vanish → GL_EQUAL mismatch. Debug via the spec's fragility checklist
(polygon offset, cull, debug addr mode, texture/LOD state, the `invariant`
qualifier actually present in the deployed VS). Do NOT flip default-ON until parity
holds.

- [ ] **Step 2: tier1 smoke, GL-clean, +0 destroys (gate ON)**

Run with the gate ON across tier1:
```bash
MC2_STATIC_PROP_DEPTH_PREPASS=1 python scripts/run_smoke.py \
  --exe "A:/Games/mc2-opengl/mc2-win64-v0.3/mc2.exe" --tier tier1
```
Expected: all PASS, Δ destroys +0, 0 GL errors.

- [ ] **Step 3: Tracy GPU comparison (the win)**

On a NEAR-foliage camera (user's navigated mc2_24 view), capture Tracy with the
gate OFF then ON. Compare `Render.GpuStaticProps` self-time and
`GpuSP.ColorAfterPrepass` vs `GpuSP.DepthPrepass`. Expected: color-pass GPU time
drops sharply on foliage-heavy frames (fewer shaded fragments); prepass adds a
small cost. Net win on foliage; record the numbers. **Headline = reduced color-pass
GPU time / invocations, not triangle count.**

- [ ] **Step 4: Opaque-heavy / low-foliage regression check (Mode A tax)**

Run a scene with little alpha-on foliage (a building-heavy mission, or mc2_01)
gate ON vs OFF and compare `Render.GpuStaticProps`. If the symmetric prepass tax
meaningfully regresses opaque-only frames, record it and recommend either keeping
default-OFF for those or implementing Mode B (alpha-only prepass — a separate
follow-up slice per the spec). Otherwise note "no opaque regression observed."

- [ ] **Step 5: Object-ID / picking check (if active)**

If the object-id buffer (`colorAttachments.color2` / loc=2) is active in this build,
verify in-engine unit/prop picking still selects correctly with the gate ON (the
surviving EQUAL fragment must write the right id). If picking is not wired in this
deploy, note it as N/A.

- [ ] **Step 6: Record results + decide default**

Append a short results block to the spec doc (numbers from Steps 1-5). If parity
holds + foliage win is real + no opaque regression → propose flipping default-ON in
a follow-up commit. Otherwise keep default-OFF (the win is opt-in via the env).
```bash
git add docs/superpowers/specs/2026-06-04-foliage-depth-prepass-overdraw-design.md
git commit -m "docs: depth-prepass validation results (parity + Tracy)"
```

---

## Self-review notes (author)

- **Spec coverage:** Mode A baseline (Tasks 1-8), GL_EQUAL parity gate (Task 9.1),
  fragility checklist (referenced in 9.1 debug), dual texture-path requirement
  (Task 1.2 + the bc7 skip-guard in 5.3), glColorMask not attachment surgery
  (Task 5.3), object-id preserved in color / skipped in prepass (Task 6.1 + 9.5),
  explicit depthWrite-off validation (Task 6 + 9.1), opaque-heavy regression scene
  (Task 9.4), reframed success metric = color-pass GPU time not tris (Task 9.3),
  default-OFF→measure→flip rollout (Task 9.6). Mode B is explicitly deferred to a
  follow-up (spec-sanctioned).
- **Known sharp edges flagged for the executor:** (a) the shared dispatch-list
  source (Task 5.2) — prefer the shared-helper refactor so prepass + color draw the
  identical set; (b) bc7Buckets path → skip+log this slice; (c) the depth program's
  OWN `u_drawIDBase` location (Task 4) — using the color program's location while
  the depth program is bound is a silent bug; (d) `invariant gl_Position` must be in
  the DEPLOYED vert, not just the repo (deploy step in Task 4/7).
- **Out of scope (separate slices):** Mode B alpha-only; position-only depth VS;
  legacy/v5 dispatch-path prepass; HZB off the prepass depth; bc7 bucket prepass.
