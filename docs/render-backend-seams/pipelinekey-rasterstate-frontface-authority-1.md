# PIPELINEKEY-RASTERSTATE-FRONTFACE-AUTHORITY-1

**Status:** SHIPPED. Promotes the PipelineKey `rasterState` axis **MISSING → PARTIAL**
by making **frontFace** an explicit, repo-owned, checked per-pipeline row (with
the already-present **cullMode** validated beside it). Scoped tightly per
PIPELINEKEY-RASTERSTATE-AUTHORITY-RECON-1.

## What changed

Registered pipelines previously relied on the **ambient process-wide GL default
`GL_CCW`** for winding — winding was a leaked global, never authored (only the
shadow pass save/restored it). Now each registered pipeline's row states its
winding, and `applyPipeline` sets it.

- **`RenderCore/PipelineDesc.h`** — new `enum class FrontFace : uint8_t { Ccw, Cw }`
  and a `FrontFace frontFace;` field. The field lands in the existing 3-byte
  padding window before the 4-byte-aligned `ssboBindingsMask`, so
  `static_assert(sizeof(PipelineDesc) <= 20)` **still holds — zero struct growth**.
- **`RenderCore/PipelineRegistry.cpp`** — all 4 rows set `frontFace = FrontFace::Ccw`
  (current behavior, now explicit). Row 0 (Invalid) is value-initialized (`Ccw`).
- **`GameOS/gameos/pipeline_binder.cpp`** — `applyPipeline` now ends with
  `glFrontFace(desc.frontFace == Cw ? GL_CW : GL_CCW)`. Since every registered
  row is `Ccw` and the global was already `GL_CCW`, this is a **behavioral no-op
  today** — it ends the registered set's dependence on ambient global winding.
- **`pipeline-key-schema.json`** — `rasterState` `MISSING → PARTIAL`
  (`authoritative_subaxes`: frontFace, cullMode; `gaps`: polygonOffset/depthBias,
  polygonMode). Each `registered_pipelines[]` gains `"frontFace": "Ccw"`.
- **`scripts/check-pipeline-key.py`** — `rasterState` must be PARTIAL/AUTHORITATIVE;
  `FrontFace` enum must exist in `PipelineDesc.h`; every registered pipeline must
  declare a `frontFace` that exists in the enum (missing → FAIL, stale/renamed →
  FAIL); `cullState.mode` must be a valid `CullMode` value (zero-risk sibling).

## Verification

- Build (`--target mc2 mc2_launcher`, RelWithDebInfo, isolated worktree) → green;
  `sizeof(PipelineDesc) <= 20` static_assert holds.
- `check-pipeline-key.py` → PASS; `rasterState` shows PARTIAL.
- Adversarial (each forces exit 1, then reverted): missing `frontFace`,
  renamed `frontFace` value, `rasterState` regressed to MISSING.
- Smoke `mc2_24` PASS; MechOpaque + shadow visual sanity PASS; no new GL errors;
  no stock mech winding inversion; no invisible static props. *(see slice log)*
- `check-contracts.sh` → `pipeline_key` PASS (the 4 fails are pre-existing
  foreign-WIP: env_registry, include_firewall, no_raw_gl_from_game,
  render_contract_gbuf1). Foreign WIP md5 unchanged.

## Explicit exclusions (NOT done here)

- **No polygon offset factor/units in PipelineDesc** — 8 B blows the 20 B budget;
  the real consumers are unregistered shadow passes; `shadowBias*` is ImGui-runtime-
  mutable. Deferred to SHADOW-CASTER-PIPELINE-REGISTRATION.
- **No shadow-caster pipeline registration.**
- **No depth-bias modeling** — it is a single-sourced *shader* axis
  (`terrain_depth_bias.hglsl` + C++ mirror), not GL raster state.
- **No legacy face-flip rewrite** — the legacy fixed-function path encodes winding
  by flipping the *culled face* (`gameos_graphics.cpp:5445-5450`), NOT via
  `glFrontFace`; it is untouched and independent of `desc.frontFace`.
- **No polygonMode** (only a future DebugWireframe pipeline needs it).
- **No render-state abstraction / RasterStateId intern-table** (explicit fields
  only; revisit when raster diversity is real).
- **No Vulkan backend.**

## Next

SHADOW-CASTER-PIPELINE-REGISTRATION-RECON-1 (recon only) — decide how shadow
caster passes become registered before polygonOffset is modeled.
