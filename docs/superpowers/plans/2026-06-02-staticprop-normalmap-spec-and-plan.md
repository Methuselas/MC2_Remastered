# Static-Prop Normal Mapping (runtime) — Spec + Plan

**Status:** design only (spec + plan). **Execution deferred** — this is a *visual* feature
whose correctness (tangent handedness, TBN, normal perturbation) can only be verified by
rendered output + cooked normal `.ktx2` fixtures (needs the `ktx` CLI, currently absent).
Do not blind-execute on compile-green; the ORM review proved how easily GPU details go
subtly wrong.

**Builds on:** `docs/staticprop-material-orm-normal-recon.md` (recon slices 5–6) and the
landed ORM arc (per-bucket linear texture-array machinery + sidecar feed + gate now exist).

---

## Spec

### Problem
Static-prop diffuse is computed **per-vertex** (`calc_light(dot(worldNormal,L))`, Gouraud).
There is no tangent attribute and no normal-map sampling. A normal map fed only into the
existing per-fragment *specular* would perturb highlights while diffuse stayed flat — the
**split-granularity** inconsistency the recon flagged. Doing normal mapping *right* requires
(a) a tangent basis and (b) moving the direct diffuse to the fragment stage.

### What this delivers
Behind the existing `MC2_STATICPROP_MATERIAL_PBR_SLOTS` gate (extended), authored **normal**
maps that perturb a **per-fragment** lighting normal used by BOTH diffuse and specular:
1. **Tangents**, generated at load in the batcher's `registerType` expand loop from
   position+UV (no `.tgl`/`TG_TypeVertex` format change — see recon §5), packed as a `vec4`
   (xyz + handedness `w`) at vertex **location 5**; zero-filled for props without a normal map.
2. **Normal texture array** — a per-bucket linear sibling (reuse the ORM array machinery
   landed this session: `s_ormBucketArrays`/`bptcInternalFormatFor(false)`/`deriveOrmSidecar`,
   generalized to a `.n.ktx2` sidecar) → populates `MaterialGpu.normalTex` + `kNormalMap`.
3. **Per-fragment lighting normal**: sample the tangent-space normal, build TBN, transform to
   world, and use it for the diffuse + specular terms — which requires **relocating the
   direct `calc_light` diffuse from the vertex shader to the fragment shader** so it shades
   on the mapped normal (the load-bearing change; isolated under a compile-define).

### Acceptance criteria
- **AC1 — Gate OFF byte-identical** (`MC2_STATICPROP_MATERIAL_PBR_SLOTS` unset): vertex
  diffuse path unchanged; new attribute/varying/define compiled out; capture two-PNG diff
  (`staticprop_baseline_02`) shows `bbox None`.
- **AC2 — Gate ON, no normal map**: a prop with `normalTex == kMaterialTexAbsent` shades
  identically to gate-OFF. **CRITICAL & HARD:** moving diffuse per-fragment must reproduce
  the vertex/Gouraud result within tolerance when the geometric normal is used (no map). This
  is the byte-identity risk the ORM review caught for AO — verify with **lights enabled**, not
  just defaults.
- **AC3 — Gate ON, with normal map**: tangent validation — flat-blue normal `(0,0,1)` ==
  no-normal; a known normal perturbs diffuse+specular as expected; UV-seam does not explode;
  mirrored-UV handedness correct (`.w`). Requires cooked normal fixtures (ktx).
- **AC4 — linear normal array** (UNORM, never sRGB). **AC5 — no MaterialGpu ABI churn**
  (`normalTex`/`kNormalMap` already exist). **AC6 — vertex format additive** (loc 5; stride
  +16 for vec4; no `.tgl` bump — generate in `registerType`). **AC7 — smoke** (mc2_10 gate-ON
  no crash/GL-error; shader compiles at runtime). **AC8 — perf**: per-fragment diffuse over ≤16
  lights is a hot-path change — Tracy GPU zone on the static-prop pass must not regress beyond
  an agreed budget before any default-ON.

### Non-goals
Emissive; AO (separate ORM follow-on); flipping the gate default-ON; mech/terrain; any
`MaterialGpu`/manifest schema change; tangent storage in `TG_TypeVertex` / on-disk `.tgl`.

### Hard constraints
- **No `.tgl` binary-cache bump**: generate tangents in `registerType` from the already-loaded
  `TG_TypeVertex` (recon §5; `tgl.cpp:525` `LoadBinaryCopy` `sizeof` hazard).
- `uniform uint` crashes shader_builder — any new uint uniform declared `int`.
- Reuse the landed ORM per-bucket array machinery for the normal array (don't reinvent).
- The per-fragment diffuse relocation MUST be byte-identical to the vertex path when using the
  geometric normal (AC2) — this is the make-or-break correctness gate.

---

## Plan (tasks; execution-deferred)

**N1 — Tangent attribute + generation (no normal sampling yet).**
- `gos_static_prop_batcher.cpp`: extend the per-vertex stride (+16 for `vec4 a_tangent`),
  generate per-triangle tangents in the `registerType` expand loop (`~:1746-1785`) from
  position+UV derivatives (Lengyel/MikkTSpace-style), store xyz + handedness `.w`; zero-fill
  when absent. Add `glVertexAttribPointer(5, 4, GL_FLOAT, ...)` after `:1900`.
- `static_prop.vert`: add `layout(location=5) in vec4 a_tangent;` forward `v_tangent`/`v_bitangentSign`
  as varyings (gated by the compile-define; zero-filled otherwise). **No lighting change yet.**
- Verify: gate-OFF byte-identity (capture diff); gate-ON smoke (no crash). Compile-green.

**N2 — Normal texture array + feed (reuse ORM machinery).**
- Generalize the landed `deriveOrmSidecar`/per-bucket build to also build `s_normalBucketArrays`
  from `.n.ktx2` sidecars (linear), layer-aligned (`normalTex == albedoTex`), neutral
  flat-normal `(128,128,255,255)` texel for absent layers; set `kNormalMap` when present.
- Bind at all albedo bind-sites (unit 2); loc-guarded uniforms `u_normalTexArr`/`u_normalSampleEnable`.
- Verify: gate-ON smoke; gate-OFF identity. Compile-green.

**N3 — Per-fragment diffuse relocation (the crux).**
- Move the `calc_light` direct-diffuse evaluation from `static_prop.vert` to `static_prop.frag`,
  under `#ifdef MC2_STATICPROP_PBR_SLOTS`, forwarding the needed inputs (light indices, world
  pos, base light) as varyings. With the **geometric** normal this must equal the current
  Gouraud result within tolerance (AC2). Keep the vertex path intact in the `#else`.
- Verify: AC2 with **lights enabled** (capture diff vs baseline, lit scene) — the gate.

**N4 — Normal map perturbs the per-fragment normal.**
- Sample `u_normalTexArr` at `normalTex`; decode tangent-space normal; build TBN from
  `v_tangent`/handedness/`v_normal`; transform to world; feed the N3 diffuse + the existing
  specular `N`. Window/`kFlagIsWindow` nodes bypass. Add debug views (sampled-normal, TBN).
- Verify: AC3 tangent validation (needs cooked normal fixtures); smoke.

**N5 — Perf + soak.**
- Tracy GPU zone on the static-prop pass; confirm per-fragment diffuse cost within budget;
  soak mc2_10; tuning knob. Default-ON decision separate.

---

## Risks / stop conditions (carried from recon + ORM review)
- **N3 byte-identity is the highest risk** (same class as the ORM-AO blocker). If per-fragment
  diffuse can't reproduce Gouraud within tolerance, normal mapping is misleading — stop and
  reconsider (viewer-only PBR, or accept specular-only normal with documented limitation).
- Tangent handedness on mirrored UVs — `vec4 .w` required (octahedral like mech loses it).
- Perf: 16-light per-fragment loop. Stop if Tracy shows unacceptable regression.
- **Execution requires interactive GPU/visual verification + `ktx` for normal fixtures** — not
  autonomously completable. This is the explicit handoff gate.

## Recommendation
Sequence N1→N2→N5. **N3 is the gate**: prototype it first behind the define and prove AC2 with
lights on before building N4. If AC2 can't be met, descope to specular-only normal (documented)
or keep normal authoring in the viewer Local-PBR ball. Execute only in an interactive session
with deploy + `ktx` + visual review.

---

## Adversarial review (2-critic panel) + revisions

### BLOCKER — AC2 is mathematically unachievable; re-scope or descope
Gouraud **interpolates the final lit color**; per-fragment **recomputes** it. `calc_light` is
nonlinear (clamps, `length`, falloff ramps, the `clamp(...,0,1)` at `vert:419`) and `v_normal`
is **un-normalized** — so interpolating the output ≠ evaluating at the interpolated input,
except on small/unsaturated triangles. **Moving diffuse per-fragment intentionally changes the
shading of EVERY prop** with a triangle spanning a lighting gradient/clamp edge (esp. the stock
`INFINITEWITHFALLOFF` sun terminator), not just normal-mapped ones. AC2 "no-map prop shades
identically to gate-OFF" is false.
- **Revision:** AC2 becomes a **bounded perceptual** criterion (SSIM ≥ T / capped ΔE on a lit
  `staticprop_baseline_02` scene), explicitly stating gate-ON is a per-vertex→per-pixel shading
  change for all props. Byte-identity stays only for **AC1 (gate-OFF)**.
- **Strongly recommended descope:** ship **specular-only normal mapping** first — the specular
  block already runs per-fragment on `v_normal` (`frag:350-415`); perturbing `N` at ~`frag:394`
  is a *small, low-risk* change with NO diffuse relocation. Deliver that as N-arc v1; treat the
  per-fragment-diffuse relocation (N3) as a separate, larger, perf-gated arc. This is the
  pragmatic correct scope, not a consolation prize.

### BLOCKER — separate the compile-define (don't couple ORM and normal)
The landed ORM uses the single `MC2_STATICPROP_MATERIAL_PBR_SLOTS` define. Reusing it for
normal-mapping means turning ORM on drags in the in-flight diffuse relocation.
- **Revision:** introduce a **second** define `MC2_STATICPROP_NORMALMAP` (own env gate) so N1/N3/N4
  compile out independently of the shipped ORM path.

### N1 tangent-generation fixes (must be explicit, not left to implementer)
- **Stride/offset correction:** current stride is **40**; offset 36 is `a_aRGBLight` (loc 4), NOT
  a free pad (the recon §1/§5 table is stale). Tangent `vec4` goes at **offset 40, new stride 56**;
  fix the stale `_pad` comment at `gos_static_prop_batcher.cpp:~255`; add
  `glVertexAttribPointer(5,4,GL_FLOAT,...,56,(void*)40)`. Loc 5 confirmed free; VBO is a single
  immutable `glBufferStorage` VAO with no other attrib site → stride bump is contained (grep for
  raw `40`/`36`/`kVertexStride` literals first).
- **Degenerate-UV NaN guard (REQUIRED):** Lengyel denom `s1*t2−s2*t1≈0` (common on stock props)
  → NaN. On degenerate (or post-Gram-Schmidt `length(T)<eps`), emit a deterministic basis
  perpendicular to N (`T=normalize(cross(N, |N.x|<0.9?xAxis:yAxis))`, `.w=1`). Neutral flat map
  then yields no perturbation → no-map identity preserved.
- **Bitangent / handedness:** compute `tdir` AND `bdir` from the UV solve; store
  `T=normalize(tdir−N·dot(N,tdir))`, `w = dot(cross(N,tdir),bdir)<0 ? −1 : 1`; shader rebuilds
  `B=cross(N,T)*v_tangent.w`. (Do NOT compute `.w` against `cross(N,T)` — always +1, breaks
  mirrored UVs.)
- **Faceted/smooth-normal mismatch (AC3 risk):** stored `src.normal` is the author's *smooth*
  normal; per-face tangent + Gram-Schmidt against it bends the frame on curved smooth surfaces,
  so **flat-blue == no-normal only holds to interpolation tolerance**. AC3 must test a *curved
  smooth-shaded* prop and state the tolerance (exact identity would need a position+normal weld
  pass the triangle-soup layout otherwise destroys).

### N2 normal-array fixes
- The ORM array builder is a **hardcoded lambda** (`deriveOrmSidecar`, neutral `255,255,255,255`,
  `[STATICPROP_ORM]` tag) — **not parameterized**. N2 is real refactor: lift it to a function
  taking `(sidecarDeriver, neutralTexel[4], logTag, requireLinearVkFormat)` + parallel
  `s_normalBucketArrays`/`s_normalLayerHasMap`. Normal neutral = **`(128,128,255,255)`** → `(0,0,1)`.
  `bptcInternalFormatFor(false)` (linear) is correct; **require vkFormat 145 (reject 146)** for
  normals so a mis-cooked sRGB normal can't silently load.
- Inherit ORM parity explicitly: add to `staticPropReleaseBuckets()` teardown; bind at all 3
  albedo bind-sites (new `kNormalTexUnit=2`); add the epilogue unbind. (Omissions = leak / stale
  unit / AC1 break.)

### N3 feasibility gaps (if the per-fragment-diffuse arc is pursued)
- `static_prop.frag` does **not** include `lighting.hglsl` today; doing so re-declares the
  `LightsData` SSBO at **binding 20** in the frag stage → changes the frag SSBO binding mask →
  regen reflect goldens + `tests/unit/test_rendercore.cpp` `ssboBindingsMask` asserts.
- `v_worldPos` exists only under `MC2_USE_VIEW_UNIFORMS` → define interlock or fallback needed.
- Diffuse must use **Stuff-space** `v_normal` (lights are Stuff-space), NOT the GL-converted
  specular `N` (`frag:394`) — mixing silently corrupts diffuse.
- Per-fragment must **reconstruct the full `litRgb`** (interpolated base/ambient/IBL/highlight +
  per-frag diffuse), carry the `kFlagIsWindow` bypass and the alpha-test `max(litRgb,0.5)` floor,
  and the **vertex shader must stop folding diffuse into `v_argb` under the define** (else
  double-count) — same trap class the ORM-AO review caught.
- **AC8 needs a concrete number** (static-prop Tracy GPU-zone Δ% cap at the deterministic camera);
  the 16-light per-fragment loop is the real cost (specular shipped cheap only because it uses a
  single flat sun varying, not a light loop).

### Found issue in the LANDED ORM code (verify + fix separately)
**S5-B:** the ORM texture bind sits inside the BC7-bucketed draw path; the non-BC7 group-array
fallback path binds only albedo, yet `u_ormSampleEnable` is uploaded `=1` unconditionally when
the gate is on. *Likely* safe in practice because the ORM **feed** (`metallicRoughnessTex` +
`kMetallicRoughness`) is also only populated in the BC7 path, so with BC7 off every material is
`kMaterialTexAbsent` and the shader's `ormTex != kMatTexAbsent` guard skips sampling — **but this
coupling is implicit and brittle.** TODO on the ORM arc: force `u_ormSampleEnable=0` whenever
`!s_staticPropBc7Enabled`, and confirm `s_ormLayerHasMap` is empty in that mode. Filed here so it
isn't cloned into the normal path.

### Net
The deferral is correct. Before any execution: (1) descope to specular-only normal OR re-scope
AC2 perceptual; (2) add the second define; (3) make N1's degenerate/handedness/stride fixes
explicit; (4) parameterize the array builder for N2; (5) only then consider the per-fragment
diffuse arc with a real perf budget. Execution still requires interactive GPU/visual verification
+ `ktx` for normal fixtures.
