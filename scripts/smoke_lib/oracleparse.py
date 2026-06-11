# scripts/smoke_lib/oracleparse.py
"""Parse mc2.exe render-oracle + per-pass signals from smoke stdout+stderr.

This is the SHARED render-oracle vocabulary used to turn smoke runs into
comparable results (Baseline A and forward). It is deliberately SEPARATE from
logparse.py: logparse.py feeds the live tier1 fault gate (crash / freeze / perf
floor); this module parses the *render correctness oracles* and per-pass proxy
timings that gate the modernization backlog. Keeping them apart means adding an
oracle here cannot destabilize the tier1 verdict path.

Oracle families parsed (the ones hand-grepped for Baseline A 82add3ca):
  - FASTPATH_DROP        terrain legacy-fallback events (want 0)
  - RENDER_SNAPSHOT v3   static-prop snapshot fallback + mismatches (want 0)
  - TEX_RESOLVE v1       texture handle resolve mismatches / oob (want 0)
  - MECH_MATERIAL_GPU v1 mech material compare mismatches (want 0)
  - OBJBATCHER v1        cpu_fallback + late_register split + submit_legacy
  - GPU_CULL v1          indirect_draw overflow + substrate + submit elapsed_us
  - TerrainLOD           arm path, parity MATCH/MISMATCH, slimVerts, fullyArmed
"""
from __future__ import annotations

import re
from dataclasses import dataclass, field
from typing import List, Optional

# --- regexes (last-occurrence wins for shutdown/summary; counts accumulate) ---
FASTPATH_RE = re.compile(r"\[FASTPATH_DROP\]")
RSNAP_RE = re.compile(
    r"\[RENDER_SNAPSHOT v3\].*?count_mismatch=(?P<cm>\d+) pkt_mismatch=(?P<pm>\d+) "
    r"meta_mismatch=(?P<mm>\d+) fallback=(?P<fb>\d+) using_snapshot=(?P<us>\d+)"
)
TEXRES_RE = re.compile(
    r"\[TEX_RESOLVE v1\] event=shutdown total_frames=(?P<tf>\d+) "
    r"total_resolves=(?P<tr>\d+) mismatches=(?P<mis>\d+) oob=(?P<oob>\d+)"
)
MECHMAT_RE = re.compile(
    r"\[MECH_MATERIAL_GPU v1\] event=compare frame=(?P<f>\d+) "
    r"mechs=(?P<mechs>\d+) mismatches=(?P<mis>\d+)"
)
OBJB_RE = re.compile(
    r"\[OBJBATCHER v1\] event=summary frames=(?P<f>\d+).*?"
    r"cpu_fallback=(?P<cf>\d+).*?fallback_rate=(?P<fr>[\d.]+).*?"
    r"submit_legacy=(?P<sl>\d+).*?late_register_recovery_skips=(?P<lr>\d+)"
)
OBJB_GPU_RE = re.compile(r"gpu_drawn_instances=(?P<g>\d+)")
CULL_IND_RE = re.compile(
    r"\[GPU_CULL v1\] event=indirect_draw buckets=(?P<b>\d+) overflow=(?P<ov>\d+) "
    r"elapsed_us=(?P<us>\d+)"
)
CULL_SUB_RE = re.compile(
    r"\[GPU_CULL v1\] event=substrate_ready records=(?P<rec>\d+) capacity=(?P<cap>\d+)"
)
TERR_ARM_RE = re.compile(
    r"\[TERRAIN_INDIRECT v1\] event=first_arm path=(?P<path>\w+) "
    r"nodeIds=(?P<n>\d+) atlasTex=(?P<atlas>\d+)"
)
TERR_PARITY_RE = re.compile(r"\[TerrainLOD parity\].*?(?P<verdict>MATCH|MISMATCH)\s*$")
TERR_PROD_RE = re.compile(
    r"\[TerrainLOD prod\] frame=(?P<f>\d+) objBlocks=(?P<ob>\d+) "
    r"objVerts=(?P<ov>\d+) solidWindow=(?P<sw>\d+) slimVerts=(?P<sv>\d+)"
)
QUADSKIP_RE = re.compile(r"\[QUADSETUP_SKIP v1\] fullyArmed=(?P<fa>\d+) skip=(?P<sk>\d+)")


@dataclass
class OracleRow:
    # Terrain arm / fastpath
    fastpath_drops: int = 0
    terrain_arm_path: Optional[str] = None   # "gpu" expected; None = never armed
    terrain_nodeids: int = 0
    terrain_atlas: int = 0
    terrain_fully_armed: Optional[bool] = None
    parity_match: int = 0
    parity_mismatch: int = 0
    slim_verts_last: Optional[int] = None     # 0 = chunk-only (slimReduce gone)
    obj_blocks_last: Optional[int] = None
    obj_verts_last: Optional[int] = None
    solid_window_last: Optional[int] = None
    # Static-prop snapshot
    rsnap_fallback: int = 0
    rsnap_count_mismatch: int = 0
    rsnap_pkt_mismatch: int = 0
    rsnap_meta_mismatch: int = 0
    rsnap_using_snapshot: Optional[int] = None
    # Texture resolve
    tex_resolves: int = 0
    tex_mismatches: int = 0
    tex_oob: int = 0
    # Mech material
    mech_mismatches: int = 0
    mech_compares: int = 0
    # ObjBatcher
    objb_cpu_fallback: int = 0
    objb_late_register_skips: int = 0
    objb_submit_legacy: int = 0
    objb_gpu_drawn: int = 0
    objb_fallback_rate: float = 0.0
    # GPU cull
    cull_indirect_overflow: int = 0
    cull_substrate_records_max: int = 0
    cull_substrate_capacity: int = 0
    cull_elapsed_us_max: int = 0


def parse_oracles(text: str) -> OracleRow:
    r = OracleRow()
    for line in text.splitlines():
        if FASTPATH_RE.search(line):
            r.fastpath_drops += 1

        m = RSNAP_RE.search(line)
        if m:
            r.rsnap_count_mismatch = max(r.rsnap_count_mismatch, int(m.group("cm")))
            r.rsnap_pkt_mismatch = max(r.rsnap_pkt_mismatch, int(m.group("pm")))
            r.rsnap_meta_mismatch = max(r.rsnap_meta_mismatch, int(m.group("mm")))
            r.rsnap_fallback = max(r.rsnap_fallback, int(m.group("fb")))
            r.rsnap_using_snapshot = int(m.group("us"))

        m = TEXRES_RE.search(line)
        if m:
            r.tex_resolves = int(m.group("tr"))
            r.tex_mismatches = int(m.group("mis"))
            r.tex_oob = int(m.group("oob"))

        m = MECHMAT_RE.search(line)
        if m:
            r.mech_compares += 1
            r.mech_mismatches = max(r.mech_mismatches, int(m.group("mis")))

        m = OBJB_RE.search(line)
        if m:
            # summary lines accumulate; last (largest frames) is authoritative
            r.objb_cpu_fallback = int(m.group("cf"))
            r.objb_fallback_rate = float(m.group("fr"))
            r.objb_submit_legacy = int(m.group("sl"))
            r.objb_late_register_skips = int(m.group("lr"))
            g = OBJB_GPU_RE.search(line)
            if g:
                r.objb_gpu_drawn = int(g.group("g"))

        m = CULL_IND_RE.search(line)
        if m:
            r.cull_indirect_overflow = max(r.cull_indirect_overflow, int(m.group("ov")))
            r.cull_elapsed_us_max = max(r.cull_elapsed_us_max, int(m.group("us")))

        m = CULL_SUB_RE.search(line)
        if m:
            r.cull_substrate_records_max = max(r.cull_substrate_records_max, int(m.group("rec")))
            r.cull_substrate_capacity = int(m.group("cap"))

        m = TERR_ARM_RE.search(line)
        if m:
            r.terrain_arm_path = m.group("path")
            r.terrain_nodeids = int(m.group("n"))
            r.terrain_atlas = int(m.group("atlas"))

        m = TERR_PARITY_RE.search(line)
        if m:
            if m.group("verdict") == "MATCH":
                r.parity_match += 1
            else:
                r.parity_mismatch += 1

        m = TERR_PROD_RE.search(line)
        if m:
            r.slim_verts_last = int(m.group("sv"))
            r.obj_blocks_last = int(m.group("ob"))
            r.obj_verts_last = int(m.group("ov"))
            r.solid_window_last = int(m.group("sw"))

        m = QUADSKIP_RE.search(line)
        if m:
            r.terrain_fully_armed = (m.group("fa") == "1")

    return r


# --- budget judgement ----------------------------------------------------------

@dataclass
class OracleVerdict:
    passed: bool
    fails: List[str] = field(default_factory=list)
    warns: List[str] = field(default_factory=list)


def judge_oracles(r: OracleRow, *, allow_late_register: bool = True) -> OracleVerdict:
    """Hard correctness asserts. These must stay 0/clean post-8z.

    OBJBATCHER cpu_fallback is NOT a hard fail when it equals
    late_register_recovery_skips (HUD nodes like `compass` registering after the
    batch window) -- that is a classified-benign non-zero (see Baseline A). It is
    a hard fail only when cpu_fallback exceeds the late-register skips, i.e. real
    geometry fell back to the CPU path.
    """
    fails: List[str] = []
    warns: List[str] = []

    if r.fastpath_drops != 0:
        fails.append(f"FASTPATH_DROP={r.fastpath_drops} (terrain dropped to legacy)")
    if r.terrain_arm_path != "gpu":
        fails.append(f"terrain_arm_path={r.terrain_arm_path} (expected gpu)")
    if r.parity_mismatch != 0:
        fails.append(f"TerrainLOD parity MISMATCH={r.parity_mismatch}")
    if r.parity_match == 0:
        warns.append("no TerrainLOD parity MATCH observed (probe absent)")
    if r.slim_verts_last not in (0, None):
        fails.append(f"slimVerts={r.slim_verts_last} (expected 0 -- legacy slimReduce alive?)")
    if r.terrain_fully_armed is False:
        fails.append("QUADSETUP_SKIP fullyArmed=0")

    if r.rsnap_fallback != 0:
        fails.append(f"RENDER_SNAPSHOT fallback={r.rsnap_fallback}")
    for nm, v in (("count", r.rsnap_count_mismatch), ("pkt", r.rsnap_pkt_mismatch),
                  ("meta", r.rsnap_meta_mismatch)):
        if v != 0:
            fails.append(f"RENDER_SNAPSHOT {nm}_mismatch={v}")

    if r.tex_mismatches != 0:
        fails.append(f"TEX_RESOLVE mismatches={r.tex_mismatches}")
    if r.tex_oob != 0:
        fails.append(f"TEX_RESOLVE oob={r.tex_oob}")

    if r.mech_mismatches != 0:
        fails.append(f"MECH_MATERIAL_GPU mismatches={r.mech_mismatches}")

    if r.objb_submit_legacy != 0:
        fails.append(f"OBJBATCHER submit_legacy={r.objb_submit_legacy}")
    real_cpu_fallback = r.objb_cpu_fallback - r.objb_late_register_skips
    if real_cpu_fallback > 0:
        fails.append(f"OBJBATCHER real cpu_fallback={real_cpu_fallback} "
                     f"(cpu_fallback={r.objb_cpu_fallback} > late_register={r.objb_late_register_skips})")
    elif r.objb_cpu_fallback > 0:
        msg = (f"OBJBATCHER cpu_fallback={r.objb_cpu_fallback} "
               f"== late_register_recovery_skips (classified benign)")
        (warns if allow_late_register else fails).append(msg)

    if r.cull_indirect_overflow != 0:
        fails.append(f"GPU_CULL indirect overflow={r.cull_indirect_overflow}")

    return OracleVerdict(passed=not fails, fails=fails, warns=warns)
