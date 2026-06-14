# Modern Tree Pack v1

Replaces three broadleaf tree families and one crane in MC2 using the static asset cook pipeline.
This is an example mod for the mc2mod toolchain — it demonstrates LOD manifests, texture cooking,
mod packaging, and byte-identical install/uninstall.

## What is replaced

| Stock appearance | Replacement | LODs |
|---|---|---|
| `tree:maple1` | Broadleaf maple | 3 variants (0 / 600m / 1200m) |
| `tree:pine1` | Broadleaf poplar | 4 variants (0 / 600m / 1200m / 2000m) |
| `tree:maple2` | White poplar | 4 variants (0 / 600m / 1200m / 2000m) |
| `staticprop:crane` | crane_big | 1 LOD |

**LOD note:** The numbered source files (maple1/2/3, poplar1/2/3/4, etc.) are same-resolution
shape variants wired as a LOD chain to demonstrate the config structure.
They are not decimated polygon LODs.

## Prerequisites

Source assets expected locally under:
  C:/Users/Joe/Downloads/GameAsset/

## Build from source

```powershell
# Full cook (clean slate):
python tools/examples/build_modern_tree_pack.py `
  --asset-root "C:/Users/Joe/Downloads/GameAsset" `
  --out "mods/modern-tree-pack-v1" `
  --deploy-root "A:/Games/mc2-opengl/mc2-win64-v0.4" `
  --clean

# Cook one family only (incremental gate):
python tools/examples/build_modern_tree_pack.py `
  --asset-root "C:/Users/Joe/Downloads/GameAsset" `
  --out "mods/modern-tree-pack-v1" `
  --family maple
```

Generated artifacts (GLBs, KTX2s) are gitignored — rebuild from source on a fresh clone.

## Pack and install

```powershell
# Pack
python tools/mc2mod/mc2mod.py pack mods/modern-tree-pack-v1 --out mods/dist

# Verify hashes
python tools/mc2mod/mc2mod.py verify-lite mods/dist/modern-tree-pack-v1-1.0.0.mc2mod

# Install to a temp deploy copy (NEVER to canonical v0.4)
python tools/mc2mod/mc2mod.py install `
  mods/dist/modern-tree-pack-v1-1.0.0.mc2mod `
  --deploy "A:/Games/mc2-opengl/mc2-win64-v0.4-mtp-test"

# Launch with mod active
$env:MC2_ACTIVE_MOD = "modern-tree-pack-v1"

# Uninstall (restores byte-identical deploy)
python tools/mc2mod/mc2mod.py uninstall modern-tree-pack-v1 `
  --deploy "A:/Games/mc2-opengl/mc2-win64-v0.4-mtp-test"
```
