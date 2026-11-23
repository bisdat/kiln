# Quiver sample

Player storage (since 0.2.0-dev.30.7, mod API 0.2.16; GitHub [#128](https://github.com/frostdev-ops/kiln/issues/128) and
[#127](https://github.com/frostdev-ops/kiln/issues/127)). Guide: `docs/kiln/modding/player-storage.md`; the rules:
`docs/kiln/services/player-storage.md`.

Every player gets two storages that Kiln keeps with the character (world saves, rejoins, other worlds, the mod off):

| Storage | Slots | Takes | At death |
|---|---|---|---|
| `quiversample:quiver` | 18 | arrows and spells (`StorageFilter.Ammunition`) | the game's tombstone rule (`StorageDeath.FollowGame`) |
| `quiversample:satchel` | 8 | anything; gear keeps its level, durability, dye and gem | kept |

Commands (as a player, on the server or the game hosting the world):

| Command | Does |
|---|---|
| `/quiver` | both storages, slot by slot |
| `/quiver store [stacks]` | ammunition from your backpack into the quiver (3 stacks unless you say) |
| `/quiver take <slot>` | a quiver stack back into your backpack |
| `/quiver use <slot>` | equips a quiver stack where it is (`Items.Equip` with the storage slot: nothing moves) |
| `/quiver activate <slot>` | swaps a quiver stack into your backpack and equips it there (`PlayerStorage.Activate`) |
| `/quiver stash <name>` | the first backpack item whose name contains `<name>` into the satchel |
| `/quiver unstash <slot>` | a satchel item back into your backpack |

The quiver's stacks also show in the HUD's ammunition and spell lists, after the game's own rows, and picking one there
equips it from the quiver: `PlayerStorage.Client.ProvideAmmo`, made in `OnLoad` on both sides (the mod's side is
`both`). The player's game shows rows from Kiln's read-only copy of that player's storages; the server checks every
pick (the storage is the sender's, the item is in it, it is ammunition of that list) before it equips.

Copy the folder into the mod profile of the server and of every player who should see the HUD rows. A player without
the mod, or without Kiln, still has the storages on the server; only the HUD rows need it on their game.

What the sample does not prove: whether the game fires from a quiver slot and takes the arrow from it (if it does not,
`/quiver activate` or `PlayerStorageAmmoOptions.Activate` is the way), and the rows and picks in a real game. Those are
the owner's in-game check.
