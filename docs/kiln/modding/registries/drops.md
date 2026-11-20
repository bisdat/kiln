# The `drops` registry

<!-- Generated from schema/registry/drops.json by tools/registrygen/registrygen.py. Do not edit. -->

The game's drop tables (default inventories): what a container, an enemy or a plant gives, with counts and chances.

Schema version 1.0.0, experimental (KILNX031: may change in any release until it is marked stable). C# entry type `Kiln.Registry.DropTableEntry`. What registries are, the stages, the conflict rules and how ids are made: [README](README.md).

## Ids

- Game entries: `game:<stable name>`. Mod entries: `<mod id>:<name>`. All ids are lower case. The mod ids `game` and `kiln` are reserved: no mod's entries use them.
- Drop tables have no names in the game. A table that templates use is named after the first of them in name order, in lower case (game:lootcontainer_enemy_scavenger_melee01); a table no template uses is game:drops_<first 8 hex digits of its GUID>. When two tables would get the same name, the one that held it first keeps it and the other is game:<name>~<first 8 hex digits of its GUID>. A table's GUID also selects it (as data packs' drops section does today).

## Fields

Applies: `now` = takes effect at the next use; `worldLoad` = takes effect when a world loads; `restart` = takes effect after a game restart. Ops: `number` = a value or the relative operations (mul, add, min, max, raiseOnly, lowerOnly); `set` = a value; `list` = clear, append, remove, replace. A field of a list's elements is named with [] in this table and with the element's index in an edit (`entries[3].chance`, or [*] for every element).

| Field | Type | Applies | Ops | Since | About |
|---|---|---|---|---|---|
| `entries` | list of entries, at most 4096 | now | list | 1.0.0 | The table's stacks in walk order (root group first, then its groups). Index i in a field path (entries[3].chance) is that order. |
| `entries[].item` | ref items | now | read-only | 1.0.0 | The item. Given when an entry is appended; read-only afterwards. |
| `entries[].count.min` | int 0..1000000 | now | number | 1.0.0 |  |
| `entries[].count.max` | int 0..1000000 | now | number | 1.0.0 |  |
| `entries[].chance` | float 0..1 | now | number | 1.0.0 | The chance the entry drops. |
| `entries[].weight` | float 0..1000000 | now | number | 1.0.0 | The weight of the entry in a group that picks one. |

## Entries

- Add (stage add, id `<your mod id>:<name>`): no.
- Remove: no.
- Raw game paths in edits (the Advanced tier): allowed.
