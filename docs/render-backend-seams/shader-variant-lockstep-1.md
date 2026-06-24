# SHADER-VARIANT-LOCKSTEP-1

The static-prop **depth-prepass program must use the same shader variant as the color
program**. Enforced by `scripts/check-shader-variant.py` (registered `shader_variant`).

## The invariant (why it matters)

The depth prepass (`StaticPropDepth`, `static_prop.vert` + `static_prop_depth.frag`) lays
`gl_Position` that a later `GL_EQUAL` color pass re-tests against. If the depth program is
built with a different variant define-set than the color program (`MC2_COALESCE`,
`MC2_USE_VIEW_UNIFORMS`, `MC2_OBJECT_ID_BUFFER`, `MC2_STATICPROP_PBR_SLOTS`), the two
vertex paths can compute slightly different positions → the GL_EQUAL test fails **silently**
→ props vanish/flicker in complex scenes. Real render-correctness, not cosmetic.

## How it holds today (by construction)

`gos_static_prop_batcher.cpp` builds the depth program by **reusing the color prefix
variables**, not re-deriving its own:

```cpp
const bool depthUsesCoalesce = (s_staticPropProgramCoalesce != 0);
const char* depthPrefix = depthUsesCoalesce ? coalescePrefix.c_str() : legacyPrefix.c_str();
makeProgram("static_prop_depth", "static_prop.vert", "static_prop_depth.frag", depthPrefix);
```

`legacyPrefix` / `coalescePrefix` are the exact prefixes used to build the color programs
(`s_staticPropProgram` / `s_staticPropProgramCoalesce`), and `depthUsesCoalesce` mirrors the
color coalesce selection — so the define-set matches byte-for-byte.

## What the checker enforces (regression guard, FAIL)

1. The `"static_prop_depth"` makeProgram call passes the `depthPrefix` variable (not an
   inline-built string).
2. `depthPrefix` is assigned from `coalescePrefix`/`legacyPrefix` (the color define-sets).
3. `depthUsesCoalesce == (s_staticPropProgramCoalesce != 0)` (mirrors color selection).
4. Anchors: the color programs are still built from `legacyPrefix` / `coalescePrefix`.

Negative-tested: replacing `depthPrefix` with an inline `"#version 430\n"` FAILs (#2);
changing `depthUsesCoalesce` to a different condition FAILs (#3).

## Scope notes
- **Mech** has no color/depth-prepass pair (it uses a separate shadow program); no lockstep
  pair to check. **Terrain/water** have no paired color/depth-prepass variants.
- Source-only lint (runtime variant keys depend on env gates a static checker can't
  evaluate); guards the *construction linkage* that makes runtime keys match.

Recon: [shader-variant-ownership-recon-1](shader-variant-ownership-recon-1.md).
