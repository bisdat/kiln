# Gear values

A mod of data only (`kiln.mod.json` and `content/gear.json`, no code) that changes gear in the numbers the game's own
tooltip shows. Guide: `docs/kiln/services/content.md`, "Gear values".

- The Craft sword (`Weapon_T1_1H_Sword_Craft`) at power 30, with three quarters of its damage as cut and a quarter as fire.
- Every tier 2 armour piece the game made Rare gets a fifth more physical armour than the game gives it, at each piece's level.
- `gearRules`: Epic and Legendary copies get more max durability (1.1 and 1.25 instead of 1.05 and 1.1), and a new Epic
  weapon gets a gem socket three times in four (the game: one in two).

Copy the folder into the mod profile of the server, and of every player who should see the new numbers in the tooltip:

- `gear` values act on the server for everyone. A Kiln player gets them through the content sync even without the mod; a
  player without Kiln plays with them but the tooltip shows the game's numbers.
- `gearRules` are read before the game starts (the mod says `"boot": true`), in each process that has the mod: the server's
  values act on the server, and a player's game shows (and predicts) them only when it has the mod too.

`kiln lint` checks the file (names, kinds, ranges). The values have not been checked against a real game's tooltip yet: the
guided client check `equip-p6.gear-tooltip` does that.
