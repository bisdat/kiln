# The `balance` registry

<!-- Generated from schema/registry/balance.json by tools/registrygen/registrygen.py. Do not edit. -->

The game's balance table: experience and levels, the flame's altars and water sources, base fog resistance, durability, sockets and perk unlock costs. The game reads it once at start.

Schema version 1.0.0, experimental (KILNX031: may change in any release until it is marked stable). C# entry type `Kiln.Registry.BalanceEntry`. What registries are, the stages, the conflict rules and how ids are made: [README](README.md).

## Ids

- Game entries: `game:balance`. Mod entries: `(none: one entry)`. All ids are lower case. The mod ids `game` and `kiln` are reserved: no mod's entries use them.
- One entry, game:balance.

## Fields

Applies: `now` = takes effect at the next use; `worldLoad` = takes effect when a world loads; `restart` = takes effect after a game restart. Ops: `number` = a value or the relative operations (mul, add, min, max, raiseOnly, lowerOnly); `set` = a value; `list` = clear, append, remove, replace. A field of a list's elements is named with [] in this table and with the element's index in an edit (`entries[3].chance`, or [*] for every element).

| Field | Type | Applies | Ops | Since | About |
|---|---|---|---|---|---|
| `xpPerLevel` | list of int 0..100000000, at most 5 | restart | list | 1.0.0 | The experience levels 1 to 5 need (later levels follow xpCurve). Exactly 5 values; set one with xpPerLevel[i]. |
| `xpCurve.start` | int 0..100000000 | restart | number | 1.0.0 |  |
| `xpCurve.end` | int 0..100000000 | restart | number | 1.0.0 |  |
| `xpCurve.levelMax` | int 1..100000 | restart | number | 1.0.0 |  |
| `xpCurve.seed` | int 0..100000000 | restart | number | 1.0.0 |  |
| `xpCurve.fromKnowledge` | int 0..100000000 | restart | number | 1.0.0 |  |
| `levelCap` | int 1..1000 | restart | number | 1.0.0 | The highest player level. |
| `skillPointsPerLevel` | int 0..1000 | restart | number | 1.0.0 | Skill points a level gives. |
| `flameLevelMax` | int 0..1000 | restart | number | 1.0.0 | The highest flame level. The per-flame-level lists need flameLevelMax + 1 values. |
| `altarsPerFlameLevel` | list of int 0..16, at most 1001 | restart | list | 1.0.0 | Flame altars allowed at each flame level (the game's code stops at 16). |
| `waterSourcesPerFlameLevel` | list of int 0..255, at most 1001 | restart | list | 1.0.0 | Water sources allowed at each flame level. |
| `buildzoneSizesPerAltarLevel` | list of entries, at most 1001 | restart | list | 1.0.0 | The size of an altar's build zone at each altar level, in metres. |
| `buildzoneSizesPerAltarLevel[].x` | float m 0..100000 | restart | number | 1.0.0 |  |
| `buildzoneSizesPerAltarLevel[].y` | float m 0..100000 | restart | number | 1.0.0 |  |
| `buildzoneSizesPerAltarLevel[].z` | float m 0..100000 | restart | number | 1.0.0 |  |
| `fogResistance.base` | int 0..100000 | restart | number | 1.0.0 |  |
| `fogResistance.perFlameLevel` | int 0..100000 | restart | number | 1.0.0 |  |
| `durability.start` | float 0..1000000 | restart | number | 1.0.0 |  |
| `durability.growth` | float 0..1000000 | restart | number | 1.0.0 |  |
| `rarityDurability.common` | float 0..100 | restart | number | 1.0.0 |  |
| `rarityDurability.uncommon` | float 0..100 | restart | number | 1.0.0 |  |
| `rarityDurability.rare` | float 0..100 | restart | number | 1.0.0 |  |
| `rarityDurability.epic` | float 0..100 | restart | number | 1.0.0 |  |
| `rarityDurability.legendary` | float 0..100 | restart | number | 1.0.0 |  |
| `socketChance.common` | float 0..1 | restart | number | 1.0.0 |  |
| `socketChance.uncommon` | float 0..1 | restart | number | 1.0.0 |  |
| `socketChance.rare` | float 0..1 | restart | number | 1.0.0 |  |
| `socketChance.epic` | float 0..1 | restart | number | 1.0.0 |  |
| `socketChance.legendary` | float 0..1 | restart | number | 1.0.0 |  |
| `unlockCost.seed` | int 0..100000 | restart | number | 1.0.0 |  |
| `unlockCost.creep` | float 0..100 | restart | number | 1.0.0 |  |
| `unlockCost.weapons` | float 0..100 | restart | number | 1.0.0 |  |
| `unlockCost.armor` | float 0..100 | restart | number | 1.0.0 |  |
| `unlockCost.rings` | float 0..100 | restart | number | 1.0.0 |  |
| `unlockCost.advanced` | float 0..100000 | restart | number | 1.0.0 |  |
| `unlockCost.advancedFrom` | int 0..5 | restart | number | 1.0.0 |  |
| `gear.seed` | float 0..100000 | restart | read-only | 1.0.0 |  |
| `gear.creep` | float 0..100 | restart | read-only | 1.0.0 |  |
| `gear.armorOffset` | float 0..100000 | restart | read-only | 1.0.0 |  |
| `gear.armorGradient` | float 0..100000 | restart | read-only | 1.0.0 |  |

## Entries

- Add (stage add, id `<your mod id>:<name>`): no.
- Remove: no.
- Raw game paths in edits (the Advanced tier): allowed.
