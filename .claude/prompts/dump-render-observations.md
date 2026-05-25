# Dump this session's render observations

Paste this prompt at a natural breakpoint in a session that has done substantive work on the MC2 rendering pipeline. The session will dump its observations into a date-stamped notes file. A separate synthesizer agent (`mc2-render-contract-synthesizer`) will later consume those notes and update `docs/render-contract.md`.

---

## The prompt (paste verbatim into the target session)

You have done substantive work on the MC2 rendering pipeline this session. Before the context is lost, capture what you learned in a structured notes file. A synthesizer agent will later merge your notes (plus other sessions' notes, plus current code, plus the existing render contract) into an updated `docs/render-contract.md`.

You are NOT writing prose for human consumption. You are writing structured input for a downstream agent.

### Where to save

Output filename: `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/docs/observations/YYYY-MM-DD-render-<topic>.md`

Replace:
- `YYYY-MM-DD` with today's date in that exact format
- `<topic>` with a hyphenated noun phrase describing the session's render focus (1-3 words). Examples: `mech-import`, `terrain-cull`, `gpu-static-prop`, `shadow-pipeline`, `compute-cull`, `substrate-coalesce`, `parity-soak`, `bring-up-checklist`.

If a file with that exact name already exists from another session today, append `-2`, `-3`, etc.

### Required file format (exact structure)

```markdown
# Render observations — YYYY-MM-DD — <topic>

**Source:** <one or two sentences: what did this session actually do? e.g. "Bringing up GPU-direct terrain decal renderer, Stage 2.B parity soak. Touched mclib/decal.cpp, shaders/decal.frag, gamecam.cpp render-call sequence.">

**Scope:** <one line: what part of the pipeline did this session see directly? e.g. "Object enqueue path, post-renderLists hook, depth state inheritance.">

---

## Confirmed facts about current code

<bullet list. Each fact = claim + file:line evidence grep-verified in this session. Be specific. Examples:
- `MC_TextureManager::renderLists` walks both `masterVertexNodes` and `masterHardwareVertexNodes` (mclib/txmmgr.cpp:N — grep `renderLists` to confirm).
- The post-renderLists hook for fast paths is at gamecam.cpp:N (grep `mcTextureManager->renderLists` for current line).

Do NOT duplicate things already in MEMORY.md or docs/render-contract.md unless the existing version is now wrong. The synthesizer reads those too; you do not need to restate them.>

---

## New observations not currently in MEMORY.md or render-contract.md

<bullet list. Each = a fact the session learned that ISN'T already encoded. This is the highest-value part of the dump. Examples:
- "`Terrain::renderWaterFastPath` is now called from `gamecam.cpp:N` instead of inside `Terrain::renderWater`. The early-return inside `renderWater` ensures the legacy enqueue is skipped. This isn't documented in the contract."
- "When a fast path uses both an indirect-draw count buffer AND a per-draw entry buffer, the entry buffer must be flushed BEFORE the indirect-draw command, otherwise the GPU reads stale entries. Discovered while debugging the substrate-coalesce per-packet rewrite."

For each observation, end with: `Where this should live: <docs/render-contract.md section name | MEMORY.md topic file | currently nowhere>`.>

---

## Contradictions found

<bullet list. Things the session learned that DISAGREE with what the contract or MEMORY.md currently says. Each = the contract/memory claim, the current code state, file:line evidence, your suggested resolution. Examples:
- "docs/render-contract.md Priority 1 says terrain.cpp uses projectZ as visibility producer. Grep returns zero projectZ refs in terrain.cpp; the call is now contained to quad.cpp. Suggested resolution: contract Priority 1 should be marked CONTAINED or REMOVED."

If you found no contradictions, write: `None found in this session.`>

---

## Open questions

<bullet list. Things this session wondered about but did not resolve. Each = the question + why it matters + what evidence was insufficient. Examples:
- "Does the substrate-coalesce path bypass `mcTextureManager::renderLists` entirely, or does it enqueue and let renderLists flush via a different code path? Read the substrate dispatch but did not trace through to the actual GL submission site."

These feed the synthesizer's AMBIGUOUS bucket — do not invent answers.>

---

## Suggested render-contract edits

<bullet list. Concrete changes you'd make to docs/render-contract.md if you were the synthesizer. Each = location in contract (section name) + proposed edit (add/remove/replace) + one-sentence rationale.

Be specific. Vague suggestions like "update Priority 1" are not useful; "Priority 1: replace 'terrain.cpp uses projectZ' with 'projectZ is contained to quad.cpp per the projectz-containment design' and downgrade priority to MAINTENANCE" is useful.

If you have no contract-edit suggestions, write: `None — this session's observations don't suggest specific contract changes.`>

---

## Methodology notes

<one paragraph: what did you grep? what files did you Read? any limits on your investigation (file you couldn't read, symbol that didn't grep cleanly, area you ran out of context to explore)?>
```

### Required quality bar

Before writing the file, satisfy each of these:

1. **Grep before line numbers.** Every `file:N` citation must be verified by Grep or Read during THIS session. If you can't verify in this session, write `file:?` and leave it for the synthesizer to resolve. Do not fabricate line numbers from memory.

2. **Don't restate what MEMORY.md or the contract already says.** Your job is the DELTA. The synthesizer reads the existing sources; redundancy makes its job harder.

3. **Surface tacit knowledge.** Things that aren't in any doc but you learned during the session — "when X happens, the usual cause is Y" or "the comment in F says A but actually B" or "this subsystem looks half-rewritten because Z" — go in "New observations." This is the highest-value content.

4. **Contradictions are precious — don't smooth them over.** If the contract or memory disagrees with what you saw, name the disagreement explicitly. The synthesizer needs to know contradictions exist; it will flag them for the user rather than silently choosing.

5. **No emoji anywhere.** Em-dash and standard Unicode punctuation OK; pictographic emoji not.

6. **Target length: 80-200 lines.** Dense and specific. Don't pad with prose.

### Process

1. Reflect on what this session actually touched. What part of the render pipeline did you see directly? What surprised you?
2. Decide the `<topic>` for the filename (1-3 hyphenated words).
3. Sketch the structure mentally — which observations belong in "Confirmed", "New", "Contradictions", "Open questions".
4. Write the file with the Write tool to the exact path specified above.
5. Print a one-line summary: "Saved YYYY-MM-DD-render-<topic>.md. N confirmed / N new / N contradictions / N questions. Suggested contract edits: <count or 'none'>." Then stop.

Do not commit the file. Do not modify docs/render-contract.md. Do not modify any code. Your only output is the notes file.
