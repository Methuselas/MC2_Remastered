# SPIRV-REFLECTION-CONTRACT-CHECK-1

> CI-only, no relink, no game. A reflection-contract gate over the baked SPIR-V
> sidecars that catches binding / sampler / FBO-output / vertex-input / varying
> **layout drift** across a program's variants before it can reach runtime —
> hardening both shipped pilots (postprocess, MechOpaque). `scripts/check-spirv-
> reflection-contract.py`, registered in `check-contracts.sh` as `spirv_reflection`.

## Why
The keyed consumer loads a baked `.spv` per (program, variant). If a shader edit
silently moves a UBO slot, an FBO output, a vertex attribute, or breaks an
inter-stage varying link, `source_sha256` drift forces a rebake — but nothing
asserted the *reflected interface* stayed contract-correct. This check makes that
explicit and derives the contract from the artifacts themselves (no hand-kept
expected-interface lists), so it stays correct as variants/pilots are added.

## The contract (per base, across baked variants)
Interface elements are classified — each class has the right rule:

| Class | Source | Rule |
|---|---|---|
| Vertex attrs (`vert` inputs) | VAO layout (fixed) | cross-variant **CONSISTENT** location |
| FBO outputs (`frag` outputs) | FBO draw buffers (fixed) | cross-variant **CONSISTENT** location |
| UBO / SSBO bindings | C++ `glBindBufferBase` literal slots | cross-variant **CONSISTENT** + present in `binding-slot-occupancy.json` |
| Inter-stage varyings (`vert` outputs ↔ `frag` inputs) | auto-mapped per linked program | **per-variant** vert-out loc == frag-in loc (link compat); cross-variant drift OK |
| Samplers | resolved by NAME at runtime | location drift OK; name cross-checked vs `sampler-unit-occupancy.json` (WARN) |

Plus, **all classes**: MONOTONIC GATING (up-set) — if an element is present for
define-set V it must be present in every baked variant whose defines ⊇ V. A
`#define` may ADD interface, never REMOVE it. (e.g. `v_objectId@2` appears IFF
`MC2_OBJECT_ID_BUFFER`; `ViewUniformsBlock@3` IFF `MC2_USE_VIEW_UNIFORMS`.)

## Design note (caught during build)
The first cut wrongly applied cross-variant consistency to **inter-stage
varyings** and flagged `v_objectIdRaw` (mech vert→frag) at loc 5 vs 8 across
variants. Investigation showed that's benign auto-map drift — within **each**
variant vert-out == frag-in (objectid: 5==5, both-on: 8==8), and each variant is
its own linked program. Corrected: varyings get the per-variant link check, not
cross-variant consistency. (The wrong check finding a benign case is exactly why
the class model matters.)

## Verified
- Current tree: **PASS** (mech.{vert,frag} ×4 variants, postprocess.{vert,frag}; 0 fail, 2 benign sampler WARNs).
- Planted FBO-output drift (`FragColor` 0→1) → FAIL.
- Planted UBO drift (`ViewUniformsBlock` 3→77, off-manifest) → FAIL.
- Monotonic + varying-link rules exercised by the real mech reflection (v_objectId gated by OBJECT_ID; v_objectIdRaw vert==frag per variant).
- 8 seam checks PASS together.

## Exclusions
CI-only — no relink, no shader/runtime change, no Vulkan, no game run. Foreign
WIP untouched.

## Next (per ranking)
`SHADER-ARTIFACT-PACKAGE-METADATA-1` (explicit package metadata: variant matrix,
source/tool/reflection hashes, runtime-support expectations), then
`SPIRV-STATICPROP-DEPTH-RECON-1`.
