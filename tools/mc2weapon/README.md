# mc2weapon

MechCommander 2 weapon viewer / modder tool. Reads every component from
`compbas.csv`, resolves each weapon's FX binding via `effects.csv`, and presents a
unified weapon catalog. Editor commands (edit stats/FX → loose mod overlay, create new
weapons) are specced in [docs/weapon-tool-design.md](../../docs/weapon-tool-design.md).

Python 3, stdlib only. No build, no engine. Weapon stats are CSV text — edits ship as a
loose `mods/<id>/data/objects/compbas.csv` overlay that the game resolves first (no
`.pak` repack needed).

## Usage

```sh
# List all stock weapons with stats + resolved FX (auto-detects the CSVs)
py -3 mc2weapon.py list

# Include non-weapon components; or emit JSON
py -3 mc2weapon.py list --all
py -3 mc2weapon.py list --json

# One weapon's full stats + FX (by masterID or name)
py -3 mc2weapon.py show 145            # PPC
py -3 mc2weapon.py show "Gauss Rifle"
py -3 mc2weapon.py show 104 --json

# Point at specific CSVs (otherwise auto-detected from source/deploy/mods)
py -3 mc2weapon.py --compbas path/to/compbas.csv --effects path/to/effects.csv list
```

## What it shows

- **Stats** (from `compbas.csv`): damage, heat, recycle (cooldown s), range bracket
  (short/medium/long), tonnage, slots (crit-hits), BR, RP, missile type, fields.
- **FX** (from `compbas` Special FX ID → `effects.csv` row): trail / muzzle-flash /
  hit / miss effect names + object number.

## Notes / reality-checks

- **masterID = the weapon's identity** (its row index in compbas.csv). Mech loadouts
  reference weapons by masterID.
- **Range in meters** is global (`gamesys.fit [WeaponRanges]` × the per-weapon
  short/med/long tag); compbas only stores the bracket.
- **Weapons have no 3D mesh.** A weapon's visual is a procedural bolt (texture + gosFX
  particles), so "model" = bolt texture + FX, not a glb upload. (Glb model upload is a
  mech/prop feature via the model-override registry, a separate subsystem.)
