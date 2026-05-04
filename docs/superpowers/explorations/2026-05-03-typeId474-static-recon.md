# typeId=474 White-Out — Focused Static Recon

Date: 2026-05-03
Branch: `claude/nifty-mendeleev`
HEAD: `d494638` (with one uncommitted edit in `shaders/include/lighting.hglsl` at lines 196-200 — see §1)
Scope: code-grounded static recon, NO code changes. Resolves contradiction between two prior analyses about CPU per-vertex lighting init, and assesses what does (and does NOT) explain the trace data for mc2_18 typeId=474 inst=0.

Skill applied: `.claude/skills/adversarial-plan-review.md` (verification appendix in §10).

---

## §1. Executive summary

**The contradiction is resolvable**: both prior analyses are partially correct. CPU **does** initialize `redFinal=greenFinal=blueFinal=0` at iteration entry (`mclib/tgl.cpp:1794`), but it then runs the magic-tag decode (lines 1804-1890) BEFORE the lighting loop. So at lighting-loop entry the values hold the magic-tag-decoded result. The trace implementer's "init at 0" is correct as a literal claim about line 1794. The substrate-gap recon's "leaving redFinal/greenFinal/blueFinal at the magic-tag-decoded value" is correct as a claim about what's in the locals when the lighting loop is *skipped* (line 1938 with `s_numLights==0`). Both describe the same code reading different snapshots in time.

**However, neither of those framings explains the trace data for typeId=474.** The trace says `numLights.x = 2 (NEVER 0)`, which means the GPU early-return at `lighting.hglsl:195-200` does NOT fire — neither the original `return vec3(1,1,1)` nor the proposed `return base_light` is executed. The recon's #1 cause hypothesis (GenericAppearance + `SetLightList(NULL,0)` → numLights==0 short-circuit) is **invalidated** for typeId=474 because the dispatch confirms typeId=474 takes the **Building** path (`bdactor.cpp`), which does NOT zero numLights.

**Working-tree state matters:** `git blame` on `shaders/include/lighting.hglsl:196-200` shows the `#ifdef MC2_STATIC_PROP_LIGHTING / return base_light / #else / return vec3(1,1,1) / #endif` block is **uncommitted "Not Committed Yet" working-tree state** (HEAD `d494638` still has the unconditional `return vec3(1,1,1)`). When committed, this fix will close the GenericAppearance white-out class but **will NOT affect typeId=474** — that bug must be elsewhere.

**Cause statement for typeId=474** (medium-confidence, residual): static analysis cannot pin a single substrate divergence that explains "GPU saturates to 0xFFFFFFFF *uniformly* for 3 verts with different per-vertex CPU outputs (0x81, 0x81, 0x1F)." Walking both kernels symbolically with the documented inputs (`numLights=2`, building path, instance light data correctly captured at update-time per the bug-5 fix) does NOT reproduce the saturation — both pipelines produce matching output. The only static hypothesis that fits the data is **per-vertex GPU input contamination**: either the per-vertex `a_aRGBLight` reaching the shader for soup verts 24/27/29 differs from the `aRGBLight` that CPU reads through the same `listOfTypeTriangles[].Vertices[]` indirection, OR the instance-side `aRGBHighlight` / `lightDataIndex` / `flags` fields read by the shader differ from what the CPU `MultiTransformShape` saw at bake time for this actor. This is **H3 from the dispatch's hypothesis list**, possibly combined with H5 (something not in the prior cause map).

**Recommendation for fix scope:** **rolled-into-N2 (defer; need more recon).** Add focused runtime instrumentation BEFORE proposing a fix. The N1 fix (early-return → `base_light`) is still correct for the GenericAppearance class — keep it as-is in the working tree, commit it, but do NOT scope it as fixing typeId=474. The typeId=474 white-out needs its own diagnostic dispatch with one-shot prints of (a) `inst.aRGBHighlight`, (b) `a_aRGBLight` for verts 24/27/29, (c) `light[2].numLights` AS READ BY THE SHADER (not as gathered C++-side), and (d) the actor's `child->aRGBHighlight` value at `submit()` time.

---

## §2. CPU per-vertex init — verbatim code with citations

The disputed init lives at `mclib/tgl.cpp:1794-1796`, immediately inside the per-vertex loop body (loop opens at line 1745):

```cpp
//----------------------------------------------------
// Lighting goes here.
DWORD redFinal=0, greenFinal=0, blueFinal=0;        // line 1794
DWORD redAmb = 0, greenAmb = 0, blueAmb = 0;        // line 1795
DWORD redSpec=0, greenSpec=0, blueSpec=0;           // line 1796
```

These are **fresh stack-locals declared anew on every iteration of `for (long j=0; j<numVertices; j++)`** at line 1745. Initialized to 0 unconditionally.

**Then** the magic-tag decode at lines 1798-1890 runs on every iteration:

- Line 1798-1801: `if (lighteningLevel > 0)` → seeds `redSpec=greenSpec=blueSpec=lighteningLevel`. Stock missions: 0 (only weather-bolt frames write it).
- Line 1803: `DWORD startVLight = theShape->listOfTypeVertices[j].aRGBLight;`
- Lines 1804-1828: Hot Pink (`startVLight == 0xffff00ff`) — sets redFinal/greenFinal/blueFinal from `theShape->hotPinkRGB` if night/twilight, else `0x2f` daytime.
- Lines 1829-1850: Hot Yellow (`0xffffff00`) — similar.
- Lines 1851-1869: Hot Green (`0xff00ff00`) — similar but **NO daytime else clause** (line 1851-1869 has no fallback; if !isNight && nightFactor <= SMALL, redFinal/greenFinal/blueFinal stay at 0).
- Lines 1870-1877: Hot Red, Hot Blue (`0xffff0000`, `0xff0000ff`) — empty bodies.
- Lines 1878-1886: "Some other kind of light" — if `(startVLight & 0x00FFFFFF) != 0` AND `!lightsOut`, sets redFinal/greenFinal/blueFinal from the BGR bytes of the tag.
- Line 1887-1890: HUD fallback — if isHudElement, all 0xFF.

**Then** the BaseVertexColor add at lines 1892-1905 (no-op when default 0).

**Then** the lighting loop opens at line 1934-1938:

```cpp
if (useVertexLighting && (Environment.Renderer != 3))
{
    if (!isSpotlight && !isWindow)
    {
        for (long i=0;i<s_numLights;i++)        // line 1938
        {
            ...
        }

        redFinal += redAmb;                     // line 2207
        blueFinal += blueAmb;
        greenFinal += greenAmb;
    }
    ...
}
```

When `s_numLights == 0`, the for loop body doesn't execute. `redAmb/greenAmb/blueAmb` were initialized 0 at line 1795 and never written. Lines 2207-2209 add 0. So the final value of `redFinal/greenFinal/blueFinal` exiting the lighting block is **whatever the magic-tag decode left them at**.

**Final argb pack at line 2232:**

```cpp
listOfVertices[j].argb = (0xff << 24) + (redFinal << 16) + (greenFinal << 8) + (blueFinal);
```

Then line 2320-2342 adds aRGBHighlight if non-zero (clamps each channel to 255).

---

## §3. GPU per-vertex init — verbatim code with citations

`shaders/static_prop.vert:152-157` calls `get_base_light`:

```glsl
vec3 base_light = get_base_light(
    perVertexARGB,
    false, 0.0, false, false,
    ptd.hotPinkRGB.rgb,
    ptd.hotYellowRGB.rgb,
    ptd.hotGreenRGB.rgb);
```

`shaders/include/lighting.hglsl:46-146` `get_base_light`:

```glsl
vec3 final = vec3(0.0);                                      // line 51
uint r = uint(clamp(startVLight.x*255.0 + 0.5, 0.0, 255.0)); // (B byte — perVertexARGB.x = B/255)
uint g = uint(clamp(startVLight.y*255.0 + 0.5, 0.0, 255.0));
uint b = uint(clamp(startVLight.z*255.0 + 0.5, 0.0, 255.0)); // (R byte)
uint a = uint(clamp(startVLight.w*255.0 + 0.5, 0.0, 255.0));

uint start_v_light = b | (g<<8) | (r<<16) | (a<<24);         // reconstructs original DWORD
```

`final` initialized to `vec3(0.0)`. Magic-tag branches at lines 66-141 mirror the CPU side:

- Hot Pink/Yellow/Green: same comparisons, `static_prop.vert:154` passes hardcoded `isNight=false, nightFactor=0.0`. **Hot Green daytime falls through with no else clause** — `final` stays vec3(0). (Lines 95-105.)
- Hot Red/Blue: empty bodies (lines 106-113).
- Non-magic colored: `final = startVLight.zyx` (line 132, `MC2_STATIC_PROP_LIGHTING` branch). `.zyx` reorders BGR-channel-order startVLight to RGB.
- HUD fallback: `final = vec3(1.0)` (line 140).

Then line 143: `final.xyz += g_scene.baseVertexColor.xyz` (default 0).
Then line 145: `return clamp(final.xyz, vec3(0.0), vec3(1.0));`

So `base_light` at static_prop.vert:152 ends up with the magic-tag-decoded value (in 0..1 float space), exactly matching what CPU's `redFinal/greenFinal/blueFinal` hold at the start of the lighting loop (in 0..255 byte space).

`shaders/include/lighting.hglsl:191-200` `calc_light` entry:

```glsl
vec3 calc_light(in int lights_index, in vec3 normal, in vec3 vertex_world_pos, in vec3 base_light)
{
    ObjectLights ld = light[lights_index];

    if (0 == ld.numLights.x)
#ifdef MC2_STATIC_PROP_LIGHTING
        return base_light;                // **uncommitted — see §4**
#else
        return vec3(1, 1, 1);
#endif

    vec3 final = base_light;              // line 202
    vec3 ambient = vec3(0.0);
```

**Critical: `git blame` on lines 196-200 shows the `#ifdef MC2_STATIC_PROP_LIGHTING / return base_light / #else / return vec3(1,1,1) / #endif` block as "Not Committed Yet" working-tree state.** HEAD `d494638` (and `014ceb8`/`38ba240` before it) all have the **unconditional** `return vec3(1, 1, 1);` at line 196. So depending on whether the recon-author intended to evaluate against committed state or working-tree state, the GPU early-return semantics differ.

For trace-data inputs (`numLights=2`), the early-return doesn't fire either way, so this distinction doesn't affect the typeId=474 analysis below. It DOES affect any future analysis that tries to argue "N1 already shipped" — the fix is in the working tree but not in HEAD.

Loop body (lines 206-268): TG_LIGHT_AMBIENT adds to `ambient`; TG_LIGHT_INFINITE / INFINITEWITHFALLOFF / POINT / SPOT add to `final` with `n_dot_l = clamp(dot(normal, -light_dir), 0, 1)` weight; TG_LIGHT_TERRAIN is a no-op (per spec R2).

Return at line 270: `return final + ambient;` — no clamp at return; clamp happens at parity-pack (`static_prop.vert:233-235`) and v_argb output (`static_prop.vert:218`, but actually `lit = clamp(lit + inst.aRGBHighlight.rgb, 0, 1)` at line 211 IS the clamp).

`static_prop.vert:211`: `lit = clamp(lit + inst.aRGBHighlight.rgb, 0.0, 1.0);` — adds aRGBHighlight, clamps.

Parity write (`static_prop.vert:232-247`): packs `lit` to BGRA uint with hardcoded alpha=255. CPU mirror at `tgl.cpp:2232` and `tgl.cpp:2341`.

---

## §4. Resolution of the contradiction

**Recon's claim** (`2026-05-03 substrate-gap recon §5 Cause #1`): "CPU's vertex loop (tgl.cpp:1938) gates on the same condition but simply skips the loop, leaving redFinal/greenFinal/blueFinal at the magic-tag-decoded value."

**VERDICT: PARTIAL.** The claim is correct as a statement about state-after-loop-skip: when the lighting loop at line 1938 is skipped (s_numLights==0 OR isSpotlight OR isWindow), `redFinal/greenFinal/blueFinal` retain whatever the magic-tag decode set them to (lines 1804-1890), which is the literal initial value 0 for the non-magic-non-HUD-non-lit-colored case, or the magic-decoded value otherwise. The recon's prose elides the init-then-decode order but the claim about state-at-loop-skip is sound.

**Trace implementer's claim** (`2026-05-03 D3 trace`): "tgl.cpp's vertex lighting loop initializes redFinal = greenFinal = blueFinal = 0 (line ≈1870) and adds contributions — it does NOT add aRGBLight itself to the final sum. aRGBLight is only used as a hot-color tag."

**VERDICT: PARTIAL / WRONG ON DETAILS.**
- "initializes redFinal=greenFinal=blueFinal=0" — TRUE at line 1794 (not 1870 — line 1870 is the Hot Red empty body, not an init site).
- "adds contributions" — TRUE for the loop body at lines 1938-2205.
- "does NOT add aRGBLight itself to the final sum" — **FALSE.** The non-magic colored-tag branch at lines 1878-1886 explicitly writes `redFinal = (startVLight>>16) & 0xFF` (etc.) BEFORE the loop. This is "adding aRGBLight to the final sum" in the sense that the per-vertex tag's R/G/B bytes seed redFinal/greenFinal/blueFinal. The Hot Pink/Yellow/Green branches likewise pre-seed from `theShape->hotPinkRGB` (etc.). The trace implementer's framing (init=0, only loop contributes) misses these per-vertex pre-seed branches.
- "aRGBLight is only used as a hot-color tag" — **FALSE.** It IS used as a hot-color tag (matched against `0xffff00ff` etc.), but for non-magic non-zero values it's also used as a per-vertex base color (the BGR bytes seed redFinal/greenFinal/blueFinal at lines 1882-1884).

**Composite resolution:** Both are looking at the same code; both are partially right. CPU initializes redFinal/greenFinal/blueFinal = 0 at iteration entry (line 1794), then conditionally seeds from the per-vertex aRGBLight tag (or from theShape->hot{Pink,Yellow,Green}RGB) at lines 1804-1890, then enters the lighting loop at line 1938 to ADD contributions. The "init" phrase is accurate at line 1794; the "magic-tag-decoded value" phrase is accurate at line 1937 (just before the loop opens). They are not contradictory — they describe different snapshots in the same per-vertex iteration.

The corresponding GPU code is structurally equivalent: `final = vec3(0.0)` at lighting.hglsl:51, then magic-tag conditional seeding, then `base_light` exits get_base_light, then `final = base_light` at calc_light:202 to enter the light loop with the seeded value. **Both pipelines have IDENTICAL semantics for this part.**

---

## §5. Step-by-step value tracking for vert=24 (cpu=0x818181, gpu=0xFFFFFFFF)

vert=24 is a **soup-vertex index** (0..numTris*3). It maps to typed-vertex `vi = listOfTypeTriangles[8].Vertices[0]` via the IBO (gos_static_prop_batcher.cpp:847-851). Both pipelines key off this same `vi` for all per-vertex data (CPU reads `listOfTypeVertices[vi]`, GPU reads the VBO row that registerType wrote from `listOfTypeVertices[vi]`).

The trace did NOT capture the aRGBLight tag for soup-vert=24 (the trace dump only covers typed verts 0-7). Without that data, the walk below is **conditional on a hypothesized tag**.

### Hypothesis A — typed vert vi has aRGBLight = 0xFF000000 (default, like typed verts 3,4,5,7)

| Step | CPU op + value | GPU op + value |
|---|---|---|
| 1. Init | tgl.cpp:1794 — redFinal=0, greenFinal=0, blueFinal=0 | lighting.hglsl:51 — final = vec3(0.0) |
| 2. lighteningLevel | 0 (stock) — skip | (not present) |
| 3. Read tag | startVLight = 0xFF000000 | start_v_light = 0xFF000000 (after reconstruction at line 58) |
| 4. Magic test Hot* | 0xFF000000 ≠ 0xffff00ff/etc — skip all magic branches | same — skip |
| 5. Non-magic test | (0xFF000000 & 0x00FFFFFF) == 0 — skip | (start_v_light & 0x00FFFFFF) != 0 is FALSE — skip |
| 6. HUD test | isHudElement = false — skip | isHudElement = false — skip |
| 7. BaseVertexColor | 0 — no-op | += vec3(0) — no-op |
| 8. Pre-loop state | redFinal/greenFinal/blueFinal = 0,0,0 | base_light = clamp(vec3(0)) = vec3(0) |
| 9. Light gate | useVertexLighting=true, !isSpotlight, !isWindow — enter | flags & (kFlagIsWindow\|kFlagIsSpotlight) == 0 — call calc_light |
| 10. numLights gate | s_numLights = 2 — enter loop | ld.numLights.x = 2 — past early-return; final = base_light = vec3(0) |
| 11. AMBIENT light | redAmb = ((startLight>>16)&0xFF), etc | ambient += lcolor (lcolor = (R/255, G/255, B/255)) |
| 12. INFINITE light | cosine = lightDir·N. If <0: redFinal += R*\|cosine\| | n_dot_l = clamp(dot(N, -L), 0, 1); final += n_dot_l * lcolor |
| 13. End-of-loop ambient add | redFinal += redAmb (line 2207) | (already in `ambient` accumulator) |
| 14. Clamp | redFinal = min(redFinal, 255) | (no clamp here; happens at parity pack) |
| 15. Pack argb | argb = 0xFF<<24 \| redFinal<<16 \| greenFinal<<8 \| blueFinal | lit = final + ambient; lit = clamp(lit + aRGBHighlight.rgb, 0, 1); pack at line 233-247 |

**Predicted output:** if AMBIENT=(0x80,0x80,0x80) and INFINITE adds 1 contribution with cosine<0 giving |cosine|≈0.005 (so adds ~1 to byte space), CPU produces 0x81. GPU: ambient=(0.502,0.502,0.502); INFINITE adds (0.005, 0.005, 0.005); lit = (0,0,0) + ambient + infinite = (0.507, 0.507, 0.507); pack = 0x81 ≈ uint(0.507*255). **Match: GPU should also be 0x81.**

**This hypothesis does NOT explain GPU=0xFFFFFFFF.**

### Hypothesis B — typed vert vi has aRGBLight = 0xFF818181 (per-vertex non-magic colored tag)

CPU step 5: `(0xFF818181 & 0x00FFFFFF) = 0x818181 != 0` — enters "Some other kind of light" branch. redFinal=0x81, greenFinal=0x81, blueFinal=0x81.
CPU step 8 pre-loop state: redFinal/greenFinal/blueFinal = 0x81/0x81/0x81.
CPU step 11+12: if both AMBIENT=0 and INFINITE adds 0 (cosine>=0), no addition. Output: 0x818181. **Matches CPU trace.**

GPU step 5: `(start_v_light & 0x00FFFFFF) != 0` is TRUE — enters non-magic colored branch. final = startVLight.zyx = (R/255, G/255, B/255) = (0.506, 0.506, 0.506).
GPU step 8 base_light = clamp((0.506,0.506,0.506)) = (0.506,0.506,0.506).
GPU step 10+11+12: final = base_light = (0.506,0.506,0.506); AMBIENT contributes 0; INFINITE n_dot_l=0 (same back-facing condition as CPU). Loop end: final = (0.506,0.506,0.506).
GPU step 14-15: lit = (0.506,0.506,0.506); aRGBHighlight = ?.

**If `inst.aRGBHighlight.rgb = vec3(0)` → lit stays (0.506,0.506,0.506); pack = 0x81 → GPU=0xFF818181. Matches CPU.**
**If `inst.aRGBHighlight.rgb` ≥ vec3(0.5) → clamp((0.506,0.506,0.506) + (≥0.5,...,...), 0, 1) = vec3(1) → pack = 0xFF → GPU=0xFFFFFFFF. Matches trace.**

**This hypothesis is consistent with trace if aRGBHighlight is non-zero on the GPU side BUT the CPU side reads aRGBHighlight=0.** Static analysis cannot confirm this — the value is set in `bdactor.cpp` and propagated identically to both pipelines. But it is the only hypothesis that fits trace data with the documented infrastructure intact.

### Hypothesis C — GPU's `inst.aRGBHighlight` reads from a different memory location than C++ wrote

If the SSBO struct layout has a misalignment, GPU could read e.g., `fogRGB` bytes as `aRGBHighlight`. But static_assert at gos_static_prop_batcher.h:34 confirms offset 80 for aRGBHighlight. std430 GLSL alignment puts a vec4 at the next 16-byte boundary after the 4 uints (offset 80). Both match. **REJECT C.**

### Hypothesis D — light[2] UBO contents differ from gathered data

If the UBO at slot 2 contains data with `numLights.x = 0` despite the C++ side gathering 2 lights, GPU early-returns vec3(1,1,1). For HEAD `d494638` (no MC2_STATIC_PROP_LIGHTING ifdef around the early-return — see §3 note), this would explicitly produce GPU=vec3(1,1,1)=0xFFFFFFFF for ALL verts of the affected actor. The CPU side correctly bakes from `s_listOfLights/s_numLights = 2` because it reads CPU-side state directly, not the UBO.

**This hypothesis fits the trace data.** It requires either (a) the UBO upload at txmmgr.cpp:1118-1127 didn't actually upload slot 2 by the time the draw runs (race with addLightDataStructure post-render-loop additions, though addLightDataStructure shouldn't run after renderLists begins), OR (b) `cachedGpuLightIndex_` is correctly 2 in C++ but the UBO's slot 2 holds a *different* actor's light data that happens to have numLights==0 (e.g. a GenericAppearance actor that ran first and had `SetLightList(NULL,0)` — the recon's #1 cause class — registered itself at slot 0/1/2 with numLights==0; then dedupe by memcmp would NOT match the building's nonzero data, so the building gets a different unique slot... unless the building's Capture coincidentally produced an all-zero record).

**This hypothesis is the most consistent with the trace's "GPU saturates uniformly across 3 verts with different per-vertex CPU values".** But it is also speculative — static analysis cannot confirm it.

### Hypothesis E — typed vert vi has Hot Green tag (0xFF00FF00) at daytime, with non-trivial nightFactor

Recon §5.B3 covered this for mc2_15 typeId=287 (cpu=0x1B1B15 gpu=0x2F2F2F). For typeId=474 with daytime + nightFactor>SMALL:
- CPU: redFinal = (hotGreenRGB>>16)&0xFF * nightFactor → some R value.
- GPU: hardcoded nightFactor=0 → no else clause for Hot Green → final stays vec3(0).

If CPU base = 0x81 and GPU base = 0, then GPU's lighting loop adds INFINITE and AMBIENT to vec3(0); CPU adds them to vec3(0x81). For both to converge at GPU=0xFFFFFFFF and CPU=0x81 requires CPU's base + lighting < 0xFF and GPU's base + lighting >= 1.0 — but that's IMPOSSIBLE because CPU's base is HIGHER, so CPU's sum should be ≥ GPU's sum. **REJECT E for typeId=474** (the polarity is wrong).

### Verdict for vert=24

**No single static-analysis-derivable hypothesis cleanly explains the trace data.** The most plausible (B with non-zero aRGBHighlight, or D with UBO-slot-2 contamination) require runtime evidence to confirm.

---

## §6. Step-by-step value tracking for vert=29 (cpu=0x1F1F1F, gpu=0xFFFFFFFF)

Same as §5 but with hypothesized typed-tag yielding CPU base = 0x1F. The structural divergence — CPU producing different per-vertex outputs (0x81 vs 0x1F) while GPU produces identical 0xFFFFFFFF — is the **single most diagnostic observation** in the trace.

**Implication:** GPU's `lit` value is INDEPENDENT of the per-vertex normal and per-vertex aRGBLight tag for these 3 verts. Two ways this can happen mechanically:

1. **GPU exits calc_light before the loop runs** (early-return), regardless of base_light. This is exactly the `numLights.x == 0 → return vec3(1,1,1)` path. Even though C++-side gather reported numLights=2, the UBO slot the shader reads has numLights==0. (Hypothesis D.)
2. **Something AFTER calc_light overrides lit to (1,1,1)**, regardless of input. The only candidate is the aRGBHighlight add at static_prop.vert:211: `lit = clamp(lit + inst.aRGBHighlight.rgb, 0, 1)`. If `inst.aRGBHighlight.rgb = vec3(>=1.0)`, every per-vertex `lit` saturates to vec3(1) regardless of input. (Hypothesis B with non-zero aRGBHighlight on GPU side.)

**Both hypotheses converge to the same diagnostic question:** is the GPU's per-instance side-data (`inst.aRGBHighlight` or `light[inst.lightDataIndex].numLights`) holding what the C++ side wrote, OR is something corrupting it between submit() and shader read?

---

## §7. Cause statement

**Static analysis is insufficient to identify the typeId=474 white-out cause definitively.** What static analysis CAN say:

- The recon's #1 cause hypothesis (GenericAppearance → numLights==0 → vec3(1,1,1)) does NOT apply to typeId=474. The dispatch confirms typeId=474 takes the Building path.
- For trace inputs (numLights=2 reported by GatherLightsParameters, lightDataIndex=2 cached on the actor), the CPU and GPU vertex kernels are arithmetically equivalent across all branches reachable from the documented input. Walking both kernels symbolically does NOT produce a divergence under the documented inputs.
- The trace's "GPU saturates uniformly while CPU varies per-vertex" pattern is mechanically consistent with **either** (a) the GPU's `light[lightDataIndex].numLights.x` being 0 despite C++ gather reporting 2 (UBO slot mismatch / contamination), **or** (b) the GPU's `inst.aRGBHighlight.rgb` being non-zero despite C++ side reading aRGBHighlight=0 at bake time. Both are SSBO/UBO data-flow hypotheses, not algorithm-divergence hypotheses.
- All algorithm-level divergences enumerated in the substrate-gap recon §5 (lighteningLevel, TG_LIGHT_TERRAIN, TG_LIGHT_POINT/SPOT routing, BaseVertexColor precision) are SECONDARY contributors at most for typeId=474 — none of them produce the observed "GPU saturates uniformly" pattern.

**Recommendation: do NOT propose a code fix from this recon.** Instead, dispatch a focused diagnostic to narrow Hypothesis D vs B.

---

## §8. Recommended fix scope

**No code fix recommended from this recon.** The N1 fix already in the working tree (`#ifdef MC2_STATIC_PROP_LIGHTING` gate around `return vec3(1,1,1)` → `return base_light`) is correct for closing the GenericAppearance white-out class identified by the recon. **It should be committed as-is.** Predicted effect: clears all GenericAppearance-population mismatches whose CPU value is achromatic (matches `base_light` after magic-tag decode) and GPU value is 0xFFFFFFFF. This includes the recon's "Cause #1" coverage estimate but does NOT include typeId=474.

**For typeId=474 specifically:** instead of a fix, the next step is **one diagnostic dispatch** that adds three one-shot prints (at the typeId=474 inst=0 vert=24/27/29 sites) and re-runs mc2_18:

1. In `gos_static_prop_batcher.cpp::submit()`, when `typeID == 474 && doTraceHeader`, dump `inst.aRGBHighlight[0..3]` and `inst.lightDataIndex`. (`aRGBHighlight` already known C++-side via `child->aRGBHighlight`; cross-checking the SSBO write is the diagnostic.)
2. In `static_prop.vert`, gated by a debug uniform, write `light[inst.lightDataIndex].numLights.x` to a parity slot OR a debug color (RGB encodes numLights). This confirms whether GPU actually sees numLights==0 vs 2.
3. In the parity snapshot at gos_static_prop_batcher.cpp:851, dump `typeShape->listOfTypeVertices[vi].aRGBLight` for vi corresponding to soup-verts 24/27/29 (NOT just the first 8 typed verts). This confirms which Hypothesis-A/B branch fires.

The output of that diagnostic resolves Hypothesis B vs D mechanically. THEN scope a fix.

**This is rolled-into-N2 (defer; need more recon) per the dispatch's classification.**

---

## §9. Remaining causes (if any) and open questions

1. **Soup-vert 24/27/29's actual `aRGBLight` tag is unknown.** The trace header dump at gos_static_prop_batcher.cpp:837-844 caps at `maxVerts = min(numTypeVertices, 8)`. Soup-vert 24 corresponds to `listOfTypeVertices[listOfTypeTriangles[8].Vertices[0]]` which could be ANY typed-vertex index (likely > 7, hence beyond the dump). **A trace extension that dumps the actual aRGBLight for the mismatching verts is strongly preferred over additional static analysis.**

2. **Whether `inst.aRGBHighlight` for typeId=474 inst=0 is actually 0 or non-zero.** Static analysis confirms C++-side passes `child->aRGBHighlight` (TG_Shape default 0). But there's no static evidence that the SSBO written reflects this — a one-shot dump confirms.

3. **Whether `light[2].numLights.x` as read by the shader is 0 or 2.** The dispatch's "numLights.x = 2 (NEVER 0)" notation is ambiguous — it might describe the C++-side `GatherLightsParameters` output (which is the `lights->numLights_` field that gets memcpy'd into `lightData_` and later uploaded to UBO slot N). If the trace doesn't directly read `light[N].numLights.x` from the shader's perspective, there's still a possibility the UBO data is corrupted or off-by-one. **One-shot debug-color output from the shader reading `light[lightDataIndex].numLights.x` resolves this.**

4. **Whether the working-tree edit at lighting.hglsl:196-200 (`#ifdef MC2_STATIC_PROP_LIGHTING / return base_light`) was active during the trace run.** If the smoke that produced the trace data was built against HEAD `d494638`, the unconditional `return vec3(1,1,1)` was active. If built against working tree, the conditional `return base_light` was active. The trace's `gpu=0xFFFFFFFF` is consistent with EITHER (the unconditional saturate, or the new path returning base_light=(1,1,1) for some bright tag) — the distinction matters for diagnosis.

5. **Recon §5.A's CPU value derivation for typeId=474.** The recon claimed cpu=0xFF818181 was explainable by the per-vertex tag being `0xFF818181`. But the trace ALSO shows verts 27 and 29 have `cpu=0xFF818181` and `cpu=0xFF1F1F1F` respectively — different per-vertex values. The recon's explanation handles vert=24/27 if their tags are both 0xFF818181, but requires vert=29's tag to be 0xFF1F1F1F. That's plausible but unverified.

6. **Recon's broader cause map (causes #2-#5) is unaffected by this analysis.** It still applies to the OTHER mismatch classes (colored-cpu-gray-gpu, grayscale-additive, colored-colored). This recon ONLY addresses typeId=474.

---

## §10. Adversarial review checklist

Symbols cited in this recon and verified live against current tree:

| Symbol / Site | File:Line | Match |
|---|---|---|
| CPU per-vertex init `redFinal=0` | mclib/tgl.cpp:1794 | ✓ verbatim |
| CPU magic-tag decode start | mclib/tgl.cpp:1803 | ✓ |
| CPU Hot Pink branch | mclib/tgl.cpp:1804-1828 | ✓ |
| CPU Hot Green daytime no-else | mclib/tgl.cpp:1851-1869 | ✓ confirmed no else clause for daytime+nightFactor<=SMALL |
| CPU non-magic colored branch | mclib/tgl.cpp:1878-1886 | ✓ |
| CPU lighting gate | mclib/tgl.cpp:1934-1938 | ✓ |
| CPU final argb pack | mclib/tgl.cpp:2232 | ✓ |
| CPU aRGBHighlight add | mclib/tgl.cpp:2320-2342 | ✓ |
| GPU `final = vec3(0.0)` | shaders/include/lighting.hglsl:51 | ✓ |
| GPU magic-tag decode start | shaders/include/lighting.hglsl:52-58 | ✓ |
| GPU Hot Green daytime no-else | shaders/include/lighting.hglsl:95-105 | ✓ confirmed no else (mirrors CPU) |
| GPU non-magic colored swizzle | shaders/include/lighting.hglsl:131-135 | ✓ `MC2_STATIC_PROP_LIGHTING` gate |
| GPU `g_scene.baseVertexColor` add | shaders/include/lighting.hglsl:143 | ✓ |
| GPU `final = clamp(final, 0, 1)` | shaders/include/lighting.hglsl:145 | ✓ |
| GPU calc_light early-return (uncommitted edit!) | shaders/include/lighting.hglsl:195-200 | ⚠ working-tree state, NOT in HEAD `d494638` per `git blame` |
| GPU `final = base_light` loop seed | shaders/include/lighting.hglsl:202 | ✓ |
| GPU TG_LIGHT_AMBIENT | shaders/include/lighting.hglsl:210-212 | ✓ |
| GPU TG_LIGHT_INFINITE | shaders/include/lighting.hglsl:213-216 | ✓ |
| static_prop.vert get_base_light call | shaders/static_prop.vert:152-157 | ✓ hardcodes `false, 0.0, false, false` |
| static_prop.vert calc_light call | shaders/static_prop.vert:199 | ✓ |
| static_prop.vert aRGBHighlight add | shaders/static_prop.vert:211 | ✓ |
| static_prop.vert parity pack | shaders/static_prop.vert:232-247 | ✓ |
| Per-vertex VBO write of aRGBLight | gos_static_prop_batcher.cpp:578 | ✓ memcpy from `typeShape->listOfTypeVertices[localVertIdx].aRGBLight` |
| Per-vertex VBO attribute bind (offset 36) | gos_static_prop_batcher.cpp:668-669 | ✓ `glVertexAttribIPointer(4, 1, GL_UNSIGNED_INT, ..., (void*)36)` |
| GpuStaticPropInstance offset asserts | gos_static_prop_batcher.h:29-35 | ✓ `aRGBHighlight` at offset 80 |
| BldgAppearance::update SetLightList | mclib/bdactor.cpp:2223 | ✓ |
| BldgAppearance::update CacheGpuLightData | mclib/bdactor.cpp:2247 | ✓ |
| BldgAppearance::update TransformMultiShape | mclib/bdactor.cpp:2264 | ✓ (parity-armed branch) |
| submitMultiShape lightDataIndex resolution | gos_static_prop_batcher.cpp:1076-1083 | ✓ uses `cachedGpuLightIndex_` |
| submitMultiShape passes child->aRGBHighlight | gos_static_prop_batcher.cpp:1116 | ✓ |
| GatherLightsParameters | mclib/txmmgr.cpp:939-1056 | ✓ |
| addLightDataStructure dedupe by memcmp | mclib/txmmgr.cpp:831-836 | ✓ |
| LightsData UBO upload | mclib/txmmgr.cpp:1118-1127 | ✓ size = `lightDataStructuresCount * sizeof(TG_HWLightsData)` |
| resetLightData per-frame | mclib/txmmgr.h:1221, txmmgr.cpp:855-858 | ✓ |
| Parity snapshot reads listOfVertices[vi].argb (NOT listOfTriangles[].aRGBLight) | gos_static_prop_batcher.cpp:851 | ✓ |
| `BaseVertexColor` default 0 | code/logmain.cpp:77, code/mechcmd2.cpp:164 | ✓ |
| `BaseVertexColor` upload to SceneData (.zyxw swizzle) | mclib/txmmgr.cpp:1147 | ✓ |

**Negative-claim grep verifications** (per CLAUDE.md):

- "GPU early-return at lighting.hglsl:196-200 with `return base_light` is NOT in HEAD `d494638`": `git blame -L 195,205 shaders/include/lighting.hglsl` shows lines 196,197,198,200 as `Not Committed Yet` (working-tree edits). HEAD versions of those lines do NOT contain the `#ifdef MC2_STATIC_PROP_LIGHTING / return base_light / #else / #endif` block — `git log -p -G"return base_light" -- shaders/include/lighting.hglsl` returns no commits introducing the phrase. **Confirmed: N1 fix is uncommitted.**
- "typeId=474 is NOT GenericAppearance": dispatch states this directly; cross-referenced bdactor.cpp:2223+ (the Building path) and noted that GenericAppearance::update with `SetLightList(NULL,0)` at genactor.cpp:1201 would zero numLights, but the Building path at bdactor.cpp:2223 passes `eye->getWorldLights(), eye->getNumLights()` which is non-zero. **Confirmed: numLights=2 from the dispatch is consistent with Building path; the recon's misidentification of typeId=474 as Generic is the source of its #1 hypothesis being inapplicable here.**
- "static_prop.vert does NOT propagate per-vertex aRGBLight alpha into v_argb output (matches CPU's hardcoded 0xFF alpha)": verified by reading static_prop.vert:218 (`v_argb = vec4(lit, 1.0)`) and parity pack alpha hardcode (`a8 = 255u` at line 238). **Confirmed.**
- "GPU has no `lighteningLevel` consumer": grep'd `lighteningLevel` in shaders/ — zero hits. Recon §4 already verified this; re-confirmed.

Load-bearing memory cross-references:

- ⭐ `cpp_glsl_ubo_struct_lockstep.md` — TG_HWLightsData ↔ ObjectLights layout match verified at tgl.h:304-310 ↔ lighting.hglsl:31-37. Total size 1808 on both sides. ✓
- ⭐ `mc2_argb_packing.md` — confirmed swizzle behavior in static_prop.vert:137-141 (BGRA decode of DWORD via uint32 attribute) + lighting.hglsl:52-58 (re-pack to original DWORD for magic comparison). ✓
- `feedback_data_flow_audit_asymmetry.md` — applied (negative-claim greps in both directions for §4 and §10).
- `feedback_offload_scope_stock_only.md` — recon scope is stock tier1+tier2 (mc2_18 is stock).

**Fix scope recommendation:** **rolled-into-N2 (defer; need more recon)** — typeId=474 cannot be diagnosed from static analysis alone. Commit the N1 fix (already in working tree) for the GenericAppearance class it does close, but do NOT scope it as fixing typeId=474. Dispatch a focused diagnostic to confirm whether the divergence is at `inst.aRGBHighlight` (Hypothesis B) or `light[lightDataIndex].numLights` (Hypothesis D) before proposing a fix.
