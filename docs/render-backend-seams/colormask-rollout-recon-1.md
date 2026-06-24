# RECON — COLORMASK-ROLLOUT-RECON-1

Read-only. COLORMASK-OWNERSHIP-1 built the gated opt-in mechanism and the byte-gate proved
a single SET-ONLY opt-in (composite) leaks attachment masks into the next frame. This recon
chooses the safe rollout model. NO code, NO behavior change.

## Writers of glColorMask / glColorMaski (classified)
| Site | pattern | leak? | class |
|---|---|---|---|
| gameos_graphics.cpp shadow bracket 3787→3809→3916 | save(GL_COLOR_WRITEMASK)→TRUE→restore | NO | SAFE_SELF_CONTAINED |
| terrain bridge 3970→4022(TRUE M5 repair)→4300 | save→TRUE→restore | NO | SAFE_SELF_CONTAINED |
| terrain mask 4351→4392(FALSE)→4527 | save→FALSE→restore | NO | SAFE_SELF_CONTAINED |
| terrain water-mask 4575→4605(FALSE)→4664 | save→FALSE→restore | NO | SAFE_SELF_CONTAINED |
| mine 4711→4766(TRUE)→4784 | save→TRUE→restore | NO | SAFE_SELF_CONTAINED |
| shadow-bracket restore 6052 | restore | NO | SAFE |
| gos_postprocess HDRI/skybox 2492-2494/2717-2719/2901… + restores 2616/2810 | per-attachment {0=T,1/2=F} save→set→restore (HDRI gated; skybox DEAD) | NO | SAFE_SELF_CONTAINED (skybox dead) |
| **composite opt-in (COLORMASK-OWNERSHIP-1, gate-ON)** | **SET-ONLY (no restore), LAST world pass** | **YES** | **LEAK_SOURCE** (now disabled) |

**Key fact:** ALL legacy colorMask writers already `glGetBooleanv(GL_COLOR_WRITEMASK)` →
set → restore. They are self-contained and DO NOT leak. The ONLY leak class is a set-only
emit with nothing restoring before the next MRT draw — i.e. exactly what an applyPipeline
opt-in introduces.

## Attachment-mask dependencies (why the leak bites)
- color0 = HDR scene color (everything writes it; masking off = black).
- color1 = GBuffer1 — written by `gos_terrain.frag` location=1 (rc_gbuffer1_shadowHandled,
  the post-shadow mask) + terrain_lod_chunk.frag. READ by screenShadow/cloud. Masking color1
  off in frame N (composite) → terrain's GBuffer1 write suppressed in frame N+1 → screenShadow
  breaks. THIS is the observed leak (sha cb5a700e → 8d40ce4a).
- color2 = R32UI objectId — written by some passes, read by ObjectId/Thermal viewmode.
- `beginScene()` (gos_postprocess.cpp:1153) binds sceneFBO + clears (incl. glClearBufferuiv
  attachment 2) but does **NOT** assert a colorMask. So a prior-frame set-only leak persists
  into the new frame's MRT scene draws. **This is the missing keystone.**

## Rollout models
1. **Full per-pass assertion** — every MRT-writing pass asserts its mask via applyPipeline.
   Self-heals (next pass overwrites), but requires routing ALL scene MRT passes first (terrain,
   mech, static) — large, and most aren't routed. Too big as a first step.
2. **Scoped restore** — every set-only mutator restores after. The bridges ALREADY do this;
   only the new applyPipeline opt-ins would need it. But "restore after draw" is awkward in
   applyPipeline (it sets state before the draw; it doesn't know when the draw ends).
3. **Hybrid anchored by a frame-begin assert** — (a) assert a known-good mask
   (all-attachments TRUE) ONCE at beginScene, so every frame starts clean and ANY prior-frame
   leak heals before the first MRT draw; (b) applyPipeline owns colorMask for routed opt-in
   passes; (c) raw helper bridges KEEP their existing save/restore (untouched — already safe).

## RECOMMENDATION — Model 3 (Hybrid), keystone = frame-begin assert
The single cheapest, safest fix is a `glColorMask(TRUE,TRUE,TRUE,TRUE)` (or per-attachment
all-TRUE) at **beginScene** right after binding sceneFBO. Rationale:
- It re-asserts the steady-state ambient (the mask is all-TRUE in normal frames already), so
  gate-OFF/normal behavior is **byte-identical** — it only matters when something leaked.
- It makes incremental opt-in SAFE: a routed pass may set its own mask (even {t,f,f}); the
  leak cannot cross into the next frame's MRT draws because beginScene resets first.
- It does NOT touch the self-contained bridges (no churn, no risk).

## Composite leak disposition
With the beginScene keystone in place, composite CAN safely opt in again (its 1/2=FALSE
mask is reset at the next beginScene before any MRT draw). Re-enable composite opt-in only
AFTER the keystone lands + re-prove byte-identical. (Alternative: leave composite un-opted;
the keystone alone unblocks the higher-value terrain routing.)

## Terrain LOD-chunk prerequisite (stated clearly)
TerrainSolidLODChunk (terrain_lod_chunk.cpp:887) writes color0 + GBuffer1 (color1). Routing it
with colorMask ownership ({t,t,f}) is SAFE ONLY AFTER the beginScene keystone exists —
otherwise lod-chunk's mask emit could leak into a later same-frame consumer or the next frame.
Do NOT route lod-chunk colorMask until COLORMASK-ROLLOUT-1 (keystone) lands and is byte-proven.

## NEXT
COLORMASK-ROLLOUT-1 (GO-with-conditions): add the beginScene all-TRUE colorMask assert
(gated MC2_PIPELINE_COLORMASK or unconditional — it is byte-identical in normal frames; prove
it). Then optionally re-enable composite opt-in. Acceptance: gate-OFF byte-identical, gate-ON
byte-identical (no composite next-frame MRT leak), black-frame checker PASS, frame trace
confirms masks. Bridges untouched. THEN TERRAIN-LODCHUNK routing recon.
