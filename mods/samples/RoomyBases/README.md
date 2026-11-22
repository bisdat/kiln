# Roomy bases

A mod of data only (`kiln.mod.json` and `blocks/bases.json`, no code) that gives a world more Flame Altars and larger
build zones with Kiln's base rules. Guide: `docs/kiln/services/build.md`, "Base rules and base limits".

- At least 6 Flame Altars from the first Flame level, and 2 more than the game allows at every level (never more than 16:
  the game counts no more).
- Every altar level's build zone at 150% of the game's own size (60, 120, 180, 240 and 240 m instead of 40, 80, 120, 160
  and 160), and at least 64 m for a new (level 1) altar.

Another mod with base rules of its own, or a mod that raises the same numbers by the game's names (More Flame Altars),
does not conflict with this one: Kiln takes the largest number any of them asks for, at every level.

The same rule in C#, in `Mod.OnBoot` (the mod says `"boot": true`):

```csharp
public override void OnBoot(BootContext boot) =>
    BootDefinitions.For(boot).Define("roomy", new BaseRulesBlock
    {
        MinimumAltars = 6, ExtraAltars = 2, BuildAreaPercent = 150,
        Sizes = [new AltarSize(1, new Vector3(64, 64, 64))],
    });
```

The rules are read before the game starts, in each process that has the mod. Copy the folder into the server's mod
profile and into every player's: a player's game draws build zones and counts altars by its own table, so a player
without the mod sees the game's sizes while the server allows the larger ones (`"clients": "mod"` says so). Read the
result back on the server with `Build.BaseLimits`.
