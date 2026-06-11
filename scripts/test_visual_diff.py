#!/usr/bin/env python3
"""Thin wrapper: runs visual_diff.py --self-test (synthetic image checks)."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import visual_diff  # noqa: E402

if __name__ == "__main__":
    sys.exit(visual_diff.self_test())
