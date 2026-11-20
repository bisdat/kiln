# The `loot` registry

<!-- Generated from schema/registry/loot.json by tools/registrygen/registrygen.py. Do not edit. -->

How an item drops as random loot: its stack size and its rarities. One entry per item that can drop.

Schema version 1.0.0, experimental (KILNX031: may change in any release until it is marked stable). C# entry type `Kiln.Registry.ItemLootEntry`. What registries are, the stages, the conflict rules and how ids are made: [README](README.md).

## Ids

- Game entries: `game:<the item's stable name>`. Mod entries: `<mod id>:<the item's name>`. All ids are lower case. The mod ids `game` and `kiln` are reserved: no mod's entries use them.
- A loot entry has the id of its item (items registry). An entry whose item has no name is game:item_<its 32-bit item id as 8 hex digits>.

## Fields

Applies: `now` = takes effect at the next use; `worldLoad` = takes effect when a world loads; `restart` = takes effect after a game restart. Ops: `number` = a value or the relative operations (mul, add, min, max, raiseOnly, lowerOnly); `set` = a value; `list` = clear, append, remove, replace. A field of a list's elements is named with [] in this table and with the element's index in an edit (`entries[3].chance`, or [*] for every element).

| Field | Type | Applies | Ops | Since | About |
|---|---|---|---|---|---|
| `stack.min` | int 0..65535 | now | number | 1.0.0 |  |
| `stack.max` | int 0..65535 | now | number | 1.0.0 |  |
| `stack.scalable` | bool | now | set | 1.0.0 | The stack grows with the world's loot amount, up to maxScaled. |
| `stack.maxScaled` | int 0..65535 | now | number | 1.0.0 |  |
| `rarities` | flags (common, uncommon, rare, epic, legendary) | now | set | 1.0.0 | The rarities a drop may roll. |
| `fixedRarity` | enum (none, common, uncommon, rare, epic, legendary) | now | set | 1.0.0 | A rarity every drop gets (none: rolled from rarities). |

## Entries

- Add (stage add, id `<your mod id>:<name>`): no.
- Remove (disabled): A removed loot entry never drops as random loot (the item itself stays).
- Raw game paths in edits (the Advanced tier): allowed.
