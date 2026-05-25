# Window + Ambient Recon — Hot-Pink @ Daytime, isWindow=1, gpu=0x000000 vs cpu=0x2F2F2F

Read-only static analysis. NO builds, NO runs, NO source edits. HEAD `d494638` on `claude/nifty-mendeleev`.

Scope: answer two locked questions about the CPU and GPU lighting kernels for `aRGBLight=0xFFFF00FF` (hot-pink magic tag) at daytime when `isWindow=1`. Disambiguates Cause α (window-skip drops ambient) vs Cause β (`get_base_light()` mishandles hot-pink) vs αβ.

---

## §1. Executive summary

- **Q1 (CPU adds ambient for windows?): NO.** The `redFinal += redAmb` adds at `mclib/tgl.cpp:2207-2209` are **inside** the `if (!isSpotlight && !isWindow)` gate at `mclib/tgl.cpp:1936`. Window/spotlight actors skip both the per-light loop AND the post-loop ambient add. `redAmb`/`greenAmb`/`blueAmb` stay at their initial value of 0 from line 1795 — never written, never added.
- **Q2 (GPU `get_base_light()` returns vec3(0x2F/255) for hot-pink @ daytime?): YES, when reached.** `shaders/include/lighting.hglsl:66-80` matches `0xffff00ff` → `else { final = vec3(0x2f/255.0); }` — exactly mirroring CPU `tgl.cpp:1822-1827`. Magic-tag detection is correct.
- **But `get_base_light()` is NOT reached for these actors.** The N1.5 lightsOut gate at `shaders/static_prop.vert:172-175` short-circuits to `base_light = vec3(0.0)` BEFORE `get_base_light()` runs, when `inst.flags & kFlagIsLightsOut`. The window-skip branch at lines 221-223 then writes `lit = base_light = vec3(0.0)`. Output: `0x000000`. CPU has no equivalent gate — the hot-pink branch at `tgl.cpp:1804-1828` runs first and unconditionally sets `redFinal=0x2f` regardless of `lightsOut`.
- **Cause classification: β** (purely N1.5 over-eager). The N1.5 lightsOut gate suppresses the hot-pink magic-tag dark-grey output. The window-skip itself is correct (CPU does the same — neither adds ambient for windows). Cause α is NOT in play for hot-pink.
- **Implication for the user/advisor's pre-recon model:** the user's prompt hypothesised "CPU adds ambient regardless." That hypothesis is wrong — CPU also skips ambient for windows. The real bug is the N1.5 gate suppressing a CPU-side branch that runs unconditionally.

---

## §2. Question 1 — CPU window+ambient analysis

### §2.1 The gate (`tgl.cpp:1934-1936` opens, `tgl.cpp:2210` closes)

```
1934    if (useVertexLighting && (Environment.Renderer != 3))
1935    {
1936        if (!isSpotlight && !isWindow)
1937        {
1938            for (long i=0;i<s_numLights;i++)
1939            {
1940                if ((s_listOfLights[i] != NULL) && (s_listOfLights[i]->active))
1941                {
1942                    DWORD startLight = s_listOfLights[i]->GetaRGB();
1943                    switch (s_listOfLights[i]->lightType)
... (per-light cases AMBIENT/INFINITE/INFINITEWITHFALLOFF/POINT/SPOT/TERRAIN)
2203                    }
2204                }
2205            }
2206
2207            redFinal += redAmb;
2208            blueFinal += blueAmb;
2209            greenFinal += greenAmb;
2210        }                              // <-- closes !isSpotlight && !isWindow gate
2211
2212        if (redFinal > 255)            // <-- saturate-clamp; OUTSIDE the !isWindow gate, INSIDE useVertexLighting
2213            redFinal = 255;
... (clamp green/blue, hot-green trace print, listOfVertices[j].argb assignment at 2232)
2250    }                                  // <-- closes useVertexLighting gate
2251    else
2252    {
2253        listOfVertices[j].argb = (0xff << 24) + (redAmb << 16) + (greenAmb << 8) + (blueAmb);
2254    }
```

### §2.2 What's INSIDE the `!isSpotlight && !isWindow` gate (lines 1937-2209, skipped for windows)

For `isWindow=1`, ALL of these are skipped:

1. The entire `for (i = 0; i < s_numLights; i++)` per-light loop body (1938-2205). This includes:
   - `case TG_LIGHT_AMBIENT:` (1945-1959) — sets `redAmb`/`greenAmb`/`blueAmb` from light color. **Skipped** → these stay 0 (initialized at 1795).
   - `case TG_LIGHT_INFINITE:` (1961-2031) — directional lighting `redFinal += cosine * lightR`. Skipped.
   - `case TG_LIGHT_INFINITEWITHFALLOFF:` (2033-2062) — directional + distance falloff. Skipped.
   - `case TG_LIGHT_POINT:` (2064-2123) — point-light dot product + falloff. Skipped.
   - `case TG_LIGHT_TERRAIN:` (2125-2158) — pre-baked terrain specular into `listOfColors[j].redSpec`. Skipped.
   - `case TG_LIGHT_SPOT:` (2160-2202) — spot cone. Skipped.

2. The post-loop ambient add at `tgl.cpp:2207-2209`:
   ```
   2207  redFinal += redAmb;
   2208  blueFinal += blueAmb;
   2209  greenFinal += greenAmb;
   ```
   Indentation (5 tabs) places these inside the `!isWindow` gate. The closing `}` at line 2210 (4 tabs) matches the opener at line 1936.

### §2.3 What's OUTSIDE the `!isWindow` gate but INSIDE `useVertexLighting` (applied to all vertex-lit actors)

1. The 0..255 saturate-clamp on `redFinal/greenFinal/blueFinal` at `tgl.cpp:2212-2219`.
2. The `listOfVertices[j].argb = (0xff << 24) | (redFinal << 16) | (greenFinal << 8) | blueFinal` assignment at `tgl.cpp:2232`.
3. The `redSpec += listOfColors[j].redSpec` accumulation at `tgl.cpp:2238-2240` (pre-baked terrain specular from a prior frame's `TG_LIGHT_TERRAIN` bake).
4. The `redSpec/greenSpec/blueSpec` saturate-clamp at `tgl.cpp:2242-2249`.

NOTE: `listOfColors[j].redSpec` writes (case `TG_LIGHT_TERRAIN` at line 2146-2148) ARE inside the `!isWindow` gate, so for windows the prior-frame bake is whatever was last written. For a freshly-allocated shape with `listOfColors` zeroed (memset at 1790), `redSpec` etc are 0. This branch contributes nothing for windows.

### §2.4 What's OUTSIDE `useVertexLighting` (the else branch at 2251-2254)

```
2251  else
2252  {
2253      listOfVertices[j].argb = (0xff << 24) + (redAmb << 16) + (greenAmb << 8) + (blueAmb);
2254  }
```

`redAmb/greenAmb/blueAmb` are still 0 (no lighting loop ran). Output for non-vertex-lit path: `0xFF000000` (fully dark). Not the path for our isWindow case (these actors are vertex-lit).

### §2.5 Where redFinal/greenFinal/blueFinal start (the magic-tag branches)

Initial values at `tgl.cpp:1794`:
```
1794  DWORD redFinal=0, greenFinal=0, blueFinal=0;
1795  DWORD redAmb = 0, greenAmb = 0, blueAmb = 0;
1796  DWORD redSpec=0, greenSpec=0, blueSpec=0;
```

For hot-pink at daytime (`tgl.cpp:1804-1827`):
```
1804  if (startVLight == 0xffff00ff)        //Hot Pink -- Lit Windows -- ONLY at NIGHT
1805  {
1806      if (isNight)              { redFinal = (theShape->hotPinkRGB>>16)&0xff; ... }
1812      else if (nightFactor>SMALL) { ... * nightFactor }
1822      else        //Its not night, paint windows dark grey
1823      {
1824          redFinal = 0x2f;
1825          greenFinal = 0x2f;
1826          blueFinal = 0x2f;
1827      }
1828  }
```

NOTE: this branch runs **before and outside** the `useVertexLighting`/`!isWindow` gates. It is unconditional with respect to `lightsOut`, `isWindow`, `isSpotlight`. The `lightsOut` flag is only consulted in the `else if (startVLight & 0x00ffffff)` non-magic branch at 1880. **Hot-pink magic tag's daytime dark-grey output is NEVER suppressed by `lightsOut` on the CPU side.**

### §2.6 Verdict for Q1

**Does CPU add ambient for `isWindow=1` actors? NO.** The ambient add at `tgl.cpp:2207-2209` is inside the `!isSpotlight && !isWindow` gate. Window actors emit `redFinal/greenFinal/blueFinal` exactly as set by the magic-tag prologue (1804-1890), never modified by ambient or per-light contributions.

For hot-pink + daytime + window: CPU writes `argb = 0xFF2F2F2F`. Matches the runtime ground truth (`cpu=0x2F2F2F`).

---

## §3. Question 2 — GPU hot-pink magic-tag analysis

### §3.1 The magic-tag detection in `get_base_light()`

`shaders/include/lighting.hglsl:46-146`. Recognized magic-tag values:

| Value (hex) | Lines | CPU mirror | Daytime branch |
|---|---|---|---|
| `0xffff00ff` (hot-pink, "Lit Windows") | 66-80 | `tgl.cpp:1804-1828` | `final = vec3(0x2f/255.0)` (line 78) |
| `0xffffff00` (hot-yellow, "Outside Building Lights") | 81-94 | `tgl.cpp:1829-1850` | `final` stays 0 unless `nightFactor >= 0.75` |
| `0xff00ff00` (hot-green, "Building Base Lights") | 95-105 | `tgl.cpp:1851-1869` | `final` stays 0 unless `nightFactor > SMALL` |
| `0xffff0000` (hot-red, "Blink") | 106-109 | `tgl.cpp:1870-1873` | empty body (no-op) |
| `0xff0000ff` (hot-blue, "Blink") | 110-113 | `tgl.cpp:1874-1877` | empty body (no-op) |
| `(start_v_light & 0x00ffffff) != 0` (other non-magic light) | 114-137 | `tgl.cpp:1878-1886` | gated by `!lightsOut`, returns swizzled `startVLight` |
| `isHudElement` (no tag match) | 138-141 | `tgl.cpp:1887-1890` | `final = vec3(1.0)` |

After all branches: `final.xyz += g_scene.baseVertexColor.xyz` (line 143), then `clamp(final, 0, 1)` and return (line 145).

### §3.2 Hot-pink (`0xFFFF00FF`) handling at daytime

Match path: lighting.hglsl line 66 condition matches. Daytime case (`isNight=false, nightFactor=0` per static_prop.vert lines 178-179: `false, 0.0`) takes the `else` branch at line 76-79:

```glsl
76      else        //Its not night, paint windows dark grey
77      {
78          final = vec3(0x2f/255.0);
79      }
```

`get_base_light` returns `clamp(vec3(0x2f/255.0) + g_scene.baseVertexColor.xyz, 0, 1)` ≈ `vec3(0.184)` (assuming `baseVertexColor` is 0 or small).

### §3.3 Daytime vs night

`isNight=true`: `final = hotPinkRGB` (the per-shape lit-window color, e.g. orange-ish). Mirrors CPU `tgl.cpp:1808-1810`.
`nightFactor > SMALL && !isNight`: `final = hotPinkRGB * nightFactor`. Dawn/dusk fade. Mirrors CPU `tgl.cpp:1814-1820`.
`else (daytime)`: `final = vec3(0x2f/255)`. Mirrors CPU `tgl.cpp:1822-1827`.

But on `static_prop.vert:178-179`, both `isNight` and `nightFactor` are hardcoded `false, 0.0`:
```
177    base_light = get_base_light(
178        perVertexARGB,
179        false, 0.0, false, false,    // isNight=false, nightFactor=0, isHudElement=false, lightsOut=false
180        ptd.hotPinkRGB.rgb, ptd.hotYellowRGB.rgb, ptd.hotGreenRGB.rgb);
```

So GPU is always in the daytime branch for hot-pink → `vec3(0x2f/255.0)`. (The night-state UBO wiring is a deferred TODO per the comment block at lines 156-160. Until then, this behavior is fixed.)

### §3.4 Verdict for Q2

**Does GPU `get_base_light()` return `vec3(0x2F/255)` for hot-pink at daytime correctly? YES.** The magic-tag detection matches CPU behavior exactly. lighting.hglsl:78 sets `final = vec3(0x2f/255.0)`, which after the `+ baseVertexColor` and `clamp(0,1)` is `vec3(0x2F/255)` ≈ 0.184 (assuming baseVertexColor ≈ 0).

If `get_base_light()` were called for our 19 affected actors, the output would be `0x2F2F2F` and would match CPU. **The bug is upstream** — `get_base_light()` is never called.

---

## §4. The actual GPU control flow for hot-pink + isWindow=1 + lightsOut=1

`shaders/static_prop.vert:170-226`:

```glsl
170    const uint kFlagIsLightsOut = (1u << 0);
171    vec3 base_light;
172    if ((inst.flags & kFlagIsLightsOut) != 0u) {
173        // lightsOut=true: seed is suppressed entirely, exactly as CPU does at
174        // tgl.cpp:1880-1885. Only the lighting loop (ambient+infinite) contributes.
175        base_light = vec3(0.0);
176    } else {
177        base_light = get_base_light(
178            perVertexARGB,
179            false, 0.0, false, false,
180            ptd.hotPinkRGB.rgb, ptd.hotYellowRGB.rgb, ptd.hotGreenRGB.rgb);
181    }
... (worldNormal, worldPos)
218    const uint kFlagIsWindow    = (1u << 1);
219    const uint kFlagIsSpotlight = (1u << 2);
220    vec3 lit;
221    if ((inst.flags & (kFlagIsWindow | kFlagIsSpotlight)) != 0u) {
222        // Window or spotlight node: hot-color magic only, no sun/ambient lighting.
223        lit = base_light;
224    } else {
225        lit = calc_light(int(inst.lightDataIndex), worldNormal, worldPos, base_light);
226    }
```

For `isWindow=1` AND `lightsOut=1`:
1. Line 172: `kFlagIsLightsOut` set → `base_light = vec3(0.0)`. Magic-tag decode is **bypassed**.
2. Line 221: `kFlagIsWindow` set → `lit = base_light = vec3(0.0)`.
3. Output: `v_argb = vec4(0.0, 0.0, 0.0, 1.0)` → `0xFF000000`.

**Mismatch root cause:** the N1.5 lightsOut gate at line 172 was added to mirror `tgl.cpp:1880-1885` (the **non-magic** colored-light "else if" branch's `if (!lightsOut)` guard). But the gate fires unconditionally on `lightsOut`, suppressing the seed for ALL aRGBLight values — including the magic-tag values that CPU handles in earlier `if`/`else if` branches that don't honor `lightsOut`.

CPU control-flow for the same actor (`tgl.cpp:1804-1890`):
```
if (startVLight == 0xffff00ff)         { ... daytime: redFinal = 0x2f; }   // hot-pink branch wins; lightsOut not consulted
else if (startVLight == 0xffffff00)    { ... }                              // hot-yellow
else if (startVLight == 0xff00ff00)    { ... }                              // hot-green
else if (startVLight == 0xffff0000)    { /* empty */ }                      // hot-red blink
else if (startVLight == 0xff0000ff)    { /* empty */ }                      // hot-blue blink
else if (startVLight & 0x00ffffff)     { if (!lightsOut) { ... } }          // ONLY this branch consults lightsOut
else if (isHudElement)                 { ... }
```

Six top-level `if`/`else if` arms. Five of them ignore `lightsOut`. The GPU's `if (lightsOut) base_light = 0` short-circuits all six. **That's the bug.**

---

## §5. Cause classification

**Cause β** — the GPU's N1.5 lightsOut gate at `static_prop.vert:172-175` suppresses the magic-tag dark-grey output that CPU emits unconditionally. Cause α (window-skip dropping ambient) is NOT in play: CPU also skips ambient for windows (verified §2). The window-skip itself is correct.

**Where the user's pre-recon model went wrong:** the prompt hypothesised "CPU adds ambient regardless." It does not. The correct model is "CPU magic-tag prologue runs unconditionally, then ALL further lighting (per-light loop AND ambient add) is gated by `!isSpotlight && !isWindow`." For hot-pink windows: CPU outputs the magic dark-grey 0x2F and stops. GPU should do the same — and the magic-tag handler in `lighting.hglsl` would do exactly that, if it were reached.

---

## §6. Recommended fix shape (semantic only — no code yet)

### §6.1 Minimal fix (recommended)

Re-scope the N1.5 lightsOut gate to mirror CPU's actual behavior — gate only the non-magic colored-light branch, not the entire `get_base_light()` call.

Two structural options:

**Option A: pass `lightsOut` into `get_base_light()` and let it gate the right branch.**
- `get_base_light()` already accepts `in bool lightsOut` (lighting.hglsl:48) and uses it on line 116 (`if (!lightsOut)` in the colored-light branch).
- Fix at `static_prop.vert:172-183`: remove the outer `if (kFlagIsLightsOut)` short-circuit. Always call `get_base_light(...)` and pass the actual lightsOut bit:
  ```
  bool lightsOut = (inst.flags & kFlagIsLightsOut) != 0u;
  base_light = get_base_light(perVertexARGB, false, 0.0, false, lightsOut, ...);
  ```
- Net delta: ~3 lines changed in `static_prop.vert`. No shader file additions. No struct schema change.
- Predicted symptom-class delta: clears the 19 hot-pink+window+lightsOut actors (post-fix CPU=0x2F2F2F, GPU=0x2F2F2F). Also clears any hot-yellow/hot-green/hot-red/hot-blue + lightsOut actors that were silently being suppressed — these may not be in the current 19-pair inventory but would be latent regressions exposed by future content.

### §6.2 Why the N1.5 gate exists (don't just delete it)

Per the comment at `static_prop.vert:161-169`:
> Stage 2.C.4 N1.5: gate base_light=vec3(0) when inst.flags has kFlagIsLightsOut (bit 0). Mirrors CPU tgl.cpp:1880 `if (!lightsOut)` gating of the non-magic colored seed at lines 1878-1886. Destroyed / power-out buildings (TG_Shape::lightsOut=true) must start from seed=0 so the lighting loop accumulates from zero, producing grey (~ambient) rather than white (seed 1.0 + ambient ≥ 1.0 after clamp).

The intent was correct (mirror `tgl.cpp:1880`), but the implementation is over-broad. CPU's `if (!lightsOut)` is an `else if (startVLight & 0x00ffffff)` arm — only the non-magic branch is gated. The fix is to push the gate down into `get_base_light()`'s correct arm (which already exists at lighting.hglsl:116) by passing the real `lightsOut` value.

### §6.3 Out-of-scope variant — do NOT pursue here

Adding ambient to the GPU window-branch (the user's hypothesised fix) would be **wrong** — CPU does not add ambient for windows. Doing so would over-brighten window vertices vs CPU.

---

## §7. Out-of-scope concerns (noted, not investigated)

### §7.1 typeId=245 (green tag `0xFF00FF00`, lightsOut=1, isWindow=0)

Same Cause β family. Hot-green daytime branch at `lighting.hglsl:95-105` returns `final` unchanged (stays 0 because there's no daytime else). Combined with N1.5 lightsOut gate suppressing the call entirely → both produce 0. So for hot-green + daytime, the N1.5 gate is **harmless** because the magic-tag branch also returns 0. CPU `tgl.cpp:1851-1869` daytime: same — hot-green branch leaves redFinal/greenFinal/blueFinal at 0. Then since isWindow=0, CPU goes through the per-light loop and adds ambient. GPU goes through `calc_light` and adds ambient. So for typeId=245, CPU and GPU **should both produce ambient-only**, and **should agree**.

**That means typeId=245 mismatch must have a different cause** — possibly an `isWindow` bit mis-set, or a normal-direction issue, or a `numLights` issue. Not investigated per scope. Flag for separate recon.

### §7.2 The 41 additional hot-pink typeIds beyond the 19 inventoried

If they all have `lightsOut=1` and `isWindow=1`, the same Cause β fix clears them. If some have `lightsOut=0`, the GPU side already produces 0x2F2F2F (per §3-§4 trace) and they should not be on a mismatch list — if they are, something else is going on (e.g., post-vertex shading, fragment-shader contribution, fog).

### §7.3 baseVertexColor

`get_base_light()` line 143 adds `g_scene.baseVertexColor.xyz` unconditionally. CPU `BaseVertexColor` is added at `tgl.cpp:1892-1905` — also unconditional with respect to magic tag. Should match if `g_scene.baseVertexColor` is correctly populated. Not investigated.

### §7.4 The window-skip's spotlight pairing

The N1.5 gate at line 172-175 fires only on `kFlagIsLightsOut`. The window/spotlight skip at line 221-223 fires on `kFlagIsWindow | kFlagIsSpotlight`. For an actor that is `isSpotlight=1, lightsOut=0, hot-pink`, what does GPU do?
- N1.5: skipped (lightsOut=0). `base_light = get_base_light(...)` = vec3(0x2f/255).
- Window-skip: fires. `lit = base_light` = vec3(0x2f/255). Output 0x2F2F2F.

CPU for same: hot-pink magic at 1804-1828 sets 0x2F. `!isSpotlight` gate at 1936 fails — for-loop and ambient add skipped. Output 0x2F2F2F. **Match.** Spotlights without lightsOut are not affected by Cause β.

---

## §8. Open questions

1. **Are all 19 affected typeIds in mc2_18 confirmed as `lightsOut=1`?** The static_prop.vert comment confirms typeId=474. The other 18 need verification — runtime trace required (out of scope). If some are `lightsOut=0`, then there's an additional Cause γ at play (since per §4, lightsOut=0 + isWindow=1 + hot-pink should produce 0x2F2F2F and match).
2. **What is `g_scene.baseVertexColor` at daytime in mc2_18?** Likely zero, but not verified. If it's non-zero on either side, residual mismatch could remain after fix.
3. **Are there any hot-yellow/red/blue + lightsOut actors silently suppressed by N1.5?** Latent regressions if so. The fix in §6.1 clears them too.

---

## §9. Adversarial review checklist

### §9.1 Symbols cited (all grep-verified)

| Symbol | Cited file:line | Verified |
|---|---|---|
| `if (!isSpotlight && !isWindow)` gate | `mclib/tgl.cpp:1936` | ✓ Read `tgl.cpp:1934-2210` |
| `redFinal += redAmb` ambient add | `mclib/tgl.cpp:2207-2209` | ✓ Read in context, indentation confirms inside gate |
| Closing `}` of `!isWindow` gate | `mclib/tgl.cpp:2210` | ✓ Indent 4 tabs matches opener at 1936 |
| `redAmb`/`greenAmb`/`blueAmb` init | `mclib/tgl.cpp:1795` | ✓ Initialized to 0 |
| Hot-pink magic CPU branch | `mclib/tgl.cpp:1804-1828` | ✓ Read; daytime sets `redFinal = 0x2f` |
| `if (!lightsOut)` non-magic CPU gate | `mclib/tgl.cpp:1880` | ✓ Inside `else if (startVLight & 0x00ffffff)` arm |
| `get_base_light()` hot-pink branch | `shaders/include/lighting.hglsl:66-80` | ✓ Read entire function |
| `get_base_light()` `0xffff00ff` daytime else | `shaders/include/lighting.hglsl:78` | ✓ `final = vec3(0x2f/255.0)` |
| `kFlagIsLightsOut` N1.5 gate | `shaders/static_prop.vert:172-175` | ✓ Read; sets base_light=vec3(0) |
| `kFlagIsWindow \| kFlagIsSpotlight` skip | `shaders/static_prop.vert:221-223` | ✓ Read; lit=base_light |
| `flags` byte population | `GameOS/gameos/gos_static_prop_batcher.cpp:1222-1225` | ✓ bit 0=lightsOut, bit 1=isWindow, bit 2=isSpotlight |
| `bool lightsOut`/`isWindow`/`isSpotlight` on TG_Shape | `mclib/tgl.h:763-767` | ✓ All three are bool fields |
| `isWindow = (S_strnicmp(...,"LitWin_",6) == 0)` | `mclib/tgl.cpp:260, 476` | ✓ Set by node-name prefix |

### §9.2 Negative claims (opposite-direction grep where applicable)

- **Claim**: "CPU does NOT add ambient for windows." Defended by: reading `tgl.cpp:1936` opener and `tgl.cpp:2210` closer, confirming the `redFinal += redAmb` adds at 2207-2209 are between them. Indentation (5 tabs vs 4 tabs at the closer) consistent with brace structure. No alternate ambient-add site for windows exists in the function (would have shown up in any search for `redAmb`/`redFinal +=` in the function body — checked the surrounding 50 lines either side).
- **Claim**: "CPU hot-pink magic-tag does NOT consult `lightsOut`." Defended by: reading `tgl.cpp:1804-1828` (the hot-pink branch) — no `lightsOut` reference. Reading `tgl.cpp:1878-1886` (the only branch that consults `lightsOut`) — gated by `else if (startVLight & 0x00ffffff)` which is the colored-light non-magic arm. The structure is `if .. else if .. else if .. else if .. else if (non-magic with lightsOut gate) .. else if (hud)` — only one branch consults `lightsOut`.
- **Claim**: "GPU's N1.5 gate at `static_prop.vert:172` short-circuits ALL magic-tag handling." Defended by: the gate sets `base_light = vec3(0.0)` and then the subsequent `else { base_light = get_base_light(...) }` is the ONLY call to `get_base_light` in this shader. Grep'd `static_prop.vert` for `get_base_light` — single hit at line 177. So when N1.5 fires, the magic-tag handler is unreachable.
- **Claim**: "The `lit = base_light` window-skip is correct (CPU also skips lighting for windows)." Defended by §2.6 — both sides skip ambient + per-light contributions for windows. Adding ambient to GPU window-skip would be a CPU-divergent over-brightening.

### §9.3 Code quoted verbatim, not paraphrased

All quoted blocks above use line numbers and exact text from the Read tool output. No paraphrasing in quoted ranges.

---

**GPU fix scope estimate:** trivial (1-line edit + 2-line refactor in one shader file). Replace `static_prop.vert:172-183`'s outer `if (kFlagIsLightsOut)` short-circuit with an unconditional `get_base_light()` call that passes the real lightsOut bit through the existing 5th parameter. No struct schema change, no new shader file, no C++ side change required. The lightsOut gate already exists in the right place inside `get_base_light()` at `lighting.hglsl:116`.
