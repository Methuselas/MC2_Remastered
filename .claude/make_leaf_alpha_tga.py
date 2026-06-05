# Author an RGBA alpha-cutout leaf TGA for the model-override tree.
#
# Source leaf diffuse (tree_small_02_leaves_diff_1k.jpg) is an RGB leaf-cluster
# atlas painted on a PURE BLACK background. We key the background out by
# luminance (a soft 18->42 ramp) to build an 8-bit alpha channel, then write an
# uncompressed 32-bit BGRA TGA with bottom-left origin (desc=8) — byte-format
# identical to the engine's other deployed prop TGAs.
#
# Deploy as data/tgl/128/a_<leafstem>.tga  (the leading "a_" is MC2's alpha-
# channel naming convention; bdactor.cpp LoadOverrideRenderShapeTextures loads
# "a_"-prefixed names as gos_Texture_Alpha + SetTextureAlpha(true) -> the
# static-prop batcher flags STATIC_PROP_FLAG_ALPHA_TEST @0.5 for that packet.)
# The assimp_importer derives the "a_" prefix for glTF MASK/BLEND (or "leaf*")
# materials, so the imported leaf material's texture name resolves to this file.
from PIL import Image
import numpy as np
import sys

SRC = sys.argv[1] if len(sys.argv) > 1 else \
    r"C:/Users/Joe/Downloads/tree_small_02_1k.gltf/textures/tree_small_02_leaves_diff_1k.jpg"
DST = sys.argv[2] if len(sys.argv) > 2 else \
    r"A:/Games/mc2-opengl/mc2-win64-v0.3/data/tgl/128/a_tree_small_02_leaves_diff_1k.tga"

im = Image.open(SRC).convert("RGB")
a = np.asarray(im).astype(np.float32)
lum = a.mean(2)
lo, hi = 18.0, 42.0
alpha8 = (np.clip((lum - lo) / (hi - lo), 0, 1) * 255).astype(np.uint8)
rgb = a.astype(np.uint8)
H, W, _ = rgb.shape

# bottom-left origin storage; channel order B,G,R,A
rgb_f, al_f = rgb[::-1], alpha8[::-1]
bgra = np.dstack([rgb_f[:, :, 2], rgb_f[:, :, 1], rgb_f[:, :, 0], al_f]).astype(np.uint8)
hdr = bytes([0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0,
             W & 255, (W >> 8) & 255, H & 255, (H >> 8) & 255, 32, 8])
with open(DST, "wb") as f:
    f.write(hdr + bgra.tobytes())
print("wrote", DST, "alpha>128 frac", round(float((al_f > 128).mean()), 3))
