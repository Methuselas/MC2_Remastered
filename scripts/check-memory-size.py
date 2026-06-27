#!/usr/bin/env python3
"""check-memory-size.py -- auto-memory index size guard.

Background: the user's auto-memory index (MEMORY.md + INDEX-*.md) lives OUTSIDE
this repo, under the Claude projects dir. When MEMORY.md silently regrew past the
context load limit (~24.4 KB) it got TRUNCATED on load -- only part of the index
was injected into the session, so handoff pointers went missing until someone
noticed (it had reached 37 KB by 2026-06-27). This check makes that failure mode
loud instead of silent: it measures every index file and FAILs if any meets/exceeds
the hard context-read limit, WARNs as an early advisory below that.

The memory dir is user/machine-specific, so a missing dir is a SKIP (exit 0),
not a failure -- this keeps the check harmless on CI or another machine.

Usage:
  py -3 scripts/check-memory-size.py [--dir PATH]
  MC2_MEMORY_DIR=... py -3 scripts/check-memory-size.py

Exit code: 0 if all checked files are under FAIL_BYTES (or dir missing);
1 if any file >= FAIL_BYTES. WARN is advisory and never fails.
"""
import argparse
import glob
import os
import sys
from pathlib import Path

# Default memory dir (user-specific; overridable via env or --dir).
DEFAULT_DIR = r"C:/Users/Joe/.claude/projects/A--Games-mc2-opengl-src/memory"

WARN_BYTES = 17100   # early advisory: index getting large
FAIL_BYTES = 24400   # hard context-read limit: load truncates past this


def index_files(mem_dir):
    """MEMORY.md + every INDEX-*.md, skipping *.original.md backups."""
    out = []
    memory = os.path.join(mem_dir, "MEMORY.md")
    if os.path.isfile(memory):
        out.append(memory)
    for p in sorted(glob.glob(os.path.join(mem_dir, "INDEX-*.md"))):
        if p.endswith(".original.md"):
            continue
        out.append(p)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dir", default=None,
                    help="memory dir (default: $MC2_MEMORY_DIR or built-in path)")
    args = ap.parse_args()

    mem_dir = args.dir or os.environ.get("MC2_MEMORY_DIR") or DEFAULT_DIR

    if not os.path.isdir(mem_dir):
        print("[check-memory-size] SKIP: memory dir not found: %s" % mem_dir)
        print("[check-memory-size] (user/machine-specific dir; not a failure)")
        return 0

    files = index_files(mem_dir)
    if not files:
        print("[check-memory-size] SKIP: no MEMORY.md / INDEX-*.md in %s" % mem_dir)
        return 0

    fail_count = 0
    warn_count = 0
    print("[check-memory-size] dir: %s" % mem_dir)
    print("[check-memory-size] WARN>=%dB  FAIL>=%dB" % (WARN_BYTES, FAIL_BYTES))
    for path in files:
        nbytes = os.path.getsize(path)
        name = Path(path).name
        if nbytes >= FAIL_BYTES:
            status = "FAIL"
            fail_count += 1
        elif nbytes >= WARN_BYTES:
            status = "WARN"
            warn_count += 1
        else:
            status = "OK"
        print("  %-4s %-28s %7dB" % (status, name, nbytes))

    print("[check-memory-size] %d OK, %d WARN, %d FAIL"
          % (len(files) - warn_count - fail_count, warn_count, fail_count))
    if fail_count:
        print("[check-memory-size] FAIL: index file(s) >= %dB will be TRUNCATED "
              "on context load -- trim the index (move detail to topic files)."
              % FAIL_BYTES, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
