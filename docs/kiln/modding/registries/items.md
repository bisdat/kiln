# The `items` registry

<!-- Generated from schema/registry/items.json by tools/registrygen/registrygen.py. Do not edit. -->

Every item the game or a mod defines: stack sizes, rarity, levels, the gear numbers a tooltip shows (power, speed, damage, armour), costs, perks and sockets.

Schema version 1.0.0, experimental (KILNX031: may change in any release until it is marked stable). C# entry type `Kiln.Registry.ItemEntry`. What registries are, the stages, the conflict rules and how ids are made: [README](README.md).

## Ids

- Game entries: `game:<stable name>`. Mod entries: `<mod id>:<name>`. All ids are lower case. The mod ids `game` and `kiln` are reserved: no mod's entries use them.
- The stable name starts as the item's debug name in lower case (Material_T1_Wood_Logs is game:material_t1_wood_logs). An item without a debug name is game:item_<its 32-bit item id as 8 hex digits>. When two items would get the same name, the one that held it first keeps it and the other is game:<name>~<first 8 hex digits of its GUID>.

## Fields

Applies: `now` = takes effect at the next use; `worldLoad` = takes effect when a world loads; `restart` = takes effect after a game restart. Ops: `number` = a value or the relative operations (mul, add, min, max, raiseOnly, lowerOnly); `set` = a value; `list` = clear, append, remove, replace. A field of a list's elements is named with [] in this table and with the element's index in an edit (`entries[3].chance`, or [*] for every element).

| Field | Type | Applies | Ops | Since | About |
|---|---|---|---|---|---|
| `maxStack` | int 1..65535 | worldLoad | number | 1.0.0 | The largest stack. In place before the world's save is read: a lower value cuts stacks in that save. |
| `rarity` | enum (common, uncommon, rare, epic, legendary) | now | set | 1.0.0 | The rarity a new copy gets when it does not roll one. Copies that exist keep theirs. |
| `rolled` | bool | now | set | 1.0.0 | New copies roll their rarity (and perks) instead of taking rarity. Copies that exist keep theirs. |
| `levels.min` | int 1..105 | now | number | 1.0.0 | The lowest level (also the level power and armour are shown at). |
| `levels.max` | int 1..105 | now | number | 1.0.0 | The highest level. |
| `socket` | bool | now | set | 1.0.0 | New copies get a gem socket. |
| `perks` | list of ref perks, at most 5 | now | list | 1.0.0 | The perk of each rarity slot, common first. A copy fills rarity + 1 slots. New copies only. |
| `power` | int 0..100000 | now | number | 1.0.0 | The damage the tooltip shows at levels.min. Kiln solves the game's damage factor for it with the balance table's constants. Formula: `gear.power`. |
| `speed` | float 0.01..100 | now | number | 1.0.0 | The speed factor of the weapon's damage (multiplies power). |
| `hasDamage` | bool | now | read-only | 1.0.0 | The item deals damage (power, speed and damage apply). |
| `damage.cut` | float 0..10 | now | number | 1.0.0 |  |
| `damage.pierce` | float 0..10 | now | number | 1.0.0 |  |
| `damage.blunt` | float 0..10 | now | number | 1.0.0 |  |
| `damage.fire` | float 0..10 | now | number | 1.0.0 |  |
| `damage.ice` | float 0..10 | now | number | 1.0.0 |  |
| `damage.shroud` | float 0..10 | now | number | 1.0.0 |  |
| `damage.lightning` | float 0..10 | now | number | 1.0.0 |  |
| `damage.poison` | float 0..10 | now | number | 1.0.0 |  |
| `damage.healing` | float 0..10 | now | number | 1.0.0 |  |
| `damage.wood` | int 0..1000000 | now | number | 1.0.0 |  |
| `damage.stone` | int 0..1000000 | now | number | 1.0.0 |  |
| `damage.metal` | int 0..1000000 | now | number | 1.0.0 |  |
| `damage.explosion` | int 0..1000000 | now | number | 1.0.0 |  |
| `hasArmor` | bool | now | read-only | 1.0.0 | The item gives armour (armor applies). |
| `armor.physical` | int 0..100000 | now | number | 1.0.0 | Formula: `gear.armor`. |
| `armor.magical` | int 0..100000 | now | number | 1.0.0 | Formula: `gear.armor`. |
| `armor.cut` | int 0..100000 | now | number | 1.0.0 | Formula: `gear.armor`. |
| `armor.pierce` | int 0..100000 | now | number | 1.0.0 | Formula: `gear.armor`. |
| `armor.blunt` | int 0..100000 | now | number | 1.0.0 | Formula: `gear.armor`. |
| `armor.fire` | int 0..100000 | now | number | 1.0.0 | Formula: `gear.armor`. |
| `armor.ice` | int 0..100000 | now | number | 1.0.0 | Formula: `gear.armor`. |
| `armor.shroud` | int 0..100000 | now | number | 1.0.0 | Formula: `gear.armor`. |
| `armor.lightning` | int 0..100000 | now | number | 1.0.0 | Formula: `gear.armor`. |
| `block` | float 0..1 | now | number | 1.0.0 | How well the item blocks (0 to 1). |
| `staminaCost` | float 0..100000 | now | number | 1.0.0 | The stamina a use costs, at the next payment. |
| `manaCost` | float 0..100000 | now | number | 1.0.0 | The mana a use costs, at the next cast. |
| `fuel.burnTime` | time s 0..86400 | worldLoad | number | 1.0.0 | How long one item burns. |
| `comfort` | int 0..1000 | worldLoad | number | 1.0.0 | The comfort the item gives when placed. When the game reads it is not traced yet: unproven until a Kiln update maps it. |

## Traits

Selectors ask for these with `has` / `hasNot`.

| Trait | Means |
|---|---|
| `weapon` | deals damage (hasDamage) |
| `armor` | gives armour (hasArmor) |
| `stackable` | maxStack above 1 |

## Entries

- Add (stage add, id `<your mod id>:<name>`): yes.
- Remove (hidden): A removed item stays in the game's tables (saves keep their copies) but is hidden from loot, drops and recipes.
- Raw game paths in edits (the Advanced tier): allowed.
