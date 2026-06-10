# gosFX Modder Drop-In: New Effect → Tie to Weapon

**Date:** 2026-06-10. Worktree: nifty-mendeleev. Companion: [gosfx-animation-lifecycle-recon.md](gosfx-animation-lifecycle-recon.md).

## Two independent problems
1. **BIND** an effect to a weapon — pure data edit, **works today**.
2. **AUTHOR** a brand-new effect spec — byte layer exists, **no modder-facing tool exists**.

---

## PART 1 — BINDING (works today, CSV only)
Chain a weapon traverses to find its effect:

```
weapon fires (mech.cpp fireWeapon)
  → MasterComponent[masterID].getWeaponSpecialEffect()        # the effectId
      source: mc2srcdata/objects/compbas.csv, col "Special FX ID"
  → weaponEffects table (mclib/weaponfx.cpp init)
      source: mc2srcdata/objects/effects.csv, row = effectId
      cols: effectName | muzzleFlash | hitEffectName | objNum | missEffectName
  → EffectLibrary::Find(name) → MakeEffect(spec->m_effectID)  # name→spec, string match
      source: data/effects/mc2.fx (the gosFX spec blob)
  → (secondary) bolt .fit HitEffect=/MissEffect= ints → createExplosion
      source: mc2srcdata/objects/<weapon>.fit
```

### MODDER-EDIT-POINTS (point existing weapon at a different effect)
| Step | File | Field |
|---|---|---|
| 1 | `mc2srcdata/objects/compbas.csv` | `Special FX ID` col on the weapon's row → new effectId |
| 2 | `mc2srcdata/objects/effects.csv` | row[effectId]: set effectName / hitEffectName / missEffectName strings |
| 3 | `data/effects/mc2.fx` | those names must resolve to a loaded spec (string match, case-insensitive) |
| 4 (opt) | `mc2srcdata/objects/<weapon>.fit` | `HitEffect=` / `MissEffect=` int for ground-impact explosion |

If reusing an **existing** spec name → steps 1-2 only, done. No recompile.

---

## PART 2 — ADDING A NEW EFFECT SPEC

### Mod-overlay file system (works today)
`MC2_ACTIVE_MOD=<modId>` → `File::open()` resolves mod first (`mclib/file.cpp:412 TryModOpen`, search order: mod-index O(1) → loose → FastFile → CD). Effects load loose: `code/mechcmd2.cpp:1673 effectFile.open("data/effects/mc2.fx")` — NOT in a FastFile. So:
- Drop `mods/<modId>/data/effects/mc2.fx` → game uses modder's whole blob.
- `mods/<modId>/mod.json` declares id/deps; priority base < deps < active (`file.cpp:349-408,523`).
- Diagnostic: `MC2_LOG_FILE_RESOLVE=1`.

**BUT the overlay is whole-blob replace.** No per-effect append: `SpecLibrary::m_effects` is `DynamicArrayOf`, frozen after `Load`, `m_effectID` stamped by index (`spec_library.cpp:66`), `MakeEffect` uses `At(index)`. No public Register/Adopt. Runtime-append = engine patch, not modding.

### Serialization-out: byte layer COMPLETE but DORMANT
Full symmetric `Save()` exists, mirrors `Load`, per subclass + per curve:
- Specs: `Effect::Specification::Save` effect.cpp:323; Singleton 54, Card 110, Shape 86, ParticleCloud 71, SpinningCloud 65, CardCloud 113, ShapeCloud 95, ShardCloud 69, PertCloud 75, DebrisCloud 96, EffectCloud 60, PointLight 83, Tube 104. (all in respective .cpp)
- Curves: `Curve::Save` fcurve.cpp:32; Constant 1260, Linear 1289, Spline 1385, Complex 1664, `SeededCurveOf::Save` fcurve.hpp:463.
- Blob writer: `EffectLibrary::Save` effectlibrary.cpp:81 → `SpecLibrary::Save` spec_library.cpp:70 (WriteGFXVersion + count + loop specs). `WriteGFXVersion` gosfx.cpp:141.

**`EffectLibrary::Save` has ZERO callers** in code/editor/Viewer. Ported from the original MSDev gosFX editor; never wired. No blank-spec constructor (specs only built via `Specification::Create(stream)` factory during Load). No text/JSON intermediate — mc2.fx is opaque binary.

### Workbench: does NOT touch effects
asset_viewer / mod-workbench (separate worktree) = mesh/material/texture only. `grep gosfx|EffectLibrary|particle|mc2.fx|fcurve` = 0 hits. No python emits mc2.fx.

---

## TOOL STATUS — `tools/mc2fx/` (slice 1 SHIPPED 2026-06-10)
Standalone game-free CLI built. `dump` works: reads loose `mc2.fx` (CRT) → live `SpecLibrary::Load` headless → JSON catalog. **904 effects** parsed from deployed v0.4 blob (PPC_Trail, LRM_Smoke, Gauss_trail, smoke...), exit 0, no GL/device.
- Build: `cmake -DENABLE_MC2FX=ON` then `--build --target mc2fx`. Exe: `build64/out/tools/mc2fx/RelWithDebInfo/mc2fx.exe`.
- Curated link (asset_viewer pattern): globs gosfx+mlr+particles+stuff, slim `mc2fx_stubs.cpp` (gos_* heap/file/draw no-ops, CRT-backed). Zero engine-source edits.
- Headless init order (load-bearing): `Stuff::InitializeClasses()` → `MidLevelRenderer::InitializeClasses(...)` → construct `MLRTexturePool::Instance(new TGAFilePool(...))` → `gosFX::InitializeClasses()` → push gosFX::Heap → Load. (texture pool needed: MLRState::Load resolves texture *names*; mcTextureManager stubbed no-op.)
- Coherent heap stack in stubs so `Verify(gos_GetCurrentHeap()==gosFX::Heap)` holds.
- **NEXT (slice 2):** per-curve depth in JSON (walk Specification subclass members per classID) + `build` subcommand (`EffectLibrary::Save` round-trip → re-emit mc2.fx). classID seen 1312/1314/1318/1322/1324 → need classID→typename map (gosfx.hpp ClassID enum).

## THE GAP (what to build for a real new-effect modder path)
Byte serialization is done. Missing, in order of effort:
1. **A driver that calls `EffectLibrary::Save`** — small CLI: `Load(mc2.fx)` → mutate → `Save(out.fx)`. Edits to existing specs land today with just this.
2. **Blank-spec construction** — "create empty Specification of type X + defaults" surface (nothing builds a spec outside stream-Load). Needed to author a NET-NEW effect vs editing one.
3. **Editable representation** — JSON/text ⇄ Specification importer/exporter, so modders aren't C++ field-poking opaque binary.
4. **Workbench wiring** — effect loader/preview/export panels analogous to its mesh/material ones (override-manifest + bundle-export model).

## Recommended modder pipeline (proposed)
```
mc2fx CLI tool (NEW, ~item 1+2+3):
  mc2fx dump  mc2.fx           → effects.json   (human-editable curves/fields)
  mc2fx build effects.json     → mc2.fx         (calls EffectLibrary::Save)
Modder:
  edit effects.json (add "plasma_trail" spec)  → build → mods/<id>/data/effects/mc2.fx
  edit compbas.csv + effects.csv to bind weapon → effectId → "plasma_trail"
  set MC2_ACTIVE_MOD=<id>, launch
```
Reuses the dormant Save() API — low engine risk. JSON schema mirrors the Specification + Curve members enumerated in the lifecycle recon. No GPU work required (CPU sim authority → oracle → GPU billboards, automatic).

## Caveats
- New spec on a **mesh class** (ShapeCloud/Shape/DebrisCloud) or **Tube** renders CPU-MLR, bypasses GPU oracle.
- Whole-blob overlay means a mod's mc2.fx must contain ALL effects it relies on (or dep-chain a base mod) — can't add one effect to the stock blob at runtime.
- effects.csv row index == effectId; inserting rows renumbers downstream — append, don't insert.
