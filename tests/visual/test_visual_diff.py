#!/usr/bin/env python3
"""Synthetic-fixture tests for the MC2 visual comparator.

Generates tiny PNGs + sidecars in a temp dir (stdlib only) and asserts the
layered verdicts + overall exit codes:

  identical                 -> MATCH  / exit 0
  1 pixel flipped > epsilon -> FLIP   / exit 1
  sidecar gate_env mismatch -> SUSPECT/ exit 2
  sub-epsilon noise (<=2)   -> MATCH  / exit 0

Run: py -3 tests/visual/test_visual_diff.py
"""

import json
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.normpath(os.path.join(HERE, "..", "..", "tools", "visual"))
sys.path.insert(0, TOOLS)

import visual_diff  # noqa: E402
from png_io import encode_png  # noqa: E402

W = H = 4
BASE_GATE = ["MC2_SMOKE_MODE", "MC2_SMOKE_SEED"]


def _solid(r, g, b):
    px = bytearray(W * H * 3)
    for p in range(W * H):
        px[p * 3] = r
        px[p * 3 + 1] = g
        px[p * 3 + 2] = b
    return px


def _write_capture(d, name, pixels, gate_env=None, deterministic=True):
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, name + ".png"), "wb") as f:
        f.write(encode_png(W, H, 3, pixels))
    side = {
        "build": {"sha": "deadbeef", "dirty": False, "branch": "test"},
        "mission": "mc2_01",
        "label": name,
        "frame": 151,
        "trigger_frame": 120,
        "deterministic": deterministic,
        "seed": 12648430,
        "width": W,
        "height": H,
        "gate_env": gate_env if gate_env is not None else list(BASE_GATE),
    }
    with open(os.path.join(d, name + ".json"), "w", encoding="utf-8") as f:
        json.dump(side, f)


def _run(baseline, candidate):
    rep = visual_diff.run(baseline, candidate, None, None)
    return rep


def _verdict(rep, name):
    for b in rep["bookmarks"]:
        if b["name"] == name:
            return b["verdict"]
    raise AssertionError("bookmark %s not in report" % name)


def main():
    tmp = tempfile.mkdtemp(prefix="mc2_vdiff_test_")
    failures = []

    def check(cond, msg):
        if cond:
            print("  PASS", msg)
        else:
            print("  FAIL", msg)
            failures.append(msg)

    base = os.path.join(tmp, "base")
    gray = _solid(100, 100, 100)
    _write_capture(base, "bm", gray)

    # 1) identical -> MATCH / exit 0
    cand = os.path.join(tmp, "ident")
    _write_capture(cand, "bm", _solid(100, 100, 100))
    rep = _run(base, cand)
    check(_verdict(rep, "bm") == "MATCH", "identical -> MATCH")
    check(rep["exit"] == 0, "identical -> exit 0")
    # identical bytes should be decided at layer 1 (hash short-circuit)
    check(rep["bookmarks"][0]["layer_decided"] == 1, "identical decided at layer 1")

    # 2) one pixel flipped beyond epsilon -> FLIP / exit 1
    cand = os.path.join(tmp, "flip")
    px = _solid(100, 100, 100)
    px[0] = 200  # delta 100 on R of pixel 0, well above epsilon
    _write_capture(cand, "bm", px)
    rep = _run(base, cand)
    check(_verdict(rep, "bm") == "FLIP", "1px flip -> FLIP")
    check(rep["exit"] == 1, "1px flip -> exit 1")
    check(rep["bookmarks"][0]["changed_px"] == 1, "1px flip -> changed_px == 1")
    check(rep["bookmarks"][0]["max_delta"] == 100, "1px flip -> max_delta 100")

    # 3) sidecar gate_env mismatch -> SUSPECT / exit 2 (even with identical pixels)
    cand = os.path.join(tmp, "suspect")
    _write_capture(cand, "bm", _solid(100, 100, 100), gate_env=["MC2_SMOKE_MODE"])
    rep = _run(base, cand)
    check(_verdict(rep, "bm") == "SUSPECT", "gate_env mismatch -> SUSPECT")
    check(rep["exit"] == 2, "gate_env mismatch -> exit 2")
    check(rep["bookmarks"][0]["layer_decided"] == 0, "suspect decided at layer 0")

    # 3b) deterministic:false -> SUSPECT / exit 2
    cand = os.path.join(tmp, "nondet")
    _write_capture(cand, "bm", _solid(100, 100, 100), deterministic=False)
    rep = _run(base, cand)
    check(_verdict(rep, "bm") == "SUSPECT", "deterministic:false -> SUSPECT")
    check(rep["exit"] == 2, "deterministic:false -> exit 2")

    # 4) sub-epsilon noise (<= 2 LSB) -> MATCH / exit 0 (not byte-identical, layer 2)
    cand = os.path.join(tmp, "noise")
    px = _solid(100, 100, 100)
    for p in range(W * H):  # +1/+2 jitter on every pixel, all within epsilon
        px[p * 3] = 102
        px[p * 3 + 1] = 101
        px[p * 3 + 2] = 100
    _write_capture(cand, "bm", px)
    rep = _run(base, cand)
    check(_verdict(rep, "bm") == "MATCH", "sub-epsilon noise -> MATCH")
    check(rep["exit"] == 0, "sub-epsilon noise -> exit 0")
    check(rep["bookmarks"][0]["layer_decided"] == 2, "noise reaches layer 2")
    check(rep["bookmarks"][0]["changed_px"] == 0, "noise -> changed_px 0")

    # 5) HTML triptych emits a self-contained file for a FLIP case
    cand = os.path.join(tmp, "flip2")
    px = _solid(100, 100, 100)
    px[0] = 255
    _write_capture(cand, "bm", px)
    html = os.path.join(tmp, "out.html")
    visual_diff.run(base, cand, os.path.join(tmp, "out.json"), html)
    with open(html, "r", encoding="utf-8") as f:
        body = f.read()
    check("data:image/png;base64," in body, "triptych embeds base64 images")
    check("<table" in body and "</table>" in body, "triptych has a table")

    print("")
    if failures:
        print("RESULT: %d FAILURE(S)" % len(failures))
        return 1
    print("RESULT: ALL TESTS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
