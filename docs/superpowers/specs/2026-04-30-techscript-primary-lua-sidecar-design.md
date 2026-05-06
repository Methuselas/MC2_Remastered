# Architectural Shift: TechScript Primary, Lua Sidecar

**Date:** 2026-04-30
**Status:** ⭐ **Active direction.** Supersedes the Lua-primary framing in the 23-doc Track C corpus.
**Origin:** [Methuselas, GitHub Discussion #18](https://github.com/ThranduilsRing/mc2-opengl-remastered/discussions/18) — TechScript Architecture Proposal
**Owner:** Methuselas-led on TechScript design and implementation; rjm/this session on Lua sidecar gating + integration.

---

## What changed

The 23-doc Track C design corpus assumed **Lua + Sol2 as the primary modding scripting layer**. Discussion #18 proposes — and this session adopts — a different primary: **TechScript**, a curated, deterministic, FIT-data-backed verb catalog inspired by Construction Set / Creation Kit authoring.

This document is the single course-correction artifact. The 23 Track C docs remain internally consistent **but are now reference for a deferred sidecar capability**, not the active scripting architecture. Anyone reading those docs first should read THIS doc to know which framing applies.

---

## Why TechScript

Methuselas's proposal makes three load-bearing arguments. They are persuasive in this codebase specifically:

1. **Format coherence with his other tracks.** FIT is already the modder-stable format for: Editor resources (rule in force at editor `0.3.0-editor`), in-game UI (the ImGui+FIT design), and now scripting. Three modding surfaces, one definition format. Modders learn FIT once. Lua would have been a fourth language.

2. **BattleTech-modder targeting wider than RogueTech.** ModTek/RogueTech-style modders use C# DLLs at the script layer — technical, programmer-only audience. The Construction Set / GECK / Creation Kit pattern (curated visual command list) targets a *significantly wider* community: mission authors, narrative designers, balance tuners. Methuselas is choosing the wider audience deliberately.

3. **Determinism + savegame stability + inspectability.** TechScript saves as instruction pointer + scoped variables in FIT. Mid-mission saves work cleanly because the runtime state is data, not call-stack. Lua's `mc2.persist` design we wrote handles the same need but is less natural — Lua state is by nature opaque; TechScript state is by nature inspectable.

Methuselas's proposal explicitly rejects "general-purpose Lua replacement" and "recreating all 288 ABL functions as 288 unique opcodes." It collapses ABL's sprawl into curated domains (Unit, Weapon, Formation, Air, Logistics, etc.).

---

## The shift, concretely

### Primary scripting layer

**Was:** Lua via Sol2 with `*_impl` C-linkage extraction (per Track C blocking-questions §Q1).
**Is:** TechScript Specials in typed-block FIT data, evaluated by a deterministic engine-side runtime.

Three architectural levels, per Methuselas: **"Brains decide WHEN. OPORDs define INTENT. TechScript executes WHAT."**

### Modder authoring UX

**Was:** VS Code + LuaLS + auto-generated type stubs + `tools/new-mod` scaffolder.
**Is:** In-engine Mission Editor with three equivalent views — visual command list, BASIC-like text, raw FIT block. Editor is the authoritative authoring surface.

### File format

**Was:** `mods/<id>/scripts/{data,control}.lua` + JSON/YAML manifests.
**Is:** Typed-block FIT data, e.g.

```
TechSpecial {
    key  = "mission.a_01.special.spring_ambush"
    type = "MissionSpecial"
    Body {
        DO UI.Print "Enemy contacts!"
        DO Spawn.Formation "mission.a_01.formation.hidden_lance"
        STOP
    }
}
```

### ABL relationship

**Was:** Lua trampolines call `*_impl` extracted from `code/ablmc2.cpp`; ABL's `execXxx` keeps existing modders running unchanged.
**Is:** ABL becomes a frozen legacy path with a standalone converter to TechScript. New mods author in TechScript only.

### Inheritance pattern

Methuselas's proposal: **core Specials cannot be deleted; modders create variants.** Same shape as OpenRA's `Inherits:` pattern but applied at the script level instead of data level.

---

## Lua's new role: deferred sidecar, "add as needed"

Per user direction (2026-04-30 chat): defer Lua, add support as needed. Lua remains a *possible* future capability, not active development. The shape of "as needed" is intentionally not pinned in this doc — when Methuselas's TechScript verb catalog hits a real expressivity wall in a real mod, we revisit Lua as one of:

- **(a) Escape hatch** — TechScript verb `DO Lua.Eval "scripts/<file>.lua"` gated behind explicit `mod.json` opt-in. The 23-doc Lua sandbox + lifecycle work becomes the implementation of one verb.
- **(b) Power-user opt-in** — mod's manifest declares `"scripting": "lua"` instead of TechScript. Mod commits to one runtime per mod; engine maintains both VMs.
- **(c) Build-time / offline tooling only** — no Lua VM in shipping engine; Lua used for `tools/lua_api_doc_gen/`, `tools/new-mod`, schema generators.

For now: **none of these are active**. The Track C work is preserved as design reference; implementation does not begin until a concrete need surfaces.

---

## What stays load-bearing from the existing corpus

The 23-doc Track C work isn't wasted. Most of it abstracts cleanly above the scripting-language choice:

| Component | Status under TechScript-primary |
|---|---|
| Modders paradise roadmap (sidecar principle, reference stack, rejection list) | ✅ All still apply; FIT replaces Lua as the modder-stable contract |
| Modder-tooling DX (REPL, profiler hooks, debug overlays) | ✅ Pattern still applies; "REPL" becomes TechScript console; profiler hooks become per-Special timing |
| Mod boundaries (NO list, sandbox, resource budgets) | ✅ TechScript's deterministic-by-construction nature is *stronger* than Lua sandboxing on most boundary categories |
| Mod test harness (`--test-mod`, snapshot, CI) | ✅ Test-mod CLI shape applies; snapshot format applies; mod-smoke tier applies |
| Cross-track perf budget audit | ⚠️ Numbers shift — TechScript per-verb cost vs Lua per-call cost differs; structure of the analysis stands |
| Stock-mission compatibility plan (60-cell matrix) | ✅ All applies; bundled `mods/_compat_test/` ships TechScript instead of Lua |
| Modifier registry decision (hybrid v1) | ✅ **Possibly more relevant** — TechScript's `Values` category and `Conditions` category compose naturally with modifier-registry queries |
| BattleTech mod-scene research | ✅ All applies; the "JSON merge operators are table stakes" finding now applies to TechScript override patterns instead |
| Build integration (Lua + Sol2 vendoring) | ⏸ Deferred until Lua sidecar lands |
| Implementation shape, blocking-questions, trampolines | ⏸ Deferred — `*_impl` extraction does not happen for Lua's sake; if some `*_impl` extractions help TechScript, that's a TechScript decision |
| API surface catalog (~85 STABLE Lua bindings) | ⏸ Reframe — TechScript verb catalog (Methuselas's 24 POC Specials + the 288-function ABL→TechScript mapping) is the modder-facing surface; the Lua bindings list becomes input to the TechScript verb catalog |
| Lua sandbox + errors, loading + lifecycle | ⏸ Deferred until escape-hatch (option a) or power-user (option b) is chosen |
| Track F (AI replacement) — 4-layer hierarchy + pilot/chassis modifier consumers | ⚠️ **Reframed by Methuselas's three-tier model** (see below) |

---

## Track F integration with TechScript's three-tier model

Methuselas's "Brains decide WHEN. OPORDs define INTENT. TechScript executes WHAT." maps onto our four-layer Track F hierarchy:

| Track F layer (Lua-primary framing) | TechScript-primary framing |
|---|---|
| L1 Strategic (per-team, ~1 Hz) | **Brains** — WHEN to fight, what objectives matter |
| L2 Tactical (per-lance, ~3 Hz) | **OPORDs** — INTENT-based, lance-level orders |
| L3 Operational (per-warrior, behavior-tree, ~10 Hz) | **TechScript Specials** — WHAT to execute |
| L4 Unit (per-warrior, alarm-driven) | TechScript at unit-tick frequency, plus event alarms |

**The pilot personality × chassis affinity → modifier-registry composition still holds.** Modifiers feed TechScript-evaluated Conditions and Values, not Lua-evaluated boolean predicates. The unification (one registry, six consumers) survives the shift.

The 0.44 Hz native brain tick rate finding from Track F-1 still applies — TechScript Specials in Brain scope tick at the same engine cadence.

---

## What's open / needs Methuselas's input

These items are not blocking; they're the natural questions to put to Methuselas as TechScript implementation begins:

1. **Lua escape-hatch policy.** Will TechScript admit a `DO Lua.Eval` verb behind opt-in? If yes, the Track C sandbox+lifecycle work activates as that verb's implementation. If no, Lua is build-time / offline tooling only.
2. **Modifier registry consumption shape.** Does TechScript's `Values` category dispatch to the modifier registry directly, or does the registry expose stat-reads via specific verbs (`DO Mech.GetEffectiveHeatDissipation`)?
3. **Brain / OPORD container.** Are Brains and OPORDs themselves TechScript Specials at different scopes, or a different mechanism entirely?
4. **ABL deprecation cadence.** "Frozen legacy path" — for how long? Stock missions stay on ABL forever? New stock content authored in TechScript, old stock content stays ABL?
5. **288-function ABL→TechScript mapping.** Where does this artifact live? Is it modder-public for reference?
6. **Editor-vs-runtime parity.** The Mission Editor has authoring views (visual / text / FIT). Runtime evaluation is FIT-only. Round-tripping invariant: any text the editor can author must produce FIT the runtime can execute. Is this enforced?

---

## Doc cross-references

**Primary source:**
- [Methuselas — TechScript Architecture Proposal (Discussion #18)](https://github.com/ThranduilsRing/mc2-opengl-remastered/discussions/18)

**Sister specs in this worktree:**
- [`2026-04-29-modders-paradise-roadmap-design.md`](2026-04-29-modders-paradise-roadmap-design.md) — sidecar principle, rejection list (still authoritative)
- [`2026-04-30-track-c-implementation-readiness-audit.md`](2026-04-30-track-c-implementation-readiness-audit.md) — Lua-primary master audit (now reference, not live)
- [`2026-04-30-pre-flight-and-risk-map.md`](2026-04-30-pre-flight-and-risk-map.md) — Lua-primary pre-flight (now reference)
- [`2026-04-30-track-f-scalable-hierarchical-ai-design.md`](2026-04-30-track-f-scalable-hierarchical-ai-design.md) — four-layer hierarchy (still applies; reframed by three-tier model above)
- [`2026-04-30-battletech-modder-conventions-design.md`](2026-04-30-battletech-modder-conventions-design.md) — collaborator stub (still applies; data-side conventions independent of script-language choice)

**Memory entries:**
- [`memory/methuselas_techscript_proposal.md`](../../../memory/methuselas_techscript_proposal.md) — short-form hook + discussion link
- [`memory/modders_paradise_roadmap.md`](../../../memory/modders_paradise_roadmap.md) — strategic backdrop (banner added 2026-04-30 noting this shift)

---

## Disposition for future sessions

**If you are starting any modding-related work after 2026-04-30:**

1. Read this doc first.
2. Read [Discussion #18](https://github.com/ThranduilsRing/mc2-opengl-remastered/discussions/18).
3. The 23-doc Track C corpus is **reference**, not active path. Pre-flight checklist + implementation-readiness audit do **not** apply to TechScript implementation; they apply to the deferred Lua sidecar if/when it lands.
4. Coordinate with Methuselas before authoring any new spec under "Track C" or "Lua" — those names are reserved for the deferred sidecar.
5. New scripting work goes under "TechScript" in `docs/superpowers/specs/` and `docs/superpowers/explorations/`.
