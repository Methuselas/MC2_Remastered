#!/usr/bin/env python3
"""visual_compare.py — VISUAL-DIFF-PERCEPTUAL-HARNESS-1

General, policy-driven image comparison for visual regression. Byte-exact is the
default and correct bar for deterministic-math shaders; a perceptual policy exists
for procedural/noise shaders (FBM + smoothstep) where the driver GLSL compiler and
glslang's SPIR-V codegen produce ULP-level float differences that flip smoothstep
edges — real pixel deltas that do NOT mean the shader is wrong, only that byte-exact
is the wrong gate for that class.

Metrics (always computed + reported):
  - byte_exact       : file bytes identical
  - max_abs_delta    : max per-channel |a-b| over all pixels (0..255)
  - pct_changed      : % of pixels with any channel delta > changed_lsb
  - ssim             : windowed SSIM on luma (1.0 == identical)

Policies (which metrics gate PASS/FAIL):
  - byte_exact   (DEFAULT) : require byte-identical
  - low_tolerance          : max_abs_delta <= 2 AND pct_changed(>2) <= 0.05%
  - perceptual             : ssim >= ssim_min AND pct_changed(>changed_lsb) <= max_pct
                             (tolerates ULP/noise drift; still fails on real change)

Per-test policy is resolved from docs/render-backend-seams/visual-tolerance-policy.json
(default byte_exact; perceptual ONLY for allowlisted families: cloud, shoreline, ...),
or forced with --policy. On FAIL an amplified diff PNG is emitted.

Usage:
  py -3 scripts/visual_compare.py <golden.png> <candidate.png> [--policy P]
        [--family NAME] [--out-diff diff.png] [--json out.json]
  py -3 scripts/visual_compare.py --self-test        # synthetic, no captures needed

Exit: 0 PASS, 2 FAIL. No render/shader code; read-only on inputs (writes diff on fail).
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
import sys
from pathlib import Path

try:
    import numpy as np
    from PIL import Image
    HAVE = True
except Exception:
    HAVE = False

ROOT = Path(__file__).resolve().parents[1]
POLICY_FILE = "docs/render-backend-seams/visual-tolerance-policy.json"

DEFAULT_POLICIES = {
    "byte_exact":    {"require_byte_identical": True},
    "low_tolerance": {"max_abs_delta": 2, "changed_lsb": 2, "max_pct_changed": 0.05},
    # perceptual: tolerate ULP/noise edge-drift (small AVERAGE delta + preserved
    # structure) while still failing real change. mean_abs_delta is the robust
    # discriminator (noise drift -> tiny mean; a real change -> large mean / SSIM
    # collapse). ssim_min is a lenient secondary structure floor. These are
    # STARTING defaults; the cloud/shoreline pilot tunes them vs real captures.
    "perceptual":    {"mean_abs_delta": 8.0, "ssim_min": 0.90, "changed_lsb": 4},
}


def _sha(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def _load(p):
    return np.asarray(Image.open(p).convert("RGB"), dtype=np.float64)


def _boxmean(x, win):
    """Exact windowed mean via integral image (numpy only, O(N))."""
    r = win // 2
    xp = np.pad(x, r, mode="edge")
    cs = np.cumsum(np.cumsum(xp, axis=0), axis=1)
    cs = np.pad(cs, ((1, 0), (1, 0)))
    H, W = x.shape
    w = 2 * r + 1
    S = cs[w:w + H, w:w + W] - cs[0:H, w:w + W] - cs[w:w + H, 0:W] + cs[0:H, 0:W]
    return S / (w * w)


def _ssim(a, b, win=7, L=255.0):
    # luma SSIM (Wang et al.) with a uniform window.
    la = a @ np.array([0.299, 0.587, 0.114])
    lb = b @ np.array([0.299, 0.587, 0.114])
    C1 = (0.01 * L) ** 2
    C2 = (0.03 * L) ** 2
    mua, mub = _boxmean(la, win), _boxmean(lb, win)
    mua2, mub2, muab = mua * mua, mub * mub, mua * mub
    sa = _boxmean(la * la, win) - mua2
    sb = _boxmean(lb * lb, win) - mub2
    sab = _boxmean(la * lb, win) - muab
    smap = ((2 * muab + C1) * (2 * sab + C2)) / ((mua2 + mub2 + C1) * (sa + sb + C2))
    return float(smap.mean())


def compute_metrics(golden, candidate):
    byte_eq = _sha(golden) == _sha(candidate)
    a, b = _load(golden), _load(candidate)
    if a.shape != b.shape:
        return {"byte_exact": False, "shape_mismatch": [a.shape, b.shape],
                "max_abs_delta": 255.0, "pct_changed": 100.0, "ssim": 0.0}
    delta = np.abs(a - b)
    maxd = float(delta.max())
    return {"byte_exact": byte_eq, "max_abs_delta": maxd,
            "delta": delta, "a": a, "b": b, "ssim": None}


def evaluate(golden, candidate, policy_name, policies, out_diff=None):
    m = compute_metrics(golden, candidate)
    pol = policies.get(policy_name, DEFAULT_POLICIES["byte_exact"])
    fails = []
    if m.get("shape_mismatch"):
        fails.append(f"shape mismatch {m['shape_mismatch']}")

    if policy_name == "byte_exact" or pol.get("require_byte_identical"):
        if not m["byte_exact"]:
            fails.append("not byte-identical")
        result = {"policy": policy_name, "byte_exact": m["byte_exact"],
                  "max_abs_delta": m.get("max_abs_delta")}
    else:
        changed_lsb = pol.get("changed_lsb", 2)
        delta = m["delta"]
        changed = (delta > changed_lsb).any(axis=2)
        pct = 100.0 * float(changed.mean())
        mean_abs = float(delta.mean())
        result = {"policy": policy_name, "byte_exact": m["byte_exact"],
                  "max_abs_delta": m["max_abs_delta"], "mean_abs_delta": mean_abs,
                  "pct_changed": pct}
        if "max_abs_delta" in pol and m["max_abs_delta"] > pol["max_abs_delta"]:
            fails.append(f"max_abs_delta {m['max_abs_delta']:.0f} > {pol['max_abs_delta']}")
        if "mean_abs_delta" in pol and mean_abs > pol["mean_abs_delta"]:
            fails.append(f"mean_abs_delta {mean_abs:.3f} > {pol['mean_abs_delta']}")
        if "max_pct_changed" in pol and pct > pol["max_pct_changed"]:
            fails.append(f"pct_changed {pct:.3f}% > {pol['max_pct_changed']}%")
        if "ssim_min" in pol:
            s = _ssim(m["a"], m["b"])
            result["ssim"] = s
            if s < pol["ssim_min"]:
                fails.append(f"ssim {s:.4f} < {pol['ssim_min']}")

    ok = not fails
    if not ok and out_diff and "delta" in m and not m.get("shape_mismatch"):
        amp = np.clip(m["delta"] * 8.0, 0, 255).astype(np.uint8)
        Image.fromarray(amp, "RGB").save(out_diff)
        result["diff_image"] = str(out_diff)
    result["fails"] = fails
    result["pass"] = ok
    return result


def load_policies(root):
    p = root / POLICY_FILE
    if p.exists():
        doc = json.load(open(p, encoding="utf-8"))
        pols = dict(DEFAULT_POLICIES)
        pols.update(doc.get("policies", {}))
        return pols, doc.get("default_policy", "byte_exact"), doc.get("allowlist", {})
    return dict(DEFAULT_POLICIES), "byte_exact", {}


# --------------------------------------------------------------------------- #
def self_test():
    if not HAVE:
        print("[visual_compare] SELF-TEST SKIP: numpy/Pillow unavailable")
        return 0
    import tempfile
    rng = np.random.RandomState(12345)
    H = W = 128
    yy, xx = np.mgrid[0:H, 0:W]
    base = ((xx + yy) % 256).astype(np.uint8)
    base = np.stack([base, (xx % 256).astype(np.uint8), (yy % 256).astype(np.uint8)], axis=2)
    td = Path(tempfile.mkdtemp())
    g = td / "g.png"; Image.fromarray(base, "RGB").save(g)

    def save(arr, name):
        p = td / name; Image.fromarray(np.clip(arr, 0, 255).astype(np.uint8), "RGB").save(p); return p

    ident = save(base.copy(), "ident.png")
    onepx = base.copy(); onepx[H // 2, W // 2] = [255, 0, 0]; onepx = save(onepx, "onepx.png")
    # noise-like: ~3% pixels nudged by up to 40 (simulates FBM/smoothstep edge drift)
    noise = base.astype(np.int16).copy()
    mask = rng.rand(H, W) < 0.03
    noise[mask] += rng.randint(-40, 41, size=(mask.sum(), 3))
    noisep = save(noise, "noise.png")
    # large visible: 30% pixels shifted by 150
    big = base.astype(np.int16).copy()
    bmask = rng.rand(H, W) < 0.30
    big[bmask] += 150
    bigp = save(big, "big.png")
    # uniform +10 tint: a real (subtle) regression — must FAIL perceptual (mean delta)
    tintp = save(base.astype(np.int16) + 10, "tint.png")

    pols = DEFAULT_POLICIES
    cases = []
    def chk(desc, golden, cand, policy, want_pass, want_diff=None):
        out_diff = td / f"diff_{len(cases)}.png"
        r = evaluate(str(golden), str(cand), policy, pols, out_diff=str(out_diff))
        ok = (r["pass"] == want_pass)
        if want_diff is not None:
            ok = ok and (os.path.exists(out_diff) == want_diff)
        cases.append((desc, ok, r["pass"], r.get("fails")))
        return ok

    chk("identical / byte_exact PASS", g, ident, "byte_exact", True)
    chk("one-pixel / byte_exact FAIL + diff emitted", g, onepx, "byte_exact", False, want_diff=True)
    chk("noise / byte_exact FAIL", g, noisep, "byte_exact", False)
    chk("noise / low_tolerance FAIL (>2 LSB)", g, noisep, "low_tolerance", False, want_diff=True)
    chk("noise / perceptual PASS (ULP-like drift)", g, noisep, "perceptual", True)
    chk("large / perceptual FAIL + diff emitted", g, bigp, "perceptual", False, want_diff=True)
    chk("uniform +10 tint / perceptual FAIL (subtle regression caught)", g, tintp, "perceptual", False)
    chk("identical / perceptual PASS", g, ident, "perceptual", True)

    allok = all(c[1] for c in cases)
    print("[visual_compare] SELF-TEST")
    for desc, ok, gotpass, fails in cases:
        print(f"  {'ok ' if ok else 'BAD'}  {desc}  (pass={gotpass}{'' if ok else ', UNEXPECTED'})")
    print(f"  result: {'PASS' if allok else 'FAIL'}")
    return 0 if allok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("golden", nargs="?")
    ap.add_argument("candidate", nargs="?")
    ap.add_argument("--policy", default=None)
    ap.add_argument("--family", default=None, help="resolve policy from the allowlist")
    ap.add_argument("--out-diff", default=None)
    ap.add_argument("--json", default=None)
    ap.add_argument("--root", default=None)
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    if not (args.golden and args.candidate):
        ap.error("golden and candidate required (or --self-test)")
    if not HAVE:
        print("[visual_compare] FAIL: numpy/Pillow required"); return 2
    root = Path(args.root) if args.root else ROOT
    pols, default_policy, allowlist = load_policies(root)
    policy = args.policy or (allowlist.get(args.family) if args.family else None) or default_policy
    r = evaluate(args.golden, args.candidate, policy, pols, out_diff=args.out_diff)
    if args.json:
        json.dump(r, open(args.json, "w"), indent=2, default=str)
    print(f"[visual_compare] policy={policy} pass={r['pass']} "
          f"byte_exact={r.get('byte_exact')} max_abs_delta={r.get('max_abs_delta')} "
          f"pct_changed={r.get('pct_changed')} ssim={r.get('ssim')}")
    for f in r["fails"]:
        print(f"  FAIL: {f}")
    if r.get("diff_image"):
        print(f"  diff: {r['diff_image']}")
    return 0 if r["pass"] else 2


if __name__ == "__main__":
    sys.exit(main())
