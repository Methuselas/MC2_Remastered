# typeId=474 CPU Lighting-Kernel Recon — `aRGBLight=0xFFFFFFE7 → lit=0x818181`

Date: 2026-05-03
Branch: `claude/nifty-mendeleev`
HEAD: `d494638`
Scope: code-grounded, READ-ONLY. Identifies what CPU `MultiTransformShape` does to `aRGBLight=0xFFFFFFE7` between function entry and the per-vertex final ARGB write that produces `lit ARGB = 0xFF818181` (vert 24/27) and `0xFF1F1F1F` (vert 29).

Skill applied: `.claude/skills/adversarial-plan-review.md` (verification appendix in §10).

---

## §1. Executive summary

CPU produces `lit = 0xFF818181` from `aRGBLight = 0xFFFFFFE7` because the **per-vertex tag is IGNORED** for this shape: `TG_Shape::lightsOut == true`, which gates the non-magic colored branch at `mclib/tgl.cpp:1878-1886` via the `if (!lightsOut)` test on line 1880. With the per-vertex seed gated off, `redFinal/greenFinal/blueFinal` stay at the iteration-entry init value of 0 (`tgl.cpp:1794`). The lighting loop (`tgl.cpp:1938-2205`) then adds **AMBIENT** (constant per-shape, ~0x80 in stock day light) plus a per-vertex **INFINITE** contribution that depends on `dot(s_lightDir, listOfTypeVertices[j].normal)` — yielding the small per-vertex variation that distinguishes vert 24 (`0x81`) from vert 29 (`0x1F`). The reduction from `0xFFFFFFE7` to grey is therefore not a "scale" — it is a **branch suppression**: the per-vertex tag never enters the math at all.

**Prior recon's claim verdict (`2026-05-03 typeId474-static-recon §4`):** the recon's reading of lines 1878-1886 (the BGR-byte seed) is structurally correct as a description of the branch body, but **omits the load-bearing `if (!lightsOut)` gate on line 1880**. For Building shapes — including typeId=474 — `lightsOut=true` is set by `bdactor.cpp:2048` (`bldgShape->SetLightsOut(true)` when `forceLightsOut==true`) or by `code/bldng.cpp:864/1504` (power-out / destroyed paths). When this gate fires, the seed code the prior recon quoted **never runs**, and `redFinal/greenFinal/blueFinal` stay 0 entering the lighting loop. The recon did not consider this gate; that omission is what made its CPU output prediction (~white) inconsistent with the observed CPU output (~grey).

**Critical finding for GPU mirror:** GPU's `get_base_light()` at `shaders/include/lighting.hglsl:131-135` (`MC2_STATIC_PROP_LIGHTING` swizzle gate) has no analog of `lightsOut` — for non-magic tags it ALWAYS returns `startVLight.zyx` (the BGR-decoded RGB). For typeId=474 with `lightsOut=true` semantically active, GPU returns `base_light = (1.0, 1.0, 0.906)` while CPU's seed is `(0,0,0)`. After the lighting loop adds AMBIENT (~0.502) + INFINITE (~0.005), GPU saturates to `vec3(1)` (clamp at lighting.hglsl:145 and at static_prop.vert:211), producing `0xFFFFFFFF`; CPU stays at `0x81`. **GPU needs a `lightsOut` equivalent** — either an instance flag passed through SSBO (`flags & kFlagLightsOut`) gating the non-magic seed branch, or precomputing the seed CPU-side and writing it into the per-vertex VBO. The shape of the fix is small (1 flag + 1 conditional) but requires a struct extension; see §7.

---

## §2. The verbatim CPU code path for vert=24 (aRGBLight=0xFFFFFFE7, lightsOut=true)

Function: `TG_Shape::MultiTransformShape` at `mclib/tgl.cpp:1674`. The per-vertex loop opens at line 1745.

### Entry to the loop body — fresh stack-locals at line 1794-1796

```cpp
//----------------------------------------------------
// Lighting goes here.
DWORD redFinal=0, greenFinal=0, blueFinal=0;        // line 1794
DWORD redAmb = 0, greenAmb = 0, blueAmb = 0;        // line 1795
DWORD redSpec=0, greenSpec=0, blueSpec=0;           // line 1796
```

State after init: `redFinal=greenFinal=blueFinal=0`.

### Optional `lighteningLevel` seed at line 1798-1801

```cpp
if (lighteningLevel > 0)
{
    redSpec = blueSpec = greenSpec = lighteningLevel;
}
```

`lighteningLevel` is a `static DWORD` initialized to 0 at `tgl.cpp:85`; only weather-bolt frames set it nonzero. **For stock daytime: skipped.** Note this writes only to `redSpec/greenSpec/greenSpec`, which are never read by the lighting loop in any path that affects redFinal/greenFinal/blueFinal — the spec channel is only added to the per-face `listOfColors[].redSpec` bucket via TG_LIGHT_TERRAIN (line 2146-2148) or TG_LIGHT_POINT/SPOT spec accumulators (lines 2093-2095, 2117-2119, 2196-2198) and never read back into `redFinal/greenFinal/blueFinal` before the final pack at line 2232. So `lighteningLevel` does not affect the final ARGB byte.

### Read tag at line 1803

```cpp
DWORD startVLight = theShape->listOfTypeVertices[j].aRGBLight;
```

For vert 24 (sharedVert per the trace), this loads `0xFFFFFFE7`.

### Magic-tag branches at lines 1804-1877

Each compares `startVLight` to a magic constant:
- `0xffff00ff` (Hot Pink) — line 1804
- `0xffffff00` (Hot Yellow) — line 1829
- `0xff00ff00` (Hot Green) — line 1851
- `0xffff0000` (Hot Red) — line 1870 (empty body)
- `0xff0000ff` (Hot Blue) — line 1874 (empty body)

`0xFFFFFFE7` matches none of these. **All five branches skipped.**

### THE KEY BRANCH: non-magic colored at lines 1878-1886

```cpp
   else if (startVLight & 0x00ffffff)        //Some other kind of light, just add it in.
   {
        if (!lightsOut)
        {
            redFinal = (startVLight>>16) & 0x000000ff;
            greenFinal = (startVLight>>8) & 0x000000ff;
            blueFinal = (startVLight) & 0x000000ff;
        }
   }
```

For `startVLight = 0xFFFFFFE7`:
- Outer test: `0xFFFFFFE7 & 0x00FFFFFF = 0x00FFFFE7`, non-zero → **enter branch.**
- Inner test: `if (!lightsOut)`.
  - **If lightsOut=false:** body fires. redFinal = 0xFF, greenFinal = 0xFF, blueFinal = 0xE7. (This is the prior recon's reading.)
  - **If lightsOut=true:** body skipped. redFinal/greenFinal/blueFinal stay at 0.

**Trace data forces lightsOut=true for typeId=474.** No other path reachable from `aRGBLight=0xFFFFFFE7` produces a CPU output of 0x818181 — every other branch either sets the seed from a hot* constant (0xFF magnitude bytes) or leaves redFinal=0 (which is what we need).

### HUD fallback at line 1887-1890

```cpp
else if (isHudElement)
{
    redFinal = blueFinal = greenFinal = 0xff;
}
```

`isHudElement` is a function parameter; for in-mission building rendering it is false. **Skipped.**

### `BaseVertexColor` add at 1892-1905

`BaseVertexColor` is a static field default 0 (per substrate-gap recon §10 verification of `code/logmain.cpp:77`); no add for stock missions. **No-op.**

### Lighting gate at 1934-1938

```cpp
if (useVertexLighting && (Environment.Renderer != 3))
{
    if (!isSpotlight && !isWindow)
    {
        for (long i=0;i<s_numLights;i++)
```

For the trace inputs (numLights=2, building shape), all three gates pass: enter the loop.

### State BEFORE loop: `redFinal=greenFinal=blueFinal=0` (since lightsOut=true).

---

## §3. The seed at lines 1878-1886 — what it ACTUALLY does for typeId=474

Quoted verbatim above (§2). The corrected reading is:

> The block at 1878-1886 is the non-magic colored seed branch. Lines 1882-1884 seed redFinal/greenFinal/blueFinal from the BGR bytes of `startVLight`. **But this seed runs only when `lightsOut == false`** (line 1880 guard). For Building shapes with `lightsOut == true`, the entire seed body at lines 1882-1884 is skipped, and redFinal/greenFinal/blueFinal retain their iteration-entry value of 0 from line 1794.

For `aRGBLight=0xFFFFFFE7` on a building with `lightsOut=true`:
- **Immediately after the block at 1878-1886:** redFinal=0, greenFinal=0, blueFinal=0.

For the same `aRGBLight=0xFFFFFFE7` on a non-`lightsOut` shape (mech, dynamic actor):
- **Immediately after the block at 1878-1886:** redFinal=0xFF, greenFinal=0xFF, blueFinal=0xE7.

**The prior recon read the inner body but missed the outer guard.** That's the entire mismatch.

---

## §4. There is no "downstream scale" — the divergence is at the seed, not after it

The dispatch hypothesized "a scale operation between 1878-1886 and the final write that reduces ~0xFFFFE7 to ~0x81." **No such scale exists.** Static analysis of `tgl.cpp:1886-2231` (the post-seed code path) confirms:

- Lines 1892-1905 (`BaseVertexColor`): adds `(BaseVertexColor>>n)&0xFF` to redFinal/greenFinal/blueFinal, then clamps each to `<= 0xFF`. Default 0; clamp-to-255 is a CEILING operation, not a scale-down. **Never reduces.**
- Lines 1934-2210 (lighting loop): each light type *adds* contributions to redFinal/greenFinal/blueFinal (TG_LIGHT_AMBIENT via redAmb/greenAmb/blueAmb tail-add at 2207-2209; TG_LIGHT_INFINITE at 1984-1986; TG_LIGHT_INFINITEWITHFALLOFF at 2057-2059; TG_LIGHT_POINT writes only to `redSpec/greenSpec/blueSpec`, never to redFinal/greenFinal/blueFinal). **Loop only adds; never subtracts or scales the existing value.**
- Lines 2212-2219 (final clamp): `if (redFinal > 255) redFinal = 255;` — saturates to 255, no scale-down.
- Line 2232 (pack): `argb = (0xff<<24) + (redFinal<<16) + (greenFinal<<8) + blueFinal` — pure pack, no math.

**Negative-claim grep verification (per `feedback_data_flow_audit_asymmetry.md`):**

- `Grep "redFinal\s*\*=" tgl.cpp` — only matches inside the Hot Pink / Hot Yellow / Hot Green nightFactor multiplies (lines 1818, 1845, 1865), all gated on the magic-tag branches that 0xFFFFFFE7 does not enter. No multiplication operator on redFinal in the code path taken.
- `Grep "redFinal\s*[<>]>" tgl.cpp` — no shifts on redFinal anywhere.
- `Grep "redFinal\s*-=" tgl.cpp` — no subtractions.
- `Grep "redFinal\s*/=" tgl.cpp` — no divisions.

**Conclusion:** between lines 1886 and 2232, redFinal/greenFinal/blueFinal can only be: re-seeded (only inside `BaseVertexColor` add — additive), incremented (loop body), or saturation-clamped. There is no scale-down. The "reduction from 0xFFFFE7 to 0x81" the dispatch's framing implied does not exist; the value started at 0 and only went up.

---

## §5. Per-vertex variation (vert 24 = 0x81, vert 29 = 0x1F)

With `redFinal/greenFinal/blueFinal` starting at 0 entering the lighting loop, the entire output for both verts comes from the loop. For numLights=2 (stock day mission, 1 AMBIENT + 1 INFINITE per `s_listOfLights`):

### TG_LIGHT_AMBIENT (lines 1945-1958)

```cpp
case TG_LIGHT_AMBIENT:
{
    redAmb = ((startLight>>16) & 0x000000ff);
    greenAmb = ((startLight>>8) & 0x000000ff);
    blueAmb = ((startLight) & 0x000000ff);
}
```

`redAmb`/`greenAmb`/`blueAmb` are **per-shape constants** — they don't depend on the per-vertex normal. For a stock day mission with AMBIENT aRGB=0x80808080 (or similar), redAmb=greenAmb=blueAmb≈0x80. **Same for vert 24 and vert 29.**

### TG_LIGHT_INFINITE (lines 1961-2030)

```cpp
float cosine = s_lightDir[i].x * theShape->listOfTypeVertices[j].normal.x;
cosine += s_lightDir[i].y * theShape->listOfTypeVertices[j].normal.y;
cosine += s_lightDir[i].z * theShape->listOfTypeVertices[j].normal.z;

if (cosine < 0.0f)
{
    float cos = fabs(cosine);
    float red = float((startLight>>16) & 0x000000ff) * cos;
    float green = float((startLight>>8) & 0x000000ff) * cos;
    float blue = float((startLight) & 0x000000ff) * cos;

    redFinal += float2long(red);
    greenFinal += float2long(green);
    blueFinal += float2long(blue);
}
```

This is **per-vertex-aware**: `cosine = dot(s_lightDir, listOfTypeVertices[j].normal)`. The contribution is gated `cosine < 0.0f` (i.e., light hitting the *front* face — the `s_lightDir` convention here is light-vector-pointing-away-from-the-source-toward-objects, so back-facing-the-light has positive cosine and contributes nothing). When it does contribute, the magnitude is `|cosine| * lightColor`.

### Tail-add at 2207-2209

```cpp
redFinal += redAmb;
blueFinal += blueAmb;
greenFinal += greenAmb;
```

### Reconstructing the trace data

Vert 24: redFinal_loop = redAmb + INFINITE_R(vert24). For output 0x81 = 129 with redAmb=0x80=128: INFINITE adds ~1. That's `cosine_24 ≈ -0.008` × lightColor 0x80 = ~1. Plausible for a face nearly perpendicular to the light direction.

Vert 29: redFinal_loop = redAmb + INFINITE_R(vert29) = 0x1F = 31. For redAmb=0x80=128 → cosine_29 must subtract 97 — but INFINITE only ADDS. **Inconsistency.**

**Resolution:** redAmb is not 0x80 for this shape. Either the AMBIENT light for typeId=474's `s_listOfLights` slot 0 is darker than my assumption (e.g., aRGB ≈ 0x1F1F1F1F, giving redAmb=0x1F), OR there's a TG_LIGHT_AMBIENT contribution I'm missing (only one TG_LIGHT_AMBIENT path exists in the code at line 1945; no other adds to redAmb). Most likely: redAmb≈0x1F (low ambient), and INFINITE adds 0x62 for vert 24 (cosine_24 ≈ -0.76 × 0x80 = ~98), and 0 for vert 29 (back-facing with cosine ≥ 0). That gives:
- vert 24: 0x1F + 0x62 = 0x81. ✓
- vert 29: 0x1F + 0x00 = 0x1F. ✓

So the per-vertex variation is fully explained by `cosine = dot(s_lightDir, normal_j)` differing between vert 24 (front-facing the infinite light, cosine<0, additive) and vert 29 (back-facing, cosine>=0, no add). Both verts share the same constant AMBIENT (≈0x1F).

This is **per-vertex-normal-driven**, exactly as the dispatch hypothesized. The CPU operation we need GPU to mirror is:
1. The `lightsOut` gate that suppresses the seed at line 1880.
2. Everything else (AMBIENT add, INFINITE n_dot_l) is already mirrored in `lighting.hglsl::calc_light` lines 210-216 — the GPU lighting loop produces matching numbers when given the same inputs (numLights, lightDir, lcolor, normal). The substrate-gap recon's §3 walk confirms structural equivalence for these light types.

---

## §6. The non-magic branch's overall behavior, summarized

For ANY non-magic `aRGBLight` tag (the `else if (startVLight & 0x00ffffff)` arm), CPU does ONE of two things:

| `lightsOut` state | Output of branch body | Comment |
|---|---|---|
| `false` (default) | redFinal/greenFinal/blueFinal seeded from BGR bytes of tag | Per-vertex tag-color self-illumination |
| `true` | redFinal/greenFinal/blueFinal stay at iteration-entry 0 | Tag IGNORED; lighting loop alone determines color |

`lightsOut` is set per-`TG_Shape` instance. Default `false` (`tgl.h:833`, `tgl.cpp:252,468`). Set `true` by:
- `bdactor.cpp:2048` (BldgAppearance::update, when `forceLightsOut==true` — destroyed-power path)
- `bdactor.cpp:4377` (TreeAppearance::update, similar)
- `gvactor.cpp:2651` (GVAppearance, vehicle destroyed)
- `mech3d.cpp:3016/3020` (Mech3D, conditional)
- `code/bldng.cpp:864,1504` (BuildingObject power-out / destroyed)
- `code/terrobj.cpp:503,567,930,987` (terrain-object destroyed)
- `code/turret.cpp:801` (turret destroyed)

**The semantic:** "this shape is a destroyed/dark variant; do not self-illuminate from per-vertex tags; let dynamic lights determine the visible color." Tags like `0xFFFFFFE7` are designer-baked window-glow / spotlight-glow / panel-glow markers that should DISAPPEAR when the building is destroyed — exactly what the gate accomplishes.

---

## §7. The GPU's missing operation

GPU's `get_base_light` at `shaders/include/lighting.hglsl:131-135`:

```glsl
#ifdef MC2_STATIC_PROP_LIGHTING
    final = startVLight.zyx;        // <-- non-magic seed; .zyx swizzles the BGR-uint to RGB
#endif
```

There is **no `lightsOut` equivalent**. For typeId=474 with `lightsOut=true` semantically active, this line still seeds `final = (1.0, 1.0, 0.906)` for `aRGBLight=0xFFFFFFE7`, which `calc_light` then uses as `base_light` — and the lighting loop adds AMBIENT + INFINITE on top, saturating to `vec3(1)` after the clamp at line 145 / 211.

**Semantic fix needed:** GPU must be told whether the shape has `lightsOut==true`, and if so, return `vec3(0)` from the non-magic seed branch (matching the gated-off CPU path at lines 1880-1885). All other branches (Hot Pink / Yellow / Green / Red / Blue / HUD) are unaffected because they don't depend on the `if (!lightsOut)` test — the magic-tag branches are unconditional within their respective magic-match arms.

**Shape of the change** (semantic only — no code proposed here per dispatch's "no code yet" rule):
- Add a per-instance `lightsOut` boolean to the GPU side, sourced from the C++ `TG_Shape::lightsOut` field at submit time.
- Either: (a) extend `GpuStaticPropInstance.flags` with a new `kFlagLightsOut` bit at submit time in `gos_static_prop_batcher.cpp::submitMultiShape`, OR (b) bake it into the per-vertex VBO (more expensive: per-vertex repeats but no struct extension).
- In `lighting.hglsl::get_base_light`, gate the non-magic colored branch (line 131-135) with `if (!lightsOut)` — mirroring the CPU `if (!lightsOut)` at `tgl.cpp:1880`.
- Predicted effect on inventory: typeId=474 white-out resolves; any other Building/Tree/GV/Mech destroyed-state actors with non-magic per-vertex tags also resolve. Magnitude depends on how many destroyed buildings have non-magic colored-tag verts — likely small population per mission but high visibility (destroyed buildings are usually in-frame).

The fix is **small (~5-10 lines across 2 files: 1 shader + 1 batcher submit site)** assuming the `flags` SSBO field has a free bit. If `flags` is full (need to grep `GpuStaticPropInstance::flags` definition), it bumps to **medium** (struct extension + UBO/SSBO size update + parity-pack version bump per `cpp_glsl_ubo_struct_lockstep.md`).

---

## §8. Was the prior recon right?

**Verdict: structurally correct, gate omitted.**

The prior recon's `2026-05-03-typeId474-static-recon.md §4` quoted the seed body at lines 1882-1884 verbatim (the BGR-byte read into redFinal/greenFinal/blueFinal). That quote is correct; those three lines do exactly what the recon claimed. The omission is the line-1880 `if (!lightsOut)` gate immediately above.

The recon's structural hypothesis tree (Hypothesis A through E) explored `numLights==0` (Hypothesis D), `aRGBHighlight!=0` (Hypothesis B), and `Hot Green` daytime (Hypothesis E) — all GPU-side data corruption / extra-ifdef hypotheses. **None of them considered "CPU's non-magic seed never fires for this shape".** The recon's framing assumed both pipelines would seed redFinal/greenFinal/blueFinal from the BGR bytes (CPU at 1882-1884, GPU at lighting.hglsl:131) and looked for divergences AFTER the seed. The actual divergence is **at** the seed: CPU's seed is gated by `lightsOut`, GPU's is not.

The recon's Hypothesis B (non-zero aRGBHighlight on GPU side) was the closest to the right answer — it correctly identified that GPU saturates from a high pre-loop value while CPU does not. But it pinned the "high pre-loop value" on `aRGBHighlight` (a per-instance field added AFTER calc_light at static_prop.vert:211) rather than on `base_light` itself (the per-vertex magic-tag-decoded value used WITHIN calc_light). Given the ambiguity between aRGBHighlight saturation and base_light saturation in the visible output (both produce `0xFFFFFFFF`), this conflation is understandable — the resolution requires reading the CPU `if (!lightsOut)` gate, which is the load-bearing piece.

**Specific corrections to the prior recon:**
1. §5 Hypothesis A's "default tag (0xFF000000)" assumption was unnecessary — the actual tag IS `0xFFFFFFE7`, but it doesn't matter because lightsOut gates the seed.
2. §5 Hypothesis B's "non-zero aRGBHighlight" speculation was a near-miss; the saturation source is base_light from the ungated GPU non-magic branch, not aRGBHighlight.
3. §7 Cause statement should add a sixth hypothesis: **CPU's non-magic seed gate (lightsOut) has no GPU mirror**. This is the likely cause and is statically verifiable (no need for runtime diagnostic dispatch).
4. §9 Open Question 5 ("recon's CPU value derivation requires vert=29's tag to be 0xFF1F1F1F") is now resolved differently — vert=29's tag is also 0xFFFFFFE7 (from the trace), and the per-vertex variation comes from normals via the INFINITE light cosine, not from per-vertex tag differences.

---

## §9. Open questions / what static analysis can't answer

1. **Confirmation that the typeId=474 shape instance in mc2_18 actually has `lightsOut == true`.** Static analysis identifies the gate semantics and the call sites that set it true (§6), but cannot confirm at-frame state without runtime instrumentation. **Recommended diagnostic:** add a one-shot print in `bdactor.cpp::BldgAppearance::update` after line 2048 dumping `bldgShape->lightsOut` at the moment of the parity-armed render for typeId=474. If true, this recon's cause statement is confirmed.

2. **Whether the call site for `lightsOut=true` is `bdactor.cpp:2048` (BldgAppearance::update with forceLightsOut), `bldng.cpp:864` (powerSupply destroyed), or `bldng.cpp:1504` (BuildingObject destroyed).** The trace data is consistent with all three. Static analysis cannot distinguish. The diagnostic above also surfaces this.

3. **The exact AMBIENT/INFINITE light values for the s_listOfLights slot on this shape.** §5 reverse-engineered redAmb≈0x1F from output values — confirmation requires dumping `s_listOfLights[0]->GetaRGB()` and `s_listOfLights[1]->GetaRGB()` at the parity render moment. Not load-bearing for the cause statement; the per-vertex variation explanation (cosine-driven INFINITE add) holds regardless of exact values.

4. **Whether other typeIds with the same symptom share `lightsOut=true`.** The dispatch's "white-out" class likely contains other destroyed-state buildings. Static analysis identifies the gate but not the population. A grep over the parity log for "white-out class" actor IDs would correlate.

---

## §10. Adversarial review checklist

Per `.claude/skills/adversarial-plan-review.md` (high-stakes recon — load-bearing decision input).

### Symbols cited (every grep-verified live)

| Symbol / Site | File:Line | Match |
|---|---|---|
| `TG_Shape::MultiTransformShape` signature | mclib/tgl.cpp:1674 | ✓ verbatim |
| Per-vertex loop entry | mclib/tgl.cpp:1745 | ✓ |
| Lighting locals init (redFinal=0 etc.) | mclib/tgl.cpp:1794-1796 | ✓ verbatim |
| `lighteningLevel` seed (spec only) | mclib/tgl.cpp:1798-1801 | ✓ verbatim |
| `lighteningLevel` static field default | mclib/tgl.cpp:85 | ✓ `DWORD TG_Shape::lighteningLevel = 0;` |
| `startVLight = listOfTypeVertices[j].aRGBLight` | mclib/tgl.cpp:1803 | ✓ verbatim |
| Hot Pink magic match `0xffff00ff` | mclib/tgl.cpp:1804 | ✓ |
| Hot Yellow magic match `0xffffff00` | mclib/tgl.cpp:1829 | ✓ |
| Hot Green magic match `0xff00ff00` | mclib/tgl.cpp:1851 | ✓ |
| Hot Red empty body `0xffff0000` | mclib/tgl.cpp:1870-1873 | ✓ |
| Hot Blue empty body `0xff0000ff` | mclib/tgl.cpp:1874-1877 | ✓ |
| **Non-magic outer test `& 0x00ffffff`** | mclib/tgl.cpp:1878 | ✓ verbatim |
| **`if (!lightsOut)` inner gate** | mclib/tgl.cpp:1880 | ✓ verbatim — load-bearing |
| Seed body BGR → R/G/B | mclib/tgl.cpp:1882-1884 | ✓ verbatim |
| HUD fallback | mclib/tgl.cpp:1887-1890 | ✓ |
| `BaseVertexColor` add + clamp | mclib/tgl.cpp:1892-1905 | ✓ |
| Lighting gate | mclib/tgl.cpp:1934-1938 | ✓ |
| TG_LIGHT_AMBIENT body | mclib/tgl.cpp:1945-1958 | ✓ verbatim |
| TG_LIGHT_INFINITE body | mclib/tgl.cpp:1961-2030 | ✓ verbatim |
| TG_LIGHT_INFINITEWITHFALLOFF | mclib/tgl.cpp:2033-2062 | ✓ |
| TG_LIGHT_POINT writes only redSpec | mclib/tgl.cpp:2064-2123 | ✓ verbatim — redSpec only, never redFinal |
| TG_LIGHT_TERRAIN writes listOfColors[] | mclib/tgl.cpp:2125-2158 | ✓ |
| TG_LIGHT_SPOT writes only redSpec | mclib/tgl.cpp:2160-2202 | ✓ |
| Tail ambient add | mclib/tgl.cpp:2207-2209 | ✓ |
| Final clamp 255 | mclib/tgl.cpp:2212-2219 | ✓ |
| Final argb pack | mclib/tgl.cpp:2232 | ✓ |
| `lightsOut` field declaration | mclib/tgl.h:763 | ✓ `bool lightsOut;` |
| `lightsOut` default false (TG_Shape ctor) | mclib/tgl.h:833 | ✓ `lightsOut = false;` |
| `lightsOut` setter | mclib/tgl.h:933-936 | ✓ `SetLightsOut` |
| TG_TypeShape default false | mclib/tgl.cpp:252,468 | ✓ |
| Top-of-function `if (lightsOut) { isNight=false; nightFactor=0; }` | mclib/tgl.cpp:1694-1698 | ✓ — confirms lightsOut affects multiple downstream branches consistently |
| BldgAppearance setLightsOut(true) call | mclib/bdactor.cpp:2048 | ✓ `if (forceLightsOut) bldgShape->SetLightsOut(true);` |
| BuildingObject powerSupply destroyed | code/bldng.cpp:864 | ✓ |
| BuildingObject destroyed flow | code/bldng.cpp:1504 | ✓ |
| GPU non-magic seed `final = startVLight.zyx` | shaders/include/lighting.hglsl:131-135 | ✓ confirmed (per substrate-gap recon §3, re-grep'd) |

### Negative-claim grep verifications (asymmetric — opposite-direction)

- **"redFinal/greenFinal/blueFinal are never multiplied by a fractional constant in the code path taken (post-seed → final-pack)":** grep'd `redFinal\s*\*=` and `greenFinal\s*\*=` in tgl.cpp — only matches inside Hot Pink (1818), Hot Yellow (1845), Hot Green (1865) blocks, all gated by magic-tag matches that 0xFFFFFFE7 does not enter. **Confirmed: no scale-down operation in code path.**
- **"redFinal is never right-shifted, divided, or subtracted":** grep'd `redFinal\s*[>]>=`, `redFinal\s*/=`, `redFinal\s*-=` in tgl.cpp — zero hits. **Confirmed.**
- **"TG_LIGHT_POINT and TG_LIGHT_SPOT do not contribute to redFinal/greenFinal/blueFinal":** read both branches verbatim (lines 2064-2123 and 2160-2202). Both write only to `redSpec/greenSpec/blueSpec` (specular accumulators). The specular accumulators are never read into redFinal/greenFinal/blueFinal in `MultiTransformShape` — they're consumed via the per-face `listOfColors[].redSpec` route at line 2146-2148 (TG_LIGHT_TERRAIN). **Confirmed: only TG_LIGHT_AMBIENT, TG_LIGHT_INFINITE, and TG_LIGHT_INFINITEWITHFALLOFF can affect the per-vertex argb byte output.** This is consistent with §5's "AMBIENT + INFINITE n_dot_l" math.
- **"GPU has no `lightsOut` analog in the lighting kernel":** grep'd `lightsOut`, `lights_out`, `kFlagLightsOut` in `shaders/` — zero hits. **Confirmed.**
- **"`lightsOut` defaults to false, so the gate doesn't fire universally":** grep'd `lightsOut\s*=\s*false` and `lightsOut\s*=\s*true` across all of mclib/, code/. False defaults at tgl.cpp:252, 468; tgl.h:833. True only at the destroyed/power-out call sites enumerated in §6. **Confirmed: gate is selective; only Buildings/Trees/GVs/Mechs/Turrets in destroyed state hit `lightsOut=true`.**
- **"The per-vertex argb output for vert 24/27/29 (all sharedVert=6 or 9) does not depend on the soup-vertex index":** the trace shows vert 24, 27 produce identical `0x818181` despite being different soup-vert indices. They map to the same sharedVert. Since the argb is read from `listOfVertices[sharedVert].argb`, both soup positions read the same value. **Confirmed: sharedVert determines output, not soup index — consistent with the indirection at gos_static_prop_batcher.cpp:911.**

### Load-bearing memory cross-references

- ⭐ `cpp_glsl_ubo_struct_lockstep.md` — extending GpuStaticPropInstance with kFlagLightsOut requires lockstep GLSL update; the §7 fix scope estimate accounts for this.
- ⭐ `mc2_argb_packing.md` — the `.zyx` swizzle on `startVLight` (BGR-decoded uint → RGB float vec3) is correct per the byte order stored in DWORD `aRGBLight`.
- `feedback_data_flow_audit_asymmetry.md` — applied for negative claims in this section (grep'd both candidate consumers and source-end declarations).
- `feedback_offload_scope_stock_only.md` — typeId=474 in mc2_18 is stock content; this recon is in-scope.

### GPU fix scope estimate

**Small (1-2 file change, ~5-10 lines):** add `kFlagLightsOut` bit to `GpuStaticPropInstance::flags` (assuming a free bit exists — needs grep verification of current flag bit usage), set it from `child->lightsOut` in `gos_static_prop_batcher.cpp::submitMultiShape`, and gate the non-magic seed branch in `lighting.hglsl:131-135` with the corresponding `(inst.flags & kFlagLightsOut) == 0` test. If `flags` is full, scope bumps to **medium** (struct extension requires SSBO size bump and parity-pack version increment per `cpp_glsl_ubo_struct_lockstep.md`). Either way, no shader-architecture change, no schema rev — the operation is mechanical and well-localized.

---

**End of recon.**
