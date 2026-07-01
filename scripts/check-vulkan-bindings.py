#!/usr/bin/env python3
"""check-vulkan-bindings.py — VULKAN-BINDINGS-CHECK-1

Offline Vulkan-prep report over the GPU binding-slot census
(`docs/render-backend-seams/binding-slot-occupancy.json`).

WHY THIS EXISTS
---------------
Under GL, a buffer binding-base slot number is only semantic *inside* a pass:
the same slot is legally rebound to different buffers in unrelated passes
(intentional multiplexing — see `binding-slot-occupancy.md`). Under Vulkan a
descriptor-set layout pins a fixed `(set, binding)` per resource. A slot that a
DISTINCT logical resource occupies while ANOTHER distinct live resource pins the
same physical slot cannot be expressed as one `(set, binding)` — it is a
descriptor collision that must be split BEFORE descriptor-set assignment.

Discriminator (from the descriptor-layout-validator recon):
  * DISTINCT logical resources both live on one slot        -> VULKAN-COLLISION
  * SAME slot on mutually-exclusive #if/#else branches       -> benign (WARN)
    (only one block compiles per permutation — e.g. gpu_cull.comp slot-9
     VisibleIds/DebugOut)
  * purely-literal cross-pass reuse (isolatable per-pipeline) -> benign share

KNOWN-PRESENT collisions (SSBO slot-14, slot-20) were classified by the
binding-slot-occupancy recon; this script converts that finding into a standing
machine-checkable report so (a) they stay tracked as Vk-prep TODOs and (b) any
NEW distinct-role slot share is caught the moment it lands.

DEFAULT = REPORT (exit 0). The known collisions are NOT yet split, so failing by
default would just wedge check-contracts.sh. Pass --strict to exit nonzero when
any collision (known or new) is present — that is the future gate, flipped on
once the slots are split.

Tooling only. No runtime, no engine change, no shader edit.

Usage:
  py -3 scripts/check-vulkan-bindings.py [--json <occupancy.json>] [--strict] [--quiet]
"""
import argparse
import json
import os
import sys

# ---------------------------------------------------------------------------
# Curated classifications (sourced from the binding-slot-occupancy recon —
# docs/render-backend-seams/binding-slot-occupancy.md §"Intentional multiplexing"
# and §"Slot-14 particle/readback sharing", plus vulkan-readiness-audit-1.md D).
# ---------------------------------------------------------------------------

# Slots the recon confirmed as REAL Vulkan (set,binding) collisions: a distinct
# logical resource pinned to the same physical slot as another distinct live
# resource that a descriptor-set-layout generator CANNOT isolate per-pipeline
# (a pinned engine-wide named constant — LIGHT_DATA_SSBO_BINDING slot-20,
# READBACK_SSBO_BINDING slot-14 — coexisting with an unrelated GLSL role).
# These are the Vk-prep TODOs: split before descriptor assignment.
# Source: binding-slot-occupancy.md §"Slot-14 particle/readback sharing" and
# vulkan-readiness-audit-1.md dimension D.
KNOWN_VK_COLLISIONS = {"SSBO:14", "SSBO:20"}

# Slots whose multiple roles are mutually-exclusive #if/#else branches (only one
# compiles per permutation). The occupancy JSON does not encode preprocessor
# nesting, so the exclusive-branch set is curated from the checker's own WARN
# annotation ("mode-alternate (intentional)"). Benign — never a collision.
BENIGN_EXCLUSIVE_SLOTS = {"SSBO:9"}

# Slots with >1 distinct GLSL role that the recon classified as INTENTIONAL
# per-pass multiplexing: purely-literal cross-pass reuse where each pipeline gets
# its own descriptor-set layout under Vulkan, so the shared GL slot number is
# isolatable (legal). NOT collisions. Source: binding-slot-occupancy.md
# §"Intentional multiplexing (documented — NOT failures)".
BENIGN_MULTIPLEXED_SLOTS = {
    "SSBO:0", "SSBO:1", "SSBO:2", "SSBO:3", "SSBO:5", "SSBO:6", "SSBO:7",
    "SSBO:8", "SSBO:11", "SSBO:12", "SSBO:15", "SSBO:16", "SSBO:23", "UBO:1",
}


def load_occupancy(json_path):
    with open(json_path, "r", encoding="utf-8") as f:
        return json.load(f)


def glsl_roles(entries):
    """Distinct GLSL logical role names on a slot. C++ entries are just
    glBindBufferBase(literal) sites and carry no logical role — ignored for
    resource-distinctness."""
    roles = []
    for e in entries:
        if e.get("side") == "glsl":
            r = e.get("role")
            if r and r not in roles:
                roles.append(r)
    return roles


def classify(occupancy):
    """Return (collisions, benign_shares, single_use) lists of dicts."""
    collisions = []      # >1 distinct live GLSL role, not exclusive-branch
    benign_shares = []   # exclusive-#if, or single-role slots bound cross-pass
    single_use = []      # exactly one GLSL role, one binding site

    for slot in sorted(occupancy.keys(), key=_slot_sort_key):
        entries = occupancy[slot]
        roles = glsl_roles(entries)
        cpp_sites = sum(1 for e in entries if e.get("side") == "cpp")

        rec = {"slot": slot, "roles": roles, "cpp_sites": cpp_sites}

        if len(roles) <= 1:
            if cpp_sites > 1 or len(entries) > 1:
                # one role, multiplexed cross-pass — benign
                rec["reason"] = "single-role (cross-pass reuse — isolatable per-pipeline)"
                benign_shares.append(rec)
            else:
                single_use.append(rec)
            continue

        # >1 distinct GLSL role on this physical slot.
        if slot in BENIGN_EXCLUSIVE_SLOTS:
            rec["reason"] = "mutually-exclusive #if branches (one compiles per permutation)"
            benign_shares.append(rec)
            continue

        if slot in BENIGN_MULTIPLEXED_SLOTS:
            rec["reason"] = "intentional per-pass multiplexing (per-pipeline descriptor-set isolatable)"
            benign_shares.append(rec)
            continue

        if slot in KNOWN_VK_COLLISIONS:
            # Recon-classified real collision — tracked Vk-prep debt.
            rec["known"] = True
            collisions.append(rec)
            continue

        # Distinct live roles, but in NEITHER curated list -> genuinely new.
        # Surfaced as a collision so it can't slip; needs human classification.
        rec["known"] = False
        collisions.append(rec)

    return collisions, benign_shares, single_use


def _slot_sort_key(slot):
    ns, _, num = slot.partition(":")
    try:
        return (ns, int(num))
    except ValueError:
        return (ns, 1 << 30)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    default_json = os.path.join(
        here, "..", "docs", "render-backend-seams", "binding-slot-occupancy.json"
    )

    ap = argparse.ArgumentParser(description="Vulkan shared-binding collision report.")
    ap.add_argument("--json", default=default_json,
                    help="path to binding-slot-occupancy.json")
    ap.add_argument("--strict", action="store_true",
                    help="exit nonzero if any VULKAN-COLLISION is present "
                         "(future gate; default is report-only, exit 0)")
    ap.add_argument("--quiet", action="store_true",
                    help="suppress the report body; still honors --strict exit code")
    args = ap.parse_args()

    json_path = os.path.normpath(args.json)
    if not os.path.isfile(json_path):
        print("check-vulkan-bindings: occupancy JSON not found: %s" % json_path,
              file=sys.stderr)
        return 2

    data = load_occupancy(json_path)
    occupancy = data.get("occupancy", {})
    collisions, benign_shares, single_use = classify(occupancy)

    total_slots = len(occupancy)
    known = [c for c in collisions if c.get("known")]
    new = [c for c in collisions if not c.get("known")]

    if not args.quiet:
        print("=" * 69)
        print("  VULKAN SHARED-BINDING REPORT  (VULKAN-BINDINGS-CHECK-1)")
        print("=" * 69)
        print("  source: %s" % json_path)
        print("  total binding slots         : %d" % total_slots)
        print("  VULKAN-COLLISIONS           : %d  (%d known / %d new)"
              % (len(collisions), len(known), len(new)))
        print("  benign shares               : %d" % len(benign_shares))
        print("  single-use slots            : %d" % len(single_use))
        print("-" * 69)

        if collisions:
            print("  VULKAN-COLLISIONS — distinct live resources on one physical slot")
            print("  (Vk (set,binding)-illegal — SPLIT before descriptor assignment):")
            for c in collisions:
                tag = "KNOWN Vk-prep TODO" if c.get("known") else "NEW — investigate"
                print("    [%s] %s  roles=%s" % (tag, c["slot"], ", ".join(c["roles"])))
                print("        (+%d C++ glBindBufferBase site(s))" % c["cpp_sites"])
            print("-" * 69)

        if benign_shares:
            print("  benign shares (legal under Vulkan — informational):")
            for b in benign_shares:
                roles = ", ".join(b["roles"]) if b["roles"] else "(literal-only)"
                print("    %-9s %s  [%s]" % (b["slot"], roles, b["reason"]))
            print("-" * 69)

        if new:
            print("  ** NEW shared binding not in the known Vk-prep set. **")
            print("  ** Either split it, or (if truly benign) add it to the")
            print("     curated classification in check-vulkan-bindings.py. **")
            print("-" * 69)

        if collisions:
            print("  %d collision(s) are Vk-prep debt. Default = report (exit 0)."
                  % len(collisions))
            print("  Run with --strict once the slots are split to gate on this.")
        else:
            print("  No Vulkan shared-binding collisions. Clear for descriptor assignment.")
        print("=" * 69)

    if args.strict and collisions:
        if args.quiet:
            slots = ", ".join(c["slot"] for c in collisions)
            print("check-vulkan-bindings: FAIL (--strict): collisions on %s" % slots,
                  file=sys.stderr)
        return 1

    if new:
        # New (uncurated) collisions are a soft signal even without --strict:
        # they were not classified by the recon. Surface via stderr but do not
        # fail the default report (the aggregator must not break on discovery).
        slots = ", ".join(c["slot"] for c in new)
        print("check-vulkan-bindings: NOTE — new uncurated shared binding(s): %s"
              % slots, file=sys.stderr)

    return 0


if __name__ == "__main__":
    sys.exit(main())
