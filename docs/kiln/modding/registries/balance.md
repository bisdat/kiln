# The `balance` registry

<!-- Generated from schema/registry/balance.json by tools/registrygen/registrygen.py. Do not edit. -->

The game's balance table: experience and levels, the flame's altars and water sources, base fog resistance, durability, sockets and perk unlock costs. The game reads it once at start.

Schema version 0.1.0, experimental (KILNX031: may change in any release until it is marked stable). C# entry type `Kiln.Registry.BalanceEntry`. What registries are, the stages, the conflict rules and how ids are made: [README](README.md).

## Ids

- Game entries: `game:balance`. Mod entries: `(none: one entry)`. All ids are lower case. The mod ids `game` and `kiln` are reserved: no mod's entries use them.
- One entry, game:balance.

## Fields

Applies: `now` = takes effect at the next use; `worldLoad` = takes effect when a world loads; `restart` = takes effect after a game restart. Ops: `number` = a value or the relative operations (mul, add, min, max, raiseOnly, lowerOnly); `set` = a value; `list` = clear, append, remove, replace; `indexed` = a list of fixed length, edited one element at a time (`xpPerLevel[2]`) with a value or a number operation. A field of a list's elements is named with [] in this table and with the element's index in an edit (`entries[3].chance`, or [*] for every element). Mapping: whether this Kiln maps the field onto the game; an unproven or unmapped field is refused per entry, with the reason, until a Kiln update maps it.

Groups of fields:

- `xpCurve`: The experience a level from 6 needs: base = min(start, end) + |end - start| * level / levelMax; need = round(base * seed * 1.5 + fromKnowledge).
- `fogResistance`: A player's shroud time.
- `durability`: An item's maximum durability: (start + growth * (level - 1)) * rarityDurability[rarity]. Each copy computes it when it is loaded.
- `rarityDurability`: The durability factor of each rarity.
- `socketChance`: The chance a new weapon of each rarity gets a gem socket (0 to 1).
- `unlockCost`: What unlocking a perk costs.
- `gear`: The constants of the power and armour formulas (items.power, items.armor). Read-only in this version: Kiln's solver follows them.

| Field | Type | Applies | Ops | Since | Mapping | About |
|---|---|---|---|---|---|---|
| `xpPerLevel` | list of int 0..100000000, exactly 5 | restart | indexed | 0.1.0 | mapped | The experience levels 1 to 5 need (later levels follow xpCurve). Exactly 5 values, edited one at a time (xpPerLevel[i]). |
| `xpCurve.start` | int 0..100000000 | restart | number | 0.1.0 | mapped |  |
| `xpCurve.end` | int 0..100000000 | restart | number | 0.1.0 | mapped |  |
| `xpCurve.levelMax` | int 1..100000 | restart | number | 0.1.0 | mapped |  |
| `xpCurve.seed` | int 0..100000000 | restart | number | 0.1.0 | mapped |  |
| `xpCurve.fromKnowledge` | int 0..100000000 | restart | number | 0.1.0 | mapped |  |
| `levelCap` | int 1..1000 | restart | number | 0.1.0 | mapped | The highest player level. |
| `skillPointsPerLevel` | int 0..1000 | restart | number | 0.1.0 | mapped | Skill points a level gives. |
| `flameLevelMax` | int 0..1000 | restart | read-only | 0.1.0 | mapped | The highest flame level. Read-only in this version: the per-flame-level lists are exactly flameLevelMax + 1 long. |
| `altarsPerFlameLevel` | list of int 0..16, as long as flameLevelMax + 1 | restart | indexed | 0.1.0 | mapped | Flame altars allowed at each flame level, one value per level from 0 to flameLevelMax (the game's code stops at 16). Edited one level at a time (altarsPerFlameLevel[i]). |
| `waterSourcesPerFlameLevel` | list of int 0..255, as long as flameLevelMax + 1 | restart | indexed | 0.1.0 | mapped | Water sources allowed at each flame level, one value per level from 0 to flameLevelMax. Edited one level at a time. |
| `buildzoneSizesPerAltarLevel` | list of entries, at most 1001 | restart | list | 0.1.0 | mapped | The size of an altar's build zone at each altar level, in metres. |
| `buildzoneSizesPerAltarLevel[].x` | float m 0..100000 | restart | number | 0.1.0 | mapped |  |
| `buildzoneSizesPerAltarLevel[].y` | float m 0..100000 | restart | number | 0.1.0 | mapped |  |
| `buildzoneSizesPerAltarLevel[].z` | float m 0..100000 | restart | number | 0.1.0 | mapped |  |
| `fogResistance.base` | int 0..100000 | restart | number | 0.1.0 | mapped |  |
| `fogResistance.perFlameLevel` | int 0..100000 | restart | number | 0.1.0 | mapped |  |
| `durability.start` | float 0..1000000 | restart | number | 0.1.0 | mapped |  |
| `durability.growth` | float 0..1000000 | restart | number | 0.1.0 | mapped |  |
| `rarityDurability.common` | float 0..100 | restart | number | 0.1.0 | mapped |  |
| `rarityDurability.uncommon` | float 0..100 | restart | number | 0.1.0 | mapped |  |
| `rarityDurability.rare` | float 0..100 | restart | number | 0.1.0 | mapped |  |
| `rarityDurability.epic` | float 0..100 | restart | number | 0.1.0 | mapped |  |
| `rarityDurability.legendary` | float 0..100 | restart | number | 0.1.0 | mapped |  |
| `socketChance.common` | float 0..1 | restart | number | 0.1.0 | mapped |  |
| `socketChance.uncommon` | float 0..1 | restart | number | 0.1.0 | mapped |  |
| `socketChance.rare` | float 0..1 | restart | number | 0.1.0 | mapped |  |
| `socketChance.epic` | float 0..1 | restart | number | 0.1.0 | mapped |  |
| `socketChance.legendary` | float 0..1 | restart | number | 0.1.0 | mapped |  |
| `unlockCost.seed` | int 0..100000 | restart | number | 0.1.0 | mapped |  |
| `unlockCost.creep` | float 0..100 | restart | number | 0.1.0 | mapped |  |
| `unlockCost.weapons` | float 0..100 | restart | number | 0.1.0 | mapped |  |
| `unlockCost.armor` | float 0..100 | restart | number | 0.1.0 | mapped |  |
| `unlockCost.rings` | float 0..100 | restart | number | 0.1.0 | mapped |  |
| `unlockCost.advanced` | float 0..100000 | restart | number | 0.1.0 | mapped |  |
| `unlockCost.advancedFrom` | int 0..5 | restart | number | 0.1.0 | mapped |  |
| `gear.seed` | float 0..100000 | restart | read-only | 0.1.0 | mapped |  |
| `gear.creep` | float 0..100 | restart | read-only | 0.1.0 | mapped |  |
| `gear.armorOffset` | float 0..100000 | restart | read-only | 0.1.0 | mapped |  |
| `gear.armorGradient` | float 0..100000 | restart | read-only | 0.1.0 | mapped |  |

## Entries

- Add (stage add, id `<your mod id>:<name>`): no.
- Remove: no.
- Raw game paths in edits (the Advanced tier): allowed.
