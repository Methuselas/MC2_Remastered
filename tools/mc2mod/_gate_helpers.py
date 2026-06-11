"""
Gate helper scripts for S8 integration gates.
Run standalone: python _gate_helpers.py <action>
"""
import hashlib
import json
import os
import sys


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def snapshot(root):
    snap = {}
    for dirpath, dirs, files in os.walk(root):
        dirs.sort()
        for fname in sorted(files):
            abs_path = os.path.join(dirpath, fname)
            rel = os.path.relpath(abs_path, root).replace("\\", "/")
            size = os.path.getsize(abs_path)
            sha = sha256_file(abs_path)
            snap[rel] = (size, sha)
    return snap


if __name__ == "__main__":
    action = sys.argv[1]

    if action == "snapshot":
        root = sys.argv[2]
        out = sys.argv[3]
        snap = snapshot(root)
        with open(out, "w") as fh:
            json.dump(snap, fh)
        print(f"snapshot: {len(snap)} files -> {out}")

    elif action == "diff":
        f1 = sys.argv[2]
        f2 = sys.argv[3]
        with open(f1) as fh:
            s1 = json.load(fh)
        with open(f2) as fh:
            s2 = json.load(fh)
        # Compare
        diff = {}
        all_keys = set(s1.keys()) | set(s2.keys())
        for k in sorted(all_keys):
            if s1.get(k) != s2.get(k):
                diff[k] = {"before": s1.get(k), "after": s2.get(k)}
        print(f"diff count: {len(diff)}")
        if diff:
            for k, v in list(diff.items())[:10]:
                print(f"  {k}: {v}")
        else:
            print("  byte-identical: PASS")
