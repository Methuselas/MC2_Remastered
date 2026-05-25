# SUPERSEDED — see [`2026-05-01-gosfx-white-saturation-handoff.md`](2026-05-01-gosfx-white-saturation-handoff.md)

This handoff was renamed and rewritten on 2026-05-01 (session 2) after RenderDoc bisection eliminated bloom, god rays, and gosFX-FBO theories. The "bloom-particle" framing in the original file name was actively misleading — the bug is not in the bloom path (which is default-off) but in **gosFX/MLR particle rendering or GL-state leakage around it**.

The current handoff is at:

> [`2026-05-01-gosfx-white-saturation-handoff.md`](2026-05-01-gosfx-white-saturation-handoff.md)

The canonical memory file is at:

> `memory/gosfx_white_saturation_bug.md` (was `bloom_bug_correlates_with_gosfx_effects.md`)

This file is kept as a redirect because external references (commit messages, other docs) may still link to it. Do not paste the old handoff into a fresh session — its hypothesis pool is mostly eliminated and its framing leads investigators down dead paths.
