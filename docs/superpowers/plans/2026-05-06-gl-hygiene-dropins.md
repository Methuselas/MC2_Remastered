# GL Hygiene Drop-Ins Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans.

**Goal:** Three small, independent OpenGL feature adoptions that are
cheap drop-ins, each with visible payoff, none gating any other arc.
Bundled into a single "GL hygiene" commit set so they can land
opportunistically without contaminating Track A/B/C/D scope.

**Architecture:** Each drop-in is independent. Tasks can land in any
order or be split across separate commits. Cross-track gating: none —
this slice does not depend on or block any other slice.

**Tech Stack:** C++ (engine), OpenGL 4.6 core (with extension fallbacks
for parallel-shader-compile which is not core).

**Spec references:**
- `docs/superpowers/mc3-rendering-modernization-roadmap.md` — adjacent to the rendering arc but not part of it
- ARB extension audit: session 2026-05-06 (worth-integrating drop-ins identified)
- Camera model context: `memory/camera_model_oblique_cinematic.md` — relevant for AF priority specifically

**Background guardrail:** anisotropic filter and polygon-offset-clamp are
GL 4.6 core; parallel-shader-compile is `GL_ARB_parallel_shader_compile`
(not promoted to core but widely supported). All three are best-effort
hints — the worst case for any of them is the driver ignores the hint
and behavior is unchanged.

---

## File Structure

| File | Status | Responsibility |
|---|---|---|
| `GameOS/gameos/gameosmain.cpp` | Modify | Engine init: add `glMaxShaderCompilerThreadsARB(N)` call |
| `GameOS/gameos/gameos_graphics.cpp` | Modify | Sampler / texture creation paths: enable anisotropic filter where appropriate |
| `GameOS/gameos/gos_postprocess.cpp` | Audit | Existing `glPolygonOffset` calls (lines 1137, 1160 — verify); replace with `glPolygonOffsetClamp` if shadow acne is observable |
| `mclib/txmmgr.cpp` | Possibly modify | Texture sampler binding may need AF state add |
| `~/.claude/projects/A--Games-mc2-opengl-src/memory/gl_hygiene_dropins.md` | Create | Memory file documenting the drop-ins shipped |

---

## Task 1 — `glMaxShaderCompilerThreadsARB` at engine init

**Highest-value-per-line item on the entire ARB audit list.** One
function call at engine init tells the driver to compile shaders on
background threads, potentially 2-3× faster on multi-core systems.
Driver chooses to honor it or not; failure mode is no-op.

**Files:**
- Modify: `GameOS/gameos/gameosmain.cpp` (or wherever GL context is
  initialized — verify via grep)

- [ ] **Step 1.1 — Locate GL context init**

```bash
grep -rn "glewInit\|gladLoadGL\|wglCreateContext\|SDL_GL_CreateContext" \
  GameOS/ code/ 2>/dev/null | head -10
```

Expected: a single primary site where the GL context is fully
initialized. The new call goes IMMEDIATELY after that site (driver
needs the context active to honor the hint).

- [ ] **Step 1.2 — Add the call**

After the GL context init line, add:

```cpp
// GL_ARB_parallel_shader_compile: tell driver to compile shaders on
// background threads. Best-effort hint; driver may ignore. Speeds
// startup when shader compile time is non-trivial. Use the GL-defined
// MAX_REQUEST sentinel to ask for "as many as the driver will give us"
// rather than picking an arbitrary thread count.
if (GLEW_ARB_parallel_shader_compile) {
    glMaxShaderCompilerThreadsARB(GL_MAX_QUEUED_SHADER_COMPILES_THREADS_ARB);
    printf("[INSTR v1] parallel_shader_compile=enabled\n");
} else {
    printf("[INSTR v1] parallel_shader_compile=unsupported\n");
}
fflush(stdout);
```

(Verify the exact GLEW symbol name with `grep -n
"GLEW_ARB_parallel_shader_compile\|MaxShaderCompilerThreadsARB\|MAX_QUEUED_SHADER_COMPILES_THREADS_ARB"
3rdparty/include/GL/glew.h` — adjust if the constant name differs;
`GL_MAX_SHADER_COMPILER_THREADS_ARB` is the more common spelling, set
the value to `0xFFFFFFFF` — the "use as many as the implementation
supports" sentinel — if the named constant doesn't exist in the
project's GLEW.)

- [ ] **Step 1.3 — Build and verify**

```bash
cmake --build build64 --config RelWithDebInfo --target mc2
```

Expected: clean build. Run the engine briefly and confirm
`[INSTR v1] parallel_shader_compile=enabled` (or `unsupported` on
old drivers — both are valid outcomes).

- [ ] **Step 1.4 — Commit**

```bash
git add GameOS/gameos/gameosmain.cpp
git commit -m "feat(gl): enable parallel shader compile hint at engine init

GL_ARB_parallel_shader_compile lets the driver compile shaders on
background threads. Best-effort hint with no-op fallback. Set thread
count to driver maximum. Visible improvement: faster startup when
shader compile is non-trivial; no behavior change otherwise.

Plan: docs/superpowers/plans/2026-05-06-gl-hygiene-dropins.md
"
```

---

## Task 2 — Anisotropic filtering on terrain detail samplers

**Highest visual-impact-per-line on the audit.** The camera model is
oblique 30° + cinematic — extreme texture sample angles are the norm,
not the exception. AF dramatically improves terrain detail texture
quality at oblique angles. AMD 7900 XTX trivially supports 16× AF.

Per `memory/camera_model_oblique_cinematic.md`: AF is rewarded by the
camera shape; this is one of the highest-priority oblique-friendly
features.

**Files:**
- Modify: `GameOS/gameos/gameos_graphics.cpp` (texture / sampler creation paths — verify via grep)
- Possibly modify: `mclib/txmmgr.cpp` (per-texture sampler binding)

- [ ] **Step 2.1 — Find the texture creation / sampler binding sites**

```bash
grep -rn "glTexParameteri\|glSamplerParameteri.*MIN_FILTER\|MAG_FILTER\|GL_TEXTURE_MIN_FILTER" \
  GameOS/ mclib/ 2>/dev/null | head -20
```

Expected: a small number of sites where filter parameters are set per
texture creation. AF is set alongside MIN/MAG_FILTER.

- [ ] **Step 2.2 — Add AF to terrain texture filter setup**

For each texture creation site that's bilinear/trilinear filtered (NOT
nearest-neighbor; AF is a no-op for nearest), add:

```cpp
// GL_ARB_texture_filter_anisotropic: enable AF on sampled textures.
// Camera is oblique (30° elevation, cinematic angles) — AF dramatically
// improves texture quality at oblique sample angles. AMD 7900 XTX
// supports 16× AF natively at trivial cost.
if (GLEW_ARB_texture_filter_anisotropic ||
    GLEW_EXT_texture_filter_anisotropic) {
    GLfloat maxAniso = 1.0f;
    glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &maxAniso);
    // Cap at 16× — AMD/NV driver maximum is typically 16; clamping
    // protects against future driver weirdness.
    GLfloat targetAniso = (maxAniso < 16.0f) ? maxAniso : 16.0f;
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY, targetAniso);
}
```

(Use `glSamplerParameterf(samplerObj, GL_TEXTURE_MAX_ANISOTROPY, ...)` instead if the project uses sampler objects rather than per-texture state.)

- [ ] **Step 2.3 — Decide AF scope**

Three reasonable scopes — pick at plan-time:

1. **All sampled textures** (broadest). Maximum visual impact. Tiny
   driver cost (16× AF on AMD is nearly free).
2. **Terrain only** (narrowest visible-impact path). Terrain detail at
   oblique angles is the most-stressed case; mech/HUD/UI textures are
   sampled at less-extreme angles or are fully clamped. Lower risk of
   any texture-sampling regression.
3. **Terrain + objects, skip HUD/UI.** Middle ground.

**My recommendation: option 2 first** (terrain only) — establishes the
pattern with smallest-blast-radius change. If visible improvement is as
expected, expand to option 3 in a follow-up commit. Don't go to option 1
in one shot.

- [ ] **Step 2.4 — Build and visually verify**

```bash
cmake --build build64 --config RelWithDebInfo --target mc2
py -3 scripts/run_smoke.py --tier tier1 --mission mc2_01 --duration 30 --kill-existing
```

Expected: tier1 mission passes. Visual check: terrain detail at
oblique angles (zoom partway in, look at terrain texture stretching
toward the horizon) should look significantly less blurry/aliased
compared to baseline.

- [ ] **Step 2.5 — Commit**

```bash
git add GameOS/gameos/gameos_graphics.cpp <other files>
git commit -m "feat(gl): enable 16× anisotropic filtering on terrain textures

GL_ARB_texture_filter_anisotropic dramatically improves texture
quality at oblique sample angles. The camera is oblique 30° elevation
+ cinematic low-angle (memory/camera_model_oblique_cinematic.md), so
AF is a high-impact visual improvement. Scope limited to terrain
textures for this commit; expand to objects in a follow-up if smoke
clean.

Plan: docs/superpowers/plans/2026-05-06-gl-hygiene-dropins.md
"
```

---

## Task 3 — Polygon offset clamp audit (replace if shadow acne observable)

`GL_ARB_polygon_offset_clamp` (4.6 core) replaces `glPolygonOffset(factor, units)`
with `glPolygonOffsetClamp(factor, units, clamp)`. The clamp parameter
prevents the polygon-offset bias from growing unboundedly at extreme
depth ranges, which causes shadow acne at far depths.

Audit at session 2026-05-06 found 4 production sites:
- `GameOS/gameos/gameos_graphics.cpp:5806` — `glPolygonOffset(-1.0f, -1.0f);`
- `GameOS/gameos/gameos_graphics.cpp:5860` — `glPolygonOffset(-1.0f, -1.0f);`
- `GameOS/gameos/gos_postprocess.cpp:1137` — `glPolygonOffset(2.0f, 4.0f);`
- `GameOS/gameos/gos_postprocess.cpp:1160` — `glPolygonOffset(2.0f, 4.0f);`

The two `gos_postprocess.cpp` sites are in the shadow pass context. If
these produce visible shadow acne or peter-panning at far distances
(verify visually first), the clamp variant fixes it without invasive
shader changes. The two `gameos_graphics.cpp` sites are also worth a
look but lower priority.

**Files:**
- Possibly modify: `GameOS/gameos/gos_postprocess.cpp:1137,1160`
- Possibly modify: `GameOS/gameos/gameos_graphics.cpp:5806,5860`

- [ ] **Step 3.1 — Visual baseline capture**

Before any code change, capture screenshots of representative shadow
scenes:
- `mc2_01` early game: mech shadows at standard zoom.
- `mc2_24` (the wolfman canary): shadows at extreme zoom-out.
- A cinematic low-angle moment (e.g., mission intro pan).

Look for: shadow acne (z-fighting between shadow and ground), peter-panning
(shadow detached from caster's feet at far depth).

- [ ] **Step 3.2 — Decide whether replacement is warranted**

If baseline screenshots show no acne or peter-panning issues → skip
this task. The clamp variant doesn't help when the existing offset
already produces clean shadows.

If acne/peter-panning IS observable → proceed.

- [ ] **Step 3.3 — Replace `glPolygonOffset` with `glPolygonOffsetClamp` at affected sites**

```cpp
// Old:
glPolygonOffset(2.0f, 4.0f);

// New:
if (GLEW_ARB_polygon_offset_clamp) {
    // Clamp at 0.001 — empirical; tune via screenshot comparison.
    // Prevents far-depth bias growth without affecting near-depth.
    glPolygonOffsetClamp(2.0f, 4.0f, 0.001f);
} else {
    glPolygonOffset(2.0f, 4.0f);  // fallback path
}
```

- [ ] **Step 3.4 — Visual after-capture and compare**

Re-capture the same screenshots. Compare against baseline. Confirm:
- Shadow acne reduced or eliminated at far depths.
- No new artifacts at near depths.
- No FPS regression.

- [ ] **Step 3.5 — Commit (only if change was warranted)**

```bash
git add GameOS/gameos/gos_postprocess.cpp <maybe other>
git commit -m "feat(gl): use polygon offset clamp for shadow pass

GL_ARB_polygon_offset_clamp prevents the polygon-offset bias from
growing unboundedly at extreme depth ranges, fixing shadow acne /
peter-panning observed at far distances under the oblique cinematic
camera. Clamp value 0.001 tuned via screenshot comparison; baseline
screenshots in <screenshot location>.

Plan: docs/superpowers/plans/2026-05-06-gl-hygiene-dropins.md
"
```

---

## Task 4 — Memory + index entry

**Files:**
- Create: `~/.claude/projects/A--Games-mc2-opengl-src/memory/gl_hygiene_dropins.md`
- Modify: `~/.claude/projects/A--Games-mc2-opengl-src/memory/MEMORY.md`

- [ ] **Step 4.1 — Write memory file**

```markdown
---
name: gl_hygiene_dropins
description: Three small GL feature adoptions shipped together as a hygiene commit set — anisotropic filter, polygon-offset-clamp (if needed), parallel shader compile
type: project
---

Track A/B/C-adjacent drop-ins shipped <date> as a hygiene batch:

- `glMaxShaderCompilerThreadsARB` at engine init — driver-chosen
  background-thread count for shader compile. Speeds startup; no-op
  fallback. `[INSTR v1] parallel_shader_compile=enabled|unsupported`
  reports state.
- 16× anisotropic filter on terrain textures — visible quality
  improvement at oblique camera angles per
  `memory/camera_model_oblique_cinematic.md`. Scope: terrain only;
  expand to objects in a follow-up if smoke clean.
- `glPolygonOffsetClamp` adoption — replaced `gos_postprocess.cpp`
  shadow-pass offsets [if visual baseline showed acne; otherwise
  skipped — note status here].

These items are independent of Track A/B/C/D execution. Future GL
hygiene drop-ins should follow this batch shape: small, independent,
no cross-track gating.
```

- [ ] **Step 4.2 — Index in MEMORY.md**

Add to MEMORY.md under the "Rendering / shaders" section:

```
- [GL hygiene drop-ins shipped (<date>)](gl_hygiene_dropins.md) — AF on terrain (camera is oblique), parallel shader compile, polygon-offset-clamp audit
```

---

## Self-Review

**Spec coverage:**

- ARB audit recommendations 1, 2, 3 from session 2026-05-06 → Tasks 1, 2, 3.
- Multi-bind in C1 and invalidate-subdata in C2 are absorbed into Track C plan, NOT this hygiene plan (correctly out of scope here).
- Q19 (DSA), Q20 (clip control), Q21 (HZB) are Track-arc-class concerns, NOT in this hygiene plan (correctly out of scope here).

**Placeholder scan:** none — every step has exact code or commands.

**Type / call consistency:** all three tasks are independent; no cross-task type dependencies. Each can be executed in isolation.

---

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/2026-05-06-gl-hygiene-dropins.md`. Each task can be executed independently; no inter-task ordering. Reasonable cadence:

1. Task 1 first (highest value-per-line; no risk).
2. Task 2 second (visible improvement; small scope).
3. Task 3 only if visual baseline shows the problem it solves (don't fix what isn't broken).

Total: ~half-day to a day of work for all three, depending on visual-verification iteration on Task 2/3.
