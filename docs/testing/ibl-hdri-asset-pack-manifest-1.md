# IBL-HDRI-ASSET-PACK-MANIFEST-1

**Status:** SHIPPED. docs/tools only — no production, no large-asset commits, no
runtime loader change.
**Parent arc:** [subsystem-harness-arc-1.md](subsystem-harness-arc-1.md).
Resolves the open item from
[lighting-staticprop-harness-recon-1.md](lighting-staticprop-harness-recon-1.md):
7 of 8 registry HDRIs are not in Git because the 16K EXRs are too large.

## The problem
`RenderCore/IblHdriRegistry.h` references 8 distinct HDRIs. Only
`DaySkyHDRI063B_4K` is small enough to track in Git; the seven 16K pureskys /
night skies (~100-200 MB each) exceed Git/LFS limits. Without an explicit policy,
every future agent either rediscovers the "missing assets" gap or tries to commit
128 MB files. The IBL harness's asset test could only be informational.

## The contract
A declarative manifest, `docs/testing/ibl_hdri_external_pack.txt`, lists the HDRIs
distributed as an **external pack** (installed into `data/hdr/`, not committed).
The IBL harness (`tools/ibl_registry_contract_harness`) now enforces:

- **`every_hdri_tracked_or_declared`** (default): every registry HDRI must be
  EITHER present on disk (tracked-in-repo / installed) OR declared in the
  manifest. A reference that is **neither** — a typo'd path, or a new sky added
  with no asset and no manifest line — **FAILS** the default suite. The known
  external pack passes because it is declared, so clean checkouts stay green.
- **`manifest_entries_match_registry`** (default): every manifest path must be
  referenced by some registry entry (catches a stale declaration left after a
  registry edit).
- **`hdri_assets_inventory`** (default, informational): lists present vs missing.
- **`hdri_assets_exist_strict`** (`--test` or `MC2_IBL_ASSET_STRICT=1`): requires
  every referenced HDRI — including external ones — to be installed locally.

Proven: removing a manifest line for an absent HDRI flips
`every_hdri_tracked_or_declared` to FAIL; restoring it → PASS.

## Manifest format
Pipe-delimited text (no JSON parser needed by the C++ harness); `#` comments:
```
<repo-relative-path> | <size-bytes-or-?> | <sha256-or-?> | <source/notes>
```
Only the path field is contract-significant. size/sha are advisory metadata for
future pack verification (`?` = not yet recorded — fill when the pack is built).

## Installing / verifying the external pack
1. Obtain the seven 16K EXRs listed in `docs/testing/ibl_hdri_external_pack.txt`
   (sources noted per line) and place them at the listed `data/hdr/` paths.
2. Cook the `.ktx2` GPU sidecars: `py -3 tools/cook_bc6h_hdri.py <name>.exr`
   (the loader uses `exrPath` and derives the `.ktx2` sidecar).
3. Verify presence:
   `build64-ibl/.../ibl_registry_contract_harness.exe --test hdri_assets_exist_strict`
   (exit 0 = all installed) or set `MC2_IBL_ASSET_STRICT=1` for the full suite.

## Adding a new sky
Cook/install the asset into `data/hdr/`, add its `RenderCore/IblHdriRegistry.h`
entry, and add a manifest line. The default suite then stays green; omitting the
manifest line (or the asset) fails `every_hdri_tracked_or_declared` immediately.

## Explicitly NOT done
- No EXR/KTX2 committed to Git (the whole point).
- No runtime loader change — `gos_postprocess.cpp` already uses `exrPath`; missing
  assets fall back to default at runtime as before.
- size/sha hashes left as `?` until the canonical pack is assembled.
