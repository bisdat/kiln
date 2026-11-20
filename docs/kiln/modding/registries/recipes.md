# The `recipes` registry

<!-- Generated from schema/registry/recipes.json by tools/registrygen/registrygen.py. Do not edit. -->

Every crafting recipe: what it takes, what it makes and how long it takes.

Schema version 1.0.0, experimental (KILNX031: may change in any release until it is marked stable). C# entry type `Kiln.Registry.RecipeEntry`. What registries are, the stages, the conflict rules and how ids are made: [README](README.md).

## Ids

- Game entries: `game:<stable name>`. Mod entries: `<mod id>:<name>`. All ids are lower case. The mod ids `game` and `kiln` are reserved: no mod's entries use them.
- The stable name starts as the recipe's debug name in lower case. A recipe without a debug name is game:recipe_<its 32-bit recipe id as 8 hex digits> (the name Kiln's content selectors already give it). Recipes and items are separate registries: a recipe may have the same id as the item it makes. When two recipes would get the same name, the one that held it first keeps it and the other is game:<name>~<first 8 hex digits of its GUID>.

## Fields

Applies: `now` = takes effect at the next use; `worldLoad` = takes effect when a world loads; `restart` = takes effect after a game restart. Ops: `number` = a value or the relative operations (mul, add, min, max, raiseOnly, lowerOnly); `set` = a value; `list` = clear, append, remove, replace. A field of a list's elements is named with [] in this table and with the element's index in an edit (`entries[3].chance`, or [*] for every element).

| Field | Type | Applies | Ops | Since | About |
|---|---|---|---|---|---|
| `inputs` | list of entries, at most 32 | now | list | 1.0.0 | What one craft takes: an item or an input category, and a count. |
| `inputs[].item` | ref items | now | set | 1.0.0 | The item (or category). |
| `inputs[].category` | ref inputCategories | now | set | 1.0.0 | An input category (any item of it); instead of item. |
| `inputs[].count` | int 1..65535 | now | number | 1.0.0 |  |
| `outputs` | list of entries, at most 32 | now | list | 1.0.0 | What one craft makes. |
| `outputs[].item` | ref items | now | set | 1.0.0 |  |
| `outputs[].count` | int 1..65535 | now | number | 1.0.0 |  |
| `duration` | time s 0..86400 | now | number | 1.0.0 | How long a station takes for one craft. |
| `unlock` | enum (game, always) | now | set | 1.0.0 | When players see the recipe: game = the game's own knowledge requirement, always = from the start. Only the crafting menu reads it (a client without Kiln keeps the game's). Read by game clients only. |
| `station` | ref stations | restart | read-only | 1.0.0 | The workshop the recipe belongs to. Read-only for game recipes: the game sorts recipes into stations once at start. |

## Traits

Selectors ask for these with `has` / `hasNot`.

| Trait | Means |
|---|---|
| `cooking` | a cooking recipe |
| `upgrade` | an upgrade recipe |

## Entries

- Add (stage add, id `<your mod id>:<name>`): yes.
- Remove (hidden): A removed recipe is taken out of every station's list.
- Raw game paths in edits (the Advanced tier): allowed.
