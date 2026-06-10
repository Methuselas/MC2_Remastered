# tools/terrain_gen/foliage_generator.py
"""PCG Lite foliage generation (Phase 4).

Deterministic tree/rock/bush instance placement from terrain masks, grouped by
superchunk. Output is {name}.foliage.json -- a sidecar consumed later by the
editor (Phase 5). This module never touches the height/burnin/elev outputs, so
the generate->save->load contract is unaffected.

Determinism: each superchunk is seeded by
    seed = recipe.seed ^ (scx * 73856093) ^ (scy * 19349663)
so the same recipe+rules always yields identical JSON, and a single superchunk
can be regenerated in isolation (editor "Regenerate Selected Superchunk").
"""
from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

from terrain_gen.material_classifier import TerrainMasks, TerrainType

CHUNK_SIZE = 20
WORLD_UNITS_PER_VERTEX = 128.0


@dataclass
class FoliageRule:
    asset:           str   = "tree_pine_a"
    kind:            str   = "tree"
    density:         float = 0.01     # candidates per vertex within a superchunk
    min_altitude:    float = 0.0
    max_altitude:    float = 1.0
    max_slope:       float = 1.0
    avoid_water:     bool  = True
    terrain_types:   list  = field(default_factory=list)   # [] = any
    noise_threshold: float = 0.0
    min_spacing:     float = 256.0    # world units between instances
    scale_min:       float = 0.9
    scale_max:       float = 1.1

    @classmethod
    def from_dict(cls, d: dict) -> "FoliageRule":
        known = {f for f in cls.__dataclass_fields__}
        return cls(**{k: v for k, v in d.items() if k in known})


def _resolve_terrain_types(names: list) -> set:
    """Map rule terrain_type names (e.g. "GRASS") to TerrainType int values.
    Unknown names are ignored with a warning."""
    out = set()
    for n in names:
        if isinstance(n, int):
            out.add(int(n)); continue
        try:
            out.add(int(TerrainType[str(n).upper()]))
        except KeyError:
            print(f"WARNING: unknown terrain_type '{n}' in foliage rule", flush=True)
    return out


class FoliageGenerator:
    def generate(self, height: np.ndarray, masks: TerrainMasks, recipe,
                 rules: list, superchunk_chunks: int = 3, progress=None) -> dict:
        """Return {"version":1,"instances":[...]} placed deterministically per
        superchunk. rules = list of FoliageRule (or dicts)."""
        rules = [r if isinstance(r, FoliageRule) else FoliageRule.from_dict(r) for r in rules]
        N = recipe.size
        sc_side = CHUNK_SIZE * superchunk_chunks          # vertices per superchunk edge
        n_sc = (N + sc_side - 1) // sc_side                # superchunks per axis
        water_level = float(recipe.materials.water_level)
        max_elev = float(recipe.height.max_elevation)
        min_elev = float(recipe.height.min_elevation)
        half = N * 0.5

        # Pre-resolve terrain-type filters per rule.
        rule_types = [_resolve_terrain_types(r.terrain_types) for r in rules]

        instances = []
        total_sc = n_sc * n_sc
        done = 0
        for scy in range(n_sc):
            for scx in range(n_sc):
                seed = (recipe.seed ^ (scx * 73856093) ^ (scy * 19349663)) & 0xFFFFFFFF
                rng = np.random.default_rng(seed)
                y0, y1 = scy * sc_side, min((scy + 1) * sc_side, N)
                x0, x1 = scx * sc_side, min((scx + 1) * sc_side, N)
                cell_w, cell_h = x1 - x0, y1 - y0
                if cell_w <= 0 or cell_h <= 0:
                    continue

                accepted_xy = []  # vertex coords, for spacing within this superchunk
                for r, allowed in zip(rules, rule_types):
                    if r.density <= 0.0:
                        continue
                    n_cand = int(round(r.density * cell_w * cell_h))
                    if n_cand <= 0:
                        continue
                    spacing_v = max(0.0, r.min_spacing / WORLD_UNITS_PER_VERTEX)
                    sp2 = spacing_v * spacing_v
                    # Deterministic candidate stream for this rule.
                    cxs = x0 + rng.random(n_cand) * cell_w
                    cys = y0 + rng.random(n_cand) * cell_h
                    rots = rng.random(n_cand) * 360.0
                    scales = r.scale_min + rng.random(n_cand) * (r.scale_max - r.scale_min)
                    for cx, cy, rot, sc in zip(cxs, cys, rots, scales):
                        ix, iy = int(cx), int(cy)
                        if ix >= N or iy >= N:
                            continue
                        alt = float(masks.altitude[iy, ix])
                        if alt < r.min_altitude or alt > r.max_altitude:
                            continue
                        if float(masks.slope[iy, ix]) > r.max_slope:
                            continue
                        if r.avoid_water and alt <= water_level:
                            continue
                        if allowed and int(masks.terrain_type[iy, ix]) not in allowed:
                            continue
                        if r.noise_threshold > 0.0 and float(masks.noise[iy, ix]) < r.noise_threshold:
                            continue
                        # Min-spacing reject (per superchunk, simple O(n^2)).
                        if sp2 > 0.0 and any((cx - ax) ** 2 + (cy - ay) ** 2 < sp2 for ax, ay in accepted_xy):
                            continue
                        accepted_xy.append((cx, cy))
                        wz = float(height[iy, ix]) * max_elev + min_elev
                        instances.append({
                            "asset": r.asset,
                            "kind": r.kind,
                            "x": round((cx - half) * WORLD_UNITS_PER_VERTEX, 3),
                            "y": round((half - cy) * WORLD_UNITS_PER_VERTEX, 3),
                            "z": round(wz, 3),
                            "rotation": round(float(rot), 2),
                            "scale": round(float(sc), 4),
                            "superchunk": [scx, scy],
                        })
                done += 1
                if progress and (done % max(1, total_sc // 10) == 0):
                    pct = 92 + int(6 * done / total_sc)
                    progress(pct, "foliage", f"superchunk {done}/{total_sc} ({len(instances)} placed)")

        return {"version": 1, "superchunk_side": sc_side, "instances": instances}


def load_rules(path: str) -> list:
    """Load foliage rules from a JSON file. Accepts either a bare list or
    {"foliage": [...]}."""
    with open(path) as f:
        d = json.load(f)
    return d.get("foliage", d) if isinstance(d, dict) else d


def write_foliage(result: dict, path: Path | str) -> None:
    with open(path, "w") as f:
        json.dump(result, f, indent=1)
