# Slice 1 Postmortem - why "retire the quadSetupTextures orphan producer" could not proceed

Date: 2026-05-19. Outcome: Slice 1 CANCELLED, premise falsified at root.
No engine code was changed (the methodology caught it pre-edit).

## The claim that died

Recon -> recon-extension (terrain-indirect + cpu-gpu-offload advisors,
greybeard) -> design -> outside review -> user-ruled Narrow-A all
modeled `TerrainQuad::setupTextures`'s five per-quad member writes
(`terrainHandle/terrainDetailHandle/overlayHandle/uvData/isCement`) as a
redundant per-frame copy of immutable Shape-C mission-load data, whose
sole consumer `TerrainQuad::draw` was default-dead via 60f2ef8 - hence
an "orphan producer" deletable by repointing `draw` at
`getTerrainFaceCacheEntry(tileR,tileC)`.

## Why it is false (grep-verified, HEAD)

`setupTextures` (quad.cpp:720-2061) computes per-quad, per-frame,
**camera-dependent visibility** and encodes it INTO the handle fields as
a cull channel:
- quad.cpp:955-967 `if (!isTerrainQuadVisible(*this)) { terrainHandle =
  ... = 0xffffffff; }` (camera-dependent; def quad.cpp:432).
- quad.cpp:738-747 comment, verbatim: reset to the `0xffffffff` sentinel
  "so draw() exits at its first guard rather than emitting tris."
- quad.cpp:2064 `if (terrainHandle != 0xffffffff)` IS `draw()`'s per-
  quad cull early-return (also the 2076 pure-water exit, the
  terrain.cpp:1124 drawPass hoist).

The static cache has no camera and is valid for ~every tile regardless
of view. Repointing `draw`/the hoist at it defeats the per-quad cull ->
the entire map renders, not the camera-visible set = the catastrophic
zoomed-out-big-map regression class, to which tier1 default-camera
smoke is structurally blind. There is no separate per-quad visibility
boolean - the handle sentinel IS the cull channel.

## Root cause of the misread

The visibility write (962-966) and the cull read (2064) are
bare-identifier `this->` member access; the sentinel-as-cull semantics
are invisible to a recipe-content-shaped grep (the recon enumerated with
`->member` pointer-arrow). Same data-flow-audit asymmetry as the
policy-split-wrapper grep trap, one layer deeper. Every analysis layer
inherited the same blind spot; none re-derived from the visibility
branch.

## The five escalating collisions (the rigor working)

1. drawPass "default-OFF" docs were lying (default-ON since 60f2ef8).
2. The DRAWALPHA dead-proof counter instruments the wrong site.
3. `setupTextures` is a 720-2061 multi-writer tangle, not a 1-site
   shuttle; `draw` not sole consumer.
4. Narrow-A: the members are dual-purpose recipe + (then thought)
   hoist-entangled state.
5. Terminal (Slice 1): the members ARE the per-frame camera
   visibility-cull channel. Static repoint impossible by construction.
6. Terminal (Slice 2): the water-projection 6-tuple's candidate set is
   gated by a per-quad `(clipped1||clipped2)` predicate that sums
   sibling-corner `clipInfo` (quad.cpp:1047-1054) - and slimReduce IS
   the producer of `clipInfo` (terrain.cpp:1811), running entirely
   before the water block (slimReduce loop terrain.cpp:1701;
   quadSetupTextures zone terrain.cpp:1939). Folding the water visit
   into slimReduce's per-vertex loop is CIRCULAR (needs clipInfo
   slimReduce hasn't produced yet). An unconditional `water&1` superset
   flips the parity probe to `identical` (the probe only checks "B adds
   nothing beyond A") while silently shifting the `setInverseProject`
   extrema = green-probe-masked regression. Relocating the block to
   another post-slimReduce pass = the old block moved = inertia.

Each collision was caught by code-grounded verification BEFORE editing
engine code. Net engine regressions: zero. Engine changes shipped: zero.

## The honest conclusion

`setupTextures`'s per-quad walk has an irreducible per-frame
camera-dependent component - the terrain visibility cull - structurally
analogous to why `slimReduce` is irreducible. The original recon's
"inertia / nothing to bank" verdict was correct, for a deeper reason
than it gave. Slice 1 is dead in every form; re-opening it as a
static-repoint = repeating a forbidden marathon. The genuine fix (if
ever wanted) is an architecture change to the per-frame terrain cull
itself - see the companion handoff
`memory/HANDOFF_actual_terrain_perframe_cull_fix.md`. Full durable
record: `memory/setuptextures_is_a_multiwriter_tangle_not_a_clean_
shuttle.md`.

**Slice 2 (water 6-tuple -> slimReduce) is ALSO terminally dead** -
collision #6 above. It was believed independent and advisor-validated;
execution proved the fold is circular on slimReduce's own `clipInfo`,
cannot be deleted (probe DIVERGENT = real unique extrema feeding
cursor/cull/camera), and cannot be relocated without inertia. The
entire quadSetupTextures-retirement effort therefore yields NO engine
change. This is not a failure - it is an exhaustive, code-grounded
proof (from every angle: greybeard, adversarial, steelman, "what are we
not thinking of") that the remaining per-frame per-quad terrain CPU
work is IRREDUCIBLE: the camera-dependent visibility cull + the
clipInfo-gated water-projection extrema, both entangled with the
slimReduce cull cascade. That proof permanently closes a recurring
inertia magnet, which is the real value delivered.
